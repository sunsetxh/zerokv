#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
COMMIT_ID="$(git -C "${ROOT_DIR}" rev-parse --short HEAD)"
ARCH="x86_64"
HOST="${HOST:-192.168.3.6}"
USER_NAME="${USER_NAME:-wyc}"
CONTAINER="${CONTAINER:-compile}"
OUT_DIR="${ROOT_DIR}/out/packages"
SRC_OUT_DIR="${ROOT_DIR}/out/src"
SRC_ARCHIVE="${SRC_OUT_DIR}/zerokv-src-${ARCH}-${COMMIT_ID}.tar.gz"
UCX_TARBALL="${ROOT_DIR}/ucx-v1.20.0.tar.gz"
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
git -C "${ROOT_DIR}" archive --format=tar.gz -o "${SRC_ARCHIVE}" HEAD

ssh_remote "cat > '${REMOTE_SRC_ARCHIVE}'" < "${SRC_ARCHIVE}"
ssh_remote "cat > '${REMOTE_UCX_TARBALL}'" < "${UCX_TARBALL}"

cat > /tmp/build_alps_x86_remote.sh <<EOF
#!/usr/bin/env bash
set -euo pipefail

CONTAINER="${CONTAINER}"
COMMIT_ID="${COMMIT_ID}"
ARCH="${ARCH}"
REMOTE_SRC_ARCHIVE="${REMOTE_SRC_ARCHIVE}"
REMOTE_UCX_TARBALL="${REMOTE_UCX_TARBALL}"
CONTAINER_SRC="/tmp/zerokv-src.tar.gz"
CONTAINER_UCX="/tmp/ucx-v1.20.0.tar.gz"
CONTAINER_BUILD_ROOT="/tmp/alps-x86-build"
CONTAINER_UCX_PREFIX="/opt/ucx-1.20.0"
PKG_DIR_NAME="${PKG_DIR_NAME}"
CONTAINER_PKG_ROOT="/tmp/\${PKG_DIR_NAME}"
HOST_OUTPUT="/tmp/\${PKG_DIR_NAME}.tar.gz"

rm -f "\${HOST_OUTPUT}"
docker cp "\${REMOTE_SRC_ARCHIVE}" "\${CONTAINER}:\${CONTAINER_SRC}"
docker cp "\${REMOTE_UCX_TARBALL}" "\${CONTAINER}:\${CONTAINER_UCX}"

docker exec "\${CONTAINER}" bash -lc "
set -euo pipefail
export CC=gcc
export CXX=g++
BUILD_ROOT='\${CONTAINER_BUILD_ROOT}'
UCX_PREFIX='\${CONTAINER_UCX_PREFIX}'
PKG_ROOT='\${CONTAINER_PKG_ROOT}'
rm -rf \"\${BUILD_ROOT}\" \"\${PKG_ROOT}\" /tmp/ucx-1.20.0
mkdir -p \"\${BUILD_ROOT}\"
cd /tmp
if [ ! -x \"\${UCX_PREFIX}/bin/ucp_info\" ]; then
  tar xzf \${CONTAINER_UCX}
  cd ucx-1.20.0
  if [ ! -x ./configure ]; then
    ./autogen.sh >/tmp/ucx-autogen-x86.log 2>&1
  fi
  ./contrib/configure-release \\
    --prefix=\"\${UCX_PREFIX}\" \\
    --with-go=no \\
    --with-java=no \\
    --enable-gtest=no \\
    >/tmp/ucx-config-x86.log 2>&1
  make -j\"\$(nproc)\" >/tmp/ucx-make-x86.log 2>&1
  make install >/tmp/ucx-install-x86.log 2>&1
fi
cd \"\${BUILD_ROOT}\"
tar xzf \${CONTAINER_SRC}
PKG_CONFIG_PATH=\"\${UCX_PREFIX}/lib/pkgconfig:\${UCX_PREFIX}/lib64/pkgconfig:\${PKG_CONFIG_PATH:-}\" \\
cmake -S . -B build \\
  -DCMAKE_BUILD_TYPE=Release \\
  -DUCX_ROOT=\"\${UCX_PREFIX}\" \\
  -DZEROKV_BUILD_TESTS=OFF \\
  -DZEROKV_BUILD_BENCHMARK=OFF \\
  -DZEROKV_BUILD_PYTHON=OFF \\
  -DZEROKV_BUILD_EXAMPLES=ON >/tmp/alps-x86-cmake.log 2>&1
cmake --build build --target zerokv alps_kv_wrap alps_kv_bench -j\"\$(nproc)\" >/tmp/alps-x86-build.log 2>&1
cmake --install build --prefix \"\${PKG_ROOT}\" >/tmp/alps-x86-install.log 2>&1
cp \"\${PKG_ROOT}/share/doc/alps_kv_wrap/README.md\" \"\${PKG_ROOT}/README.md\"
printf '%s\n' \"\${COMMIT_ID}\" > \"\${PKG_ROOT}/COMMIT_ID\"
printf '%s\n' \"\${ARCH}\" > \"\${PKG_ROOT}/ARCH\"
cp \"\${UCX_PREFIX}/bin/ucx_info\" \"\${PKG_ROOT}/bin/ucx_info\"
cp \"\${UCX_PREFIX}/bin/ucp_info\" \"\${PKG_ROOT}/bin/ucp_info\"
"

docker exec "\${CONTAINER}" bash -lc "cd /tmp && tar -czf /tmp/\${PKG_DIR_NAME}.tar.gz \${PKG_DIR_NAME}"
docker cp "\${CONTAINER}:/tmp/\${PKG_DIR_NAME}.tar.gz" "\${HOST_OUTPUT}"
ls -lh "\${HOST_OUTPUT}"
EOF

chmod +x /tmp/build_alps_x86_remote.sh
ssh_remote "cat > '${REMOTE_SCRIPT}'" < /tmp/build_alps_x86_remote.sh
ssh_remote "bash '${REMOTE_SCRIPT}'"

ssh_remote "cat '/tmp/${PKG_DIR_NAME}.tar.gz'" > "${OUTPUT_TARBALL}"
cp "${OUTPUT_TARBALL}" "${LATEST_OUTPUT_TARBALL}"

echo "Created: ${OUTPUT_TARBALL}"
echo "Updated: ${LATEST_OUTPUT_TARBALL}"
