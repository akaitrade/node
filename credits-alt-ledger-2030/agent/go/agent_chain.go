/*
 * Agent Chain SDK for Go
 * 
 * Personal blockchain management and cross-agent coordination
 */

package agent

import (
	"crypto/sha256"
	"encoding/binary"
	"encoding/hex"
	"encoding/json"
	"fmt"
	"sync"
	"time"
)

// ChainID represents a unique identifier for an agent chain
type ChainID [32]byte

// AgentAddress represents a unique agent address
type AgentAddress [32]byte

// TransactionHash represents a transaction hash
type TransactionHash [32]byte

// AgentOperation represents an operation on an agent chain
type AgentOperation struct {
	Type     string                 `json:"type"`
	Data     map[string]interface{} `json:"data"`
	Nonce    uint64                 `json:"nonce"`
	GasLimit uint64                 `json:"gas_limit"`
}

// AgentTransaction represents a transaction on an agent chain
type AgentTransaction struct {
	ID        TransactionHash `json:"id"`
	From      AgentAddress    `json:"from"`
	To        AgentAddress    `json:"to"`
	Operation AgentOperation  `json:"operation"`
	Value     uint64          `json:"value"`
	Timestamp uint64          `json:"timestamp"`
	Signature []byte          `json:"signature"`
}

// AgentBlock represents a block in an agent chain
type AgentBlock struct {
	Height       uint64              `json:"height"`
	PreviousHash TransactionHash     `json:"previous_hash"`
	Timestamp    uint64              `json:"timestamp"`
	Transactions []AgentTransaction  `json:"transactions"`
	StateRoot    [32]byte            `json:"state_root"`
	Hash         TransactionHash     `json:"hash"`
}

// AgentChainState represents the current state of an agent chain
type AgentChainState struct {
	Owner          AgentAddress      `json:"owner"`
	ChainID        ChainID           `json:"chain_id"`
	Height         uint64            `json:"height"`
	LastBlockHash  TransactionHash   `json:"last_block_hash"`
	Nonce          uint64            `json:"nonce"`
	Balance        uint64            `json:"balance"`
	StateData      map[string][]byte `json:"state_data"`
	DAGHeight      uint64            `json:"dag_height"`
	LastSyncTime   uint64            `json:"last_sync_time"`
}

// PersonalAgentChain manages a personal agent chain
type PersonalAgentChain struct {
	mu           sync.RWMutex
	state        AgentChainState
	pendingTxs   []AgentTransaction
	coordinator  *CrossAgentCoordinator
	cnsResolver  CNSResolver
	eventHandlers map[string][]EventHandler
}

// EventHandler represents a callback for chain events
type EventHandler func(event ChainEvent)

// ChainEvent represents an event on the agent chain
type ChainEvent struct {
	Type      string      `json:"type"`
	Data      interface{} `json:"data"`
	Timestamp uint64      `json:"timestamp"`
	BlockHeight uint64    `json:"block_height"`
}

// CNSResolver interface for resolving CNS names
type CNSResolver interface {
	Resolve(namespace, name string) (*AgentAddress, error)
	Register(namespace, name, relay string) error
	Update(namespace, name, relay string) error
	Transfer(namespace, name string, newOwner AgentAddress) error
}

// NewPersonalAgentChain creates a new personal agent chain
func NewPersonalAgentChain(owner AgentAddress, cnsName string) (*PersonalAgentChain, error) {
	chainID := generateChainID(owner, cnsName)
	
	chain := &PersonalAgentChain{
		state: AgentChainState{
			Owner:         owner,
			ChainID:       chainID,
			Height:        0,
			LastBlockHash: TransactionHash{},
			Nonce:         0,
			Balance:       0,
			StateData:     make(map[string][]byte),
			DAGHeight:     0,
			LastSyncTime:  uint64(time.Now().Unix()),
		},
		pendingTxs:    make([]AgentTransaction, 0),
		eventHandlers: make(map[string][]EventHandler),
	}
	
	return chain, nil
}

// GetState returns the current chain state
func (pac *PersonalAgentChain) GetState() AgentChainState {
	pac.mu.RLock()
	defer pac.mu.RUnlock()
	return pac.state
}

