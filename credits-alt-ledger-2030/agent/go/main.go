/*
 * CREDITS ALT-LEDGER 2030 - Agent Chain CLI
 * 
 * Standalone agent chain interface for personal blockchain management
 * Supports cross-agent coordination and CNS integration
 */

package main

import (
	"bufio"
	"encoding/hex"
	"encoding/json"
	"fmt"
	"log"
	"os"
	"strconv"
	"strings"
	"time"

	"github.com/credits/alt-ledger-2030/agent"
)

// CLIConfig represents the configuration for the CLI
type CLIConfig struct {
	DataDir    string `json:"data_dir"`
	AgentName  string `json:"agent_name"`
	CNSEnabled bool   `json:"cns_enabled"`
	LogLevel   string `json:"log_level"`
	AutoCommit bool   `json:"auto_commit"`
}

// AgentCLI represents the main CLI interface
type AgentCLI struct {
	config     CLIConfig
	agentChain *agent.PersonalAgentChain
	running    bool
}

// NewAgentCLI creates a new agent CLI instance
func NewAgentCLI(config CLIConfig) (*AgentCLI, error) {
	// Create agent address from name
	agentAddr := generateAgentAddress(config.AgentName)
	
	// Create personal agent chain
	chain, err := agent.NewPersonalAgentChain(agentAddr, config.AgentName)
	if err != nil {
		return nil, fmt.Errorf("failed to create agent chain: %v", err)
	}

	cli := &AgentCLI{
		config:     config,
		agentChain: chain,
		running:    true,
	}

	// Setup event handlers
	cli.setupEventHandlers()

	return cli, nil
}

// Start starts the agent CLI
func (cli *AgentCLI) Start() {
	fmt.Println("🚀 CREDITS ALT-LEDGER 2030 - Agent Chain CLI")
	fmt.Printf("👤 Agent: %s\n", cli.config.AgentName)
	fmt.Printf("📍 Address: %s\n", cli.agentChain.GetState().Owner.String())
	fmt.Printf("⛓️  Chain ID: %s\n", cli.agentChain.GetState().ChainID.String())
	fmt.Println("✅ Agent chain initialized successfully")
	
	cli.printHelp()
	cli.runInteractiveLoop()
}

// runInteractiveLoop runs the main interactive command loop
func (cli *AgentCLI) runInteractiveLoop() {
	scanner := bufio.NewScanner(os.Stdin)
	
	for cli.running {
		fmt.Print("agent> ")
		
		if !scanner.Scan() {
			break
		}
		
		input := strings.TrimSpace(scanner.Text())
		if input == "" {
			continue
		}

		if err := cli.handleCommand(input); err != nil {
			fmt.Printf("❌ Error: %v\n", err)
		}
	}
	
	if err := scanner.Err(); err != nil {
		fmt.Printf("❌ Scanner error: %v\n", err)
	}
}

// handleCommand handles a single command
func (cli *AgentCLI) handleCommand(input string) error {
	parts := strings.Fields(input)
	if len(parts) == 0 {
		return nil
	}

	command := strings.ToLower(parts[0])
	args := parts[1:]

	switch command {
	case "help":
		cli.printHelp()
	case "status":
		cli.printStatus()
	case "balance":
		cli.printBalance()
	case "create-tx":
		return cli.createTransaction(args)
	case "commit":
		return cli.commitBlock()
	case "transfer":
		return cli.transfer(args)
	case "set-state":
		return cli.setState(args)
	case "get-state":
		return cli.getState(args)
	case "register-cns":
		return cli.registerCNS(args)
	case "resolve-cns":
		return cli.resolveCNS(args)
	case "history":
		cli.printHistory()
	case "simulate":
		return cli.simulateActivity(args)
	case "export":
		return cli.exportState(args)
	case "quit", "exit":
		cli.running = false
		fmt.Println("👋 Goodbye!")
	default:
		return fmt.Errorf("unknown command: %s. Type 'help' for available commands", command)
	}

	return nil
}

