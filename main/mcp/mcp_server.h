#ifndef MCP_SERVER_H
#define MCP_SERVER_H

#include <string>
#include <vector>
#include <functional>
#include <memory>
#include <mutex>
#include <variant>
#include <optional>
#include <stdexcept>
#include <type_traits>
#include <utility>

#include <cJSON.h>

/**
 * @file mcp_server.h
 * @brief 设备端 MCP JSON-RPC 工具注册、参数校验、调用和应答框架。
 */

/**
 * @brief MCP 工具允许返回的值类型；cJSON* 的所有权在序列化后由框架释放。
 */
using ReturnValue = std::variant<bool, int, std::string, cJSON*>;

class PropertyList;

/**
 * @brief 异步 MCP 工具完成后返回的标准结果。
 *
 * text 保存最终写入 MCP content.text 的 UTF-8 内容；当内容是业务对象时应使用
 * 紧凑 JSON 文本。is_error 为 true 时，框架仍通过 JSON-RPC result 返回，但会设置
 * MCP 规范要求的 isError=true，使大模型能够区分业务失败与正常工具结果。
 */
struct McpToolResult {
    /**
     * @brief 获取或设置写入 MCP content.text 的 UTF-8 文本。
     */
    std::string text;
    /**
     * @brief 获取或设置本次工具调用是否属于业务失败。
     */
    bool is_error = false;
};

/**
 * @brief 异步工具执行结束时必须调用一次的完成回调。
 * @param result 工具的结构化文本和错误标志。
 */
using McpToolCompletion = std::function<void(McpToolResult result)>;

/**
 * @brief 参数校验完成后启动异步工作的工具回调。
 * @param properties 已填入默认值并完成类型、范围校验的参数列表。
 * @param completion 工作完成后用于回复原 JSON-RPC 请求的回调。
 */
using AsyncMcpToolCallback = std::function<void(
    const PropertyList& properties,
    McpToolCompletion completion)>;

/**
 * @brief MCP 输入属性支持的 JSON Schema 基础类型。
 */
enum PropertyType {
    kPropertyTypeBoolean,
    kPropertyTypeInteger,
    kPropertyTypeString
};

/**
 * @brief 单个工具输入属性的名称、类型、默认值和整数范围约束。
 */
class Property {
private:
    std::string name_;
    PropertyType type_;
    std::variant<bool, int, std::string> value_;
    bool has_default_value_;
    bool required_;
    bool provided_ = false;
    std::optional<int> min_value_;  ///< 整数属性允许的最小值。
    std::optional<int> max_value_;  ///< 整数属性允许的最大值。

public:
    /**
     * @brief 创建必填属性。
     * @param name JSON 字段名。
     * @param type 字段类型。
     */
    Property(const std::string& name, PropertyType type)
        : name_(name), type_(type), has_default_value_(false), required_(true) {}

    /**
     * @brief 创建带默认值的可选属性。
     * @param default_value 类型必须与 type 对应。
     */
    template<typename T>
    Property(const std::string& name, PropertyType type, const T& default_value)
        : name_(name), type_(type), has_default_value_(true), required_(false) {
        value_ = default_value;
    }

    /**
     * @brief 创建有取值范围的必填整数属性。
     */
    Property(const std::string& name, PropertyType type, int min_value, int max_value)
        : name_(name), type_(type), has_default_value_(false), required_(true), min_value_(min_value), max_value_(max_value) {
        if (type != kPropertyTypeInteger) {
            throw std::invalid_argument("Range limits only apply to integer properties");
        }
    }

    /**
     * @brief 创建有默认值和范围的可选整数属性；默认值必须落在闭区间内。
     */
    Property(const std::string& name, PropertyType type, int default_value, int min_value, int max_value)
        : name_(name), type_(type), has_default_value_(true), required_(false), min_value_(min_value), max_value_(max_value) {
        if (type != kPropertyTypeInteger) {
            throw std::invalid_argument("Range limits only apply to integer properties");
        }
        if (default_value < min_value || default_value > max_value) {
            throw std::invalid_argument("Default value must be within the specified range");
        }
        value_ = default_value;
    }

