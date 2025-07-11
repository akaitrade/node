/*!
 * CREDITS ALT-LEDGER 2030 - Network Module
 * 
 * Real TCP networking implementation for node-to-node communication
 */

use std::collections::HashMap;
use std::net::SocketAddr;
use std::sync::Arc;
use tokio::net::{TcpListener, TcpStream};
use tokio::sync::{mpsc, RwLock, Mutex};
use tokio::io::{AsyncReadExt, AsyncWriteExt};
use serde::{Deserialize, Serialize};
use crate::{DAGVertex, VertexHash, DAGError};

/// Network message types for peer communication
#[derive(Debug, Clone, Serialize, Deserialize)]
pub enum NetworkMessage {
    /// Handshake message to establish connection
    Handshake {
        node_id: String,
        version: String,
        listen_port: u16,
        known_peers: Vec<SocketAddr>,
    },
    /// Response to handshake
    HandshakeResponse {
        node_id: String,
        accepted: bool,
        known_peers: Vec<SocketAddr>,
    },
    /// New vertex to propagate
    NewVertex {
        vertex: DAGVertex,
    },
    /// Request for specific vertex
    VertexRequest {
        hash: VertexHash,
    },
    /// Response with requested vertex
    VertexResponse {
        vertex: Option<DAGVertex>,
    },
    /// Consensus vote
    ConsensusVote {
        round: u64,
        vertex_hash: VertexHash,
        vote: bool,
    },
    /// Heartbeat to keep connection alive
    Ping,
    /// Response to ping
    Pong,
    /// Peer discovery - share known peers
    PeerShare {
        peers: Vec<SocketAddr>,
    },
}

/// Peer connection information
#[derive(Debug, Clone)]
pub struct PeerInfo {
    pub node_id: String,
    pub address: SocketAddr,
    pub last_seen: std::time::Instant,
    pub connected: bool,
}

/// Network manager for handling all network operations
pub struct NetworkManager {
    /// This node's ID
    node_id: String,
    /// Port to listen on
    listen_port: u16,
    /// Connected peers
    peers: Arc<RwLock<HashMap<String, PeerInfo>>>,
    /// Message sender for outgoing messages
    message_tx: mpsc::UnboundedSender<(String, NetworkMessage)>,
    /// Message receiver for incoming messages
    message_rx: Arc<RwLock<mpsc::UnboundedReceiver<(String, NetworkMessage)>>>,
    /// Known peer addresses to connect to
    bootstrap_peers: Vec<SocketAddr>,
}

impl NetworkManager {
    /// Create new network manager
    pub fn new(node_id: String, listen_port: u16, bootstrap_peers: Vec<SocketAddr>) -> Self {
        let (message_tx, message_rx) = mpsc::unbounded_channel();
        
        Self {
            node_id,
            listen_port,
            peers: Arc::new(RwLock::new(HashMap::new())),
            message_tx,
            message_rx: Arc::new(RwLock::new(message_rx)),
            bootstrap_peers,
        }
    }

    /// Start the network manager
    pub async fn start(&self) -> Result<(), DAGError> {
        println!("🌐 Starting network on port {}", self.listen_port);
        
        // Start TCP listener
        self.start_listener().await?;
        
        // Connect to bootstrap peers
        self.connect_to_bootstrap_peers().await;
        
        // Start peer maintenance tasks
        self.start_peer_maintenance().await;
        
        println!("✅ Network started successfully");
        Ok(())
    }

    /// Start TCP listener for incoming connections
    async fn start_listener(&self) -> Result<(), DAGError> {
        let addr = format!("0.0.0.0:{}", self.listen_port);
        let listener = TcpListener::bind(&addr).await
            .map_err(|e| DAGError::NetworkError(format!("Failed to bind to {}: {}", addr, e)))?;
        
        let peers = self.peers.clone();
        let node_id = self.node_id.clone();
        let message_tx = self.message_tx.clone();
        
        tokio::spawn(async move {
            println!("🔗 Listening for connections on {}", addr);
            
            while let Ok((stream, peer_addr)) = listener.accept().await {
                println!("📡 New connection from {}", peer_addr);
                
                let peers = peers.clone();
                let node_id = node_id.clone();
                let message_tx = message_tx.clone();
                
                tokio::spawn(async move {
                    if let Err(e) = Self::handle_peer_connection(stream, peer_addr, peers, node_id, message_tx).await {
                        eprintln!("❌ Peer connection error: {}", e);
                    }
                });
            }
        });
        
        Ok(())
    }

