#!/usr/bin/env bash
# Incremental build: only recompiles files that changed since the last build, then installs.
# CMake is re-run first so newly added source files and script folders are picked up (this is quick).
# Falls back to a full rebuild if there is no existing build yet.
set -euo pipefail

TC_CODE_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD_DIR="$TC_CODE_DIR/build"
INSTALL_DIR="$TC_CODE_DIR/server"
BUILD_CORES=$(( $(nproc) > 1 ? $(nproc) - 1 : 1 ))

if [[ ! -f "$BUILD_DIR/CMakeCache.txt" ]]; then
  echo ">> No existing build found, running a full rebuild instead"
  exec "$TC_CODE_DIR/rebuild.sh"
fi

cd "$BUILD_DIR"

echo ">> Refreshing CMake configuration"
cmake "$TC_CODE_DIR"

echo ">> Compiling changed files with $BUILD_CORES jobs"
make -j"$BUILD_CORES"

echo ">> Installing to $INSTALL_DIR"
make install

echo ">> Incremental build complete. Binaries are in $INSTALL_DIR/bin"
