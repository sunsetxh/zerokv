#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
GIT_COMMON_DIR="$(git -C "${ROOT_DIR}" rev-parse --git-common-dir)"
COMMON_ROOT="$(cd "$(dirname "${GIT_COMMON_DIR}")" && pwd)"
if [[ -n "${TARGET_COMMIT:-}" ]]; then
    if [[ -z "${TARGET_COMMIT//[[:space:]]/}" ]] || [[ "${TARGET_COMMIT}" =~ [[:space:]] ]]; then
        echo "TARGET_COMMIT must be non-empty and contain no whitespace" >&2
        exit 1
    fi
    COMMIT_ID="$(git -C "${ROOT_DIR}" rev-parse "${TARGET_COMMIT}")"
    PACKAGE_TAG="$(git -C "${ROOT_DIR}" rev-parse --short "${TARGET_COMMIT}")"
else
    COMMIT_ID="$(git -C "${ROOT_DIR}" rev-parse HEAD)"
    PACKAGE_TAG="$(git -C "${ROOT_DIR}" rev-parse --short HEAD)"
fi
ARCH="x86_64"
IMAGE="${IMAGE:-rockylinux:8}"
CONTAINER_NAME="${CONTAINER_NAME:-zerokv-pkg-x86-glibc228-${PACKAGE_TAG}}"
KEEP_CONTAINER="${KEEP_CONTAINER:-0}"
OUT_DIR="${ROOT_DIR}/out/packages"
SRC_OUT_DIR="${ROOT_DIR}/out/src"
SRC_ARCHIVE="${SRC_OUT_DIR}/zerokv-src-${ARCH}-${COMMIT_ID}.tar.gz"
UCX_TARBALL="${ROOT_DIR}/ucx-v1.20.0.tar.gz"
if [[ ! -f "${UCX_TARBALL}" ]]; then
    UCX_TARBALL="${COMMON_ROOT}/ucx-v1.20.0.tar.gz"
fi
CONTAINER_SRC="/tmp/zerokv-src-${ARCH}-${COMMIT_ID}.tar.gz"
CONTAINER_UCX="/tmp/ucx-v1.20.0.tar.gz"
CONTAINER_SCRIPT="/tmp/build_alps_inside_glibc228.sh"
PKG_DIR_NAME="zerokv-${ARCH}-${PACKAGE_TAG}"
OUTPUT_TARBALL="${OUT_DIR}/${PKG_DIR_NAME}.tar.gz"
LATEST_OUTPUT_TARBALL="${OUT_DIR}/zerokv-${ARCH}.tar.gz"
LOCAL_SCRIPT="$(mktemp)"

cleanup() {
    rm -f "${LOCAL_SCRIPT}"
    if [[ "${KEEP_CONTAINER}" != "1" ]] && docker container inspect "${CONTAINER_NAME}" >/dev/null 2>&1; then
        docker rm -f "${CONTAINER_NAME}" >/dev/null 2>&1 || true
    fi
}
trap cleanup EXIT

container_exec() {
    docker exec "${CONTAINER_NAME}" bash -lc "$*"
}

if [[ ! -f "${UCX_TARBALL}" ]]; then
    echo "UCX tarball not found: ${UCX_TARBALL}" >&2
    exit 1
fi

mkdir -p "${OUT_DIR}" "${SRC_OUT_DIR}"
if [[ -n "${TARGET_COMMIT:-}" ]]; then
    git -C "${ROOT_DIR}" archive --format=tar.gz -o "${SRC_ARCHIVE}" "${TARGET_COMMIT}"
else
    git -C "${ROOT_DIR}" archive --format=tar.gz -o "${SRC_ARCHIVE}" HEAD
fi

if ! docker image inspect "${IMAGE}" >/dev/null 2>&1; then
    docker pull "${IMAGE}" >/dev/null
fi

if docker container inspect "${CONTAINER_NAME}" >/dev/null 2>&1; then
    docker rm -f "${CONTAINER_NAME}" >/dev/null
