#!/bin/bash
# start_vms.sh - Launch two QEMU VMs with socket multicast for RDMA simulation
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
WORK_DIR="${SCRIPT_DIR}/.vm_work"
EFI_FIRMWARE="/opt/homebrew/share/qemu/edk2-aarch64-code.fd"

VM1_DISK="${WORK_DIR}/vm1-overlay.qcow2"
VM2_DISK="${WORK_DIR}/vm2-overlay.qcow2"
VM1_SEED="${WORK_DIR}/vm1-seed.iso"
VM2_SEED="${WORK_DIR}/vm2-seed.iso"

VM1_SSH_PORT=2222
VM2_SSH_PORT=2223
VM1_MAC="52:54:00:12:34:56"
VM2_MAC="52:54:00:12:34:57"
INTERVM_SOCKET_PORT=12345

VM1_LOG="${WORK_DIR}/vm1.log"
VM2_LOG="${WORK_DIR}/vm2.log"
VM1_PID_FILE="${WORK_DIR}/vm1.pid"
VM2_PID_FILE="${WORK_DIR}/vm2.pid"

# --- Check files exist ---
check_files() {
    local missing=()
    for f in "${VM1_DISK}" "${VM2_DISK}" "${VM1_SEED}" "${VM2_SEED}" "${EFI_FIRMWARE}"; do
        if [[ ! -f "${f}" ]]; then
            missing+=("${f}")
        fi
    done
    if [[ ${#missing[@]} -gt 0 ]]; then
        echo "ERROR: Missing files. Run setup.sh first."
        printf "  - %s\n" "${missing[@]}"
        exit 1
    fi
}

# --- Check if VMs are already running ---
check_running() {
    if [[ -f "${VM1_PID_FILE}" ]] && kill -0 "$(cat "${VM1_PID_FILE}")" 2>/dev/null; then
        echo "ERROR: VM1 is already running (PID $(cat "${VM1_PID_FILE}"))"
        echo "       Run stop_vms.sh first or delete ${VM1_PID_FILE}"
        exit 1
    fi
    if [[ -f "${VM2_PID_FILE}" ]] && kill -0 "$(cat "${VM2_PID_FILE}")" 2>/dev/null; then
        echo "ERROR: VM2 is already running (PID $(cat "${VM2_PID_FILE}"))"
        echo "       Run stop_vms.sh first or delete ${VM2_PID_FILE}"
        exit 1
    fi
}

# --- Start a single VM ---
start_vm() {
    local vm_num=$1
    local disk=$2
    local seed=$3
    local ssh_port=$4
    local mac=$5
    local logfile=$6
    local pidfile=$7

    echo "==> Starting VM${vm_num} (SSH: localhost:${ssh_port})..."

    # Build inter-VM network backend: VM1 listens, VM2 connects
    local netdev_arg
    if [[ "${vm_num}" == "1" ]]; then
        netdev_arg="socket,id=net0,listen=127.0.0.1:${INTERVM_SOCKET_PORT}"
    else
        netdev_arg="socket,id=net0,connect=127.0.0.1:${INTERVM_SOCKET_PORT}"
    fi

    qemu-system-aarch64 \
        -machine virt,highmem=off \
        -accel hvf \
        -cpu cortex-a57 \
        -m 2G \
        -smp 2 \
        -drive if=pflash,format=raw,readonly=on,file="${EFI_FIRMWARE}" \
        -drive if=virtio,format=qcow2,file="${disk}" \
        -drive if=virtio,format=raw,file="${seed}" \
        -netdev "${netdev_arg}" \
        -device virtio-net-pci,netdev=net0,mac="${mac}" \
        -netdev user,id=net1,hostfwd=tcp::${ssh_port}-:22 \
        -device virtio-net-pci,netdev=net1 \
        -nographic \
        -serial mon:stdio \
        > "${logfile}" 2>&1 &

    local pid=$!
    echo "${pid}" > "${pidfile}"
    echo "    VM${vm_num} started (PID: ${pid}, log: ${logfile})"
}

# --- Wait for SSH connectivity ---
wait_for_ssh() {
    local ssh_port=$1
    local vm_name=$2
    local max_attempts=60
    local attempt=1

    echo "==> Waiting for ${vm_name} SSH on port ${ssh_port}..."

    while [[ ${attempt} -le ${max_attempts} ]]; do
        if sshpass -p "axon" ssh \
            -o StrictHostKeyChecking=no \
            -o UserKnownHostsFile=/dev/null \
            -o ConnectTimeout=2 \
            -p "${ssh_port}" \
            axon@localhost \
            "echo ok" &>/dev/null; then
            echo "    ${vm_name} SSH ready (attempt ${attempt}/${max_attempts})"
            return 0
        fi
        attempt=$((attempt + 1))
        sleep 2
    done

    echo "    WARNING: ${vm_name} SSH not ready after ${max_attempts} attempts."
    echo "    The VM may still be booting. Check log: ${WORK_DIR}/vm*.log"
    return 1
}

# --- Main ---
main() {
    check_files
    check_running

    # Start VM1 first (it creates the multicast group)
    start_vm 1 "${VM1_DISK}" "${VM1_SEED}" "${VM1_SSH_PORT}" "${VM1_MAC}" "${VM1_LOG}" "${VM1_PID_FILE}"

    # Brief delay before starting VM2 so mcast group is established
    sleep 2

    # Start VM2 (joins the multicast group)
    start_vm 2 "${VM2_DISK}" "${VM2_SEED}" "${VM2_SSH_PORT}" "${VM2_MAC}" "${VM2_LOG}" "${VM2_PID_FILE}"

    echo ""
    echo "==> Both VMs launched. Waiting for SSH connectivity..."

    # Install sshpass if not available (needed for automated SSH)
    if ! command -v sshpass &>/dev/null; then
        echo "    Installing sshpass for automated SSH..."
        brew install sshpass 2>/dev/null || brew install esolitos/ipa/sshpass 2>/dev/null || true
    fi

    wait_for_ssh "${VM1_SSH_PORT}" "VM1" || true
    wait_for_ssh "${VM2_SSH_PORT}" "VM2" || true

    echo ""
    echo "==> VMs are running:"
    echo "    VM1: SSH → ssh -p ${VM1_SSH_PORT} axon@localhost  (password: axon)"
    echo "    VM2: SSH → ssh -p ${VM2_SSH_PORT} axon@localhost  (password: axon)"
    echo "    Log:  ${WORK_DIR}/vm{1,2}.log"
    echo ""
    echo "    Next step: ./provision.sh"
}

main "$@"
