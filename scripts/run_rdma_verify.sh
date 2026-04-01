#!/bin/bash
set -e

# Change directory to the zerokv project root
cd "$(dirname "$0")/.."

IMAGE_NAME="zerokv-rdma-sim"

echo "Building Docker image: $IMAGE_NAME..."
docker build -t "$IMAGE_NAME" -f scripts/rdma_sim/Dockerfile scripts/rdma_sim/

echo "Starting RDMA simulation container..."
echo "Note: Running with --privileged to enable RDMA kernel modules."

# Note: We mount the whole project but use a clean build dir inside docker
# To avoid permissions issues, we use the current user's UID/GID where possible,
# but for simple verification, mounting and running as root inside docker is fine.
docker run --privileged --network host --rm \
    -v "$(pwd):/zerokv" \
    -w /zerokv \
    "$IMAGE_NAME"
