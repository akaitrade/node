/*!
 * C Foreign Function Interface (FFI) for DAG Engine
 * 
 * Provides C-compatible interface for integration with existing C++ CREDITS node
 */

use crate::{
    DAGEngine, DAGVertex, DAGEngineConfig, DAGStatistics, DAGEvent,
    TransactionData, BLSSignature, VertexHash, ShardId,
    ConsensusConfig, ShardConfig, DAGError, DAGErrorCode,
};
use std::ffi::{CStr, CString};
use std::os::raw::{c_char, c_int, c_uint, c_ulong};
use std::ptr;
use std::slice;
use std::sync::Arc;
use parking_lot::Mutex;
use std::collections::HashMap;
use env_logger;

/// Opaque handle for DAG engine instances
pub type DAGEngineHandle = *mut std::ffi::c_void;

use std::sync::Once;

/// Global registry of DAG engine instances
static mut DAG_ENGINES: Option<Mutex<HashMap<usize, Arc<DAGEngine>>>> = None;
static DAG_ENGINES_INIT: Once = Once::new();
static NEXT_HANDLE: Mutex<usize> = Mutex::new(1);

fn get_dag_engines() -> &'static Mutex<HashMap<usize, Arc<DAGEngine>>> {
    unsafe {
        DAG_ENGINES_INIT.call_once(|| {
            DAG_ENGINES = Some(Mutex::new(HashMap::new()));
        });
        DAG_ENGINES.as_ref().unwrap()
    }
}

/// C-compatible DAG engine configuration
#[repr(C)]
#[derive(Debug)]
pub struct CDAGEngineConfig {
    pub storage_path: *const c_char,
    pub consensus_config: CConsensusConfig,
    pub shard_config: CShardConfig,
}

/// C-compatible consensus configuration
#[repr(C)]
#[derive(Debug)]
pub struct CConsensusConfig {
    pub min_validators: c_uint,
    pub max_validators: c_uint,
    pub bft_threshold: f64,
    pub round_timeout_ms: c_ulong,
    pub max_finality_rounds: c_uint,
}

/// C-compatible shard configuration
#[repr(C)]
#[derive(Debug)]
pub struct CShardConfig {
    pub initial_shard_count: c_uint,
    pub max_shard_tps: c_uint,
    pub min_shard_tps: c_uint,
    pub max_shard_count: c_uint,
    pub rebalance_interval_secs: c_ulong,
}

/// C-compatible DAG vertex
#[repr(C)]
#[derive(Debug)]
pub struct CDAGVertex {
    pub hash: [u8; 32],
    pub tx_hash: [u8; 32],
    pub logical_clock: c_ulong,
    pub parent_count: c_uint,
    pub parents: *const [u8; 32],
    pub shard_id: c_uint,
    pub transaction_data: CTransactionData,
    pub signature: CBLSSignature,
    pub timestamp: c_ulong,
}

/// C-compatible transaction data
#[repr(C)]
#[derive(Debug)]
pub struct CTransactionData {
    pub source: [u8; 32],
    pub target: [u8; 32],
    pub amount: c_ulong,
    pub currency: c_uint,
    pub fee: c_ulong,
    pub nonce: c_ulong,
    pub user_data_len: c_uint,
    pub user_data: *const u8,
}

/// C-compatible BLS signature
#[repr(C)]
#[derive(Debug)]
pub struct CBLSSignature {
    pub signature: [u8; 48],
    pub public_key: [u8; 48],
}

/// C-compatible DAG statistics
#[repr(C)]
#[derive(Debug)]
pub struct CDAGStatistics {
    pub total_vertices: c_ulong,
    pub active_shards: c_uint,
    pub cache_hit_rate: f64,
    pub consensus_rounds: c_ulong,
}

/// Create new DAG engine instance
#[no_mangle]
pub extern "C" fn dag_engine_new(config: *const CDAGEngineConfig) -> DAGEngineHandle {
    if config.is_null() {
        return ptr::null_mut();
    }

    let config = unsafe { &*config };
    
    // Convert C configuration to Rust
    let storage_path = unsafe {
        if config.storage_path.is_null() {
            return ptr::null_mut();
        }
        match CStr::from_ptr(config.storage_path).to_str() {
            Ok(path) => path.to_string(),
            Err(_) => return ptr::null_mut(),
        }
    };

    let rust_config = DAGEngineConfig {
        storage_path,
        consensus_config: ConsensusConfig {
            min_validators: config.consensus_config.min_validators,
            max_validators: config.consensus_config.max_validators,
            bft_threshold: config.consensus_config.bft_threshold,
            round_timeout_ms: config.consensus_config.round_timeout_ms as u64,
            max_finality_rounds: config.consensus_config.max_finality_rounds,
        },
        shard_config: ShardConfig {
            initial_shard_count: config.shard_config.initial_shard_count,
            max_shard_tps: config.shard_config.max_shard_tps,
            min_shard_tps: config.shard_config.min_shard_tps,
            max_shard_count: config.shard_config.max_shard_count,
            rebalance_interval_secs: config.shard_config.rebalance_interval_secs as u64,
        },
    };

    // Create DAG engine (now synchronous)
    let engine = match DAGEngine::new(rust_config) {
        Ok(engine) => Arc::new(engine),
        Err(_) => return ptr::null_mut(),
    };

    // Store in global registry
    let mut engines = get_dag_engines().lock();
    let mut next_handle = NEXT_HANDLE.lock();
    let handle = *next_handle;
    *next_handle += 1;
    
    engines.insert(handle, engine);
    handle as DAGEngineHandle
}

