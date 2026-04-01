#!/bin/bash
# run_test.sh - Run automated dual-node RDMA tests between two VMs
#
# Tests:
#   1. rxe0 device verification
#   2. ibv_rc_pingpong (Soft-RoCE baseline)
#   3. ibv_ud_pingpong
#   4. zerokv ping_pong (tag send/recv across nodes)
#   5. zerokv rdma_put_get (RDMA put/get + atomics across nodes)
set -euo pipefail

VM1_SSH_PORT=2222
VM2_SSH_PORT=2223
VM1_RDMA_IP="10.0.0.1"
VM2_RDMA_IP="10.0.0.2"
SSH_USER="axon"
SSH_PASS="axon"
SSH_OPTS="-o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -o LogLevel=ERROR"

PASS=0
FAIL=0
RESULTS=()

# --- SSH helper ---
vm_ssh() {
    local port=$1
    shift
    sshpass -p "${SSH_PASS}" ssh ${SSH_OPTS} -p "${port}" "${SSH_USER}@localhost" "$@"
}

# --- Test result helper ---
record_result() {
    local test_name=$1
    local status=$2
    if [[ "${status}" == "true" ]]; then
        PASS=$((PASS + 1))
        RESULTS+=("  PASS  ${test_name}")
        echo "    [PASS] ${test_name}"
    else
        FAIL=$((FAIL + 1))
        RESULTS+=("  FAIL  ${test_name}")
        echo "    [FAIL] ${test_name}"
    fi
}

# --- Test 1: Verify rxe0 on both VMs ---
test_rxe_devices() {
    echo "==> Test 1: Verify rxe0 on both VMs..."

    local vm1_ok="false"
    local vm2_ok="false"

    vm_ssh "${VM1_SSH_PORT}" "ibv_devices 2>&1 | grep -q rxe0" && vm1_ok="true" || true
    vm_ssh "${VM2_SSH_PORT}" "ibv_devices 2>&1 | grep -q rxe0" && vm2_ok="true" || true

    record_result "VM1 ibv_devices shows rxe0" "${vm1_ok}"
    record_result "VM2 ibv_devices shows rxe0" "${vm2_ok}"
}

# --- Test 2: ibv_rc_pingpong ---
test_rc_pingpong() {
    echo "==> Test 2: ibv_rc_pingpong (RC transport)..."

    # Start server on VM1
    vm_ssh "${VM1_SSH_PORT}" "ibv_rc_pingpong -d rxe0 -g 0" > /dev/null 2>&1 &
    local server_pid=$!
    sleep 3

    local client_output
    if client_output=$(vm_ssh "${VM2_SSH_PORT}" \
        "timeout 30 ibv_rc_pingpong -d rxe0 -g 0 ${VM1_RDMA_IP}" 2>&1); then
        record_result "ibv_rc_pingpong VM1<->VM2" "true"
        local throughput
        throughput=$(echo "${client_output}" | grep -oE '[0-9]+\.[0-9]+ Mbit/sec' | head -1 || echo "N/A")
        echo "    Throughput: ${throughput}"
    else
        record_result "ibv_rc_pingpong VM1<->VM2" "false"
        echo "    Client output (last 5 lines):"
        echo "${client_output}" | tail -5 | sed 's/^/      /'
    fi

    kill "${server_pid}" 2>/dev/null || true
    wait "${server_pid}" 2>/dev/null || true
}

# --- Test 3: ibv_ud_pingpong ---
test_ud_pingpong() {
    echo "==> Test 3: ibv_ud_pingpong (UD transport)..."

    vm_ssh "${VM1_SSH_PORT}" "ibv_ud_pingpong -d rxe0 -g 0 -s 1024" > /dev/null 2>&1 &
    local server_pid=$!
    sleep 3

    local client_output
    if client_output=$(vm_ssh "${VM2_SSH_PORT}" \
        "timeout 30 ibv_ud_pingpong -d rxe0 -g 0 -s 1024 ${VM1_RDMA_IP}" 2>&1); then
        record_result "ibv_ud_pingpong VM1<->VM2" "true"
        local throughput
        throughput=$(echo "${client_output}" | grep -oE '[0-9]+\.[0-9]+ Mbit/sec' | head -1 || echo "N/A")
        echo "    Throughput: ${throughput}"
    else
        record_result "ibv_ud_pingpong VM1<->VM2" "false"
        echo "    Client output (last 5 lines):"
        echo "${client_output}" | tail -5 | sed 's/^/      /'
    fi

    kill "${server_pid}" 2>/dev/null || true
    wait "${server_pid}" 2>/dev/null || true
}

# --- Test 4: zerokv binaries exist ---
test_axon_binaries() {
    echo "==> Test 4: zerokv binaries exist..."

    for bin in ping_pong send_recv rdma_put_get; do
        local exists="false"
        vm_ssh "${VM1_SSH_PORT}" "test -x /tmp/zerokv/build/${bin}" && exists="true" || true
        record_result "zerokv ${bin} binary exists in VM1" "${exists}"

        local exists2="false"
        vm_ssh "${VM2_SSH_PORT}" "test -x /tmp/zerokv/build/${bin}" && exists2="true" || true
        record_result "zerokv ${bin} binary exists in VM2" "${exists2}"
    done
}

