/*!
 * Dynamic Sharding Coordinator
 * 
 * Implements CNS namespace-based sharding with automatic load balancing
 */

use crate::dag_vertex::{DAGVertex, ShardId};
use crate::error::DAGError;
use serde::{Deserialize, Serialize};
use std::collections::{HashMap, HashSet};
use std::sync::Arc;
use parking_lot::RwLock;
use blake3;

/// Dynamic shard coordinator
pub struct ShardCoordinator {
    /// Current shard configuration
    config: ShardConfig,
    /// Active shards
    shards: Arc<RwLock<HashMap<ShardId, ShardInfo>>>,
    /// Shard load statistics
    load_stats: Arc<RwLock<HashMap<ShardId, ShardLoadStats>>>,
    /// CNS namespace assignments
    namespace_assignments: Arc<RwLock<HashMap<String, ShardId>>>,
    /// Next available shard ID
    next_shard_id: Arc<RwLock<ShardId>>,
}

/// Shard configuration
#[derive(Debug, Clone)]
pub struct ShardConfig {
    /// Initial number of shards
    pub initial_shard_count: u32,
    /// Maximum TPS per shard before splitting
    pub max_shard_tps: u32,
    /// Minimum TPS per shard before merging
    pub min_shard_tps: u32,
    /// Maximum number of shards
    pub max_shard_count: u32,
    /// Rebalancing interval in seconds
    pub rebalance_interval_secs: u64,
}

impl Default for ShardConfig {
    fn default() -> Self {
        Self {
            initial_shard_count: 4,
            max_shard_tps: 10_000,
            min_shard_tps: 1_000,
            max_shard_count: 1024,
            rebalance_interval_secs: 300, // 5 minutes
        }
    }
}

/// Information about a shard
#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct ShardInfo {
    /// Shard ID
    pub shard_id: ShardId,
    /// CNS namespaces assigned to this shard
    pub namespaces: HashSet<String>,
    /// Hash range (for consistent hashing)
    pub hash_range: HashRange,
    /// Creation timestamp
    pub created_at: u64,
    /// Parent shard (if split from another)
    pub parent_shard: Option<ShardId>,
    /// Status
    pub status: ShardStatus,
}

/// Hash range for consistent hashing
#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct HashRange {
    /// Start of hash range (inclusive)
    pub start: u64,
    /// End of hash range (exclusive)
    pub end: u64,
}

/// Shard status
#[derive(Debug, Clone, Serialize, Deserialize, PartialEq)]
pub enum ShardStatus {
    Active,
    Splitting,
    Merging,
    Inactive,
}

/// Load statistics for a shard
#[derive(Debug, Clone, Default)]
pub struct ShardLoadStats {
    /// Transactions per second (TPS)
    pub current_tps: f64,
    /// Average TPS over time window
    pub average_tps: f64,
    /// Peak TPS observed
    pub peak_tps: f64,
    /// Number of vertices in shard
    pub vertex_count: u64,
    /// Total storage size in bytes
    pub storage_size: u64,
    /// Last update timestamp
    pub last_update: u64,
    /// Historical TPS samples
    pub tps_history: Vec<TPSSample>,
}

/// TPS measurement sample
#[derive(Debug, Clone)]
pub struct TPSSample {
    pub timestamp: u64,
    pub tps: f64,
}

/// Shard rebalancing operation
#[derive(Debug, Clone)]
pub enum ShardOperation {
    Split {
        shard_id: ShardId,
        new_shard_ids: Vec<ShardId>,
    },
    Merge {
        shard_ids: Vec<ShardId>,
        new_shard_id: ShardId,
    },
    Reassign {
        namespace: String,
        from_shard: ShardId,
        to_shard: ShardId,
    },
}