    inline const std::string& name() const { return name_; }
    inline PropertyType type() const { return type_; }
    inline bool has_default_value() const { return has_default_value_; }
    inline bool required() const { return required_; }
    inline bool has_value() const { return has_default_value_ || provided_; }
    inline void set_required(bool required) { required_ = required; }
    inline bool has_range() const { return min_value_.has_value() && max_value_.has_value(); }
    inline int min_value() const { return min_value_.value_or(0); }
    inline int max_value() const { return max_value_.value_or(0); }
    inline void set_minimum(int value) { min_value_ = value; }
    inline void set_maximum(int value) { max_value_ = value; }

    template<typename T>
    /**
     * @brief 以指定 C++ 类型读取当前值；类型不匹配时 std::variant 会抛异常。
     */
    inline T value() const {
        return std::get<T>(value_);
    }

    template<typename T>
    /**
     * @brief 设置属性值；整数会先执行最小/最大范围校验。
     */
    inline void set_value(const T& value) {
        // 只有整数属性具有数值范围；字符串和布尔属性只做 variant 类型约束。
        if constexpr (std::is_same_v<T, int>) {
            if (min_value_.has_value() && value < min_value_.value()) {
                throw std::invalid_argument("Value is below minimum allowed: " + std::to_string(min_value_.value()));
            }
            if (max_value_.has_value() && value > max_value_.value()) {
                throw std::invalid_argument("Value exceeds maximum allowed: " + std::to_string(max_value_.value()));
            }
        }
        value_ = value;
        provided_ = true;
    }

    /**
     * @brief 生成该属性的 JSON Schema 片段。
     */
    std::string to_json() const {
        cJSON *json = cJSON_CreateObject();
        
        if (type_ == kPropertyTypeBoolean) {
            cJSON_AddStringToObject(json, "type", "boolean");
            if (has_default_value_) {
                cJSON_AddBoolToObject(json, "default", value<bool>());
            }
        } else if (type_ == kPropertyTypeInteger) {
            cJSON_AddStringToObject(json, "type", "integer");
            if (has_default_value_) {
                cJSON_AddNumberToObject(json, "default", value<int>());
            }
            if (min_value_.has_value()) {
                cJSON_AddNumberToObject(json, "minimum", min_value_.value());
            }
            if (max_value_.has_value()) {
                cJSON_AddNumberToObject(json, "maximum", max_value_.value());
            }
        } else if (type_ == kPropertyTypeString) {
            cJSON_AddStringToObject(json, "type", "string");
            if (has_default_value_) {
                cJSON_AddStringToObject(json, "default", value<std::string>().c_str());
            }
        }
        
        char *json_str = cJSON_PrintUnformatted(json);
        std::string result(json_str);
        cJSON_free(json_str);
        cJSON_Delete(json);
        
        return result;
    }
};

/**
 * @brief 保持工具属性声明顺序，并提供按名称访问和 Schema 序列化。
 */
class PropertyList {
private:
    std::vector<Property> properties_;

public:
    PropertyList() = default;
    PropertyList(const std::vector<Property>& properties) : properties_(properties) {}
    /**
     * @brief 追加属性定义。
     * @param property 会复制到列表。
     */
    void AddProperty(const Property& property) {
        properties_.push_back(property);
    }

    /**
     * @brief 按名称取得属性；不存在时抛出 runtime_error。
     */
    const Property& operator[](const std::string& name) const {
        for (const auto& property : properties_) {
            if (property.name() == name) {
                return property;
            }
        }
        throw std::runtime_error("Property not found: " + name);
    }

    auto begin() { return properties_.begin(); }
    auto end() { return properties_.end(); }
    auto begin() const { return properties_.begin(); }
    auto end() const { return properties_.end(); }

