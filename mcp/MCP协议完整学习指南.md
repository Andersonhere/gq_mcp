# MCP (Model Context Protocol) 协议完整学习指南

本文档整理自 `gq_mcp` 仓库的 C++ MCP 实现，涵盖 MCP 协议的核心概念、传输机制、服务端/客户端开发流程，以及代码实例对照讲解。

---

## 一、MCP 协议概述

### 1.1 什么是 MCP？

**MCP (Model Context Protocol)** 是一个开放协议，让 AI 模型宿主（如 Cursor、Claude Desktop）能够通过统一的约定发现和调用外部能力：
- **工具 (Tools)**：模型可主动请求执行的操作
- **资源 (Resources)**：只读的上下文数据
- **提示 (Prompts)**：模板化的消息序列

### 1.2 协议版本

| 版本 | 发布日期 | 主要特性 |
|------|----------|----------|
| **2025-03-26** | 最新 | Streamable HTTP 传输，资源模板，工具注解 |
| **2024-11-05** | 兼容 | HTTP+SSE 传输 |

本实现基于 **2025-03-26** 版本，同时兼容旧版。

### 1.3 协议栈

```
┌─────────────────────────────────────────┐
│         应用层能力                       │
│  Tools / Resources / Prompts            │
├─────────────────────────────────────────┤
│         JSON-RPC 2.0 消息层             │
│  request / response / notification      │
├─────────────────────────────────────────┤
│         传输层                           │
│  Streamable HTTP / HTTP+SSE / Stdio     │
└─────────────────────────────────────────┘
```

---

## 二、核心概念详解

### 2.1 JSON-RPC 2.0 消息格式

所有 MCP 消息都基于 JSON-RPC 2.0 规范：

#### 请求 (Request)
```json
{
  "jsonrpc": "2.0",
  "id": 1,
  "method": "tools/call",
  "params": {
    "name": "get_time",
    "arguments": {}
  }
}
```

#### 响应 (Response)
```json
{
  "jsonrpc": "2.0",
  "id": 1,
  "result": {
    "content": [
      {"type": "text", "text": "Server local time: Sun Apr 20 10:30:00 2025"}
    ],
    "isError": false
  }
}
```

#### 通知 (Notification) - 无需响应
```json
{
  "jsonrpc": "2.0",
  "method": "notifications/initialized",
  "params": {}
}
```

### 2.2 错误码定义

| 错误码 | 名称 | 含义 |
|--------|------|------|
| -32700 | parse_error | JSON 解析失败 |
| -32600 | invalid_request | 无效的请求对象 |
| -32601 | method_not_found | 方法不存在 |
| -32602 | invalid_params | 无效的参数 |
| -32603 | internal_error | 内部错误 |
| -32000 ~ -32099 | server_error | 服务器错误范围 |

**代码定义** (`mcp_message.h`):
```cpp
enum class error_code {
    parse_error = -32700,
    invalid_request = -32600,
    method_not_found = -32601,
    invalid_params = -32602,
    internal_error = -32603,
    server_error_start = -32000,
    server_error_end = -32099
};
```

---

## 三、传输协议详解

### 3.1 Streamable HTTP (2025-03-26)

**端点**: `/mcp`

**特点**:
- 单一端点处理所有请求
- 支持会话管理 (Mcp-Session-Id)
- 支持 POST / GET / DELETE 方法

**交互流程**:

```
客户端                              服务端
  │                                   │
  │──── POST /mcp (initialize) ───────│
  │     无 Session-Id                 │
  │                                   │
  │←─── 响应 + Mcp-Session-Id ────────│
  │                                   │
  │──── POST /mcp (initialized通知) ──│
  │     带 Mcp-Session-Id             │
  │                                   │
  │←─── HTTP 202 ─────────────────────│
  │                                   │
  │──── POST /mcp (tools/list) ───────│
  │     带 Mcp-Session-Id             │
  │                                   │
  │←─── 工具列表 ─────────────────────│
  │                                   │
```

