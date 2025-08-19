project(
    'pwc',
    'c',
    version: '0.1.0',
    license: 'GPL-2.0-only',
    meson_version: '>=0.59.0',
    default_options: [
    'c_std=c11',
    'warning_level=2',
    ],
    )

little_endian = target_machine.endian() == 'little'
big_endian = target_machine.endian() == 'big'

add_project_arguments([
	'-D_POSIX_C_SOURCE=200809L',
	'-DWLR_USE_UNSTABLE',
	'-DWLR_PRIVATE=',
	'-DWLR_LITTLE_ENDIAN=@0@'.format(little_endian.to_int()),
	'-DWLR_BIG_ENDIAN=@0@'.format(big_endian.to_int()),
], language: 'c')


cc = meson.get_compiler('c')

wlroots = dependency(
  'wlroots-0.19',
  default_options: ['default_library=static', 'examples=false'],
  version: ['>=0.19.0'],
)

wlroots_has_xwayland = wlroots.get_variable('have_xwayland') == 'true'

wayland_server = dependency('wayland-server', version: '>=1.19.0')
wayland_protos = dependency('wayland-protocols', version: '>=1.39')
xkbcommon = dependency('xkbcommon')
xcb = dependency('xcb', required: get_option('xwayland'))
xcb_ewmh = dependency('xcb-ewmh', required: get_option('xwayland'))
xcb_icccm = dependency('xcb-icccm', required: get_option('xwayland'))
drm_full = dependency('libdrm')
drm = drm_full.partial_dependency(compile_args: true, includes: true)
xml2 = dependency('libxml-2.0')
glib = dependency('glib-2.0')
cairo = dependency('cairo')
pangocairo = dependency('pangocairo')
input = dependency('libinput', version: '>=1.14')
pixman = dependency('pixman-1')
math = cc.find_library('m')
png = dependency('libpng')
svg = dependency('librsvg-2.0', version: '>=2.46', required: false)

xwayland = dependency(
  'xwayland',
  version: '>=21.1.9',
  required: get_option('xwayland'),
)

if get_option('xwayland').enabled() and not wlroots_has_xwayland
	error('no wlroots Xwayland support')
endif
if get_option('xwayland').disabled()
	have_xwayland = false
elif not xwayland.found()
	warning('disabling xwayland, requires version >= 21.1.9')
	have_xwayland = false
elif xcb.found() and wlroots_has_xwayland
	have_xwayland = true
else
	have_xwayland = false
endif

pwc_inc = include_directories('include')

subdir('protocols')

pwc_deps = [
server_protos,
  wayland_server,
  wlroots,
  xkbcommon,
  xcb_ewmh,
  xcb_icccm,
  xml2,
  glib,
  cairo,
  drm,
  pangocairo,
  input,
  pixman,
  math,
  png,
]

subdir('src')

executable('pwc', pwc_sources, include_directories: [pwc_inc], dependencies: pwc_deps, install: true,)

install_data('data/pwc.desktop', install_dir: get_option('datadir') / 'wayland-sessions')

install_data('data/pwc-portals.conf', install_dir: get_option('datadir') / 'xdg-desktop-portal')
