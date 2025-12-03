#!/bin/bash
# Download required Wayland protocols

set -e

PROTOCOL_DIR="protocol"
mkdir -p "$PROTOCOL_DIR"

# wlr-layer-shell-unstable-v1
if [ ! -f "$PROTOCOL_DIR/wlr-layer-shell-unstable-v1.xml" ]; then
    echo "Downloading wlr-layer-shell-unstable-v1.xml..."
    curl -sL -o "$PROTOCOL_DIR/wlr-layer-shell-unstable-v1.xml" \
        "https://gitlab.freedesktop.org/wlroots/wlr-protocols/-/raw/master/unstable/wlr-layer-shell-unstable-v1.xml"
fi

echo "Protocols downloaded successfully!"
echo ""
echo "You can now build with:"
echo "  meson setup build"
echo "  ninja -C build"
