/**
 * Learning-oriented MCP server built on hkr04/cpp-mcp (HTTP + Streamable HTTP + legacy SSE).
 *
 * Covers: tools/list + tools/call, resources (static text, file, binary, URI template),
 * prompts/list + prompts/get (via register_method), initialize instructions, ping.
 *
 * Run: learn_mcp_server [--host 0.0.0.0] [--port 8080]
 * Remote URL for Streamable HTTP clients: http://HOST:PORT/mcp
 */

#include "mcp_resource.h"
#include "mcp_server.h"
#include "mcp_tool.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>

namespace fs = std::filesystem;

static const std::chrono::steady_clock::time_point k_start = std::chrono::steady_clock::now();

static std::string steady_uptime_seconds() {
    const auto d = std::chrono::steady_clock::now() - k_start;
    const auto s = std::chrono::duration_cast<std::chrono::seconds>(d).count();
    return std::to_string(s);
}

static mcp::json get_time_handler(const mcp::json& /*params*/, const std::string& /*session_id*/) {
    const auto now = std::chrono::system_clock::now();
    const std::time_t t = std::chrono::system_clock::to_time_t(now);
    std::string time_str = std::ctime(&t);
    if (!time_str.empty() && time_str.back() == '\n') {
        time_str.pop_back();
    }
    return mcp::json::array({{{"type", "text"}, {"text", "Server local time: " + time_str}}});
}

static mcp::json echo_handler(const mcp::json& params, const std::string& /*session_id*/) {
    if (!params.contains("text") || !params["text"].is_string()) {
        throw mcp::mcp_exception(mcp::error_code::invalid_params, "Missing string field 'text'");
    }
    std::string text = params["text"].get<std::string>();
    if (params.contains("uppercase") && params["uppercase"].get<bool>()) {
        std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
    }
    if (params.contains("reverse") && params["reverse"].get<bool>()) {
        std::reverse(text.begin(), text.end());
    }
    return mcp::json::array({{{"type", "text"}, {"text", text}}});
}

static mcp::json calculator_handler(const mcp::json& params, const std::string& /*session_id*/) {
    if (!params.contains("operation")) {
        throw mcp::mcp_exception(mcp::error_code::invalid_params, "Missing 'operation'");
    }
    const std::string op = params["operation"].get<std::string>();
    if (!params.contains("a") || !params.contains("b")) {
        throw mcp::mcp_exception(mcp::error_code::invalid_params, "Missing 'a' or 'b'");
    }
    const double a = params["a"].get<double>();
    const double b = params["b"].get<double>();
    double out = 0.0;
    if (op == "add") {
        out = a + b;
    } else if (op == "subtract") {
        out = a - b;
    } else if (op == "multiply") {
        out = a * b;
    } else if (op == "divide") {
        if (b == 0.0) {
            throw mcp::mcp_exception(mcp::error_code::invalid_params, "Division by zero");
        }
        out = a / b;
    } else {
        throw mcp::mcp_exception(mcp::error_code::invalid_params, "Unknown operation: " + op);
    }
    return mcp::json::array({{{"type", "text"}, {"text", std::to_string(out)}}});
}

static mcp::json server_stats_handler(const mcp::json& /*params*/, const std::string& session_id) {
    std::ostringstream oss;
    oss << "{\n"
        << "  \"uptimeSeconds\": " << steady_uptime_seconds() << ",\n"
        << "  \"sessionId\": \"" << session_id << "\",\n"
        << "  \"cppMcpProtocol\": \"" << mcp::MCP_VERSION << "\"\n"
        << "}";
    return mcp::json::array({{{"type", "text"}, {"text", oss.str()}}});
}

