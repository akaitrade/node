# CREDITS ALT-LEDGER 2030 - Node Examples

This directory contains ready-to-use example configurations for different node setups. Each example includes all necessary files to run a node or network.

## Available Examples

### 🔸 **single-node/**
Basic standalone node for development and testing.
- **Use case**: Learning, development, local testing
- **Files**: config.toml, start.bat, README.md
- **Network**: None (standalone)

### 🔸 **two-nodes/**
Two connected nodes for basic networking tests.
- **Use case**: Testing peer connections, basic consensus
- **Files**: node1/, node2/ directories with configs
- **Network**: Local connection between two nodes

### 🔸 **network-cluster/**
Multi-node network (4 nodes) simulating a small blockchain network.
- **Use case**: Testing full consensus, load testing
- **Files**: node1-4/ directories, bootstrap config
- **Network**: Full mesh network with consensus

### 🔸 **bootstrap-node/**
Production bootstrap node configuration.
- **Use case**: Main network entry point, production deployment
- **Files**: Production config, security settings, monitoring
- **Network**: Accepts connections from other nodes

### 🔸 **agent-node/**
Personal agent chain node configuration.
- **Use case**: Personal blockchain, off-chain operations
- **Files**: Agent-specific configs, personal chain settings
- **Network**: Connects to DAG nodes for synchronization

## Quick Start

1. **Choose an example** based on your use case
2. **Copy the example folder** to your working directory
3. **Follow the README.md** in that folder
4. **Run the start script** or use the provided commands

## Example Selection Guide

| Need | Example | Nodes | Complexity |
|------|---------|-------|------------|
| Learn the basics | single-node | 1 | ⭐ |
| Test networking | two-nodes | 2 | ⭐⭐ |
| Full blockchain | network-cluster | 4 | ⭐⭐⭐ |
| Production setup | bootstrap-node | 1+ | ⭐⭐⭐⭐ |
| Personal chain | agent-node | 1 | ⭐⭐ |

## Configuration Templates

Each example includes:
- ✅ **config.toml** - Node configuration with unique data directories
- ✅ **bootstrap.json** - Network bootstrap info (if needed)
- ✅ **start.bat** - Windows startup script with correct paths
- ✅ **start.sh** - Linux startup script  
- ✅ **clean-data.bat** - Clean all data for fresh start
- ✅ **README.md** - Specific instructions
- ✅ **logs/** - Log directory
- ✅ **data_*/** - Isolated data directories (no conflicts)
- ✅ **certs/** - TLS certificates (if needed)

## Common Operations

### Copy and Run
```bash
# Copy an example
xcopy /E examples\single-node my-node\

# Customize the config
notepad my-node\config.toml

# Start the node
cd my-node
start.bat
```

### Multiple Networks
```bash
# Run different examples simultaneously
cd examples\two-nodes
start-network.bat

# In another terminal
cd examples\agent-node  
start.bat
```

### Custom Configuration
Each example serves as a template. You can:
1. Copy any example as a starting point
2. Modify configs for your specific needs
3. Combine elements from different examples
4. Scale up/down by adding/removing nodes

## Support Files

All examples include standard support files:
- **genesis.json** - Genesis block configuration
- **validator_keys.json** - Validator key pairs
- **network_topology.json** - Network structure definition
- **performance_profile.toml** - Performance tuning settings