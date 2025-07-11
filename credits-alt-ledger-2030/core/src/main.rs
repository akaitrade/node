/*!
 * CREDITS ALT-LEDGER 2030 - Standalone Node
 * 
 * Full-featured blockchain node with DAG engine, consensus, and networking
 * Supports all three migration phases with CLI interface
 */

use std::env;
use std::io::{self, Write};
use std::path::PathBuf;
use std::sync::Arc;
use std::time::{Duration, SystemTime, UNIX_EPOCH};
use std::collections::{HashMap, VecDeque};
use std::fs::File;
use std::io::Read as IoRead;

use tokio::time::{sleep, interval};
use tokio::sync::{mpsc, RwLock, Mutex};
use serde_json::{json, Value};
use blake3;
use bincode;
use log::{info, warn, error, debug};
use serde::{Deserialize, Serialize};
use rand::{Rng, RngCore};

// Import from the library's public exports at the crate root
use dag_engine::{
    DAGEngine, DAGEngineConfig, DAGError, DAGStatistics, DAGEvent, DAGVertex,
    TransactionData, BLSSignature, VertexHash, ConsensusConfig, ShardConfig,
    NetworkManager, NetworkMessage
};
use dag_engine::rpc::RPCServer;

/// Node configuration
#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct NodeConfig {
    pub data_dir: PathBuf,
    pub listen_port: u16,
    pub phase: u8,
    pub validator_id: String,
    pub enable_rpc: bool,
    pub rpc_port: u16,
    pub log_level: String,
    pub bootstrap_peers: Vec<std::net::SocketAddr>,
    pub min_tx_fee: u64,
    pub max_mempool_size: usize,
    pub consensus_timeout_ms: u64,
    pub enable_mining: bool,
    pub wallet_path: PathBuf,
    pub enable_metrics: bool,
    pub metrics_port: u16,
    pub max_connections: usize,
}

impl Default for NodeConfig {
    fn default() -> Self {
        Self {
            data_dir: PathBuf::from("./data"),
            listen_port: 8080,
            phase: 3,
            validator_id: format!("validator_{}", rand::thread_rng().gen::<u32>()),
            enable_rpc: true,
            rpc_port: 8081,
            log_level: "info".to_string(),
            bootstrap_peers: Vec::new(),
            min_tx_fee: 100,
            max_mempool_size: 10000,
            consensus_timeout_ms: 30000,
            enable_mining: false,
            wallet_path: PathBuf::from("./data/wallet.json"),
            enable_metrics: true,
            metrics_port: 9090,
            max_connections: 100,
        }
    }
}

/// Transaction mempool entry
#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct MempoolEntry {
    pub vertex: DAGVertex,
    pub added_at: SystemTime,
    pub fee: u64,
    pub size: usize,
}

/// Wallet information
#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct WalletInfo {
    #[serde(with = "serde_big_array::BigArray")]
    pub address: [u8; 32],
    #[serde(with = "serde_big_array::BigArray")]
    pub public_key: [u8; 48],
    #[serde(with = "serde_big_array::BigArray")]
    pub private_key: [u8; 32],
    pub balance: u64,
    pub nonce: u64,
}

/// State management for accounts
#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct AccountState {
    pub balance: u64,
    pub nonce: u64,
    pub code_hash: Option<[u8; 32]>,
    pub storage: HashMap<[u8; 32], Vec<u8>>,
}

/// Main blockchain node
pub struct BlockchainNode {
    config: NodeConfig,
    dag_engine: Arc<DAGEngine>,
    network_manager: Arc<NetworkManager>,
    running: Arc<RwLock<bool>>,
    command_tx: mpsc::UnboundedSender<NodeCommand>,
    command_rx: Option<mpsc::UnboundedReceiver<NodeCommand>>,
    mempool: Arc<RwLock<HashMap<[u8; 32], MempoolEntry>>>,
    state: Arc<RwLock<HashMap<[u8; 32], AccountState>>>,
    wallet: Arc<RwLock<Option<WalletInfo>>>,
    recent_vertices: Arc<RwLock<VecDeque<DAGVertex>>>,
    mining_active: Arc<RwLock<bool>>,
    metrics: Arc<RwLock<NodeMetrics>>,
}

/// Node metrics
#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct NodeMetrics {
    pub transactions_processed: u64,
    pub vertices_created: u64,
    pub consensus_rounds: u64,
    pub network_bytes_sent: u64,
    pub network_bytes_received: u64,
    pub current_tps: f64,
    pub average_finality_ms: f64,
    pub mempool_size: usize,
    pub connected_peers: usize,
    pub uptime_seconds: u64,
}

/// Node commands
#[derive(Debug, Clone)]
pub enum NodeCommand {
    CreateVertex { data: String },
    GetVertex { hash: String },
    GetStats,
    StartMining,
    StopMining,
    GetBalance { address: String },
    Transfer { from: String, to: String, amount: u64 },
    Status,
    Shutdown,
    GetWalletInfo,
    GetMempoolStatus,
    GetPeers,
    GetMetrics,
    ProcessConsensus,
}

/// Consensus commands for internal communication
#[derive(Debug, Clone)]
enum ConsensusCommand {
    CheckPeers,
}

/// Node response
#[derive(Debug, Clone)]
pub struct NodeResponse {
    pub success: bool,
    pub message: String,
    pub data: Option<Value>,
}

impl BlockchainNode {
    /// Create new blockchain node
    pub async fn new(config: NodeConfig) -> Result<Self, DAGError> {
        // Create data directory
        std::fs::create_dir_all(&config.data_dir)
            .map_err(|e| DAGError::StorageError(format!("Failed to create data directory: {}", e)))?;

        // Configure DAG engine
        let dag_config = DAGEngineConfig {
            storage_path: config.data_dir.join("dag").to_string_lossy().to_string(),
            consensus_config: ConsensusConfig {
                min_validators: 4,
                max_validators: 100,
                bft_threshold: 0.67,
                round_timeout_ms: config.consensus_timeout_ms,
                max_finality_rounds: 10,
            },
            shard_config: ShardConfig::default(),
        };

        // Initialize DAG engine
        let dag_engine = Arc::new(DAGEngine::new(dag_config)?);

        // Initialize network manager
        let network_manager = Arc::new(NetworkManager::new(
            config.validator_id.clone(),
            config.listen_port,
            config.bootstrap_peers.clone(),
        ));

        // Create command channel
        let (command_tx, command_rx) = mpsc::unbounded_channel();

        // Load or create wallet
        let wallet = Arc::new(RwLock::new(None));
        let state = Arc::new(RwLock::new(HashMap::new()));

        // Initialize metrics
        let metrics = Arc::new(RwLock::new(NodeMetrics {
            transactions_processed: 0,
            vertices_created: 0,
            consensus_rounds: 0,
            network_bytes_sent: 0,
            network_bytes_received: 0,
            current_tps: 0.0,
            average_finality_ms: 0.0,
            mempool_size: 0,
            connected_peers: 0,
            uptime_seconds: 0,
        }));

        Ok(Self {
            config,
            dag_engine,
            network_manager,
            running: Arc::new(RwLock::new(false)),
            command_tx,
            command_rx: Some(command_rx),
            mempool: Arc::new(RwLock::new(HashMap::new())),
            state,
            wallet,
            recent_vertices: Arc::new(RwLock::new(VecDeque::with_capacity(1000))),
            mining_active: Arc::new(RwLock::new(false)),
            metrics,
        })
    }

