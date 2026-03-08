# ZeroKV Dockerfile
FROM ubuntu:22.04

LABEL maintainer="zerokv team"
LABEL description="High-performance distributed KV store"

# Install build dependencies
RUN apt-get update && apt-get install -y \
    build-essential \
    cmake \
    git \
    libnuma-dev \
    python3-dev \
    python3-pip \
    automake \
    libtool \
    pkg-config \
    && rm -rf /var/lib/apt/lists/*

# Build UCX from source (version 1.19.0)
ENV UCX_VERSION=1.19.0
RUN cd /tmp && \
    git clone --depth 1 --branch ${UCX_VERSION} https://github.com/openucx/ucx.git ucx_src && \
    cd ucx_src && \
    ./autogen.sh && \
    ./configure --prefix=/usr/local --enable-optimizations --disable-debug && \
    make -j$(nproc) && \
    make install && \
    ldconfig && \
    rm -rf /tmp/ucx_src

# Build arguments
ARG BUILD_TYPE=Release
ARG BUILD_TESTS=ON

# Build
WORKDIR /build
COPY . .
RUN mkdir -p build && cd build && \
    cmake .. -DCMAKE_BUILD_TYPE=$BUILD_TYPE -DBUILD_TESTS=$BUILD_TESTS && \
    make -j$(nproc)

# Runtime
WORKDIR /app
COPY --from=build /build/zerokv_server /app/
COPY --from=build/build/libzerokv.a /app/

EXPOSE 5000

ENTRYPOINT ["./zerokv_server", "-p", "5000", "-m", "4096"]
