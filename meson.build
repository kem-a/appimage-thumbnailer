project('appimage-thumbnailer', 'c',
  version: 'v2.0.2',
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

subdir('src')

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