static void register_prompt_handlers(mcp::server& server) {
    server.register_method("prompts/list", [](const mcp::json& params, const std::string& /*session_id*/) -> mcp::json {
        mcp::json prompts = mcp::json::array();
        prompts.push_back({
            {"name", "code_review"},
            {"description", "Structured request to review a snippet for bugs and style"},
            {"arguments",
             mcp::json::array({
                 {{"name", "code"}, {"description", "Source code to review"}, {"required", true}},
             })},
        });
        prompts.push_back({
            {"name", "summarize_topic"},
            {"description", "Ask the model to summarize a topic in a few bullets"},
            {"arguments",
             mcp::json::array({
                 {{"name", "topic"}, {"description", "What to summarize"}, {"required", true}},
                 {{"name", "audience"}, {"description", "Who the summary is for"}, {"required", false}},
             })},
        });
        prompts.push_back({
            {"name", "with_embedded_readme"},
            {"description", "Demonstrates embedded resource content inside a prompt message (spec 2025-03-26)"},
            {"arguments", mcp::json::array()},
        });

        mcp::json result{{"prompts", prompts}};
        if (params.contains("cursor")) {
            result["nextCursor"] = "";
        }
        return result;
    });

    server.register_method("prompts/get", [](const mcp::json& params, const std::string& /*session_id*/) -> mcp::json {
        if (!params.contains("name") || !params["name"].is_string()) {
            throw mcp::mcp_exception(mcp::error_code::invalid_params, "Missing prompt name");
        }
        const std::string name = params["name"].get<std::string>();
        const mcp::json args = params.contains("arguments") ? params["arguments"] : mcp::json::object();

        if (name == "code_review") {
            const std::string code = args.value("code", "// paste your code here");
            const std::string body = "Review the following code for correctness, edge cases, and style. "
                                     "Suggest concrete improvements.\n\n```\n" +
                                     code + "\n```";
            return mcp::json{
                {"description", "Code review"},
                {"messages",
                 mcp::json::array({
                     {{"role", "user"}, {"content", mcp::json{{"type", "text"}, {"text", body}}}},
                 })},
            };
        }
        if (name == "summarize_topic") {
            const std::string topic = args.value("topic", "MCP transports");
            const std::string audience = args.value("audience", "a backend engineer");
            const std::string body = "Summarize \"" + topic + "\" in 5 short bullets for " + audience + ".";
            return mcp::json{
                {"description", "Topic summary"},
                {"messages",
                 mcp::json::array({
                     {{"role", "user"}, {"content", mcp::json{{"type", "text"}, {"text", body}}}},
                 })},
            };
        }
        if (name == "with_embedded_readme") {
            const mcp::json resource_block = {
                {"uri", "learn://docs/overview"},
                {"mimeType", "text/markdown"},
                {"text",
                 "# learn_mcp_server\n\nThis prompt embeds a **resource** block per MCP prompts spec. "
                 "Compare with `resources/read` on `learn://docs/overview`.\n"},
            };
            return mcp::json{
                {"description", "Embedded resource demo"},
                {"messages",
                 mcp::json::array({
                     {{"role", "user"},
                      {"content",
                       mcp::json{
                           {"type", "resource"},
                           {"resource", resource_block},
                       }}},
                 })},
            };
        }

        throw mcp::mcp_exception(mcp::error_code::invalid_params, "Unknown prompt: " + name);
    });
}

