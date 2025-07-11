# Manual Build Instructions

## If the batch file doesn't work, follow these manual steps:

### Step 1: Navigate to the project directory
```cmd
cd C:\Users\BK_HOME\Documents\baseprojects\websocketordinalbk\node\credits-alt-ledger-2030
```

### Step 2: Build the Rust DAG Node
```cmd
cd core
cargo build --release --bin credits-node
```

**Expected output:**
```
Compiling dag_engine v1.0.0 (C:\Users\BK_HOME\Documents\baseprojects\websocketordinalbk\node\credits-alt-ledger-2030\core)
Finished release [optimized] target(s) in X.XXs
```

**If successful, you'll have:**
- `core\target\release\credits-node.exe`

### Step 3: Build the Go Agent CLI
```cmd
cd ..\agent\go
go build -o agent-cli.exe main.go agent_chain.go cross_agent.go
```

**Expected output:**
```
(No output if successful)
```

**If successful, you'll have:**
- `agent\go\agent-cli.exe`

### Step 4: Test the executables

#### Test DAG Node:
```cmd
cd ..\..\core\target\release
credits-node.exe --help
```

#### Test Agent CLI:
```cmd
cd ..\..\..\agent\go
agent-cli.exe --help
```

### Step 5: Run with convenience scripts

#### DAG Node:
```cmd
cd ..\..\scripts
run_node.bat --type dag
```

#### Agent Chain:
```cmd
run_node.bat --type agent --agent-name alice
```

## Alternative: Use PowerShell

If cmd doesn't work, try PowerShell:

```powershell
# Navigate to project
Set-Location "C:\Users\BK_HOME\Documents\baseprojects\websocketordinalbk\node\credits-alt-ledger-2030"

# Build Rust
Set-Location "core"
cargo build --release --bin credits-node

# Build Go
Set-Location "..\agent\go"
go build -o agent-cli.exe main.go agent_chain.go cross_agent.go

# Test
Set-Location "..\..\scripts"
.\run_node.bat --type dag
```

## Troubleshooting

### If Rust build fails:
1. Check if you're in the correct directory (should see `Cargo.toml`)
2. Run `cargo clean` then try again
3. Check Rust version: `cargo --version` (need 1.70+)

### If Go build fails:
1. Check if you're in the correct directory (should see `go.mod`)
2. Run `go mod tidy` then try again
3. Check Go version: `go version` (need 1.19+)

### If executables don't run:
1. Check if they exist in the expected locations
2. Try running with full path
3. Check Windows permissions

## Success Indicators

You should see these files created:
- `core\target\release\credits-node.exe` (Rust DAG Node)
- `agent\go\agent-cli.exe` (Go Agent CLI)

Both should respond to `--help` flag.