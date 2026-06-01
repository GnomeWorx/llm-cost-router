#ifndef LLM_COST_ROUTER_COST_H
#define LLM_COST_ROUTER_COST_H

#include <string>
#include <vector>
#include <chrono>
#include <nlohmann/json.hpp>

struct UsageEntry {
    std::chrono::system_clock::time_point timestamp;
    std::string model;
    std::string backend;        // "ollama" or "cloud"
    std::string provider;       // "deepseek", "ollama", etc.
    int64_t     inputTokens  = 0;
    int64_t     outputTokens = 0;
    int64_t     cacheTokens  = 0;
    double      cost         = 0.0;
    int64_t     durationMs   = 0;
    bool        streamed     = false;
    int         statusCode   = 200;
};

struct DailySummary {
    std::string date;
    int         requestCount    = 0;
    int64_t     totalInput      = 0;
    int64_t     totalOutput     = 0;
    int64_t     totalCache      = 0;
    double      totalCost       = 0.0;
    int         ollamaCount     = 0;
    int         cloudCount      = 0;
};

class CostTracker {
public:
    explicit CostTracker(const std::string& logPath);

    // Log a completed request
    void logRequest(const UsageEntry& entry);

    // Get today's summary
    DailySummary getDailySummary();

    // Get rolling history (last N days)
    std::vector<DailySummary> getHistory(int days = 7);

    // Get today's running cost total
    double todayCost();

    // Check if daily budget is exceeded
    bool isOverBudget(double budget);

    // Reload log file
    void reload();

private:
    std::string m_logPath;
    std::vector<UsageEntry> m_entries;

    void ensureLogDir();
    std::string todayStr() const;
    UsageEntry parseLine(const std::string& line) const;
};

// Cost rates per 1K tokens (USD)
constexpr double COST_DEEPSEEK_INPUT  = 0.00027;   // $0.27/M tok
constexpr double COST_DEEPSEEK_OUTPUT = 0.00110;   // $1.10/M tok
constexpr double COST_DEEPSEEK_CACHE  = 0.00007;   // $0.07/M tok cached
constexpr double COST_OLLAMA          = 0.0;       // Free

#endif // LLM_COST_ROUTER_COST_H