    /// Handle individual peer connection
    async fn handle_peer_connection(
        mut stream: TcpStream,
        peer_addr: SocketAddr,
        peers: Arc<RwLock<HashMap<String, PeerInfo>>>,
        our_node_id: String,
        message_tx: mpsc::UnboundedSender<(String, NetworkMessage)>,
    ) -> Result<(), DAGError> {
        let mut buffer = vec![0; 4096];
        
        loop {
            match stream.read(&mut buffer).await {
                Ok(0) => {
                    // Connection closed
                    println!("📡 Connection closed by {}", peer_addr);
                    break;
                }
                Ok(n) => {
                    // Parse message
                    if let Ok(message) = bincode::deserialize::<NetworkMessage>(&buffer[..n]) {
                        match &message {
                            NetworkMessage::Handshake { node_id, version: _, listen_port, known_peers: _ } => {
                                println!("🤝 Handshake from node {} (listening on port {})", node_id, listen_port);
                                
                                // Create the correct peer address using the listen port, not the source port
                                let peer_listen_addr = std::net::SocketAddr::new(peer_addr.ip(), *listen_port);
                                
                                // Add peer to our list
                                {
                                    let mut peers_write = peers.write().await;
                                    peers_write.insert(node_id.clone(), PeerInfo {
                                        node_id: node_id.clone(),
                                        address: peer_listen_addr,
                                        last_seen: std::time::Instant::now(),
                                        connected: true,
                                    });
                                }
                                
                                // Send handshake response with known peers
                                let response_peers: Vec<SocketAddr> = {
                                    let peers_read = peers.read().await;
                                    peers_read.values()
                                        .filter(|p| p.connected && p.node_id != *node_id)
                                        .map(|p| p.address)
                                        .collect()
                                };
                                
                                println!("🌐 Sending handshake response to {} with {} known peers", node_id, response_peers.len());
                                
                                let response = NetworkMessage::HandshakeResponse {
                                    node_id: our_node_id.clone(),
                                    accepted: true,
                                    known_peers: response_peers,
                                };
                                
                                if let Ok(data) = bincode::serialize(&response) {
                                    let _ = stream.write_all(&data).await;
                                }
                                
                                // Forward message to main handler
                                let _ = message_tx.send((node_id.clone(), message));
                            }
                            _ => {
                                // Forward other messages to main handler
                                if let NetworkMessage::HandshakeResponse { node_id, accepted: _, known_peers: _ } = &message {
                                    let _ = message_tx.send((node_id.clone(), message));
                                }
                            }
                        }
                    }
                }
                Err(e) => {
                    eprintln!("❌ Error reading from {}: {}", peer_addr, e);
                    break;
                }
            }
        }
        
        Ok(())
    }

    /// Connect to bootstrap peers
    async fn connect_to_bootstrap_peers(&self) {
        for peer_addr in &self.bootstrap_peers {
            let node_id = self.node_id.clone();
            let listen_port = self.listen_port;
            let peers = self.peers.clone();
            let message_tx = self.message_tx.clone();
            let addr = *peer_addr;
            
            tokio::spawn(async move {
                if let Err(e) = Self::connect_to_peer(addr, node_id, listen_port, peers, message_tx).await {
                    eprintln!("❌ Failed to connect to {}: {}", addr, e);
                } else {
                    println!("✅ Connected to peer {}", addr);
                }
            });
        }
    }

