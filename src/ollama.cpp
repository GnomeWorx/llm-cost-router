#include "ollama.h"
#include <iostream>
#include <sstream>

// RAII helper to decrement active requests counter
struct ActiveRequestGuard {
    std::atomic<int>& counter;
    ActiveRequestGuard(std::atomic<int>& c) : counter(c) { counter++; }
    ~ActiveRequestGuard() { counter--; }
};

OllamaClient::OllamaClient(const std::string& baseUrl)
    : m_baseUrl(baseUrl), m_http(baseUrl) {
    m_http.set_connection_timeout(30);
    m_http.set_read_timeout(120);
    m_http.set_write_timeout(30);
}

nlohmann::json OllamaClient::openAItoOllama(const nlohmann::json& openAIReq) {
    nlohmann::json ollamaReq;
    ollamaReq["model"] = openAIReq.value("model", "qwen2.5-coder:7b");
    ollamaReq["stream"] = openAIReq.value("stream", false);

    if (openAIReq.contains("messages")) {
        ollamaReq["messages"] = openAIReq["messages"];
    }

    // Options mapping
    nlohmann::json options;
    if (openAIReq.contains("temperature")) {
        options["temperature"] = openAIReq["temperature"];
    }
    if (openAIReq.contains("max_tokens")) {
        options["num_predict"] = openAIReq["max_tokens"];
    }
    if (openAIReq.contains("top_p")) {
        options["top_p"] = openAIReq["top_p"];
    }
    if (!options.empty()) {
        ollamaReq["options"] = options;
    }

    return ollamaReq;
}

nlohmann::json OllamaClient::ollamaToOpenAI(const nlohmann::json& ollamaResp,
                                             const std::string& model) {
    nlohmann::json resp;
    resp["id"] = "ollama-" + std::to_string(
        std::chrono::system_clock::now().time_since_epoch().count());
    resp["object"] = "chat.completion";
    resp["created"] = std::chrono::system_clock::to_time_t(
        std::chrono::system_clock::now());
    resp["model"] = model;

    nlohmann::json choice;
    choice["index"] = 0;
    choice["finish_reason"] = "stop";

    nlohmann::json msg;
    msg["role"] = "assistant";

    // Extract content — some models (minimax-m3:cloud) put the answer in "thinking"
    auto msgObj = ollamaResp.value("message", nlohmann::json({}));
    std::string content = msgObj.value("content", "");
    if (content.empty() && msgObj.contains("thinking")) {
        content = msgObj["thinking"].get<std::string>();
    }
    msg["content"] = content;
    choice["message"] = msg;
    resp["choices"] = {choice};

    // Usage
    nlohmann::json usage;
    usage["prompt_tokens"]     = ollamaResp.value("prompt_eval_count", 0);
    usage["completion_tokens"]  = ollamaResp.value("eval_count", 0);
    usage["total_tokens"]      = usage["prompt_tokens"].get<int>() +
                                 usage["completion_tokens"].get<int>();
    resp["usage"] = usage;

    return resp;
}

std::string OllamaClient::ollamaChunkToSSE(const nlohmann::json& chunk,
                                            const std::string& model,
                                            const std::string& id) {
    nlohmann::json sse;
    sse["id"] = id;
    sse["object"] = "chat.completion.chunk";
    sse["created"] = std::chrono::system_clock::to_time_t(
        std::chrono::system_clock::now());
    sse["model"] = model;

    nlohmann::json choice;
    choice["index"] = 0;
    choice["finish_reason"] = nullptr;

    nlohmann::json delta;
    if (chunk.contains("message") && chunk["message"].is_object()) {
        auto& msg = chunk["message"];
        if (msg.contains("content") && msg["content"].is_string() && !msg["content"].get<std::string>().empty()) {
            delta["content"] = msg["content"];
        } else if (msg.contains("thinking") && msg["thinking"].is_string()) {
            delta["content"] = msg["thinking"];
        } else {
            delta["content"] = "";
        }
    } else if (chunk.contains("response")) {
        delta["content"] = chunk["response"];
    } else {
        delta["content"] = "";
    }
    delta["role"] = "assistant";
    choice["delta"] = delta;
    sse["choices"] = {choice};

    return "data: " + sse.dump() + "\n\n";
}

