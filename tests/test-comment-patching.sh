#!/bin/sh
# Test comment patching via source-highlight comment formatter
# Verifies that paths in comments (not just strings) are patched

. "$(dirname "$0")/test-helper.sh"

check_nix_available
check_patchnar_available

WORKDIR=$(create_workdir)
setup_workdir_cleanup "$WORKDIR"
cd "$WORKDIR"

mkdir -p pkg/bin

# Test 1: Shell script comments
echo "Testing shell script comment patching..."

cat > pkg/bin/commented_shell << 'EOF'
#!/nix/store/abc123-bash-5.2/bin/bash
# This script uses /nix/store/xyz789-perl-5.42/bin/perl for processing
# Also references /nix/store/def456-python-3.11/bin/python
echo "actual code here"
EOF
chmod +x pkg/bin/commented_shell

create_test_nar pkg input.nar
run_patchnar < input.nar > output.nar

result=$(extract_from_nar output.nar /bin/commented_shell)
assert_contains "$result" "# This script uses /data/data/com.termux.nix/files/usr/nix/store/xyz789-perl-5.42/bin/perl" \
    "path in first comment patched"
assert_contains "$result" "# Also references /data/data/com.termux.nix/files/usr/nix/store/def456-python-3.11/bin/python" \
    "path in second comment patched"


# Test 2: Perl script comments
echo ""
echo "Testing perl script comment patching..."

cat > pkg/bin/commented_perl << 'EOF'
#!/nix/store/abc123-perl-5.42/bin/perl
# Depends on /nix/store/lib111-somelib/lib/libfoo.so
use strict;
# See /nix/store/doc222-docs/share/doc for documentation
print "hello\n";
EOF
chmod +x pkg/bin/commented_perl

create_test_nar pkg input.nar
run_patchnar < input.nar > output.nar

result=$(extract_from_nar output.nar /bin/commented_perl)
assert_contains "$result" "# Depends on /data/data/com.termux.nix/files/usr/nix/store/lib111-somelib/lib/libfoo.so" \
    "path in perl comment patched"
assert_contains "$result" "# See /data/data/com.termux.nix/files/usr/nix/store/doc222-docs/share/doc" \
    "second perl comment patched"


# Test 3: Python script comments
echo ""
echo "Testing python script comment patching..."

cat > pkg/bin/commented_python << 'EOF'
#!/nix/store/abc123-python-3.11/bin/python3
# Configuration at /nix/store/cfg333-config/etc/config.ini
def main():
    # Data files in /nix/store/data444-data/share/data
    print("hello")
EOF
chmod +x pkg/bin/commented_python

create_test_nar pkg input.nar
run_patchnar < input.nar > output.nar

result=$(extract_from_nar output.nar /bin/commented_python)
assert_contains "$result" "# Configuration at /data/data/com.termux.nix/files/usr/nix/store/cfg333-config/etc/config.ini" \
    "path in python comment patched"
assert_contains "$result" "# Data files in /data/data/com.termux.nix/files/usr/nix/store/data444-data/share/data" \
    "second python comment patched"


# Test 4: Comments without nix store paths should be unchanged
echo ""
echo "Testing comments without nix paths unchanged..."

cat > pkg/bin/normal_comments << 'EOF'
#!/nix/store/abc123-bash-5.2/bin/bash
# This is a normal comment
# /usr/bin/something is not a nix path
# /home/user/file is also not a nix path
echo "test"
EOF
chmod +x pkg/bin/normal_comments

create_test_nar pkg input.nar
run_patchnar < input.nar > output.nar

result=$(extract_from_nar output.nar /bin/normal_comments)
assert_contains "$result" "# This is a normal comment" \
    "normal comment unchanged"
assert_contains "$result" "# /usr/bin/something is not a nix path" \
    "/usr/bin path unchanged"
assert_contains "$result" "# /home/user/file is also not a nix path" \
    "/home path unchanged"

print_summary