// printHelp prints the help message
func (cli *AgentCLI) printHelp() {
	fmt.Println("\n📚 Available Commands:")
	fmt.Println("  help              - Show this help message")
	fmt.Println("  status            - Show agent and chain status")
	fmt.Println("  balance           - Show current balance")
	fmt.Println("  create-tx <to> <op> [value] - Create a transaction")
	fmt.Println("  commit            - Commit pending transactions to a block")
	fmt.Println("  transfer <to> <amount> - Transfer value to another agent")
	fmt.Println("  set-state <key> <value> - Set custom state data")
	fmt.Println("  get-state <key>   - Get custom state data")
	fmt.Println("  register-cns <namespace> <name> <relay> - Register CNS name")
	fmt.Println("  resolve-cns <namespace> <name> - Resolve CNS name")
	fmt.Println("  history           - Show transaction history")
	fmt.Println("  simulate <count>  - Simulate random activity")
	fmt.Println("  export <format>   - Export state (json/csv)")
	fmt.Println("  quit/exit         - Exit the CLI")
	fmt.Println("")
}

// printStatus prints the current status
func (cli *AgentCLI) printStatus() {
	state := cli.agentChain.GetState()
	
	fmt.Println("\n📊 Agent Chain Status:")
	fmt.Printf("  Agent Name: %s\n", cli.config.AgentName)
	fmt.Printf("  Address: %s\n", state.Owner.String())
	fmt.Printf("  Chain ID: %s\n", state.ChainID.String())
	fmt.Printf("  Height: %d\n", state.Height)
	fmt.Printf("  Nonce: %d\n", state.Nonce)
	fmt.Printf("  Balance: %d CREDITS\n", state.Balance)
	fmt.Printf("  DAG Height: %d\n", state.DAGHeight)
	fmt.Printf("  Last Block: %s\n", state.LastBlockHash.String())
	fmt.Printf("  Last Sync: %s\n", time.Unix(int64(state.LastSyncTime), 0).Format("2006-01-02 15:04:05"))
	fmt.Printf("  State Data Keys: %d\n", len(state.StateData))
	fmt.Println("")
}

// printBalance prints the current balance
func (cli *AgentCLI) printBalance() {
	state := cli.agentChain.GetState()
	fmt.Printf("💰 Balance: %d CREDITS\n", state.Balance)
}

// createTransaction creates a new transaction
func (cli *AgentCLI) createTransaction(args []string) error {
	if len(args) < 2 {
		return fmt.Errorf("usage: create-tx <to> <operation> [value]")
	}

	// Parse destination address
	toAddr, err := parseAgentAddress(args[0])
	if err != nil {
		return fmt.Errorf("invalid destination address: %v", err)
	}

	// Parse operation
	operation := agent.AgentOperation{
		Type:     args[1],
		Data:     make(map[string]interface{}),
		Nonce:    cli.agentChain.GetState().Nonce + 1,
		GasLimit: 21000,
	}

	// Parse value if provided
	var value uint64 = 0
	if len(args) > 2 {
		if v, err := strconv.ParseUint(args[2], 10, 64); err == nil {
			value = v
		}
	}

	// Create transaction
	tx, err := cli.agentChain.CreateTransaction(toAddr, operation, value)
	if err != nil {
		return fmt.Errorf("failed to create transaction: %v", err)
	}

	fmt.Printf("✅ Transaction created: %s\n", tx.ID.String())
	fmt.Printf("  To: %s\n", tx.To.String())
	fmt.Printf("  Operation: %s\n", tx.Operation.Type)
	fmt.Printf("  Value: %d CREDITS\n", tx.Value)
	
	if cli.config.AutoCommit {
		fmt.Println("🔄 Auto-committing transaction...")
		return cli.commitBlock()
	}
	
	return nil
}

