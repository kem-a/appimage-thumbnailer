# Directory where bundled tools will be installed
tools_dir = get_option('prefix') / get_option('libdir') / 'appimage-thumbnailer'

executable('appimage-thumbnailer',
  'appimage-thumbnailer.c',
  'appimage-type.c',
  'dwarfs-extract.c',
  'squashfs-extract.c',
  dependencies: declared_deps,
  c_args: [
    '-DDWARFS_TOOLS_DIR="@0@"'.format(tools_dir),
    '-DSQUASHFS_TOOLS_DIR="@0@"'.format(tools_dir),
  ],
  install: true
)
