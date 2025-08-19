#include <assert.h>
#include <getopt.h>
#include <stdbool.h>
#include <stdlib.h>
#include <stdio.h>
#include <time.h>
#include <unistd.h>
#include <wayland-server-core.h>
#include <wlr/backend.h>
#include <wlr/render/allocator.h>
#include <wlr/render/wlr_renderer.h>
#include <wlr/types/wlr_cursor.h>
#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_data_device.h>
#include <wlr/types/wlr_input_device.h>
#include <wlr/types/wlr_keyboard.h>
#include <wlr/types/wlr_output.h>
#include <wlr/types/wlr_output_layout.h>
#include <wlr/types/wlr_pointer.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/types/wlr_seat.h>
#include <wlr/types/wlr_subcompositor.h>
#include <wlr/types/wlr_xcursor_manager.h>
#include <wlr/types/wlr_xdg_shell.h>
#include <wlr/util/log.h>
#include <xkbcommon/xkbcommon.h>

enum pwc_cursor_mode {
    PWC_CURSOR_PASSTHROUGH,
    PWC_CURSOR_MOVE,
    PWC_CURSOR_RESIZE,
};

struct pwc_server {
    struct wl_display *wl_display;
    struct wlr_backend *backend;
    struct wlr_renderer *renderer;
    struct wlr_allocator *allocator;
    struct wlr_scene *scene;
    struct wlr_scene_output_layout *scene_layout;

    struct wlr_xdg_shell *xdg_shell;
    struct wl_listener new_xdg_toplevel;
    struct wl_listener new_xdg_popup;
    struct wl_list toplevels;

    struct wlr_cursor *cursor;
    struct wlr_xcursor_manager *cursor_mgr;
    struct wl_listener cursor_motion;
    struct wl_listener cursor_motion_absolute;
    struct wl_listener cursor_button;
    struct wl_listener cursor_axis;
    struct wl_listener cursor_frame;

    struct wlr_seat *seat;
    struct wl_listener new_input;
    struct wl_listener request_cursor;
    struct wl_listener pointer_focus_change;
    struct wl_listener request_set_selection;
    struct wl_list keyboards;
    enum pwc_cursor_mode cursor_mode;
    struct pwc_toplevel *grabbed_toplevel;
    double grab_x, grab_y;
    struct wlr_box grab_geobox;
    uint32_t resize_edges;

    struct wlr_output_layout *output_layout;
    struct wl_list outputs;
    struct wl_listener new_output;
};

struct pwc_output {
    struct wl_list link;
    struct pwc_server *server;
    struct wlr_output *wlr_output;
    struct wl_listener frame;
    struct wl_listener request_state;
    struct wl_listener destroy;
};

struct pwc_toplevel {
    struct wl_list link;
    struct pwc_server *server;
    struct wlr_xdg_toplevel *xdg_toplevel;
    struct wlr_scene_tree *scene_tree;
    struct wl_listener map;
    struct wl_listener unmap;
    struct wl_listener commit;
    struct wl_listener destroy;
    struct wl_listener request_move;
    struct wl_listener request_resize;
    struct wl_listener request_maximize;
    struct wl_listener request_fullscreen;
};

struct pwc_popup {
    struct wlr_xdg_popup *xdg_popup;
    struct wl_listener commit;
    struct wl_listener destroy;
};

struct pwc_keyboard {
    struct wl_list link;
    struct pwc_server *server;
    struct wlr_keyboard *wlr_keyboard;

    struct wl_listener modifiers;
    struct wl_listener key;
    struct wl_listener destroy;
};


static void focus_toplevel(struct pwc_toplevel *toplevel){
  // Only deals with keyboard
    if (toplevel == NULL) return;
    struct pwc_server *server = toplevel->server;
    struct wlr_seat *seat = server -> seat;
    struct wlr_surface *prev_surface = seat->keyboard_state.focused_surface;
    struct wlr_surface *surface = toplevel->xdg_toplevel->base->surface;
    if (prev_surface == surface) return;

    if (prev_surface){
        // Deactive prevously focused surface. Letclient know it is not longer in focus and repain accordingly
        struct wlr_xdg_toplevel *prev_toplevel = wlr_xdg_toplevel_try_from_wlr_surface(prev_surface);
        if (prev_toplevel != NULL) wlr_xdg_toplevel_set_activated(prev_toplevel,false);
    }

    struct wlr_keyboard *keyboard = wlr_seat_get_keyboard(seat);
    // Move the toplevel to the front
    wlr_scene_node_raise_to_top(&toplevel->scene_tree->node);
    wl_list_remove(&toplevel->link);
    wl_list_insert(&server->toplevels, &toplevel->link);
    // Activate new surface
    wlr_xdg_toplevel_set_activated(toplevel->xdg_toplevel,true);
    // Tell the seat to have the keyboard enter this surface. wlroots keeps track of this and sends key events
    if (keyboard != NULL){
        wlr_seat_keyboard_notify_enter(seat, surface, keyboard->keycodes, keyboard->num_keycodes, &keyboard->modifiers);
    }

}

