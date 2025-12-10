project('appimage-thumbnailer', 'c',
  version: 'v3.0.0',
  default_options: ['warning_level=3', 'c_std=c11']
)

cc = meson.get_compiler('c')
glib_dep = dependency('glib-2.0', version: '>=2.56')
gio_dep = dependency('gio-2.0', version: '>=2.56')
gdk_pixbuf_dep = dependency('gdk-pixbuf-2.0', version: '>=2.42')
librsvg_dep = dependency('librsvg-2.0', version: '>=2.54')
cairo_dep = dependency('cairo')
m_dep = cc.find_library('m', required: false)

add_project_arguments('-DAPPIMAGE_THUMBNAILER_VERSION="@0@"'.format(meson.project_version()),
  language: 'c'
)

declared_deps = [glib_dep, gio_dep, gdk_pixbuf_dep, librsvg_dep, cairo_dep]
if m_dep.found()
  declared_deps += m_dep
endif

# DwarFS tools configuration
dwarfs_version = '0.14.1'
dwarfs_tools_dir = get_option('prefix') / get_option('libdir') / 'appimage-thumbnailer'

# Map host CPU to DwarFS release naming
host_cpu = host_machine.cpu_family()
dwarfs_arch_map = {
  'x86_64': 'x86_64',
  'aarch64': 'aarch64',
  'arm': 'arm',
  'x86': 'i386',
  'ppc64': 'ppc64',
  'ppc64le': 'ppc64le',
  'riscv64': 'riscv64',
  's390x': 's390x',
}

dwarfs_arch = dwarfs_arch_map.get(host_cpu, '')

# Option to skip bundling dwarfs tools (for distro packaging)
bundle_dwarfs = get_option('bundle_dwarfs')

subdir('src')

# Install bundled DwarFS tools if enabled and architecture is supported
if bundle_dwarfs and dwarfs_arch != ''
  fetch_script = files('scripts/fetch-dwarfs-tools.sh')
  
  # Custom target to download and extract dwarfs tools during build
  dwarfs_tools = custom_target('dwarfs-tools',
    output: ['dwarfsextract', 'dwarfsck'],
    command: [
      'sh', fetch_script, dwarfs_version, dwarfs_arch, meson.current_build_dir()
    ],
    install: true,
    install_dir: dwarfs_tools_dir
  )
elif bundle_dwarfs and dwarfs_arch == ''
  warning('DwarFS tools bundling requested but architecture "@0@" is not supported'.format(host_cpu))
endif

thumbnailer_conf = configuration_data()
thumbnailer_binary = join_paths(get_option('prefix'), get_option('bindir'), 'appimage-thumbnailer')
thumbnailer_conf.set('EXEC_PATH', thumbnailer_binary)
thumbnailer_conf.set('TRYEXEC_PATH', thumbnailer_binary)

thumbnailer_file = configure_file(
  input: 'appimage-thumbnailer.thumbnailer.in',
  output: 'appimage-thumbnailer.thumbnailer',
  configuration: thumbnailer_conf
)

install_data(thumbnailer_file,
  install_dir: join_paths(get_option('datadir'), 'thumbnailers')
)

# Add MIME type association during installation
meson.add_install_script('xdg-mime', 'default', 'appimage-thumbnailer.desktop', 'application/vnd.appimage')

# Clear thumbnail cache so the updated handler gets picked up immediately
meson.add_install_script('sh', '-c', 'rm -rf ~/.cache/thumbnails/*')
