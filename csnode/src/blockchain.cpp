#define CS_LOG_CHANNEL "blockchain"
#include <chrono>
#include <cstdlib>
#include <iomanip>
#include <sstream>
#include <string_view>
#include <base58.h>
#include <csdb/currency.hpp>
#include <lib/system/hash.hpp>
#include <lib/system/logger.hpp>
#include <lib/system/utils.hpp>
#include <limits>

#ifdef DBSQL
#include <dbsql/roundinfo.hpp>
#endif
#include <csnode/blockchain.hpp>
#include <csnode/chain_integrity.hpp>
#include <csnode/blockhashes.hpp>
#include <csnode/conveyer.hpp>
#include <csnode/datastream.hpp>
#include <csnode/fee.hpp>
#include <csnode/nodeutils.hpp>
#include <csnode/node.hpp>
#include <csnode/transactionsindex.hpp>
#include <csnode/transactionsiterator.hpp>
#include <csnode/configholder.hpp>
#include <csnode/confirmationlist.hpp>
#include <solver/consensus.hpp>
#include <solver/smartcontracts.hpp>

#include <boost/filesystem.hpp>


using namespace cs;
namespace fs = boost::filesystem;

namespace {
const char* cachesPath = "./caches";
const char* kLogPrefix = "BLOCKCHAIN: ";
} // namespace

BlockChain::BlockChain(csdb::Address genesisAddress, csdb::Address startAddress, bool recreateIndex)
: good_(false)
, dbLock_()
, genesisAddress_(genesisAddress)
, startAddress_(startAddress)
, walletIds_(new WalletsIds)
, walletsCacheStorage_(new WalletsCache(*walletIds_))
, cacheMutex_() {
    createCachesPath();
    walletsCacheUpdater_ = walletsCacheStorage_->createUpdater();
    blockHashes_ = std::make_unique<cs::BlockHashes>(cachesPath);
    cachedBlocks_ = std::make_unique<cs::PoolCache>(cachesPath);
    trxIndex_ = std::make_unique<cs::TransactionsIndex>(*this, cachesPath, recreateIndex);
}

BlockChain::~BlockChain() {}

void BlockChain::subscribeToSignals() {
    // the order of two following calls matters
    cs::Connector::connect(&storage_.readBlockEvent(), trxIndex_.get(), &TransactionsIndex::onReadFromDb);
    cs::Connector::connect(&storage_.readBlockEvent(), this, &BlockChain::onReadFromDB);
    cs::Connector::connect(&storage_.readingStartedEvent(), trxIndex_.get(), &TransactionsIndex::onStartReadFromDb);
    cs::Connector::connect(&storage_.readingStartedEvent(), this, &BlockChain::onStartReadFromDB);

    cs::Connector::connect(&storage_.readingStoppedEvent(), trxIndex_.get(), &TransactionsIndex::onDbReadFinished);
}

bool BlockChain::bindSerializationManToCaches(
    cs::CachesSerializationManager* serializationManPtr,
    std::set<cs::PublicKey>& initialConfidants
) {
    if (!serializationManPtr) {
        cserror() << "NO SERIALIZATION MANAGER PROVIDED!";
        return false;
    }

    serializationManPtr_ = serializationManPtr;

    serializationManPtr_->bind(*this, initialConfidants);
    serializationManPtr_->bind(*walletsCacheStorage_);
    serializationManPtr_->bind(*walletIds_);

    return true;
}

bool BlockChain::tryQuickStart(
    cs::CachesSerializationManager* serializationManPtr,
    std::set<cs::PublicKey>& initialConfidants,
    const std::string& dbPath,
    const std::string& dbBackend
) {
    cslog() << "Try QUICK START...";
    if (!bindSerializationManToCaches(serializationManPtr, initialConfidants)) {
        return false;
    }

    std::set<cs::PublicKey> reserveConf;
    for (auto it : initialConfidants) {
        reserveConf.insert(it);
    }

    // single-block chain-binding check; Storage::open re-opens for live walk
    auto verifier = [&dbPath, &dbBackend](const cs::CheckpointHead& head) -> bool {
        if (head.head_hash.empty()) return true;  // legacy / no binding to verify
        auto db = cs::chain_integrity::open_db(dbBackend, dbPath);
        if (!db) {
            cswarning() << "tryQuickStart: cannot open chain DB for verification; allowing checkpoint";
            return true;
        }
        const bool ok = cs::chain_integrity::verify_at(*db, head.sequence, head.head_hash);
        if (!ok) {
            cserror() << "tryQuickStart: chain-binding mismatch at seq " << head.sequence
                      << "; quarantining checkpoint";
        }
        return ok;
    };

    bool ok = serializationManPtr_->load(verifier);

    if (ok) {
        cslog() << "Caches for QUICK START loaded successfully!";
    } else {
        cswarning() << "Could not load caches for QUICK START, continue with slow start :(";
        for (auto it : reserveConf) {
            initialConfidants.insert(it);
        }
    }

    return ok;
}

bool BlockChain::init(
    const std::string& path,
    cs::CachesSerializationManager* serializationManPtr,
    std::set<cs::PublicKey>& initialConfidants,
    cs::Sequence newBlockchainTop
  ) {
    cs::Connector::connect(&this->removeBlockEvent, trxIndex_.get(), &TransactionsIndex::onRemoveBlock);

    lastSequence_ = 0;
    bool successfulQuickStart = false;

    if (newBlockchainTop == cs::kWrongSequence) {
        if (trxIndex_->recreate() || !trxIndex_->isReady() || trxIndex_->looksEmpty()) {
            cswarning() << "trxIndex needs rebuild — skipping QuickStart, slow-start will populate it";
            if (!trxIndex_->recreate()) {
                trxIndex_->forceRebuild();
            }
            bindSerializationManToCaches(serializationManPtr, initialConfidants);
        }
        else {
            successfulQuickStart = tryQuickStart(serializationManPtr, initialConfidants,
                                                 path, cs::ConfigHolder::instance().config()->getStorageSettings().dbBackend);
            if (!successfulQuickStart && trxIndex_->recreate()) {
                cslog() << "Cannot use QUICK START, trxIndex has to be recreated";
            }
        }
    }

    cs::Sequence firstBlockToReadInDatabase = 0;
    if (successfulQuickStart) {
        if (lastSequence_ != 0) {
            firstBlockToReadInDatabase = lastSequence_ + 1;
            trxIndex_->pinFloor(lastSequence_.load());
            emit successfullQuickStartEvent(csdb::Amount(blockRewardIntegral_, blockRewardFraction_), csdb::Amount(miningCoefficientIntegral_, miningCoefficientFraction_), miningOn_, miningOn_, TimeMinStage1_);
        }

        csinfo() << "QUICK START! lastSequence_   is " << lastSequence_.load();
        csinfo() << "QUICK START! first block to read in database is " << firstBlockToReadInDatabase;

#ifdef CS_MAINNET_UUID
        // QS bypasses onReadFromDB(block#1); check uuid_ here too.
        if (uuid_.load() == CS_MAINNET_UUID) {
            cserror() << kLogPrefix << "CS_REFUSE_MAINNET build detected mainnet UUID "
                      << uuid_.load() << " (via QuickStart) — refusing to run.";
            std::exit(2);
        }
#endif
    }
    else {
        cslog() << "SLOW START...";
    }

    cslog() << kLogPrefix << "Trying to open DB...";

    size_t totalLoaded = 0;
    bool checkTrxIndexRecreate = true;

    csdb::Storage::OpenCallback progress = [&](const csdb::Storage::OpenProgress& progress) {
        if (stop_) {
            cslog() << kLogPrefix << "shutdown requested, aborting slow start at "
                    << WithDelimiters(progress.poolsProcessed);
            return true;
        }
        if (checkTrxIndexRecreate) {
          checkTrxIndexRecreate = false;
          if (trxIndex_->recreate() && successfulQuickStart) {
              cslog() << "Blockchain: TrxIndex must be recreated, cancel QUICK START... Restart NODE, please";
              trxIndex_->invalidate();
              return true;
          }
        }

        totalLoaded = progress.poolsProcessed;
        if (totalLoaded % 1000 == 0) {
            std::cout << '\r' << WithDelimiters(progress.poolsProcessed) << std::flush;
        }
        return false;
    };

    const auto& storageCfg = cs::ConfigHolder::instance().config()->getStorageSettings();

    if (!storage_.open(path, progress, newBlockchainTop, firstBlockToReadInDatabase,
                       storageCfg.asyncWriteQueueSize, storageCfg.writeBatchSize,
                       storageCfg.useStubs,
                       static_cast<uint64_t>(storageCfg.rocksdbBlockCacheMb) << 20,
                       static_cast<uint64_t>(storageCfg.rocksdbMemtableMb) << 20,
                       storageCfg.dbBackend)) {
        cserror() << kLogPrefix << "Couldn't open database at " << path;
        return false;
    }

    // Spin up the parallel signature verifier (sync-time only; idle at steady state).
    if (storageCfg.verifyWorkerCount > 0 && storageCfg.verifyBatchSize > 1) {
        verifierPool_ = std::make_unique<cs::VerifierPool>(storageCfg.verifyWorkerCount);
        verifyBatchSize_ = storageCfg.verifyBatchSize;
        cslog() << kLogPrefix << "verifier pool: " << storageCfg.verifyWorkerCount
                << " workers, batch size " << storageCfg.verifyBatchSize;
    }
    if (storageCfg.progressLogInterval > 0) {
        progressLogInterval_ = storageCfg.progressLogInterval;
    }

    if (newBlockchainTop != cs::kWrongSequence) {
        // chain was just trimmed; align trxIndex if it was ahead.
        if (trxIndex_) trxIndex_->trimToFloor(newBlockchainTop);
        return true;
    }

    cslog() << "\rDB is opened, loaded " << WithDelimiters(totalLoaded) << " blocks";

    if (storage_.last_hash().is_empty()) {
        csdebug() << "Last hash is empty...";
        if (storage_.size()) {
            cserror() << "failed!!! Delete the Database!!! It will be restored from nothing...";
            return false;
        }
        if (successfulQuickStart) {
            serializationManPtr_->clear();
        }
        writeGenesisBlock();
    }
    else {
        if (!postInitFromDB(successfulQuickStart)) {
            return false;
        }
    }

    // full genesis-to-head walk completed — mark caches validated; carries forward via sentinel.bin
    if (!successfulQuickStart && !stop_ && serializationManPtr_) {
        serializationManPtr_->setCompletedFromGenesis();
    }

    good_ = true;
    blocksToBeRemoved_ = totalLoaded - 1; // any amount to remave after start
    return true;
}

bool BlockChain::isGood() const {
    return good_;
}

void BlockChain::flushIndexes() {
    if (trxIndex_) {
        trxIndex_->flush();
    }
}

bool BlockChain::isTrxIndexReady() const {
    return !trxIndex_ || trxIndex_->isReady();
}

uint64_t BlockChain::uuid() const {
    return uuid_;
}

void BlockChain::onStartReadFromDB(cs::Sequence lastWrittenPoolSeq) {
    if (lastWrittenPoolSeq > 0) {
        cslog() << kLogPrefix << "start reading blocks from DB, " 
                << "last is " << WithDelimiters(lastWrittenPoolSeq);
    }
}

void BlockChain::onReadFromDB(csdb::Pool block, bool* shouldStop) {
    // stop_ is honoured by the OpenCallback (UserCancelled), not here.
    auto blockSeq = block.sequence();
    {
        const auto prev = lastSequence_.load();
        if (prev > blockSeq + 100) {
            cslog() << "TRACE: onReadFromDB lastSequence_ regress from " << prev << " to " << blockSeq;
        }
    }
    lastSequence_ = blockSeq;
    if (blockSeq == 1) {
        cs::Lock lock(dbLock_);
        uuid_ = uuidFromBlock(block);
        csdebug() << kLogPrefix << "UUID = " << uuid_;
#ifdef CS_MAINNET_UUID
        if (uuid_ == CS_MAINNET_UUID) {
            cserror() << kLogPrefix << "CS_REFUSE_MAINNET build detected mainnet UUID "
                      << uuid_ << " — refusing to run.";
            std::exit(2);
        }
#endif
    }

    if (!updateWalletIds(block, *walletsCacheUpdater_.get())) {
        cserror() << kLogPrefix << "updateWalletIds() failed on block #" << block.sequence();
        *shouldStop = true;
    }
    else {
        if (!blockHashes_->onNextBlock(block)) {
            cserror() << kLogPrefix << "blockHashes_->onReadBlock(block) failed on block #" << block.sequence();
            *shouldStop = true;
        }
        else {
            if (block.transactions_count() > 0) {
                const auto block_time = BlockChain::getBlockTime(block);
                for (auto& t : block.transactions()) {
                    t.set_time(block_time);
                }
            }
            updateNonEmptyBlocks(block);
            walletsCacheUpdater_->loadNextBlock(block, block.confidants(), *this);
        }
    }

    if (serializationManPtr_ && blockSeq && !*shouldStop) {
        const auto& sto = cs::ConfigHolder::instance().config()->getStorageSettings();
        const auto every = sto.checkpointEvery > 0 ? sto.checkpointEvery : kQuickStartSaveCachesInterval;
        const auto now = std::chrono::steady_clock::now();
        const bool byCount = (every > 0 && blockSeq % every == 0);
        const bool byTime = (sto.checkpointEveryMinutes > 0
            && now - lastCheckpointWallClock_ >= std::chrono::minutes(sto.checkpointEveryMinutes));
        if (byCount || byTime) {
            cslog() << kLogPrefix << "slow start: saving checkpoint at block " << WithDelimiters(blockSeq);
            trxIndex_->flush();
            storage_.flush();   // sync chain DB WAL alongside indexes and QS save
            cs::CheckpointHead head;
            head.sequence = blockSeq;
            head.head_hash = block.hash().to_binary();
            head.prev_hash = block.previous_hash().to_binary();
            if (serializationManPtr_->save(blockSeq, head)) {
                serializationManPtr_->pruneCheckpoints(sto.checkpointKeep);
                lastCheckpointWallClock_ = now;
            }
            else {
                cserror() << kLogPrefix << "slow start: cannot save checkpoint at " << blockSeq;
            }
        }
    }
}

inline void BlockChain::updateNonEmptyBlocks(const csdb::Pool& pool) {
    const auto transactionsCount = pool.transactions_count();

    if (transactionsCount > 0) {
        totalTransactionsCount_ += transactionsCount;

        if (lastNonEmptyBlock_.transCount && pool.sequence() != lastNonEmptyBlock_.poolSeq) {
            previousNonEmpty_[pool.sequence()] = lastNonEmptyBlock_;
        }

        lastNonEmptyBlock_.poolSeq = pool.sequence();
        lastNonEmptyBlock_.transCount = static_cast<uint32_t>(transactionsCount);
    }
}

bool BlockChain::postInitFromDB(bool successfulQuickStart) {
    auto func = [](const cs::PublicKey& key, const WalletData& wallet) {
        double bal = wallet.balance_.to_double();
        if (bal < -std::numeric_limits<double>::min()) {
            csdebug() << kLogPrefix << "Wallet with negative balance (" << bal << ") detected: "
                      << cs::Utils::byteStreamToHex(key.data(), key.size()) << " ("
                      << EncodeBase58(key.data(), key.data() + key.size()) << ")";
        }
        return true;
    };
    walletsCacheStorage_->iterateOverWallets(func);
    if (successfulQuickStart) {
        emit stopReadingBlocksEvent(totalTransactionsCount_, successfulQuickStart);
    }
    return true;
}

csdb::PoolHash BlockChain::getLastHash() const {
    std::lock_guard lock(dbLock_);

    if (deferredBlock_.is_valid()) {
        return deferredBlock_.hash().clone();
    }

    return storage_.last_hash();
}

