#include <ordinal_json_parser.hpp>
#include <algorithm>
#include <sstream>

namespace cs {

std::string OrdinalJsonParser::trim(const std::string& str) {
    size_t first = str.find_first_not_of(" \t\n\r");
    if (first == std::string::npos) return "";
    size_t last = str.find_last_not_of(" \t\n\r");
    return str.substr(first, (last - first + 1));
}

std::string OrdinalJsonParser::unquote(const std::string& str) {
    if (str.length() >= 2 && str.front() == '"' && str.back() == '"') {
        return str.substr(1, str.length() - 2);
    }
    return str;
}

std::optional<OrdinalJsonParser::JsonObject> OrdinalJsonParser::parse(const std::string& json) {
    JsonObject result;
    
    // Basic validation
    std::string trimmed = trim(json);
    if (trimmed.empty() || trimmed.front() != '{' || trimmed.back() != '}') {
        return std::nullopt;
    }
    
    // Remove outer braces
    std::string content = trimmed.substr(1, trimmed.length() - 2);
    
    // Simple parser for key-value pairs
    std::stringstream ss(content);
    std::string item;
    
    while (std::getline(ss, item, ',')) {
        size_t colonPos = item.find(':');
        if (colonPos == std::string::npos) continue;
        
        std::string key = trim(item.substr(0, colonPos));
        std::string value = trim(item.substr(colonPos + 1));
        
        // Remove quotes
        key = unquote(key);
        value = unquote(value);
        
        if (!key.empty()) {
            result[key] = value;
        }
    }
    
    return result;
}

std::string OrdinalJsonParser::getString(const JsonObject& obj, const std::string& key, const std::string& defaultValue) {
    auto it = obj.find(key);
    return (it != obj.end()) ? it->second : defaultValue;
}

int64_t OrdinalJsonParser::getInt64(const JsonObject& obj, const std::string& key, int64_t defaultValue) {
    auto it = obj.find(key);
    if (it != obj.end()) {
        try {
            return std::stoll(it->second);
        } catch (...) {
            return defaultValue;
        }
    }
    return defaultValue;
}

std::string OrdinalJsonParser::serialize(const JsonObject& obj) {
    std::stringstream ss;
    ss << "{";
    bool first = true;
    for (const auto& [key, value] : obj) {
        if (!first) ss << ",";
        ss << "\"" << key << "\":\"" << value << "\"";
        first = false;
    }
    ss << "}";
    return ss.str();
}

} // namespace cs