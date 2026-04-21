#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
COMMON_ROOT="$(cd "$(dirname "$(git -C "${ROOT_DIR}" rev-parse --git-common-dir)")" && pwd)"
ARCH="aarch64"
COMMIT=""
VM1=""
VM_USER=""
VM_PASS=""
OUT_DIR="${ROOT_DIR}/out"

usage() {
    cat <<'EOF'
Usage: build_pkg_arm_remote.sh --commit <sha> --vm1 <host[:port]> --vm-user <user> --vm-pass <pass> [--out-dir <dir>]
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

if [[ -z "${COMMIT}" || -z "${VM1}" || -z "${VM_USER}" || -z "${VM_PASS}" ]]; then
    usage >&2
    exit 1
fi

COMMIT="$(git -C "${ROOT_DIR}" rev-parse "${COMMIT}")"
PACKAGE_TAG="$(git -C "${ROOT_DIR}" rev-parse --short "${COMMIT}")"

if [[ "${VM1}" == *:* ]]; then
    VM_HOST="${VM1%:*}"
    VM_PORT="${VM1##*:}"
else
    VM_HOST="${VM1}"
    VM_PORT="22"
fi

if [[ -z "${VM_PORT}" ]]; then
    VM_PORT="22"
fi

UCX_TARBALL="${ROOT_DIR}/ucx-v1.20.0.tar.gz"
if [[ ! -f "${UCX_TARBALL}" ]]; then
    UCX_TARBALL="${COMMON_ROOT}/ucx-v1.20.0.tar.gz"
fi
if [[ ! -f "${UCX_TARBALL}" ]]; then
    echo "UCX tarball not found: ${UCX_TARBALL}" >&2
    exit 1
fi

RELEASE_DIR="${OUT_DIR%/}/release-verify/${COMMIT}/arm"
SRC_DIR="${OUT_DIR%/}/src"
PACKAGE_DIR="${OUT_DIR%/}/packages"
BUILD_LOG="${RELEASE_DIR}/build.log"
PACKAGE_TXT="${RELEASE_DIR}/package.txt"
SRC_ARCHIVE="${SRC_DIR}/zerokv-src-${ARCH}-${COMMIT}.tar.gz"
LOCAL_TARBALL="${PACKAGE_DIR}/zerokv-${ARCH}-${PACKAGE_TAG}.tar.gz"
LATEST_TARBALL="${PACKAGE_DIR}/zerokv-${ARCH}.tar.gz"

REMOTE_SRC_ARCHIVE="/tmp/zerokv-src-${ARCH}-${COMMIT}.tar.gz"
REMOTE_UCX_TARBALL="/tmp/ucx-v1.20.0.tar.gz"
REMOTE_TARBALL="/tmp/zerokv-${ARCH}-${PACKAGE_TAG}.tar.gz"
REMOTE_PACKAGE_TXT="/tmp/zerokv-${ARCH}-${PACKAGE_TAG}.package.txt"
REMOTE_SCRIPT="$(mktemp)"
ASKPASS_SCRIPT=""

SSH_OPTS=(
    -o StrictHostKeyChecking=no
    -o UserKnownHostsFile=/dev/null
    -o LogLevel=ERROR
)

cleanup() {
    rm -f "${REMOTE_SCRIPT}" "${ASKPASS_SCRIPT}"
}
trap cleanup EXIT

build_password_prefix() {
    if command -v sshpass >/dev/null 2>&1; then
        PASSWORD_PREFIX=(sshpass -p "${VM_PASS}")
        return
    fi
    ASKPASS_SCRIPT="$(mktemp)"
    printf '%s\n' '#!/usr/bin/env bash' "printf '%s\n' $(printf '%q' "${VM_PASS}")" > "${ASKPASS_SCRIPT}"
    chmod 700 "${ASKPASS_SCRIPT}"
    PASSWORD_PREFIX=(
        env
        SSH_ASKPASS="${ASKPASS_SCRIPT}"
        SSH_ASKPASS_REQUIRE=force
        DISPLAY=codex
        setsid
        -w
    )
}

build_password_prefix

vm_ssh() {
    "${PASSWORD_PREFIX[@]}" ssh -p "${VM_PORT}" "${SSH_OPTS[@]}" "${VM_USER}@${VM_HOST}" "$@" < /dev/null
}

vm_scp_to() {
    local src="$1"
    local dst="$2"
    "${PASSWORD_PREFIX[@]}" scp -P "${VM_PORT}" "${SSH_OPTS[@]}" "${src}" "${VM_USER}@${VM_HOST}:${dst}" < /dev/null
}

vm_scp_from() {
    local src="$1"
    local dst="$2"
    "${PASSWORD_PREFIX[@]}" scp -P "${VM_PORT}" "${SSH_OPTS[@]}" "${VM_USER}@${VM_HOST}:${src}" "${dst}" < /dev/null
}

mkdir -p "${RELEASE_DIR}" "${SRC_DIR}" "${PACKAGE_DIR}"
rm -f "${BUILD_LOG}" "${PACKAGE_TXT}" "${LOCAL_TARBALL}"

git -C "${ROOT_DIR}" archive --format=tar.gz -o "${SRC_ARCHIVE}" "${COMMIT}"
vm_scp_to "${SRC_ARCHIVE}" "${REMOTE_SRC_ARCHIVE}"
vm_scp_to "${UCX_TARBALL}" "${REMOTE_UCX_TARBALL}"

cat > "${REMOTE_SCRIPT}" <<'EOF'
#!/usr/bin/env bash
set -euo pipefail

: "${COMMIT_ID:?}"
: "${PACKAGE_TAG:?}"
: "${REMOTE_SRC_ARCHIVE:?}"
: "${REMOTE_UCX_TARBALL:?}"
: "${REMOTE_TARBALL:?}"
: "${REMOTE_PACKAGE_TXT:?}"

ARCH="aarch64"
PKG_DIR_NAME="zerokv-${ARCH}-${PACKAGE_TAG}"
WORK_ROOT="/tmp/arm-release-verify-${COMMIT_ID}"
PKG_DIR="/tmp/${PKG_DIR_NAME}"
UCX_PREFIX="${HOME}/.local/ucx-static-pic"
MAX_GLIBCXX="3.4.28"

version_gt() {
    local lhs="$1"
    local rhs="$2"
    [[ "$(printf '%s\n%s\n' "${lhs}" "${rhs}" | sort -V | tail -n 1)" == "${lhs}" && "${lhs}" != "${rhs}" ]]
}

record() {
    printf '%s\n' "$1" | tee -a "${REMOTE_PACKAGE_TXT}"
}

max_glibcxx() {
    local file="$1"
    objdump -T "${file}" 2>/dev/null | sed -n 's/.*\(GLIBCXX_[0-9.]*\).*/\1/p' | sort -Vu | tail -n 1
}

stage_ucx_runtime() {
    mkdir -p "${PKG_DIR}/lib" "${PKG_DIR}/lib/ucx"
    shopt -s nullglob
    local found_shared=0
    local found_modules=0
    for dir in "${UCX_PREFIX}/lib" "${UCX_PREFIX}/lib64"; do
        [[ -d "${dir}" ]] || continue
        for lib in "${dir}"/libuc*.so*; do
            cp -a "${lib}" "${PKG_DIR}/lib/"
            found_shared=1
        done
        if [[ -d "${dir}/ucx" ]]; then
            cp -a "${dir}/ucx/." "${PKG_DIR}/lib/ucx/"
            found_modules=1
        fi
    done
    shopt -u nullglob
    if [[ "${found_shared}" != "1" ]]; then
        echo "Failed to stage UCX shared libraries" >&2
        exit 1
    fi
    if [[ "${found_modules}" != "1" ]]; then
        echo "Failed to stage UCX provider modules" >&2
        exit 1
    fi
}

validate_pkg() {
    local rel
    for rel in \
        COMMIT_ID \
        ARCH \
        README.md \
        docs/alps_kv_wrap/README.md \
        examples/cpp_usage.cpp \
        examples/python_usage.py \
        bin/alps_kv_bench \
        bin/ping_pong \
        bin/send_recv \
        bin/rdma_put_get \
        bin/kv_demo \
        bin/kv_wait_fetch \
        bin/message_kv_demo \
        bin/kv_bench \
        bin/ucx_info \
        lib/libalps_kv_wrap.so \
        lib/libzerokv.so
    do
        [[ -e "${PKG_DIR}/${rel}" ]] || {
            echo "Missing required package file: ${rel}" >&2
            exit 1
        }
        record "present: ${rel}"
    done
    local recorded_commit
    local recorded_arch
    recorded_commit="$(<"${PKG_DIR}/COMMIT_ID")"
    recorded_arch="$(<"${PKG_DIR}/ARCH")"
    if [[ "${recorded_commit}" != "${COMMIT_ID}" ]]; then
        echo "Package commit mismatch: expected ${COMMIT_ID}, got ${recorded_commit}" >&2
        exit 1
    fi
    if [[ "${recorded_arch}" != "${ARCH}" ]]; then
        echo "Package arch mismatch: expected ${ARCH}, got ${recorded_arch}" >&2
        exit 1
    fi
    record "validated_commit=${recorded_commit}"
    record "validated_arch=${recorded_arch}"
    [[ -d "${PKG_DIR}/lib/ucx" ]] || {
        echo "Missing UCX provider directory" >&2
        exit 1
    }
    [[ -n "$(find "${PKG_DIR}/lib/ucx" -maxdepth 1 -type f -print -quit)" ]] || {
        echo "Missing UCX provider modules" >&2
        exit 1
    }
    record "present: lib/ucx/provider-modules"
    record "commit=$(cat "${PKG_DIR}/COMMIT_ID")"
    record "arch=$(cat "${PKG_DIR}/ARCH")"
    record "compiler=$(gcc --version | head -n 1)"
    record "toolchain=$(g++ --version | head -n 1)"
    record "fingerprint=$(strings "${PKG_DIR}/lib/libalps_kv_wrap.so" | grep -m1 'GCC:' || true)"

    local file
    for file in \
        "${PKG_DIR}/lib/libalps_kv_wrap.so" \
        "${PKG_DIR}/lib/libzerokv.so" \
        "${PKG_DIR}/bin/alps_kv_bench" \
        "${PKG_DIR}/bin/ping_pong" \
        "${PKG_DIR}/bin/send_recv" \
        "${PKG_DIR}/bin/rdma_put_get" \
        "${PKG_DIR}/bin/kv_demo" \
        "${PKG_DIR}/bin/kv_wait_fetch" \
        "${PKG_DIR}/bin/message_kv_demo" \
        "${PKG_DIR}/bin/kv_bench"
    do
        local glibcxx
        glibcxx="$(max_glibcxx "${file}")"
        record "${file##${PKG_DIR}/}: ${glibcxx:-none}"
        if [[ -n "${glibcxx}" ]]; then
            local version="${glibcxx#GLIBCXX_}"
            if version_gt "${version}" "${MAX_GLIBCXX}"; then
                echo "GLIBCXX regression in ${file}: ${glibcxx}" >&2
                exit 1
            fi
        fi
    done

}

rm -rf "${WORK_ROOT}" "${PKG_DIR}" "${REMOTE_TARBALL}"
mkdir -p "${WORK_ROOT}"
printf '' > "${REMOTE_PACKAGE_TXT}"

if [[ ! -x "${UCX_PREFIX}/bin/ucp_info" ]] || ! "${UCX_PREFIX}/bin/ucp_info" -v 2>/dev/null | grep -q '1.20.0'; then
    cd /tmp
    rm -rf /tmp/ucx-1.20.0
    tar xzf "${REMOTE_UCX_TARBALL}"
    cd /tmp/ucx-1.20.0
    if [[ ! -x ./configure ]]; then
        ./autogen.sh >/tmp/ucx-autogen-arm.log 2>&1
    fi
    export CFLAGS="${CFLAGS:-} -fPIC"
    export CXXFLAGS="${CXXFLAGS:-} -fPIC"
    ./contrib/configure-release \
        --prefix="${UCX_PREFIX}" \
        --with-go=no \
        --with-java=no \
        --enable-gtest=no \
        >/tmp/ucx-config-arm.log 2>&1
    make -j"$(nproc)" >/tmp/ucx-make-arm.log 2>&1
    make install >/tmp/ucx-install-arm.log 2>&1
fi

cd "${WORK_ROOT}"
tar xzf "${REMOTE_SRC_ARCHIVE}"
PKG_CONFIG_PATH="${UCX_PREFIX}/lib/pkgconfig:${UCX_PREFIX}/lib64/pkgconfig:${PKG_CONFIG_PATH:-}" \
cmake -S . -B build \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_CXX_COMPILER=g++-10 \
    -DUCX_ROOT="${UCX_PREFIX}" \
    -DZEROKV_BUILD_TESTS=OFF \
    -DZEROKV_BUILD_EXAMPLES=ON \
    -DZEROKV_BUILD_BENCHMARK=OFF \
    -DZEROKV_BUILD_PYTHON=OFF \
    -DCMAKE_SHARED_LINKER_FLAGS='-static-libstdc++ -static-libgcc' \
    -DCMAKE_EXE_LINKER_FLAGS='-static-libstdc++ -static-libgcc'
build_targets=(
    zerokv
    alps_kv_wrap
    ping_pong
    mpi_send_recv_bench
    send_recv
    rdma_put_get
    kv_demo
    kv_wait_fetch
    message_kv_demo
    kv_bench
    alps_kv_bench
)
cmake --build build --target "${build_targets[@]}" -j"$(nproc)"
cmake --install build --prefix "${PKG_DIR}"

stage_ucx_runtime
printf '%s\n' "${COMMIT_ID}" > "${PKG_DIR}/COMMIT_ID"
printf '%s\n' "${ARCH}" > "${PKG_DIR}/ARCH"
cp "${UCX_PREFIX}/bin/ucx_info" "${PKG_DIR}/bin/ucx_info"
if [[ -x "${UCX_PREFIX}/bin/ucp_info" ]]; then
    cp "${UCX_PREFIX}/bin/ucp_info" "${PKG_DIR}/bin/ucp_info"
fi

validate_pkg
tar -C /tmp -czf "${REMOTE_TARBALL}" "${PKG_DIR_NAME}"
tar -tzf "${REMOTE_TARBALL}" >/dev/null
record "tarball=$(basename "${REMOTE_TARBALL}")"
record "created_tarball=${REMOTE_TARBALL}"
EOF

vm_scp_to "${REMOTE_SCRIPT}" "/tmp/build_pkg_arm_remote.sh"

if ! vm_ssh \
    "COMMIT_ID='${COMMIT}' PACKAGE_TAG='${PACKAGE_TAG}' REMOTE_SRC_ARCHIVE='${REMOTE_SRC_ARCHIVE}' REMOTE_UCX_TARBALL='${REMOTE_UCX_TARBALL}' REMOTE_TARBALL='${REMOTE_TARBALL}' REMOTE_PACKAGE_TXT='${REMOTE_PACKAGE_TXT}' bash /tmp/build_pkg_arm_remote.sh" \
    |& tee "${BUILD_LOG}"; then
    echo "ARM remote build failed" >&2
    exit 1
fi

vm_scp_from "${REMOTE_PACKAGE_TXT}" "${PACKAGE_TXT}"
vm_scp_from "${REMOTE_TARBALL}" "${LOCAL_TARBALL}"
cp "${LOCAL_TARBALL}" "${LATEST_TARBALL}"

echo "Created: ${LOCAL_TARBALL}"
echo "Updated: ${LATEST_TARBALL}"
echo "Wrote: ${BUILD_LOG}"
echo "Recorded: ${PACKAGE_TXT}"
