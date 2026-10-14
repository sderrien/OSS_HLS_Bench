#!/bin/bash
set -e

export CC=gcc
export CXX=g++
export PATH=$(which gcc | xargs dirname):$PATH

# Resolve the source tree from this script rather than from the developer's
# checkout path. This keeps the legacy build usable after cloning or renaming
# the repository directory.
script_dir=$(CDPATH= cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
cd "$script_dir"

echo "=== Step 1: Build CLooG/ISL ==="
mkdir -p cloog/install
cd cloog
./autogen.sh
cd isl
./autogen.sh
cd ..
./configure --prefix=$(pwd)/cloog/install
make
make install
cd ..

echo "=== CLooG/ISL build complete ==="
