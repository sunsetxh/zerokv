#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"

COMMIT=""
VM1=""
VM2=""
VM_USER=""
VM_PASS=""
OUT_DIR="${ROOT_DIR}/out"

usage() {
    cat <<'EOF'
Usage: release_verify_examples.sh --commit <sha> --vm1 <host[:port]> --vm2 <host[:port]> --vm-user <user> --vm-pass <pass> [--out-dir <dir>]
EOF
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --commit)
            COMMIT="${2:-}"
            shift 2
            ;;
        --vm1)
            VM1="${2:-}"
            shift 2
            ;;
        --vm2)
            VM2="${2:-}"
            shift 2
            ;;
        --vm-user)
            VM_USER="${2:-}"
            shift 2
            ;;
        --vm-pass)
            VM_PASS="${2:-}"
            shift 2
            ;;
        --out-dir)
            OUT_DIR="${2:-}"
            shift 2
            ;;
        -h|--help)
            usage
            exit 0
            ;;
        *)
            echo "Unknown argument: $1" >&2
            usage >&2
            exit 1
            ;;
    esac
done

if [[ -z "${COMMIT}" || -z "${VM1}" || -z "${VM2}" || -z "${VM_USER}" || -z "${VM_PASS}" ]]; then
    usage >&2
    exit 1
fi

if [[ "${VM1}" == *:* ]]; then
    VM1_HOST="${VM1%:*}"
    VM1_PORT="${VM1##*:}"
else
    VM1_HOST="${VM1}"
    VM1_PORT="22"
fi

if [[ "${VM2}" == *:* ]]; then
    VM2_HOST="${VM2%:*}"
    VM2_PORT="${VM2##*:}"
else
    VM2_HOST="${VM2}"
    VM2_PORT="22"
fi

SSH_OPTS=(
    -o StrictHostKeyChecking=no
    -o UserKnownHostsFile=/dev/null
    -o LogLevel=ERROR
)

VM1_RDMA_IP="10.0.0.1"
VM2_RDMA_IP="10.0.0.2"
REMOTE_SOURCE_ARCHIVE="/tmp/zerokv-src-aarch64-${COMMIT}.tar.gz"
REMOTE_PACKAGE_TARBALL="/tmp/alps_kv_wrap_pkg-aarch64-${COMMIT}.tar.gz"
REMOTE_PACKAGE_ROOT="/tmp/alps_kv_wrap_pkg-aarch64-${COMMIT}"
REMOTE_VM1_TREE="/tmp/release-verify-${COMMIT}-vm1"
REMOTE_VM2_TREE="/tmp/release-verify-${COMMIT}-vm2"
LOCAL_RELEASE_DIR="${OUT_DIR%/}/release-verify/${COMMIT}/arm"
LOCAL_EXAMPLES_DIR="${LOCAL_RELEASE_DIR}/examples"
LOCAL_SETUP_DIR="${LOCAL_EXAMPLES_DIR}/_setup"
LOCAL_SRC_ARCHIVE="${OUT_DIR%/}/src/zerokv-src-aarch64-${COMMIT}.tar.gz"
LOCAL_PACKAGE_TARBALL="${OUT_DIR%/}/packages/alps_kv_wrap_pkg-aarch64-${COMMIT}.tar.gz"
LOCAL_VM1_BUILD_LOG="${LOCAL_SETUP_DIR}/vm1-build.log"
LOCAL_VM2_BUILD_LOG="${LOCAL_SETUP_DIR}/vm2-build.log"
LOCAL_VM1_PACKAGE_LOG="${LOCAL_SETUP_DIR}/vm1-package.log"
LOCAL_VM2_PACKAGE_LOG="${LOCAL_SETUP_DIR}/vm2-package.log"

mapfile -t EXAMPLE_ROWS < <(
    python3 - "${COMMIT}" "${OUT_DIR%/}" <<'PY'
import sys
from scripts.release_verify_lib import render_example_commands

for item in render_example_commands(sys.argv[1], out_dir=sys.argv[2]):
    print(
        item["name"],
        item["source"],
        ",".join(item["build_targets"]) or "-",
        item["log_dir"],
        sep="\x1f",
    )
PY
)

