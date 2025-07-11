/*!
 * Unified DAG Vertex Storage Engine
 * 
 * Supports both RocksDB and Sled backends with conditional compilation
 */

use crate::dag_vertex::{DAGVertex, VertexHash, ShardId};
use serde::{Serialize, Deserialize};
use std::path::Path;
use std::sync::Arc;
use std::collections::HashMap;
use parking_lot::RwLock;
use dashmap::DashMap;
use lru::LruCache;
use thiserror::Error;

#[derive(Error, Debug)]
pub enum StorageError {
    #[cfg(feature = "rocksdb-backend")]
    #[error("RocksDB error: {0}")]
    RocksDBError(#[from] rocksdb::Error),
    
    #[cfg(feature = "sled-backend")]
    #[error("Sled error: {0}")]
    SledError(#[from] sled::Error),
    
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

/// Unified DAG vertex store with configurable backend
pub struct DAGVertexStore {
    #[cfg(feature = "rocksdb-backend")]
    rocksdb: Option<Arc<rocksdb::DB>>,
    
    #[cfg(feature = "sled-backend")]
    sled_db: Option<sled::Db>,
    #[cfg(feature = "sled-backend")]
    vertices_tree: Option<sled::Tree>,
    #[cfg(feature = "sled-backend")]
    parents_tree: Option<sled::Tree>,
    #[cfg(feature = "sled-backend")]
    children_tree: Option<sled::Tree>,
    #[cfg(feature = "sled-backend")]
    shard_tree: Option<sled::Tree>,
    
    // Common components
    vertex_cache: Arc<RwLock<LruCache<VertexHash, DAGVertex>>>,
    parents_cache: Arc<RwLock<LruCache<VertexHash, Vec<VertexHash>>>>,
    stats: Arc<RwLock<StorageStats>>,
    cache_size: usize,
}

impl DAGVertexStore {
    pub fn new<P: AsRef<Path>>(db_path: P, cache_size: usize) -> StorageResult<Self> {
        let vertex_cache = Arc::new(RwLock::new(LruCache::new(
            std::num::NonZeroUsize::new(cache_size).unwrap()
        )));
        let parents_cache = Arc::new(RwLock::new(LruCache::new(
            std::num::NonZeroUsize::new(cache_size / 2).unwrap()
        )));
        
        let stats = Arc::new(RwLock::new(StorageStats {
            total_vertices: 0,
            total_size_bytes: 0,
            cache_hits: 0,
            cache_misses: 0,
            write_operations: 0,
            read_operations: 0,
        }));

        #[cfg(feature = "rocksdb-backend")]
        {
            let mut opts = rocksdb::Options::default();
            opts.create_if_missing(true);
            opts.set_compression_type(rocksdb::DBCompressionType::Lz4);
            opts.set_write_buffer_size(64 * 1024 * 1024); // 64MB
            opts.set_max_write_buffer_number(3);
            opts.set_target_file_size_base(64 * 1024 * 1024);
            opts.set_level_zero_file_num_compaction_trigger(8);
            opts.set_level_zero_slowdown_writes_trigger(17);
            opts.set_level_zero_stop_writes_trigger(24);
            opts.set_max_background_jobs(4);
            opts.set_bytes_per_sync(1048576);

            let db = Arc::new(rocksdb::DB::open(&opts, db_path)?);
            
            let store = DAGVertexStore {
                rocksdb: Some(db),
                #[cfg(feature = "sled-backend")]
                sled_db: None,
                #[cfg(feature = "sled-backend")]
                vertices_tree: None,
                #[cfg(feature = "sled-backend")]
                parents_tree: None,
                #[cfg(feature = "sled-backend")]
                children_tree: None,
                #[cfg(feature = "sled-backend")]
                shard_tree: None,
                vertex_cache,
                parents_cache,
                stats,
                cache_size,
            };
            
            store.update_vertex_count()?;
            return Ok(store);
        }

        #[cfg(feature = "sled-backend")]
        {
            let db = sled::open(db_path)?;
            let vertices_tree = db.open_tree("vertices")?;
            let parents_tree = db.open_tree("parents")?;
            let children_tree = db.open_tree("children")?;
            let shard_tree = db.open_tree("shards")?;
            
            let store = DAGVertexStore {
                #[cfg(feature = "rocksdb-backend")]
                rocksdb: None,
                sled_db: Some(db),
                vertices_tree: Some(vertices_tree),
                parents_tree: Some(parents_tree),
                children_tree: Some(children_tree),
                shard_tree: Some(shard_tree),
                vertex_cache,
                parents_cache,
                stats,
                cache_size,
            };
            
            store.update_vertex_count()?;
            return Ok(store);
        }

        #[cfg(not(any(feature = "rocksdb-backend", feature = "sled-backend")))]
        {
            panic!("No database backend enabled. Enable either 'rocksdb-backend' or 'sled-backend' feature.");
        }
    }

    pub fn store_vertex(&self, vertex: &DAGVertex) -> StorageResult<()> {
        let vertex_key = vertex.hash.to_vec();
        let vertex_data = bincode::serialize(vertex)?;

        #[cfg(feature = "rocksdb-backend")]
        if let Some(ref db) = self.rocksdb {
            // Store vertex
            db.put(&vertex_key, &vertex_data)?;
            
            // Store parent relationships
            if !vertex.parents.is_empty() {
                let mut parents_key = vec![0x01];
                parents_key.extend_from_slice(&vertex_key);
                let parents_data = bincode::serialize(&vertex.parents)?;
                db.put(parents_key, parents_data)?;
                
                // Update children relationships
                for parent_hash in &vertex.parents {
                    self.add_child_relationship_rocksdb(db, parent_hash, &vertex.hash)?;
                }
            }
            
            // Store shard mapping
            let shard_key = format!("shard_{}", vertex.shard_id).into_bytes();
            let mut shard_vertices = self.get_shard_vertices(vertex.shard_id)?;
            shard_vertices.push(vertex.hash);
            let shard_data = bincode::serialize(&shard_vertices)?;
            let mut full_shard_key = vec![0x02];
            full_shard_key.extend_from_slice(&shard_key);
            db.put(full_shard_key, shard_data)?;
        }

        #[cfg(feature = "sled-backend")]
        if let (Some(ref vertices_tree), Some(ref parents_tree), Some(ref shard_tree)) = 
            (&self.vertices_tree, &self.parents_tree, &self.shard_tree) {
            
            // Store vertex
            vertices_tree.insert(&vertex_key, vertex_data.clone())?;
            
            // Store parent relationships
            if !vertex.parents.is_empty() {
                let parents_data = bincode::serialize(&vertex.parents)?;
                parents_tree.insert(&vertex_key, parents_data)?;
                
                // Update children relationships
                for parent_hash in &vertex.parents {
                    self.add_child_relationship_sled(parent_hash, &vertex.hash)?;
                }
            }
            
            // Store shard mapping
            let shard_key = format!("shard_{}", vertex.shard_id);
            let mut shard_vertices = self.get_shard_vertices(vertex.shard_id)?;
            shard_vertices.push(vertex.hash);
            let shard_data = bincode::serialize(&shard_vertices)?;
            shard_tree.insert(shard_key.as_bytes(), shard_data)?;
            
            // Flush to disk
            if let Some(ref db) = self.sled_db {
                db.flush()?;
            }
        }

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

        Ok(())
    }

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
        let vertex_key = hash.to_vec();

        #[cfg(feature = "rocksdb-backend")]
        if let Some(ref db) = self.rocksdb {
            if let Some(vertex_data) = db.get(&vertex_key)? {
                let vertex: DAGVertex = bincode::deserialize(&vertex_data)?;
                
                // Update cache
                {
                    let mut cache = self.vertex_cache.write();
                    cache.put(*hash, vertex.clone());
                }
                
                self.increment_read_operations();
                return Ok(Some(vertex));
            }
        }

        #[cfg(feature = "sled-backend")]
        if let Some(ref vertices_tree) = self.vertices_tree {
            if let Some(vertex_data) = vertices_tree.get(&vertex_key)? {
                let vertex: DAGVertex = bincode::deserialize(&vertex_data)?;
                
                // Update cache
                {
                    let mut cache = self.vertex_cache.write();
                    cache.put(*hash, vertex.clone());
                }
                
                self.increment_read_operations();
                return Ok(Some(vertex));
            }
        }

        Ok(None)
    }

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
        let vertex_key = hash.to_vec();

        #[cfg(feature = "rocksdb-backend")]
        if let Some(ref db) = self.rocksdb {
            let mut parents_key = vec![0x01];
            parents_key.extend_from_slice(&vertex_key);
            if let Some(parents_data) = db.get(parents_key)? {
                let parents: Vec<VertexHash> = bincode::deserialize(&parents_data)?;
                
                // Update cache
                {
                    let mut cache = self.parents_cache.write();
                    cache.put(*hash, parents.clone());
                }
                
                self.increment_read_operations();
                return Ok(parents);
            }
        }

        #[cfg(feature = "sled-backend")]
        if let Some(ref parents_tree) = self.parents_tree {
            if let Some(parents_data) = parents_tree.get(&vertex_key)? {
                let parents: Vec<VertexHash> = bincode::deserialize(&parents_data)?;
                
                // Update cache
                {
                    let mut cache = self.parents_cache.write();
                    cache.put(*hash, parents.clone());
                }
                
                self.increment_read_operations();
                return Ok(parents);
            }
        }

        Ok(Vec::new())
    }

    pub fn get_children(&self, hash: &VertexHash) -> StorageResult<Vec<VertexHash>> {
        let vertex_key = hash.to_vec();

        #[cfg(feature = "rocksdb-backend")]
        if let Some(ref db) = self.rocksdb {
            let mut children_key = vec![0x03];
            children_key.extend_from_slice(&vertex_key);
            if let Some(children_data) = db.get(children_key)? {
                let children: Vec<VertexHash> = bincode::deserialize(&children_data)?;
                self.increment_read_operations();
                return Ok(children);
            }
        }

        #[cfg(feature = "sled-backend")]
        if let Some(ref children_tree) = self.children_tree {
            if let Some(children_data) = children_tree.get(&vertex_key)? {
                let children: Vec<VertexHash> = bincode::deserialize(&children_data)?;
                self.increment_read_operations();
                return Ok(children);
            }
        }

        Ok(Vec::new())
    }

    pub fn get_shard_vertices(&self, shard_id: ShardId) -> StorageResult<Vec<VertexHash>> {
        let shard_key = format!("shard_{}", shard_id);

        #[cfg(feature = "rocksdb-backend")]
        if let Some(ref db) = self.rocksdb {
            let mut key = vec![0x02];
            key.extend_from_slice(shard_key.as_bytes());
            if let Some(shard_data) = db.get(key)? {
                let vertices: Vec<VertexHash> = bincode::deserialize(&shard_data)?;
                self.increment_read_operations();
                return Ok(vertices);
            }
        }

        #[cfg(feature = "sled-backend")]
        if let Some(ref shard_tree) = self.shard_tree {
            if let Some(shard_data) = shard_tree.get(shard_key.as_bytes())? {
                let vertices: Vec<VertexHash> = bincode::deserialize(&shard_data)?;
                self.increment_read_operations();
                return Ok(vertices);
            }
        }

        Ok(Vec::new())
    }

    pub fn vertex_exists(&self, hash: &VertexHash) -> StorageResult<bool> {
        // Check cache first
        {
            let cache = self.vertex_cache.read();
            if cache.peek(hash).is_some() {
                return Ok(true);
            }
        }

        let vertex_key = hash.to_vec();

        #[cfg(feature = "rocksdb-backend")]
        if let Some(ref db) = self.rocksdb {
            return Ok(db.get(&vertex_key)?.is_some());
        }

        #[cfg(feature = "sled-backend")]
        if let Some(ref vertices_tree) = self.vertices_tree {
            return Ok(vertices_tree.contains_key(&vertex_key)?);
        }

        Ok(false)
    }

    pub fn get_stats(&self) -> StorageStats {
        self.stats.read().clone()
    }

    pub fn compact(&self) -> StorageResult<()> {
        #[cfg(feature = "rocksdb-backend")]
        if let Some(ref db) = self.rocksdb {
            db.compact_range::<&[u8], &[u8]>(None, None);
        }

        #[cfg(feature = "sled-backend")]
        if let Some(ref db) = self.sled_db {
            db.flush()?;
        }

        Ok(())
    }

    // Private helper methods

    #[cfg(feature = "rocksdb-backend")]
    fn add_child_relationship_rocksdb(&self, db: &rocksdb::DB, parent_hash: &VertexHash, child_hash: &VertexHash) -> StorageResult<()> {
        let parent_key = parent_hash.to_vec();
        let mut children_key = vec![0x03];
        children_key.extend_from_slice(&parent_key);
        
        let mut children = if let Some(children_data) = db.get(&children_key)? {
            bincode::deserialize::<Vec<VertexHash>>(&children_data)?
        } else {
            Vec::new()
        };
        
        if !children.contains(child_hash) {
            children.push(*child_hash);
            let children_data = bincode::serialize(&children)?;
            db.put(children_key, children_data)?;
        }
        
        Ok(())
    }

    #[cfg(feature = "sled-backend")]
    fn add_child_relationship_sled(&self, parent_hash: &VertexHash, child_hash: &VertexHash) -> StorageResult<()> {
        if let Some(ref children_tree) = self.children_tree {
            let parent_key = parent_hash.to_vec();
            let mut children = if let Some(children_data) = children_tree.get(&parent_key)? {
                bincode::deserialize::<Vec<VertexHash>>(&children_data)?
            } else {
                Vec::new()
            };
            
            if !children.contains(child_hash) {
                children.push(*child_hash);
                let children_data = bincode::serialize(&children)?;
                children_tree.insert(&parent_key, children_data)?;
            }
        }
        
        Ok(())
    }

    fn update_vertex_count(&self) -> StorageResult<()> {
        #[cfg(feature = "rocksdb-backend")]
        if let Some(ref db) = self.rocksdb {
            let iter = db.iterator(rocksdb::IteratorMode::Start);
            let count = iter.count() as u64;
            
            {
                let mut stats = self.stats.write();
                stats.total_vertices = count;
            }
        }

        #[cfg(feature = "sled-backend")]
        if let Some(ref vertices_tree) = self.vertices_tree {
            let count = vertices_tree.len() as u64;
            let size = if let Some(ref db) = self.sled_db {
                db.size_on_disk()?
            } else {
                0
            };
            
            {
                let mut stats = self.stats.write();
                stats.total_vertices = count;
                stats.total_size_bytes = size;
            }
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