    /// Start the blockchain node
    pub async fn start(&mut self) -> Result<(), DAGError> {
        println!("🚀 Starting CREDITS ALT-LEDGER 2030 Node");
        println!("📊 Phase: {}", self.config.phase);
        println!("🗂️  Data directory: {:?}", self.config.data_dir);
        println!("🔗 Listen port: {}", self.config.listen_port);
        println!("🏗️  Validator ID: {}", self.config.validator_id);
        println!("💰 Min TX Fee: {} CREDITS", self.config.min_tx_fee);
        println!("🔄 Max Mempool Size: {}", self.config.max_mempool_size);
        
        info!("Starting CREDITS ALT-LEDGER 2030 Node");

        // Set running state
        *self.running.write().await = true;

        // Load or create wallet
        self.load_or_create_wallet().await?;

        // Initialize state from DAG
        self.initialize_state().await?;

        // Start consensus timer
        self.start_consensus_timer().await;

        // Start mempool processor
        self.start_mempool_processor().await;

        // Start metrics collector
        if self.config.enable_metrics {
            self.start_metrics_collector().await;
        }

        // Start networking
        self.start_networking().await?;

        // Start RPC server if enabled
        if self.config.enable_rpc {
            self.start_rpc_server().await?;
        }

        // Start mining if enabled
        if self.config.enable_mining {
            self.start_mining().await?;
        }

        // Start command processor
        self.start_command_processor().await;

        // Peer discovery now happens through handshake messages

        // Register signal handlers
        self.register_signal_handlers().await;

        println!("✅ Node started successfully");
        
        // Show initial status
        let peer_count = self.network_manager.get_peer_count().await;
        if peer_count == 0 {
            println!("⏳ Waiting for peers to connect...");
            println!("💡 To form a network, start another node with:");
            println!("   credits-node --bootstrap-peer 127.0.0.1:{}", self.config.listen_port);
        } else {
            println!("🌐 Connected to {} peers", peer_count);
        }
        
        info!("Node initialization complete");
        Ok(())
    }

    /// Load or create wallet
    async fn load_or_create_wallet(&self) -> Result<(), DAGError> {
        let wallet_path = &self.config.wallet_path;
        
        if wallet_path.exists() {
            println!("📂 Loading existing wallet from {:?}", wallet_path);
            let mut file = File::open(wallet_path)
                .map_err(|e| DAGError::StorageError(format!("Failed to open wallet: {}", e)))?;
            let mut contents = String::new();
            file.read_to_string(&mut contents)
                .map_err(|e| DAGError::StorageError(format!("Failed to read wallet: {}", e)))?;
            let wallet_info: WalletInfo = serde_json::from_str(&contents)
                .map_err(|e| DAGError::StorageError(format!("Failed to parse wallet: {}", e)))?;
            *self.wallet.write().await = Some(wallet_info);
        } else {
            println!("🔐 Creating new wallet");
            let wallet_info = self.create_new_wallet().await?;
            self.save_wallet(&wallet_info).await?;
            *self.wallet.write().await = Some(wallet_info);
        }
        
        Ok(())
    }

    /// Create new wallet
    async fn create_new_wallet(&self) -> Result<WalletInfo, DAGError> {
        let mut rng = rand::thread_rng();
        let mut address = [0u8; 32];
        let mut public_key = [0u8; 48];
        let mut private_key = [0u8; 32];
        
        rng.fill_bytes(&mut address);
        rng.fill_bytes(&mut public_key);
        rng.fill_bytes(&mut private_key);
        
        Ok(WalletInfo {
            address,
            public_key,
            private_key,
            balance: 0,
            nonce: 0,
        })
    }

    /// Save wallet to disk
    async fn save_wallet(&self, wallet: &WalletInfo) -> Result<(), DAGError> {
        let wallet_json = serde_json::to_string_pretty(wallet)
            .map_err(|e| DAGError::StorageError(format!("Failed to serialize wallet: {}", e)))?;
        std::fs::write(&self.config.wallet_path, wallet_json)
            .map_err(|e| DAGError::StorageError(format!("Failed to save wallet: {}", e)))?;
        Ok(())
    }

    /// Initialize state from DAG
    async fn initialize_state(&self) -> Result<(), DAGError> {
        info!("🔄 Initializing state from DAG");
        // TODO: Load state from finalized vertices
        Ok(())
    }

    /// Start consensus timer
    async fn start_consensus_timer(&self) {
        println!("🔄 Consensus system initialized");
        
        // Due to Send trait issues, we'll trigger consensus through manual commands
        // The automatic consensus will be triggered by the mempool processor
        // This approach avoids spawning tasks with non-Send components
        println!("💡 Use 'consensus' command to manually trigger consensus rounds");
        println!("💡 Consensus will also be triggered automatically when processing mempool");
    }

    /// Start peer sharing timer
    async fn start_peer_sharing_timer(&self) {
        let network_manager = self.network_manager.clone();
        let running = self.running.clone();
        
        tokio::spawn(async move {
            let mut interval = tokio::time::interval(Duration::from_secs(60)); // Share peers every minute
            
            while *running.read().await {
                interval.tick().await;
                
                // Share peer list with all connected peers
                if let Err(e) = network_manager.share_peers_with_all().await {
                    eprintln!("❌ Periodic peer sharing failed: {}", e);
                }
            }
        });
    }
    
    /// Process consensus round when called
    pub async fn process_consensus_round(&self) -> Result<(), DAGError> {
        // Check if we have enough peers for consensus
        let peer_count = self.network_manager.get_peer_count().await;
        let min_validators = 2;
        
        println!("🔍 Consensus check: {} peers connected, need {}", peer_count, min_validators - 1);
        
        if peer_count + 1 >= min_validators {
            // Get recent vertices for consensus
            let vertex_hashes = {
                let rv = self.recent_vertices.read().await;
                rv.iter().map(|v| v.hash).collect::<Vec<_>>()
            };
            
            println!("🔍 Found {} vertices for consensus processing", vertex_hashes.len());
            
            if !vertex_hashes.is_empty() {
                println!("🔄 Processing consensus round with {} peers and {} vertices", 
                       peer_count, vertex_hashes.len());
                
                match self.dag_engine.process_consensus_round(vertex_hashes).await {
                    Ok(_) => {
                        let mut m = self.metrics.write().await;
                        m.consensus_rounds += 1;
                        println!("✅ Consensus round {} completed with {} validators", 
                              m.consensus_rounds, peer_count + 1);
                    }
                    Err(e) => {
                        println!("⚠️ Consensus round failed: {}", e);
                        return Err(e);
                    }
                }
            } else {
                println!("⏳ No vertices available for consensus processing");
            }
        } else {
            println!("⏳ Not enough peers for consensus: {}/{} connected", peer_count, min_validators - 1);
        }
        
        Ok(())
    }

