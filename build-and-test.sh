#!/bin/bash
set -euxo pipefail

cd "$(dirname "${0}")"

POSTGRES_VERSION=${POSTGRES_VERSION:-11}

if [[ -n "${USE_DOCKER:-}" ]]; then
    unset USE_DOCKER
    docker build -f Dockerfile.test --build-arg=POSTGRES_VERSION="${POSTGRES_VERSION}" -t postgres-protobuf-test:"${POSTGRES_VERSION}" .
    docker run postgres-protobuf-test:"${POSTGRES_VERSION}" env __IN_DOCKER=1 /app/build-and-test.sh
    exit $?
fi

if [[ -n "${__IN_DOCKER:-}" ]]; then
    sudo /etc/init.d/postgresql restart
fi

if [[ -z "${NO_CLEAN:-}" ]]; then
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
