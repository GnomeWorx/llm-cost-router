#include "router.h"
#include "config.h"
#include "cost.h"
#include "ollama.h"
#include "cloud.h"
#include <algorithm>
#include <cctype>
#include <sstream>
#include <iostream>

Router::Router(const AppConfig& config, OllamaClient& ollama, CloudClient& cloud)
    : m_config(config), m_ollama(ollama), m_cloud(cloud) {}

// ── Helpers ──

// Rough token estimate: ~4 chars per token
static int estimateTokenCount(const nlohmann::json& request) {
    int chars = 0;
    if (request.contains("messages") && request["messages"].is_array()) {
        for (const auto& msg : request["messages"]) {
            if (msg.contains("content") && msg["content"].is_string()) {
                chars += msg["content"].get<std::string>().size();
            }
            if (msg.contains("role") && msg["role"].is_string()) {
                chars += msg["role"].get<std::string>().size();
            }
        }
    }
    // System prompt if present
    if (request.contains("system") && request["system"].is_string()) {
        chars += request["system"].get<std::string>().size();
    }
    return chars / 4 + 1;  // floor division + 1 to avoid zero
}

// Case-insensitive substring search in all message content
static bool hasEscalateKeywords(const nlohmann::json& request,
                                 const std::vector<std::string>& keywords) {
    if (keywords.empty()) return false;

    // Collect all text from messages
    std::string allText;
    if (request.contains("messages") && request["messages"].is_array()) {
        for (const auto& msg : request["messages"]) {
            if (msg.contains("content") && msg["content"].is_string()) {
                allText += msg["content"].get<std::string>() + " ";
            }
        }
    }

    // Convert to lowercase for case-insensitive matching
    std::string lower = allText;
    std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);

    for (const auto& kw : keywords) {
        std::string kwLower = kw;
        std::transform(kwLower.begin(), kwLower.end(), kwLower.begin(), ::tolower);
        if (lower.find(kwLower) != std::string::npos) {
            return true;
        }
    }
    return false;
}

static double calculateCost(const std::string& backend,
                             int64_t inTokens, int64_t outTokens, int64_t cacheTokens) {
    if (backend == "ollama") return 0.0;
    // DeepSeek pricing
    return (inTokens / 1000.0) * COST_DEEPSEEK_INPUT +
           (outTokens / 1000.0) * COST_DEEPSEEK_OUTPUT +
           (cacheTokens / 1000.0) * COST_DEEPSEEK_CACHE;
}

// Strip _meta from request before forwarding to backends
static nlohmann::json stripMeta(const nlohmann::json& req) {
    nlohmann::json clean = req;
    clean.erase("_meta");
    return clean;
}

// ── decide() ──
RouteDecision Router::decide(const nlohmann::json& request) {
    RouteDecision d;
    const auto& rc = m_config.router;

    // 0. If the requested model matches the Ollama backend model, always route to Ollama
    if (request.contains("model") && request["model"].is_string()) {
        std::string reqModel = request["model"];
        if (reqModel == m_config.ollama.model) {
            d.backend = "ollama";
            d.reason = "model matches ollama backend: " + reqModel;
            goto resolve_model;
        }
    }

    // 1. Force override from header (stored in _meta.force_backend)
    if (request.contains("_meta") && request["_meta"].is_object() &&
        request["_meta"].contains("force_backend")) {
        std::string fb = request["_meta"]["force_backend"];
        if (fb == "ollama" || fb == "cloud") {
            d.backend = fb;
            d.reason = "header override";
            goto resolve_model;
        }
    }

    // 2. Model name suffix -cloud forces cloud
    if (request.contains("model") && request["model"].is_string()) {
        std::string model = request["model"];
        if (model.size() > 6 && model.substr(model.size() - 6) == "-cloud") {
            d.backend = "cloud";
            d.reason = "model name ends with -cloud";
            goto resolve_model;
        }
    }

    // 3. Token threshold
    {
        int estTokens = estimateTokenCount(request);
        if (estTokens < rc.tokenThreshold) {
            d.backend = rc.defaultBackend;
            d.reason = "estimated tokens (" + std::to_string(estTokens) +
                       ") < threshold (" + std::to_string(rc.tokenThreshold) + ")";
            goto resolve_model;
        }
    }

    // 4. Keyword escalation
    if (rc.keywordEscalation && hasEscalateKeywords(request, rc.escalateKeywords)) {
        d.backend = "cloud";
        d.reason = "keyword escalation triggered";
        goto resolve_model;
    }

    // 5. Fallback to default
    d.backend = rc.defaultBackend;
    d.reason = "default backend (fallback)";

resolve_model:
    if (d.backend == "ollama") {
        d.model = m_config.ollama.model;
        d.provider = "ollama";
    } else {
        d.model = request.value("model", m_config.cloud.model);
        // Strip -cloud suffix if present
        if (d.model.size() > 6 && d.model.substr(d.model.size() - 6) == "-cloud") {
            d.model = d.model.substr(0, d.model.size() - 6);
        }
        d.provider = m_config.cloud.provider;
    }

    return d;
}

