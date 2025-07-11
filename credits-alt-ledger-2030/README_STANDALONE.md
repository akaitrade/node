# CREDITS ALT-LEDGER 2030 - Standalone Usage Guide

## Overview

This document explains how to use the standalone blockchain executables that have been added to the CREDITS ALT-LEDGER 2030 project. The standalone executables allow you to test and interact with the blockchain technology without requiring integration with existing CREDITS infrastructure.

## Available Standalone Components

### 1. **DAG Engine Node** (`credits-node`)
- **Location**: `core/src/main.rs`
- **Language**: Rust
- **Purpose**: Full-featured blockchain node with DAG engine, consensus, and networking
- **Features**:
  - Interactive CLI with blockchain commands
  - DAG vertex creation and management
  - Consensus simulation
  - Transaction processing
  - Statistics and monitoring
  - RPC server (simulated)

### 2. **Agent Chain CLI** (`agent-cli`)
- **Location**: `agent/go/main.go`
- **Language**: Go
- **Purpose**: Personal blockchain management and cross-agent coordination
- **Features**:
  - Personal agent chain management
  - Transaction creation and commit
  - Balance and state management
  - CNS (Credits Name Service) integration
  - Export functionality (JSON/CSV)
  - Activity simulation

## Building the Standalone Executables

### Prerequisites
- **Rust** 1.70+ (for DAG engine)
- **Go** 1.19+ (for agent chain)
- **CMake** 3.15+ (for C++ components)

### Build Commands

```bash
# Build everything (including standalone executables)
./scripts/build.sh

# Build only Rust components
cd core && cargo build --release

# Build only Go components
cd agent/go && go build -o agent-cli main.go agent_chain.go cross_agent.go
```

## Running the Standalone Nodes

### Quick Start

```bash
# Run DAG node with defaults
./scripts/run_node.sh

# Run agent chain
./scripts/run_node.sh --type agent

# Windows users
.\scripts\run_node.bat
```

### DAG Engine Node

```bash
# Basic usage
./scripts/run_node.sh --type dag

# Custom configuration
./scripts/run_node.sh --type dag --phase 1 --port 9000 --data-dir ./my_data

# Direct execution (if Rust is installed)
cd core && cargo run --release --bin credits-node -- --help
```

#### DAG Node CLI Commands

Once the DAG node is running, you can use these interactive commands:

```
credits-node> help                    # Show all commands
credits-node> status                  # Show node status
credits-node> stats                   # Show blockchain statistics
credits-node> create "Hello World"    # Create a new DAG vertex
credits-node> get <vertex-hash>       # Get vertex by hash
credits-node> balance <address>       # Get balance (simulated)
credits-node> transfer <from> <to> <amount>  # Transfer funds
credits-node> start-mining            # Start mining (simulated)
credits-node> stop-mining             # Stop mining (simulated)
credits-node> quit                    # Exit the node
```

### Agent Chain CLI

```bash
# Basic usage
./scripts/run_node.sh --type agent

# Custom agent name
./scripts/run_node.sh --type agent --agent-name alice

# Direct execution (if Go is installed)
cd agent/go && go run main.go agent_chain.go cross_agent.go --agent-name alice
```

#### Agent Chain CLI Commands

```
agent> help                           # Show all commands
agent> status                         # Show agent and chain status
agent> balance                        # Show current balance
agent> create-tx <to> <operation> [value]  # Create transaction
agent> commit                         # Commit pending transactions
agent> transfer <to> <amount>         # Transfer value to another agent
agent> set-state <key> <value>        # Set custom state data
agent> get-state <key>                # Get custom state data
agent> register-cns <namespace> <name> <relay>  # Register CNS name
agent> resolve-cns <namespace> <name>  # Resolve CNS name
agent> history                        # Show transaction history
agent> simulate <count>               # Simulate random activity
agent> export <format>                # Export state (json/csv)
agent> quit                           # Exit the CLI
```

## Configuration

### Configuration File (`config.toml`)

The standalone nodes can be configured using the `config.toml` file:

```toml
[node]
validator_id = "validator_1"
listen_port = 8080
rpc_port = 8081
phase = 3
data_dir = "./data"

[dag_engine]
storage_backend = "sled"
cache_size = 10000

[consensus]
voting_threshold = 0.67
max_validators = 100

[agent]
agent_name = "agent_1"
auto_commit = false
cns_enabled = true
```

### Command Line Options

Both nodes support command line configuration:

