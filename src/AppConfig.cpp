#include "AppConfig.h"

#include <cctype>
#include <cstdlib>
#include <fstream>
#include <limits>
#include <sstream>
#include <utility>

namespace {

constexpr const char* kConfigDirName = "mwb-client";
constexpr const char* kConfigFileName = "config.ini";

std::string_view Trim(std::string_view value) {
    size_t start = 0;
    while (start < value.size() && std::isspace(static_cast<unsigned char>(value[start])) != 0) {
        ++start;
    }

    size_t end = value.size();
    while (end > start && std::isspace(static_cast<unsigned char>(value[end - 1])) != 0) {
        --end;
    }

    return value.substr(start, end - start);
}

std::string ToLower(std::string_view value) {
    std::string lowered;
    lowered.reserve(value.size());
    for (const char ch : value) {
        lowered.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(ch))));
    }
    return lowered;
}

std::string_view StripMatchingQuotes(std::string_view value) {
    if (value.size() < 2) {
        return value;
    }

    const char first = value.front();
    const char last = value.back();
    if ((first == '"' && last == '"') || (first == '\'' && last == '\'')) {
        return value.substr(1, value.size() - 2);
    }

    return value;
}

void SetError(std::string* errorMessage, std::string message) {
    if (errorMessage != nullptr) {
        *errorMessage = std::move(message);
    }
}

bool ParseAndAssignInt(std::string_view value,
                       int minValue,
                       int maxValue,
                       std::optional<int>& outValue) {
    const auto parsed = mwb::ParseConfigInt(value, minValue, maxValue);
    if (!parsed.has_value()) {
        return false;
    }

    outValue = *parsed;
    return true;
}

bool ParseAndAssignRequiredInt(std::string_view value,
                               int minValue,
                               int maxValue,
                               int& outValue) {
    const auto parsed = mwb::ParseConfigInt(value, minValue, maxValue);
    if (!parsed.has_value()) {
        return false;
    }

    outValue = *parsed;
    return true;
}

std::string RenderBool(bool value) {
    return value ? "true" : "false";
}

void AssignInlineKey(mwb::AppConfig& config, std::string value) {
    config.key = std::move(value);
    if (!config.key.empty()) {
        config.keyFile.clear();
        config.keySecretId.clear();
    }
}

void AssignKeyFile(mwb::AppConfig& config, std::string value) {
    config.keyFile = std::move(value);
    if (!config.keyFile.empty()) {
        config.key.clear();
        config.keySecretId.clear();
    }
}

void AssignKeySecretId(mwb::AppConfig& config, std::string value) {
    config.keySecretId = std::move(value);
    if (!config.keySecretId.empty()) {
        config.key.clear();
        config.keyFile.clear();
    }
}

} // namespace

