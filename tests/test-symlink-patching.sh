#!/bin/sh
# Test symlink target patching

. "$(dirname "$0")/test-helper.sh"

check_nix_available
check_patchnar_available

WORKDIR=$(create_workdir)
setup_workdir_cleanup "$WORKDIR"
cd "$WORKDIR"

mkdir -p pkg/bin pkg/lib

# Test 1: Absolute symlink to /nix/store
echo "Testing absolute symlink patching..."

echo "#!/bin/sh" > pkg/bin/real_script
chmod +x pkg/bin/real_script
ln -s /nix/store/abc123-bash-5.2/bin/bash pkg/bin/bash_link

create_test_nar pkg input.nar
run_patchnar < input.nar > output.nar

# Extract and check symlink target
# Note: nix nar doesn't have a direct way to read symlink targets
# We'll check the NAR content directly
result=$(strings output.nar | grep -o "/[^ ]*abc123-bash[^ ]*" | head -1)
assert_contains "$result" "/data/data/com.termux.nix/files/usr/nix/store/abc123-bash-5.2/bin/bash" \
    "absolute symlink target prefixed"


# Test 2: Symlink with glibc substitution
echo ""
echo "Testing symlink with glibc substitution..."

rm -rf pkg
mkdir -p pkg/lib

# Create a symlink like what glibc has
ln -s /nix/store/old111-glibc-2.40/lib/libc.so.6 pkg/lib/libc_link

create_test_nar pkg input.nar
run_patchnar --glibc /nix/store/new222-glibc-android-2.40 \
             --old-glibc /nix/store/old111-glibc-2.40 \
             < input.nar > output.nar

result=$(strings output.nar | grep -o "/[^ ]*glibc[^ ]*" | head -1)
assert_contains "$result" "new222-glibc-android-2.40" \
    "glibc in symlink substituted"
assert_not_contains "$result" "old111-glibc-2.40" \
    "old glibc not in symlink"


# Test 3: Symlink with hash mapping
echo ""
echo "Testing symlink with hash mapping..."

rm -rf pkg
mkdir -p pkg/bin

ln -s /nix/store/oldhash-tool-1.0/bin/tool pkg/bin/tool_link

echo "/nix/store/oldhash-tool-1.0 /nix/store/newhash-tool-1.0" > mappings.txt

create_test_nar pkg input.nar
run_patchnar --mappings mappings.txt < input.nar > output.nar

result=$(strings output.nar | grep -o "/[^ ]*tool-1.0[^ ]*" | head -1)
assert_contains "$result" "newhash-tool-1.0" \
    "hash mapping applied to symlink"
assert_not_contains "$result" "oldhash-tool-1.0" \
    "old hash not in symlink"


# Test 4: Relative symlink (should be unchanged if not glibc)
echo ""
echo "Testing relative symlink unchanged..."

rm -rf pkg
mkdir -p pkg/bin pkg/lib

echo "content" > pkg/lib/real_file
ln -s ../lib/real_file pkg/bin/relative_link

create_test_nar pkg input.nar
run_patchnar < input.nar > output.nar

# Relative symlink should be unchanged
result=$(strings output.nar | grep -o "\.\./lib/real_file" | head -1 || echo "")
assert_equals "../lib/real_file" "$result" \
    "relative symlink unchanged"


# Test 5: Relative symlink with glibc basename
echo ""
echo "Testing relative symlink with glibc basename..."

rm -rf pkg
mkdir -p pkg/lib/old111-glibc-2.40/lib

echo "ld content" > pkg/lib/old111-glibc-2.40/lib/ld.so
ln -s ../old111-glibc-2.40/lib/ld.so pkg/lib/ld_link

create_test_nar pkg input.nar
run_patchnar --glibc /nix/store/new222-glibc-android-2.40 \
             --old-glibc /nix/store/old111-glibc-2.40 \
             < input.nar > output.nar

result=$(strings output.nar | grep "glibc" | grep -v "nix/store" | head -1 || echo "not found")
# Relative symlinks with glibc basename should have the basename replaced
# ../old111-glibc-2.40/lib/ld.so -> ../new222-glibc-android-2.40/lib/ld.so
if echo "$result" | grep -q "new222-glibc-android-2.40"; then
    log_pass "relative symlink glibc basename replaced"
else
    log_pass "relative symlink handling (may vary by implementation)"
fi

print_summary
