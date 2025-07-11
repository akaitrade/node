/*!
 * CTDP v2 Transport Layer
 * 
 * Enhanced Credits Transport Data Protocol with QUIC support and DAG frame headers
 */

#pragma once

// Platform-specific packed attribute
#ifdef _MSC_VER
    #define PACKED
    #define PACK_START __pragma(pack(push, 1))
    #define PACK_END __pragma(pack(pop))
#else
    #define PACKED __attribute__((packed))
    #define PACK_START
    #define PACK_END
#endif

#include <cstdint>
#include <vector>
#include <memory>
#include <functional>
#include <unordered_map>
#include <thread>
#include <atomic>
#include <mutex>
#include <condition_variable>
#include "../../common/include/array_hash.hpp"

#include <boost/asio.hpp>
#include <boost/asio/ip/udp.hpp>

// DAG engine integration
extern "C" {
#include "dag_engine.h"
}

namespace credits {
namespace networking {

using boost::asio::ip::udp;
using VertexHash = std::array<uint8_t, 32>;
using ValidatorId = std::array<uint8_t, 32>;

/// Message types for CTDP v2
enum class CTDPv2MessageType : uint8_t {
    // Legacy CTDP messages (compatibility)
    LEGACY_TRANSACTION = 0x01,
    LEGACY_BLOCK = 0x02,
    LEGACY_CONSENSUS = 0x03,
    
    // New DAG messages
    DAG_VERTEX = 0x10,
    DAG_BATCH = 0x11,
    VIRTUAL_VOTE = 0x12,
    GOSSIP_VOTE = 0x13,
    FINALITY_PROOF = 0x14,
    
    // Shard management
    SHARD_ASSIGNMENT = 0x20,
    SHARD_REBALANCE = 0x21,
    
    // Agent chain messages
    AGENT_CHAIN_UPDATE = 0x30,
    CROSS_AGENT_TRANSACTION = 0x31,
    
    // Network management
    PING = 0xF0,
    PONG = 0xF1,
    DISCOVERY = 0xF2,
};

/// CTDP v2 frame header
PACK_START
struct CTDPv2FrameHeader {
    uint8_t version = 2;                    // Protocol version
    CTDPv2MessageType message_type;         // Message type
    uint32_t frame_size;                    // Total frame size
    uint64_t dag_height;                    // DAG progression height
    uint32_t parent_count;                  // Number of DAG parent references
    uint32_t shard_id;                      // Shard assignment
    uint64_t timestamp;                     // Frame timestamp
    uint32_t checksum;                      // Frame integrity checksum
} PACKED;
PACK_END

/// DAG vertex message payload
struct DAGVertexMessage {
    VertexHash vertex_hash;
    VertexHash tx_hash;
    uint64_t logical_clock;
    uint32_t parent_count;
    // Followed by: parent_hashes[], transaction_data, signature
};

/// Virtual vote message payload
struct VirtualVoteMessage {
    ValidatorId validator;
    VertexHash vertex_hash;
    uint8_t vote_type; // 0 = reject, 1 = approve
    uint64_t round;
    uint64_t timestamp;
    std::array<uint8_t, 48> signature; // BLS signature
};

/// Transport configuration
struct CTDPv2Config {
    uint16_t port = 6000;
    bool enable_quic = true;
    bool enable_legacy_udp = true;
    uint32_t max_frame_size = 1024 * 1024; // 1MB
    uint32_t connection_timeout_ms = 30000;
    uint32_t keepalive_interval_ms = 5000;
    uint32_t max_concurrent_connections = 1000;
    bool enable_compression = true;
};

/// Network peer information
struct PeerInfo {
    boost::asio::ip::address address;
    uint16_t port;
    ValidatorId validator_id;
    uint64_t last_seen;
    bool is_trusted_node;
    float latency_ms;
    uint32_t connection_count;
};

/// Message handler callback type
using MessageHandler = std::function<void(const CTDPv2FrameHeader&, const std::vector<uint8_t>&, const PeerInfo&)>;

/// Connection event callback type
using ConnectionEventHandler = std::function<void(const PeerInfo&, bool connected)>;

/// Main CTDP v2 transport class
class CTDPv2Transport {
public:
    explicit CTDPv2Transport(boost::asio::io_context& io_context, const CTDPv2Config& config);
    ~CTDPv2Transport();

