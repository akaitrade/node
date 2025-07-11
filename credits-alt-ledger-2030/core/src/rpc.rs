/*!
 * CREDITS ALT-LEDGER 2030 - HTTP RPC Server
 * 
 * Real HTTP API server for blockchain operations
 */

use std::convert::Infallible;
use std::sync::Arc;
use hyper::service::{make_service_fn, service_fn};
use hyper::{Body, Method, Request, Response, Server, StatusCode};
use serde_json::json;
use crate::{DAGEngine, DAGError};

/// RPC server for handling HTTP API requests
pub struct RPCServer {
    port: u16,
    dag_engine: Arc<DAGEngine>,
}

impl RPCServer {
    /// Create new RPC server
    pub fn new(port: u16, dag_engine: Arc<DAGEngine>) -> Self {
        Self {
            port,
            dag_engine,
        }
    }

    /// Start the RPC server
    pub async fn start(&self) -> Result<(), DAGError> {
        let addr = ([0, 0, 0, 0], self.port).into();
        let dag_engine = self.dag_engine.clone();

        let make_svc = make_service_fn(move |_conn| {
            let dag_engine = dag_engine.clone();
            async move {
                Ok::<_, Infallible>(service_fn(move |req| {
                    let dag_engine = dag_engine.clone();
                    handle_request(req, dag_engine)
                }))
            }
        });

        let server = Server::bind(&addr).serve(make_svc);

        println!("🌐 HTTP RPC server listening on http://0.0.0.0:{}", self.port);

        if let Err(e) = server.await {
            return Err(DAGError::NetworkError(format!("RPC server error: {}", e)));
        }

        Ok(())
    }
}

/// Handle individual HTTP requests
async fn handle_request(
    req: Request<Body>,
    dag_engine: Arc<DAGEngine>,
) -> Result<Response<Body>, Infallible> {
    let response = match (req.method(), req.uri().path()) {
        (&Method::GET, "/status") => handle_status(dag_engine).await,
        (&Method::GET, "/stats") => handle_stats(dag_engine).await,
        (&Method::POST, "/create") => handle_create_vertex(req, dag_engine).await,
        (&Method::GET, path) if path.starts_with("/vertex/") => {
            let hash_str = &path[8..]; // Remove "/vertex/" prefix
            handle_get_vertex(hash_str, dag_engine).await
        }
        (&Method::GET, "/health") => handle_health().await,
        _ => {
            let mut response = Response::new(Body::from("Not Found"));
            *response.status_mut() = StatusCode::NOT_FOUND;
            response
        }
    };

    // Add CORS headers
    let mut response = response;
    response.headers_mut().insert("Access-Control-Allow-Origin", "*".parse().unwrap());
    response.headers_mut().insert("Access-Control-Allow-Methods", "GET, POST, OPTIONS".parse().unwrap());
    response.headers_mut().insert("Access-Control-Allow-Headers", "Content-Type".parse().unwrap());
    response.headers_mut().insert("Content-Type", "application/json".parse().unwrap());

    Ok(response)
}

/// Handle /status endpoint
async fn handle_status(dag_engine: Arc<DAGEngine>) -> Response<Body> {
    match dag_engine.get_statistics().await {
        Ok(stats) => {
            let status = json!({
                "status": "running",
                "version": "1.0.0",
                "total_vertices": stats.total_vertices,
                "active_shards": stats.active_shards,
                "consensus_rounds": stats.consensus_rounds,
                "cache_hit_rate": stats.cache_hit_rate,
                "uptime": std::time::SystemTime::now()
                    .duration_since(std::time::UNIX_EPOCH)
                    .unwrap_or_default()
                    .as_secs()
            });
            
            Response::new(Body::from(status.to_string()))
        }
        Err(e) => {
            let error = json!({
                "error": "Failed to get status",
                "details": e.to_string()
            });
            
            let mut response = Response::new(Body::from(error.to_string()));
            *response.status_mut() = StatusCode::INTERNAL_SERVER_ERROR;
            response
        }
    }
}

/// Handle /stats endpoint
async fn handle_stats(dag_engine: Arc<DAGEngine>) -> Response<Body> {
    match dag_engine.get_statistics().await {
        Ok(stats) => {
            let response = json!({
                "total_vertices": stats.total_vertices,
                "active_shards": stats.active_shards,
                "cache_hit_rate": stats.cache_hit_rate,
                "consensus_rounds": stats.consensus_rounds
            });
            
            Response::new(Body::from(response.to_string()))
        }
        Err(e) => {
            let error = json!({
                "error": "Failed to get statistics",
                "details": e.to_string()
            });
            
            let mut response = Response::new(Body::from(error.to_string()));
            *response.status_mut() = StatusCode::INTERNAL_SERVER_ERROR;
            response
        }
    }
}

