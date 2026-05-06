#ifndef POOLSYNCHRONIZER_HPP
#define POOLSYNCHRONIZER_HPP

#include <chrono>

#include <csdb/pool.hpp>

#include <csnode/blockchain.hpp>
#include <csnode/nodecore.hpp>

#include <lib/system/common.hpp>
#include <lib/system/timer.hpp>
#include <lib/system/signals.hpp>

#include <net/neighbourhood.hpp>

namespace cs {
using PoolSynchronizerRequestSignal = Signal<void(const PublicKey& target, const PoolsRequestedSequences& sequences)>;

class PoolSynchronizer {
public:
    explicit PoolSynchronizer(BlockChain* blockChain);

    void sync(RoundNumber roundNum, RoundNumber difference = kRoundDifferentForSync);
    void syncLastPool();
    void getBlockReply(PoolsBlock&& poolsBlock, const cs::PublicKey& sender);
    bool isSyncroStarted() const;

    // Idempotent shutdown hook. Called by Node::stop() BEFORE transport_ is
    // torn down so the watchdog timer cannot fire onTimeOut() against an
    // already-freed transport. After stop() returns, sync()/syncLastPool()/
    // onTimeOut() become no-ops; the timer is stopped and the synchroLog is
    // cleared. Safe to call multiple times.
    void stop();

    cs::Sequence getMaxNeighbourSequence();
    static const RoundNumber kRoundDifferentForSync = values::kDefaultMetaStorageMaxSize;
    void getSyncroMessage(const cs::PublicKey& sender, SyncroMessage msg);

    static const size_t kFreeBlocksTimeoutMs = 10000;
    static const size_t kCachedBlocksLimit = 10000;
    static const size_t kPerPeerCooldownMs = 5000;     // peer "busy" window after a request
    static const size_t kNoAnswerEntryTtlMs = 10000;   // NoAnswer entry GC after this

    //void trySource(cs::Sequence finSeq, cs::PublicKey& source);
    //void showNeighbours();
    //void syncTill(cs::Sequence finSeq, const cs::PublicKey& source, bool newCall);

public signals:
    PoolSynchronizerRequestSignal sendRequest;

public slots:
    void onStoreBlockTimeElapsed();
    void onPingReceived(Sequence sequence, const PublicKey& publicKey);
    void onNeighbourAdded(const PublicKey&, Sequence);
    void onNeighbourRemoved(const PublicKey& publicKey);

private slots:
    void onTimeOut();

private:
    void sendBlockRequest();
    void sendBlock(const PublicKey& neighbour, const PoolsRequestedSequences& sequences);

    void addSynchroLog(const cs::PublicKey& sender, cs::PoolsRequestedSequences& sequences, SyncroMessage msg);
    bool changeSynchroLog(const cs::PublicKey& sender, SyncroMessage msg);
    void updateSynchroLog();
    bool removeSynchroLog(const cs::PublicKey& sender);
    bool checkSynchroLog(const cs::PublicKey& sender);

    bool showSyncronizationProgress(Sequence lastWrittenSequence) const;
    void manageSyncBlocks(cs::PoolsBlock&& poolsBlock);

    void synchroFinished();
    size_t nextIndex(size_t index) const;

    BlockChain* blockChain_;

    std::atomic<bool> isSyncroStarted_ = false;
    std::atomic<bool> stopped_ = false;

    std::unordered_map<PublicKey, Sequence> neighbours_;

    // Single-threaded: all callers run via CallQueuePolicy.
    std::map<cs::PublicKey, std::tuple<cs::PoolsRequestedSequences, SyncroMessage, uint64_t>> synchroLog_;
    Timer timer_;
};
}  // namespace cs
#endif  // POOLSYNCHRONIZER_HPP