/// Destroy DAG engine instance
#[no_mangle]
pub extern "C" fn dag_engine_destroy(handle: DAGEngineHandle) {
    if handle.is_null() {
        return;
    }

    let handle_id = handle as usize;
    let mut engines = get_dag_engines().lock();
    engines.remove(&handle_id);
}

/// Insert vertex into DAG
#[no_mangle]
pub extern "C" fn dag_engine_insert_vertex(
    handle: DAGEngineHandle,
    vertex: *const CDAGVertex,
) -> DAGErrorCode {
    if handle.is_null() || vertex.is_null() {
        return DAGErrorCode::FFIError;
    }

    let handle_id = handle as usize;
    let engines = get_dag_engines().lock();
    let engine = match engines.get(&handle_id) {
        Some(engine) => engine.clone(),
        None => return DAGErrorCode::FFIError,
    };
    drop(engines);

    let c_vertex = unsafe { &*vertex };

    // Convert C vertex to Rust vertex
    let parents = if c_vertex.parent_count > 0 && !c_vertex.parents.is_null() {
        unsafe {
            slice::from_raw_parts(c_vertex.parents, c_vertex.parent_count as usize)
                .iter()
                .cloned()
                .collect()
        }
    } else {
        Vec::new()
    };

    let user_data = if c_vertex.transaction_data.user_data_len > 0 && !c_vertex.transaction_data.user_data.is_null() {
        unsafe {
            slice::from_raw_parts(
                c_vertex.transaction_data.user_data,
                c_vertex.transaction_data.user_data_len as usize,
            ).to_vec()
        }
    } else {
        Vec::new()
    };

    let transaction_data = TransactionData {
        source: c_vertex.transaction_data.source,
        target: c_vertex.transaction_data.target,
        amount: c_vertex.transaction_data.amount as u64,
        currency: c_vertex.transaction_data.currency,
        fee: c_vertex.transaction_data.fee as u64,
        nonce: c_vertex.transaction_data.nonce as u64,
        user_data,
    };

    let signature = BLSSignature {
        signature: c_vertex.signature.signature,
        public_key: c_vertex.signature.public_key,
        aggregate_info: None,
    };

    let rust_vertex = DAGVertex {
        hash: c_vertex.hash,
        tx_hash: c_vertex.tx_hash,
        logical_clock: c_vertex.logical_clock as u64,
        parents,
        shard_id: c_vertex.shard_id,
        transaction_data,
        signature,
        timestamp: c_vertex.timestamp as u64,
        proof: None,
    };

    // Insert vertex asynchronously
    let rt = match tokio::runtime::Runtime::new() {
        Ok(rt) => rt,
        Err(_) => return DAGErrorCode::FFIError,
    };

    match rt.block_on(engine.insert_vertex(rust_vertex)) {
        Ok(_) => DAGErrorCode::Success,
        Err(e) => e.into(),
    }
}

/// Get vertex by hash
#[no_mangle]
pub extern "C" fn dag_engine_get_vertex(
    handle: DAGEngineHandle,
    hash: *const [u8; 32],
    vertex_out: *mut CDAGVertex,
) -> DAGErrorCode {
    if handle.is_null() || hash.is_null() || vertex_out.is_null() {
        return DAGErrorCode::FFIError;
    }

    let handle_id = handle as usize;
    let engines = get_dag_engines().lock();
    let engine = match engines.get(&handle_id) {
        Some(engine) => engine.clone(),
        None => return DAGErrorCode::FFIError,
    };
    drop(engines);

    let vertex_hash = unsafe { *hash };

    // Get vertex asynchronously
    let rt = match tokio::runtime::Runtime::new() {
        Ok(rt) => rt,
        Err(_) => return DAGErrorCode::FFIError,
    };

    let vertex = match rt.block_on(engine.get_vertex(&vertex_hash)) {
        Ok(Some(vertex)) => vertex,
        Ok(None) => return DAGErrorCode::ValidationError, // Not found
        Err(e) => return e.into(),
    };

    // Convert Rust vertex to C vertex
    unsafe {
        (*vertex_out).hash = vertex.hash;
        (*vertex_out).tx_hash = vertex.tx_hash;
        (*vertex_out).logical_clock = vertex.logical_clock as c_ulong;
        (*vertex_out).parent_count = vertex.parents.len() as c_uint;
        (*vertex_out).shard_id = vertex.shard_id;
        (*vertex_out).timestamp = vertex.timestamp as c_ulong;

        // Copy transaction data
        (*vertex_out).transaction_data.source = vertex.transaction_data.source;
        (*vertex_out).transaction_data.target = vertex.transaction_data.target;
        (*vertex_out).transaction_data.amount = vertex.transaction_data.amount as c_ulong;
        (*vertex_out).transaction_data.currency = vertex.transaction_data.currency;
        (*vertex_out).transaction_data.fee = vertex.transaction_data.fee as c_ulong;
        (*vertex_out).transaction_data.nonce = vertex.transaction_data.nonce as c_ulong;
        (*vertex_out).transaction_data.user_data_len = vertex.transaction_data.user_data.len() as c_uint;

        // Copy signature
        (*vertex_out).signature.signature = vertex.signature.signature;
        (*vertex_out).signature.public_key = vertex.signature.public_key;

        // Note: Caller is responsible for managing memory for parents and user_data
        // This is a simplified implementation for demonstration
    }

    DAGErrorCode::Success
}