    /**
     * @return 所有被 Schema 明确标记为必填的字段名。
     */
    std::vector<std::string> GetRequired() const {
        std::vector<std::string> required;
        for (auto& property : properties_) {
            if (property.required()) {
                required.push_back(property.name());
            }
        }
        return required;
    }

    /**
     * @brief 生成 properties 对象的 JSON Schema 文本。
     */
    std::string to_json() const {
        cJSON *json = cJSON_CreateObject();
        
        for (const auto& property : properties_) {
            cJSON *prop_json = cJSON_Parse(property.to_json().c_str());
            cJSON_AddItemToObject(json, property.name().c_str(), prop_json);
        }
        
        char *json_str = cJSON_PrintUnformatted(json);
        std::string result(json_str);
        cJSON_free(json_str);
        cJSON_Delete(json);
        
        return result;
    }
};

/**
 * @brief 一条经过后端签发并完成固件校验的动态 MCP 工具定义。
 */
struct McpDynamicToolDefinition {
    /**
     * @brief 全局唯一工具名称。
     */
    std::string name;
    /**
     * @brief 提供给大模型的工具用途说明。
     */
    std::string description;
    /**
     * @brief 固件执行基础类型校验时使用的参数列表。
     */
    PropertyList properties;
    /**
     * @brief 后端发布的完整受限 JSON Schema，用于原样生成 tools/list。
     */
    std::string input_schema_json;
    /**
     * @brief 将调用代理到固定后端工具版本的异步回调。
     */
    AsyncMcpToolCallback callback;
};

/**
 * @brief 一个可被模型或用户调用的 MCP 工具及其执行回调。
 */
class McpTool {
private:
    std::string name_;
    std::string description_;
    PropertyList properties_;
    std::function<ReturnValue(const PropertyList&)> callback_;
    AsyncMcpToolCallback async_callback_;
    bool user_only_ = false;
    bool dynamic_ = false;
    std::string input_schema_json_;

public:
    /**
     * @param name MCP 工具唯一名称，通常使用 device.xxx 格式。
     * @param description 提供给大模型的用途描述。
     * @param properties 输入参数 Schema。
     * @param callback 校验完成后执行的同步回调。
     */
    McpTool(const std::string& name, 
            const std::string& description, 
            const PropertyList& properties, 
            std::function<ReturnValue(const PropertyList&)> callback)
        : name_(name), 
        description_(description), 
        properties_(properties), 
        callback_(callback) {}

    /**
     * @brief 创建不会阻塞应用主任务的异步 MCP 工具。
     * @param name MCP 工具唯一名称。
     * @param description 提供给大模型的工具用途和调用约束。
     * @param properties 输入参数 Schema。
     * @param callback 负责把实际工作投递给后台 Worker，并在结束时调用 completion。
     */
    McpTool(const std::string& name,
            const std::string& description,
            const PropertyList& properties,
            AsyncMcpToolCallback callback)
        : name_(name),
          description_(description),
          properties_(properties),
          async_callback_(std::move(callback)) {}

    /**
     * @brief 创建保留后端原始输入 Schema 的动态异步工具。
     * @param name 工具名称。
     * @param description 工具说明。
     * @param properties 固件基础参数校验定义。
     * @param input_schema_json 已校验的完整 object JSON Schema。
     * @param callback 固定版本代理执行回调。
     */
    McpTool(const std::string& name,
            const std::string& description,
            const PropertyList& properties,
            const std::string& input_schema_json,
            AsyncMcpToolCallback callback)
        : name_(name),
          description_(description),
          properties_(properties),
          async_callback_(std::move(callback)),
          dynamic_(true),
          input_schema_json_(input_schema_json) {}

    /**
     * @brief 标记工具只供用户界面使用，不暴露给模型自主选择。
     */
    void set_user_only(bool user_only) { user_only_ = user_only; }
    inline const std::string& name() const { return name_; }
    inline const std::string& description() const { return description_; }
    inline const PropertyList& properties() const { return properties_; }
    inline bool user_only() const { return user_only_; }
    inline bool dynamic() const { return dynamic_; }
    /**
     * @return true 表示工具通过完成回调异步返回结果。
     */
    inline bool is_async() const { return static_cast<bool>(async_callback_); }

