/*!
 * aBFT Virtual Voting Consensus
 * 
 * Asynchronous Byzantine Fault Tolerant consensus with mathematical finality proofs
 * Implements gossip-about-gossip protocol for virtual voting
 */

use crate::dag_vertex::{VertexHash, ValidatorId};
use crate::error::DAGError;
use serde::{Deserialize, Serialize};
use serde_big_array::BigArray;
use std::collections::{HashMap, HashSet};
use parking_lot::RwLock;
use std::sync::Arc;
use std::time::{SystemTime, UNIX_EPOCH};
use blake3;
use bincode;

/// Virtual voting consensus coordinator
pub struct VirtualVotingConsensus {
    /// Configuration parameters
    config: ConsensusConfig,
    /// Current validator set
    validators: Arc<RwLock<ValidatorSet>>,
    /// Virtual vote storage
    votes: Arc<RwLock<HashMap<VertexHash, VoteRecord>>>,
    /// Gossip vote storage (votes on votes)
    gossip_votes: Arc<RwLock<HashMap<VoteHash, GossipVoteRecord>>>,
    /// Finality proofs
    finality_proofs: Arc<RwLock<HashMap<VertexHash, FinalityProof>>>,
    /// Current round number
    current_round: Arc<RwLock<u64>>,
    /// Round statistics
    round_stats: Arc<RwLock<HashMap<u64, RoundStats>>>,
}

/// Configuration for consensus
#[derive(Debug, Clone)]
pub struct ConsensusConfig {
    /// Minimum number of validators required
    pub min_validators: u32,
    /// Maximum number of validators allowed
    pub max_validators: u32,
    /// Byzantine fault tolerance threshold (f = (n-1)/3)
    pub bft_threshold: f64,
    /// Timeout for consensus rounds (milliseconds)
    pub round_timeout_ms: u64,
    /// Maximum rounds to wait for finality
    pub max_finality_rounds: u32,
}

impl Default for ConsensusConfig {
    fn default() -> Self {
        Self {
            min_validators: 4,
            max_validators: 25,
            bft_threshold: 0.67, // >2/3 majority
            round_timeout_ms: 2000,
            max_finality_rounds: 10,
        }
    }
}

/// Set of validators participating in consensus
#[derive(Debug, Clone, Default)]
pub struct ValidatorSet {
    /// Active validators with their stake amounts
    pub validators: HashMap<ValidatorId, ValidatorInfo>,
    /// Total stake in the network
    pub total_stake: u64,
    /// Current epoch
    pub epoch: u64,
}

/// Information about a validator
#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct ValidatorInfo {
    /// Validator's public key
    #[serde(with = "BigArray")]
    pub public_key: [u8; 48],
    /// Staked amount (minimum 50k CS)
    pub stake: u64,
    /// Validator tier based on stake
    pub tier: ValidatorTier,
    /// Last activity timestamp
    pub last_activity: u64,
    /// Performance score (0.0 - 1.0)
    pub performance_score: f64,
}

/// Validator tiers based on stake amount
#[derive(Debug, Clone, Serialize, Deserialize, PartialEq)]
pub enum ValidatorTier {
    Bronze,   // 50k - 100k CS
    Silver,   // 100k - 250k CS  
    Gold,     // 250k - 500k CS
    Platinum, // 500k+ CS
}

/// Virtual vote on a DAG vertex
#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct VirtualVote {
    /// Validator who cast the vote
    pub validator: ValidatorId,
    /// Vertex being voted on
    pub vertex_hash: VertexHash,
    /// Vote type (approve/reject)
    pub vote_type: VoteType,
    /// Round number
    pub round: u64,
    /// Timestamp
    pub timestamp: u64,
    /// BLS signature of the vote
    #[serde(with = "BigArray")]
    pub signature: [u8; 48],
    /// Proof of stake eligibility
    pub stake_proof: StakeProof,
}

impl Default for VirtualVote {
    fn default() -> Self {
        Self {
            validator: [0u8; 32],
            vertex_hash: [0u8; 32],
            vote_type: VoteType::Approve,
            round: 0,
            timestamp: 0,
            signature: [0u8; 48],
            stake_proof: StakeProof::default(),
        }
    }
}

/// Type of vote
#[derive(Debug, Clone, Serialize, Deserialize, PartialEq)]
pub enum VoteType {
    Approve,
    Reject,
}

