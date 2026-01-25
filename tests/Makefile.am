# Test suite for patchnar
# Uses autotools parallel-tests feature

# Path to built patchnar binary
PATCHNAR = $(top_builddir)/src/patchnar

# Export for test scripts
export PATCHNAR

# Shell-based integration tests
TESTS = \
	test-transform-store-path.sh \
	test-shebang-patching.sh \
	test-comment-patching.sh \
	test-string-patching.sh \
	test-glibc-substitution.sh \
	test-hash-mappings.sh \
	test-symlink-patching.sh \
	test-language-detection.sh

# Test scripts need to be executable
TEST_EXTENSIONS = .sh
SH_LOG_COMPILER = $(SHELL)

# Distribute test files
EXTRA_DIST = $(TESTS) test-helper.sh

# Clean up test artifacts
clean-local:
	rm -rf test-workdir-*