static void keyboard_handle_modifiers(struct wl_listener *listener, void *data){
    // Event is raised when a modifier key is pressed
    struct pwc_keyboard *keyboard = wl_container_of(listener, keyboard, modifiers);
    // Seat can only have one keyboard because of the Wayland protocall.
    // All keyboards asre assigned as one in the same seat
    wlr_seat_set_keyboard(keyboard->server->seat, keyboard->wlr_keyboard);
    wlr_seat_keyboard_notify_modifiers(keyboard->server->seat, &keyboard->wlr_keyboard->modifiers);
}

static bool handle_keybindings(struct pwc_server *server, xkb_keysym_t sym){
    // Handle compositor keybinds. Compositor is processing and not passing keys. Assumes alt is held down.
    switch (sym){
        case XKB_KEY_Escape:
            wl_display_terminate(server->wl_display);
            break;
        case XKB_KEY_F1:
            // Cycle to next toplevel
            if (wl_list_length(&server->toplevels) < 2) break;
            struct pwc_toplevel *next_toplevel = wl_container_of(server->toplevels.prev, next_toplevel, link);
            focus_toplevel(next_toplevel);
            break;
        case XKB_KEY_Return:
            // Open terminal
            system("alacritty &");
            break;
        default: return false;
    }
    return true;
}

static void keyboard_handle_key(struct wl_listener *listener, void *data){
    // Event is raised when a key is pressed or released
    struct pwc_keyboard *keyboard = wl_container_of(listener,keyboard,key);
    struct pwc_server *server = keyboard->server;
    struct wlr_keyboard_key_event *event = data;
    struct wlr_seat *seat = server->seat;

    // Translate libinput keycode -> xkbcommon
    uint32_t keycode = event->keycode + 8;
    // Get list of keysyms based on the keymap for this keyboard
    const xkb_keysym_t *syms;
    int nsyms = xkb_state_key_get_syms(keyboard->wlr_keyboard->xkb_state, keycode , &syms);

    bool handled = false;
    uint32_t modifiers = wlr_keyboard_get_modifiers(keyboard->wlr_keyboard);
    if ((modifiers & WLR_MODIFIER_ALT) && event->state == WL_KEYBOARD_KEY_STATE_PRESSED){
        // If alt is held down and this button was _pressed_, we attempt to processes it as a compositor keybinding
        for (int i = 0; i < nsyms; i++){
            handled = handle_keybindings(server,syms[i]);
        }
    }

    if (!handled){
        // Otherwise, pass it along to client
        wlr_seat_set_keyboard(seat, keyboard->wlr_keyboard);
        wlr_seat_keyboard_notify_key(seat, event->time_msec, event->keycode, event->state);
    }
}

static void keyboard_handle_destroy(struct wl_listener *listener, void *data){
    // This event is raised by the keyboard base wlr_input_device to signal destruction of wlr_keyboard. AKA the keyboard got disconnected
    struct pwc_keyboard *keyboard = wl_container_of(listener, keyboard, destroy);
    wl_list_remove(&keyboard->modifiers.link);
    wl_list_remove(&keyboard->key.link);
    wl_list_remove(&keyboard->destroy.link);
    wl_list_remove(&keyboard->link);
    free(keyboard);
}

static void server_new_keyboard(struct pwc_server *server, struct wlr_input_device *device){
    struct wlr_keyboard *wlr_keyboard = wlr_keyboard_from_input_device(device);
    struct pwc_keyboard *keyboard = calloc(1, sizeof(*keyboard));
    keyboard->server = server;
    keyboard->wlr_keyboard = wlr_keyboard;

    // Preparing an XKB keymap and assigning it to the keyboard. Assumes defaults (US)
    struct xkb_context *context = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
    struct xkb_keymap *keymap = xkb_keymap_new_from_names(context, NULL, XKB_KEYMAP_COMPILE_NO_FLAGS);

    wlr_keyboard_set_keymap(wlr_keyboard, keymap);
    xkb_keymap_unref(keymap);
    xkb_context_unref(context);
    wlr_keyboard_set_repeat_info(wlr_keyboard, 25, 600);

    // Set up listeners for keyboard events
    keyboard->modifiers.notify = keyboard_handle_modifiers;
    wl_signal_add(&wlr_keyboard->events.modifiers, &keyboard->modifiers);
    keyboard->key.notify = keyboard_handle_key;
    wl_signal_add(&wlr_keyboard->events.key, &keyboard->key);
    keyboard->destroy.notify = keyboard_handle_destroy;
    wl_signal_add(&device->events.destroy, &keyboard->destroy);

    wlr_seat_set_keyboard(server->seat, keyboard->wlr_keyboard);

    // Add keyboard to list
    wl_list_insert(&server->keyboards, &keyboard->link);
}

static void server_new_pointer(struct pwc_server *server, struct wlr_input_device *device){
    // Don't do anything special with pointers, all is handled in wlr_cursor. Could use to  do libinput config, acceleration etc
    wlr_cursor_attach_input_device(server->cursor, device);

}

