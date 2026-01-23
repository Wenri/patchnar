set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_EXTENSIONS OFF)

set(PAGESIZE 4096)

add_executable(${PROJECT_NAME} patchelf.cc elf.h patchelf.h)

target_compile_definitions(
  patchelf PRIVATE PAGESIZE=${PAGESIZE}
                   PACKAGE_STRING="patchelf ${VERSION_STRING}")

install(TARGETS patchelf RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

# patchnar - NAR stream patcher for Android
find_package(PkgConfig REQUIRED)
pkg_check_modules(SOURCE_HIGHLIGHT REQUIRED source-highlight>=3.0)
find_package(Threads REQUIRED)

add_executable(patchnar patchnar.cc nar.cc nar.h patchelf.cc elf.h patchelf.h)

target_compile_definitions(
  patchnar PRIVATE PAGESIZE=${PAGESIZE}
                   PACKAGE_STRING="patchelf ${VERSION_STRING}"
                   PATCHELF_AS_LIBRARY)

target_include_directories(patchnar PRIVATE ${SOURCE_HIGHLIGHT_INCLUDE_DIRS})
target_link_libraries(patchnar PRIVATE ${SOURCE_HIGHLIGHT_LIBRARIES} Threads::Threads)

install(TARGETS patchnar RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