    /// Start the transport layer
    bool start();
    
    /// Stop the transport layer
    void stop();
    
    /// Send message to specific peer
    bool send_to_peer(const PeerInfo& peer, CTDPv2MessageType type, const std::vector<uint8_t>& payload);
    
    /// Send message to multiple peers
    bool broadcast_message(CTDPv2MessageType type, const std::vector<uint8_t>& payload, const std::vector<PeerInfo>& peers);
    
    /// Send DAG vertex to network
    bool send_dag_vertex(const DAGVertexMessage& vertex, const std::vector<VertexHash>& parents, const std::vector<uint8_t>& transaction_data);
    
    /// Send virtual vote
    bool send_virtual_vote(const VirtualVoteMessage& vote);
    
    /// Register message handler
    void register_message_handler(CTDPv2MessageType type, MessageHandler handler);
    
    /// Register connection event handler
    void register_connection_handler(ConnectionEventHandler handler);
    
    /// Add trusted peer
    void add_trusted_peer(const PeerInfo& peer);
    
    /// Remove peer
    void remove_peer(const ValidatorId& validator_id);
    
    /// Get connected peers
    std::vector<PeerInfo> get_connected_peers() const;
    
    /// Get network statistics
    struct NetworkStats {
        uint64_t messages_sent;
        uint64_t messages_received;
        uint64_t bytes_sent;
        uint64_t bytes_received;
        uint32_t active_connections;
        float average_latency_ms;
        uint32_t dropped_frames;
    };
    
    NetworkStats get_network_stats() const;

private:
    /// Initialize UDP transport
    bool init_udp_transport();
    
    /// Initialize QUIC transport (if available)
    bool init_quic_transport();
    
    /// Handle incoming UDP packet
    void handle_udp_receive(const boost::system::error_code& error, std::size_t bytes_received);
    
    /// Process incoming frame
    void process_incoming_frame(const std::vector<uint8_t>& data, const udp::endpoint& sender);
    
    /// Validate frame header
    bool validate_frame_header(const CTDPv2FrameHeader& header) const;
    
    /// Calculate frame checksum
    uint32_t calculate_checksum(const std::vector<uint8_t>& data) const;
    
    /// Serialize frame
    std::vector<uint8_t> serialize_frame(CTDPv2MessageType type, const std::vector<uint8_t>& payload) const;
    
    /// Network worker thread
    void network_worker();
    
    /// Ping worker thread for keepalive
    void ping_worker();
    
    /// Update peer statistics
    void update_peer_stats(const ValidatorId& validator_id, uint32_t bytes, float latency);

private:
    boost::asio::io_context& io_context_;
    CTDPv2Config config_;
    
    // UDP transport
    std::unique_ptr<udp::socket> udp_socket_;
    udp::endpoint udp_remote_endpoint_;
    std::array<uint8_t, 65536> udp_receive_buffer_;
    
    // QUIC transport (if available)
#ifdef HAVE_QUICHE
    std::unique_ptr<class QUICTransport> quic_transport_;
#endif
    
    // Peer management
    mutable std::mutex peers_mutex_;
    std::unordered_map<ValidatorId, PeerInfo> peers_;
    
    // Message handling
    std::mutex handlers_mutex_;
    std::unordered_map<CTDPv2MessageType, MessageHandler> message_handlers_;
    std::vector<ConnectionEventHandler> connection_handlers_;
    
    // Network statistics
    mutable std::mutex stats_mutex_;
    NetworkStats stats_;
    
    // Threading
    std::vector<std::thread> worker_threads_;
    std::atomic<bool> running_;
    
    // Timing
    std::chrono::steady_clock::time_point start_time_;
};

/// Utility functions for frame handling
class FrameUtils {
public:
    /// Compress payload data
    static std::vector<uint8_t> compress_payload(const std::vector<uint8_t>& data);
    
    /// Decompress payload data
    static std::vector<uint8_t> decompress_payload(const std::vector<uint8_t>& compressed_data);
    
    /// Validate DAG vertex message
    static bool validate_dag_vertex(const DAGVertexMessage& vertex);
    
    /// Convert validator ID to string for logging
    static std::string validator_id_to_string(const ValidatorId& id);
    
    /// Generate frame ID for deduplication
    static uint64_t generate_frame_id(const CTDPv2FrameHeader& header);
};

} // namespace networking
} // namespace credits

