#!/bin/bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "$0")/../.." && pwd)"
COMMIT_ID="$(git -C "${ROOT_DIR}" rev-parse --short HEAD)"
ARCH="aarch64"
OUT_DIR="${ROOT_DIR}/out/packages"
WORK_DIR="/Volumes/data/axon-focal-build"
BASE_IMAGE="${WORK_DIR}/ubuntu-20.04-server-cloudimg-arm64.img"
OVERLAY="${WORK_DIR}/build-overlay.qcow2"
SEED_DIR="${WORK_DIR}/seed"
SEED_ISO="${WORK_DIR}/seed.iso"
VM_LOG="${WORK_DIR}/vm-build.log"
VM_PID_FILE="${WORK_DIR}/vm.pid"
VM_SSH_PORT=2224
EFI_FIRMWARE="/opt/homebrew/share/qemu/edk2-aarch64-code.fd"
IMAGE_URL="https://cloud-images.ubuntu.com/releases/focal/release/ubuntu-20.04-server-cloudimg-arm64.img"
SSH_OPTS=(-o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -o LogLevel=ERROR)
SRC_TARBALL="/tmp/alps_kv_wrap_src.tar.gz"
REMOTE_BUILD_SCRIPT="/tmp/axon_focal_remote_build.sh"
REMOTE_UCX_TARBALL="/tmp/ucx-v1.20.0.tar.gz"
REMOTE_SRC_TARBALL="/tmp/alps_kv_wrap_src.tar.gz"
REMOTE_CMAKE_TARBALL="/tmp/cmake-4.3.1-linux-aarch64.tar.gz"
PKG_DIR_NAME="alps_kv_wrap_pkg-${ARCH}-${COMMIT_ID}"
OUTPUT_TARBALL="${OUT_DIR}/${PKG_DIR_NAME}.tar.gz"
LATEST_OUTPUT_TARBALL="${OUT_DIR}/alps_kv_wrap_pkg-${ARCH}.tar.gz"
LOCAL_INSPECT_DIR="/tmp/${PKG_DIR_NAME}-inspect"
CMAKE_ARCHIVE="${WORK_DIR}/cmake-4.3.1-linux-aarch64.tar.gz"
CMAKE_URL="https://github.com/Kitware/CMake/releases/download/v4.3.1/cmake-4.3.1-linux-aarch64.tar.gz"

vm_ssh() {
    sshpass -p axon ssh -p "${VM_SSH_PORT}" "${SSH_OPTS[@]}" axon@localhost "$@"
}

vm_copy_to() {
    local src=$1
    local dst=$2
    cat "${src}" | sshpass -p axon ssh -p "${VM_SSH_PORT}" "${SSH_OPTS[@]}" axon@localhost "cat > '${dst}'"
}

vm_copy_from() {
    local src=$1
    local dst=$2
    sshpass -p axon ssh -p "${VM_SSH_PORT}" "${SSH_OPTS[@]}" axon@localhost "cat '${src}'" > "${dst}"
}

cleanup_vm() {
    if [[ -f "${VM_PID_FILE}" ]]; then
        local pid
        pid=$(cat "${VM_PID_FILE}" 2>/dev/null || true)
        if [[ -n "${pid}" ]] && kill -0 "${pid}" 2>/dev/null; then
            kill "${pid}" 2>/dev/null || true
            sleep 2
            kill -9 "${pid}" 2>/dev/null || true
        fi
        rm -f "${VM_PID_FILE}"
    fi
}

trap cleanup_vm EXIT

prepare_base_image() {
    mkdir -p "${OUT_DIR}"
    mkdir -p "${WORK_DIR}"
    if [[ ! -f "${BASE_IMAGE}" ]]; then
        echo "==> Downloading Ubuntu 20.04 ARM64 cloud image..."
        curl -fL "${IMAGE_URL}" -o "${BASE_IMAGE}.tmp"
        mv "${BASE_IMAGE}.tmp" "${BASE_IMAGE}"
    fi
    if [[ ! -f "${CMAKE_ARCHIVE}" ]]; then
        echo "==> Downloading CMake 4.3.1 ARM64..."
        curl -fL "${CMAKE_URL}" -o "${CMAKE_ARCHIVE}.tmp"
        mv "${CMAKE_ARCHIVE}.tmp" "${CMAKE_ARCHIVE}"
    fi
    qemu-img info "${BASE_IMAGE}" | sed -n '1,12p'
}