std::string BlockChain::getLastTimeStamp() const {
    std::lock_guard<decltype(dbLock_)> l(dbLock_);

    if (deferredBlock_.is_valid()) {
        if (deferredBlock_.user_field_ids().count(kFieldTimestamp) > 0) {
            return deferredBlock_.user_field(kFieldTimestamp).value<std::string>();
        }
        else {
            return std::string("0");
        }
    }
    else {
        return getLastBlock().user_field(kFieldTimestamp).value<std::string>();
    }
}

cs::Bytes BlockChain::getLastRealTrusted() const {
    std::lock_guard<decltype(dbLock_)> l(dbLock_);

    if (deferredBlock_.is_valid()) {
        return cs::Utils::bitsToMask(deferredBlock_.numberTrusted(), deferredBlock_.realTrusted());
    }
    else {
        return cs::Utils::bitsToMask(getLastBlock().numberTrusted(), getLastBlock().realTrusted());
    }
}

void BlockChain::writeGenesisBlock() {
    cswarning() << kLogPrefix << "Adding the genesis block";

    csdb::Pool genesis;
    csdb::Transaction transaction;

    std::string strAddr = "5B3YXqDTcWQFGAqEJQJP3Bg1ZK8FFtHtgCiFLT5VAxpe";
    std::vector<uint8_t> pub_key;
    DecodeBase58(strAddr, pub_key);

    csdb::Address test_address = csdb::Address::from_public_key(pub_key);
    transaction.set_target(test_address);
    transaction.set_source(genesisAddress_);
    transaction.set_currency(csdb::Currency(1));
    transaction.set_amount(csdb::Amount(249'471'071, 0));
    transaction.set_max_fee(csdb::AmountCommission(0.0));
    transaction.set_counted_fee(csdb::AmountCommission(0.0));
    transaction.set_innerID(0);

    genesis.add_transaction(transaction);

    genesis.set_previous_hash(csdb::PoolHash());
    genesis.set_sequence(0);
    addNewWalletsToPool(genesis);

    csdebug() << kLogPrefix << "Genesis block completed ... trying to save";

    /*ignored =*/ finalizeBlock(genesis, true, cs::PublicKeys{});
    /*ignored =*/ applyBlockToCaches(genesis);
    deferredBlock_ = genesis;
    emit storeBlockEvent(deferredBlock_);

    csdebug() << genesis.hash().to_string();

    //uint32_t bSize;
    //auto p = genesis.to_byte_stream(bSize);
    //csdebug() << "Genesis" << cs::Utils::byteStreamToHex(p, bSize);
}

void BlockChain::iterateOverWallets(const std::function<bool(const cs::PublicKey&, const cs::WalletsCache::WalletData&)> func) {
    std::lock_guard lock(cacheMutex_);
    walletsCacheStorage_->iterateOverWallets(func);
}

#ifdef MONITOR_NODE
void BlockChain::iterateOverWriters(const std::function<bool(const cs::PublicKey&, const cs::WalletsCache::TrustedData&)> func) {
    std::lock_guard lock(cacheMutex_);
    walletsCacheStorage_->iterateOverWriters(func);
}

void BlockChain::applyToWallet(const csdb::Address& addr, const std::function<void(const cs::WalletsCache::WalletData&)> func) {
    std::lock_guard lock(cacheMutex_);
    auto pub = getAddressByType(addr, BlockChain::AddressType::PublicKey);
    auto wd = walletsCacheUpdater_->findWallet(pub.public_key());

    if (wd) {
        func(*wd);
    }
}
#endif

size_t BlockChain::getSize() const {
    std::lock_guard lock(dbLock_);
    const auto storageSize = storage_.size();
    return deferredBlock_.is_valid() ? (storageSize + 1) : storageSize;
}

csdb::Pool BlockChain::loadBlock(const csdb::PoolHash& ph) const {
    if (ph.is_empty()) {
        return csdb::Pool{};
    }

    std::lock_guard l(dbLock_);

    if (deferredBlock_.hash() == ph) {
        return deferredBlock_.clone();
    }

    return storage_.pool_load(ph);
}

csdb::Pool BlockChain::loadBlock(const cs::Sequence sequence) const {
    std::lock_guard lock(dbLock_);

    if (deferredBlock_.is_valid() && deferredBlock_.sequence() == sequence) {
        // deferredBlock already composed:
        return deferredBlock_.clone();
    }
    if (sequence > getLastSeq()) {
        return csdb::Pool{};
    }
    return storage_.pool_load(sequence);
}

csdb::Pool BlockChain::loadBlockForSync(const cs::Sequence sequence) const {
    if (uncertainLastBlockFlag_ && uncertainSequence_ == sequence) {
        return csdb::Pool{};
    }
    else {
        return loadBlock(sequence);
    }
}

csdb::Pool BlockChain::loadBlockMeta(const csdb::PoolHash& ph, size_t& cnt) const {
    std::lock_guard lock(dbLock_);

    if (deferredBlock_.hash() == ph) {
        return deferredBlock_.clone();
    }

    return storage_.pool_load_meta(ph, cnt);
}

csdb::Transaction BlockChain::loadTransaction(const csdb::TransactionID& transId) const {
    std::lock_guard l(dbLock_);
    csdb::Transaction transaction;

    if (deferredBlock_.is_valid() && deferredBlock_.sequence() == transId.pool_seq()) {
        transaction = deferredBlock_.transaction(transId).clone();
        transaction.set_time(BlockChain::getBlockTime(deferredBlock_));
    }
    else {
        transaction = storage_.transaction(transId).clone();
        auto tmp = storage_.pool_load(transId.pool_seq());
        transaction.set_time(BlockChain::getBlockTime(tmp));
    }

    return transaction;
}

// - remove the last block from the top of blockchain
// - remove pair (hash, sequence) from cache (blockHashes_)
// - decrement the last sequence by 1
// - undo all transactions / new wallets
void BlockChain::removeLastBlock() {
    if (blocksToBeRemoved_ == 0) {
        csmeta(csdebug) << kLogPrefix << "There are no blocks, allowed to be removed";
        return;
    }
    //--blocksToBeRemoved_;
	cs::Sequence remove_seq = lastSequence_;
	csdb::PoolHash remove_hash = blockHashes_->find(remove_seq);
    csmeta(csdebug) << remove_seq;
    csdb::Pool pool{};

    {
        std::lock_guard lock(dbLock_);

        if (deferredBlock_.is_valid()) {
            pool = deferredBlock_;
            deferredBlock_ = csdb::Pool{};
        }
        else {
            pool = storage_.pool_remove_last();
        }
    }

    if (!pool.is_valid()) {
        csmeta(cserror) << kLogPrefix << "Error! Removed pool is not valid";

		if (remove_hash.is_empty()) {
			cserror() << kLogPrefix << "storage is corrupted, storage rescan is required, removed hash is empty";
			return;
		}
        csinfo() << kLogPrefix << "pool to be deleted is not valid, trying to use repair_remove";
		{
			std::lock_guard lock(dbLock_);
			if (!storage_.pool_remove_last_repair(remove_seq, remove_hash)) {
				cserror() << kLogPrefix << "storage is corrupted, storage rescan is required";
				return;
			}
		}

		cswarning() << kLogPrefix << "Wallets balances maybe invalidated, storage rescan required";
    }
    else {
        // just removed pool is valid

        if (!(remove_hash == pool.hash())) {
            cswarning() << kLogPrefix << "Hashes cache is corrupted, storage rescan is required: last: " 
                << remove_hash.to_string() << ", pool: " << pool.hash().to_string() 
                << ", total hashes: " << blockHashes_->size() 
                << ", total pools: " << getLastSeq() + 1ULL;
            remove_hash = pool.hash();
        }

        if (pool.sequence() == 0) {
            csmeta(cswarning) << kLogPrefix << "Attempt to remove Genesis block !!!!!";
            return;
        }

		// such operations are only possible on valid pool:
        totalTransactionsCount_ -= pool.transactions().size();
		walletsCacheUpdater_->loadNextBlock(pool, pool.confidants(), *this, true);
		// remove wallets exposed by the block
		removeWalletsInPoolFromCache(pool);
		// signal all subscribers, transaction index is still consistent up to removed block!
		emit removeBlockEvent(pool);

        if (lastNonEmptyBlock_.poolSeq == pool.sequence()) {
            lastNonEmptyBlock_ = previousNonEmpty_[lastNonEmptyBlock_.poolSeq];
            previousNonEmpty_.erase(pool.sequence());
        }
    }

    // to be sure, try to remove both sequence and hash
    if (!blockHashes_->remove(remove_seq)) {
        blockHashes_->remove(remove_hash);
    }
    --lastSequence_;
    cslog() << "TRACE: removeLastBlock decremented lastSequence_ to " << lastSequence_.load()
            << " (blocksToBeRemoved_=" << blocksToBeRemoved_ << ")";

    csmeta(csdebug) << kLogPrefix << "done";
}

bool BlockChain::compromiseLastBlock(const csdb::PoolHash& desired_hash) {
    csdb::Pool last_block = csdb::Pool{};
    {
        cs::Lock lock(dbLock_);

        if (deferredBlock_.is_valid()) {
            last_block = deferredBlock_.clone();
        }
    }
    if (!last_block.is_valid()) {
        cserror() << kLogPrefix << "can only compromise the deferred block, not flushed yet";
        uncertainLastBlockFlag_ = false;
        return false;
    }

    const auto seq = last_block.sequence();
    const auto current = last_block.hash();
    const auto desired = desired_hash;

    if (uncertainLastBlockFlag_ && seq != uncertainSequence_) {
        cswarning() << kLogPrefix << "change uncertain sequence from " << uncertainSequence_ << " to " << seq;
    }
    uncertainSequence_ = seq;
    if (uncertainLastBlockFlag_ && current != uncertainHash_) {
        cswarning() << kLogPrefix << "change uncertain hash from " << uncertainHash_.to_string() << " to " << current.to_string();
    }
    uncertainHash_ = current;
    if (uncertainLastBlockFlag_ && desired != desiredHash_) {
        cswarning() << kLogPrefix << "change desired hash from " << desiredHash_.to_string() << " to " << desired.to_string();
    }
    desiredHash_ = desired;

    uncertainLastBlockFlag_ = true;
    cslog() << kLogPrefix << "block " << WithDelimiters(uncertainSequence_) << ", hash " << uncertainHash_.to_string()
        << " is uncertain, desired hash " << desiredHash_.to_string();

    /*signal*/ uncertainBlock(uncertainSequence_);
    return true;
}

void BlockChain::updateLastTransactions(const std::vector<std::pair<cs::PublicKey, csdb::TransactionID>>& updates) {
    std::lock_guard l(cacheMutex_);
    walletsCacheUpdater_->updateLastTransactions(updates);
}

/*static*/
csdb::Address BlockChain::getAddressFromKey(const std::string& key) {
    if (key.size() == kPublicKeyLength) {
        csdb::Address res = csdb::Address::from_public_key(key.data());
        return res;
    }
    else {
        csdb::internal::WalletId id = *reinterpret_cast<const csdb::internal::WalletId*>(key.data());
        csdb::Address res = csdb::Address::from_wallet_id(id);
        return res;
    }
}

/*static*/
csdb::Address BlockChain::getAddressFromKey(const cs::Bytes& key) {
    if (key.size() == kPublicKeyLength) {
        csdb::Address res = csdb::Address::from_public_key(key);
        return res;
    }
    else {
        csdb::internal::WalletId id = *reinterpret_cast<const csdb::internal::WalletId*>(key.data());
        csdb::Address res = csdb::Address::from_wallet_id(id);
        return res;
    }
}

/*static*/
uint64_t BlockChain::getBlockTime(const csdb::Pool& block) noexcept {
    if (block.is_valid()) {
        if (block.user_field_ids().count(kFieldTimestamp) > 0) {
            std::string tmp = block.user_field(kFieldTimestamp).value<std::string>();
            try {
                return std::stoull(tmp);
            }
            catch (...) {
                csdebug() << kLogPrefix << "block " << WithDelimiters(block.sequence()) << " contains incorrect timestamp value " << tmp;
            }
        }
    }
    return 0;
}

void BlockChain::removeWalletsInPoolFromCache(const csdb::Pool& pool) {
    try {
        std::lock_guard lock(cacheMutex_);
        const csdb::Pool::NewWallets& newWallets = pool.newWallets();

        for (const auto& newWall : newWallets) {
            csdb::Address newWallAddress;
            if (!pool.getWalletAddress(newWall, newWallAddress)) {
                cserror() << kLogPrefix << "Wrong new wallet data";
                return;
            }
            if (!walletIds_->normal().remove(newWallAddress)) {
                cswarning() << kLogPrefix << "Wallet was not removed " << newWallAddress.to_string();
            }
        }

        for (const auto& it : pool.transactions()) {
            if (cs::SmartContracts::is_deploy(it)) {
                if (!walletIds_->normal().remove(it.target())) {
                    cswarning() << kLogPrefix << "Contract address was not removed: " << it.target().to_string();
                }
            }
        }

    }
    catch (std::exception& e) {
        cserror() << "Exc=" << e.what();
    }
    catch (...) {
        cserror() << "Exc=...";
    }
}

void BlockChain::logBlockInfo(csdb::Pool& pool) {
    const auto& trusted = pool.confidants();
    std::string realTrustedString;
    auto mask = cs::Utils::bitsToMask(pool.numberTrusted(), pool.realTrusted());
    for (auto i : mask) {
        realTrustedString = realTrustedString + "[" + std::to_string(static_cast<int>(i)) + "] ";
    }

    csdebug() << " trusted count " << trusted.size() << ", RealTrusted = " << realTrustedString;
    for (const auto& t : trusted) {
        csdebug() << "\t- " << cs::Utils::byteStreamToHex(t.data(), t.size());
    }
    csdebug() << " transactions count " << pool.transactions_count();
    if (pool.user_field_ids().count(kFieldTimestamp) > 0) {
        csdebug() << " time: " << pool.user_field(kFieldTimestamp).value<std::string>().c_str();
    }
    csdebug() << " previous hash: " << pool.previous_hash().to_string();
    csdebug() << " hash(" << pool.sequence() << "): " << pool.hash().to_string();
    csdebug() << " last storage size: " << getSize();
}

bool BlockChain::verifyBlockSignatures(const csdb::Pool& pool,
                                       const cs::PublicKeys& lastConfidants,
                                       bool isTrusted) const {
    cs::Sequence currentSequence = pool.sequence();
    const auto& signatures = pool.signatures();
    const auto& realTrusted = pool.realTrusted();

    if (signatures.empty() && (!isTrusted || pool.sequence() != 0)) {
        return false;
    }

    if (signatures.size() < static_cast<size_t>(cs::Utils::maskValue(realTrusted)) && !isTrusted && pool.sequence() != 0) {
        return false;
    }

    // Round confirmations: this block's roundConfirmations against PREVIOUS block's confidants.
    if (currentSequence > 1) {
        cs::Bytes trustedToHash;
        cs::ODataStream tth(trustedToHash);
        tth << currentSequence;
        tth << pool.confidants();
        cs::Hash trustedHash = cscrypto::calculateHash(trustedToHash.data(), trustedToHash.size());

        const cs::Signatures& sigs = pool.roundConfirmations();
        const auto confMask = cs::Utils::bitsToMask(pool.numberConfirmations(), pool.roundConfirmationMask());

        if (!BlockChain::isBootstrap(pool) && confMask.size() > 1) {
            if (!NodeUtils::checkGroupSignature(lastConfidants, confMask, sigs, trustedHash)) {
                return false;
            }
        }
    }

    // Pool signatures: this block's signatures against THIS block's confidants.
    if (currentSequence > 0) {
        Hash tempHash;
        auto hash = pool.hash().to_binary();
        std::copy(hash.cbegin(), hash.cend(), tempHash.data());
        const auto mask = cs::Utils::bitsToMask(pool.numberTrusted(), pool.realTrusted());
        if (!NodeUtils::checkGroupSignature(pool.confidants(), mask, signatures, tempHash)) {
            return false;
        }
    }

    return true;
}

