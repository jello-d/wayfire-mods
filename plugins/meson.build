project('tackup-wayfire-plugins', 'cpp',
        version : '0.1.0',
        default_options : ['cpp_std=c++17'])

# Built against the installed wayfire in /opt/wayfire (via PKG_CONFIG_PATH),
# so these stay decoupled from the wayfire-plugins-extra version.
wayfire     = dependency('wayfire')
metadatadir = wayfire.get_variable(pkgconfig : 'metadatadir')
plugindir   = join_paths(get_option('libdir'), 'wayfire')

plugins = ['edge-flipping', 'workspace-wallpaper', 'wsync',
           'directional-focus', 'workarea-guard']
foreach name : plugins
    shared_module(name, name + '.cpp',
                  dependencies : [wayfire],
                  install      : true,
                  install_dir  : plugindir)
endforeach

# workarea-guard ships no metadata xml (config-less); the rest do.
install_data('edge-flipping.xml', 'workspace-wallpaper.xml', 'wsync.xml',
             'directional-focus.xml',
             install_dir : metadatadir)

# Multi-file plugins live in their own subdir with their own meson.build.
subdir('atlas')
