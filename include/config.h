#ifndef LLM_COST_ROUTER_CONFIG_H
#define LLM_COST_ROUTER_CONFIG_H

#include <string>
#include <vector>
#include <nlohmann/json.hpp>

struct RouterConfig {
    std::string defaultBackend = "ollama";   // "ollama" or "cloud"
    int         tokenThreshold  = 1200;       // prompts under this → defaultBackend
    bool        keywordEscalation = true;     // trigger keywords force cloud
    bool        llmJudgeEnabled = false;      // Phase 3
    double      dailyBudgetUsd = 15.0;        // hard cap

    std::vector<std::string> escalateKeywords = {
        "architecture", "design", "refactor", "plan", "deploy",
        "security", "optimize", "complex", "analyse", "investigate",
        "strategy", "migration", "deadlock", "race condition"
    };

    // Retry on failure mode — try Ollama first, only fall back to cloud if it refuses
    bool retryOnFailure = false;
    int  retryMaxTokens  = 3000;  // skip retry if estimated tokens exceed this (Ollama can't handle huge system prompts)
    int  retryTimeoutMs  = 30000; // max ms to wait for Ollama before escalating to cloud
    std::vector<std::string> retryRefusalPatterns = {
        "i cannot", "i'm sorry", "i am sorry", "i don't have",
        "i am not able", "i'm not able", "i do not have",
        "i don't have enough", "i do not have enough",
        "insufficient", "i cannot provide",
        "i'm just an ai", "i am an ai", "as an ai",
        "i cannot assist", "i cannot answer",
        "i don't know", "i do not know", "i'm not sure",
        "unable to", "not able to"
    };
};

struct OllamaConfig {
    std::string baseUrl = "http://192.168.1.154:11434";
    std::string model   = "qwen2.5-coder:7b";
};

struct CloudConfig {
    std::string provider   = "deepseek";
    std::string baseUrl    = "https://api.deepseek.com";
    std::string apiKey     = "";
    std::string model      = "deepseek-v4-flash";
};

struct ServerConfig {
    int port = 8100;
    std::string logPath = std::string(getenv("HOME") ? getenv("HOME") : "") + "/.hermes/cost-router/usage.log";
};

struct AppConfig {
    RouterConfig router;
    OllamaConfig ollama;
    CloudConfig  cloud;
    ServerConfig server;
    bool         costTracking = true;
};

inline void to_json(nlohmann::json& j, const RouterConfig& r) {
    j = nlohmann::json{
        {"default_backend", r.defaultBackend},
        {"token_threshold", r.tokenThreshold},
        {"keyword_escalation", r.keywordEscalation},
        {"llm_judge_enabled", r.llmJudgeEnabled},
        {"daily_budget_usd", r.dailyBudgetUsd},
        {"escalate_keywords", r.escalateKeywords},
        {"retry_on_failure", r.retryOnFailure},
        {"retry_max_tokens", r.retryMaxTokens},
        {"retry_timeout_ms", r.retryTimeoutMs},
        {"retry_refusal_patterns", r.retryRefusalPatterns}
    };
}

inline void from_json(const nlohmann::json& j, RouterConfig& r) {
    r.defaultBackend       = j.value("default_backend", r.defaultBackend);
    r.tokenThreshold       = j.value("token_threshold", r.tokenThreshold);
    r.keywordEscalation    = j.value("keyword_escalation", r.keywordEscalation);
    r.llmJudgeEnabled      = j.value("llm_judge_enabled", r.llmJudgeEnabled);
    r.dailyBudgetUsd       = j.value("daily_budget_usd", r.dailyBudgetUsd);
    r.escalateKeywords     = j.value("escalate_keywords", r.escalateKeywords);
    r.retryOnFailure       = j.value("retry_on_failure", r.retryOnFailure);
    r.retryMaxTokens       = j.value("retry_max_tokens", r.retryMaxTokens);
    r.retryTimeoutMs       = j.value("retry_timeout_ms", r.retryTimeoutMs);
    r.retryRefusalPatterns = j.value("retry_refusal_patterns", r.retryRefusalPatterns);
}

inline void to_json(nlohmann::json& j, const OllamaConfig& o) {
    j = nlohmann::json{{"base_url", o.baseUrl}, {"model", o.model}};
}

inline void from_json(const nlohmann::json& j, OllamaConfig& o) {
    o.baseUrl = j.value("base_url", o.baseUrl);
    o.model   = j.value("model", o.model);
}

inline void to_json(nlohmann::json& j, const CloudConfig& c) {
    j = nlohmann::json{
        {"provider", c.provider},
        {"base_url", c.baseUrl},
        {"api_key", c.apiKey},
        {"model", c.model}
    };
}

inline void from_json(const nlohmann::json& j, CloudConfig& c) {
    c.provider = j.value("provider", c.provider);
    c.baseUrl  = j.value("base_url", c.baseUrl);
    c.apiKey   = j.value("api_key", c.apiKey);
    c.model    = j.value("model", c.model);
}

inline void to_json(nlohmann::json& j, const ServerConfig& s) {
    j = nlohmann::json{{"port", s.port}, {"log_path", s.logPath}};
}

inline void from_json(const nlohmann::json& j, ServerConfig& s) {
    s.port    = j.value("port", s.port);
    s.logPath = j.value("log_path", s.logPath);
}

inline void to_json(nlohmann::json& j, const AppConfig& a) {
    j = nlohmann::json{
        {"router", a.router},
        {"ollama", a.ollama},
        {"cloud", a.cloud},
        {"server", a.server},
        {"cost_tracking", a.costTracking}
    };
}

inline void from_json(const nlohmann::json& j, AppConfig& a) {
    if (j.contains("router")) j["router"].get_to(a.router);
    if (j.contains("ollama")) j["ollama"].get_to(a.ollama);
    if (j.contains("cloud"))  j["cloud"].get_to(a.cloud);
    if (j.contains("server")) j["server"].get_to(a.server);
    a.costTracking = j.value("cost_tracking", a.costTracking);
}

class Config {
public:
    static AppConfig load(const std::string& path);
    static void save(const std::string& path, const AppConfig& cfg);
    static AppConfig defaultConfig();
    static std::string defaultConfigPath();
};

#endif // LLM_COST_ROUTER_CONFIG_H
