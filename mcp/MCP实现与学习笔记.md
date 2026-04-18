# MCP 学习服务实现笔记（`learn_mcp_server`）

本文整理本仓库中 **C++ MCP 学习服务** 从选型到运行、再到 Cursor 接入的完整过程，并说明**每一步在协议与工程上的作用**。

---

## 一、我们要解决什么问题

**MCP（Model Context Protocol）** 让「模型宿主」（如 Cursor）用**统一约定**去发现和调用外部能力：工具（Tools）、只读上下文（Resources）、提示模板（Prompts）等，而不必为每个 IDE 各写一套私有接口。

本项目的学习目标：

- 理解 **应用层能力**（tools / resources / prompts）如何注册与暴露；
- 理解 **传输层**：本服务通过 **HTTP** 提供 **Streamable HTTP**（路径 `/mcp`）等，便于部署在服务器、在本地 Cursor 里用 `url` 连接；
- 在真实代码里走通 **initialize → 能力协商 → list/call** 的流程。

---

## 二、技术选型：为什么用 `cpp-mcp`

| 选择 | 作用与意义 |
|------|------------|
| **语言选 C++** | 便于与现有 C++ 栈、高性能或嵌入式场景结合；协议本身与语言无关。 |
| **库选 [hkr04/cpp-mcp](https://github.com/hkr04/cpp-mcp)** | Anthropic **没有官方 C++ SDK**；该库实现了 JSON-RPC、HTTP 服务、**2025-03-26** 相关传输与资源/工具注册，避免从零手写帧解析与路由，学习重点放在「业务暴露了什么 MCP 能力」上。 |
| **不手写完整协议栈** | MCP 细节多（会话、初始化、错误码、多传输）；用库可把时间花在 **语义与边界**（参数校验、安全、资源 URI 设计）上。 |

---

## 三、仓库与目录结构（本地）

```
wkspace/
├── .gitmodules                # 声明子模块：mcp/third_party/cpp-mcp → 官方 cpp-mcp
└── mcp/
    ├── CMakeLists.txt         # 构建：引入子模块中的 cpp-mcp + 编译 learn_mcp_server
    ├── third_party/
    │   └── cpp-mcp/           # Git 子模块（hkr04/cpp-mcp），clone 后需 git submodule update --init
    ├── src/
    │   └── learn_mcp_server.cpp   # 学习用服务端入口与注册逻辑
    ├── DEPLOY.txt             # 部署与端口、防火墙要点
    └── MCP实现与学习笔记.md   # 本文档
```

**意义**：把「第三方协议库」与「你的业务代码」分离；**子模块**固定依赖仓库与提交，团队拉代码后统一 `git submodule update --init` 即可恢复相同版本。若未初始化子模块，CMake 会依次尝试旧的 `../cpp-mcp-upstream` 路径，或走 FetchContent 从网络拉取（不推荐作为长期方案）。

### 子模块常用命令（在仓库根 `wkspace/` 下）

| 命令 | 作用 |
|------|------|
| `git submodule update --init --recursive` | 首次克隆后拉取 `cpp-mcp` 子模块内容 |
| `git clone --recurse-submodules <url>` | 克隆主仓库时一并拉子模块 |
| `git submodule update --remote mcp/third_party/cpp-mcp` | 将子模块跟踪分支更新到远端最新（慎用，需回归测试） |

---

## 四、实现过程分步说明

### 步骤 1：编写根 `CMakeLists.txt`

**做了什么**

- 声明 C++17、工程名 `learn_mcp_server`；
- 默认 `CPP_MCP_ROOT` 指向 **`third_party/cpp-mcp`（子模块）** 并 `add_subdirectory`；若不存在则尝试旧的 `../cpp-mcp-upstream`，再否则 `FetchContent`；
- 关闭 `MCP_BUILD_TESTS`，减少无关构建；
- `add_executable(learn_mcp_server …)` 并 `target_link_libraries(… PRIVATE mcp)`；
- `target_include_directories` 指向 cpp-mcp 的 `include` 与 `common`（内含 `json.hpp`、`httplib.h` 等）。

**作用与意义**

- **可复现**：别人克隆仓库后一条 CMake 命令即可编出同一二进制；
- **链接 `mcp` 静态库**：你的进程里包含服务端实现（HTTP、JSON-RPC、tools/resources 默认处理器等）；
- **包含路径**：你的 `.cpp` 里只需 `#include "mcp_server.h"` 等，与官方示例一致。

---

### 步骤 2：实现 `learn_mcp_server.cpp` 总体骨架

**做了什么**

- `main` 解析 `--host` / `--port`（默认 `0.0.0.0` 与端口可配），便于部署与改端口；
- 构造 `mcp::server::configuration`，设置监听地址、线程池等；
- 创建 `mcp::server`，依次：`set_server_info`、`set_instructions`、`set_capabilities`，再注册资源与工具，最后 `start(true)` 阻塞运行。

**作用与意义**

- **配置与代码分离**：监听地址/端口属于运维关切，命令行传入避免改代码重编译；
- **`set_server_info`**：在 `initialize` 响应里标识**本服务器名称与版本**，客户端日志与调试可区分多服务；
- **`set_instructions`**：给模型/用户的**高层说明**（非强制 schema），帮助理解何时用 tools、何时读 resources；
- **`set_capabilities`**：声明本服务支持 **tools / resources / prompts** 及是否 `listChanged` 等，属于 **能力协商**——客户端据此启用对应 UI 与请求；
- **`start(true)`**：在当前线程跑 HTTP 事件循环，适合简单部署；生产可再包装为 systemd 等。

---

### 步骤 3：准备 `data/` 与文件资源

**做了什么**

- `fs::create_directories(data_dir)`，并写入 `data/sample_notes.txt`；
- 用 `mcp::file_resource` 注册到 **`file://` + 绝对路径** 的 URI（与 `register_resource` 的 key 一致）。

**作用与意义**

- **Resources** 在协议上表示「**模型可读、通常不主动改服务器状态**」的上下文；**文件资源**演示「磁盘上的真实数据源」如何映射成 URI；
- 先创建文件再注册，避免 `file_resource` 构造时因文件不存在抛错。

---

### 步骤 4：注册静态文本资源与二进制资源

**做了什么**

- `mcp::text_resource`：`learn://docs/overview`，`set_text` 写入 Markdown 说明；
- `mcp::binary_resource`：`learn://blob/ping`，`set_data` 写入少量字节，`resources/read` 时库会按规范用 **base64** 等形式表达。

**作用与意义**

- **静态 text**：适合「说明文档、配置摘要、固定知识块」；
- **binary**：适合「小图标、证书片段、非 UTF-8 数据」；理解 **MIME 类型** 与 **blob** 与 **text** 在 read 结果中的差异。

---

### 步骤 5：注册资源模板 `register_resource_template`

**做了什么**

- 模板 URI 形如 `learn://echo/{message}`，匹配后由 handler 根据 **`uri_params`** 拼出正文。

**作用与意义**

- 对应规范中的 **参数化资源 URI（RFC 6570 风格）**；同一逻辑可服务无限 URI，而不必为每个片段单独 `register_resource`；
- 与静态资源互补：**静态**列在 `resources/list`，**模板**出现在 `resources/templates/list`，读具体 URI 时走 `resources/read`。

---

### 步骤 6：注册 Tools（`tool_builder` + handler）

**做了什么**

- 例如：`get_time`、`echo`、`calculator`、`server_stats`；
- 每个 handler 返回 **MCP 要求的 content 数组**（如 `type: text`）；
- 对非法参数 `throw mcp::mcp_exception(invalid_params, …)`，库在 `tools/call` 里可转为带 `isError` 的响应；
- 对只读工具使用 `with_annotations` 设置 **`readOnlyHint`**（2025 草案类提示），表达「副作用预期」。

**作用与意义**

- **Tools** 表示「**模型可主动请求执行的操作**」，带输入 schema（`inputSchema`），适合计算、查时间、调内部 API；
- **与 Resources 区分**：resources 偏「读上下文」；tools 偏「动作」——尽管只读工具也可存在，语义上应对齐产品边界；
- **注解**：帮助宿主做 UI 或策略（例如是否默认允许自动执行）。

---

### 步骤 7：用 `register_method` 实现 Prompts

**做了什么**

- `prompts/list`：返回若干条 `name`、`description`、`arguments`；
- `prompts/get`：根据 `name` 与 `arguments` 组装 **`messages`**（含纯 `text` 与 **embedded `resource`** 示例）。

**作用与意义**

- **Prompts** 在协议上是「**用户可选用**的模板化多轮消息」，适合代码评审、总结等固定套路；
- `cpp-mcp` 的 `server` 未提供与 `register_tool` 同级的 `register_prompt`，因此通过 **`register_method` 直接挂载 JSON-RPC 方法名**，等价于在路由表里增加 `prompts/list` 与 `prompts/get`；
- **Embedded resource**：展示 prompt 消息如何引用服务端资源内容，而不只拼字符串。

---

### 步骤 8：编译与运行

**做了什么**（命令层面）

```bash
cd mcp
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --target learn_mcp_server -j"$(nproc)"
./build/learn_mcp_server --host 0.0.0.0 --port 18088
```

**作用与意义**

- **`--target learn_mcp_server`**：只编你的可执行文件及其依赖的 `mcp` 库，缩短迭代时间（全量编会带上 cpp-mcp 自带 examples）；
- **监听 `0.0.0.0`**：允许非本机访问，便于「服务器部署 + 本地 Cursor」；
- **日志**：cpp-mcp 使用内部 logger；业务上**勿向 stdout 打印调试信息**以免干扰某些传输（本服务主要走 HTTP，习惯上仍建议调试走 stderr 或文件）。

---

### 步骤 9：部署与安全（概要）

**做了什么**

- 详见同目录 **`DEPLOY.txt`**：端口、防火墙、HTTPS 反代、`/mcp` 路径说明。

**作用与意义**

- **HTTP MCP 暴露在公网**时必须有 **TLS、鉴权、限流** 等外层防护；本学习进程**未内置登录**，仅靠网络边界与反向代理补齐。

---

### 步骤 10：在 Cursor 中配置 `~/.cursor/mcp.json`

**做了什么**

- 为 **远程** MCP 增加一项：`"url": "http://<主机>:<端口>/mcp"`；
- **不必**写 `type`（与 `command` 的 stdio 型区分）。

**作用与意义**

- Cursor 作为 **MCP Client**，通过 **Streamable HTTP** 与你在步骤 8、9 起的服务通信；
- **`/mcp`** 必须与 cpp-mcp 默认的 **Streamable HTTP 端点**一致（见启动时 stderr 打印的路径说明）。

---

## 五、与 MCP 规范概念的对应关系（便于对照官方文档）

| 概念 | 在本项目中的体现 |
|------|------------------|
| JSON-RPC 2.0 | 由 cpp-mcp 收发与映射到 `register_method` / 内置 `tools/*`、`resources/*` |
| `initialize` / `initialized` | 库处理；你在 `set_capabilities` / `set_server_info` / `set_instructions` 影响协商结果 |
| `ping` | 库内置，用于探活 |
| `tools/list`、`tools/call` | `register_tool` 时注册 |
| `resources/list`、`resources/read`、`resources/templates/list` | 注册资源/模板时注册 |
| `prompts/list`、`prompts/get` | 手动 `register_method` |
| 传输：Streamable HTTP | `http://HOST:PORT/mcp`（另含旧版 SSE 路径，学习时可忽略或对照规范演进） |

官方规范入口：<https://modelcontextprotocol.io/>

---

## 六、建议的自测顺序（加深理解）

1. 启动 `learn_mcp_server`，确认日志里 **端口与 `/mcp`**。  
2. 在 Cursor 中连接后，看 **MCP Logs** 是否完成 initialize。  
3. 依次让 Agent：**列工具并调用** `get_time` / `calculator`；**读资源** `learn://docs/overview` 与 `file://.../sample_notes.txt`；**列模板并读** `learn://echo/test`。  
4. 使用 **Prompts**（若 Cursor UI 暴露）或等价客户端调用 `prompts/list` / `prompts/get`。  

---

## 七、延伸阅读与改进方向

- 阅读 `cpp-mcp-upstream/examples/server_example.cpp` 与 `include/mcp_server.h`，对照 **会话、`Mcp-Session-Id`、DELETE** 等 HTTP 传输细节。  
- 生产环境：**TLS**、**认证**、**仅内网或 VPN**、**最小权限工具**。  
- 若需 **stdio** 供本机 Cursor 直接拉起进程：需另一套启动方式（`command` + `args`），与当前「纯 HTTP 部署」并行学习对比。  

---

*文档版本与 `learn_mcp_server` 源码同步维护；路径以仓库内 `mcp/` 为准。*
