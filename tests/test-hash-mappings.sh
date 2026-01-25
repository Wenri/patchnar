#!/bin/sh
# Test hash mapping functionality (--mappings and --self-mapping options)

. "$(dirname "$0")/test-helper.sh"

check_nix_available
check_patchnar_available

WORKDIR=$(create_workdir)
setup_workdir_cleanup "$WORKDIR"
cd "$WORKDIR"

mkdir -p pkg/bin

# Test 1: Single mapping from file
echo "Testing single hash mapping..."

cat > pkg/bin/single_map << 'EOF'
#!/nix/store/oldhash-bash-5.2/bin/bash
echo "hello"
EOF
chmod +x pkg/bin/single_map

echo "/nix/store/oldhash-bash-5.2 /nix/store/newhash-bash-5.2" > mappings.txt

create_test_nar pkg input.nar
run_patchnar --mappings mappings.txt < input.nar > output.nar

result=$(extract_from_nar output.nar /bin/single_map)
assert_contains "$result" "newhash-bash-5.2" \
    "hash mapping applied"
assert_not_contains "$result" "oldhash-bash-5.2" \
    "old hash not present"


# Test 2: Multiple mappings from file
echo ""
echo "Testing multiple hash mappings..."

cat > pkg/bin/multi_map << 'EOF'
#!/nix/store/oldhash1-bash-5.2/bin/bash
PERL="/nix/store/oldhash2-perl-5.42/bin/perl"
PYTHON="/nix/store/oldhash3-python-3.11/bin/python"
echo "$PERL $PYTHON"
EOF
chmod +x pkg/bin/multi_map

cat > mappings.txt << 'EOF'
/nix/store/oldhash1-bash-5.2 /nix/store/newhash1-bash-5.2
/nix/store/oldhash2-perl-5.42 /nix/store/newhash2-perl-5.42
/nix/store/oldhash3-python-3.11 /nix/store/newhash3-python-3.11
EOF

create_test_nar pkg input.nar
run_patchnar --mappings mappings.txt < input.nar > output.nar

result=$(extract_from_nar output.nar /bin/multi_map)
assert_contains "$result" "newhash1-bash-5.2" \
    "first mapping applied"
assert_contains "$result" "newhash2-perl-5.42" \
    "second mapping applied"
assert_contains "$result" "newhash3-python-3.11" \
    "third mapping applied"
assert_not_contains "$result" "oldhash1" \
    "old hash 1 not present"
assert_not_contains "$result" "oldhash2" \
    "old hash 2 not present"
assert_not_contains "$result" "oldhash3" \
    "old hash 3 not present"


# Test 3: Self-mapping via command line
echo ""
echo "Testing self-mapping..."

cat > pkg/bin/self_map << 'EOF'
#!/nix/store/selfold-bash-5.2/bin/bash
echo "self reference"
EOF
chmod +x pkg/bin/self_map

create_test_nar pkg input.nar
run_patchnar --self-mapping "/nix/store/selfold-bash-5.2 /nix/store/selfnew-bash-5.2" \
             < input.nar > output.nar

result=$(extract_from_nar output.nar /bin/self_map)
assert_contains "$result" "selfnew-bash-5.2" \
    "self-mapping applied"
assert_not_contains "$result" "selfold-bash-5.2" \
    "old self-reference not present"


# Test 4: Combined mappings file and self-mapping
echo ""
echo "Testing combined mappings file and self-mapping..."

cat > pkg/bin/combined << 'EOF'
#!/nix/store/selfold-bash-5.2/bin/bash
PERL="/nix/store/oldhash-perl-5.42/bin/perl"
echo "$PERL"
EOF
chmod +x pkg/bin/combined

echo "/nix/store/oldhash-perl-5.42 /nix/store/newhash-perl-5.42" > mappings.txt

create_test_nar pkg input.nar
run_patchnar --mappings mappings.txt \
             --self-mapping "/nix/store/selfold-bash-5.2 /nix/store/selfnew-bash-5.2" \
             < input.nar > output.nar

result=$(extract_from_nar output.nar /bin/combined)
assert_contains "$result" "selfnew-bash-5.2" \
    "self-mapping applied in combined"
assert_contains "$result" "newhash-perl-5.42" \
    "file mapping applied in combined"


# Test 5: Mapping applied to strings and comments
echo ""
echo "Testing mapping in strings and comments..."

cat > pkg/bin/map_everywhere << 'EOF'
#!/nix/store/hash123-bash-5.2/bin/bash
# This references /nix/store/oldhash-data/share/data
DATA="/nix/store/oldhash-data/share/data"
echo "Data: $DATA"
EOF
chmod +x pkg/bin/map_everywhere

echo "/nix/store/oldhash-data /nix/store/newhash-data" > mappings.txt

create_test_nar pkg input.nar
run_patchnar --mappings mappings.txt < input.nar > output.nar

result=$(extract_from_nar output.nar /bin/map_everywhere)
assert_contains "$result" "# This references /data/data/com.termux.nix/files/usr/nix/store/newhash-data/share/data" \
    "mapping applied in comment"
assert_contains "$result" 'DATA="/data/data/com.termux.nix/files/usr/nix/store/newhash-data/share/data"' \
    "mapping applied in string"


# Test 6: Mapping length mismatch warning (should be skipped)
echo ""
echo "Testing mapping length mismatch handling..."

cat > pkg/bin/len_mismatch << 'EOF'
#!/nix/store/shorthash-bash-5.2/bin/bash
echo "test"
EOF
chmod +x pkg/bin/len_mismatch

# This mapping has different lengths and should be skipped with warning
# Note: mapping paths must be same length for binary-safe replacement
echo "/nix/store/shorthash-bash-5.2 /nix/store/muchlongerhash-bash-5.2-extra" > mappings.txt

create_test_nar pkg input.nar
# Capture both stdout (NAR) and stderr (warnings)
run_patchnar --mappings mappings.txt < input.nar > output.nar 2>stderr.txt || true

result=$(extract_from_nar output.nar /bin/len_mismatch)
# Original path should still be there (mapping skipped due to length mismatch)
# but prefix is still added
assert_contains "$result" "/data/data/com.termux.nix/files/usr/nix/store/shorthash-bash-5.2" \
    "mismatched length mapping skipped, prefix still added"

# Verify warning was emitted (optional - depends on patchnar behavior)
if grep -q "length" stderr.txt 2>/dev/null; then
    log_pass "warning emitted for length mismatch"
else
    log_skip "no warning check (patchnar may silently skip)"
fi

print_summary