declare -A EXAMPLE_LOG_DIRS=()
declare -A BUILD_TARGET_SEEN=()
EXAMPLE_ORDER=()
BUILD_TARGETS=()

for row in "${EXAMPLE_ROWS[@]}"; do
    IFS=$'\x1f' read -r name source build_targets_csv log_dir <<<"${row}"
    EXAMPLE_ORDER+=("${name}")
    EXAMPLE_LOG_DIRS["${name}"]="${log_dir}"
    if [[ "${source}" != "build_tree" || "${build_targets_csv}" == "-" ]]; then
        continue
    fi
    IFS=',' read -r -a row_targets <<<"${build_targets_csv}"
    for target in "${row_targets[@]}"; do
        if [[ -n "${BUILD_TARGET_SEEN[${target}]:-}" ]]; then
            continue
        fi
        BUILD_TARGET_SEEN["${target}"]=1
        BUILD_TARGETS+=("${target}")
    done
done

command -v sshpass >/dev/null 2>&1 || {
    echo "sshpass is required" >&2
    exit 1
}

if [[ ! -f "${LOCAL_PACKAGE_TARBALL}" ]]; then
    echo "Missing packaged artifact: ${LOCAL_PACKAGE_TARBALL}" >&2
    exit 1
fi

mkdir -p "${LOCAL_SETUP_DIR}" "${LOCAL_EXAMPLES_DIR}"
rm -f "${LOCAL_VM1_BUILD_LOG}" "${LOCAL_VM2_BUILD_LOG}" "${LOCAL_VM1_PACKAGE_LOG}" "${LOCAL_VM2_PACKAGE_LOG}"
mkdir -p "$(dirname "${LOCAL_SRC_ARCHIVE}")"
git -C "${ROOT_DIR}" archive --format=tar.gz -o "${LOCAL_SRC_ARCHIVE}" "${COMMIT}"

build_env_prefix() {
    printf 'env UCX_PROTO_ENABLE=n UCX_NET_DEVICES=rxe0:1 UCX_TLS=rc,sm,self'
}

package_env_prefix() {
    printf 'env UCX_PROTO_ENABLE=n UCX_NET_DEVICES=rxe0:1 UCX_TLS=rc,sm,self LD_LIBRARY_PATH=%q' "${REMOTE_PACKAGE_ROOT}/lib"
}

log_dir_for() {
    printf '%s' "${EXAMPLE_LOG_DIRS[$1]}"
}

vm_ssh() {
    local host="$1"
    local port="$2"
    shift 2
    sshpass -p "${VM_PASS}" ssh -p "${port}" "${SSH_OPTS[@]}" "${VM_USER}@${host}" "$@"
}

vm_scp_to() {
    local host="$1"
    local port="$2"
    local src="$3"
    local dst="$4"
    sshpass -p "${VM_PASS}" scp -P "${port}" "${SSH_OPTS[@]}" "${src}" "${VM_USER}@${host}:${dst}"
}

start_remote_bg() {
    local host="$1"
    local port="$2"
    local log_path="$3"
    shift 3
    sshpass -p "${VM_PASS}" ssh -p "${port}" "${SSH_OPTS[@]}" "${VM_USER}@${host}" "$@" >"${log_path}" 2>&1 &
    START_REMOTE_BG_PID=$!
}

cleanup_pids=()
cleanup() {
    local pid
    for pid in "${cleanup_pids[@]:-}"; do
        kill "${pid}" 2>/dev/null || true
        wait "${pid}" 2>/dev/null || true
    done
}
trap cleanup EXIT

forget_pid() {
    local target="$1"
    local remaining=()
    local pid
    for pid in "${cleanup_pids[@]:-}"; do
        if [[ "${pid}" != "${target}" ]]; then
            remaining+=("${pid}")
        fi
    done
    cleanup_pids=("${remaining[@]}")
}

wait_for_log_pattern() {
    local log_path="$1"
    local pattern="$2"
    local label="$3"
    local pid="${4:-}"
    local timeout_s="${5:-30}"
    local attempt
    for ((attempt = 0; attempt < timeout_s * 10; attempt++)); do
        if [[ -f "${log_path}" ]] && grep -Eq "${pattern}" "${log_path}"; then
            return 0
        fi
        if [[ -n "${pid}" ]] && ! kill -0 "${pid}" 2>/dev/null; then
            wait "${pid}" 2>/dev/null || true
            forget_pid "${pid}"
            echo "FAIL arm examples: ${label} log=${log_path}" >&2
            return 1
        fi
        sleep 0.1
    done
    echo "FAIL arm examples: ${label} log=${log_path}" >&2
    return 1
}

