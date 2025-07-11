/*!
 * CTDP v2 Transport Implementation
 */

#include "ctdp_v2_transport.hpp"
#include <iostream>
#include <chrono>
#include <algorithm>
// Note: CRC32C functionality can be implemented with standard library or removed
// #include <crc32c/crc32c.h>

// Simple CRC32 placeholder implementation
inline uint32_t crc32c_calculate(const void* data, size_t length) {
    // Placeholder - in production, use proper CRC32C implementation
    uint32_t crc = 0;
    const uint8_t* bytes = static_cast<const uint8_t*>(data);
    for (size_t i = 0; i < length; ++i) {
        crc = ((crc >> 8) | (crc << 24)) ^ bytes[i];
    }
    return crc;
}

#ifdef HAVE_QUICHE
#include "quic_transport.hpp"
#endif

namespace credits {
namespace networking {

CTDPv2Transport::CTDPv2Transport(boost::asio::io_context& io_context, const CTDPv2Config& config)
    : io_context_(io_context)
    , config_(config)
    , running_(false)
    , start_time_(std::chrono::steady_clock::now()) {
    
    // Initialize statistics
    stats_ = {};
}

CTDPv2Transport::~CTDPv2Transport() {
    stop();
}

bool CTDPv2Transport::start() {
    if (running_.load()) {
        return false;
    }

    std::cout << "Starting CTDP v2 transport on port " << config_.port << std::endl;

    // Initialize UDP transport
    if (!init_udp_transport()) {
        std::cerr << "Failed to initialize UDP transport" << std::endl;
        return false;
    }

    // Initialize QUIC transport if enabled and available
#ifdef HAVE_QUICHE
    if (config_.enable_quic) {
        if (!init_quic_transport()) {
            std::cerr << "Failed to initialize QUIC transport, falling back to UDP only" << std::endl;
            config_.enable_quic = false;
        }
    }
#else
    if (config_.enable_quic) {
        std::cerr << "QUIC support not compiled in, using UDP only" << std::endl;
        config_.enable_quic = false;
    }
#endif

    running_.store(true);

    // Start worker threads
    worker_threads_.emplace_back(&CTDPv2Transport::network_worker, this);
    worker_threads_.emplace_back(&CTDPv2Transport::ping_worker, this);

    // Start async receive
    udp_socket_->async_receive_from(
        boost::asio::buffer(udp_receive_buffer_),
        udp_remote_endpoint_,
        [this](const boost::system::error_code& error, std::size_t bytes_received) {
            handle_udp_receive(error, bytes_received);
        }
    );

    std::cout << "CTDP v2 transport started successfully" << std::endl;
    return true;
}

void CTDPv2Transport::stop() {
    if (!running_.load()) {
        return;
    }

    std::cout << "Stopping CTDP v2 transport" << std::endl;
    running_.store(false);

    // Close sockets
    if (udp_socket_) {
        udp_socket_->close();
    }

#ifdef HAVE_QUICHE
    if (quic_transport_) {
        quic_transport_->stop();
    }
#endif

    // Join worker threads
    for (auto& thread : worker_threads_) {
        if (thread.joinable()) {
            thread.join();
        }
    }
    worker_threads_.clear();

    std::cout << "CTDP v2 transport stopped" << std::endl;
}

bool CTDPv2Transport::send_to_peer(const PeerInfo& peer, CTDPv2MessageType type, const std::vector<uint8_t>& payload) {
    if (!running_.load()) {
        return false;
    }

    try {
        auto frame_data = serialize_frame(type, payload);
        
        udp::endpoint endpoint(peer.address, peer.port);
        auto bytes_sent = udp_socket_->send_to(boost::asio::buffer(frame_data), endpoint);
        
        // Update statistics
        {
            std::lock_guard<std::mutex> lock(stats_mutex_);
            stats_.messages_sent++;
            stats_.bytes_sent += bytes_sent;
        }
        
        // Update peer statistics
        update_peer_stats(peer.validator_id, static_cast<uint32_t>(bytes_sent), 0.0f);
        
        return true;
    } catch (const std::exception& e) {
        std::cerr << "Failed to send to peer: " << e.what() << std::endl;
        return false;
    }
}

bool CTDPv2Transport::broadcast_message(CTDPv2MessageType type, const std::vector<uint8_t>& payload, const std::vector<PeerInfo>& peers) {
    if (!running_.load()) {
        return false;
    }

    bool success = true;
    for (const auto& peer : peers) {
        if (!send_to_peer(peer, type, payload)) {
            success = false;
        }
    }
    
    return success;
}

bool CTDPv2Transport::send_dag_vertex(const DAGVertexMessage& vertex, const std::vector<VertexHash>& parents, const std::vector<uint8_t>& transaction_data) {
    if (!running_.load()) {
        return false;
    }

    // Serialize DAG vertex message
    std::vector<uint8_t> payload;
    
    // Add vertex header
    payload.insert(payload.end(), vertex.vertex_hash.begin(), vertex.vertex_hash.end());
    payload.insert(payload.end(), vertex.tx_hash.begin(), vertex.tx_hash.end());
    
    // Add logical clock (8 bytes, big-endian)
    for (int i = 7; i >= 0; --i) {
        payload.push_back(static_cast<uint8_t>(vertex.logical_clock >> (i * 8)));
    }
    
    // Add parent count (4 bytes, big-endian)
    for (int i = 3; i >= 0; --i) {
        payload.push_back(static_cast<uint8_t>(vertex.parent_count >> (i * 8)));
    }
    
    // Add parent hashes
    for (const auto& parent : parents) {
        payload.insert(payload.end(), parent.begin(), parent.end());
    }
    
    // Add transaction data length (4 bytes, big-endian)
    uint32_t tx_data_len = static_cast<uint32_t>(transaction_data.size());
    for (int i = 3; i >= 0; --i) {
        payload.push_back(static_cast<uint8_t>(tx_data_len >> (i * 8)));
    }
    
    // Add transaction data
    payload.insert(payload.end(), transaction_data.begin(), transaction_data.end());
    
    // Broadcast to all peers
    std::vector<PeerInfo> peers;
    {
        std::lock_guard<std::mutex> lock(peers_mutex_);
        for (const auto& [id, peer] : peers_) {
            peers.push_back(peer);
        }
    }
    
    return broadcast_message(CTDPv2MessageType::DAG_VERTEX, payload, peers);
}

bool CTDPv2Transport::send_virtual_vote(const VirtualVoteMessage& vote) {
    if (!running_.load()) {
        return false;
    }

    // Serialize virtual vote message
    std::vector<uint8_t> payload;
    
    // Add validator ID
    payload.insert(payload.end(), vote.validator.begin(), vote.validator.end());
    
    // Add vertex hash
    payload.insert(payload.end(), vote.vertex_hash.begin(), vote.vertex_hash.end());
    
    // Add vote type
    payload.push_back(vote.vote_type);
    
    // Add round (8 bytes, big-endian)
    for (int i = 7; i >= 0; --i) {
        payload.push_back(static_cast<uint8_t>(vote.round >> (i * 8)));
    }
    
    // Add timestamp (8 bytes, big-endian)
    for (int i = 7; i >= 0; --i) {
        payload.push_back(static_cast<uint8_t>(vote.timestamp >> (i * 8)));
    }
    
    // Add signature
    payload.insert(payload.end(), vote.signature.begin(), vote.signature.end());
    
    // Broadcast to trusted nodes only
    std::vector<PeerInfo> trusted_peers;
    {
        std::lock_guard<std::mutex> lock(peers_mutex_);
        for (const auto& [id, peer] : peers_) {
            if (peer.is_trusted_node) {
                trusted_peers.push_back(peer);
            }
        }
    }
    
    return broadcast_message(CTDPv2MessageType::VIRTUAL_VOTE, payload, trusted_peers);
}

void CTDPv2Transport::register_message_handler(CTDPv2MessageType type, MessageHandler handler) {
    std::lock_guard<std::mutex> lock(handlers_mutex_);
    message_handlers_[type] = std::move(handler);
}

void CTDPv2Transport::register_connection_handler(ConnectionEventHandler handler) {
    std::lock_guard<std::mutex> lock(handlers_mutex_);
    connection_handlers_.push_back(std::move(handler));
}

void CTDPv2Transport::add_trusted_peer(const PeerInfo& peer) {
    std::lock_guard<std::mutex> lock(peers_mutex_);
    peers_[peer.validator_id] = peer;
    
    // Notify connection handlers
    std::lock_guard<std::mutex> handler_lock(handlers_mutex_);
    for (const auto& handler : connection_handlers_) {
        handler(peer, true);
    }
}

void CTDPv2Transport::remove_peer(const ValidatorId& validator_id) {
    std::lock_guard<std::mutex> lock(peers_mutex_);
    auto it = peers_.find(validator_id);
    if (it != peers_.end()) {
        PeerInfo peer = it->second;
        peers_.erase(it);
        
        // Notify connection handlers
        std::lock_guard<std::mutex> handler_lock(handlers_mutex_);
        for (const auto& handler : connection_handlers_) {
            handler(peer, false);
        }
    }
}

std::vector<PeerInfo> CTDPv2Transport::get_connected_peers() const {
    std::lock_guard<std::mutex> lock(peers_mutex_);
    std::vector<PeerInfo> peers;
    for (const auto& [id, peer] : peers_) {
        peers.push_back(peer);
    }
    return peers;
}

CTDPv2Transport::NetworkStats CTDPv2Transport::get_network_stats() const {
    std::lock_guard<std::mutex> lock(stats_mutex_);
    return stats_;
}

bool CTDPv2Transport::init_udp_transport() {
    try {
        udp_socket_ = std::make_unique<udp::socket>(io_context_, udp::endpoint(udp::v4(), config_.port));
        
        // Set socket options
        udp_socket_->set_option(udp::socket::reuse_address(true));
        udp_socket_->set_option(boost::asio::socket_base::receive_buffer_size(1024 * 1024)); // 1MB buffer
        udp_socket_->set_option(boost::asio::socket_base::send_buffer_size(1024 * 1024)); // 1MB buffer
        
        std::cout << "UDP transport initialized on port " << config_.port << std::endl;
        return true;
    } catch (const std::exception& e) {
        std::cerr << "Failed to initialize UDP transport: " << e.what() << std::endl;
        return false;
    }
}

bool CTDPv2Transport::init_quic_transport() {
#ifdef HAVE_QUICHE
    try {
        quic_transport_ = std::make_unique<QUICTransport>(io_context_, config_);
        return quic_transport_->start();
    } catch (const std::exception& e) {
        std::cerr << "Failed to initialize QUIC transport: " << e.what() << std::endl;
        return false;
    }
#else
    return false;
#endif
}

void CTDPv2Transport::handle_udp_receive(const boost::system::error_code& error, std::size_t bytes_received) {
    if (!running_.load()) {
        return;
    }

    if (!error && bytes_received > 0) {
        // Process the received frame
        std::vector<uint8_t> data(udp_receive_buffer_.begin(), udp_receive_buffer_.begin() + bytes_received);
        process_incoming_frame(data, udp_remote_endpoint_);
        
        // Update statistics
        {
            std::lock_guard<std::mutex> lock(stats_mutex_);
            stats_.messages_received++;
            stats_.bytes_received += bytes_received;
        }
    } else if (error != boost::asio::error::operation_aborted) {
        std::cerr << "UDP receive error: " << error.message() << std::endl;
        
        // Update statistics
        {
            std::lock_guard<std::mutex> lock(stats_mutex_);
            stats_.dropped_frames++;
        }
    }

    // Continue receiving
    if (running_.load()) {
        udp_socket_->async_receive_from(
            boost::asio::buffer(udp_receive_buffer_),
            udp_remote_endpoint_,
            [this](const boost::system::error_code& error, std::size_t bytes_received) {
                handle_udp_receive(error, bytes_received);
            }
        );
    }
}

void CTDPv2Transport::process_incoming_frame(const std::vector<uint8_t>& data, const udp::endpoint& sender) {
    if (data.size() < sizeof(CTDPv2FrameHeader)) {
        std::cerr << "Received frame too small" << std::endl;
        return;
    }

    // Parse frame header
    CTDPv2FrameHeader header;
    std::memcpy(&header, data.data(), sizeof(header));
    
    // Validate header
    if (!validate_frame_header(header)) {
        std::cerr << "Invalid frame header received" << std::endl;
        return;
    }
    
    // Verify checksum
    auto expected_checksum = calculate_checksum(data);
    if (header.checksum != expected_checksum) {
        std::cerr << "Frame checksum mismatch" << std::endl;
        return;
    }
    
    // Extract payload
    std::vector<uint8_t> payload(data.begin() + sizeof(CTDPv2FrameHeader), data.end());
    
    // Create peer info from sender
    PeerInfo sender_info;
    sender_info.address = sender.address();
    sender_info.port = sender.port();
    sender_info.last_seen = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()
    ).count();
    
