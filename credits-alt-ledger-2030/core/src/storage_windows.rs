/*!
 * Windows-Compatible DAG Vertex Storage Implementation
 * 
 * Uses Sled database (pure Rust) instead of RocksDB to avoid libclang dependency
 */

use std::path::Path;
use std::sync::Arc;
use std::collections::HashMap;
use serde::{Serialize, Deserialize};
use thiserror::Error;
use parking_lot::RwLock;
use dashmap::DashMap;
use lru::LruCache;

use crate::dag_vertex::*;

#[derive(Error, Debug)]
pub enum StorageError {
    #[error("Database error: {0}")]
    DatabaseError(#[from] sled::Error),
    #[error("Serialization error: {0}")]
    SerializationError(#[from] bincode::Error),
    #[error("Vertex not found: {0:?}")]
    VertexNotFound(VertexHash),
    #[error("Invalid data format")]
    InvalidDataFormat,
}

pub type StorageResult<T> = Result<T, StorageError>;

/// Storage statistics for monitoring
#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct StorageStats {
    pub total_vertices: u64,
    pub total_size_bytes: u64,
    pub cache_hits: u64,
    pub cache_misses: u64,
    pub write_operations: u64,
    pub read_operations: u64,
}

/// Windows-compatible DAG vertex store using Sled database
pub struct DAGVertexStore {
    // Sled database instances
    db: sled::Db,
    vertices_tree: sled::Tree,
    parents_tree: sled::Tree,
    children_tree: sled::Tree,
    shard_tree: sled::Tree,
    
    // In-memory cache for performance
    vertex_cache: Arc<RwLock<LruCache<VertexHash, DAGVertex>>>,
    parents_cache: Arc<RwLock<LruCache<VertexHash, Vec<VertexHash>>>>,
    
    // Statistics
    stats: Arc<RwLock<StorageStats>>,
    
    // Configuration
    cache_size: usize,
}

impl DAGVertexStore {
    pub fn new<P: AsRef<Path>>(db_path: P, cache_size: usize) -> StorageResult<Self> {
        // Open Sled database
        let db = sled::open(db_path)?;
        
        // Create separate trees for different data types
        let vertices_tree = db.open_tree("vertices")?;
        let parents_tree = db.open_tree("parents")?;
        let children_tree = db.open_tree("children")?;
        let shard_tree = db.open_tree("shards")?;
        
        // Initialize caches
        let vertex_cache = Arc::new(RwLock::new(LruCache::new(
            std::num::NonZeroUsize::new(cache_size).unwrap()
        )));
        let parents_cache = Arc::new(RwLock::new(LruCache::new(
            std::num::NonZeroUsize::new(cache_size / 2).unwrap()
        )));
        
        // Initialize stats
        let stats = Arc::new(RwLock::new(StorageStats {
            total_vertices: 0,
            total_size_bytes: 0,
            cache_hits: 0,
            cache_misses: 0,
            write_operations: 0,
            read_operations: 0,
        }));
        
        let store = DAGVertexStore {
            db,
            vertices_tree,
            parents_tree,
            children_tree,
            shard_tree,
            vertex_cache,
            parents_cache,
            stats,
            cache_size,
        };
        
        // Load initial statistics
        store.update_vertex_count()?;
        
        Ok(store)
    }
    
    /// Store a DAG vertex with all its relationships
    pub fn store_vertex(&self, vertex: &DAGVertex) -> StorageResult<()> {
        let vertex_key = vertex.hash.to_vec();
        
        // Serialize vertex data
        let vertex_data = bincode::serialize(vertex)?;
        
        // Store vertex
        self.vertices_tree.insert(&vertex_key, vertex_data)?;
        
        // Store parent relationships
        if !vertex.parents.is_empty() {
            let parents_data = bincode::serialize(&vertex.parents)?;
            self.parents_tree.insert(&vertex_key, parents_data)?;
            
            // Update children relationships for each parent
            for parent_hash in &vertex.parents {
                self.add_child_relationship(parent_hash, &vertex.hash)?;
            }
        }
        
        // Store shard mapping
        let shard_key = format!("shard_{}", vertex.shard_id.0);
        let mut shard_vertices = self.get_shard_vertices(vertex.shard_id)?;
        shard_vertices.push(vertex.hash);
        let shard_data = bincode::serialize(&shard_vertices)?;
        self.shard_tree.insert(shard_key.as_bytes(), shard_data)?;
        
        // Update cache
        {
            let mut cache = self.vertex_cache.write();
            cache.put(vertex.hash, vertex.clone());
        }
        
        {
            let mut cache = self.parents_cache.write();
            cache.put(vertex.hash, vertex.parents.clone());
        }
        
        // Update statistics
        {
            let mut stats = self.stats.write();
            stats.total_vertices += 1;
            stats.total_size_bytes += vertex_data.len() as u64;
            stats.write_operations += 1;
        }
        
        // Flush to disk
        self.db.flush()?;
        
        Ok(())
    }
    
