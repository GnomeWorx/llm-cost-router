#include "cloud.h"
#include <iostream>
#include <sstream>

CloudClient::CloudClient(const std::string& baseUrl,
                          const std::string& apiKey,
                          const std::string& defaultModel)
    : m_baseUrl(baseUrl), m_apiKey(apiKey), m_defaultModel(defaultModel),
      m_http(baseUrl) {
    m_http.set_connection_timeout(30);
    m_http.set_read_timeout(120);
}

nlohmann::json CloudClient::chatCompletion(const nlohmann::json& request) {
    nlohmann::json body = prepareRequest(request);
    auto res = m_http.Post("/v1/chat/completions",
                            {{"Content-Type", "application/json"},
                             {"Authorization", "Bearer " + m_apiKey},
                             {"Accept", "application/json"}},
                            body.dump(),
                            "application/json");

    if (!res || res->status != 200) {
        int status = res ? res->status : 0;
        std::string errMsg = "Cloud API error (HTTP " + std::to_string(status) + ")";
        if (res) {
            try {
                auto errJson = nlohmann::json::parse(res->body);
                if (errJson.contains("error") && errJson["error"].is_object()) {
                    errMsg = errJson["error"].value("message", errMsg);
                }
            } catch (...) {}
        }
        return {{"error", true}, {"message", errMsg}, {"status", status}};
    }

    try {
        return nlohmann::json::parse(res->body);
    } catch (const std::exception& e) {
        // Include raw body snippet for diagnosing non-JSON responses
        std::string rawPreview = res->body.substr(0, 200);
        // sanitize: replace newline chars with space
        for (auto& c : rawPreview) { if (c == '\x0a') c = ' '; }
        return {{"error", true},
                {"message", std::string("Parse error: ") + e.what() + " | body: " + rawPreview},
                {"status", 502}};
    }
}

nlohmann::json CloudClient::chatCompletionStream(
    const nlohmann::json& request,
    std::function<void(const std::string& chunk)> onChunk)
{
    nlohmann::json body = prepareRequest(request);
    body["stream"] = true;

    nlohmann::json usage = {
        {"content", ""},
        {"prompt_tokens", 0},
        {"completion_tokens", 0},
        {"cache_tokens", 0}
    };

    auto res = m_http.Post("/v1/chat/completions",
        {{"Content-Type", "application/json"},
         {"Authorization", "Bearer " + m_apiKey},
         {"Accept", "text/event-stream"}},
        body.dump(),
        "application/json",
        [&](const char* data, size_t data_length) {
            std::string chunk(data, data_length);
            onChunk(chunk);

            // Split by SSE event boundaries (\x0a\x0a) to handle
            // multiple events in one TCP chunk (e.g. usage + [DONE])
            std::istringstream stream(chunk);
            std::string sseLine;
            while (std::getline(stream, sseLine)) {
                // Trim trailing \x0d
                if (!sseLine.empty() && sseLine.back() == '\x0d')
                    sseLine.pop_back();

                if (sseLine.find("data: ") != 0) continue;
                std::string payload = sseLine.substr(6);
                if (payload == "[DONE]") continue;

                try {
                    auto j = nlohmann::json::parse(payload);
                    if (j.contains("choices") && j["choices"].is_array() && !j["choices"].empty()) {
                        auto& delta = j["choices"][0]["delta"];
                        if (delta.contains("content") && delta["content"].is_string()) {
                            usage["content"] = usage["content"].get<std::string>() + delta["content"].get<std::string>();
                        }
                    }
                    if (j.contains("usage")) {
                        auto& u = j["usage"];
                        usage["prompt_tokens"] = u.value("prompt_tokens", 0);
                        usage["completion_tokens"] = u.value("completion_tokens", 0);
                        usage["cache_tokens"] = u.value("prompt_cache_hit_tokens", 0);
                    }
                } catch (...) {}
            }
            return true;
        });

    if (!res) {
        usage["error"] = true;
        usage["message"] = "Cloud stream connection failed";
    }

    return usage;
}

nlohmann::json CloudClient::prepareRequest(const nlohmann::json& req) {
    nlohmann::json j = req;
    if (!j.contains("model") || !j["model"].is_string() || j["model"].get<std::string>().empty()) {
        j["model"] = m_defaultModel;
    }
    return j;
}

bool CloudClient::health() {
    auto res = m_http.Get("/v1/models",
        {{"Authorization", "Bearer " + m_apiKey}});
    if (res && res->status == 200) return true;

    // Also try root path for some providers
    res = m_http.Get("/",
        {{"Authorization", "Bearer " + m_apiKey}});
    return res && res->status < 500;
}