**关键代码** (`mcp_server.h`):
```cpp
// Streamable HTTP transport (2025-03-26)
void handle_mcp_post(const httplib::Request& req, httplib::Response& res);
void handle_mcp_get(const httplib::Request& req, httplib::Response& res);
void handle_mcp_delete(const httplib::Request& req, httplib::Response& res);
```

### 3.2 HTTP+SSE (2024-11-05 兼容)

**端点**: 
- SSE: `/sse` 
- 消息: `/message`

用于向后兼容，新项目推荐使用 Streamable HTTP。

### 3.3 Stdio 传输

适用于本地进程通信，通过标准输入输出传输 JSON-RPC 消息。

---

## 四、服务端开发详解

### 4.1 服务端架构

```
┌─────────────────────────────────────────────────────────┐
│                    mcp::server                          │
├─────────────────────────────────────────────────────────┤
│  配置层                                                  │
│  - host / port                                          │
│  - server_info (name, version)                          │
│  - capabilities                                         │
│  - instructions                                         │
├─────────────────────────────────────────────────────────┤
│  路由层                                                  │
│  - method_handlers_ (方法 -> 处理器)                     │
│  - tools_ (工具名 -> 工具定义 + 处理器)                  │
│  - resources_ (URI -> 资源对象)                          │
│  - resource_templates_ (URI模板列表)                     │
├─────────────────────────────────────────────────────────┤
│  会话层                                                  │
│  - session_dispatchers_ (会话事件分发器)                 │
│  - session_initialized_ (会话初始化状态)                 │
├─────────────────────────────────────────────────────────┤
│  传输层                                                  │
│  - http_server_ (httplib::Server)                       │
│  - thread_pool_ (线程池)                                │
└─────────────────────────────────────────────────────────┘
```

### 4.2 创建服务端步骤

#### 步骤1: 配置服务器

```cpp
// 创建配置
mcp::server::configuration srv_conf;
srv_conf.host = "0.0.0.0";
srv_conf.port = 8080;
srv_conf.threadpool_size = std::max(2u, std::thread::hardware_concurrency());

// 创建服务器实例
mcp::server server(srv_conf);
```

#### 步骤2: 设置服务器信息

```cpp
server.set_server_info("learn_mcp_server", "0.1.0");
server.set_instructions(
    "Learning MCP server (C++). Use tools for side effects and computation, "
    "resources/read for stable context, resources/templates/list + dynamic URIs "
    "for parameterized documents, and prompts/get for reusable prompt templates.");
```

**协议对应**:
- `set_server_info` → `initialize` 响应中的 `serverInfo`
- `set_instructions` → `initialize` 响应中的 `instructions`

#### 步骤3: 设置能力声明

```cpp
server.set_capabilities(mcp::json{
    {"tools", mcp::json{{"listChanged", false}}},
    {"resources", mcp::json{{"subscribe", false}, {"listChanged", false}}},
    {"prompts", mcp::json{{"listChanged", false}}},
});
```

**协议对应**:
- `set_capabilities` → `initialize` 响应中的 `capabilities`

#### 步骤4: 注册资源

##### 静态文本资源
```cpp
auto overview = std::make_shared<mcp::text_resource>(
    "learn://docs/overview",    // URI
    "Overview",                  // 名称
    "text/markdown",            // MIME类型
    "Static markdown resource"  // 描述
);
overview->set_text("# learn_mcp_server\n\n- **Tools**: ...");
server.register_resource("learn://docs/overview", overview);
```

##### 文件资源
```cpp
const std::string file_uri = std::string("file://") + sample_file.string();
auto notes = std::make_shared<mcp::file_resource>(
    sample_file.string(),  // 文件路径
    "text/plain",          // MIME类型
    "Notes file"           // 描述
);
server.register_resource(file_uri, notes);
```

