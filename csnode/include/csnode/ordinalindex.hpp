#ifndef ORDINALINDEX_HPP
#define ORDINALINDEX_HPP

#include <map>
#include <memory>
#include <string>
#include <vector>
#include <set>
#include <algorithm>
#include <cctype>

#include <csdb/address.hpp>
#include <csdb/transaction.hpp>
#include <lib/system/common.hpp>
#include <lib/system/mmappedfile.hpp>
#include <lmdb.hpp>
#include <csnode/ordinal_json_parser.hpp>

class BlockChain;

namespace csdb {
class Pool;
} // namespace csdb

namespace cs {

// Ordinal inscription types
enum class OrdinalType : uint8_t {
    Unknown = 0,
    CNS = 1,      // Credits Name System (implements CONP spec)
    Token = 2,    // Token operations
    Deploy = 3,   // Token deployment
};

// CNS (Credits Name System) inscription - implements CONP specification
struct CNSInscription {
    std::string p;      // namespace: "cdns" or "cns"
    std::string op;     // operation: "reg", "upd", "trf"
    std::string cns;    // name (UTF-8, case-insensitive, no spaces)
    std::string relay;  // optional relay data (wallet address, IPFS CID, URL, etc.)
    csdb::Address owner; // current owner address (holder)
    Sequence blockNumber = 0;  // block number where name was registered
    cs::Sequence txIndex = 0;  // transaction index in the block
    
    bool isValid() const {
        // Validate namespace
        if (p != "cdns" && p != "cns") return false;
        
        // Validate operation
        if (op != "reg" && op != "upd" && op != "trf") return false;
        
        // Name must not be empty
        if (cns.empty()) return false;
        
        return true;
    }
    
    // Helper to normalize name for comparison (lowercase)
    std::string normalizedName() const {
        std::string normalized = cns;
        std::transform(normalized.begin(), normalized.end(), normalized.begin(), ::tolower);
        return normalized;
    }
    
    // Helper to normalize namespace for comparison (lowercase)
    std::string normalizedNamespace() const {
        std::string normalized = p;
        std::transform(normalized.begin(), normalized.end(), normalized.begin(), ::tolower);
        return normalized;
    }
};

// Token inscription for minting
struct TokenInscription {
    std::string p;      // protocol
    std::string op;     // operation
    std::string tick;   // ticker symbol
    int64_t amt;        // amount to mint
    
    bool isValid() const {
        return !p.empty() && !op.empty() && !tick.empty() && amt > 0;
    }
};

// Token deployment inscription
struct TokenDeployInscription {
    std::string p;      // protocol
    std::string op;     // operation
    std::string tick;   // ticker symbol
    int64_t max;        // max supply
    int64_t lim;        // limit per mint
    
    bool isValid() const {
        return !p.empty() && !op.empty() && !tick.empty() && max > 0 && lim > 0;
    }
};

// Token state tracking
struct TokenState {
    std::string ticker;
    int64_t maxSupply;
    int64_t limitPerMint;
    int64_t totalMinted;
    Sequence deployBlock;
    csdb::Address deployer;
};

// Ordinal metadata stored in LMDB
struct OrdinalMetadata {
    OrdinalType type;
    Sequence blockNumber;
    cs::Sequence txIndex;
    csdb::Address source;
    std::string data; // JSON serialized inscription data
};

class OrdinalIndex {
public:
    // Callback function types for notifications
    using OrdinalNotificationCallback = std::function<void(const std::string& type, const std::string& data, Sequence blockNumber, cs::Sequence txIndex)>;
    
    OrdinalIndex(BlockChain&, const std::string& _path, bool _recreate = false);
    ~OrdinalIndex() = default;
    
    // Set callback for ordinal notifications
    void setNotificationCallback(OrdinalNotificationCallback callback) {
        notificationCallback_ = callback;
    }

    void update(const csdb::Pool&);
    void invalidate();
    void close();

    bool recreate() const;

    // Query methods for CNS names
    std::vector<CNSInscription> getCNSByOwner(const csdb::Address& addr) const;
    std::optional<CNSInscription> getCNSByName(const std::string& namespace_, const std::string& name) const;
    bool isCNSNameAvailable(const std::string& namespace_, const std::string& name) const;
    
