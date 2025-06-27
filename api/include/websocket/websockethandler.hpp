#ifndef WEBSOCKETHANDLER_HPP
#define WEBSOCKETHANDLER_HPP

#include <memory>
#include <string>
#include <functional>
#include <map>
#include <set>
#include <mutex>
#include <websocketpp/common/connection_hdl.hpp>
#include <nlohmann/json.hpp>

namespace api {
class APIHandler;
}

namespace apiexec {
class APIEXECHandler;
}

namespace cs {
namespace websocket {

using json = nlohmann::json;
using ConnectionHdl = websocketpp::connection_hdl;

enum class MessageType {
    // Requests
    GetStatus = 1,
    GetBalance = 2,
    GetTransaction = 3,
    GetPool = 4,
    GetPools = 5,
    GetPoolsInfo = 6,
    GetTransactions = 7,
    GetLastBlockInfo = 8,
    GetCounters = 9,
    GetSmartContract = 10,
    GetSmartContracts = 11,
    GetSmartContractAddresses = 12,
    
    // Token API (13-30)
    TokenBalancesGet = 13,
    TokenTransfersGet = 14,
    TokenTransferGet = 15,
    TokenTransfersListGet = 16,
    TokenWalletTransfersGet = 17,
    TokenTransactionsGet = 18,
    TokenInfoGet = 19,
    TokenHoldersGet = 20,
    TokensListGet = 21,
    
    // Ordinal API (31-40)
    OrdinalSNSCheck = 31,
    OrdinalSNSGetByHolder = 32,
    OrdinalTokenGet = 33,
    OrdinalTokenBalanceGet = 34,
    OrdinalTokensList = 35,
    OrdinalStatsGet = 36,
    
    // Subscriptions
    Subscribe = 100,
    Unsubscribe = 101,
    
    // Notifications
    NewBlock = 200,
    NewTransaction = 201,
    TransactionStatus = 202,
    SmartContractEvent = 203,
    TokenTransfer = 204,
    TokenDeploy = 205,
    OrdinalInscription = 206,
    OrdinalTransfer = 207,
    
    // System
    Error = 400,
    Ping = 500,
    Pong = 501
};

struct WebSocketMessage {
    MessageType type;
    std::string id;
    json data;
};

class WebSocketHandler {
public:
    WebSocketHandler(std::shared_ptr<api::APIHandler> apiHandler,
                     std::shared_ptr<apiexec::APIEXECHandler> apiExecHandler);
    ~WebSocketHandler();

    void handleMessage(ConnectionHdl hdl, const std::string& message);
    void handleConnect(ConnectionHdl hdl);
    void handleDisconnect(ConnectionHdl hdl);

    using SendCallback = std::function<void(ConnectionHdl, const std::string&)>;
    using BroadcastCallback = std::function<void(const std::string&)>;
    
    void setSendCallback(SendCallback callback) { sendCallback_ = callback; }
    void setBroadcastCallback(BroadcastCallback callback) { broadcastCallback_ = callback; }

    // Event notifications (called from blockchain events)
    void notifyNewBlock(const json& blockInfo);
    void notifyNewTransaction(const json& txInfo);
    void notifyTransactionStatus(const std::string& txId, const json& status);
    void notifySmartContractEvent(const json& event);
    void notifyTokenTransfer(const json& transferInfo);
    void notifyTokenDeploy(const json& deployInfo);
    void notifyOrdinalInscription(const json& inscriptionInfo);
    void notifyOrdinalTransfer(const json& transferInfo);

private:
    WebSocketMessage parseMessage(const std::string& message);
    std::string serializeMessage(MessageType type, const std::string& id, const json& data);
    
    void processRequest(ConnectionHdl hdl, const WebSocketMessage& msg);
    void processSubscription(ConnectionHdl hdl, const WebSocketMessage& msg);
    
    // API request handlers
    void handleGetStatus(ConnectionHdl hdl, const WebSocketMessage& msg);
    void handleGetBalance(ConnectionHdl hdl, const WebSocketMessage& msg);
    void handleGetTransaction(ConnectionHdl hdl, const WebSocketMessage& msg);
    void handleGetPool(ConnectionHdl hdl, const WebSocketMessage& msg);
    void handleGetPools(ConnectionHdl hdl, const WebSocketMessage& msg);
    void handleGetPoolsInfo(ConnectionHdl hdl, const WebSocketMessage& msg);
    void handleGetTransactions(ConnectionHdl hdl, const WebSocketMessage& msg);
    void handleGetLastBlockInfo(ConnectionHdl hdl, const WebSocketMessage& msg);
    void handleGetCounters(ConnectionHdl hdl, const WebSocketMessage& msg);
    void handleGetSmartContract(ConnectionHdl hdl, const WebSocketMessage& msg);
    void handleGetSmartContracts(ConnectionHdl hdl, const WebSocketMessage& msg);
    void handleGetSmartContractAddresses(ConnectionHdl hdl, const WebSocketMessage& msg);
    
    // Token API handlers
    void handleTokenBalancesGet(ConnectionHdl hdl, const WebSocketMessage& msg);
    void handleTokenTransfersGet(ConnectionHdl hdl, const WebSocketMessage& msg);
    void handleTokenTransferGet(ConnectionHdl hdl, const WebSocketMessage& msg);
    void handleTokenTransfersListGet(ConnectionHdl hdl, const WebSocketMessage& msg);
    void handleTokenWalletTransfersGet(ConnectionHdl hdl, const WebSocketMessage& msg);
    void handleTokenTransactionsGet(ConnectionHdl hdl, const WebSocketMessage& msg);
    void handleTokenInfoGet(ConnectionHdl hdl, const WebSocketMessage& msg);
    void handleTokenHoldersGet(ConnectionHdl hdl, const WebSocketMessage& msg);
    void handleTokensListGet(ConnectionHdl hdl, const WebSocketMessage& msg);
    
    // Ordinal API handlers
    void handleOrdinalSNSCheck(ConnectionHdl hdl, const WebSocketMessage& msg);
    void handleOrdinalSNSGetByHolder(ConnectionHdl hdl, const WebSocketMessage& msg);
    void handleOrdinalTokenGet(ConnectionHdl hdl, const WebSocketMessage& msg);
    void handleOrdinalTokenBalanceGet(ConnectionHdl hdl, const WebSocketMessage& msg);
    void handleOrdinalTokensList(ConnectionHdl hdl, const WebSocketMessage& msg);
    void handleOrdinalStatsGet(ConnectionHdl hdl, const WebSocketMessage& msg);
    
    void sendError(ConnectionHdl hdl, const std::string& id, const std::string& error);
    void sendResponse(ConnectionHdl hdl, MessageType type, const std::string& id, const json& data);

private:
    std::shared_ptr<api::APIHandler> apiHandler_;
    std::shared_ptr<apiexec::APIEXECHandler> apiExecHandler_;
    
    SendCallback sendCallback_;
    BroadcastCallback broadcastCallback_;
    
    // Subscription management
    std::mutex subscriptions_mutex_;
    std::map<ConnectionHdl, std::set<std::string>, std::owner_less<ConnectionHdl>> subscriptions_;
};

} // namespace websocket
} // namespace cs

#endif // WEBSOCKETHANDLER_HPP