##### 二进制资源
```cpp
auto ping_blob = std::make_shared<mcp::binary_resource>(
    "learn://blob/ping",
    "Tiny blob",
    "application/octet-stream",
    "Two-byte binary resource"
);
const uint8_t bytes[2] = {'M', 'C'};
ping_blob->set_data(bytes, sizeof(bytes));
server.register_resource("learn://blob/ping", ping_blob);
```

##### 资源模板 (动态URI)
```cpp
server.register_resource_template(
    "learn://echo/{message}",      // URI模板 (RFC 6570)
    "Echo template",               // 名称
    "text/plain",                  // MIME类型
    "RFC6570-style template",      // 描述
    [](const std::string& uri, 
       const std::map<std::string, std::string>& uri_params,
       const std::string& /*session*/) -> mcp::json {
        const std::string msg = uri_params.count("message") ? uri_params.at("message") : "";
        return mcp::json{
            {"uri", uri},
            {"mimeType", "text/plain"},
            {"text", "Echo template resolved. Segment: " + msg},
        };
    });
```

**协议对应**:
- 静态资源 → `resources/list` 返回的列表
- 资源模板 → `resources/templates/list` 返回的列表
- 读取资源 → `resources/read` 请求

#### 步骤5: 注册工具

##### 定义工具
```cpp
// 使用 tool_builder 流式API
mcp::tool time_tool = mcp::tool_builder("get_time")
    .with_description("Return the server's current local time (read-only)")
    .with_annotations(mcp::json{{"readOnlyHint", true}})
    .build();

mcp::tool echo_tool = mcp::tool_builder("echo")
    .with_description("Echo text with optional transforms")
    .with_string_param("text", "Input text", true)           // 必填
    .with_boolean_param("uppercase", "Convert to uppercase", false)  // 选填
    .with_boolean_param("reverse", "Reverse characters", false)
    .build();

mcp::tool calc_tool = mcp::tool_builder("calculator")
    .with_description("add | subtract | multiply | divide")
    .with_string_param("operation", "One of add, subtract, multiply, divide", true)
    .with_number_param("a", "Left operand", true)
    .with_number_param("b", "Right operand", true)
    .build();
```

##### 实现处理器

```cpp
// 处理器签名
using tool_handler = std::function<json(const json& params, const std::string& session_id)>;

// 示例: get_time 处理器
static mcp::json get_time_handler(const mcp::json& params, const std::string& session_id) {
    const auto now = std::chrono::system_clock::now();
    const std::time_t t = std::chrono::system_clock::to_time_t(now);
    std::string time_str = std::ctime(&t);
    if (!time_str.empty() && time_str.back() == '\n') {
        time_str.pop_back();
    }
    // 返回 content 数组
    return mcp::json::array({
        {{"type", "text"}, {"text", "Server local time: " + time_str}}
    });
}

// 示例: echo 处理器 (带参数验证)
static mcp::json echo_handler(const mcp::json& params, const std::string& session_id) {
    // 参数验证
    if (!params.contains("text") || !params["text"].is_string()) {
        throw mcp::mcp_exception(mcp::error_code::invalid_params, "Missing string field 'text'");
    }
    
    std::string text = params["text"].get<std::string>();
    
    // 可选参数处理
    if (params.contains("uppercase") && params["uppercase"].get<bool>()) {
        std::transform(text.begin(), text.end(), text.begin(), ::toupper);
    }
    if (params.contains("reverse") && params["reverse"].get<bool>()) {
        std::reverse(text.begin(), text.end());
    }
    
    return mcp::json::array({
        {{"type", "text"}, {"text", text}}
    });
}

// 示例: calculator 处理器 (带错误处理)
static mcp::json calculator_handler(const mcp::json& params, const std::string& session_id) {
    if (!params.contains("operation")) {
        throw mcp::mcp_exception(mcp::error_code::invalid_params, "Missing 'operation'");
    }
    
    const std::string op = params["operation"].get<std::string>();
    const double a = params["a"].get<double>();
    const double b = params["b"].get<double>();
    
    double result = 0.0;
    if (op == "add") {
        result = a + b;
    } else if (op == "divide") {
        if (b == 0.0) {
            throw mcp::mcp_exception(mcp::error_code::invalid_params, "Division by zero");
        }
        result = a / b;
    } else {
        throw mcp::mcp_exception(mcp::error_code::invalid_params, "Unknown operation: " + op);
    }
    
    return mcp::json::array({
        {{"type", "text"}, {"text", std::to_string(result)}}
    });
}
```

