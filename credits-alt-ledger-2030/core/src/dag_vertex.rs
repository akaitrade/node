/*!
 * DAG Vertex Implementation
 * 
 * Core data structure representing individual transactions in the DAG
 */

use serde::{Deserialize, Serialize};
use serde_big_array::BigArray;
use blake3;
use std::fmt;

pub type VertexHash = [u8; 32];
pub type TransactionHash = [u8; 32];
pub type ValidatorId = [u8; 32];
pub type ShardId = u32;

/// DAG Vertex representing a transaction with parent references
#[derive(Debug, Clone, Serialize, Deserialize, PartialEq)]
pub struct DAGVertex {
    /// Unique hash of this vertex
    pub hash: VertexHash,
    /// Transaction hash from the original transaction
    pub tx_hash: TransactionHash,
    /// Logical clock (Lamport timestamp) for ordering
    pub logical_clock: u64,
    /// Parent vertex hashes (≥2 for DAG property, except genesis)
    pub parents: Vec<VertexHash>,
    /// Shard assignment for dynamic sharding
    pub shard_id: ShardId,
    /// Transaction data
    pub transaction_data: TransactionData,
    /// BLS aggregate signature
    pub signature: BLSSignature,
    /// Timestamp when vertex was created
    pub timestamp: u64,
    /// Proof data for verification
    pub proof: Option<ZKProof>,
}

/// Transaction data embedded in DAG vertex
#[derive(Debug, Clone, Serialize, Deserialize, PartialEq)]
pub struct TransactionData {
    /// Source address (32-byte)
    pub source: [u8; 32],
    /// Target address (32-byte)
    pub target: [u8; 32],
    /// Amount being transferred
    pub amount: u64,
    /// Currency type (1 = CS, others for tokens)
    pub currency: u32,
    /// Gas fee for execution
    pub fee: u64,
    /// Nonce for replay protection
    pub nonce: u64,
    /// Additional user data (ordinal inscriptions, etc.)
    pub user_data: Vec<u8>,
}

/// BLS signature for aggregate verification
#[derive(Debug, Clone, Serialize, Deserialize, PartialEq)]
pub struct BLSSignature {
    /// Signature bytes (48 bytes for BLS12-381)
    #[serde(with = "BigArray")]
    pub signature: [u8; 48],
    /// Public key that signed (48 bytes compressed)
    #[serde(with = "BigArray")]
    pub public_key: [u8; 48],
    /// Aggregate signature info
    pub aggregate_info: Option<AggregateInfo>,
}

/// Information about signature aggregation
#[derive(Debug, Clone, Serialize, Deserialize, PartialEq)]
pub struct AggregateInfo {
    /// Number of signatures aggregated
    pub signature_count: u32,
    /// Bitmap of participating validators
    pub participant_bitmap: Vec<u8>,
}

/// Zero-knowledge proof for privacy/verification
#[derive(Debug, Clone, Serialize, Deserialize, PartialEq)]
pub struct ZKProof {
    /// Proof data
    pub proof: Vec<u8>,
    /// Public inputs
    pub public_inputs: Vec<u8>,
    /// Verification key hash
    pub vk_hash: [u8; 32],
}

impl DAGVertex {
    /// Create a new DAG vertex
    pub fn new(
        tx_hash: TransactionHash,
        logical_clock: u64,
        parents: Vec<VertexHash>,
        shard_id: ShardId,
        transaction_data: TransactionData,
        signature: BLSSignature,
    ) -> Self {
        let timestamp = std::time::SystemTime::now()
            .duration_since(std::time::UNIX_EPOCH)
            .unwrap()
            .as_millis() as u64;

        let mut vertex = Self {
            hash: [0; 32],
            tx_hash,
            logical_clock,
            parents,
            shard_id,
            transaction_data,
            signature,
            timestamp,
            proof: None,
        };

        vertex.hash = vertex.calculate_hash();
        vertex
    }