nlohmann::json OllamaClient::chatCompletion(const nlohmann::json& request) {
    ActiveRequestGuard guard(m_activeRequests); // Increment on entry, decrement on exit
    auto ollamaReq = openAItoOllama(request);
    ollamaReq["stream"] = false;

    httplib::Headers headers = {{"Content-Type", "application/json"}};
    auto res = m_http.Post("/api/chat", headers, ollamaReq.dump(), "application/json");

    if (!res) {
        return {{"error", true}, {"message", "No response from Ollama"}};
    }

    if (res->status != 200) {
        return {{"error", true},
                {"status", res->status},
                {"message", res->body}};
    }

    try {
        auto ollamaResp = nlohmann::json::parse(res->body);
        return ollamaToOpenAI(ollamaResp, request.value("model", "qwen2.5-coder:7b"));
    } catch (...) {
        return {{"error", true}, {"message", "Failed to parse Ollama response"}};
    }
}

nlohmann::json OllamaClient::chatCompletionStream(
    const nlohmann::json& request,
    std::function<void(const std::string& chunk)> onChunk) {

    ActiveRequestGuard guard(m_activeRequests); // Increment on entry, decrement on exit

    auto ollamaReq = openAItoOllama(request);
    ollamaReq["stream"] = true;

    std::string id = "ollama-" + std::to_string(
        std::chrono::system_clock::now().time_since_epoch().count());
    std::string model = request.value("model", "qwen2.5-coder:7b");
    std::string fullContent;
    int promptTokens = 0;
    int evalTokens = 0;

    httplib::Headers headers = {{"Content-Type", "application/json"}};
    auto res = m_http.Post("/api/chat", headers, ollamaReq.dump(), "application/json",
        [&](const char* data, size_t data_len) {
            std::string chunk(data, data_len);
            try {
                auto j = nlohmann::json::parse(chunk);
                std::string sse = ollamaChunkToSSE(j, model, id);
                onChunk(sse);

                // Accumulate content
                if (j.contains("message") && j["message"].is_object()) {
                    auto& msg = j["message"];
                    if (msg.contains("content") && msg["content"].is_string()) {
                        fullContent += msg["content"].get<std::string>();
                    } else if (msg.contains("thinking") && msg["thinking"].is_string()) {
                        fullContent += msg["thinking"].get<std::string>();
                    }
                }
                if (j.contains("prompt_eval_count")) {
                    promptTokens = j["prompt_eval_count"];
                }
                if (j.contains("eval_count")) {
                    evalTokens = j["eval_count"];
                }

                // Check for done
                if (j.value("done", false)) {
                    // Send final [DONE] chunk
                    nlohmann::json finalChoice;
                    finalChoice["index"] = 0;
                    finalChoice["finish_reason"] = "stop";
                    finalChoice["delta"] = nlohmann::json::object();

                    nlohmann::json finalSse;
                    finalSse["id"] = id;
                    finalSse["object"] = "chat.completion.chunk";
                    finalSse["created"] = std::chrono::system_clock::to_time_t(
                        std::chrono::system_clock::now());
                    finalSse["model"] = model;
                    finalSse["choices"] = {finalChoice};

                    onChunk("data: " + finalSse.dump() + "\n\n");
                    onChunk("data: [DONE]\n\n");
                }
            } catch (...) {}
            return true;
        }
    );

    if (!res) {
        // Build error response
        nlohmann::json errSse;
        errSse["error"] = true;
        errSse["message"] = "Stream connection failed";
        onChunk("data: " + errSse.dump() + "\n\n");
        onChunk("data: [DONE]\n\n");
    }

    // Build accumulated response for cost tracking
    nlohmann::json finalResp;
    finalResp["content"] = fullContent;
    finalResp["prompt_tokens"] = promptTokens;
    finalResp["completion_tokens"] = evalTokens;
    finalResp["total_tokens"] = promptTokens + evalTokens;
    return finalResp;
}

bool OllamaClient::health() {
    auto res = m_http.Get("/api/tags");
    return res && res->status == 200;
}

void OllamaClient::setReadTimeout(int seconds) {
    m_http.set_read_timeout(seconds);
}
