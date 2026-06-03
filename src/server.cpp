#include <iostream>
#include <sstream>
#include <iomanip>
#include <queue>
#include <mutex>
#include <condition_variable>
#include <memory>
#include <chrono>
#include <algorithm>
#include <cctype>

#include "server.h"

using json = nlohmann::json;

// =============================================================================
// StreamState — shared state between the stream thread and the HTTP sink
// =============================================================================
struct StreamState {
    std::queue<std::string>     chunks;      // SSE-formatted chunks
    std::mutex                  mutex;
    std::condition_variable     cv;
    RouteResult                 result;      // final accumulated result
    bool                        done = false;
};

// =============================================================================
// Refusal detection — checks if an LLM response is a refusal or low-quality
// =============================================================================
static std::string extractContent(const nlohmann::json& response) {
    std::string content;
    if (response.contains("choices") && response["choices"].is_array()
        && !response["choices"].empty()) {
        auto& choice = response["choices"][0];
        if (choice.contains("message") && choice["message"].contains("content")) {
            content = choice["message"]["content"].get<std::string>();
        } else if (choice.contains("delta") && choice["delta"].contains("content")) {
            content = choice["delta"]["content"].get<std::string>();
        }
    }
    return content;
}

static bool isRefusalResponse(const nlohmann::json& response,
                               const std::vector<std::string>& patterns) {
    // Error from the backend counts as refusal
    if (response.contains("error") && response["error"] == true) return true;
    if (response.contains("error") && response["error"].is_string()
        && !response["error"].get<std::string>().empty()) return true;

    std::string content = extractContent(response);

    // Empty or trivial content (<5 chars) counts as refusal
    if (content.size() < 5) return true;

    // Check against refusal patterns (case-insensitive)
    std::string lower = content;
    std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);

    for (const auto& pat : patterns) {
        if (lower.find(pat) != std::string::npos) return true;
    }

    return false;
}

// ── Helper: rough token estimate (chars/4 + 1) ──
static int estimateRequestTokens(const nlohmann::json& request) {
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
    if (request.contains("system") && request["system"].is_string()) {
        chars += request["system"].get<std::string>().size();
    }
    return chars / 4 + 1;
}

// =============================================================================
// Construction / destruction
// =============================================================================
Server::Server(const AppConfig& config)
    : m_config(config)
    , m_ollama(config.ollama.baseUrl)
    , m_cloud(config.cloud.baseUrl,
              config.cloud.apiKey,
              config.cloud.model)
    , m_cost(config.server.logPath)
    , m_router(config, m_ollama, m_cloud)
{
    // ── Register HTTP handlers ──
    m_http.Post("/v1/chat/completions",
        [this](const httplib::Request& req, httplib::Response& res) {
            handleChatCompletion(req, res);
        });

    m_http.Get("/v1/models",
        [this](const httplib::Request& req, httplib::Response& res) {
            handleModels(req, res);
        });

    m_http.Get("/health",
        [this](const httplib::Request& req, httplib::Response& res) {
            handleHealth(req, res);
        });

    m_http.Get("/admin/summary",
        [this](const httplib::Request& req, httplib::Response& res) {
            handleAdminSummary(req, res);
        });

    m_http.Get("/admin/config",
        [this](const httplib::Request& req, httplib::Response& res) {
            handleAdminConfigGet(req, res);
        });

    m_http.Post("/admin/config",
        [this](const httplib::Request& req, httplib::Response& res) {
            handleAdminConfigPost(req, res);
        });

    m_http.Get("/admin/refresh-usage",
        [this](const httplib::Request& req, httplib::Response& res) {
            handleAdminRefreshUsage(req, res);
        });
}

Server::~Server() {
    stop();
}

