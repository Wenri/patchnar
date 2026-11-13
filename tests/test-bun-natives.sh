#!/bin/sh
# bun_graph --natives: the private Bun entry points TaskSolver's unbundled Claude
# Code calls, located by instruction signatures that must match exactly once.
# The fixture assembles each signature, a sibling sharing mi_free's first 36
# bytes, a bun.String.cloneLatin1-shaped function, the loader's clone call and a
# decoy with the same argument setup after a call to something else.
set -eu

if test "$(uname -m)" != x86_64 || test "$(uname)" != Linux; then
    exit 77
fi

BUN_GRAPH=${BUN_GRAPH:-../src/bun_graph}
BUN_GRAPH="$(cd "$(dirname "$BUN_GRAPH")" && pwd)/$(basename "$BUN_GRAPH")"
CC=${CC:-cc}
NM=${NM:-nm}

workdir=$(mktemp -d "${TMPDIR:-/tmp}/test-workdir-natives-XXXXXX")
trap 'rm -rf "$workdir"' EXIT HUP INT TERM
cd "$workdir"

fixture() {  # fixture NAME [EXTRA_ASSEMBLY]
    cat > "$1.S" <<EOF
    .text
    .globl _start
_start:
    ret
    .p2align 4
emit:
    .byte 0x55,0x48,0x89,0xe5,0x48,0x83,0xec,0x20,0x4c,0x89,0xc0,0x41,0x89,0xc8,0x89,0xd1
    .byte 0x0f,0x28,0x45,0x10,0x0f,0x11,0x44,0x24,0x08,0x4c,0x89,0x0c,0x24,0xba,0x01,0x00
    .byte 0x00,0x00,0x49,0x89,0xc1,0xe8,0x06,0x00,0x00,0x00,0x48,0x83,0xc4,0x20,0x5d,0xc3
    .p2align 4
destroy:
    .byte 0x55,0x48,0x89,0xe5,0x41,0x57,0x41,0x56,0x53,0x50,0x48,0x89,0xfb,0x8b,0x47,0x64
    .byte 0x4c,0x8b,0x77,0x58,0x48,0x85,0xc0,0x75,0x2d,0x83,0x7b,0x60,0x00,0x78,0x78,0x4d
    .byte 0x85,0xf6
    .p2align 4
free:
    .byte 0x55,0x48,0x89,0xe5,0x48,0x89,0xfe,0x48,0x8b,0x05,0x11,0x22,0x33,0x44,0x48,0x89
    .byte 0xf9,0x48,0xc1,0xe9,0x1d,0x48,0x8b,0x44,0xc8,0x50,0x89,0xf1,0xc1,0xe9,0x0d,0x81
    .byte 0xe1,0xf8,0xff,0x00,0x00,0x48,0x8b,0x3c,0x08,0x48,0x85,0xff,0x74,0x29,0x48,0x8b
    .byte 0x07
    .p2align 4
free_sibling:
    .byte 0x55,0x48,0x89,0xe5,0x48,0x89,0xfe,0x48,0x8b,0x05,0x55,0x66,0x77,0x08,0x48,0x89
    .byte 0xf9,0x48,0xc1,0xe9,0x1d,0x48,0x8b,0x44,0xc8,0x50,0x89,0xf1,0xc1,0xe9,0x0d,0x81
    .byte 0xe1,0xf8,0xff,0x00,0x00,0x90,0x90,0xc3
    .p2align 4
to_wtf:
    .byte 0x55,0x48,0x89,0xe5,0x53,0x50,0x48,0x89,0xfb,0x0f,0xb6,0x06,0x83,0xf8,0x03,0x75
    .byte 0x1d,0x48,0x8b,0x46,0x08,0x48,0x8b,0x56,0x10,0x48,0x89,0xdf,0x48,0x89,0xc6,0xe8
    .byte 0x00,0x00,0x00,0x00
    .p2align 4
clone:
    push %rbp
    mov %rsp, %rbp
    push %r14
    push %r12
    mov %rdx, %r14
    lea 0x14(%r14), %rdi
    call _start
    test %rax, %rax
    je 1f
    mov %rax, %r12
    add \$0x14, %rax
    movl \$2, (%r12)
    mov %r14d, 4(%r12)
    mov %rax, 8(%r12)
    movl \$4, 0x10(%r12)
1:  pop %r12
    pop %r14
    pop %rbp
    ret
    .p2align 4
other:
    ret
    .p2align 4
loader:
    push %rbp
    mov %rsp, %rbp
decoy:
    call other
    mov -0x120(%rbp), %rax
    mov 0x20(%rax), %rsi
    mov -0x50(%rbp), %rax
    mov 0x20(%rax), %rdx
    mov 0x28(%rax), %rcx
    lea -0x1d0(%rbp), %rdi
    call _start
${3:-site:
    call clone
    mov -0x120(%rbp), %rax
    mov 0x20(%rax), %rsi
    mov -0x50(%rbp), %rax
    mov 0x20(%rax), %rdx
    mov 0x28(%rax), %rcx
    lea -0x1d0(%rbp), %rdi
    call _start}
    pop %rbp
    ret
${2:-}
EOF
    "$CC" -nostdlib -static -no-pie -o "$1" "$1.S"
}