// commitBlock commits pending transactions
func (cli *AgentCLI) commitBlock() error {
	block, err := cli.agentChain.CommitBlock()
	if err != nil {
		return fmt.Errorf("failed to commit block: %v", err)
	}

	fmt.Printf("✅ Block committed: Height %d\n", block.Height)
	fmt.Printf("  Hash: %s\n", block.Hash.String())
	fmt.Printf("  Transactions: %d\n", len(block.Transactions))
	fmt.Printf("  Timestamp: %s\n", time.Unix(int64(block.Timestamp/1000), 0).Format("2006-01-02 15:04:05"))
	
	return nil
}

// transfer transfers value to another agent
func (cli *AgentCLI) transfer(args []string) error {
	if len(args) < 2 {
		return fmt.Errorf("usage: transfer <to> <amount>")
	}

	// Parse destination address
	toAddr, err := parseAgentAddress(args[0])
	if err != nil {
		return fmt.Errorf("invalid destination address: %v", err)
	}

	// Parse amount
	amount, err := strconv.ParseUint(args[1], 10, 64)
	if err != nil {
		return fmt.Errorf("invalid amount: %v", err)
	}

	// Execute transfer
	if err := cli.agentChain.TransferValue(toAddr, amount); err != nil {
		return fmt.Errorf("transfer failed: %v", err)
	}

	fmt.Printf("✅ Transfer initiated: %d CREDITS to %s\n", amount, toAddr.String())
	
	if cli.config.AutoCommit {
		fmt.Println("🔄 Auto-committing transfer...")
		return cli.commitBlock()
	}
	
	return nil
}

// setState sets custom state data
func (cli *AgentCLI) setState(args []string) error {
	if len(args) < 2 {
		return fmt.Errorf("usage: set-state <key> <value>")
	}

	key := args[0]
	value := strings.Join(args[1:], " ")
	
	cli.agentChain.SetStateData(key, []byte(value))
	
	fmt.Printf("✅ State data set: %s = %s\n", key, value)
	return nil
}

// getState gets custom state data
func (cli *AgentCLI) getState(args []string) error {
	if len(args) < 1 {
		return fmt.Errorf("usage: get-state <key>")
	}

	key := args[0]
	value := cli.agentChain.GetStateData(key)
	
	if value == nil {
		fmt.Printf("❌ No state data found for key: %s\n", key)
	} else {
		fmt.Printf("📊 State data: %s = %s\n", key, string(value))
	}
	
	return nil
}

// registerCNS registers a CNS name
func (cli *AgentCLI) registerCNS(args []string) error {
	if len(args) < 3 {
		return fmt.Errorf("usage: register-cns <namespace> <name> <relay>")
	}

	namespace := args[0]
	name := args[1]
	relay := args[2]

	// Note: This is a simplified implementation
	// In a real system, this would integrate with the actual CNS
	fmt.Printf("📝 CNS Registration (simulated):\n")
	fmt.Printf("  Namespace: %s\n", namespace)
	fmt.Printf("  Name: %s\n", name)
	fmt.Printf("  Relay: %s\n", relay)
	fmt.Printf("  Owner: %s\n", cli.agentChain.GetState().Owner.String())
	fmt.Println("✅ CNS registration completed")
	
	return nil
}

// resolveCNS resolves a CNS name
func (cli *AgentCLI) resolveCNS(args []string) error {
	if len(args) < 2 {
		return fmt.Errorf("usage: resolve-cns <namespace> <name>")
	}

	namespace := args[0]
	name := args[1]

	// Note: This is a simplified implementation
	fmt.Printf("🔍 CNS Resolution (simulated):\n")
	fmt.Printf("  Namespace: %s\n", namespace)
	fmt.Printf("  Name: %s\n", name)
	fmt.Printf("  Resolved to: %s\n", generateAgentAddress(name).String())
	
	return nil
}

// printHistory prints transaction history
func (cli *AgentCLI) printHistory() {
	fmt.Println("📜 Transaction History:")
	fmt.Println("  (History feature would show committed transactions)")
	fmt.Println("  Current implementation focuses on real-time operations")
}