// =============================================================================
// Start / stop
// =============================================================================
void Server::start() {
    if (m_running.exchange(true)) return;

    if (!m_http.bind_to_port("0.0.0.0", m_config.server.port)) {
        std::cerr << "[Server] Failed to bind to port " << m_config.server.port << "\n";
        m_running = false;
        return;
    }

    std::cout << "\n"
              << "  ╔══════════════════════════════════════════════════════╗\n"
              << "  ║              LLM Cost Router  v1.0                   ║\n"
              << "  ║──────────────────────────────────────────────────────║\n"
              << "  ║  Port:      " << std::left << std::setw(38) << m_config.server.port << "║\n"
              << "  ║  Default:   " << std::setw(38) << m_config.router.defaultBackend << "║\n"
              << "  ║  Monthly:  $" << std::fixed << std::setprecision(2)
              << std::setw(37) << m_config.router.monthlyBudgetUsd << "║\n"
              << "  ║  Log:       " << std::setw(38) << m_config.server.logPath << "║\n"
              << "  ╚══════════════════════════════════════════════════════╝\n"
              << "\n"
              << "  Listening on http://0.0.0.0:" << m_config.server.port << "\n"
              << "  Endpoints:\n"
              << "    POST /v1/chat/completions  OpenAI-compatible chat\n"
              << "    GET  /health               Health check\n"
              << "    GET  /admin/summary         Daily cost summary\n"
              << "    GET  /admin/config          Current configuration\n"
              << "    POST /admin/config          Update configuration\n"
              << "    GET  /admin/refresh-usage   Refresh usage data\n"
              << "\n";

    m_running = true;
    m_http.listen_after_bind();
}

void Server::stop() {
    if (!m_running.exchange(false)) return;
    m_http.stop();
}

// =============================================================================
// 429 helper
// =============================================================================
void Server::respondOverBudget(httplib::Response& res) {
    double monthly = m_config.router.monthlyBudgetUsd;
    auto days = CostTracker::daysInMonth();
    auto today = CostTracker::dayOfMonth();
    double daily = monthly / days;
    json body = {
        {"error", json{
            {"message", "Monthly budget ($" +
                        std::to_string(monthly) +
                        "/mo, ~$" + std::to_string(daily).substr(0, 5) +
                        "/day) pro-rata limit reached on day " +
                        std::to_string(today) + "/" + std::to_string(days) +
                        ". Increase cap via POST /admin/config."},
            {"type", "insufficient_quota"},
            {"code", 429}
        }}
    };
    res.status = 429;
    res.set_content(body.dump(2), "application/json");
}

