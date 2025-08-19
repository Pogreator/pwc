# PWC

<img width="4080" height="3072" alt="image" src="https://github.com/user-attachments/assets/27cecfa0-1a31-45d9-891d-524d41f1a7ef" />
<sub> no screenshotting tool works with it :/ <sub>

PWC is a wayland compositor made for fun. Is it ever going to be the best? probably not.  
Currently WIP. It has no features other than working :/.  

# Installation

## Dependancies

- meson
- ninja
- wayland
- libinput
- xcb-util
- vulkan-renderer

## Building it

1. `git clone https://github.com/Pogreator/pwc.git`
2. `cd pwc`
3. `meson setup build`
4. `ninja -C build`

To open, simply execute `pwc` in the create build directory.
