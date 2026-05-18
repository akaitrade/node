#ifndef CSNODE_ANCHORBLOCK_HPP
#define CSNODE_ANCHORBLOCK_HPP

#include <cscrypto/cscrypto.hpp>
#include <csnode/nodecore.hpp>

#include <vector>

namespace cs {

// Phase 2.5a anchor block payload (advisory).
//
// Carried as a block user-field (BlockChain::kFieldAnchorBlock) on sequences
// where seq % kAnchorCadence == 0. Self-linking via prevAnchorHash so that
// the anchor chain forms a sparse, fast-verifiable summary of the main chain.
//
// Phase 2.5a: wideQuorumSig is left empty; the aggregate-sig collection
// across the K-round window lands in 2.5b/2.5c. trustedSetSnapshot is
// captured at anchor emission from the current round's confidant set.
struct AnchorPayload {
    cs::Sequence sequence{0};
    cscrypto::MultisetDigest stateRootWallets{};   // 32 bytes
    cs::Hash prevAnchorHash{};                     // 32 bytes
    std::vector<cs::PublicKey> trustedSetSnapshot;
    cs::Bytes wideQuorumSig;                       // placeholder until 2.5b

    cs::Bytes serialize() const;
    static bool deserialize(const cs::Bytes& in, AnchorPayload& out);

    // Stable hash over the payload (excluding wideQuorumSig) — used as
    // prevAnchorHash by the next anchor.
    cs::Hash contentHash() const;

    bool empty() const { return sequence == 0 && trustedSetSnapshot.empty(); }
};

}  // namespace cs
#endif  // CSNODE_ANCHORBLOCK_HPP