// =============================================================================
// POST /v1/chat/completions  (main endpoint)
// =============================================================================
void Server::handleChatCompletion(const httplib::Request& req, httplib::Response& res) {
    // 1. Parse body ----------------------------------------------------------
    json body;
    try {
        body = json::parse(req.body);
    } catch (const std::exception& e) {
        json err = {{"error", {{"message", "Invalid JSON: " + std::string(e.what())},
                               {"type", "invalid_request_error"},
                               {"code", 400}}}};
        res.status = 400;
        res.set_content(err.dump(2), "application/json");
        return;
    }

    // Ensure messages array exists
    if (!body.contains("messages") || !body["messages"].is_array() ||
        body["messages"].empty()) {
        json err = {{"error", {{"message", "Missing required field: messages"},
                               {"type", "invalid_request_error"},
                               {"code", 400}}}};
        res.status = 400;
        res.set_content(err.dump(2), "application/json");
        return;
    }

    // 2. Budget check --------------------------------------------------------
    if (m_config.costTracking && m_cost.isOverMonthlyBudget(m_config.router.monthlyBudgetUsd)) {
        respondOverBudget(res);
        return;
    }

    // 3. Decide backend ------------------------------------------------------
    RouteDecision decision;
    try {
        decision = m_router.decide(body);
    } catch (const std::exception& e) {
        json err = {{"error", {{"message", "Routing decision failed: " + std::string(e.what())},
                               {"type", "routing_error"},
                               {"code", 500}}}};
        res.status = 500;
        res.set_content(err.dump(2), "application/json");
        return;
    }

    // 4. Streaming or not? ---------------------------------------------------
    bool streaming = body.value("stream", false);

    if (streaming) {
        handleStreamingChat(body, decision, res);
    } else {
        // =====================================================================
        //  Non-streaming path
        // =====================================================================

        // ── Retry-on-failure mode: try Ollama first, escalate on refusal ──
        int estTokens = estimateRequestTokens(body);
        if (m_config.router.retryOnFailure && decision.backend == "cloud"
            && estTokens <= m_config.router.retryMaxTokens) {
            std::clog << "[Retry] est=" << estTokens << " tok — trying Ollama first despite keyword escalation\n";
            RouteDecision ollamaDec;
            ollamaDec.backend  = "ollama";
            ollamaDec.model    = m_config.ollama.model;
            ollamaDec.provider = "ollama";
            ollamaDec.reason   = "retry_on_failure: try Ollama first";

            std::clog << "[Retry] " << decision.reason
                      << " — trying Ollama first anyway\n";

            // Set shorter timeout for the Ollama retry attempt
            m_ollama.setReadTimeout(m_config.router.retryTimeoutMs / 1000);

            auto ollamaResult = m_router.route(ollamaDec, body);

            // Restore normal timeout
            m_ollama.setReadTimeout(120);

            if (!ollamaResult.error &&
                !isRefusalResponse(ollamaResult.response,
                    m_config.router.retryRefusalPatterns)) {
                // Ollama succeeded! Log as ollama (free) and return
                if (m_config.costTracking) {
                    UsageEntry entry;
                    entry.timestamp    = std::chrono::system_clock::now();
                    entry.model        = ollamaResult.model;
                    entry.backend      = "ollama";
                    entry.provider     = ollamaResult.provider;
                    entry.inputTokens  = ollamaResult.inputTokens;
                    entry.outputTokens = ollamaResult.outputTokens;
                    entry.cacheTokens  = ollamaResult.cacheTokens;
                    entry.cost         = 0.0;
                    entry.streamed     = false;
                    entry.statusCode   = 200;
                    entry.reason       = ollamaDec.reason;
                    m_cost.logRequest(entry);
                }

                res.status = 200;
                res.set_content(ollamaResult.response.dump(2), "application/json");
                return;
            }

            // Ollama refused — fall back to cloud
            std::clog << "[Retry] Ollama refused — escalating to "
                      << m_config.cloud.model << "\n";
            decision.backend  = "cloud";
            decision.model    = m_config.cloud.model;
            decision.provider = m_config.cloud.provider;
            decision.reason   = "retry_on_failure: ollama refused, escalated to cloud";
            // Fall through to normal route+log below
        }

        // ── Normal route execution ──
        auto result = m_router.route(decision, body);

        if (result.error) {
            json err = {{"error", {{"message", result.errorMessage},
                                   {"type", "upstream_error"},
                                   {"code", result.statusCode}}}};
            res.status = result.statusCode > 0 ? result.statusCode : 502;
            res.set_content(err.dump(2), "application/json");
            return;
        }

        // 5. Log usage -------------------------------------------------------
        if (m_config.costTracking) {
            UsageEntry entry;
            entry.timestamp    = std::chrono::system_clock::now();
            entry.model        = result.model;
            entry.backend      = decision.backend;
            entry.provider     = result.provider;
            entry.inputTokens  = result.inputTokens;
            entry.outputTokens = result.outputTokens;
            entry.cacheTokens  = result.cacheTokens;
            entry.cost         = result.cost;
            entry.streamed     = false;
            entry.statusCode   = 200;
            entry.reason       = decision.reason;
            m_cost.logRequest(entry);
        }

        // 6. Send response ---------------------------------------------------
        res.status = 200;
        res.set_content(result.response.dump(2), "application/json");
    }
}

// =============================================================================
// GET /v1/models  — OpenAI-compatible model listing (needed by clients like Hermes)
// =============================================================================
void Server::handleModels(const httplib::Request& /*req*/, httplib::Response& res) {
    json models = json::array();
    bool ollamaOk = m_ollama.health();
    bool cloudOk  = m_cloud.health();

    if (ollamaOk) {
        models.push_back({
            {"id", m_config.ollama.model},
            {"object", "model"},
            {"owned_by", "ollama"}
        });
    }
    if (cloudOk) {
        std::string cloudModel = m_config.cloud.model;
        models.push_back({
            {"id", cloudModel},
            {"object", "model"},
            {"owned_by", "cloud"},
            {"provider", m_config.cloud.provider}
        });
        // Also expose the -cloud suffix variant for explicit routing
        models.push_back({
            {"id", cloudModel + "-cloud"},
            {"object", "model"},
            {"owned_by", "cloud"},
            {"provider", m_config.cloud.provider}
        });
    }

    json resp = {
        {"object", "list"},
        {"data", models}
    };
    res.set_content(resp.dump(2), "application/json");
}

// =============================================================================
// GET /health
// =============================================================================
void Server::handleHealth(const httplib::Request& /*req*/, httplib::Response& res) {
    bool ollamaOk = m_ollama.health();
    bool cloudOk  = m_cloud.health();

    json h = {
        {"status",    ollamaOk || cloudOk ? "ok" : "degraded"},
        {"version",   "1.0.0"},
        {"backends", json{
            {"ollama", ollamaOk},
            {"cloud",  cloudOk}
        }},
        {"cost_tracking", m_config.costTracking},
        {"daily_cost",    m_cost.todayCost()}
    };
    res.set_content(h.dump(2), "application/json");
}