bool BlockChain::finalizeBlock(csdb::Pool& pool, bool isTrusted, cs::PublicKeys lastConfidants, bool skipVerify) {
    if (!pool.compose()) {
        csmeta(cserror) << kLogPrefix << "Couldn't compose block: " << pool.sequence();
        return false;
    }

    if (!skipVerify) {
        if (!verifyBlockSignatures(pool, lastConfidants, isTrusted)) {
            cswarning() << kLogPrefix << "block #" << pool.sequence() << " failed signature verification";
            return false;
        }
    }

    if (pool.transactions_count() > 0) {
        const auto block_time = BlockChain::getBlockTime(pool);
        for (auto& t : pool.transactions()) {
            t.set_time(block_time);
        }
    }

    csmeta(csdetails) << kLogPrefix << "last hash: " << pool.hash().to_string();
    return true;
}

bool BlockChain::applyBlockToCaches(const csdb::Pool& pool) {
    if (!walletsCacheUpdater_) {
        cserror() << "apply block to caches: wallets cache updater unitialized";
        return false;
    }

    csdebug() << kLogPrefix << "store block #" << pool.sequence() << " to chain, update wallets ids";
    updateWalletIds(pool, *walletsCacheUpdater_);

    // ATTENTION! Due to undesired side effect trxIndex_ must be updated prior to wallets caches
    // update transactions index
    trxIndex_->update(pool);

    // update wallet caches

    // former updateFromNextBlock(pool) method:
    try {
        std::lock_guard lock(cacheMutex_);
        // currently block stores own round confidants, not next round:
        const auto& currentRoundConfidants = pool.confidants();
        walletsCacheUpdater_->loadNextBlock(pool, currentRoundConfidants, *this);

        if (!blockHashes_->onNextBlock(pool)) {
            cslog() << kLogPrefix << "Error updating block hashes storage";
        }

        // update non-empty block storage
        updateNonEmptyBlocks(pool);
    }
    catch (std::exception & e) {
        cserror() << "apply block to caches, exception: " << e.what();
        return false;
    }
    catch (...) {
        cserror() << "apply block to caches, unexpected exception";
        return false;
    }

    if (serializationManPtr_ && pool.sequence()) {
        const auto& sto = cs::ConfigHolder::instance().config()->getStorageSettings();
        const auto every = sto.checkpointEvery > 0 ? sto.checkpointEvery : kQuickStartSaveCachesInterval;
        const auto now = std::chrono::steady_clock::now();
        const bool byCount = (every > 0 && pool.sequence() % every == 0);
        const bool byTime = (sto.checkpointEveryMinutes > 0
            && now - lastCheckpointWallClock_ >= std::chrono::minutes(sto.checkpointEveryMinutes));
        if (byCount || byTime) {
            trxIndex_->flush();
            storage_.flush();   // sync chain DB WAL alongside indexes and QS save
            cs::CheckpointHead head;
            head.sequence = pool.sequence();
            head.head_hash = pool.hash().to_binary();
            head.prev_hash = pool.previous_hash().to_binary();
            if (serializationManPtr_->save(pool.sequence(), head)) {
                serializationManPtr_->pruneCheckpoints(sto.checkpointKeep);
                lastCheckpointWallClock_ = now;
            }
            else {
                cserror() << "Cannot save caches with version " << pool.sequence();
            }
        }
    }

    return true;
}

csdb::PoolHash BlockChain::getHashBySequence(cs::Sequence seq) const {
    std::lock_guard lock(dbLock_);

    if (deferredBlock_.sequence() == seq) {
        return deferredBlock_.hash().clone();
    }

    csdb::PoolHash tmp = blockHashes_->find(seq);
    if (!tmp.is_empty()) {
        return tmp;
    }

    return storage_.pool_hash(seq);
}

cs::Sequence BlockChain::getSequenceByHash(const csdb::PoolHash& hash) const {
    std::lock_guard lock(dbLock_);
    
    if (deferredBlock_.hash() == hash) {
        return deferredBlock_.sequence();
    }

    cs::Sequence seq = blockHashes_->find(hash);
    if (seq != kWrongSequence) {
        return seq;
    }

    return storage_.pool_sequence(hash);
}

uint64_t BlockChain::getWalletsCountWithBalance() {
    std::lock_guard lock(cacheMutex_);

    uint64_t count = 0;
    auto proc = [&](const cs::PublicKey&, const WalletData& wallet) {
        constexpr csdb::Amount zero_balance(0);
        if (wallet.balance_ >= zero_balance) {
            count++;
        }
        return true;
    };
    walletsCacheStorage_->iterateOverWallets(proc);
    return count;
}

uint64_t BlockChain::getWalletsCount() const {
    std::lock_guard lock(cacheMutex_);
    return walletsCacheStorage_->getCount();
}

void BlockChain::getTransactions(Transactions& transactions, csdb::Address address, uint64_t offset, uint64_t limit) {
    for (auto trIt = cs::TransactionsIterator(*this, address); trIt.isValid(); trIt.next()) {
        if (offset > 0) {
            --offset;
            continue;
        }

        transactions.push_back(*trIt);
        transactions.back().set_time(BlockChain::getBlockTime(trIt.getPool()));

        if (--limit == 0)
            break;
    }
}

void BlockChain::getAccountRegTime(uint64_t& aTime, csdb::Address address) {
    const cs::Sequence firstBlock = 1U;
    for (auto trIt = cs::TransactionsIterator(*this, address); trIt.isValid(); trIt.next()) {
        aTime=BlockChain::getBlockTime(trIt.getPool());
    }
    if (aTime == 0) {
        aTime = getBlockTime(loadBlock(firstBlock));
    }
}

void BlockChain::getTransactionsUntill(Transactions& transactions, csdb::Address address, csdb::TransactionID id, uint16_t flagg) {
    for (auto trIt = cs::TransactionsIterator(*this, address); trIt.isValid(); trIt.next()) {
        if (id.pool_seq() + 1 > cs::Conveyer::instance().currentRoundNumber() 
            && id.pool_seq() > trIt->id().pool_seq()) {
            break;
        }
        if (id.pool_seq() < trIt->id().pool_seq() || (id.pool_seq() == trIt->id().pool_seq() && id.index() < trIt->id().index())) {
            if (flagg == 1) {
                bool match = false;
                if (trIt->target().is_public_key())
                {
                    if (trIt->target().public_key() == address.public_key()) {
                        match = true;
                    }
                }
                if (trIt->target().is_wallet_id())
                {
                    csdb::Address pKey;
                    findAddrByWalletId(trIt->target().wallet_id(), pKey);
                    if (pKey.public_key() == address.public_key()) {
                        match = true;
                    }
                }
                if(match)
                {
                    transactions.push_back(*trIt);
                    transactions.back().set_time(getBlockTime(trIt.getPool()));
                }
            }
            if (flagg ==2){
                bool match = false;
                if (trIt->source().is_public_key())
                {
                    if (trIt->source().public_key() == address.public_key()) {
                        match = true;
                    }
                }
                if (trIt->source().is_wallet_id())
                {
                    csdb::Address pKey;
                    findAddrByWalletId(trIt->source().wallet_id(), pKey);
                    if (pKey.public_key() == address.public_key()) {
                        match = true;
                    }
                }
                if (match)
                {
                    transactions.push_back(*trIt);
                    transactions.back().set_time(getBlockTime(trIt.getPool()));
                }
            }

            if (flagg == 3) {
                transactions.push_back(*trIt);
                transactions.back().set_time(getBlockTime(trIt.getPool()));
            }
        }
        //if (--limit == 0)
        //    break;
    }
}

bool BlockChain::updateWalletIds(const csdb::Pool& pool, WalletsCache::Updater& proc) {
    try {
        std::lock_guard lock(cacheMutex_);

        const csdb::Pool::NewWallets& newWallets = pool.newWallets();
        for (const auto& newWall : newWallets) {
            csdb::Address newWallAddress;
            if (!pool.getWalletAddress(newWall, newWallAddress)) {
                cserror() << kLogPrefix << "Wrong new wallet data";
                return false;
            }

            if (!insertNewWalletId(newWallAddress, newWall.walletId_, proc)) {
                cserror() << kLogPrefix << "Wallet was already added as new";
            }
        }
    }
    catch (std::exception& e) {
        cserror() << "Exc=" << e.what();
        return false;
    }
    catch (...) {
        cserror() << "Exc=...";
        return false;
    }

    return true;
}

bool BlockChain::insertNewWalletId(const csdb::Address& newWallAddress, WalletId newWalletId, WalletsCache::Updater&) {
    if (!walletIds_->normal().insert(newWallAddress, newWalletId)) {
        cserror() << kLogPrefix << "Cannot add new wallet";
        return false;
    }

    return true;
}

bool BlockChain::addNewWalletsToPool(csdb::Pool& pool) {
    csdebug() << kLogPrefix << "store block #" << pool.sequence() << " add new wallets to pool";

    csdb::Pool::NewWallets* newWallets = pool.newWallets();

    if (!newWallets) {
        cserror() << kLogPrefix << "Pool is read-only";
        return false;
    }

    newWallets->clear();

    std::map<csdb::Address, std::pair<WalletId, csdb::Pool::NewWalletInfo::AddressId>> addrsAndIds;

    csdb::Pool::Transactions& transactions = pool.transactions();
    for (size_t idx = 0; idx < transactions.size(); ++idx) {
        addrsAndIds[transactions[idx].source()].second = {idx, csdb::Pool::NewWalletInfo::AddressType::AddressIsSource};
        addrsAndIds[transactions[idx].target()].second = {idx, csdb::Pool::NewWalletInfo::AddressType::AddressIsTarget};
    }

    const auto& confidants = pool.confidants();
    size_t confWalletsIndexStart = transactions.size();
    for (size_t i = 0; i < confidants.size(); ++i) {
        addrsAndIds[csdb::Address::from_public_key(confidants[i])].second = {confWalletsIndexStart + i, csdb::Pool::NewWalletInfo::AddressType::AddressIsTarget};
    }

    addrsAndIds.erase(genesisAddress_);

    {
        std::lock_guard lock(cacheMutex_);
        walletIds_->normal().fillIds(addrsAndIds);
    }

    for (auto& addrAndId : addrsAndIds) {
        if (!addrAndId.first.is_public_key() || addrAndId.second.first == WalletsIds::kWrongWalletId || addrAndId.first == genesisAddress_) {
            continue;
        }
        newWallets->emplace_back(csdb::Pool::NewWalletInfo{addrAndId.second.second, addrAndId.second.first});
    }
    return true;
}

void BlockChain::tryFlushDeferredBlock() {
    cs::Lock lock(dbLock_);
    if (deferredBlock_.is_valid() && deferredBlock_.is_read_only()) {
        Hash tempHash;
        auto hash = deferredBlock_.hash().to_binary();
        std::copy(hash.cbegin(), hash.cend(), tempHash.data());
        auto mask = cs::Utils::bitsToMask(deferredBlock_.numberTrusted(), deferredBlock_.realTrusted());
        if (NodeUtils::checkGroupSignature(deferredBlock_.confidants(), mask, deferredBlock_.signatures(), tempHash)) {
            deferredBlock_.set_storage(storage_);
            if (deferredBlock_.save()) {
#ifdef DBSQL
                dbsql::saveConfidants(deferredBlock_.sequence(), deferredBlock_.confidants(), deferredBlock_.realTrusted());
#endif
                csdebug() << kLogPrefix << "block #" << WithDelimiters(deferredBlock_.sequence()) << " is flushed to DB";
                deferredBlock_ = csdb::Pool{};
            }
            else {
                cserror() << kLogPrefix << "Failed to flush block #" << WithDelimiters(deferredBlock_.sequence()) << " to DB";
            }
        }
    }
}

void BlockChain::close() {
    stop_ = true;
    tryFlushDeferredBlock();

    // Capture chain-binding info BEFORE storage is closed; getHashBySequence
    // needs the storage open.
    cs::CheckpointHead headInfo;
    if (serializationManPtr_) {
        const auto topSeq = getLastSeq();
        auto headHash = getHashBySequence(topSeq);
        if (!headHash.is_empty()) {
            headInfo.sequence = topSeq;
            headInfo.head_hash = headHash.to_binary();
            if (topSeq > 0) {
                auto prevHash = getHashBySequence(topSeq - 1);
                if (!prevHash.is_empty()) headInfo.prev_hash = prevHash.to_binary();
            }
        }
    }

    cs::Lock lock(dbLock_);
    storage_.close();
    cs::Connector::disconnect(&storage_.readBlockEvent(), this, &BlockChain::onReadFromDB);
    blockHashes_->close();
    trxIndex_->close();

    if (!serializationManPtr_) {
        csinfo() << "Blockchain: no serialization manager provided to save caches for QUICK START.";
        return;
    }

    csinfo() << "Blockchain: try to save caches for QUICK START.";

    if (serializationManPtr_->save(0, headInfo)) {
      csinfo() << "Blockchain: caches for QUICK START saved successfully.";
    }
    else {
      csinfo() << "~Blockchain: couldn't save caches for QUICK START.";
    }
}

bool BlockChain::getTransaction(const csdb::Address& addr, const int64_t& innerId, csdb::Transaction& result) const {
    for (auto it = cs::TransactionsIterator(*this, addr); it.isValid(); it.next()) {
        if (it->innerID() == innerId) {
            result = *it;
            return true;
        }
    }
    return false;
}

bool BlockChain::updateContractData(const csdb::Address& abs_addr, const cs::Bytes& data) const {
    cs::Lock lock(dbLock_);
    return storage_.update_contract_data(abs_addr, data);
}

bool BlockChain::getContractData(const csdb::Address& abs_addr, cs::Bytes& data) const {
    cs::Lock lock(dbLock_);
    return storage_.get_contract_data(abs_addr, data);
}

void BlockChain::createCachesPath() {
    fs::path dbPath(cachesPath);
    boost::system::error_code code;
    const auto res = fs::is_directory(dbPath, code);

    if (!res) {
        fs::create_directory(dbPath);
    }
}

bool BlockChain::findWalletData(const csdb::Address& address, WalletData& wallData, WalletId& id) const {
    if (address.is_wallet_id()) {
        id = address.wallet_id();
        return findWalletData(address.wallet_id(), wallData);
    }

    std::lock_guard lock(cacheMutex_);

    if (!walletIds_->normal().find(address, id)) {
        return false;
    }

    return findWalletData_Unsafe(id, wallData);
}

bool BlockChain::findWalletData(const csdb::Address& address, WalletData& wallData) const {
    if (address.is_wallet_id()) {
        return findWalletData(address.wallet_id(), wallData);
    }

    std::lock_guard lock(cacheMutex_);

    auto wallDataPtr = walletsCacheUpdater_->findWallet(address.public_key());
    if (wallDataPtr) {
        wallData = *wallDataPtr;
        return true;
    }
    return false;
}

bool BlockChain::findWalletData(WalletId id, WalletData& wallData) const {
    std::lock_guard lock(cacheMutex_);
    return findWalletData_Unsafe(id, wallData);
}

double BlockChain::getStakingCoefficient(StakingCoefficient coeff) const {
    switch (coeff) {
    case StakingCoefficient::ThreeMonth:
        return 0.25;
    case StakingCoefficient::SixMonth:
        return 0.5;
    case StakingCoefficient::NineMonth:
        return 0.75;
    case StakingCoefficient::Anni:
        return 1;
    default:
        return 0.1;
    }
}

