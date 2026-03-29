#!/bin/bash
# stop_vms.sh - Stop the two QEMU VMs
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
WORK_DIR="${SCRIPT_DIR}/.vm_work"

VM1_PID_FILE="${WORK_DIR}/vm1.pid"
VM2_PID_FILE="${WORK_DIR}/vm2.pid"

stop_vm() {
    local pidfile=$1
    local vm_name=$2

    if [[ ! -f "${pidfile}" ]]; then
        echo "    ${vm_name}: No PID file found (${pidfile})"
        return
    fi

    local pid
    pid=$(cat "${pidfile}")

    if ! kill -0 "${pid}" 2>/dev/null; then
        echo "    ${vm_name}: Process ${pid} not running (stale PID file)"
        rm -f "${pidfile}"
        return
    fi

    echo "==> Stopping ${vm_name} (PID: ${pid})..."

    # Try graceful shutdown via QEMU monitor (send system_powerdown)
    # Use the serial/monitor to send quit command
    kill -TERM "${pid}" 2>/dev/null || true

    # Wait up to 10 seconds for graceful shutdown
    local waited=0
    while kill -0 "${pid}" 2>/dev/null && [[ ${waited} -lt 10 ]]; do
        sleep 1
        waited=$((waited + 1))
    done

    # Force kill if still running
    if kill -0 "${pid}" 2>/dev/null; then
        echo "    ${vm_name}: Force killing..."
        kill -KILL "${pid}" 2>/dev/null || true
    fi

    rm -f "${pidfile}"
    echo "    ${vm_name} stopped."
}

main() {
    echo "==> Stopping QEMU VMs..."
    stop_vm "${VM1_PID_FILE}" "VM1"
    stop_vm "${VM2_PID_FILE}" "VM2"
    echo "==> Done."
}

main "$@"
