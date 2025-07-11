/*!
 * DAG Vertex Storage Engine
 * 
 * High-performance LSM-tree based storage for DAG vertices
 * Designed for 100k+ insertions per second
 */

use crate::{DAGVertex, VertexHash, ShardId, DAGError};
use rocksdb::{DB, Options, WriteBatch, IteratorMode, Direction};
use serde::{Deserialize, Serialize};
use std::sync::Arc;
use parking_lot::RwLock;
use dashmap::DashMap;
use rayon::prelude::*;
use std::path::Path;
use std::collections::HashMap;

/// Key prefixes for different data types
const VERTEX_PREFIX: u8 = 0x01;
const INDEX_PREFIX: u8 = 0x02;
const SHARD_PREFIX: u8 = 0x03;
const META_PREFIX: u8 = 0x04;

/// High-performance DAG vertex storage
pub struct DAGVertexStore {
    /// RocksDB instance for persistent storage
    db: Arc<DB>,
    /// In-memory indices for fast lookups
    indices: Arc<RwLock<StorageIndices>>,
    /// Per-shard statistics
    shard_stats: Arc<DashMap<ShardId, ShardStats>>,
    /// Write batch cache for bulk operations
    write_cache: Arc<DashMap<VertexHash, DAGVertex>>,
    /// Storage configuration
    config: StorageConfig,
}

/// Storage indices for fast queries
#[derive(Debug, Default)]
struct StorageIndices {
    /// Parent -> Children mapping for DAG traversal
    children_index: HashMap<VertexHash, Vec<VertexHash>>,
    /// Shard -> Vertices mapping
    shard_index: HashMap<ShardId, Vec<VertexHash>>,
    /// Logical clock -> Vertices mapping for ordering
    clock_index: HashMap<u64, Vec<VertexHash>>,
}

/// Per-shard statistics
#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct ShardStats {
    pub vertex_count: u64,
    pub total_size_bytes: u64,
    pub last_update: u64,
    pub average_vertex_size: f64,
}

/// Storage configuration
#[derive(Debug, Clone)]
pub struct StorageConfig {
    pub max_write_cache_size: usize,
    pub batch_write_threshold: usize,
    pub enable_compression: bool,
    pub max_open_files: i32,
    pub write_buffer_size: usize,
}

impl Default for StorageConfig {
    fn default() -> Self {
        Self {
            max_write_cache_size: 10000,
            batch_write_threshold: 1000,
            enable_compression: true,
            max_open_files: 1000,
            write_buffer_size: 64 * 1024 * 1024, // 64MB
        }
    }
}

impl DAGVertexStore {
    /// Create new storage instance
    pub async fn new<P: AsRef<Path>>(path: P) -> Result<Self, DAGError> {
        Self::with_config(path, StorageConfig::default()).await
    }

    /// Create new storage instance with custom configuration
    pub async fn with_config<P: AsRef<Path>>(path: P, config: StorageConfig) -> Result<Self, DAGError> {
        let mut opts = Options::default();
        opts.create_if_missing(true);
        opts.set_max_open_files(config.max_open_files);
        opts.set_write_buffer_size(config.write_buffer_size);
        opts.set_max_write_buffer_number(3);
        opts.set_target_file_size_base(64 * 1024 * 1024); // 64MB
        
        if config.enable_compression {
            opts.set_compression_type(rocksdb::DBCompressionType::Lz4);
        }

        // LSM-tree optimization for high write throughput
        opts.set_level_zero_file_num_compaction_trigger(4);
        opts.set_level_zero_slowdown_writes_trigger(20);
        opts.set_level_zero_stop_writes_trigger(36);
        opts.set_max_background_jobs(4);

        let db = Arc::new(DB::open(&opts, path).map_err(|e| DAGError::StorageError(e.to_string()))?);
        
        let store = Self {
            db,
            indices: Arc::new(RwLock::new(StorageIndices::default())),
            shard_stats: Arc::new(DashMap::new()),
            write_cache: Arc::new(DashMap::new()),
            config,
        };

        // Load existing indices
        store.rebuild_indices().await?;
        
        Ok(store)
    }

