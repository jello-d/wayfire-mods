# atlas -- multi-output overview/switcher. Built against the installed wayfire
# like the single-TU plugins in the parent meson.build. `wayfire`, `plugindir`
# and `metadatadir` are inherited from the parent (this dir is subdir()'d after
# they are defined).
#
# global-wall.cpp is the vendored global workspace-wall variant (the wall mode
# renderer); global-switcher.cpp is the cross-output alt+tab (switch/switch_all)
# overlay. No extra link deps yet; when drag lands it will link
# libwayfire-move-drag-interface -- add it then.

# '.' for atlas headers; '..' so <stb/stb_image.h> (the vendored image loader
# under build/src/wayfire/stb/) resolves for the ring's wallpaper backdrop.
atlas_inc = include_directories('.', '..')

shared_module('atlas',
              'atlas.cpp',
              'global-wall.cpp',
              'global-switcher.cpp',
              'global-ring.cpp',
              dependencies        : [wayfire],
              include_directories : [atlas_inc],
              install             : true,
              install_dir         : plugindir)

install_data('atlas.xml', install_dir : metadatadir)
