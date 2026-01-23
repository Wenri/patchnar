# Configure DEFAULT_PAGESIZE via config.h
confdata = configuration_data()
page_size = get_option('page_size')
if page_size != 'auto'
  confdata.set_quoted('DEFAULT_PAGESIZE', page_size)
else
  # For "auto", leave it undefined so runtime detection happens
endif

config_h = configure_file(output : 'config.h', configuration : confdata)

# Dependencies
thread_dep = dependency('threads')
boost_dep = dependency('boost')  # Required by source-highlight
srchilite_dep = dependency('source-highlight', version : '>=3.0')
tbb_dep = dependency('tbb')

# Common sources for patchelf functionality
patchelf_sources = ['patchelf.cc', 'patchelf.h']

executable(
  'patchelf',
  [ patchelf_sources, config_h ],
  include_directories : include_directories('.'),
  cpp_args : [ '-include', meson.current_build_dir() / 'config.h' ],
  install : true,
)

# patchnar - NAR stream patcher for Android
executable(
  'patchnar',
  [ 'patchnar.cc', 'nar.cc', 'nar.h', patchelf_sources, config_h ],
  include_directories : include_directories('.'),
  cpp_args : [
    '-include', meson.current_build_dir() / 'config.h',
    '-DPATCHELF_AS_LIBRARY',  # Exclude patchelf's main()
  ],
  dependencies : [ thread_dep, boost_dep, srchilite_dep, tbb_dep ],
  install : true,
)