bool BlockChain::findWalletData_Unsafe(WalletId id, WalletData& wallData) const {
    auto pubKey = getAddressByType(csdb::Address::from_wallet_id(id), AddressType::PublicKey);
    auto wallDataPtr = walletsCacheUpdater_->findWallet(pubKey.public_key());

    if (wallDataPtr) {
        wallData = *wallDataPtr;
        return true;
    }

    return false;
}

bool BlockChain::findWalletId(const WalletAddress& address, WalletId& id) const {
    if (address.is_wallet_id()) {
        id = address.wallet_id();
        return true;
    }
    else if (address.is_public_key()) {
        std::lock_guard lock(cacheMutex_);
        return walletIds_->normal().find(address, id);
    }

    cserror() << kLogPrefix << "Wrong address";
    return false;
}

bool BlockChain::findAddrByWalletId(const WalletId id, csdb::Address& addr) const {
    if (!walletIds_->normal().findaddr(id, addr)) {
        return false;
    }

    return true;
}

bool BlockChain::checkForConsistency(csdb::Pool& pool, bool isNew) {
    if (pool.sequence() == 0) {
        return true;
    }
    if (pool.confidants().size() < pool.signatures().size()) {
        return false;
    }
    if (cs::Utils::maskValue(pool.realTrusted()) != pool.signatures().size()) {
        return false;
    }
    csdb::Pool tmp = pool.clone();
    if (isNew && !tmp.compose()) {
        csinfo() << kLogPrefix << "Check for consistency: can't compose block";
        return false;
    }

    cs::Bytes checking = tmp.to_binary();
    csdb::Pool tmpCopy = csdb::Pool::from_binary(std::move(checking));
    if (tmpCopy.sequence() == 0) {
        csinfo() << kLogPrefix << "Check for consistency: Failed to create correct binary representation of block #" << pool.sequence();
        return false;
    }

    if (isNew && tmpCopy.previous_hash() != getLastHash()) {
        csinfo() << kLogPrefix << "Check for consistency: block hash in pool #" << pool.sequence() << " doesn't correspond to the last one";
        return false;
    }
    if (!isNew && tmpCopy.previous_hash() != loadBlock(tmpCopy.sequence() - 1ULL).hash()) {
        csinfo() << kLogPrefix << "Check for consistency: block hash in pool #" << pool.sequence() << " doesn't correspond to the last one";
        return false;
    }

    return true;

}

std::string  BlockChain::printWalletCaches() {
    std::string res;
    csdb::Amount totalCheck{ 0 };
    res += ":\nLast block: " + std::to_string(lastSequence_) + "\n#.     Public Key:                                                    Balance:                    Delegated:  TrxsCount: LastTrxId:  TrxID: Heap:\n";
    int counter = 0;
    iterateOverWallets([&res, &counter, &totalCheck](const cs::PublicKey& addr, const cs::WalletsCache::WalletData& wd) {
        ++counter;
        res += std::to_string(counter) + ". " + cs::Utils::byteStreamToHex(addr.data(), addr.size()) + "   ";
        auto am = wd.balance_.to_string();
        totalCheck += wd.balance_ + wd.delegated_;
        res += am;
        for (size_t k = am.size(); k < 28; ++k) { // 28 positions are covered with " " to align digits
            res += " ";
        }
        auto deleg = wd.delegated_.to_string();
        res += deleg;
        //res += std::to_string(wd.transNum_) + "   ";
        //res += (wd.trxTail_.getLastTransactionId() > 1'000'000'000 ? "No" : std::to_string(wd.trxTail_.getLastTransactionId())) + "   ";
        //res += (wd.lastTransaction_.pool_seq() > 1'000'000'000 ? "No" : std::to_string(wd.lastTransaction_.pool_seq())) + "." + std::to_string(wd.lastTransaction_.index()) + "  ";
        //res += wd.trxTail_.printHeap();
        res += "\n";

        if (wd.delegateSources_ && !wd.delegateSources_->empty()) {
            int delCounter = 0;
            res += "    Delegate Sources(" + std::to_string(wd.delegateSources_->size()) + "):" + "\n";
            for (auto& it : *wd.delegateSources_) {
                ++delCounter;
                res += "        " + std::to_string(counter) + "." + std::to_string(delCounter) + " " + cs::Utils::byteStreamToHex(it.first.data(), it.first.size());
                int cnt = 0;
                for (auto& itt : it.second) {
                    if (cnt > 0) {
                        res += "                                                                            ";
                    }
                    res += "                      " + itt.amount.to_string() + "      " + std::to_string(itt.time) + "\n";
                    ++cnt;
                }
            }
        }
        if (wd.delegateTargets_ && !wd.delegateTargets_->empty()) {
            int delCounter = 0;
            res += "    Delegate Targets(" + std::to_string(wd.delegateTargets_->size()) + "):" + "\n";
            for (auto& it : *wd.delegateTargets_) {
                ++delCounter;
                res += "        " + std::to_string(counter) + "." + std::to_string(delCounter) + " " + cs::Utils::byteStreamToHex(it.first.data(), it.first.size());
                int cnt = 0;
                for (auto& itt : it.second) {
                    if (cnt > 0) {
                        res += "                                                                            ";
                    }
                    res += "                      " + itt.amount.to_string() + "      " + std::to_string(itt.time) + "\n";
                    ++cnt;
                }
            }
        }
        return true;

    });
    res += "---------------------------------------------------------\n";
    res += "Total: " + totalCheck.to_string();
    //csdebug() << res;
    return res;
}

std::optional<csdb::Pool> BlockChain::recordBlock(csdb::Pool& pool, bool isTrusted, bool skipVerify) {
    const auto last_seq = getLastSeq();
    const auto pool_seq = pool.sequence();

    csdebug() << kLogPrefix << "finish & store block #" << pool_seq << " to chain";

    if (last_seq + 1 != pool_seq) {
        cserror() << kLogPrefix << "cannot record block #" << pool_seq << " to chain, last sequence " << last_seq;
        return std::nullopt;
    }

    pool.set_previous_hash(getLastHash());
    if (!checkForConsistency(pool, true)) {
        csdebug() << kLogPrefix << "Pool #" << pool_seq << " failed the consistency check";
        return std::nullopt;
    }
    if (!checkForConsistency(deferredBlock_, false)) {
        csdebug() << kLogPrefix << "Pool #" << deferredBlock_.sequence() << " failed the consistency check";
        //emit stopNode(true);
        return std::nullopt;
    }

    constexpr cs::Sequence NoSequence = std::numeric_limits<cs::Sequence>::max();
    cs::Sequence flushed_block_seq = NoSequence;

    //if the block is not applied here, but the deferred block is already saved 
//we have situation when we try to save the deferred block anther time
//the pool counter was not incremented and we have to save this block again
    cs::PublicKeys lastConfidants;
    if (pool_seq > 1) {

        cs::Lock lock(dbLock_);

        if (deferredBlock_.sequence() + 1 == pool_seq) {
            lastConfidants = deferredBlock_.confidants();
        }
        else {
            lastConfidants = loadBlock(pool_seq - 1).confidants();
        }
    }

    if (finalizeBlock(pool, isTrusted, lastConfidants, skipVerify)) {
        csdebug() << kLogPrefix << "The block is correct";
        if (!applyBlockToCaches(pool)) {
            csdebug() << kLogPrefix << "failed to apply block to caches";
            return std::nullopt;
        }
    }
    else {
        csdebug() << kLogPrefix << "the signatures of the block are insufficient or incorrect";
        setBlocksToBeRemoved(1U);
        return std::nullopt;
    }
    //========================================

    {
        cs::Lock lock(dbLock_);

        if (deferredBlock_.is_valid()) {

            deferredBlock_.set_storage(storage_);

            if (deferredBlock_.save()) {
#ifdef DBSQL
                dbsql::saveConfidants(pool_seq, deferredBlock_.confidants(), deferredBlock_.realTrusted());
#endif
                flushed_block_seq = deferredBlock_.sequence();
                if (uuid_ == 0 && flushed_block_seq == 1) {
                    uuid_ = uuidFromBlock(deferredBlock_);
                    csdebug() << kLogPrefix << "UUID = " << uuid_;
                }
            }
            else {
                csmeta(cserror) << kLogPrefix << "Couldn't save block: " << deferredBlock_.sequence();
                return std::nullopt;
            }
        }
    }

    if (flushed_block_seq != NoSequence) {
        csdebug() << "---------------------------- Flush block #" << flushed_block_seq << " to disk ---------------------------";
        csdebug() << "signatures amount = " << deferredBlock_.signatures().size() << ", smartSignatures amount = " << deferredBlock_.smartSignatures().size()
                  << ", see block info above";
        csdebug() << "----------------------------------------------------------------------------------";
    }


    {
        cs::Lock lock(dbLock_);

        deferredBlock_ = pool;
        pool = deferredBlock_.clone();
        {
            const auto prev = lastSequence_.load();
            const auto newSeq = deferredBlock_.sequence();
            if (prev > newSeq + 100) {
                cslog() << "TRACE: recordBlock lastSequence_ regress from " << prev << " to " << newSeq
                        << " (deferredBlock pool seq=" << newSeq << ")";
            }
        }
        lastSequence_ = deferredBlock_.sequence();
    }

    //csdetails() << kLogPrefix << "Pool #" << deferredBlock_.sequence() << ": " << cs::Utils::byteStreamToHex(deferredBlock_.to_binary().data(), deferredBlock_.to_binary().size());
    emit storeBlockEvent(pool);
    if constexpr (false && (pool.transactions_count() > 0 || pool.sequence() % 10 == 0)) {//log code
        std::string res = printWalletCaches() + "\nTransactions: \n";
        csdb::Amount r_cost{ 0 };
        for (auto it : pool.transactions()) {
            res += it.id().to_string() + " " + it.source().to_string() + " -> " + it.target().to_string() + " : " + it.amount().to_string() 
                + ", Counted fee: " + std::to_string(it.counted_fee().to_double()) + ", Max fee: " + std::to_string(it.max_fee().to_double()) + "\n";
            r_cost += it.counted_fee().to_double();
        }
        res += "Round cost: " + pool.roundCost().to_string() + " Counted Round cost: " + r_cost.to_string();
        csdebug() << res;
    }

    // log cached block
    csdebug() << "----------------------- Defer block #" << pool.sequence() << " until next round ----------------------";
    logBlockInfo(pool);
    csdebug() << "----------------------------------- " << pool.sequence() << " --------------------------------------";

    return std::make_optional(pool);
}

bool BlockChain::updateLastBlock(cs::RoundPackage& rPackage) {
    return updateLastBlock(rPackage, deferredBlock_);
}

bool BlockChain::updateLastBlock(cs::RoundPackage& rPackage, const csdb::Pool& poolFrom) {
    csdebug() << kLogPrefix << "Starting update last block: check ...";
    //if (deferredBlock_.is_valid()) {
    //  csdebug() << "BLOCKCHAIN> Deferred block is invalid, can't update it";
    //  return false;
    //}
    if (poolFrom.is_read_only()) {
        csdebug() << kLogPrefix << "Deferred block is read_only, be carefull";
        //return false;
    }

    if (poolFrom.sequence() != rPackage.poolMetaInfo().sequenceNumber) {
        csdebug() << kLogPrefix << "Deferred block sequence " << poolFrom.sequence() << " doesn't equal to that in the roundPackage " << rPackage.poolMetaInfo().sequenceNumber << ", can't update it";
        return false;
    }
    if (poolFrom.signatures().size() >= rPackage.poolSignatures().size()) {
        csdebug() << kLogPrefix << "Deferred block has more or the same amount Signatures, than received roundPackage, can't update it";
        return true;
    }
    if (poolFrom.previous_hash() != rPackage.poolMetaInfo().previousHash) {
        csdebug() << kLogPrefix << "Deferred block PREVIOUS HASH doesn't equal to that in the roundPackage, can't update it";
        return false;
    }
    csdebug() << kLogPrefix << "Ok";

    csdb::Pool tmpPool;
    tmpPool.set_sequence(poolFrom.sequence());
    tmpPool.set_previous_hash(poolFrom.previous_hash());
    tmpPool.add_real_trusted(cs::Utils::maskToBits(rPackage.poolMetaInfo().realTrustedMask));
    csdebug() << kLogPrefix << "new mask set to deferred block: " << cs::TrustedMask::toString(rPackage.poolMetaInfo().realTrustedMask);
    tmpPool.add_number_trusted(static_cast<uint8_t>(rPackage.poolMetaInfo().realTrustedMask.size()));
    tmpPool.setRoundCost(poolFrom.roundCost());
    tmpPool.set_confidants(poolFrom.confidants());
    for (auto& it : poolFrom.transactions()) {
        tmpPool.add_transaction(it);
    }
    BlockChain::setTimestamp(tmpPool, rPackage.poolMetaInfo().timestamp);
    for (auto& it : poolFrom.smartSignatures()) {
        tmpPool.add_smart_signature(it);
    }
    csdb::Pool::NewWallets* newWallets = tmpPool.newWallets();
    const csdb::Pool::NewWallets& defWallets = poolFrom.newWallets();
    if (!newWallets) {
        csdebug() << kLogPrefix << "newPool is read-only";
        return false;
    }

    for (auto it : defWallets) {
        newWallets->push_back(it);
    }

    if (rPackage.poolMetaInfo().sequenceNumber > 1) {
        tmpPool.add_number_confirmations(poolFrom.numberConfirmations());
        tmpPool.add_confirmation_mask(poolFrom.roundConfirmationMask());
        tmpPool.add_round_confirmations(poolFrom.roundConfirmations());
    }

    return deferredBlockExchange(rPackage, tmpPool);
}

/*static*/
void BlockChain::setTimestamp(csdb::Pool& block, const std::string& timestamp) {
    block.add_user_field(BlockChain::kFieldTimestamp, timestamp);
}

// user field "kFieldServiceInfo": [0] - info version, [1] - block flag, == 1 if boostrap block

/*static*/
void BlockChain::setBootstrap(csdb::Pool& block, bool is_bootstrap) {
    if (is_bootstrap) {
        std::string info;
        if (block.user_field_ids().count(BlockChain::kFieldServiceInfo) > 0) {
            info = block.user_field(BlockChain::kFieldServiceInfo).value<std::string>();
        }
        if (info.size() >= 2) {
            if (info[1] == '\001') {
                // already set
                csdetails() << "BlockChain: block #" << block.sequence() << " bootstrap flag has already set";
                return;
            }
        }
        else {
            info.resize(2);
        }
        info[1] = '\001'; // boostrap block
        block.add_user_field(BlockChain::kFieldServiceInfo, info);
        csdebug() << "BlockChain: set block #" << block.sequence() << " bootstrap flag";
    }
    else {
        // clear bootstrap flag if set, otherwise ignore
        if (block.user_field_ids().count(BlockChain::kFieldServiceInfo) > 0) {
            std::string info = block.user_field(BlockChain::kFieldServiceInfo).value<std::string>();
            if (info.size() >= 2) {
                if (info[1] == '\000') {
                    // already unset
                    csdetails() << "BlockChain: block #" << block.sequence() << " bootstrap flag has already unset";
                    return;
                }
                info[1] = '\000'; // non-boostrap block
                block.add_user_field(BlockChain::kFieldServiceInfo, info);
                csdebug() << "BlockChain: clear block #" << block.sequence() << " bootstrap flag";
            }
            else {
                cserror() << "BlockChain: unable to parse block service info, incompatible version";
            }
        }
    }
}

/*static*/
bool BlockChain::isBootstrap(const csdb::Pool& block) {
    if (block.user_field_ids().count(BlockChain::kFieldServiceInfo) > 0) {
        std::string s = block.user_field(BlockChain::kFieldServiceInfo).value<std::string>();
        if (s.size() >= 2 && s[0] == 0) {
            return s[1] == 1;
        }
        else {
            csdebug() << "BlockChain: unable to parse block service info, incompatible version";
        }
    }
    return false;
}

