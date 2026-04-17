#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
GIT_COMMON_DIR="$(git -C "${ROOT_DIR}" rev-parse --git-common-dir)"
COMMON_ROOT="$(cd "$(dirname "${GIT_COMMON_DIR}")" && pwd)"
COMMIT_ID="$(git -C "${ROOT_DIR}" rev-parse --short HEAD)"
ARCH="x86_64"
HOST="${HOST:-192.168.3.6}"
USER_NAME="${USER_NAME:-wyc}"
CONTAINER="${CONTAINER:-compile}"
OUT_DIR="${ROOT_DIR}/out/packages"
SRC_OUT_DIR="${ROOT_DIR}/out/src"
SRC_ARCHIVE="${SRC_OUT_DIR}/zerokv-src-${ARCH}-${COMMIT_ID}.tar.gz"
UCX_TARBALL="${ROOT_DIR}/ucx-v1.20.0.tar.gz"
if [[ ! -f "${UCX_TARBALL}" ]]; then
    UCX_TARBALL="${COMMON_ROOT}/ucx-v1.20.0.tar.gz"
fi
REMOTE_SRC_ARCHIVE="/tmp/zerokv-src-${ARCH}-${COMMIT_ID}.tar.gz"
REMOTE_UCX_TARBALL="/tmp/ucx-v1.20.0.tar.gz"
REMOTE_SCRIPT="/tmp/build_alps_x86_remote.sh"
PKG_DIR_NAME="alps_kv_wrap_pkg-${ARCH}-${COMMIT_ID}"
OUTPUT_TARBALL="${OUT_DIR}/${PKG_DIR_NAME}.tar.gz"
LATEST_OUTPUT_TARBALL="${OUT_DIR}/alps_kv_wrap_pkg-${ARCH}.tar.gz"

ssh_remote() {
    ssh -o BatchMode=yes \
        -o StrictHostKeyChecking=no \
        -o UserKnownHostsFile=/dev/null \
        "${USER_NAME}@${HOST}" "$@"
}

mkdir -p "${OUT_DIR}" "${SRC_OUT_DIR}"
if [[ ! -f "${UCX_TARBALL}" ]]; then
    echo "UCX tarball not found: ${UCX_TARBALL}" >&2
    exit 1
fi
git -C "${ROOT_DIR}" archive --format=tar.gz -o "${SRC_ARCHIVE}" HEAD

ssh_remote "cat > '${REMOTE_SRC_ARCHIVE}'" < "${SRC_ARCHIVE}"
ssh_remote "cat > '${REMOTE_UCX_TARBALL}'" < "${UCX_TARBALL}"

cat > /tmp/build_alps_x86_remote.sh <<'EOF'
#!/usr/bin/env bash
set -euo pipefail

CONTAINER="__CONTAINER__"
COMMIT_ID="__COMMIT_ID__"
ARCH="__ARCH__"
REMOTE_SRC_ARCHIVE="__REMOTE_SRC_ARCHIVE__"
REMOTE_UCX_TARBALL="__REMOTE_UCX_TARBALL__"
CONTAINER_SRC="/tmp/zerokv-src.tar.gz"
CONTAINER_UCX="/tmp/ucx-v1.20.0.tar.gz"
CONTAINER_SCRIPT="/tmp/build_alps_inside_compile.sh"
CONTAINER_BUILD_ROOT="/tmp/alps-x86-build"
CONTAINER_UCX_PREFIX="/opt/ucx-1.20.0-static-pic"
PKG_DIR_NAME="__PKG_DIR_NAME__"
CONTAINER_PKG_ROOT="/tmp/${PKG_DIR_NAME}"
HOST_OUTPUT="/tmp/${PKG_DIR_NAME}.tar.gz"

rm -f "${HOST_OUTPUT}"
docker cp "${REMOTE_SRC_ARCHIVE}" "${CONTAINER}:${CONTAINER_SRC}"
docker cp "${REMOTE_UCX_TARBALL}" "${CONTAINER}:${CONTAINER_UCX}"

cat > /tmp/build_alps_inside_compile.sh <<EOS
#!/usr/bin/env bash
set -euo pipefail

export CC=gcc
export CXX=g++
BUILD_ROOT="${CONTAINER_BUILD_ROOT}"
UCX_PREFIX="${CONTAINER_UCX_PREFIX}"
PKG_ROOT="${CONTAINER_PKG_ROOT}"
CONTAINER_SRC="${CONTAINER_SRC}"
CONTAINER_UCX="${CONTAINER_UCX}"
COMMIT_ID="${COMMIT_ID}"
ARCH="${ARCH}"