impl ShardCoordinator {
    /// Create new shard coordinator
    pub fn new(config: ShardConfig) -> Self {
        let mut shards = HashMap::new();
        let mut load_stats = HashMap::new();
        
        // Initialize with default shards
        for i in 0..config.initial_shard_count {
            let shard_info = ShardInfo {
                shard_id: i,
                namespaces: HashSet::new(),
                hash_range: HashRange {
                    start: (u64::MAX / config.initial_shard_count as u64) * i as u64,
                    end: (u64::MAX / config.initial_shard_count as u64) * (i + 1) as u64,
                },
                created_at: std::time::SystemTime::now()
                    .duration_since(std::time::UNIX_EPOCH)
                    .unwrap()
                    .as_secs(),
                parent_shard: None,
                status: ShardStatus::Active,
            };
            
            shards.insert(i, shard_info);
            load_stats.insert(i, ShardLoadStats::default());
        }

        let initial_shard_count = config.initial_shard_count;
        
        Self {
            config,
            shards: Arc::new(RwLock::new(shards)),
            load_stats: Arc::new(RwLock::new(load_stats)),
            namespace_assignments: Arc::new(RwLock::new(HashMap::new())),
            next_shard_id: Arc::new(RwLock::new(initial_shard_count)),
        }
    }

    /// Assign shard for a DAG vertex based on CNS namespace
    pub fn assign_shard(&self, vertex: &DAGVertex) -> ShardId {
        // Extract CNS namespace from transaction data
        let namespace = self.extract_cns_namespace(vertex);
        
        // Check if namespace already has an assignment
        {
            let assignments = self.namespace_assignments.read();
            if let Some(&assigned_shard) = assignments.get(&namespace) {
                return assigned_shard;
            }
        }

        // Calculate consistent hash for namespace
        let hash = self.calculate_namespace_hash(&namespace);
        let shard_id = self.find_shard_for_hash(hash);
        
        // Store assignment
        {
            let mut assignments = self.namespace_assignments.write();
            assignments.insert(namespace.clone(), shard_id);
        }

        // Update shard info
        {
            let mut shards = self.shards.write();
            if let Some(shard) = shards.get_mut(&shard_id) {
                shard.namespaces.insert(namespace);
            }
        }

        shard_id
    }

    /// Update load statistics for a shard
    pub fn update_load_stats(&self, shard_id: ShardId, tps: f64) {
        let timestamp = std::time::SystemTime::now()
            .duration_since(std::time::UNIX_EPOCH)
            .unwrap()
            .as_secs();

        let mut load_stats = self.load_stats.write();
        if let Some(stats) = load_stats.get_mut(&shard_id) {
            stats.current_tps = tps;
            stats.peak_tps = stats.peak_tps.max(tps);
            stats.last_update = timestamp;
            
            // Add to history
            stats.tps_history.push(TPSSample { timestamp, tps });
            
            // Keep only recent history (last hour)
            let cutoff = timestamp.saturating_sub(3600);
            stats.tps_history.retain(|sample| sample.timestamp >= cutoff);
            
            // Calculate average
            if !stats.tps_history.is_empty() {
                stats.average_tps = stats.tps_history.iter().map(|s| s.tps).sum::<f64>() 
                    / stats.tps_history.len() as f64;
            }
        }
    }

    /// Check if rebalancing is needed and return operations
    pub fn check_rebalancing(&self) -> Vec<ShardOperation> {
        let load_stats = self.load_stats.read();
        let mut operations = Vec::new();

        // Check for shards that need splitting
        for (&shard_id, stats) in load_stats.iter() {
            if stats.average_tps > self.config.max_shard_tps as f64 {
                if let Some(split_ops) = self.plan_shard_split(shard_id) {
                    operations.extend(split_ops);
                }
            }
        }

        // Check for shards that can be merged
        let low_load_shards: Vec<_> = load_stats.iter()
            .filter(|(_, stats)| stats.average_tps < self.config.min_shard_tps as f64)
            .map(|(&shard_id, _)| shard_id)
            .collect();

        if low_load_shards.len() >= 2 {
            if let Some(merge_op) = self.plan_shard_merge(low_load_shards) {
                operations.push(merge_op);
            }
        }

        operations
    }

