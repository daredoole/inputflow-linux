#include "MediaKeyBridge.h"

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <iostream>
#include <mutex>
#include <sstream>
#include <sys/wait.h>
#include <unistd.h>

namespace mwb {

namespace {

struct MediaKeyBridgeConfig {
    bool mprisEnabled{true};
    std::string playerName;
};

std::mutex g_mediaKeyBridgeConfigMutex;
MediaKeyBridgeConfig g_mediaKeyBridgeConfig;

bool IsTruthyEnv(const char* name) {
    const char* value = std::getenv(name);
    if (value == nullptr) {
        return false;
    }

    const std::string text(value);
    return text == "1" || text == "true" || text == "TRUE" || text == "yes" || text == "YES" ||
           text == "on" || text == "ON";
}

bool DebugMediaKeysEnabled() {
    static const bool enabled = IsTruthyEnv("MWB_DEBUG_MEDIA_KEYS");
    return enabled;
}

std::optional<std::string> FindExecutableInPath(const std::string& name) {
    if (name.empty()) {
        return std::nullopt;
    }

    if (name.find('/') != std::string::npos) {
        return access(name.c_str(), X_OK) == 0 ? std::optional<std::string>(name) : std::nullopt;
    }

    const char* pathValue = std::getenv("PATH");
    if (pathValue == nullptr) {
        return std::nullopt;
    }

    std::stringstream stream(pathValue);
    std::string directory;
    while (std::getline(stream, directory, ':')) {
        if (directory.empty()) {
            directory = ".";
        }

        const std::string candidate = directory + "/" + name;
        if (access(candidate.c_str(), X_OK) == 0) {
            return candidate;
        }
    }

    return std::nullopt;
}

bool RunCommandQuietly(const std::vector<std::string>& argv) {
    if (argv.empty()) {
        return false;
    }

    const pid_t pid = fork();
    if (pid < 0) {
        return false;
    }

    if (pid == 0) {
        const int devNull = open("/dev/null", O_RDWR);
        if (devNull >= 0) {
            dup2(devNull, STDOUT_FILENO);
            dup2(devNull, STDERR_FILENO);
            close(devNull);
        }

        std::vector<char*> execArgv;
        execArgv.reserve(argv.size() + 1);
        for (const std::string& arg : argv) {
            execArgv.push_back(const_cast<char*>(arg.c_str()));
        }
        execArgv.push_back(nullptr);

        execv(execArgv[0], execArgv.data());
        _exit(127);
    }

    int status = 0;
    while (waitpid(pid, &status, 0) < 0) {
        if (errno != EINTR) {
            return false;
        }
    }

    return WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

} // namespace

std::optional<MediaKeyAction> MediaActionForVirtualKey(int vkCode) {
    switch (vkCode) {
        case 0xB0:
            return MediaKeyAction::Next;
        case 0xB1:
            return MediaKeyAction::Previous;
        case 0xB2:
            return MediaKeyAction::Stop;
        case 0xB3:
            return MediaKeyAction::PlayPause;
        default:
            return std::nullopt;
    }
}

const char* PlayerctlCommandForAction(MediaKeyAction action) {
    switch (action) {
        case MediaKeyAction::Previous:
            return "previous";
        case MediaKeyAction::Stop:
            return "stop";
        case MediaKeyAction::PlayPause:
            return "play-pause";
        case MediaKeyAction::Next:
            return "next";
    }

    return "";
}

std::vector<std::string> BuildPlayerctlArguments(MediaKeyAction action, const std::string& playerName) {
    std::vector<std::string> args{"playerctl"};
    if (!playerName.empty()) {
        args.push_back("--player");
        args.push_back(playerName);
    }
    args.push_back(PlayerctlCommandForAction(action));
    return args;
}

void ConfigureMediaKeyBridge(bool mprisEnabled, const std::string& playerName) {
    std::lock_guard<std::mutex> lock(g_mediaKeyBridgeConfigMutex);
    g_mediaKeyBridgeConfig.mprisEnabled = mprisEnabled;
    g_mediaKeyBridgeConfig.playerName = playerName;
}

bool DispatchMediaKeyAction(MediaKeyAction action) {
    MediaKeyBridgeConfig config;
    {
        std::lock_guard<std::mutex> lock(g_mediaKeyBridgeConfigMutex);
        config = g_mediaKeyBridgeConfig;
    }

    if (const char* playerNameEnv = std::getenv("MWB_MPRIS_PLAYER");
        playerNameEnv != nullptr && *playerNameEnv != '\0') {
        config.playerName = playerNameEnv;
    }
    if (IsTruthyEnv("MWB_DISABLE_MPRIS_MEDIA_KEYS")) {
        config.mprisEnabled = false;
    }

    if (!config.mprisEnabled) {
        return false;
    }

    const auto playerctlPath = FindExecutableInPath("playerctl");
    if (!playerctlPath.has_value()) {
        if (DebugMediaKeysEnabled()) {
            std::cout << "[MEDIA] playerctl not found; media key was not dispatched via MPRIS." << std::endl;
        }
        return false;
    }

    std::vector<std::string> args = BuildPlayerctlArguments(action, config.playerName);
    args[0] = *playerctlPath;

    const bool ok = RunCommandQuietly(args);
    if (DebugMediaKeysEnabled()) {
        std::cout << "[MEDIA] MPRIS action " << PlayerctlCommandForAction(action)
                  << (ok ? " dispatched" : " failed") << std::endl;
    }
    return ok;
}

} // namespace mwb