// CreateTransaction creates a new transaction on the agent chain
func (pac *PersonalAgentChain) CreateTransaction(to AgentAddress, operation AgentOperation, value uint64) (*AgentTransaction, error) {
	pac.mu.Lock()
	defer pac.mu.Unlock()
	
	// Generate transaction ID
	txID := pac.generateTransactionID(to, operation, value)
	
	tx := AgentTransaction{
		ID:        txID,
		From:      pac.state.Owner,
		To:        to,
		Operation: operation,
		Value:     value,
		Timestamp: uint64(time.Now().UnixMilli()),
	}
	
	// Sign transaction (simplified - would use actual cryptographic signing)
	tx.Signature = pac.signTransaction(tx)
	
	// Add to pending transactions
	pac.pendingTxs = append(pac.pendingTxs, tx)
	
	// Emit event
	pac.emitEvent(ChainEvent{
		Type:        "transaction_created",
		Data:        tx,
		Timestamp:   tx.Timestamp,
		BlockHeight: pac.state.Height,
	})
	
	return &tx, nil
}

// CommitBlock commits pending transactions to a new block
func (pac *PersonalAgentChain) CommitBlock() (*AgentBlock, error) {
	pac.mu.Lock()
	defer pac.mu.Unlock()
	
	if len(pac.pendingTxs) == 0 {
		return nil, fmt.Errorf("no pending transactions to commit")
	}
	
	// Create new block
	block := AgentBlock{
		Height:       pac.state.Height + 1,
		PreviousHash: pac.state.LastBlockHash,
		Timestamp:    uint64(time.Now().UnixMilli()),
		Transactions: make([]AgentTransaction, len(pac.pendingTxs)),
	}
	
	copy(block.Transactions, pac.pendingTxs)
	
	// Calculate state root (simplified)
	block.StateRoot = pac.calculateStateRoot()
	
	// Calculate block hash
	block.Hash = pac.calculateBlockHash(block)
	
	// Update chain state
	pac.state.Height = block.Height
	pac.state.LastBlockHash = block.Hash
	pac.state.Nonce++
	
	// Process transactions and update state
	for _, tx := range block.Transactions {
		if err := pac.processTx(tx); err != nil {
			return nil, fmt.Errorf("failed to process transaction %x: %v", tx.ID, err)
		}
	}
	
	// Clear pending transactions
	pac.pendingTxs = pac.pendingTxs[:0]
	
	// Emit event
	pac.emitEvent(ChainEvent{
		Type:        "block_committed",
		Data:        block,
		Timestamp:   block.Timestamp,
		BlockHeight: block.Height,
	})
	
	return &block, nil
}

// TransferValue transfers value to another agent
func (pac *PersonalAgentChain) TransferValue(to AgentAddress, amount uint64) error {
	pac.mu.Lock()
	defer pac.mu.Unlock()
	
	if pac.state.Balance < amount {
		return fmt.Errorf("insufficient balance: have %d, need %d", pac.state.Balance, amount)
	}
	
	operation := AgentOperation{
		Type: "transfer",
		Data: map[string]interface{}{
			"amount": amount,
		},
		Nonce:    pac.state.Nonce + 1,
		GasLimit: 21000,
	}
	
	_, err := pac.CreateTransaction(to, operation, amount)
	return err
}

// RegisterCNSName registers a CNS name for this agent
func (pac *PersonalAgentChain) RegisterCNSName(namespace, name, relay string) error {
	if pac.cnsResolver == nil {
		return fmt.Errorf("CNS resolver not configured")
	}
	
	operation := AgentOperation{
		Type: "cns_register",
		Data: map[string]interface{}{
			"namespace": namespace,
			"name":      name,
			"relay":     relay,
		},
		Nonce:    pac.state.Nonce + 1,
		GasLimit: 50000,
	}
	
	// Create self-transaction for CNS registration
	_, err := pac.CreateTransaction(pac.state.Owner, operation, 0)
	if err != nil {
		return err
	}
	
	// Execute CNS registration
	return pac.cnsResolver.Register(namespace, name, relay)
}

// ResolveCNSName resolves a CNS name to an agent address
func (pac *PersonalAgentChain) ResolveCNSName(namespace, name string) (*AgentAddress, error) {
	if pac.cnsResolver == nil {
		return nil, fmt.Errorf("CNS resolver not configured")
	}
	
	return pac.cnsResolver.Resolve(namespace, name)
}