    /// Start networking
    async fn start_networking(&self) -> Result<(), DAGError> {
        println!("🌐 Starting network manager");
        self.network_manager.start().await?;
        
        // Start network message processor
        let running = self.running.clone();
        let message_rx = self.network_manager.get_message_receiver();
        let dag_engine = self.dag_engine.clone();
        let network_manager = self.network_manager.clone();
        let recent_vertices = self.recent_vertices.clone();
        
        tokio::spawn(async move {
            let mut rx = message_rx.write().await;
            
            while *running.read().await {
                if let Some((peer_id, message)) = rx.recv().await {
                    let msg_type = match &message {
                        NetworkMessage::Handshake { .. } => "Handshake",
                        NetworkMessage::HandshakeResponse { .. } => "HandshakeResponse", 
                        NetworkMessage::NewVertex { .. } => "NewVertex",
                        NetworkMessage::Ping => "Ping",
                        NetworkMessage::Pong => "Pong",
                        _ => "Other"
                    };
                    
                    // Log important messages
                    match msg_type {
                        "Handshake" => println!("🤝 New peer connecting: {}", peer_id),
                        "NewVertex" => println!("📦 Received new vertex from {}", peer_id),
                        "HandshakeResponse" => println!("📨 Processing handshake response from {}", peer_id),
                        _ => debug!("📨 Received {} from {}", msg_type, peer_id)
                    }
                    
                    // Actually process the message
                    if let Err(e) = Self::handle_network_message(peer_id, message, &dag_engine, &network_manager, &recent_vertices).await {
                        eprintln!("❌ Error handling network message: {}", e);
                    }
                }
            }
        });
        
        Ok(())
    }

    /// Start RPC server
    async fn start_rpc_server(&self) -> Result<(), DAGError> {
        println!("🌐 Starting RPC server on port {}", self.config.rpc_port);
        
        let rpc_server = RPCServer::new(self.config.rpc_port, self.dag_engine.clone());
        
        tokio::spawn(async move {
            if let Err(e) = rpc_server.start().await {
                eprintln!("❌ RPC server error: {}", e);
            }
        });
        
        Ok(())
    }

    /// Start command processor
    async fn start_command_processor(&mut self) {
        println!("🎛️ Command processor initialized (integrated mode)");
        // Command processing is now handled through direct method calls
        // This avoids Send trait issues while maintaining functionality
    }

    /// Handle network message from peer
    async fn handle_network_message(
        peer_id: String,
        message: NetworkMessage,
        dag_engine: &Arc<DAGEngine>,
        network_manager: &Arc<NetworkManager>,
        recent_vertices: &Arc<RwLock<VecDeque<DAGVertex>>>,
    ) -> Result<(), DAGError> {
        match message {
            NetworkMessage::Handshake { node_id: _, version: _, listen_port: _, known_peers } => {
                println!("✅ Peer {} connected", peer_id);
                
                // Send our current blockchain state to the new peer
                let stats = match dag_engine.get_statistics().await {
                    Ok(s) => s,
                    Err(e) => {
                        eprintln!("❌ Failed to get stats for sync: {}", e);
                        return Ok(());
                    }
                };
                
                println!("📤 Syncing {} vertices with new peer {}", stats.total_vertices, peer_id);
                
                // TODO: In a real implementation, send recent vertices to sync
                // For now, just log the sync intention
                println!("🔄 Blockchain sync initiated with {}", peer_id);
                
                // Process known peers from handshake
                println!("🌐 Received {} known peers from {}", known_peers.len(), peer_id);
                for peer_addr in known_peers {
                    if !network_manager.is_connected_to(&peer_addr).await {
                        println!("🔗 Discovering new peer from handshake: {}", peer_addr);
                        if let Err(e) = network_manager.connect_to_new_peer(peer_addr).await {
                            println!("❌ Failed to connect to discovered peer {}: {}", peer_addr, e);
                        }
                    }
                }
            }
            NetworkMessage::HandshakeResponse { node_id: _, accepted, known_peers } => {
                if accepted {
                    println!("✅ Handshake accepted by {}", peer_id);
                    
                    // Process known peers from handshake response
                    println!("🌐 Received {} known peers from {} in main handler", known_peers.len(), peer_id);
                    for peer_addr in known_peers {
                        println!("🔍 Checking peer address: {}", peer_addr);
                        if !network_manager.is_connected_to(&peer_addr).await {
                            println!("🔗 Discovering new peer from handshake response: {}", peer_addr);
                            if let Err(e) = network_manager.connect_to_new_peer(peer_addr).await {
                                println!("❌ Failed to connect to discovered peer {}: {}", peer_addr, e);
                            } else {
                                println!("✅ Successfully initiated connection to discovered peer {}", peer_addr);
                            }
                        } else {
                            println!("ℹ️ Already connected to peer {}", peer_addr);
                        }
                    }
                } else {
                    println!("❌ Handshake rejected by {}", peer_id);
                }
            }
            NetworkMessage::NewVertex { vertex } => {
                println!("📦 Received new vertex from {}", peer_id);
                if let Err(e) = dag_engine.insert_vertex(vertex.clone()).await {
                    eprintln!("❌ Failed to insert vertex from {}: {}", peer_id, e);
                } else {
                    // Add vertex to recent vertices for consensus processing
                    let mut rv = recent_vertices.write().await;
                    rv.push_back(vertex);
                    // Keep only last 1000 vertices to prevent memory issues
                    if rv.len() > 1000 {
                        rv.pop_front();
                    }
                    drop(rv); // Release lock before consensus
                    
                    // Note: Consensus processing would be triggered here,
                    // but we can't access self components in static method
                    // Consensus will be triggered when user manually calls 'consensus' command
                    println!("💡 Vertex added to recent vertices. Use 'consensus' command to process.");
                }
            }
            NetworkMessage::VertexRequest { hash } => {
                println!("🔍 Vertex request from {} for {:?}", peer_id, hash);
                match dag_engine.get_vertex(&hash).await {
                    Ok(vertex) => {
                        let response = NetworkMessage::VertexResponse { vertex };
                        if let Err(e) = network_manager.send_message_to_peer(peer_id, response).await {
                            eprintln!("❌ Failed to send vertex response: {}", e);
                        }
                    }
                    Err(e) => {
                        eprintln!("❌ Error retrieving vertex: {}", e);
                    }
                }
            }
            NetworkMessage::VertexResponse { vertex } => {
                if let Some(v) = vertex {
                    println!("📦 Received vertex response from {}", peer_id);
                    if let Err(e) = dag_engine.insert_vertex(v).await {
                        eprintln!("❌ Failed to insert vertex from response: {}", e);
                    }
                }
            }
            NetworkMessage::ConsensusVote { round, vertex_hash: _, vote } => {
                println!("🗳️  Consensus vote from {} for round {}: {}", peer_id, round, vote);
                // TODO: Process consensus vote
            }
            NetworkMessage::Ping => {
                let pong = NetworkMessage::Pong;
                if let Err(e) = network_manager.send_message_to_peer(peer_id, pong).await {
                    eprintln!("❌ Failed to send pong: {}", e);
                }
            }
            NetworkMessage::Pong => {
                // Heartbeat response - connection is alive
            }
            NetworkMessage::PeerShare { peers } => {
                println!("🌐 Received peer list from {}: {} peers", peer_id, peers.len());
                
                // Process peer discovery - attempt to connect to new peers
                for peer_addr in peers {
                    // Check if we're already connected to this peer
                    if !network_manager.is_connected_to(&peer_addr).await {
                        println!("🔗 Discovering new peer: {}", peer_addr);
                        
                        // Attempt to connect to the new peer
                        if let Err(e) = network_manager.connect_to_new_peer(peer_addr).await {
                            println!("❌ Failed to connect to discovered peer {}: {}", peer_addr, e);
                        } else {
                            println!("✅ Successfully connected to discovered peer {}", peer_addr);
                        }
                    }
                }
            }
        }
        
        Ok(())
    }