##### 注册工具
```cpp
server.register_tool(time_tool, get_time_handler);
server.register_tool(echo_tool, echo_handler);
server.register_tool(calc_tool, calculator_handler);
```

**协议对应**:
- `tools/list` → 返回所有注册的工具及其 `inputSchema`
- `tools/call` → 调用对应的 handler

#### 步骤6: 注册提示 (Prompts)

```cpp
static void register_prompt_handlers(mcp::server& server) {
    // 注册 prompts/list
    server.register_method("prompts/list", 
        [](const mcp::json& params, const std::string& session_id) -> mcp::json {
            mcp::json prompts = mcp::json::array();
            prompts.push_back({
                {"name", "code_review"},
                {"description", "Review code for bugs and style"},
                {"arguments", mcp::json::array({
                    {{"name", "code"}, {"description", "Source code"}, {"required", true}},
                })},
            });
            return {{"prompts", prompts}};
        });

    // 注册 prompts/get
    server.register_method("prompts/get",
        [](const mcp::json& params, const std::string& session_id) -> mcp::json {
            const std::string name = params["name"].get<std::string>();
            const mcp::json args = params.value("arguments", mcp::json::object());
            
            if (name == "code_review") {
                const std::string code = args.value("code", "// paste code here");
                return mcp::json{
                    {"description", "Code review"},
                    {"messages", mcp::json::array({
                        {{"role", "user"}, 
                         {"content", {{"type", "text"}, 
                                      {"text", "Review this code:\n```\n" + code + "\n```"}}}},
                    })},
                };
            }
            throw mcp::mcp_exception(mcp::error_code::invalid_params, "Unknown prompt: " + name);
        });
}
```

**协议对应**:
- `prompts/list` → 返回可用提示模板
- `prompts/get` → 返回填充后的消息序列

#### 步骤7: 启动服务器

```cpp
server.start(true);  // true = 阻塞模式
```

### 4.3 完整服务端示例

参见 `learn_mcp_server.cpp`:

```cpp
int main(int argc, char** argv) {
    // 解析命令行参数
    std::string host = "0.0.0.0";
    int port = 8080;
    // ... 参数解析 ...

    // 创建数据目录
    const fs::path data_dir = fs::absolute(fs::path("data"));
    fs::create_directories(data_dir);

    // 配置并创建服务器
    mcp::server::configuration srv_conf;
    srv_conf.host = host;
    srv_conf.port = port;
    mcp::server server(srv_conf);

    // 设置服务器信息
    server.set_server_info("learn_mcp_server", "0.1.0");
    server.set_instructions("...");
    server.set_capabilities(...);

    // 注册资源
    server.register_resource(...);
    server.register_resource_template(...);

    // 注册工具
    server.register_tool(...);

    // 注册提示
    register_prompt_handlers(server);

    // 启动服务器
    server.start(true);
    return 0;
}
```

---

## 五、客户端开发详解

### 5.1 客户端接口

