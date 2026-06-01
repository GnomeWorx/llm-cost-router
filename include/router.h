#ifndef LLM_COST_ROUTER_ROUTER_H
#define LLM_COST_ROUTER_ROUTER_H

#include <string>
#include <functional>
#include <nlohmann/json.hpp>

// ── Forward declarations ──
class OllamaClient;
class CloudClient;
struct AppConfig;

// =============================================================================
// RouteDecision — result of the routing logic
// =============================================================================
struct RouteDecision {
    std::string backend;        // "ollama" or "cloud"
    std::string model;          // resolved model name for the chosen backend
    std::string provider;       // e.g. "deepseek", "ollama"
    std::string reason;         // human-readable reason for the decision
    double      estimatedCost;  // estimated cost for this request
};

// =============================================================================
// RouteResult — response from the chosen backend after execution
// =============================================================================
struct RouteResult {
    nlohmann::json response;    // fully translated OpenAI-format response
    std::string    model;       // actual model used
    std::string    provider;    // actual provider
    int64_t        inputTokens  = 0;
    int64_t        outputTokens = 0;
    int64_t        cacheTokens  = 0;
    double         cost         = 0.0;
    bool           error        = false;
    std::string    errorMessage;
    int            statusCode   = 200;
};

// =============================================================================
// Router — decides which backend to use and routes the request
// =============================================================================
class Router {
public:
    Router(const AppConfig& config,
           OllamaClient& ollama,
           CloudClient& cloud);

    // Analyse the incoming request and decide which backend to route to
    RouteDecision decide(const nlohmann::json& request);

    // Execute a non-streaming request against the chosen backend
    RouteResult route(const RouteDecision& decision,
                      const nlohmann::json& request);

    // Execute a streaming request; onChunk receives SSE-formatted text chunks
    // Returns the accumulated RouteResult for cost-tracking purposes
    RouteResult routeStream(
        const RouteDecision& decision,
        const nlohmann::json& request,
        std::function<void(const std::string& chunk)> onChunk);

private:
    const AppConfig& m_config;
    OllamaClient&    m_ollama;
    CloudClient&     m_cloud;
};

#endif // LLM_COST_ROUTER_ROUTER_H
