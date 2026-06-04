#include <iostream>
#include <csignal>
#include <cstdlib>
#include <memory>
#include <string>
#include <sys/file.h>   // flock()
#include <sys/stat.h>   // mkdir()
#include <unistd.h>     // getpid()
#include <fcntl.h>      // open() flags
#include <cstring>      // strerror(), strdup()

#include "config.h"
#include "server.h"

// Global pointer so the signal handler can reach the server
static std::unique_ptr<Server> g_server;

// =============================================================================
// PID lock file — prevents duplicate instances sharing the same port
// =============================================================================
static int g_pidLockFd = -1;
static const char* g_pidLockPath = nullptr;

/// Attempts to acquire an exclusive lock on a PID file.
/// Returns true if this is the first/only instance.
/// On failure, prints the existing PID and returns false.
static bool acquirePidLock(const std::string& lockPath) {
    // Ensure parent directory exists
    auto dir = lockPath.substr(0, lockPath.rfind('/'));
    if (dir.size() > 0) {
        ::mkdir(dir.c_str(), 0755);
    }

    int fd = ::open(lockPath.c_str(), O_RDWR | O_CREAT, 0644);
    if (fd < 0) {
        std::cerr << "[pidlock] FATAL: cannot create " << lockPath << ": "
                  << strerror(errno) << std::endl;
        return false;
    }

    if (::flock(fd, LOCK_EX | LOCK_NB) != 0) {
        // Another instance holds the lock — read its PID for the error message
        std::string existingPid = "(unknown)";
        char buf[32] = {0};
        ssize_t n = ::read(fd, buf, sizeof(buf) - 1);
        if (n > 0) existingPid = std::string(buf, n);
        std::cerr << "[pidlock] FATAL: another instance (PID " << existingPid
                  << ") already holds " << lockPath << std::endl;
        ::close(fd);
        return false;
    }

    // Truncate and write our PID
    ::ftruncate(fd, 0);
    ::lseek(fd, 0, SEEK_SET);
    std::string pidStr = std::to_string(::getpid()) + "\n";
    ::write(fd, pidStr.data(), pidStr.size());

    // Store for cleanup
    g_pidLockFd = fd;
    g_pidLockPath = strdup(lockPath.c_str());

    std::cout << "[pidlock] acquired lock on " << lockPath
              << " (PID " << ::getpid() << ")" << std::endl;
    return true;
}

/// Releases the PID lock and removes the file.
static void releasePidLock() {
    if (g_pidLockFd >= 0) {
        ::close(g_pidLockFd);
        g_pidLockFd = -1;
    }
    if (g_pidLockPath) {
        if (::unlink(g_pidLockPath) != 0 && errno != ENOENT) {
            std::cerr << "[pidlock] warning: could not remove " << g_pidLockPath << ": "
                      << strerror(errno) << std::endl;
        }
        std::free(const_cast<char*>(g_pidLockPath));
        g_pidLockPath = nullptr;
    }
}

// =============================================================================
// Signal handler — graceful shutdown on SIGINT / SIGTERM
// =============================================================================
static void signalHandler(int sig) {
    static bool once = false;
    if (once) {
        std::cerr << std::endl << "[main] Forced exit." << std::endl;
        std::_Exit(0);
    }
    once = true;

    std::cout << std::endl << "[main] Caught signal " << sig
              << ", shutting down..." << std::endl;
    if (g_server) {
        g_server->stop();
    }
    releasePidLock();
}

// =============================================================================
// Usage
// =============================================================================
static void usage(const char* prog) {
    std::cerr << "Usage: " << prog << " [options]" << std::endl
              << "Options:" << std::endl
              << "  -c, --config PATH   Config file path" << std::endl
              << "                      (default: ~/.config/llm-cost-router/config.json)" << std::endl
              << "  -h, --help          Show this help" << std::endl;
}

// =============================================================================
// Main
// =============================================================================
int main(int argc, char* argv[]) {
    // ---- Parse CLI args ----------------------------------------------------
    std::string configPath;
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "-c" || arg == "--config") {
            if (i + 1 < argc) {
                configPath = argv[++i];
            } else {
                std::cerr << "[main] --config requires a path argument." << std::endl;
                return 1;
            }
        } else if (arg == "-h" || arg == "--help") {
            usage(argv[0]);
            return 0;
        } else {
            std::cerr << "[main] Unknown option: " << arg << std::endl;
            usage(argv[0]);
            return 1;
        }
    }

    // ---- Resolve config path ------------------------------------------------
    if (configPath.empty()) {
        configPath = Config::defaultConfigPath();
    }

    // ---- Install signal handlers --------------------------------------------
    std::signal(SIGINT,  signalHandler);
    std::signal(SIGTERM, signalHandler);
    std::signal(SIGPIPE, SIG_IGN);   // ignore broken pipe

    // ---- Load configuration -------------------------------------------------
    std::cout << "[main] Loading config from " << configPath << std::endl;
    AppConfig config;
    try {
        config = Config::load(configPath);
    } catch (const std::exception& e) {
        std::cerr << "[main] Fatal: failed to load config: " << e.what() << std::endl;
        return 1;
    }

    // Validate critical config
    if (config.cloud.apiKey.empty() && config.router.defaultBackend == "cloud") {
        std::cerr << "[main] Warning: default backend is 'cloud' but no API key "
                  << "is configured." << std::endl;
        std::cerr << "         Requests routed to cloud will fail." << std::endl;
    }

    // ---- PID lock - prevent duplicate instances -----------------------------
    {
        std::string lockPath = Config::defaultConfigPath() + ".pid";
        if (!acquirePidLock(lockPath)) {
            return 1;
        }
    }

    // ---- Start server -------------------------------------------------------
    try {
        g_server = std::make_unique<Server>(config);
        g_server->start();
    } catch (const std::exception& e) {
        std::cerr << "[main] Fatal: " << e.what() << std::endl;
        releasePidLock();
        return 1;
    }

    // ---- Cleanup ------------------------------------------------------------
    g_server.reset();
    releasePidLock();
    std::cout << "[main] Goodbye." << std::endl;
    return 0;
}
