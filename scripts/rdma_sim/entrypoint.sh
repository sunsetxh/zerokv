#!/bin/bash
set -e

echo "--- Starting RDMA Simulation Setup ---"

# 1. Load the Soft-RoCE driver
# Note: This requires the container to be run with --privileged
echo "Loading rdma_rxe kernel module..."
modprobe rdma_rxe || echo "Warning: modprobe failed. Kernel might already have it or not support it."

# 2. Get the primary network interface (usually eth0)
NET_DEV=$(ip route get 8.8.8.8 | grep -oP 'dev \K\S+' || echo "eth0")
echo "Using network interface: $NET_DEV"

# 3. Add a Soft-RoCE device linked to the network interface
echo "Creating RDMA device rxe0..."
rdma link add rxe0 type rxe netdev "$NET_DEV" || echo "Note: rxe0 might already exist."

# 4. Verify the device is present
echo "Verifying RDMA devices:"
ibv_devices

# 5. Build zerokv
echo "Building zerokv..."
mkdir -p build_docker && cd build_docker
cmake .. -DZEROKV_BUILD_TESTS=ON -DZEROKV_BUILD_PYTHON=OFF
make -j$(nproc)

# 6. Run RDMA validation tests
echo "--- Running RDMA Verification ---"
# Set UCX to use the simulated RDMA device
export UCX_NET_DEVICES=rxe0
# Force use of RC (Reliable Connection) transport which is typical for RDMA
export UCX_TLS=rc,shm

# Run specific RDMA test
./tests/integration/test_rdma || { echo "RDMA test failed!"; exit 1; }

echo "--- RDMA Simulation Verification Successful! ---"