prepare_overlay_and_seed() {
    rm -f "${OVERLAY}"
    qemu-img create -f qcow2 -b "${BASE_IMAGE}" -F qcow2 "${OVERLAY}" 20G >/dev/null

    rm -f "${SEED_ISO}"
    rm -rf "${SEED_DIR}"
    mkdir -p "${SEED_DIR}"
    cat > "${SEED_DIR}/meta-data" <<'EOF'
instance-id: axon-focal-build
local-hostname: axon-focal-build
EOF
    cat > "${SEED_DIR}/network-config" <<'EOF'
version: 2
ethernets:
  id0:
    match:
      name: "enp0s1"
    dhcp4: true
EOF
    cat > "${SEED_DIR}/user-data" <<'EOF'
#cloud-config
hostname: axon-focal-build
manage_etc_hosts: true
users:
  - name: axon
    plain_text_passwd: axon
    lock_passwd: false
    shell: /bin/bash
    sudo: ALL=(ALL) NOPASSWD:ALL
ssh_pwauth: true
runcmd:
  - echo "cloud-init complete for axon-focal-build"
EOF
    hdiutil makehybrid -o "${SEED_ISO}" "${SEED_DIR}" \
        -joliet -iso -default-volume-name cidata >/dev/null 2>&1
    rm -rf "${SEED_DIR}"
}

start_vm() {
    rm -f "${VM_LOG}" "${VM_PID_FILE}"
    qemu-system-aarch64 \
        -machine virt,highmem=off \
        -accel hvf \
        -cpu cortex-a57 \
        -m 2G \
        -smp 2 \
        -drive if=pflash,format=raw,readonly=on,file="${EFI_FIRMWARE}" \
        -drive if=virtio,format=qcow2,file="${OVERLAY}" \
        -drive if=virtio,format=raw,file="${SEED_ISO}" \
        -netdev user,id=net1,hostfwd=tcp::${VM_SSH_PORT}-:22 \
        -device virtio-net-pci,netdev=net1 \
        -nographic \
        -serial mon:stdio \
        > "${VM_LOG}" 2>&1 &
    echo "$!" > "${VM_PID_FILE}"
    echo "==> VM PID: $(cat "${VM_PID_FILE}")"
}

wait_for_ssh() {
    for _ in {1..120}; do
        if vm_ssh "echo ready" >/dev/null 2>&1; then
            echo "==> SSH ready on ${VM_SSH_PORT}"
            sleep 5
            vm_ssh "echo post-ready-check" >/dev/null 2>&1
            return 0
        fi
        sleep 2
    done
    echo "ERROR: SSH not ready on port ${VM_SSH_PORT}" >&2
    tail -n 80 "${VM_LOG}" >&2 || true
    return 1
}