static void server_new_input(struct wl_listener *listener, void *data){
    // Event raised by backend when new input device is available
    struct pwc_server *server = wl_container_of(listener, server, new_input);
    struct wlr_input_device *device = data;
    switch (device->type){
        case WLR_INPUT_DEVICE_KEYBOARD:
            server_new_keyboard(server,device);
            break;
        case WLR_INPUT_DEVICE_POINTER:
            server_new_pointer(server, device);
            break;
        default: break;
    }

    // Let wlr_seat know what capabilities are, which is communicated to the client. Cursor is always there no matter what
    uint32_t caps = WL_SEAT_CAPABILITY_POINTER;
    if (!wl_list_empty(&server->keyboards)) caps |= WL_SEAT_CAPABILITY_KEYBOARD;
    wlr_seat_set_capabilities(server->seat, caps);
}

static void seat_request_cursor(struct wl_listener *listener, void *data){
    struct pwc_server *server = wl_container_of(listener, server, request_cursor);
    // Event is rasied by the seat when a client provides a cursor image
    struct wlr_seat_pointer_request_set_cursor_event *event = data;
    struct wlr_seat_client *focused_client = server->seat->pointer_state.focused_client;
    // This can be sent by any client, make sure this one has pointer focus first
    if (focused_client == event->seat_client){
        wlr_cursor_set_surface(server -> cursor, event->surface, event->hotspot_x, event->hotspot_y);
    }
}

static void seat_pointer_focus_change(struct wl_listener *listener, void *data){
    struct pwc_server *server = wl_container_of(listener, server, pointer_focus_change);
    // This event is raised when pointer focus is changed, including closure of the client
    // Cursor image set to default if target surface is NULL
    struct wlr_seat_pointer_focus_change_event *event = data;
    if (event->new_surface == NULL){
        wlr_cursor_set_xcursor(server-> cursor, server->cursor_mgr, "default");
    }
}

static void seat_request_set_selection(struct wl_listener *listener, void *data) {
    // Event raised by seat when client wants to set the selection, usually when user copies something ( OPTIONAL )
    struct pwc_server *server = wl_container_of(listener, server, request_set_selection);
    struct wlr_seat_request_set_selection_event *event = data;
    wlr_seat_set_selection(server->seat, event->source, event->serial);
}

static struct pwc_toplevel *desktop_toplevel_at(struct pwc_server *server, double lx, double ly, struct wlr_surface **surface, double *sx, double *sy){
    // This returns the topmost node in the scene at the givern layout coords
    struct wlr_scene_node *node = wlr_scene_node_at(&server->scene->tree.node, lx, ly, sx, sy);
    if (node == NULL || node->type != WLR_SCENE_NODE_BUFFER) return NULL;
    struct wlr_scene_buffer *scene_buffer = wlr_scene_buffer_from_node(node);
    struct wlr_scene_surface *scene_surface = wlr_scene_surface_try_from_buffer(scene_buffer);
    if (!scene_surface) return NULL;
    *surface = scene_surface->surface;
    // Find the corresponding node to the pwc_toplevel at the root of this surface tree
    struct wlr_scene_tree *tree = node->parent;
    while (tree != NULL && tree->node.data == NULL) tree = tree->node.parent;
    return tree->node.data;
}

static void reset_cursor_mode(struct pwc_server *server){
    // Reset the cursor mode to passthrough
    server->cursor_mode = PWC_CURSOR_PASSTHROUGH;
    server->grabbed_toplevel = NULL;
}

static void process_cursor_mode(struct pwc_server *server){
    // Move the grabbed toplevel to new_position
    struct pwc_toplevel *toplevel = server->grabbed_toplevel;
    wlr_scene_node_set_position(&toplevel->scene_tree->node, server->cursor->x - server->grab_x, server->cursor->y - server->grab_y);
}

static void process_cursor_resize(struct pwc_server *server){
    // Resizing the grabbed toplevel can be complicated because the user can resize from any corner or resize_edges
    // This resizes and moves the toplevel. Shortcuts have been taken.
    // In a more fleshed out one, you would wait for the client to prepare a buffer at the new size then commit.
    struct pwc_toplevel *toplevel = server->grabbed_toplevel;
    double border_x = server->cursor->x - server->grab_x;
    double border_y = server->cursor->y - server->grab_y;
    int new_left = server->grab_geobox.x;
    int new_right = server->grab_geobox.x + server->grab_geobox.width;
    int new_top = server->grab_geobox.y;
    int new_bottom = server->grab_geobox.y + server->grab_geobox.height;

    if (server->resize_edges & WLR_EDGE_TOP){
        new_top = border_y;
        if (new_top >= new_bottom) new_top = new_bottom - 1;
    }
    else if (server->resize_edges & WLR_EDGE_BOTTOM){
        new_bottom = border_y;
        if (new_bottom <= new_top) new_bottom = new_top + 1;
    }

    if (server->resize_edges & WLR_EDGE_LEFT){
        new_left = border_x;
        if (new_left >= new_right) new_left = new_right - 1;
    }
    else if (server->resize_edges & WLR_EDGE_RIGHT){
        new_right = border_x;
        if (new_right <= new_left) new_right = new_left + 1;
    }

    struct wlr_box *geo_box = &toplevel->xdg_toplevel->base->geometry;
    wlr_scene_node_set_position(&toplevel->scene_tree->node, new_left - geo_box->x, new_top - geo_box->y);

    int new_width = new_right - new_left;
    int new_height = new_bottom - new_top;
    wlr_xdg_toplevel_set_size(toplevel->xdg_toplevel, new_width, new_height);
}