```bash
# DAG Node
credits-node --data-dir ./data --port 8080 --phase 3 --validator-id validator_1

# Agent Chain
agent-cli --data-dir ./agent_data --agent-name alice --auto-commit
```

## Usage Examples

### Example 1: Simple DAG Operations

```bash
# Start DAG node
./scripts/run_node.sh --type dag

# In the interactive CLI:
credits-node> create "Genesis block"
credits-node> create "Second transaction"
credits-node> stats
credits-node> status
```

### Example 2: Agent Chain Operations

```bash
# Start agent chain
./scripts/run_node.sh --type agent --agent-name alice

# In the interactive CLI:
agent> status
agent> create-tx bob transfer 100
agent> commit
agent> balance
agent> simulate 5
agent> export json
```

### Example 3: Multi-Agent Setup

```bash
# Terminal 1: Start Alice's agent
./scripts/run_node.sh --type agent --agent-name alice --data-dir ./alice_data

# Terminal 2: Start Bob's agent
./scripts/run_node.sh --type agent --agent-name bob --data-dir ./bob_data

# In Alice's CLI:
agent> transfer bob 50
agent> commit

# In Bob's CLI:
agent> balance  # (simulated balance update)
```

## Migration Phases

The standalone executables support all three migration phases:

### Phase 1 (Parallel Processing)
```bash
./scripts/run_node.sh --type dag --phase 1
```
- Parallel processing within existing BoTs
- Target: 10k+ TPS
- Compatible with existing CREDITS infrastructure

### Phase 2 (Hybrid DAG-Block)
```bash
./scripts/run_node.sh --type dag --phase 2
```
- Hybrid DAG-Block architecture
- Agent chains integration
- Target: 50k+ TPS

### Phase 3 (Pure DAG)
```bash
./scripts/run_node.sh --type dag --phase 3
```
- Pure DAG architecture
- Unbounded horizontal scaling
- Target: Unlimited TPS

## Debugging and Monitoring

### Log Files
- DAG Node: `./data/logs/node.log`
- Agent Chain: `./agent_data/logs/agent.log`

### Environment Variables
```bash
export RUST_LOG=debug        # Enable debug logging for Rust
export CREDITS_LOG_LEVEL=debug  # Enable debug logging
export CREDITS_DATA_DIR=./data   # Set data directory
```

### Statistics and Monitoring
```bash
# Get node statistics
credits-node> stats

# Get agent status
agent> status

# Export agent state
agent> export json
```

## Testing and Development

### Running Tests
```bash
# Rust tests
cd core && cargo test

# Go tests
cd agent/go && go test ./...
```

### Simulation Mode
```bash
# Run with simulation
agent> simulate 10  # Create 10 random transactions

# Auto-commit mode
./scripts/run_node.sh --type agent --agent-name test_agent
agent> # Transactions are automatically committed
```

### Performance Testing
```bash
# Create multiple vertices quickly
credits-node> create "tx1"
credits-node> create "tx2"
credits-node> create "tx3"
credits-node> stats  # Check performance metrics
```

## Troubleshooting

### Common Issues

1. **"Cargo not found"**: Install Rust toolchain
2. **"Go not found"**: Install Go toolchain
3. **"Failed to create data directory"**: Check permissions
4. **"Port already in use"**: Use different ports with `--port` and `--rpc-port`

### Build Issues
```bash
# Clean build
rm -rf build/
rm -rf core/target/
./scripts/build.sh

# Check dependencies
cargo --version
go version
cmake --version
```

### Runtime Issues
```bash
# Check logs
tail -f ./data/logs/node.log

# Verify data directory
ls -la ./data/

# Check ports
netstat -an | grep 8080
```

## Integration with Existing Systems

While these are standalone executables, they can be integrated with existing systems:

1. **Library Integration**: Use the built libraries (`credits_alt_ledger.dll`) in existing applications
2. **RPC Interface**: Connect to the RPC server (port 8081 by default)
3. **File Export**: Use the export functionality to integrate with other systems
4. **Configuration**: Use the configuration system to match existing setups

## Next Steps

1. **Test the functionality**: Run both node types and explore the CLI commands
2. **Experiment with phases**: Try different migration phases to see the differences
3. **Multi-node setup**: Run multiple instances to simulate a network
4. **Performance testing**: Use the simulation features to test performance
5. **Integration planning**: Consider how to integrate with your existing CREDITS infrastructure

For more detailed information about the underlying architecture, see the main README.md file.