// ── route() — non-streaming ──
RouteResult Router::route(const RouteDecision& decision,
                          const nlohmann::json& request) {
    RouteResult result;
    result.model = decision.model;
    result.provider = decision.provider;

    nlohmann::json backendReq = stripMeta(request);
    backendReq["model"] = decision.model;

    try {
        nlohmann::json backendResp;

        if (decision.backend == "ollama") {
            backendResp = m_ollama.chatCompletion(backendReq);
        } else {
            backendResp = m_cloud.chatCompletion(backendReq);
        }

        // Check for errors
        if (backendResp.contains("error") && backendResp["error"] == true) {
            result.error = true;
            result.errorMessage = backendResp.value("message", "Unknown backend error");
            result.statusCode = backendResp.value("status", 502);
            return result;
        }

        result.response = backendResp;

        // Extract token usage
        if (backendResp.contains("usage")) {
            auto& u = backendResp["usage"];
            result.inputTokens  = u.value("prompt_tokens", 0LL);
            result.outputTokens = u.value("completion_tokens", 0LL);
            result.cacheTokens  = u.value("prompt_cache_hit_tokens", 0LL);
        }

        result.cost = calculateCost(decision.backend,
                                     result.inputTokens,
                                     result.outputTokens,
                                     result.cacheTokens);

    } catch (const std::exception& e) {
        result.error = true;
        result.errorMessage = "Backend exception: " + std::string(e.what());
        result.statusCode = 502;
    }

    return result;
}

// ── routeStream() — streaming ──
RouteResult Router::routeStream(
    const RouteDecision& decision,
    const nlohmann::json& request,
    std::function<void(const std::string& chunk)> onChunk)
{
    RouteResult result;
    result.model = decision.model;
    result.provider = decision.provider;

    nlohmann::json backendReq = stripMeta(request);
    backendReq["model"] = decision.model;
    backendReq["stream"] = true;

    try {
        nlohmann::json acc;

        if (decision.backend == "ollama") {
            acc = m_ollama.chatCompletionStream(backendReq, onChunk);
        } else {
            acc = m_cloud.chatCompletionStream(backendReq, onChunk);
        }

        // Extract token usage from accumulated response
        result.inputTokens  = acc.value("prompt_tokens", 0LL);
        result.outputTokens = acc.value("completion_tokens", 0LL);
        result.cacheTokens  = acc.value("cache_tokens", 0LL);
        result.cost = calculateCost(decision.backend,
                                     result.inputTokens,
                                     result.outputTokens,
                                     result.cacheTokens);

    } catch (const std::exception& e) {
        result.error = true;
        result.errorMessage = "Stream exception: " + std::string(e.what());
        result.statusCode = 502;
    }

    return result;
}
