#include <csnode/anchorblock.hpp>
#include <csnode/datastream.hpp>

#include <cscrypto/cscrypto.hpp>

namespace cs {

cs::Bytes AnchorPayload::serialize() const {
    cs::Bytes buf;
    cs::ODataStream s(buf);
    s << sequence;
    s << stateRootWallets;
    s << prevAnchorHash;
    s << static_cast<uint16_t>(trustedSetSnapshot.size());
    for (const auto& k : trustedSetSnapshot) {
        s << k;
    }
    s << wideQuorumSig;
    return buf;
}

bool AnchorPayload::deserialize(const cs::Bytes& in, AnchorPayload& out) {
    if (in.empty()) {
        return false;
    }
    cs::IDataStream s(in.data(), in.size());
    s >> out.sequence;
    s >> out.stateRootWallets;
    s >> out.prevAnchorHash;
    uint16_t n = 0;
    s >> n;
    out.trustedSetSnapshot.clear();
    out.trustedSetSnapshot.reserve(n);
    for (uint16_t i = 0; i < n; ++i) {
        cs::PublicKey k{};
        s >> k;
        out.trustedSetSnapshot.push_back(k);
    }
    s >> out.wideQuorumSig;
    return s.isValid();
}

cs::Hash AnchorPayload::contentHash() const {
    cs::Bytes buf;
    cs::ODataStream s(buf);
    s << sequence;
    s << stateRootWallets;
    s << prevAnchorHash;
    s << static_cast<uint16_t>(trustedSetSnapshot.size());
    for (const auto& k : trustedSetSnapshot) {
        s << k;
    }
    return cscrypto::calculateHash(buf.data(), buf.size());
}

}  // namespace cs