    /// Execute shard rebalancing operations
    pub async fn execute_rebalancing(&self, operations: Vec<ShardOperation>) -> Result<(), DAGError> {
        for operation in operations {
            match operation {
                ShardOperation::Split { shard_id, new_shard_ids } => {
                    self.execute_split(shard_id, new_shard_ids).await?;
                }
                ShardOperation::Merge { shard_ids, new_shard_id } => {
                    self.execute_merge(shard_ids, new_shard_id).await?;
                }
                ShardOperation::Reassign { namespace, from_shard, to_shard } => {
                    self.execute_reassignment(namespace, from_shard, to_shard).await?;
                }
            }
        }
        Ok(())
    }

    /// Get active shard count
    pub fn get_active_shard_count(&self) -> u32 {
        let shards = self.shards.read();
        shards.values().filter(|s| s.status == ShardStatus::Active).count() as u32
    }

    /// Get shard information
    pub fn get_shard_info(&self, shard_id: ShardId) -> Option<ShardInfo> {
        self.shards.read().get(&shard_id).cloned()
    }

    /// Get load statistics for shard
    pub fn get_load_stats(&self, shard_id: ShardId) -> Option<ShardLoadStats> {
        self.load_stats.read().get(&shard_id).cloned()
    }

    /// Extract CNS namespace from vertex transaction data
    fn extract_cns_namespace(&self, vertex: &DAGVertex) -> String {
        // Check if transaction contains CNS data
        if vertex.transaction_data.is_cns_transaction() {
            if let Ok(json_str) = std::str::from_utf8(&vertex.transaction_data.user_data) {
                // Parse JSON to extract namespace
                if let Ok(json_val) = serde_json::from_str::<serde_json::Value>(json_str) {
                    if let Some(namespace) = json_val.get("p").and_then(|v| v.as_str()) {
                        return namespace.to_string();
                    }
                }
            }
        }

        // Default namespace for non-CNS transactions
        "default".to_string()
    }

    /// Calculate consistent hash for namespace
    fn calculate_namespace_hash(&self, namespace: &str) -> u64 {
        let mut hasher = blake3::Hasher::new();
        hasher.update(namespace.as_bytes());
        let hash = hasher.finalize();
        
        // Convert first 8 bytes to u64
        let mut bytes = [0u8; 8];
        bytes.copy_from_slice(&hash.as_bytes()[0..8]);
        u64::from_be_bytes(bytes)
    }

    /// Find shard for given hash value
    fn find_shard_for_hash(&self, hash: u64) -> ShardId {
        let shards = self.shards.read();
        
        for (&shard_id, shard_info) in shards.iter() {
            if shard_info.status == ShardStatus::Active &&
               hash >= shard_info.hash_range.start &&
               hash < shard_info.hash_range.end {
                return shard_id;
            }
        }
        
        // Fallback to shard 0 if no match found
        0
    }

    /// Plan shard split operation
    fn plan_shard_split(&self, shard_id: ShardId) -> Option<Vec<ShardOperation>> {
        let shards = self.shards.read();
        if shards.len() >= self.config.max_shard_count as usize {
            return None; // Already at maximum shard count
        }

        let shard = shards.get(&shard_id)?;
        if shard.status != ShardStatus::Active {
            return None;
        }

        // Reserve two new shard IDs atomically
        let new_shard_ids = {
            let mut next_id = self.next_shard_id.write();
            let current_id = *next_id;
            let ids = vec![current_id, current_id + 1];
            *next_id = current_id + 2; // Reserve both IDs
            ids
        };

        Some(vec![ShardOperation::Split { shard_id, new_shard_ids }])
    }

    /// Plan shard merge operation
    fn plan_shard_merge(&self, shard_ids: Vec<ShardId>) -> Option<ShardOperation> {
        if shard_ids.len() < 2 {
            return None;
        }

        let next_id = *self.next_shard_id.read();
        Some(ShardOperation::Merge {
            shard_ids: shard_ids.into_iter().take(2).collect(),
            new_shard_id: next_id,
        })
    }