namespace mwb {

AppConfig LoadDefaultAppConfig() {
    return AppConfig{};
}

std::optional<bool> ParseConfigBool(std::string_view value) {
    const std::string lowered = ToLower(Trim(value));
    if (lowered == "1" || lowered == "true" || lowered == "yes" || lowered == "on") {
        return true;
    }
    if (lowered == "0" || lowered == "false" || lowered == "no" || lowered == "off") {
        return false;
    }
    return std::nullopt;
}

std::optional<int> ParseConfigInt(std::string_view value) {
    const std::string trimmed(Trim(value));
    if (trimmed.empty()) {
        return std::nullopt;
    }

    try {
        size_t end = 0;
        const long long parsed = std::stoll(trimmed, &end, 10);
        if (end != trimmed.size()) {
            return std::nullopt;
        }
        if (parsed < std::numeric_limits<int>::min() || parsed > std::numeric_limits<int>::max()) {
            return std::nullopt;
        }
        return static_cast<int>(parsed);
    } catch (...) {
        return std::nullopt;
    }
}

std::optional<int> ParseConfigInt(std::string_view value, int minValue, int maxValue) {
    const auto parsed = ParseConfigInt(value);
    if (!parsed.has_value() || *parsed < minValue || *parsed > maxValue) {
        return std::nullopt;
    }
    return parsed;
}

std::filesystem::path ResolveDefaultAppConfigPath() {
    if (const char* xdgConfigHome = std::getenv("XDG_CONFIG_HOME");
        xdgConfigHome != nullptr && xdgConfigHome[0] != '\0') {
        return std::filesystem::path(xdgConfigHome) / kConfigDirName / kConfigFileName;
    }

    if (const char* home = std::getenv("HOME");
        home != nullptr && home[0] != '\0') {
        return std::filesystem::path(home) / ".config" / kConfigDirName / kConfigFileName;
    }

    return {};
}

bool ParseAppConfig(std::string_view text, AppConfig& outConfig, std::string* errorMessage) {
    std::istringstream stream{std::string(text)};
    std::string line;
    size_t lineNumber = 0;

    while (std::getline(stream, line)) {
        ++lineNumber;
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }

        std::string_view trimmed = Trim(line);
        if (trimmed.empty() || trimmed.front() == '#' || trimmed.front() == ';') {
            continue;
        }

        const size_t separator = trimmed.find('=');
        if (separator == std::string_view::npos) {
            SetError(errorMessage, "Config line " + std::to_string(lineNumber) + " is missing '='.");
            return false;
        }

        const std::string_view key = Trim(trimmed.substr(0, separator));
        std::string_view value = Trim(trimmed.substr(separator + 1));
        value = StripMatchingQuotes(value);

        if (key.empty()) {
            SetError(errorMessage, "Config line " + std::to_string(lineNumber) + " has an empty key.");
            return false;
        }

        if (key == "host") {
            outConfig.host = std::string(value);
            continue;
        }

        if (key == "key") {
            AssignInlineKey(outConfig, std::string(value));
            continue;
        }

        if (key == "key_file") {
            AssignKeyFile(outConfig, std::string(value));
            continue;
        }

        if (key == "key_secret_id") {
            AssignKeySecretId(outConfig, std::string(value));
            continue;
        }

        if (key == "machine_name") {
            outConfig.machineName = std::string(value);
            continue;
        }

        if (key == "port") {
            if (!ParseAndAssignRequiredInt(value, 1, 65535, outConfig.port)) {
                SetError(errorMessage, "Config key 'port' expects an integer between 1 and 65535.");
                return false;
            }
            continue;
        }

        if (key == "clipboard_enabled") {
            const auto parsed = ParseConfigBool(value);
            if (!parsed.has_value()) {
                SetError(errorMessage, "Config key 'clipboard_enabled' expects true/false.");
                return false;
            }
            outConfig.clipboardEnabled = *parsed;
            continue;
        }

        if (key == "clipboard_send_enabled") {
            const auto parsed = ParseConfigBool(value);
            if (!parsed.has_value()) {
                SetError(errorMessage, "Config key 'clipboard_send_enabled' expects true/false.");
                return false;
            }
            outConfig.clipboardSendEnabled = *parsed;
            continue;
        }

        if (key == "clipboard_force_poll") {
            const auto parsed = ParseConfigBool(value);
            if (!parsed.has_value()) {
                SetError(errorMessage, "Config key 'clipboard_force_poll' expects true/false.");
                return false;
            }
            outConfig.clipboardForcePoll = *parsed;
            continue;
        }

        if (key == "clipboard_poll_ms") {
            if (!ParseAndAssignRequiredInt(value, 1, std::numeric_limits<int>::max(), outConfig.clipboardPollMs)) {
                SetError(errorMessage, "Config key 'clipboard_poll_ms' expects a positive integer.");
                return false;
            }
            continue;
        }

        if (key == "auto_connect_enabled") {
            const auto parsed = ParseConfigBool(value);
            if (!parsed.has_value()) {
                SetError(errorMessage, "Config key 'auto_connect_enabled' expects true/false.");
                return false;
            }
            outConfig.autoConnectEnabled = *parsed;
            continue;
        }

        if (key == "reconnect_initial_backoff_ms") {
            if (!ParseAndAssignRequiredInt(value, 100, std::numeric_limits<int>::max(), outConfig.reconnectInitialBackoffMs)) {
                SetError(errorMessage, "Config key 'reconnect_initial_backoff_ms' expects an integer >= 100.");
                return false;
            }
            continue;
        }

        if (key == "reconnect_max_backoff_ms") {
            if (!ParseAndAssignRequiredInt(value, 100, std::numeric_limits<int>::max(), outConfig.reconnectMaxBackoffMs)) {
                SetError(errorMessage, "Config key 'reconnect_max_backoff_ms' expects an integer >= 100.");
                return false;
            }
            continue;
        }

        if (key == "reconnect_idle_retry_ms") {
            if (!ParseAndAssignRequiredInt(value, 100, std::numeric_limits<int>::max(), outConfig.reconnectIdleRetryMs)) {
                SetError(errorMessage, "Config key 'reconnect_idle_retry_ms' expects an integer >= 100.");
                return false;
            }
            continue;
        }

        if (key == "screen_width") {
            if (Trim(value).empty()) {
                outConfig.screenWidth.reset();
            } else if (!ParseAndAssignInt(value, 1, std::numeric_limits<int>::max(), outConfig.screenWidth)) {
                SetError(errorMessage, "Config key 'screen_width' expects a positive integer.");
                return false;
            }
            continue;
        }

        if (key == "screen_height") {
            if (Trim(value).empty()) {
                outConfig.screenHeight.reset();
            } else if (!ParseAndAssignInt(value, 1, std::numeric_limits<int>::max(), outConfig.screenHeight)) {
                SetError(errorMessage, "Config key 'screen_height' expects a positive integer.");
                return false;
            }
            continue;
        }

        if (key == "mpris_media_keys_enabled") {
            const auto parsed = ParseConfigBool(value);
            if (!parsed.has_value()) {
                SetError(errorMessage, "Config key 'mpris_media_keys_enabled' expects true/false.");
                return false;
            }
            outConfig.mprisMediaKeysEnabled = *parsed;
            continue;
        }

        if (key == "mpris_player") {
            outConfig.mprisPlayer = std::string(value);
            continue;
        }

        if (key == "latency_report") {
            const auto parsed = ParseConfigBool(value);
            if (!parsed.has_value()) {
                SetError(errorMessage, "Config key 'latency_report' expects true/false.");
                return false;
            }
            outConfig.latencyReport = *parsed;
            continue;
        }

        SetError(errorMessage, "Unknown config key '" + std::string(key) + "' on line " + std::to_string(lineNumber) + ".");
        return false;
    }