static void process_cursor_motion(struct pwc_server *server, uint32_t time){
    // If the mode is non-passthrough, delegate to those functions
    if (server->cursor_mode == PWC_CURSOR_MOVE){process_cursor_mode(server); return;}
    else if (server->cursor_mode == PWC_CURSOR_RESIZE){process_cursor_resize(server); return;}

    // Otherwise, find the toplevel under the pointer and send the event along
    double sx, sy;
    struct wlr_seat *seat = server->seat;
    struct wlr_surface *surface = NULL;
    struct pwc_toplevel *toplevel = desktop_toplevel_at(server, server->cursor->x, server->cursor->y, &surface, &sx, &sy);

    // If there is no toplevel under the cursor, set cursor image to default
    if (!toplevel) wlr_cursor_set_xcursor(server->cursor, server->cursor_mgr, "default");

    if (surface){
        // Send pointer enter and motion events.
        // The enter event gives the surface "Pointer Focus", which is distinct from keyboard focus
        // wlroots will avoid sending duplicate enter/motion events if surface already had pointer focus or client is aware of the coordinates passed
        wlr_seat_pointer_notify_enter(seat, surface, sx, sy);
        wlr_seat_pointer_notify_motion(seat, time, sx, sy);
    }
    else{
        // Clear pointer focus so future button events and such are not sent to the last client
        wlr_seat_pointer_clear_focus(seat);
    }
}

static void server_cursor_motion(struct wl_listener *listener, void *data){
    // Event is forwarded by the cursor when a pointer emits a _relative_ pointer motion event (i.e. delta)
    struct pwc_server *server = wl_container_of(listener, server, cursor_motion);
    struct wlr_pointer_motion_event *event = data;
    // The cursor does not move unless we tell it to.
    // The cursor automatically handles constraining the motion to the output layout, as well as any special config applied.
    wlr_cursor_move(server->cursor, &event->pointer->base, event->delta_x, event->delta_y);
    process_cursor_motion(server, event->time_msec);
}

static void server_cursor_motion_absolute(struct wl_listener *listener, void *data){
    // Event is forwarded by the cursor when a pointer emits a _absolute_ pointer motion event from 0..1 on each axis.
    // This happens, for example, when wlroots is running under a wayland window rather than KMS+DRM and the mouse is moved over...
    // the window. The mouse can etner from any edge so it has to be warped there.
    struct pwc_server *server = wl_container_of(listener, server, cursor_motion_absolute);
    struct wlr_pointer_motion_absolute_event *event = data;
    wlr_cursor_warp_absolute(server->cursor, &event->pointer->base, event->x, event->y);
    process_cursor_motion(server, event->time_msec);
}

static void server_cursor_button(struct wl_listener *listener, void *data){
    // This event is forwarded by the cursor when a pointer emits a button event
    struct pwc_server *server = wl_container_of(listener, server, cursor_button);
    struct wlr_pointer_button_event *event = data;
    // Notify client with pointer focus that a button press has occured
    wlr_seat_pointer_notify_button(server->seat, event->time_msec, event->button, event->state);
    if (event->state == WL_POINTER_BUTTON_STATE_RELEASED){
        // If you released any button, we exit interactive move/resize mode
        reset_cursor_mode(server);
    }
    else{
        // Focus that client if the button was _pressed_
        double sx, sy;
        struct wlr_surface *surface = NULL;
        struct pwc_toplevel *toplevel = desktop_toplevel_at(server, server->cursor->x, server->cursor->y, &surface, &sx,  &sy);
        focus_toplevel(toplevel);
    }
}

static void server_cursor_axis(struct wl_listener *listener, void *data){
    // This event is forwarded by the cursor when a pointer emits an axis event (i.e. moving scroll wheel)
    struct pwc_server *server = wl_container_of(listener, server, cursor_axis);
    struct wlr_pointer_axis_event *event = data;
    // Notify the client with pointer focus of the axis event
    wlr_seat_pointer_notify_axis(server->seat, event->time_msec, event->orientation, event->delta,
                                 event->delta_discrete, event->source, event->relative_direction);
}

void server_cursor_frame(struct wl_listener *listener, void *data){
    // Event is forwarded by the cursor when a pointer emits a frame event.
    // Frame events are sent after regular pointer events to group multiple events together.
    struct pwc_server *server = wl_container_of(listener, server, cursor_frame);
    wlr_seat_pointer_notify_frame(server->seat);
}

static void output_frame(struct wl_listener *listener, void *data){
    // Function called every time an output is ready to display a frame, generally at output refresh rate.
    struct pwc_output *output = wl_container_of(listener, output, frame);
    struct wlr_scene *scene = output->server->scene;

    struct wlr_scene_output *scene_output = wlr_scene_get_scene_output(scene, output->wlr_output);

    // Render the scene if needed then commit
    wlr_scene_output_commit(scene_output, NULL);

    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    wlr_scene_output_send_frame_done(scene_output, &now);
}

static void output_request_state(struct wl_listener *listener, void *data){
    // Function is called when the backend requests a new state for the output
    struct pwc_output *output = wl_container_of(listener, output, request_state);
    const struct wlr_output_event_request_state *event = data;
    wlr_output_commit_state(output->wlr_output, event->state);
}

