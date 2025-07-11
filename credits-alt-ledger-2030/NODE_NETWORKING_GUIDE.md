# CREDITS ALT-LEDGER 2030 - Node Networking Guide

## How Nodes Connect to Each Other

The CREDITS ALT-LEDGER 2030 blockchain uses a hybrid networking approach combining traditional P2P discovery with the innovative Agent-Centric architecture.

## Network Architecture

### 1. Node Types

**DAG Nodes (Validators)**
- Run the main DAG consensus engine
- Process transactions and maintain the global ledger
- Communicate via CTDP v2 (Credits Transport Data Protocol)
- Default port: 8080

**Agent Nodes (Personal Chains)**
- Manage individual agent chains
- Connect to DAG nodes to sync state
- Can operate offline and sync later
- Default port: 8090

### 2. Node Discovery Methods

#### A. Bootstrap Nodes (Recommended for Starting)
Create a `bootstrap.json` file in your data directory:

```json
{
  "bootstrap_nodes": [
    {
      "id": "validator_1",
      "address": "192.168.1.100:8080",
      "public_key": "..."
    },
    {
      "id": "validator_2", 
      "address": "192.168.1.101:8080",
      "public_key": "..."
    }
  ]
}
```

#### B. Manual Peer Configuration
Add peers directly in `config.toml`:

```toml
[networking]
# Static peers for initial connection
peers = [
    "192.168.1.100:8080",
    "192.168.1.101:8080",
    "10.0.0.5:8080"
]

# Enable peer discovery
enable_discovery = true
discovery_interval = 30
```

#### C. Local Network Discovery
For nodes on the same network:

```toml
[networking]
# Enable local discovery via UDP broadcast
enable_local_discovery = true
broadcast_port = 8888
```

### 3. Running Multiple Nodes

#### Example 1: Two Nodes on Same Machine

**Node 1 (Genesis/Bootstrap):**
```bash
cd node1
scripts\run_node.bat --port 8080 --data-dir ./data1 --validator-id validator_1
```

**Node 2 (Connecting to Node 1):**
```bash
cd node2
# Create bootstrap.json pointing to Node 1
echo { "bootstrap_nodes": [{"id": "validator_1", "address": "localhost:8080"}] } > data2\bootstrap.json

scripts\run_node.bat --port 8081 --data-dir ./data2 --validator-id validator_2
```

#### Example 2: Nodes on Different Machines

**Machine A (192.168.1.100):**
```bash
scripts\run_node.bat --port 8080 --validator-id validator_1
```

**Machine B (192.168.1.101):**
```bash
# Create config with Machine A as peer
scripts\run_node.bat --port 8080 --validator-id validator_2 --peer 192.168.1.100:8080
```

### 4. Network Topology

```
                    ┌─────────────┐
                    │  Bootstrap  │
                    │    Node     │
                    └──────┬──────┘
                           │
                ┌──────────┴──────────┐
                │                     │
         ┌──────▼──────┐      ┌──────▼──────┐
         │  Validator  │      │  Validator  │
         │   Node 1    │◄────►│   Node 2    │
         └──────┬──────┘      └──────┬──────┘
                │                     │
         ┌──────▼──────┐      ┌──────▼──────┐
         │    Agent    │      │    Agent    │
         │   Chain 1   │      │   Chain 2   │
         └─────────────┘      └─────────────┘
```

### 5. Connection Process

1. **Initial Handshake**
   - Node starts and loads bootstrap peers
   - Sends HELLO message with node info
   - Receives peer list from bootstrap nodes

2. **Peer Discovery**
   - Exchanges peer lists with connected nodes
   - Maintains active connection pool
   - Periodically pings peers for liveness

3. **State Synchronization**
   - New nodes request current state
   - Download DAG vertices from peers
   - Verify and apply state transitions

### 6. Quick Start Commands

#### Single Node (Standalone Testing)
```bash
scripts\run_node.bat
```

#### Two-Node Network (Same Machine)
```bash
# Terminal 1
scripts\run_node.bat --port 8080 --validator-id node1

# Terminal 2  
scripts\run_node.bat --port 8081 --validator-id node2 --peer localhost:8080
```

#### Multi-Node Network (Different Machines)
```bash
# Machine 1 (Bootstrap)
scripts\run_node.bat --port 8080 --validator-id bootstrap

# Machine 2
scripts\run_node.bat --port 8080 --validator-id node2 --bootstrap 192.168.1.100:8080

# Machine 3
scripts\run_node.bat --port 8080 --validator-id node3 --bootstrap 192.168.1.100:8080
```

### 7. Configuration Examples

#### Minimal Network Config
```toml
[networking]
listen_port = 8080
peers = ["192.168.1.100:8080"]  # At least one peer to connect
```

#### Full Network Config
```toml
[networking]
# Basic settings
listen_port = 8080
bind_address = "0.0.0.0"  # Listen on all interfaces

# Peer discovery
enable_discovery = true
discovery_interval = 30
max_peers = 50
min_peers = 3

# Known peers
peers = [
    "192.168.1.100:8080",
    "192.168.1.101:8080",
    "node.example.com:8080"
]

# Connection settings
connection_timeout_seconds = 30
max_connections = 1000
enable_compression = true

# Protocol settings
transport_protocol = "ctdp_v2"
enable_quic = true
enable_tls = true
```

### 8. Monitoring Connections

Check node connections using the CLI:

```bash
# Start node with CLI
scripts\run_node.bat

# In the CLI, check peers
credits-node> status
credits-node> peers
credits-node> network-info
```

### 9. Troubleshooting

**Node Can't Find Peers:**
1. Check firewall settings (allow port 8080)
2. Verify bootstrap.json exists and is valid
3. Ensure peers are running and accessible
4. Check network connectivity: `ping <peer-ip>`

**Connection Refused:**
1. Verify the peer is running
2. Check the port is correct
3. Ensure no firewall blocking
4. Try telnet test: `telnet <peer-ip> 8080`

**Slow Synchronization:**
1. Check network bandwidth
2. Increase peer connections in config
3. Verify system resources (CPU, RAM)
4. Check disk I/O performance

### 10. Advanced Networking

#### Running Behind NAT
```toml
[networking]
# Public address for other nodes to connect
public_address = "your-public-ip:8080"
listen_port = 8080

# Enable NAT traversal
enable_nat_traversal = true
upnp_enabled = true
```

#### Using DNS Seeds
```toml
[networking]
# DNS seed nodes for discovery
dns_seeds = [
    "seed1.credits-ledger.network",
    "seed2.credits-ledger.network"
]
```

#### Private Network Setup
```toml
[networking]
# Disable public discovery
enable_discovery = false
enable_local_discovery = false

# Only connect to specific peers
peers = [
    "10.0.0.1:8080",
    "10.0.0.2:8080",
    "10.0.0.3:8080"
]

# Require authentication
require_auth = true
network_id = "private-credits-network"
```

## Summary

The CREDITS ALT-LEDGER 2030 network is designed to be flexible and easy to set up:

1. **Simplest Setup**: Just run nodes with `--peer` flag pointing to each other
2. **Production Setup**: Use bootstrap.json with known stable nodes
3. **Development**: Run multiple nodes on localhost with different ports
4. **Enterprise**: Configure private networks with specific peer lists

The hybrid architecture allows both traditional blockchain networking and innovative agent-centric personal chains, providing flexibility for various use cases.