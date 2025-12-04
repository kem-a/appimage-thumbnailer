project('appimage-thumbnailer', 'c',
  version: '1.1.0',
  default_options: ['warning_level=3', 'c_std=c11']
)

cc = meson.get_compiler('c')
glib_dep = dependency('glib-2.0', version: '>=2.56')
gio_dep = dependency('gio-2.0', version: '>=2.56')
gdk_pixbuf_dep = dependency('gdk-pixbuf-2.0', version: '>=2.42')
librsvg_dep = dependency('librsvg-2.0', version: '>=2.54')
cairo_dep = dependency('cairo')
m_dep = cc.find_library('m', required: false)

declared_deps = [glib_dep, gio_dep, gdk_pixbuf_dep, librsvg_dep, cairo_dep]
if m_dep.found()
  declared_deps += m_dep
endif

subdir('src')

install_data('appimage-thumbnailer.thumbnailer',
  install_dir: join_paths(get_option('datadir'), 'thumbnailers')
)
