#!/bin/bash
#
# Run clang-tidy against the current CoreS3 AirPlay core tree.

set -e

FIX_FLAG=""
if [ "$1" = "--fix" ]; then
  FIX_FLAG="--fix-errors"
fi

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"

cd "$PROJECT_DIR"

# Submodule paths to exclude
SUBMODULES="components/u8g2"
INACTIVE_COMPONENTS=""

export PATH="$(brew --prefix llvm)/bin:$PATH"

# Ensure clang-tidy is available (install via brew if needed)
if ! command -v clang-tidy &>/dev/null; then
  if command -v brew &>/dev/null; then
    echo "clang-tidy not found, installing llvm via brew..."
    brew install llvm
    export PATH="$(brew --prefix llvm)/bin:$PATH"
  else
    echo "Error: clang-tidy not found and brew is not available to install it"
    exit 1
  fi
fi

# Ensure compile_commands.json exists
COMPILE_DB="$PROJECT_DIR/compile_commands.json"
if [ ! -f "$COMPILE_DB" ] && [ -f "$PROJECT_DIR/.pio/build/m5cores3/compile_commands.json" ]; then
  COMPILE_DB="$PROJECT_DIR/.pio/build/m5cores3/compile_commands.json"
fi

if [ ! -f "$COMPILE_DB" ]; then
  echo "Error: compile_commands.json not found. Run '~/.platformio/penv/bin/pio run -e m5cores3' first."
  exit 1
fi

# Build exclusion args for find
EXCLUDE_ARGS=()
for sm in $SUBMODULES; do
  EXCLUDE_ARGS+=(-path "$sm" -prune -o)
done
for inactive in $INACTIVE_COMPONENTS; do
  EXCLUDE_ARGS+=(-path "$inactive" -prune -o)
done

# Find all C source files in main/ and the active product components.
SOURCES=$(find main components "${EXCLUDE_ARGS[@]}" \
  -path "*/managed_components" -prune -o \
  -path "*/.pio" -prune -o \
  \( -name "*.c" -o -name "*.h" \) -print)

echo "=== Running clang-tidy ==="
WARNINGS=0
for file in $SOURCES; do
  echo "Checking $file..."
  # Run clang-tidy, capture output, filter out ESP-IDF "file not found" errors
  # which are expected when running on the host outside the build environment
  OUTPUT=$(clang-tidy $FIX_FLAG -p "$(dirname "$COMPILE_DB")" "$file" 2>&1 || true)
  # Filter out lines about missing ESP-IDF/lwip/freertos headers
  FILTERED=$(echo "$OUTPUT" | grep -v "file not found \[clang-diagnostic" || true)
  # Check if any warnings remain (lines containing ": warning:")
  if echo "$FILTERED" | grep -q ": warning:"; then
    echo "$FILTERED" | grep -E "(: warning:|: note:)"
    WARNINGS=1
  fi
done

if [ "$WARNINGS" -ne 0 ]; then
  echo ""
  echo "clang-tidy found issues"
  exit 1
fi

echo "All files pass clang-tidy checks"
