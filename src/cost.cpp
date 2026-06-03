#include "cost.h"
#include <fstream>
#include <sstream>
#include <iomanip>
#include <iostream>
#include <algorithm>
#include <ctime>

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

std::string CostTracker::getAdjustedDateString(std::chrono::system_clock::time_point timestamp) const {
    auto t = std::chrono::system_clock::to_time_t(timestamp);
    std::tm tm;
    localtime_r(&t, &tm);

    // If between 00:00:00 and 00:00:59 (inclusive), it belongs to the previous day
    if (tm.tm_hour == 0 && tm.tm_min == 0) {
        // Subtract one minute to get to the previous day correctly
        auto adjustedTimestamp = timestamp - std::chrono::minutes(1);
        auto adjustedT = std::chrono::system_clock::to_time_t(adjustedTimestamp);
        std::tm adjustedTm;
        localtime_r(&adjustedT, &adjustedTm);
        std::ostringstream os;
        os << std::put_time(&adjustedTm, "%Y-%m-%d");
        return os.str();
    } else {
        std::ostringstream os;
        os << std::put_time(&tm, "%Y-%m-%d");
        return os.str();
    }
}

UsageEntry CostTracker::parseLine(const std::string& line) const {
    UsageEntry e;
    try {
        auto j = nlohmann::json::parse(line);

        // Parse timestamp from ISO format "2026-06-03T12:50:27"
        std::string ts = j.value("timestamp", "");
        if (!ts.empty()) {
            std::tm tm = {};
            std::istringstream ss(ts);
            ss >> std::get_time(&tm, "%Y-%m-%dT%H:%M:%S");
            if (!ss.fail()) {
                auto tt = std::mktime(&tm);
                e.timestamp = std::chrono::system_clock::from_time_t(tt);
            }
        }

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
        e.reason       = j.value("reason", "");
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
    j["reason"]        = entry.reason;
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
    s.date = todayStr(); // This is the calendar date, not adjusted

    // Get the adjusted date string for 'now'
    std::string adjustedTodayStr = getAdjustedDateString(std::chrono::system_clock::now());

    for (const auto& e : m_entries) {
        // Compare with the adjusted date string
        if (getAdjustedDateString(e.timestamp) != adjustedTodayStr) continue;

        s.requestCount++;
        s.totalInput  += e.inputTokens;
        s.totalOutput += e.outputTokens;
        s.totalCache  += e.cacheTokens;
        s.totalCost   += e.cost;
        if (e.backend == "ollama") {
            s.ollamaCount++;
            s.ollamaInput  += e.inputTokens;
            s.ollamaOutput += e.outputTokens;
            s.ollamaCache  += e.cacheTokens;
        } else {
            s.cloudCount++;
            s.cloudInput  += e.inputTokens;
            s.cloudOutput += e.outputTokens;
            s.cloudCache  += e.cacheTokens;
        }
    }

    return s;
}

std::vector<DailySummary> CostTracker::getHistory(int days) {
    std::map<std::string, DailySummary> dayMap;

    for (const auto& e : m_entries) {
        std::string adjustedDate = getAdjustedDateString(e.timestamp);
        auto& s = dayMap[adjustedDate];
        s.date = adjustedDate; // Store the adjusted date as the summary date
        s.requestCount++;
        s.totalInput  += e.inputTokens;
        s.totalOutput += e.outputTokens;
        s.totalCache  += e.cacheTokens;
        s.totalCost   += e.cost;
        if (e.backend == "ollama") {
            s.ollamaCount++;
            s.ollamaInput  += e.inputTokens;
            s.ollamaOutput += e.outputTokens;
            s.ollamaCache  += e.cacheTokens;
        } else {
            s.cloudCount++;
            s.cloudInput  += e.inputTokens;
            s.cloudOutput += e.outputTokens;
            s.cloudCache  += e.cacheTokens;
        }
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

// ── Monthly budget helpers ──────────────────────────────────────

int CostTracker::dayOfMonth() {
    auto now = std::chrono::system_clock::now();
    auto t = std::chrono::system_clock::to_time_t(now);
    std::tm tm;
    localtime_r(&t, &tm);
    return tm.tm_mday;
}

int CostTracker::daysInMonth() {
    auto now = std::chrono::system_clock::now();
    auto t = std::chrono::system_clock::to_time_t(now);
    std::tm tm;
    localtime_r(&t, &tm);
    // Advance to next month's day 0 = last day of current month
    std::tm next = tm;
    next.tm_mon++;
    next.tm_mday = 0;
    std::mktime(&next);
    return next.tm_mday;
}

double CostTracker::monthToDateCost() {
    double total = 0.0;
    int today = dayOfMonth();
    int currentMonth = []() {
        auto now = std::chrono::system_clock::now();
        auto t = std::chrono::system_clock::to_time_t(now);
        std::tm tm;
        localtime_r(&t, &tm);
        return tm.tm_mon;
    }();

    for (const auto& e : m_entries) {
        auto t = std::chrono::system_clock::to_time_t(e.timestamp);
        std::tm tm;
        localtime_r(&t, &tm);
        if (tm.tm_mon == currentMonth && tm.tm_mday <= today) {
            total += e.cost;
        }
    }
    return total;
}

// Replace old daily-over-budget with monthly-or-daily check
// Monthly: total cost since day 1 must not exceed (elapsed_days / total_days) * monthly_budget
bool CostTracker::isOverMonthlyBudget(double monthlyBudget) {
    int today = dayOfMonth();
    int totalDays = daysInMonth();
    if (totalDays == 0) return false;

    double dailyAllowance = monthlyBudget / totalDays;
    double allowableSoFar = today * dailyAllowance;
    return monthToDateCost() >= allowableSoFar;
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