bool BlockChain::deferredBlockExchange(cs::RoundPackage& rPackage, const csdb::Pool& newPool) {

    // final compose and test:
    csdb::Pool tmp_clone = newPool.clone();
    auto tmp = rPackage.poolSignatures();
    tmp_clone.set_signatures(tmp);
    tmp_clone.compose();
    Hash tempHash;
    auto hash = tmp_clone.hash();
    auto bytes = hash.to_binary();
    std::copy(bytes.cbegin(), bytes.cend(), tempHash.data());
    if (NodeUtils::checkGroupSignature(tmp_clone.confidants(), rPackage.poolMetaInfo().realTrustedMask, rPackage.poolSignatures(), tempHash)) {
        csmeta(csdebug) << kLogPrefix << "The number of signatures is sufficient and all of them are OK!";
        if (!checkForConsistency(tmp_clone, true)) {
            csdebug() << kLogPrefix << "Replace the deferred block #" << tmp_clone.sequence() << ": consistency check failed";
            return false;
        }
    }
    else {
        cswarning() << kLogPrefix << "Some of Pool Signatures aren't valid. The pool will not be written to DB. It will be automatically written, when we get proper data";
        return false;
    }


    // update deferred block
    std::lock_guard lock(dbLock_);
    deferredBlock_ = tmp_clone;
    this->blockHashes_->update(deferredBlock_);

    return true;
}

bool BlockChain::isSpecial(const csdb::Transaction& t) {
    if (t.user_field(cs::trx_uf::sp::managing).is_valid()) {
        return true;
    }
    return false;
}

bool BlockChain::storeBlock(csdb::Pool& pool, cs::PoolStoreType type, bool skipVerify) {
    const auto lastSequence = getLastSeq();
    const auto poolSequence = pool.sequence();
    
    csdebug() << csfunc() << "last #" << lastSequence << ", pool #" << poolSequence;


    if (poolSequence < lastSequence) {
        // ignore
        csdebug() << kLogPrefix << "ignore oudated block #" << poolSequence << ", last written #" << lastSequence;
        // it is not error, so caller code nothing to do with it
        return true;
    }

    if (!BlockChain::isBootstrap(pool)) {
        // Stub-derived empty pools have no confirmations on disk; only enforce
        // for non-empty pools. Coordinated upgrade requirement.
        const bool requireConfirmations = pool.transactions_count() > 0;
        if (requireConfirmations && (pool.numberConfirmations() == 0 || pool.roundConfirmations().size() == 0) && pool.sequence() > 1) {
            return false;
        }
    }

    if (poolSequence == lastSequence) {
        csdebug() << kLogPrefix << "poolSequence == lastSequence";
        if (isLastBlockUncertain() && pool.sequence() == uncertainSequence_) {
            cslog() << kLogPrefix << "review replacement for uncertain block " << WithDelimiters(poolSequence);
            if (pool.hash() == desiredHash_) {
                cslog() << kLogPrefix << "replacement candidate has excactly desired hash, compare content of both block versions";

                std::lock_guard lock(dbLock_);

                if (BlockChain::testContentEqual(pool, deferredBlock_)) {
                    deferredBlock_ = pool;
                    resetUncertainState();
                    ++cntUncertainReplaced;
                    csdebug() << kLogPrefix << "get desired last block with the same content, continue with blockchain successfully";
                    return true;
                }
                else {
                    csdebug() << kLogPrefix << "the desired last block has the different content, drop it and remove own last block";
                    removeLastBlock();
                    return false;
                }
            }
            else {
                cslog() << kLogPrefix << "replacement candidate has undesired hash, ignore";
            }
        }

        std::lock_guard lock(dbLock_);

        // ignore
        csdebug() << kLogPrefix << "ignore oudated block #" << poolSequence << ", last written #" << lastSequence;
        // it is not error, so caller code nothing to do with it
        return true;
    }

    if (poolSequence == lastSequence + 1) {
        debugRecomputeBlockDiff(pool);
        static const bool s_writerDiff = [] {
            const char* v = std::getenv("CS_DEBUG_RECOMPUTE");
            return v && *v && std::string_view(v) != "0";
        }();
        if (s_writerDiff && localCandidateGetter_) {
            const csdb::Pool* localCandidate = localCandidateGetter_();
            if (localCandidate && localCandidate->is_valid()
                    && localCandidate->sequence() == poolSequence
                    && localCandidate->hash() != pool.hash()) {
                debugWriterDiff(pool, *localCandidate);
            }
        }
        if (pool.previous_hash() != getLastHash()) {
            csdebug() << "BLOCKCHAIN> new pool\'s prev. hash does not equal to current last hash";
            if (getLastHash().is_empty()) {
                cserror() << kLogPrefix << "own last hash is empty";
            }
            if (pool.previous_hash().is_empty()) {
                cserror() << "BLOCKCHAIN> new pool\'s prev. hash is empty, don\'t write it, do not any harm to our blockchain";
                return false;
            }
            if (compromiseLastBlock(pool.previous_hash())) {
                csdebug() << kLogPrefix << "compromise own last block and cancel store operation";
            }
            else {
                //if (lastSequence + 5ULL < cs::Conveyer::instance().currentRoundNumber()) {
                //    arrangeBlocksInCache();
                //}
                csdebug() << kLogPrefix << "remove own last block and cancel store operation";
                removeLastBlock();
            }
            return false;
        }

        setTransactionsFees(pool, type);
        if (type == cs::PoolStoreType::Created) {
            if (!addNewWalletsToPool(pool)) {
                csdebug() << kLogPrefix << "can't write a block without adding new wallets";
            }
        }

        //validate block to prevent bc from saving invalid instances:
        bool check_failed = false;
        emit tryToStoreBlockEvent(pool, &check_failed);
        if (check_failed) {
            csdebug() << kLogPrefix << "The pool " << pool.sequence() << " is invalid, won't be stored";
            if (lastSequence_ == poolSequence) {
                --lastSequence_;
                cslog() << "TRACE: tryToStoreBlock check_failed decremented lastSequence_ to " << lastSequence_.load();
                csdebug() << kLogPrefix << "Deleting defered block: " << deferredBlock_.sequence();
                deferredBlock_ = csdb::Pool{};
            }
            badBlocks_.push_back(pool.sequence());
            emit alarmBadBlock(pool.sequence());
            return false;
        }

        // write immediately
        if (recordBlock(pool, false, skipVerify).has_value()) {
            csdebug() << kLogPrefix << "block #" << poolSequence << " has recorded to chain successfully";
            // unable to call because stack overflow in case of huge written blocks amount possible:
            // testCachedBlocks();
            blocksToBeRemoved_ = 1;
            resetUncertainState(); // every successful record require the new confirmation of uncertainity
            return true;
        }

        csdebug() << kLogPrefix << "failed to store block #" << poolSequence << " to chain";

        // no need to perform removeLastBlock() as we've updated only wallet ids
        removeWalletsInPoolFromCache(pool);
        //here the problem could arise if deferred the block is saved to db
        if (lastSequence_ == poolSequence) {
            --lastSequence_;
            cslog() << "TRACE: storeBlock-record-failed decremented lastSequence_ to " << lastSequence_.load();
            deferredBlock_ = csdb::Pool{};
        }

        return false;
    }

    cs::Lock lock(cachedBlocksMutex_);

    const auto poolHash = pool.hash();
    if (cachedBlocks_->contains(poolSequence)) {
        csdebug() << kLogPrefix << "ignore duplicated block #" << poolSequence << " in cache";
        // it is not error, so caller code nothing to do with it
        cachedBlockEvent(poolSequence);
        return true;
    }

    // cache block for future recording
    cachedBlocks_->insert(pool, type);

    csdebug() << kLogPrefix << "cache block #" << poolSequence << "("
        << (pool.is_read_only() ? "Read-Only" : "Normal")
        << ") signed by " << pool.signatures().size()
        << " nodes for future (" << cachedBlocks_->size() << " total)";

    cachedBlockEvent(poolSequence);

    // cache always successful
    return true;
}

std::string BlockChain::poolInfo(const csdb::Pool& pool) {
    std::string res = "";
    res += "seq: " + std::to_string(pool.sequence());
    res += ", trxs: " + std::to_string(pool.transactions().size());
    res += ", sigs: " + std::to_string(pool.signatures().size());
    //res += ", hash: " + pool.hash().to_string();
    return res;
}

void BlockChain::testCachedBlocks() {
    csdebug() << kLogPrefix << "test cached blocks";

    cs::Lock lock(cachedBlocksMutex_);

    if (cachedBlocks_->isEmpty()) {
        csdebug() << kLogPrefix << "no cached blocks";
        return;
    }

    auto lastSeq = getLastSeq() + 1;

    // clear unnecessary sequence
    if (cachedBlocks_->minSequence() < lastSeq) {
        csdebug() << kLogPrefix << "Remove outdated blocks up to #" << lastSeq << " from cache";
        cachedBlocks_->remove(cachedBlocks_->minSequence(), lastSeq - 1);
    }

    size_t countStored = 0;
    cs::Sequence fromSeq = lastSeq;
    const size_t batchSize = (verifierPool_ && verifyBatchSize_ > 1) ? verifyBatchSize_ : 1;

    while (!cachedBlocks_->isEmpty()) {
        if (stop_) {
            return;
        }

        const auto firstBlockInCache = cachedBlocks_->minSequence();
        if (firstBlockInCache != lastSeq) {
            csdebug() << kLogPrefix << "Stop store blocks from cache. Next blocks in cache #" << firstBlockInCache;
            break;
        }

        // Collect a batch of consecutive blocks (peek; do not pop yet).
        std::vector<cs::PoolCache::Data> batch;
        batch.reserve(batchSize);
        cs::Sequence wantSeq = lastSeq;
        for (size_t i = 0; i < batchSize; ++i) {
            if (!cachedBlocks_->contains(wantSeq)) break;
            auto v = cachedBlocks_->value(wantSeq);
            if (!v.has_value()) break;
            batch.push_back(std::move(v.value()));
            ++wantSeq;
        }

        if (batch.empty()) break;

        const bool useParallelVerify = verifierPool_ && batch.size() > 1;
        size_t verifiedCount = 0;

        if (useParallelVerify) {
            // Build lastConfidants per block. First needs the previous chain block;
            // subsequent ones get them from the preceding batch entry directly.
            std::vector<cs::PublicKeys> lastConfList(batch.size());
            if (lastSeq > 1) {
                cs::Lock dblock(dbLock_);
                if (deferredBlock_.is_valid() && deferredBlock_.sequence() + 1 == lastSeq) {
                    lastConfList[0] = deferredBlock_.confidants();
                }
                else {
                    lastConfList[0] = loadBlock(lastSeq - 1).confidants();
                }
            }
            for (size_t i = 1; i < batch.size(); ++i) {
                lastConfList[i] = batch[i - 1].pool.confidants();
            }

            // Warm any lazily-cached Pool fields on the main thread before
            // dispatching workers. hash() is the documented lazy field
            // (Pool::hash() updateHash on first access). Touching the others
            // here is defensive — confirms each pool's deserialised data is
            // fully materialised before any worker touches it.
            for (auto& d : batch) {
                (void)d.pool.hash();
                (void)d.pool.signatures();
                (void)d.pool.confidants();
                (void)d.pool.realTrusted();
                (void)d.pool.numberTrusted();
                (void)d.pool.roundConfirmations();
                (void)d.pool.numberConfirmations();
                (void)d.pool.roundConfirmationMask();
            }

            // Dispatch parallel verifies. Capture stable pointers by VALUE
            // rather than references-to-local-references; the latter is
            // implementation-defined and was the suspected cause of
            // intermittent verify failures on the last block of each batch.
            std::vector<std::future<bool>> futures;
            futures.reserve(batch.size());
            for (size_t i = 0; i < batch.size(); ++i) {
                const csdb::Pool* poolPtr = &batch[i].pool;
                const cs::PublicKeys* lcPtr = &lastConfList[i];
                futures.push_back(verifierPool_->submit([this, poolPtr, lcPtr]() {
                    return verifyBlockSignatures(*poolPtr, *lcPtr, /*isTrusted=*/false);
                }));
            }

            // Wait for ALL verifies to complete before applying any block.
            // Avoids storeBlock running in parallel with still-pending verify
            // futures (storeBlock takes Pool& mutable; any global side
            // effect from apply could otherwise race a pending verify).
            std::vector<bool> verifyOk;
            verifyOk.reserve(batch.size());
            for (auto& f : futures) {
                verifyOk.push_back(f.get());
            }

            // Apply in order. Stop applying at the first verify failure.
            for (size_t i = 0; i < batch.size(); ++i) {
                if (!verifyOk[i]) {
                    cserror() << kLogPrefix << "verify failed at block " << batch[i].pool.sequence()
                              << " in batch; dropping it and rest from cache";
                    break;
                }
                cachedBlocks_->remove(batch[i].pool.sequence());
                if (!storeBlock(batch[i].pool, batch[i].type, /*skipVerify=*/true)) {
                    cserror() << kLogPrefix << "apply failed at block " << batch[i].pool.sequence()
                              << "; dropping rest from cache";
                    break;
                }
                ++verifiedCount;
                ++countStored;
            }

            // Drop unapplied tail (verify failure or apply failure).
            for (size_t i = verifiedCount; i < batch.size(); ++i) {
                cachedBlocks_->remove(batch[i].pool.sequence());
            }
        }
        else {
            // Single-block path (steady-state, or verifierPool not configured).
            auto data = cachedBlocks_->pop(firstBlockInCache);
            if (!data.has_value()) {
                cswarning() << "cached blocks returned not valid pool, stop testing cache";
                break;
            }
            if (data.value().pool.is_read_only() && data.value().type == cs::PoolStoreType::Created) {
                cswarning() << "created block from chache is read-only";
            }
            if (!storeBlock(data.value().pool, data.value().type)) {
                cserror() << kLogPrefix << "Failed to record cached block to chain, drop it & wait to request again";
                break;
            }
            ++verifiedCount;
            ++countStored;
        }

        if (countStored >= progressLogInterval_) {
            cslog() << "BLOCKCHAIN> stored " << WithDelimiters(countStored)
                << " blocks " << WithDelimiters(fromSeq) << " .. " << WithDelimiters(fromSeq + countStored) << " from cache";
            countStored = 0;
            fromSeq = lastSeq + 1;
        }

        if (verifiedCount < batch.size()) {
            // Hit a failure; let next sync request bring the missing blocks again.
            break;
        }

        lastSeq = getLastSeq() + 1;
    }

    if (countStored > 0) {
        cslog() << "BLOCKCHAIN> stored " << WithDelimiters(countStored)
            << " blocks " << WithDelimiters(fromSeq) << " .. " << WithDelimiters(fromSeq + countStored) << " from cache";
        countStored = 0;
        fromSeq = lastSeq + 1;
    }
}

std::optional<BlockChain::SequenceInterval> BlockChain::getFreeSpaceBlocks() const {
    auto lastWrittenSequence = getLastSeq();
    cs::Sequence sequence = 0;

    {
        cs::Lock lock(cachedBlocksMutex_);

        if (!cachedBlocks_->isEmpty()) {
            sequence = cachedBlocks_->minSequence();
        }
    }

    if (sequence <= lastWrittenSequence || (sequence - lastWrittenSequence) == 0) {
        return std::nullopt;
    }

    return std::make_optional(std::make_pair(lastWrittenSequence + 1, sequence));
}

const cs::ReadBlockSignal& BlockChain::readBlockEvent() const {
    return storage_.readBlockEvent();
}

const cs::StartReadingBlocksSignal& BlockChain::startReadingBlocksEvent() const {
    return storage_.readingStartedEvent();
}

std::size_t BlockChain::getCachedBlocksSize() const {
    cs::Lock lock(cachedBlocksMutex_);
    return cachedBlocks_->size();
}

