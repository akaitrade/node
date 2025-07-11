#!/bin/bash

#
# CREDITS ALT-LEDGER 2030 - Node Runner Script
#
# Convenience script to run the standalone blockchain node
# with various configuration options
#

set -e

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m' # No Color

# Logging functions
log_info() {
    echo -e "${BLUE}[INFO]${NC} $1"
}

log_success() {
    echo -e "${GREEN}[SUCCESS]${NC} $1"
}

log_warning() {
    echo -e "${YELLOW}[WARNING]${NC} $1"
}

log_error() {
    echo -e "${RED}[ERROR]${NC} $1"
}

# Project root
PROJECT_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${PROJECT_ROOT}/build"
CORE_DIR="${PROJECT_ROOT}/core"
AGENT_DIR="${PROJECT_ROOT}/agent/go"

# Default configuration
NODE_TYPE="dag"
DATA_DIR="${PROJECT_ROOT}/data"
PHASE="3"
PORT="8080"
RPC_PORT="8081"
VALIDATOR_ID="validator_1"
AGENT_NAME="agent_1"
CONFIG_FILE="${PROJECT_ROOT}/config.toml"

# Parse command line arguments
while [[ $# -gt 0 ]]; do
    case $1 in
        --type)
            NODE_TYPE="$2"
            shift 2
            ;;
        --phase)
            PHASE="$2"
            shift 2
            ;;
        --port)
            PORT="$2"
            shift 2
            ;;
        --rpc-port)
            RPC_PORT="$2"
            shift 2
            ;;
        --data-dir)
            DATA_DIR="$2"
            shift 2
            ;;
        --validator-id)
            VALIDATOR_ID="$2"
            shift 2
            ;;
        --agent-name)
            AGENT_NAME="$2"
            shift 2
            ;;
        --config)
            CONFIG_FILE="$2"
            shift 2
            ;;
        --help)
            cat << EOF
CREDITS ALT-LEDGER 2030 - Node Runner

Usage: $0 [OPTIONS]

Options:
  --type <dag|agent>       Node type (default: dag)
  --phase <1|2|3>          Migration phase (default: 3)
  --port <port>            Listen port (default: 8080)
  --rpc-port <port>        RPC port (default: 8081)
  --data-dir <path>        Data directory (default: ./data)
  --validator-id <id>      Validator ID (default: validator_1)
  --agent-name <name>      Agent name (default: agent_1)
  --config <file>          Configuration file (default: config.toml)
  --help                   Show this help message

Node Types:
  dag     - DAG engine blockchain node (Rust)
  agent   - Agent chain personal blockchain (Go)

Examples:
  $0                              # Run DAG node with defaults
  $0 --type agent                 # Run agent chain
  $0 --phase 1 --port 9000        # Run Phase 1 DAG node on port 9000
  $0 --type agent --agent-name alice # Run agent chain for 'alice'
EOF
            exit 0
            ;;
        *)
            log_error "Unknown option: $1"
            exit 1
            ;;
    esac
done

# Create necessary directories
mkdir -p "$DATA_DIR"
mkdir -p "${DATA_DIR}/logs"
mkdir -p "${DATA_DIR}/certs"

# Function to check if executables exist
check_executables() {
    local missing=false
    
    if [[ "$NODE_TYPE" == "dag" ]]; then
        if [[ ! -f "${BUILD_DIR}/Release/credits_alt_ledger.dll" ]]; then
            log_error "DAG engine library not found. Please run build.sh first."
            missing=true
        fi
    elif [[ "$NODE_TYPE" == "agent" ]]; then
        if [[ ! -f "${AGENT_DIR}/go.mod" ]]; then
            log_error "Agent Go module not found."
            missing=true
        fi
    fi
    
    if [[ "$missing" == true ]]; then
        log_error "Please build the project first using: ./scripts/build.sh"
        exit 1
    fi
}

