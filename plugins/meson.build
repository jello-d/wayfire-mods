project('tackup-wayfire-plugins', 'cpp', 'c',
        version : '0.1.0',
        default_options : ['cpp_std=c++17'])

# Built against the installed wayfire in /opt/wayfire (via PKG_CONFIG_PATH),
# so these stay decoupled from the wayfire-plugins-extra version.
wayfire     = dependency('wayfire')
metadatadir = wayfire.get_variable(pkgconfig : 'metadatadir')
plugindir   = join_paths(get_option('libdir'), 'wayfire')

plugins = ['edge-flipping', 'workspace-wallpaper', 'wsync',
           'directional-focus', 'workarea-guard', 'enclave-mark',
           'toplevel-capture', 'surface-names', 'edge-warp', 'idle-audit']
# toplevel-capture serves its own ext_output_image_capture_source_manager_v1
# (one per enclave), so it needs the protocol's server code, which wlroots
# keeps private. The capture-source XML refers to the foreign-toplevel handle,
# so that protocol's code comes too.
wl_scanner   = find_program('wayland-scanner', native : true)
wl_protocols = dependency('wayland-protocols').get_variable(
    pkgconfig : 'pkgdatadir')
capture_protocols = []
foreach name : ['ext-image-capture-source', 'ext-foreign-toplevel-list']
    path = join_paths(wl_protocols, 'staging', name, name + '-v1.xml')
    capture_protocols += custom_target(input : path,
        output : '@BASENAME@-protocol.h',
        command : [wl_scanner, 'server-header', '@INPUT@', '@OUTPUT@'])
    capture_protocols += custom_target(input : path,
        output : '@BASENAME@-protocol.c',
        command : [wl_scanner, 'private-code', '@INPUT@', '@OUTPUT@'])
endforeach

# Per-plugin extras beyond wayfire itself: enclave-mark draws its name tab
# with wayfire's cairo-util.hpp (cairo + pango).
extra_deps = {
    'enclave-mark' : [dependency('cairo'), dependency('pangocairo')],
    'surface-names' : [dependency('cairo')],
    'toplevel-capture' : [dependency('wayland-server')],
}
extra_sources = {'toplevel-capture' : capture_protocols}
foreach name : plugins
    shared_module(name, [name + '.cpp'] + extra_sources.get(name, []),
                  dependencies : [wayfire] + extra_deps.get(name, []),
                  install      : true,
                  install_dir  : plugindir)
endforeach

# workarea-guard and idle-audit ship no metadata xml (config-less); the
# rest do.
install_data('edge-flipping.xml', 'workspace-wallpaper.xml', 'wsync.xml',
             'directional-focus.xml', 'enclave-mark.xml', 'surface-names.xml',
             'edge-warp.xml',
             install_dir : metadatadir)

# Multi-file plugins live in their own subdir with their own meson.build.
subdir('atlas')
