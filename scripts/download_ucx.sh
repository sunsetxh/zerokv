#!/bin/bash
# UCX Source Download Script
# Downloads UCX 1.19.0 source code to thirdparty/ucx/

UCX_VERSION="1.19.0"
UCX_DIR="thirdparty/ucx"

echo "Downloading UCX ${UCX_VERSION}..."

if [ -d "$UCX_DIR" ]; then
    echo "UCX source already exists at $UCX_DIR"
    read -p "Re-download? (y/n): " -n 1 -r
    echo
    if [[ ! $REPLY =~ ^[Yy]$ ]]; then
        echo "Using existing UCX source"
        exit 0
    fi
    rm -rf "$UCX_DIR"
fi

mkdir -p thirdparty
cd thirdparty

git clone --depth 1 --branch ${UCX_VERSION} https://github.com/openucx/ucx.git ucx

echo "UCX ${UCX_VERSION} downloaded to ${UCX_DIR}"
echo ""
echo "To build UCX:"
echo "  cd ${UCX_DIR}"
echo "  ./autogen.sh"
echo "  ./configure --prefix=/usr/local"
echo "  make -j\$(nproc)"
echo "  sudo make install"
echo "  sudo ldconfig"
