#!/bin/bash
set -euxo pipefail

if [ -z "${NO_CLEAN:-}" ]; then
    make clean
fi
make -j16
sudo make install
if make installcheck; then
    echo "Tests passed"
else
    diff -u expected/postgres_protobuf.out results/postgres_protobuf.out
    exit 1
fi