fi

docker create --name "${CONTAINER_NAME}" "${IMAGE}" bash -lc "sleep infinity" >/dev/null
docker start "${CONTAINER_NAME}" >/dev/null
docker cp "${SRC_ARCHIVE}" "${CONTAINER_NAME}:${CONTAINER_SRC}"
docker cp "${UCX_TARBALL}" "${CONTAINER_NAME}:${CONTAINER_UCX}"

cat > "${LOCAL_SCRIPT}" <<'EOF'
#!/usr/bin/env bash
set -euo pipefail

CONTAINER_SRC="__CONTAINER_SRC__"
CONTAINER_UCX="__CONTAINER_UCX__"
PKG_DIR_NAME="__PKG_DIR_NAME__"
COMMIT_ID="__COMMIT_ID__"
ARCH="__ARCH__"
UCX_PREFIX="/opt/ucx-1.20.0-static-pic"
BUILD_ROOT="/tmp/alps-x86-build"
PKG_ROOT="/tmp/${PKG_DIR_NAME}"
OUTPUT_TARBALL="/tmp/${PKG_DIR_NAME}.tar.gz"
UCX_VERSION="1.20.0"
MAX_GLIBC="2.28"

version_gt() {
    local lhs="$1"
    local rhs="$2"
    [[ "$(printf '%s\n%s\n' "${lhs}" "${rhs}" | sort -V | tail -n 1)" == "${lhs}" && "${lhs}" != "${rhs}" ]]
}

glibc_version="$(ldd --version | sed -n '1s/.* //p')"
echo "== build container glibc =="
echo "${glibc_version}"
if [[ "${glibc_version}" != "${MAX_GLIBC}" ]]; then
    echo "Expected glibc ${MAX_GLIBC}, got ${glibc_version}" >&2
    exit 1
fi

dnf install -y epel-release >/tmp/alps-dnf-epel.log 2>&1 || true
dnf install -y \
    gcc-toolset-12-gcc \
    gcc-toolset-12-gcc-c++ \
    gcc-toolset-12-binutils \
    cmake \
    make \
    pkgconfig \
    git \
    tar \
    gzip \
    which \
    file \
    perl \
    python3 \
    autoconf \
    automake \
    libtool \
    numactl-devel \
    libnl3-devel \
    rdma-core-devel \
    >/tmp/alps-dnf-install.log 2>&1

source /opt/rh/gcc-toolset-12/enable
export CC=gcc
export CXX=g++

copy_ucx_runtime() {
    mkdir -p "${PKG_ROOT}/lib" "${PKG_ROOT}/lib/ucx"
    shopt -s nullglob
    local found_shared=0
    local found_modules=0
    for dir in "${UCX_PREFIX}/lib" "${UCX_PREFIX}/lib64"; do
        [[ -d "${dir}" ]] || continue
        for lib in "${dir}"/libuc*.so*; do
            cp -a "${lib}" "${PKG_ROOT}/lib/"
            found_shared=1
        done
        if [[ -d "${dir}/ucx" ]]; then
            cp -a "${dir}/ucx/." "${PKG_ROOT}/lib/ucx/"
            found_modules=1
        fi
    done
    shopt -u nullglob

    if [[ "${found_shared}" != "1" ]]; then
        echo "Failed to stage UCX shared libraries from ${UCX_PREFIX}" >&2
        exit 1
    fi
    if [[ "${found_modules}" != "1" ]]; then
        echo "Failed to stage UCX provider modules from ${UCX_PREFIX}" >&2
        exit 1
    fi
}

rm -rf "${BUILD_ROOT}" "${PKG_ROOT}" "${OUTPUT_TARBALL}" /tmp/ucx-1.20.0
mkdir -p "${BUILD_ROOT}"