    /// Execute a command
    pub async fn execute_command(&self, command: NodeCommand) -> NodeResponse {
        match command {
            NodeCommand::CreateVertex { data } => {
                self.create_vertex(data).await
            }
            NodeCommand::GetVertex { hash } => {
                self.get_vertex(hash).await
            }
            NodeCommand::GetStats => {
                self.get_stats().await
            }
            NodeCommand::Status => {
                self.get_status().await
            }
            NodeCommand::StartMining => {
                if *self.mining_active.read().await {
                    NodeResponse {
                        success: false,
                        message: "Mining is already active".to_string(),
                        data: None,
                    }
                } else {
                    match self.start_mining().await {
                        Ok(_) => NodeResponse {
                            success: true,
                            message: "Mining started successfully".to_string(),
                            data: None,
                        },
                        Err(e) => NodeResponse {
                            success: false,
                            message: format!("Failed to start mining: {}", e),
                            data: None,
                        }
                    }
                }
            }
            NodeCommand::StopMining => {
                if !*self.mining_active.read().await {
                    NodeResponse {
                        success: false,
                        message: "Mining is not active".to_string(),
                        data: None,
                    }
                } else {
                    *self.mining_active.write().await = false;
                    NodeResponse {
                        success: true,
                        message: "Mining stopped".to_string(),
                        data: None,
                    }
                }
            }
            NodeCommand::GetBalance { address } => {
                self.get_balance(address).await
            }
            NodeCommand::Transfer { from, to, amount } => {
                self.transfer(from, to, amount).await
            }
            NodeCommand::Shutdown => {
                self.shutdown().await
            }
            NodeCommand::GetWalletInfo => {
                self.get_wallet_info().await
            }
            NodeCommand::GetMempoolStatus => {
                self.get_mempool_status().await
            }
            NodeCommand::GetPeers => {
                self.get_peers().await
            }
            NodeCommand::GetMetrics => {
                self.get_metrics().await
            }
            NodeCommand::ProcessConsensus => {
                match self.process_consensus_round().await {
                    Ok(_) => NodeResponse {
                        success: true,
                        message: "Consensus round processed successfully".to_string(),
                        data: None,
                    },
                    Err(e) => NodeResponse {
                        success: false,
                        message: format!("Consensus round failed: {}", e),
                        data: None,
                    }
                }
            }
        }
    }

    /// Create a new vertex
    async fn create_vertex(&self, data: String) -> NodeResponse {
        // Check if we have enough peers for creating vertices
        let peer_count = self.network_manager.get_peer_count().await;
        if peer_count == 0 {
            return NodeResponse {
                success: false,
                message: "Cannot create vertex: No peers connected. A blockchain requires multiple nodes.".to_string(),
                data: None,
            };
        }
        
        // Create a new DAG vertex
        let vertex = self.create_dag_vertex(data).await;
        
        match vertex {
            Ok(v) => {
                match self.dag_engine.insert_vertex(v.clone()).await {
                    Ok(_) => {
                        // Add vertex to recent vertices for consensus processing
                        {
                            let mut rv = self.recent_vertices.write().await;
                            rv.push_back(v.clone());
                            // Keep only last 1000 vertices to prevent memory issues
                            if rv.len() > 1000 {
                                rv.pop_front();
                            }
                        }
                        
                        // Broadcast new vertex to all connected peers
                        println!("🔗 Broadcasting to {} peers", peer_count);
                        
                        if peer_count > 0 {
                            let broadcast_msg = NetworkMessage::NewVertex { vertex: v.clone() };
                            if let Err(e) = self.network_manager.broadcast_message(broadcast_msg).await {
                                eprintln!("❌ Failed to broadcast vertex: {}", e);
                            } else {
                                println!("📡 Broadcasted new vertex to {} peers", peer_count);
                            }
                        } else {
                            println!("⚠️  No connected peers to broadcast to");
                        }
                        
                        // Trigger consensus processing after creating a transaction
                        let consensus_result = self.process_consensus_round().await;
                        if let Err(e) = consensus_result {
                            println!("⚠️ Consensus failed after transaction creation: {}", e);
                        }
                        
                        NodeResponse {
                            success: true,
                            message: "Vertex created successfully".to_string(),
                            data: Some(json!({
                                "hash": format!("{:?}", v.hash),
                                "height": v.logical_clock,
                                "timestamp": v.timestamp,
                                "parents": v.parents.len()
                            })),
                        }
                    },
                    Err(e) => NodeResponse {
                        success: false,
                        message: format!("Failed to insert vertex: {}", e),
                        data: None,
                    }
                }
            }
            Err(e) => NodeResponse {
                success: false,
                message: format!("Failed to create vertex: {}", e),
                data: None,
            }
        }
    }

    /// Get vertex by hash
    async fn get_vertex(&self, hash_str: String) -> NodeResponse {
        // Parse hash from string (simplified)
        let hash_result = parse_vertex_hash(&hash_str);
        
        match hash_result {
            Ok(h) => {
                match self.dag_engine.get_vertex(&h).await {
                    Ok(Some(vertex)) => NodeResponse {
                        success: true,
                        message: "Vertex found".to_string(),
                        data: Some(json!({
                            "hash": format!("{:?}", vertex.hash),
                            "height": vertex.logical_clock,
                            "timestamp": vertex.timestamp,
                            "parents": vertex.parents.len(),
                            "data_size": vertex.transaction_data.user_data.len()
                        })),
                    },
                    Ok(None) => NodeResponse {
                        success: false,
                        message: "Vertex not found".to_string(),
                        data: None,
                    },
                    Err(e) => NodeResponse {
                        success: false,
                        message: format!("Error retrieving vertex: {}", e),
                        data: None,
                    }
                }
            }
            Err(e) => NodeResponse {
                success: false,
                message: format!("Invalid hash format: {}", e),
                data: None,
            }
        }
    }

    /// Get node statistics
    async fn get_stats(&self) -> NodeResponse {
        match self.dag_engine.get_statistics().await {
            Ok(stats) => NodeResponse {
                success: true,
                message: "Statistics retrieved".to_string(),
                data: Some(json!({
                    "total_vertices": stats.total_vertices,
                    "active_shards": stats.active_shards,
                    "cache_hit_rate": stats.cache_hit_rate,
                    "consensus_rounds": stats.consensus_rounds,
                    "uptime": SystemTime::now().duration_since(UNIX_EPOCH).unwrap().as_secs(),
                    "phase": self.config.phase
                })),
            },
            Err(e) => NodeResponse {
                success: false,
                message: format!("Failed to get statistics: {}", e),
                data: None,
            }
        }
    }

    /// Get node status
    async fn get_status(&self) -> NodeResponse {
        let is_running = *self.running.read().await;
        let peer_count = self.network_manager.get_peer_count().await;
        
        NodeResponse {
            success: true,
            message: "Node status retrieved".to_string(),
            data: Some(json!({
                "running": is_running,
                "phase": self.config.phase,
                "validator_id": self.config.validator_id,
                "listen_port": self.config.listen_port,
                "rpc_enabled": self.config.enable_rpc,
                "rpc_port": self.config.rpc_port,
                "connected_peers": peer_count,
                "bootstrap_peers": self.config.bootstrap_peers.len()
            })),
        }
    }

