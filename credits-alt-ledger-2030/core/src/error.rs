/*!
 * Error types for ALT-LEDGER 2030 DAG Engine
 */

use thiserror::Error;

/// Main error type for DAG operations
#[derive(Error, Debug, Clone)]
pub enum DAGError {
    #[error("Storage error: {0}")]
    StorageError(String),

    #[error("Serialization error: {0}")]
    SerializationError(String),

    #[error("Invalid vertex: {0}")]
    InvalidVertex(String),

    #[error("Consensus error: {0}")]
    ConsensusError(String),

    #[error("Shard error: {0}")]
    ShardError(String),

    #[error("Network error: {0}")]
    NetworkError(String),

    #[error("Configuration error: {0}")]
    ConfigError(String),

    #[error("Validation error: {0}")]
    ValidationError(String),

    #[error("Timeout error: {0}")]
    TimeoutError(String),

    #[error("Cryptographic error: {0}")]
    CryptoError(String),

    #[error("FFI error: {0}")]
    FFIError(String),

    #[error("IO error: {0}")]
    IOError(String),
}

impl From<std::io::Error> for DAGError {
    fn from(error: std::io::Error) -> Self {
        DAGError::IOError(error.to_string())
    }
}

impl From<serde_json::Error> for DAGError {
    fn from(error: serde_json::Error) -> Self {
        DAGError::SerializationError(error.to_string())
    }
}

impl From<bincode::Error> for DAGError {
    fn from(error: bincode::Error) -> Self {
        DAGError::SerializationError(error.to_string())
    }
}

/// Result type for DAG operations
pub type DAGResult<T> = Result<T, DAGError>;

/// Error codes for FFI interface
#[repr(C)]
#[derive(Debug, Clone, Copy, PartialEq)]
pub enum DAGErrorCode {
    Success = 0,
    StorageError = 1,
    SerializationError = 2,
    InvalidVertex = 3,
    ConsensusError = 4,
    ShardError = 5,
    NetworkError = 6,
    ConfigError = 7,
    ValidationError = 8,
    TimeoutError = 9,
    CryptoError = 10,
    FFIError = 11,
    IOError = 12,
    UnknownError = 99,
}

impl From<DAGError> for DAGErrorCode {
    fn from(error: DAGError) -> Self {
        match error {
            DAGError::StorageError(_) => DAGErrorCode::StorageError,
            DAGError::SerializationError(_) => DAGErrorCode::SerializationError,
            DAGError::InvalidVertex(_) => DAGErrorCode::InvalidVertex,
            DAGError::ConsensusError(_) => DAGErrorCode::ConsensusError,
            DAGError::ShardError(_) => DAGErrorCode::ShardError,
            DAGError::NetworkError(_) => DAGErrorCode::NetworkError,
            DAGError::ConfigError(_) => DAGErrorCode::ConfigError,
            DAGError::ValidationError(_) => DAGErrorCode::ValidationError,
            DAGError::TimeoutError(_) => DAGErrorCode::TimeoutError,
            DAGError::CryptoError(_) => DAGErrorCode::CryptoError,
            DAGError::FFIError(_) => DAGErrorCode::FFIError,
            DAGError::IOError(_) => DAGErrorCode::IOError,
        }
    }
}