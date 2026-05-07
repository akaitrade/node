#include <transactionsindex.hpp>

#include <algorithm>
#include <set>
#include <vector>

#include <csdb/internal/utils.hpp>
#include <csdb/pool.hpp>
#include <csdb/transaction.hpp>
#include <lib/system/logger.hpp>
#include <csnode/blockchain.hpp>
#include <csnode/transactionsiterator.hpp>

namespace {
constexpr const char* kDbPath = "/indexdb";
// Reserved keys; lengths can't collide with the 40-byte (pubkey + sequence) entries.
constexpr const char* kLastIndexedKey = "__last_indexed__";
constexpr const char* kIncompleteKey  = "__incomplete__";
constexpr const char* kCompleteKey    = "__walked_complete__";

auto getTrxIndexKey(const cs::PublicKey& _pubKey, cs::Sequence _seq) {
    cs::Bytes ret(_pubKey.begin(), _pubKey.end());
    ret.resize(ret.size() + sizeof(_seq));

    auto ptr = reinterpret_cast<uint8_t*>(&_seq);
    std::copy(ptr, ptr + sizeof(_seq), ret.begin() + _pubKey.size());
    return ret;
}
} // namespace

namespace cs {

TransactionsIndex::TransactionsIndex(BlockChain& _bc, const std::string& _path, bool _recreate)
    : bc_(_bc),
      rootPath_(_path),
      db_(std::make_unique<Lmdb>(_path + kDbPath)) {
    // last_indexed lives inside the same LMDB as the index entries (replaces
    // the old mmap'd /last_indexed file, which was not crash-safe and went
    // stale even on clean shutdown — the OS lazily flushed Boost mapped_file
    // pages, so on restart its value lagged the LMDB and forced a full reindex).
    init();
    if (_recreate || !loadLastIndexedFromDb()) {
        recreate_ = true;
    }
    loadIncompleteFromDb();
    loadCompleteFlagFromDb();
    if (indexIncomplete_) {
        cslog() << "TRACE: trxIndex __incomplete__ flag set on load — Trusted role gated until slow-start rebuilds the index";
    }
    if (!indexComplete_ && !recreate_ && lastIndexedPool_ >= 1000) {
        cslog() << "TRACE: trxIndex has no completion sentinel — pre-patch indexdb suspected, will be rebuilt by slow-start";
    }
}

bool TransactionsIndex::recreate() const {
    return recreate_;
}

bool TransactionsIndex::isReady() const {
    if (indexIncomplete_) return false;
    if (lastIndexedPool_ == kWrongSequence) return false;
    if (lastIndexedPool_ < 1000) return true;
    return indexComplete_;
}

bool TransactionsIndex::looksEmpty() const {
    if (!db_->isOpen()) return false;
    if (lastIndexedPool_ == kWrongSequence || lastIndexedPool_ < 1000) return false;
    return !indexComplete_;
}

void TransactionsIndex::forceRebuild() {
    lastIndexedPool_ = kWrongSequence;
    recreate_ = true;
    if (indexIncomplete_) {
        indexIncomplete_ = false;
        updateIncompleteFlag();
    }
    if (indexComplete_) {
        indexComplete_ = false;
        updateCompleteFlag();
    }
    updateLastIndexed();
}

void TransactionsIndex::onStartReadFromDb(Sequence _lastWrittenPoolSeq) {
    if (recreate_) {
        return;
    }
    if (lastIndexedPool_ == kWrongSequence) {
        return;
    }
    if (lastIndexedPool_ != _lastWrittenPoolSeq) {
        cslog() << "TRACE: trxIndex onStartReadFromDb lastIndexedPool=" << lastIndexedPool_
                << " lastWrittenPoolSeq=" << _lastWrittenPoolSeq
                << " gap=" << (static_cast<long long>(lastIndexedPool_)
                               - static_cast<long long>(_lastWrittenPoolSeq));
    }
    if (lastIndexedPool_ <= _lastWrittenPoolSeq) {
        return;
    }
    constexpr Sequence kMaxRollbackGap = 10000;
    const Sequence aheadGap = lastIndexedPool_ - _lastWrittenPoolSeq;
    if (aheadGap > kMaxRollbackGap) {
        cslog() << "TRACE: trxIndex gap " << aheadGap << " > " << kMaxRollbackGap << ", recreate";
        recreate_ = true;
        return;
    }
    cslog() << "TRACE: trxIndex roll back lastIndexedPool " << lastIndexedPool_
            << " -> " << _lastWrittenPoolSeq;
    lastIndexedPool_ = _lastWrittenPoolSeq;
    updateLastIndexed();
    if (db_->isOpen()) {
        db_->flush();
    }
}

void TransactionsIndex::onReadFromDb(const csdb::Pool& _pool) {
    if (_pool.sequence() == 0 && recreate_) {
        reset();
        init();
    }

    if (recreate_ || lastIndexedPool_ < _pool.sequence()) {
        updateFromNextBlock(_pool);
    }
}

void TransactionsIndex::onDbReadFinished() {
    if (recreate_) {
        recreate_ = false;
        if (indexIncomplete_) {
            indexIncomplete_ = false;
            updateIncompleteFlag();
            cslog() << "trxIndex __incomplete__ cleared by slow-start — Trusted role unblocked";
        }
        if (!indexComplete_) {
            indexComplete_ = true;
            updateCompleteFlag();
            cslog() << "trxIndex marked complete by slow-start walk";
        }
        lapoos_.clear();
        cslog() << "Recreated index 0 -> " << lastIndexedPool_
                << ". Continue to keep it actual from new blocks.";
    }
    else {
        updateLastIndexed();
    }
}

void TransactionsIndex::onRemoveBlock(const csdb::Pool& _pool) {
    std::set<csdb::Address> uniqueAddresses;
    std::vector<std::pair<cs::PublicKey, csdb::TransactionID>> updates;

    auto lbd = [&_pool, &updates, &uniqueAddresses, this](const csdb::Address& _addr, cs::Sequence _sq) {
        auto key = bc_.getAddressByType(_addr, BlockChain::AddressType::PublicKey);

        if (uniqueAddresses.insert(key).second) {
            auto it = TransactionsIterator(bc_, _addr, _pool);
            bool found = false;

            for (; it.isValid(); it.next()) {
                if (it->id().pool_seq() < _sq) {
                    updates.push_back(std::make_pair(key.public_key(), it->id()));
                    found = true;
                    break;
                }
            }
            if (!found) {
                updates.push_back(std::make_pair(key.public_key(),
                                                 csdb::TransactionID(kWrongSequence, kWrongSequence)));
            }
            removeLastTransBlock(key.public_key(), _sq);
        }
    };

    for (const auto& t : _pool.transactions()) {
        lbd(t.source(), lastIndexedPool_);
        lbd(t.target(), lastIndexedPool_);
    }
    --lastIndexedPool_;
    updateLastIndexed();

    if (updates.size()) {
        bc_.updateLastTransactions(updates);
    }
}

void TransactionsIndex::update(const csdb::Pool& _pool) {
    updateFromNextBlock(_pool);
}

void TransactionsIndex::invalidate() {
    lastIndexedPool_ = kWrongSequence;
    updateLastIndexed();
}

void TransactionsIndex::close() {
    if (db_->isOpen()) {
        db_->flush();
        db_->close();
    }
}

void TransactionsIndex::flush() {
    if (db_->isOpen()) {
        db_->flush();
    }
}

void TransactionsIndex::pinFloor(Sequence floor) {
    if (floor == kWrongSequence) {
        return;
    }
    if (lastIndexedPool_ == kWrongSequence || lastIndexedPool_ < floor) {
        const Sequence prev = (lastIndexedPool_ == kWrongSequence) ? 0 : lastIndexedPool_;
        cslog() << "TRACE: trxIndex pinFloor lifting lastIndexedPool from "
                << lastIndexedPool_ << " to " << floor;
        // gap left unwalked → consensus would propose a divergent trx set. gate Trusted.
        if (recreate_ || prev + 1 < floor) {
            indexIncomplete_ = true;
            cslog() << "TRACE: trxIndex marked __incomplete__ — Trusted role gated; "
                       "wipe " << rootPath_ << kDbPath << " and restart for slow-start to clear";
            updateIncompleteFlag();
        }
        lastIndexedPool_ = floor;
        recreate_ = false;
        updateLastIndexed();
        if (db_->isOpen()) {
            db_->flush();
        }
    }
}

void TransactionsIndex::updateFromNextBlock(const csdb::Pool& _pool) {
    std::set<csdb::Address> indexedAddrs;

    auto lbd = [&indexedAddrs, &_pool, this](const csdb::Address& _addr) {
        auto key = bc_.getAddressByType(_addr, BlockChain::AddressType::PublicKey);

        if (indexedAddrs.insert(key).second) {
            Sequence lapoo;
            if (recreate_) {
                auto it = lapoos_.find(key);
                if (it == lapoos_.end()) {
                    lapoo = cs::kWrongSequence;
                }
                else {
                    lapoo = it->second;
                }
                lapoos_[key] = _pool.sequence();
            }
            else {
                lapoo = bc_.getLastTransaction(key).pool_seq();
            }

            if (lapoo != _pool.sequence()) {
                setPrevTransBlock(key.public_key(), _pool.sequence(), lapoo);
            }
            else {
                cserror() << "Attempt to make trx index inconsistent, curr pool num is "
                          << _pool.sequence() << ", prev pool num is " << lapoo
                          << ". For public key: "
                          << EncodeBase58(key.public_key().data(), key.public_key().data() + key.public_key().size())
                          << ", recreate status is " << recreate_;
            }
        }
    };

    for (auto& tr : _pool.transactions()) {
        lbd(tr.source());
        lbd(tr.target());
    }

    lastIndexedPool_ = _pool.sequence();
    updateLastIndexed();
}

void TransactionsIndex::setPrevTransBlock(const PublicKey& _pubKey, cs::Sequence _curr, cs::Sequence _prev) {
    db_->insert(getTrxIndexKey(_pubKey, _curr), _prev);
}

void TransactionsIndex::removeLastTransBlock(const PublicKey& _pubKey, cs::Sequence _curr) {
    db_->remove(getTrxIndexKey(_pubKey, _curr));
}

Sequence TransactionsIndex::getPrevTransBlock(const csdb::Address& _addr, Sequence _prev) const {
    auto key = getTrxIndexKey(bc_.getAddressByType(_addr, BlockChain::AddressType::PublicKey).public_key(), _prev);
    if (db_->size() == 0 || !db_->isKeyExists(key)) {
        return kWrongSequence;
    }
    return db_->value<Sequence>(key);
}

bool TransactionsIndex::loadLastIndexedFromDb() {
    if (!db_->isOpen() || !db_->isKeyExists(kLastIndexedKey)) {
        return false;
    }
    cs::Sequence v = db_->value<cs::Sequence>(kLastIndexedKey);
    if (v == kWrongSequence) {
        return false;
    }
    lastIndexedPool_ = v;
    return true;
}

inline void TransactionsIndex::updateLastIndexed() {
    if (db_->isOpen()) {
        db_->insert(kLastIndexedKey, lastIndexedPool_);
    }
}

void TransactionsIndex::loadIncompleteFromDb() {
    if (!db_->isOpen() || !db_->isKeyExists(kIncompleteKey)) {
        indexIncomplete_ = false;
        return;
    }
    indexIncomplete_ = (db_->value<uint8_t>(kIncompleteKey) != 0);
}

void TransactionsIndex::updateIncompleteFlag() {
    if (db_->isOpen()) {
        db_->insert(kIncompleteKey, static_cast<uint8_t>(indexIncomplete_ ? 1 : 0));
        db_->flush();
    }
}

void TransactionsIndex::loadCompleteFlagFromDb() {
    if (!db_->isOpen() || !db_->isKeyExists(kCompleteKey)) {
        indexComplete_ = false;
        return;
    }
    indexComplete_ = (db_->value<uint8_t>(kCompleteKey) != 0);
}

void TransactionsIndex::updateCompleteFlag() {
    if (db_->isOpen()) {
        db_->insert(kCompleteKey, static_cast<uint8_t>(indexComplete_ ? 1 : 0));
        db_->flush();
    }
}

void TransactionsIndex::onDbFailed(const LmdbException& e) {
    cswarning() << csfunc() << ", transactions index database exception " << e.what();
}

inline void TransactionsIndex::init() {
    Connector::connect(&db_->failed, this, &TransactionsIndex::onDbFailed);

    db_->setMapSize(cs::Lmdb::Default1GbMapSize);
    db_->open();
}

inline void TransactionsIndex::reset() {
    Connector::disconnect(&db_->failed, this, &TransactionsIndex::onDbFailed);
    db_->close();
    db_.reset(nullptr);
    csdb::internal::path_remove(rootPath_ + kDbPath);
    db_ = std::make_unique<Lmdb>(rootPath_ + kDbPath);
}
} // namespace cs