    // Find message handler
    std::lock_guard<std::mutex> lock(handlers_mutex_);
    auto it = message_handlers_.find(header.message_type);
    if (it != message_handlers_.end()) {
        it->second(header, payload, sender_info);
    } else {
        std::cerr << "No handler for message type: " << static_cast<int>(header.message_type) << std::endl;
    }
}

bool CTDPv2Transport::validate_frame_header(const CTDPv2FrameHeader& header) const {
    // Check protocol version
    if (header.version != 2) {
        return false;
    }
    
    // Check frame size
    if (header.frame_size > config_.max_frame_size) {
        return false;
    }
    
    // Check timestamp (not too far in the future or past)
    auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()
    ).count();
    
    if (header.timestamp > static_cast<uint64_t>(now + 60000) || // 1 minute in future
        header.timestamp < static_cast<uint64_t>(now - 300000)) { // 5 minutes in past
        return false;
    }
    
    return true;
}

uint32_t CTDPv2Transport::calculate_checksum(const std::vector<uint8_t>& data) const {
    // Skip the checksum field in the header when calculating
    std::vector<uint8_t> data_to_hash = data;
    if (data_to_hash.size() >= sizeof(CTDPv2FrameHeader)) {
        // Zero out the checksum field
        std::memset(data_to_hash.data() + offsetof(CTDPv2FrameHeader, checksum), 0, sizeof(uint32_t));
    }
    
    return crc32c_calculate(data_to_hash.data(), data_to_hash.size());
}