std::size_t BlockChain::getCachedBlocksSizeSynced() const {
    cs::Lock lock(cachedBlocksMutex_);
    return cachedBlocks_->sizeSynced();
}

cs::Sequence BlockChain::getCachedBlocksMinSequence() const {
    cs::Lock lock(cachedBlocksMutex_);
    return cachedBlocks_->isEmpty() ? cs::kWrongSequence : cachedBlocks_->minSequence();
}

void BlockChain::clearBlockCache() {
    cs::Lock lock(cachedBlocksMutex_);
    cachedBlocks_->clear();
}

std::vector<BlockChain::SequenceInterval> BlockChain::getRequiredBlocks(cs::Sequence maxSequence) const {
    cs::Sequence seq = getLastSeq();
    const auto firstSequence = seq + 1;
    const cs::Sequence inclusiveLast = (maxSequence != cs::kWrongSequence)
        ? maxSequence
        : (cs::Conveyer::instance().currentRoundNumber() >= 1
            ? cs::Conveyer::instance().currentRoundNumber() - 1
            : cs::Sequence{0});

    if (firstSequence > inclusiveLast) {
        return std::vector<SequenceInterval>();
    }

    const auto roundNumber = inclusiveLast;

    if (cachedBlocks_->isEmpty()) {
        return std::vector<SequenceInterval>{ {firstSequence, roundNumber} };
    }

    auto ranges = cachedBlocks_->ranges();
    const auto minCached = cachedBlocks_->minSequence();
    const auto maxCached = cachedBlocks_->maxSequence();

    if (firstSequence < minCached) {
        ranges.emplace_back(firstSequence, minCached - 1);
    }
    if (maxCached < roundNumber) {
        ranges.emplace_back(maxCached + 1, roundNumber);
    }
    return ranges;
}

void BlockChain::setTransactionsFees(TransactionsPacket& packet) {
    fee::setCountedFees(packet.transactions());
}

void BlockChain::setTransactionsFees(csdb::Pool& pool, cs::PoolStoreType type) {
    csdebug() << __func__;
    if (pool.is_read_only() && type == cs::PoolStoreType::Created) {
        cserror() << kLogPrefix << "Pool is read-only";
        return;
    }
    fee::setCountedFees(pool.transactions());
}

void BlockChain::setTransactionsFees(std::vector<csdb::Transaction>& transactions) {
    fee::setCountedFees(transactions);
}

void BlockChain::setTransactionsFees(std::vector<csdb::Transaction>& transactions, const cs::Bytes&) {
    fee::setCountedFees(transactions);
}

const csdb::Address& BlockChain::getGenesisAddress() const {
    return genesisAddress_;
}

csdb::Address BlockChain::getAddressByType(const csdb::Address& addr, AddressType type) const {
    csdb::Address addr_res{};
    switch (type) {
        case AddressType::PublicKey:
            if (addr.is_public_key() || !findAddrByWalletId(addr.wallet_id(), addr_res)) {
                addr_res = addr;
            }

            break;
        case AddressType::Id:
            uint32_t _id;
            if (findWalletId(addr, _id)) {
                addr_res = csdb::Address::from_wallet_id(_id);
            }

            break;
    }
    return addr_res;
}

bool BlockChain::isEqual(const csdb::Address& laddr, const csdb::Address& raddr) const {
    if (getAddressByType(laddr, AddressType::PublicKey) == getAddressByType(raddr, AddressType::PublicKey)) {
        return true;
    }

    return false;
}

uint32_t BlockChain::getTransactionsCount(const csdb::Address& addr) {
    std::lock_guard lock(cacheMutex_);

    auto pubKey = getAddressByType(addr, AddressType::PublicKey);
    auto wallDataPtr = walletsCacheUpdater_->findWallet(pubKey.public_key());

    if (!wallDataPtr) {
        return 0;
    }

    return static_cast<uint32_t>(wallDataPtr->transNum_);
}

csdb::TransactionID BlockChain::getLastTransaction(const csdb::Address& addr) const {
    std::lock_guard lock(cacheMutex_);

    auto pubKey = getAddressByType(addr, AddressType::PublicKey);
    auto wallDataPtr = walletsCacheUpdater_->findWallet(pubKey.public_key());

    if (!wallDataPtr) {
        return csdb::TransactionID();
    }

    return wallDataPtr->lastTransaction_;
}

cs::Sequence BlockChain::getPreviousPoolSeq(const csdb::Address& addr, cs::Sequence ps) const {
    auto previousSequence = trxIndex_->getPrevTransBlock(addr, ps);

    if (previousSequence == ps) {
        auto pubKey = getAddressByType(addr, AddressType::PublicKey).public_key();
        cserror() << kLogPrefix << "Inconsistent transaction index for public key: "
                  << EncodeBase58(Bytes(pubKey.begin(), pubKey.end()))
                  << ", block seq is " << ps;

        if (cs::ConfigHolder::instance().config()->autoShutdownEnabled()) {
            cserror() << kLogPrefix << "Node will be stopped due to index error. Please restart it.";

            trxIndex_->invalidate();
            Node::requestStop();
        }

        return kWrongSequence;
    }

    return previousSequence;
}

std::pair<cs::Sequence, uint32_t> BlockChain::getLastNonEmptyBlock() {
    std::lock_guard lock(dbLock_);
    return std::make_pair(lastNonEmptyBlock_.poolSeq, lastNonEmptyBlock_.transCount);
}

std::pair<cs::Sequence, uint32_t> BlockChain::getPreviousNonEmptyBlock(cs::Sequence seq) {
    std::lock_guard lock(dbLock_);
    const auto it = previousNonEmpty_.find(seq);

    if (it != previousNonEmpty_.end()) {
        return std::make_pair(it->second.poolSeq, it->second.transCount);
    }

    return std::pair<cs::Sequence, uint32_t>(cs::kWrongSequence, 0);
}

cs::Sequence BlockChain::getLastSeq() const {
    return lastSequence_;
}

const MultiWallets& BlockChain::multiWallets() const {
    return walletsCacheStorage_->multiWallets();
}

namespace {
bool debugRecomputeOn() {
    static const bool en = [] {
        const char* v = std::getenv("CS_DEBUG_RECOMPUTE");
        return v && *v && std::string_view(v) != "0";
    }();
    return en;
}

std::string amtToString(const csdb::Amount& a) {
    std::ostringstream os;
    os << a.integral() << "." << a.fraction();
    return os.str();
}
} // namespace

// Mirrors SolverCore::setBlockReward (solver/src/solvercore.cpp:363) but driven
// purely off the received pool + this node's wallet cache. Same algorithm,
// same ordering, same fixed-point math.
std::string BlockChain::computeShadowBlockReward(const csdb::Pool& rx) const {
    const auto confidants = rx.confidants();
    const auto realTrusted = cs::Utils::bitsToMask(rx.numberTrusted(), rx.realTrusted());
    csdb::Amount totalFee = rx.roundCost();
    if (totalFee == csdb::Amount{ 0 }) {
        for (const auto& tr : rx.transactions()) {
            totalFee += csdb::Amount(tr.counted_fee().to_double());
        }
    }
    csdb::Amount totalStake = 0;
    std::vector<csdb::Amount> confidantAndStake;
    int32_t realTrustedNumber = 0;
    const uint8_t kUntrustedMarker = 255;

    for (size_t i = 0; i < confidants.size(); ++i) {
        csdb::Amount nodeConfidantAndStake;
        csdb::Amount nodeConfidantAndFreezenStake;
        csdb::Amount totalNodeStake = 0;
        if (realTrusted[i] == kUntrustedMarker) {
            confidantAndStake.push_back(csdb::Amount{ 0 });
            continue;
        }
        ++realTrustedNumber;
        BlockChain::WalletData wData;
        findWalletData(csdb::Address::from_public_key(confidants[i]), wData);
        totalNodeStake += wData.balance_;
        nodeConfidantAndStake += wData.balance_;
        if (wData.delegateSources_ != nullptr && wData.delegateSources_->size() > 0) {
            for (auto& keyAndStake : *(wData.delegateSources_)) {
                for (auto& tm : keyAndStake.second) {
                    if (tm.coeff == cs::StakingCoefficient::NoStaking) {
                        nodeConfidantAndStake += tm.amount;
                    }
                    else {
                        nodeConfidantAndFreezenStake += tm.amount * getStakingCoefficient(tm.coeff);
                    }
                }
            }
        }
        totalNodeStake = nodeConfidantAndStake + nodeConfidantAndFreezenStake;

        csdb::Amount totaNodeCutStake = totalNodeStake;
        if (totaNodeCutStake > Consensus::MaxStakeValue) {
            totaNodeCutStake = Consensus::MaxStakeValue;
        }

        confidantAndStake.push_back(
            (nodeConfidantAndStake * getStakingCoefficient(cs::StakingCoefficient::NoStaking) + nodeConfidantAndFreezenStake)
            * (totalNodeStake > csdb::Amount{ 1 } ? totaNodeCutStake / totalNodeStake : csdb::Amount{ 1 })
        );
        totalStake += confidantAndStake[i];
    }

    csdb::Amount minedValue = rx.sequence() < Consensus::StartingDPOS
        ? csdb::Amount{ 0 }
        : totalFee * Consensus::miningCoefficient + Consensus::blockReward;
    if (totalStake < csdb::Amount{ 1 }) {
        totalStake = csdb::Amount{ 1 };
    }
    csdb::Amount oneMiningPart = Consensus::stakingOn ? minedValue / totalStake : csdb::Amount{ 0 };
    csdb::Amount payedReward = 0;
    size_t numPayedTrusted = 0;
    cs::Bytes fldBytes;
    cs::ODataStream stream(fldBytes);
    for (size_t i = 0; i < confidants.size(); ++i) {
        csdb::Amount rewardToPay = 0;
        if (realTrusted[i] != kUntrustedMarker) {
            if (static_cast<int32_t>(numPayedTrusted) == realTrustedNumber - 1) {
                rewardToPay = minedValue - payedReward;
            }
            else {
                rewardToPay = oneMiningPart * confidantAndStake[i];
            }
            ++numPayedTrusted;
        }
        if (rewardToPay > minedValue || rewardToPay < csdb::Amount{ 0 }) {
            rewardToPay = csdb::Amount{ 0 };
        }
        stream << rewardToPay.integral() << rewardToPay.fraction();
        payedReward += rewardToPay;
    }
    return std::string(fldBytes.begin(), fldBytes.end());
}

void BlockChain::debugRecomputeBlockDiff(const csdb::Pool& rx) const {
    if (!debugRecomputeOn()) return;

    std::ostringstream ss;
    ss << "\n=== RECOMPUTE_DIFF seq=" << rx.sequence() << " ===\n";

    {
        const auto myPrev = getLastHash();
        const auto rxPrev = rx.previous_hash();
        ss << "  prev_hash    rx="    << rxPrev.to_string()
           << " local=" << myPrev.to_string()
           << (myPrev == rxPrev ? "  OK" : "  DIVERGE") << "\n";
    }
    {
        const auto mySeq = getLastSeq() + 1;
        ss << "  sequence     rx="    << rx.sequence()
           << " local_next=" << mySeq
           << (mySeq == rx.sequence() ? "  OK" : "  DIVERGE") << "\n";
    }
    {
        std::string ts;
        if (rx.user_field_ids().count(kFieldTimestamp) > 0) {
            ts = rx.user_field(kFieldTimestamp).value<std::string>();
        }
        ss << "  timestamp    rx=\"" << ts << "\"\n";
    }
    {
        std::string rxR;
        if (rx.user_field_ids().count(kFieldBlockReward) > 0) {
            rxR = rx.user_field(kFieldBlockReward).value<std::string>();
        }
        ss << "  blockReward  rx_size=" << rxR.size()
           << " rx_hex=" << cs::Utils::byteStreamToHex(
                  reinterpret_cast<const uint8_t*>(rxR.data()), rxR.size())
           << "\n";
    }
    {
        const auto& conf = rx.confidants();
        ss << "  numberTrusted rx=" << static_cast<int>(rx.numberTrusted())
           << "  confidants_count=" << conf.size() << "\n";
        ss << "  realTrusted   rx=0x" << std::hex << rx.realTrusted() << std::dec << "\n";
    }
    {
        const auto& nw = rx.newWallets();
        const size_t cnt = nw.size();
        cs::WalletsIds::WalletId myNext = 0;
        {
            std::lock_guard lock(cacheMutex_);
            myNext = walletIds_->getNextId();
        }
        ss << "  newWallets    rx_count=" << cnt
           << "  local_nextId=" << myNext << "\n";
    }
    {
        ss << "  numberConfirmations rx=" << static_cast<int>(rx.numberConfirmations())
           << "  confirmationMask rx=0x" << std::hex << rx.roundConfirmationMask() << std::dec
           << "  confirmations_size=" << rx.roundConfirmations().size() << "\n";
    }
    {
        ss << "  pool_hash   rx=" << rx.hash().to_string() << "\n";
    }

    // Per-confidant local WalletData (inputs to setBlockReward).
    ss << "  -- per-confidant local WalletData --\n";
    for (size_t i = 0; i < rx.confidants().size(); ++i) {
        const auto& pk = rx.confidants()[i];
        WalletData wd;
        const bool found = findWalletData(csdb::Address::from_public_key(pk), wd);
        const size_t srcCount = (wd.delegateSources_ ? wd.delegateSources_->size() : 0u);
        const size_t tgtCount = (wd.delegateTargets_ ? wd.delegateTargets_->size() : 0u);
        ss << "    [" << i << "] "
           << cs::Utils::byteStreamToHex(pk.data(), pk.size()).substr(0, 16) << "...  "
           << (found ? "found" : "MISSING")
           << "  bal="   << amtToString(wd.balance_)
           << "  deleg=" << amtToString(wd.delegated_)
           << "  srcN="  << srcCount
           << "  tgtN="  << tgtCount
           << "  trxN="  << wd.transNum_ << "\n";
    }
    // Per-field SHA-256 fingerprints of the received block. Pair with the
    // sha.* lines in TRUSTED_SIGN_DUMP to localize which sub-region of the
    // signed stream diverged between this node and the network majority.
    auto sha = [](const void* p, size_t n) -> std::string {
        const auto h = cscrypto::calculateHash(static_cast<const cs::Byte*>(p), n);
        return cs::Utils::byteStreamToHex(h.data(), h.size());
    };
    auto appendRaw = [](cs::Bytes& dst, const void* p, size_t n) {
        const auto* b = static_cast<const cs::Byte*>(p);
        dst.insert(dst.end(), b, b + n);
    };

    {
        std::string ts;
        if (rx.user_field_ids().count(kFieldTimestamp) > 0) {
            ts = rx.user_field(kFieldTimestamp).value<std::string>();
        }
        ss << "  sha.timestamp     " << sha(ts.data(), ts.size()) << "\n";
    }
    {
        std::string rxR;
        if (rx.user_field_ids().count(kFieldBlockReward) > 0) {
            rxR = rx.user_field(kFieldBlockReward).value<std::string>();
        }
        ss << "  sha.blockReward   " << sha(rxR.data(), rxR.size()) << "\n";

        // Shadow recomputation: what blockReward WOULD this node produce if it
        // had signed this round? Diff against rx_hex tells us whether this
        // node's wallet-state-driven calc agrees with the network writer's.
        const std::string localR = computeShadowBlockReward(rx);
        ss << "  sha.blockReward.local " << sha(localR.data(), localR.size())
           << (localR == rxR ? "  MATCH" : "  DIVERGE") << "\n";
        if (localR != rxR) {
            ss << "  local.blockReward hex="
               << cs::Utils::byteStreamToHex(
                      reinterpret_cast<const uint8_t*>(localR.data()), localR.size())
               << "\n";
        }
    }
    {
        cs::Bytes buf;
        for (const auto& k : rx.confidants()) {
            appendRaw(buf, k.data(), k.size());
        }
        ss << "  sha.confidants    " << sha(buf.data(), buf.size())
           << "  count=" << rx.confidants().size() << "\n";
    }
    {
        const uint64_t rt = rx.realTrusted();
        const uint8_t  nt = rx.numberTrusted();
        cs::Bytes buf;
        appendRaw(buf, &rt, sizeof(rt));
        buf.push_back(nt);
        ss << "  sha.rt+nt         " << sha(buf.data(), buf.size()) << "\n";
    }
    {
        cs::Bytes buf;
        const uint8_t  nc = rx.numberConfirmations();
        const uint64_t cm = rx.roundConfirmationMask();
        buf.push_back(nc);
        appendRaw(buf, &cm, sizeof(cm));
        for (const auto& sig : rx.roundConfirmations()) {
            appendRaw(buf, sig.data(), sig.size());
        }
        const std::string rxSha = sha(buf.data(), buf.size());
        ss << "  sha.confirmations " << rxSha
           << "  count=" << rx.roundConfirmations().size() << "\n";

        // Shadow confirmation view: query this node's confirmationList_ for the
        // same round and serialize it the same way. If sha differs, this node's
        // local confirmation set drifted from the network writer's set — a
        // sign-time-only divergence we can't catch by comparing finalized state.
        if (confirmationGetter_) {
            const auto local = confirmationGetter_(static_cast<cs::RoundNumber>(rx.sequence()));
            if (local.has_value()) {
                cs::Bytes lbuf;
                const uint8_t lnc = static_cast<uint8_t>(local->mask.size());
                const uint64_t lcm = cs::Utils::maskToBits(local->mask);
                lbuf.push_back(lnc);
                appendRaw(lbuf, &lcm, sizeof(lcm));
                for (const auto& sig : local->signatures) {
                    appendRaw(lbuf, sig.data(), sig.size());
                }
                const std::string localSha = sha(lbuf.data(), lbuf.size());
                ss << "  sha.confirmations.local " << localSha
                   << "  count=" << local->signatures.size()
                   << (localSha == rxSha ? "  MATCH" : "  DIVERGE") << "\n";
            }
            else {
                ss << "  sha.confirmations.local (no entry in confirmationList_)\n";
            }
        }
    }
    {
        const auto& nw = rx.newWallets();
        cs::Bytes buf;
        for (const auto& w : nw) {
            const size_t   ti = w.addressId_.trxInd_;
            const size_t   at = w.addressId_.addressType_;
            const auto     wi = w.walletId_;
            appendRaw(buf, &ti, sizeof(ti));
            appendRaw(buf, &at, sizeof(at));
            appendRaw(buf, &wi, sizeof(wi));
        }
        ss << "  sha.newWallets    " << sha(buf.data(), buf.size())
           << "  count=" << nw.size() << "\n";
    }
    {
        const csdb::Amount rc = rx.roundCost();
        const int32_t  ri = rc.integral();
        const uint64_t rf = rc.fraction();
        cs::Bytes buf;
        appendRaw(buf, &ri, sizeof(ri));
        appendRaw(buf, &rf, sizeof(rf));
        ss << "  sha.roundCost     " << sha(buf.data(), buf.size())
           << "  integral=" << ri << "  fraction=" << rf << "\n";
    }
    {
        ss << "  trx.count         " << rx.transactions().size() << "\n";
    }

    // Full to_binary() byte stream for the received block. csdb::Pool caches
    // its binary form during compose(); the rx we get here may not have been
    // composed yet (signatures applied but compose() pending), so .to_binary()
    // returns empty. Clone+compose to force the canonical form, then diff
    // against the local TRUSTED_SIGN_DUMP to_binary_hex for the same seq to
    // find the exact diverging byte/field. Skipped if size > 64 KB.
    csdb::Pool rxClone = rx.clone();
    rxClone.compose();
    const cs::Bytes binBytes = rxClone.to_binary();
    ss << "  to_binary_size " << binBytes.size() << "\n";
    ss << "  sha.to_binary     " << sha(binBytes.data(), binBytes.size()) << "\n";
    if (binBytes.size() <= 65536) {
        ss << "  to_binary_hex  "
           << cs::Utils::byteStreamToHex(binBytes.data(), binBytes.size()) << "\n";
    } else {
        ss << "  to_binary_hex  (skipped: size > 64KB)\n";
    }
    ss << "  composed_hash  " << rxClone.hash().to_string() << "\n";
    ss << "=== END_RECOMPUTE_DIFF seq=" << rx.sequence() << " ===";

    csdebug() << ss.str();
}

