#!/bin/bash
# Build script for ZeroKV

set -e

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

# Default options
BUILD_TYPE="Release"
BUILD_TESTS="ON"
BUILD_EXAMPLES="ON"
BUILD_PYTHON="ON"
USE_REAL_NPU="OFF"
CLEAN_BUILD="OFF"
NUM_JOBS=$(nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 4)

# Parse command line arguments
while [[ $# -gt 0 ]]; do
    case $1 in
        --debug)
            BUILD_TYPE="Debug"
            shift
            ;;
        --release)
            BUILD_TYPE="Release"
            shift
            ;;
        --no-tests)
            BUILD_TESTS="OFF"
            shift
            ;;
        --no-examples)
            BUILD_EXAMPLES="OFF"
            shift
            ;;
        --no-python)
            BUILD_PYTHON="OFF"
            shift
            ;;
        --use-npu)
            USE_REAL_NPU="ON"
            shift
            ;;
        --clean)
            CLEAN_BUILD="ON"
            shift
            ;;
        -j*)
            NUM_JOBS="${1#-j}"
            shift
            ;;
        --help)
            echo "Usage: $0 [options]"
            echo ""
            echo "Options:"
            echo "  --debug          Build in Debug mode (default: Release)"
            echo "  --release        Build in Release mode"
            echo "  --no-tests       Don't build tests"
            echo "  --no-examples    Don't build examples"
            echo "  --no-python      Don't build Python bindings"
            echo "  --use-npu        Use real NPU hardware (requires ACL)"
            echo "  --clean          Clean build directory before building"
            echo "  -jN              Use N parallel jobs (default: $(nproc))"
            echo "  --help           Show this help message"
            exit 0
            ;;
        *)
            echo -e "${RED}Unknown option: $1${NC}"
            echo "Use --help for usage information"
            exit 1
            ;;
    esac
done

# Project root directory
PROJECT_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${PROJECT_ROOT}/build"

echo -e "${GREEN}ZeroKV Build Script${NC}"
echo "================================"
echo "Project root: ${PROJECT_ROOT}"
echo "Build type: ${BUILD_TYPE}"
echo "Build tests: ${BUILD_TESTS}"
echo "Build examples: ${BUILD_EXAMPLES}"
echo "Build Python: ${BUILD_PYTHON}"
echo "Use real NPU: ${USE_REAL_NPU}"
echo "Parallel jobs: ${NUM_JOBS}"
echo ""

# Clean build directory if requested
if [ "${CLEAN_BUILD}" == "ON" ]; then
    echo -e "${YELLOW}Cleaning build directory...${NC}"
    rm -rf "${BUILD_DIR}"
fi

# Create build directory
mkdir -p "${BUILD_DIR}"
cd "${BUILD_DIR}"

# Run CMake
echo -e "${GREEN}Running CMake...${NC}"
cmake .. \
    -DCMAKE_BUILD_TYPE="${BUILD_TYPE}" \
    -DBUILD_TESTS="${BUILD_TESTS}" \
    -DBUILD_EXAMPLES="${BUILD_EXAMPLES}" \
    -DBUILD_PYTHON="${BUILD_PYTHON}" \
    -DUSE_REAL_NPU="${USE_REAL_NPU}"

# Build
echo -e "${GREEN}Building...${NC}"
cmake --build . -- -j"${NUM_JOBS}"

# Run tests if built
if [ "${BUILD_TESTS}" == "ON" ]; then
    echo ""
    echo -e "${GREEN}Running tests...${NC}"
    ctest --output-on-failure -j"${NUM_JOBS}" || true
fi

# Summary
echo ""
echo -e "${GREEN}================================${NC}"
echo -e "${GREEN}Build completed successfully!${NC}"
echo -e "${GREEN}================================${NC}"
echo ""
echo "Binaries are in: ${BUILD_DIR}/bin"
echo "Libraries are in: ${BUILD_DIR}/lib"
echo ""

if [ "${BUILD_EXAMPLES}" == "ON" ]; then
    echo "To run examples:"
    echo "  ${BUILD_DIR}/bin/simple_server"
    echo "  ${BUILD_DIR}/bin/simple_client"
    echo ""
fi

if [ "${BUILD_PYTHON}" == "ON" ]; then
    echo "To install Python package:"
    echo "  cd ${PROJECT_ROOT}/python"
    echo "  pip install -e ."
    echo ""
fi

echo "To run monitoring stack:"
echo "  cd ${PROJECT_ROOT}/monitoring"
echo "  docker-compose up -d"
echo "  Open http://localhost:3000 for Grafana"
echo ""
