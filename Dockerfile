FROM ubuntu:22.04

RUN apt-get update \
 && DEBIAN_FRONTEND=noninteractive apt-get install -y \
       curl \
       gnupg2 \
       zlib1g-dev \
       build-essential \
       ruby \
       sudo \
       cmake \
       ninja-build \
       pkg-config \
 && echo "deb https://apt.postgresql.org/pub/repos/apt/ jammy-pgdg main" > /etc/apt/sources.list.d/pgdg.list \
 && curl -sL https://www.postgresql.org/media/keys/ACCC4CF8.asc | apt-key add -

# Compiling the protobuf library is quite slow, so do this before installing the correct Postgres version
COPY third_party/protobuf/ /app/third_party/protobuf
COPY build-protobuf-library.sh /app/
RUN /app/build-protobuf-library.sh

ARG POSTGRES_VERSION=11

RUN apt-get update \
 && DEBIAN_FRONTEND=noninteractive apt-get install -y \
        postgresql-${POSTGRES_VERSION} \
        postgresql-server-dev-${POSTGRES_VERSION} \
 && apt-get clean \
 && rm -Rf /var/lib/apt/lists/* \
 && /etc/init.d/postgresql restart \
 && sudo -u postgres createuser -s root

COPY /test_protos /app/test_protos
COPY Makefile *.hpp *.cpp *.rb *.sh *.sql *.control *.md *.txt /app/
RUN cd /app && make clean && make -j16 dist
