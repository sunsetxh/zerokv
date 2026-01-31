#!/bin/bash
# Quick start script for ZeroKV demo

set -e

RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m'

PROJECT_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

echo -e "${GREEN}ZeroKV Quick Start${NC}"
echo "=============================="
echo ""

# Check if build exists
if [ ! -f "${PROJECT_ROOT}/build/bin/simple_server" ]; then
    echo -e "${YELLOW}Build not found. Building project...${NC}"
    "${PROJECT_ROOT}/scripts/build.sh" --release
    echo ""
fi

# Start monitoring stack
echo -e "${GREEN}Starting monitoring stack...${NC}"
cd "${PROJECT_ROOT}/monitoring"
docker-compose up -d
echo "Monitoring stack started:"
echo "  - Prometheus: http://localhost:9090"
echo "  - Grafana: http://localhost:3000 (admin/admin)"
echo "  - Alertmanager: http://localhost:9093"
echo ""

# Wait for services to be ready
echo "Waiting for services to be ready..."
sleep 5

# Start server in background
echo -e "${GREEN}Starting ZeroKV Server...${NC}"
"${PROJECT_ROOT}/build/bin/simple_server" 0 0.0.0.0 50051 > /tmp/zerokv_server.log 2>&1 &
SERVER_PID=$!
echo "Server started (PID: ${SERVER_PID})"
echo "Server logs: /tmp/zerokv_server.log"
echo ""

# Wait for server to be ready
sleep 2

# Run client
echo -e "${GREEN}Running ZeroKV Client...${NC}"
"${PROJECT_ROOT}/build/bin/simple_client" 127.0.0.1 50051 1

echo ""
echo -e "${GREEN}=============================${NC}"
echo -e "${GREEN}Demo completed successfully!${NC}"
echo -e "${GREEN}=============================${NC}"
echo ""
echo "Next steps:"
echo "  1. View monitoring dashboard: http://localhost:3000"
echo "  2. Check server logs: tail -f /tmp/zerokv_server.log"
echo "  3. Run Python examples: python ${PROJECT_ROOT}/examples/python_example.py"
echo ""
echo "To stop the demo:"
echo "  kill ${SERVER_PID}"
echo "  cd ${PROJECT_ROOT}/monitoring && docker-compose down"
echo ""
