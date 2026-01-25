#!/bin/sh
# Common test helper functions for patchnar tests

# Exit on first error
set -e

# Colors for output (disabled if not a terminal)
if [ -t 1 ]; then
    RED='\033[0;31m'
    GREEN='\033[0;32m'
    YELLOW='\033[0;33m'
    NC='\033[0m' # No Color
else
    RED=''
    GREEN=''
    YELLOW=''
    NC=''
fi

# Test counter
TESTS_RUN=0
TESTS_PASSED=0
TESTS_FAILED=0

# Create unique work directory for this test
# Returns the path - caller should set up cleanup trap
create_workdir() {
    mktemp -d "${TMPDIR:-/tmp}/test-workdir-XXXXXX"
}

# Setup cleanup trap for workdir (call after create_workdir)
setup_workdir_cleanup() {
    trap "rm -rf '$1'" EXIT
}

# Log test result
log_pass() {
    TESTS_RUN=$((TESTS_RUN + 1))
    TESTS_PASSED=$((TESTS_PASSED + 1))
    printf "${GREEN}PASS${NC}: %s\n" "$1"
}

log_fail() {
    TESTS_RUN=$((TESTS_RUN + 1))
    TESTS_FAILED=$((TESTS_FAILED + 1))
    printf "${RED}FAIL${NC}: %s\n" "$1"
    if [ -n "$2" ]; then
        printf "  Expected: %s\n" "$2"
    fi
    if [ -n "$3" ]; then
        printf "  Got:      %s\n" "$3"
    fi
}

log_skip() {
    printf "${YELLOW}SKIP${NC}: %s\n" "$1"
}

# Print test summary
print_summary() {
    echo ""
    echo "================================"
    printf "Tests run: %d, Passed: %d, Failed: %d\n" \
        "$TESTS_RUN" "$TESTS_PASSED" "$TESTS_FAILED"
    echo "================================"

    if [ "$TESTS_FAILED" -gt 0 ]; then
        return 1
    fi
    return 0
}

# Assert string contains substring
assert_contains() {
    local haystack="$1"
    local needle="$2"
    local description="$3"

    if echo "$haystack" | grep -qF "$needle"; then
        log_pass "$description"
        return 0
    else
        log_fail "$description" "contains '$needle'" "'$haystack'"
        return 1
    fi
}

# Assert string does NOT contain substring
assert_not_contains() {
    local haystack="$1"
    local needle="$2"
    local description="$3"

    if echo "$haystack" | grep -qF "$needle"; then
        log_fail "$description" "not contains '$needle'" "'$haystack'"
        return 1
    else
        log_pass "$description"
        return 0
    fi
}

# Assert strings are equal
assert_equals() {
    local expected="$1"
    local actual="$2"
    local description="$3"

    if [ "$expected" = "$actual" ]; then
        log_pass "$description"
        return 0
    else
        log_fail "$description" "'$expected'" "'$actual'"
        return 1
    fi
}

# Create a test NAR from a directory
create_test_nar() {
    local dir="$1"
    local output="$2"
    nix nar pack "$dir" > "$output" 2>/dev/null
}

# Extract a file from NAR
extract_from_nar() {
    local nar="$1"
    local path="$2"
    nix nar cat "$nar" "$path" 2>/dev/null
}

# Run patchnar with arguments
run_patchnar() {
    "$PATCHNAR" "$@"
}

# Check if nix is available (required for NAR operations)
check_nix_available() {
    if ! command -v nix >/dev/null 2>&1; then
        log_skip "nix command not available"
        exit 77  # Autotools skip exit code
    fi
}

# Check if patchnar is available
check_patchnar_available() {
    if [ -z "$PATCHNAR" ] || [ ! -x "$PATCHNAR" ]; then
        # Get the directory containing this script
        SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"

        # Try to find patchnar in various locations
        for candidate in \
            "$SCRIPT_DIR/../result/bin/patchnar" \
            "$SCRIPT_DIR/../src/patchnar" \
            "../result/bin/patchnar" \
            "../src/patchnar" \
            "./src/patchnar"; do
            if [ -x "$candidate" ]; then
                PATCHNAR="$candidate"
                break
            fi
        done

        if [ -z "$PATCHNAR" ] || [ ! -x "$PATCHNAR" ]; then
            echo "ERROR: patchnar not found (PATCHNAR=$PATCHNAR)"
            echo "Set PATCHNAR environment variable to the patchnar binary path"
            exit 1
        fi
    fi
}