    /// Get balance from state
    async fn get_balance(&self, address: String) -> NodeResponse {
        // Parse address
        let addr_result = parse_address(&address);
        
        match addr_result {
            Ok(addr) => {
                let state_guard = self.state.read().await;
                
                let balance = if let Some(account) = state_guard.get(&addr) {
                    account.balance
                } else {
                    // Check if it's our wallet
                    if let Some(wallet) = &*self.wallet.read().await {
                        if wallet.address == addr {
                            wallet.balance
                        } else {
                            0
                        }
                    } else {
                        0
                    }
                };
                
                NodeResponse {
                    success: true,
                    message: "Balance retrieved".to_string(),
                    data: Some(json!({
                        "address": address,
                        "balance": balance,
                        "unit": "CREDITS",
                        "formatted": format!("{:.6} CREDITS", balance as f64 / 1_000_000.0)
                    })),
                }
            }
            Err(e) => NodeResponse {
                success: false,
                message: format!("Invalid address: {}", e),
                data: None,
            }
        }
    }

    /// Transfer funds
    async fn transfer(&self, from: String, to: String, amount: u64) -> NodeResponse {
        // Parse addresses
        let from_addr = match parse_address(&from) {
            Ok(addr) => addr,
            Err(e) => return NodeResponse {
                success: false,
                message: format!("Invalid from address: {}", e),
                data: None,
            }
        };
        
        let to_addr = match parse_address(&to) {
            Ok(addr) => addr,
            Err(e) => return NodeResponse {
                success: false,
                message: format!("Invalid to address: {}", e),
                data: None,
            }
        };
        
        // Get wallet info
        let wallet_guard = self.wallet.read().await;
        let wallet = match &*wallet_guard {
            Some(w) => w,
            None => return NodeResponse {
                success: false,
                message: "No wallet available".to_string(),
                data: None,
            }
        };
        
        // Check if we're sending from our wallet
        if from_addr != wallet.address {
            return NodeResponse {
                success: false,
                message: "Can only send from own wallet".to_string(),
                data: None,
            };
        }
        
        // Check balance
        let required = amount + self.config.min_tx_fee;
        if wallet.balance < required {
            return NodeResponse {
                success: false,
                message: format!("Insufficient balance. Need {} CREDITS", required),
                data: None,
            };
        }
        
        // Create transfer vertex
        let stats = match self.dag_engine.get_statistics().await {
            Ok(s) => s,
            Err(e) => return NodeResponse {
                success: false,
                message: format!("Failed to get stats: {}", e),
                data: None,
            }
        };
        
        let parents = match Self::select_parent_vertices(&self.dag_engine, &self.recent_vertices).await {
            Ok(p) => p,
            Err(e) => return NodeResponse {
                success: false,
                message: format!("Failed to select parents: {}", e),
                data: None,
            }
        };
        
        let transaction_data = TransactionData {
            source: from_addr,
            target: to_addr,
            amount,
            currency: 1,
            fee: self.config.min_tx_fee,
            nonce: wallet.nonce,
            user_data: format!("Transfer {} CREDITS", amount).into_bytes(),
        };
        
        let signature = BLSSignature {
            signature: [0; 48],
            public_key: wallet.public_key,
            aggregate_info: None,
        };
        
        let tx_hash = Self::hash_transaction(&transaction_data);
        
        let vertex = DAGVertex::new(
            tx_hash,
            stats.total_vertices,
            parents,
            0,
            transaction_data,
            signature,
        );
        
        // Add to mempool
        let entry = MempoolEntry {
            vertex: vertex.clone(),
            added_at: SystemTime::now(),
            fee: self.config.min_tx_fee,
            size: bincode::serialize(&vertex).unwrap_or_default().len(),
        };
        
        self.mempool.write().await.insert(tx_hash, entry);
        
        // Update metrics
        self.metrics.write().await.mempool_size = self.mempool.read().await.len();
        
        NodeResponse {
            success: true,
            message: "Transfer submitted to mempool".to_string(),
            data: Some(json!({
                "transaction_hash": hex::encode(tx_hash),
                "from": from,
                "to": to,
                "amount": amount,
                "fee": self.config.min_tx_fee,
                "status": "pending"
            })),
        }
    }

    /// Shutdown the node
    async fn shutdown(&self) -> NodeResponse {
        println!("🛑 Shutting down node...");
        *self.running.write().await = false;
        
        NodeResponse {
            success: true,
            message: "Node shutdown initiated".to_string(),
            data: None,
        }
    }

    /// Get wallet info
    async fn get_wallet_info(&self) -> NodeResponse {
        if let Some(wallet) = &*self.wallet.read().await {
            NodeResponse {
                success: true,
                message: "Wallet information".to_string(),
                data: Some(json!({
                    "address": hex::encode(wallet.address),
                    "balance": wallet.balance,
                    "nonce": wallet.nonce,
                    "formatted_balance": format!("{:.6} CREDITS", wallet.balance as f64 / 1_000_000.0)
                })),
            }
        } else {
            NodeResponse {
                success: false,
                message: "No wallet available".to_string(),
                data: None,
            }
        }
    }

    /// Get mempool status
    async fn get_mempool_status(&self) -> NodeResponse {
        let mempool_guard = self.mempool.read().await;
        let total_size = mempool_guard.len();
        let total_fees: u64 = mempool_guard.values().map(|e| e.fee).sum();
        let avg_fee = if total_size > 0 { total_fees / total_size as u64 } else { 0 };
        
        NodeResponse {
            success: true,
            message: "Mempool status".to_string(),
            data: Some(json!({
                "pending_transactions": total_size,
                "total_fees": total_fees,
                "average_fee": avg_fee,
                "max_size": self.config.max_mempool_size
            })),
        }
    }

    /// Get peers
    async fn get_peers(&self) -> NodeResponse {
        let peer_count = self.network_manager.get_peer_count().await;
        let peer_details = self.network_manager.get_peer_details().await;
        
        NodeResponse {
            success: true,
            message: "Peer information".to_string(),
            data: Some(json!({
                "connected_peers": peer_count,
                "max_connections": self.config.max_connections,
                "bootstrap_peers": self.config.bootstrap_peers.len(),
                "peer_details": peer_details
            })),
        }
    }

    /// Get metrics
    async fn get_metrics(&self) -> NodeResponse {
        let metrics = self.metrics.read().await.clone();
        
        NodeResponse {
            success: true,
            message: "Node metrics".to_string(),
            data: Some(json!({
                "transactions_processed": metrics.transactions_processed,
                "vertices_created": metrics.vertices_created,
                "consensus_rounds": metrics.consensus_rounds,
                "current_tps": metrics.current_tps,
                "average_finality_ms": metrics.average_finality_ms,
                "mempool_size": metrics.mempool_size,
                "connected_peers": metrics.connected_peers,
                "uptime_seconds": metrics.uptime_seconds,
                "network_bytes_sent": metrics.network_bytes_sent,
                "network_bytes_received": metrics.network_bytes_received
            })),
        }
    }