if [[ ! -x "${UCX_PREFIX}/bin/ucp_info" ]] || \
   ! LD_LIBRARY_PATH="${UCX_PREFIX}/lib:${UCX_PREFIX}/lib64:${LD_LIBRARY_PATH:-}" \
        "${UCX_PREFIX}/bin/ucp_info" -v 2>/dev/null | grep -q "${UCX_VERSION}"; then
    cd /tmp
    tar xzf "${CONTAINER_UCX}"
    cd /tmp/ucx-1.20.0
    if [[ ! -x ./configure ]]; then
        ./autogen.sh >/tmp/ucx-autogen-x86.log 2>&1
    fi
    export CFLAGS="${CFLAGS:-} -fPIC"
    export CXXFLAGS="${CXXFLAGS:-} -fPIC"
    ./contrib/configure-release \
        --prefix="${UCX_PREFIX}" \
        --with-go=no \
        --with-java=no \
        --enable-gtest=no \
        >/tmp/ucx-config-x86.log 2>&1
    make -j"$(nproc)" >/tmp/ucx-make-x86.log 2>&1
    make install >/tmp/ucx-install-x86.log 2>&1
fi

cd "${BUILD_ROOT}"
tar xzf "${CONTAINER_SRC}"
PKG_CONFIG_PATH="${UCX_PREFIX}/lib/pkgconfig:${UCX_PREFIX}/lib64/pkgconfig:${PKG_CONFIG_PATH:-}" \
cmake -S . -B build \
    -DCMAKE_BUILD_TYPE=Release \
    -DUCX_ROOT="${UCX_PREFIX}" \
    -DZEROKV_BUILD_TESTS=OFF \
    -DZEROKV_BUILD_BENCHMARK=OFF \
    -DZEROKV_BUILD_PYTHON=OFF \
    -DZEROKV_BUILD_EXAMPLES=ON \
    >/tmp/alps-x86-cmake.log 2>&1
cmake --build build --target \
    zerokv \
    alps_kv_wrap \
    ping_pong \
    send_recv \
    rdma_put_get \
    kv_demo \
    kv_wait_fetch \
    message_kv_demo \
    kv_bench \
    alps_kv_bench \
    -j"$(nproc)" \
    >/tmp/alps-x86-build.log 2>&1
cmake --install build --prefix "${PKG_ROOT}" >/tmp/alps-x86-install.log 2>&1

copy_ucx_runtime
printf '%s\n' "${COMMIT_ID}" > "${PKG_ROOT}/COMMIT_ID"
printf '%s\n' "${ARCH}" > "${PKG_ROOT}/ARCH"
cp "${UCX_PREFIX}/bin/ucx_info" "${PKG_ROOT}/bin/ucx_info"
if [[ -x "${UCX_PREFIX}/bin/ucp_info" ]]; then
    cp "${UCX_PREFIX}/bin/ucp_info" "${PKG_ROOT}/bin/ucp_info"
fi

echo "== packaged files =="
find "${PKG_ROOT}" -maxdepth 2 -type f | sort

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
    lib/libzerokv.so; do
    [[ -e "${PKG_ROOT}/${rel}" ]] || {
        echo "Missing required package file: ${rel}" >&2
        exit 1
    }
done

check_no_dynamic_ucx() {
    local file="$1"
    if readelf -d "${file}" 2>/dev/null | grep -E 'Shared library: \[(libucp|libucs|libuct|libucm)\.so'; then
        return 0
    fi
    echo "Expected packaged UCX dependency missing in ${file}" >&2
    exit 1
}

check_no_missing_runtime_deps() {
    local file="$1"
    local ldd_output
    ldd_output="$(
        LD_LIBRARY_PATH="${PKG_ROOT}/lib:${LD_LIBRARY_PATH:-}" ldd "${file}" 2>&1
    )" || true
    echo "${ldd_output}"
    if grep -q 'not found' <<<"${ldd_output}"; then
        echo "Missing runtime dependency in ${file}" >&2
        exit 1
    fi
}

