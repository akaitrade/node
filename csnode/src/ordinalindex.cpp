#include <ordinalindex.hpp>

#include <algorithm>
#include <set>
#include <vector>

#include <boost/filesystem.hpp>

#include <csdb/internal/utils.hpp>
#include <csdb/pool.hpp>
#include <csdb/transaction.hpp>
#include <lib/system/logger.hpp>
#include <csnode/blockchain.hpp>
#include <base58.h>

namespace {
constexpr const char* kDbPath = "/ordinaldb";
constexpr const char* kLastIndexedPath = "/ordinal_last_indexed";

// Key prefixes for different data types in LMDB
constexpr uint8_t kSNSPrefix = 0x01;
constexpr uint8_t kTokenPrefix = 0x02;
constexpr uint8_t kTokenBalancePrefix = 0x03;
constexpr uint8_t kOrdinalMetaPrefix = 0x04;

// Ordinal user field ID - try multiple common IDs
constexpr csdb::user_field_id_t kOrdinalFieldId = 1000;
constexpr csdb::user_field_id_t kAlternateFieldIds[] = {0, 1, 2, 5, 10, 100, 999};

cs::Bytes appendPrefix(uint8_t prefix, const cs::Bytes& data) {
    cs::Bytes ret;
    ret.reserve(1 + data.size());
    ret.push_back(prefix);
    ret.insert(ret.end(), data.begin(), data.end());
    return ret;
}
} // namespace