wait_pid() {
    local pid="$1"
    local label="$2"
    local log_path="$3"
    local status=0
    wait "${pid}" || status=$?
    forget_pid "${pid}"
    if [[ "${status}" -ne 0 ]]; then
        echo "FAIL arm examples: ${label} log=${log_path}" >&2
        return 1
    fi
}

ensure_running() {
    local pid="$1"
    local label="$2"
    local log_path="$3"
    if kill -0 "${pid}" 2>/dev/null; then
        return 0
    fi
    wait "${pid}" 2>/dev/null || true
    forget_pid "${pid}"
    echo "FAIL arm examples: ${label} log=${log_path}" >&2
    return 1
}

terminate_bg() {
    local pid="$1"
    local label="$2"
    local log_path="$3"
    if ! ensure_running "${pid}" "${label}" "${log_path}"; then
        return 1
    fi
    kill "${pid}" 2>/dev/null || true
    wait "${pid}" 2>/dev/null || true
    forget_pid "${pid}"
}

stop_bg_best_effort() {
    local pid="$1"
    kill "${pid}" 2>/dev/null || true
    wait "${pid}" 2>/dev/null || true
    forget_pid "${pid}"
}

prepare_build_tree() {
    local vm_label="$1"
    local host="$2"
    local port="$3"
    local remote_root="$4"
    local log_path="$5"

    vm_scp_to "${host}" "${port}" "${LOCAL_SRC_ARCHIVE}" "${REMOTE_SOURCE_ARCHIVE}"

    local build_jobs='$(nproc)'
    local remote_cmd
    remote_cmd="set -euo pipefail; rm -rf $(printf '%q' "${remote_root}"); mkdir -p $(printf '%q' "${remote_root}"); tar xzf $(printf '%q' "${REMOTE_SOURCE_ARCHIVE}") -C $(printf '%q' "${remote_root}"); cd $(printf '%q' "${remote_root}"); PKG_CONFIG_PATH=/usr/local/ucx-static-pic/lib/pkgconfig:/usr/local/ucx-static-pic/lib64/pkgconfig:\${PKG_CONFIG_PATH:-} cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_COMPILER=g++-10 -DUCX_ROOT=/usr/local/ucx-static-pic -DZEROKV_BUILD_TESTS=OFF -DZEROKV_BUILD_EXAMPLES=ON -DZEROKV_BUILD_BENCHMARK=OFF -DZEROKV_BUILD_PYTHON=OFF; cmake --build build --target ${BUILD_TARGETS[*]} -j${build_jobs}"

    if ! vm_ssh "${host}" "${port}" "${remote_cmd}" >"${log_path}" 2>&1; then
        echo "FAIL arm examples setup: ${vm_label} build log=${log_path}" >&2
        exit 1
    fi
}

prepare_package_tree() {
    local vm_label="$1"
    local host="$2"
    local port="$3"
    local log_path="$4"

    vm_scp_to "${host}" "${port}" "${LOCAL_PACKAGE_TARBALL}" "${REMOTE_PACKAGE_TARBALL}"

    local remote_cmd
    remote_cmd="set -euo pipefail; rm -rf $(printf '%q' "${REMOTE_PACKAGE_ROOT}"); tar xzf $(printf '%q' "${REMOTE_PACKAGE_TARBALL}") -C /tmp"

    if ! vm_ssh "${host}" "${port}" "${remote_cmd}" >"${log_path}" 2>&1; then
        echo "FAIL arm examples setup: ${vm_label} package log=${log_path}" >&2
        exit 1
    fi
}

