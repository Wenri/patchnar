#!/bin/sh
# Test language detection (extension-based and shebang-based)

. "$(dirname "$0")/test-helper.sh"

check_nix_available
check_patchnar_available

WORKDIR=$(create_workdir)
setup_workdir_cleanup "$WORKDIR"
cd "$WORKDIR"

mkdir -p pkg/bin pkg/share

# Test 1: Extension-based detection (.sh)
echo "Testing .sh extension detection..."

cat > pkg/share/script.sh << 'EOF'
#!/bin/bash
path="/nix/store/abc123-data/share/data"
echo "$path"
EOF

create_test_nar pkg input.nar
run_patchnar < input.nar > output.nar

result=$(extract_from_nar output.nar /share/script.sh)
assert_contains "$result" "/data/data/com.termux.nix/files/usr/nix/store/abc123-data/share/data" \
    ".sh extension: string patched"


# Test 2: Extension-based detection (.pm - Perl module)
# Note: .pl extension maps to prolog.lang, not perl.lang
# Use .pm (Perl module) for reliable Perl detection
echo ""
echo "Testing .pm extension detection..."

cat > pkg/share/script.pm << 'EOF'
#!/usr/bin/perl
my $path = "/nix/store/abc123-data/share/data";
print "$path\n";
1;
EOF

create_test_nar pkg input.nar
run_patchnar < input.nar > output.nar

result=$(extract_from_nar output.nar /share/script.pm)
assert_contains "$result" "/data/data/com.termux.nix/files/usr/nix/store/abc123-data/share/data" \
    ".pm extension: string patched"


# Test 3: Extension-based detection (.py)
echo ""
echo "Testing .py extension detection..."

cat > pkg/share/script.py << 'EOF'
#!/usr/bin/python3
path = "/nix/store/abc123-data/share/data"
print(path)
EOF

create_test_nar pkg input.nar
run_patchnar < input.nar > output.nar

result=$(extract_from_nar output.nar /share/script.py)
assert_contains "$result" "/data/data/com.termux.nix/files/usr/nix/store/abc123-data/share/data" \
    ".py extension: string patched"


# Test 4: Shebang-based detection (no extension, bash)
echo ""
echo "Testing shebang detection for bash..."

cat > pkg/bin/bash_no_ext << 'EOF'
#!/nix/store/abc123-bash-5.2/bin/bash
path="/nix/store/data111-data/share/data"
echo "$path"
EOF
chmod +x pkg/bin/bash_no_ext

create_test_nar pkg input.nar
run_patchnar < input.nar > output.nar

result=$(extract_from_nar output.nar /bin/bash_no_ext)
assert_contains "$result" "/data/data/com.termux.nix/files/usr/nix/store/data111-data/share/data" \
    "shebang detection: bash string patched"


# Test 5: Shebang-based detection (no extension, perl)
echo ""
echo "Testing shebang detection for perl..."

cat > pkg/bin/perl_no_ext << 'EOF'
#!/nix/store/abc123-perl-5.42/bin/perl
my $path = "/nix/store/data222-data/share/data";
print "$path\n";
EOF
chmod +x pkg/bin/perl_no_ext

create_test_nar pkg input.nar
run_patchnar < input.nar > output.nar

result=$(extract_from_nar output.nar /bin/perl_no_ext)
assert_contains "$result" "/data/data/com.termux.nix/files/usr/nix/store/data222-data/share/data" \
    "shebang detection: perl string patched"


# Test 6: Nix store path normalization in shebang detection
echo ""
echo "Testing Nix path normalization for language detection..."

# This tests the fix for detectLanguage() that normalizes Nix paths
# before inference (e.g., #!/nix/store/xxx-perl-5.42/bin/perl -> #!/bin/perl)
cat > pkg/bin/nix_shebang << 'EOF'
#!/nix/store/0gfsfmgbyy35akc4waha0cq3gf6xan1r-perl-5.42.0/bin/perl
my $data = "/nix/store/data333-data/share/data";
print "$data\n";
EOF
chmod +x pkg/bin/nix_shebang

create_test_nar pkg input.nar
run_patchnar < input.nar > output.nar

result=$(extract_from_nar output.nar /bin/nix_shebang)
# The string should be patched, proving language was detected correctly
assert_contains "$result" "/data/data/com.termux.nix/files/usr/nix/store/data333-data/share/data" \
    "Nix path normalized: perl detected and string patched"


# Test 7: Skipped extension (.html)
echo ""
echo "Testing skipped extension (.html)..."

cat > pkg/share/page.html << 'EOF'
<html>
<body>Path: /nix/store/abc123-data/share/data</body>
</html>
EOF

create_test_nar pkg input.nar
run_patchnar < input.nar > output.nar

result=$(extract_from_nar output.nar /share/page.html)
# HTML should NOT be patched (in SKIP_EXTENSIONS)
assert_not_contains "$result" "/data/data/com.termux.nix/files/usr" \
    ".html skipped: no patching"
assert_contains "$result" "/nix/store/abc123-data" \
    ".html skipped: original path preserved"


# Test 8: Skipped extension (.png placeholder)
echo ""
echo "Testing skipped extension (.png)..."

# Create a fake PNG (just some bytes)
printf '\x89PNG\r\n\x1a\n' > pkg/share/image.png

create_test_nar pkg input.nar
run_patchnar < input.nar > output.nar

# PNG should pass through unchanged
# Just verify no crash and file exists
if extract_from_nar output.nar /share/image.png >/dev/null 2>&1; then
    log_pass ".png skipped: file passed through"
else
    log_fail ".png skipped: file should pass through"
fi


# Test 9: Fallback for unknown shebang
echo ""
echo "Testing fallback for unknown shebang..."

cat > pkg/bin/unknown_shebang << 'EOF'
#!/nix/store/xxx-unknown-1.0/lib/ld.so /nix/store/yyy-bash-5.2/bin/bash
echo "Path in body: /nix/store/zzz-data/share/data"
EOF
chmod +x pkg/bin/unknown_shebang

create_test_nar pkg input.nar
run_patchnar < input.nar > output.nar

result=$(extract_from_nar output.nar /bin/unknown_shebang)
# Shebang should be patched (fallback mode)
assert_contains "$result" "#!/data/data/com.termux.nix/files/usr/nix/store/xxx-unknown-1.0" \
    "fallback: shebang patched"
# Body string NOT patched (fallback only does shebang)
assert_contains "$result" 'echo "Path in body: /nix/store/zzz-data/share/data"' \
    "fallback: body string not patched (expected)"

print_summary