/// Get DAG statistics
#[no_mangle]
pub extern "C" fn dag_engine_get_statistics(
    handle: DAGEngineHandle,
    stats_out: *mut CDAGStatistics,
) -> DAGErrorCode {
    if handle.is_null() || stats_out.is_null() {
        return DAGErrorCode::FFIError;
    }

    let handle_id = handle as usize;
    let engines = get_dag_engines().lock();
    let engine = match engines.get(&handle_id) {
        Some(engine) => engine.clone(),
        None => return DAGErrorCode::FFIError,
    };
    drop(engines);

    // Get statistics asynchronously
    let rt = match tokio::runtime::Runtime::new() {
        Ok(rt) => rt,
        Err(_) => return DAGErrorCode::FFIError,
    };

    let stats = match rt.block_on(engine.get_statistics()) {
        Ok(stats) => stats,
        Err(e) => return e.into(),
    };

    // Convert to C statistics
    unsafe {
        (*stats_out).total_vertices = stats.total_vertices as c_ulong;
        (*stats_out).active_shards = stats.active_shards;
        (*stats_out).cache_hit_rate = stats.cache_hit_rate;
        (*stats_out).consensus_rounds = stats.consensus_rounds as c_ulong;
    }

    DAGErrorCode::Success
}

/// Get error message for error code
#[no_mangle]
pub extern "C" fn dag_error_message(error_code: DAGErrorCode) -> *const c_char {
    let message = match error_code {
        DAGErrorCode::Success => "Success",
        DAGErrorCode::StorageError => "Storage error",
        DAGErrorCode::SerializationError => "Serialization error",
        DAGErrorCode::InvalidVertex => "Invalid vertex",
        DAGErrorCode::ConsensusError => "Consensus error",
        DAGErrorCode::ShardError => "Shard error",
        DAGErrorCode::NetworkError => "Network error",
        DAGErrorCode::ConfigError => "Configuration error",
        DAGErrorCode::ValidationError => "Validation error",
        DAGErrorCode::TimeoutError => "Timeout error",
        DAGErrorCode::CryptoError => "Cryptographic error",
        DAGErrorCode::FFIError => "FFI error",
        DAGErrorCode::IOError => "IO error",
        DAGErrorCode::UnknownError => "Unknown error",
    };

    // Note: This returns a static string, which is safe for FFI
    // In production, might want to use a thread-local storage for dynamic messages
    message.as_ptr() as *const c_char
}

/// Initialize the DAG engine library
#[no_mangle]
pub extern "C" fn dag_engine_init() -> DAGErrorCode {
    // Initialize logging and other global state if needed
    env_logger::try_init().ok();
    DAGErrorCode::Success
}

/// Cleanup the DAG engine library
#[no_mangle]
pub extern "C" fn dag_engine_cleanup() {
    // Cleanup global state
    let mut engines = get_dag_engines().lock();
    engines.clear();
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::ffi::CString;

    #[test]
    fn test_ffi_config_conversion() {
        let storage_path = CString::new("/tmp/test").unwrap();
        let c_config = CDAGEngineConfig {
            storage_path: storage_path.as_ptr(),
            consensus_config: CConsensusConfig {
                min_validators: 4,
                max_validators: 25,
                bft_threshold: 0.67,
                round_timeout_ms: 2000,
                max_finality_rounds: 10,
            },
            shard_config: CShardConfig {
                initial_shard_count: 4,
                max_shard_tps: 10000,
                min_shard_tps: 1000,
                max_shard_count: 1024,
                rebalance_interval_secs: 300,
            },
        };

        // Test that configuration conversion doesn't crash
        let handle = dag_engine_new(&c_config);
        assert!(!handle.is_null());
        
        dag_engine_destroy(handle);
    }

    #[test]
    fn test_error_code_conversion() {
        let storage_error = DAGError::StorageError("test".to_string());
        let error_code: DAGErrorCode = storage_error.into();
        assert_eq!(error_code, DAGErrorCode::StorageError);
    }

    #[test]
    fn test_error_messages() {
        let message = dag_error_message(DAGErrorCode::Success);
        assert!(!message.is_null());
        
        let message = dag_error_message(DAGErrorCode::StorageError);
        assert!(!message.is_null());
    }
}