run_ping_pong() {
    local log_dir
    log_dir="$(log_dir_for ping_pong)"
    mkdir -p "${log_dir}"

    local server_log="${log_dir}/server.log"
    local client_log="${log_dir}/client.log"
    local server_cmd client_cmd server_pid

    server_cmd="cd $(printf '%q' "${REMOTE_VM1_TREE}") && $(build_env_prefix) timeout 180 ./build/ping_pong --listen 0.0.0.0:13337 --transport rdma"
    client_cmd="cd $(printf '%q' "${REMOTE_VM2_TREE}") && $(build_env_prefix) timeout 180 ./build/ping_pong --connect ${VM1_RDMA_IP}:13337 --transport rdma"

    start_remote_bg "${VM1_HOST}" "${VM1_PORT}" "${server_log}" "${server_cmd}"
    server_pid="${START_REMOTE_BG_PID}"
    cleanup_pids+=("${server_pid}")
    wait_for_log_pattern "${server_log}" "Listening on " "ping_pong server" "${server_pid}" 30

    if ! vm_ssh "${VM2_HOST}" "${VM2_PORT}" "${client_cmd}" >"${client_log}" 2>&1; then
        echo "FAIL arm examples: ping_pong log=${client_log}" >&2
        stop_bg_best_effort "${server_pid}"
        return 1
    fi

    if ! wait_pid "${server_pid}" "ping_pong" "${server_log}"; then
        return 1
    fi
    echo "PASS arm examples: ping_pong"
}

run_rdma_put_get() {
    local log_dir
    log_dir="$(log_dir_for rdma_put_get)"
    mkdir -p "${log_dir}"

    local server_log="${log_dir}/server.log"
    local client_log="${log_dir}/client.log"
    local server_cmd client_cmd server_pid

    server_cmd="cd $(printf '%q' "${REMOTE_VM1_TREE}") && $(build_env_prefix) timeout 180 ./build/rdma_put_get --listen 0.0.0.0:13339 --transport rdma"
    client_cmd="cd $(printf '%q' "${REMOTE_VM2_TREE}") && $(build_env_prefix) timeout 180 ./build/rdma_put_get --connect ${VM1_RDMA_IP}:13339 --transport rdma"

    start_remote_bg "${VM1_HOST}" "${VM1_PORT}" "${server_log}" "${server_cmd}"
    server_pid="${START_REMOTE_BG_PID}"
    cleanup_pids+=("${server_pid}")
    wait_for_log_pattern "${server_log}" "Listening on " "rdma_put_get server" "${server_pid}" 30

    if ! vm_ssh "${VM2_HOST}" "${VM2_PORT}" "${client_cmd}" >"${client_log}" 2>&1; then
        echo "FAIL arm examples: rdma_put_get log=${client_log}" >&2
        stop_bg_best_effort "${server_pid}"
        return 1
    fi

    if ! wait_pid "${server_pid}" "rdma_put_get" "${server_log}"; then
        return 1
    fi
    echo "PASS arm examples: rdma_put_get"
}

run_kv_demo() {
    local log_dir
    log_dir="$(log_dir_for kv_demo)"
    mkdir -p "${log_dir}"

    local server_log="${log_dir}/server.log"
    local publisher_log="${log_dir}/publisher.log"
    local client_log="${log_dir}/client.log"
    local server_cmd publisher_cmd client_cmd server_pid publisher_pid

    server_cmd="cd $(printf '%q' "${REMOTE_VM1_TREE}") && $(build_env_prefix) timeout 180 ./build/kv_demo --mode server --listen ${VM1_RDMA_IP}:15000 --transport rdma"
    publisher_cmd="cd $(printf '%q' "${REMOTE_VM1_TREE}") && $(build_env_prefix) timeout 180 ./build/kv_demo --mode publish --server-addr ${VM1_RDMA_IP}:15000 --data-addr ${VM1_RDMA_IP}:0 --node-id publisher --key release-verify-kv-demo --value hello-release-verify --transport rdma --hold"
    client_cmd="cd $(printf '%q' "${REMOTE_VM2_TREE}") && $(build_env_prefix) timeout 180 ./build/kv_demo --mode fetch --server-addr ${VM1_RDMA_IP}:15000 --data-addr ${VM2_RDMA_IP}:0 --node-id reader --key release-verify-kv-demo --transport rdma"

    start_remote_bg "${VM1_HOST}" "${VM1_PORT}" "${server_log}" "${server_cmd}"
    server_pid="${START_REMOTE_BG_PID}"
    cleanup_pids+=("${server_pid}")
    wait_for_log_pattern "${server_log}" "KV server listening on " "kv_demo server" "${server_pid}" 30

    start_remote_bg "${VM1_HOST}" "${VM1_PORT}" "${publisher_log}" "${publisher_cmd}"
    publisher_pid="${START_REMOTE_BG_PID}"
    cleanup_pids+=("${publisher_pid}")
    wait_for_log_pattern "${publisher_log}" "Published key=release-verify-kv-demo" "kv_demo publisher" "${publisher_pid}" 30

    if ! vm_ssh "${VM2_HOST}" "${VM2_PORT}" "${client_cmd}" >"${client_log}" 2>&1; then
        echo "FAIL arm examples: kv_demo log=${client_log}" >&2
        stop_bg_best_effort "${publisher_pid}"
        stop_bg_best_effort "${server_pid}"
        return 1
    fi

    terminate_bg "${publisher_pid}" "kv_demo publisher" "${publisher_log}"

    terminate_bg "${server_pid}" "kv_demo server" "${server_log}"
    echo "PASS arm examples: kv_demo"
}

