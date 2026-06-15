#!/usr/bin/env bash
# Build AMY module for Move Anything (ARM64)
#
# Uses CMake to build the AMY core engine and plugin wrapper.
# Automatically uses Docker for cross-compilation if needed.
# Set CROSS_PREFIX to skip Docker (e.g., for native ARM builds).
set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(dirname "$SCRIPT_DIR")"
IMAGE_NAME="move-anything-amy-builder"

# Check if we need Docker
if [ -z "$CROSS_PREFIX" ] && [ ! -f "/.dockerenv" ]; then
    echo "=== AMY Module Build (via Docker) ==="
    echo ""

    # Build Docker image if needed
    if ! docker image inspect "$IMAGE_NAME" &>/dev/null; then
        echo "Building Docker image (first time only)..."
        docker build -t "$IMAGE_NAME" -f "$SCRIPT_DIR/Dockerfile" "$REPO_ROOT"
        echo ""
    fi

    # Run build inside container
    echo "Running build..."
    docker run --rm \
        -v "$REPO_ROOT:/build" \
        -u "$(id -u):$(id -g)" \
        -w /build \
        "$IMAGE_NAME" \
        ./scripts/build.sh

    echo ""
    echo "=== Done ==="
    exit 0
fi

# === Actual build (runs in Docker or with cross-compiler) ===
cd "$REPO_ROOT"

echo "=== Building AMY Module ==="

# Create build directory
mkdir -p build

# Run CMake configure with cross-compilation toolchain
echo "Configuring CMake..."
cmake -B build \
    -DCMAKE_TOOLCHAIN_FILE=cmake/aarch64-toolchain.cmake \
    -DCMAKE_BUILD_TYPE=Release \
    -G Ninja \
    2>&1

# Build
echo "Building (this may take a while)..."
cmake --build build --target amy-move-plugin -j$(nproc) 2>&1

# Package
echo "Packaging..."
mkdir -p dist/amy

# Copy files to dist
cat src/module.json > dist/amy/module.json
[ -f src/help.json ] && cat src/help.json > dist/amy/help.json
[ -f LICENSE ] && cat LICENSE > dist/amy/LICENSE
[ -f NOTICE ]  && cat NOTICE  > dist/amy/NOTICE
cat src/ui.js > dist/amy/ui.js
[ -f src/web_ui.html ] && cat src/web_ui.html > dist/amy/web_ui.html
[ -f src/dsp/amy/docs/amy.js ] && cat src/dsp/amy/docs/amy.js > dist/amy/amy.js
cat build/dsp.so > dist/amy/dsp.so
chmod +x dist/amy/dsp.so

# Create tarball for release
cd dist
tar -czvf amy-module.tar.gz amy/
cd ..

echo ""
echo "=== Build Complete ==="
echo "Output: dist/amy/"
echo "Tarball: dist/amy-module.tar.gz"
echo ""
echo "To install on Move:"
echo "  ./scripts/install.sh"