static void output_destroy(struct wl_listener *listener, void *data){
    struct pwc_output *output = wl_container_of(listener, output, destroy);

    wl_list_remove(&output->frame.link);
    wl_list_remove(&output->request_state.link);
    wl_list_remove(&output->destroy.link);
    wl_list_remove(&output->link);
    free(output);
}

static void server_new_output(struct wl_listener *listener, void *data){
    // Event is raised by the backend when a new output becomes available
    struct pwc_server *server = wl_container_of(listener, server, new_output);
    struct wlr_output *wlr_output = data;

    // Configures the output created by the backend to use wlroot allocator and renderer
    wlr_output_init_render(wlr_output, server->allocator, server->renderer);

    // The output may be disabled, switch it on
    struct wlr_output_state state;
    wlr_output_state_init(&state);
    wlr_output_state_set_enabled(&state, true);

    // Some backends don't have modes.
    // DRM + KMS does, and they need a mode before they can be used for output.
    // Mode is a tuple of (width, height, refresh rate) and each monitor supports only a..
    // specific set of modes, we just pick the monitor's prefered mode.
    // A more sophisticated compositor would let the user pick it.
    struct wlr_output_mode *mode = wlr_output_preferred_mode(wlr_output);
    if (mode != NULL) wlr_output_state_set_mode(&state, mode);

    // Automatically applies the new output state
    wlr_output_commit_state(wlr_output, &state);
    wlr_output_state_finish(&state);

    // Allocates and configures our state for this output
    struct pwc_output *output = calloc(1, sizeof(*output));
    output->wlr_output = wlr_output;
    output->server = server;

    // Sets up a listener for the frame event
    output->frame.notify = output_frame;
    wl_signal_add(&wlr_output->events.frame, &output->frame);

    // Sets up a listener for the state request event
    output->request_state.notify = output_request_state;
    wl_signal_add(&wlr_output->events.request_state, &output->request_state);

    // Sets up a listener for the destroy event
    output->destroy.notify = output_destroy;
    wl_signal_add(&wlr_output->events.destroy, &output->destroy);

    wl_list_insert(&server->outputs, &output->link);

    // Adds this to the output layout.
    // The audo_add function arrages outputs from left to right in the order they appear.
    // A more sophisticated compositor would let the user configure output arrangment.
    // The output layout utility automatically adds a wl_output global to the display..
    // which wayland clients can see to find out information about output.

    struct wlr_output_layout_output *l_output = wlr_output_layout_add_auto(server->output_layout, wlr_output);
    struct wlr_scene_output *scene_output = wlr_scene_output_create(server->scene, wlr_output);
    wlr_scene_output_layout_add_output(server->scene_layout, l_output, scene_output);
}

static void xdg_toplevel_map(struct wl_listener *listener, void *data){
    // Called when the surface is mapped, or ready to display on screen
    struct pwc_toplevel *toplevel = wl_container_of(listener, toplevel, map);
    wl_list_insert(&toplevel->server->toplevels, &toplevel->link);
    focus_toplevel(toplevel);
}

static void xdg_toplevel_unmap(struct wl_listener *listener, void *data){
    // Called when the surface is unmapped, and should no longer be shown
    struct pwc_toplevel *toplevel = wl_container_of(listener, toplevel, map);
    // Reset cursor mode
    if (toplevel == toplevel->server->grabbed_toplevel) reset_cursor_mode(toplevel->server);
    wl_list_remove(&toplevel->link);
}

static void xdg_toplevel_commit(struct wl_listener *listener, void *data){
    // Called when a new surface state is committed
    struct pwc_toplevel *toplevel = wl_container_of(listener, toplevel, commit);

    // When an xdg_surface performs an inital commity the compositor must reply with a configuration so that the client..
    // can map the surface. xdg_toplevel with 0,0 size lets the client pick the dimensions itself.
    if (toplevel->xdg_toplevel->base->initial_commit){
        wlr_xdg_toplevel_set_size(toplevel->xdg_toplevel, 0, 0);
    }
}

static void xdg_toplevel_destroy(struct wl_listener *listener, void *data){
    // Called when the xdg_toplevel is destroyed
    struct pwc_toplevel *toplevel = wl_container_of(listener, toplevel, destroy);

    wl_list_remove(&toplevel->map.link);
    wl_list_remove(&toplevel->unmap.link);
    wl_list_remove(&toplevel->commit.link);
    wl_list_remove(&toplevel->destroy.link);
    wl_list_remove(&toplevel->request_move.link);
    wl_list_remove(&toplevel->request_resize.link);
    wl_list_remove(&toplevel->request_maximize.link);
    wl_list_remove(&toplevel->request_fullscreen.link);

    free(toplevel);
}

