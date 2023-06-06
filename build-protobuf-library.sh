#!/bin/bash
set -euxo pipefail

cd "$(dirname "${0}")"

cd third_party/protobuf
rm -Rf _build
cmake -B _build -S . -G Ninja \
    -DCMAKE_CXX_FLAGS=-fPIC \
    -DCMAKE_BUILD_TYPE=Release \
    -Dprotobuf_BUILD_TESTS=OFF \
    -DCMAKE_INSTALL_PREFIX=_install
cmake --build _build
cd _build
ninja install