address() {  # address BINARY SYMBOL -> decimal
    echo $((0x$("$NM" "$1" | awk -v symbol="$2" '$3 == symbol { print $1 }')))
}

fixture natives
expected=$(printf '{"format": 1, "natives": {"module_bytecode_generator": %s, "cached_bytecode_destructor": %s, "mi_free": %s, "bun_string_to_wtf": %s, "latin1_clone": %s, "bundled_source_clone_call": %s}}' \
    "$(address natives emit)" "$(address natives destroy)" "$(address natives free)" \
    "$(address natives to_wtf)" "$(address natives clone)" "$(address natives site)")
actual=$("$BUN_GRAPH" natives --natives)
if test "$actual" != "$expected"; then
    echo "bun_graph --natives reported" >&2
    echo "  $actual" >&2
    echo "expected" >&2
    echo "  $expected" >&2
    exit 1
fi

# Inlined createIfDifferent(path.text) with register-held specifier and path (Claude Code 2.1.323+).
fixture natives_inlined "" "decoy_inlined:
    call other
    mov 0x20(%rbx), %rbx
    mov 0x20(%r15), %r14
    mov 0x28(%r15), %r15
    mov %rbx, %rdi
    mov %r14, %rsi
    mov %r15, %rdx
    call _start
    lea -0x270(%rbp), %rdi
site:
    call clone
    mov 0x20(%rbx), %rbx
    mov 0x20(%r15), %r14
    mov 0x28(%r15), %r15
    mov %rbx, %rdi
    mov %r14, %rsi
    mov %r15, %rdx
    call _start
    lea -0x270(%rbp), %rdi"
expected_inlined=$(printf '{"format": 1, "natives": {"module_bytecode_generator": %s, "cached_bytecode_destructor": %s, "mi_free": %s, "bun_string_to_wtf": %s, "latin1_clone": %s, "bundled_source_clone_call": %s}}' \
    "$(address natives_inlined emit)" "$(address natives_inlined destroy)" "$(address natives_inlined free)" \
    "$(address natives_inlined to_wtf)" "$(address natives_inlined clone)" "$(address natives_inlined site)")
actual_inlined=$("$BUN_GRAPH" natives_inlined --natives)
if test "$actual_inlined" != "$expected_inlined"; then
    echo "bun_graph --natives (inlined createIfDifferent) reported" >&2
    echo "  $actual_inlined" >&2
    echo "expected" >&2
    echo "  $expected_inlined" >&2
    exit 1
fi

# A second copy of any signature is ambiguous: no report, exit 2, the entry named.
fixture ambiguous "    .p2align 4
emit_again:
    .byte 0x55,0x48,0x89,0xe5,0x48,0x83,0xec,0x20,0x4c,0x89,0xc0,0x41,0x89,0xc8,0x89,0xd1
    .byte 0x0f,0x28,0x45,0x10,0x0f,0x11,0x44,0x24,0x08,0x4c,0x89,0x0c,0x24,0xba,0x01,0x00
    .byte 0x00,0x00,0x49,0x89,0xc1,0xe8,0x77,0x00,0x00,0x00,0x48,0x83,0xc4,0x20,0x5d,0xc3"
status=0
"$BUN_GRAPH" ambiguous --natives > report 2> errors || status=$?
if test "$status" != 2 || test -s report || ! grep -q '^module_bytecode_generator: 2 signature matches' errors; then
    echo "an ambiguous signature must fail with status 2 (got $status):" >&2
    cat report errors >&2
    exit 1
fi

fixture ambiguous_clone "    .p2align 4
site_again:
    call clone
    mov 0x20(%rbx), %rbx
    mov 0x20(%r15), %r14
    mov 0x28(%r15), %r15
    mov %rbx, %rdi
    mov %r14, %rsi
    mov %r15, %rdx
    call _start
    lea -0x270(%rbp), %rdi
    ret"
status=0
"$BUN_GRAPH" ambiguous_clone --natives > report 2> errors || status=$?
if test "$status" != 2 || test -s report || ! grep -q '^bundled_source_clone_call: 2 loader call sites, not 1' errors; then
    echo "an ambiguous clone call must fail with status 2 (got $status):" >&2
    cat report errors >&2
    exit 1
fi

echo "bun_graph --natives: entry points located; ambiguity rejected"