// simulateActivity simulates random blockchain activity
func (cli *AgentCLI) simulateActivity(args []string) error {
	count := 5
	if len(args) > 0 {
		if c, err := strconv.Atoi(args[0]); err == nil {
			count = c
		}
	}

	fmt.Printf("🎯 Simulating %d random activities...\n", count)
	
	for i := 0; i < count; i++ {
		// Generate random agent address
		randomAddr := generateAgentAddress(fmt.Sprintf("agent_%d", i))
		
		// Create random operation
		operation := agent.AgentOperation{
			Type: "simulate",
			Data: map[string]interface{}{
				"action": fmt.Sprintf("simulation_%d", i),
				"timestamp": time.Now().Unix(),
			},
			Nonce:    cli.agentChain.GetState().Nonce + 1,
			GasLimit: 21000,
		}

		// Create transaction
		tx, err := cli.agentChain.CreateTransaction(randomAddr, operation, 0)
		if err != nil {
			fmt.Printf("❌ Failed to create simulation transaction %d: %v\n", i, err)
			continue
		}

		fmt.Printf("  %d. Transaction: %s\n", i+1, tx.ID.String())
		
		// Small delay between transactions
		time.Sleep(100 * time.Millisecond)
	}

	// Commit all simulated transactions
	if err := cli.commitBlock(); err != nil {
		return fmt.Errorf("failed to commit simulation block: %v", err)
	}

	fmt.Println("✅ Simulation completed")
	return nil
}

// exportState exports the current state
func (cli *AgentCLI) exportState(args []string) error {
	format := "json"
	if len(args) > 0 {
		format = strings.ToLower(args[0])
	}

	state := cli.agentChain.GetState()
	
	switch format {
	case "json":
		data, err := json.MarshalIndent(state, "", "  ")
		if err != nil {
			return fmt.Errorf("failed to marshal state: %v", err)
		}
		
		filename := fmt.Sprintf("agent_state_%s.json", cli.config.AgentName)
		if err := os.WriteFile(filename, data, 0644); err != nil {
			return fmt.Errorf("failed to write state file: %v", err)
		}
		
		fmt.Printf("✅ State exported to %s\n", filename)
		
	case "csv":
		filename := fmt.Sprintf("agent_state_%s.csv", cli.config.AgentName)
		file, err := os.Create(filename)
		if err != nil {
			return fmt.Errorf("failed to create CSV file: %v", err)
		}
		defer file.Close()
		
		// Write CSV header
		fmt.Fprintf(file, "Key,Value\n")
		fmt.Fprintf(file, "Agent Name,%s\n", cli.config.AgentName)
		fmt.Fprintf(file, "Address,%s\n", state.Owner.String())
		fmt.Fprintf(file, "Chain ID,%s\n", state.ChainID.String())
		fmt.Fprintf(file, "Height,%d\n", state.Height)
		fmt.Fprintf(file, "Nonce,%d\n", state.Nonce)
		fmt.Fprintf(file, "Balance,%d\n", state.Balance)
		fmt.Fprintf(file, "DAG Height,%d\n", state.DAGHeight)
		
		for key, value := range state.StateData {
			fmt.Fprintf(file, "State_%s,%s\n", key, hex.EncodeToString(value))
		}
		
		fmt.Printf("✅ State exported to %s\n", filename)
		
	default:
		return fmt.Errorf("unsupported export format: %s (supported: json, csv)", format)
	}

	return nil
}

// setupEventHandlers sets up event handlers for the agent chain
func (cli *AgentCLI) setupEventHandlers() {
	// Handler for transaction creation
	cli.agentChain.AddEventHandler("transaction_created", func(event agent.ChainEvent) {
		fmt.Printf("📤 Transaction created: %s\n", event.Type)
	})

	// Handler for block commits
	cli.agentChain.AddEventHandler("block_committed", func(event agent.ChainEvent) {
		fmt.Printf("⛓️  Block committed at height: %d\n", event.BlockHeight)
	})

	// Handler for state updates
	cli.agentChain.AddEventHandler("state_updated", func(event agent.ChainEvent) {
		fmt.Printf("📊 State updated: %s\n", event.Type)
	})

	// Handler for DAG sync
	cli.agentChain.AddEventHandler("dag_synced", func(event agent.ChainEvent) {
		fmt.Printf("🔄 DAG synced: %s\n", event.Type)
	})
}