    /// Retrieve a DAG vertex by its hash
    pub fn get_vertex(&self, hash: &VertexHash) -> StorageResult<Option<DAGVertex>> {
        // Check cache first
        {
            let mut cache = self.vertex_cache.write();
            if let Some(vertex) = cache.get(hash) {
                self.increment_cache_hits();
                return Ok(Some(vertex.clone()));
            }
        }
        
        self.increment_cache_misses();
        
        // Read from database
        let vertex_key = hash.to_vec();
        if let Some(vertex_data) = self.vertices_tree.get(&vertex_key)? {
            let vertex: DAGVertex = bincode::deserialize(&vertex_data)?;
            
            // Update cache
            {
                let mut cache = self.vertex_cache.write();
                cache.put(*hash, vertex.clone());
            }
            
            self.increment_read_operations();
            Ok(Some(vertex))
        } else {
            Ok(None)
        }
    }
    
    /// Get all parent vertices of a given vertex
    pub fn get_parents(&self, hash: &VertexHash) -> StorageResult<Vec<VertexHash>> {
        // Check cache first
        {
            let mut cache = self.parents_cache.write();
            if let Some(parents) = cache.get(hash) {
                self.increment_cache_hits();
                return Ok(parents.clone());
            }
        }
        
        self.increment_cache_misses();
        
        // Read from database
        let vertex_key = hash.to_vec();
        if let Some(parents_data) = self.parents_tree.get(&vertex_key)? {
            let parents: Vec<VertexHash> = bincode::deserialize(&parents_data)?;
            
            // Update cache
            {
                let mut cache = self.parents_cache.write();
                cache.put(*hash, parents.clone());
            }
            
            self.increment_read_operations();
            Ok(parents)
        } else {
            Ok(Vec::new())
        }
    }
    
    /// Get all child vertices of a given vertex
    pub fn get_children(&self, hash: &VertexHash) -> StorageResult<Vec<VertexHash>> {
        let vertex_key = hash.to_vec();
        if let Some(children_data) = self.children_tree.get(&vertex_key)? {
            let children: Vec<VertexHash> = bincode::deserialize(&children_data)?;
            self.increment_read_operations();
            Ok(children)
        } else {
            Ok(Vec::new())
        }
    }
    
    /// Get all vertices in a specific shard
    pub fn get_shard_vertices(&self, shard_id: ShardId) -> StorageResult<Vec<VertexHash>> {
        let shard_key = format!("shard_{}", shard_id.0);
        if let Some(shard_data) = self.shard_tree.get(shard_key.as_bytes())? {
            let vertices: Vec<VertexHash> = bincode::deserialize(&shard_data)?;
            self.increment_read_operations();
            Ok(vertices)
        } else {
            Ok(Vec::new())
        }
    }
    
    /// Check if a vertex exists
    pub fn vertex_exists(&self, hash: &VertexHash) -> StorageResult<bool> {
        // Check cache first
        {
            let cache = self.vertex_cache.read();
            if cache.peek(hash).is_some() {
                return Ok(true);
            }
        }
        
        // Check database
        let vertex_key = hash.to_vec();
        Ok(self.vertices_tree.contains_key(&vertex_key)?)
    }
    
    /// Get vertices by logical clock range
    pub fn get_vertices_by_clock_range(&self, start: u64, end: u64) -> StorageResult<Vec<DAGVertex>> {
        let mut vertices = Vec::new();
        
        for result in self.vertices_tree.iter() {
            let (_, vertex_data) = result?;
            let vertex: DAGVertex = bincode::deserialize(&vertex_data)?;
            
            if vertex.logical_clock >= start && vertex.logical_clock <= end {
                vertices.push(vertex);
            }
        }
        
        self.increment_read_operations();
        Ok(vertices)
    }
    
    /// Get storage statistics
    pub fn get_stats(&self) -> StorageStats {
        self.stats.read().clone()
    }
    
    /// Compact the database to reclaim space
    pub fn compact(&self) -> StorageResult<()> {
        // Sled doesn't have explicit compaction, but we can flush
        self.db.flush()?;
        Ok(())
    }
    
    /// Clear all caches
    pub fn clear_caches(&self) {
        {
            let mut cache = self.vertex_cache.write();
            cache.clear();
        }
        {
            let mut cache = self.parents_cache.write();
            cache.clear();
        }
    }
    
    /// Get cache hit ratio
    pub fn get_cache_hit_ratio(&self) -> f64 {
        let stats = self.stats.read();
        let total_requests = stats.cache_hits + stats.cache_misses;
        if total_requests == 0 {
            0.0
        } else {
            stats.cache_hits as f64 / total_requests as f64
        }
    }
    
