#!/bin/bash
set -euxo pipefail

cd "$(dirname "${0}")"

if [ -n "${USE_DOCKER:-}" ]; then
    docker build -f Dockerfile.test -t postgres-protobuf-test .
    docker run postgres-protobuf-test env __IN_DOCKER=1 /app/debug-build-and-test.sh
    exit $?
fi

if [ -n "${__IN_DOCKER:-}" ]; then
    sudo /etc/init.d/postgresql restart
fi

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
