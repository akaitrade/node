# Two Nodes Example

A basic two-node CREDITS ALT-LEDGER 2030 network for testing peer connections and consensus.

## What This Example Provides

- ✅ **Two Connected Nodes**: Bootstrap node + peer node
- ✅ **Real Consensus**: 2-node consensus with 67% voting threshold
- ✅ **Network Communication**: CTDP v2 protocol with peer discovery
- ✅ **Different Ports**: Nodes run on different ports (8080/8082)
- ✅ **Easy Startup**: Single script starts both nodes

## Quick Start

### Windows
```bash
cd examples\two-nodes
start-network.bat
```

### Manual Start (Any Platform)

**Option 1: Using individual start scripts**
```bash
# Terminal 1 - Start bootstrap node
cd examples\two-nodes\node1
start.bat

# Terminal 2 - Start peer node  
cd examples\two-nodes\node2
start.bat
```

**Option 2: Using main scripts directly**
```bash
# Terminal 1 - Start bootstrap node
cd examples\two-nodes\node1
..\..\..\scripts\run_node.bat --config config.toml

# Terminal 2 - Start peer node
cd examples\two-nodes\node2
..\..\..\scripts\run_node.bat --config config.toml
```

## Network Architecture

```
┌─────────────────┐         ┌─────────────────┐
│     Node 1      │◄──────► │     Node 2      │
│   (Bootstrap)   │         │     (Peer)      │
│  localhost:8080 │         │  localhost:8082 │
│   RPC: 8081     │         │   RPC: 8083     │
└─────────────────┘         └─────────────────┘
```

## Testing the Network

### 1. Check Connection Status
```bash
# Check Node 1
curl http://localhost:8081/status

# Check Node 2  
curl http://localhost:8083/status
```

### 2. Test Consensus
```bash
# Create transaction on Node 1
# CLI: create "hello from node 1"

# Check if it appears on Node 2
# CLI: stats
```

### 3. Monitor Synchronization
Both nodes should show:
- Same vertex count
- Same consensus rounds
- Connected peers: 1

## Configuration Details

### Node 1 (Bootstrap)
- **Port**: 8080 (RPC: 8081)
- **Role**: Bootstrap node, accepts connections
- **Peers**: None initially (waits for connections)
- **Data**: `node1/data_node1/` (completely separate directory)
- **Logs**: `node1/logs/bootstrap_node.log`

### Node 2 (Peer)
- **Port**: 8082 (RPC: 8083)  
- **Role**: Peer node, connects to bootstrap
- **Peers**: `["localhost:8080"]`
- **Data**: `node2/data_node2/` (completely separate directory)
- **Logs**: `node2/logs/peer_node.log`

### Consensus Settings
- **Algorithm**: Virtual Voting
- **Threshold**: 67% (both nodes must agree)
- **Timeout**: 30 seconds per round
- **Validators**: 2 total

## File Structure

```
two-nodes/
├── README.md              # This file
├── bootstrap.json         # Network configuration
├── start-network.bat      # Start both nodes
├── clean-data.bat         # Clean all data for fresh start
├── node1/                 # Bootstrap node
│   ├── config.toml       # Node 1 configuration
│   ├── start.bat         # Individual node starter
│   ├── data_node1/       # Node 1 blockchain data (isolated)
│   └── logs/             # Node 1 logs
└── node2/                 # Peer node
    ├── config.toml       # Node 2 configuration
    ├── start.bat         # Individual node starter  
    ├── data_node2/       # Node 2 blockchain data (isolated)
    └── logs/             # Node 2 logs
```

## API Testing

### Node 1 API (Bootstrap)
```bash
# Status
curl http://localhost:8081/status

# Stats
curl http://localhost:8081/stats

# Create transaction
curl -X POST http://localhost:8081/create -d '{"data":"from node 1"}'
```

### Node 2 API (Peer)
```bash
# Status
curl http://localhost:8083/status

# Stats  
curl http://localhost:8083/stats

# Create transaction
curl -X POST http://localhost:8083/create -d '{"data":"from node 2"}'
```

## Common Operations

### Add More Nodes
1. Copy `node2/` to `node3/`
2. Edit `node3/config.toml`:
   ```toml
   validator_id = "validator_3"
   listen_port = 8084
   rpc_port = 8085
   ```
3. Add to bootstrap.json validators list
4. Start node3

### Test Network Partition
1. Stop Node 2 (close window)
2. Try creating transactions on Node 1
3. Restart Node 2
4. Check synchronization

### Performance Testing
```bash
# Create many transactions quickly
for i in {1..100}; do
  curl -X POST http://localhost:8081/create -d "{\"data\":\"tx $i\"}"
done
```

## Troubleshooting

### Database Lock Error (Fixed)
If you see "could not acquire lock" errors:
1. Run `clean-data.bat` to clear all data
2. Or stop all nodes and restart fresh

### Nodes Won't Connect
1. Check firewall (allow ports 8080, 8082)
2. Verify both nodes are running
3. Check logs for connection errors:
   - `node1/logs/bootstrap_node.log`
   - `node2/logs/peer_node.log`

### Consensus Issues
1. Check voting threshold (should be 0.67)
2. Verify both validators are active
3. Monitor consensus rounds in CLI

### Port Conflicts
1. Change ports in config.toml files
2. Update peers list in node2/config.toml
3. Update bootstrap.json addresses

### Performance Issues
1. Increase memory limits in configs
2. Add more worker threads
3. Check disk I/O performance

## Next Steps

Once you have two nodes working:

1. **Try network-cluster** - Experience full 4-node consensus
2. **Test agent-node** - Connect personal chains to this network
3. **Build applications** - Use the dual APIs for redundancy
4. **Experiment with failures** - Test network resilience

## Learning Objectives

This example teaches:
- ✅ **Peer Discovery**: How nodes find each other
- ✅ **Consensus Mechanics**: Multi-node agreement process
- ✅ **Network Protocols**: CTDP v2 communication
- ✅ **Configuration Management**: Multiple node setups
- ✅ **API Usage**: Interacting with distributed nodes