#ifndef LLM_COST_ROUTER_CLOUD_H
#define LLM_COST_ROUTER_CLOUD_H

#include <string>
#include <functional>
#include <nlohmann/json.hpp>
#include <httplib.h>

class CloudClient {
public:
    CloudClient(const std::string& baseUrl,
                const std::string& apiKey,
                const std::string& defaultModel);
    ~CloudClient() = default;

    // Non-streaming chat completion
    nlohmann::json chatCompletion(const nlohmann::json& request);

    // Streaming chat completion (SSE chunks via callback)
    // Returns the final accumulated response for cost tracking
    nlohmann::json chatCompletionStream(
        const nlohmann::json& request,
        std::function<void(const std::string& chunk)> onChunk);

    // Ensure model name is correct in request
    nlohmann::json prepareRequest(const nlohmann::json& request);

    // Check if backend is reachable
    bool health();

private:
    std::string m_baseUrl;
    std::string m_apiKey;
    std::string m_defaultModel;
    httplib::Client m_http;
};

#endif // LLM_COST_ROUTER_CLOUD_H
