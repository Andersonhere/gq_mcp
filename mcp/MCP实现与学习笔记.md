# MCP 学习服务实现笔记（`learn_mcp_server`）

本文整理本仓库中 **C++ MCP 学习服务** 从选型到运行、再到 Cursor 接入的完整过程，并说明**每一步在协议与工程上的作用**；**「四（续）」**概括 **HTTP + JSON-RPC 请求形态、接口目录、params/result 与典型场景**，并与 **`learn_mcp_server`** 已注册能力对齐；**§7** 给出可复制的 **`curl`** 序列（与 **`cpp-mcp`** 的 Streamable HTTP 实现一致）。文末增加 **与 `learn_mcp_server.cpp`、`CMakeLists.txt` 逐段对照的源码导读**，便于把文字和真实代码对上号。

> **线上副本（GitHub Pages）**：[C++ MCP 学习服务实现笔记](https://andersonhere.github.io/cpp-mcp-learn-server-note/) — 正文修订以本仓库本文件为准。

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

## 三、仓库与目录结构（以 `gq_mcp` 仓库为准）

在 **WorkGuide** 中，`gq_mcp` 以子模块形式挂载；笔记与源码的**权威路径**在子模块内 `mcp/` 下：

```
gq_mcp/                        # 子模块根（远端：Andersonhere/gq_mcp）
└── mcp/
    ├── CMakeLists.txt         # 构建：引入 cpp-mcp + 编译 learn_mcp_server
    ├── third_party/
    │   └── cpp-mcp/           # Git 子模块（Andersonhere/cpp-mcp fork，源自 hkr04/cpp-mcp）
    ├── src/
    │   └── learn_mcp_server.cpp   # 学习用服务端入口与注册逻辑
    ├── DEPLOY.txt             # 部署与端口、防火墙要点
    └── MCP实现与学习笔记.md   # 本文档
```

**意义**：把「第三方协议库」与「你的业务代码」分离；**子模块**固定依赖仓库与提交，团队拉代码后统一 `git submodule update --init` 即可恢复相同版本。若未初始化子模块，CMake 会依次尝试旧的 `../cpp-mcp-upstream` 路径，或走 FetchContent 从网络拉取（不推荐作为长期方案）。

### 子模块常用命令（在 `gq_mcp` 或主仓库根目录，视你在哪一层克隆）

| 命令 | 作用 |
|------|------|
| `git submodule update --init --recursive` | 首次克隆后拉取嵌套的 `cpp-mcp` 等子模块内容 |
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

- `main` 解析 `--host` / `--port`（默认 `0.0.0.0` 与 `8080`），便于部署与改端口；
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

## 四（续）、MCP 与客户端的交互链

本节说明：**客户端以何种方式发请求**（HTTP + JSON-RPC）、**路径/方法目录**、各 **JSON-RPC 接口** 的 **params / result** 要点，以及 **典型触发场景**；并与本仓库 `learn_mcp_server` 中已注册的能力对齐。

### 0. 客户端请求方式与「目录」

**传输与路径**

- 配置入口（示例）：`~/.cursor/mcp.json` 中 **`"url": "http://<主机>:<端口>/mcp"`**（末尾路径与 **`cpp-mcp`** 的 **Streamable HTTP** 端点一致；默认即 **`/mcp`**，以进程启动时 stderr 打印为准）。
- **HTTP**：在 Streamable HTTP 下，常见为对 **`/mcp`** 的 **`POST` / `GET` / `DELETE`** 等组合完成一次会话内的多帧交互；**具体分帧、SSE 回退路径** 由库实现，排障时以 **MCP Logs** 与 **`cpp-mcp`** 文档为准。
- **应用层**：每条业务语义仍是 **JSON-RPC 2.0** 消息（可封装在 HTTP 体或流式帧中）。

**单条 JSON-RPC 请求形态（逻辑层）**

```json
{ "jsonrpc": "2.0", "id": "<任意字符串或数字>", "method": "<方法名>", "params": { } }
```

- **`method`**：即下文「接口目录」中的字符串（如 `initialize`、`tools/list`）。
- **`params`**：对象；无参时多为 `{}` 或省略（以客户端为准）。
- **响应**：`{ "jsonrpc": "2.0", "id": "<同上>", "result": ... }` 或 **`error`**（`code` / `message` / `data`）。

**会话头（实现相关）**

- 多请求可能携带 **`Mcp-Session-Id`** 等头以关联同一会话；是否出现、何时建立，以 **`cpp-mcp` + 客户端** 为准。

**接口「目录」（按能力域）**

| 能力域 | JSON-RPC 方法名（节选） | 典型顺序 |
|--------|-------------------------|----------|
| 生命周期 | `initialize` → `initialized` | 连接后首先执行 |
| 探活 | `ping` | 任意时刻可选 |
| 工具 | `tools/list`、`tools/call` | 先 list 再 call（宿主可缓存 list） |
| 资源 | `resources/list`、`resources/templates/list`、`resources/read` | list 发现 URI；read 拉正文 |
| 提示 | `prompts/list`、`prompts/get` | list 选模板；get 取消息链 |

---

### 1. 建连后第一件事：握手

| 方法 | params（要点） | result / 通知语义 | 典型场景 |
|------|----------------|-------------------|----------|
| **`initialize`** | 客户端常带：`protocolVersion`、`capabilities`、`clientInfo`（名称/版本等） | **`result`** 中含协商后的 **`protocolVersion`**、**`capabilities`**、**`serverInfo`**（名称/版本）、以及服务端 **`instructions`** 等；本仓库中 **`serverInfo` / `instructions` / `capabilities`** 来自 `set_server_info`、`set_instructions`、`set_capabilities` | **首次**连上 MCP 服务；宿主与服务器对齐协议版本与双方能力 |
| **`initialized`** | 多为空对象或极简字段（以客户端为准） | 通常无 **`result`** 语义上的业务负载，表示「我已就绪」 | 客户端**收到**合法的 `initialize` **结果之后**发出，表示后续可调用 `tools/*`、`resources/*` 等 |

**说明**：在规范语义上，**应先完成 `initialize` / `initialized`**，再大量下发业务类 `method`；否则服务端可拒绝或行为未定义。

---

### 2. 发现能力：`list` 族

| 方法 | params（要点） | result（要点） | 典型场景 |
|------|----------------|----------------|----------|
| **`tools/list`** | 常为空 `{}`；若支持分页，可含 **`cursor`**（字符串） | **`tools`**：数组；每项含 **`name`**、**`description`**、**`inputSchema`**（JSON Schema），及可选注解 | 打开 MCP 面板、Agent 规划「可调哪些工具」、或宿主刷新工具缓存 |
| **`resources/list`** | 常为空 `{}`；可含 **`cursor`** | **`resources`**：静态资源描述（**`uri`**、**`name`**、**`mimeType`**、**`description`** 等） | 模型或 UI 需要枚举可读上下文 |
| **`resources/templates/list`** | 常为空 `{}`；可含 **`cursor`** | **`resourceTemplates`**：模板 URI 模式与说明 | 需要 **RFC 6570 风格** 动态 URI（如 `learn://echo/{message}`）前的发现步骤 |
| **`prompts/list`** | 本仓库实现：若带 **`cursor`**，结果中附加 **`nextCursor`**（示例里为空串） | **`prompts`**：`name`、`description`、`arguments` 元数据数组 | 展示可选 Prompt 模板、供用户或 Agent 选择 |

---

### 3. 使用能力：`call` / `read` / `get`

| 方法 | params（要点） | result（要点） | 典型场景 |
|------|----------------|----------------|----------|
| **`tools/call`** | **`name`**：工具名字符串；**`arguments`**：对象，字段需符合该工具 **`inputSchema`** | 成功时 **`content`**：数组，元素多为 **`{ "type": "text", "text": "..." }`** 等；失败时走 **`error`** 或 **`isError`**（由库与客户端共同呈现） | Agent 执行「查时间、计算器、echo」等；用户点击运行工具 |
| **`resources/read`** | **`uri`**：资源 URI（须与 list 或模板解析结果一致） | 资源内容（**文本**或 **blob**；blob 常带 **base64** 等表达，视库与 MIME） | 模型需要把某段 **稳定上下文** 读入对话（如 `learn://docs/overview`） |
| **`prompts/get`** | **`name`**：模板名；**`arguments`**：模板参数对象（可省略，由服务端默认值填充） | **`description`** + **`messages`**：多轮消息骨架（可含 **embedded resource**） | 用户选用「代码评审 / 总结 / 嵌入式资源演示」等固定套路 |

---

### 4. 保活与其它

| 方法 | params | result | 典型场景 |
|------|--------|--------|----------|
| **`ping`** | 多为 `{}` | 空对象或简单确认（以库为准） | **探活**、脚本检测服务、部分宿主**间歇**检测 |

**说明**：**不**应简单等价成「必须靠周期心跳维持 TCP」；空闲断连更多与 **HTTP/代理/会话超时** 有关，`ping` 只是可选探针。

---

### 5. 本服务 `learn_mcp_server` 已注册能力速查（与源码一致）

**工具 `tools/call.name` / `arguments`（摘要）**

| `name` | `arguments`（字段） | 正常 `content` 形态 | 典型场景 |
|--------|---------------------|---------------------|----------|
| `get_time` | 无必填字段 | 单行文本：服务器本地时间 | 问「现在几点」 |
| `echo` | **`text`**（必填）；**`uppercase`**、**`reverse`**（可选布尔） | 处理后的字符串 | 演示字符串变换 |
| `calculator` | **`operation`**：`add` / `subtract` / `multiply` / `divide`；**`a`**、**`b`**（数值） | 运算结果的文本 | 简单算术 |
| `server_stats` | 无必填字段 | 文本内含 JSON 串：`uptimeSeconds`、`sessionId`、`cppMcpProtocol` | 进程存活与协议版本排查 |

**资源 `resources/read.uri`（示例）**

| URI 类型 | 示例 | 典型场景 |
|----------|------|----------|
| 静态 Markdown | `learn://docs/overview` | 读站点说明 / 固定知识块 |
| 磁盘文件 | `file://` + 绝对路径（启动时 stderr 会打印样例） | 演示 **file_resource** |
| 二进制 | `learn://blob/ping` | 演示 **base64 / blob** 读路径 |
| 模板实例 | `learn://echo/hello-mcp`（`{message}` 由路径段替换） | 演示 **模板资源** 解析 |

**提示 `prompts/get.name` / `arguments`**

| `name` | `arguments` | 典型场景 |
|--------|-------------|----------|
| `code_review` | **`code`**（建议提供源码字符串） | 固定代码评审提示 |
| `summarize_topic` | **`topic`**；**`audience`** 可选 | 要点总结 |
| `with_embedded_readme` | 无必填 | 演示 **prompt 内嵌 resource** |

---

### 6. 与本仓库代码的对应关系

| 协议阶段 / 方法 | 在本项目中的体现 |
|----------------|------------------|
| `initialize` 结果 | `set_server_info`、`set_instructions`、`set_capabilities` |
| `tools/list` / `tools/call` | `register_tool` 与 `get_time_handler` / `echo_handler` / `calculator_handler` / `server_stats_handler` |
| `resources/list` / `resources/read` / `resources/templates/list` | `register_resource`、`register_resource_template` |
| `prompts/list` / `prompts/get` | `register_prompt_handlers` 内的 `register_method` |

### 7. 附录：`curl` 真实请求序列（Streamable HTTP，`cpp-mcp` 实现）

以下与 **`cpp-mcp`** 中 **`mcp_server.cpp::handle_mcp_post`** 行为一致（你本仓库子模块路径：`mcp/third_party/cpp-mcp/`）：

- **`POST .../mcp` + `initialize`**：**不要**带 **`Mcp-Session-Id`**；响应 **HTTP 头**会下发 **`Mcp-Session-Id`**，正文为 **JSON-RPC result**。
- **后续带 `id` 的 JSON-RPC 请求**（如 `tools/list`）：**必须**带请求头 **`Mcp-Session-Id: <上一步返回值>`**；且须先发送 **`notifications/initialized`**，否则 `process_request` 会返回 **`Session not initialized`**（见同文件 `is_session_initialized` 检查）。
- **仅含通知、且无带 `id` 的请求**时，服务端可返回 **HTTP 202**，正文可能为空。
- **`cpp-mcp` 客户端**用 `request::create_notification("initialized")` 发出的方法名为 **`notifications/initialized`**（前缀 `notifications/`），**JSON 中不出现 `id` 字段**。

请先本地启动服务，例如：`./learn_mcp_server --host 127.0.0.1 --port 8080`。

```bash
# 按你的监听地址修改
BASE=http://127.0.0.1:8080
HDR=/tmp/mcp-curl.headers
BODY=/tmp/mcp-curl.body
```

**（1）`initialize` — 建立会话，读取 `Mcp-Session-Id`**

```bash
curl -sS -D "$HDR" -o "$BODY" \
  -H 'Content-Type: application/json' \
  -H 'Accept: application/json' \
  -X POST "$BASE/mcp" \
  --data-binary '{"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2025-03-26","capabilities":{},"clientInfo":{"name":"curl-demo","version":"0.0.1"}}}'

# 从响应头取出会话 ID（macOS / Linux 常见写法）
SID=$(grep -i '^mcp-session-id:' "$HDR" | head -1 | awk '{print $2}' | tr -d '\r')
echo "Mcp-Session-Id=$SID"
# 查看 JSON-RPC 结果（无 jq 时可改用 python3 -m json.tool）
command -v jq >/dev/null && jq . <"$BODY" || cat "$BODY"
```

**（2）`notifications/initialized` — 通知（无 `id`），标记会话已就绪**

```bash
curl -sS -o /tmp/mcp-initdone.txt -w 'HTTP %{http_code}\n' \
  -H 'Content-Type: application/json' \
  -H "Mcp-Session-Id: $SID" \
  -X POST "$BASE/mcp" \
  --data-binary '{"jsonrpc":"2.0","method":"notifications/initialized","params":{}}'
# 常见为 202；正文可能为空
```

**（3）`tools/list`**

```bash
curl -sS \
  -H 'Content-Type: application/json' \
  -H "Mcp-Session-Id: $SID" \
  -X POST "$BASE/mcp" \
  --data-binary '{"jsonrpc":"2.0","id":2,"method":"tools/list","params":{}}' | (command -v jq >/dev/null && jq . || cat)
```

**（4）`tools/call` — 示例 `get_time`**

```bash
curl -sS \
  -H 'Content-Type: application/json' \
  -H "Mcp-Session-Id: $SID" \
  -X POST "$BASE/mcp" \
  --data-binary '{"jsonrpc":"2.0","id":3,"method":"tools/call","params":{"name":"get_time","arguments":{}}}' | (command -v jq >/dev/null && jq . || cat)
```

成功时 **`result`** 内常见形态为：`{ "content": [ { "type": "text", "text": "..." } ], "isError": false }`（由 `cpp-mcp` 的 `tools/call` 封装层与 handler 共同组成）。

**（5）`resources/read` — 示例静态 URI**

```bash
curl -sS \
  -H 'Content-Type: application/json' \
  -H "Mcp-Session-Id: $SID" \
  -X POST "$BASE/mcp" \
  --data-binary '{"jsonrpc":"2.0","id":4,"method":"resources/read","params":{"uri":"learn://docs/overview"}}' | (command -v jq >/dev/null && jq . || cat)
```

**（6）`ping`**

```bash
curl -sS \
  -H 'Content-Type: application/json' \
  -H "Mcp-Session-Id: $SID" \
  -X POST "$BASE/mcp" \
  --data-binary '{"jsonrpc":"2.0","id":5,"method":"ping","params":{}}' | (command -v jq >/dev/null && jq . || cat)
```

**（7）可选：关闭会话 — `DELETE /mcp`**

```bash
curl -sS -o /tmp/mcp-del.txt -w 'HTTP %{http_code}\n' \
  -H "Mcp-Session-Id: $SID" \
  -X DELETE "$BASE/mcp"
```

**排障提示**

- 若返回 **`Missing Mcp-Session-Id header`**：说明该请求被判定为非 `initialize`，但未带头。
- 若返回 **`Session not initialized`**：先检查是否已发 **`notifications/initialized`**（且方法名**不要**写成裸的 `initialized`，需与 `cpp-mcp` 一致）。
- **`initialize` 上再次携带已有 `Mcp-Session-Id`** 可能被拒绝（实现中视为重复初始化）。

---

## 五、核心源码导读（与仓库文件逐段对照）

以下片段均来自当前 `mcp/` 目录下的 **`CMakeLists.txt`** 与 **`src/learn_mcp_server.cpp`**，行号以你本地检出为准；这里侧重「这段代码在协议里承担什么角色」。

### 5.1 CMake：如何接上 `cpp-mcp`

解析顺序：**子模块路径 → 旧式平级目录 → FetchContent**。这样新同事只拉主仓库忘记子模块时仍有机会自动拉依赖，但生产/严肃学习仍推荐固定子模块提交。

```cmake
set(CPP_MCP_DEFAULT "${CMAKE_CURRENT_LIST_DIR}/third_party/cpp-mcp")
set(CPP_MCP_ROOT "${CPP_MCP_DEFAULT}" CACHE PATH "Path to cpp-mcp source tree (submodule: Andersonhere/cpp-mcp)")

if(EXISTS "${CPP_MCP_ROOT}/CMakeLists.txt")
  message(STATUS "Using cpp-mcp at ${CPP_MCP_ROOT}")
elseif(EXISTS "${CMAKE_CURRENT_LIST_DIR}/../cpp-mcp-upstream/CMakeLists.txt")
  ...
else()
  include(FetchContent)
  FetchContent_Declare(cpp_mcp_upstream GIT_REPOSITORY https://github.com/Andersonhere/cpp-mcp.git ...)
endif()

set(MCP_BUILD_TESTS OFF CACHE BOOL "" FORCE)
add_subdirectory("${CPP_MCP_ROOT}" "${CMAKE_BINARY_DIR}/_cpp_mcp")

add_executable(learn_mcp_server src/learn_mcp_server.cpp)
target_link_libraries(learn_mcp_server PRIVATE mcp)
target_include_directories(learn_mcp_server PRIVATE
  "${CPP_MCP_ROOT}/include"
  "${CPP_MCP_ROOT}/common")
```

- **`add_subdirectory(... _cpp_mcp)`**：把第三方 CMake 工程编进当前 build 树，生成名为 `mcp` 的库目标。
- **`target_link_libraries(... PRIVATE mcp)`**：你的可执行文件链接静态库，运行时自带 HTTP + JSON-RPC 服务端实现。
- **`include` + `common`**：暴露 `mcp_server.h`、`mcp_tool.h` 以及第三方头（如 nlohmann JSON、httplib）。

### 5.2 `main`：命令行、`data/` 与 `server` 配置

```cpp
std::string host = "0.0.0.0";
int port = 8080;
for (int i = 1; i < argc; ++i) {
    if (std::strcmp(argv[i], "--host") == 0 && i + 1 < argc) host = argv[++i];
    else if (std::strcmp(argv[i], "--port") == 0 && i + 1 < argc) port = std::atoi(argv[++i]);
    ...
}
const fs::path data_dir = fs::absolute(fs::path("data"));
fs::create_directories(data_dir);
// 写入 sample_notes.txt，供 file_resource 使用
```

- **`--host` / `--port`**：运维可调，无需重编译。
- **`data/` 与样例文件**：保证后续 `file_resource` 指向的磁盘路径**在注册前已存在**，避免构造失败。

```cpp
mcp::server::configuration srv_conf;
srv_conf.host = host;
srv_conf.port = port;
srv_conf.threadpool_size = std::max(2u, std::thread::hardware_concurrency());

mcp::server server(srv_conf);
server.set_server_info("learn_mcp_server", "0.1.0");
server.set_instructions("Learning MCP server (C++)...");
server.set_capabilities(mcp::json{
    {"tools", mcp::json{{"listChanged", false}}},
    {"resources", mcp::json{{"subscribe", false}, {"listChanged", false}}},
    {"prompts", mcp::json{{"listChanged", false}}},
});
```

- **`set_server_info`**：对应 MCP `initialize` 结果里的 server 标识。
- **`set_instructions`**：自然语言说明，帮助模型选择 tools / resources / prompts。
- **`set_capabilities`**：显式打开 **tools / resources / prompts** 三块能力；`listChanged` 等为 false 表示本示例不声明「列表会推送变更」。

### 5.3 资源：`text_resource` / `file_resource` / `binary_resource` / 模板

```cpp
auto overview = std::make_shared<mcp::text_resource>(
    "learn://docs/overview", "Overview", "text/markdown", "Static markdown resource (text_resource)");
overview->set_text("# learn_mcp_server\n\n- **Tools**: ...");
server.register_resource("learn://docs/overview", overview);

const std::string file_uri = std::string("file://") + sample_file.string();
auto notes = std::make_shared<mcp::file_resource>(sample_file.string(), "text/plain", "Notes file on server disk");
server.register_resource(file_uri, notes);

auto ping_blob = std::make_shared<mcp::binary_resource>(...);
ping_blob->set_data(bytes, sizeof(bytes));
server.register_resource("learn://blob/ping", ping_blob);
```

- **第一个参数（URI）** 既是业务 ID，也是客户端 `resources/read` 时传入的 `uri`。
- **`file://` + 绝对路径**：与 MCP 里「用 URI 表示资源」的习惯一致；注意权限与路径泄露风险（学习用进程勿直接暴露敏感目录）。

**资源模板**：lambda 接收 `uri_params`，把路径里的 `{message}` 解析成键值，返回带 `mimeType` 与 `text` 的 JSON——对应 **`resources/read` 对动态 URI 的响应体形状**（由库封装在协议帧里）。

```cpp
server.register_resource_template(
    "learn://echo/{message}", "Echo template", "text/plain", "...",
    [](const std::string& uri, const std::map<std::string, std::string>& uri_params, ...)
        -> mcp::json {
        const std::string msg = uri_params.count("message") ? uri_params.at("message") : "";
        return mcp::json{{"uri", uri}, {"mimeType", "text/plain"},
                         {"text", "Echo template resolved. Segment: " + msg}};
    });
```

### 5.4 工具：`tool_builder` 与 handler 的契约

**声明侧**：链式 API 生成带 `inputSchema` 的 tool 描述，并可加 **`readOnlyHint`** 注解。

```cpp
mcp::tool time_tool = mcp::tool_builder("get_time")
                          .with_description("Return the server's current local time (read-only)")
                          .with_annotations(mcp::json{{"readOnlyHint", true}})
                          .build();
server.register_tool(time_tool, get_time_handler);
```

**实现侧**：handler 接收 `params`（JSON）与 `session_id`，返回 **content 数组**；与 MCP `tools/call` 结果中的 `content` 列表一致。

```cpp
static mcp::json get_time_handler(const mcp::json& /*params*/, const std::string& /*session_id*/) {
    ...
    return mcp::json::array({{{"type", "text"}, {"text", "Server local time: " + time_str}}});
}
```

**错误路径**：`echo_handler` / `calculator_handler` 在参数缺失时 **`throw mcp::mcp_exception(mcp::error_code::invalid_params, "...")`**。库会映射为 JSON-RPC 错误，客户端可将该次 `tools/call` 标为失败或 `isError`，而无需你在 handler 里手写 HTTP。

```cpp
if (!params.contains("text") || !params["text"].is_string()) {
    throw mcp::mcp_exception(mcp::error_code::invalid_params, "Missing string field 'text'");
}
```

**`server_stats_handler`**：演示只读工具仍可返回「结构化信息」——此处拼了一段 JSON 字符串放在单个 `text` 块里；若客户端支持，也可扩展为多块或更细的 content type（仍以宿主实现为准）。

### 5.5 Prompts：`register_method` 手写 `prompts/list` 与 `prompts/get`

`register_prompt_handlers` 内对 **`prompts/list`** 返回 `prompts` 数组；若请求里带分页字段 `cursor`，则附加 **`nextCursor`**（此处置空字符串表示无下一页），与常见 MCP 客户端行为对齐。

**`prompts/get`** 根据 `name` 分支：

- **`code_review` / `summarize_topic`**：`messages` 里是一条 `role: user`、`content` 为 `{type: text, text: ...}` 的简单用户消息。
- **`with_embedded_readme`**：`content` 为 **`{type: resource, resource: {uri, mimeType, text}}`**，演示规范里「prompt 内嵌资源」形态，可与 `resources/read` 同一 URI 对照阅读。

---

## 六、与 MCP 规范概念的对应关系（便于对照官方文档）

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

## 七、建议的自测顺序（加深理解）

1. 启动 `learn_mcp_server`，确认日志里 **端口与 `/mcp`**。  
2. 在 Cursor 中连接后，看 **MCP Logs** 是否完成 initialize。  
3. 依次让 Agent：**列工具并调用** `get_time` / `calculator`；**读资源** `learn://docs/overview` 与 `file://.../sample_notes.txt`；**列模板并读** `learn://echo/test`。  
4. 使用 **Prompts**（若 Cursor UI 暴露）或等价客户端调用 `prompts/list` / `prompts/get`。  

---

## 八、延伸阅读与改进方向

- 阅读 `cpp-mcp` 仓库中的 `examples/server_example.cpp` 与 `include/mcp_server.h`，对照 **会话、`Mcp-Session-Id`、DELETE** 等 HTTP 传输细节。  
- 生产环境：**TLS**、**认证**、**仅内网或 VPN**、**最小权限工具**。  
- 若需 **stdio** 供本机 Cursor 直接拉起进程：需另一套启动方式（`command` + `args`），与当前「纯 HTTP 部署」并行学习对比。  

---

*文档版本与 `learn_mcp_server` 源码同步维护；路径以仓库内 `mcp/` 为准。*