int main(int argc, char** argv) {
    std::string host = "0.0.0.0";
    int port = 8080;

    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--host") == 0 && i + 1 < argc) {
            host = argv[++i];
        } else if (std::strcmp(argv[i], "--port") == 0 && i + 1 < argc) {
            port = std::atoi(argv[++i]);
        } else if (std::strcmp(argv[i], "--help") == 0 || std::strcmp(argv[i], "-h") == 0) {
            std::fprintf(stderr, "Usage: %s [--host HOST] [--port PORT]\n", argv[0]);
            return 0;
        }
    }

    const fs::path data_dir = fs::absolute(fs::path("data"));
    fs::create_directories(data_dir);
    const fs::path sample_file = data_dir / "sample_notes.txt";
    {
        std::ofstream out(sample_file, std::ios::binary);
        out << "This file backs the `file_resource` demo URI.\n"
            << "Try resources/read with the printed URI after startup.\n";
    }

    mcp::server::configuration srv_conf;
    srv_conf.host = host;
    srv_conf.port = port;
    srv_conf.threadpool_size = std::max(2u, std::thread::hardware_concurrency());

    mcp::server server(srv_conf);
    server.set_server_info("learn_mcp_server", "0.1.0");
    server.set_instructions(
        "Learning MCP server (C++). Use tools for side effects and computation, resources/read for "
        "stable context, resources/templates/list + dynamic URIs for parameterized documents, and "
        "prompts/get for reusable prompt templates. Streamable HTTP endpoint: POST/GET /mcp "
        "(see cpp-mcp / MCP 2025-03-26).");

    server.set_capabilities(mcp::json{
        {"tools", mcp::json{{"listChanged", false}}},
        {"resources", mcp::json{{"subscribe", false}, {"listChanged", false}}},
        {"prompts", mcp::json{{"listChanged", false}}},
    });

    // --- Resources: static text, on-disk file, small binary blob, URI template ---
    auto overview = std::make_shared<mcp::text_resource>(
        "learn://docs/overview",
        "Overview",
        "text/markdown",
        "Static markdown resource (text_resource)");
    overview->set_text(
        "# learn_mcp_server\n\n"
        "- **Tools**: `get_time`, `echo`, `calculator`, `server_stats`\n"
        "- **Resources**: this URI, `file://...` sample file, `learn://blob/ping`, template `learn://echo/{message}`\n"
        "- **Prompts**: `code_review`, `summarize_topic`, `with_embedded_readme`\n");

    server.register_resource("learn://docs/overview", overview);

    const std::string file_uri = std::string("file://") + sample_file.string();
    auto notes = std::make_shared<mcp::file_resource>(sample_file.string(), "text/plain", "Notes file on server disk");
    server.register_resource(file_uri, notes);

    auto ping_blob = std::make_shared<mcp::binary_resource>(
        "learn://blob/ping",
        "Tiny blob",
        "application/octet-stream",
        "Two-byte binary resource (base64 in resources/read)");
    const uint8_t bytes[2] = {'M', 'C'};
    ping_blob->set_data(bytes, sizeof(bytes));
    server.register_resource("learn://blob/ping", ping_blob);

    server.register_resource_template(
        "learn://echo/{message}",
        "Echo template",
        "text/plain",
        "RFC6570-style template: replace {message} in the URI path",
        [](const std::string& uri, const std::map<std::string, std::string>& uri_params, const std::string& /*session*/)
            -> mcp::json {
            const std::string msg = uri_params.count("message") ? uri_params.at("message") : "";
            return mcp::json{
                {"uri", uri},
                {"mimeType", "text/plain"},
                {"text", "Echo template resolved. Segment: " + msg},
            };
        });

    // --- Tools ---
    mcp::tool time_tool = mcp::tool_builder("get_time")
                              .with_description("Return the server's current local time (read-only)")
                              .with_annotations(mcp::json{{"readOnlyHint", true}})
                              .build();

    mcp::tool echo_tool = mcp::tool_builder("echo")
                              .with_description("Echo text with optional uppercase / reverse transforms")
                              .with_string_param("text", "Input text", true)
                              .with_boolean_param("uppercase", "If true, fold to uppercase", false)
                              .with_boolean_param("reverse", "If true, reverse characters", false)
                              .build();

    mcp::tool calc_tool = mcp::tool_builder("calculator")
                              .with_description("add | subtract | multiply | divide on two numbers")
                              .with_string_param("operation", "One of add, subtract, multiply, divide", true)
                              .with_number_param("a", "Left operand", true)
                              .with_number_param("b", "Right operand", true)
                              .build();

    mcp::tool stats_tool = mcp::tool_builder("server_stats")
                               .with_description("Process uptime and active MCP session id (read-only)")
                               .with_annotations(mcp::json{{"readOnlyHint", true}})
                               .build();

    server.register_tool(time_tool, get_time_handler);
    server.register_tool(echo_tool, echo_handler);
    server.register_tool(calc_tool, calculator_handler);
    server.register_tool(stats_tool, server_stats_handler);

    register_prompt_handlers(server);

    std::fprintf(stderr, "learn_mcp_server listening on http://%s:%d\n", host.c_str(), port);
    std::fprintf(stderr, "  Streamable HTTP (2025-03-26): POST/GET/DELETE  path %s\n", srv_conf.mcp_endpoint.c_str());
    std::fprintf(stderr, "  Legacy HTTP+SSE:               SSE path %s , message path %s\n", srv_conf.sse_endpoint.c_str(),
        srv_conf.msg_endpoint.c_str());
    std::fprintf(stderr, "Sample static resource URI: learn://docs/overview\n");
    std::fprintf(stderr, "Sample file resource URI:  %s\n", file_uri.c_str());
    std::fprintf(stderr, "Sample template URI:       learn://echo/hello-mcp\n");

    server.start(true);
    return 0;
}
