#include <iostream>
#include <csignal>
#include <cstdlib>
#include <memory>

#include "config.h"
#include "server.h"

// Global pointer so the signal handler can reach the server
static std::unique_ptr<Server> g_server;

// =============================================================================
// Signal handler — graceful shutdown on SIGINT / SIGTERM
// =============================================================================
static void signalHandler(int sig) {
    static bool once = false;
    if (once) {
        std::cerr << "\n[main] Forced exit.\n";
        std::_Exit(1);
    }
    once = true;

    std::cout << "\n[main] Caught signal " << sig
              << ", shutting down...\n";
    if (g_server) {
        g_server->stop();
    }
}

// =============================================================================
// Usage
// =============================================================================
static void usage(const char* prog) {
    std::cerr << "Usage: " << prog << " [options]\n"
              << "Options:\n"
              << "  -c, --config PATH   Config file path\n"
              << "                      (default: ~/.config/llm-cost-router/config.json)\n"
              << "  -h, --help          Show this help\n";
}

// =============================================================================
// Main
// =============================================================================
int main(int argc, char* argv[]) {
    // ── Parse CLI args ────────────────────────────────────────────────────
    std::string configPath;
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "-c" || arg == "--config") {
            if (i + 1 < argc) {
                configPath = argv[++i];
            } else {
                std::cerr << "[main] --config requires a path argument.\n";
                return 1;
            }
        } else if (arg == "-h" || arg == "--help") {
            usage(argv[0]);
            return 0;
        } else {
            std::cerr << "[main] Unknown option: " << arg << "\n";
            usage(argv[0]);
            return 1;
        }
    }

    // ── Resolve config path ───────────────────────────────────────────────
    if (configPath.empty()) {
        configPath = Config::defaultConfigPath();
    }

    // ── Install signal handlers ───────────────────────────────────────────
    std::signal(SIGINT,  signalHandler);
    std::signal(SIGTERM, signalHandler);
    std::signal(SIGPIPE, SIG_IGN);   // ignore broken pipe

    // ── Load configuration ────────────────────────────────────────────────
    std::cout << "[main] Loading config from " << configPath << "\n";
    AppConfig config;
    try {
        config = Config::load(configPath);
    } catch (const std::exception& e) {
        std::cerr << "[main] Fatal: failed to load config: " << e.what() << "\n";
        return 1;
    }

    // Validate critical config
    if (config.cloud.apiKey.empty() && config.router.defaultBackend == "cloud") {
        std::cerr << "[main] Warning: default backend is 'cloud' but no API key "
                  << "is configured.\n";
        std::cerr << "         Requests routed to cloud will fail.\n";
    }

    // ── Start server ──────────────────────────────────────────────────────
    try {
        g_server = std::make_unique<Server>(config);
        g_server->start();
    } catch (const std::exception& e) {
        std::cerr << "[main] Fatal: " << e.what() << "\n";
        return 1;
    }

    // ── Cleanup ───────────────────────────────────────────────────────────
    g_server.reset();
    std::cout << "[main] Goodbye.\n";
    return 0;
}