```cpp
class client {
public:
    // 初始化连接
    virtual bool initialize(const std::string& client_name, 
                           const std::string& client_version) = 0;
    
    // 探活
    virtual bool ping() = 0;
    
    // 设置能力
    virtual void set_capabilities(const json& capabilities) = 0;
    
    // 发送请求
    virtual response send_request(const std::string& method, 
                                  const json& params = json::object()) = 0;
    
    // 发送通知
    virtual void send_notification(const std::string& method, 
                                   const json& params = json::object()) = 0;
    
    // 获取服务器能力
    virtual json get_server_capabilities() = 0;
    
    // 调用工具
    virtual json call_tool(const std::string& tool_name, 
                          const json& arguments = json::object()) = 0;
    
    // 获取工具列表
    virtual std::vector<tool> get_tools() = 0;
    
    // 列出资源
    virtual json list_resources(const std::string& cursor = "") = 0;
    
    // 读取资源
    virtual json read_resource(const std::string& resource_uri) = 0;
    
    // 列出资源模板
    virtual json list_resource_templates() = 0;
};
```

### 5.2 SSE 客户端示例

```cpp
#include "mcp_sse_client.h"

int main() {
    // 创建客户端
    mcp::sse_client client("http://localhost:8080");

    // 设置能力
    client.set_capabilities({
        {"roots", {{"listChanged", true}}}
    });

    // 设置超时
    client.set_timeout(10);

    // 初始化连接
    if (!client.initialize("MyClient", "1.0.0")) {
        std::cerr << "Failed to initialize" << std::endl;
        return 1;
    }

    // 探活
    if (!client.ping()) {
        std::cerr << "Ping failed" << std::endl;
        return 1;
    }

    // 获取服务器能力
    mcp::json caps = client.get_server_capabilities();
    std::cout << "Server capabilities: " << caps.dump(4) << std::endl;

    // 获取工具列表
    auto tools = client.get_tools();
    for (const auto& tool : tools) {
        std::cout << "- " << tool.name << ": " << tool.description << std::endl;
    }

    // 调用工具
    mcp::json result = client.call_tool("get_time");
    std::cout << "Time: " << result["content"][0]["text"] << std::endl;

    // 调用带参数的工具
    result = client.call_tool("echo", {
        {"text", "Hello, MCP!"},
        {"uppercase", true}
    });
    std::cout << "Echo: " << result["content"][0]["text"] << std::endl;

    // 读取资源
    result = client.read_resource("learn://docs/overview");
    std::cout << "Resource: " << result << std::endl;

    return 0;
}
```

### 5.3 客户端交互流程

```cpp
// 1. 设置能力
client.set_capabilities({
    {"roots", {{"listChanged", true}}},
    {"sampling", {}}
});

// 2. 初始化 (发送 initialize 请求)
client.initialize("MyClient", "1.0.0");
// 内部发送: {"jsonrpc":"2.0","id":1,"method":"initialize","params":{...}}

// 3. 探活 (可选)
client.ping();
// 内部发送: {"jsonrpc":"2.0","id":2,"method":"ping","params":{}}

// 4. 获取工具列表
auto tools = client.get_tools();
// 内部发送: {"jsonrpc":"2.0","id":3,"method":"tools/list","params":{}}

// 5. 调用工具
json result = client.call_tool("calculator", {
    {"operation", "add"},
    {"a", 10},
    {"b", 5}
});
// 内部发送: {"jsonrpc":"2.0","id":4,"method":"tools/call",
//            "params":{"name":"calculator","arguments":{...}}}

// 6. 读取资源
json resource = client.read_resource("learn://docs/overview");
// 内部发送: {"jsonrpc":"2.0","id":5,"method":"resources/read",
//            "params":{"uri":"learn://docs/overview"}}
```

---

## 六、协议方法目录

### 6.1 生命周期方法

| 方法 | 方向 | 说明 |
|------|------|------|
| `initialize` | 客户端→服务端 | 初始化连接，协商能力 |
| `notifications/initialized` | 客户端→服务端 | 通知初始化完成 |
| `ping` | 双向 | 探活 |

### 6.2 工具方法

| 方法 | 说明 |
|------|------|
| `tools/list` | 列出可用工具 |
| `tools/call` | 调用工具 |

### 6.3 资源方法

| 方法 | 说明 |
|------|------|
| `resources/list` | 列出静态资源 |
| `resources/templates/list` | 列出资源模板 |
| `resources/read` | 读取资源内容 |
| `resources/subscribe` | 订阅资源变更 |
| `resources/unsubscribe` | 取消订阅 |