/// Proof of stake eligibility for voting
#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct StakeProof {
    /// Stake amount
    pub stake: u64,
    /// Merkle proof of stake
    pub merkle_proof: Vec<[u8; 32]>,
    /// Stake commitment
    pub commitment: [u8; 32],
}

impl Default for StakeProof {
    fn default() -> Self {
        Self {
            stake: 0,
            merkle_proof: Vec::new(),
            commitment: [0u8; 32],
        }
    }
}

/// Hash of a virtual vote
pub type VoteHash = [u8; 32];

/// Gossip vote (vote on a vote)
#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct GossipVote {
    /// Validator casting the gossip vote
    pub validator: ValidatorId,
    /// Original vote being witnessed
    pub original_vote_hash: VoteHash,
    /// Witness attestation
    pub witness_type: WitnessType,
    /// Round number
    pub round: u64,
    /// Timestamp
    pub timestamp: u64,
    /// BLS signature
    #[serde(with = "BigArray")]
    pub signature: [u8; 48],
}

/// Type of witness attestation
#[derive(Debug, Clone, Serialize, Deserialize, PartialEq)]
pub enum WitnessType {
    /// Witnessed the vote directly
    DirectWitness,
    /// Heard about the vote from others
    IndirectWitness,
}

/// Record of votes for a vertex
#[derive(Debug, Clone, Default)]
pub struct VoteRecord {
    /// All votes for this vertex
    pub votes: Vec<VirtualVote>,
    /// Approval votes
    pub approvals: u32,
    /// Rejection votes
    pub rejections: u32,
    /// Total stake voting
    pub total_voting_stake: u64,
    /// Consensus reached flag
    pub consensus_reached: bool,
}

/// Record of gossip votes
#[derive(Debug, Clone, Default)]
pub struct GossipVoteRecord {
    /// Original vote
    pub original_vote: VirtualVote,
    /// Gossip votes on this vote
    pub gossip_votes: Vec<GossipVote>,
    /// Direct witnesses
    pub direct_witnesses: u32,
    /// Indirect witnesses
    pub indirect_witnesses: u32,
    /// Gossip consensus reached
    pub gossip_consensus: bool,
}

/// Mathematical proof of finality
#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct FinalityProof {
    /// Vertex that achieved finality
    pub vertex_hash: VertexHash,
    /// Round in which finality was achieved
    pub finality_round: u64,
    /// Supporting virtual votes (>2/3 majority)
    pub supporting_votes: Vec<VirtualVote>,
    /// Supporting gossip votes (>2/3 witnessed)
    pub witness_votes: Vec<GossipVote>,
    /// BFT safety proof
    pub bft_proof: BFTSafetyProof,
    /// Timestamp of finality
    pub finality_timestamp: u64,
}

/// BFT safety mathematical proof
#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct BFTSafetyProof {
    /// Total number of validators (n)
    pub total_validators: u32,
    /// Maximum Byzantine faults tolerated (f = (n-1)/3)
    pub max_byzantine_faults: u32,
    /// Number of supporting votes (must be > 2f+1)
    pub supporting_vote_count: u32,
    /// Total stake supporting (must be > 2/3)
    pub supporting_stake: u64,
    /// Total network stake
    pub total_stake: u64,
    /// Safety condition satisfied
    pub safety_satisfied: bool,
}

/// Round statistics
#[derive(Debug, Clone, Default)]
pub struct RoundStats {
    /// Number of vertices proposed
    pub vertices_proposed: u32,
    /// Number of vertices finalized
    pub vertices_finalized: u32,
    /// Number of active validators
    pub active_validators: u32,
    /// Round duration in milliseconds
    pub duration_ms: u64,
    /// Average consensus time
    pub avg_consensus_time_ms: u64,
}

impl VirtualVotingConsensus {
    /// Create new consensus coordinator
    pub fn new(config: ConsensusConfig) -> Self {
        Self {
            config,
            validators: Arc::new(RwLock::new(ValidatorSet::default())),
            votes: Arc::new(RwLock::new(HashMap::new())),
            gossip_votes: Arc::new(RwLock::new(HashMap::new())),
            finality_proofs: Arc::new(RwLock::new(HashMap::new())),
            current_round: Arc::new(RwLock::new(1)),
            round_stats: Arc::new(RwLock::new(HashMap::new())),
        }
    }

