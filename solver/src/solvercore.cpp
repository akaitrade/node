#include <smartcontracts.hpp>
#include <solvercontext.hpp>
#include <solvercore.hpp>
#include <states/nostate.hpp>


#pragma warning(push)
#pragma warning(disable : 4267 4244 4100 4245)
#include <csnode/node.hpp>
#pragma warning(pop)

#include <csnode/datastream.hpp>
#include <csnode/walletsstate.hpp>
#include <lib/system/logger.hpp>

#include <csnode/blockchain.hpp>
#include <lib/system/utils.hpp>

#include <cstdlib>
#include <functional>
#include <limits>
#include <sstream>
#include <string>
#include <string_view>
#include <list>
#include <iomanip>

namespace
{
    const char* log_prefix = "SolverCore: ";
}

namespace cs {

// initial values for SolverCore options

// To track timeout for active state
constexpr const bool TimeoutsEnabled = false;
// To enable perform a transition to the same state
constexpr const bool RepeatStateEnabled = true;
// Special mode: uses debug transition table
constexpr const bool DebugModeOn = false;
// Special mode: uses monitor mode transition table
constexpr const bool MonitorModeOn =
#if defined(MONITOR_NODE)
    true;
#else
    false;
#endif  // MONITOR_NODE

constexpr const bool WebWalletModeOn =
#if defined(WEB_WALLET_NODE) && false
    true;
#else
    false;
#endif  // WEB_WALLET_NODE

// default (test intended) constructor
SolverCore::SolverCore()
// options
: opt_timeouts_enabled(TimeoutsEnabled)
, opt_repeat_state_enabled(RepeatStateEnabled)
, opt_mode(Mode::Default)
// inner data
, pcontext(std::make_unique<SolverContext>(*this))
, tag_state_expired(CallsQueueScheduler::no_tag)
, req_stop(true)
, pnode(nullptr)
, pws(nullptr)
, psmarts(nullptr)

/*, smartProcess_(this)*/ {
    if constexpr (MonitorModeOn) {
        cslog() << log_prefix << "opt_monitor_mode is on, so use special transition table";
        InitMonitorModeTransitions();
    }
    else if constexpr (WebWalletModeOn) {
        cslog() << log_prefix << "opt_web_wallet_mode is on, so use special transition table";
        InitWebWalletModeTransitions();
    }
    else if constexpr (DebugModeOn) {
        cslog() << log_prefix << "opt_debug_mode is on, so use special transition table";
        InitDebugModeTransitions();
    }
    else if constexpr (true) {
        cslog() << log_prefix << "use default transition table";
        InitTransitions();
    }
}

// actual constructor
SolverCore::SolverCore(Node* pNode, csdb::Address GenesisAddress, csdb::Address StartAddress)
: SolverCore() {
    addr_genesis = GenesisAddress;
    addr_start = StartAddress;
    pnode = pNode;
    auto& bc = pNode->getBlockChain();
    pws = std::make_unique<cs::WalletsState>(bc.getCacheUpdater());
    psmarts = std::make_unique<cs::SmartContracts>(bc, scheduler);
    if (!pVal_) {
        pVal_ = std::make_unique<IterValidator>(pcontext->wallets());
    }
}

SolverCore::~SolverCore() {
    scheduler.Stop();
    transitions.clear();
}

void SolverCore::ExecuteStart(Event start_event) {
    if (!is_finished()) {
        cswarning() << log_prefix << "cannot start again, already started";
        return;
    }
    req_stop = false;
    handleTransitions(start_event);
}

void SolverCore::finish() {
    if (pstate) {
        pstate->off(*pcontext);
    }
    scheduler.RemoveAll();
    tag_state_expired = CallsQueueScheduler::no_tag;
    pstate = std::make_shared<NoState>();
    req_stop = true;
}

void SolverCore::setState(const StatePtr& pState) {
    if (!opt_repeat_state_enabled) {
        if (pState == pstate) {
            return;
        }
    }
    if (tag_state_expired != CallsQueueScheduler::no_tag) {
        // no timeout, cancel waiting
        scheduler.Remove(tag_state_expired);
        tag_state_expired = CallsQueueScheduler::no_tag;
    }
    else {
        // state changed due timeout from within expired state
    }

    if (pstate) {
        csdetails() << log_prefix << "pstate-off";
        pstate->off(*pcontext);
    }
    if (Consensus::Log) {
        csdebug() << log_prefix << "switch " << (pstate ? pstate->name() : "null") << " -> " << (pState ? pState->name() : "null");
    }
    pstate = pState;
    if (!pstate) {
        return;
    }
    pstate->on(*pcontext);

    auto closure = [this]() {
        csdebug() << log_prefix << "state " << pstate->name() << " is expired";
        // clear flag to know timeout expired
        tag_state_expired = CallsQueueScheduler::no_tag;
        // control state switch
        std::weak_ptr<INodeState> p1(pstate);
        pstate->expired(*pcontext);
        if (pstate == p1.lock()) {
            // expired state did not change to another one, do it now
            csdebug() << log_prefix << "there is no state set on expiration of " << pstate->name();
            // setNormalState();
        }
    };

    // timeout handling
    if (opt_timeouts_enabled) {
        tag_state_expired = scheduler.InsertOnce(Consensus::DefaultStateTimeout, closure, true /*replace if exists*/);
    }
}

void SolverCore::handleTransitions(Event evt) {
    if (!pstate) {
        // unable to work until initTransitions() called
        return;
    }
    if (Event::BigBang == evt) {
        cswarning() << log_prefix << "Bootstrap on";
    }
    const auto& variants = transitions[pstate];
    if (variants.empty()) {
        cserror() << log_prefix << "there are no transitions for " << pstate->name();
        return;
    }
    auto it = variants.find(evt);
    if (it == variants.cend()) {
        // such event is ignored in current state
        csdebug() << log_prefix << "event " << static_cast<int>(evt) << " ignored in state " << pstate->name();
        return;
    }
    setState(it->second);
}

bool SolverCore::stateCompleted(Result res) {
    if (Result::Failure == res) {
        cserror() << log_prefix << "error in state " << (pstate ? pstate->name() : "null - Consensus state can't be completed. Trying to resolve ... ");
    }
    return (Result::Finish == res);
}

bool SolverCore::stateFailed(Result res) {
    if (Result::Failure == res) {
        cserror() << log_prefix << "error in state " << (pstate ? pstate->name() : "null - Consensus state can't be completed. Trying to resolve ... ");
        return true;
    }
    return false;
}

// void SolverCore::adjustTrustedCandidates(cs::Bytes mask, cs::PublicKeys& confidants) {
//  for (int i = 0; i < mask.size(); ++i) {
//    if (mask[i] == cs::ConfidantConsts::InvalidConfidantIndex) {
//      auto it = std::find(trusted_candidates.cbegin(), trusted_candidates.cend(), confidants[i]);
//      if (it != trusted_candidates.cend()) {
//        trusted_candidates.erase(it);
//      }
//    }
//  }
//}

uint64_t SolverCore::lastTimeStamp() {
    int64_t lts;
    try {
        lts = std::stoll(pnode->getBlockChain().getLastTimeStamp());
    }
    catch (...) {
        csdebug() << "Timestamp was announced as zero";
        return 0;
    }
    return lts;
}

std::string SolverCore::chooseTimeStamp(cs::Bytes mask) {
    static const bool s_recomp = [] {
        const char* v = std::getenv("CS_DEBUG_RECOMPUTE");
        return v && *v && std::string_view(v) != "0";
    }();
    // Use frozen snapshot if set this round; fall back to live storage otherwise.
    const auto& src = !stageOneSnapshot.empty() ? stageOneSnapshot : stageOneStorage;
    std::ostringstream tsd;
    if (s_recomp) {
        tsd << "\n=== CHOOSE_TS_DUMP seq=" << (pnode->getBlockChain().getLastSeq() + 1)
            << " mask=" << cs::Utils::byteStreamToHex(mask.data(), mask.size())
            << " src=" << (!stageOneSnapshot.empty() ? "snapshot" : "live")
            << " src.size=" << src.size()
            << " (live=" << stageOneStorage.size() << ")\n";
    }
    int64_t lastTimeStamp;
    try {
      lastTimeStamp = std::stoll(pnode->getBlockChain().getLastTimeStamp());
    } catch(...) {
        csdebug() << "ChooseTimeStamp - Timestamp was announced as zero";
      return std::to_string(0);
    }
    if (s_recomp) tsd << "  lastTimeStamp=" << lastTimeStamp << "\n";

    std::list<double> stamps;
    double sx = 0, sx2 = 0;
    double mean = 0.0;
    int N = 0;
    for (auto& it : src) {
        if (it.sender >= cs::Conveyer::instance().confidantsCount()) {
            if (s_recomp) tsd << "  sender=" << (int)it.sender << " out-of-range, skip\n";
            continue;
        }
        const bool maskOK = mask[it.sender] != cs::ConfidantConsts::InvalidConfidantIndex;
        const bool tsOK = !it.roundTimeStamp.empty();
        if (s_recomp) {
            tsd << "  sender=" << (int)it.sender
                << " mask[" << (int)it.sender << "]=" << (int)mask[it.sender]
                << " ts=\"" << it.roundTimeStamp << "\""
                << " tsOK=" << tsOK << " maskOK=" << maskOK;
        }
        if (!it.roundTimeStamp.empty() && mask[it.sender] != cs::ConfidantConsts::InvalidConfidantIndex) {
            int64_t tStamp;
            try {
                tStamp = std::stoll(it.roundTimeStamp);
            } catch(...) {
                if (s_recomp) tsd << " PARSE_FAIL\n";
                cswarning() << log_prefix << "incompatible timestamp received from [" << (int)it.sender << "]";
                continue;
            }
            if (tStamp > lastTimeStamp) {
                double x = static_cast<double>(tStamp - lastTimeStamp);
                if (x > DBL_EPSILON) {
                    stamps.push_back(x);
                    sx += x;
                    sx2 += x * x;
                    ++N;
                    if (s_recomp) tsd << " counted x=" << x << "\n";
                } else if (s_recomp) {
                    tsd << " skip(x<eps)\n";
                }
            } else if (s_recomp) {
                tsd << " skip(tStamp<=lastTS,diff=" << (tStamp - lastTimeStamp) << ")\n";
            }
        } else if (s_recomp) {
            tsd << " NOT_COUNTED\n";
        }
    }
    if (s_recomp) tsd << "  pre-drop N=" << N << " sx=" << sx << "\n";

    bool isDrop = true;
    while (isDrop) {
        if (N == 0) {
            csdebug() << "There is no nodes with valid TimeStamp";
            int64_t N0 = 0;
            int64_t sx0 = 0;
            for (auto& it : src) {
                int64_t tStamp;
                try {
                    tStamp = std::stoull(it.roundTimeStamp);
                }
                catch (...) {
                    cswarning() << log_prefix << "incompatible timestamp received from [" << (int)it.sender << "]";
                    continue;
                }
                ++N0;
                sx0 += static_cast<int64_t>(tStamp);
            }

            if (N0 > 0) {
                return std::to_string(sx0/N0);
            }
            else {
                return std::to_string(lastTimeStamp + 1);
            }

        }
        double N_1 = 1. / N;
        mean = sx * N_1;
        double disp = sx2 * N_1 - mean * mean;
        isDrop = false;
        for (auto it = stamps.begin(), end = stamps.end(); it != end;) {
            double x = *it;
            auto it_ = it++;
            double value = x - mean;
            if (value * value >  9. * disp) {
                sx -= x; sx2 -= x * x; --N;
                stamps.erase(it_);
                isDrop = true;
                if (s_recomp) tsd << "  dropped outlier x=" << x << " mean=" << mean << "\n";
            }
        }
    }

    auto meanTimeStamp = static_cast<int64_t>(lastTimeStamp + mean + 0.5);
    //csdebug() << "finish: " << meanTimeStamp;
    if (s_recomp) {
        tsd << "  post-drop N=" << N << " mean=" << mean
            << " result=" << meanTimeStamp << "\n";
        tsd << "=== END_CHOOSE_TS_DUMP ===";
        cslog() << tsd.str();
    }
    return std::to_string(meanTimeStamp);
}


std::string SolverCore::setBlockReward(csdb::Pool& defBlock, const cs::Bytes& realTrusted) {
    auto confidants = defBlock.confidants();
    //csdebug() << "setBlockReward - conf num: " << confidants.size();
    csdb::Amount totalFee = defBlock.roundCost();
    //csdebug() << "setBlockReward - round cost: " << totalFee.to_string();
    if (totalFee == csdb::Amount{ 0 }) {
        for (auto& tr : defBlock.transactions()) {
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
        pnode->getBlockChain().findWalletData(csdb::Address::from_public_key(confidants[i]), wData);
        totalNodeStake += wData.balance_;
        //csdebug() << "setBlockReward - applying to: " << cs::Utils::byteStreamToHex(confidants[i]);
        //csdebug() << "setBlockReward - node balance added: " << totalNodeStake.to_string();
        nodeConfidantAndStake += wData.balance_;
        if (wData.delegateSources_ != nullptr && wData.delegateSources_->size() > 0) {
            for (auto& keyAndStake : *(wData.delegateSources_)) {
                for(auto& tm : keyAndStake.second){
                    if (tm.coeff == StakingCoefficient::NoStaking) {
                        nodeConfidantAndStake += tm.amount;
                        //csdebug() << "setBlockReward - simple delegation added: " << tm.amount.to_string();
                    }
                    else {
                        nodeConfidantAndFreezenStake += tm.amount * pnode->getBlockChain().getStakingCoefficient(tm.coeff);
                        //csdebug() << "setBlockReward - time delegation added: " << tm.amount.to_string() << " as " << nodeConfidantAndFreezenStake.to_string();
                    }

                }
            }
        }
        totalNodeStake = nodeConfidantAndStake + nodeConfidantAndFreezenStake;
        //csdebug() << "setBlockReward - total node stake: " << totalNodeStake.to_string();

        csdb::Amount totaNodeCutStake = totalNodeStake;
        if (totaNodeCutStake > Consensus::MaxStakeValue) {
            totaNodeCutStake = Consensus::MaxStakeValue;
        }

        confidantAndStake.push_back((nodeConfidantAndStake * pnode->getBlockChain().getStakingCoefficient(StakingCoefficient::NoStaking) + nodeConfidantAndFreezenStake) * (totalNodeStake > csdb::Amount{ 1 } ? totaNodeCutStake / totalNodeStake : csdb::Amount{ 1 }));
        totalStake += confidantAndStake[i];
        //csdebug() << "setBlockReward - final confAndStake: " << confidantAndStake[i].to_string();
    }

    csdb::Amount minedValue = defBlock.sequence() < Consensus::StartingDPOS ? csdb::Amount{ 0 } : totalFee * Consensus::miningCoefficient + Consensus::blockReward;
    //csdb::Amount onePartOfFee = feeOnly / realTrustedNumber;
    if (totalStake < csdb::Amount{ 1 }) {
        totalStake = csdb::Amount{ 1 };
    }
    csdb::Amount oneMiningPart = Consensus::stakingOn ? minedValue / totalStake : csdb::Amount{ 0 };
    //csdb::Amount payedFee = 0;
    csdb::Amount payedReward = 0;
    size_t numPayedTrusted = 0;
    cs::Bytes fldBytes;
    cs::ODataStream stream(fldBytes);
    //csdebug() << "setBlockReward - minedValue: " << minedValue.to_string() << ", " << oneMiningPart.to_string();
    for (size_t i = 0; i < confidants.size(); ++i) {
        csdb::Amount rewardToPay = 0;
        if (realTrusted[i] != kUntrustedMarker){
            if (numPayedTrusted == realTrustedNumber - 1) {
                rewardToPay = minedValue - payedReward;
            }
            else {
                rewardToPay = oneMiningPart * confidantAndStake[i];
            }
            ++numPayedTrusted;
        }
        
        if (rewardToPay > minedValue || rewardToPay < csdb::Amount{ 0 }) {
            cserror() << "setBlockReward -> reward value beyond range: " << rewardToPay.to_string() << " - reduced to zero";
            rewardToPay = csdb::Amount{ 0 };
        }
        stream << rewardToPay.integral() << rewardToPay.fraction();
        //csdebug() << "setBlockReward -> " << cs::Utils::byteStreamToHex(confidants[i]) << ", Mined: " << rewardToPay.to_string();

        payedReward += rewardToPay;
        if (rewardToPay > minedValue || rewardToPay < csdb::Amount{ 0 }) {
            cserror() << "setBlockReward -> total payed reward value beyond range: " << payedReward.to_string();
        }
        
    }
    std::string fld(fldBytes.begin(), fldBytes.end());
    //csdebug() << "setBlockReward - final string: " << cs::Utils::byteStreamToHex(fld.data(), fld.size());
    return fld;

    
}

// TODO: this function is to be implemented the block and RoundTable building <====
void SolverCore::spawn_next_round(const cs::PublicKeys& nodes, const cs::PacketsHashes& hashes, std::string&& currentTimeStamp, cs::StageThree& stage3) {
    //csmeta(csdetails) << "start";
    cs::Conveyer& conveyer = cs::Conveyer::instance();
    if (conveyer.roundTable(conveyer.currentRoundNumber()) == nullptr) {
        // test if is full round data available (metaStarage[r] + roundtable in metastorage[r])
        return;
    }
    cs::RoundTable table;
    //TODO: place here adjustTrustedCandidates() call
    table.round = conveyer.currentRoundNumber() + 1;
    table.confidants = nodes;
    table.hashes = hashes;
    justCreatedRoundPackage_.updateRoundTable(table);
    csdetails() << log_prefix << "applying " << hashes.size() << " hashes to ROUND Table";

    // only for new consensus
    cs::PoolMetaInfo poolMetaInfo;
    std::string timeStamp = conveyer.currentRoundNumber() == 1 ? currentTimeStamp : chooseTimeStamp(stage3.realTrustedMask);
    poolMetaInfo.sequenceNumber = pnode->getBlockChain().getLastSeq() + 1;
    poolMetaInfo.timestamp = std::move(timeStamp);
    auto ptr = conveyer.characteristic(conveyer.currentRoundNumber());
    if (ptr != nullptr) {
        poolMetaInfo.characteristic.mask = ptr->mask;
    }
    //const auto confirmation = pnode->getConfirmation(conveyer.currentRoundNumber());
    //if (confirmation.has_value()) {
    //    poolMetaInfo.confirmationMask = confirmation.value().mask;
    //    poolMetaInfo.confirmations = confirmation.value().signatures;
    //}

    csdebug() << log_prefix << "timestamp: " << poolMetaInfo.timestamp;
    for (std::size_t i = 0; i < hashes.size(); ++i) {
        csdetails() << log_prefix << '\t' << i << ". " << hashes[i].toString();
    }

    //if (stage3.sender != cs::ConfidantConsts::InvalidConfidantIndex) {
    //    const cs::ConfidantsKeys& confidants = conveyer.confidants();
    //    if (stage3.writer < confidants.size()) {
    //        poolMetaInfo.writerKey = confidants[stage3.writer];
    //    }
    //    else {
    //        cserror() << log_prefix << "stage-3 writer index: " << static_cast<int>(stage3.writer)
    //            << ", out of range is current confidants size: " << confidants.size();
    //    }
    //}

    poolMetaInfo.realTrustedMask = stage3.realTrustedMask;
    poolMetaInfo.previousHash = pnode->getBlockChain().getLastHash();

    cs::Bytes confirmationMask;
    cs::Signatures confirmations;
    // TODO: in this method we delete the local hashes - so if we need to rebuild thid pool again from the roundTable it's impossible
    uint32_t binSize = 0;
    if (stage3.iteration == 0) {
        std::optional<csdb::Pool> pool = conveyer.applyCharacteristic(poolMetaInfo);

        if (!pool.has_value()) {
            cserror() << log_prefix << "applyCharacteristic() failed to create block";
            return;
        }
//        uploadNewStates(conveyer.uploadNewStates());
        deferredBlock_ = std::move(pool.value());
        deferredBlock_.set_confidants(conveyer.confidants());
        
        //csmeta(csdebug) << "block #" << deferredBlock_.sequence() << " add new wallets to pool";
        pnode->getBlockChain().addNewWalletsToPool(deferredBlock_);
        pnode->getBlockChain().setTransactionsFees(deferredBlock_);
        if (deferredBlock_.sequence() > Consensus::StartingDPOS && Consensus::miningOn) {
            poolMetaInfo.reward = setBlockReward(deferredBlock_, poolMetaInfo.realTrustedMask);
            //csdebug() << log_prefix << "Reward: " << cs::Utils::byteStreamToHex(poolMetaInfo.reward.data(), poolMetaInfo.reward.size());
            deferredBlock_.add_user_field(BlockChain::kFieldBlockReward, poolMetaInfo.reward);
        }
        justCreatedRoundPackage_.updatePoolMeta(poolMetaInfo);
        
        const auto confirmation = pnode->getConfirmation(conveyer.currentRoundNumber());
        if (confirmation.has_value()) {
            confirmationMask = confirmation.value().mask;
            confirmations = confirmation.value().signatures;
        }
        if (justCreatedRoundPackage_.poolMetaInfo().sequenceNumber > 1) {
            deferredBlock_.add_number_confirmations(static_cast<uint8_t>(confirmationMask.size()));
            deferredBlock_.add_confirmation_mask(cs::Utils::maskToBits(confirmationMask));
            deferredBlock_.add_round_confirmations(confirmations);
        }
    }
    else {
        csdb::Pool tmpPool;
        tmpPool.set_sequence(deferredBlock_.sequence());
        tmpPool.set_previous_hash(deferredBlock_.previous_hash());
        tmpPool.add_real_trusted(cs::Utils::maskToBits(stage3.realTrustedMask));
        tmpPool.add_number_trusted(static_cast<uint8_t>(stage3.realTrustedMask.size()));

        tmpPool.setRoundCost(deferredBlock_.roundCost());
        tmpPool.set_confidants(deferredBlock_.confidants());
        for (auto& it : deferredBlock_.transactions()) {
            tmpPool.add_transaction(it);
        }


        if (tmpPool.sequence() > Consensus::StartingDPOS && Consensus::miningOn) {
            poolMetaInfo.reward = setBlockReward(tmpPool, poolMetaInfo.realTrustedMask);
            //csdebug() << log_prefix << "Reward: " << cs::Utils::byteStreamToHex(poolMetaInfo.reward.data(), poolMetaInfo.reward.size());
            tmpPool.add_user_field(BlockChain::kFieldBlockReward, poolMetaInfo.reward);
        }
        justCreatedRoundPackage_.updatePoolMeta(poolMetaInfo);
        BlockChain::setTimestamp(tmpPool, justCreatedRoundPackage_.poolMetaInfo().timestamp);

        for (auto& it : deferredBlock_.smartSignatures()) {
            tmpPool.add_smart_signature(it);
        }

        csdb::Pool::NewWallets* newWallets = tmpPool.newWallets();
        csdb::Pool::NewWallets* defWallets = deferredBlock_.newWallets();
        if (!newWallets) {
            cserror() << log_prefix << "newPool is read-only";
            return;
        }
        for (auto& it : *defWallets) {
            newWallets->push_back(it);
        }
        const auto confirmation = pnode->getConfirmation(conveyer.currentRoundNumber());
        if (confirmation.has_value()) {
            confirmationMask = confirmation.value().mask;
            confirmations = confirmation.value().signatures;
        }
        if (poolMetaInfo.sequenceNumber > 1 && !pnode->isBootstrapRound()) {
            tmpPool.add_number_confirmations(static_cast<uint8_t>(confirmationMask.size()));
            tmpPool.add_confirmation_mask(cs::Utils::maskToBits(confirmationMask));
            tmpPool.add_round_confirmations(confirmations);
        }

        deferredBlock_ = csdb::Pool{};
        deferredBlock_ = tmpPool;
    }
    if (pnode->isBootstrapRound()) {
        BlockChain::setBootstrap(deferredBlock_, true);
    }
    deferredBlock_.to_byte_stream(binSize);
    //csdebug() << log_prefix << "pool #" << deferredBlock_.sequence() << ": " << cs::Utils::byteStreamToHex(deferredBlock_.to_binary().data(), deferredBlock_.to_binary().size());

    size_t cnt_total = deferredBlock_.transactions_count();
    if (cnt_total > 0) {
        size_t cnt = std::min(cnt_total, size_t(5));
        std::ostringstream os;
        os << "counted fee (total " << cnt_total << "): ";
        const auto& trxs = deferredBlock_.transactions();
        for (size_t i = 0; i < cnt; ++i) {
            if (i > 0) {
                os << ',';
            }
            os << trxs[i].counted_fee().to_double();
        }
        csdebug() << log_prefix << os.str();
    }
    const auto lastHashBin = deferredBlock_.hash().to_binary();
    // Zero blockHash before copy so an empty hash source can't leave stale
    // heap/stack memory in the field (which we'd then sign over → invalid
    // signatures rejected by peers as "Block Signatures are not valid").
    std::fill(stage3.blockHash.begin(), stage3.blockHash.end(), uint8_t{0});
    if (lastHashBin.empty()) {
        cserror() << log_prefix << "stage3: deferredBlock_.hash() is empty for seq="
                  << deferredBlock_.sequence() << " — refusing to sign zero hash, aborting stage3";
        return;
    }
    std::copy(lastHashBin.cbegin(), lastHashBin.cend(), stage3.blockHash.begin());
    stage3.blockSignature = cscrypto::generateSignature(private_key, stage3.blockHash.data(), stage3.blockHash.size());

    // CS_DEBUG_RECOMPUTE: dump signed-blockHash inputs for cross-node diffing.
    {
        static const bool s_recomp = [] {
            const char* v = std::getenv("CS_DEBUG_RECOMPUTE");
            return v && *v && std::string_view(v) != "0";
        }();
        if (s_recomp) {
            std::ostringstream ss;
            ss << "\n=== TRUSTED_SIGN_DUMP seq=" << deferredBlock_.sequence() << " ===\n";
            ss << "  prev_hash      " << deferredBlock_.previous_hash().to_string() << "\n";
            ss << "  sequence       " << deferredBlock_.sequence() << "\n";
            ss << "  deferred.hash  " << deferredBlock_.hash().to_string() << "\n";
            ss << "  stage3.blockHash " << cs::Utils::byteStreamToHex(stage3.blockHash.data(), stage3.blockHash.size()) << "\n";
            std::string ts;
            if (deferredBlock_.user_field_ids().count(BlockChain::kFieldTimestamp) > 0) {
                ts = deferredBlock_.user_field(BlockChain::kFieldTimestamp).value<std::string>();
            }
            ss << "  timestamp      \"" << ts << "\"\n";
            std::string br;
            if (deferredBlock_.user_field_ids().count(BlockChain::kFieldBlockReward) > 0) {
                br = deferredBlock_.user_field(BlockChain::kFieldBlockReward).value<std::string>();
            }
            ss << "  blockReward    size=" << br.size()
               << " hex=" << cs::Utils::byteStreamToHex(reinterpret_cast<const uint8_t*>(br.data()), br.size()) << "\n";
            ss << "  numberTrusted  " << static_cast<int>(deferredBlock_.numberTrusted()) << "\n";
            ss << "  realTrusted    0x" << std::hex << deferredBlock_.realTrusted() << std::dec << "\n";
            const auto& maskBytes = stage3.realTrustedMask;
            ss << "  realTrustedMask " << cs::Utils::byteStreamToHex(maskBytes.data(), maskBytes.size()) << "\n";
            ss << "  numberConfirmations " << static_cast<int>(deferredBlock_.numberConfirmations())
               << "  confirmationMask 0x" << std::hex << deferredBlock_.roundConfirmationMask() << std::dec
               << "  confirmations_size=" << deferredBlock_.roundConfirmations().size() << "\n";
            ss << "  -- per-confidant local WalletData (inputs to setBlockReward) --\n";
            const auto& conf = deferredBlock_.confidants();
            for (size_t i = 0; i < conf.size(); ++i) {
                BlockChain::WalletData wd;
                const bool found = pnode->getBlockChain().findWalletData(csdb::Address::from_public_key(conf[i]), wd);
                const size_t srcN = (wd.delegateSources_ ? wd.delegateSources_->size() : 0u);
                const size_t tgtN = (wd.delegateTargets_ ? wd.delegateTargets_->size() : 0u);
                ss << "    [" << i << "] "
                   << cs::Utils::byteStreamToHex(conf[i].data(), conf[i].size()).substr(0, 16) << "...  "
                   << (found ? "found" : "MISSING")
                   << "  bal="   << wd.balance_.integral()   << "." << wd.balance_.fraction()
                   << "  deleg=" << wd.delegated_.integral() << "." << wd.delegated_.fraction()
                   << "  srcN="  << srcN
                   << "  tgtN="  << tgtN
                   << "  trxN="  << wd.transNum_ << "\n";
            }
            // Full to_binary() byte stream we just hashed. This is the
            // single source of truth for blockHash — diff against the
            // received block's bytes in RECOMPUTE_DIFF to localize the
            // exact diverging field. (Skipped if size > 64 KB.)
            const cs::Bytes binBytes = deferredBlock_.to_binary();
            ss << "  to_binary_size " << binBytes.size() << "\n";
            if (binBytes.size() <= 65536) {
                ss << "  to_binary_hex  "
                   << cs::Utils::byteStreamToHex(binBytes.data(), binBytes.size()) << "\n";
            } else {
                ss << "  to_binary_hex  (skipped: size > 64KB)\n";
            }
            ss << "=== END_TRUSTED_SIGN_DUMP seq=" << deferredBlock_.sequence() << " ===";
            cslog() << ss.str();
        }
    }


    //pnode->prepareRoundTable(table, poolMetaInfo, stage3);
    //csmeta(csdetails) << "end";

    const cs::Characteristic* block_characteristic = conveyer.characteristic(conveyer.currentRoundNumber());

    if (!block_characteristic) {
        csmeta(cserror) << "Send round info characteristic not found, logic error";
        return;
    }
    bool showVersion = justCreatedRoundPackage_.roundTable().round >= Consensus::StartingDPOS && Consensus::miningOn;
    cs::Bytes bytes = justCreatedRoundPackage_.bytesToSign(showVersion);
    // Same defensive zero as blockHash: calculateHash on empty input is
    // not contractually defined to zero-fill, so we do it explicitly.
    std::fill(stage3.roundHash.begin(), stage3.roundHash.end(), uint8_t{0});
    if (bytes.empty()) {
        cserror() << log_prefix << "stage3: roundTable bytesToSign is empty for seq="
                  << deferredBlock_.sequence() << " — refusing to sign zero hash, aborting stage3";
        return;
    }
    stage3.roundHash = cscrypto::calculateHash(bytes.data(), bytes.size());

    cs::Bytes messageToSign;
    messageToSign.reserve(sizeof(cs::RoundNumber) + sizeof(uint8_t) + sizeof(cs::Hash));
    cs::ODataStream signStream(messageToSign);
    signStream << justCreatedRoundPackage_.roundTable().round;
    signStream << pnode->subRound();
    signStream << stage3.roundHash;
    stage3.roundSignature = cscrypto::generateSignature(private_key, stage3.roundHash.data(), stage3.roundHash.size());

    cs::Bytes trustedList;
    cs::ODataStream tStream(trustedList);
    tStream << justCreatedRoundPackage_.roundTable().round;
    tStream << justCreatedRoundPackage_.roundTable().confidants;
    std::fill(stage3.trustedHash.begin(), stage3.trustedHash.end(), uint8_t{0});
    if (trustedList.empty()) {
        cserror() << log_prefix << "stage3: trustedList is empty for seq="
                  << deferredBlock_.sequence() << " — refusing to sign zero hash, aborting stage3";
        return;
    }
    stage3.trustedHash = cscrypto::calculateHash(trustedList.data(), trustedList.size());
    stage3.trustedSignature = cscrypto::generateSignature(private_key, stage3.trustedHash.data(), stage3.trustedHash.size());

    csdebug() << "NODE> StageThree prepared:" << std::endl << cs::StageThree::toString(stage3);
}

void SolverCore::uploadNewStates([[maybe_unused]] std::vector<csdb::Transaction> newStates) {
    //psmarts.
}

void SolverCore::sendRoundTable() {
    pnode->sendRoundTable(justCreatedRoundPackage_);
}

void SolverCore::checkZeroSmartSignatures(csdb::Pool& pool) {
    auto smartSignatures = pool.smartSignatures();
    for (auto it : smartSignatures) {
        bool metZeroSignature = false;
        bool blockLoaded = false;
        csdb::Pool smartPool;
        cs::PublicKeys smartConfidants;
        for (auto itt : it.signatures) {
            csdebug() << "NODE> Signature: " << cs::Utils::byteStreamToHex(itt.second.data(), itt.second.size());
            if (!metZeroSignature && std::memcmp(cs::Zero::signature.data(), itt.second.data(), itt.second.size()) == 0) {
                csdebug() << "NODE> Found Zero-signature";
                metZeroSignature = true;
            }
            if (metZeroSignature && !blockLoaded) {
                smartPool = pnode->getBlockChain().loadBlock(it.smartConsensusPool);
                if (smartPool.sequence() != 0) {
                    smartConfidants = smartPool.confidants();
                }
                blockLoaded = true;
            }
            if (metZeroSignature && std::memcmp(cs::Zero::signature.data(), itt.second.data(), itt.second.size()) == 0) {
                if (itt.first < smartConfidants.size()) {
                    csdebug() << "NODE> Sending PublicKey to grayList";
                    addToGraylist(smartConfidants[itt.first], Consensus::GrayListPunishment);
                }
            }
        }
    }
}

bool SolverCore::addSignaturesToDeferredBlock(cs::Signatures&& blockSignatures) {
    csmeta(csdetails) << "begin";
    if (!deferredBlock_.is_valid()) {
        csmeta(cserror) << " ... Failed - deferred block is not valid. Node will solve this problem automatically";
        return false;
    }

    for (auto& it : blockSignatures) {
        csdetails() << log_prefix << cs::Utils::byteStreamToHex(it.data(), it.size());
    }
    deferredBlock_.set_signatures(blockSignatures);

    auto resPool = pnode->getBlockChain().createBlock(deferredBlock_);

    if (!resPool.has_value()) {
        cserror() << log_prefix << "Blockchain failed to write new block, it will do it later when get proper data";
        return false;
    }
    else {
        checkZeroSmartSignatures(resPool.value());
    }
    //pnode->cleanConfirmationList(deferredBlock_.sequence());
    deferredBlock_ = csdb::Pool();

    csmeta(csdetails) << "end";
    updateLastPackageSignatures();
    return true;
}

csdb::Pool& SolverCore::getDeferredBlock() {
    return deferredBlock_;
}

void SolverCore::updateLastPackageSignatures() {
    justCreatedRoundPackage_.updatePoolSignatures(lastSentSignatures_.poolSignatures);
    justCreatedRoundPackage_.updateRoundSignatures(lastSentSignatures_.roundSignatures);
    justCreatedRoundPackage_.updateTrustedSignatures(lastSentSignatures_.trustedConfirmation);

    pnode->setCurrentRP(justCreatedRoundPackage_);
}

void SolverCore::removeDeferredBlock(cs::Sequence seq) {
    if (deferredBlock_.sequence() == seq) {
        pnode->getBlockChain().removeWalletsInPoolFromCache(deferredBlock_);
        deferredBlock_ = csdb::Pool();
        csdebug() << log_prefix << "just created new block was thrown away";
    }
    else {
        csdebug() << log_prefix << "we don't have the correct block to throw";
    }
}

uint8_t SolverCore::subRound() {
    return (pnode->subRound());
}

std::optional<cs::Characteristic> SolverCore::ownValidation(cs::TransactionsPacket& packet, cs::PacketsVector& smartsPackets) {
    const std::size_t transactionsCount = packet.transactionsCount();

    cs::Characteristic characteristic;
    csdebug() << "Before characteristic creation";
    if (transactionsCount > 0) {
        characteristic = pVal_->formCharacteristic(*pcontext, packet.transactions(), smartsPackets);
        pVal_->normalizeCharacteristic(characteristic);
    }
    csdebug() << "After characteristic creation";
    if (characteristic.mask.size() != transactionsCount) {
        cserror() << log_prefix << ": characteristic mask size is not equal to transactions count in build_vector()";
        return std::nullopt;
    }
    return std::make_optional<cs::Characteristic>(std::move(characteristic));
}

bool SolverCore::isInGrayList(cs::PublicKey key) {
    return grayList_.find(key) != grayList_.end();
}

}  // namespace cs
