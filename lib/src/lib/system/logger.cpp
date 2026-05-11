#include <lib/system/logger.hpp>

#include <iostream>

#include <boost/log/attributes/constant.hpp>
#include <boost/log/core.hpp>
#include <boost/log/utility/setup/common_attributes.hpp>
#include <boost/log/utility/setup/filter_parser.hpp>
#include <boost/log/utility/setup/formatter_parser.hpp>
#include <boost/log/utility/setup/from_settings.hpp>

namespace logger {
void initialize(const logging::settings& settings) {
    logging::add_common_attributes();

    // Make every record carry a Channel attribute. Per-TU loggers attach their
    // own value (consensus, sync, etc); records without a logger-attached
    // Channel fall back to "default" so filter expressions referencing
    // %Channel% can be evaluated safely without throwing.
    logging::core::get()->add_global_attribute(
        "Channel", logging::attributes::constant<std::string>("default"));

    // formatters
    logging::register_simple_formatter_factory<severity_level, char>(logging::trivial::tag::severity::get_name());
    logging::register_simple_formatter_factory<std::string, char>("Channel");
    // filters
    logging::register_simple_filter_factory<severity_level>(logging::trivial::tag::severity::get_name());
    logging::register_simple_filter_factory<std::string>("Channel");

    try {
        logging::init_from_settings(settings);
    } catch (const std::exception& e) {
        std::cerr << "[logger] init_from_settings threw: " << e.what() << std::endl;
        std::cerr << "[logger] (log filter / format parsing failed; node will exit)" << std::endl;
        throw;
    } catch (...) {
        std::cerr << "[logger] init_from_settings threw an unknown exception" << std::endl;
        throw;
    }
}

void cleanup() {
    logging::core::get()->remove_all_sinks();
}
}  // namespace logger
