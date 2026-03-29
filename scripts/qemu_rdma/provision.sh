#!/bin/bash
# provision.sh - Install deps, load Soft-RoCE, compile axon in both VMs
#
# Prerequisites: start_vms.sh has been run and both VMs have SSH ready
# This script handles:
#   - Switching to tuna mirror for faster downloads (China region)
#   - Installing all packages including linux-modules-extra (for rdma_rxe)
#   - Loading Soft-RoCE and creating rxe0 on both VMs
#   - Configuring static IPs for inter-VM communication
#   - Transferring and compiling axon
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"

VM1_SSH_PORT=2222
VM2_SSH_PORT=2223
SSH_USER="axon"
SSH_PASS="axon"
SSH_OPTS="-o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -o LogLevel=ERROR"

# --- SSH helper ---
vm_ssh() {
    local port=$1
    shift
    sshpass -p "${SSH_PASS}" ssh ${SSH_OPTS} -p "${port}" "${SSH_USER}@localhost" "$@"
}

# --- SCP helper ---
vm_scp() {
    local port=$1
    local src=$2
    local dst=$3
    sshpass -p "${SSH_PASS}" scp ${SSH_OPTS} -P "${port}" "${src}" "${SSH_USER}@localhost:${dst}"
}

# --- Detect the inter-VM network interface inside a VM ---
# The inter-VM interface is NOT the default route interface (user-mode NAT for SSH)
detect_inter_vm_iface() {
    local port=$1
    vm_ssh "${port}" bash -s <<'DETECT'
#!/bin/bash
DEFAULT_IFACE=$(ip route show default | awk '{print $5}' | head -1)
for iface in /sys/class/net/*; do
    name=$(basename "$iface")
    if [[ "$name" != "lo" && "$name" != "$DEFAULT_IFACE" ]]; then
        echo "$name"
        exit 0
    fi
done
echo "UNKNOWN"
DETECT
}

# --- Provision a single VM ---
provision_vm() {
    local port=$1
    local vm_name=$2

    echo "==> Provisioning ${vm_name} (SSH port ${port})..."

    # 1. Switch to tuna mirror for faster downloads
    echo "    Switching to tuna mirror..."
    vm_ssh "${port}" \
        "sudo sed -i 's|http://ports.ubuntu.com/ubuntu-ports|https://mirrors.tuna.tsinghua.edu.cn/ubuntu-ports|g' /etc/apt/sources.list" 2>/dev/null

    # 2. Install all dependencies
    # Note: linux-modules-extra is required for rdma_rxe kernel module
    echo "    Installing packages (this takes a while in QEMU)..."
    vm_ssh "${port}" bash -s <<'INSTALL'
#!/bin/bash
set -e
sudo apt-get update -qq
sudo apt-get install -y -qq \
    rdma-core ibverbs-utils \
    build-essential cmake git pkg-config \
    iproute2 kmod net-tools \
    sshpass pciutils ethtool \
    linux-modules-extra-$(uname -r) \
    autoconf automake libtool \
    libnuma-dev
INSTALL

    # 2b. Build UCX >= 1.14 from source (Ubuntu 22.04 ships UCX 1.12 which
    #     doesn't recognize Soft-RoCE IB transports properly)
    echo "    Building UCX from source (v1.18.0)..."
    vm_ssh "${port}" bash -s <<'BUILD_UCX'
#!/bin/bash
set -e
UCX_VER="1.18.0"
UCX_PREFIX="/usr/local"

# Check if already installed
if ucp_info -v 2>/dev/null | grep -q "${UCX_VER}"; then
    echo "    UCX ${UCX_VER} already installed, skipping build."
    exit 0
fi

cd /tmp
rm -rf ucx-${UCX_VER}
if [ ! -f ucx-${UCX_VER}.tar.gz ]; then
    wget -q https://github.com/openucx/ucx/releases/download/v${UCX_VER}/ucx-${UCX_VER}.tar.gz
fi
tar xzf ucx-${UCX_VER}.tar.gz
cd ucx-${UCX_VER}
./contrib/configure-release --prefix=${UCX_PREFIX} --with-rdmcm --enable-mt
make -j$(nproc) 2>&1 | tail -1
sudo make install
sudo ldconfig

echo "    UCX ${UCX_VER} installed to ${UCX_PREFIX}"
ucp_info -v 2>&1 | head -3
BUILD_UCX

    # 3. Load Soft-RoCE kernel module
    echo "    Loading Soft-RoCE (rdma_rxe)..."
    vm_ssh "${port}" bash -s <<'LOAD_RXE'
#!/bin/bash
set -e
sudo modprobe rdma_rxe
if ! lsmod | grep -q rdma_rxe; then
    echo "ERROR: rdma_rxe module failed to load"
    exit 1
fi
LOAD_RXE

    # 4. Detect inter-VM interface and create rxe0
    echo "    Detecting inter-VM network interface..."
    local inter_vm_iface
    inter_vm_iface=$(detect_inter_vm_iface "${port}")
    echo "    Inter-VM interface: ${inter_vm_iface}"

    echo "    Creating rxe0 on ${inter_vm_iface}..."
    vm_ssh "${port}" bash -s <<CREATE_RXE
#!/bin/bash
set -e
sudo rdma link delete rxe0 2>/dev/null || true
sudo rdma link add rxe0 type rxe netdev ${inter_vm_iface}
ibv_devices
CREATE_RXE

    # 5. Configure static IP on inter-VM interface
    if [[ "${vm_name}" == "VM1" ]]; then
        local ip="10.0.0.1"
    else
        local ip="10.0.0.2"
    fi
    echo "    Configuring IP ${ip} on ${inter_vm_iface}..."
    vm_ssh "${port}" "sudo ip addr add ${ip}/24 dev ${inter_vm_iface} 2>/dev/null || true; sudo ip link set ${inter_vm_iface} up"

    # 6. Transfer and compile axon
    echo "    Transferring axon source code..."
    vm_ssh "${port}" "rm -rf /tmp/axon && mkdir -p /tmp/axon"
    tar -C "${PROJECT_ROOT}" \
        --exclude='.git' \
        --exclude='build' \
        --exclude='.vm_work' \
        --exclude='*.o' \
        --exclude='*.a' \
        -czf /tmp/axon-src.tar.gz .
    vm_scp "${port}" /tmp/axon-src.tar.gz /tmp/axon-src.tar.gz
    rm -f /tmp/axon-src.tar.gz

    echo "    Compiling axon..."
    vm_ssh "${port}" bash -s <<'COMPILE'
#!/bin/bash
set -e
cd /tmp/axon
tar xzf /tmp/axon-src.tar.gz
cmake -B build \
    -DCMAKE_BUILD_TYPE=Release \
    -DAXON_BUILD_TESTS=OFF \
    -DAXON_BUILD_EXAMPLES=ON \
    -DAXON_BUILD_BENCHMARK=OFF \
    -DAXON_BUILD_PYTHON=OFF
cmake --build build -j$(nproc)
echo "Build complete!"
ls -la build/ping_pong build/send_recv build/rdma_put_get 2>/dev/null || true
COMPILE

    echo "    ${vm_name} provisioned successfully."
}

# --- Verify connectivity ---
verify_connectivity() {
    echo ""
    echo "==> Verifying inter-VM connectivity..."

    echo "    Ping VM1 (10.0.0.1) from VM2..."
    vm_ssh "${VM2_SSH_PORT}" "ping -c 3 -W 2 10.0.0.1" || {
        echo "    WARNING: Ping failed. VMs may not be able to communicate."
        echo "    Check that both VMs are running and use listen/connect networking."
    }
}

# --- Main ---
main() {
    if ! command -v sshpass &>/dev/null; then
        echo "ERROR: sshpass not found. Install with: brew install esolitos/ipa/sshpass"
        exit 1
    fi

    provision_vm "${VM1_SSH_PORT}" "VM1"
    provision_vm "${VM2_SSH_PORT}" "VM2"
    verify_connectivity

    echo ""
    echo "==> Provisioning complete!"
    echo "    VM1: rxe0 on 10.0.0.1 (SSH port ${VM1_SSH_PORT})"
    echo "    VM2: rxe0 on 10.0.0.2 (SSH port ${VM2_SSH_PORT})"
    echo ""
    echo "    Next step: ./run_test.sh"
}

main "$@"