    /// Insert a DAG vertex
    pub async fn insert_vertex(&self, vertex: &DAGVertex, shard_id: ShardId) -> Result<(), DAGError> {
        // Add to write cache first for immediate availability
        self.write_cache.insert(vertex.hash, vertex.clone());
        
        // Create storage key
        let key = self.make_vertex_key(&vertex.hash, vertex.logical_clock, shard_id);
        let value = bincode::serialize(vertex).map_err(|e| DAGError::SerializationError(e.to_string()))?;
        
        // Write to database
        self.db.put(&key, &value).map_err(|e| DAGError::StorageError(e.to_string()))?;
        
        // Update indices
        self.update_indices(vertex, shard_id).await?;
        
        // Update shard statistics
        self.update_shard_stats(shard_id, vertex).await;
        
        // Check if we should flush write cache
        if self.write_cache.len() >= self.config.max_write_cache_size {
            self.flush_write_cache().await?;
        }
        
        Ok(())
    }

    /// Insert multiple vertices in batch for high throughput
    pub async fn insert_vertex_batch(&self, vertices: Vec<(DAGVertex, ShardId)>) -> Result<(), DAGError> {
        if vertices.is_empty() {
            return Ok(());
        }

        let mut batch = WriteBatch::default();
        
        // Process in parallel for better performance
        let serialized_vertices: Vec<_> = vertices
            .par_iter()
            .map(|(vertex, shard_id)| {
                let key = self.make_vertex_key(&vertex.hash, vertex.logical_clock, *shard_id);
                let value = bincode::serialize(vertex).unwrap_or_default();
                (key, value, vertex.clone(), *shard_id)
            })
            .collect();

        // Add to batch
        for (key, value, vertex, shard_id) in &serialized_vertices {
            batch.put(key, value);
            self.write_cache.insert(vertex.hash, vertex.clone());
        }

        // Write batch to database
        self.db.write(batch).map_err(|e| DAGError::StorageError(e.to_string()))?;

        // Update indices for all vertices
        for (_, _, vertex, shard_id) in serialized_vertices {
            self.update_indices(&vertex, shard_id).await?;
            self.update_shard_stats(shard_id, &vertex).await;
        }

        Ok(())
    }

    /// Get vertex by hash
    pub async fn get_vertex(&self, hash: &VertexHash) -> Result<Option<DAGVertex>, DAGError> {
        // Check write cache first
        if let Some(vertex) = self.write_cache.get(hash) {
            return Ok(Some(vertex.clone()));
        }

        // Search in database across all possible shards and clocks
        let prefix = [VERTEX_PREFIX];
        let iter = self.db.iterator(IteratorMode::From(&prefix, Direction::Forward));
        
        for item in iter {
            let (key, value) = item.map_err(|e| DAGError::StorageError(e.to_string()))?;
            
            // Parse key to extract vertex hash
            if key.len() >= 33 {
                let stored_hash: VertexHash = key[1..33].try_into().unwrap_or([0; 32]);
                if stored_hash == *hash {
                    let vertex: DAGVertex = bincode::deserialize(&value)
                        .map_err(|e| DAGError::SerializationError(e.to_string()))?;
                    return Ok(Some(vertex));
                }
            }
        }

        Ok(None)
    }

    /// Get vertices by shard
    pub async fn get_vertices_by_shard(&self, shard_id: ShardId, limit: Option<usize>) -> Result<Vec<DAGVertex>, DAGError> {
        let indices = self.indices.read();
        let vertex_hashes = indices.shard_index.get(&shard_id).cloned().unwrap_or_default();
        
        let limit = limit.unwrap_or(vertex_hashes.len()).min(vertex_hashes.len());
        let mut vertices = Vec::with_capacity(limit);
        
        for hash in vertex_hashes.iter().take(limit) {
            if let Some(vertex) = self.get_vertex(hash).await? {
                vertices.push(vertex);
            }
        }
        
        Ok(vertices)
    }

    /// Get children of a vertex for DAG traversal
    pub async fn get_children(&self, parent_hash: &VertexHash) -> Result<Vec<DAGVertex>, DAGError> {
        let indices = self.indices.read();
        let child_hashes = indices.children_index.get(parent_hash).cloned().unwrap_or_default();
        
        let mut children = Vec::with_capacity(child_hashes.len());
        for hash in child_hashes {
            if let Some(vertex) = self.get_vertex(&hash).await? {
                children.push(vertex);
            }
        }
        
        Ok(children)
    }