// =============================================================================
// GET /admin/summary
// =============================================================================
void Server::handleAdminSummary(const httplib::Request& /*req*/, httplib::Response& res) {
    json summary;

    // Today's breakdown
    auto daily = m_cost.getDailySummary();
    summary["today"] = {
        {"date",            daily.date},
        {"requests",        daily.requestCount},
        {"input_tokens",    daily.totalInput},
        {"output_tokens",   daily.totalOutput},
        {"cache_tokens",    daily.totalCache},
        {"total_cost",      daily.totalCost},
        {"ollama_requests", daily.ollamaCount},
        {"cloud_requests",  daily.cloudCount},
        {"ollama_input_tokens",  daily.ollamaInput},
        {"ollama_output_tokens", daily.ollamaOutput},
        {"ollama_cache_tokens",  daily.ollamaCache},
        {"cloud_input_tokens",   daily.cloudInput},
        {"cloud_output_tokens",  daily.cloudOutput},
        {"cloud_cache_tokens",   daily.cloudCache}
    };

    // Rolling history (last 7 days)
    auto history = m_cost.getHistory(7);
    json histArr = json::array();
    for (const auto& h : history) {
        histArr.push_back({
            {"date",        h.date},
            {"requests",    h.requestCount},
            {"total_cost",  h.totalCost}
        });
    }
    summary["history"] = histArr;

    // Budget info (monthly, prorated by day)
    double monthly         = m_config.router.monthlyBudgetUsd;
    int today              = CostTracker::dayOfMonth();
    int totalDays          = CostTracker::daysInMonth();
    double allowancePerDay = monthly / totalDays;
    double allowanceSoFar  = today * allowancePerDay;
    double monthToDate     = m_cost.monthToDateCost();
    double remaining       = allowanceSoFar - monthToDate;
    if (remaining < 0.0) remaining = 0.0;
    summary["budget"] = {
        {"monthly_budget",     monthly},
        {"daily_allowance",    allowancePerDay},
        {"month_to_date_cost", monthToDate},
        {"day_of_month",       today},
        {"days_in_month",      totalDays},
        {"remaining",          remaining},
        {"over_budget",        m_cost.isOverMonthlyBudget(monthly)}
    };

    res.set_content(summary.dump(2), "application/json");
}

// =============================================================================
// GET /admin/config
// =============================================================================
void Server::handleAdminConfigGet(const httplib::Request& /*req*/,
                                   httplib::Response& res) {
    json cfg = m_config;
    res.set_content(cfg.dump(2), "application/json");
}

// =============================================================================
// POST /admin/config
// =============================================================================
void Server::handleAdminConfigPost(const httplib::Request& req,
                                    httplib::Response& res) {
    json body;
    try {
        body = json::parse(req.body);
    } catch (const std::exception& e) {
        json err = {{"error", {{"message", "Invalid JSON: " + std::string(e.what())},
                               {"type", "invalid_request_error"},
                               {"code", 400}}}};
        res.status = 400;
        res.set_content(err.dump(2), "application/json");
        return;
    }

    try {
        // Merge: only update the sections provided in the request body
        AppConfig newCfg = m_config;

        if (body.contains("router")) newCfg.router = body["router"];
        if (body.contains("ollama")) newCfg.ollama = body["ollama"];
        if (body.contains("cloud"))  newCfg.cloud  = body["cloud"];
        if (body.contains("server")) newCfg.server = body["server"];
        if (body.contains("cost_tracking")) {
            newCfg.costTracking = body["cost_tracking"];
        }

        m_config = newCfg;

        // Persist to disk
        Config::save(Config::defaultConfigPath(), m_config);

        json resp = {
            {"status",  "ok"},
            {"message", "Configuration updated"},
            {"config",  m_config}
        };
        res.set_content(resp.dump(2), "application/json");

    } catch (const std::exception& e) {
        json err = {{"error", {{"message", "Failed to apply config update: " +
                                            std::string(e.what())}}}};
        res.status = 400;
        res.set_content(err.dump(2), "application/json");
    }
}

