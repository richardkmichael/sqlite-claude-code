#!/bin/bash
# Simple test runner for Claude Code extension

set -e  # Exit on error

TEST_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(dirname "$TEST_DIR")"
# Allow override via CLAUDE_CODE_EXT for ASan builds
EXTENSION="${CLAUDE_CODE_EXT:-$PROJECT_DIR/build/claude_code.dylib}"

# Colors for output
GREEN='\033[0;32m'
RED='\033[0;31m'
NC='\033[0m' # No Color

# Export DYLD_INSERT_LIBRARIES if set (for ASan)
if [ -n "$DYLD_INSERT_LIBRARIES" ]; then
    export DYLD_INSERT_LIBRARIES
fi

echo "Running tests for Claude Code extension..."
echo

# Check extension exists
if [ ! -f "$EXTENSION" ]; then
    echo -e "${RED}ERROR: Extension not found at $EXTENSION${NC}"
    echo "Run 'make' first to build the extension"
    exit 1
fi

# Run each test file
TESTS_PASSED=0
TESTS_FAILED=0

# Save original HOME
ORIGINAL_HOME="$HOME"

for test_file in "$TEST_DIR"/*.test; do
    if [ ! -f "$test_file" ]; then
        continue
    fi

    test_name=$(basename "$test_file" .test)
    echo "Running test: $test_name"

    # Restore HOME for each test
    export HOME="$ORIGINAL_HOME"

    # Set environment variable for env_config test
    if [ "$test_name" = "env_config" ]; then
        export CLAUDE_PROJECTS_DIR="./test-projects"
    elif [ "$test_name" = "missing_default" ] || [ "$test_name" = "readonly" ] || [ "$test_name" = "errors" ]; then
        # These tests expect failures
        unset CLAUDE_PROJECTS_DIR
        if [ "$test_name" = "missing_default" ]; then
            export HOME="/tmp/nonexistent_home_$$"
        fi
        # These tests should fail, so invert the result
        if sed "s|./build/claude_code.dylib|$EXTENSION|g" "$test_file" | sqlite3 :memory: 2>&1; then
            echo -e "${RED}  FAIL (expected error but succeeded)${NC}"
            ((TESTS_FAILED++))
        else
            echo -e "${GREEN}  PASS (correctly failed)${NC}"
            ((TESTS_PASSED++))
        fi
        continue
    else
        unset CLAUDE_PROJECTS_DIR
    fi

    if sed "s|./build/claude_code.dylib|$EXTENSION|g" "$test_file" | sqlite3 :memory: > /dev/null 2>&1; then
        echo -e "${GREEN}  PASS${NC}"
        ((TESTS_PASSED++))
    else
        echo -e "${RED}  FAIL${NC}"
        ((TESTS_FAILED++))
    fi
done

# Restore original HOME
export HOME="$ORIGINAL_HOME"

# Clean up
unset CLAUDE_PROJECTS_DIR

echo
echo "================="
echo "Tests passed: $TESTS_PASSED"
echo "Tests failed: $TESTS_FAILED"
echo "================="

if [ $TESTS_FAILED -gt 0 ]; then
    exit 1
fi

exit 0