std::vector<uint8_t> CTDPv2Transport::serialize_frame(CTDPv2MessageType type, const std::vector<uint8_t>& payload) const {
    CTDPv2FrameHeader header;
    header.version = 2;
    header.message_type = type;
    header.frame_size = static_cast<uint32_t>(sizeof(CTDPv2FrameHeader) + payload.size());
    header.dag_height = 0; // TODO: Get from DAG engine
    header.parent_count = 0; // TODO: Set appropriately for DAG messages
    header.shard_id = 0; // TODO: Get from shard coordinator
    header.timestamp = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()
    ).count();
    header.checksum = 0; // Will be calculated after serialization
    
    // Serialize frame
    std::vector<uint8_t> frame;
    frame.resize(sizeof(CTDPv2FrameHeader) + payload.size());
    
    std::memcpy(frame.data(), &header, sizeof(CTDPv2FrameHeader));
    std::memcpy(frame.data() + sizeof(CTDPv2FrameHeader), payload.data(), payload.size());
    
    // Calculate and set checksum
    uint32_t checksum = calculate_checksum(frame);
    std::memcpy(frame.data() + offsetof(CTDPv2FrameHeader, checksum), &checksum, sizeof(uint32_t));
    
    return frame;
}

void CTDPv2Transport::network_worker() {
    std::cout << "Network worker thread started" << std::endl;
    
    while (running_.load()) {
        try {
            io_context_.run_one();
        } catch (const std::exception& e) {
            std::cerr << "Network worker exception: " << e.what() << std::endl;
        }
        
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    
    std::cout << "Network worker thread stopped" << std::endl;
}

void CTDPv2Transport::ping_worker() {
    std::cout << "Ping worker thread started" << std::endl;
    
    while (running_.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(config_.keepalive_interval_ms));
        
        if (!running_.load()) {
            break;
        }
        
        // Send ping to all peers
        std::vector<PeerInfo> peers;
        {
            std::lock_guard<std::mutex> lock(peers_mutex_);
            for (const auto& [id, peer] : peers_) {
                peers.push_back(peer);
            }
        }
        
        for (const auto& peer : peers) {
            std::vector<uint8_t> ping_payload; // Empty payload for ping
            send_to_peer(peer, CTDPv2MessageType::PING, ping_payload);
        }
    }
    
    std::cout << "Ping worker thread stopped" << std::endl;
}

