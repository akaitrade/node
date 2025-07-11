/*
 * Cross-Agent Transaction Coordinator
 * 
 * Implements two-phase commit protocol for atomic transactions across multiple agent chains
 */

package agent

import (
	"context"
	"crypto/sha256"
	"encoding/binary"
	"encoding/hex"
	"encoding/json"
	"fmt"
	"sync"
	"time"
)

// CrossAgentTransactionID represents a unique identifier for cross-agent transactions
type CrossAgentTransactionID [32]byte

// CrossAgentOperation represents an operation that spans multiple agent chains
type CrossAgentOperation struct {
	ID           CrossAgentTransactionID    `json:"id"`
	Participants []AgentAddress             `json:"participants"`
	Operations   map[AgentAddress]AgentOperation `json:"operations"`
	Coordinator  AgentAddress               `json:"coordinator"`
	Timeout      time.Duration              `json:"timeout"`
	CreatedAt    time.Time                  `json:"created_at"`
	Status       TransactionStatus          `json:"status"`
}

// TransactionStatus represents the status of a cross-agent transaction
type TransactionStatus string

const (
	StatusPending   TransactionStatus = "pending"
	StatusPreparing TransactionStatus = "preparing"
	StatusPrepared  TransactionStatus = "prepared"
	StatusCommitting TransactionStatus = "committing"
	StatusCommitted TransactionStatus = "committed"
	StatusAborting  TransactionStatus = "aborting"
	StatusAborted   TransactionStatus = "aborted"
	StatusFailed    TransactionStatus = "failed"
)

// PrepareResponse represents a participant's response to the prepare phase
type PrepareResponse struct {
	Participant AgentAddress `json:"participant"`
	Success     bool         `json:"success"`
	Error       string       `json:"error,omitempty"`
	Timestamp   time.Time    `json:"timestamp"`
}

// CommitResponse represents a participant's response to the commit phase
type CommitResponse struct {
	Participant AgentAddress `json:"participant"`
	Success     bool         `json:"success"`
	Error       string       `json:"error,omitempty"`
	Timestamp   time.Time    `json:"timestamp"`
}

// CrossAgentCoordinator manages cross-agent transactions using two-phase commit
type CrossAgentCoordinator struct {
	mu                sync.RWMutex
	activeTransactions map[CrossAgentTransactionID]*CrossAgentOperation
	agentChains       map[AgentAddress]*PersonalAgentChain
	networkClient     NetworkClient
	timeoutDuration   time.Duration
}

// NetworkClient interface for communicating with other agents
type NetworkClient interface {
	SendPrepareRequest(ctx context.Context, participant AgentAddress, operation AgentOperation) (*PrepareResponse, error)
	SendCommitRequest(ctx context.Context, participant AgentAddress, txID CrossAgentTransactionID) (*CommitResponse, error)
	SendAbortRequest(ctx context.Context, participant AgentAddress, txID CrossAgentTransactionID) error
}

// NewCrossAgentCoordinator creates a new cross-agent coordinator
func NewCrossAgentCoordinator(networkClient NetworkClient) *CrossAgentCoordinator {
	return &CrossAgentCoordinator{
		activeTransactions: make(map[CrossAgentTransactionID]*CrossAgentOperation),
		agentChains:       make(map[AgentAddress]*PersonalAgentChain),
		networkClient:     networkClient,
		timeoutDuration:   30 * time.Second,
	}
}

// RegisterAgentChain registers an agent chain with the coordinator
func (cac *CrossAgentCoordinator) RegisterAgentChain(chain *PersonalAgentChain) {
	cac.mu.Lock()
	defer cac.mu.Unlock()
	
	state := chain.GetState()
	cac.agentChains[state.Owner] = chain
	chain.SetCrossAgentCoordinator(cac)
}