write_remote_build_script() {
    cat > /tmp/axon_focal_remote_build.sh <<'EOF'
#!/bin/bash
set -euo pipefail

export DEBIAN_FRONTEND=noninteractive
: "${COMMIT_ID:=unknown}"
ARCH="aarch64"
PKG_DIR_NAME="alps_kv_wrap_pkg-${ARCH}-${COMMIT_ID}"
sudo apt-get update -o Acquire::Retries=5 -o Acquire::https::Timeout=30 -qq
sudo apt-get install -y \
    gcc-10 g++-10 cmake git pkg-config make \
    build-essential rdma-core ibverbs-utils \
    iproute2 kmod net-tools sshpass pciutils ethtool \
    autoconf automake libtool libnuma-dev curl ca-certificates \
    -o Acquire::Retries=5 -o Acquire::https::Timeout=30 --fix-missing
sudo update-alternatives --install /usr/bin/gcc gcc /usr/bin/gcc-10 100 || true
sudo update-alternatives --install /usr/bin/g++ g++ /usr/bin/g++-10 100 || true
sudo update-alternatives --set gcc /usr/bin/gcc-10
sudo update-alternatives --set g++ /usr/bin/g++-10

export CC=gcc-10
export CXX=g++-10
UCX_VER=1.20.0
UCX_PREFIX="/usr/local/ucx-static-pic"
UCX_INFO_STATIC_BUILD="/tmp/ucx-tools-static"
UCX_INFO_STATIC_BIN="${UCX_INFO_STATIC_BUILD}/src/tools/info/ucx_info"
CMAKE_DIR=/opt/cmake-4.3.1-linux-aarch64

cd /tmp
sudo rm -rf "${CMAKE_DIR}"
sudo tar -C /opt -xzf /tmp/cmake-4.3.1-linux-aarch64.tar.gz
export PATH="${CMAKE_DIR}/bin:${PATH}"

if [[ ! -x "${UCX_PREFIX}/bin/ucp_info" ]] || ! "${UCX_PREFIX}/bin/ucp_info" -v 2>/dev/null | grep -q "${UCX_VER}"; then
    rm -rf "ucx-${UCX_VER}"
    tar xzf /tmp/ucx-v1.20.0.tar.gz
    cd "ucx-${UCX_VER}"
    if [[ ! -x ./configure ]]; then
        ./autogen.sh >/tmp/ucx-autogen.log 2>&1
    fi
    export CFLAGS="${CFLAGS:-} -fPIC"
    export CXXFLAGS="${CXXFLAGS:-} -fPIC"
    ./contrib/configure-release --prefix="${UCX_PREFIX}" --with-rdmcm --enable-mt --with-go=no --with-java=no --enable-gtest=no \
        >/tmp/ucx-config.log 2>&1
    make -j"$(nproc)" >/tmp/ucx-make.log 2>&1
    sudo make install >/tmp/ucx-install.log 2>&1
    sudo ldconfig
fi

rm -rf "${UCX_INFO_STATIC_BUILD}"
mkdir -p "${UCX_INFO_STATIC_BUILD}"
tar xzf /tmp/ucx-v1.20.0.tar.gz -C "${UCX_INFO_STATIC_BUILD}" --strip-components=1
cd "${UCX_INFO_STATIC_BUILD}"
if [[ ! -x ./configure ]]; then
    ./autogen.sh >/tmp/ucx-static-tools-autogen.log 2>&1
fi
./contrib/configure-release \
    --prefix="${UCX_INFO_STATIC_BUILD}/install" \
    --disable-shared \
    --enable-static \
    --with-rdmcm \
    --enable-mt \
    --with-go=no \
    --with-java=no \
    --enable-gtest=no \
    >/tmp/ucx-static-tools-config.log 2>&1
make -j"$(nproc)" -k >/tmp/ucx-static-tools-make.log 2>&1 || true
if [[ ! -x "${UCX_INFO_STATIC_BIN}" ]]; then
    echo "Failed to build static ucx_info" >&2
    tail -n 80 /tmp/ucx-static-tools-make.log >&2 || true
    exit 1
fi

rm -rf /tmp/axon-build "/tmp/${PKG_DIR_NAME}" "/tmp/${PKG_DIR_NAME}.tar.gz"
mkdir -p /tmp/axon-build
cd /tmp/axon-build
tar xzf /tmp/alps_kv_wrap_src.tar.gz
cmake -S . -B build \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_CXX_COMPILER=g++-10 \
    -DUCX_ROOT="${UCX_PREFIX}" \
    -DZEROKV_LINK_UCX_STATIC=ON \
    -DZEROKV_BUILD_TESTS=OFF \
    -DZEROKV_BUILD_EXAMPLES=ON \
    -DZEROKV_BUILD_BENCHMARK=OFF \
    -DZEROKV_BUILD_PYTHON=OFF \
    -DCMAKE_SHARED_LINKER_FLAGS='-static-libstdc++ -static-libgcc' \
    -DCMAKE_EXE_LINKER_FLAGS='-static-libstdc++ -static-libgcc'
cmake --build build --target zerokv alps_kv_wrap alps_kv_bench -j"$(nproc)"
cmake --install build --prefix "/tmp/${PKG_DIR_NAME}"

cd /tmp
cp "/tmp/${PKG_DIR_NAME}/share/doc/alps_kv_wrap/README.md" "/tmp/${PKG_DIR_NAME}/README.md"
printf '%s\n' "${COMMIT_ID}" > "/tmp/${PKG_DIR_NAME}/COMMIT_ID"
printf '%s\n' "${ARCH}" > "/tmp/${PKG_DIR_NAME}/ARCH"
cp "${UCX_INFO_STATIC_BIN}" "/tmp/${PKG_DIR_NAME}/bin/ucx_info"
if [[ -x "${UCX_PREFIX}/bin/ucp_info" ]]; then
    cp "${UCX_PREFIX}/bin/ucp_info" "/tmp/${PKG_DIR_NAME}/bin/ucp_info"
fi
tar -czf "/tmp/${PKG_DIR_NAME}.tar.gz" "${PKG_DIR_NAME}"

echo "== remote compiler string =="
strings "/tmp/${PKG_DIR_NAME}/lib/libalps_kv_wrap.so" | \
    grep 'GCC: (Ubuntu 10.5.0-1ubuntu1~20.04) 10.5.0' | head -n 1

echo "== remote GLIBCXX max =="
for f in \
    "/tmp/${PKG_DIR_NAME}/lib/libalps_kv_wrap.so" \
    "/tmp/${PKG_DIR_NAME}/lib/libzerokv.so" \
    "/tmp/${PKG_DIR_NAME}/bin/alps_kv_bench"; do
    echo "-- ${f}"
    objdump -T "${f}" | sed -n 's/.*\(GLIBCXX_[0-9.]*\).*/\1/p' | sort -Vu | tail -n 10
done
EOF
    chmod +x /tmp/axon_focal_remote_build.sh
}

