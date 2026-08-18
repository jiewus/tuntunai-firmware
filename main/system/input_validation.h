#ifndef INPUT_VALIDATION_H
#define INPUT_VALIDATION_H

#include <string>
#include <vector>

namespace input_validation {

/**
 * @brief 严格解析点分十进制版本号。
 * @param version 待解析的版本号。
 * @param components 成功时写入各个非负整数段。
 * @return 输入非空、格式正确且所有分段均可表示为 int 时返回 true。
 */
bool ParseDottedVersion(const std::string& version, std::vector<int>& components);

/**
 * @brief 解析 MQTT 主机和可选端口。
 * @param endpoint 主机、主机加端口或方括号包围的 IPv6 地址。
 * @param default_port 未显式提供端口时使用的端口。
 * @param host 成功时写入不含方括号和端口的主机。
 * @param port 成功时写入 1 至 65535 范围内的端口。
 * @return 地址和端口格式均有效时返回 true。
 */
bool ParseMqttEndpoint(const std::string& endpoint, int default_port,
                       std::string& host, int& port);

}  // namespace input_validation

#endif  // INPUT_VALIDATION_H