# Function to run DAG node
run_dag_node() {
    log_info "Starting DAG Engine Node"
    log_info "Phase: $PHASE"
    log_info "Port: $PORT"
    log_info "RPC Port: $RPC_PORT"
    log_info "Data Directory: $DATA_DIR"
    log_info "Validator ID: $VALIDATOR_ID"
    
    cd "$CORE_DIR"
    
    # Check if we can use cargo
    if command -v cargo &> /dev/null; then
        log_info "Running with Cargo..."
        cargo run --release --bin credits-node -- \
            --data-dir "$DATA_DIR" \
            --port "$PORT" \
            --rpc-port "$RPC_PORT" \
            --phase "$PHASE" \
            --validator-id "$VALIDATOR_ID"
    else
        log_warning "Cargo not found. Checking for pre-built executable..."
        
        # Look for pre-built executable
        EXECUTABLE=""
        if [[ -f "${BUILD_DIR}/Release/credits-node.exe" ]]; then
            EXECUTABLE="${BUILD_DIR}/Release/credits-node.exe"
        elif [[ -f "${CORE_DIR}/target/release/credits-node.exe" ]]; then
            EXECUTABLE="${CORE_DIR}/target/release/credits-node.exe"
        elif [[ -f "${CORE_DIR}/target/release/credits-node" ]]; then
            EXECUTABLE="${CORE_DIR}/target/release/credits-node"
        fi
        
        if [[ -n "$EXECUTABLE" ]]; then
            log_info "Using pre-built executable: $EXECUTABLE"
            "$EXECUTABLE" \
                --data-dir "$DATA_DIR" \
                --port "$PORT" \
                --rpc-port "$RPC_PORT" \
                --phase "$PHASE" \
                --validator-id "$VALIDATOR_ID"
        else
            log_error "No executable found. Please build the project first."
            exit 1
        fi
    fi
}

# Function to run agent node
run_agent_node() {
    log_info "Starting Agent Chain Node"
    log_info "Agent Name: $AGENT_NAME"
    log_info "Data Directory: $DATA_DIR"
    
    cd "$AGENT_DIR"
    
    # Check if we can use go
    if command -v go &> /dev/null; then
        log_info "Running with Go..."
        go run main.go agent_chain.go cross_agent.go \
            --data-dir "$DATA_DIR" \
            --agent-name "$AGENT_NAME"
    else
        log_warning "Go not found. Checking for pre-built executable..."
        
        # Look for pre-built executable
        EXECUTABLE=""
        if [[ -f "${BUILD_DIR}/agent-cli.exe" ]]; then
            EXECUTABLE="${BUILD_DIR}/agent-cli.exe"
        elif [[ -f "${AGENT_DIR}/agent-cli.exe" ]]; then
            EXECUTABLE="${AGENT_DIR}/agent-cli.exe"
        elif [[ -f "${AGENT_DIR}/agent-cli" ]]; then
            EXECUTABLE="${AGENT_DIR}/agent-cli"
        fi
        
        if [[ -n "$EXECUTABLE" ]]; then
            log_info "Using pre-built executable: $EXECUTABLE"
            "$EXECUTABLE" \
                --data-dir "$DATA_DIR" \
                --agent-name "$AGENT_NAME"
        else
            log_error "No executable found. Please build the project first."
            exit 1
        fi
    fi
}

# Function to show node info
show_node_info() {
    log_info "CREDITS ALT-LEDGER 2030 - Node Information"
    echo "================================"
    echo "Node Type: $NODE_TYPE"
    echo "Phase: $PHASE"
    echo "Data Directory: $DATA_DIR"
    echo "Configuration: $CONFIG_FILE"
    echo "Project Root: $PROJECT_ROOT"
    echo "Build Directory: $BUILD_DIR"
    echo "================================"
}

# Function to setup environment
setup_environment() {
    log_info "Setting up environment..."
    
    # Create configuration if it doesn't exist
    if [[ ! -f "$CONFIG_FILE" ]]; then
        log_warning "Configuration file not found. Using defaults."
    fi
    
    # Set environment variables
    export CREDITS_DATA_DIR="$DATA_DIR"
    export CREDITS_PHASE="$PHASE"
    export CREDITS_LOG_LEVEL="info"
    export RUST_LOG="info"
    
    log_success "Environment setup complete"
}

# Function to cleanup on exit
cleanup() {
    log_info "Cleaning up..."
    # Kill any background processes if needed
    # In a real implementation, this would gracefully shut down the node
}

# Set up signal handlers
trap cleanup EXIT INT TERM

# Main execution
main() {
    show_node_info
    setup_environment
    check_executables
    
    case "$NODE_TYPE" in
        "dag")
            run_dag_node
            ;;
        "agent")
            run_agent_node
            ;;
        *)
            log_error "Invalid node type: $NODE_TYPE"
            log_error "Supported types: dag, agent"
            exit 1
            ;;
    esac
}

# Run main function
main