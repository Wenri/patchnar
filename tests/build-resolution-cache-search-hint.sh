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
# descriptor dump in $d for the expect_* helpers below.
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

# libfoo.so is present here, so without the hwcaps guard it would resolve to an
# absolute "=path"; the guard must force a "?" hint regardless.
mkdir -p "${SCRATCH}/hw/glibc-hwcaps"
cp libfoo.so "${SCRATCH}/hw/"
make_cached "${SCRATCH}/main-hwcaps" "${here}/${SCRATCH}/hw"
expect_entry "?${here}/${SCRATCH}/hw" \
    "glibc-hwcaps directory was not recorded as a '?' search hint"
expect_no_entry "=${here}/${SCRATCH}/hw/libfoo.so" \
    "library under a glibc-hwcaps dir was wrongly resolved to a path"

# An empty run-path component (the caller's current directory at run time)
# only resolves at run time; it must become a bare "?" hint, kept in run-path
# order, so the later exact libs entry cannot bypass that search position.
make_cached "${SCRATCH}/main-cwd" ":${libs}"
expect_entry "?:=${libs}/libfoo.so" \
    "empty run-path component was not recorded as a '?' hint before the exact entry"

# A relative component would otherwise be probed against patchelf's own
# working directory and could be baked in as a bogus relative "=" path.
make_cached "${SCRATCH}/main-rel" "relative/dir:${libs}"
expect_entry "?relative/dir:=${libs}/libfoo.so" \
    "relative run-path component was not recorded as a '?' search hint"
expect_no_entry "=relative/dir/libfoo.so" \
    "relative run-path component was wrongly baked into an '=' path"

# An absolute directory absent at patch time may be populated at run time
# (e.g. /run/opengl-driver/lib inside a build sandbox); it must be recorded
# as a "?" hint in run-path order, so the later exact libs entry cannot
# bypass that search position.
make_cached "${SCRATCH}/main-missing" "${here}/${SCRATCH}/does-not-exist:${libs}"
expect_entry "?${here}/${SCRATCH}/does-not-exist:=${libs}/libfoo.so" \
    "missing run-path directory was not recorded as a '?' hint before the exact entry"

# A trailing empty run-path component ("libs:") is a CWD search position at
# the end of the search order, just like a leading one; it must be preserved
# as a trailing bare "?" hint after the exact entry, not silently dropped by
# the run-path splitter.
make_cached "${SCRATCH}/main-cwd-trailing" "${libs}:"
expect_entry "=${libs}/libfoo.so:?" \
    "trailing empty run-path component was not recorded as a trailing '?' hint"

# A run-path entry that exists as a plain file is not a searchable directory;
# like a missing directory, it may be a placeholder that becomes a populated
# directory at run time, so it must keep its search position as a "?" hint
# instead of being silently dropped.
touch "${SCRATCH}/not-a-dir"
make_cached "${SCRATCH}/main-notdir" "${here}/${SCRATCH}/not-a-dir:${libs}"
expect_entry "?${here}/${SCRATCH}/not-a-dir:=${libs}/libfoo.so" \
    "plain-file run-path entry was not recorded as a '?' hint before the exact entry"

# A directory that exists but cannot be searched by the patching user may be
# readable at run time; its position must also be kept as a "?" hint. Root
# ignores directory permissions, so the probe behaves differently there.
if [ "$(id -u)" != 0 ]; then
    mkdir -p "${SCRATCH}/no-access"
    chmod 000 "${SCRATCH}/no-access"
    make_cached "${SCRATCH}/main-noaccess" "${here}/${SCRATCH}/no-access:${libs}"
    chmod 700 "${SCRATCH}/no-access"
    expect_entry "?${here}/${SCRATCH}/no-access:=${libs}/libfoo.so" \
        "unsearchable run-path directory was not recorded as a '?' hint before the exact entry"
fi

echo "PASS"