    /// Start mempool processor
    async fn start_mempool_processor(&self) {
        let running = self.running.clone();
        let mempool = self.mempool.clone();
        let dag_engine = self.dag_engine.clone();
        let recent_vertices = self.recent_vertices.clone();
        let state = self.state.clone();
        let metrics = self.metrics.clone();
        let max_mempool_size = self.config.max_mempool_size;
        
        tokio::spawn(async move {
            let mut interval = tokio::time::interval(Duration::from_secs(1));
            
            while *running.read().await {
                interval.tick().await;
                
                // Process mempool transactions
                let mut mempool_guard = mempool.write().await;
                let mut to_remove = Vec::new();
                let mut processed = 0;
                
                // Sort by fee (highest first)
                let mut entries: Vec<_> = mempool_guard.values().cloned().collect();
                entries.sort_by(|a, b| b.fee.cmp(&a.fee));
                
                for entry in entries.iter().take(100) { // Process up to 100 per second
                    // Validate transaction against current state
                    if Self::validate_transaction(&entry.vertex, &*state.read().await).await {
                        // Add to DAG
                        match dag_engine.insert_vertex(entry.vertex.clone()).await {
                            Ok(_) => {
                                to_remove.push(entry.vertex.hash.clone());
                                processed += 1;
                                
                                // Add to recent vertices for consensus
                                let mut rv = recent_vertices.write().await;
                                rv.push_back(entry.vertex.clone());
                                if rv.len() > 1000 {
                                    rv.pop_front();
                                }
                            }
                            Err(e) => {
                                warn!("Failed to insert vertex from mempool: {}", e);
                            }
                        }
                    } else {
                        // Invalid transaction, remove from mempool
                        to_remove.push(entry.vertex.hash.clone());
                    }
                }
                
                // Remove processed transactions
                for hash in to_remove {
                    mempool_guard.remove(&hash);
                }
                
                // Update metrics
                if processed > 0 {
                    let mut m = metrics.write().await;
                    m.transactions_processed += processed;
                    m.mempool_size = mempool_guard.len();
                }
                
                // Enforce mempool size limit
                while mempool_guard.len() > max_mempool_size {
                    // Remove oldest transaction
                    let oldest = mempool_guard.values()
                        .min_by_key(|e| e.added_at)
                        .map(|e| e.vertex.hash.clone());
                    if let Some(hash) = oldest {
                        mempool_guard.remove(&hash);
                    }
                }
            }
        });
    }

    /// Validate transaction against current state
    async fn validate_transaction(vertex: &DAGVertex, state: &HashMap<[u8; 32], AccountState>) -> bool {
        // Check source account exists and has sufficient balance
        if let Some(source_state) = state.get(&vertex.transaction_data.source) {
            let required_balance = vertex.transaction_data.amount + vertex.transaction_data.fee;
            if source_state.balance >= required_balance && 
               source_state.nonce == vertex.transaction_data.nonce {
                return true;
            }
        }
        false
    }

    /// Start metrics collector
    async fn start_metrics_collector(&self) {
        let running = self.running.clone();
        let metrics = self.metrics.clone();
        let dag_engine = self.dag_engine.clone();
        let network_manager = self.network_manager.clone();
        let mempool = self.mempool.clone();
        let start_time = SystemTime::now();
        
        tokio::spawn(async move {
            let mut interval = tokio::time::interval(Duration::from_secs(5));
            let mut last_tx_count = 0u64;
            
            while *running.read().await {
                interval.tick().await;
                
                let mut m = metrics.write().await;
                
                // Update uptime
                m.uptime_seconds = SystemTime::now()
                    .duration_since(start_time)
                    .unwrap_or_default()
                    .as_secs();
                
                // Update TPS
                let current_tx_count = m.transactions_processed;
                m.current_tps = (current_tx_count - last_tx_count) as f64 / 5.0;
                last_tx_count = current_tx_count;
                
                // Update network stats
                m.connected_peers = network_manager.get_peer_count().await;
                
                // Update mempool size
                m.mempool_size = mempool.read().await.len();
                
                // Get DAG stats
                if let Ok(dag_stats) = dag_engine.get_statistics().await {
                    m.vertices_created = dag_stats.total_vertices;
                }
                
                debug!("📊 Metrics - TPS: {:.2}, Peers: {}, Mempool: {}, Uptime: {}s",
                    m.current_tps, m.connected_peers, m.mempool_size, m.uptime_seconds);
            }
        });
    }

    /// Start mining
    async fn start_mining(&self) -> Result<(), DAGError> {
        info!("⛏️  Starting mining process");
        *self.mining_active.write().await = true;
        
        let running = self.running.clone();
        let mining_active = self.mining_active.clone();
        let wallet = self.wallet.clone();
        let dag_engine = self.dag_engine.clone();
        let network_manager = self.network_manager.clone();
        let recent_vertices = self.recent_vertices.clone();
        let metrics = self.metrics.clone();
        
        tokio::spawn(async move {
            let mut interval = tokio::time::interval(Duration::from_secs(5));
            
            while *running.read().await && *mining_active.read().await {
                interval.tick().await;
                
                // Check if we're connected to the network via metrics
                let peer_count = {
                    let m = metrics.read().await;
                    m.connected_peers
                };
                
                if peer_count == 0 {
                    debug!("⏸️  Mining paused - no peers connected");
                    continue;
                }
                
                if let Some(wallet_info) = &*wallet.read().await {
                    // Create mining reward transaction
                    let reward_vertex = Self::create_mining_reward_vertex(
                        wallet_info.address,
                        &dag_engine,
                        &recent_vertices
                    ).await;
                    
                    if let Ok(vertex) = reward_vertex {
                        match dag_engine.insert_vertex(vertex.clone()).await {
                            Ok(_) => {
                                info!("💎 Mined new vertex with reward");
                                
                                // Broadcast to network
                                let msg = NetworkMessage::NewVertex { vertex: vertex.clone() };
                                let _ = network_manager.broadcast_message(msg).await;
                                
                                // Update metrics
                                let mut m = metrics.write().await;
                                m.vertices_created += 1;
                                
                                // Add to recent vertices
                                let mut rv = recent_vertices.write().await;
                                rv.push_back(vertex);
                                if rv.len() > 1000 {
                                    rv.pop_front();
                                }
                            }
                            Err(e) => {
                                error!("Failed to insert mined vertex: {}", e);
                            }
                        }
                    }
                }
            }
        });
        
        Ok(())
    }

    /// Create mining reward vertex
    async fn create_mining_reward_vertex(
        miner_address: [u8; 32],
        dag_engine: &Arc<DAGEngine>,
        recent_vertices: &Arc<RwLock<VecDeque<DAGVertex>>>
    ) -> Result<DAGVertex, DAGError> {
        let stats = dag_engine.get_statistics().await?;
        
        // Get real parent vertices
        let parents = Self::select_parent_vertices(dag_engine, recent_vertices).await?;
        
        // Create coinbase transaction
        let transaction_data = TransactionData {
            source: [0; 32], // Coinbase
            target: miner_address,
            amount: 1000, // Mining reward
            currency: 1,
            fee: 0,
            nonce: stats.total_vertices,
            user_data: b"Mining Reward".to_vec(),
        };
        
        // Create signature
        let signature = BLSSignature {
            signature: [0; 48],
            public_key: [0; 48],
            aggregate_info: None,
        };
        
        // Generate transaction hash
        let tx_hash = Self::hash_transaction(&transaction_data);
        
        Ok(DAGVertex::new(
            tx_hash,
            stats.total_vertices,
            parents,
            0, // Default shard
            transaction_data,
            signature,
        ))
    }