# --- Test 5: zerokv ping_pong (tag send/recv) ---
test_axon_ping_pong_tcp() {
    echo "==> Test 5: zerokv ping_pong (TCP transport)..."

    # Start server on VM1
    vm_ssh "${VM1_SSH_PORT}" \
        "cd /tmp/zerokv && timeout 60 ./build/ping_pong --listen 0.0.0.0:13337 --transport tcp" \
        > /dev/null 2>&1 &
    local server_pid=$!
    sleep 3

    local client_output
    if client_output=$(vm_ssh "${VM2_SSH_PORT}" \
        "cd /tmp/zerokv && timeout 60 ./build/ping_pong --connect ${VM1_RDMA_IP}:13337 --transport tcp" 2>&1); then
        # Check for success indicator in output
        if echo "${client_output}" | grep -q "Completed"; then
            record_result "zerokv ping_pong (TCP) VM1<->VM2" "true"
            echo "${client_output}" | grep -E "(latency|Throughput|Completed)" | sed 's/^/      /'
        else
            record_result "zerokv ping_pong (TCP) VM1<->VM2" "false"
            echo "    Client output (last 10 lines):"
            echo "${client_output}" | tail -10 | sed 's/^/      /'
        fi
    else
        record_result "zerokv ping_pong (TCP) VM1<->VM2" "false"
        echo "    Client exited with error"
        echo "${client_output}" | tail -10 | sed 's/^/      /'
    fi

    kill "${server_pid}" 2>/dev/null || true
    wait "${server_pid}" 2>/dev/null || true
}

# --- Test 6: zerokv ping_pong (RDMA transport) ---
test_axon_ping_pong_rdma() {
    echo "==> Test 6: zerokv ping_pong (RDMA transport)..."

    vm_ssh "${VM1_SSH_PORT}" \
        "cd /tmp/zerokv && timeout 60 ./build/ping_pong --listen 0.0.0.0:13338 --transport rdma" \
        > /dev/null 2>&1 &
    local server_pid=$!
    sleep 3

    local client_output
    if client_output=$(vm_ssh "${VM2_SSH_PORT}" \
        "cd /tmp/zerokv && timeout 60 ./build/ping_pong --connect ${VM1_RDMA_IP}:13338 --transport rdma" 2>&1); then
        if echo "${client_output}" | grep -q "Completed"; then
            record_result "zerokv ping_pong (RDMA) VM1<->VM2" "true"
            echo "${client_output}" | grep -E "(latency|Throughput|Completed)" | sed 's/^/      /'
        else
            record_result "zerokv ping_pong (RDMA) VM1<->VM2" "false"
            echo "    Client output (last 10 lines):"
            echo "${client_output}" | tail -10 | sed 's/^/      /'
        fi
    else
        record_result "zerokv ping_pong (RDMA) VM1<->VM2" "false"
        echo "    Client exited with error"
        echo "${client_output}" | tail -10 | sed 's/^/      /'
    fi

    kill "${server_pid}" 2>/dev/null || true
    wait "${server_pid}" 2>/dev/null || true
}

# --- Test 7: zerokv rdma_put_get (RDMA put/get + atomics) ---
test_axon_rdma_put_get() {
    echo "==> Test 7: zerokv rdma_put_get (RDMA put/get + atomics)..."

    local transport="rdma"
    local port=13339

    vm_ssh "${VM1_SSH_PORT}" \
        "cd /tmp/zerokv && timeout 120 ./build/rdma_put_get --listen 0.0.0.0:${port} --transport ${transport}" \
        > /tmp/axon_rdma_server.log 2>&1 &
    local server_pid=$!
    sleep 3

    local client_output
    if client_output=$(vm_ssh "${VM2_SSH_PORT}" \
        "cd /tmp/zerokv && timeout 120 ./build/rdma_put_get --connect ${VM1_RDMA_IP}:${port} --transport ${transport}" 2>&1); then
        # Check for success
        if echo "${client_output}" | grep -q "Client done"; then
            record_result "zerokv rdma_put_get (RDMA) VM1<->VM2" "true"
            echo "${client_output}" | grep -E "(Put|Get|fadd|cswap|verified|done)" | sed 's/^/      /'
        else
            record_result "zerokv rdma_put_get (RDMA) VM1<->VM2" "false"
            echo "    Client output (last 15 lines):"
            echo "${client_output}" | tail -15 | sed 's/^/      /'
        fi
    else
        record_result "zerokv rdma_put_get (RDMA) VM1<->VM2" "false"
        echo "    Client exited with error"
        echo "${client_output}" | tail -15 | sed 's/^/      /'
    fi

    # Check server output too
    echo "    Server output:"
    cat /tmp/axon_rdma_server.log | tail -10 | sed 's/^/      /'

    kill "${server_pid}" 2>/dev/null || true
    wait "${server_pid}" 2>/dev/null || true
    rm -f /tmp/axon_rdma_server.log
}

# --- Print summary ---
print_summary() {
    echo ""
    echo "=========================================="
    echo "  Test Results Summary"
    echo "=========================================="
    for r in "${RESULTS[@]}"; do
        echo "${r}"
    done
    echo "------------------------------------------"
    echo "  Total: $((PASS + FAIL))  Passed: ${PASS}  Failed: ${FAIL}"
    echo "=========================================="

    if [[ ${FAIL} -gt 0 ]]; then
        return 1
    fi
    return 0
}

# --- Main ---
main() {
    if ! command -v sshpass &>/dev/null; then
        echo "ERROR: sshpass not found. Install with: brew install esolitos/ipa/sshpass"
        exit 1
    fi

    echo "==> Starting dual-node RDMA tests..."
    echo "    VM1: ${VM1_RDMA_IP} (SSH port ${VM1_SSH_PORT})"
    echo "    VM2: ${VM2_RDMA_IP} (SSH port ${VM2_SSH_PORT})"
    echo ""

    test_rxe_devices
    test_rc_pingpong
    test_ud_pingpong
    test_axon_binaries
    test_axon_ping_pong_tcp
    test_axon_ping_pong_rdma
    test_axon_rdma_put_get

    print_summary
}

main "$@"
