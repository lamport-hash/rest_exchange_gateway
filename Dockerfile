FROM ubuntu:24.04

ARG DEBIAN_FRONTEND=noninteractive

RUN apt-get update && apt-get install -y --no-install-recommends \
        build-essential \
        ca-certificates \
        ccache \
        clang-format \
        cmake \
        curl \
        git \
        libboost-dev \
        libboost-system-dev \
        libssl-dev \
        ninja-build \
        openssl \
        zlib1g-dev \
    && rm -rf /var/lib/apt/lists/*

ENV CCACHE_DIR=/ccache
RUN mkdir -p /ccache && CCACHE_DIR=/ccache ccache --set-config=max_size=5G

WORKDIR /workspace/rest_exchange_gateway

CMD ["sleep", "infinity"]
