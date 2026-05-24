#define CS_LOG_CHANNEL "smartcontracts"
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string_view>
#include <exception>

#include <lib/system/utils.hpp>

//#include <boost/archive/text_oarchive.hpp>
//#include <boost/archive/text_iarchive.hpp>

#include <boost/archive/binary_oarchive.hpp>
#include <boost/archive/binary_iarchive.hpp>

#include <solver/smartcontracts.hpp>
#include <csnode/smartcontracts_serializer.hpp>
#include <csnode/serializers_helper.hpp>

#include "logger.hpp"

namespace {
const std::string kDataFileName = "smartcontracts.dat";
const std::string kLogPrefix = "SmartContracts_Serializer: ";
} // namespace

namespace cs {
void SmartContracts_Serializer::bind(SmartContracts& contracts) {
    contracts_ = &contracts;
    csdebug() << "Contracts bindings made";
}

void SmartContracts_Serializer::clear(const std::filesystem::path& rootDir) {
    (void)rootDir;
    contracts_->clear();
}

void SmartContracts_Serializer::save(const std::filesystem::path& rootDir) {
    std::ofstream ofs(rootDir / kDataFileName, std::ios::binary);
    boost::archive::binary_oarchive oa(ofs);
    csdebug() << kLogPrefix << __func__;
    contracts_->printClassInfo();
    const cs::Bytes payload = contracts_->serialize();
    // CS_DEBUG_RECOMPUTE: log hash of bytes about to be written.
    {
        static const bool s_recomp = [] {
            const char* v = std::getenv("CS_DEBUG_RECOMPUTE");
            return v && *v && std::string_view(v) != "0";
        }();
        if (s_recomp) {
            const auto h = ::cscrypto::calculateHash(payload.data(), payload.size());
            std::ostringstream ss;
            ss << "\n=== SMART_SERDE_DUMP save size=" << payload.size()
               << " hash=" << cs::Utils::byteStreamToHex(h.data(), h.size())
               << " ===\n=== END_SMART_SERDE_DUMP ===";
            cslog() << ss.str();
        }
    }
    oa << payload;
}

::cscrypto::Hash SmartContracts_Serializer::hash() {
    {
        std::ofstream ofs(kDataFileName, std::ios::binary);
        {
          boost::archive::binary_oarchive oa(
            ofs,
            boost::archive::no_header | boost::archive::no_codecvt
          );
          csdebug() << kLogPrefix << __func__;
          contracts_->printClassInfo();
          oa << contracts_->serialize();
        }

    }
    
    auto result = SerializersHelper::getHashFromFile(kDataFileName);
    //std::filesystem::remove(kDataFileName);
    return result;
}

void SmartContracts_Serializer::load(const std::filesystem::path& rootDir) {
    std::ifstream ifs(rootDir / kDataFileName, std::ios::binary);
    boost::archive::binary_iarchive ia(ifs);
    csdebug() << kLogPrefix << __func__;
    Bytes data;
    ia >> data;
    // CS_DEBUG_RECOMPUTE: log hash of bytes just read — compare with save hash to detect serde drift.
    {
        static const bool s_recomp = [] {
            const char* v = std::getenv("CS_DEBUG_RECOMPUTE");
            return v && *v && std::string_view(v) != "0";
        }();
        if (s_recomp) {
            const auto h = ::cscrypto::calculateHash(data.data(), data.size());
            std::ostringstream ss;
            ss << "\n=== SMART_SERDE_DUMP load size=" << data.size()
               << " hash=" << cs::Utils::byteStreamToHex(h.data(), h.size())
               << " ===\n=== END_SMART_SERDE_DUMP ===";
            cslog() << ss.str();
        }
    }
    contracts_->deserialize(data);
    contracts_->printClassInfo();

}
}  // namespace cs
