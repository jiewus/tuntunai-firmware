#include "system/input_validation.h"

#include <cassert>
#include <string>
#include <vector>

int main() {
    std::vector<int> version;
    assert(input_validation::ParseDottedVersion("1.0.12", version));
    assert((version == std::vector<int>{1, 0, 12}));
    assert(!input_validation::ParseDottedVersion("", version));
    assert(!input_validation::ParseDottedVersion("1..2", version));
    assert(!input_validation::ParseDottedVersion("1.2.", version));
    assert(!input_validation::ParseDottedVersion("1.-2", version));
    assert(!input_validation::ParseDottedVersion("1.2147483648", version));
    assert(!input_validation::ParseDottedVersion("1.2.3.4.5.6.7.8.9", version));

    std::string host;
    int port = 0;
    assert(input_validation::ParseMqttEndpoint("mqtt.example.com", 8883, host, port));
    assert(host == "mqtt.example.com" && port == 8883);
    assert(input_validation::ParseMqttEndpoint("mqtt.example.com:443", 8883, host, port));
    assert(host == "mqtt.example.com" && port == 443);
    assert(input_validation::ParseMqttEndpoint("[2001:db8::1]:8883", 443, host, port));
    assert(host == "2001:db8::1" && port == 8883);
    assert(input_validation::ParseMqttEndpoint("2001:db8::1", 8883, host, port));
    assert(host == "2001:db8::1" && port == 8883);
    assert(!input_validation::ParseMqttEndpoint("mqtt.example.com:0", 8883, host, port));
    assert(!input_validation::ParseMqttEndpoint("mqtt.example.com:65536", 8883, host, port));
    assert(!input_validation::ParseMqttEndpoint("mqtt.example.com:abc", 8883, host, port));
    assert(!input_validation::ParseMqttEndpoint("mqtt host:8883", 8883, host, port));
    assert(!input_validation::ParseMqttEndpoint("[2001:db8::1]extra", 8883, host, port));
}