    /// Connect to a specific peer
    async fn connect_to_peer(
        peer_addr: SocketAddr,
        our_node_id: String,
        our_listen_port: u16,
        peers: Arc<RwLock<HashMap<String, PeerInfo>>>,
        message_tx: mpsc::UnboundedSender<(String, NetworkMessage)>,
    ) -> Result<(), DAGError> {
        // Small delay to ensure the other node is ready
        tokio::time::sleep(tokio::time::Duration::from_secs(2)).await;
        
        let mut stream = TcpStream::connect(peer_addr).await
            .map_err(|e| DAGError::NetworkError(format!("Failed to connect to {}: {}", peer_addr, e)))?;
        
        // Send handshake with known peers
        let known_peers: Vec<SocketAddr> = {
            let peers_read = peers.read().await;
            peers_read.values()
                .filter(|p| p.connected)
                .map(|p| p.address)
                .collect()
        };
        
        println!("🌐 Sending handshake to {} with {} known peers", peer_addr, known_peers.len());
        
        let handshake = NetworkMessage::Handshake {
            node_id: our_node_id.clone(),
            version: "1.0.0".to_string(),
            listen_port: our_listen_port,
            known_peers,
        };
        
        let data = bincode::serialize(&handshake)
            .map_err(|e| DAGError::NetworkError(format!("Serialization error: {}", e)))?;
        
        stream.write_all(&data).await
            .map_err(|e| DAGError::NetworkError(format!("Failed to send handshake: {}", e)))?;
        
        // Wait for handshake response
        let mut buffer = vec![0; 4096];
        match stream.read(&mut buffer).await {
            Ok(n) if n > 0 => {
                if let Ok(response) = bincode::deserialize::<NetworkMessage>(&buffer[..n]) {
                    let response_clone = response.clone();
                    match response {
                        NetworkMessage::HandshakeResponse { node_id, accepted, known_peers: _ } => {
                            if accepted {
                                println!("✅ Handshake accepted by {}", node_id);
                                
                                // Add the peer to our list
                                {
                                    let mut peers_write = peers.write().await;
                                    peers_write.insert(node_id.clone(), PeerInfo {
                                        node_id: node_id.clone(),
                                        address: peer_addr,
                                        last_seen: std::time::Instant::now(),
                                        connected: true,
                                    });
                                }
                                
                                // Forward the handshake response to the main handler for peer discovery
                                println!("📤 Forwarding handshake response to main handler");
                                if let Err(e) = message_tx.send((node_id.clone(), response_clone)) {
                                    println!("❌ Failed to forward handshake response: {}", e);
                                } else {
                                    println!("✅ Handshake response forwarded to main handler");
                                }
                                
                                // Continue handling the connection for future messages
                                Self::handle_peer_connection(stream, peer_addr, peers, our_node_id, message_tx).await
                            } else {
                                println!("❌ Handshake rejected by {}", node_id);
                                Err(DAGError::NetworkError("Handshake rejected".to_string()))
                            }
                        }
                        _ => {
                            println!("❌ Unexpected response from {}", peer_addr);
                            Err(DAGError::NetworkError("Unexpected response".to_string()))
                        }
                    }
                } else {
                    println!("❌ Failed to deserialize response from {}", peer_addr);
                    Err(DAGError::NetworkError("Failed to deserialize response".to_string()))
                }
            }
            Ok(_) => {
                println!("❌ Connection closed by {}", peer_addr);
                Err(DAGError::NetworkError("Connection closed".to_string()))
            }
            Err(e) => {
                println!("❌ Error reading response from {}: {}", peer_addr, e);
                Err(DAGError::NetworkError(format!("Error reading response: {}", e)))
            }
        }
    }

    /// Start peer maintenance tasks
    async fn start_peer_maintenance(&self) {
        let peers = self.peers.clone();
        
        tokio::spawn(async move {
            let mut interval = tokio::time::interval(tokio::time::Duration::from_secs(30));
            
            loop {
                interval.tick().await;
                
                // Clean up disconnected peers
                let mut peers_write = peers.write().await;
                let now = std::time::Instant::now();
                
                peers_write.retain(|_id, peer| {
                    now.duration_since(peer.last_seen).as_secs() < 120 // 2 minutes timeout
                });
                
                if !peers_write.is_empty() {
                    println!("🔗 Active peers: {}", peers_write.len());
                }
            }
        });
    }

    /// Broadcast message to all connected peers
    pub async fn broadcast_message(&self, message: NetworkMessage) -> Result<(), DAGError> {
        let peers = self.peers.read().await;
        
        for (peer_id, peer_info) in peers.iter() {
            if peer_info.connected {
                if let Err(e) = self.send_message_to_peer(peer_id.clone(), message.clone()).await {
                    eprintln!("❌ Failed to send message to {}: {}", peer_id, e);
                }
            }
        }
        
        Ok(())
    }

