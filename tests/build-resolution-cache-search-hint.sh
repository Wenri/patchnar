#! /bin/sh -e
# Run-path entries that can't be resolved at patch time -- dynamic-string
# tokens ($ORIGIN/$LIB/$PLATFORM), glibc-hwcaps directories, and components
# that only resolve or exist at run time -- must be recorded as "?<dir>"
# search hints, never as absolute "=<path>" entries.
SCRATCH=scratch/$(basename "$0" .sh)
READELF=${READELF:-readelf}
PATCHELF=$(readlink -f "../src/patchelf")

rm -rf "${SCRATCH}"
mkdir -p "${SCRATCH}/libs"

cp libfoo.so "${SCRATCH}/libs/"

here=$(pwd)
libs="${here}/${SCRATCH}/libs"

descriptor() {
    ${READELF} -p .note.nixos.ldcache "$1"
}

# Sets the run path on a fresh copy of main, builds the cache, and leaves the
# descriptor dump in $d for the expect_* helpers.
make_cached() {
    cp main "$1"
    ${PATCHELF} --set-rpath "$2" "$1"
    ${PATCHELF} --build-resolution-cache "$1"
    d=$(descriptor "$1")
    echo "$d"
}

expect_entry() {
    if ! echo "$d" | grep -qF "$1"; then
        echo "FAIL: $2"
        exit 1
    fi
}

expect_no_entry() {
    if echo "$d" | grep -qF "$1"; then
        echo "FAIL: $2"
        exit 1
    fi
}

# $ORIGIN is a literal loader token; it must reach patchelf unexpanded.
# shellcheck disable=SC2016
make_cached "${SCRATCH}/main-origin" '$ORIGIN/libs'
# shellcheck disable=SC2016
expect_entry '?$ORIGIN/libs' "\$ORIGIN run path was not recorded as a '?' search hint"
# shellcheck disable=SC2016
expect_no_entry '=$ORIGIN/libs' "\$ORIGIN run path was wrongly baked into an absolute '=' path"

# libfoo.so is present here, so the hwcaps guard must force a "?" hint anyway.
mkdir -p "${SCRATCH}/hw/glibc-hwcaps"
cp libfoo.so "${SCRATCH}/hw/"
make_cached "${SCRATCH}/main-hwcaps" "${here}/${SCRATCH}/hw"
expect_entry "?${here}/${SCRATCH}/hw" \
    "glibc-hwcaps directory was not recorded as a '?' search hint"
expect_no_entry "=${here}/${SCRATCH}/hw/libfoo.so" \
    "library under a glibc-hwcaps dir was wrongly resolved to a path"

# An empty component is the run-time current directory: a bare "?" hint.
make_cached "${SCRATCH}/main-cwd" ":${libs}"
expect_entry "?:=${libs}/libfoo.so" \
    "empty run-path component was not recorded as a '?' hint before the exact entry"

# A relative component must not be baked in against patchelf's own cwd.
make_cached "${SCRATCH}/main-rel" "relative/dir:${libs}"
expect_entry "?relative/dir:=${libs}/libfoo.so" \
    "relative run-path component was not recorded as a '?' search hint"
expect_no_entry "=relative/dir/libfoo.so" \
    "relative run-path component was wrongly baked into an '=' path"

# An absent directory may be populated at run time (e.g. /run/opengl-driver/lib
# in a build sandbox): keep it as a "?" hint.
make_cached "${SCRATCH}/main-missing" "${here}/${SCRATCH}/does-not-exist:${libs}"
expect_entry "?${here}/${SCRATCH}/does-not-exist:=${libs}/libfoo.so" \
    "missing run-path directory was not recorded as a '?' hint before the exact entry"

# A trailing empty component ("libs:") is a final CWD search position: kept as
# a trailing "?" hint, not dropped by the splitter.
make_cached "${SCRATCH}/main-cwd-trailing" "${libs}:"
expect_entry "=${libs}/libfoo.so:?" \
    "trailing empty run-path component was not recorded as a trailing '?' hint"

# A plain-file entry is not a searchable directory; keep it as a "?" hint.
touch "${SCRATCH}/not-a-dir"
make_cached "${SCRATCH}/main-notdir" "${here}/${SCRATCH}/not-a-dir:${libs}"
expect_entry "?${here}/${SCRATCH}/not-a-dir:=${libs}/libfoo.so" \
    "plain-file run-path entry was not recorded as a '?' hint before the exact entry"

# A directory unsearchable by the patching user may be readable at run time:
# keep it as a "?" hint. Skip as root, which ignores directory permissions.
if [ "$(id -u)" != 0 ]; then
    mkdir -p "${SCRATCH}/no-access"
    chmod 000 "${SCRATCH}/no-access"
    make_cached "${SCRATCH}/main-noaccess" "${here}/${SCRATCH}/no-access:${libs}"
    chmod 700 "${SCRATCH}/no-access"
    expect_entry "?${here}/${SCRATCH}/no-access:=${libs}/libfoo.so" \
        "unsearchable run-path directory was not recorded as a '?' hint before the exact entry"
fi

echo "PASS"
