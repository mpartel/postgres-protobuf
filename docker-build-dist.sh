#!/bin/bash
set -euxo pipefail

cd "$(dirname "${0}")"

POSTGRES_VERSION=${POSTGRES_VERSION:-11}

mkdir -p dist
docker build --build-arg=POSTGRES_VERSION="${POSTGRES_VERSION}" -t postgres-protobuf-build:"${POSTGRES_VERSION}" .
docker run -i --rm --volume "$(pwd)/dist:/out" postgres-protobuf-build:"${POSTGRES_VERSION}" pg_config --version
docker run -i --rm --volume "$(pwd)/dist:/out" postgres-protobuf-build:"${POSTGRES_VERSION}" /bin/bash -c 'cp -f /app/dist/*.tar.gz /out/'