static void begin_interactive(struct pwc_toplevel *toplevel, enum pwc_cursor_mode mode, uint32_t edges){
    // This function sets up an interactive move or resize operation where the compositor..
    // stops propegating pointer events to clients and instead consumes them itself, to move or resize windows
    struct pwc_server *server = toplevel->server;

    server->grabbed_toplevel = toplevel;
    server->cursor_mode = mode;

    if (mode == PWC_CURSOR_MOVE){
        server->grab_x = server->cursor->x - toplevel->scene_tree->node.x;
        server->grab_y = server->cursor->y - toplevel->scene_tree->node.y;
    }
    else{
        struct wlr_box *geo_box = &toplevel->xdg_toplevel->base->geometry;

        double border_x = (toplevel->scene_tree->node.x + geo_box->x) + ((edges & WLR_EDGE_RIGHT) ? geo_box->width : 0);
        double border_y = (toplevel->scene_tree->node.y + geo_box->y) + ((edges & WLR_EDGE_BOTTOM) ? geo_box->height : 0);

        server->grab_x = server->cursor->x - border_x;
        server->grab_y = server->cursor->y - border_y;

        server->grab_geobox = *geo_box;
        server->grab_geobox.x += toplevel->scene_tree->node.x;
        server->grab_geobox.y += toplevel->scene_tree->node.y;

        server->resize_edges = edges;
    }
}

static void xdg_toplevel_request_move(struct wl_listener *listener, void *data){
    // This event is raised when a client would like to begin an interactive move, typically because the user clicked..
    // on their client-side decorations. A more sophisticated compositor would check the provided serial against a list..
    // of button press serials sent to this client, to prevent the client from requestin this whenever they want.
    struct pwc_toplevel *toplevel = wl_container_of(listener, toplevel, request_move);
    begin_interactive(toplevel, PWC_CURSOR_MOVE, 0);
}

static void xdg_toplevel_request_resize(struct wl_listener *listener, void *data){
    // This event is raised when a client would like to begin an interactive resize. ^
    struct wlr_xdg_toplevel_resize_event *event = data;
    struct pwc_toplevel *toplevel = wl_container_of(listener, toplevel, request_resize);
    begin_interactive(toplevel, PWC_CURSOR_RESIZE, event->edges);
}

static void xdg_toplevel_request_maximize(struct wl_listener *listener, void *data){
    // This event is raised when a client would like to maximize itself. ^^
    struct pwc_toplevel *toplevel = wl_container_of(listener, toplevel, request_resize);
    if (toplevel->xdg_toplevel->base->initialized){
        wlr_xdg_surface_schedule_configure(toplevel->xdg_toplevel->base);
    }
}

static void xdg_toplevel_request_fullscreen(struct wl_listener *listener, void *data){
    // This event is raised when a client would like to fullscreen itself. ^^^
    struct pwc_toplevel *toplevel = wl_container_of(listener, toplevel, request_fullscreen);
    if (toplevel->xdg_toplevel->base->initialized){
        wlr_xdg_surface_schedule_configure(toplevel->xdg_toplevel->base);
    }
}

static void server_new_xdg_toplevel(struct wl_listener *listener, void *data){
    // This event is raised when a client creates a new toplevel ( application window )
    struct pwc_server *server = wl_container_of(listener, server, new_xdg_toplevel);
    struct wlr_xdg_toplevel *xdg_toplevel = data;

    // Allocate a pwc_toplevel for this surface
    struct pwc_toplevel *toplevel = calloc(1, sizeof(*toplevel));
    toplevel->server = server;
    toplevel->xdg_toplevel = xdg_toplevel;
    toplevel->scene_tree = wlr_scene_xdg_surface_create(&toplevel->server->scene->tree, xdg_toplevel->base);
    toplevel->scene_tree->node.data = toplevel;
    xdg_toplevel->base->data = toplevel->scene_tree;

    // Listen to various events it can emit
    toplevel->map.notify = xdg_toplevel_map;
    wl_signal_add(&xdg_toplevel->base->surface->events.map, &toplevel->map);
    toplevel->unmap.notify = xdg_toplevel_unmap;
    wl_signal_add(&xdg_toplevel->base->surface->events.unmap, &toplevel->unmap);
    toplevel->commit.notify = xdg_toplevel_commit;
    wl_signal_add(&xdg_toplevel->base->surface->events.commit, &toplevel->commit);

    toplevel->destroy.notify = xdg_toplevel_destroy;
    wl_signal_add(&xdg_toplevel->base->surface->events.destroy, &toplevel->destroy);

    // cotd
    toplevel->request_move.notify = xdg_toplevel_request_move;
    wl_signal_add(&xdg_toplevel->events.request_move, &toplevel->request_move);
    toplevel->request_resize.notify = xdg_toplevel_request_resize;
    wl_signal_add(&xdg_toplevel->events.request_resize, &toplevel->request_resize);
    toplevel->request_maximize.notify = xdg_toplevel_request_maximize;
    wl_signal_add(&xdg_toplevel->events.request_maximize, &toplevel->request_maximize);
    toplevel->request_fullscreen.notify = xdg_toplevel_request_fullscreen;
    wl_signal_add(&xdg_toplevel->events.request_fullscreen, &toplevel->request_fullscreen);

}

static void xdg_popup_commit(struct wl_listener *listener, void *data){
    // Called when a new surface state is committed
    struct pwc_popup *popup = wl_container_of(listener, popup, commit);

    // When an xdg_surface performs an initial commit, the compositor must reply with..
    // a configuration so the client can map the surface.
    // A more sophisticated compositor might change a xdg_popup's geometry to ensure it is
    // positioned offscreen
    if (popup->xdg_popup->base->initial_commit){
        wlr_xdg_surface_schedule_configure(popup->xdg_popup->base);
    }
}