// ---------------------------------------------------------------------------
// WRITER_DIFF + cascade sub-dumps
// Called when this node had a local candidate (deferredBlock_ in SolverCore)
// for the same seq as an incoming network-finalized block and they hash-differ.
// Gated by CS_DEBUG_RECOMPUTE=1.
// ---------------------------------------------------------------------------
void BlockChain::debugWriterDiff(const csdb::Pool& net, const csdb::Pool& local) const {
    // Reuse the sha / appendRaw helpers from RECOMPUTE_DIFF.
    auto sha = [](const void* p, size_t n) -> std::string {
        const auto h = cscrypto::calculateHash(static_cast<const cs::Byte*>(p), n);
        return cs::Utils::byteStreamToHex(h.data(), h.size());
    };
    auto appendRaw = [](cs::Bytes& dst, const void* p, size_t n) {
        const auto* b = static_cast<const cs::Byte*>(p);
        dst.insert(dst.end(), b, b + n);
    };

    // Helper: emit one aligned table row.
    // col widths: label=30, local=64, network=64, status=6
    auto row = [](std::ostringstream& s,
                  const std::string& label,
                  const std::string& lcl,
                  const std::string& ntw) {
        const bool match = (lcl == ntw);
        s << "  " << std::left << std::setw(30) << label
          << std::setw(66) << lcl
          << std::setw(66) << ntw
          << (match ? "MATCH" : "DIFFER") << "\n";
    };

    const cs::Sequence seq = net.sequence();
    std::ostringstream ss;
    ss << std::left;
    ss << "\n=== WRITER_DIFF seq=" << seq << " ===\n";
    ss << "  " << std::setw(30) << ""
       << std::setw(66) << "local"
       << std::setw(66) << "network"
       << "status\n";

    // --- sha.timestamp ---
    std::string localTs, netTs;
    if (local.user_field_ids().count(kFieldTimestamp) > 0)
        localTs = local.user_field(kFieldTimestamp).value<std::string>();
    if (net.user_field_ids().count(kFieldTimestamp) > 0)
        netTs = net.user_field(kFieldTimestamp).value<std::string>();
    const std::string tsLocalSha = sha(localTs.data(), localTs.size());
    const std::string tsNetSha   = sha(netTs.data(),   netTs.size());
    row(ss, "sha.timestamp", tsLocalSha, tsNetSha);

    // --- sha.blockReward ---
    std::string localBr, netBr;
    if (local.user_field_ids().count(kFieldBlockReward) > 0)
        localBr = local.user_field(kFieldBlockReward).value<std::string>();
    if (net.user_field_ids().count(kFieldBlockReward) > 0)
        netBr = net.user_field(kFieldBlockReward).value<std::string>();
    const std::string brLocalSha = sha(localBr.data(), localBr.size());
    const std::string brNetSha   = sha(netBr.data(),   netBr.size());
    row(ss, "sha.blockReward", brLocalSha, brNetSha);
    const bool brDiffer = (brLocalSha != brNetSha);

    // --- sha.confidants ---
    {
        cs::Bytes lBuf, nBuf;
        for (const auto& k : local.confidants()) appendRaw(lBuf, k.data(), k.size());
        for (const auto& k : net.confidants())   appendRaw(nBuf, k.data(), k.size());
        row(ss, "sha.confidants",
            sha(lBuf.data(), lBuf.size()) + " cnt=" + std::to_string(local.confidants().size()),
            sha(nBuf.data(), nBuf.size()) + " cnt=" + std::to_string(net.confidants().size()));
    }

    // --- sha.realTrustedMask (raw mask bytes from stage3 aren't stored on pool;
    //     we use bitsToMask round-trip which is what the pool stores) ---
    {
        const auto lMask = cs::Utils::bitsToMask(local.numberTrusted(), local.realTrusted());
        const auto nMask = cs::Utils::bitsToMask(net.numberTrusted(),   net.realTrusted());
        row(ss, "sha.realTrustedMask",
            sha(lMask.data(), lMask.size()),
            sha(nMask.data(), nMask.size()));
    }

    // --- sha.numberTrusted+rt ---
    {
        cs::Bytes lBuf, nBuf;
        { const uint64_t rt = local.realTrusted(); const uint8_t nt = local.numberTrusted();
          appendRaw(lBuf, &rt, sizeof(rt)); lBuf.push_back(nt); }
        { const uint64_t rt = net.realTrusted();   const uint8_t nt = net.numberTrusted();
          appendRaw(nBuf, &rt, sizeof(rt)); nBuf.push_back(nt); }
        row(ss, "sha.numberTrusted+rt",
            sha(lBuf.data(), lBuf.size()),
            sha(nBuf.data(), nBuf.size()));
    }

    // --- sha.newWallets ---
    {
        const auto& lNW = local.newWallets();
        const auto& nNW = net.newWallets();
        cs::Bytes lBuf, nBuf;
        for (const auto& w : lNW) {
            const size_t ti = w.addressId_.trxInd_; const size_t at = w.addressId_.addressType_; const auto wi = w.walletId_;
            appendRaw(lBuf, &ti, sizeof(ti)); appendRaw(lBuf, &at, sizeof(at)); appendRaw(lBuf, &wi, sizeof(wi));
        }
        for (const auto& w : nNW) {
            const size_t ti = w.addressId_.trxInd_; const size_t at = w.addressId_.addressType_; const auto wi = w.walletId_;
            appendRaw(nBuf, &ti, sizeof(ti)); appendRaw(nBuf, &at, sizeof(at)); appendRaw(nBuf, &wi, sizeof(wi));
        }
        row(ss, "sha.newWallets",
            sha(lBuf.data(), lBuf.size()) + " cnt=" + std::to_string(lNW.size()),
            sha(nBuf.data(), nBuf.size()) + " cnt=" + std::to_string(nNW.size()));
        const bool nwDiffer = (sha(lBuf.data(), lBuf.size()) != sha(nBuf.data(), nBuf.size()));

        // NEWWALLETS_DIFF sub-dump
        if (nwDiffer) {
            ss << "\n  --- NEWWALLETS_DIFF seq=" << seq << " ---\n";
            const size_t maxI = std::max(lNW.size(), nNW.size());
            for (size_t i = 0; i < maxI; ++i) {
                const bool hasL = i < lNW.size();
                const bool hasN = i < nNW.size();
                std::string lStr = hasL
                    ? ("addrType=" + std::to_string(lNW[i].addressId_.addressType_)
                     + " trxInd=" + std::to_string(lNW[i].addressId_.trxInd_)
                     + " walletId=" + std::to_string(lNW[i].walletId_))
                    : "(none)";
                std::string nStr = hasN
                    ? ("addrType=" + std::to_string(nNW[i].addressId_.addressType_)
                     + " trxInd=" + std::to_string(nNW[i].addressId_.trxInd_)
                     + " walletId=" + std::to_string(nNW[i].walletId_))
                    : "(none)";
                const bool same = hasL && hasN
                    && lNW[i].addressId_.trxInd_     == nNW[i].addressId_.trxInd_
                    && lNW[i].addressId_.addressType_ == nNW[i].addressId_.addressType_
                    && lNW[i].walletId_               == nNW[i].walletId_;
                ss << "    [" << i << "] local: " << lStr
                   << "  net: " << nStr
                   << (same ? "  MATCH" : "  DIFFER") << "\n";
            }
            ss << "  --- END_NEWWALLETS_DIFF seq=" << seq << " ---\n";
        }
    }

    // --- sha.confirmations ---
    {
        cs::Bytes lBuf, nBuf;
        { const uint8_t nc = local.numberConfirmations(); const uint64_t cm = local.roundConfirmationMask();
          lBuf.push_back(nc); appendRaw(lBuf, &cm, sizeof(cm));
          for (const auto& sig : local.roundConfirmations()) appendRaw(lBuf, sig.data(), sig.size()); }
        { const uint8_t nc = net.numberConfirmations();   const uint64_t cm = net.roundConfirmationMask();
          nBuf.push_back(nc); appendRaw(nBuf, &cm, sizeof(cm));
          for (const auto& sig : net.roundConfirmations()) appendRaw(nBuf, sig.data(), sig.size()); }
        const std::string confLocalSha = sha(lBuf.data(), lBuf.size());
        const std::string confNetSha   = sha(nBuf.data(), nBuf.size());
        row(ss, "sha.confirmations",
            confLocalSha + " cnt=" + std::to_string(local.roundConfirmations().size()),
            confNetSha   + " cnt=" + std::to_string(net.roundConfirmations().size()));
        const bool confDiffer = (confLocalSha != confNetSha);

        // CONFIRMATIONS_DIFF sub-dump
        if (confDiffer) {
            ss << "\n  --- CONFIRMATIONS_DIFF seq=" << seq << " ---\n";
            ss << "    local:  count=" << static_cast<int>(local.numberConfirmations())
               << "  mask=0x" << std::hex << local.roundConfirmationMask() << std::dec
               << "  sigs=" << local.roundConfirmations().size() << "\n";
            ss << "    network: count=" << static_cast<int>(net.numberConfirmations())
               << "  mask=0x" << std::hex << net.roundConfirmationMask() << std::dec
               << "  sigs=" << net.roundConfirmations().size() << "\n";

            // Per-sig comparison using confidant list as key reference
            const auto& lConf = local.confidants();
            const auto& nConf = net.confidants();
            const auto& lSigs = local.roundConfirmations();
            const auto& nSigs = net.roundConfirmations();
            const size_t maxSig = std::max(lSigs.size(), nSigs.size());
            for (size_t i = 0; i < maxSig; ++i) {
                const bool hasL = i < lSigs.size();
                const bool hasN = i < nSigs.size();
                std::string pkStr;
                if (i < lConf.size())
                    pkStr = cs::Utils::byteStreamToHex(lConf[i].data(), lConf[i].size()).substr(0, 16) + "...";
                else if (i < nConf.size())
                    pkStr = cs::Utils::byteStreamToHex(nConf[i].data(), nConf[i].size()).substr(0, 16) + "...";
                const bool same = hasL && hasN && lSigs[i] == nSigs[i];
                ss << "    [" << i << "] pk=" << pkStr
                   << (hasL ? "" : "  local:MISSING")
                   << (hasN ? "" : "  net:MISSING")
                   << (same ? "  MATCH" : (!hasL || !hasN ? "  MISSING" : "  DIFFER")) << "\n";
            }
            ss << "  --- END_CONFIRMATIONS_DIFF seq=" << seq << " ---\n";
        }
    }

    // --- sha.roundCost ---
    {
        cs::Bytes lBuf, nBuf;
        { const csdb::Amount rc = local.roundCost(); const int32_t ri = rc.integral(); const uint64_t rf = rc.fraction();
          appendRaw(lBuf, &ri, sizeof(ri)); appendRaw(lBuf, &rf, sizeof(rf)); }
        { const csdb::Amount rc = net.roundCost();   const int32_t ri = rc.integral(); const uint64_t rf = rc.fraction();
          appendRaw(nBuf, &ri, sizeof(ri)); appendRaw(nBuf, &rf, sizeof(rf)); }
        row(ss, "sha.roundCost",
            sha(lBuf.data(), lBuf.size()) + " val=" + local.roundCost().to_string(),
            sha(nBuf.data(), nBuf.size()) + " val=" + net.roundCost().to_string());
    }

    // --- sha.transactions ---
    {
        cs::Bytes lBuf, nBuf;
        for (const auto& t : local.transactions()) {
            const auto b = t.to_byte_stream_for_sig();
            appendRaw(lBuf, b.data(), b.size());
        }
        for (const auto& t : net.transactions()) {
            const auto b = t.to_byte_stream_for_sig();
            appendRaw(nBuf, b.data(), b.size());
        }
        row(ss, "sha.transactions",
            sha(lBuf.data(), lBuf.size()) + " cnt=" + std::to_string(local.transactions_count()),
            sha(nBuf.data(), nBuf.size()) + " cnt=" + std::to_string(net.transactions_count()));
    }

    // --- sha.smartSignatures ---
    {
        cs::Bytes lBuf, nBuf;
        for (const auto& ss2 : local.smartSignatures()) {
            appendRaw(lBuf, &ss2.smartConsensusPool, sizeof(ss2.smartConsensusPool));
            for (const auto& kv : ss2.signatures)
                appendRaw(lBuf, kv.second.data(), kv.second.size());
        }
        for (const auto& ss2 : net.smartSignatures()) {
            appendRaw(nBuf, &ss2.smartConsensusPool, sizeof(ss2.smartConsensusPool));
            for (const auto& kv : ss2.signatures)
                appendRaw(nBuf, kv.second.data(), kv.second.size());
        }
        row(ss, "sha.smartSignatures",
            sha(lBuf.data(), lBuf.size()) + " cnt=" + std::to_string(local.smartSignatures().size()),
            sha(nBuf.data(), nBuf.size()) + " cnt=" + std::to_string(net.smartSignatures().size()));
    }

    // --- canonical hashes ---
    ss << "  hash.local     " << local.hash().to_string() << "\n";
    ss << "  hash.network   " << net.hash().to_string() << "\n";

    // --- to_binary comparison ---
    const cs::Bytes localBin = local.to_binary();
    csdb::Pool netClone = net.clone();
    netClone.compose();
    const cs::Bytes netBin = netClone.to_binary();
    ss << "  to_binary_size local=" << localBin.size() << "  network=" << netBin.size() << "\n";
    if (localBin.size() <= 65536) {
        ss << "  to_binary_hex local   "
           << cs::Utils::byteStreamToHex(localBin.data(), localBin.size()) << "\n";
    } else {
        ss << "  to_binary_hex local   (skipped: size > 64KB)\n";
    }
    if (netBin.size() <= 65536) {
        ss << "  to_binary_hex network "
           << cs::Utils::byteStreamToHex(netBin.data(), netBin.size()) << "\n";
    } else {
        ss << "  to_binary_hex network (skipped: size > 64KB)\n";
    }

    // WALLETS_DIFF sub-dump: per-confidant wallet state when blockReward differs
    if (brDiffer) {
        ss << "\n  --- WALLETS_DIFF seq=" << seq << " (blockReward DIFFER) ---\n";
        ss << "    local blockReward hex="
           << cs::Utils::byteStreamToHex(reinterpret_cast<const uint8_t*>(localBr.data()), localBr.size()) << "\n";
        ss << "    net   blockReward hex="
           << cs::Utils::byteStreamToHex(reinterpret_cast<const uint8_t*>(netBr.data()),   netBr.size())   << "\n";
        const auto& conf = net.confidants();
        for (size_t i = 0; i < conf.size(); ++i) {
            WalletData wd;
            const bool found = findWalletData(csdb::Address::from_public_key(conf[i]), wd);
            const size_t srcN = wd.delegateSources_ ? wd.delegateSources_->size() : 0u;
            const size_t tgtN = wd.delegateTargets_ ? wd.delegateTargets_->size() : 0u;
            ss << "    [" << i << "] "
               << cs::Utils::byteStreamToHex(conf[i].data(), conf[i].size()).substr(0, 16) << "...  "
               << (found ? "found" : "MISSING")
               << "  bal="   << amtToString(wd.balance_)
               << "  deleg=" << amtToString(wd.delegated_)
               << "  srcN="  << srcN
               << "  tgtN="  << tgtN
               << "  trxN="  << wd.transNum_ << "\n";
        }
        ss << "  --- END_WALLETS_DIFF seq=" << seq << " ---\n";
    }

    // CHARACTERISTIC_DIFF sub-dump: transaction count + char mask when other
    // fields don't explain the divergence (catch-all for remaining byte diffs).
    {
        const bool anyFieldDiffer = (tsLocalSha != tsNetSha || brDiffer
            || local.transactions_count() != net.transactions_count()
            || local.smartSignatures().size() != net.smartSignatures().size());
        if (!anyFieldDiffer && localBin != netBin) {
            ss << "\n  --- CHARACTERISTIC_DIFF seq=" << seq << " (binary differs but fields match) ---\n";
            ss << "    local:  trx_count=" << local.transactions_count()
               << "  numberTrusted=" << static_cast<int>(local.numberTrusted())
               << "  realTrusted=0x" << std::hex << local.realTrusted() << std::dec << "\n";
            ss << "    network: trx_count=" << net.transactions_count()
               << "  numberTrusted=" << static_cast<int>(net.numberTrusted())
               << "  realTrusted=0x" << std::hex << net.realTrusted() << std::dec << "\n";
            ss << "  --- END_CHARACTERISTIC_DIFF seq=" << seq << " ---\n";
        }
    }

    ss << "=== END_WRITER_DIFF seq=" << seq << " ===";
    cslog() << ss.str();
}