/// Handle /create endpoint
async fn handle_create_vertex(req: Request<Body>, dag_engine: Arc<DAGEngine>) -> Response<Body> {
    // Read request body
    let body_bytes = match hyper::body::to_bytes(req.into_body()).await {
        Ok(bytes) => bytes,
        Err(e) => {
            let error = json!({
                "error": "Failed to read request body",
                "details": e.to_string()
            });
            
            let mut response = Response::new(Body::from(error.to_string()));
            *response.status_mut() = StatusCode::BAD_REQUEST;
            return response;
        }
    };

    // Parse JSON
    let json_value: serde_json::Value = match serde_json::from_slice(&body_bytes) {
        Ok(value) => value,
        Err(e) => {
            let error = json!({
                "error": "Invalid JSON",
                "details": e.to_string()
            });
            
            let mut response = Response::new(Body::from(error.to_string()));
            *response.status_mut() = StatusCode::BAD_REQUEST;
            return response;
        }
    };

    // Extract data field
    let data = match json_value.get("data").and_then(|v| v.as_str()) {
        Some(data) => data,
        None => {
            let error = json!({
                "error": "Missing 'data' field in request"
            });
            
            let mut response = Response::new(Body::from(error.to_string()));
            *response.status_mut() = StatusCode::BAD_REQUEST;
            return response;
        }
    };

    // Create vertex (simplified - in real implementation this would be more complex)
    let vertex_result = create_simple_vertex(data.to_string(), &dag_engine).await;

    match vertex_result {
        Ok(vertex) => {
            let response = json!({
                "success": true,
                "message": "Vertex created successfully",
                "vertex": {
                    "hash": format!("{:?}", vertex.hash),
                    "height": vertex.logical_clock,
                    "timestamp": vertex.timestamp,
                    "data_size": vertex.transaction_data.user_data.len()
                }
            });
            
            Response::new(Body::from(response.to_string()))
        }
        Err(e) => {
            let error = json!({
                "error": "Failed to create vertex",
                "details": e.to_string()
            });
            
            let mut response = Response::new(Body::from(error.to_string()));
            *response.status_mut() = StatusCode::INTERNAL_SERVER_ERROR;
            response
        }
    }
}

/// Handle /vertex/{hash} endpoint
async fn handle_get_vertex(hash_str: &str, dag_engine: Arc<DAGEngine>) -> Response<Body> {
    // Parse hash (simplified)
    let hash_result = parse_vertex_hash(hash_str);
    
    match hash_result {
        Ok(hash) => {
            match dag_engine.get_vertex(&hash).await {
                Ok(Some(vertex)) => {
                    let response = json!({
                        "found": true,
                        "vertex": {
                            "hash": format!("{:?}", vertex.hash),
                            "height": vertex.logical_clock,
                            "timestamp": vertex.timestamp,
                            "parents": vertex.parents.len(),
                            "data_size": vertex.transaction_data.user_data.len()
                        }
                    });
                    
                    Response::new(Body::from(response.to_string()))
                }
                Ok(None) => {
                    let response = json!({
                        "found": false,
                        "message": "Vertex not found"
                    });
                    
                    let mut response = Response::new(Body::from(response.to_string()));
                    *response.status_mut() = StatusCode::NOT_FOUND;
                    response
                }
                Err(e) => {
                    let error = json!({
                        "error": "Failed to retrieve vertex",
                        "details": e.to_string()
                    });
                    
                    let mut response = Response::new(Body::from(error.to_string()));
                    *response.status_mut() = StatusCode::INTERNAL_SERVER_ERROR;
                    response
                }
            }
        }
        Err(e) => {
            let error = json!({
                "error": "Invalid hash format",
                "details": e
            });
            
            let mut response = Response::new(Body::from(error.to_string()));
            *response.status_mut() = StatusCode::BAD_REQUEST;
            response
        }
    }
}

/// Handle /health endpoint
async fn handle_health() -> Response<Body> {
    let response = json!({
        "status": "healthy",
        "timestamp": std::time::SystemTime::now()
            .duration_since(std::time::UNIX_EPOCH)
            .unwrap_or_default()
            .as_secs()
    });
    
    Response::new(Body::from(response.to_string()))
}

/// Create a simple vertex (helper function)
async fn create_simple_vertex(data: String, dag_engine: &Arc<DAGEngine>) -> Result<crate::DAGVertex, DAGError> {
    use crate::{DAGVertex, TransactionData, BLSSignature};
    
    // Get current stats to determine height
    let stats = dag_engine.get_statistics().await?;
    
    // Create transaction data
    let transaction_data = TransactionData {
        source: [0; 32], // Simplified for demo
        target: [0; 32], // Simplified for demo
        amount: 0,
        currency: 1,
        fee: 0,
        nonce: stats.total_vertices,
        user_data: data.into_bytes(),
    };

    // Create signature (simplified for demo)
    let signature = BLSSignature {
        signature: [0; 48],
        public_key: [0; 48],
        aggregate_info: None,
    };

    // Generate transaction hash
    let tx_hash = {
        use blake3;
        let mut hasher = blake3::Hasher::new();
        hasher.update(&bincode::serialize(&transaction_data).unwrap_or_default());
        let hash = hasher.finalize();
        let mut result = [0u8; 32];
        result.copy_from_slice(hash.as_bytes());
        result
    };

    let vertex = DAGVertex::new(
        tx_hash,
        stats.total_vertices,
        Vec::new(), // No parents for demo
        0, // Default shard
        transaction_data,
        signature,
    );

    // Insert vertex into DAG
    dag_engine.insert_vertex(vertex.clone()).await?;

    Ok(vertex)
}

/// Parse vertex hash from string (helper function)
fn parse_vertex_hash(hash_str: &str) -> Result<crate::VertexHash, String> {
    if hash_str.len() != 64 {
        return Err("Hash must be 64 characters long".to_string());
    }
    
    let mut hash = [0u8; 32];
    for (i, chunk) in hash_str.as_bytes().chunks(2).enumerate() {
        if i >= 32 {
            return Err("Hash too long".to_string());
        }
        let hex_str = std::str::from_utf8(chunk).map_err(|_| "Invalid UTF-8")?;
        hash[i] = u8::from_str_radix(hex_str, 16).map_err(|_| "Invalid hex character")?;
    }
    
    Ok(hash)
}