// ExecuteAtomicTransaction executes a cross-agent transaction atomically
func (cac *CrossAgentCoordinator) ExecuteAtomicTransaction(
	participants []AgentAddress,
	operations map[AgentAddress]AgentOperation,
	coordinator AgentAddress,
) (*CrossAgentOperation, error) {
	
	// Generate transaction ID
	txID := cac.generateTransactionID(participants, operations, coordinator)
	
	// Create cross-agent operation
	crossTx := &CrossAgentOperation{
		ID:           txID,
		Participants: participants,
		Operations:   operations,
		Coordinator:  coordinator,
		Timeout:      cac.timeoutDuration,
		CreatedAt:    time.Now(),
		Status:       StatusPending,
	}
	
	// Store active transaction
	cac.mu.Lock()
	cac.activeTransactions[txID] = crossTx
	cac.mu.Unlock()
	
	// Execute two-phase commit
	err := cac.executeTwoPhaseCommit(crossTx)
	
	// Clean up transaction
	cac.mu.Lock()
	delete(cac.activeTransactions, txID)
	cac.mu.Unlock()
	
	return crossTx, err
}

// AtomicTransfer performs an atomic value transfer between agents
func (cac *CrossAgentCoordinator) AtomicTransfer(
	from, to AgentAddress,
	amount uint64,
	coordinator AgentAddress,
) error {
	
	participants := []AgentAddress{from, to}
	operations := map[AgentAddress]AgentOperation{
		from: {
			Type: "atomic_transfer_debit",
			Data: map[string]interface{}{
				"to":     to,
				"amount": amount,
			},
			Nonce:    0, // Will be set by the chain
			GasLimit: 21000,
		},
		to: {
			Type: "atomic_transfer_credit",
			Data: map[string]interface{}{
				"from":   from,
				"amount": amount,
			},
			Nonce:    0, // Will be set by the chain
			GasLimit: 21000,
		},
	}
	
	_, err := cac.ExecuteAtomicTransaction(participants, operations, coordinator)
	return err
}

// AtomicSwap performs an atomic swap of values between agents
func (cac *CrossAgentCoordinator) AtomicSwap(
	agent1, agent2 AgentAddress,
	amount1, amount2 uint64,
	coordinator AgentAddress,
) error {
	
	participants := []AgentAddress{agent1, agent2}
	operations := map[AgentAddress]AgentOperation{
		agent1: {
			Type: "atomic_swap",
			Data: map[string]interface{}{
				"counterpart":    agent2,
				"send_amount":    amount1,
				"receive_amount": amount2,
			},
			Nonce:    0,
			GasLimit: 42000,
		},
		agent2: {
			Type: "atomic_swap",
			Data: map[string]interface{}{
				"counterpart":    agent1,
				"send_amount":    amount2,
				"receive_amount": amount1,
			},
			Nonce:    0,
			GasLimit: 42000,
		},
	}
	
	_, err := cac.ExecuteAtomicTransaction(participants, operations, coordinator)
	return err
}

// GetActiveTransactions returns all active cross-agent transactions
func (cac *CrossAgentCoordinator) GetActiveTransactions() []CrossAgentOperation {
	cac.mu.RLock()
	defer cac.mu.RUnlock()
	
	transactions := make([]CrossAgentOperation, 0, len(cac.activeTransactions))
	for _, tx := range cac.activeTransactions {
		transactions = append(transactions, *tx)
	}
	
	return transactions
}

// GetTransaction returns a specific cross-agent transaction
func (cac *CrossAgentCoordinator) GetTransaction(txID CrossAgentTransactionID) (*CrossAgentOperation, bool) {
	cac.mu.RLock()
	defer cac.mu.RUnlock()
	
	tx, exists := cac.activeTransactions[txID]
	if exists {
		return tx, true
	}
	return nil, false
}

// Private methods