    /// Create genesis vertex (no parents)
    pub fn genesis() -> Self {
        let genesis_data = TransactionData {
            source: [0; 32],
            target: [0; 32],
            amount: 0,
            currency: 1,
            fee: 0,
            nonce: 0,
            user_data: b"CREDITS ALT-LEDGER 2030 Genesis".to_vec(),
        };

        let genesis_signature = BLSSignature {
            signature: [0; 48],
            public_key: [0; 48],
            aggregate_info: None,
        };

        Self::new(
            [0; 32], // Genesis tx hash
            0,       // Genesis logical clock
            vec![],  // No parents
            0,       // Shard 0
            genesis_data,
            genesis_signature,
        )
    }

    /// Calculate vertex hash using BLAKE3
    pub fn calculate_hash(&self) -> VertexHash {
        let mut hasher = blake3::Hasher::new();
        
        // Hash core vertex data (excluding the hash field itself)
        hasher.update(&self.tx_hash);
        hasher.update(&self.logical_clock.to_be_bytes());
        hasher.update(&self.shard_id.to_be_bytes());
        hasher.update(&self.timestamp.to_be_bytes());
        
        // Hash parent references
        for parent in &self.parents {
            hasher.update(parent);
        }
        
        // Hash transaction data
        hasher.update(&bincode::serialize(&self.transaction_data).unwrap_or_default());
        
        // Hash signature
        hasher.update(&self.signature.signature);
        hasher.update(&self.signature.public_key);
        
        let hash = hasher.finalize();
        let mut result = [0u8; 32];
        result.copy_from_slice(hash.as_bytes());
        result
    }

    /// Verify vertex signature
    pub fn verify_signature(&self) -> bool {
        // Simplified verification - in production would use actual BLS verification
        // Would verify that signature.public_key signed the vertex data
        !self.signature.signature.iter().all(|&b| b == 0)
    }

    /// Get vertex size in bytes for storage calculations
    pub fn serialized_size(&self) -> usize {
        bincode::serialized_size(self).unwrap_or(0) as usize
    }

    /// Check if this is a genesis vertex
    pub fn is_genesis(&self) -> bool {
        self.parents.is_empty() && self.logical_clock == 0
    }

    /// Get parent count
    pub fn parent_count(&self) -> usize {
        self.parents.len()
    }

    /// Validate DAG properties
    pub fn validate_dag_properties(&self) -> Result<(), String> {
        // Genesis vertex can have no parents
        if self.is_genesis() {
            return Ok(());
        }

        // Non-genesis vertices must have at least 2 parents for DAG property
        if self.parents.len() < 2 {
            return Err(format!("Non-genesis vertex must have ≥2 parents, found {}", self.parents.len()));
        }

        // Parents must be unique
        let mut sorted_parents = self.parents.clone();
        sorted_parents.sort();
        sorted_parents.dedup();
        if sorted_parents.len() != self.parents.len() {
            return Err("Duplicate parent references found".to_string());
        }

        // Logical clock must be positive for non-genesis
        if self.logical_clock == 0 {
            return Err("Non-genesis vertex must have positive logical clock".to_string());
        }

        Ok(())
    }
}

impl TransactionData {
    /// Create new transaction data
    pub fn new(
        source: [u8; 32],
        target: [u8; 32],
        amount: u64,
        currency: u32,
        fee: u64,
        nonce: u64,
        user_data: Vec<u8>,
    ) -> Self {
        Self {
            source,
            target,
            amount,
            currency,
            fee,
            nonce,
            user_data,
        }
    }

    /// Check if this is a CNS (Credits Name System) transaction
    pub fn is_cns_transaction(&self) -> bool {
        // Check if user_data contains CNS inscription
        if self.user_data.is_empty() {
            return false;
        }

        // Try to parse as JSON to detect CNS operations
        if let Ok(json_str) = std::str::from_utf8(&self.user_data) {
            return json_str.contains("\"p\":\"cns\"") || json_str.contains("\"p\":\"cdns\"");
        }

        false
    }

    /// Check if this is an ordinal token transaction
    pub fn is_ordinal_transaction(&self) -> bool {
        if self.user_data.is_empty() {
            return false;
        }

        if let Ok(json_str) = std::str::from_utf8(&self.user_data) {
            return json_str.contains("\"op\":\"mint\"") || json_str.contains("\"op\":\"deploy\"");
        }

        false
    }

