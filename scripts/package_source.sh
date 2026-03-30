#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
DATE_TAG="$(date +%Y%m%d)"
HEAD_SHA="$(git -C "${ROOT_DIR}" rev-parse --short HEAD)"
PKG_NAME="axon-src-${DATE_TAG}-${HEAD_SHA}"
OUT_DIR="${ROOT_DIR}"
INCLUDE_THIRD_PARTY=1

usage() {
    cat <<EOF
Usage: $(basename "$0") [options]

Options:
  --output-dir DIR         Output directory for the archive (default: repo root)
  --name NAME              Base package name without extension
  --exclude-third-party    Do not include third_party/
  -h, --help               Show this help

Default behavior:
  - package current HEAD source tree
  - include third_party/ if present
  - exclude .git, build/, and common local artifacts
EOF
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --output-dir)
            OUT_DIR="$2"
            shift 2
            ;;
        --name)
            PKG_NAME="$2"
            shift 2
            ;;
        --exclude-third-party)
            INCLUDE_THIRD_PARTY=0
            shift
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

mkdir -p "${OUT_DIR}"

TMP_DIR="$(mktemp -d)"
STAGE_DIR="${TMP_DIR}/${PKG_NAME}"
mkdir -p "${STAGE_DIR}"

cleanup() {
    rm -rf "${TMP_DIR}"
}
trap cleanup EXIT

rsync -a \
    --exclude='.git/' \
    --exclude='build/' \
    --exclude='.ccb/' \
    --exclude='.DS_Store' \
    --exclude='__pycache__/' \
    --exclude='*.pyc' \
    --exclude='*.pyo' \
    --exclude='*.tar.gz' \
    --exclude='scripts/qemu_rdma/.vm_work/' \
    "${ROOT_DIR}/" "${STAGE_DIR}/"

if [[ ${INCLUDE_THIRD_PARTY} -eq 0 ]]; then
    rm -rf "${STAGE_DIR}/third_party"
fi

ARCHIVE_PATH="${OUT_DIR}/${PKG_NAME}.tar.gz"
tar -C "${TMP_DIR}" -czf "${ARCHIVE_PATH}" "${PKG_NAME}"

echo "Created: ${ARCHIVE_PATH}"
