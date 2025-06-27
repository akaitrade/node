#include <websocket/websocketserver.hpp>
#include <websocket/websockethandler.hpp>
#include <lib/system/logger.hpp>
#include <csnode/configholder.hpp>

namespace cs {
namespace websocket {

WebSocketServer::WebSocketServer(uint16_t port, std::shared_ptr<WebSocketHandler> handler)
    : port_(port)
    , handler_(handler)
    , running_(false)
    , stop_flag_(false) {
    
    // Set logging settings - disable verbose frame logging to prevent spam
    server_.set_error_channels(websocketpp::log::elevel::all);
    server_.set_access_channels(websocketpp::log::alevel::connect | 
                               websocketpp::log::alevel::disconnect |
                               websocketpp::log::alevel::app);
    
    // Initialize Asio
    server_.init_asio();
    server_.set_reuse_addr(true);
    
    // Set validation handler for debugging browser connections
    server_.set_validate_handler(std::bind(&WebSocketServer::onValidate, this, std::placeholders::_1));
    
    // Set callbacks
    server_.set_open_handler(std::bind(&WebSocketServer::onOpen, this, std::placeholders::_1));
    server_.set_close_handler(std::bind(&WebSocketServer::onClose, this, std::placeholders::_1));
    server_.set_message_handler(std::bind(&WebSocketServer::onMessage, this, std::placeholders::_1, std::placeholders::_2));
    server_.set_fail_handler(std::bind(&WebSocketServer::onError, this, std::placeholders::_1));
    
    // Set handler callbacks
    handler_->setSendCallback(std::bind(&WebSocketServer::sendTo, this, std::placeholders::_1, std::placeholders::_2));
    handler_->setBroadcastCallback(std::bind(&WebSocketServer::broadcast, this, std::placeholders::_1));
}

WebSocketServer::~WebSocketServer() {
    stop();
}

void WebSocketServer::start() {
    if (running_.load()) {
        return;
    }
    
    stop_flag_ = false;
    server_thread_ = std::thread(&WebSocketServer::run, this);
}

void WebSocketServer::stop() {
    if (!running_.load()) {
        return;
    }
    
    stop_flag_ = true;
    
    try {
        // Close all connections
        {
            std::lock_guard<std::mutex> lock(connections_mutex_);
            for (auto& hdl : connections_) {
                server_.close(hdl, websocketpp::close::status::going_away, "Server shutting down");
            }
        }
        
        // Stop accepting new connections
        if (server_.is_listening()) {
            server_.stop_listening();
        }
        
        // Stop the server
        server_.stop();
        
        // Wait for the thread to finish
        if (server_thread_.joinable()) {
            server_thread_.join();
        }
    }
    catch (const std::exception& e) {
        cserror() << "Error stopping WebSocket server: " << e.what();
    }
    
    running_ = false;
}

void WebSocketServer::run() {
    constexpr const uint32_t kTestConfigPortPeriod_sec = 10;
    constexpr const int32_t kRestartPause_ms = 200;
    
    while (!stop_flag_) {
        // Check if WebSocket is enabled in config
        const uint16_t websocket_port = port_;
        if (websocket_port == 0) {
            cslog() << "WebSocket server is disabled (websocket_port = 0)";
            std::this_thread::sleep_for(std::chrono::seconds(kTestConfigPortPeriod_sec));
            continue;
        }
        
        try {
            cslog() << "Starting WebSocket server on port " << websocket_port;
            
            // Listen on specified port
            server_.listen(websocket_port);
            
            // Start accept loop
            server_.start_accept();
            
            running_ = true;
            
            // Run the server
            server_.run();
            
            if (stop_flag_) {
                cslog() << "WebSocket server stopped on port " << websocket_port;
                break;
            }
            
            cslog() << "WebSocket server is trying to restart";
        }
        catch (const websocketpp::exception& e) {
            cserror() << "WebSocket server error: " << e.what();
        }
        catch (const std::exception& e) {
            cserror() << "WebSocket server stopped unexpectedly: " << e.what();
        }
        
        running_ = false;
        
        // Wait before restarting
        std::this_thread::sleep_for(std::chrono::milliseconds(kRestartPause_ms));
        
        if (stop_flag_) {
            break;
        }
        
        // Reset the server for restart
        server_.reset();
    }
}

void WebSocketServer::onOpen(ConnectionHdl hdl) {
    {
        std::lock_guard<std::mutex> lock(connections_mutex_);
        connections_.insert(hdl);
    }
    
    handler_->handleConnect(hdl);
    
    auto con = server_.get_con_from_hdl(hdl);
    cslog() << "WebSocket connection opened from: " << con->get_remote_endpoint();
}

void WebSocketServer::onClose(ConnectionHdl hdl) {
    {
        std::lock_guard<std::mutex> lock(connections_mutex_);
        connections_.erase(hdl);
    }
    
    handler_->handleDisconnect(hdl);
    
    auto con = server_.get_con_from_hdl(hdl);
    cslog() << "WebSocket connection closed from: " << con->get_remote_endpoint();
}

void WebSocketServer::onMessage(ConnectionHdl hdl, MessagePtr msg) {
    try {
        handler_->handleMessage(hdl, msg->get_payload());
    }
    catch (const std::exception& e) {
        cserror() << "Error handling WebSocket message: " << e.what();
    }
}

void WebSocketServer::onError(ConnectionHdl hdl) {
    auto con = server_.get_con_from_hdl(hdl);
    cserror() << "WebSocket connection error: " << con->get_ec().message();
}

bool WebSocketServer::onValidate(ConnectionHdl hdl) {
    auto con = server_.get_con_from_hdl(hdl);
    
    // Log the connection attempt for debugging
    cslog() << "WebSocket validation for: " << con->get_remote_endpoint();
    cslog() << "Origin: " << con->get_request_header("Origin");
    cslog() << "User-Agent: " << con->get_request_header("User-Agent");
    cslog() << "Sec-WebSocket-Version: " << con->get_request_header("Sec-WebSocket-Version");
    cslog() << "Sec-WebSocket-Key: " << con->get_request_header("Sec-WebSocket-Key");
    
    // Accept all connections (return true to accept, false to reject)
    return true;
}

void WebSocketServer::broadcast(const std::string& message) {
    std::lock_guard<std::mutex> lock(connections_mutex_);
    for (auto& hdl : connections_) {
        try {
            server_.send(hdl, message, websocketpp::frame::opcode::text);
        }
        catch (const websocketpp::exception& e) {
            cserror() << "Error broadcasting message: " << e.what();
        }
    }
}

void WebSocketServer::sendTo(ConnectionHdl hdl, const std::string& message) {
    try {
        server_.send(hdl, message, websocketpp::frame::opcode::text);
    }
    catch (const websocketpp::exception& e) {
        cserror() << "Error sending message: " << e.what();
    }
}

} // namespace websocket
} // namespace cs