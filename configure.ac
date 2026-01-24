AC_PREREQ([2.62])
AC_INIT([patchnar], m4_esyscmd([printf $(cat ./version)]))
AC_CONFIG_SRCDIR([src/patchnar.cc])
AC_CONFIG_AUX_DIR([build-aux])
AM_INIT_AUTOMAKE([1.11.1 -Wall -Werror dist-bzip2 foreign color-tests parallel-tests])
AC_CONFIG_MACRO_DIR([m4])

AC_CHECK_TOOL([STRIP], [strip])

AM_PROG_CC_C_O
AC_PROG_CXX
AC_LANG([C++])
AM_PROG_AS

# Check for pkg-config
PKG_PROG_PKG_CONFIG

# Check for source-highlight library (required for patchnar language detection)
PKG_CHECK_MODULES([SOURCE_HIGHLIGHT], [source-highlight >= 3.0],
    [],
    [AC_MSG_ERROR([source-highlight >= 3.0 is required for language detection])])

# Source-highlight data directory (contains .lang files)
# Auto-detect from pkg-config, allow override via --with-source-highlight-data-dir
AC_ARG_WITH([source-highlight-data-dir],
   AS_HELP_STRING([--with-source-highlight-data-dir=DIR], [Path to source-highlight data files (auto-detected if not specified)]),
   [SOURCE_HIGHLIGHT_DATA_DIR="$withval"],
   [SOURCE_HIGHLIGHT_DATA_DIR=""]
)

# Auto-detect if not specified
if test -z "$SOURCE_HIGHLIGHT_DATA_DIR"; then
    SOURCE_HIGHLIGHT_DATA_DIR=`$PKG_CONFIG --variable=pkgdatadir source-highlight 2>/dev/null`
fi

# Verify the directory exists and contains .lang files
if test -z "$SOURCE_HIGHLIGHT_DATA_DIR"; then
    AC_MSG_ERROR([Could not detect source-highlight data directory. Use --with-source-highlight-data-dir=DIR])
elif test ! -d "$SOURCE_HIGHLIGHT_DATA_DIR"; then
    AC_MSG_ERROR([source-highlight data directory does not exist: $SOURCE_HIGHLIGHT_DATA_DIR])
elif test ! -f "$SOURCE_HIGHLIGHT_DATA_DIR/sh.lang"; then
    AC_MSG_ERROR([source-highlight data directory missing .lang files: $SOURCE_HIGHLIGHT_DATA_DIR])
fi

AC_MSG_RESULT([Using source-highlight data directory: $SOURCE_HIGHLIGHT_DATA_DIR])
AC_DEFINE_UNQUOTED([SOURCE_HIGHLIGHT_DATA_DIR], ["${SOURCE_HIGHLIGHT_DATA_DIR}"],
    [Path to source-highlight data files (.lang files)])

DEFAULT_PAGESIZE=auto
AC_ARG_WITH([page-size],
   AS_HELP_STRING([--with-page-size=SIZE], [Specify default pagesize (default auto)]),
   DEFAULT_PAGESIZE=$withval
)

if test "$DEFAULT_PAGESIZE" != auto; then
    AC_DEFINE_UNQUOTED(DEFAULT_PAGESIZE, ${DEFAULT_PAGESIZE})
    AC_MSG_RESULT([Setting page size to ${DEFAULT_PAGESIZE}])
fi

AC_ARG_WITH([asan],
   AS_HELP_STRING([--with-asan], [Build with address sanitizer])
)
AM_CONDITIONAL([WITH_ASAN], [test x"$with_asan" = xyes])

AX_CXX_COMPILE_STDCXX([23], [noext], [])

AC_ARG_WITH([ubsan],
   AS_HELP_STRING([--with-ubsan], [Build with undefined behavior sanitizer])
)
AM_CONDITIONAL([WITH_UBSAN], [test x"$with_ubsan" = xyes])

AC_CONFIG_FILES([Makefile src/Makefile])
AC_OUTPUT