func (cac *CrossAgentCoordinator) executeTwoPhaseCommit(crossTx *CrossAgentOperation) error {
	ctx, cancel := context.WithTimeout(context.Background(), crossTx.Timeout)
	defer cancel()
	
	// Phase 1: Prepare
	crossTx.Status = StatusPreparing
	if !cac.preparePhase(ctx, crossTx) {
		crossTx.Status = StatusAborting
		cac.abortPhase(ctx, crossTx)
		crossTx.Status = StatusAborted
		return fmt.Errorf("prepare phase failed")
	}
	
	crossTx.Status = StatusPrepared
	
	// Phase 2: Commit
	crossTx.Status = StatusCommitting
	if !cac.commitPhase(ctx, crossTx) {
		crossTx.Status = StatusFailed
		return fmt.Errorf("commit phase failed")
	}
	
	crossTx.Status = StatusCommitted
	return nil
}

func (cac *CrossAgentCoordinator) preparePhase(ctx context.Context, crossTx *CrossAgentOperation) bool {
	prepareResponses := make(chan PrepareResponse, len(crossTx.Participants))
	
	// Send prepare requests to all participants
	for _, participant := range crossTx.Participants {
		go func(p AgentAddress) {
			operation := crossTx.Operations[p]
			
			// Check if this is a local agent chain
			if chain, exists := cac.agentChains[p]; exists {
				// Local prepare
				response := cac.prepareLocal(chain, operation)
				prepareResponses <- response
			} else {
				// Remote prepare
				response, err := cac.networkClient.SendPrepareRequest(ctx, p, operation)
				if err != nil {
					prepareResponses <- PrepareResponse{
						Participant: p,
						Success:     false,
						Error:       err.Error(),
						Timestamp:   time.Now(),
					}
				} else {
					prepareResponses <- *response
				}
			}
		}(participant)
	}
	
	// Collect responses
	successCount := 0
	for i := 0; i < len(crossTx.Participants); i++ {
		select {
		case response := <-prepareResponses:
			if response.Success {
				successCount++
			} else {
				fmt.Printf("Prepare failed for participant %s: %s\n", response.Participant, response.Error)
			}
		case <-ctx.Done():
			fmt.Println("Prepare phase timeout")
			return false
		}
	}
	
	// All participants must be prepared
	return successCount == len(crossTx.Participants)
}

func (cac *CrossAgentCoordinator) commitPhase(ctx context.Context, crossTx *CrossAgentOperation) bool {
	commitResponses := make(chan CommitResponse, len(crossTx.Participants))
	
	// Send commit requests to all participants
	for _, participant := range crossTx.Participants {
		go func(p AgentAddress) {
			// Check if this is a local agent chain
			if chain, exists := cac.agentChains[p]; exists {
				// Local commit
				response := cac.commitLocal(chain, crossTx.ID)
				commitResponses <- response
			} else {
				// Remote commit
				response, err := cac.networkClient.SendCommitRequest(ctx, p, crossTx.ID)
				if err != nil {
					commitResponses <- CommitResponse{
						Participant: p,
						Success:     false,
						Error:       err.Error(),
						Timestamp:   time.Now(),
					}
				} else {
					commitResponses <- *response
				}
			}
		}(participant)
	}
	
	// Collect responses
	successCount := 0
	for i := 0; i < len(crossTx.Participants); i++ {
		select {
		case response := <-commitResponses:
			if response.Success {
				successCount++
			} else {
				fmt.Printf("Commit failed for participant %s: %s\n", response.Participant, response.Error)
			}
		case <-ctx.Done():
			fmt.Println("Commit phase timeout")
			return false
		}
	}
	
	// All participants must commit successfully
	return successCount == len(crossTx.Participants)
}

func (cac *CrossAgentCoordinator) abortPhase(ctx context.Context, crossTx *CrossAgentOperation) {
	// Send abort requests to all participants
	for _, participant := range crossTx.Participants {
		go func(p AgentAddress) {
			// Check if this is a local agent chain
			if chain, exists := cac.agentChains[p]; exists {
				// Local abort
				cac.abortLocal(chain, crossTx.ID)
			} else {
				// Remote abort
				cac.networkClient.SendAbortRequest(ctx, p, crossTx.ID)
			}
		}(participant)
	}
}