check_glibc_floor() {
    local file="$1"
    local max_glibc
    max_glibc="$(objdump -T "${file}" 2>/dev/null | sed -n 's/.*\(GLIBC_[0-9.]*\).*/\1/p' | sort -Vu | tail -n 1)"
    echo "${file}: ${max_glibc:-none}"
    if [[ -n "${max_glibc}" ]] && version_gt "${max_glibc#GLIBC_}" "${MAX_GLIBC}"; then
        echo "GLIBC requirement too new in ${file}: ${max_glibc}" >&2
        exit 1
    fi
}

echo "== ELF dependency verification =="
for file in \
    "${PKG_ROOT}/lib/libalps_kv_wrap.so" \
    "${PKG_ROOT}/lib/libzerokv.so" \
    "${PKG_ROOT}/bin/alps_kv_bench" \
    "${PKG_ROOT}/bin/ping_pong" \
    "${PKG_ROOT}/bin/send_recv" \
    "${PKG_ROOT}/bin/rdma_put_get" \
    "${PKG_ROOT}/bin/kv_demo" \
    "${PKG_ROOT}/bin/kv_wait_fetch" \
    "${PKG_ROOT}/bin/message_kv_demo" \
    "${PKG_ROOT}/bin/kv_bench"; do
    echo "-- ${file}"
    check_no_missing_runtime_deps "${file}"
done
check_no_dynamic_ucx "${PKG_ROOT}/lib/libzerokv.so"

echo "== UCX runtime verification =="
find "${PKG_ROOT}/lib/ucx" -maxdepth 1 -type f | sort | sed -n '1,40p'
UCX_MODULE_DIR="${PKG_ROOT}/lib/ucx" \
LD_LIBRARY_PATH="${PKG_ROOT}/lib:${LD_LIBRARY_PATH:-}" \
    "${PKG_ROOT}/bin/ucx_info" -d | grep -E 'rc_verbs|rc_mlx5|mlx5|rdmacm' >/tmp/alps-x86-ucx-runtime.log
sed -n '1,20p' /tmp/alps-x86-ucx-runtime.log

echo "== GLIBC floor verification =="
for file in \
    "${PKG_ROOT}/lib/libalps_kv_wrap.so" \
    "${PKG_ROOT}/lib/libzerokv.so" \
    "${PKG_ROOT}/bin/alps_kv_bench" \
    "${PKG_ROOT}/bin/ping_pong" \
    "${PKG_ROOT}/bin/send_recv" \
    "${PKG_ROOT}/bin/rdma_put_get" \
    "${PKG_ROOT}/bin/kv_demo" \
    "${PKG_ROOT}/bin/kv_wait_fetch" \
    "${PKG_ROOT}/bin/message_kv_demo" \
    "${PKG_ROOT}/bin/kv_bench"; do
    check_glibc_floor "${file}"
done

tar -C /tmp -czf "${OUTPUT_TARBALL}" "${PKG_DIR_NAME}"
ls -lh "${OUTPUT_TARBALL}"
EOF

perl -0pi -e \
    "s|__CONTAINER_SRC__|${CONTAINER_SRC}|g; s|__CONTAINER_UCX__|${CONTAINER_UCX}|g; s|__PKG_DIR_NAME__|${PKG_DIR_NAME}|g; s|__COMMIT_ID__|${COMMIT_ID}|g; s|__ARCH__|${ARCH}|g" \
    "${LOCAL_SCRIPT}"

chmod +x "${LOCAL_SCRIPT}"
docker cp "${LOCAL_SCRIPT}" "${CONTAINER_NAME}:${CONTAINER_SCRIPT}"
container_exec "bash '${CONTAINER_SCRIPT}'"

docker cp "${CONTAINER_NAME}:/tmp/${PKG_DIR_NAME}.tar.gz" "${OUTPUT_TARBALL}"
cp "${OUTPUT_TARBALL}" "${LATEST_OUTPUT_TARBALL}"

echo "Created: ${OUTPUT_TARBALL}"
echo "Updated: ${LATEST_OUTPUT_TARBALL}"
