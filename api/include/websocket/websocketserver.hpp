#ifndef WEBSOCKETSERVER_HPP
#define WEBSOCKETSERVER_HPP

#include <atomic>
#include <memory>
#include <thread>
#include <functional>
#include <set>
#include <mutex>
#include <websocketpp/config/asio_no_tls.hpp>
#include <websocketpp/server.hpp>
#include <websocketpp/common/connection_hdl.hpp>

namespace cs {
namespace websocket {

using Server = websocketpp::server<websocketpp::config::asio>;
using ConnectionHdl = websocketpp::connection_hdl;
using MessagePtr = Server::message_ptr;

class WebSocketHandler;

class WebSocketServer {
public:
    WebSocketServer(uint16_t port, std::shared_ptr<WebSocketHandler> handler);
    ~WebSocketServer();

    WebSocketServer(const WebSocketServer&) = delete;
    WebSocketServer& operator=(const WebSocketServer&) = delete;

    void start();
    void stop();
    bool isRunning() const { return running_.load(); }

    void broadcast(const std::string& message);
    void sendTo(ConnectionHdl hdl, const std::string& message);

private:
    void onOpen(ConnectionHdl hdl);
    void onClose(ConnectionHdl hdl);
    void onMessage(ConnectionHdl hdl, MessagePtr msg);
    void onError(ConnectionHdl hdl);
    bool onValidate(ConnectionHdl hdl);

    void run();

private:
    uint16_t port_;
    std::shared_ptr<WebSocketHandler> handler_;
    std::atomic<bool> running_;
    std::atomic<bool> stop_flag_;
    
    Server server_;
    std::thread server_thread_;
    
    std::mutex connections_mutex_;
    std::set<ConnectionHdl, std::owner_less<ConnectionHdl>> connections_;
};

} // namespace websocket
} // namespace cs

#endif // WEBSOCKETSERVER_HPP