    /// Execute shard split
    async fn execute_split(&self, shard_id: ShardId, new_shard_ids: Vec<ShardId>) -> Result<(), DAGError> {
        let mut shards = self.shards.write();
        let mut load_stats = self.load_stats.write();
        let mut next_id = self.next_shard_id.write();

        // Get the original shard data and mark as splitting
        let (range_start, range_end) = {
            let original_shard = shards.get_mut(&shard_id)
                .ok_or_else(|| DAGError::ShardError(format!("Shard {} not found", shard_id)))?;

            // Mark original shard as splitting
            original_shard.status = ShardStatus::Splitting;
            
            // Extract range data
            (original_shard.hash_range.start, original_shard.hash_range.end)
        };

        // Calculate new hash ranges
        let range_size = (range_end - range_start) / 2;
        let mid_point = range_start + range_size;

        // Create first new shard
        let shard1 = ShardInfo {
            shard_id: new_shard_ids[0],
            namespaces: HashSet::new(),
            hash_range: HashRange {
                start: range_start,
                end: mid_point,
            },
            created_at: std::time::SystemTime::now()
                .duration_since(std::time::UNIX_EPOCH)
                .unwrap()
                .as_secs(),
            parent_shard: Some(shard_id),
            status: ShardStatus::Active,
        };

        // Create second new shard
        let shard2 = ShardInfo {
            shard_id: new_shard_ids[1],
            namespaces: HashSet::new(),
            hash_range: HashRange {
                start: mid_point,
                end: range_end,
            },
            created_at: std::time::SystemTime::now()
                .duration_since(std::time::UNIX_EPOCH)
                .unwrap()
                .as_secs(),
            parent_shard: Some(shard_id),
            status: ShardStatus::Active,
        };

        // Add new shards
        shards.insert(new_shard_ids[0], shard1);
        shards.insert(new_shard_ids[1], shard2);

        // Initialize load stats for new shards
        load_stats.insert(new_shard_ids[0], ShardLoadStats::default());
        load_stats.insert(new_shard_ids[1], ShardLoadStats::default());

        // Note: next_shard_id was already updated during planning

        // Mark original shard as inactive
        if let Some(original_shard) = shards.get_mut(&shard_id) {
            original_shard.status = ShardStatus::Inactive;
        }

        Ok(())
    }

    /// Execute shard merge
    async fn execute_merge(&self, shard_ids: Vec<ShardId>, new_shard_id: ShardId) -> Result<(), DAGError> {
        let mut shards = self.shards.write();
        let mut load_stats = self.load_stats.write();
        let mut next_id = self.next_shard_id.write();

        // Get the shards to merge
        let mut namespaces = HashSet::new();
        let mut min_start = u64::MAX;
        let mut max_end = 0u64;

        for &shard_id in &shard_ids {
            let shard = shards.get_mut(&shard_id)
                .ok_or_else(|| DAGError::ShardError(format!("Shard {} not found", shard_id)))?;
            
            shard.status = ShardStatus::Merging;
            namespaces.extend(shard.namespaces.clone());
            min_start = min_start.min(shard.hash_range.start);
            max_end = max_end.max(shard.hash_range.end);
        }

        // Create merged shard
        let merged_shard = ShardInfo {
            shard_id: new_shard_id,
            namespaces,
            hash_range: HashRange {
                start: min_start,
                end: max_end,
            },
            created_at: std::time::SystemTime::now()
                .duration_since(std::time::UNIX_EPOCH)
                .unwrap()
                .as_secs(),
            parent_shard: None,
            status: ShardStatus::Active,
        };

        // Add merged shard
        shards.insert(new_shard_id, merged_shard);
        load_stats.insert(new_shard_id, ShardLoadStats::default());

        // Mark original shards as inactive
        for &shard_id in &shard_ids {
            if let Some(shard) = shards.get_mut(&shard_id) {
                shard.status = ShardStatus::Inactive;
            }
        }

        // Update next shard ID
        *next_id = new_shard_id + 1;

        Ok(())
    }

