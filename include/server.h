#ifndef LLM_COST_ROUTER_SERVER_H
#define LLM_COST_ROUTER_SERVER_H

#include <string>
#include <memory>
#include <thread>
#include <atomic>
#include <httplib.h>
#include "config.h"
#include "router.h"
#include "cost.h"
#include "ollama.h"
#include "cloud.h"

class Server {
public:
    explicit Server(const AppConfig& config);
    ~Server();

    void start();
    void stop();

private:
    AppConfig     m_config;
    OllamaClient  m_ollama;
    CloudClient   m_cloud;
    CostTracker   m_cost;
    Router        m_router;
    httplib::Server m_http;
    std::atomic<bool> m_running{false};

    // ── Route helpers for the 429 response ──
    void respondOverBudget(httplib::Response& res);

    // ── Handlers ──
    void handleChatCompletion(const httplib::Request& req, httplib::Response& res);
    void handleModels(const httplib::Request& req, httplib::Response& res);
    void handleHealth(const httplib::Request& req, httplib::Response& res);
    void handleAdminSummary(const httplib::Request& req, httplib::Response& res);
    void handleAdminConfigGet(const httplib::Request& req, httplib::Response& res);
    void handleAdminConfigPost(const httplib::Request& req, httplib::Response& res);
    void handleAdminRefreshUsage(const httplib::Request& req, httplib::Response& res);

    // ── Streaming helpers ──
    void handleStreamingChat(const nlohmann::json& body,
                             const RouteDecision& decision,
                             httplib::Response& res);
};

#endif // LLM_COST_ROUTER_SERVER_H