    /// Get total vertex count
    pub async fn get_vertex_count(&self) -> Result<u64, DAGError> {
        let mut count = 0u64;
        
        let prefix = [VERTEX_PREFIX];
        let iter = self.db.iterator(IteratorMode::From(&prefix, Direction::Forward));
        
        for item in iter {
            let (key, _) = item.map_err(|e| DAGError::StorageError(e.to_string()))?;
            if key.is_empty() || key[0] != VERTEX_PREFIX {
                break;
            }
            count += 1;
        }
        
        Ok(count)
    }

    /// Get shard statistics
    pub async fn get_shard_stats(&self, shard_id: ShardId) -> Option<ShardStats> {
        self.shard_stats.get(&shard_id).map(|entry| entry.clone())
    }

    /// Get all shard statistics
    pub async fn get_all_shard_stats(&self) -> HashMap<ShardId, ShardStats> {
        self.shard_stats.iter().map(|entry| (*entry.key(), entry.value().clone())).collect()
    }

    /// Flush write cache to ensure persistence
    pub async fn flush_write_cache(&self) -> Result<(), DAGError> {
        self.write_cache.clear();
        self.db.flush().map_err(|e| DAGError::StorageError(e.to_string()))?;
        Ok(())
    }

    /// Compact database for optimal performance
    pub async fn compact(&self) -> Result<(), DAGError> {
        self.db.compact_range(None::<&[u8]>, None::<&[u8]>);
        Ok(())
    }

    /// Create storage key for vertex
    fn make_vertex_key(&self, vertex_hash: &VertexHash, logical_clock: u64, shard_id: ShardId) -> Vec<u8> {
        let mut key = Vec::with_capacity(45);
        key.push(VERTEX_PREFIX);
        key.extend_from_slice(vertex_hash);
        key.extend_from_slice(&logical_clock.to_be_bytes());
        key.extend_from_slice(&shard_id.to_be_bytes());
        key
    }

    /// Update in-memory indices
    async fn update_indices(&self, vertex: &DAGVertex, shard_id: ShardId) -> Result<(), DAGError> {
        let mut indices = self.indices.write();
        
        // Update children index
        for parent_hash in &vertex.parents {
            indices.children_index
                .entry(*parent_hash)
                .or_insert_with(Vec::new)
                .push(vertex.hash);
        }
        
        // Update shard index
        indices.shard_index
            .entry(shard_id)
            .or_insert_with(Vec::new)
            .push(vertex.hash);
        
        // Update clock index
        indices.clock_index
            .entry(vertex.logical_clock)
            .or_insert_with(Vec::new)
            .push(vertex.hash);
        
        Ok(())
    }

    /// Update shard statistics
    async fn update_shard_stats(&self, shard_id: ShardId, vertex: &DAGVertex) {
        let vertex_size = vertex.serialized_size() as u64;
        let timestamp = std::time::SystemTime::now()
            .duration_since(std::time::UNIX_EPOCH)
            .unwrap()
            .as_millis() as u64;

        self.shard_stats.entry(shard_id)
            .and_modify(|stats| {
                stats.vertex_count += 1;
                stats.total_size_bytes += vertex_size;
                stats.last_update = timestamp;
                stats.average_vertex_size = stats.total_size_bytes as f64 / stats.vertex_count as f64;
            })
            .or_insert(ShardStats {
                vertex_count: 1,
                total_size_bytes: vertex_size,
                last_update: timestamp,
                average_vertex_size: vertex_size as f64,
            });
    }

    /// Rebuild indices from storage (recovery operation)
    async fn rebuild_indices(&self) -> Result<(), DAGError> {
        let mut indices = self.indices.write();
        indices.children_index.clear();
        indices.shard_index.clear();
        indices.clock_index.clear();

        let prefix = [VERTEX_PREFIX];
        let iter = self.db.iterator(IteratorMode::From(&prefix, Direction::Forward));

        for item in iter {
            let (key, value) = item.map_err(|e| DAGError::StorageError(e.to_string()))?;
            
            if key.is_empty() || key[0] != VERTEX_PREFIX {
                break;
            }

            // Parse key to extract shard_id
            if key.len() >= 45 {
                let shard_id = u32::from_be_bytes(key[41..45].try_into().unwrap_or([0; 4]));
                
                // Deserialize vertex
                let vertex: DAGVertex = bincode::deserialize(&value)
                    .map_err(|e| DAGError::SerializationError(e.to_string()))?;
                
                // Update indices
                for parent_hash in &vertex.parents {
                    indices.children_index
                        .entry(*parent_hash)
                        .or_insert_with(Vec::new)
                        .push(vertex.hash);
                }
                
                indices.shard_index
                    .entry(shard_id)
                    .or_insert_with(Vec::new)
                    .push(vertex.hash);
                
                indices.clock_index
                    .entry(vertex.logical_clock)
                    .or_insert_with(Vec::new)
                    .push(vertex.hash);
            }
        }

        Ok(())
    }
}

