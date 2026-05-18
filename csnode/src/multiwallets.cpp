#include "csnode/multiwallets.hpp"
#include "lib/system/common.hpp"
#include "csnode/nodecore.hpp"
#include <lib/system/logger.hpp>
#include <lib/system/utils.hpp>
#include <csnode/configholder.hpp>

#include <cstring>

namespace {

// Canonical byte serialisation of a wallet for ECMH inclusion. Layout:
//   pubkey(32)
//   balance.integral(4) balance.fraction(8)
//   delegated.integral(4) delegated.fraction(8)
//   transNum(8)
//   lastTx.pool_seq(8) lastTx.index(8)
//   delegateSources tag(1) [if 1: u32 size, then for each (key, vec): key(32) u32 vsize + N*TimeMoney(29)]
//   delegateTargets tag(1) [same as above]
//   trxTail digest bytes (137 bytes: greatest(8) | isValueSet(1) | bits(128))
//
// Empty wallet: 80 + 1 + 1 + 137 = 219 bytes. Scales with delegation count.
// All integers little-endian. Map iteration is in key order (std::map invariant),
// vector iteration in insertion order. MONITOR_NODE-only fields are excluded.
cs::Bytes serializeWalletForDigest(const cs::WalletsCache::WalletData& w) {
    cs::Bytes buf;
    buf.reserve(256);

    auto appendBytes = [&](const void* p, size_t n) {
        const auto* b = static_cast<const cscrypto::Byte*>(p);
        buf.insert(buf.end(), b, b + n);
    };
    auto appendU64 = [&](uint64_t v) {
        for (int i = 0; i < 8; ++i) {
            buf.push_back(static_cast<cscrypto::Byte>((v >> (i * 8)) & 0xFF));
        }
    };
    auto appendU32 = [&](uint32_t v) {
        for (int i = 0; i < 4; ++i) {
            buf.push_back(static_cast<cscrypto::Byte>((v >> (i * 8)) & 0xFF));
        }
    };
    auto appendI32 = [&](int32_t v) {
        appendU32(static_cast<uint32_t>(v));
    };
    auto appendAmount = [&](const csdb::Amount& a) {
        appendI32(a.integral());
        appendU64(a.fraction());
    };
    auto appendTimeMoney = [&](const cs::TimeMoney& tm) {
        appendU64(tm.initialTime);
        appendU64(tm.time);
        appendAmount(tm.amount);
        buf.push_back(static_cast<cscrypto::Byte>(tm.coeff));
    };
    auto appendDelegateMap = [&](const std::shared_ptr<std::map<cs::PublicKey, std::vector<cs::TimeMoney>>>& m) {
        if (!m) {
            buf.push_back(0);
            return;
        }
        buf.push_back(1);
        appendU32(static_cast<uint32_t>(m->size()));
        for (const auto& [key, vec] : *m) {
            appendBytes(key.data(), key.size());
            appendU32(static_cast<uint32_t>(vec.size()));
            for (const auto& tm : vec) {
                appendTimeMoney(tm);
            }
        }
    };

    // Fixed part (80 bytes)
    appendBytes(w.key_.data(), w.key_.size());
    appendAmount(w.balance_);
    appendAmount(w.delegated_);
    appendU64(w.transNum_);
    appendU64(w.lastTransaction_.pool_seq());
    appendU64(w.lastTransaction_.index());

    // Delegations
    appendDelegateMap(w.delegateSources_);
    appendDelegateMap(w.delegateTargets_);

    // Transactions tail (BitHeap canonical bytes: 137 bytes for int64_t/1024-bit)
    auto tail = w.trxTail_.serializeDigestBytes();
    appendBytes(tail.data(), tail.size());

    return buf;
}

}  // namespace

bool cs::MultiWallets::contains(const cs::PublicKey& key) const {
    cs::Lock lock(mutex_);

    auto& byKey = indexes_.get<Tags::ByPublicKey>();
    return byKey.find(key) != byKey.end();
}

size_t cs::MultiWallets::size() const {
    cs::Lock lock(mutex_);
    return indexes_.size();
}

csdb::Amount cs::MultiWallets::balance(const cs::PublicKey& key) const {
    cs::Lock lock(mutex_);

    auto& keys = indexes_.get<Tags::ByPublicKey>();
    return keys.find(key)->balance_;
}

uint64_t cs::MultiWallets::transactionsCount(const cs::PublicKey& key) const {
    cs::Lock lock(mutex_);

    auto& keys = indexes_.get<Tags::ByPublicKey>();
    return keys.find(key)->transNum_;
}

#ifdef MONITOR_NODE
uint64_t cs::MultiWallets::createTime(const cs::PublicKey& key) const {
    cs::Lock lock(mutex_);

    auto& keys = indexes_.get<Tags::ByPublicKey>();
    return keys.find(key)->createTime_;
}
#endif

bool cs::MultiWallets::getWalletData(cs::MultiWallets::InternalData& data) const {
  cs::Lock lock(mutex_);

  auto& keys = indexes_.get<Tags::ByPublicKey>();

  auto it = keys.find(data.key_);
  if (it == keys.end()) {
    return false;
  }
  
  data = *it;
  return true;
}

void cs::MultiWallets::onWalletCacheUpdated(const cs::WalletsCache::WalletData& data) {
    //csdebug() << __func__;
    cs::Lock lock(mutex_);
    auto& byKey = indexes_.get<Tags::ByPublicKey>();
    const auto& conf = cs::ConfigHolder::instance().config();
    if (conf->getBalanceChangeFlag() && data.key_ == conf->getBalanceChangeKey()) {
        csdebug() << "Wallet updated: "
	    << conf->getBalanceChangeAddress()
	    << ", balance: " << data.balance_.to_string()
	    << ", delegated: " << data.delegated_.to_string();
    }

    if (auto iter = byKey.find(data.key_); iter != byKey.end()) {
        auto oldBytes = serializeWalletForDigest(*iter);
        stateHash_.remove(oldBytes.data(), oldBytes.size());
        byKey.replace(iter, data);
    }
    else {
        indexes_.insert(data);
    }
    auto newBytes = serializeWalletForDigest(data);
    stateHash_.add(newBytes.data(), newBytes.size());
}

cscrypto::MultisetDigest cs::MultiWallets::stateDigest() const {
    cs::Lock lock(mutex_);
    return stateHash_.digest();
}

void cs::MultiWallets::rebuildStateDigest() {
    cs::Lock lock(mutex_);
    stateHash_.clear();
    auto& byKey = indexes_.get<Tags::ByPublicKey>();
    for (const auto& w : byKey) {
        auto bytes = serializeWalletForDigest(w);
        stateHash_.add(bytes.data(), bytes.size());
    }
}

void cs::MultiWallets::iterate(std::function<bool(const PublicKey& key, const InternalData& data)> func) {
    cs::Lock lock(mutex_);
    for (auto it = indexes_.begin(); it != indexes_.end(); ++it) {
        if (!func(it->key_, *it)) {
            break;
        }
    }
}

csdb::Amount cs::MultiWallets::checkWallets() {
    cs::Lock lock(mutex_);
    csdb::Amount total{ 0 };
    for (auto it = indexes_.begin(); it != indexes_.end(); ++it) {
        total += it->balance_;
        total += it->delegated_;
    }
    return total;
}