    return true;
}

bool LoadAppConfig(const std::filesystem::path& path, AppConfig& outConfig, std::string* errorMessage) {
    std::ifstream file(path);
    if (!file) {
        SetError(errorMessage, "Failed to open config file: " + path.string());
        return false;
    }

    std::ostringstream buffer;
    buffer << file.rdbuf();
    if (!file.good() && !file.eof()) {
        SetError(errorMessage, "Failed to read config file: " + path.string());
        return false;
    }

    outConfig = LoadDefaultAppConfig();
    return ParseAppConfig(buffer.str(), outConfig, errorMessage);
}

bool LoadAppConfigFromDefaultPath(AppConfig& outConfig, std::string* errorMessage) {
    const std::filesystem::path path = ResolveDefaultAppConfigPath();
    if (path.empty()) {
        SetError(errorMessage, "Could not resolve a default config path from XDG_CONFIG_HOME or HOME.");
        return false;
    }

    return LoadAppConfig(path, outConfig, errorMessage);
}

std::string RenderAppConfig(const AppConfig& config) {
    std::ostringstream out;
    out << "host=" << config.host << '\n';
    out << "key=" << config.key << '\n';
    out << "key_file=" << config.keyFile << '\n';
    out << "key_secret_id=" << config.keySecretId << '\n';
    out << "machine_name=" << config.machineName << '\n';
    out << "port=" << config.port << '\n';
    out << "clipboard_enabled=" << RenderBool(config.clipboardEnabled) << '\n';
    out << "clipboard_send_enabled=" << RenderBool(config.clipboardSendEnabled) << '\n';
    out << "clipboard_force_poll=" << RenderBool(config.clipboardForcePoll) << '\n';
    out << "clipboard_poll_ms=" << config.clipboardPollMs << '\n';
    out << "auto_connect_enabled=" << RenderBool(config.autoConnectEnabled) << '\n';
    out << "reconnect_initial_backoff_ms=" << config.reconnectInitialBackoffMs << '\n';
    out << "reconnect_max_backoff_ms=" << config.reconnectMaxBackoffMs << '\n';
    out << "reconnect_idle_retry_ms=" << config.reconnectIdleRetryMs << '\n';
    out << "screen_width=";
    if (config.screenWidth.has_value()) {
        out << *config.screenWidth;
    }
    out << '\n';
    out << "screen_height=";
    if (config.screenHeight.has_value()) {
        out << *config.screenHeight;
    }
    out << '\n';
    out << "mpris_media_keys_enabled=" << RenderBool(config.mprisMediaKeysEnabled) << '\n';
    out << "mpris_player=" << config.mprisPlayer << '\n';
    out << "latency_report=" << RenderBool(config.latencyReport) << '\n';
    return out.str();
}

