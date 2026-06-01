#ifndef LLM_COST_ROUTER_OLLAMA_H
#define LLM_COST_ROUTER_OLLAMA_H

#include <string>
#include <functional>
#include <nlohmann/json.hpp>
#include <httplib.h>

class OllamaClient {
public:
    explicit OllamaClient(const std::string& baseUrl);
    ~OllamaClient() = default;

    // Non-streaming chat completion
    nlohmann::json chatCompletion(const nlohmann::json& request);

    // Streaming chat completion (SSE chunks via callback)
    // Returns the final accumulated response
    nlohmann::json chatCompletionStream(
        const nlohmann::json& request,
        std::function<void(const std::string& chunk)> onChunk);

    // Translate OpenAI-style request to Ollama-style
    static nlohmann::json openAItoOllama(const nlohmann::json& openAIReq);

    // Translate Ollama-style response to OpenAI-style
    static nlohmann::json ollamaToOpenAI(const nlohmann::json& ollamaResp,
                                         const std::string& model);

    // Translate Ollama streaming chunk to OpenAI SSE format
    static std::string ollamaChunkToSSE(const nlohmann::json& chunk,
                                         const std::string& model,
                                         const std::string& id);

    // Check if backend is reachable
    bool health();

    // Override read timeout for retry-mode calls (shorter timeout)
    void setReadTimeout(int seconds);

private:
    std::string m_baseUrl;
    httplib::Client m_http;
};

#endif // LLM_COST_ROUTER_OLLAMA_H