    /// Process consensus round for given vertices
    pub async fn process_round(&mut self, vertices: Vec<VertexHash>) -> Result<Vec<FinalityProof>, DAGError> {
        let round = {
            let mut current_round = self.current_round.write();
            *current_round += 1;
            *current_round
        };

        let start_time = SystemTime::now().duration_since(UNIX_EPOCH).unwrap().as_millis() as u64;
        let mut finality_proofs = Vec::new();

        // Initialize round statistics
        {
            let mut stats = self.round_stats.write();
            stats.insert(round, RoundStats {
                vertices_proposed: vertices.len() as u32,
                ..Default::default()
            });
        }

        // Phase 1: Collect virtual votes for each vertex
        for vertex_hash in &vertices {
            self.collect_virtual_votes(*vertex_hash, round).await?;
        }

        // Phase 2: Process gossip-about-gossip
        self.process_gossip_votes(round).await?;

        // Phase 3: Check for mathematical finality
        for vertex_hash in vertices {
            if let Some(proof) = self.check_finality(vertex_hash, round).await? {
                finality_proofs.push(proof);
            }
        }

        // Update round statistics
        let end_time = SystemTime::now().duration_since(UNIX_EPOCH).unwrap().as_millis() as u64;
        {
            let mut stats = self.round_stats.write();
            if let Some(round_stat) = stats.get_mut(&round) {
                round_stat.vertices_finalized = finality_proofs.len() as u32;
                round_stat.active_validators = self.validators.read().validators.len() as u32;
                round_stat.duration_ms = end_time - start_time;
                round_stat.avg_consensus_time_ms = round_stat.duration_ms / round_stat.vertices_proposed.max(1) as u64;
            }
        }

        Ok(finality_proofs)
    }

    /// Collect virtual votes from validators
    async fn collect_virtual_votes(&self, vertex_hash: VertexHash, round: u64) -> Result<(), DAGError> {
        let validators = self.validators.read();
        let mut vote_record = VoteRecord::default();

        // Simulate virtual voting (in production, would collect from network)
        for (validator_id, validator_info) in &validators.validators {
            // Simulate validator decision (in production, would be actual validation)
            let vote_type = if self.validate_vertex_by_validator(&vertex_hash, validator_id).await {
                VoteType::Approve
            } else {
                VoteType::Reject
            };

            let vote = VirtualVote {
                validator: *validator_id,
                vertex_hash,
                vote_type: vote_type.clone(),
                round,
                timestamp: SystemTime::now().duration_since(UNIX_EPOCH).unwrap().as_millis() as u64,
                signature: [0; 48], // Would be actual BLS signature
                stake_proof: StakeProof {
                    stake: validator_info.stake,
                    merkle_proof: vec![], // Would be actual Merkle proof
                    commitment: [0; 32],
                },
            };

            vote_record.votes.push(vote);
            vote_record.total_voting_stake += validator_info.stake;

            match vote_type {
                VoteType::Approve => vote_record.approvals += 1,
                VoteType::Reject => vote_record.rejections += 1,
            }
        }

        // Check if consensus is reached (>2/3 majority)
        let total_validators = validators.validators.len() as u32;
        let required_votes = (total_validators * 2 / 3) + 1;
        let required_stake = (validators.total_stake * 2 / 3) + 1;

        vote_record.consensus_reached = vote_record.approvals >= required_votes &&
                                       vote_record.total_voting_stake >= required_stake;

        // Store vote record
        {
            let mut votes = self.votes.write();
            votes.insert(vertex_hash, vote_record);
        }

        Ok(())
    }

