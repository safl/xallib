project('xal', 'c',
  version: '0.1.0',
  meson_version: '>=0.55.0',
  default_options: [
    'c_std=gnu11',
    'warning_level=2',
    'buildtype=release',
    'default_library=both',
    'b_staticpic=true'
  ],
)

cc = meson.get_compiler('c')

conf_data = configuration_data()
conf_data.set('XAL_DEBUG_ENABLED', get_option('debug_messages'))
conf = configure_file(
  configuration : conf_data,
  output : 'xal_config.h',
)

xallib_libdir = get_option('libdir')
xallib_rpath = get_option('prefix') + '/' + xallib_libdir

add_project_arguments(['-include', 'xal_config.h'], language: 'c')

rt_dep = cc.find_library('rt', required: true)
if not cc.has_function('clock_gettime', prefix: '#include <time.h>', dependencies: rt_dep)
  error('clock_gettime not found')
endif

xnvme_dep = dependency(
  'xnvme',
  version: '>=0.7.0',
  required: true
)

xallib_deps = [
  rt_dep,
  xnvme_dep,
]

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

include_dirs = include_directories('include', 'tp')

xal_library = library('xal',
  sources: sources,
  dependencies: xallib_deps,
  include_directories: include_dirs,
  install: true,
)

xal_cli = executable('xal',
  sources: 'src/cli.c',
  dependencies: xallib_deps,
  link_with: xal_library,
  include_directories: include_dirs,
  install_rpath: xallib_rpath,
  install: true
)

subdir('tools')

install_headers(public_headers, subdir: 'xal')
