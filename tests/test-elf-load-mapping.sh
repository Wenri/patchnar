#!/bin/sh
# Regressions from upstream patchelf's executable-page-growth.sh and
# strip-gap-rewrite.sh (including e21fc60's headerBase precondition).
set -eu

if test "$(uname -m)" != x86_64 || test "$(uname)" != Linux; then
    exit 77
fi

PATCHELF=${PATCHELF:-../src/patchelf}
PATCHELF="$(cd "$(dirname "$PATCHELF")" && pwd)/$(basename "$PATCHELF")"
READELF=${READELF:-readelf}
STRIP=${STRIP:-strip}
CC=${CC:-cc}

workdir=$(mktemp -d "${TMPDIR:-/tmp}/test-workdir-elf-XXXXXX")
trap 'rm -rf "$workdir"' EXIT HUP INT TERM
cd "$workdir"

# Keep the upstream fixture's large BSS and shared-library dependency chain.
cat > main.c <<'EOF'
#include <stdio.h>
char buf[16 * 1024 * 1024];
int foo(void);
int main(void) { puts("Hello World"); return foo(); }
EOF
cat > foo.c <<'EOF'
const char interp[] __attribute__((section(".interp"))) = "/foo/bar";
int bar(void);
int foo(void) { return 12 + bar(); }
EOF
cat > bar.c <<'EOF'
int bar(void) { return 34; }
EOF

# Deliberate word splitting permits configured compiler commands such as ccache.
$CC -fPIC -shared bar.c -o libbar.so
$CC -fPIC -shared foo.c -L. -lbar -o libfoo.so
$CC -fno-pie -no-pie main.c -L. -Wl,-rpath-link,. -lfoo -o main
$CC -fno-pie -no-pie main.c -L. -Wl,-rpath-link,. -lfoo \
    -Wl,-rpath,/nonexistent -o main-rpath

first_load_addr() {
    "$READELF" -lW "$1" | awk '$1 == "LOAD" { print $3; exit }'
}

check_runs() {
    exit_code=0
    LD_LIBRARY_PATH=. "$1" || exit_code=$?
    test "$exit_code" -eq 46
}

original_load=$(first_load_addr main)
"$PATCHELF" --set-rpath "$(printf '%0800d' 0)" main
modified_load=$(first_load_addr main)
# 800 bytes fit into one extra page, without a second unnecessary page.
test "$((original_load - modified_load))" -eq 4096
check_runs ./main
echo "PASS: executable grows by one page"

# With an RPATH already present, this moves .dynstr without moving .dynamic.
# The oversized page leaves padding between LOADs that strip removes.
"$PATCHELF" --page-size 8192 --set-rpath "$(printf '%0800d' 0)" main-rpath
"$STRIP" main-rpath
"$READELF" -lW main-rpath | awk '$1 == "LOAD" { print $3, $2 }' | {
    read -r addr1 off1
    read -r addr2 off2
    test "$((addr1 - off1))" -ne "$((addr2 - off2))"
}
"$PATCHELF" --set-rpath "$(printf '%01600d' 0)" main-rpath
check_runs ./main-rpath
echo "PASS: stripped executable retains valid LOAD header mapping"