std::string RenderSampleAppConfig() {
    AppConfig sample = LoadDefaultAppConfig();
    sample.host = "192.0.2.10";
    sample.keyFile = "security-key";
    sample.machineName = "fedora";

    std::ostringstream out;
    out << "# InputFlow Linux client config.\n";
    out << "# Save as $XDG_CONFIG_HOME/mwb-client/config.ini or ~/.config/mwb-client/config.ini.\n";
    out << "# Blank lines and lines starting with # or ; are ignored.\n";
    out << "# Prefer key_secret_id=... or key_file=... to keep the security key out of this config file.\n";
    out << "# key_secret_id uses the desktop keyring via Secret Service when secret-tool is available.\n";
    out << "# Relative key_file paths resolve against the directory containing config.ini.\n";
    out << "# Set auto_connect_enabled=false to keep the service idle until you re-enable it.\n";
    out << "# Set screen_width and screen_height to your local desktop size when needed.\n";
    out << RenderAppConfig(sample);
    return out.str();
}

bool WriteAppConfig(const std::filesystem::path& path,
                    const AppConfig& config,
                    std::string* errorMessage) {
    try {
        const std::filesystem::path parent = path.parent_path();
        if (!parent.empty()) {
            std::filesystem::create_directories(parent);
        }

        std::ofstream file(path, std::ios::out | std::ios::trunc);
        if (!file) {
            SetError(errorMessage, "Failed to open config file for writing: " + path.string());
            return false;
        }

        file << RenderAppConfig(config);
        if (!file) {
            SetError(errorMessage, "Failed to write config file: " + path.string());
            return false;
        }
        file.close();

        std::error_code permissionError;
        std::filesystem::permissions(
            path,
            std::filesystem::perms::owner_read | std::filesystem::perms::owner_write,
            std::filesystem::perm_options::replace,
            permissionError);
        if (permissionError) {
            SetError(errorMessage, "Failed to secure config file permissions for '" + path.string() + "': " + permissionError.message());
            return false;
        }
    } catch (const std::exception& ex) {
        SetError(errorMessage, "Failed to write config file '" + path.string() + "': " + ex.what());
        return false;
    }

    return true;
}

bool WriteAppConfigToDefaultPath(const AppConfig& config, std::string* errorMessage) {
    const std::filesystem::path path = ResolveDefaultAppConfigPath();
    if (path.empty()) {
        SetError(errorMessage, "Could not resolve a default config path from XDG_CONFIG_HOME or HOME.");
        return false;
    }

    return WriteAppConfig(path, config, errorMessage);
}

} // namespace mwb
