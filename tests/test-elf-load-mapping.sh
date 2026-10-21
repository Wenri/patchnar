#!/bin/sh
# Regressions from upstream patchelf's executable-page-growth.sh,
# strip-gap-rewrite.sh (including e21fc60's headerBase precondition and
# 59acc3d9's 64 KiB page case) and executable-header-rewrite.sh (59acc3d9:
# header LOAD reuse without overlapping mappings).
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
# The oversized page leaves padding between LOADs that strip removes.  With
# the 64 KiB page, the replacement needs more than one additional page, so
# growth must still reserve enough file space when it reuses the header LOAD.
cp main-rpath main-rpath.orig
for page_size in 8192 65536; do
    cp main-rpath.orig main-rpath
    "$PATCHELF" --page-size "$page_size" --set-rpath "$(printf '%0800d' 0)" main-rpath
    "$STRIP" main-rpath
    "$READELF" -lW main-rpath | awk '$1 == "LOAD" { print $3, $2 }' | {
        read -r addr1 off1
        read -r addr2 off2
        test "$((addr1 - off1))" -ne "$((addr2 - off2))"
    }
    size=1600
    if test "$page_size" -eq 65536; then
        size=16000
    fi
    "$PATCHELF" --set-rpath "$(printf "%0${size}d" 0)" main-rpath
    check_runs ./main-rpath
done
echo "PASS: stripped executable retains valid LOAD header mapping"

# Large symbol tables make the second rewrite extend the small header LOAD
# created for .interp; without an RPATH, .dynamic must also become writable,
# and later growth must reuse the header LOAD without overlapping mappings
# or needless LOAD splits (upstream's many-syms-no-pie fixture).
i=1
while test "$i" -le 2000; do
    echo "void f$i(void) {}"
    i=$((i + 1))
done > many-syms.c
{
    echo "int main(void) {"
    i=1
    while test "$i" -le 2000; do
        echo "void f$i(void); f$i();"
        i=$((i + 1))
    done
    echo "return 0; }"
} > many-syms-main.c
$CC -fPIC -shared many-syms.c -o libmany-syms.so
$CC -fno-pie -no-pie many-syms-main.c -L. -Wl,--disable-new-dtags -Wl,-rpath-link,. \
    -Wl,-z,noseparate-code -Wl,--hash-style=both -Wl,-rpath,/nonexistent \
    -lmany-syms -o many-syms-no-pie

page_size=$(getconf PAGESIZE)

check_load_pages() {
    "$READELF" -lW "$1" | awk '$1 == "LOAD" { print $3, $6 }' | {
        previous_end=0
        while read -r addr size; do
            test "$((addr / page_size * page_size))" -ge "$previous_end"
            previous_end=$(((addr + size + page_size - 1) / page_size * page_size))
        done
        test "$previous_end" -gt 0
    }
}

check_dynamic_writable() {
    dynamic=$("$READELF" -lW "$1" | awk '$1 == "DYNAMIC" { print $3, $6 }')
    read -r addr size <<EOF
$dynamic
EOF
    dynamic_start=$((addr))
    dynamic_end=$((addr + size))
    test "$dynamic_end" -gt "$dynamic_start"
    # Check the LOAD permissions, since PT_DYNAMIC's flags do not control
    # the mapping the loader writes DT_DEBUG into.
    "$READELF" -lW "$1" | awk '$1 == "LOAD" && /W/ { print $3, $6 }' | {
        writable=0
        while read -r addr size; do
            if test "$((addr))" -le "$dynamic_start" &&
                test "$((addr + size))" -ge "$dynamic_end"; then
                writable=1
            fi
        done
        test "$writable" -eq 1
    }
}

interpreter=$("$PATCHELF" --print-interpreter many-syms-no-pie)
new_interpreter="$workdir/$(printf '%0200d' 0)"
ln -s "$interpreter" "$new_interpreter"
for rpath in present absent; do
    cp many-syms-no-pie many-syms-rewrite
    if test "$rpath" = absent; then
        "$PATCHELF" --remove-rpath many-syms-rewrite
    fi
    "$PATCHELF" --set-interpreter "$new_interpreter" many-syms-rewrite
    "$PATCHELF" --set-rpath "$workdir" many-syms-rewrite
    check_load_pages many-syms-rewrite
    check_dynamic_writable many-syms-rewrite
    ./many-syms-rewrite
    for size in 6000 16000; do
        "$PATCHELF" --set-rpath "$workdir:$(printf "%0${size}d" 0)" many-syms-rewrite
        check_load_pages many-syms-rewrite
        check_dynamic_writable many-syms-rewrite
        ./many-syms-rewrite
    done
done
echo "PASS: executable header rewrite keeps LOAD mappings disjoint"
