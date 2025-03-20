project('xal', 'c',
  version: '0.1.0',
  meson_version: '>=0.55.0',
  default_options: ['c_std=c11', 'warning_level=3']
)

conf_data = configuration_data()
conf_data.set('XAL_DEBUG_ENABLED', get_option('buildtype') == 'debug')
conf = configure_file(
  configuration : conf_data,
  output : 'xal_config.h',
)

add_project_arguments(['-include', 'xal_config.h'], language: 'c')

public_headers = files(
  'include/libxal.h',
  'include/libxal_util.h',
)

sources = files(
  'src/xal.c',
  'src/xal_pool.c',
  'src/pp.c',
  'src/utils.c'
)

include_dirs = include_directories('include')

xal_library = static_library('xal',
  sources: sources,
  include_directories: include_dirs,
  install: true
)

xal_cli = executable('xal',
  sources: 'src/cli.c',
  link_with: xal_library,
  include_directories: include_dirs,
  install: true
)

install_headers(public_headers, subdir: 'xal')