    /**
     * @brief 生成 MCP tools/list 中的单个工具声明。
     */
    std::string to_json() const {
        std::vector<std::string> required = properties_.GetRequired();
        
        cJSON *json = cJSON_CreateObject();
        cJSON_AddStringToObject(json, "name", name_.c_str());
        cJSON_AddStringToObject(json, "description", description_.c_str());
        
        cJSON* input_schema = input_schema_json_.empty()
            ? cJSON_CreateObject()
            : cJSON_Parse(input_schema_json_.c_str());
        if (input_schema_json_.empty()) {
            cJSON_AddStringToObject(input_schema, "type", "object");

            cJSON *properties = cJSON_Parse(properties_.to_json().c_str());
            cJSON_AddItemToObject(input_schema, "properties", properties);

            if (!required.empty()) {
                cJSON *required_array = cJSON_CreateArray();
                for (const auto& property : required) {
                    cJSON_AddItemToArray(required_array, cJSON_CreateString(property.c_str()));
                }
                cJSON_AddItemToObject(input_schema, "required", required_array);
            }
        }
        
        cJSON_AddItemToObject(json, "inputSchema", input_schema);

        // user_only 工具附带 audience=user 注解，使云端不把它加入模型工具列表。
        if (user_only_) {
            cJSON *annotations = cJSON_CreateObject();
            cJSON *audience = cJSON_CreateArray();
            cJSON_AddItemToArray(audience, cJSON_CreateString("user"));
            cJSON_AddItemToObject(annotations, "audience", audience);
            cJSON_AddItemToObject(json, "annotations", annotations);
        }
        
        char *json_str = cJSON_PrintUnformatted(json);
        std::string result(json_str);
        cJSON_free(json_str);
        cJSON_Delete(json);
        
        return result;
    }

    /**
     * @brief 调用工具并把多种 C++ 返回值统一包装成 MCP content 响应。
     */
    std::string Call(const PropertyList& properties) {
        ReturnValue return_value = callback_(properties);
        // 返回结果
        cJSON* result = cJSON_CreateObject();
        cJSON* content = cJSON_CreateArray();

        cJSON* text = cJSON_CreateObject();
        cJSON_AddStringToObject(text, "type", "text");
        if (std::holds_alternative<std::string>(return_value)) {
            cJSON_AddStringToObject(text, "text", std::get<std::string>(return_value).c_str());
        } else if (std::holds_alternative<bool>(return_value)) {
            cJSON_AddStringToObject(text, "text", std::get<bool>(return_value) ? "true" : "false");
        } else if (std::holds_alternative<int>(return_value)) {
            cJSON_AddStringToObject(text, "text", std::to_string(std::get<int>(return_value)).c_str());
        } else if (std::holds_alternative<cJSON*>(return_value)) {
            cJSON* json = std::get<cJSON*>(return_value);
            char* json_str = cJSON_PrintUnformatted(json);
            cJSON_AddStringToObject(text, "text", json_str);
            cJSON_free(json_str);
            cJSON_Delete(json);
        }
        cJSON_AddItemToArray(content, text);
        cJSON_AddItemToObject(result, "content", content);
        cJSON_AddBoolToObject(result, "isError", false);

        auto json_str = cJSON_PrintUnformatted(result);
        std::string result_str(json_str);
        cJSON_free(json_str);
        cJSON_Delete(result);
        return result_str;
    }

    /**
     * @brief 启动异步工具，并把后台结果交给调用方提供的完成回调。
     * @param properties 已完成校验的工具参数副本。
     * @param completion 绑定原 JSON-RPC 请求编号的回复函数。
     * @throws std::runtime_error 当前工具不是异步工具时抛出。
     */
    void CallAsync(const PropertyList& properties, McpToolCompletion completion) {
        if (!async_callback_) {
            throw std::runtime_error("Tool is not asynchronous: " + name_);
        }
        async_callback_(properties, std::move(completion));
    }
};

