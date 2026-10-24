#!/usr/bin/env bash
set -euo pipefail
cd -- "$(dirname -- "${BASH_SOURCE[0]}")"
command -v cmake >/dev/null || { echo 'Run ./prep.sh first.' >&2; exit 1; }
echo "Removing previous build directory..."
rm -rf -- ./build
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