    /// Process gossip-about-gossip votes
    async fn process_gossip_votes(&self, round: u64) -> Result<(), DAGError> {
        let votes = self.votes.read();
        let validators = self.validators.read();

        for (vertex_hash, vote_record) in votes.iter() {
            for vote in &vote_record.votes {
                let vote_hash = self.calculate_vote_hash(vote);
                let mut gossip_record = GossipVoteRecord {
                    original_vote: vote.clone(),
                    gossip_votes: Vec::new(),
                    direct_witnesses: 0,
                    indirect_witnesses: 0,
                    gossip_consensus: false,
                };

                // Collect gossip votes from other validators
                for (validator_id, _) in &validators.validators {
                    if *validator_id == vote.validator {
                        continue; // Skip self
                    }

                    // Simulate witnessing (in production, would be actual network gossip)
                    let witness_type = if self.validator_witnesses_vote(*validator_id, &vote_hash).await {
                        WitnessType::DirectWitness
                    } else {
                        WitnessType::IndirectWitness
                    };

                    let gossip_vote = GossipVote {
                        validator: *validator_id,
                        original_vote_hash: vote_hash,
                        witness_type: witness_type.clone(),
                        round,
                        timestamp: SystemTime::now().duration_since(UNIX_EPOCH).unwrap().as_millis() as u64,
                        signature: [0; 48], // Would be actual BLS signature
                    };

                    gossip_record.gossip_votes.push(gossip_vote);

                    match witness_type {
                        WitnessType::DirectWitness => gossip_record.direct_witnesses += 1,
                        WitnessType::IndirectWitness => gossip_record.indirect_witnesses += 1,
                    }
                }

                // Check gossip consensus (>2/3 witnesses)
                let total_witnesses = gossip_record.direct_witnesses + gossip_record.indirect_witnesses;
                let required_witnesses = (validators.validators.len() as u32 * 2 / 3) + 1;
                gossip_record.gossip_consensus = total_witnesses >= required_witnesses;

                // Store gossip record
                {
                    let mut gossip_votes = self.gossip_votes.write();
                    gossip_votes.insert(vote_hash, gossip_record);
                }
            }
        }

        Ok(())
    }

    /// Check if vertex achieved mathematical finality
    async fn check_finality(&self, vertex_hash: VertexHash, round: u64) -> Result<Option<FinalityProof>, DAGError> {
        let votes = self.votes.read();
        let gossip_votes = self.gossip_votes.read();
        let validators = self.validators.read();

        // Get vote record for this vertex
        let vote_record = match votes.get(&vertex_hash) {
            Some(record) => record,
            None => return Ok(None),
        };

        // Check if consensus was reached
        if !vote_record.consensus_reached {
            return Ok(None);
        }

        // Collect supporting votes and gossip evidence
        let mut supporting_votes = Vec::new();
        let mut witness_votes = Vec::new();

        for vote in &vote_record.votes {
            if vote.vote_type == VoteType::Approve {
                supporting_votes.push(vote.clone());

                // Find corresponding gossip votes
                let vote_hash = self.calculate_vote_hash(vote);
                if let Some(gossip_record) = gossip_votes.get(&vote_hash) {
                    if gossip_record.gossip_consensus {
                        witness_votes.extend(gossip_record.gossip_votes.clone());
                    }
                }
            }
        }

        // Create BFT safety proof
        let total_validators = validators.validators.len() as u32;
        let max_byzantine_faults = (total_validators.saturating_sub(1)) / 3;
        let required_votes = (2 * max_byzantine_faults) + 1;

        let bft_proof = BFTSafetyProof {
            total_validators,
            max_byzantine_faults,
            supporting_vote_count: supporting_votes.len() as u32,
            supporting_stake: vote_record.total_voting_stake,
            total_stake: validators.total_stake,
            safety_satisfied: supporting_votes.len() as u32 >= required_votes &&
                            vote_record.total_voting_stake > (validators.total_stake * 2 / 3),
        };

        // Only create finality proof if BFT safety is satisfied
        if bft_proof.safety_satisfied {
            let finality_proof = FinalityProof {
                vertex_hash,
                finality_round: round,
                supporting_votes,
                witness_votes,
                bft_proof,
                finality_timestamp: SystemTime::now().duration_since(UNIX_EPOCH).unwrap().as_millis() as u64,
            };

            // Store finality proof
            {
                let mut proofs = self.finality_proofs.write();
                proofs.insert(vertex_hash, finality_proof.clone());
            }

            Ok(Some(finality_proof))
        } else {
            Ok(None)
        }
    }

    /// Add validator to the set
    pub fn add_validator(&self, validator_id: ValidatorId, validator_info: ValidatorInfo) {
        let mut validators = self.validators.write();
        validators.total_stake += validator_info.stake;
        validators.validators.insert(validator_id, validator_info);
    }

    /// Remove validator from the set
    pub fn remove_validator(&self, validator_id: &ValidatorId) {
        let mut validators = self.validators.write();
        if let Some(info) = validators.validators.remove(validator_id) {
            validators.total_stake = validators.total_stake.saturating_sub(info.stake);
        }
    }

    /// Get current round count
    pub fn get_round_count(&self) -> u64 {
        *self.current_round.read()
    }

    /// Get finality proof for vertex
    pub fn get_finality_proof(&self, vertex_hash: &VertexHash) -> Option<FinalityProof> {
        self.finality_proofs.read().get(vertex_hash).cloned()
    }