/**
 * @brief MCP 单例服务器，解析云端请求并通过 Application 的协议通道应答。
 */
class McpServer {
public:
    static McpServer& GetInstance() {
        static McpServer instance;
        return instance;
    }

    /**
     * @brief 注册音量、亮度、设备状态等可供模型调用的通用工具。
     */
    void AddCommonTools();
    /**
     * @brief 注册仅供用户界面直接调用的工具。
     */
    void AddUserOnlyTools();
    /**
     * @brief 接管动态创建的工具指针并转换为共享所有权。
     * @param tool 使用 new 创建的工具；无论名称是否重复，本方法都会负责释放。
     */
    void AddTool(McpTool* tool);
    /**
     * @brief 便捷注册普通工具，各参数含义同 McpTool 构造器。
     */
    void AddTool(const std::string& name, const std::string& description, const PropertyList& properties, std::function<ReturnValue(const PropertyList&)> callback);
    /**
     * @brief 注册由后台任务执行并通过完成回调应答的异步工具。
     * @param name MCP 工具的全局唯一名称。
     * @param description 提供给大模型的工具用途说明。
     * @param properties 工具参数定义、默认值和范围约束。
     * @param callback 参数校验后调用的异步启动函数。
     */
    void AddAsyncTool(const std::string& name, const std::string& description,
                      const PropertyList& properties, AsyncMcpToolCallback callback);
    /**
     * @brief 原子替换当前全部后端动态工具，保留固件内置和 user_only 工具。
     * @param definitions 已完成名称、数量和 Schema 校验的不可变定义数组。
     * @return 名称没有与固件静态工具冲突且替换成功时返回 true。
     * @note 必须从 Application 主任务调用；工具读取仍由内部互斥锁保护。
     */
    bool ReplaceDynamicTools(std::vector<McpDynamicToolDefinition> definitions);
    /**
     * @brief 注册带 audience=user 注解的工具。
     */
    void AddUserOnlyTool(const std::string& name, const std::string& description, const PropertyList& properties, std::function<ReturnValue(const PropertyList&)> callback);
    /**
     * @brief 解析已构造的 JSON-RPC 根对象；json 仅在调用期间有效。
     */
    void ParseMessage(const cJSON* json);
    /**
     * @brief 解析 JSON 文本并转发到对象版本。
     */
    void ParseMessage(const std::string& message);

private:
    McpServer();
    ~McpServer();

    /**
     * @brief 通知当前 MCP 客户端工具清单已经发生变化。
     * @details 仅在动态工具实际完成替换后调用。客户端收到标准通知后应重新请求 tools/list，
     * 从而避免继续使用会话建立时缓存的旧工具清单。
     */
    void NotifyToolsListChanged();

    /**
     * @brief 回复 JSON-RPC result。
     * @param id 请求编号。
     * @param result 已序列化结果对象。
     */
    void ReplyResult(int id, const std::string& result);
    /**
     * @brief 回复 JSON-RPC error。
     * @param message 可诊断错误说明。
     */
    void ReplyError(int id, const std::string& message);
    /**
     * @brief 将异步工具结果包装为 MCP content 数组并回复原请求。
     * @param id 原 tools/call 请求编号。
     * @param result 后台任务返回的文本和业务错误标志。
     */
    void ReplyToolResult(int id, const McpToolResult& result);

    /**
     * @brief 分页返回工具列表。
     * @param cursor 起始游标。
     * @param list_user_only_tools 是否只列用户工具。
     */
    void GetToolsList(int id, const std::string& cursor, bool list_user_only_tools);
    /**
     * @brief 查找工具、解析和校验参数，再执行工具回调。
     */
    void DoToolCall(int id, const std::string& tool_name, const cJSON* tool_arguments);

    std::vector<std::shared_ptr<McpTool>> tools_;
    std::mutex tools_mutex_;
};

#endif // MCP_SERVER_H