// Utility functions

// generateAgentAddress generates an agent address from a name
func generateAgentAddress(name string) agent.AgentAddress {
	// Simple hash-based address generation
	hash := make([]byte, 32)
	nameBytes := []byte(name)
	
	for i := 0; i < 32; i++ {
		if i < len(nameBytes) {
			hash[i] = nameBytes[i]
		} else {
			hash[i] = byte(i) // Simple padding
		}
	}
	
	var addr agent.AgentAddress
	copy(addr[:], hash)
	return addr
}

// parseAgentAddress parses an agent address from string
func parseAgentAddress(addrStr string) (agent.AgentAddress, error) {
	// If it's a name (not hex), generate address
	if len(addrStr) != 64 || !isHex(addrStr) {
		return generateAgentAddress(addrStr), nil
	}
	
	// Parse as hex
	bytes, err := hex.DecodeString(addrStr)
	if err != nil {
		return agent.AgentAddress{}, fmt.Errorf("invalid hex address: %v", err)
	}
	
	if len(bytes) != 32 {
		return agent.AgentAddress{}, fmt.Errorf("address must be 32 bytes")
	}
	
	var addr agent.AgentAddress
	copy(addr[:], bytes)
	return addr, nil
}

// isHex checks if a string is valid hex
func isHex(s string) bool {
	for _, r := range s {
		if !((r >= '0' && r <= '9') || (r >= 'a' && r <= 'f') || (r >= 'A' && r <= 'F')) {
			return false
		}
	}
	return true
}

// parseConfig parses configuration from arguments
func parseConfig(args []string) CLIConfig {
	config := CLIConfig{
		DataDir:    "./agent_data",
		AgentName:  "agent_1",
		CNSEnabled: true,
		LogLevel:   "info",
		AutoCommit: false,
	}

	for i := 0; i < len(args); i++ {
		switch args[i] {
		case "--data-dir":
			if i+1 < len(args) {
				config.DataDir = args[i+1]
				i++
			}
		case "--agent-name":
			if i+1 < len(args) {
				config.AgentName = args[i+1]
				i++
			}
		case "--auto-commit":
			config.AutoCommit = true
		case "--no-cns":
			config.CNSEnabled = false
		case "--log-level":
			if i+1 < len(args) {
				config.LogLevel = args[i+1]
				i++
			}
		case "--help":
			printUsage()
			os.Exit(0)
		}
	}

	return config
}

// printUsage prints usage information
func printUsage() {
	fmt.Println("CREDITS ALT-LEDGER 2030 - Agent Chain CLI")
	fmt.Println("")
	fmt.Println("Usage: agent-cli [OPTIONS]")
	fmt.Println("")
	fmt.Println("Options:")
	fmt.Println("  --data-dir <path>     Data directory (default: ./agent_data)")
	fmt.Println("  --agent-name <name>   Agent name (default: agent_1)")
	fmt.Println("  --auto-commit         Auto-commit transactions")
	fmt.Println("  --no-cns              Disable CNS integration")
	fmt.Println("  --log-level <level>   Log level (default: info)")
	fmt.Println("  --help                Show this help message")
	fmt.Println("")
	fmt.Println("Examples:")
	fmt.Println("  agent-cli                              # Start with defaults")
	fmt.Println("  agent-cli --agent-name alice           # Start as agent 'alice'")
	fmt.Println("  agent-cli --auto-commit --agent-name bob # Auto-commit mode")
}

// main function
func main() {
	// Parse configuration
	config := parseConfig(os.Args[1:])

	// Create data directory
	if err := os.MkdirAll(config.DataDir, 0755); err != nil {
		log.Fatalf("Failed to create data directory: %v", err)
	}

	// Create and start CLI
	cli, err := NewAgentCLI(config)
	if err != nil {
		log.Fatalf("Failed to create agent CLI: %v", err)
	}

	// Start the CLI
	cli.Start()
}