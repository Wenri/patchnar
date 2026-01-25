#!/bin/sh
# Test string literal patching via source-highlight string formatter

. "$(dirname "$0")/test-helper.sh"

check_nix_available
check_patchnar_available

WORKDIR=$(create_workdir)
setup_workdir_cleanup "$WORKDIR"
cd "$WORKDIR"

mkdir -p pkg/bin

# Test 1: Shell script double-quoted strings
echo "Testing shell script string patching..."

cat > pkg/bin/string_shell << 'EOF'
#!/nix/store/abc123-bash-5.2/bin/bash
path="/nix/store/xyz789-data/share/data"
echo "Config at /nix/store/cfg111-config/etc/app.conf"
EOF
chmod +x pkg/bin/string_shell

create_test_nar pkg input.nar
run_patchnar < input.nar > output.nar

result=$(extract_from_nar output.nar /bin/string_shell)
assert_contains "$result" 'path="/data/data/com.termux.nix/files/usr/nix/store/xyz789-data/share/data"' \
    "path in variable assignment patched"
assert_contains "$result" 'echo "Config at /data/data/com.termux.nix/files/usr/nix/store/cfg111-config/etc/app.conf"' \
    "path in echo string patched"


# Test 2: Shell script single-quoted strings
echo ""
echo "Testing shell single-quoted string patching..."

cat > pkg/bin/single_quote_shell << 'EOF'
#!/nix/store/abc123-bash-5.2/bin/bash
path='/nix/store/xyz789-data/share/data'
echo 'File at /nix/store/file111-file/bin/app'
EOF
chmod +x pkg/bin/single_quote_shell

create_test_nar pkg input.nar
run_patchnar < input.nar > output.nar

result=$(extract_from_nar output.nar /bin/single_quote_shell)
assert_contains "$result" "path='/data/data/com.termux.nix/files/usr/nix/store/xyz789-data/share/data'" \
    "single-quoted variable patched"
assert_contains "$result" "echo 'File at /data/data/com.termux.nix/files/usr/nix/store/file111-file/bin/app'" \
    "single-quoted echo patched"


# Test 3: Perl string literals
echo ""
echo "Testing perl string patching..."

cat > pkg/bin/string_perl << 'EOF'
#!/nix/store/abc123-perl-5.42/bin/perl
my $config = "/nix/store/cfg222-config/etc/perl.conf";
my $data = '/nix/store/data333-data/share/perl';
print "Loading from $config\n";
EOF
chmod +x pkg/bin/string_perl

create_test_nar pkg input.nar
run_patchnar < input.nar > output.nar

result=$(extract_from_nar output.nar /bin/string_perl)
assert_contains "$result" 'my $config = "/data/data/com.termux.nix/files/usr/nix/store/cfg222-config/etc/perl.conf"' \
    "perl double-quoted string patched"
assert_contains "$result" "my \$data = '/data/data/com.termux.nix/files/usr/nix/store/data333-data/share/perl'" \
    "perl single-quoted string patched"


# Test 4: Python string literals
echo ""
echo "Testing python string patching..."

cat > pkg/bin/string_python << 'EOF'
#!/nix/store/abc123-python-3.11/bin/python3
config_path = "/nix/store/cfg444-config/etc/python.conf"
data_path = '/nix/store/data555-data/share/python'
print(f"Config: {config_path}")
EOF
chmod +x pkg/bin/string_python

create_test_nar pkg input.nar
run_patchnar < input.nar > output.nar

result=$(extract_from_nar output.nar /bin/string_python)
assert_contains "$result" 'config_path = "/data/data/com.termux.nix/files/usr/nix/store/cfg444-config/etc/python.conf"' \
    "python double-quoted string patched"
assert_contains "$result" "data_path = '/data/data/com.termux.nix/files/usr/nix/store/data555-data/share/python'" \
    "python single-quoted string patched"


# Test 5: Strings without nix paths unchanged
echo ""
echo "Testing non-nix strings unchanged..."

cat > pkg/bin/normal_strings << 'EOF'
#!/nix/store/abc123-bash-5.2/bin/bash
msg="Hello, World!"
path="/usr/local/bin/app"
home="/home/user/data"
echo "$msg"
EOF
chmod +x pkg/bin/normal_strings

create_test_nar pkg input.nar
run_patchnar < input.nar > output.nar

result=$(extract_from_nar output.nar /bin/normal_strings)
assert_contains "$result" 'msg="Hello, World!"' \
    "normal string unchanged"
assert_contains "$result" 'path="/usr/local/bin/app"' \
    "/usr/local path unchanged"
assert_contains "$result" 'home="/home/user/data"' \
    "/home path unchanged"

print_summary