func (cac *CrossAgentCoordinator) prepareLocal(chain *PersonalAgentChain, operation AgentOperation) PrepareResponse {
	// Validate that the operation can be executed
	state := chain.GetState()
	
	switch operation.Type {
	case "atomic_transfer_debit":
		if amount, ok := operation.Data["amount"].(float64); ok {
			if state.Balance >= uint64(amount) {
				return PrepareResponse{
					Participant: state.Owner,
					Success:     true,
					Timestamp:   time.Now(),
				}
			}
		}
		return PrepareResponse{
			Participant: state.Owner,
			Success:     false,
			Error:       "insufficient balance",
			Timestamp:   time.Now(),
		}
		
	case "atomic_transfer_credit", "atomic_swap":
		// These operations don't require pre-validation
		return PrepareResponse{
			Participant: state.Owner,
			Success:     true,
			Timestamp:   time.Now(),
		}
		
	default:
		return PrepareResponse{
			Participant: state.Owner,
			Success:     false,
			Error:       "unknown operation type",
			Timestamp:   time.Now(),
		}
	}
}

func (cac *CrossAgentCoordinator) commitLocal(chain *PersonalAgentChain, txID CrossAgentTransactionID) CommitResponse {
	// Get the transaction operation
	cac.mu.RLock()
	crossTx, exists := cac.activeTransactions[txID]
	cac.mu.RUnlock()
	
	if !exists {
		return CommitResponse{
			Participant: chain.GetState().Owner,
			Success:     false,
			Error:       "transaction not found",
			Timestamp:   time.Now(),
		}
	}
	
	state := chain.GetState()
	operation := crossTx.Operations[state.Owner]
	
	// Create and execute the transaction
	_, err := chain.CreateTransaction(state.Owner, operation, 0)
	if err != nil {
		return CommitResponse{
			Participant: state.Owner,
			Success:     false,
			Error:       err.Error(),
			Timestamp:   time.Now(),
		}
	}
	
	// Commit the block
	_, err = chain.CommitBlock()
	if err != nil {
		return CommitResponse{
			Participant: state.Owner,
			Success:     false,
			Error:       err.Error(),
			Timestamp:   time.Now(),
		}
	}
	
	return CommitResponse{
		Participant: state.Owner,
		Success:     true,
		Timestamp:   time.Now(),
	}
}

func (cac *CrossAgentCoordinator) abortLocal(chain *PersonalAgentChain, txID CrossAgentTransactionID) {
	// Local abort - just clean up any prepared state
	// In this simplified implementation, we don't maintain prepared state
	fmt.Printf("Aborting transaction %x for local chain %s\n", txID, chain.GetState().Owner)
}

func (cac *CrossAgentCoordinator) generateTransactionID(
	participants []AgentAddress,
	operations map[AgentAddress]AgentOperation,
	coordinator AgentAddress,
) CrossAgentTransactionID {
	
	hasher := sha256.New()
	
	// Hash coordinator
	hasher.Write(coordinator[:])
	
	// Hash participants
	for _, participant := range participants {
		hasher.Write(participant[:])
	}
	
	// Hash operations
	for addr, operation := range operations {
		hasher.Write(addr[:])
		hasher.Write([]byte(operation.Type))
		
		if operationBytes, err := json.Marshal(operation.Data); err == nil {
			hasher.Write(operationBytes)
		}
	}
	
	// Hash timestamp for uniqueness
	timestampBytes := make([]byte, 8)
	binary.BigEndian.PutUint64(timestampBytes, uint64(time.Now().UnixNano()))
	hasher.Write(timestampBytes)
	
	hash := hasher.Sum(nil)
	var txID CrossAgentTransactionID
	copy(txID[:], hash)
	return txID
}

// String representation
func (txID CrossAgentTransactionID) String() string {
	return hex.EncodeToString(txID[:])
}