    std::vector<TokenState> getAllTokens() const;
    std::optional<TokenState> getToken(const std::string& ticker) const;
    int64_t getTokenBalance(const csdb::Address& addr, const std::string& ticker) const;
    
    // Statistics
    size_t getTotalCNSCount() const;
    size_t getTotalTokenCount() const;
    size_t getTotalInscriptionCount() const;

public slots:
    void onStartReadFromDb(Sequence _lastWrittenPoolSeq);
    void onReadFromDb(const csdb::Pool&);
    void onDbReadFinished();
    void onRemoveBlock(const csdb::Pool&);

private slots:
    void onDbFailed(const LmdbException&);

private:
    void init();
    void reset();

    void updateFromNextBlock(const csdb::Pool&);
    void updateLastIndexed();

    static bool hasToRecreate(const std::string&, cs::Sequence&);
    
    // Ordinal parsing and validation
    std::optional<OrdinalMetadata> parseOrdinalFromTransaction(const csdb::Transaction& tx);
    std::optional<CNSInscription> parseCNSInscription(const OrdinalJsonParser::JsonObject& json);
    std::optional<TokenInscription> parseTokenInscription(const OrdinalJsonParser::JsonObject& json);
    std::optional<TokenDeployInscription> parseTokenDeployInscription(const OrdinalJsonParser::JsonObject& json);
    
    // Storage operations
    void storeCNS(const CNSInscription& cns, const csdb::TransactionID& txId, const csdb::Address& sender);
    void updateCNS(const std::string& namespace_, const std::string& name, const std::string& relay, const csdb::TransactionID& txId, const csdb::Address& sender);
    void transferCNS(const std::string& namespace_, const std::string& name, const csdb::Address& newOwner, const csdb::TransactionID& txId, const csdb::Address& sender);
    void storeTokenDeploy(const TokenDeployInscription& deploy, const csdb::TransactionID& txId, const csdb::Address& deployer);
    void storeTokenMint(const TokenInscription& mint, const csdb::TransactionID& txId, const csdb::Address& minter);
    
    // Index removal operations (for reorg handling)
    void removeCNS(const std::string& namespace_, const std::string& name);
    void removeTokenMint(const std::string& ticker, int64_t amount);
    
    // Helper methods
    cs::Bytes getCNSKey(const std::string& namespace_, const std::string& name) const;
    cs::Bytes getTokenKey(const std::string& ticker) const;
    cs::Bytes getTokenBalanceKey(const csdb::Address& addr, const std::string& ticker) const;
    cs::Bytes getOrdinalMetaKey(const csdb::TransactionID& txId) const;
    
    // CNS validation helpers
    bool isValidCNSName(const std::string& name) const;
    bool isValidUTF8(const std::string& str) const;
    
    // Serialization methods
    cs::Bytes serializeOrdinalMetadata(const OrdinalMetadata& meta);
    
    // Counter initialization
    void initializeCounters() const;
    size_t countExistingEntries(uint8_t prefix) const;

    BlockChain& bc_;
    const std::string rootPath_;
    std::unique_ptr<Lmdb> db_;
    Sequence lastIndexedPool_ = 0;
    bool recreate_;
    MMappedFileWrap<FileSink> lastIndexedFile_;
    
    // In-memory caches for fast lookup during indexing
    std::map<std::pair<std::string, std::string>, CNSInscription> cnsCache_;  // Key: (namespace, name)
    std::map<std::string, TokenState> tokenCache_;
    std::map<std::pair<csdb::Address, std::string>, int64_t> balanceCache_;
    
    // Statistics counters
    mutable size_t totalCNSCount_ = 0;
    mutable size_t totalTokenCount_ = 0;
    mutable size_t totalInscriptionCount_ = 0;
    mutable bool countersInitialized_ = false;
    
    // Notification callback
    OrdinalNotificationCallback notificationCallback_;
};

} // namespace cs

#endif // ORDINALINDEX_HPP