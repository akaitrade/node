#!/bin/bash
# CREDITS ALT-LEDGER 2030 - Single Node Starter (Linux)
# Simple development node for learning and testing

echo "========================================"
echo "  CREDITS ALT-LEDGER 2030 - Single Node"
echo "========================================"
echo

# Create necessary directories
mkdir -p data/dag_data
mkdir -p logs

echo "Starting single development node..."
echo
echo "Node Configuration:"
echo "- Type: DAG Blockchain Node"
echo "- Mode: Development (standalone)"
echo "- Port: 8080"
echo "- RPC Port: 8081"
echo "- Data Dir: ./data"
echo "- Config: ./config.toml"
echo

# Find the correct path to scripts
SCRIPTS_DIR="../../scripts"
if [ ! -f "$SCRIPTS_DIR/run_node.sh" ]; then
    SCRIPTS_DIR="../../../scripts"
fi
if [ ! -f "$SCRIPTS_DIR/run_node.sh" ]; then
    echo "ERROR: Cannot find scripts directory"
    echo "Please ensure you're running from the examples/single-node directory"
    exit 1
fi

echo "Starting node..."
echo "Press Ctrl+C to stop the node"
echo

# Start the node with this configuration
$SCRIPTS_DIR/run_node.sh --config config.toml --data-dir ./data --port 8080 --rpc-port 8081 --validator-id dev-node-1

echo
echo "Node stopped."