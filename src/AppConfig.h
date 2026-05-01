#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace mwb {

enum class ConnectionMode {
    PowerToys,
    InputFlow,
    Hybrid,
};

struct AppConfig {
    ConnectionMode connectionMode{ConnectionMode::PowerToys};
    std::string host;
    std::string key;
    std::string keyFile;
    std::string keySecretId;
    std::string machineName;
    int port{15101};
    bool clipboardEnabled{true};
    bool clipboardSendEnabled{true};
    bool clipboardForcePoll{false};
    int clipboardPollMs{5000};
    bool autoConnectEnabled{true};
    int reconnectInitialBackoffMs{1000};
    int reconnectMaxBackoffMs{30000};
    int reconnectIdleRetryMs{30000};
    std::optional<int> screenWidth;
    std::optional<int> screenHeight;
    bool mprisMediaKeysEnabled{true};
    std::string mprisPlayer;
    bool latencyReport{false};
    bool topologyRuntimeEnabled{false};
    std::string topologyFile;
    bool androidPeersEnabled{false};
    int androidRelayPort{15102};
    std::string androidRelaySecret;
    std::string androidPeerName{"android"};
    std::string androidCaptureBackend{"none"};
    bool androidLayoutEditorEnabled{true};
    int androidDeviceWidth{1920};
    int androidDeviceHeight{1200};
};

AppConfig LoadDefaultAppConfig();

std::optional<ConnectionMode> ParseConnectionMode(std::string_view value);
std::string_view ConnectionModeName(ConnectionMode mode);
bool PowerToysCompatibilityEnabled(ConnectionMode mode);
bool InputFlowPeersEnabled(ConnectionMode mode);

std::optional<bool> ParseConfigBool(std::string_view value);
std::optional<int> ParseConfigInt(std::string_view value);
std::optional<int> ParseConfigInt(std::string_view value, int minValue, int maxValue);

std::filesystem::path ResolveDefaultAppConfigPath();

bool ParseAppConfig(std::string_view text, AppConfig& outConfig, std::string* errorMessage = nullptr);
bool LoadAppConfig(const std::filesystem::path& path, AppConfig& outConfig, std::string* errorMessage = nullptr);
bool LoadAppConfigFromDefaultPath(AppConfig& outConfig, std::string* errorMessage = nullptr);

std::string RenderAppConfig(const AppConfig& config);
std::string RenderSampleAppConfig();

bool WriteAppConfig(const std::filesystem::path& path, const AppConfig& config, std::string* errorMessage = nullptr);
bool WriteAppConfigToDefaultPath(const AppConfig& config, std::string* errorMessage = nullptr);

inline std::filesystem::path DefaultConfigPath() {
    return ResolveDefaultAppConfigPath();
}

inline bool LoadConfigFile(const std::filesystem::path& path, AppConfig& config, std::string& errorMessage) {
    return LoadAppConfig(path, config, &errorMessage);
}

inline bool SaveConfigFile(const std::filesystem::path& path, const AppConfig& config, std::string& errorMessage) {
    return WriteAppConfig(path, config, &errorMessage);
}

} // namespace mwb
