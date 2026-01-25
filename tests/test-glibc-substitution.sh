#!/bin/sh
# Test glibc path substitution (--glibc and --old-glibc options)

. "$(dirname "$0")/test-helper.sh"

check_nix_available
check_patchnar_available

WORKDIR=$(create_workdir)
setup_workdir_cleanup "$WORKDIR"
cd "$WORKDIR"

mkdir -p pkg/bin

# Test 1: Glibc in shebang
echo "Testing glibc substitution in shebang..."

cat > pkg/bin/glibc_shebang << 'EOF'
#!/nix/store/old111-glibc-2.40/lib/ld-linux-aarch64.so.1 /nix/store/bash123-bash-5.2/bin/bash
echo "test"
EOF
chmod +x pkg/bin/glibc_shebang

create_test_nar pkg input.nar
run_patchnar --glibc /nix/store/new222-glibc-android-2.40 \
             --old-glibc /nix/store/old111-glibc-2.40 \
             < input.nar > output.nar

result=$(extract_from_nar output.nar /bin/glibc_shebang)
assert_contains "$result" "new222-glibc-android-2.40" \
    "old glibc replaced with new in shebang"
assert_not_contains "$result" "old111-glibc-2.40" \
    "old glibc not present in shebang"


# Test 2: Glibc in string literals
echo ""
echo "Testing glibc substitution in strings..."

cat > pkg/bin/glibc_strings << 'EOF'
#!/nix/store/bash123-bash-5.2/bin/bash
LIBC="/nix/store/old111-glibc-2.40/lib/libc.so.6"
LD="/nix/store/old111-glibc-2.40/lib/ld-linux-aarch64.so.1"
echo "Using glibc from /nix/store/old111-glibc-2.40"
EOF
chmod +x pkg/bin/glibc_strings

create_test_nar pkg input.nar
run_patchnar --glibc /nix/store/new222-glibc-android-2.40 \
             --old-glibc /nix/store/old111-glibc-2.40 \
             < input.nar > output.nar

result=$(extract_from_nar output.nar /bin/glibc_strings)
assert_contains "$result" 'LIBC="/data/data/com.termux.nix/files/usr/nix/store/new222-glibc-android-2.40/lib/libc.so.6"' \
    "glibc in LIBC variable substituted"
assert_contains "$result" 'LD="/data/data/com.termux.nix/files/usr/nix/store/new222-glibc-android-2.40/lib/ld-linux-aarch64.so.1"' \
    "glibc in LD variable substituted"
assert_not_contains "$result" "old111-glibc-2.40" \
    "old glibc not present anywhere"


# Test 3: Glibc in comments
echo ""
echo "Testing glibc substitution in comments..."

cat > pkg/bin/glibc_comments << 'EOF'
#!/nix/store/bash123-bash-5.2/bin/bash
# This script requires /nix/store/old111-glibc-2.40/lib/libc.so.6
# Built against /nix/store/old111-glibc-2.40
echo "hello"
EOF
chmod +x pkg/bin/glibc_comments

create_test_nar pkg input.nar
run_patchnar --glibc /nix/store/new222-glibc-android-2.40 \
             --old-glibc /nix/store/old111-glibc-2.40 \
             < input.nar > output.nar

result=$(extract_from_nar output.nar /bin/glibc_comments)
assert_contains "$result" "# This script requires /data/data/com.termux.nix/files/usr/nix/store/new222-glibc-android-2.40/lib/libc.so.6" \
    "glibc in first comment substituted"
assert_contains "$result" "# Built against /data/data/com.termux.nix/files/usr/nix/store/new222-glibc-android-2.40" \
    "glibc in second comment substituted"


# Test 4: Glibc substitution without --old-glibc (no substitution)
echo ""
echo "Testing no substitution when --old-glibc not provided..."

cat > pkg/bin/no_old_glibc << 'EOF'
#!/nix/store/some111-glibc-2.40/lib/ld-linux.so /nix/store/bash-5.2/bin/bash
echo "test"
EOF
chmod +x pkg/bin/no_old_glibc

create_test_nar pkg input.nar
run_patchnar --glibc /nix/store/new222-glibc-android-2.40 \
             < input.nar > output.nar

result=$(extract_from_nar output.nar /bin/no_old_glibc)
# Without --old-glibc, no substitution happens (only prefix added)
assert_contains "$result" "some111-glibc-2.40" \
    "original glibc preserved when --old-glibc not provided"


# Test 5: Mixed glibc and other paths
echo ""
echo "Testing mixed glibc and other paths..."

cat > pkg/bin/mixed_paths << 'EOF'
#!/nix/store/bash123-bash-5.2/bin/bash
GLIBC="/nix/store/old111-glibc-2.40/lib/libc.so.6"
OTHER="/nix/store/other999-package/bin/tool"
echo "$GLIBC $OTHER"
EOF
chmod +x pkg/bin/mixed_paths

create_test_nar pkg input.nar
run_patchnar --glibc /nix/store/new222-glibc-android-2.40 \
             --old-glibc /nix/store/old111-glibc-2.40 \
             < input.nar > output.nar

result=$(extract_from_nar output.nar /bin/mixed_paths)
assert_contains "$result" "new222-glibc-android-2.40" \
    "glibc path substituted"
assert_contains "$result" "other999-package" \
    "other path preserved"
assert_not_contains "$result" "old111-glibc" \
    "old glibc not present"

print_summary