void CTDPv2Transport::update_peer_stats(const ValidatorId& validator_id, uint32_t bytes, float latency) {
    std::lock_guard<std::mutex> lock(peers_mutex_);
    auto it = peers_.find(validator_id);
    if (it != peers_.end()) {
        it->second.last_seen = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()
        ).count();
        
        if (latency > 0.0f) {
            // Update moving average of latency
            it->second.latency_ms = (it->second.latency_ms * 0.9f) + (latency * 0.1f);
        }
    }
}

// FrameUtils implementation
std::vector<uint8_t> FrameUtils::compress_payload(const std::vector<uint8_t>& data) {
    // Simplified compression - in production would use LZ4 or similar
    return data; // Return uncompressed for now
}

std::vector<uint8_t> FrameUtils::decompress_payload(const std::vector<uint8_t>& compressed_data) {
    // Simplified decompression - in production would use LZ4 or similar
    return compressed_data; // Return as-is for now
}

bool FrameUtils::validate_dag_vertex(const DAGVertexMessage& vertex) {
    // Basic validation
    if (vertex.parent_count > 10) { // Reasonable limit for DAG parents
        return false;
    }
    
    // Check for non-zero hashes (genesis vertex exception handled elsewhere)
    bool all_zero_vertex = std::all_of(vertex.vertex_hash.begin(), vertex.vertex_hash.end(), [](uint8_t b) { return b == 0; });
    bool all_zero_tx = std::all_of(vertex.tx_hash.begin(), vertex.tx_hash.end(), [](uint8_t b) { return b == 0; });
    
    if (all_zero_vertex && all_zero_tx && vertex.logical_clock != 0) {
        return false; // Only genesis can have zero hashes
    }
    
    return true;
}

std::string FrameUtils::validator_id_to_string(const ValidatorId& id) {
    std::string result;
    result.reserve(64);
    for (uint8_t byte : id) {
        char hex[3];
        snprintf(hex, sizeof(hex), "%02x", byte);
        result.append(hex);
    }
    return result;
}

uint64_t FrameUtils::generate_frame_id(const CTDPv2FrameHeader& header) {
    // Simple frame ID generation for deduplication
    return (static_cast<uint64_t>(header.timestamp) << 32) | 
           (static_cast<uint64_t>(header.shard_id) << 16) |
           static_cast<uint64_t>(header.message_type);
}

} // namespace networking
} // namespace credits