#!/bin/sh
# Test transformStorePath functionality via ELF interpreter patching
# transformStorePath: glibc replacement -> hash mapping -> prefix

. "$(dirname "$0")/test-helper.sh"

check_nix_available
check_patchnar_available

WORKDIR=$(create_workdir)
setup_workdir_cleanup "$WORKDIR"
cd "$WORKDIR"

# Create a minimal test package with a script
mkdir -p pkg/bin

# Test 1: Basic prefix addition
echo "Testing basic store path prefix addition..."

cat > pkg/bin/test1 << 'EOF'
#!/nix/store/abc123-bash-5.2/bin/bash
echo "hello"
EOF
chmod +x pkg/bin/test1

create_test_nar pkg input.nar
run_patchnar < input.nar > output.nar

result=$(extract_from_nar output.nar /bin/test1)
assert_contains "$result" "/data/data/com.termux.nix/files/usr/nix/store/abc123-bash-5.2/bin/bash" \
    "prefix added to /nix/store path"


# Test 2: Glibc substitution + prefix
echo ""
echo "Testing glibc substitution with prefix..."

cat > pkg/bin/test2 << 'EOF'
#!/nix/store/old111-glibc-2.40/lib/ld-linux-aarch64.so.1 /nix/store/abc123-bash-5.2/bin/bash
echo "test"
EOF
chmod +x pkg/bin/test2

create_test_nar pkg input.nar
run_patchnar --glibc /nix/store/new222-glibc-android-2.40 \
             --old-glibc /nix/store/old111-glibc-2.40 \
             < input.nar > output.nar

result=$(extract_from_nar output.nar /bin/test2)
assert_contains "$result" "/data/data/com.termux.nix/files/usr/nix/store/new222-glibc-android-2.40/lib/ld-linux-aarch64.so.1" \
    "old glibc replaced with new glibc and prefixed"
assert_not_contains "$result" "old111-glibc" \
    "old glibc path not present"


# Test 3: Hash mapping + prefix
echo ""
echo "Testing hash mapping with prefix..."

cat > pkg/bin/test3 << 'EOF'
#!/nix/store/oldhash-perl-5.42/bin/perl
print "test";
EOF
chmod +x pkg/bin/test3

# Create mappings file
echo "/nix/store/oldhash-perl-5.42 /nix/store/newhash-perl-5.42" > mappings.txt

create_test_nar pkg input.nar
run_patchnar --mappings mappings.txt < input.nar > output.nar

result=$(extract_from_nar output.nar /bin/test3)
assert_contains "$result" "/data/data/com.termux.nix/files/usr/nix/store/newhash-perl-5.42/bin/perl" \
    "hash mapping applied and prefixed"
assert_not_contains "$result" "oldhash-perl" \
    "old hash not present"


# Test 4: Combined: glibc + hash mapping + prefix (order matters!)
echo ""
echo "Testing combined transformation order..."

cat > pkg/bin/test4 << 'EOF'
#!/nix/store/old111-glibc-2.40/lib/ld-linux.so /nix/store/oldhash-bash-5.2/bin/bash
echo "test"
EOF
chmod +x pkg/bin/test4

echo "/nix/store/oldhash-bash-5.2 /nix/store/newhash-bash-5.2" > mappings.txt

create_test_nar pkg input.nar
run_patchnar --glibc /nix/store/new222-glibc-android-2.40 \
             --old-glibc /nix/store/old111-glibc-2.40 \
             --mappings mappings.txt \
             < input.nar > output.nar

result=$(extract_from_nar output.nar /bin/test4)
assert_contains "$result" "new222-glibc-android-2.40" \
    "glibc substituted"
assert_contains "$result" "newhash-bash-5.2" \
    "hash mapping applied"
assert_contains "$result" "/data/data/com.termux.nix/files/usr/nix/store/" \
    "prefix added"

print_summary
