#!/bin/bash
set -euxo pipefail

PROTOBUF_VERSION=3.11.2

cd "$(dirname "${0}")"

rm -Rf third_party/protobuf-*
mkdir -p third_party/
cd third_party
curl -L "https://github.com/protocolbuffers/protobuf/releases/download/v${PROTOBUF_VERSION}/protobuf-cpp-${PROTOBUF_VERSION}.tar.gz" | tar xzf -
mv protobuf-"${PROTOBUF_VERSION}" protobuf
cd protobuf
./configure --with-pic --enable-static
make -j16
