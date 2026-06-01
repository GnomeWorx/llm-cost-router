#include "cost.h"
#include <fstream>
#include <sstream>
#include <iomanip>
#include <iostream>
#include <algorithm>

CostTracker::CostTracker(const std::string& logPath)
    : m_logPath(logPath) {
    ensureLogDir();
    reload();
}

void CostTracker::ensureLogDir() {
    auto slash = m_logPath.rfind('/');
    if (slash != std::string::npos) {
        std::string dir = m_logPath.substr(0, slash);
        std::string cmd = "mkdir -p " + dir;
        system(cmd.c_str());
    }
}

std::string CostTracker::todayStr() const {
    auto now = std::chrono::system_clock::now();
    auto t = std::chrono::system_clock::to_time_t(now);
    std::tm tm;
    localtime_r(&t, &tm);
    std::ostringstream os;
    os << std::put_time(&tm, "%Y-%m-%d");
    return os.str();
}

UsageEntry CostTracker::parseLine(const std::string& line) const {
    UsageEntry e;
    try {
        auto j = nlohmann::json::parse(line);
        e.backend      = j.value("backend", "");
        e.provider     = j.value("provider", "");
        e.model        = j.value("model", "");
        e.inputTokens  = j.value("input_tokens", 0LL);
        e.outputTokens = j.value("output_tokens", 0LL);
        e.cacheTokens  = j.value("cache_tokens", 0LL);
        e.cost         = j.value("cost", 0.0);
        e.durationMs   = j.value("duration_ms", 0LL);
        e.streamed     = j.value("streamed", false);
        e.statusCode   = j.value("status", 200);
    } catch (...) {}
    return e;
}

void CostTracker::logRequest(const UsageEntry& entry) {
    std::ofstream f(m_logPath, std::ios::app);
    if (!f.is_open()) return;

    auto t = std::chrono::system_clock::to_time_t(entry.timestamp);
    std::tm tm;
    localtime_r(&t, &tm);
    std::ostringstream ts;
    ts << std::put_time(&tm, "%Y-%m-%dT%H:%M:%S");

    nlohmann::json j;
    j["timestamp"]     = ts.str();
    j["model"]         = entry.model;
    j["backend"]       = entry.backend;
    j["provider"]      = entry.provider;
    j["input_tokens"]  = entry.inputTokens;
    j["output_tokens"] = entry.outputTokens;
    j["cache_tokens"]  = entry.cacheTokens;
    j["cost"]          = entry.cost;
    j["duration_ms"]   = entry.durationMs;
    j["streamed"]      = entry.streamed;
    j["status"]        = entry.statusCode;

    f << j.dump() << "\n";

    // Also keep in memory
    m_entries.push_back(entry);
    if (m_entries.size() > 100000) {
        m_entries.erase(m_entries.begin(), m_entries.begin() + 50000);
    }
}

DailySummary CostTracker::getDailySummary() {
    DailySummary s;
    s.date = todayStr();

    for (const auto& e : m_entries) {
        auto t = std::chrono::system_clock::to_time_t(e.timestamp);
        std::tm tm;
        localtime_r(&t, &tm);
        std::ostringstream ds;
        ds << std::put_time(&tm, "%Y-%m-%d");
        if (ds.str() != s.date) continue;

        s.requestCount++;
        s.totalInput  += e.inputTokens;
        s.totalOutput += e.outputTokens;
        s.totalCache  += e.cacheTokens;
        s.totalCost   += e.cost;
        if (e.backend == "ollama") s.ollamaCount++;
        else s.cloudCount++;
    }

    return s;
}

std::vector<DailySummary> CostTracker::getHistory(int days) {
    std::map<std::string, DailySummary> dayMap;

    for (const auto& e : m_entries) {
        auto t = std::chrono::system_clock::to_time_t(e.timestamp);
        std::tm tm;
        localtime_r(&t, &tm);
        std::ostringstream ds;
        ds << std::put_time(&tm, "%Y-%m-%d");
        auto& s = dayMap[ds.str()];
        s.date = ds.str();
        s.requestCount++;
        s.totalInput  += e.inputTokens;
        s.totalOutput += e.outputTokens;
        s.totalCache  += e.cacheTokens;
        s.totalCost   += e.cost;
        if (e.backend == "ollama") s.ollamaCount++;
        else s.cloudCount++;
    }

    std::vector<DailySummary> result;
    for (auto& [k, v] : dayMap) {
        result.push_back(v);
    }
    std::sort(result.begin(), result.end(),
        [](const DailySummary& a, const DailySummary& b) {
            return a.date > b.date;
        });

    if ((int)result.size() > days) result.resize(days);
    return result;
}

double CostTracker::todayCost() {
    return getDailySummary().totalCost;
}

bool CostTracker::isOverBudget(double budget) {
    return todayCost() >= budget;
}

void CostTracker::reload() {
    m_entries.clear();
    std::ifstream f(m_logPath);
    if (!f.is_open()) return;

    std::string line;
    while (std::getline(f, line)) {
        if (line.empty()) continue;
        m_entries.push_back(parseLine(line));
    }

    // Keep last 100K
    if (m_entries.size() > 100000) {
        m_entries.erase(m_entries.begin(), m_entries.begin() + (m_entries.size() - 100000));
    }
}