### 6.4 提示方法

| 方法 | 说明 |
|------|------|
| `prompts/list` | 列出可用提示模板 |
| `prompts/get` | 获取填充后的提示消息 |

---

## 七、curl 测试示例

### 7.1 初始化会话

```bash
# 设置变量
BASE=http://127.0.0.1:8080
HDR=/tmp/mcp-curl.headers
BODY=/tmp/mcp-curl.body

# 发送 initialize 请求
curl -sS -D "$HDR" -o "$BODY" \
  -H 'Content-Type: application/json' \
  -H 'Accept: application/json' \
  -X POST "$BASE/mcp" \
  --data-binary '{"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2025-03-26","capabilities":{},"clientInfo":{"name":"curl-demo","version":"0.0.1"}}}'

# 提取会话ID
SID=$(grep -i '^mcp-session-id:' "$HDR" | head -1 | awk '{print $2}' | tr -d '\r')
echo "Mcp-Session-Id=$SID"
```

### 7.2 发送初始化完成通知

```bash
curl -sS -o /dev/null -w 'HTTP %{http_code}\n' \
  -H 'Content-Type: application/json' \
  -H "Mcp-Session-Id: $SID" \
  -X POST "$BASE/mcp" \
  --data-binary '{"jsonrpc":"2.0","method":"notifications/initialized","params":{}}'
# 预期返回 HTTP 202
```

### 7.3 列出工具

```bash
curl -sS \
  -H 'Content-Type: application/json' \
  -H "Mcp-Session-Id: $SID" \
  -X POST "$BASE/mcp" \
  --data-binary '{"jsonrpc":"2.0","id":2,"method":"tools/list","params":{}}' | jq .
```

### 7.4 调用工具

```bash
# 调用 get_time
curl -sS \
  -H 'Content-Type: application/json' \
  -H "Mcp-Session-Id: $SID" \
  -X POST "$BASE/mcp" \
  --data-binary '{"jsonrpc":"2.0","id":3,"method":"tools/call","params":{"name":"get_time","arguments":{}}}' | jq .

# 调用 calculator
curl -sS \
  -H 'Content-Type: application/json' \
  -H "Mcp-Session-Id: $SID" \
  -X POST "$BASE/mcp" \
  --data-binary '{"jsonrpc":"2.0","id":4,"method":"tools/call","params":{"name":"calculator","arguments":{"operation":"add","a":10,"b":5}}}' | jq .
```

### 7.5 读取资源

```bash
curl -sS \
  -H 'Content-Type: application/json' \
  -H "Mcp-Session-Id: $SID" \
  -X POST "$BASE/mcp" \
  --data-binary '{"jsonrpc":"2.0","id":5,"method":"resources/read","params":{"uri":"learn://docs/overview"}}' | jq .
```

### 7.6 探活

```bash
curl -sS \
  -H 'Content-Type: application/json' \
  -H "Mcp-Session-Id: $SID" \
  -X POST "$BASE/mcp" \
  --data-binary '{"jsonrpc":"2.0","id":6,"method":"ping","params":{}}' | jq .
```

### 7.7 关闭会话

```bash
curl -sS -o /dev/null -w 'HTTP %{http_code}\n' \
  -H "Mcp-Session-Id: $SID" \
  -X DELETE "$BASE/mcp"
```

---

## 八、依赖库说明

### 8.1 核心依赖

| 库 | 用途 | 文件 |
|----|------|------|
| **nlohmann/json** | JSON 解析与生成 | `json.hpp` |
| **cpp-httplib** | HTTP 服务器/客户端 | `httplib.h` |
| **base64** | Base64 编解码 (二进制资源) | `base64.hpp` |

### 8.2 CMake 配置