// =============================================================================
// GET /admin/refresh-usage
// =============================================================================
void Server::handleAdminRefreshUsage(const httplib::Request& /*req*/,
                                       httplib::Response& res) {
    try {
        m_cost.reload();
        json resp = {
            {"status",  "ok"},
            {"message", "Usage data refreshed"}
        };
        res.set_content(resp.dump(2), "application/json");
    } catch (const std::exception& e) {
        json err = {{"error", {{"message", "Failed to refresh usage data: " +
                                            std::string(e.what())}}}};
        res.status = 500;
        res.set_content(err.dump(2), "application/json");
    }
}


// =============================================================================
// Streaming chat  (POST /v1/chat/completions with stream=true)
// =============================================================================
void Server::handleStreamingChat(const json& body,
                                  const RouteDecision& decision,
                                  httplib::Response& res)
{
    auto state = std::make_shared<StreamState>();

    // Generate a unique request id for SSE framing
    auto now  = std::chrono::system_clock::now();
    auto us   = std::chrono::duration_cast<std::chrono::microseconds>(
                    now.time_since_epoch()).count();
    auto id   = "chatcmpl-" + std::to_string(us);

    // ── Retry-on-failure mode: collect Ollama stream, retry on refusal ──
    int est = estimateRequestTokens(body);
    if (m_config.router.retryOnFailure && decision.backend == "cloud"
        && est <= m_config.router.retryMaxTokens) {
        std::clog << "[Retry] (stream) est=" << est << " tok — trying Ollama first despite keyword escalation\n";
        std::string buffer;
        int chunkCount = 0;

        RouteDecision ollamaDec;
        ollamaDec.backend  = "ollama";
        ollamaDec.model    = m_config.ollama.model;
        ollamaDec.provider = "ollama";
        ollamaDec.reason   = "retry_on_failure: try Ollama stream first";

        std::clog << "[Retry] (stream) est=" << est << " tok — trying Ollama first\n";

        // Set shorter timeout for Ollama retry
        m_ollama.setReadTimeout(m_config.router.retryTimeoutMs / 1000);

        RouteResult ollamaResult = m_router.routeStream(ollamaDec, body,
            [&buffer, &chunkCount](const std::string& chunk) {
                buffer += chunk;
                chunkCount++;
            });

        // Restore normal timeout
        m_ollama.setReadTimeout(120);

        // Check if Ollama's response was a refusal
        if (!ollamaResult.error && chunkCount > 0 &&
            !isRefusalResponse(ollamaResult.response,
                m_config.router.retryRefusalPatterns)) {
            // Ollama succeeded — serve the buffered chunks as SSE
            std::clog << "[Retry] (stream) Ollama succeeded — "
                      << chunkCount << " chunks, $0\n";

            json finalResp = ollamaResult.response;
            auto model      = ollamaResult.model;

            // Log usage
            if (m_config.costTracking) {
                UsageEntry entry;
                entry.timestamp    = std::chrono::system_clock::now();
                entry.model        = model;
                entry.backend      = "ollama";
                entry.provider     = ollamaResult.provider;
                entry.inputTokens  = ollamaResult.inputTokens;
                entry.outputTokens = ollamaResult.outputTokens;
                entry.cacheTokens  = ollamaResult.cacheTokens;
                entry.cost         = 0.0;
                entry.streamed     = true;
                entry.statusCode   = 200;
                entry.reason       = ollamaDec.reason;
                m_cost.logRequest(entry);
            }

            // Append final [DONE] marker
            buffer += "data: [DONE]\n\n";

            res.status = 200;
            res.set_content(buffer, "text/event-stream");
            return;
        }

        // Ollama refused — fall back to cloud non-streaming
        std::clog << "[Retry] (stream) Ollama refused — escalating to "
                  << m_config.cloud.model << "\n";

        json cloudBody = body;
        cloudBody["stream"] = false;

        RouteDecision cloudDec;
        cloudDec.backend  = "cloud";
        cloudDec.model    = m_config.cloud.model;
        cloudDec.provider = m_config.cloud.provider;
        cloudDec.reason   = "retry_on_failure: ollama stream refused, retry cloud";

        auto cloudResult = m_router.route(cloudDec, body);

        if (cloudResult.error) {
            json err = {{"error", {{"message", cloudResult.errorMessage},
                                   {"type", "upstream_error"},
                                   {"code", cloudResult.statusCode}}}};
            res.status = cloudResult.statusCode > 0 ? cloudResult.statusCode : 502;
            res.set_content(err.dump(2), "application/json");
            return;
        }

        // Log cloud usage
        if (m_config.costTracking) {
            UsageEntry entry;
            entry.timestamp    = std::chrono::system_clock::now();
            entry.model        = cloudResult.model;
            entry.backend      = "cloud";
            entry.provider     = cloudResult.provider;
            entry.inputTokens  = cloudResult.inputTokens;
            entry.outputTokens = cloudResult.outputTokens;
            entry.cacheTokens  = cloudResult.cacheTokens;
            entry.cost         = cloudResult.cost;
            entry.streamed     = true;
            entry.statusCode   = 200;
            entry.reason       = cloudDec.reason;
            m_cost.logRequest(entry);
        }

        // Serve the cloud response as a single SSE chunk + [DONE]
        std::string model = cloudResult.model;
        auto ts = std::chrono::duration_cast<std::chrono::microseconds>(
                      std::chrono::system_clock::now().time_since_epoch()).count();
        std::string sseId = "chatcmpl-" + std::to_string(ts);

        std::string sseBuffer;
        // Extract content from cloud response
        std::string content = extractContent(cloudResult.response);

        // Send as one SSE chunk (the whole response)
        json chunk = {
            {"choices", json::array({json{
                {"delta", json{{"content", content}, {"role", "assistant"}}},
                {"finish_reason", nullptr},
                {"index", 0}
            }})},
            {"created", ts / 1000000},
            {"id", sseId},
            {"model", model},
            {"object", "chat.completion.chunk"}
        };
        sseBuffer += "data: " + chunk.dump() + "\n\n";

        // Send final empty chunk with finish_reason=stop
        json finalChunk = {
            {"choices", json::array({json{
                {"delta", json::object()},
                {"finish_reason", "stop"},
                {"index", 0}
            }})},
            {"created", ts / 1000000},
            {"id", sseId},
            {"model", model},
            {"object", "chat.completion.chunk"}
        };
        sseBuffer += "data: " + finalChunk.dump() + "\n\n";
        sseBuffer += "data: [DONE]\n\n";

        res.status = 200;
        res.set_content(sseBuffer, "text/event-stream");
        return;
    }

    // ── Normal streaming path (no retry, or Ollama already chosen) ──
    auto model = decision.model;

    // Background thread: drive the upstream streaming client
    auto streamThread = std::make_shared<std::thread>(
        [this, state, body, decision]() {
            RouteResult result = m_router.routeStream(decision, body,
                [state](const std::string& chunk) {
                    std::lock_guard<std::mutex> lock(state->mutex);
                    state->chunks.push(chunk);
                    state->cv.notify_one();
                });

            // Log usage after stream completes
            if (m_config.costTracking && !result.error) {
                UsageEntry entry;
                entry.timestamp    = std::chrono::system_clock::now();
                entry.model        = result.model;
                entry.backend      = decision.backend;
                entry.provider     = result.provider;
                entry.inputTokens  = result.inputTokens;
                entry.outputTokens = result.outputTokens;
                entry.cacheTokens  = result.cacheTokens;
                entry.cost         = result.cost;
                entry.streamed     = true;
                entry.statusCode   = 200;
                entry.reason       = decision.reason;
                m_cost.logRequest(entry);
            }

            {
                std::lock_guard<std::mutex> lock(state->mutex);
                state->result = result;
                state->done   = true;
            }
            state->cv.notify_one();
        });

    // Content provider: bridge push -> pull for httplib
    res.set_chunked_content_provider(
        "text/event-stream",
        [state, id, model](size_t /*offset*/,
                           httplib::DataSink& sink) -> bool
        {
            std::unique_lock<std::mutex> lock(state->mutex);

            // If stream already finished with no pending chunks, stop
            if (state->done && state->chunks.empty()) {
                return false;
            }

            // Block until data arrives or stream finishes
            state->cv.wait(lock, [state]() {
                return !state->chunks.empty() || state->done;
            });

            // Drain all available chunks into the sink
            while (!state->chunks.empty()) {
                sink.os << state->chunks.front();
                state->chunks.pop();
            }

            if (state->done) {
                sink.done();
                return false;
            }

            return true;
        },
        [streamThread](bool /*success*/) {
            if (streamThread->joinable()) {
                streamThread->join();
            }
        });
}