package_source_tree() {
    rm -f "${SRC_TARBALL}"
    git -C "${ROOT_DIR}" archive --format=tar.gz -o "${SRC_TARBALL}" HEAD
}

inspect_local_package() {
    rm -rf "${LOCAL_INSPECT_DIR}"
    mkdir -p "${LOCAL_INSPECT_DIR}"
    tar -xzf "${OUTPUT_TARBALL}" -C "${LOCAL_INSPECT_DIR}"
    for f in \
        "${LOCAL_INSPECT_DIR}/${PKG_DIR_NAME}/lib/libalps_kv_wrap.so" \
        "${LOCAL_INSPECT_DIR}/${PKG_DIR_NAME}/lib/libzerokv.so" \
        "${LOCAL_INSPECT_DIR}/${PKG_DIR_NAME}/bin/alps_kv_bench"; do
        echo "== local ${f} =="
        objdump -T "${f}" | sed -n 's/.*\(GLIBCXX_[0-9.]*\).*/\1/p' | sort -Vu | tail -n 10
        strings "${f}" | rg 'GCC: \(Ubuntu .*\\) 10\\.' | tail -n 1 || true
    done
    echo "== packaged commit id =="
    cat "${LOCAL_INSPECT_DIR}/${PKG_DIR_NAME}/COMMIT_ID"
    echo "== packaged arch =="
    cat "${LOCAL_INSPECT_DIR}/${PKG_DIR_NAME}/ARCH"
    echo "== packaged readme =="
    ls -lh "${LOCAL_INSPECT_DIR}/${PKG_DIR_NAME}/README.md"
    echo "== packaged ucx tools =="
    ls -lh "${LOCAL_INSPECT_DIR}/${PKG_DIR_NAME}/bin/ucx_info"
    if [[ -e "${LOCAL_INSPECT_DIR}/${PKG_DIR_NAME}/bin/ucp_info" ]]; then
        ls -lh "${LOCAL_INSPECT_DIR}/${PKG_DIR_NAME}/bin/ucp_info"
    fi
    ls -lh "${OUTPUT_TARBALL}"
}

prepare_base_image
prepare_overlay_and_seed
start_vm
wait_for_ssh

write_remote_build_script
package_source_tree
rm -f "${OUTPUT_TARBALL}"
rm -f "${LATEST_OUTPUT_TARBALL}"

vm_copy_to "${SRC_TARBALL}" "${REMOTE_SRC_TARBALL}"
vm_copy_to "${ROOT_DIR}/ucx-v1.20.0.tar.gz" "${REMOTE_UCX_TARBALL}"
vm_copy_to "${CMAKE_ARCHIVE}" "${REMOTE_CMAKE_TARBALL}"
vm_copy_to /tmp/axon_focal_remote_build.sh "${REMOTE_BUILD_SCRIPT}"
vm_ssh "COMMIT_ID='${COMMIT_ID}' bash ${REMOTE_BUILD_SCRIPT}"
vm_copy_from "/tmp/${PKG_DIR_NAME}.tar.gz" "${OUTPUT_TARBALL}"
cp "${OUTPUT_TARBALL}" "${LATEST_OUTPUT_TARBALL}"

inspect_local_package
