#ifndef ORDINAL_JSON_PARSER_HPP
#define ORDINAL_JSON_PARSER_HPP

#include <string>
#include <map>
#include <optional>

namespace cs {

// Simple JSON parser for ordinal inscriptions
class OrdinalJsonParser {
public:
    using JsonObject = std::map<std::string, std::string>;
    
    // Parse a simple JSON object with string values only
    static std::optional<JsonObject> parse(const std::string& json);
    
    // Extract string value from JSON object
    static std::string getString(const JsonObject& obj, const std::string& key, const std::string& defaultValue = "");
    
    // Extract integer value from JSON object
    static int64_t getInt64(const JsonObject& obj, const std::string& key, int64_t defaultValue = 0);
    
    // Convenience method for getInt64
    static int64_t getInt(const JsonObject& obj, const std::string& key, int64_t defaultValue = 0) {
        return getInt64(obj, key, defaultValue);
    }
    
    // Serialize JSON object to string
    static std::string serialize(const JsonObject& obj);
    
private:
    // Helper to trim whitespace
    static std::string trim(const std::string& str);
    
    // Helper to remove quotes from string
    static std::string unquote(const std::string& str);
};

} // namespace cs

#endif // ORDINAL_JSON_PARSER_HPP