run_kv_wait_fetch() {
    local log_dir
    log_dir="$(log_dir_for kv_wait_fetch)"
    mkdir -p "${log_dir}"

    local server_log="${log_dir}/server.log"
    local waiter_log="${log_dir}/waiter.log"
    local publisher_log="${log_dir}/publisher.log"
    local server_cmd waiter_cmd publisher_cmd server_pid waiter_pid publisher_pid

    server_cmd="cd $(printf '%q' "${REMOTE_VM1_TREE}") && $(build_env_prefix) timeout 180 ./build/kv_demo --mode server --listen ${VM1_RDMA_IP}:15150 --transport rdma"
    waiter_cmd="cd $(printf '%q' "${REMOTE_VM1_TREE}") && $(build_env_prefix) timeout 180 ./build/kv_wait_fetch --mode subscribe-fetch-once --server-addr ${VM1_RDMA_IP}:15150 --data-addr ${VM1_RDMA_IP}:0 --node-id waiter --key release-verify-waitfetch --transport rdma --timeout-ms 10000"
    publisher_cmd="cd $(printf '%q' "${REMOTE_VM2_TREE}") && $(build_env_prefix) timeout 180 ./build/kv_demo --mode publish --server-addr ${VM1_RDMA_IP}:15150 --data-addr ${VM2_RDMA_IP}:0 --node-id publisher --key release-verify-waitfetch --value hello-waitfetch --transport rdma --hold"

    start_remote_bg "${VM1_HOST}" "${VM1_PORT}" "${server_log}" "${server_cmd}"
    server_pid="${START_REMOTE_BG_PID}"
    cleanup_pids+=("${server_pid}")
    wait_for_log_pattern "${server_log}" "KV server listening on " "kv_wait_fetch server" "${server_pid}" 30

    start_remote_bg "${VM1_HOST}" "${VM1_PORT}" "${waiter_log}" "${waiter_cmd}"
    waiter_pid="${START_REMOTE_BG_PID}"
    cleanup_pids+=("${waiter_pid}")
    sleep 1

    start_remote_bg "${VM2_HOST}" "${VM2_PORT}" "${publisher_log}" "${publisher_cmd}"
    publisher_pid="${START_REMOTE_BG_PID}"
    cleanup_pids+=("${publisher_pid}")
    wait_for_log_pattern "${publisher_log}" "Published key=release-verify-waitfetch" "kv_wait_fetch publisher" "${publisher_pid}" 30

    if ! wait_pid "${waiter_pid}" "kv_wait_fetch" "${waiter_log}"; then
        stop_bg_best_effort "${publisher_pid}"
        stop_bg_best_effort "${server_pid}"
        return 1
    fi

    terminate_bg "${publisher_pid}" "kv_wait_fetch publisher" "${publisher_log}"

    terminate_bg "${server_pid}" "kv_wait_fetch server" "${server_log}"
    echo "PASS arm examples: kv_wait_fetch"
}

