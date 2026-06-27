#!/bin/bash
set -euxo pipefail

cd "$(dirname "${0}")"

POSTGRES_VERSION=${POSTGRES_VERSION:-11}

DOCKER="${DOCKER:-$(command -v docker || command -v podman)}"
if [[ -z "${DOCKER}" ]]; then
    echo "Error: no container runtime found (need docker or podman)" >&2
    exit 1
fi

mkdir -p dist
"${DOCKER}" build --build-arg=POSTGRES_VERSION="${POSTGRES_VERSION}" -t postgres-protobuf-build:"${POSTGRES_VERSION}" .
"${DOCKER}" run -i --rm --volume "$(pwd)/dist:/out" postgres-protobuf-build:"${POSTGRES_VERSION}" pg_config --version
"${DOCKER}" run -i --rm --volume "$(pwd)/dist:/out" postgres-protobuf-build:"${POSTGRES_VERSION}" /bin/bash -c 'cp -f /app/dist/*.tar.gz /out/'
