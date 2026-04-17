#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
GIT_COMMON_DIR="$(git -C "${ROOT_DIR}" rev-parse --git-common-dir)"
COMMON_ROOT="$(cd "$(dirname "${GIT_COMMON_DIR}")" && pwd)"
COMMIT_ID="$(git -C "${ROOT_DIR}" rev-parse --short HEAD)"
ARCH="x86_64"
IMAGE="${IMAGE:-rockylinux:8}"
CONTAINER_NAME="${CONTAINER_NAME:-alps-pkg-x86-glibc228-${COMMIT_ID}}"
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
PKG_DIR_NAME="alps_kv_wrap_pkg-${ARCH}-${COMMIT_ID}"
OUTPUT_TARBALL="${OUT_DIR}/${PKG_DIR_NAME}.tar.gz"
LATEST_OUTPUT_TARBALL="${OUT_DIR}/alps_kv_wrap_pkg-${ARCH}.tar.gz"
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
git -C "${ROOT_DIR}" archive --format=tar.gz -o "${SRC_ARCHIVE}" HEAD

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
UCX_INFO_STATIC_BUILD="/tmp/ucx-1.20.0-tools-static"
UCX_INFO_STATIC_BIN="${UCX_INFO_STATIC_BUILD}/src/tools/info/ucx_info"
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

rm -rf "${BUILD_ROOT}" "${PKG_ROOT}" "${OUTPUT_TARBALL}" /tmp/ucx-1.20.0 "${UCX_INFO_STATIC_BUILD}"
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

cd /tmp
mkdir -p "${UCX_INFO_STATIC_BUILD}"
tar xzf "${CONTAINER_UCX}" -C "${UCX_INFO_STATIC_BUILD}" --strip-components=1
cd "${UCX_INFO_STATIC_BUILD}"
if [[ ! -x ./configure ]]; then
    ./autogen.sh >/tmp/ucx-autogen-static-tools-x86.log 2>&1
fi
./contrib/configure-release \
    --prefix="${UCX_INFO_STATIC_BUILD}/install" \
    --disable-shared \
    --enable-static \
    --with-go=no \
    --with-java=no \
    --enable-gtest=no \
    >/tmp/ucx-config-static-tools-x86.log 2>&1
make -j"$(nproc)" -k >/tmp/ucx-make-static-tools-x86.log 2>&1 || true
if [[ ! -x "${UCX_INFO_STATIC_BIN}" ]]; then
    echo "Failed to build static ucx_info" >&2
    tail -n 80 /tmp/ucx-make-static-tools-x86.log >&2 || true
    exit 1
fi

cd "${BUILD_ROOT}"
tar xzf "${CONTAINER_SRC}"
PKG_CONFIG_PATH="${UCX_PREFIX}/lib/pkgconfig:${UCX_PREFIX}/lib64/pkgconfig:${PKG_CONFIG_PATH:-}" \
cmake -S . -B build \
    -DCMAKE_BUILD_TYPE=Release \
    -DUCX_ROOT="${UCX_PREFIX}" \
    -DZEROKV_LINK_UCX_STATIC=ON \
    -DZEROKV_BUILD_TESTS=OFF \
    -DZEROKV_BUILD_BENCHMARK=OFF \
    -DZEROKV_BUILD_PYTHON=OFF \
    -DZEROKV_BUILD_EXAMPLES=ON \
    >/tmp/alps-x86-cmake.log 2>&1
cmake --build build --target zerokv alps_kv_wrap alps_kv_bench -j"$(nproc)" \
    >/tmp/alps-x86-build.log 2>&1
cmake --install build --prefix "${PKG_ROOT}" >/tmp/alps-x86-install.log 2>&1

cp "${PKG_ROOT}/share/doc/alps_kv_wrap/README.md" "${PKG_ROOT}/README.md"
printf '%s\n' "${COMMIT_ID}" > "${PKG_ROOT}/COMMIT_ID"
printf '%s\n' "${ARCH}" > "${PKG_ROOT}/ARCH"
cp "${UCX_INFO_STATIC_BIN}" "${PKG_ROOT}/bin/ucx_info"
if [[ -x "${UCX_PREFIX}/bin/ucp_info" ]]; then
    cp "${UCX_PREFIX}/bin/ucp_info" "${PKG_ROOT}/bin/ucp_info"
fi

echo "== packaged files =="
find "${PKG_ROOT}" -maxdepth 2 -type f | sort

check_no_dynamic_ucx() {
    local file="$1"
    if readelf -d "${file}" 2>/dev/null | grep -E 'Shared library: \[(libucp|libucs|libuct|libucm)\.so'; then
        echo "Dynamic UCX dependency found in ${file}" >&2
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
    "${PKG_ROOT}/bin/alps_kv_bench"; do
    echo "-- ${file}"
    ldd "${file}" || true
    check_no_dynamic_ucx "${file}"
done

echo "== GLIBC floor verification =="
for file in \
    "${PKG_ROOT}/lib/libalps_kv_wrap.so" \
    "${PKG_ROOT}/lib/libzerokv.so" \
    "${PKG_ROOT}/bin/alps_kv_bench"; do
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