run_message_kv_demo() {
    local log_dir
    log_dir="$(log_dir_for message_kv_demo)"
    mkdir -p "${log_dir}"

    local rank0_log="${log_dir}/rank0.log"
    local rank1_log="${log_dir}/rank1.log"
    local rank0_cmd rank1_cmd rank0_pid

    rank0_cmd="cd $(printf '%q' "${REMOTE_VM1_TREE}") && $(build_env_prefix) timeout 180 ./build/message_kv_demo --role rank0 --listen ${VM1_RDMA_IP}:16040 --data-addr ${VM1_RDMA_IP}:0 --node-id rank0 --messages 1 --sizes 1K --warmup-rounds 0 --timeout-ms 10000 --transport rdma"
    rank1_cmd="cd $(printf '%q' "${REMOTE_VM2_TREE}") && $(build_env_prefix) timeout 180 ./build/message_kv_demo --role rank1 --server-addr ${VM1_RDMA_IP}:16040 --data-addr ${VM2_RDMA_IP}:0 --node-id rank1 --threads 1 --send-mode sync --warmup-rounds 0 --sizes 1K --timeout-ms 10000 --transport rdma"

    start_remote_bg "${VM1_HOST}" "${VM1_PORT}" "${rank0_log}" "${rank0_cmd}"
    rank0_pid="${START_REMOTE_BG_PID}"
    cleanup_pids+=("${rank0_pid}")
    sleep 1

    if ! vm_ssh "${VM2_HOST}" "${VM2_PORT}" "${rank1_cmd}" >"${rank1_log}" 2>&1; then
        echo "FAIL arm examples: message_kv_demo log=${rank1_log}" >&2
        stop_bg_best_effort "${rank0_pid}"
        return 1
    fi

    if ! wait_pid "${rank0_pid}" "message_kv_demo" "${rank0_log}"; then
        return 1
    fi
    echo "PASS arm examples: message_kv_demo"
}

run_alps_kv_bench() {
    local log_dir
    log_dir="$(log_dir_for alps_kv_bench)"
    mkdir -p "${log_dir}"

    local server_log="${log_dir}/server.log"
    local client_log="${log_dir}/client.log"
    local server_cmd client_cmd server_pid

    server_cmd="cd $(printf '%q' "${REMOTE_PACKAGE_ROOT}") && $(package_env_prefix) timeout 180 ./bin/alps_kv_bench --mode server --port 16000 --sizes 256K --iters 1 --warmup 0 --threads 1"
    client_cmd="cd $(printf '%q' "${REMOTE_PACKAGE_ROOT}") && $(package_env_prefix) timeout 180 ./bin/alps_kv_bench --mode client --host ${VM1_RDMA_IP} --port 16000 --sizes 256K --iters 1 --warmup 0 --threads 1"

    start_remote_bg "${VM1_HOST}" "${VM1_PORT}" "${server_log}" "${server_cmd}"
    server_pid="${START_REMOTE_BG_PID}"
    cleanup_pids+=("${server_pid}")
    wait_for_log_pattern "${server_log}" "ALPS_KV_LISTEN address=" "alps_kv_bench server" "${server_pid}" 30

    if ! vm_ssh "${VM2_HOST}" "${VM2_PORT}" "${client_cmd}" >"${client_log}" 2>&1; then
        echo "FAIL arm examples: alps_kv_bench log=${client_log}" >&2
        stop_bg_best_effort "${server_pid}"
        return 1
    fi

    if ! wait_pid "${server_pid}" "alps_kv_bench" "${server_log}"; then
        return 1
    fi
    echo "PASS arm examples: alps_kv_bench"
}

prepare_build_tree "vm1" "${VM1_HOST}" "${VM1_PORT}" "${REMOTE_VM1_TREE}" "${LOCAL_VM1_BUILD_LOG}"
prepare_build_tree "vm2" "${VM2_HOST}" "${VM2_PORT}" "${REMOTE_VM2_TREE}" "${LOCAL_VM2_BUILD_LOG}"
prepare_package_tree "vm1" "${VM1_HOST}" "${VM1_PORT}" "${LOCAL_VM1_PACKAGE_LOG}"
prepare_package_tree "vm2" "${VM2_HOST}" "${VM2_PORT}" "${LOCAL_VM2_PACKAGE_LOG}"

run_ping_pong
run_rdma_put_get
run_kv_demo
run_kv_wait_fetch
run_message_kv_demo
run_alps_kv_bench
