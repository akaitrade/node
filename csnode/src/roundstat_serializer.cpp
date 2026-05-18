#include <fstream>
#include <sstream>
#include <exception>

#include <boost/archive/text_oarchive.hpp>
#include <boost/archive/text_iarchive.hpp>
#include <boost/archive/binary_iarchive.hpp> // legacy-format fallback on load

#include <csnode/roundstat_serializer.hpp>
#include <csnode/serializers_helper.hpp>
#include <csnode/roundstat.hpp>
#include "logger.hpp"

namespace {
    const std::string kDataFileName = "roundstat.dat";
    const std::string kLogPrefix = "RoundStat_Serializer: ";
} // namespace

namespace cs {
    void RoundStat_Serializer::bind(RoundStat& roundStat) {
        roundStat_ = &roundStat;
        csdebug() << "Roundstat bindings made";
    }

    void RoundStat_Serializer::clear(const std::filesystem::path& rootDir) {
        (void)rootDir;
        roundStat_->clear();
    }


    void RoundStat_Serializer::printClassInfo() {
        roundStat_->printClassInfo();


    }

    void RoundStat_Serializer::save(const std::filesystem::path& rootDir) {
        std::ofstream ofs(rootDir / kDataFileName, std::ios::binary);
        boost::archive::text_oarchive oa(ofs);
        csdebug() << kLogPrefix << __func__;
        roundStat_->printClassInfo();
        oa << roundStat_->serialize();
    }

    ::cscrypto::Hash RoundStat_Serializer::hash() {
        {
            std::ofstream ofs(kDataFileName, std::ios::binary);
            {
                boost::archive::text_oarchive oa(
                    ofs,
                    boost::archive::no_header | boost::archive::no_codecvt
                );
                oa << roundStat_->serialize();
                //printClassInfo();
            }
        }

        auto result = SerializersHelper::getHashFromFile(kDataFileName);
        //std::filesystem::remove(kDataFileName);
        return result;
    }


    void RoundStat_Serializer::load(const std::filesystem::path& rootDir) {
        csdebug() << kLogPrefix << __func__;
        const auto path = rootDir / kDataFileName;
        Bytes data;
        // Try the current (text, cross-OS portable) format first; on parse error,
        // fall back to the legacy binary archive so checkpoints saved by older
        // nodes still load. After the next QS save the file is rewritten as text.
        bool loaded = false;
        try {
            std::ifstream ifs(path, std::ios::binary);
            boost::archive::text_iarchive ia(ifs);
            ia >> data;
            loaded = true;
        }
        catch (const std::exception& e) {
            csinfo() << kLogPrefix << "text archive read failed (" << e.what()
                     << "); trying legacy binary";
        }
        if (!loaded) {
            std::ifstream ifs(path, std::ios::binary);
            boost::archive::binary_iarchive ia(ifs);
            ia >> data;
        }
        roundStat_->deserialize(data);
        printClassInfo();
    }
}