// SetStateData sets custom state data
func (pac *PersonalAgentChain) SetStateData(key string, value []byte) {
	pac.mu.Lock()
	defer pac.mu.Unlock()
	
	pac.state.StateData[key] = value
	
	// Emit event
	pac.emitEvent(ChainEvent{
		Type: "state_updated",
		Data: map[string]interface{}{
			"key":   key,
			"value": hex.EncodeToString(value),
		},
		Timestamp:   uint64(time.Now().UnixMilli()),
		BlockHeight: pac.state.Height,
	})
}

// GetStateData gets custom state data
func (pac *PersonalAgentChain) GetStateData(key string) []byte {
	pac.mu.RLock()
	defer pac.mu.RUnlock()
	
	return pac.state.StateData[key]
}

// SyncWithDAG synchronizes the agent chain with the main DAG
func (pac *PersonalAgentChain) SyncWithDAG(dagHeight uint64) error {
	pac.mu.Lock()
	defer pac.mu.Unlock()
	
	if dagHeight <= pac.state.DAGHeight {
		return nil // Already synced
	}
	
	pac.state.DAGHeight = dagHeight
	pac.state.LastSyncTime = uint64(time.Now().Unix())
	
	// Emit sync event
	pac.emitEvent(ChainEvent{
		Type: "dag_synced",
		Data: map[string]interface{}{
			"dag_height": dagHeight,
			"sync_time":  pac.state.LastSyncTime,
		},
		Timestamp:   pac.state.LastSyncTime,
		BlockHeight: pac.state.Height,
	})
	
	return nil
}

// AddEventHandler adds an event handler for chain events
func (pac *PersonalAgentChain) AddEventHandler(eventType string, handler EventHandler) {
	pac.mu.Lock()
	defer pac.mu.Unlock()
	
	if pac.eventHandlers[eventType] == nil {
		pac.eventHandlers[eventType] = make([]EventHandler, 0)
	}
	
	pac.eventHandlers[eventType] = append(pac.eventHandlers[eventType], handler)
}

// SetCrossAgentCoordinator sets the cross-agent coordinator
func (pac *PersonalAgentChain) SetCrossAgentCoordinator(coordinator *CrossAgentCoordinator) {
	pac.coordinator = coordinator
}

// SetCNSResolver sets the CNS resolver
func (pac *PersonalAgentChain) SetCNSResolver(resolver CNSResolver) {
	pac.cnsResolver = resolver
}

// Private methods

func (pac *PersonalAgentChain) generateTransactionID(to AgentAddress, operation AgentOperation, value uint64) TransactionHash {
	hasher := sha256.New()
	
	// Hash transaction components
	hasher.Write(pac.state.Owner[:])
	hasher.Write(to[:])
	hasher.Write([]byte(operation.Type))
	
	// Hash operation data
	if operationBytes, err := json.Marshal(operation.Data); err == nil {
		hasher.Write(operationBytes)
	}
	
	// Hash value and timestamp
	valueBytes := make([]byte, 8)
	binary.BigEndian.PutUint64(valueBytes, value)
	hasher.Write(valueBytes)
	
	timestampBytes := make([]byte, 8)
	binary.BigEndian.PutUint64(timestampBytes, uint64(time.Now().UnixNano()))
	hasher.Write(timestampBytes)
	
	hash := hasher.Sum(nil)
	var txID TransactionHash
	copy(txID[:], hash)
	return txID
}

func (pac *PersonalAgentChain) signTransaction(tx AgentTransaction) []byte {
	// Simplified signing - in production would use actual cryptographic signing
	hasher := sha256.New()
	
	hasher.Write(tx.ID[:])
	hasher.Write(tx.From[:])
	hasher.Write(tx.To[:])
	
	if txBytes, err := json.Marshal(tx.Operation); err == nil {
		hasher.Write(txBytes)
	}
	
	valueBytes := make([]byte, 8)
	binary.BigEndian.PutUint64(valueBytes, tx.Value)
	hasher.Write(valueBytes)
	
	timestampBytes := make([]byte, 8)
	binary.BigEndian.PutUint64(timestampBytes, tx.Timestamp)
	hasher.Write(timestampBytes)
	
	return hasher.Sum(nil)
}

