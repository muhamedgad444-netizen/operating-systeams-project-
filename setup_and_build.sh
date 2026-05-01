#!/bin/bash
# setup_and_build.sh
# Run this script on Ubuntu/Debian Linux or WSL Ubuntu to install
# all required tools and build + launch xv6 with enhanced journaling.
#
# Usage:
#   chmod +x setup_and_build.sh
#   ./setup_and_build.sh

set -e

echo "=============================================="
echo "  XV6 Enhanced Journaling - Build Script"
echo "=============================================="

# 1. Install dependencies
echo ""
echo "[1/3] Installing build dependencies..."
sudo apt-get update -qq
sudo apt-get install -y -q \
    build-essential \
    gcc \
    gcc-multilib \
    make \
    qemu-system-x86 \
    git

echo "      Done."

# 2. Build xv6
echo ""
echo "[2/3] Building xv6..."
make clean
make

echo "      Build successful!"

# 3. Launch
echo ""
echo "[3/3] Launching xv6 in QEMU (no graphical window)..."
echo ""
echo "  Once you see the xv6 shell prompt '$', type:"
echo ""
echo "      journaltest"
echo ""
echo "  Press Ctrl-A then X to exit QEMU."
echo ""
echo "=============================================="
echo ""

make qemu-nox
