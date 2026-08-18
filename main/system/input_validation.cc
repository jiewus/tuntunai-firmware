#include "system/input_validation.h"

#include <algorithm>
#include <charconv>
#include <cctype>
#include <system_error>

namespace input_validation {
namespace {

constexpr size_t kMaximumVersionLength = 64;
constexpr size_t kMaximumVersionComponents = 8;
constexpr size_t kMaximumEndpointLength = 255;

bool ParsePort(const std::string& text, int& port) {
    if (text.empty()) {
        return false;
    }

    int parsed = 0;
    const char* begin = text.data();
    const char* end = begin + text.size();
    const auto result = std::from_chars(begin, end, parsed);
    if (result.ec != std::errc() || result.ptr != end || parsed < 1 || parsed > 65535) {
        return false;
    }
    port = parsed;
    return true;
}

bool HasWhitespaceOrControlCharacter(const std::string& value) {
    return std::any_of(value.begin(), value.end(), [](unsigned char character) {
        return std::isspace(character) != 0 || std::iscntrl(character) != 0;
    });
}

}  // namespace

bool ParseDottedVersion(const std::string& version, std::vector<int>& components) {
    components.clear();
    if (version.empty() || version.size() > kMaximumVersionLength) {
        return false;
    }

    size_t start = 0;
    while (start < version.size()) {
        if (components.size() >= kMaximumVersionComponents) {
            components.clear();
            return false;
        }

        const size_t separator = version.find('.', start);
        const size_t end_index = separator == std::string::npos ? version.size() : separator;
        if (end_index == start) {
            components.clear();
            return false;
        }

        int component = 0;
        const char* begin = version.data() + start;
        const char* end = version.data() + end_index;
        const auto result = std::from_chars(begin, end, component);
        if (result.ec != std::errc() || result.ptr != end || component < 0) {
            components.clear();
            return false;
        }
        components.push_back(component);

        if (separator == std::string::npos) {
            return true;
        }
        start = separator + 1;
    }

    components.clear();
    return false;
}

bool ParseMqttEndpoint(const std::string& endpoint, int default_port,
                       std::string& host, int& port) {
    host.clear();
    port = default_port;
    if (endpoint.empty() || endpoint.size() > kMaximumEndpointLength
        || default_port < 1 || default_port > 65535
        || HasWhitespaceOrControlCharacter(endpoint)) {
        return false;
    }

    if (endpoint.front() == '[') {
        const size_t closing_bracket = endpoint.find(']');
        if (closing_bracket == std::string::npos || closing_bracket == 1) {
            return false;
        }
        host = endpoint.substr(1, closing_bracket - 1);
        if (closing_bracket + 1 == endpoint.size()) {
            return true;
        }
        if (endpoint[closing_bracket + 1] != ':') {
            host.clear();
            return false;
        }
        if (!ParsePort(endpoint.substr(closing_bracket + 2), port)) {
            host.clear();
            return false;
        }
        return true;
    }

    const size_t first_colon = endpoint.find(':');
    const size_t last_colon = endpoint.rfind(':');
    if (first_colon != std::string::npos && first_colon == last_colon) {
        host = endpoint.substr(0, first_colon);
        if (host.empty() || !ParsePort(endpoint.substr(first_colon + 1), port)) {
            host.clear();
            return false;
        }
        return true;
    }

    host = endpoint;
    return true;
}

}  // namespace input_validation