    /// Get transaction value including fee
    pub fn total_value(&self) -> u64 {
        self.amount.saturating_add(self.fee)
    }
}

impl BLSSignature {
    /// Create new BLS signature
    pub fn new(signature: [u8; 48], public_key: [u8; 48]) -> Self {
        Self {
            signature,
            public_key,
            aggregate_info: None,
        }
    }

    /// Create aggregate signature
    pub fn aggregate(signatures: Vec<BLSSignature>) -> Self {
        // Simplified aggregation - in production would use actual BLS aggregation
        let mut aggregate_sig = [0u8; 48];
        let mut aggregate_pk = [0u8; 48];
        
        for (i, sig) in signatures.iter().enumerate().take(48) {
            aggregate_sig[i] = sig.signature[i];
            aggregate_pk[i] = sig.public_key[i];
        }

        Self {
            signature: aggregate_sig,
            public_key: aggregate_pk,
            aggregate_info: Some(AggregateInfo {
                signature_count: signatures.len() as u32,
                participant_bitmap: vec![0xFF; (signatures.len() + 7) / 8],
            }),
        }
    }

    /// Check if this is an aggregate signature
    pub fn is_aggregate(&self) -> bool {
        self.aggregate_info.is_some()
    }
}

impl fmt::Display for DAGVertex {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(
            f,
            "DAGVertex(hash: {:?}, clock: {}, parents: {}, shard: {})",
            &self.hash[..8],
            self.logical_clock,
            self.parents.len(),
            self.shard_id
        )
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn test_genesis_vertex() {
        let genesis = DAGVertex::genesis();
        assert!(genesis.is_genesis());
        assert_eq!(genesis.logical_clock, 0);
        assert_eq!(genesis.parents.len(), 0);
        assert!(genesis.validate_dag_properties().is_ok());
    }

    #[test]
    fn test_vertex_hash_consistency() {
        let tx_data = TransactionData::new(
            [1; 32], [2; 32], 100, 1, 10, 1, vec![1, 2, 3]
        );
        let signature = BLSSignature::new([1; 48], [2; 48]);
        
        let vertex1 = DAGVertex::new([3; 32], 1, vec![[0; 32]], 0, tx_data.clone(), signature.clone());
        let vertex2 = DAGVertex::new([3; 32], 1, vec![[0; 32]], 0, tx_data, signature);
        
        assert_eq!(vertex1.hash, vertex2.hash);
    }

    #[test]
    fn test_dag_properties_validation() {
        let tx_data = TransactionData::new([1; 32], [2; 32], 100, 1, 10, 1, vec![]);
        let signature = BLSSignature::new([1; 48], [2; 48]);
        
        // Valid vertex with 2 parents
        let vertex = DAGVertex::new([3; 32], 1, vec![[0; 32], [1; 32]], 0, tx_data.clone(), signature.clone());
        assert!(vertex.validate_dag_properties().is_ok());
        
        // Invalid vertex with only 1 parent
        let invalid_vertex = DAGVertex::new([3; 32], 1, vec![[0; 32]], 0, tx_data, signature);
        assert!(invalid_vertex.validate_dag_properties().is_err());
    }

    #[test]
    fn test_cns_transaction_detection() {
        let cns_data = br#"{"p":"cns","op":"reg","cns":"alice","relay":"https://alice.credits"}"#;
        let tx_data = TransactionData::new([1; 32], [2; 32], 0, 1, 10, 1, cns_data.to_vec());
        
        assert!(tx_data.is_cns_transaction());
        assert!(!tx_data.is_ordinal_transaction());
    }

    #[test]
    fn test_ordinal_transaction_detection() {
        let ordinal_data = br#"{"p":"crc-20","op":"mint","tick":"CREDS","amt":"1000"}"#;
        let tx_data = TransactionData::new([1; 32], [2; 32], 0, 1, 10, 1, ordinal_data.to_vec());
        
        assert!(!tx_data.is_cns_transaction());
        assert!(tx_data.is_ordinal_transaction());
    }
}