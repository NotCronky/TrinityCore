#!/usr/bin/env bash
# Full rebuild: wipes the build directory, reconfigures with CMake, compiles everything and installs.
set -euo pipefail

TC_CODE_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD_DIR="$TC_CODE_DIR/build"
INSTALL_DIR="$TC_CODE_DIR/server"
BUILD_CORES=$(( $(nproc) > 1 ? $(nproc) - 1 : 1 ))

echo ">> Removing old build directory: $BUILD_DIR"
rm -rf "$BUILD_DIR"
mkdir -p "$BUILD_DIR"
cd "$BUILD_DIR"

echo ">> Configuring"
cmake "$TC_CODE_DIR" -DCMAKE_INSTALL_PREFIX="$INSTALL_DIR" \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DCMAKE_C_COMPILER=/usr/bin/clang \
  -DCMAKE_CXX_COMPILER=/usr/bin/clang++ \
  -DWITH_WARNINGS=1 -DTOOLS=1 \
  -DSCRIPTS=static

echo ">> Compiling with $BUILD_CORES jobs"
make -j"$BUILD_CORES"

echo ">> Installing to $INSTALL_DIR"
make install

echo ">> Full rebuild complete. Binaries are in $INSTALL_DIR/bin"