impl Drop for DAGVertexStore {
    fn drop(&mut self) {
        // Flush any remaining writes
        let _ = self.db.flush();
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::dag_vertex::{DAGVertex, TransactionData, BLSSignature};
    use tempfile::TempDir;

    #[tokio::test]
    async fn test_storage_creation() {
        let temp_dir = TempDir::new().unwrap();
        let storage = DAGVertexStore::new(temp_dir.path()).await.unwrap();
        
        let count = storage.get_vertex_count().await.unwrap();
        assert_eq!(count, 0);
    }

    #[tokio::test]
    async fn test_vertex_insertion_and_retrieval() {
        let temp_dir = TempDir::new().unwrap();
        let storage = DAGVertexStore::new(temp_dir.path()).await.unwrap();
        
        // Create test vertex
        let tx_data = TransactionData::new([1; 32], [2; 32], 100, 1, 10, 1, vec![]);
        let signature = BLSSignature::new([1; 48], [2; 48]);
        let vertex = DAGVertex::new([3; 32], 1, vec![[0; 32], [4; 32]], 0, tx_data, signature);
        
        // Insert vertex
        storage.insert_vertex(&vertex, 0).await.unwrap();
        
        // Retrieve vertex
        let retrieved = storage.get_vertex(&vertex.hash).await.unwrap();
        assert!(retrieved.is_some());
        assert_eq!(retrieved.unwrap().hash, vertex.hash);
        
        // Check count
        let count = storage.get_vertex_count().await.unwrap();
        assert_eq!(count, 1);
    }

    #[tokio::test]
    async fn test_batch_insertion() {
        let temp_dir = TempDir::new().unwrap();
        let storage = DAGVertexStore::new(temp_dir.path()).await.unwrap();
        
        // Create multiple vertices
        let mut vertices = Vec::new();
        for i in 0..10 {
            let tx_data = TransactionData::new([i; 32], [i+1; 32], 100, 1, 10, i as u64, vec![]);
            let signature = BLSSignature::new([i; 48], [i+1; 48]);
            let vertex = DAGVertex::new([i; 32], i as u64 + 1, vec![[0; 32], [i+2; 32]], i % 3, tx_data, signature);
            vertices.push((vertex, i % 3));
        }
        
        // Insert batch
        storage.insert_vertex_batch(vertices).await.unwrap();
        
        // Check count
        let count = storage.get_vertex_count().await.unwrap();
        assert_eq!(count, 10);
        
        // Check shard distribution
        let shard_0_vertices = storage.get_vertices_by_shard(0, None).await.unwrap();
        let shard_1_vertices = storage.get_vertices_by_shard(1, None).await.unwrap();
        let shard_2_vertices = storage.get_vertices_by_shard(2, None).await.unwrap();
        
        assert!(shard_0_vertices.len() >= 3);
        assert!(shard_1_vertices.len() >= 3);
        assert!(shard_2_vertices.len() >= 3);
    }

    #[tokio::test]
    async fn test_children_index() {
        let temp_dir = TempDir::new().unwrap();
        let storage = DAGVertexStore::new(temp_dir.path()).await.unwrap();
        
        // Create parent vertex
        let parent_hash = [1; 32];
        let tx_data = TransactionData::new([1; 32], [2; 32], 100, 1, 10, 1, vec![]);
        let signature = BLSSignature::new([1; 48], [2; 48]);
        let parent = DAGVertex::new(parent_hash, 1, vec![], 0, tx_data.clone(), signature.clone());
        storage.insert_vertex(&parent, 0).await.unwrap();
        
        // Create children vertices
        for i in 0..3 {
            let child = DAGVertex::new([i+10; 32], i+2, vec![parent_hash, [i+5; 32]], 0, tx_data.clone(), signature.clone());
            storage.insert_vertex(&child, 0).await.unwrap();
        }
        
        // Get children
        let children = storage.get_children(&parent_hash).await.unwrap();
        assert_eq!(children.len(), 3);
    }
}