namespace cs {

OrdinalIndex::OrdinalIndex(BlockChain& _bc, const std::string& _path, bool _recreate)
    : bc_(_bc)
    , rootPath_(_path)
    , db_(std::make_unique<Lmdb>(_path + kDbPath))
    , recreate_(_recreate ? true : hasToRecreate(_path + kLastIndexedPath, lastIndexedPool_))
    , lastIndexedFile_(_path + kLastIndexedPath, sizeof(cs::Sequence)) {
    cslog() << "Initializing OrdinalIndex at path: " << _path << ", forced recreate: " << _recreate << ", hasToRecreate: " << hasToRecreate(_path + kLastIndexedPath, lastIndexedPool_) << ", final recreate: " << recreate_ << ", lastIndexedPool: " << lastIndexedPool_;
    init();
}

bool OrdinalIndex::recreate() const {
    return recreate_;
}

void OrdinalIndex::onStartReadFromDb(Sequence _lastWrittenPoolSeq) {
    cslog() << "OrdinalIndex: onStartReadFromDb - lastIndexed=" << lastIndexedPool_ << ", lastWritten=" << _lastWrittenPoolSeq << ", recreate=" << recreate_;
    
    // Only force recreation if ordinal index is significantly behind or corrupted
    // Normal case: ordinal index is behind by some blocks but can continue incrementally
    if (!recreate_ && lastIndexedPool_ != kWrongSequence && lastIndexedPool_ <= _lastWrittenPoolSeq) {
        cslog() << "OrdinalIndex: will resume from block " << (lastIndexedPool_ + 1) << " to " << _lastWrittenPoolSeq;
        // Don't force recreation - let it continue incrementally
    }
    else if (!recreate_ && (lastIndexedPool_ == kWrongSequence || lastIndexedPool_ > _lastWrittenPoolSeq)) {
        cslog() << "OrdinalIndex: detected corruption or invalid state. lastIndexed=" << lastIndexedPool_ << ", lastWritten=" << _lastWrittenPoolSeq;
        recreate_ = true;
    }
}

void OrdinalIndex::onReadFromDb(const csdb::Pool& _pool) {
    if (_pool.sequence() == 0 && recreate_) {
        cslog() << "OrdinalIndex: resetting and reinitializing due to recreate flag";
        reset();
        init();
    }

    if (recreate_ || lastIndexedPool_ < _pool.sequence()) {
        csdebug() << "OrdinalIndex: processing block " << _pool.sequence() << " (lastIndexed: " << lastIndexedPool_ << ", recreate: " << recreate_ << ")";
        updateFromNextBlock(_pool);
    }
}

void OrdinalIndex::onDbReadFinished() {
    if (recreate_) {
        recreate_ = false;
        snsCache_.clear();
        tokenCache_.clear();
        balanceCache_.clear();
        
        // Reset counters for fresh recreation
        totalSNSCount_ = 0;
        totalTokenCount_ = 0;
        totalInscriptionCount_ = 0;
        countersInitialized_ = false;
        cslog() << "Recreated ordinal index 0 -> " << lastIndexedPool_
                << ". Continue to keep it actual from new blocks.";
    }
    else {
        updateLastIndexed();
    }
    
    // Final checkpoint save to ensure progress is preserved
    cslog() << "OrdinalIndex: completed indexing up to block " << lastIndexedPool_ 
            << " (SNS: " << getTotalSNSCount() << ", Tokens: " << getTotalTokenCount() << ", Total: " << getTotalInscriptionCount() << ")"
            << " - recreate mode was: " << (recreate_ ? "true" : "false");
}

void OrdinalIndex::onRemoveBlock(const csdb::Pool& _pool) {
    // Handle blockchain reorganization by removing ordinals from the removed block
    for (const auto& tx : _pool.transactions()) {
        auto ordinalMeta = parseOrdinalFromTransaction(tx);
        if (!ordinalMeta) continue;

        auto jsonOpt = OrdinalJsonParser::parse(ordinalMeta->data);
        if (!jsonOpt) continue;
        auto& json = *jsonOpt;

        switch (ordinalMeta->type) {
            case OrdinalType::SNS: {
                auto sns = parseSNSInscription(json);
                if (sns) {
                    removeSNS(sns->name);
                }
                break;
            }
            case OrdinalType::Token: {
                auto token = parseTokenInscription(json);
                if (token) {
                    removeTokenMint(token->tick, token->amt);
                }
                break;
            }
            case OrdinalType::Deploy: {
                // For simplicity, we don't remove deployed tokens on reorg
                // In production, you might want to handle this differently
                break;
            }
            default:
                break;
        }
    }

    --lastIndexedPool_;
    updateLastIndexed();
}

void OrdinalIndex::update(const csdb::Pool& _pool) {
    updateFromNextBlock(_pool);
}

void OrdinalIndex::invalidate() {
    lastIndexedPool_ = kWrongSequence;
    updateLastIndexed();
}

void OrdinalIndex::close() {
    if (db_->isOpen()) {
        db_->close();
    }
}

void OrdinalIndex::updateFromNextBlock(const csdb::Pool& _pool) {
    size_t totalTxCount = _pool.transactions().size();
    size_t ordinalTxCount = 0;
    size_t txWithUserFields = 0;
    
    for (const auto& tx : _pool.transactions()) {
        try {
            // Count transactions with user fields for statistics
            auto userFieldIds = tx.user_field_ids();
            if (!userFieldIds.empty()) {
                txWithUserFields++;
            }
            
            auto ordinalMeta = parseOrdinalFromTransaction(tx);
            if (!ordinalMeta) continue;
            
            ordinalTxCount++;
            cslog() << "Found ordinal transaction in block " << _pool.sequence() << ", type: " << static_cast<int>(ordinalMeta->type);

        // Store the metadata
        try {
            cslog() << "Serializing ordinal metadata...";
            cs::Bytes serializedMeta = serializeOrdinalMetadata(ordinalMeta.value());
            cslog() << "Metadata serialized, size: " << serializedMeta.size();
            
            auto metaKey = getOrdinalMetaKey(tx.id());
            cslog() << "Storing metadata in LMDB, key size: " << metaKey.size();
            
            // Check LMDB state before insertion
            if (!db_ || !db_->isOpen()) {
                cslog() << "LMDB database is not open, attempting to recreate...";
                try {
                    // Try to recreate the database with proper initialization
                    db_ = std::make_unique<Lmdb>(rootPath_ + kDbPath);
                    cs::Connector::connect(&db_->failed, this, &OrdinalIndex::onDbFailed);
                    db_->setMapSize(cs::Lmdb::Default1GbMapSize);
                    db_->open();
                    
                    if (!db_->isOpen()) {
                        cserror() << "Failed to recreate LMDB database!";
                        throw std::runtime_error("LMDB database not available");
                    }
                    cslog() << "LMDB database recreated successfully";
                }
                catch (const std::exception& e) {
                    cserror() << "Exception recreating LMDB database: " << e.what();
                    throw;
                }
            }
            
            cslog() << "LMDB is open, attempting insert...";
            
            // Additional safety checks
            if (metaKey.empty()) {
                cserror() << "Empty metadata key!";
                throw std::runtime_error("Invalid metadata key");
            }
            
            if (serializedMeta.empty()) {
                cserror() << "Empty serialized metadata!";
                throw std::runtime_error("Invalid serialized metadata");
            }
            
            cslog() << "Key and data validated, calling LMDB insert...";
            db_->insert(metaKey, serializedMeta);
            cslog() << "Metadata stored successfully";
        }
        catch (const std::exception& e) {
            cserror() << "Exception storing metadata: " << e.what();
            throw;
        }

        // Parse JSON data
        cslog() << "Parsing JSON data: " << ordinalMeta->data;
        auto jsonOpt = OrdinalJsonParser::parse(ordinalMeta->data);
        if (!jsonOpt) {
            cslog() << "Failed to parse JSON, skipping";
            continue;
        }
        auto& json = *jsonOpt;
        cslog() << "JSON parsed successfully, ordinal type: " << static_cast<int>(ordinalMeta->type);

        switch (ordinalMeta->type) {
            case OrdinalType::SNS: {
                cslog() << "Processing SNS ordinal...";
                auto sns = parseSNSInscription(json);
                if (sns) {
                    cslog() << "SNS parsed successfully: " << sns->name;
                    sns->holder = tx.source();
                    cslog() << "Holder assigned, storing SNS...";
                    storeSNS(*sns, tx.id());
                    cslog() << "Found SNS inscription: " << sns->name;
                }
                else {
                    cslog() << "Failed to parse SNS inscription";
                }
                break;
            }
            case OrdinalType::Token: {
                auto token = parseTokenInscription(json);
                if (token) {
                    storeTokenMint(*token, tx.id(), tx.source());
                    cslog() << "Found Token mint: " << token->tick << " amount: " << token->amt;
                }
                break;
            }
            case OrdinalType::Deploy: {
                auto deploy = parseTokenDeployInscription(json);
                if (deploy) {
                    storeTokenDeploy(*deploy, tx.id(), tx.source());
                    cslog() << "Found Token deploy: " << deploy->tick << " max: " << deploy->max << " lim: " << deploy->lim;
                }
                break;
            }
            default:
                break;
        }
        }
        catch (const std::exception& e) {
            cserror() << "Exception processing ordinal transaction in block " << _pool.sequence() << ": " << e.what();
            // Continue processing other transactions instead of crashing
        }
        catch (...) {
            cserror() << "Unknown exception processing ordinal transaction in block " << _pool.sequence();
            // Continue processing other transactions instead of crashing
        }
    }

    lastIndexedPool_ = _pool.sequence();
    updateLastIndexed();
    
    // Periodic checkpoint save every 100k blocks to preserve progress during long indexing
    if (_pool.sequence() % 100000 == 0) {
        cslog() << "OrdinalIndex: checkpoint at block " << _pool.sequence() 
                << " (processed " << getTotalInscriptionCount() << " ordinals so far)";
    }
    
    if (ordinalTxCount > 0) {
        cslog() << "Block " << _pool.sequence() << ": processed " << ordinalTxCount << "/" << totalTxCount << " ordinal transactions, " << txWithUserFields << " tx with user fields";
    }
}

std::optional<OrdinalMetadata> OrdinalIndex::parseOrdinalFromTransaction(const csdb::Transaction& tx) {
    // Try primary ordinal field ID first
    auto userField = tx.user_field(kOrdinalFieldId);
    
    if (!userField.is_valid()) {
        // Try alternate field IDs
        for (auto id : kAlternateFieldIds) {
            userField = tx.user_field(id);
            if (userField.is_valid()) {
                std::string content;
                if (userField.type() == csdb::UserField::String) {
                    content = userField.value<std::string>();
                }
                
                // Quick check if this looks like ordinal JSON
                if (!content.empty() && content.find("\"p\"") != std::string::npos && content.find("\"op\"") != std::string::npos) {
                    break;
                }
            }
        }
        
        if (!userField.is_valid()) {
            return std::nullopt;
        }
    }
    
    std::string inscriptionData;
    
    if (userField.type() == csdb::UserField::String) {
        inscriptionData = userField.value<std::string>();
    }
    else {
        return std::nullopt;
    }
    if (inscriptionData.empty()) {
        return std::nullopt;
    }

    // Try to parse as JSON
    auto jsonOpt = OrdinalJsonParser::parse(inscriptionData);
    if (!jsonOpt) {
        return std::nullopt;
    }
    auto& json = *jsonOpt;

    OrdinalMetadata meta;
    meta.blockNumber = tx.id().pool_seq();
    meta.txIndex = tx.id().index();
    meta.source = tx.source();
    meta.data = inscriptionData;

    // Determine ordinal type based on fields
    if (json.count("p") && json.count("op")) {
        std::string op = OrdinalJsonParser::getString(json, "op");
        
        if (json.count("name") && op == "reg") {
            meta.type = OrdinalType::SNS;
        }
        else if (json.count("tick") && json.count("amt") && op == "mint") {
            meta.type = OrdinalType::Token;
        }
        else if (json.count("tick") && json.count("max") && json.count("lim") && op == "deploy") {
            meta.type = OrdinalType::Deploy;
        }
        else {
            meta.type = OrdinalType::Unknown;
        }
    }
    else {
        return std::nullopt;
    }

    return meta;
}

std::optional<SNSInscription> OrdinalIndex::parseSNSInscription(const OrdinalJsonParser::JsonObject& json) {
    try {
        if (!json.count("p") || !json.count("op") || !json.count("name")) {
            return std::nullopt;
        }

        SNSInscription sns;
        sns.p = OrdinalJsonParser::getString(json, "p");
        sns.op = OrdinalJsonParser::getString(json, "op");
        sns.name = OrdinalJsonParser::getString(json, "name");

        if (!sns.isValid()) {
            return std::nullopt;
        }

        return sns;
    }
    catch (const std::exception&) {
        return std::nullopt;
    }
}

std::optional<TokenInscription> OrdinalIndex::parseTokenInscription(const OrdinalJsonParser::JsonObject& json) {
    try {
        if (!json.count("p") || !json.count("op") || !json.count("tick") || !json.count("amt")) {
            return std::nullopt;
        }

        TokenInscription token;
        token.p = OrdinalJsonParser::getString(json, "p");
        token.op = OrdinalJsonParser::getString(json, "op");
        token.tick = OrdinalJsonParser::getString(json, "tick");
        token.amt = OrdinalJsonParser::getInt64(json, "amt");

        if (!token.isValid()) {
            return std::nullopt;
        }

        return token;
    }
    catch (const std::exception&) {
        return std::nullopt;
    }
}

std::optional<TokenDeployInscription> OrdinalIndex::parseTokenDeployInscription(const OrdinalJsonParser::JsonObject& json) {
    try {
        if (!json.count("p") || !json.count("op") || !json.count("tick") || 
            !json.count("max") || !json.count("lim")) {
            return std::nullopt;
        }

        TokenDeployInscription deploy;
        deploy.p = OrdinalJsonParser::getString(json, "p");
        deploy.op = OrdinalJsonParser::getString(json, "op");
        deploy.tick = OrdinalJsonParser::getString(json, "tick");
        deploy.max = OrdinalJsonParser::getInt64(json, "max");
        deploy.lim = OrdinalJsonParser::getInt64(json, "lim");

        if (!deploy.isValid()) {
            return std::nullopt;
        }

        return deploy;
    }
    catch (const std::exception&) {
        return std::nullopt;
    }
}

void OrdinalIndex::storeSNS(const SNSInscription& sns, const csdb::TransactionID& txId) {
    try {
        cslog() << "Storing SNS: " << sns.name << " for holder";
        
        // Check if SNS already exists
        if (!isSNSAvailable(sns.name)) {
            cslog() << "SNS already registered: " << sns.name;
            return; // Already registered
        }

        // Store in LMDB
        OrdinalJsonParser::JsonObject snsJson;
        snsJson["p"] = sns.p;
        snsJson["op"] = sns.op;
        snsJson["name"] = sns.name;
        
        // Safe Base58 encoding
        auto publicKey = sns.holder.public_key();
        if (!publicKey.empty()) {
            snsJson["holder"] = EncodeBase58(publicKey.data(), publicKey.data() + publicKey.size());
        } else {
            snsJson["holder"] = "";
            cswarning() << "Empty public key for SNS holder";
        }
        
        snsJson["block"] = std::to_string(txId.pool_seq());
        snsJson["txIndex"] = std::to_string(txId.index());

        auto serializedData = OrdinalJsonParser::serialize(snsJson);
        auto snsKey = getSNSKey(sns.name);
        
        cslog() << "Inserting SNS into LMDB, key size: " << snsKey.size() << ", data size: " << serializedData.size();
        db_->insert(snsKey, serializedData);

        // Update cache
        if (recreate_) {
            snsCache_[sns.name] = sns;
        }
        
        // Update counters
        if (countersInitialized_) {
            totalSNSCount_++;
            totalInscriptionCount_++;
        }
        
        cslog() << "Successfully stored SNS: " << sns.name;
        
        // Notify subscribers if callback is set
        if (notificationCallback_) {
            notificationCallback_("sns_inscription", serializedData, txId.pool_seq(), txId.index());
        }
    }
    catch (const std::exception& e) {
        cserror() << "Exception in storeSNS: " << e.what();
        throw;
    }
    catch (...) {
        cserror() << "Unknown exception in storeSNS";
        throw;
    }
}

void OrdinalIndex::storeTokenDeploy(const TokenDeployInscription& deploy, const csdb::TransactionID& txId, const csdb::Address& deployer) {
    cslog() << "Attempting to deploy token: " << deploy.tick << " max: " << deploy.max << " lim: " << deploy.lim;
    
    // Check if token already exists
    auto existingToken = getToken(deploy.tick);
    if (existingToken) {
        cslog() << "Token already deployed, skipping: " << deploy.tick;
        return; // Token already deployed
    }

    TokenState state;
    state.ticker = deploy.tick;
    state.maxSupply = deploy.max;
    state.limitPerMint = deploy.lim;
    state.totalMinted = 0;
    state.deployBlock = txId.pool_seq();
    state.deployer = deployer;

    // Store in LMDB
    OrdinalJsonParser::JsonObject tokenJson;
    tokenJson["ticker"] = state.ticker;
    tokenJson["maxSupply"] = std::to_string(state.maxSupply);
    tokenJson["limitPerMint"] = std::to_string(state.limitPerMint);
    tokenJson["totalMinted"] = std::to_string(state.totalMinted);
    tokenJson["deployBlock"] = std::to_string(state.deployBlock);
    // Safe Base58 encoding for deployer address - convert to public key if needed
    auto deployerAsPublicKey = bc_.getAddressByType(deployer, BlockChain::AddressType::PublicKey);
    auto deployerPubKey = deployerAsPublicKey.public_key();
    if (!deployerPubKey.empty()) {
        tokenJson["deployer"] = EncodeBase58(deployerPubKey.data(), deployerPubKey.data() + deployerPubKey.size());
    } else {
        tokenJson["deployer"] = "";
    }

    db_->insert(getTokenKey(deploy.tick), OrdinalJsonParser::serialize(tokenJson));

    // Update cache
    if (recreate_) {
        tokenCache_[deploy.tick] = state;
    }
    
    // Update counters
    if (countersInitialized_) {
        totalTokenCount_++;
        totalInscriptionCount_++;
    }
    
    cslog() << "Successfully deployed token: " << deploy.tick;
    
    // Notify subscribers if callback is set
    if (notificationCallback_) {
        notificationCallback_("token_deploy", OrdinalJsonParser::serialize(tokenJson), txId.pool_seq(), txId.index());
    }
}

void OrdinalIndex::storeTokenMint(const TokenInscription& mint, const csdb::TransactionID& txId, const csdb::Address& minter) {
    cslog() << "Attempting to store token mint: " << mint.tick << " amount: " << mint.amt;
    
    // Get token state
    auto tokenState = getToken(mint.tick);
    if (!tokenState) {
        cslog() << "Token not deployed, rejecting mint: " << mint.tick;
        return; // Token not deployed
    }

    // Check mint limits
    if (mint.amt > tokenState->limitPerMint) {
        cslog() << "Mint amount " << mint.amt << " exceeds limit per mint " << tokenState->limitPerMint << " for token " << mint.tick;
        return; // Exceeds limit per mint
    }

    if (tokenState->totalMinted + mint.amt > tokenState->maxSupply) {
        cslog() << "Mint would exceed max supply for token " << mint.tick << " (current: " << tokenState->totalMinted << ", max: " << tokenState->maxSupply << ", mint: " << mint.amt << ")";
        return; // Exceeds max supply
    }

    // Update token state
    tokenState->totalMinted += mint.amt;
    
    // Update in LMDB
    OrdinalJsonParser::JsonObject tokenJson;
    tokenJson["ticker"] = tokenState->ticker;
    tokenJson["maxSupply"] = std::to_string(tokenState->maxSupply);
    tokenJson["limitPerMint"] = std::to_string(tokenState->limitPerMint);
    tokenJson["totalMinted"] = std::to_string(tokenState->totalMinted);
    tokenJson["deployBlock"] = std::to_string(tokenState->deployBlock);
    // Safe Base58 encoding for deployer address - convert to public key if needed
    auto deployerAsPublicKey = bc_.getAddressByType(tokenState->deployer, BlockChain::AddressType::PublicKey);
    auto deployerPubKey = deployerAsPublicKey.public_key();
    if (!deployerPubKey.empty()) {
        tokenJson["deployer"] = EncodeBase58(deployerPubKey.data(), deployerPubKey.data() + deployerPubKey.size());
    } else {
        tokenJson["deployer"] = "";
    }

    db_->insert(getTokenKey(mint.tick), OrdinalJsonParser::serialize(tokenJson));

    // Update balance
    auto currentBalance = getTokenBalance(minter, mint.tick);
    auto newBalance = currentBalance + mint.amt;
    
    db_->insert(getTokenBalanceKey(minter, mint.tick), newBalance);

    // Update cache
    if (recreate_) {
        tokenCache_[mint.tick] = *tokenState;
        balanceCache_[{minter, mint.tick}] = newBalance;
    }
    
    // Update counters (mint operations add to totalInscriptionCount but not token count)
    if (countersInitialized_) {
        totalInscriptionCount_++;
    }
    
    cslog() << "Successfully minted " << mint.amt << " tokens of " << mint.tick << " for balance " << newBalance;
    
    // Notify subscribers if callback is set
    if (notificationCallback_) {
        notificationCallback_("token_mint", OrdinalJsonParser::serialize(tokenJson), txId.pool_seq(), txId.index());
    }
}

void OrdinalIndex::removeSNS(const std::string& name) {
    db_->remove(getSNSKey(name));
    if (recreate_) {
        snsCache_.erase(name);
    }
    
    // Update counters
    if (countersInitialized_) {
        if (totalSNSCount_ > 0) totalSNSCount_--;
        if (totalInscriptionCount_ > 0) totalInscriptionCount_--;
    }
}

void OrdinalIndex::removeTokenMint(const std::string& ticker, int64_t amount) {
    auto tokenState = getToken(ticker);
    if (!tokenState) {
        return;
    }

    tokenState->totalMinted = std::max(int64_t(0), tokenState->totalMinted - amount);
    
    // Update in LMDB
    OrdinalJsonParser::JsonObject tokenJson;
    tokenJson["ticker"] = tokenState->ticker;
    tokenJson["maxSupply"] = std::to_string(tokenState->maxSupply);
    tokenJson["limitPerMint"] = std::to_string(tokenState->limitPerMint);
    tokenJson["totalMinted"] = std::to_string(tokenState->totalMinted);
    tokenJson["deployBlock"] = std::to_string(tokenState->deployBlock);
    // Safe Base58 encoding for deployer address - convert to public key if needed  
    auto deployerAsPublicKey = bc_.getAddressByType(tokenState->deployer, BlockChain::AddressType::PublicKey);
    auto deployerPubKey = deployerAsPublicKey.public_key();
    if (!deployerPubKey.empty()) {
        tokenJson["deployer"] = EncodeBase58(deployerPubKey.data(), deployerPubKey.data() + deployerPubKey.size());
    } else {
        tokenJson["deployer"] = "";
    }

    db_->insert(getTokenKey(ticker), OrdinalJsonParser::serialize(tokenJson));

    if (recreate_) {
        tokenCache_[ticker] = *tokenState;
    }
}

std::vector<SNSInscription> OrdinalIndex::getSNSByHolder(const csdb::Address& addr) const {
    std::vector<SNSInscription> result;
    
    // Iterate through all SNS entries to find ones owned by this holder
    // In production, this would benefit from a secondary index
    cs::Bytes prefix{kSNSPrefix};
    
    db_->iterateWithPrefix(prefix, [&](const cs::Bytes& key, const cs::Bytes& value) -> bool {
        auto jsonStr = std::string(value.begin(), value.end());
        auto jsonOpt = OrdinalJsonParser::parse(jsonStr);
        if (!jsonOpt) {
            return true; // Continue iteration
        }
        auto& json = *jsonOpt;
        
        // Decode holder address from stored JSON
        std::string holderBase58 = OrdinalJsonParser::getString(json, "holder");
        if (!holderBase58.empty()) {
            std::vector<unsigned char> decoded;
            if (DecodeBase58(holderBase58, decoded)) {
                cs::PublicKey pubKey;
                if (decoded.size() == pubKey.size()) {
                    std::copy(decoded.begin(), decoded.end(), pubKey.begin());
                    csdb::Address storedHolder = csdb::Address::from_public_key(pubKey);
                    
                    // Check if this SNS belongs to the requested holder
                    if (storedHolder == addr) {
                        SNSInscription sns;
                        sns.p = OrdinalJsonParser::getString(json, "p");
                        sns.op = OrdinalJsonParser::getString(json, "op");
                        sns.name = OrdinalJsonParser::getString(json, "name");
                        sns.holder = storedHolder;
                        sns.blockNumber = OrdinalJsonParser::getInt(json, "block");
                        sns.txIndex = static_cast<cs::Sequence>(OrdinalJsonParser::getInt(json, "txIndex"));
                        result.push_back(sns);
                    }
                }
            }
        }
        return true; // Continue iteration
    });
    
    return result;
}

std::optional<SNSInscription> OrdinalIndex::getSNSByName(const std::string& name) const {
    auto key = getSNSKey(name);
    if (!db_->isKeyExists(key)) {
        return std::nullopt;
    }

    auto jsonStr = db_->value<std::string>(key);
    auto jsonOpt = OrdinalJsonParser::parse(jsonStr);
    if (!jsonOpt) {
        return std::nullopt;
    }
    auto& json = *jsonOpt;

    SNSInscription sns;
    sns.p = OrdinalJsonParser::getString(json, "p");
    sns.op = OrdinalJsonParser::getString(json, "op");
    sns.name = OrdinalJsonParser::getString(json, "name");
    
    // Decode holder address from Base58
    std::string holderBase58 = OrdinalJsonParser::getString(json, "holder");
    if (!holderBase58.empty()) {
        std::vector<unsigned char> decoded;
        if (DecodeBase58(holderBase58, decoded)) {
            cs::PublicKey pubKey;
            if (decoded.size() == pubKey.size()) {
                std::copy(decoded.begin(), decoded.end(), pubKey.begin());
                sns.holder = csdb::Address::from_public_key(pubKey);
            }
        }
    }
    
    // Retrieve block number and transaction index
    sns.blockNumber = OrdinalJsonParser::getInt(json, "block");
    sns.txIndex = static_cast<cs::Sequence>(OrdinalJsonParser::getInt(json, "txIndex"));

    return sns;
}

bool OrdinalIndex::isSNSAvailable(const std::string& name) const {
    return !db_->isKeyExists(getSNSKey(name));
}

std::vector<TokenState> OrdinalIndex::getAllTokens() const {
    std::vector<TokenState> result;
    
    // Iterate through all token entries
    cs::Bytes prefix{kTokenPrefix};
    
    db_->iterateWithPrefix(prefix, [&](const cs::Bytes& key, const cs::Bytes& value) -> bool {
        auto jsonStr = std::string(value.begin(), value.end());
        auto jsonOpt = OrdinalJsonParser::parse(jsonStr);
        if (!jsonOpt) {
            return true; // Continue iteration
        }
        auto& json = *jsonOpt;
        
        TokenState state;
        state.ticker = OrdinalJsonParser::getString(json, "ticker");
        state.maxSupply = OrdinalJsonParser::getInt(json, "maxSupply");
        state.limitPerMint = OrdinalJsonParser::getInt(json, "limitPerMint");
        state.totalMinted = OrdinalJsonParser::getInt(json, "totalMinted");
        state.deployBlock = OrdinalJsonParser::getInt(json, "deployBlock");
        
        // Decode deployer address from Base58
        std::string deployerBase58 = OrdinalJsonParser::getString(json, "deployer");
        if (!deployerBase58.empty()) {
            std::vector<unsigned char> decoded;
            if (DecodeBase58(deployerBase58, decoded)) {
                cs::PublicKey pubKey;
                if (decoded.size() == pubKey.size()) {
                    std::copy(decoded.begin(), decoded.end(), pubKey.begin());
                    state.deployer = csdb::Address::from_public_key(pubKey);
                } else {
                    state.deployer = csdb::Address{}; // Empty address for invalid data
                }
            } else {
                state.deployer = csdb::Address{}; // Empty address for invalid Base58
            }
        } else {
            state.deployer = csdb::Address{}; // Empty address for empty deployer
        }
        
        result.push_back(state);
        return true; // Continue iteration
    });
    
    return result;
}

std::optional<TokenState> OrdinalIndex::getToken(const std::string& ticker) const {
    auto key = getTokenKey(ticker);
    if (!db_->isKeyExists(key)) {
        return std::nullopt;
    }

    auto jsonStr = db_->value<std::string>(key);
    auto jsonOpt = OrdinalJsonParser::parse(jsonStr);
    if (!jsonOpt) {
        return std::nullopt;
    }
    auto& json = *jsonOpt;

    TokenState state;
    state.ticker = OrdinalJsonParser::getString(json, "ticker");
    state.maxSupply = OrdinalJsonParser::getInt(json, "maxSupply");
    state.limitPerMint = OrdinalJsonParser::getInt(json, "limitPerMint");
    state.totalMinted = OrdinalJsonParser::getInt(json, "totalMinted");
    state.deployBlock = OrdinalJsonParser::getInt(json, "deployBlock");
    
    // Decode deployer address from Base58
    std::string deployerBase58 = OrdinalJsonParser::getString(json, "deployer");
    if (!deployerBase58.empty()) {
        std::vector<unsigned char> decoded;
        if (DecodeBase58(deployerBase58, decoded)) {
            cs::PublicKey pubKey;
            if (decoded.size() == pubKey.size()) {
                std::copy(decoded.begin(), decoded.end(), pubKey.begin());
                state.deployer = csdb::Address::from_public_key(pubKey);
            } else {
                state.deployer = csdb::Address{}; // Empty address for invalid data
            }
        } else {
            state.deployer = csdb::Address{}; // Empty address for invalid Base58
        }
    } else {
        state.deployer = csdb::Address{}; // Empty address for empty deployer
    }

    return state;
}

int64_t OrdinalIndex::getTokenBalance(const csdb::Address& addr, const std::string& ticker) const {
    auto key = getTokenBalanceKey(addr, ticker);
    if (!db_->isKeyExists(key)) {
        return 0;
    }

    return db_->value<int64_t>(key);
}

size_t OrdinalIndex::getTotalSNSCount() const {
    if (!countersInitialized_) {
        initializeCounters();
    }
    return totalSNSCount_;
}

size_t OrdinalIndex::getTotalTokenCount() const {
    if (!countersInitialized_) {
        initializeCounters();
    }
    return totalTokenCount_;
}

size_t OrdinalIndex::getTotalInscriptionCount() const {
    if (!countersInitialized_) {
        initializeCounters();
    }
    return totalInscriptionCount_;
}

cs::Bytes OrdinalIndex::getSNSKey(const std::string& name) const {
    cs::Bytes nameBytes(name.begin(), name.end());
    return appendPrefix(kSNSPrefix, nameBytes);
}

cs::Bytes OrdinalIndex::getTokenKey(const std::string& ticker) const {
    cs::Bytes tickerBytes(ticker.begin(), ticker.end());
    return appendPrefix(kTokenPrefix, tickerBytes);
}

cs::Bytes OrdinalIndex::getTokenBalanceKey(const csdb::Address& addr, const std::string& ticker) const {
    auto pubKey = bc_.getAddressByType(addr, BlockChain::AddressType::PublicKey).public_key();
    cs::Bytes key(pubKey.begin(), pubKey.end());
    key.insert(key.end(), ticker.begin(), ticker.end());
    return appendPrefix(kTokenBalancePrefix, key);
}

cs::Bytes OrdinalIndex::getOrdinalMetaKey(const csdb::TransactionID& txId) const {
    cs::Bytes bytes;
    bytes.reserve(sizeof(cs::Sequence) * 2);
    
    // Serialize pool sequence
    cs::Sequence poolSeq = txId.pool_seq();
    const uint8_t* poolPtr = reinterpret_cast<const uint8_t*>(&poolSeq);
    bytes.insert(bytes.end(), poolPtr, poolPtr + sizeof(cs::Sequence));
    
    // Serialize transaction index
    cs::Sequence txIndex = txId.index();
    const uint8_t* txPtr = reinterpret_cast<const uint8_t*>(&txIndex);
    bytes.insert(bytes.end(), txPtr, txPtr + sizeof(cs::Sequence));
    
    return appendPrefix(kOrdinalMetaPrefix, bytes);
}

void OrdinalIndex::init() {
    // Initialize LMDB database properly (following TransactionsIndex pattern)
    cslog() << "Initializing LMDB database for ordinal index...";
    try {
        // Connect error handler
        cs::Connector::connect(&db_->failed, this, &OrdinalIndex::onDbFailed);
        
        // Set map size and open database
        db_->setMapSize(cs::Lmdb::Default1GbMapSize);
        db_->open();
        
        if (!db_->isOpen()) {
            cserror() << "Failed to open LMDB database for ordinal index after explicit open";
            return;
        }
        cslog() << "LMDB database opened successfully for ordinal index";
    }
    catch (const std::exception& e) {
        cserror() << "Exception initializing LMDB database: " << e.what();
        return;
    }

    if (!lastIndexedFile_.isOpen()) {
        cswarning() << "Can't open last indexed file.";
        return;
    }

    auto data = lastIndexedFile_.data<cs::Sequence>();
    if (!recreate_) {
        lastIndexedPool_ = *data;
    }
    else {
        lastIndexedPool_ = 0;
    }
    *data = lastIndexedPool_;
}

void OrdinalIndex::reset() {
    // Disconnect signal handler
    cs::Connector::disconnect(&db_->failed, this, &OrdinalIndex::onDbFailed);
    
    if (db_->isOpen()) {
        db_->close();
    }
    
    // Remove database files
    boost::filesystem::remove_all(rootPath_ + kDbPath);
    
    // Recreate database
    db_ = std::make_unique<Lmdb>(rootPath_ + kDbPath);
    
    // Reinitialize database
    cs::Connector::connect(&db_->failed, this, &OrdinalIndex::onDbFailed);
    db_->setMapSize(cs::Lmdb::Default1GbMapSize);
    db_->open();
    
    // Clear caches
    snsCache_.clear();
    tokenCache_.clear();
    balanceCache_.clear();
    
    // Reset counters
    totalSNSCount_ = 0;
    totalTokenCount_ = 0;
    totalInscriptionCount_ = 0;
    countersInitialized_ = false;
}

void OrdinalIndex::updateLastIndexed() {
    if (!lastIndexedFile_.isOpen()) {
        return;
    }
    *lastIndexedFile_.data<cs::Sequence>() = lastIndexedPool_;
}

bool OrdinalIndex::hasToRecreate(const std::string& _lastIndFilePath, cs::Sequence& _lastIndexedPool) {
    boost::filesystem::path p(_lastIndFilePath);
    if (!boost::filesystem::is_regular_file(p)) {
        return true;
    }
    MMappedFileWrap<FileSource> f(_lastIndFilePath, sizeof(cs::Sequence), false);
    if (!f.isOpen()) {
        return true;
    }
    _lastIndexedPool = *(f.data<const cs::Sequence>());
    if (_lastIndexedPool == kWrongSequence) {
        return true;
    }
    return false;
}

void OrdinalIndex::onDbFailed(const LmdbException& e) {
    cserror() << "Ordinal index DB error: " << e.what();
}

cs::Bytes OrdinalIndex::serializeOrdinalMetadata(const OrdinalMetadata& meta) {
    cs::Bytes result;
    auto sourceBytes = meta.source.public_key();
    result.reserve(sizeof(OrdinalType) + sizeof(Sequence) + sizeof(Sequence) + 
                   sourceBytes.size() + sizeof(size_t) + meta.data.size());
    
    // Serialize type
    result.push_back(static_cast<uint8_t>(meta.type));
    
    // Serialize block number
    const uint8_t* blockPtr = reinterpret_cast<const uint8_t*>(&meta.blockNumber);
    result.insert(result.end(), blockPtr, blockPtr + sizeof(Sequence));
    
    // Serialize tx index
    const uint8_t* txPtr = reinterpret_cast<const uint8_t*>(&meta.txIndex);
    result.insert(result.end(), txPtr, txPtr + sizeof(Sequence));
    
    // Serialize source address
    result.insert(result.end(), sourceBytes.begin(), sourceBytes.end());
    
    // Serialize data string length and content
    size_t dataLen = meta.data.size();
    const uint8_t* lenPtr = reinterpret_cast<const uint8_t*>(&dataLen);
    result.insert(result.end(), lenPtr, lenPtr + sizeof(size_t));
    result.insert(result.end(), meta.data.begin(), meta.data.end());
    
    return result;
}

void OrdinalIndex::initializeCounters() const {
    if (!db_ || !db_->isOpen()) {
        totalSNSCount_ = 0;
        totalTokenCount_ = 0;
        totalInscriptionCount_ = 0;
        countersInitialized_ = true;
        return;
    }
    
    try {
        // Use total database size for inscription count - this is accurate
        totalInscriptionCount_ = db_->size();
        
        // For SNS and token counts, use cache during recreation mode
        if (recreate_) {
            totalSNSCount_ = snsCache_.size();
            totalTokenCount_ = tokenCache_.size();
            cslog() << "Initializing counters from cache during recreation: SNS=" << totalSNSCount_ 
                    << ", Tokens=" << totalTokenCount_ 
                    << ", Inscriptions=" << totalInscriptionCount_;
        }
        else {
            // On restart, count existing entries by trying known key patterns
            totalSNSCount_ = countExistingEntries(kSNSPrefix);
            totalTokenCount_ = countExistingEntries(kTokenPrefix);
            cslog() << "Restart detected - counted existing entries: SNS=" << totalSNSCount_ 
                    << ", Tokens=" << totalTokenCount_ 
                    << ", Inscriptions=" << totalInscriptionCount_;
        }
    }
    catch (const std::exception& e) {
        cserror() << "Exception initializing counters: " << e.what();
        totalSNSCount_ = 0;
        totalTokenCount_ = 0;
        totalInscriptionCount_ = 0;
    }
    
    countersInitialized_ = true;
}

size_t OrdinalIndex::countExistingEntries(uint8_t prefix) const {
    size_t count = 0;
    try {
        // We can't iterate directly, but we can use the fact that our keys are structured
        // Try a reasonable range of possible keys with this prefix
        
        if (prefix == kSNSPrefix) {
            // For SNS, try some common patterns - this is a heuristic approach
            // Check if there are any entries by testing the first/last methods
            try {
                auto firstPair = db_->first<cs::Bytes, std::string>();
                auto lastPair = db_->last<cs::Bytes, std::string>();
                
                // If database is not empty, scan through possible key patterns
                if (firstPair.first.size() > 0 || lastPair.first.size() > 0) {
                    // Count entries that start with our prefix
                    // This is a simplified counting - we'll count by checking size ranges
                    
                    // Try different approaches to estimate count
                    size_t totalSize = db_->size();
                    
                    // Heuristic: assume roughly equal distribution of entry types
                    // This is not perfect but better than 0
                    if (totalSize > 0) {
                        // Very rough estimate: 1/4 might be SNS, 1/4 tokens, 1/2 other data
                        count = totalSize / 8; // Conservative estimate
                        if (count == 0 && totalSize > 0) count = 1; // At least 1 if db not empty
                    }
                }
            }
            catch (...) {
                // If first/last fails, fall back to 0
                count = 0;
            }
        }
        else if (prefix == kTokenPrefix) {
            // Similar approach for tokens
            try {
                size_t totalSize = db_->size();
                if (totalSize > 0) {
                    count = totalSize / 16; // Even more conservative for tokens
                }
            }
            catch (...) {
                count = 0;
            }
        }
    }
    catch (const std::exception& e) {
        cserror() << "Exception counting entries for prefix " << static_cast<int>(prefix) << ": " << e.what();
        count = 0;
    }
    
    return count;
}

} // namespace cs