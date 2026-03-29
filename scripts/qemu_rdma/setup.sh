#!/bin/bash
# setup.sh - Download Ubuntu cloud image, create VM overlays, generate cloud-init seeds
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
WORK_DIR="${SCRIPT_DIR}/.vm_work"
IMAGE_URL="https://cloud-images.ubuntu.com/releases/22.04/release/ubuntu-22.04-server-cloudimg-arm64.img"
BASE_IMAGE="${WORK_DIR}/ubuntu-22.04-server-cloudimg-arm64.qcow2"
EFI_FIRMWARE="/opt/homebrew/share/qemu/edk2-aarch64-code.fd"

VM1_DISK="${WORK_DIR}/vm1-overlay.qcow2"
VM2_DISK="${WORK_DIR}/vm2-overlay.qcow2"
VM1_SEED="${WORK_DIR}/vm1-seed.iso"
VM2_SEED="${WORK_DIR}/vm2-seed.iso"

mkdir -p "${WORK_DIR}"

# --- Check prerequisites ---
check_prereqs() {
    echo "==> Checking prerequisites..."
    local missing=()

    if ! command -v qemu-system-aarch64 &>/dev/null; then
        missing+=("qemu-system-aarch64 (brew install qemu)")
    fi
    if ! command -v qemu-img &>/dev/null; then
        missing+=("qemu-img (brew install qemu)")
    fi
    if [[ ! -f "${EFI_FIRMWARE}" ]]; then
        missing+=("EFI firmware: ${EFI_FIRMWARE} (comes with brew install qemu)")
    fi
    if ! command -v hdiutil &>/dev/null; then
        missing+=("hdiutil (macOS built-in)")
    fi

    if [[ ${#missing[@]} -gt 0 ]]; then
        echo "Missing prerequisites:"
        printf "  - %s\n" "${missing[@]}"
        exit 1
    fi
    echo "    All prerequisites met."
}

# --- Download base image ---
download_image() {
    if [[ -f "${BASE_IMAGE}" ]]; then
        echo "==> Base image already exists: ${BASE_IMAGE}"
        return
    fi

    echo "==> Downloading Ubuntu 22.04 Server cloudimg ARM64..."
    echo "    URL: ${IMAGE_URL}"
    echo "    This may take a while (~500MB)..."

    local tmp_file="${BASE_IMAGE}.download"
    curl -fSL -o "${tmp_file}" "${IMAGE_URL}"
    mv "${tmp_file}" "${BASE_IMAGE}"
    echo "    Download complete."
}

# --- Create overlay disks ---
create_overlays() {
    echo "==> Creating overlay disks..."

    for vm_disk in 1:"${VM1_DISK}" 2:"${VM2_DISK}"; do
        local vm="${vm_disk%%:*}"
        local disk="${vm_disk#*:}"
        if [[ -f "${disk}" ]]; then
            echo "    VM${vm} overlay already exists: ${disk}"
            continue
        fi
        qemu-img create -f qcow2 -b "${BASE_IMAGE}" -F qcow2 "${disk}" 20G
        echo "    Created VM${vm} overlay: ${disk}"
    done
}

# --- Generate cloud-init seed ISO ---
# Creates a NoCloud seed ISO with user-data and meta-data
generate_seed() {
    local vm_num=$1
    local hostname="axon-vm${vm_num}"
    local seed_iso="${WORK_DIR}/vm${vm_num}-seed.iso"
    local seed_dir="${WORK_DIR}/vm${vm_num}-seed"

    echo "==> Generating cloud-init seed for VM${vm_num}..."

    rm -rf "${seed_dir}"
    mkdir -p "${seed_dir}"

    # meta-data
    cat > "${seed_dir}/meta-data" <<EOF
instance-id: ${hostname}
local-hostname: ${hostname}
EOF

    # network-config - configure both NICs with DHCP
    # enp0s1 = socket mcast (net0), enp0s2 = user-mode NAT (net1, for SSH/internet)
    cat > "${seed_dir}/network-config" <<EOF
version: 2
ethernets:
  id0:
    match:
      name: "enp0s1"
    dhcp4: true
    dhcp4-overrides:
      use-routes: false
  id1:
    match:
      name: "enp0s2"
    dhcp4: true
EOF

    # user-data (cloud-config)
    # Note: packages are installed by provision.sh instead of cloud-init
    # because DNS may not be available during first boot
    cat > "${seed_dir}/user-data" <<EOF
#cloud-config
hostname: ${hostname}
manage_etc_hosts: true

users:
  - name: axon
    plain_text_passwd: axon
    lock_passwd: false
    shell: /bin/bash
    sudo: ALL=(ALL) NOPASSWD:ALL

ssh_pwauth: true

runcmd:
  - echo "cloud-init complete for ${hostname}"
EOF

    # Create ISO using hdiutil (macOS native)
    # Cloud-init NoCloud datasource looks for a filesystem labeled "cidata"
    hdiutil makehybrid -o "${seed_iso}" "${seed_dir}" \
        -joliet -iso \
        -default-volume-name cidata \
        2>/dev/null

    rm -rf "${seed_dir}"
    echo "    Seed ISO created: ${seed_iso}"
}

# --- Main ---
main() {
    check_prereqs
    download_image
    create_overlays
    generate_seed 1
    generate_seed 2

    echo ""
    echo "==> Setup complete! Files in ${WORK_DIR}/:"
    ls -lh "${WORK_DIR}/"
    echo ""
    echo "    Next step: ./start_vms.sh"
}

main "$@"
