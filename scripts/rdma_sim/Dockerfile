FROM ubuntu:22.04

# Avoid interactive prompts during apt-get
ENV DEBIAN_FRONTEND=noninteractive

# Install core build tools and RDMA libraries
RUN apt-get update && apt-get install -y \
    build-essential \
    cmake \
    git \
    iproute2 \
    kmod \
    libibverbs-dev \
    ibverbs-providers \
    ibverbs-utils \
    rdmacm-utils \
    libibumad-dev \
    libucx-dev \
    ucx-utils \
    python3-dev \
    libgtest-dev \
    && rm -rf /var/lib/apt/lists/*

# Set up workdir
WORKDIR /zerokv

# The entrypoint script will be copied/mounted
COPY entrypoint.sh /entrypoint.sh
RUN chmod +x /entrypoint.sh

ENTRYPOINT ["/entrypoint.sh"]