    /// Calculate hash of a vote
    fn calculate_vote_hash(&self, vote: &VirtualVote) -> VoteHash {
        let mut hasher = blake3::Hasher::new();
        hasher.update(&bincode::serialize(vote).unwrap_or_default());
        let hash = hasher.finalize();
        let mut result = [0u8; 32];
        result.copy_from_slice(hash.as_bytes());
        result
    }

    /// Simulate validator vertex validation (in production, would be actual validation)
    async fn validate_vertex_by_validator(&self, _vertex_hash: &VertexHash, _validator_id: &ValidatorId) -> bool {
        // Simplified validation - in production would perform actual vertex validation
        // Including signature verification, transaction validity, etc.
        true // Assume valid for simulation
    }

    /// Simulate validator witnessing a vote (in production, would check network gossip)
    async fn validator_witnesses_vote(&self, _validator_id: ValidatorId, _vote_hash: &VoteHash) -> bool {
        // Simplified witnessing - in production would check if validator received vote via gossip
        true // Assume witnessed for simulation
    }
}

impl ValidatorInfo {
    /// Create new validator info
    pub fn new(public_key: [u8; 48], stake: u64) -> Self {
        let tier = match stake {
            50_000..=99_999 => ValidatorTier::Bronze,
            100_000..=249_999 => ValidatorTier::Silver,
            250_000..=499_999 => ValidatorTier::Gold,
            500_000.. => ValidatorTier::Platinum,
            _ => ValidatorTier::Bronze, // Minimum stake
        };

        Self {
            public_key,
            stake,
            tier,
            last_activity: SystemTime::now().duration_since(UNIX_EPOCH).unwrap().as_millis() as u64,
            performance_score: 1.0,
        }
    }

    /// Update validator performance score
    pub fn update_performance(&mut self, new_score: f64) {
        self.performance_score = new_score.clamp(0.0, 1.0);
        self.last_activity = SystemTime::now().duration_since(UNIX_EPOCH).unwrap().as_millis() as u64;
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[tokio::test]
    async fn test_consensus_creation() {
        let config = ConsensusConfig::default();
        let consensus = VirtualVotingConsensus::new(config);
        assert_eq!(consensus.get_round_count(), 1);
    }

    #[tokio::test]
    async fn test_validator_management() {
        let consensus = VirtualVotingConsensus::new(ConsensusConfig::default());
        
        let validator_id = [1; 32];
        let validator_info = ValidatorInfo::new([1; 48], 100_000);
        
        consensus.add_validator(validator_id, validator_info);
        
        let validators = consensus.validators.read();
        assert_eq!(validators.validators.len(), 1);
        assert_eq!(validators.total_stake, 100_000);
    }

    #[tokio::test]
    async fn test_consensus_round() {
        let mut consensus = VirtualVotingConsensus::new(ConsensusConfig::default());
        
        // Add validators
        for i in 0..5 {
            let validator_id = [i; 32];
            let validator_info = ValidatorInfo::new([i; 48], 100_000);
            consensus.add_validator(validator_id, validator_info);
        }
        
        // Process consensus round
        let vertices = vec![[1; 32], [2; 32]];
        let proofs = consensus.process_round(vertices).await.unwrap();
        
        // Should achieve finality for both vertices with 5 validators
        assert_eq!(proofs.len(), 2);
        assert_eq!(consensus.get_round_count(), 2);
    }

    #[test]
    fn test_validator_tiers() {
        let bronze = ValidatorInfo::new([1; 48], 75_000);
        assert_eq!(bronze.tier, ValidatorTier::Bronze);
        
        let silver = ValidatorInfo::new([2; 48], 150_000);
        assert_eq!(silver.tier, ValidatorTier::Silver);
        
        let gold = ValidatorInfo::new([3; 48], 350_000);
        assert_eq!(gold.tier, ValidatorTier::Gold);
        
        let platinum = ValidatorInfo::new([4; 48], 750_000);
        assert_eq!(platinum.tier, ValidatorTier::Platinum);
    }

    #[test]
    fn test_bft_safety_proof() {
        let proof = BFTSafetyProof {
            total_validators: 10,
            max_byzantine_faults: 3,
            supporting_vote_count: 7,
            supporting_stake: 700_000,
            total_stake: 1_000_000,
            safety_satisfied: true,
        };
        
        // Should satisfy BFT conditions
        assert!(proof.supporting_vote_count > 2 * proof.max_byzantine_faults);
        assert!(proof.supporting_stake > proof.total_stake * 2 / 3);
    }
}