func (pac *PersonalAgentChain) calculateStateRoot() [32]byte {
	hasher := sha256.New()
	
	// Hash state components
	hasher.Write(pac.state.Owner[:])
	hasher.Write(pac.state.ChainID[:])
	
	heightBytes := make([]byte, 8)
	binary.BigEndian.PutUint64(heightBytes, pac.state.Height)
	hasher.Write(heightBytes)
	
	nonceBytes := make([]byte, 8)
	binary.BigEndian.PutUint64(nonceBytes, pac.state.Nonce)
	hasher.Write(nonceBytes)
	
	balanceBytes := make([]byte, 8)
	binary.BigEndian.PutUint64(balanceBytes, pac.state.Balance)
	hasher.Write(balanceBytes)
	
	// Hash state data
	for key, value := range pac.state.StateData {
		hasher.Write([]byte(key))
		hasher.Write(value)
	}
	
	hash := hasher.Sum(nil)
	var stateRoot [32]byte
	copy(stateRoot[:], hash)
	return stateRoot
}

func (pac *PersonalAgentChain) calculateBlockHash(block AgentBlock) TransactionHash {
	hasher := sha256.New()
	
	// Hash block components
	heightBytes := make([]byte, 8)
	binary.BigEndian.PutUint64(heightBytes, block.Height)
	hasher.Write(heightBytes)
	
	hasher.Write(block.PreviousHash[:])
	
	timestampBytes := make([]byte, 8)
	binary.BigEndian.PutUint64(timestampBytes, block.Timestamp)
	hasher.Write(timestampBytes)
	
	hasher.Write(block.StateRoot[:])
	
	// Hash all transactions
	for _, tx := range block.Transactions {
		hasher.Write(tx.ID[:])
	}
	
	hash := hasher.Sum(nil)
	var blockHash TransactionHash
	copy(blockHash[:], hash)
	return blockHash
}

func (pac *PersonalAgentChain) processTx(tx AgentTransaction) error {
	switch tx.Operation.Type {
	case "transfer":
		return pac.processTransfer(tx)
	case "cns_register":
		return pac.processCNSRegister(tx)
	default:
		return fmt.Errorf("unknown operation type: %s", tx.Operation.Type)
	}
}

func (pac *PersonalAgentChain) processTransfer(tx AgentTransaction) error {
	if amount, ok := tx.Operation.Data["amount"].(float64); ok {
		transferAmount := uint64(amount)
		if pac.state.Balance >= transferAmount {
			pac.state.Balance -= transferAmount
			return nil
		}
		return fmt.Errorf("insufficient balance for transfer")
	}
	return fmt.Errorf("invalid transfer amount")
}

func (pac *PersonalAgentChain) processCNSRegister(tx AgentTransaction) error {
	// CNS registration is handled by the resolver
	// Here we just validate the operation
	namespace, ok1 := tx.Operation.Data["namespace"].(string)
	name, ok2 := tx.Operation.Data["name"].(string)
	
	if !ok1 || !ok2 || namespace == "" || name == "" {
		return fmt.Errorf("invalid CNS registration data")
	}
	
	return nil
}

func (pac *PersonalAgentChain) emitEvent(event ChainEvent) {
	handlers, exists := pac.eventHandlers[event.Type]
	if !exists {
		return
	}
	
	// Call all handlers for this event type
	for _, handler := range handlers {
		go handler(event) // Call asynchronously
	}
}

// Utility functions

func generateChainID(owner AgentAddress, cnsName string) ChainID {
	hasher := sha256.New()
	hasher.Write(owner[:])
	hasher.Write([]byte(cnsName))
	hasher.Write([]byte("CREDITS_AGENT_CHAIN"))
	
	hash := hasher.Sum(nil)
	var chainID ChainID
	copy(chainID[:], hash)
	return chainID
}

// String representations for debugging

func (addr AgentAddress) String() string {
	return hex.EncodeToString(addr[:])
}

func (chainID ChainID) String() string {
	return hex.EncodeToString(chainID[:])
}

func (txHash TransactionHash) String() string {
	return hex.EncodeToString(txHash[:])
}