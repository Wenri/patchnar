#!/bin/sh
# bun_graph --clean: the runtime without its standalone module graph, for both
# payload layouts `bun build --compile` has used (the writable PT_LOAD grown
# over the payload; PT_BUN_PAYLOAD with its pieces), checked field by field by
# the fixture that wrote the image.  Also: the report names the layout, an
# ahead-of-time compiled payload refuses --extract, and an image without a
# payload refuses --clean.
set -eu
if test "$(uname)" != Linux; then
    exit 77
fi
BUN_GRAPH=${BUN_GRAPH:-../src/bun_graph}
BUN_GRAPH="$(cd "$(dirname "$BUN_GRAPH")" && pwd)/$(basename "$BUN_GRAPH")"
CC=${CC:-cc}
srcdir=${srcdir:-$(dirname "$0")}
fixture_source="$(cd "$srcdir" && pwd)/bun-clean-fixture.c"
workdir=$(mktemp -d "${TMPDIR:-/tmp}/test-workdir-bun-clean-XXXXXX")
trap 'rm -rf "$workdir"' EXIT HUP INT TERM
cd "$workdir"
"$CC" -std=c11 -Wall -o fixture "$fixture_source"

for layout in section segment; do
    ./fixture gen "$layout" "$layout.elf"
    "$BUN_GRAPH" "$layout.elf" --clean "$layout.clean" > clean.out
    grep -q "^clean runtime: $layout.clean (" clean.out || {
        echo "--clean did not report the $layout image:" >&2; cat clean.out >&2; exit 1; }
    ./fixture check "$layout" "$layout.elf" "$layout.clean"
    "$BUN_GRAPH" "$layout.elf" > report.out 2> report.err || true
    grep -q "^payload layout: $layout" report.out || {
        echo "the report does not name the $layout layout:" >&2; cat report.out report.err >&2; exit 1; }
    "$BUN_GRAPH" "$layout.clean" --clean twice 2> twice.err && { echo "a clean image must refuse --clean" >&2; exit 1; }
    grep -q 'not found' twice.err
done

# Ahead-of-time compiled code (the segment fixture's executable piece) is reported and not extractable.
grep -q 'ahead-of-time' report.out
status=0
"$BUN_GRAPH" segment.elf --extract out > extract.out 2> extract.err || status=$?
test "$status" = 1 && grep -q 'ahead-of-time compiled code' extract.err || {
    echo "an ahead-of-time payload must refuse --extract (status $status):" >&2; cat extract.out extract.err >&2; exit 1; }

./fixture gen none none.elf
status=0
"$BUN_GRAPH" none.elf --clean none.clean 2> none.err || status=$?
test "$status" = 1 && grep -q '.bun section not found' none.err && ! test -e none.clean || {
    echo "an image without a payload must refuse --clean (status $status):" >&2; cat none.err >&2; exit 1; }
"$BUN_GRAPH" section.elf --clean 2> usage.err && exit 1
"$BUN_GRAPH" section.elf --natives --clean x 2> modes.err && exit 1
grep -q 'separate modes' modes.err

echo "bun_graph --clean: both payload layouts; ahead-of-time code refused; no payload refused"
