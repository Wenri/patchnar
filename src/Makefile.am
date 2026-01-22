AM_CXXFLAGS = -Wall -Wextra -Wcast-qual -std=c++17 -D_FILE_OFFSET_BITS=64

if WITH_ASAN
AM_CXXFLAGS += -fsanitize=address -fsanitize-address-use-after-scope
endif

if WITH_UBSAN
AM_CXXFLAGS += -fsanitize=undefined
endif

SAN_FLAGS = -fno-optimize-sibling-calls -fno-omit-frame-pointer
if WITH_ASAN
AM_CXXFLAGS += $(SAN_FLAGS)
else
if WITH_UBSAN
AM_CXXFLAGS += $(SAN_FLAGS)
endif
endif

bin_PROGRAMS = patchelf patchnar

patchelf_SOURCES = patchelf.cc elf.h patchelf.h

# patchnar uses patchelf as library (PATCHELF_AS_LIBRARY excludes main)
# Links against source-highlight for language detection and pthread for thread pool
patchnar_SOURCES = patchnar.cc nar.cc nar.h patchelf.cc elf.h patchelf.h
patchnar_CXXFLAGS = $(AM_CXXFLAGS) -DPATCHELF_AS_LIBRARY $(SOURCE_HIGHLIGHT_CFLAGS) -pthread
patchnar_LDFLAGS = -pthread
patchnar_LDADD = $(SOURCE_HIGHLIGHT_LIBS)