void BlockChain::setBlocksToBeRemoved(cs::Sequence number) {
    if (blocksToBeRemoved_ > 0) {
        csdebug() << kLogPrefix << "Can't change number of blocks to be removed, because the previous removal is still not finished";
        return;
    }
    if (number > 100) {
        cslog() << "TRACE: setBlocksToBeRemoved with number=" << number << " (lastSeq=" << lastSequence_.load() << ")";
    }
    csdebug() << kLogPrefix << "Allowed NUMBER blocks to remove is set to " << blocksToBeRemoved_;
    blocksToBeRemoved_ = number;
}

namespace {
    static void serializeComparableContent(cs::ODataStream<cs::Bytes>& out, const csdb::Pool& block) {
        out << block.sequence();

        // put user fields except special timestamp field "TimestampID"
        const auto ids = block.user_field_ids();
        size_t cnt = ids.size();
        if (cnt > 0) {
            cnt -= ids.count(BlockChain::kFieldTimestamp);
            if (cnt > 0) {
                for (const auto id: ids) {
                    if (id == BlockChain::kFieldTimestamp) {
                        continue;
                    }
                    out << id;
                    const auto fld = block.user_field(id);
                    if (fld.is_valid()) {
                        switch (fld.type()) {
                        case csdb::UserField::Integer:
                            out << fld.value<int>();
                            break;
                        case csdb::UserField::Amount:
                            out << fld.value<csdb::Amount>();
                            break;
                        case csdb::UserField::String:
                            out << fld.value<std::string>();
                            break;
                        default:
                            break;
                        }
                    }
                }
            }
        }

        out << block.roundCost();

        out << block.transactions_count();
        for (const auto& t : block.transactions()) {
            out << t.to_byte_stream_for_sig();
        }

        const auto& wallets = block.newWallets();
        out << wallets.size();
        for (const auto& w : wallets) {
            union {
                csdb::Pool::NewWalletInfo::AddressId address_id;
                size_t value;
            } conv{};
            conv.address_id = w.addressId_;
            out << conv.value;
            out << w.walletId_;
        }
        const auto& confidants = block.confidants();
        for (const auto& it : confidants) {
            out << it;
        }
        out << block.previous_hash();
    }
}

/*static*/
bool BlockChain::testContentEqual(const csdb::Pool& lhs, const csdb::Pool& rhs) {
    if (lhs.is_valid() != rhs.is_valid()) {
        return false;
    }
    if (!lhs.is_valid()) {
        // both has no content
        return true;
    }

    cs::Bytes lhs_bin;
    cs::ODataStream lhs_out(lhs_bin);
    serializeComparableContent(lhs_out, lhs);
    const cs::Hash l = cscrypto::calculateHash(lhs_bin.data(), lhs_bin.size());

    cs::Bytes rhs_bin;
    cs::ODataStream rhs_out(rhs_bin);
    serializeComparableContent(rhs_out, rhs);
    const cs::Hash r = cscrypto::calculateHash(rhs_bin.data(), rhs_bin.size());

    return std::equal(l.cbegin(), l.cend(), r.cbegin());
}

void BlockChain::addIncorrectBlockNumber(cs::Sequence seq) {
    incorrectBlocks_.push_back(seq);
}

std::vector<cs::Sequence>* BlockChain::getIncorrectBlockNumbers() {
    return &incorrectBlocks_;
}

void BlockChain::showDBParams() {
    csinfo() << "last seq = " << lastSequence_ << ", db size = " << storage_.size();
}
//while caching the blocks are put out of the storeage in the with sequence decrement
void BlockChain::cacheLastBlocks() {
    csinfo() << kLogPrefix << __func__;//we have to begin with good block
    bool firstIteration = false;
    if (!antiForkMode_) {
        antiForkMode_ = true;
        firstIteration = true;
    }

    cs::Sequence lastSeq;
    while (!incorrectBlocks_.empty() || !selectionFinished_) {
        csinfo() << kLogPrefix << "Starting blocks transferring cycle";
        auto lastBlock = getLastBlock();
        lastSeq = lastBlock.sequence();
        csinfo() << kLogPrefix << "now dealing with " << lastSeq;

        if (incorrectBlocks_.back() < lastBlock.sequence()) {
            csinfo() << kLogPrefix << "incorrect block sequence not reached";//and selFin == " << (selectionFinished_?"true":"false");
        }
        else if (incorrectBlocks_.back() == lastSeq) {
            csinfo() << kLogPrefix << "Incorrect block reached, remove it form list";
            incorrectBlocks_.pop_back();
        }
        else {
            csinfo() << kLogPrefix << "Incorrect block overjumped - hmm .. look though your code better";
        }

        if (lastBlock.is_valid() && lastBlock.hash() == lastPrevHash_ || firstIteration) {
            selectionFinished_ = true;
            csinfo() << kLogPrefix << "caching block " << lastBlock.sequence();
            cachedBlocks_->insert(lastBlock, cs::PoolStoreType::Restored);
        }
        else {
            emittingRequest_ = 1;
            selectionFinished_ = false;
            csinfo() << kLogPrefix << "reached block with nonconsistant hash: orderNecessaryBlock";
            emit orderNecessaryBlock(lastPrevHash_, lastSeq);
            blocksToBeRemoved_ = 1;
            removeLastBlock();
            return;
        }

        lastPrevHash_ = lastBlock.previous_hash();
        blocksToBeRemoved_ = 1;
        removeLastBlock();
    }
    antiForkMode_ = false;
    lastPrevHash_ = csdb::PoolHash();
 }

void BlockChain::replaceCachedIncorrectBlock(const csdb::Pool& block) {
    csdebug() << kLogPrefix << __func__;
    lastPrevHash_ = block.previous_hash();
    cachedBlocks_->insert(block, cs::PoolStoreType::Synced);
    if (emittingRequest_ == 1) {
        emittingRequest_ = 0;
        cacheLastBlocks();
    }
    if (emittingRequest_ == 2) {
        emittingRequest_ = 0;
        arrangeBlocksInCache();
    }
    if (emittingRequest_ == 3) {
        emittingRequest_ = 0;
        badBlockIssue(block);
    }
}

void BlockChain::arrangeBlocksInCache() {
    csdebug() << kLogPrefix << __func__;
    if (!antiForkMode_ && cachedBlocks_->maxSequence() < getLastSeq() + 5ULL) {
        return;
    }
    if (neededCacheSeq_ == 0ULL) {
        antiForkMode_ = true;
        neededCacheSeq_ = getLastSeq() + 4ULL;
        startingBchSeq_ = getLastSeq();
    }

    while (neededCacheSeq_ > getLastSeq() || lastPrevHash_ != getLastHash()) {
        if (cachedBlocks_->contains(neededCacheSeq_)) {
            auto data = cachedBlocks_->value(neededCacheSeq_);
            csdb::Pool currentCachedBlock;
            if (data.has_value()) {
                currentCachedBlock = data.value().pool;
            }
            else {
                //TODO - think how to find the first synched block in cache !!!
                csdebug() << "No valid synced block found in cache trying to order it: " << neededCacheSeq_;
                emittingRequest_ = 2;
                emit orderNecessaryBlock(lastPrevHash_, neededCacheSeq_);
                return;
            }
            lastPrevHash_ = currentCachedBlock.previous_hash();
        }
        else {
            csdebug() << "No synced block found in cache trying to order it :" << neededCacheSeq_;
            emittingRequest_ = 2;
            emit orderNecessaryBlock(lastPrevHash_, neededCacheSeq_);
            return;
        }

        
        --neededCacheSeq_;
    }

    antiForkMode_ = false;
    lastPrevHash_ = csdb::PoolHash();
    neededCacheSeq_ = 0ULL;
    startingBchSeq_ = 0ULL;
}

void BlockChain::lookForBadBlocks() {
    if (badBlocks_.empty()) {
        return;
    }
    //TODO: explore bad blocks
    //for (auto it : badBlocks_) {

    //}
}

void BlockChain::badBlockIssue(const csdb::Pool& pool) {
    csdebug() << kLogPrefix << __func__ << ": "<<  pool.sequence();
    if (!antiForkMode_ && cachedBlocks_->maxSequence() < getLastSeq() + 5ULL) {
        csdebug() << kLogPrefix << "AntiForkMode is " << (antiForkMode_?"ON":"OFF") << ", cached blocks: "
            << cachedBlocks_->maxSequence() << ", last seq: " << getLastSeq();
        return;
    }
    cachedBlocks_->insert(pool, cs::PoolStoreType::Synced);
    if (neededCacheSeq_ == 0ULL) {
        csdebug() << kLogPrefix << "Initializing variables";
        antiForkMode_ = true;
        lastPrevHash_ = getLastHash();
        startingBchSeq_ = getLastSeq();
        neededCacheSeq_ = startingBchSeq_;
    }
    while (lastPrevHash_ == pool.previous_hash()) {
        if (cachedBlocks_->contains(neededCacheSeq_) && cachedBlocks_->value(neededCacheSeq_).has_value()) {
            csdb::Pool currentBlock = cachedBlocks_->value(neededCacheSeq_).value().pool;
            if (cachedBlocks_->maxSequence() > neededCacheSeq_ && cachedBlocks_->contains(++neededCacheSeq_)) {
                lastPrevHash_ = currentBlock.hash();
            }
        }

        else {
            emittingRequest_ = 3;
            emit orderNecessaryBlock(lastPrevHash_, neededCacheSeq_);
            return;
        }

    }
    arrangeBlocksInCache();
}

void BlockChain::getCachedMissedBlock(const csdb::Pool& block) {
    csdebug() << kLogPrefix << __func__;
    lastPrevHash_ = block.previous_hash();
    cachedBlocks_->insert(block, cs::PoolStoreType::Synced);
    cacheLastBlocks();
}

void BlockChain::setBlockReward(csdb::Amount reward) {
    blockRewardIntegral_ = reward.integral();
    blockRewardFraction_ = reward.fraction();
 
}
void BlockChain::setMiningCoefficient(csdb::Amount coefficient) {
    miningCoefficientIntegral_ = coefficient.integral();
    miningCoefficientFraction_ = coefficient.fraction();
}
void BlockChain::setMiningOn(bool mOn) {
    stakingOn_ = mOn;
}
void BlockChain::setStakingOn(bool stOn) {
    miningOn_ = stOn;
}
void BlockChain::setTimeMinStage1(uint32_t timeStage1) {
    TimeMinStage1_ = timeStage1;
}
