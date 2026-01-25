#!/bin/sh
# Test shebang patching via source-highlight comment formatter

. "$(dirname "$0")/test-helper.sh"

check_nix_available
check_patchnar_available

WORKDIR=$(create_workdir)
setup_workdir_cleanup "$WORKDIR"
cd "$WORKDIR"

mkdir -p pkg/bin

# Test 1: Shell script shebang (detected via shebang)
echo "Testing shell script shebang patching..."

cat > pkg/bin/shell_script << 'EOF'
#!/nix/store/abc123-bash-5.2/bin/bash
echo "hello world"
EOF
chmod +x pkg/bin/shell_script

create_test_nar pkg input.nar
run_patchnar < input.nar > output.nar

result=$(extract_from_nar output.nar /bin/shell_script)
assert_contains "$result" "#!/data/data/com.termux.nix/files/usr/nix/store/abc123-bash-5.2/bin/bash" \
    "bash shebang patched"


# Test 2: Perl script shebang (detected via shebang)
echo ""
echo "Testing perl script shebang patching..."

cat > pkg/bin/perl_script << 'EOF'
#!/nix/store/xyz789-perl-5.42/bin/perl
use strict;
print "hello\n";
EOF
chmod +x pkg/bin/perl_script

create_test_nar pkg input.nar
run_patchnar < input.nar > output.nar

result=$(extract_from_nar output.nar /bin/perl_script)
assert_contains "$result" "#!/data/data/com.termux.nix/files/usr/nix/store/xyz789-perl-5.42/bin/perl" \
    "perl shebang patched"


# Test 3: Python script shebang
echo ""
echo "Testing python script shebang patching..."

cat > pkg/bin/python_script << 'EOF'
#!/nix/store/def456-python-3.11/bin/python3
print("hello")
EOF
chmod +x pkg/bin/python_script

create_test_nar pkg input.nar
run_patchnar < input.nar > output.nar

result=$(extract_from_nar output.nar /bin/python_script)
assert_contains "$result" "#!/data/data/com.termux.nix/files/usr/nix/store/def456-python-3.11/bin/python3" \
    "python shebang patched"


# Test 4: Shebang with /usr/bin/env
echo ""
echo "Testing /usr/bin/env shebang (no patching needed)..."

cat > pkg/bin/env_script << 'EOF'
#!/usr/bin/env bash
echo "hello"
EOF
chmod +x pkg/bin/env_script

create_test_nar pkg input.nar
run_patchnar < input.nar > output.nar

result=$(extract_from_nar output.nar /bin/env_script)
assert_contains "$result" "#!/usr/bin/env bash" \
    "env shebang unchanged"
assert_not_contains "$result" "/data/data" \
    "no prefix added to /usr/bin/env"


# Test 5: Shebang with multiple paths (ld.so style) - fallback mode
echo ""
echo "Testing ld.so shebang (fallback shebang patching)..."

cat > pkg/bin/ldso_script << 'EOF'
#!/nix/store/glibc-2.40/lib/ld-linux-aarch64.so.1 /nix/store/bash-5.2/bin/bash
echo "complex shebang"
EOF
chmod +x pkg/bin/ldso_script

create_test_nar pkg input.nar
run_patchnar < input.nar > output.nar

result=$(extract_from_nar output.nar /bin/ldso_script)
# Both paths should be prefixed (fallback mode patches shebang line)
assert_contains "$result" "/data/data/com.termux.nix/files/usr/nix/store/glibc-2.40/lib/ld-linux-aarch64.so.1" \
    "first path in shebang patched"
assert_contains "$result" "/data/data/com.termux.nix/files/usr/nix/store/bash-5.2/bin/bash" \
    "second path in shebang patched"

print_summary