    /// Execute namespace reassignment
    async fn execute_reassignment(
        &self,
        namespace: String,
        from_shard: ShardId,
        to_shard: ShardId,
    ) -> Result<(), DAGError> {
        let mut shards = self.shards.write();
        let mut assignments = self.namespace_assignments.write();

        // Remove from old shard
        if let Some(shard) = shards.get_mut(&from_shard) {
            shard.namespaces.remove(&namespace);
        }

        // Add to new shard
        if let Some(shard) = shards.get_mut(&to_shard) {
            shard.namespaces.insert(namespace.clone());
        }

        // Update assignment
        assignments.insert(namespace, to_shard);

        Ok(())
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::dag_vertex::{DAGVertex, TransactionData, BLSSignature};

    #[test]
    fn test_shard_coordinator_creation() {
        let config = ShardConfig::default();
        let coordinator = ShardCoordinator::new(config.clone());
        
        assert_eq!(coordinator.get_active_shard_count(), config.initial_shard_count);
    }

    #[test]
    fn test_namespace_hash_consistency() {
        let coordinator = ShardCoordinator::new(ShardConfig::default());
        
        let hash1 = coordinator.calculate_namespace_hash("test");
        let hash2 = coordinator.calculate_namespace_hash("test");
        let hash3 = coordinator.calculate_namespace_hash("different");
        
        assert_eq!(hash1, hash2);
        assert_ne!(hash1, hash3);
    }

    #[test]
    fn test_cns_namespace_extraction() {
        let coordinator = ShardCoordinator::new(ShardConfig::default());
        
        // Create CNS transaction
        let cns_data = br#"{"p":"cns","op":"reg","cns":"alice"}"#;
        let tx_data = TransactionData::new([1; 32], [2; 32], 0, 1, 10, 1, cns_data.to_vec());
        let signature = BLSSignature::new([1; 48], [2; 48]);
        let vertex = DAGVertex::new([3; 32], 1, vec![[0; 32], [4; 32]], 0, tx_data, signature);
        
        let namespace = coordinator.extract_cns_namespace(&vertex);
        assert_eq!(namespace, "cns");
    }

    #[test]
    fn test_shard_assignment() {
        let coordinator = ShardCoordinator::new(ShardConfig::default());
        
        // Create test vertex
        let tx_data = TransactionData::new([1; 32], [2; 32], 100, 1, 10, 1, vec![]);
        let signature = BLSSignature::new([1; 48], [2; 48]);
        let vertex = DAGVertex::new([3; 32], 1, vec![[0; 32], [4; 32]], 0, tx_data, signature);
        
        let shard_id = coordinator.assign_shard(&vertex);
        assert!(shard_id < 4); // Should be one of the initial shards
        
        // Second assignment should be consistent
        let shard_id2 = coordinator.assign_shard(&vertex);
        assert_eq!(shard_id, shard_id2);
    }

    #[test]
    fn test_load_stats_update() {
        let coordinator = ShardCoordinator::new(ShardConfig::default());
        
        coordinator.update_load_stats(0, 1000.0);
        coordinator.update_load_stats(0, 1500.0);
        coordinator.update_load_stats(0, 2000.0);
        
        let stats = coordinator.get_load_stats(0).unwrap();
        assert_eq!(stats.current_tps, 2000.0);
        assert_eq!(stats.peak_tps, 2000.0);
        assert!(stats.average_tps > 0.0);
        assert_eq!(stats.tps_history.len(), 3);
    }

    #[tokio::test]
    async fn test_shard_rebalancing_debug() {
        let mut config = ShardConfig::default();
        config.max_shard_tps = 2000; // High threshold for splitting
        config.min_shard_tps = 500;  // Low threshold for merging
        let coordinator = ShardCoordinator::new(config);
        
        // Verify initial state
        assert_eq!(coordinator.get_active_shard_count(), 4);
        
        // Add baseline load to other shards (between min=500 and max=2000 thresholds)
        for shard_id in 1..4 {
            for _ in 0..3 {
                coordinator.update_load_stats(shard_id, 1000.0); // Above min (500) but below max (2000)
                tokio::time::sleep(tokio::time::Duration::from_millis(5)).await;
            }
        }
        
        // Simulate high load on shard 0 multiple times to build history
        for i in 0..5 {
            coordinator.update_load_stats(0, 2500.0); // Above max threshold (2000)
            // Add small delay with different timestamps
            tokio::time::sleep(tokio::time::Duration::from_millis(10)).await;
        }
        
        // Verify load stats have been updated
        let stats = coordinator.get_load_stats(0).unwrap();
        assert!(stats.average_tps > 2000.0, "Average TPS should be > 2000, got {}", stats.average_tps);
        
        let operations = coordinator.check_rebalancing();
        assert!(!operations.is_empty(), "Should have rebalancing operations");
        
        // Debug: Print all operations
        println!("Planned operations: {}", operations.len());
        for (i, operation) in operations.iter().enumerate() {
            match operation {
                ShardOperation::Split { shard_id, new_shard_ids } => {
                    println!("Operation {}: Split shard {} into shards {:?}", i, shard_id, new_shard_ids);
                }
                ShardOperation::Merge { shard_ids, new_shard_id } => {
                    println!("Operation {}: Merge shards {:?} into shard {}", i, shard_ids, new_shard_id);
                }
                ShardOperation::Reassign { namespace, from_shard, to_shard } => {
                    println!("Operation {}: Reassign {} from shard {} to shard {}", i, namespace, from_shard, to_shard);
                }
            }
        }
        
        // Verify operation type
        assert!(matches!(operations[0], ShardOperation::Split { .. }), "First operation should be a split");
        assert_eq!(operations.len(), 1, "Should only have one operation (split), got {}", operations.len());
        
        // Count active shards before split
        let before_count = coordinator.get_active_shard_count();
        println!("Active shards before split: {}", before_count);
        
        // Execute rebalancing
        coordinator.execute_rebalancing(operations).await.unwrap();
        
        // Count active shards after split
        let final_count = coordinator.get_active_shard_count();
        println!("Active shards after split: {}", final_count);
        
        // Debug: Print all shard statuses
        for i in 0..10 {
            if let Some(shard_info) = coordinator.get_shard_info(i) {
                println!("Shard {}: status = {:?}", i, shard_info.status);
            }
        }
        
        // Should have more shards now (4 + 2 new - 1 inactive = 5 active)
        assert!(final_count > 4, "Expected > 4 active shards, got {}", final_count);
    }

    #[tokio::test]
    async fn test_shard_split_simple() {
        let config = ShardConfig::default();
        let coordinator = ShardCoordinator::new(config);
        
        // Verify initial state: 4 active shards (0,1,2,3)
        assert_eq!(coordinator.get_active_shard_count(), 4);
        
        // Manually create a split operation for shard 0
        let operations = vec![ShardOperation::Split { 
            shard_id: 0, 
            new_shard_ids: vec![4, 5] 
        }];
        
        // Execute split
        coordinator.execute_rebalancing(operations).await.unwrap();
        
        // Should now have 5 active shards: 1,2,3,4,5 (0 becomes inactive)
        let final_count = coordinator.get_active_shard_count();
        println!("Final active shard count: {}", final_count);
        
        // Debug: Print all shard statuses
        for i in 0..8 {
            if let Some(shard_info) = coordinator.get_shard_info(i) {
                println!("Shard {}: status = {:?}", i, shard_info.status);
            }
        }
        
        assert_eq!(final_count, 5, "Expected exactly 5 active shards after split");
    }
}