static void xdg_popup_destroy(struct wl_listener *listener, void *data){
    // Called when the xdg_popup is destroyed
    struct pwc_popup *popup = wl_container_of(listener, popup, destroy);

    wl_list_remove(&popup->commit.link);
    wl_list_remove(&popup->destroy.link);

    free(popup);
}

static void server_new_xdg_popup(struct wl_listener *listener, void *data){
    // This event is raised when a client creates a new popup
    struct wlr_xdg_popup *xdg_popup = data;

    struct pwc_popup *popup = calloc(1,sizeof(*popup));
    popup->xdg_popup = xdg_popup;

    // xdg popups must be added to the scene graph so they get rendered..
    // wlroots scene graph provides this but it must have the proper parent scene provided.
    // To do this we always set the user data field of xdg_surfaces to the corresponding scene node
    struct wlr_xdg_surface *parent = wlr_xdg_surface_try_from_wlr_surface(xdg_popup->parent);
    assert(parent != NULL);
    struct wlr_scene_tree *parent_tree = parent->data;
    xdg_popup->base->data = wlr_scene_xdg_surface_create(parent_tree, xdg_popup->base);

    popup->commit.notify = xdg_popup_commit;
    wl_signal_add(&xdg_popup->base->surface->events.commit, &popup->commit);

    popup->destroy.notify = xdg_popup_destroy;
    wl_signal_add(&xdg_popup->base->surface->events.destroy, &popup->destroy);
}

