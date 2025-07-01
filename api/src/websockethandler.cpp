#include <websocket/websockethandler.hpp>
#include <apihandler.hpp>
#include <lib/system/logger.hpp>
#include <sstream>
#include <base58.h>

namespace cs {
namespace websocket {

// Base58 encoding/decoding helper functions for WebSocket API
std::string encodeBase58(const cs::Bytes& bytes) {
    return EncodeBase58(bytes.data(), bytes.data() + bytes.size());
}

std::string encodeBase58(const cs::PublicKey& key) {
    return EncodeBase58(key.data(), key.data() + key.size());
}

bool decodeBase58(const std::string& str, cs::Bytes& bytes) {
    std::vector<unsigned char> decoded;
    if (!DecodeBase58(str, decoded)) {
        return false;
    }
    bytes.assign(decoded.begin(), decoded.end());
    return true;
}

WebSocketHandler::WebSocketHandler(std::shared_ptr<api::APIHandler> apiHandler,
                                   std::shared_ptr<apiexec::APIEXECHandler> apiExecHandler)
    : apiHandler_(apiHandler)
    , apiExecHandler_(apiExecHandler) {
}

WebSocketHandler::~WebSocketHandler() {
}

void WebSocketHandler::handleMessage(ConnectionHdl hdl, const std::string& message) {
    try {
        WebSocketMessage msg = parseMessage(message);
        
        if (msg.type == MessageType::Ping) {
            sendResponse(hdl, MessageType::Pong, msg.id, json::object());
            return;
        }
        
        if (msg.type >= MessageType::Subscribe && msg.type <= MessageType::Unsubscribe) {
            processSubscription(hdl, msg);
        } else {
            processRequest(hdl, msg);
        }
    }
    catch (const std::exception& e) {
        sendError(hdl, "", std::string("Invalid message format: ") + e.what());
    }
}

void WebSocketHandler::handleConnect(ConnectionHdl hdl) {
    std::lock_guard<std::mutex> lock(subscriptions_mutex_);
    subscriptions_[hdl] = std::set<std::string>();
}

void WebSocketHandler::handleDisconnect(ConnectionHdl hdl) {
    std::lock_guard<std::mutex> lock(subscriptions_mutex_);
    subscriptions_.erase(hdl);
}

WebSocketMessage WebSocketHandler::parseMessage(const std::string& message) {
    json j = json::parse(message);
    
    WebSocketMessage msg;
    msg.type = static_cast<MessageType>(j["type"].get<int>());
    msg.id = j["id"].get<std::string>();
    
    if (j.contains("data")) {
        msg.data = j["data"];
    }
    
    return msg;
}

std::string WebSocketHandler::serializeMessage(MessageType type, const std::string& id, const json& data) {
    json msg;
    msg["type"] = static_cast<int>(type);
    msg["id"] = id;
    msg["data"] = data;
    return msg.dump();
}

void WebSocketHandler::processRequest(ConnectionHdl hdl, const WebSocketMessage& msg) {
    switch (msg.type) {
        case MessageType::GetStatus:
            handleGetStatus(hdl, msg);
            break;
        case MessageType::GetBalance:
            handleGetBalance(hdl, msg);
            break;
        case MessageType::GetTransaction:
            handleGetTransaction(hdl, msg);
            break;
        case MessageType::GetPool:
            handleGetPool(hdl, msg);
            break;
        case MessageType::GetPools:
            handleGetPools(hdl, msg);
            break;
        case MessageType::GetPoolsInfo:
            handleGetPoolsInfo(hdl, msg);
            break;
        case MessageType::GetTransactions:
            handleGetTransactions(hdl, msg);
            break;
        case MessageType::GetLastBlockInfo:
            handleGetLastBlockInfo(hdl, msg);
            break;
        case MessageType::GetCounters:
            handleGetCounters(hdl, msg);
            break;
        case MessageType::GetSmartContract:
            handleGetSmartContract(hdl, msg);
            break;
        case MessageType::GetSmartContracts:
            handleGetSmartContracts(hdl, msg);
            break;
        case MessageType::GetSmartContractAddresses:
            handleGetSmartContractAddresses(hdl, msg);
            break;
        case MessageType::GetSmartContractsAll:
            handleGetSmartContractsAll(hdl, msg);
            break;
        case MessageType::GetSmartContractData:
            handleGetSmartContractData(hdl, msg);
            break;
        case MessageType::SmartContractCompile:
            handleSmartContractCompile(hdl, msg);
            break;
        case MessageType::GetContractAllMethods:
            handleGetContractAllMethods(hdl, msg);
            break;
        case MessageType::GetContractMethods:
            handleGetContractMethods(hdl, msg);
            break;
        case MessageType::GetSmartMethodParams:
            handleGetSmartMethodParams(hdl, msg);
            break;
        case MessageType::SmartContractExecute:
            handleSmartContractExecute(hdl, msg);
            break;
        case MessageType::SendTransaction:
            handleSendTransaction(hdl, msg);
            break;
            
        // Token API
        case MessageType::TokenBalancesGet:
            handleTokenBalancesGet(hdl, msg);
            break;
        case MessageType::TokenTransfersGet:
            handleTokenTransfersGet(hdl, msg);
            break;
        case MessageType::TokenTransferGet:
            handleTokenTransferGet(hdl, msg);
            break;
        case MessageType::TokenTransfersListGet:
            handleTokenTransfersListGet(hdl, msg);
            break;
        case MessageType::TokenWalletTransfersGet:
            handleTokenWalletTransfersGet(hdl, msg);
            break;
        case MessageType::TokenTransactionsGet:
            handleTokenTransactionsGet(hdl, msg);
            break;
        case MessageType::TokenInfoGet:
            handleTokenInfoGet(hdl, msg);
            break;
        case MessageType::TokenHoldersGet:
            handleTokenHoldersGet(hdl, msg);
            break;
        case MessageType::TokensListGet:
            handleTokensListGet(hdl, msg);
            break;
            
        // Ordinal API
        case MessageType::OrdinalCNSCheck:
            handleOrdinalCNSCheck(hdl, msg);
            break;
        case MessageType::OrdinalCNSGetByHolder:
            handleOrdinalCNSGetByHolder(hdl, msg);
            break;
        case MessageType::OrdinalTokenGet:
            handleOrdinalTokenGet(hdl, msg);
            break;
        case MessageType::OrdinalTokenBalanceGet:
            handleOrdinalTokenBalanceGet(hdl, msg);
            break;
        case MessageType::OrdinalTokensList:
            handleOrdinalTokensList(hdl, msg);
            break;
        case MessageType::OrdinalStatsGet:
            handleOrdinalStatsGet(hdl, msg);
            break;
        case MessageType::OrdinalCDNSGet:
            handleOrdinalCDNSGet(hdl, msg);
            break;
            
        default:
            sendError(hdl, msg.id, "Unknown message type");
            break;
    }
}

void WebSocketHandler::processSubscription(ConnectionHdl hdl, const WebSocketMessage& msg) {
    std::lock_guard<std::mutex> lock(subscriptions_mutex_);
    
    if (msg.type == MessageType::Subscribe) {
        std::string topic = msg.data["topic"].get<std::string>();
        subscriptions_[hdl].insert(topic);
        
        json response;
        response["subscribed"] = topic;
        sendResponse(hdl, msg.type, msg.id, response);
    }
    else if (msg.type == MessageType::Unsubscribe) {
        std::string topic = msg.data["topic"].get<std::string>();
        subscriptions_[hdl].erase(topic);
        
        json response;
        response["unsubscribed"] = topic;
        sendResponse(hdl, msg.type, msg.id, response);
    }
}

void WebSocketHandler::handleGetStatus(ConnectionHdl hdl, const WebSocketMessage& msg) {
    try {
        api::SyncStateResult result;
        apiHandler_->SyncStateGet(result);
        
        json response;
        response["currRound"] = result.currRound;
        response["lastBlock"] = result.lastBlock;
        
        sendResponse(hdl, msg.type, msg.id, response);
    }
    catch (const std::exception& e) {
        sendError(hdl, msg.id, std::string("Error getting status: ") + e.what());
    }
}

void WebSocketHandler::handleGetBalance(ConnectionHdl hdl, const WebSocketMessage& msg) {
    try {
        std::string addressBase58 = msg.data["address"].get<std::string>();
        
        // Decode Base58 address to binary format for API call
        cs::Bytes addressBytes;
        if (!decodeBase58(addressBase58, addressBytes)) {
            sendError(hdl, msg.id, "Invalid Base58 address format");
            return;
        }
        
        // Convert to string for API (API handler expects string format)
        std::string addressBinary(addressBytes.begin(), addressBytes.end());
        
        api::WalletBalanceGetResult result;
        apiHandler_->WalletBalanceGet(result, addressBinary);
        
        if (result.status.code != 0) {
            sendError(hdl, msg.id, "Address not found");
            return;
        }
        
        json response;
        response["address"] = addressBase58; // Return original Base58 format
        response["balance"] = result.balance.integral + (result.balance.fraction / 1000000000000000000.0);
        response["integral"] = result.balance.integral;
        response["fraction"] = result.balance.fraction;
        
        sendResponse(hdl, msg.type, msg.id, response);
    }
    catch (const std::exception& e) {
        sendError(hdl, msg.id, std::string("Error getting balance: ") + e.what());
    }
}

void WebSocketHandler::handleGetTransaction(ConnectionHdl hdl, const WebSocketMessage& msg) {
    try {
        int64_t poolSeq = msg.data["poolSeq"].get<int64_t>();
        int32_t index = msg.data["index"].get<int32_t>();
        
        api::TransactionId transactionId;
        transactionId.poolSeq = poolSeq;
        transactionId.index = index;
        
        api::TransactionGetResult result;
        apiHandler_->TransactionGet(result, transactionId);
        
        if (result.status.code == 0 && result.found) {
            json response;
            response["found"] = true;
            response["poolSeq"] = result.transaction.id.poolSeq;
            response["index"] = result.transaction.id.index;
            
            // Convert binary addresses to Base58 format
            cs::Bytes sourceBytes(result.transaction.trxn.source.begin(), result.transaction.trxn.source.end());
            cs::Bytes targetBytes(result.transaction.trxn.target.begin(), result.transaction.trxn.target.end());
            
            response["source"] = encodeBase58(sourceBytes);
            response["target"] = encodeBase58(targetBytes);
            response["amount"] = result.transaction.trxn.amount.integral + (result.transaction.trxn.amount.fraction / 1000000000000000000.0);
            response["currency"] = result.transaction.trxn.currency;
            
            // Include userFields if present
            if (result.transaction.trxn.__isset.userFields && !result.transaction.trxn.userFields.empty()) {
                // userFields is already a string in the API, just include it directly
                response["userFields"] = result.transaction.trxn.userFields;
            }
            
            sendResponse(hdl, msg.type, msg.id, response);
        } else {
            json response;
            response["found"] = false;
            sendResponse(hdl, msg.type, msg.id, response);
        }
    }
    catch (const std::exception& e) {
        sendError(hdl, msg.id, std::string("Error getting transaction: ") + e.what());
    }
}

void WebSocketHandler::handleGetPool(ConnectionHdl hdl, const WebSocketMessage& msg) {
    try {
        int64_t sequence = msg.data["sequence"].get<int64_t>();
        
        api::PoolInfoGetResult result;
        apiHandler_->PoolInfoGet(result, sequence, 0);
        
        if (result.status.code != 0) {
            sendError(hdl, msg.id, "Pool not found");
            return;
        }
        
        json response;
        response["sequence"] = result.pool.poolNumber;
        response["hash"] = result.pool.hash;
        response["prevHash"] = result.pool.prevHash;
        response["time"] = result.pool.time;
        response["transactionsCount"] = result.pool.transactionsCount;
        
        sendResponse(hdl, msg.type, msg.id, response);
    }
    catch (const std::exception& e) {
        sendError(hdl, msg.id, std::string("Error getting pool: ") + e.what());
    }
}

void WebSocketHandler::handleGetPools(ConnectionHdl hdl, const WebSocketMessage& msg) {
    try {
        int64_t offset = msg.data["offset"].get<int64_t>();
        int64_t limit = msg.data["limit"].get<int64_t>();
        
        api::PoolListGetResult result;
        apiHandler_->PoolListGet(result, offset, limit);
        
        json response;
        response["pools"] = json::array();
        
        for (const auto& pool : result.pools) {
            json poolJson;
            poolJson["sequence"] = pool.poolNumber;
            poolJson["hash"] = pool.hash;
            poolJson["time"] = pool.time;
            poolJson["transactionsCount"] = pool.transactionsCount;
            response["pools"].push_back(poolJson);
        }
        
        sendResponse(hdl, msg.type, msg.id, response);
    }
    catch (const std::exception& e) {
        sendError(hdl, msg.id, std::string("Error getting pools: ") + e.what());
    }
}

void WebSocketHandler::handleGetPoolsInfo(ConnectionHdl hdl, const WebSocketMessage& msg) {
    try {
        int64_t offset = msg.data["offset"].get<int64_t>();
        int64_t limit = msg.data["limit"].get<int64_t>();
        
        api::PoolListGetResult result;
        apiHandler_->PoolListGet(result, offset, limit);
        
        json response;
        response["pools"] = json::array();
        
        for (const auto& pool : result.pools) {
            json poolJson;
            poolJson["sequence"] = pool.poolNumber;
            poolJson["hash"] = pool.hash;
            poolJson["prevHash"] = pool.prevHash;
            poolJson["time"] = pool.time;
            poolJson["transactionsCount"] = pool.transactionsCount;
            response["pools"].push_back(poolJson);
        }
        
        sendResponse(hdl, msg.type, msg.id, response);
    }
    catch (const std::exception& e) {
        sendError(hdl, msg.id, std::string("Error getting pools info: ") + e.what());
    }
}

void WebSocketHandler::handleGetTransactions(ConnectionHdl hdl, const WebSocketMessage& msg) {
    try {
        std::string addressBase58 = msg.data["address"].get<std::string>();
        int64_t offset = msg.data["offset"].get<int64_t>();
        int64_t limit = msg.data["limit"].get<int64_t>();
        
        // Decode Base58 address to binary format for API call
        cs::Bytes addressBytes;
        if (!decodeBase58(addressBase58, addressBytes)) {
            sendError(hdl, msg.id, "Invalid Base58 address format");
            return;
        }
        
        // Convert to string for API
        std::string addressBinary(addressBytes.begin(), addressBytes.end());
        
        api::TransactionsGetResult result;
        apiHandler_->TransactionsGet(result, addressBinary, offset, limit);
        
        json response;
        response["transactions"] = json::array();
        
        for (const auto& tx : result.transactions) {
            json txJson;
            txJson["poolSeq"] = tx.id.poolSeq;
            txJson["index"] = tx.id.index;
            
            // Convert binary addresses to Base58 format
            cs::Bytes sourceBytes(tx.trxn.source.begin(), tx.trxn.source.end());
            cs::Bytes targetBytes(tx.trxn.target.begin(), tx.trxn.target.end());
            
            txJson["source"] = encodeBase58(sourceBytes);
            txJson["target"] = encodeBase58(targetBytes);
            txJson["amount"] = tx.trxn.amount.integral + (tx.trxn.amount.fraction / 1000000000000000000.0);
            txJson["currency"] = tx.trxn.currency;
            response["transactions"].push_back(txJson);
        }
        
        sendResponse(hdl, msg.type, msg.id, response);
    }
    catch (const std::exception& e) {
        sendError(hdl, msg.id, std::string("Error getting transactions: ") + e.what());
    }
}

void WebSocketHandler::handleGetLastBlockInfo(ConnectionHdl hdl, const WebSocketMessage& msg) {
    try {
        api::PoolInfoGetResult result;
        apiHandler_->PoolInfoGet(result, -1, 0); // Get last pool
        
        if (result.status.code != 0) {
            sendError(hdl, msg.id, "Last block not found");
            return;
        }
        
        json response;
        response["sequence"] = result.pool.poolNumber;
        response["hash"] = result.pool.hash;
        response["prevHash"] = result.pool.prevHash;
        response["time"] = result.pool.time;
        response["transactionsCount"] = result.pool.transactionsCount;
        
        sendResponse(hdl, msg.type, msg.id, response);
    }
    catch (const std::exception& e) {
        sendError(hdl, msg.id, std::string("Error getting last block info: ") + e.what());
    }
}

void WebSocketHandler::handleGetCounters(ConnectionHdl hdl, const WebSocketMessage& msg) {
    try {
        api::StatsGetResult result;
        apiHandler_->StatsGet(result);
        
        json response;
        response["stats"] = json::array();
        
        for (const auto& periodStat : result.stats) {
            json periodJson;
            periodJson["periodDuration"] = periodStat.periodDuration;
            periodJson["poolsCount"] = periodStat.poolsCount;
            periodJson["transactionsCount"] = periodStat.transactionsCount;
            periodJson["smartContractsCount"] = periodStat.smartContractsCount;
            response["stats"].push_back(periodJson);
        }
        
        sendResponse(hdl, msg.type, msg.id, response);
    }
    catch (const std::exception& e) {
        sendError(hdl, msg.id, std::string("Error getting counters: ") + e.what());
    }
}

void WebSocketHandler::handleGetSmartContract(ConnectionHdl hdl, const WebSocketMessage& msg) {
    try {
        std::string addressBase58 = msg.data["address"].get<std::string>();
        
        // Decode Base58 address to binary format for API call
        cs::Bytes addressBytes;
        if (!decodeBase58(addressBase58, addressBytes)) {
            sendError(hdl, msg.id, "Invalid Base58 address format");
            return;
        }
        
        // Convert to string for API
        std::string addressBinary(addressBytes.begin(), addressBytes.end());
        
        api::SmartContractGetResult result;
        apiHandler_->SmartContractGet(result, addressBinary);
        
        if (result.status.code != 0) {
            sendError(hdl, msg.id, result.status.message);
            return;
        }
        
        json response;
        // Convert binary addresses to Base58 format
        cs::Bytes contractAddressBytes(result.smartContract.address.begin(), result.smartContract.address.end());
        cs::Bytes deployerBytes(result.smartContract.deployer.begin(), result.smartContract.deployer.end());
        
        response["address"] = encodeBase58(contractAddressBytes);
        response["deployer"] = encodeBase58(deployerBytes);
        
        // Convert binary objectState to Base58 for JSON safety
        if (!result.smartContract.objectState.empty()) {
            cs::Bytes objectStateBytes(result.smartContract.objectState.begin(), result.smartContract.objectState.end());
            response["objectState"] = encodeBase58(objectStateBytes);
        } else {
            response["objectState"] = "";
        }
        response["createTime"] = result.smartContract.createTime;
        response["transactionsCount"] = result.smartContract.transactionsCount;
        
        // Add smart contract deploy info if available
        if (!result.smartContract.smartContractDeploy.sourceCode.empty()) {
            json deployInfo;
            deployInfo["sourceCode"] = result.smartContract.smartContractDeploy.sourceCode;
            deployInfo["hashState"] = result.smartContract.smartContractDeploy.hashState;
            deployInfo["tokenStandard"] = result.smartContract.smartContractDeploy.tokenStandard;
            deployInfo["lang"] = result.smartContract.smartContractDeploy.lang;
            response["deployInfo"] = deployInfo;
        }
        
        sendResponse(hdl, msg.type, msg.id, response);
    }
    catch (const std::exception& e) {
        sendError(hdl, msg.id, std::string("Error getting smart contract: ") + e.what());
    }
}

void WebSocketHandler::handleGetSmartContracts(ConnectionHdl hdl, const WebSocketMessage& msg) {
    try {
        std::string deployerBase58 = msg.data["deployer"].get<std::string>();
        int64_t offset = msg.data.value("offset", 0);
        int64_t limit = msg.data.value("limit", 10);
        
        // Decode Base58 deployer address to binary format for API call
        cs::Bytes deployerBytes;
        if (!decodeBase58(deployerBase58, deployerBytes)) {
            sendError(hdl, msg.id, "Invalid Base58 deployer address format");
            return;
        }
        
        // Convert to string for API
        std::string deployerBinary(deployerBytes.begin(), deployerBytes.end());
        
        api::SmartContractsListGetResult result;
        apiHandler_->SmartContractsListGet(result, deployerBinary, offset, limit);
        
        if (result.status.code != 0) {
            sendError(hdl, msg.id, result.status.message);
            return;
        }
        
        json response;
        response["count"] = result.count;
        response["smartContracts"] = json::array();
        
        for (const auto& contract : result.smartContractsList) {
            json contractJson;
            
            // Convert binary addresses to Base58 format
            cs::Bytes contractAddressBytes(contract.address.begin(), contract.address.end());
            cs::Bytes contractDeployerBytes(contract.deployer.begin(), contract.deployer.end());
            
            contractJson["address"] = encodeBase58(contractAddressBytes);
            contractJson["deployer"] = encodeBase58(contractDeployerBytes);
            
            // Convert binary objectState to Base58 for JSON safety
            if (!contract.objectState.empty()) {
                cs::Bytes objectStateBytes(contract.objectState.begin(), contract.objectState.end());
                contractJson["objectState"] = encodeBase58(objectStateBytes);
            } else {
                contractJson["objectState"] = "";
            }
            contractJson["createTime"] = contract.createTime;
            contractJson["transactionsCount"] = contract.transactionsCount;
            response["smartContracts"].push_back(contractJson);
        }
        
        sendResponse(hdl, msg.type, msg.id, response);
    }
    catch (const std::exception& e) {
        sendError(hdl, msg.id, std::string("Error getting smart contracts: ") + e.what());
    }
}

void WebSocketHandler::handleGetSmartContractAddresses(ConnectionHdl hdl, const WebSocketMessage& msg) {
    try {
        std::string deployerBase58 = msg.data["deployer"].get<std::string>();
        // Note: offset and limit not used by SmartContractAddressesListGet API
        
        // Decode Base58 deployer address to binary format for API call
        cs::Bytes deployerBytes;
        if (!decodeBase58(deployerBase58, deployerBytes)) {
            sendError(hdl, msg.id, "Invalid Base58 deployer address format");
            return;
        }
        
        // Convert to string for API
        std::string deployerBinary(deployerBytes.begin(), deployerBytes.end());
        
        api::SmartContractAddressesListGetResult result;
        apiHandler_->SmartContractAddressesListGet(result, deployerBinary);
        
        if (result.status.code != 0) {
            sendError(hdl, msg.id, result.status.message);
            return;
        }
        
        json response;
        json addressesArray = json::array();
        
        // Convert binary addresses to Base58 format
        for (const auto& address : result.addressesList) {
            cs::Bytes addressBytes(address.begin(), address.end());
            addressesArray.push_back(encodeBase58(addressBytes));
        }
        
        response["addresses"] = addressesArray;
        response["count"] = static_cast<int>(result.addressesList.size());
        
        sendResponse(hdl, msg.type, msg.id, response);
    }
    catch (const std::exception& e) {
        sendError(hdl, msg.id, std::string("Error getting smart contract addresses: ") + e.what());
    }
}

void WebSocketHandler::handleGetSmartContractsAll(ConnectionHdl hdl, const WebSocketMessage& msg) {
    try {
        int64_t offset = msg.data.value("offset", 0);
        int64_t limit = msg.data.value("limit", 10);
        
        api::SmartContractsListGetResult result;
        apiHandler_->SmartContractsAllListGet(result, offset, limit);
        
        json response;
        response["smartContracts"] = json::array();
        
        for (const auto& contract : result.smartContractsList) {
            json contractJson;
            
            // Convert binary addresses to Base58 format
            cs::Bytes contractAddressBytes(contract.address.begin(), contract.address.end());
            cs::Bytes contractDeployerBytes(contract.deployer.begin(), contract.deployer.end());
            
            contractJson["address"] = encodeBase58(contractAddressBytes);
            contractJson["deployer"] = encodeBase58(contractDeployerBytes);
            
            // Convert binary objectState to Base58 for JSON safety
            if (!contract.objectState.empty()) {
                cs::Bytes objectStateBytes(contract.objectState.begin(), contract.objectState.end());
                contractJson["objectState"] = encodeBase58(objectStateBytes);
            } else {
                contractJson["objectState"] = "";
            }
            contractJson["createTime"] = contract.createTime;
            contractJson["transactionsCount"] = contract.transactionsCount;
            response["smartContracts"].push_back(contractJson);
        }
        
        sendResponse(hdl, msg.type, msg.id, response);
    }
    catch (const std::exception& e) {
        sendError(hdl, msg.id, std::string("Error getting all smart contracts: ") + e.what());
    }
}

void WebSocketHandler::handleGetSmartContractData(ConnectionHdl hdl, const WebSocketMessage& msg) {
    try {
        std::string addressBase58 = msg.data["address"].get<std::string>();
        
        // Decode Base58 address to binary format for API call
        cs::Bytes addressBytes;
        if (!decodeBase58(addressBase58, addressBytes)) {
            sendError(hdl, msg.id, "Invalid Base58 address format");
            return;
        }
        
        std::string addressBinary(addressBytes.begin(), addressBytes.end());
        
        api::SmartContractDataResult result;
        apiHandler_->SmartContractDataGet(result, addressBinary);
        
        if (result.status.code != 0) {
            sendError(hdl, msg.id, result.status.message);
            return;
        }
        
        json response;
        response["methods"] = json::array();
        response["variables"] = json::object();
        
        // Convert methods
        for (const auto& method : result.methods) {
            json methodJson;
            methodJson["returnType"] = method.returnType;
            methodJson["name"] = method.name;
            methodJson["arguments"] = json::array();
            
            for (const auto& arg : method.arguments) {
                json argJson;
                argJson["type"] = arg.type;
                argJson["name"] = arg.name;
                methodJson["arguments"].push_back(argJson);
            }
            
            response["methods"].push_back(methodJson);
        }
        
        // Convert variables (basic conversion, may need enhancement)
        for (const auto& variable : result.variables) {
            // TODO: Implement proper variant conversion - for now just add placeholder
            response["variables"][variable.first] = "variant_value";
        }
        
        sendResponse(hdl, msg.type, msg.id, response);
    }
    catch (const std::exception& e) {
        sendError(hdl, msg.id, std::string("Error getting smart contract data: ") + e.what());
    }
}

void WebSocketHandler::handleSmartContractCompile(ConnectionHdl hdl, const WebSocketMessage& msg) {
    try {
        std::string sourceCode = msg.data["sourceCode"].get<std::string>();
        
        api::SmartContractCompileResult result;
        apiHandler_->SmartContractCompile(result, sourceCode);
        
        if (result.status.code != 0) {
            sendError(hdl, msg.id, result.status.message);
            return;
        }
        
        json response;
        response["byteCodeObjects"] = json::array();
        response["tokenStandard"] = result.tokenStandard;
        response["methods"] = json::array();
        
        // Convert bytecode objects (basic conversion)
        for (const auto& obj : result.byteCodeObjects) {
            json objJson;
            objJson["name"] = obj.name;
            objJson["byteCode"] = obj.byteCode; // Base64 encoded binary data
            response["byteCodeObjects"].push_back(objJson);
        }
        
        // Convert method signatures
        for (const auto& method : result.methods) {
            json methodJson;
            methodJson["signature"] = method.signature;
            cs::Bytes addressBytes(method.address.begin(), method.address.end());
            methodJson["address"] = encodeBase58(addressBytes);
            response["methods"].push_back(methodJson);
        }
        
        sendResponse(hdl, msg.type, msg.id, response);
    }
    catch (const std::exception& e) {
        sendError(hdl, msg.id, std::string("Error compiling smart contract: ") + e.what());
    }
}

void WebSocketHandler::handleGetContractAllMethods(ConnectionHdl hdl, const WebSocketMessage& msg) {
    try {
        // This function requires bytecode objects as input
        if (!msg.data.contains("byteCodeObjects") || !msg.data["byteCodeObjects"].is_array()) {
            sendError(hdl, msg.id, "Missing or invalid byteCodeObjects parameter");
            return;
        }
        
        std::vector<general::ByteCodeObject> byteCodeObjects;
        
        // Convert JSON bytecode objects to Thrift format
        for (const auto& objJson : msg.data["byteCodeObjects"]) {
            general::ByteCodeObject obj;
            obj.name = objJson["name"].get<std::string>();
            obj.byteCode = objJson["byteCode"].get<std::string>();
            byteCodeObjects.push_back(obj);
        }
        
        api::ContractAllMethodsGetResult result;
        apiHandler_->ContractAllMethodsGet(result, byteCodeObjects);
        
        if (result.code != 0) {
            sendError(hdl, msg.id, result.message);
            return;
        }
        
        json response;
        response["methods"] = json::array();
        
        for (const auto& method : result.methods) {
            json methodJson;
            methodJson["name"] = method.name;
            methodJson["returnType"] = method.returnType;
            methodJson["arguments"] = json::array();
            
            for (const auto& arg : method.arguments) {
                json argJson;
                argJson["type"] = arg.type;
                argJson["name"] = arg.name;
                methodJson["arguments"].push_back(argJson);
            }
            
            response["methods"].push_back(methodJson);
        }
        
        sendResponse(hdl, msg.type, msg.id, response);
    }
    catch (const std::exception& e) {
        sendError(hdl, msg.id, std::string("Error getting contract methods: ") + e.what());
    }
}

void WebSocketHandler::handleGetContractMethods(ConnectionHdl hdl, const WebSocketMessage& msg) {
    try {
        std::string addressBase58 = msg.data["address"].get<std::string>();
        
        // Decode Base58 address to binary format for API call
        cs::Bytes addressBytes;
        if (!decodeBase58(addressBase58, addressBytes)) {
            sendError(hdl, msg.id, "Invalid Base58 address format");
            return;
        }
        
        std::string addressBinary(addressBytes.begin(), addressBytes.end());
        
        api::SmartContractDataResult result;
        apiHandler_->SmartContractDataGet(result, addressBinary);
        
        if (result.status.code != 0) {
            sendError(hdl, msg.id, result.status.message);
            return;
        }
        
        json response;
        response["methods"] = json::array();
        
        for (const auto& method : result.methods) {
            json methodJson;
            methodJson["name"] = method.name;
            methodJson["returnType"] = method.returnType;
            methodJson["arguments"] = json::array();
            
            for (const auto& arg : method.arguments) {
                json argJson;
                argJson["type"] = arg.type;
                argJson["name"] = arg.name;
                methodJson["arguments"].push_back(argJson);
            }
            
            response["methods"].push_back(methodJson);
        }
        
        sendResponse(hdl, msg.type, msg.id, response);
    }
    catch (const std::exception& e) {
        sendError(hdl, msg.id, std::string("Error getting contract methods: ") + e.what());
    }
}

void WebSocketHandler::handleGetSmartMethodParams(ConnectionHdl hdl, const WebSocketMessage& msg) {
    try {
        std::string addressBase58 = msg.data["address"].get<std::string>();
        int64_t transactionId = msg.data["transactionId"].get<int64_t>();
        
        // Decode Base58 address to binary format for API call
        cs::Bytes addressBytes;
        if (!decodeBase58(addressBase58, addressBytes)) {
            sendError(hdl, msg.id, "Invalid Base58 address format");
            return;
        }
        
        std::string addressBinary(addressBytes.begin(), addressBytes.end());
        
        api::SmartMethodParamsGetResult result;
        apiHandler_->SmartMethodParamsGet(result, addressBinary, transactionId);
        
        if (result.status.code != 0) {
            sendError(hdl, msg.id, result.status.message);
            return;
        }
        
        json response;
        response["method"] = result.method;
        response["params"] = json::array();
        
        // Convert variant parameters (basic conversion)
        for (size_t i = 0; i < result.params.size(); ++i) {
            // TODO: Implement proper variant conversion - for now just add placeholder
            response["params"].push_back("variant_param_" + std::to_string(i));
        }
        
        sendResponse(hdl, msg.type, msg.id, response);
    }
    catch (const std::exception& e) {
        sendError(hdl, msg.id, std::string("Error getting method parameters: ") + e.what());
    }
}

void WebSocketHandler::handleSmartContractExecute(ConnectionHdl hdl, const WebSocketMessage& msg) {
    try {
        // Required parameters
        std::string senderAddressBase58 = msg.data["senderAddress"].get<std::string>();
        std::string contractAddressBase58 = msg.data["contractAddress"].get<std::string>();
        std::string method = msg.data["method"].get<std::string>();
        bool saveToBlockchain = msg.data.value("saveToBlockchain", true); // Default: save to blockchain
        double maxFee = msg.data.value("maxFee", 0.1); // Default fee
        
        // Optional parameters
        std::vector<general::Variant> params;
        if (msg.data.contains("params") && msg.data["params"].is_array()) {
            for (const auto& paramJson : msg.data["params"]) {
                general::Variant param;
                // Simple parameter conversion - handle String, int, boolean
                if (paramJson.contains("String")) {
                    param.__set_v_string(paramJson["String"].get<std::string>());
                } else if (paramJson.contains("int")) {
                    param.__set_v_int(paramJson["int"].get<int32_t>());
                } else if (paramJson.contains("boolean")) {
                    param.__set_v_boolean(paramJson["boolean"].get<bool>());
                } else if (paramJson.contains("double")) {
                    param.__set_v_double(paramJson["double"].get<double>());
                }
                params.push_back(param);
            }
        }
        
        std::vector<general::Address> usedContracts;
        if (msg.data.contains("usedContracts") && msg.data["usedContracts"].is_array()) {
            for (const auto& contractAddrJson : msg.data["usedContracts"]) {
                std::string contractBase58 = contractAddrJson.get<std::string>();
                cs::Bytes contractBytes;
                if (decodeBase58(contractBase58, contractBytes)) {
                    std::string contractBinary(contractBytes.begin(), contractBytes.end());
                    usedContracts.push_back(contractBinary);
                }
            }
        }
        
        // Decode addresses
        cs::Bytes senderBytes, contractBytes;
        if (!decodeBase58(senderAddressBase58, senderBytes)) {
            sendError(hdl, msg.id, "Invalid sender address format");
            return;
        }
        if (!decodeBase58(contractAddressBase58, contractBytes)) {
            sendError(hdl, msg.id, "Invalid contract address format");
            return;
        }
        
        std::string senderBinary(senderBytes.begin(), senderBytes.end());
        std::string contractBinary(contractBytes.begin(), contractBytes.end());
        
        // Create transaction
        api::Transaction transaction;
        transaction.id = 0; // Will be set by the system
        transaction.source = senderBinary;
        transaction.target = contractBinary;
        
        // Set amount to 0 for contract execution
        transaction.amount.integral = 0;
        transaction.amount.fraction = 0;
        
        // Set fee
        transaction.fee.commission = static_cast<int16_t>(maxFee * 1000); // Convert to fee format
        
        // Create smart contract invocation
        api::SmartContractInvocation invocation;
        invocation.method = method;
        invocation.params = params;
        invocation.usedContracts = usedContracts;
        invocation.forgetNewState = !saveToBlockchain; // Invert because forgetNewState means don't save
        invocation.version = 1;
        
        transaction.__set_smartContract(invocation);
        transaction.type = api::TransactionType::TT_ContractCall;
        transaction.currency = 1; // CS currency
        transaction.timeCreation = std::time(nullptr) * 1000; // Current time in milliseconds
        
        // Execute the transaction
        api::TransactionFlowResult result;
        apiHandler_->TransactionFlow(result, transaction);
        
        if (result.status.code != 0) {
            sendError(hdl, msg.id, result.status.message);
            return;
        }
        
        json response;
        response["roundNum"] = result.roundNum;
        response["transactionId"] = {
            {"poolSeq", result.id.poolSeq},
            {"index", result.id.index}
        };
        response["fee"] = {
            {"integral", result.fee.integral},
            {"fraction", result.fee.fraction}
        };
        response["saveToBlockchain"] = saveToBlockchain;
        
        // Include smart contract result if available
        if (result.__isset.smart_contract_result) {
            // TODO: Convert variant result to JSON - for now return simple indicator
            response["hasResult"] = true;
            response["resultType"] = "variant"; // Placeholder - needs proper variant conversion
        } else {
            response["hasResult"] = false;
        }
        
        sendResponse(hdl, msg.type, msg.id, response);
    }
    catch (const std::exception& e) {
        sendError(hdl, msg.id, std::string("Error executing smart contract: ") + e.what());
    }
}

void WebSocketHandler::handleSendTransaction(ConnectionHdl hdl, const WebSocketMessage& msg) {
    try {
        // Required parameters
        std::string senderAddressBase58 = msg.data["senderAddress"].get<std::string>();
        std::string targetAddressBase58 = msg.data["targetAddress"].get<std::string>();
        double amount = msg.data["amount"].get<double>();
        double maxFee = msg.data["maxFee"].get<double>();
        
        // Optional parameters
        std::string userFields = msg.data.value("userFields", "");
        
        // Decode addresses
        cs::Bytes senderBytes, targetBytes;
        if (!decodeBase58(senderAddressBase58, senderBytes)) {
            sendError(hdl, msg.id, "Invalid sender address format");
            return;
        }
        if (!decodeBase58(targetAddressBase58, targetBytes)) {
            sendError(hdl, msg.id, "Invalid target address format");
            return;
        }
        
        std::string senderBinary(senderBytes.begin(), senderBytes.end());
        std::string targetBinary(targetBytes.begin(), targetBytes.end());
        
        // Parse amount (integral and fractional parts)
        int64_t amountIntegral = static_cast<int64_t>(amount);
        int64_t amountFraction = static_cast<int64_t>((amount - amountIntegral) * 1000000000000000000LL); // 18 decimal places
        
        // Create transaction
        api::Transaction transaction;
        transaction.id = 0; // Will be set by the system
        transaction.source = senderBinary;
        transaction.target = targetBinary;
        
        // Set amount
        transaction.amount.integral = amountIntegral;
        transaction.amount.fraction = amountFraction;
        
        // Set fee (convert to Credits fee format)
        transaction.fee.commission = static_cast<int16_t>(maxFee * 1000);
        
        transaction.type = api::TransactionType::TT_Transfer;
        transaction.currency = 1; // CS currency
        transaction.timeCreation = std::time(nullptr) * 1000; // Current time in milliseconds
        
        // Handle user fields if provided
        if (!userFields.empty()) {
            // Convert user fields string to binary
            transaction.__set_userFields(userFields);
        }
        
        // Execute the transaction using TransactionFlow
        api::TransactionFlowResult result;
        apiHandler_->TransactionFlow(result, transaction);
        
        if (result.status.code != 0) {
            sendError(hdl, msg.id, result.status.message);
            return;
        }
        
        json response;
        response["roundNum"] = result.roundNum;
        response["transactionId"] = {
            {"poolSeq", result.id.poolSeq},
            {"index", result.id.index}
        };
        response["fee"] = {
            {"integral", result.fee.integral},
            {"fraction", result.fee.fraction}
        };
        response["amount"] = {
            {"integral", amountIntegral},
            {"fraction", amountFraction}
        };
        
        sendResponse(hdl, msg.type, msg.id, response);
    }
    catch (const std::exception& e) {
        sendError(hdl, msg.id, std::string("Error sending transaction: ") + e.what());
    }
}

void WebSocketHandler::notifyNewBlock(const json& blockInfo) {
    std::lock_guard<std::mutex> lock(subscriptions_mutex_);
    
    if (!sendCallback_) {
        return; // Callback not set, cannot send notifications
    }
    
    std::string message = serializeMessage(MessageType::NewBlock, "", blockInfo);
    
    for (const auto& [hdl, topics] : subscriptions_) {
        if (topics.find("blocks") != topics.end()) {
            sendCallback_(hdl, message);
        }
    }
}

void WebSocketHandler::notifyNewTransaction(const json& txInfo) {
    std::lock_guard<std::mutex> lock(subscriptions_mutex_);
    
    if (!sendCallback_) {
        return; // Callback not set, cannot send notifications
    }
    
    std::string message = serializeMessage(MessageType::NewTransaction, "", txInfo);
    
    for (const auto& [hdl, topics] : subscriptions_) {
        if (topics.find("transactions") != topics.end()) {
            sendCallback_(hdl, message);
        }
    }
}

void WebSocketHandler::notifyTransactionStatus(const std::string& txId, const json& status) {
    std::lock_guard<std::mutex> lock(subscriptions_mutex_);
    
    json data;
    data["transactionId"] = txId;
    data["status"] = status;
    
    std::string message = serializeMessage(MessageType::TransactionStatus, "", data);
    
    for (const auto& [hdl, topics] : subscriptions_) {
        if (topics.find("tx:" + txId) != topics.end()) {
            sendCallback_(hdl, message);
        }
    }
}

void WebSocketHandler::notifySmartContractEvent(const json& event) {
    std::lock_guard<std::mutex> lock(subscriptions_mutex_);
    
    std::string message = serializeMessage(MessageType::SmartContractEvent, "", event);
    
    for (const auto& [hdl, topics] : subscriptions_) {
        if (topics.find("smart_contracts") != topics.end()) {
            sendCallback_(hdl, message);
        }
    }
}

void WebSocketHandler::sendError(ConnectionHdl hdl, const std::string& id, const std::string& error) {
    json data;
    data["error"] = error;
    sendResponse(hdl, MessageType::Error, id, data);
}

void WebSocketHandler::sendResponse(ConnectionHdl hdl, MessageType type, const std::string& id, const json& data) {
    std::string message = serializeMessage(type, id, data);
    if (sendCallback_) {
        sendCallback_(hdl, message);
    }
}

// Token API implementations
void WebSocketHandler::handleTokenBalancesGet(ConnectionHdl hdl, const WebSocketMessage& msg) {
    try {
        std::string addressBase58 = msg.data["address"].get<std::string>();
        
        // Decode Base58 address to binary format for API call
        cs::Bytes addressBytes;
        if (!decodeBase58(addressBase58, addressBytes)) {
            sendError(hdl, msg.id, "Invalid Base58 address format");
            return;
        }
        
        std::string addressBinary(addressBytes.begin(), addressBytes.end());
        
        api::TokenBalancesResult result;
        apiHandler_->TokenBalancesGet(result, addressBinary);
        
        if (result.status.code != 0) {
            sendError(hdl, msg.id, result.status.message);
            return;
        }
        
        json response;
        response["address"] = addressBase58;
        response["tokens"] = json::array();
        
        for (const auto& balance : result.balances) {
            json tokenJson;
            cs::Bytes tokenAddressBytes(balance.token.begin(), balance.token.end());
            tokenJson["token"] = encodeBase58(tokenAddressBytes);
            tokenJson["code"] = balance.code;
            tokenJson["name"] = balance.name;
            tokenJson["balance"] = balance.balance;
            response["tokens"].push_back(tokenJson);
        }
        
        sendResponse(hdl, msg.type, msg.id, response);
    }
    catch (const std::exception& e) {
        sendError(hdl, msg.id, std::string("Error getting token balances: ") + e.what());
    }
}

void WebSocketHandler::handleTokenTransfersGet(ConnectionHdl hdl, const WebSocketMessage& msg) {
    try {
        std::string tokenBase58 = msg.data["token"].get<std::string>();
        int64_t offset = msg.data["offset"].get<int64_t>();
        int64_t limit = msg.data["limit"].get<int64_t>();
        
        // Decode Base58 token address to binary format for API call
        cs::Bytes tokenBytes;
        if (!decodeBase58(tokenBase58, tokenBytes)) {
            sendError(hdl, msg.id, "Invalid Base58 token address format");
            return;
        }
        
        std::string tokenBinary(tokenBytes.begin(), tokenBytes.end());
        
        api::TokenTransfersResult result;
        apiHandler_->TokenTransfersGet(result, tokenBinary, offset, limit);
        
        if (result.status.code != 0) {
            sendError(hdl, msg.id, result.status.message);
            return;
        }
        
        json response;
        response["transfers"] = json::array();
        
        for (const auto& transfer : result.transfers) {
            json transferJson;
            cs::Bytes tokenAddressBytes(transfer.token.begin(), transfer.token.end());
            cs::Bytes senderBytes(transfer.sender.begin(), transfer.sender.end());
            cs::Bytes receiverBytes(transfer.receiver.begin(), transfer.receiver.end());
            cs::Bytes initiatorBytes(transfer.initiator.begin(), transfer.initiator.end());
            
            transferJson["token"] = encodeBase58(tokenAddressBytes);
            transferJson["code"] = transfer.code;
            transferJson["sender"] = encodeBase58(senderBytes);
            transferJson["receiver"] = encodeBase58(receiverBytes);
            transferJson["amount"] = transfer.amount;
            transferJson["initiator"] = encodeBase58(initiatorBytes);
            transferJson["poolSeq"] = transfer.transaction.poolSeq;
            transferJson["index"] = transfer.transaction.index;
            transferJson["time"] = transfer.time;
            
            response["transfers"].push_back(transferJson);
        }
        
        sendResponse(hdl, msg.type, msg.id, response);
    }
    catch (const std::exception& e) {
        sendError(hdl, msg.id, std::string("Error getting token transfers: ") + e.what());
    }
}

void WebSocketHandler::handleTokenTransferGet(ConnectionHdl hdl, const WebSocketMessage& msg) {
    try {
        std::string tokenBase58 = msg.data["token"].get<std::string>();
        int64_t poolSeq = msg.data["poolSeq"].get<int64_t>();
        int32_t index = msg.data["index"].get<int32_t>();
        
        // Decode Base58 token address to binary format for API call
        cs::Bytes tokenBytes;
        if (!decodeBase58(tokenBase58, tokenBytes)) {
            sendError(hdl, msg.id, "Invalid Base58 token address format");
            return;
        }
        
        std::string tokenBinary(tokenBytes.begin(), tokenBytes.end());
        
        api::TransactionId transactionId;
        transactionId.poolSeq = poolSeq;
        transactionId.index = index;
        
        api::TokenTransfersResult result;
        apiHandler_->TokenTransferGet(result, tokenBinary, transactionId);
        
        if (result.status.code != 0) {
            sendError(hdl, msg.id, result.status.message);
            return;
        }
        
        json response;
        response["transfers"] = json::array();
        
        for (const auto& transfer : result.transfers) {
            json transferJson;
            cs::Bytes tokenAddressBytes(transfer.token.begin(), transfer.token.end());
            cs::Bytes senderBytes(transfer.sender.begin(), transfer.sender.end());
            cs::Bytes receiverBytes(transfer.receiver.begin(), transfer.receiver.end());
            cs::Bytes initiatorBytes(transfer.initiator.begin(), transfer.initiator.end());
            
            transferJson["token"] = encodeBase58(tokenAddressBytes);
            transferJson["code"] = transfer.code;
            transferJson["sender"] = encodeBase58(senderBytes);
            transferJson["receiver"] = encodeBase58(receiverBytes);
            transferJson["amount"] = transfer.amount;
            transferJson["initiator"] = encodeBase58(initiatorBytes);
            transferJson["poolSeq"] = transfer.transaction.poolSeq;
            transferJson["index"] = transfer.transaction.index;
            transferJson["time"] = transfer.time;
            
            response["transfers"].push_back(transferJson);
        }
        
        sendResponse(hdl, msg.type, msg.id, response);
    }
    catch (const std::exception& e) {
        sendError(hdl, msg.id, std::string("Error getting token transfer: ") + e.what());
    }
}

void WebSocketHandler::handleTokenTransfersListGet(ConnectionHdl hdl, const WebSocketMessage& msg) {
    try {
        int64_t offset = msg.data["offset"].get<int64_t>();
        int64_t limit = msg.data["limit"].get<int64_t>();
        
        api::TokenTransfersResult result;
        apiHandler_->TokenTransfersListGet(result, offset, limit);
        
        if (result.status.code != 0) {
            sendError(hdl, msg.id, result.status.message);
            return;
        }
        
        json response;
        response["transfers"] = json::array();
        
        for (const auto& transfer : result.transfers) {
            json transferJson;
            cs::Bytes tokenAddressBytes(transfer.token.begin(), transfer.token.end());
            cs::Bytes senderBytes(transfer.sender.begin(), transfer.sender.end());
            cs::Bytes receiverBytes(transfer.receiver.begin(), transfer.receiver.end());
            cs::Bytes initiatorBytes(transfer.initiator.begin(), transfer.initiator.end());
            
            transferJson["token"] = encodeBase58(tokenAddressBytes);
            transferJson["code"] = transfer.code;
            transferJson["sender"] = encodeBase58(senderBytes);
            transferJson["receiver"] = encodeBase58(receiverBytes);
            transferJson["amount"] = transfer.amount;
            transferJson["initiator"] = encodeBase58(initiatorBytes);
            transferJson["poolSeq"] = transfer.transaction.poolSeq;
            transferJson["index"] = transfer.transaction.index;
            transferJson["time"] = transfer.time;
            
            response["transfers"].push_back(transferJson);
        }
        
        sendResponse(hdl, msg.type, msg.id, response);
    }
    catch (const std::exception& e) {
        sendError(hdl, msg.id, std::string("Error getting token transfers list: ") + e.what());
    }
}

void WebSocketHandler::handleTokenWalletTransfersGet(ConnectionHdl hdl, const WebSocketMessage& msg) {
    try {
        std::string tokenBase58 = msg.data["token"].get<std::string>();
        std::string addressBase58 = msg.data["address"].get<std::string>();
        int64_t offset = msg.data["offset"].get<int64_t>();
        int64_t limit = msg.data["limit"].get<int64_t>();
        
        // Decode Base58 addresses to binary format for API call
        cs::Bytes tokenBytes, addressBytes;
        if (!decodeBase58(tokenBase58, tokenBytes) || !decodeBase58(addressBase58, addressBytes)) {
            sendError(hdl, msg.id, "Invalid Base58 address format");
            return;
        }
        
        std::string tokenBinary(tokenBytes.begin(), tokenBytes.end());
        std::string addressBinary(addressBytes.begin(), addressBytes.end());
        
        api::TokenTransfersResult result;
        apiHandler_->TokenWalletTransfersGet(result, tokenBinary, addressBinary, offset, limit);
        
        if (result.status.code != 0) {
            sendError(hdl, msg.id, result.status.message);
            return;
        }
        
        json response;
        response["transfers"] = json::array();
        
        for (const auto& transfer : result.transfers) {
            json transferJson;
            cs::Bytes tokenAddressBytes(transfer.token.begin(), transfer.token.end());
            cs::Bytes senderBytes(transfer.sender.begin(), transfer.sender.end());
            cs::Bytes receiverBytes(transfer.receiver.begin(), transfer.receiver.end());
            cs::Bytes initiatorBytes(transfer.initiator.begin(), transfer.initiator.end());
            
            transferJson["token"] = encodeBase58(tokenAddressBytes);
            transferJson["code"] = transfer.code;
            transferJson["sender"] = encodeBase58(senderBytes);
            transferJson["receiver"] = encodeBase58(receiverBytes);
            transferJson["amount"] = transfer.amount;
            transferJson["initiator"] = encodeBase58(initiatorBytes);
            transferJson["poolSeq"] = transfer.transaction.poolSeq;
            transferJson["index"] = transfer.transaction.index;
            transferJson["time"] = transfer.time;
            
            response["transfers"].push_back(transferJson);
        }
        
        sendResponse(hdl, msg.type, msg.id, response);
    }
    catch (const std::exception& e) {
        sendError(hdl, msg.id, std::string("Error getting wallet token transfers: ") + e.what());
    }
}

void WebSocketHandler::handleTokenTransactionsGet(ConnectionHdl hdl, const WebSocketMessage& msg) {
    try {
        std::string tokenBase58 = msg.data["token"].get<std::string>();
        int64_t offset = msg.data["offset"].get<int64_t>();
        int64_t limit = msg.data["limit"].get<int64_t>();
        
        // Decode Base58 token address to binary format for API call
        cs::Bytes tokenBytes;
        if (!decodeBase58(tokenBase58, tokenBytes)) {
            sendError(hdl, msg.id, "Invalid Base58 token address format");
            return;
        }
        
        std::string tokenBinary(tokenBytes.begin(), tokenBytes.end());
        
        api::TokenTransactionsResult result;
        apiHandler_->TokenTransactionsGet(result, tokenBinary, offset, limit);
        
        if (result.status.code != 0) {
            sendError(hdl, msg.id, result.status.message);
            return;
        }
        
        json response;
        response["transactions"] = json::array();
        
        for (const auto& transaction : result.transactions) {
            json txJson;
            cs::Bytes tokenAddressBytes(transaction.token.begin(), transaction.token.end());
            cs::Bytes initiatorBytes(transaction.initiator.begin(), transaction.initiator.end());
            
            txJson["token"] = encodeBase58(tokenAddressBytes);
            txJson["poolSeq"] = transaction.transaction.poolSeq;
            txJson["index"] = transaction.transaction.index;
            txJson["time"] = transaction.time;
            txJson["initiator"] = encodeBase58(initiatorBytes);
            txJson["method"] = transaction.method;
            // Convert Variant vector to JSON array
            json paramsJson = json::array();
            for (const auto& param : transaction.params) {
                // Convert Variant to appropriate JSON type
                if (param.__isset.v_boolean) {
                    paramsJson.push_back(param.v_boolean);
                } else if (param.__isset.v_int) {
                    paramsJson.push_back(param.v_int);
                } else if (param.__isset.v_long) {
                    paramsJson.push_back(param.v_long);
                } else if (param.__isset.v_double) {
                    paramsJson.push_back(param.v_double);
                } else if (param.__isset.v_string) {
                    paramsJson.push_back(param.v_string);
                } else {
                    paramsJson.push_back(nullptr); // Unknown type
                }
            }
            txJson["params"] = paramsJson;
            txJson["state"] = static_cast<int>(transaction.state);
            
            response["transactions"].push_back(txJson);
        }
        
        sendResponse(hdl, msg.type, msg.id, response);
    }
    catch (const std::exception& e) {
        sendError(hdl, msg.id, std::string("Error getting token transactions: ") + e.what());
    }
}

void WebSocketHandler::handleTokenInfoGet(ConnectionHdl hdl, const WebSocketMessage& msg) {
    try {
        std::string tokenBase58 = msg.data["token"].get<std::string>();
        
        // Decode Base58 token address to binary format for API call
        cs::Bytes tokenBytes;
        if (!decodeBase58(tokenBase58, tokenBytes)) {
            sendError(hdl, msg.id, "Invalid Base58 token address format");
            return;
        }
        
        std::string tokenBinary(tokenBytes.begin(), tokenBytes.end());
        
        api::TokenInfoResult result;
        apiHandler_->TokenInfoGet(result, tokenBinary);
        
        if (result.status.code != 0) {
            sendError(hdl, msg.id, result.status.message);
            return;
        }
        
        json response;
        cs::Bytes tokenAddressBytes(result.token.address.begin(), result.token.address.end());
        cs::Bytes ownerBytes(result.token.owner.begin(), result.token.owner.end());
        
        response["address"] = encodeBase58(tokenAddressBytes);
        response["code"] = result.token.code;
        response["name"] = result.token.name;
        response["totalSupply"] = result.token.totalSupply;
        response["owner"] = encodeBase58(ownerBytes);
        response["transfersCount"] = result.token.transfersCount;
        response["transactionsCount"] = result.token.transactionsCount;
        response["holdersCount"] = result.token.holdersCount;
        response["tokenStandard"] = static_cast<int>(result.token.tokenStandard);
        
        sendResponse(hdl, msg.type, msg.id, response);
    }
    catch (const std::exception& e) {
        sendError(hdl, msg.id, std::string("Error getting token info: ") + e.what());
    }
}

void WebSocketHandler::handleTokenHoldersGet(ConnectionHdl hdl, const WebSocketMessage& msg) {
    try {
        std::string tokenBase58 = msg.data["token"].get<std::string>();
        int64_t offset = msg.data["offset"].get<int64_t>();
        int64_t limit = msg.data["limit"].get<int64_t>();
        api::TokenHoldersSortField order = static_cast<api::TokenHoldersSortField>(msg.data.value("order", 0));
        bool desc = msg.data.value("desc", false);
        
        // Decode Base58 token address to binary format for API call
        cs::Bytes tokenBytes;
        if (!decodeBase58(tokenBase58, tokenBytes)) {
            sendError(hdl, msg.id, "Invalid Base58 token address format");
            return;
        }
        
        std::string tokenBinary(tokenBytes.begin(), tokenBytes.end());
        
        api::TokenHoldersResult result;
        apiHandler_->TokenHoldersGet(result, tokenBinary, offset, limit, order, desc);
        
        if (result.status.code != 0) {
            sendError(hdl, msg.id, result.status.message);
            return;
        }
        
        json response;
        response["holders"] = json::array();
        
        for (const auto& holder : result.holders) {
            json holderJson;
            cs::Bytes holderAddressBytes(holder.holder.begin(), holder.holder.end());
            cs::Bytes tokenAddressBytes(holder.token.begin(), holder.token.end());
            
            holderJson["holder"] = encodeBase58(holderAddressBytes);
            holderJson["token"] = encodeBase58(tokenAddressBytes);
            holderJson["balance"] = holder.balance;
            holderJson["transfersCount"] = holder.transfersCount;
            
            response["holders"].push_back(holderJson);
        }
        
        sendResponse(hdl, msg.type, msg.id, response);
    }
    catch (const std::exception& e) {
        sendError(hdl, msg.id, std::string("Error getting token holders: ") + e.what());
    }
}

void WebSocketHandler::handleTokensListGet(ConnectionHdl hdl, const WebSocketMessage& msg) {
    try {
        int64_t offset = msg.data["offset"].get<int64_t>();
        int64_t limit = msg.data["limit"].get<int64_t>();
        api::TokensListSortField order = static_cast<api::TokensListSortField>(msg.data.value("order", 0));
        bool desc = msg.data.value("desc", false);
        
        // Parse filters if provided
        api::TokenFilters filters;
        if (msg.data.contains("filters")) {
            if (msg.data["filters"].contains("name")) {
                filters.name = msg.data["filters"]["name"].get<std::string>();
            }
            if (msg.data["filters"].contains("code")) {
                filters.code = msg.data["filters"]["code"].get<std::string>();
            }
            if (msg.data["filters"].contains("tokenStandard")) {
                filters.tokenStandard = msg.data["filters"]["tokenStandard"].get<int>();
            }
        }
        
        api::TokensListResult result;
        apiHandler_->TokensListGet(result, offset, limit, order, desc, filters);
        
        if (result.status.code != 0) {
            sendError(hdl, msg.id, result.status.message);
            return;
        }
        
        json response;
        response["count"] = result.count;
        response["tokens"] = json::array();
        
        for (const auto& token : result.tokens) {
            json tokenJson;
            cs::Bytes tokenAddressBytes(token.address.begin(), token.address.end());
            cs::Bytes ownerBytes(token.owner.begin(), token.owner.end());
            
            tokenJson["address"] = encodeBase58(tokenAddressBytes);
            tokenJson["code"] = token.code;
            tokenJson["name"] = token.name;
            tokenJson["totalSupply"] = token.totalSupply;
            tokenJson["owner"] = encodeBase58(ownerBytes);
            tokenJson["transfersCount"] = token.transfersCount;
            tokenJson["transactionsCount"] = token.transactionsCount;
            tokenJson["holdersCount"] = token.holdersCount;
            tokenJson["tokenStandard"] = static_cast<int>(token.tokenStandard);
            
            response["tokens"].push_back(tokenJson);
        }
        
        sendResponse(hdl, msg.type, msg.id, response);
    }
    catch (const std::exception& e) {
        sendError(hdl, msg.id, std::string("Error getting tokens list: ") + e.what());
    }
}

// Ordinal API implementations
void WebSocketHandler::handleOrdinalCNSCheck(ConnectionHdl hdl, const WebSocketMessage& msg) {
    try {
        std::string name = msg.data["name"].get<std::string>();
        
        api::OrdinalCNSCheckResult result;
        apiHandler_->OrdinalCNSCheck(result, name);
        
        if (result.status.code != 0) {
            sendError(hdl, msg.id, result.status.message);
            return;
        }
        
        json response;
        response["available"] = result.available;
        
        if (result.__isset.cnsInfo) {
            json cnsInfo;
            cnsInfo["protocol"] = result.cnsInfo.protocol;
            cnsInfo["operation"] = result.cnsInfo.operation;
            cnsInfo["name"] = result.cnsInfo.name;
            
            cs::Bytes holderBytes(result.cnsInfo.holder.begin(), result.cnsInfo.holder.end());
            cnsInfo["holder"] = encodeBase58(holderBytes);
            cnsInfo["blockNumber"] = result.cnsInfo.blockNumber;
            cnsInfo["txIndex"] = result.cnsInfo.txIndex;
            
            // Add relay field if available
            if (result.cnsInfo.__isset.relay) {
                cnsInfo["relay"] = result.cnsInfo.relay;
            } else {
                cnsInfo["relay"] = "";
            }
            
            // Updated to CNS
            response["cnsInfo"] = cnsInfo;
        }
        
        sendResponse(hdl, msg.type, msg.id, response);
    }
    catch (const std::exception& e) {
        sendError(hdl, msg.id, std::string("Error checking CNS name: ") + e.what());
    }
}

void WebSocketHandler::handleOrdinalCNSGetByHolder(ConnectionHdl hdl, const WebSocketMessage& msg) {
    try {
        std::string holderBase58 = msg.data["holder"].get<std::string>();
        
        // Decode Base58 holder address to binary format for API call
        cs::Bytes holderBytes;
        if (!decodeBase58(holderBase58, holderBytes)) {
            sendError(hdl, msg.id, "Invalid Base58 holder address format");
            return;
        }
        
        std::string holderBinary(holderBytes.begin(), holderBytes.end());
        
        api::OrdinalCNSGetResult result;
        apiHandler_->OrdinalCNSGetByHolder(result, holderBinary);
        
        if (result.status.code != 0) {
            sendError(hdl, msg.id, result.status.message);
            return;
        }
        
        json response;
        response["cnsEntries"] = json::array();
        
        for (const auto& cnsEntry : result.cnsEntries) {
            json cnsJson;
            cnsJson["protocol"] = cnsEntry.protocol;
            cnsJson["operation"] = cnsEntry.operation;
            cnsJson["name"] = cnsEntry.name;
            
            cs::Bytes entryHolderBytes(cnsEntry.holder.begin(), cnsEntry.holder.end());
            cnsJson["holder"] = encodeBase58(entryHolderBytes);
            cnsJson["blockNumber"] = cnsEntry.blockNumber;
            cnsJson["txIndex"] = cnsEntry.txIndex;
            
            // Add relay field if available
            if (cnsEntry.__isset.relay) {
                cnsJson["relay"] = cnsEntry.relay;
            } else {
                cnsJson["relay"] = "";
            }
            
            response["cnsEntries"].push_back(cnsJson);
        }
        
        sendResponse(hdl, msg.type, msg.id, response);
    }
    catch (const std::exception& e) {
        sendError(hdl, msg.id, std::string("Error getting CNS by holder: ") + e.what());
    }
}

void WebSocketHandler::handleOrdinalTokenGet(ConnectionHdl hdl, const WebSocketMessage& msg) {
    try {
        std::string ticker = msg.data["ticker"].get<std::string>();
        
        api::OrdinalTokenInfoResult result;
        apiHandler_->OrdinalTokenGet(result, ticker);
        
        if (result.status.code != 0) {
            sendError(hdl, msg.id, result.status.message);
            return;
        }
        
        json response;
        if (result.__isset.tokenInfo) {
            json tokenInfo;
            tokenInfo["ticker"] = result.tokenInfo.ticker;
            tokenInfo["maxSupply"] = result.tokenInfo.maxSupply;
            tokenInfo["limitPerMint"] = result.tokenInfo.limitPerMint;
            tokenInfo["totalMinted"] = result.tokenInfo.totalMinted;
            tokenInfo["deployBlock"] = result.tokenInfo.deployBlock;
            
            cs::Bytes deployerBytes(result.tokenInfo.deployer.begin(), result.tokenInfo.deployer.end());
            // Check if deployer bytes are valid (not empty or all zeros)
            bool validDeployer = !deployerBytes.empty() && 
                               std::any_of(deployerBytes.begin(), deployerBytes.end(), [](uint8_t b) { return b != 0; });
            
            if (validDeployer) {
                tokenInfo["deployer"] = encodeBase58(deployerBytes);
            } else {
                tokenInfo["deployer"] = ""; // Return empty string for invalid deployer
            }
            
            response["tokenInfo"] = tokenInfo;
        }
        
        sendResponse(hdl, msg.type, msg.id, response);
    }
    catch (const std::exception& e) {
        sendError(hdl, msg.id, std::string("Error getting ordinal token: ") + e.what());
    }
}

void WebSocketHandler::handleOrdinalTokenBalanceGet(ConnectionHdl hdl, const WebSocketMessage& msg) {
    try {
        std::string addressBase58 = msg.data["address"].get<std::string>();
        std::string ticker = msg.data["ticker"].get<std::string>();
        
        // Decode Base58 address to binary format for API call
        cs::Bytes addressBytes;
        if (!decodeBase58(addressBase58, addressBytes)) {
            sendError(hdl, msg.id, "Invalid Base58 address format");
            return;
        }
        
        std::string addressBinary(addressBytes.begin(), addressBytes.end());
        
        api::OrdinalTokenBalanceResult result;
        apiHandler_->OrdinalTokenBalanceGet(result, addressBinary, ticker);
        
        if (result.status.code != 0) {
            sendError(hdl, msg.id, result.status.message);
            return;
        }
        
        json response;
        response["address"] = addressBase58;
        response["ticker"] = ticker;
        response["balance"] = result.balance;
        
        sendResponse(hdl, msg.type, msg.id, response);
    }
    catch (const std::exception& e) {
        sendError(hdl, msg.id, std::string("Error getting ordinal token balance: ") + e.what());
    }
}

void WebSocketHandler::handleOrdinalTokensList(ConnectionHdl hdl, const WebSocketMessage& msg) {
    try {
        int64_t offset = msg.data["offset"].get<int64_t>();
        int64_t limit = msg.data["limit"].get<int64_t>();
        
        api::OrdinalTokensListResult result;
        apiHandler_->OrdinalTokensList(result, offset, limit);
        
        if (result.status.code != 0) {
            sendError(hdl, msg.id, result.status.message);
            return;
        }
        
        json response;
        response["count"] = result.count;
        response["tokens"] = json::array();
        
        for (const auto& token : result.tokens) {
            json tokenJson;
            tokenJson["ticker"] = token.ticker;
            tokenJson["maxSupply"] = token.maxSupply;
            tokenJson["limitPerMint"] = token.limitPerMint;
            tokenJson["totalMinted"] = token.totalMinted;
            tokenJson["deployBlock"] = token.deployBlock;
            
            cs::Bytes deployerBytes(token.deployer.begin(), token.deployer.end());
            // Check if deployer bytes are valid (not empty or all zeros)
            bool validDeployer = !deployerBytes.empty() && 
                               std::any_of(deployerBytes.begin(), deployerBytes.end(), [](uint8_t b) { return b != 0; });
            
            if (validDeployer) {
                tokenJson["deployer"] = encodeBase58(deployerBytes);
            } else {
                tokenJson["deployer"] = ""; // Return empty string for invalid deployer
            }
            
            response["tokens"].push_back(tokenJson);
        }
        
        sendResponse(hdl, msg.type, msg.id, response);
    }
    catch (const std::exception& e) {
        sendError(hdl, msg.id, std::string("Error getting ordinal tokens list: ") + e.what());
    }
}

void WebSocketHandler::handleOrdinalStatsGet(ConnectionHdl hdl, const WebSocketMessage& msg) {
    try {
        api::OrdinalStatsResult result;
        apiHandler_->OrdinalStatsGet(result);
        
        if (result.status.code != 0) {
            sendError(hdl, msg.id, result.status.message);
            return;
        }
        
        json response;
        response["totalCNS"] = result.totalCNS;
        response["totalTokens"] = result.totalTokens;
        response["totalInscriptions"] = result.totalInscriptions;
        
        sendResponse(hdl, msg.type, msg.id, response);
    }
    catch (const std::exception& e) {
        sendError(hdl, msg.id, std::string("Error getting ordinal stats: ") + e.what());
    }
}

void WebSocketHandler::handleOrdinalCDNSGet(ConnectionHdl hdl, const WebSocketMessage& msg) {
    try {
        std::string name = msg.data["name"].get<std::string>();
        
        api::OrdinalCNSCheckResult result;
        apiHandler_->OrdinalCNSCheck(result, name);
        
        if (result.status.code != 0) {
            sendError(hdl, msg.id, result.status.message);
            return;
        }
        
        json response;
        response["available"] = result.available;
        response["relay"] = "";
        
        // Only return relay information if CNS exists
        if (!result.available && result.__isset.cnsInfo) {
            if (result.cnsInfo.__isset.relay) {
                response["relay"] = result.cnsInfo.relay;
            }
        }
        
        sendResponse(hdl, msg.type, msg.id, response);
    }
    catch (const std::exception& e) {
        sendError(hdl, msg.id, std::string("Error getting CNS relay: ") + e.what());
    }
}

// Additional notification methods for tokens and ordinals
void WebSocketHandler::notifyTokenTransfer(const json& transferInfo) {
    std::lock_guard<std::mutex> lock(subscriptions_mutex_);
    
    if (!sendCallback_) {
        return; // Callback not set, cannot send notifications
    }
    
    std::string message = serializeMessage(MessageType::TokenTransfer, "", transferInfo);
    
    for (const auto& [hdl, topics] : subscriptions_) {
        if (topics.find("token_transfers") != topics.end()) {
            sendCallback_(hdl, message);
        }
    }
}

void WebSocketHandler::notifyTokenDeploy(const json& deployInfo) {
    std::lock_guard<std::mutex> lock(subscriptions_mutex_);
    
    std::string message = serializeMessage(MessageType::TokenDeploy, "", deployInfo);
    
    for (const auto& [hdl, topics] : subscriptions_) {
        if (topics.find("token_deploys") != topics.end()) {
            sendCallback_(hdl, message);
        }
    }
}

void WebSocketHandler::notifyOrdinalInscription(const json& inscriptionInfo) {
    std::lock_guard<std::mutex> lock(subscriptions_mutex_);
    
    std::string message = serializeMessage(MessageType::OrdinalInscription, "", inscriptionInfo);
    
    for (const auto& [hdl, topics] : subscriptions_) {
        if (topics.find("ordinal_inscriptions") != topics.end()) {
            sendCallback_(hdl, message);
        }
    }
}

void WebSocketHandler::notifyOrdinalTransfer(const json& transferInfo) {
    std::lock_guard<std::mutex> lock(subscriptions_mutex_);
    
    std::string message = serializeMessage(MessageType::OrdinalTransfer, "", transferInfo);
    
    for (const auto& [hdl, topics] : subscriptions_) {
        if (topics.find("ordinal_transfers") != topics.end()) {
            sendCallback_(hdl, message);
        }
    }
}

} // namespace websocket
} // namespace cs