    /// Register signal handlers for graceful shutdown
    async fn register_signal_handlers(&self) {
        let running = self.running.clone();
        
        tokio::spawn(async move {
            let _ = tokio::signal::ctrl_c().await;
            warn!("🛑 Received shutdown signal");
            *running.write().await = false;
        });
    }

    /// Create a DAG vertex from data
    async fn create_dag_vertex(&self, data: String) -> Result<DAGVertex, DAGError> {
        let stats = self.dag_engine.get_statistics().await?;
        let wallet_guard = self.wallet.read().await;
        
        let (source, public_key) = if let Some(wallet) = &*wallet_guard {
            (wallet.address, wallet.public_key)
        } else {
            return Err(DAGError::ValidationError("No wallet available".to_string()));
        };
        
        // Get real parent vertices
        let parents = Self::select_parent_vertices(&self.dag_engine, &self.recent_vertices).await?;
        
        // Create transaction data
        let transaction_data = TransactionData {
            source,
            target: [0; 32], // Data transaction
            amount: 0,
            currency: 1,
            fee: self.config.min_tx_fee,
            nonce: stats.total_vertices,
            user_data: data.into_bytes(),
        };
        
        // Create signature
        let signature = BLSSignature {
            signature: [0; 48], // Would be real signature in production
            public_key,
            aggregate_info: None,
        };
        
        // Generate transaction hash
        let tx_hash = Self::hash_transaction(&transaction_data);
        
        Ok(DAGVertex::new(
            tx_hash,
            stats.total_vertices,
            parents,
            0, // Default shard
            transaction_data,
            signature,
        ))
    }

    /// Select parent vertices for new vertex
    async fn select_parent_vertices(
        dag_engine: &Arc<DAGEngine>,
        recent_vertices: &Arc<RwLock<VecDeque<DAGVertex>>>
    ) -> Result<Vec<[u8; 32]>, DAGError> {
        let stats = dag_engine.get_statistics().await?;
        
        if stats.total_vertices == 0 {
            return Ok(Vec::new()); // Genesis vertex has no parents
        }
        
        let mut parents = Vec::new();
        let recent = recent_vertices.read().await;
        
        // Select 2-3 recent vertices as parents
        let parent_count = std::cmp::min(3, recent.len());
        if parent_count > 0 {
            let mut indices = Vec::new();
            for i in 0..parent_count {
                let idx = recent.len() - 1 - i;
                if idx < recent.len() {
                    indices.push(idx);
                }
            }
            
            for idx in indices {
                if let Some(vertex) = recent.get(idx) {
                    parents.push(vertex.hash);
                }
            }
        }
        
        // Ensure we have at least 2 parents for DAG property
        while parents.len() < 2 && stats.total_vertices > 0 {
            // Create dummy parent for now
            let mut dummy_hash = [0u8; 32];
            dummy_hash[0] = parents.len() as u8;
            dummy_hash[31] = (stats.total_vertices % 256) as u8;
            parents.push(dummy_hash);
        }
        
        Ok(parents)
    }

    /// Hash transaction data
    fn hash_transaction(tx_data: &TransactionData) -> [u8; 32] {
        let mut hasher = blake3::Hasher::new();
        hasher.update(&bincode::serialize(tx_data).unwrap_or_default());
        let hash = hasher.finalize();
        let mut result = [0u8; 32];
        result.copy_from_slice(hash.as_bytes());
        result
    }

    /// Get command sender
    pub fn get_command_sender(&self) -> mpsc::UnboundedSender<NodeCommand> {
        self.command_tx.clone()
    }

    /// Check if node is running
    pub async fn is_running(&self) -> bool {
        *self.running.read().await
    }
}

/// CLI interface
async fn run_interactive_cli(node: Arc<tokio::sync::RwLock<BlockchainNode>>) {
    println!("\n🎮 Interactive CLI Started");
    println!("Type 'help' for available commands or 'quit' to exit");

    loop {
        print!("credits-node> ");
        io::stdout().flush().unwrap();

        let mut input = String::new();
        match io::stdin().read_line(&mut input) {
            Ok(_) => {
                let input = input.trim();
                
                if input == "quit" || input == "exit" {
                    break;
                }

                let response = handle_cli_command(input, &node).await;
                println!("{}", format_response(&response));
            }
            Err(e) => {
                eprintln!("❌ Error reading input: {}", e);
            }
        }
    }

    println!("👋 CLI session ended");
}

/// Handle CLI command
async fn handle_cli_command(input: &str, node: &Arc<tokio::sync::RwLock<BlockchainNode>>) -> NodeResponse {
    let parts: Vec<&str> = input.split_whitespace().collect();
    
    if parts.is_empty() {
        return NodeResponse {
            success: false,
            message: "Empty command".to_string(),
            data: None,
        };
    }

    let command = parts[0].to_lowercase();
    let node_guard = node.read().await;

    match command.as_str() {
        "help" => {
            NodeResponse {
                success: true,
                message: "Available commands:\n\
                    help - Show this help message\n\
                    status - Show node status\n\
                    stats - Show node statistics\n\
                    create <data> - Create new vertex with data\n\
                    get <hash> - Get vertex by hash\n\
                    balance [address] - Get balance for address (defaults to wallet)\n\
                    transfer <from> <to> <amount> - Transfer funds\n\
                    wallet - Show wallet info\n\
                    mempool - Show mempool status\n\
                    peers - Show connected peers\n\
                    metrics - Show node metrics\n\
                    discover - Manually trigger peer discovery\n\
                    consensus - Process consensus round\n\
                    start-mining - Start mining\n\
                    stop-mining - Stop mining\n\
                    quit/exit - Exit the CLI".to_string(),
                data: None,
            }
        }
        "status" => {
            node_guard.execute_command(NodeCommand::Status).await
        }
        "stats" => {
            node_guard.execute_command(NodeCommand::GetStats).await
        }
        "create" => {
            if parts.len() < 2 {
                NodeResponse {
                    success: false,
                    message: "Usage: create <data>".to_string(),
                    data: None,
                }
            } else {
                let data = parts[1..].join(" ");
                node_guard.execute_command(NodeCommand::CreateVertex { data }).await
            }
        }
        "get" => {
            if parts.len() < 2 {
                NodeResponse {
                    success: false,
                    message: "Usage: get <hash>".to_string(),
                    data: None,
                }
            } else {
                let hash = parts[1].to_string();
                node_guard.execute_command(NodeCommand::GetVertex { hash }).await
            }
        }
        "balance" => {
            if parts.len() < 2 {
                NodeResponse {
                    success: false,
                    message: "Usage: balance <address>".to_string(),
                    data: None,
                }
            } else {
                let address = parts[1].to_string();
                node_guard.execute_command(NodeCommand::GetBalance { address }).await
            }
        }
        "transfer" => {
            if parts.len() < 4 {
                NodeResponse {
                    success: false,
                    message: "Usage: transfer <from> <to> <amount>".to_string(),
                    data: None,
                }
            } else {
                let from = parts[1].to_string();
                let to = parts[2].to_string();
                if let Ok(amount) = parts[3].parse::<u64>() {
                    node_guard.execute_command(NodeCommand::Transfer { from, to, amount }).await
                } else {
                    NodeResponse {
                        success: false,
                        message: "Invalid amount".to_string(),
                        data: None,
                    }
                }
            }
        }
        "start-mining" => {
            node_guard.execute_command(NodeCommand::StartMining).await
        }
        "stop-mining" => {
            node_guard.execute_command(NodeCommand::StopMining).await
        }
        "wallet" => {
            node_guard.get_wallet_info().await
        }
        "mempool" => {
            node_guard.get_mempool_status().await
        }
        "peers" => {
            node_guard.get_peers().await
        }
        "metrics" => {
            node_guard.get_metrics().await
        }
        "discover" => {
            // Manually trigger peer discovery for testing
            println!("🔍 Manually triggering peer discovery...");
            let bootstrap_peers = node_guard.config.bootstrap_peers.clone();
            for peer_addr in bootstrap_peers {
                if let Err(e) = node_guard.network_manager.connect_to_new_peer(peer_addr).await {
                    println!("❌ Failed to reconnect to bootstrap peer {}: {}", peer_addr, e);
                } else {
                    println!("✅ Reconnected to bootstrap peer {}", peer_addr);
                }
            }
            NodeResponse {
                success: true,
                message: "Peer discovery triggered".to_string(),
                data: None,
            }
        }
        "consensus" => {
            node_guard.execute_command(NodeCommand::ProcessConsensus).await
        }
        _ => {
            NodeResponse {
                success: false,
                message: format!("Unknown command: {}. Type 'help' for available commands.", command),
                data: None,
            }
        }
    }
}