```cmake
# 默认使用子模块路径
set(CPP_MCP_DEFAULT "${CMAKE_CURRENT_LIST_DIR}/third_party/cpp-mcp")
set(CPP_MCP_ROOT "${CPP_MCP_DEFAULT}" CACHE PATH "Path to cpp-mcp source tree")

# 依赖查找顺序：子模块 → 旧式路径 → FetchContent
if(EXISTS "${CPP_MCP_ROOT}/CMakeLists.txt")
  # 使用子模块
elseif(EXISTS "${CMAKE_CURRENT_LIST_DIR}/../cpp-mcp-upstream/CMakeLists.txt")
  # 使用旧式路径
elseif(GQ_MCP_FETCH_CPP_MCP_IF_MISSING)
  # 从网络拉取
  include(FetchContent)
  FetchContent_Declare(cpp_mcp_upstream 
    GIT_REPOSITORY https://github.com/Andersonhere/cpp-mcp.git
    GIT_TAG main)
  FetchContent_MakeAvailable(cpp_mcp_upstream)
endif()

# 关闭测试
set(MCP_BUILD_TESTS OFF CACHE BOOL "" FORCE)

# 添加依赖
add_subdirectory("${CPP_MCP_ROOT}" "${CMAKE_BINARY_DIR}/_cpp_mcp")

# 链接库
add_executable(learn_mcp_server src/learn_mcp_server.cpp)
target_link_libraries(learn_mcp_server PRIVATE mcp)
target_include_directories(learn_mcp_server PRIVATE
  "${CPP_MCP_ROOT}/include"
  "${CPP_MCP_ROOT}/common")
```

---

## 九、构建与运行

### 9.1 构建步骤

```bash
# 进入项目目录
cd gq_mcp/mcp

# 初始化子模块（首次）
git submodule update --init --recursive

# 配置构建
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release

# 编译
cmake --build build --target learn_mcp_server -j$(nproc)
```

### 9.2 运行服务器

```bash
./build/learn_mcp_server --host 0.0.0.0 --port 8080
```

### 9.3 Cursor 配置

在 `~/.cursor/mcp.json` 中添加：

```json
{
  "mcpServers": {
    "learn-server": {
      "url": "http://localhost:8080/mcp"
    }
  }
}
```

---

## 十、最佳实践

### 10.1 服务端设计

1. **工具设计**
   - 单一职责：每个工具只做一件事
   - 清晰描述：帮助模型理解何时使用
   - 参数验证：使用 `mcp_exception` 返回明确错误
   - 只读注解：对无副作用工具设置 `readOnlyHint`

2. **资源设计**
   - URI 规范：使用有意义的 URI 方案
   - MIME 类型：正确设置资源类型
   - 权限控制：避免暴露敏感文件

3. **错误处理**
   - 使用标准错误码
   - 提供有意义的错误消息
   - 记录日志便于调试

### 10.2 安全考虑

1. **传输安全**
   - 生产环境使用 HTTPS
   - 配置反向代理添加认证
   - 限制访问 IP

2. **权限控制**
   - 最小权限原则
   - 工具输入验证
   - 资源访问控制

3. **会话管理**
   - 设置合理的会话超时
   - 限制最大会话数
   - 及时清理无效会话

---

## 十一、附录

### A. 协议版本对照

| 特性 | 2024-11-05 | 2025-03-26 |
|------|------------|------------|
| 传输方式 | HTTP+SSE | Streamable HTTP |
| 资源模板 | 不支持 | 支持 |
| 工具注解 | 不支持 | 支持 |
| 会话管理 | SSE-based | Header-based |

### B. 参考资料

- [MCP 官方规范](https://modelcontextprotocol.io/)
- [cpp-mcp 仓库](https://github.com/hkr04/cpp-mcp)
- [JSON-RPC 2.0 规范](https://www.jsonrpc.org/specification)
- [RFC 6570 URI 模板](https://tools.ietf.org/html/rfc6570)

---

*文档版本: 2025-04-20*
*基于 gq_mcp 仓库整理*