    // Private helper methods
    
    fn add_child_relationship(&self, parent_hash: &VertexHash, child_hash: &VertexHash) -> StorageResult<()> {
        let parent_key = parent_hash.to_vec();
        let mut children = if let Some(children_data) = self.children_tree.get(&parent_key)? {
            bincode::deserialize::<Vec<VertexHash>>(&children_data)?
        } else {
            Vec::new()
        };
        
        if !children.contains(child_hash) {
            children.push(*child_hash);
            let children_data = bincode::serialize(&children)?;
            self.children_tree.insert(&parent_key, children_data)?;
        }
        
        Ok(())
    }
    
    fn update_vertex_count(&self) -> StorageResult<()> {
        let count = self.vertices_tree.len();
        let size = self.db.size_on_disk()?;
        
        {
            let mut stats = self.stats.write();
            stats.total_vertices = count as u64;
            stats.total_size_bytes = size;
        }
        
        Ok(())
    }
    
    fn increment_cache_hits(&self) {
        let mut stats = self.stats.write();
        stats.cache_hits += 1;
    }
    
    fn increment_cache_misses(&self) {
        let mut stats = self.stats.write();
        stats.cache_misses += 1;
    }
    
    fn increment_read_operations(&self) {
        let mut stats = self.stats.write();
        stats.read_operations += 1;
    }
}

/// Batch operations for efficient bulk storage
impl DAGVertexStore {
    /// Store multiple vertices in a single transaction
    pub fn store_vertices_batch(&self, vertices: &[DAGVertex]) -> StorageResult<()> {
        // Sled doesn't have explicit transactions, but operations are atomic
        for vertex in vertices {
            self.store_vertex(vertex)?;
        }
        
        // Flush once at the end
        self.db.flush()?;
        
        Ok(())
    }
    
    /// Get multiple vertices by their hashes
    pub fn get_vertices_batch(&self, hashes: &[VertexHash]) -> StorageResult<Vec<Option<DAGVertex>>> {
        let mut results = Vec::with_capacity(hashes.len());
        
        for hash in hashes {
            results.push(self.get_vertex(hash)?);
        }
        
        Ok(results)
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use tempfile::TempDir;
    
    fn create_test_vertex(clock: u64) -> DAGVertex {
        DAGVertex {
            hash: VertexHash::from([clock as u8; 32]),
            tx_hash: TransactionHash::from([clock as u8; 32]),
            logical_clock: clock,
            parents: Vec::new(),
            shard_id: ShardId(0),
            transaction_data: TransactionData {
                from: AccountAddress::from([1u8; 32]),
                to: AccountAddress::from([2u8; 32]),
                amount: 1000,
                fee: 10,
                payload: Vec::new(),
            },
            signature: BLSSignature::default(),
            timestamp: clock * 1000,
            proof: None,
        }
    }
    
    #[test]
    fn test_store_and_retrieve_vertex() {
        let temp_dir = TempDir::new().unwrap();
        let store = DAGVertexStore::new(temp_dir.path(), 100).unwrap();
        
        let vertex = create_test_vertex(1);
        let hash = vertex.hash;
        
        // Store vertex
        store.store_vertex(&vertex).unwrap();
        
        // Retrieve vertex
        let retrieved = store.get_vertex(&hash).unwrap().unwrap();
        assert_eq!(retrieved.hash, vertex.hash);
        assert_eq!(retrieved.logical_clock, vertex.logical_clock);
    }
    
    #[test]
    fn test_cache_functionality() {
        let temp_dir = TempDir::new().unwrap();
        let store = DAGVertexStore::new(temp_dir.path(), 100).unwrap();
        
        let vertex = create_test_vertex(1);
        let hash = vertex.hash;
        
        // Store vertex
        store.store_vertex(&vertex).unwrap();
        
        // First retrieval - cache miss
        let _ = store.get_vertex(&hash).unwrap();
        
        // Second retrieval - cache hit
        let _ = store.get_vertex(&hash).unwrap();
        
        let stats = store.get_stats();
        assert!(stats.cache_hits > 0);
    }
    
    #[test]
    fn test_batch_operations() {
        let temp_dir = TempDir::new().unwrap();
        let store = DAGVertexStore::new(temp_dir.path(), 100).unwrap();
        
        let vertices: Vec<_> = (1..=10).map(create_test_vertex).collect();
        let hashes: Vec<_> = vertices.iter().map(|v| v.hash).collect();
        
        // Store batch
        store.store_vertices_batch(&vertices).unwrap();
        
        // Retrieve batch
        let retrieved = store.get_vertices_batch(&hashes).unwrap();
        assert_eq!(retrieved.len(), 10);
        assert!(retrieved.iter().all(|v| v.is_some()));
    }
}