rm -rf "${CONTAINER_BUILD_ROOT}" "${CONTAINER_PKG_ROOT}" /tmp/ucx-1.20.0
mkdir -p "${CONTAINER_BUILD_ROOT}"
cd /tmp
if [ ! -x "${CONTAINER_UCX_PREFIX}/bin/ucp_info" ]; then
  tar xzf "${CONTAINER_UCX}"
  cd ucx-1.20.0
  if [ ! -x ./configure ]; then
    ./autogen.sh >/tmp/ucx-autogen-x86.log 2>&1
  fi
  export CFLAGS="${CFLAGS:-} -fPIC"
  export CXXFLAGS="${CXXFLAGS:-} -fPIC"
  ./contrib/configure-release \
    --prefix="${CONTAINER_UCX_PREFIX}" \
    --with-go=no \
    --with-java=no \
    --enable-gtest=no \
    >/tmp/ucx-config-x86.log 2>&1
  make -j"$(nproc)" >/tmp/ucx-make-x86.log 2>&1
  make install >/tmp/ucx-install-x86.log 2>&1
fi
cd "${CONTAINER_BUILD_ROOT}"
tar xzf "${CONTAINER_SRC}"
PKG_CONFIG_PATH="${CONTAINER_UCX_PREFIX}/lib/pkgconfig:${CONTAINER_UCX_PREFIX}/lib64/pkgconfig:${PKG_CONFIG_PATH:-}" \
cmake -S . -B build \
  -DCMAKE_BUILD_TYPE=Release \
  -DUCX_ROOT="${CONTAINER_UCX_PREFIX}" \
  -DZEROKV_LINK_UCX_STATIC=ON \
  -DZEROKV_BUILD_TESTS=OFF \
  -DZEROKV_BUILD_BENCHMARK=OFF \
  -DZEROKV_BUILD_PYTHON=OFF \
  -DZEROKV_BUILD_EXAMPLES=ON >/tmp/alps-x86-cmake.log 2>&1
cmake --build build --target zerokv alps_kv_wrap alps_kv_bench -j"$(nproc)" >/tmp/alps-x86-build.log 2>&1
cmake --install build --prefix "${CONTAINER_PKG_ROOT}" >/tmp/alps-x86-install.log 2>&1
cp "${CONTAINER_PKG_ROOT}/share/doc/alps_kv_wrap/README.md" "${CONTAINER_PKG_ROOT}/README.md"
printf '%s\n' "${COMMIT_ID}" > "${CONTAINER_PKG_ROOT}/COMMIT_ID"
printf '%s\n' "${ARCH}" > "${CONTAINER_PKG_ROOT}/ARCH"
cp "${CONTAINER_UCX_PREFIX}/bin/ucx_info" "${CONTAINER_PKG_ROOT}/bin/ucx_info"
if [ -x "${CONTAINER_UCX_PREFIX}/bin/ucp_info" ]; then
  cp "${CONTAINER_UCX_PREFIX}/bin/ucp_info" "${CONTAINER_PKG_ROOT}/bin/ucp_info"
fi
EOS

docker cp /tmp/build_alps_inside_compile.sh "${CONTAINER}:${CONTAINER_SCRIPT}"
docker exec "${CONTAINER}" bash "${CONTAINER_SCRIPT}"

docker exec "${CONTAINER}" bash -lc "cd /tmp && tar -czf /tmp/${PKG_DIR_NAME}.tar.gz ${PKG_DIR_NAME}"
docker cp "${CONTAINER}:/tmp/${PKG_DIR_NAME}.tar.gz" "${HOST_OUTPUT}"
ls -lh "${HOST_OUTPUT}"
EOF

perl -0pi -e "s|__CONTAINER__|${CONTAINER}|g; s|__COMMIT_ID__|${COMMIT_ID}|g; s|__ARCH__|${ARCH}|g; s|__REMOTE_SRC_ARCHIVE__|${REMOTE_SRC_ARCHIVE}|g; s|__REMOTE_UCX_TARBALL__|${REMOTE_UCX_TARBALL}|g; s|__PKG_DIR_NAME__|${PKG_DIR_NAME}|g" /tmp/build_alps_x86_remote.sh

chmod +x /tmp/build_alps_x86_remote.sh
ssh_remote "cat > '${REMOTE_SCRIPT}'" < /tmp/build_alps_x86_remote.sh
ssh_remote "bash '${REMOTE_SCRIPT}'"

ssh_remote "cat '/tmp/${PKG_DIR_NAME}.tar.gz'" > "${OUTPUT_TARBALL}"
cp "${OUTPUT_TARBALL}" "${LATEST_OUTPUT_TARBALL}"

echo "Created: ${OUTPUT_TARBALL}"
echo "Updated: ${LATEST_OUTPUT_TARBALL}"