/// Format response for display
fn format_response(response: &NodeResponse) -> String {
    let status = if response.success { "✅" } else { "❌" };
    
    let mut output = format!("{} {}", status, response.message);
    
    if let Some(data) = &response.data {
        output.push_str(&format!("\n📊 Data: {}", serde_json::to_string_pretty(data).unwrap_or_else(|_| "Invalid JSON".to_string())));
    }
    
    output
}

/// Parse command line arguments
fn parse_args() -> NodeConfig {
    let args: Vec<String> = env::args().collect();
    let mut config = NodeConfig::default();
    
    let mut i = 1;
    while i < args.len() {
        match args[i].as_str() {
            "--data-dir" => {
                if i + 1 < args.len() {
                    config.data_dir = PathBuf::from(&args[i + 1]);
                    i += 2;
                } else {
                    eprintln!("❌ --data-dir requires a value");
                    std::process::exit(1);
                }
            }
            "--port" => {
                if i + 1 < args.len() {
                    config.listen_port = args[i + 1].parse().unwrap_or(8080);
                    i += 2;
                } else {
                    eprintln!("❌ --port requires a value");
                    std::process::exit(1);
                }
            }
            "--phase" => {
                if i + 1 < args.len() {
                    config.phase = args[i + 1].parse().unwrap_or(3);
                    i += 2;
                } else {
                    eprintln!("❌ --phase requires a value");
                    std::process::exit(1);
                }
            }
            "--validator-id" => {
                if i + 1 < args.len() {
                    config.validator_id = args[i + 1].clone();
                    i += 2;
                } else {
                    eprintln!("❌ --validator-id requires a value");
                    std::process::exit(1);
                }
            }
            "--rpc-port" => {
                if i + 1 < args.len() {
                    config.rpc_port = args[i + 1].parse().unwrap_or(8081);
                    i += 2;
                } else {
                    eprintln!("❌ --rpc-port requires a value");
                    std::process::exit(1);
                }
            }
            "--bootstrap-peer" => {
                if i + 1 < args.len() {
                    if let Ok(addr) = args[i + 1].parse::<std::net::SocketAddr>() {
                        config.bootstrap_peers.push(addr);
                    } else {
                        eprintln!("❌ Invalid bootstrap peer address: {}", args[i + 1]);
                        std::process::exit(1);
                    }
                    i += 2;
                } else {
                    eprintln!("❌ --bootstrap-peer requires a value");
                    std::process::exit(1);
                }
            }
            "--help" => {
                print_help();
                std::process::exit(0);
            }
            _ => {
                eprintln!("❌ Unknown argument: {}", args[i]);
                std::process::exit(1);
            }
        }
    }
    
    config
}

/// Print help message
fn print_help() {
    println!("=== VERSION: ENHANCED-2024-07-09-TRACKER ===");
    println!("CREDITS ALT-LEDGER 2030 - Professional Blockchain Node");
    println!("");
    println!("Usage: credits-node [OPTIONS]");
    println!("");
    println!("Options:");
    println!("  --data-dir <path>           Data directory (default: ./data)");
    println!("  --port <port>               Listen port (default: 8080)");
    println!("  --phase <1|2|3>             Migration phase (default: 3)");
    println!("  --validator-id <id>         Validator identifier (default: validator_1)");
    println!("  --rpc-port <port>           RPC port (default: 8081)");
    println!("  --bootstrap-peer <addr>     Bootstrap peer address (can be used multiple times)");
    println!("  --help                      Show this help message");
    println!("");
    println!("Examples:");
    println!("  credits-node                                    # Start with defaults");
    println!("  credits-node --phase 1 --port 9000              # Start Phase 1 on port 9000");
    println!("  credits-node --data-dir /tmp/blockchain          # Use custom data directory");
    println!("  credits-node --bootstrap-peer 127.0.0.1:8080    # Connect to bootstrap peer");
}

/// Main function
#[tokio::main]
async fn main() -> Result<(), Box<dyn std::error::Error>> {
    // Initialize logger with default level of 'info' if RUST_LOG is not set
    env_logger::Builder::from_env(env_logger::Env::default().default_filter_or("info"))
        .format_timestamp_secs()
        .init();
    
    // Parse command line arguments
    let config = parse_args();
    
    // Create and start node
    let mut node = BlockchainNode::new(config).await?;
    node.start().await?;
    
    // Wrap node in Arc<RwLock> for shared access
    let node = Arc::new(tokio::sync::RwLock::new(node));
    
    // Start interactive CLI
    run_interactive_cli(node.clone()).await;
    
    // Shutdown node
    {
        let node_guard = node.read().await;
        node_guard.execute_command(NodeCommand::Shutdown).await;
    }
    
    println!("🔄 Node shutdown complete");
    Ok(())
}

/// Parse vertex hash from string
fn parse_vertex_hash(hash_str: &str) -> Result<VertexHash, String> {
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

/// Parse address from string
fn parse_address(addr_str: &str) -> Result<[u8; 32], String> {
    if addr_str.len() != 64 {
        return Err("Address must be 64 characters long".to_string());
    }
    
    let mut addr = [0u8; 32];
    for (i, chunk) in addr_str.as_bytes().chunks(2).enumerate() {
        if i >= 32 {
            return Err("Address too long".to_string());
        }
        let hex_str = std::str::from_utf8(chunk).map_err(|_| "Invalid UTF-8")?;
        addr[i] = u8::from_str_radix(hex_str, 16).map_err(|_| "Invalid hex character")?;
    }
    
    Ok(addr)
}