int main(int argc, char *argv[]){
    wlr_log_init(WLR_DEBUG, NULL);
    char *startup_cmd = NULL;

    int c;
    while ((c = getopt(argc, argv, "s:h")) != -1){
        switch (c){
            case 's':
                startup_cmd = optarg;
                break;
            default:
                printf("Usage: %s [-s startup command]\n", argv[0]);
                return 0;
        }
    }
    if (optind < argc){
        printf("Usage: %s [-s startup command]\n", argv[0]);
        return 0;
    }

    struct pwc_server server = {0};
    // The wayland display is managed by libwayland. It handles accepting clients from the Unix..
    // socket, managing Wayland globals and so on.
    server.wl_display = wl_display_create();
    // The backend is a wlroots feature which abstracts the underlying input and output hardware.
    // The autocreate option will choose the most suitable backend based on the current environment.
    server.backend = wlr_backend_autocreate(wl_display_get_event_loop(server.wl_display), NULL);
    if (server.backend == NULL){
        wlr_log(WLR_ERROR, "failed to create wlr_backend");
        return 1;
    }

    // Autocreates a renderer, either pixman, GLES2 or Vulkan. THe user can also specify a renderer..
    // using the WLR_RENDERER env var. The renderer is responsible for defining the various pixel formats..
    // it supports for shared memory, this configures that for clients.
    server.renderer = wlr_renderer_autocreate(server.backend);
    if (server.renderer == NULL){
        wlr_log(WLR_ERROR, "failed to create wlr_renderer");
        return 1;
    }

    wlr_renderer_init_wl_display(server.renderer, server.wl_display);

    // Autocreate an allocator.
    // The allocator is the bridge between the renderer and the backend. It handles the buffer creation..
    // allowing wlroots to render onto the screen.
    server.allocator = wlr_allocator_autocreate(server.backend, server.renderer);
    if (server.allocator == NULL){
        wlr_log(WLR_ERROR, "failed to create wlr_allocator");
        return 1;
    }

    // This creates some hands-off wlroots interfaces. The compositor is necessary for clients to allocate..
    // surfaces, the subcompositor allows to assign the role of subsurfaces to surfaces and the data device..
    // manager handles the clipboard. Each of these wlroots interfaces has room for you to play with their..
    // behaviour.
    // Note: Client cannot set the selection directly without compositor approval. See the handling of the..
    // request_set_selection event below
    wlr_compositor_create(server.wl_display, 5, server.renderer);
    wlr_subcompositor_create(server.wl_display);
    wlr_data_device_manager_create(server.wl_display);

    // Creates an output layout, which is a wlroots utility for working with an arrangment of screens in a physical layout
    server.output_layout = wlr_output_layout_create(server.wl_display);

    // Configures a listener to be notified when new outputs are available on the backend
    wl_list_init(&server.outputs);
    server.new_output.notify = server_new_output;
    wl_signal_add(&server.backend->events.new_output, &server.new_output);

    // Creates a scene graph. wlroots abstraction that handles all rendering and damage tracking. All that needs to be done..
    // is to add things that should be rendered to the scene graph at the proper positions and then call wlr_scene_output_commit()..
    // to render a frame if necessary
    server.scene = wlr_scene_create();
    server.scene_layout = wlr_scene_attach_output_layout(server.scene, server.output_layout);

    // Set up xdg-shell version 3. Wayland protocall which is used for application windows.
    // https://drewdevault.com/2018/07/29/Wayland-shells.html
    wl_list_init(&server.toplevels);
    server.xdg_shell = wlr_xdg_shell_create(server.wl_display, 3);
    server.new_xdg_toplevel.notify = server_new_xdg_toplevel;
    wl_signal_add(&server.xdg_shell->events.new_toplevel, &server.new_xdg_toplevel);
    server.new_xdg_popup.notify = server_new_xdg_popup;
    wl_signal_add(&server.xdg_shell->events.new_popup, &server.new_xdg_popup);

    // Creates a cursor, which is a wlroots utility for tracking the cursor image shown on screen
    server.cursor = wlr_cursor_create();
    wlr_cursor_attach_output_layout(server.cursor, server.output_layout);

    // Creates an xcursor manager. wlroots utility which loads up xcursor themes to source cursor images from..
    // and makes sure that cursor images are available at all scale factors on the screen ( necessary for HiDPI support )
    server.cursor_mgr = wlr_xcursor_manager_create(NULL, 24);

    // wlr_cursor ONLY displays an image on screen. Input device needs to be attached. ALl aggregate events will be generated. In..
    // these events we can choose how we want to process them, forwarding them to clients and moving the cursor around.
    // https://drewdevault.com/2018/07/17/Input-handling-in-wlroots.html
    server.cursor_mode = PWC_CURSOR_PASSTHROUGH;
    server.cursor_motion.notify = server_cursor_motion;
    wl_signal_add(&server.cursor->events.motion, &server.cursor_motion);
    server.cursor_motion_absolute.notify = server_cursor_motion_absolute;
    wl_signal_add(&server.cursor->events.motion_absolute, &server.cursor_motion_absolute);
    server.cursor_button.notify = server_cursor_button;
    wl_signal_add(&server.cursor->events.button, &server.cursor_button);
    server.cursor_axis.notify = server_cursor_axis;
    wl_signal_add(&server.cursor->events.axis, &server.cursor_axis);
    server.cursor_frame.notify = server_cursor_frame;
    wl_signal_add(&server.cursor->events.frame, &server.cursor_frame);

    // Configures a seat, which is a single "seat" at which a user sits and operates the computer. This includes up to one keyboard, pointer..
    // touch, drawing tablet device. A listener is also rigged to let us know when new input devices are available on the backend
    wl_list_init(&server.keyboards);
    server.new_input.notify = server_new_input;
    wl_signal_add(&server.backend->events.new_input, &server.new_input);
    server.seat = wlr_seat_create(server.wl_display, "seat0");
    server.request_cursor.notify = seat_request_cursor;
    wl_signal_add(&server.seat->events.request_set_cursor, &server.request_cursor);
    server.pointer_focus_change.notify = seat_pointer_focus_change;
    wl_signal_add(&server.seat->pointer_state.events.focus_change, &server.pointer_focus_change);
    server.request_set_selection.notify = seat_request_set_selection;
    wl_signal_add(&server.seat->events.request_set_selection, &server.request_set_selection);

    // Add a unix socket to the wayland display
    const char *socket = wl_display_add_socket_auto(server.wl_display);
    if (!socket){
        wlr_backend_destroy(server.backend);
        return 1;
    }

    // Start the backend. This will enumerate outputs and inputs, become the DRM master, etc
    if (!wlr_backend_start(server.backend)){
        wlr_backend_destroy(server.backend);
        wl_display_destroy(server.wl_display);
        return 1;
    }

    // Set the WAYLAND_DISPLAY environment variable to our socket and run the startup command if requested
    setenv("WAYLAND_DISPLAY", socket, true);
    if (startup_cmd){
        if (fork() == 0){
            execl("/bin/sh", "/bin/sh", "-c", startup_cmd, (void *)NULL);
        }
    }

    // Run the Wayland event loop. This does not return until you exit the compositor. Starting the backend rigged up all..
    // of the necessary event loop configuration to listen to libinput events, DRM events, generate frame events at the refresh ray, etc.
    wlr_log(WLR_INFO, "Running Wayland compositor on WAYLAND_DISPLAY=%s", socket);
    wl_display_run(server.wl_display);

    // Once wl_display_run returns, we destroy all clients then shutdown the server

    wl_display_destroy_clients(server.wl_display);

    wl_list_remove(&server.new_xdg_toplevel.link);
    wl_list_remove(&server.new_xdg_popup.link);

    wl_list_remove(&server.cursor_motion.link);
    wl_list_remove(&server.cursor_motion_absolute.link);
    wl_list_remove(&server.cursor_button.link);
    wl_list_remove(&server.cursor_axis.link);
    wl_list_remove(&server.cursor_frame.link);

    wl_list_remove(&server.new_input.link);
    wl_list_remove(&server.request_cursor.link);
    wl_list_remove(&server.pointer_focus_change.link);
    wl_list_remove(&server.request_set_selection.link);

    wl_list_remove(&server.new_output.link);

    wlr_scene_node_destroy(&server.scene->tree.node);
    wlr_xcursor_manager_destroy(server.cursor_mgr);
    wlr_cursor_destroy(server.cursor);
    wlr_allocator_destroy(server.allocator);
    wlr_renderer_destroy(server.renderer);
    wlr_backend_destroy(server.backend);
    wl_display_destroy(server.wl_display);
    return 0;
}
