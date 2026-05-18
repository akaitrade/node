#ifndef BITHEAP_H
#define BITHEAP_H

#include <bitset>
#include <climits>
#include <cstdint>
#include <limits>
#include <vector>

#include <boost/serialization/serialization.hpp>
#include <boost/serialization/bitset.hpp>

namespace cs {
template <typename T, size_t BitSize = sizeof(T) * CHAR_BIT, typename = std::enable_if<std::is_integral<T>::value>>
class BitHeap {
public:
    using MinMaxRange = std::pair<T, T>;

public:
    BitHeap()
        : greatest_(std::numeric_limits<T>::max())
        , isValueSet_(false) {
    }

    void push(T val) {
        if (!isValueSet_) {
            greatest_ = val;
            isValueSet_ = true;
            return;
        }

        if (val > greatest_) {
            T shift = val - greatest_;
            bits_ <<= shift;
            // curr greatest
            size_t ind = shift - 1;
            if (ind < BitSize)
                bits_.set(ind);
            // new greatest
            greatest_ = val;
        }
        else if (val < greatest_) {
            size_t ind = greatest_ - val - 1;
            if (ind < BitSize)
                bits_.set(ind);
        }
    }

    void pop(T val) {
        if (val < greatest_) {
            size_t ind = greatest_ - val - 1;
            if (ind < BitSize) {
                bits_.reset(ind);
            }
            return;
        }
        if (val == greatest_) {
            --greatest_;
            int count = BitSize;
            while (!bits_[0] && count--) {
                bits_ >>= 1;
                --greatest_;
            }
            if (count < 0) {
                isValueSet_ = false;
                greatest_ = std::numeric_limits<T>::max();
                return;
            }
            bits_ >>= 1;
        }
    }

    bool empty() const {
        return !isValueSet_;
    }

    MinMaxRange minMaxRange() const {
        return std::make_pair(greatest_ - BitSize, greatest_);
    }

    bool contains(T val) const {
        if (val > greatest_)
            return false;
        else if (val == greatest_)
            return true;
        else {
            size_t ind = greatest_ - val - 1;
            if (ind < BitSize)
                return bits_.test(ind) == 1;
            else
                return false;
        }
    }

    size_t count() const {
        if (empty())
            return 0;
        else
            return 1 + bits_.count();
    }

    T getGreatest() {
        return greatest_;
    }

    // Canonical fixed-layout byte serialisation for state-root digest.
    // Layout: greatest_ (sizeof(T) bytes, little-endian) | isValueSet_ (1 byte) | bits_ (BitSize/8 bytes, lo-to-hi).
    // Empty state is deterministic (greatest_=numeric_limits::max(), isValueSet_=0, bits_=0).
    std::vector<uint8_t> serializeDigestBytes() const {
        std::vector<uint8_t> out;
        out.reserve(sizeof(T) + 1 + BitSize / 8);
        const auto u = static_cast<uint64_t>(greatest_);
        for (size_t i = 0; i < sizeof(T); ++i) {
            out.push_back(static_cast<uint8_t>((u >> (i * 8)) & 0xFF));
        }
        out.push_back(isValueSet_ ? uint8_t{1} : uint8_t{0});
        for (size_t byteIdx = 0; byteIdx < BitSize / 8; ++byteIdx) {
            uint8_t v = 0;
            for (size_t b = 0; b < 8; ++b) {
                if (bits_.test(byteIdx * 8 + b)) {
                    v |= static_cast<uint8_t>(1u << b);
                }
            }
            out.push_back(v);
        }
        return out;
    }

private:
    friend class boost::serialization::access;
    template<class Archive>
    void serialize(Archive &ar, [[maybe_unused]] const unsigned int version) {
        ar & greatest_;
        ar & isValueSet_;
        ar & bits_;
    }

    T greatest_;
    uint8_t isValueSet_;
    std::bitset<BitSize> bits_;
};

}  // namespace cs

#endif