    /// Send message to specific peer
    pub async fn send_message_to_peer(&self, peer_id: String, message: NetworkMessage) -> Result<(), DAGError> {
        // Get peer address
        let peer_addr = {
            let peers = self.peers.read().await;
            if let Some(peer_info) = peers.get(&peer_id) {
                peer_info.address
            } else {
                return Err(DAGError::NetworkError(format!("Peer {} not found", peer_id)));
            }
        };
        
        // Log message type
        println!("📤 Sending message to {}: {:?}", peer_id, 
                 match &message {
                     NetworkMessage::NewVertex { .. } => "NewVertex",
                     NetworkMessage::Ping => "Ping",
                     NetworkMessage::Pong => "Pong",
                     NetworkMessage::HandshakeResponse { .. } => "HandshakeResponse",
                     NetworkMessage::PeerShare { .. } => "PeerShare",
                     _ => "Other"
                 });
        
        // Send message over TCP
        match TcpStream::connect(peer_addr).await {
            Ok(mut stream) => {
                if let Ok(data) = bincode::serialize(&message) {
                    if let Err(e) = stream.write_all(&data).await {
                        return Err(DAGError::NetworkError(format!("Failed to send message: {}", e)));
                    }
                    println!("✅ Message sent successfully to {}", peer_id);
                } else {
                    return Err(DAGError::NetworkError("Failed to serialize message".to_string()));
                }
            }
            Err(e) => {
                return Err(DAGError::NetworkError(format!("Failed to connect to peer: {}", e)));
            }
        }
        
        Ok(())
    }

    /// Get connected peer count
    pub async fn get_peer_count(&self) -> usize {
        self.peers.read().await.values()
            .filter(|peer| peer.connected)
            .count()
    }

    /// Get detailed peer information
    pub async fn get_peer_details(&self) -> Vec<serde_json::Value> {
        let peers = self.peers.read().await;
        
        peers.values()
            .map(|peer| {
                serde_json::json!({
                    "node_id": peer.node_id,
                    "address": peer.address.to_string(),
                    "connected": peer.connected,
                    "last_seen": peer.last_seen.elapsed().as_secs()
                })
            })
            .collect()
    }

    /// Get message receiver for the main node to process network messages
    pub fn get_message_receiver(&self) -> Arc<RwLock<mpsc::UnboundedReceiver<(String, NetworkMessage)>>> {
        self.message_rx.clone()
    }

    /// Check if we're connected to a specific peer address
    pub async fn is_connected_to(&self, peer_addr: &SocketAddr) -> bool {
        let peers = self.peers.read().await;
        
        // Check if any connected peer has this address
        for peer_info in peers.values() {
            if peer_info.connected && peer_info.address == *peer_addr {
                return true;
            }
        }
        
        false
    }

    /// Connect to a new peer discovered through peer discovery
    pub async fn connect_to_new_peer(&self, peer_addr: SocketAddr) -> Result<(), DAGError> {
        let node_id = self.node_id.clone();
        let listen_port = self.listen_port;
        let peers = self.peers.clone();
        let message_tx = self.message_tx.clone();
        
        // Spawn connection attempt in background
        tokio::spawn(async move {
            if let Err(e) = Self::connect_to_peer(peer_addr, node_id, listen_port, peers, message_tx).await {
                eprintln!("❌ Failed to connect to discovered peer {}: {}", peer_addr, e);
            } else {
                println!("✅ Successfully connected to discovered peer {}", peer_addr);
            }
        });
        
        Ok(())
    }

    /// Get list of connected peer addresses for sharing
    pub async fn get_peer_addresses(&self) -> Vec<SocketAddr> {
        let peers = self.peers.read().await;
        
        peers.values()
            .filter(|peer| peer.connected)
            .map(|peer| peer.address)
            .collect()
    }

    /// Share peer list with all connected peers
    pub async fn share_peers_with_all(&self) -> Result<(), DAGError> {
        let peer_addresses = self.get_peer_addresses().await;
        
        if !peer_addresses.is_empty() {
            let peer_share_message = NetworkMessage::PeerShare {
                peers: peer_addresses.clone(),
            };
            
            println!("🌐 Sharing {} peer addresses with all connected peers", peer_addresses.len());
            self.broadcast_message(peer_share_message).await?;
        }
        
        Ok(())
    }
}

/// Network error types
#[derive(Debug, thiserror::Error)]
pub enum NetworkError {
    #[error("Connection failed: {0}")]
    ConnectionFailed(String),
    #[error("Serialization error: {0}")]
    SerializationError(String),
    #[error("Invalid message: {0}")]
    InvalidMessage(String),
}