#!/bin/bash
# Install AMY module to Move
set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(dirname "$SCRIPT_DIR")"

cd "$REPO_ROOT"

if [ ! -d "dist/amy" ]; then
    echo "Error: dist/amy not found. Run ./scripts/build.sh first."
    exit 1
fi

echo "=== Installing AMY Module ==="

# Deploy to Move
echo "Copying module to Move..."
ssh ableton@move.local "mkdir -p /data/UserData/schwung/modules/sound_generators/amy"
scp -r dist/amy/* ableton@move.local:/data/UserData/schwung/modules/sound_generators/amy/

# Install chain presets if they exist
if [ -d "src/chain_patches" ]; then
    echo "Installing chain presets..."
    scp src/chain_patches/*.json ableton@move.local:/data/UserData/schwung/patches/
fi

# Set permissions
echo "Setting permissions..."
ssh ableton@move.local "chmod -R a+rw /data/UserData/schwung/modules/sound_generators/amy"

echo ""
echo "=== Install Complete ==="
echo "Module installed to: /data/UserData/schwung/modules/sound_generators/amy/"
echo ""
echo "Restart Move Anything to load the new module."
