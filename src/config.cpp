#include "config.h"
#include <fstream>
#include <iostream>
#include <regex>
#include <sys/stat.h>

// Expand ${VAR_NAME} or $VAR_NAME environment variable references in a string
static std::string expandEnvVars(const std::string& s) {
    static const std::regex re(R"(\$\{([^}]+)\}|\$([A-Za-z_][A-Za-z0-9_]*))");
    std::string result;
    auto pos = s.cbegin();
    auto end = s.cend();
    std::smatch m;
    while (std::regex_search(pos, end, m, re)) {
        result.append(pos, m[0].first);
        const char* env = nullptr;
        if (m[1].matched) {
            env = getenv(m[1].str().c_str());
        } else if (m[2].matched) {
            env = getenv(m[2].str().c_str());
        }
        result.append(env ? env : "");
        pos = m[0].second;
    }
    result.append(pos, end);
    return result;
}

AppConfig Config::load(const std::string& path) {
    std::ifstream f(path);
    if (!f.is_open()) {
        std::cerr << "Config not found at " << path << ", creating default config.\n";
        auto cfg = defaultConfig();
        save(path, cfg);
        return cfg;
    }
    try {
        nlohmann::json j;
        f >> j;

        // Expand env vars in the raw JSON string, then re-parse
        std::string raw = j.dump();
        std::string expanded = expandEnvVars(raw);
        auto expandedJ = nlohmann::json::parse(expanded);

        return expandedJ.get<AppConfig>();
    } catch (const std::exception& e) {
        std::cerr << "Failed to parse config: " << e.what() << "\n";
        std::cerr << "Using default config.\n";
        return defaultConfig();
    }
}

void Config::save(const std::string& path, const AppConfig& cfg) {
    // Ensure directory exists
    auto slash = path.rfind('/');
    if (slash != std::string::npos) {
        std::string dir = path.substr(0, slash);
        std::string cmd = "mkdir -p " + dir;
        system(cmd.c_str());
    }

    std::ofstream f(path);
    if (!f.is_open()) {
        std::cerr << "Cannot write config to " << path << "\n";
        return;
    }
    nlohmann::json j = cfg;
    f << j.dump(2) << "\n";
}

AppConfig Config::defaultConfig() {
    return AppConfig{};
}

std::string Config::defaultConfigPath() {
    const char* home = getenv("HOME");
    if (!home) home = "/tmp";
    return std::string(home) + "/.config/llm-cost-router/config.json";
}
