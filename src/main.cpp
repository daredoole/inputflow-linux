#include <algorithm>
#include <atomic>
#include <arpa/inet.h>
#include <cctype>
#include <cstdint>
#include <chrono>
#include <csignal>
#include <cstring>
#include <cstdlib>
#include <filesystem>
#include <fcntl.h>
#include <fstream>
#include <grp.h>
#include <initializer_list>
#include <iostream>
#include <limits>
#include <netdb.h>
#include <optional>
#include <poll.h>
#include <sstream>
#include <string>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/utsname.h>
#include <thread>
#include <unistd.h>
#include <vector>

#include "AppConfig.h"
#include "AppState.h"
#include "ClientRuntime.h"
#include "Discovery.h"
#include "PeerRecovery.h"
#include "SecretStore.h"

namespace {

constexpr int kDefaultPort = 15101;

std::atomic<bool> g_stopRequested{false};

void HandleStopSignal(int) {
    g_stopRequested = true;
}

void PrintGeneralUsage(std::ostream& out, const char* argv0) {
    const std::string binary = std::filesystem::path(argv0).filename().string();
    out << "Usage: " << binary << " <WINDOWS_IP> <SECURITY_KEY> [PORT]\n";
    out << "       " << binary
        << " run [--config PATH] [--state PATH] [--host IP] [--key KEY | --key-file PATH | --key-secret-id ID]"
        << " [--name NAME] [--port PORT]"
        << " [--enable-clipboard | --disable-clipboard | --clipboard-receive-only | --clipboard-full]"
        << " [--clipboard-force-poll] [--clipboard-poll-ms MS]"
        << " [--screen-width PX --screen-height PX]"
        << " [--enable-mpris-media-keys | --disable-mpris-media-keys] [--mpris-player PLAYER]"
        << " [--auto-connect | --manual-only]"
        << " [--reconnect-initial-backoff-ms MS] [--reconnect-max-backoff-ms MS] [--reconnect-idle-retry-ms MS]"
        << " [--latency-report]\n";
    out << "       " << binary << " discover [--state PATH] [--port PORT] [--timeout-ms MS] [--max-hosts N]\n";
    out << "       " << binary << " doctor [--config PATH]\n";
    out << "       " << binary << " init-config [--config PATH] [--force] [--host IP] [--key KEY | --key-file PATH | --key-secret-id ID] [--name NAME] [--port PORT]\n";
    out << "       " << binary << " export-windows-pair [--config PATH] [--output PATH] [--force] [--linux-ip IP] [--position auto|top-left|top-right|bottom-left|bottom-right] [--key KEY | --key-file PATH | --key-secret-id ID] [--name NAME]\n";
    out << "       " << binary << " install-user-service [--config PATH] [--unit PATH] [--force]\n";
    out << "       " << binary << " secret-store [--config PATH] --secret-id ID [--key KEY | --key-file PATH | --stdin]\n";
    out << "       " << binary << " secret-clear [--config PATH] [--secret-id ID]\n";
    out << "Connection policy: auto_connect_enabled=true|false, reconnect_initial_backoff_ms, reconnect_max_backoff_ms, reconnect_idle_retry_ms in config\n";
    out << "Media keys: mpris_media_keys_enabled=true|false, mpris_player=PLAYER in config\n";
    out << "Set latency_report=true, MWB_LATENCY_REPORT=1, or use --latency-report to print client-side input queue/inject timing on shutdown\n";
}

bool IsTruthyEnv(const char* name) {
    const char* value = std::getenv(name);
    if (value == nullptr) {
        return false;
    }

    const std::string text(value);
    return text == "1" || text == "true" || text == "TRUE" || text == "yes" || text == "YES" || text == "on" || text == "ON";
}

std::optional<std::filesystem::path> FindExecutableInPath(const std::string& name) {
    if (name.empty()) {
        return std::nullopt;
    }
    if (name.find('/') != std::string::npos) {
        return access(name.c_str(), X_OK) == 0 ? std::optional<std::filesystem::path>(name) : std::nullopt;
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
        const std::filesystem::path candidate = std::filesystem::path(directory) / name;
        if (access(candidate.c_str(), X_OK) == 0) {
            return candidate;
        }
    }

    return std::nullopt;
}

std::filesystem::path DefaultUserServicePath() {
    if (const char* xdg = std::getenv("XDG_CONFIG_HOME"); xdg != nullptr && *xdg != '\0') {
        return std::filesystem::path(xdg) / "systemd" / "user" / "mwb-client.service";
    }
    if (const char* home = std::getenv("HOME"); home != nullptr && *home != '\0') {
        return std::filesystem::path(home) / ".config" / "systemd" / "user" / "mwb-client.service";
    }
    return std::filesystem::current_path() / "mwb-client.service";
}

bool CommandSucceeds(const std::string& command) {
    return std::system((command + " >/dev/null 2>&1").c_str()) == 0;
}

std::string GroupName(gid_t gid) {
    if (const struct group* group = getgrgid(gid); group != nullptr && group->gr_name != nullptr) {
        return group->gr_name;
    }
    return std::to_string(static_cast<unsigned long>(gid));
}

std::string FormatMode(mode_t mode) {
    std::ostringstream output;
    output << "0" << std::oct << (mode & 0777);
    return output.str();
}

bool CurrentUserInGroup(const std::string& groupName) {
    const struct group* group = getgrnam(groupName.c_str());
    if (group == nullptr) {
        return false;
    }

    if (getgid() == group->gr_gid || getegid() == group->gr_gid) {
        return true;
    }

    const int groupCount = getgroups(0, nullptr);
    if (groupCount <= 0) {
        return false;
    }

    std::vector<gid_t> groups(static_cast<std::size_t>(groupCount));
    if (getgroups(groupCount, groups.data()) < 0) {
        return false;
    }

    return std::find(groups.begin(), groups.end(), group->gr_gid) != groups.end();
}

bool FileExistsAny(std::initializer_list<const char*> paths) {
    for (const char* path : paths) {
        if (path != nullptr && std::filesystem::exists(path)) {
            return true;
        }
    }
    return false;
}

std::optional<std::string> ReadFirstLineFile(const std::filesystem::path& path) {
    std::ifstream file(path);
    std::string line;
    if (!file || !std::getline(file, line)) {
        return std::nullopt;
    }
    return line;
}

std::string EnvValueOrUnset(const char* name) {
    const char* value = std::getenv(name);
    if (value == nullptr || *value == '\0') {
        return "<unset>";
    }
    return value;
}

std::optional<std::string> OsPrettyName() {
    std::ifstream file("/etc/os-release");
    std::string line;
    while (std::getline(file, line)) {
        constexpr std::string_view prefix = "PRETTY_NAME=";
        if (line.rfind(prefix, 0) != 0) {
            continue;
        }
        std::string value = line.substr(prefix.size());
        if (value.size() >= 2 && value.front() == '"' && value.back() == '"') {
            value = value.substr(1, value.size() - 2);
        }
        return value;
    }
    return std::nullopt;
}

std::string KernelRelease() {
    struct utsname info {};
    if (uname(&info) == 0) {
        return info.release;
    }
    return "unknown";
}

std::string DrmSummary() {
    const std::filesystem::path drmRoot("/sys/class/drm");
    if (!std::filesystem::exists(drmRoot)) {
        return "unavailable";
    }

    std::vector<std::string> connectors;
    std::error_code error;
    for (const auto& entry : std::filesystem::directory_iterator(drmRoot, error)) {
        if (error || !entry.is_directory()) {
            continue;
        }

        const auto status = ReadFirstLineFile(entry.path() / "status");
        if (!status || *status != "connected") {
            continue;
        }

        const auto enabled = ReadFirstLineFile(entry.path() / "enabled").value_or("unknown");
        const auto mode = ReadFirstLineFile(entry.path() / "modes").value_or("unknown");
        connectors.push_back(entry.path().filename().string() + "=" + enabled + "/" + mode);
    }

    if (connectors.empty()) {
        return "no connected DRM connectors found";
    }

    std::ostringstream output;
    for (std::size_t index = 0; index < connectors.size(); ++index) {
        if (index > 0) {
            output << "; ";
        }
        output << connectors[index];
    }
    return output.str();
}

void PrintDoctorLine(const std::string& status, const std::string& name, const std::string& detail) {
    std::cout << "[" << status << "] " << name << ": " << detail << std::endl;
}

std::optional<int> ParsePositiveInt(const std::string& value) {
    try {
        size_t end = 0;
        const long parsed = std::stol(value, &end);
        if (end != value.size() || parsed <= 0 || parsed > std::numeric_limits<int>::max()) {
            return std::nullopt;
        }
        return static_cast<int>(parsed);
    } catch (...) {
        return std::nullopt;
    }
}

bool ReadEnvPositiveInt(const char* name, std::optional<int>& outValue) {
    const char* value = std::getenv(name);
    if (value == nullptr || *value == '\0') {
        return true;
    }

    const auto parsed = ParsePositiveInt(value);
    if (!parsed) {
        std::cerr << "ERR: Invalid value for " << name << "." << std::endl;
        return false;
    }

    outValue = *parsed;
    return true;
}

std::string_view TrimWhitespace(std::string_view value) {
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

void SetInlineKey(mwb::AppConfig& config,
                  std::optional<std::filesystem::path>& keyFileBaseDir,
                  std::string value) {
    config.key = std::move(value);
    if (!config.key.empty()) {
        config.keyFile.clear();
        config.keySecretId.clear();
        keyFileBaseDir.reset();
    }
}

void SetKeyFile(mwb::AppConfig& config,
                std::optional<std::filesystem::path>& keyFileBaseDir,
                std::string value,
                std::optional<std::filesystem::path> baseDir = std::nullopt) {
    config.keyFile = std::move(value);
    if (config.keyFile.empty()) {
        keyFileBaseDir.reset();
        return;
    }

    config.key.clear();
    config.keySecretId.clear();
    keyFileBaseDir = std::move(baseDir);
}

void SetKeySecretId(mwb::AppConfig& config,
                    std::optional<std::filesystem::path>& keyFileBaseDir,
                    std::string value) {
    config.keySecretId = std::move(value);
    if (!config.keySecretId.empty()) {
        config.key.clear();
        config.keyFile.clear();
        keyFileBaseDir.reset();
    }
}

bool ResolveConfiguredKey(mwb::AppConfig& config,
                          const std::optional<std::filesystem::path>& keyFileBaseDir,
                          std::string* errorMessage) {
    if (!config.key.empty()) {
        return true;
    }

    if (config.keyFile.empty()) {
        if (config.keySecretId.empty()) {
            return true;
        }

        std::string resolvedKey;
        if (!mwb::LookupSecretKey(config.keySecretId, resolvedKey, errorMessage)) {
            return false;
        }
        config.key = resolvedKey;
        return true;
    }

    std::filesystem::path keyPath = config.keyFile;
    if (keyPath.is_relative() && keyFileBaseDir.has_value() && !keyFileBaseDir->empty()) {
        keyPath = *keyFileBaseDir / keyPath;
    }

    std::ifstream file(keyPath);
    if (!file) {
        if (errorMessage != nullptr) {
            *errorMessage = "Failed to open key file: " + keyPath.string();
        }
        return false;
    }

    std::ostringstream buffer;
    buffer << file.rdbuf();
    if (!file.good() && !file.eof()) {
        if (errorMessage != nullptr) {
            *errorMessage = "Failed to read key file: " + keyPath.string();
        }
        return false;
    }

    const std::string fileContents = buffer.str();
    const std::string resolvedKey(TrimWhitespace(fileContents));
    if (resolvedKey.empty()) {
        if (errorMessage != nullptr) {
            *errorMessage = "Key file is empty: " + keyPath.string();
        }
        return false;
    }

    config.key = resolvedKey;
    return true;
}

bool ApplyEnvironmentOverrides(mwb::AppConfig& config,
                               std::optional<std::filesystem::path>& keyFileBaseDir) {
    if (const char* keyFile = std::getenv("MWB_KEY_FILE"); keyFile != nullptr && *keyFile != '\0') {
        SetKeyFile(config, keyFileBaseDir, keyFile);
    }
    if (const char* keySecretId = std::getenv("MWB_KEY_SECRET_ID");
        keySecretId != nullptr && *keySecretId != '\0') {
        SetKeySecretId(config, keyFileBaseDir, keySecretId);
    }

    std::optional<int> widthOverride;
    std::optional<int> heightOverride;
    if (!ReadEnvPositiveInt("MWB_SCREEN_WIDTH", widthOverride) ||
        !ReadEnvPositiveInt("MWB_SCREEN_HEIGHT", heightOverride)) {
        return false;
    }

    if (widthOverride && heightOverride) {
        config.screenWidth = *widthOverride;
        config.screenHeight = *heightOverride;
    } else if (widthOverride || heightOverride) {
        std::cerr << "WARN: Ignoring incomplete MWB_SCREEN_WIDTH/MWB_SCREEN_HEIGHT override." << std::endl;
    }

    if (IsTruthyEnv("MWB_DISABLE_CLIPBOARD")) {
        config.clipboardEnabled = false;
    }
    if (IsTruthyEnv("MWB_CLIPBOARD_RECEIVE_ONLY")) {
        config.clipboardSendEnabled = false;
    }
    if (IsTruthyEnv("MWB_CLIPBOARD_FORCE_POLL")) {
        config.clipboardForcePoll = true;
    }

    std::optional<int> clipboardPollOverride;
    if (!ReadEnvPositiveInt("MWB_CLIPBOARD_POLL_MS", clipboardPollOverride)) {
        return false;
    }
    if (clipboardPollOverride) {
        config.clipboardPollMs = *clipboardPollOverride;
    }

    if (const char* mprisPlayer = std::getenv("MWB_MPRIS_PLAYER");
        mprisPlayer != nullptr && *mprisPlayer != '\0') {
        config.mprisPlayer = mprisPlayer;
    }
    if (IsTruthyEnv("MWB_DISABLE_MPRIS_MEDIA_KEYS")) {
        config.mprisMediaKeysEnabled = false;
    }
    if (IsTruthyEnv("MWB_LATENCY_REPORT")) {
        config.latencyReport = true;
    }

    if (const char* autoConnect = std::getenv("MWB_AUTO_CONNECT_ENABLED");
        autoConnect != nullptr && *autoConnect != '\0') {
        const auto parsed = mwb::ParseConfigBool(autoConnect);
        if (!parsed.has_value()) {
            std::cerr << "ERR: Invalid value for MWB_AUTO_CONNECT_ENABLED." << std::endl;
            return false;
        }
        config.autoConnectEnabled = *parsed;
    }

    std::optional<int> reconnectInitialOverride;
    std::optional<int> reconnectMaxOverride;
    std::optional<int> reconnectIdleOverride;
    if (!ReadEnvPositiveInt("MWB_RECONNECT_INITIAL_BACKOFF_MS", reconnectInitialOverride) ||
        !ReadEnvPositiveInt("MWB_RECONNECT_MAX_BACKOFF_MS", reconnectMaxOverride) ||
        !ReadEnvPositiveInt("MWB_RECONNECT_IDLE_RETRY_MS", reconnectIdleOverride)) {
        return false;
    }
    if (reconnectInitialOverride) {
        config.reconnectInitialBackoffMs = std::max(100, *reconnectInitialOverride);
    }
    if (reconnectMaxOverride) {
        config.reconnectMaxBackoffMs = std::max(100, *reconnectMaxOverride);
    }
    if (reconnectIdleOverride) {
        config.reconnectIdleRetryMs = std::max(100, *reconnectIdleOverride);
    }
    if (config.reconnectMaxBackoffMs < config.reconnectInitialBackoffMs) {
        config.reconnectMaxBackoffMs = config.reconnectInitialBackoffMs;
    }
    if (config.reconnectIdleRetryMs < config.reconnectMaxBackoffMs) {
        config.reconnectIdleRetryMs = config.reconnectMaxBackoffMs;
    }

    return true;
}

std::filesystem::path ResolveExecutablePath() {
    std::error_code error;
    const auto exe = std::filesystem::read_symlink("/proc/self/exe", error);
    if (!error && !exe.empty()) {
        return exe;
    }
    return std::filesystem::current_path() / "mwb_client";
}

std::string ReadAllFromStdin() {
    std::ostringstream buffer;
    buffer << std::cin.rdbuf();
    return buffer.str();
}

std::string EscapePowerShellSingleQuoted(std::string_view value) {
    std::string escaped;
    escaped.reserve(value.size());
    for (const char ch : value) {
        if (ch == '\'') {
            escaped += "''";
            continue;
        }
        escaped.push_back(ch);
    }
    return escaped;
}

std::string SanitizeFileStem(std::string_view value) {
    std::string result;
    result.reserve(value.size());
    for (const unsigned char ch : value) {
        if (std::isalnum(ch) != 0) {
            result.push_back(static_cast<char>(std::tolower(ch)));
        } else if (ch == '-' || ch == '_') {
            result.push_back(static_cast<char>(ch));
        } else if (!result.empty() && result.back() != '-') {
            result.push_back('-');
        }
    }

    while (!result.empty() && result.back() == '-') {
        result.pop_back();
    }

    return result.empty() ? "inputflow" : result;
}

std::optional<std::string> DetectOutboundLocalIpv4(const std::string& host, int port) {
    if (host.empty() || port <= 0 || port > std::numeric_limits<uint16_t>::max()) {
        return std::nullopt;
    }

    struct addrinfo hints {};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    hints.ai_protocol = IPPROTO_UDP;

    const std::string service = std::to_string(port);
    struct addrinfo* results = nullptr;
    if (getaddrinfo(host.c_str(), service.c_str(), &hints, &results) != 0 || results == nullptr) {
        return std::nullopt;
    }

    std::optional<std::string> localIp;
    for (const struct addrinfo* current = results; current != nullptr; current = current->ai_next) {
        if (current->ai_family != AF_INET || current->ai_addr == nullptr) {
            continue;
        }

        const int fd = socket(current->ai_family, current->ai_socktype, current->ai_protocol);
        if (fd < 0) {
            continue;
        }

        if (connect(fd, current->ai_addr, current->ai_addrlen) != 0) {
            close(fd);
            continue;
        }

        sockaddr_in localAddress {};
        socklen_t localAddressLength = sizeof(localAddress);
        if (getsockname(fd, reinterpret_cast<sockaddr*>(&localAddress), &localAddressLength) == 0) {
            char buffer[INET_ADDRSTRLEN] = {};
            if (inet_ntop(AF_INET, &localAddress.sin_addr, buffer, sizeof(buffer)) != nullptr) {
                localIp = std::string(buffer);
                close(fd);
                break;
            }
        }

        close(fd);
    }

    freeaddrinfo(results);
    return localIp;
}

std::optional<std::string> NormalizePeerPosition(std::string_view value) {
    std::string normalized;
    normalized.reserve(value.size());
    for (const unsigned char ch : value) {
        if (std::isalnum(ch) != 0) {
            normalized.push_back(static_cast<char>(std::tolower(ch)));
        }
    }

    if (normalized.empty() || normalized == "auto") {
        return std::string("Auto");
    }
    if (normalized == "topleft") {
        return std::string("TopLeft");
    }
    if (normalized == "topright") {
        return std::string("TopRight");
    }
    if (normalized == "bottomleft") {
        return std::string("BottomLeft");
    }
    if (normalized == "bottomright") {
        return std::string("BottomRight");
    }

    return std::nullopt;
}

std::string RenderWindowsPairScript(const std::string& peerName,
                                    const std::string& peerIp,
                                    const std::string& securityKey,
                                    const std::string& peerPosition) {
    std::ostringstream out;
    out
        << "param([switch]$ClosePowerToys)\n\n"
        << "$ErrorActionPreference = 'Stop'\n"
        << "$PeerName = '" << EscapePowerShellSingleQuoted(peerName) << "'\n"
        << "$PeerIp = '" << EscapePowerShellSingleQuoted(peerIp) << "'\n"
        << "$SecurityKey = '" << EscapePowerShellSingleQuoted(securityKey) << "'\n\n"
        << "$PeerPosition = '" << EscapePowerShellSingleQuoted(peerPosition) << "'\n\n"
        << "function Stop-PowerToysProcesses {\n"
        << "    $names = @('PowerToys', 'PowerToys.MouseWithoutBorders', 'MouseWithoutBorders')\n"
        << "    foreach ($name in $names) {\n"
        << "        Get-Process -Name $name -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue\n"
        << "    }\n"
        << "}\n\n"
        << "function Ensure-ArrayLength {\n"
        << "    param([System.Collections.IList]$List, [int]$Length)\n"
        << "    while ($List.Count -lt $Length) {\n"
        << "        $List.Add('') | Out-Null\n"
        << "    }\n"
        << "}\n\n"
        << "function Parse-MachinePool {\n"
        << "    param([string]$Value)\n"
        << "    $entries = New-Object System.Collections.ArrayList\n"
        << "    if ([string]::IsNullOrWhiteSpace($Value)) {\n"
        << "        return ,$entries\n"
        << "    }\n"
        << "    foreach ($part in ($Value -split ',')) {\n"
        << "        $segments = $part -split ':', 2\n"
        << "        $name = if ($segments.Count -ge 1) { [string]$segments[0] } else { '' }\n"
        << "        $id = if ($segments.Count -ge 2) { [string]$segments[1] } else { '' }\n"
        << "        [void]$entries.Add([pscustomobject]@{ Name = $name; Id = $id })\n"
        << "    }\n"
        << "    return ,$entries\n"
        << "}\n\n"
        << "function Serialize-MachinePool {\n"
        << "    param([System.Collections.IList]$Entries)\n"
        << "    $parts = New-Object 'System.Collections.Generic.List[string]'\n"
        << "    foreach ($entry in $Entries) {\n"
        << "        $parts.Add(([string]$entry.Name + ':' + [string]$entry.Id)) | Out-Null\n"
        << "    }\n"
        << "    while ($parts.Count -lt 4) {\n"
        << "        $parts.Add(':') | Out-Null\n"
        << "    }\n"
        << "    while ($parts.Count -gt 4) {\n"
        << "        $parts.RemoveAt($parts.Count - 1)\n"
        << "    }\n"
        << "    return ($parts -join ',')\n"
        << "}\n\n"
        << "function Upsert-Name2IP {\n"
        << "    param([string]$Value, [string]$Name, [string]$Ip)\n"
        << "    $parts = New-Object System.Collections.ArrayList\n"
        << "    $updated = $false\n"
        << "    foreach ($part in ($Value -split ',')) {\n"
        << "        if ([string]::IsNullOrWhiteSpace($part)) {\n"
        << "            continue\n"
        << "        }\n"
        << "        $segments = $part -split ':', 2\n"
        << "        $entryName = if ($segments.Count -ge 1) { [string]$segments[0] } else { '' }\n"
        << "        if ($entryName.Equals($Name, [System.StringComparison]::OrdinalIgnoreCase)) {\n"
        << "            if (-not $updated) {\n"
        << "                [void]$parts.Add($Name + ':' + $Ip)\n"
        << "                $updated = $true\n"
        << "            }\n"
        << "            continue\n"
        << "        }\n"
        << "        [void]$parts.Add($part)\n"
        << "    }\n"
        << "    if (-not $updated) {\n"
        << "        [void]$parts.Add($Name + ':' + $Ip)\n"
        << "    }\n"
        << "    return ($parts -join ',')\n"
        << "}\n\n"
        << "function Get-PositionSlotIndex {\n"
        << "    param([string]$Position)\n"
        << "    switch ($Position) {\n"
        << "        'TopLeft' { return 0 }\n"
        << "        'TopRight' { return 1 }\n"
        << "        'BottomLeft' { return 2 }\n"
        << "        'BottomRight' { return 3 }\n"
        << "        default { return -1 }\n"
        << "    }\n"
        << "}\n\n"
        << "function Find-MachineSlot {\n"
        << "    param([System.Collections.IList]$Matrix, [string]$Name)\n"
        << "    for ($i = 0; $i -lt $Matrix.Count; $i++) {\n"
        << "        if ([string]$Matrix[$i] -ceq $Name) {\n"
        << "            return $i\n"
        << "        }\n"
        << "    }\n"
        << "    return -1\n"
        << "}\n\n"
        << "function Resolve-PeerSlot {\n"
        << "    param([System.Collections.IList]$Matrix, [int]$SelfSlot, [int]$ExistingPeerSlot, [string]$Position)\n"
        << "    if ($Position -ne 'Auto') {\n"
        << "        return (Get-PositionSlotIndex -Position $Position)\n"
        << "    }\n"
        << "    if ($ExistingPeerSlot -ge 0 -and $ExistingPeerSlot -ne $SelfSlot) {\n"
        << "        return $ExistingPeerSlot\n"
        << "    }\n"
        << "    for ($i = 0; $i -lt $Matrix.Count; $i++) {\n"
        << "        if ($i -eq $SelfSlot) {\n"
        << "            continue\n"
        << "        }\n"
        << "        if ([string]::IsNullOrWhiteSpace([string]$Matrix[$i])) {\n"
        << "            return $i\n"
        << "        }\n"
        << "    }\n"
        << "    if ($SelfSlot -ne 1) {\n"
        << "        return 1\n"
        << "    }\n"
        << "    return 0\n"
        << "}\n\n"
        << "$settingsPath = Join-Path $env:LOCALAPPDATA 'Microsoft\\PowerToys\\MouseWithoutBorders\\settings.json'\n"
        << "if (-not (Test-Path -LiteralPath $settingsPath)) {\n"
        << "    throw 'Mouse Without Borders settings.json was not found. Start PowerToys once before running this helper.'\n"
        << "}\n\n"
        << "if ($ClosePowerToys) {\n"
        << "    Stop-PowerToysProcesses\n"
        << "}\n\n"
        << "$timestamp = Get-Date -Format 'yyyyMMdd-HHmmss'\n"
        << "$backupPath = $settingsPath + '.bak-' + $timestamp\n"
        << "Copy-Item -LiteralPath $settingsPath -Destination $backupPath -Force\n\n"
        << "$jsonText = Get-Content -LiteralPath $settingsPath -Raw -Encoding UTF8\n"
        << "$settings = $jsonText | ConvertFrom-Json\n"
        << "$props = $settings.properties\n\n"
        << "if ($null -eq $props.SecurityKey) {\n"
        << "    $props | Add-Member -NotePropertyName SecurityKey -NotePropertyValue ([pscustomobject]@{ value = $SecurityKey })\n"
        << "} else {\n"
        << "    $props.SecurityKey.value = $SecurityKey\n"
        << "}\n\n"
        << "$matrix = New-Object System.Collections.ArrayList\n"
        << "foreach ($item in $props.MachineMatrixString) {\n"
        << "    [void]$matrix.Add([string]$item)\n"
        << "}\n"
        << "Ensure-ArrayLength -List $matrix -Length 4\n\n"
        << "$selfName = [string]$env:COMPUTERNAME\n"
        << "$selfSlot = Find-MachineSlot -Matrix $matrix -Name $selfName\n"
        << "$existingPeerSlot = Find-MachineSlot -Matrix $matrix -Name $PeerName\n"
        << "if ($selfSlot -lt 0) {\n"
        << "    $selfSlot = 0\n"
        << "}\n"
        << "for ($i = 0; $i -lt $matrix.Count; $i++) {\n"
        << "    $entry = [string]$matrix[$i]\n"
        << "    if ($entry.Equals($selfName, [System.StringComparison]::OrdinalIgnoreCase) -or\n"
        << "        $entry.Equals($PeerName, [System.StringComparison]::OrdinalIgnoreCase)) {\n"
        << "        $matrix[$i] = ''\n"
        << "    }\n"
        << "}\n"
        << "$matrix[$selfSlot] = $selfName\n"
        << "$slot = Resolve-PeerSlot -Matrix $matrix -SelfSlot $selfSlot -ExistingPeerSlot $existingPeerSlot -Position $PeerPosition\n"
        << "if ($slot -lt 0 -or $slot -ge $matrix.Count) {\n"
        << "    throw 'Resolved peer slot is out of range.'\n"
        << "}\n"
        << "if ($slot -eq $selfSlot) {\n"
        << "    throw ('Peer position ' + $PeerPosition + ' collides with the local Windows machine slot.')\n"
        << "}\n"
        << "$displaced = [string]$matrix[$slot]\n"
        << "$matrix[$slot] = $PeerName\n"
        << "if (-not [string]::IsNullOrWhiteSpace($displaced) -and\n"
        << "    -not $displaced.Equals($selfName, [System.StringComparison]::OrdinalIgnoreCase) -and\n"
        << "    -not $displaced.Equals($PeerName, [System.StringComparison]::OrdinalIgnoreCase)) {\n"
        << "    $fallbackSlot = -1\n"
        << "    if ($existingPeerSlot -ge 0 -and $existingPeerSlot -ne $slot -and $existingPeerSlot -ne $selfSlot -and\n"
        << "        [string]::IsNullOrWhiteSpace([string]$matrix[$existingPeerSlot])) {\n"
        << "        $fallbackSlot = $existingPeerSlot\n"
        << "    } else {\n"
        << "        for ($i = 0; $i -lt $matrix.Count; $i++) {\n"
        << "            if ($i -eq $selfSlot -or $i -eq $slot) {\n"
        << "                continue\n"
        << "            }\n"
        << "            if ([string]::IsNullOrWhiteSpace([string]$matrix[$i])) {\n"
        << "                $fallbackSlot = $i\n"
        << "                break\n"
        << "            }\n"
        << "        }\n"
        << "    }\n"
        << "    if ($fallbackSlot -lt 0) {\n"
        << "        throw ('Cannot place ' + $PeerName + ' at ' + $PeerPosition + ' without overwriting existing machine ' + $displaced + '.')\n"
        << "    }\n"
        << "    $matrix[$fallbackSlot] = $displaced\n"
        << "}\n"
        << "$props.MachineMatrixString = @($matrix)\n\n"
        << "$existingPool = ''\n"
        << "if ($null -ne $props.MachinePool -and $null -ne $props.MachinePool.value) {\n"
        << "    $existingPool = [string]$props.MachinePool.value\n"
        << "} elseif ($null -eq $props.MachinePool) {\n"
        << "    $props | Add-Member -NotePropertyName MachinePool -NotePropertyValue ([pscustomobject]@{ value = '' })\n"
        << "}\n\n"
        << "$selfId = 'NONE'\n"
        << "$otherEntries = New-Object System.Collections.ArrayList\n"
        << "foreach ($entry in (Parse-MachinePool -Value $existingPool)) {\n"
        << "    $name = [string]$entry.Name\n"
        << "    $id = [string]$entry.Id\n"
        << "    if ([string]::IsNullOrWhiteSpace($name) -and [string]::IsNullOrWhiteSpace($id)) {\n"
        << "        continue\n"
        << "    }\n"
        << "    if ($name.Equals($selfName, [System.StringComparison]::OrdinalIgnoreCase)) {\n"
        << "        if (-not [string]::IsNullOrWhiteSpace($id)) {\n"
        << "            $selfId = $id\n"
        << "        }\n"
        << "        continue\n"
        << "    }\n"
        << "    if ($name.Equals($PeerName, [System.StringComparison]::OrdinalIgnoreCase)) {\n"
        << "        continue\n"
        << "    }\n"
        << "    if ($otherEntries.Count -lt 2) {\n"
        << "        [void]$otherEntries.Add([pscustomobject]@{ Name = $name; Id = $id })\n"
        << "    }\n"
        << "}\n\n"
        << "$entries = New-Object System.Collections.ArrayList\n"
        << "[void]$entries.Add([pscustomobject]@{ Name = $selfName; Id = $selfId })\n"
        << "[void]$entries.Add([pscustomobject]@{ Name = $PeerName; Id = 'NONE' })\n"
        << "foreach ($entry in $otherEntries) {\n"
        << "    if ($entries.Count -ge 4) {\n"
        << "        break\n"
        << "    }\n"
        << "    [void]$entries.Add($entry)\n"
        << "}\n"
        << "$props.MachinePool.value = Serialize-MachinePool -Entries $entries\n\n"
        << "$existingName2IP = ''\n"
        << "if ($null -ne $props.Name2IP -and $null -ne $props.Name2IP.value) {\n"
        << "    $existingName2IP = [string]$props.Name2IP.value\n"
        << "} elseif ($null -eq $props.Name2IP) {\n"
        << "    $props | Add-Member -NotePropertyName Name2IP -NotePropertyValue ([pscustomobject]@{ value = '' })\n"
        << "}\n"
        << "$props.Name2IP.value = Upsert-Name2IP -Value $existingName2IP -Name $PeerName -Ip $PeerIp\n\n"
        << "$settings | ConvertTo-Json -Depth 16 | Set-Content -LiteralPath $settingsPath -Encoding UTF8\n\n"
        << "Write-Host ('Updated settings: ' + $settingsPath)\n"
        << "Write-Host ('Backup written: ' + $backupPath)\n"
        << "Write-Host ('SecurityKey synchronized for ' + $PeerName)\n"
        << "Write-Host ('PeerPosition: ' + $PeerPosition)\n"
        << "Write-Host ('MachineMatrixString: ' + (($props.MachineMatrixString | ForEach-Object { [string]$_ }) -join ','))\n"
        << "Write-Host ('MachinePool: ' + [string]$props.MachinePool.value)\n"
        << "Write-Host ('Name2IP: ' + [string]$props.Name2IP.value)\n";
    return out.str();
}

int RunClient(const mwb::AppConfig& config,
              const std::filesystem::path& statePath);

bool SetNonBlocking(int fd) {
    const int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0) {
        return false;
    }
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0;
}

std::optional<std::string> ProbeReachableIpv4Host(const std::string& host, int port, int timeoutMs) {
    if (host.empty() || port <= 0 || port > std::numeric_limits<uint16_t>::max()) {
        return std::nullopt;
    }

    struct addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;

    const std::string service = std::to_string(port);
    struct addrinfo* results = nullptr;
    if (getaddrinfo(host.c_str(), service.c_str(), &hints, &results) != 0 || results == nullptr) {
        return std::nullopt;
    }

    std::optional<std::string> reachableIp;
    for (const struct addrinfo* current = results; current != nullptr; current = current->ai_next) {
        if (current->ai_family != AF_INET || current->ai_addr == nullptr) {
            continue;
        }

        const auto* address = reinterpret_cast<const sockaddr_in*>(current->ai_addr);
        char buffer[INET_ADDRSTRLEN] = {};
        if (inet_ntop(AF_INET, &address->sin_addr, buffer, sizeof(buffer)) == nullptr) {
            continue;
        }

        int fd = socket(current->ai_family, current->ai_socktype, current->ai_protocol);
        if (fd < 0) {
            continue;
        }

        if (!SetNonBlocking(fd)) {
            close(fd);
            continue;
        }

        const int connectResult = connect(fd, current->ai_addr, current->ai_addrlen);
        if (connectResult == 0) {
            reachableIp = std::string(buffer);
            close(fd);
            break;
        }

        if (errno != EINPROGRESS && errno != EINTR) {
            close(fd);
            continue;
        }

        struct pollfd descriptor{};
        descriptor.fd = fd;
        descriptor.events = POLLOUT;

        const int pollResult = poll(&descriptor, 1, timeoutMs);
        if (pollResult > 0) {
            int socketError = 0;
            socklen_t socketErrorLength = sizeof(socketError);
            if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &socketError, &socketErrorLength) == 0 &&
                socketError == 0) {
                reachableIp = std::string(buffer);
                close(fd);
                break;
            }
        }

        close(fd);
    }

    freeaddrinfo(results);
    return reachableIp;
}

std::optional<std::string> TryRecoverHostFromKnownPeers(const mwb::AppConfig& config,
                                                        const mwb::AppState& state) {
    const bool configuredHostIsIpv4 = mwb::IsIpv4Literal(config.host);
    if (configuredHostIsIpv4 && ProbeReachableIpv4Host(config.host, config.port, 200).has_value()) {
        return std::nullopt;
    }

    for (const auto& host : mwb::CollectRecoveryCandidateHosts(state, config.host, config.port)) {
        if (auto reachable = ProbeReachableIpv4Host(host, config.port, 250)) {
            std::cout << "[RECOVERY] Configured peer " << config.host
                      << " is unavailable; reusing verified peer address "
                      << *reachable << std::endl;
            return reachable;
        }
    }

    return std::nullopt;
}

std::int64_t CurrentEpochSeconds() {
    return std::chrono::duration_cast<std::chrono::seconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

bool LoadOptionalState(const std::filesystem::path& statePath, mwb::AppState& state) {
    if (!std::filesystem::exists(statePath)) {
        return true;
    }

    std::string error;
    if (!mwb::LoadAppState(statePath, state, error)) {
        std::cerr << "ERR: " << error << std::endl;
        return false;
    }
    return true;
}

bool SaveStateOrReport(const std::filesystem::path& statePath, const mwb::AppState& state) {
    std::string error;
    if (!mwb::SaveAppState(statePath, state, error)) {
        std::cerr << "ERR: " << error << std::endl;
        return false;
    }
    return true;
}

int RunClient(const mwb::AppConfig& config,
              const std::filesystem::path& statePath) {
    mwb::AppState state;
    if (!LoadOptionalState(statePath, state)) {
        return 1;
    }
    mwb::ClearConnectedPeers(state);
    (void)mwb::EnsureLocalMachineId(state);
    if (!SaveStateOrReport(statePath, state)) {
        return 1;
    }

    mwb::AppConfig runtimeConfig = config;
    if (const auto recoveredHost = TryRecoverHostFromKnownPeers(config, state); recoveredHost.has_value()) {
        runtimeConfig.host = *recoveredHost;
    }

    std::mutex stateMutex;
    mwb::RuntimeOptions options;
    options.host = runtimeConfig.host;
    options.key = runtimeConfig.key;
    options.port = runtimeConfig.port;
    options.clipboardEnabled = runtimeConfig.clipboardEnabled;
    options.clipboardSendEnabled = runtimeConfig.clipboardSendEnabled;
    options.clipboardForcePoll = runtimeConfig.clipboardForcePoll;
    options.clipboardPollMs = runtimeConfig.clipboardPollMs;
    options.autoConnectEnabled = runtimeConfig.autoConnectEnabled;
    options.reconnectInitialBackoffMs = runtimeConfig.reconnectInitialBackoffMs;
    options.reconnectMaxBackoffMs = runtimeConfig.reconnectMaxBackoffMs;
    options.reconnectIdleRetryMs = runtimeConfig.reconnectIdleRetryMs;
    options.screenWidth = runtimeConfig.screenWidth;
    options.screenHeight = runtimeConfig.screenHeight;
    options.mprisMediaKeysEnabled = runtimeConfig.mprisMediaKeysEnabled;
    options.mprisPlayer = runtimeConfig.mprisPlayer;
    options.localMachineId = state.localMachineId;
    options.localMachineName = runtimeConfig.machineName;
    options.debugInputLogging = IsTruthyEnv("MWB_DEBUG_INPUT");
    options.debugKeyLogging = IsTruthyEnv("MWB_DEBUG_KEYS");
    options.debugShortcutLogging = IsTruthyEnv("MWB_DEBUG_SHORTCUTS");
    options.latencyReport = runtimeConfig.latencyReport;
    options.onSessionEstablished = [&](const std::string& host, int port, const std::string& remoteName, uint32_t, uint32_t localMachineId) {
        std::lock_guard<std::mutex> lock(stateMutex);
        mwb::MarkSessionEstablished(state, host, port, remoteName, localMachineId, CurrentEpochSeconds());
        (void)SaveStateOrReport(statePath, state);
    };
    options.onSessionDisconnected = [&]() {
        std::lock_guard<std::mutex> lock(stateMutex);
        mwb::MarkSessionDisconnected(state);
        (void)SaveStateOrReport(statePath, state);
    };

    mwb::ClientRuntime runtime(std::move(options));

    g_stopRequested = false;
    std::signal(SIGINT, HandleStopSignal);
    std::signal(SIGTERM, HandleStopSignal);

    std::thread signalWatcher([&]() {
        while (!g_stopRequested.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        runtime.Stop();
    });

    const int rc = runtime.Run();
    g_stopRequested = true;
    if (signalWatcher.joinable()) {
        signalWatcher.join();
    }
    return rc;
}

int HandleLegacyRun(int argc, char** argv) {
    mwb::AppConfig config;
    std::optional<std::filesystem::path> keyFileBaseDir;
    config.host = argv[1];
    if (!ApplyEnvironmentOverrides(config, keyFileBaseDir)) {
        return 1;
    }
    SetInlineKey(config, keyFileBaseDir, argv[2]);
    if (argc == 4) {
        const auto parsedPort = ParsePositiveInt(argv[3]);
        if (!parsedPort || *parsedPort > std::numeric_limits<uint16_t>::max()) {
            std::cerr << "ERR: Invalid port '" << argv[3] << "'." << std::endl;
            PrintGeneralUsage(std::cerr, argv[0]);
            return 1;
        }
        config.port = *parsedPort;
    }

    std::string error;
    if (!ResolveConfiguredKey(config, keyFileBaseDir, &error)) {
        std::cerr << "ERR: " << error << std::endl;
        return 1;
    }
    return RunClient(config, mwb::DefaultStatePath());
}

int HandleRunCommand(const std::string& binary, const std::vector<std::string>& args) {
    mwb::AppConfig config;
    std::filesystem::path configPath = mwb::DefaultConfigPath();
    std::filesystem::path statePath = mwb::DefaultStatePath();
    std::optional<std::filesystem::path> keyFileBaseDir;
    bool explicitConfig = false;

    for (std::size_t index = 0; index < args.size(); ++index) {
        const std::string& arg = args[index];
        auto requireValue = [&](const char* flag) -> std::optional<std::string> {
            if (index + 1 >= args.size()) {
                std::cerr << "ERR: Missing value for " << flag << "." << std::endl;
                return std::nullopt;
            }
            return args[++index];
        };

        if (arg == "--config") {
            const auto value = requireValue("--config");
            if (!value) {
                return 1;
            }
            configPath = *value;
            explicitConfig = true;
        } else if (arg == "--state") {
            const auto value = requireValue("--state");
            if (!value) {
                return 1;
            }
            statePath = *value;
        }
    }

    if (std::filesystem::exists(configPath)) {
        std::string error;
        if (!mwb::LoadConfigFile(configPath, config, error)) {
            std::cerr << "ERR: " << error << std::endl;
            return 1;
        }
        if (!config.keyFile.empty()) {
            keyFileBaseDir = configPath.parent_path();
        }
    } else if (explicitConfig) {
        std::cerr << "ERR: Config file not found: " << configPath << std::endl;
        return 1;
    }

    if (!ApplyEnvironmentOverrides(config, keyFileBaseDir)) {
        return 1;
    }

    for (std::size_t index = 0; index < args.size(); ++index) {
        const std::string& arg = args[index];
        auto requireValue = [&](const char* flag) -> std::optional<std::string> {
            if (index + 1 >= args.size()) {
                std::cerr << "ERR: Missing value for " << flag << "." << std::endl;
                return std::nullopt;
            }
            return args[++index];
        };

        if (arg == "--config") {
            ++index;
        } else if (arg == "--state") {
            ++index;
        } else if (arg == "--host") {
            const auto value = requireValue("--host");
            if (!value) {
                return 1;
            }
            config.host = *value;
        } else if (arg == "--key") {
            const auto value = requireValue("--key");
            if (!value) {
                return 1;
            }
            SetInlineKey(config, keyFileBaseDir, *value);
        } else if (arg == "--key-file") {
            const auto value = requireValue("--key-file");
            if (!value) {
                return 1;
            }
            SetKeyFile(config, keyFileBaseDir, *value);
        } else if (arg == "--key-secret-id") {
            const auto value = requireValue("--key-secret-id");
            if (!value) {
                return 1;
            }
            SetKeySecretId(config, keyFileBaseDir, *value);
        } else if (arg == "--name") {
            const auto value = requireValue("--name");
            if (!value) {
                return 1;
            }
            config.machineName = *value;
        } else if (arg == "--port") {
            const auto value = requireValue("--port");
            if (!value) {
                return 1;
            }
            const auto parsed = ParsePositiveInt(*value);
            if (!parsed || *parsed > std::numeric_limits<uint16_t>::max()) {
                std::cerr << "ERR: Invalid --port value." << std::endl;
                return 1;
            }
            config.port = *parsed;
        } else if (arg == "--enable-clipboard") {
            config.clipboardEnabled = true;
            config.clipboardSendEnabled = true;
        } else if (arg == "--disable-clipboard") {
            config.clipboardEnabled = false;
        } else if (arg == "--clipboard-receive-only") {
            config.clipboardEnabled = true;
            config.clipboardSendEnabled = false;
        } else if (arg == "--clipboard-full") {
            config.clipboardEnabled = true;
            config.clipboardSendEnabled = true;
        } else if (arg == "--clipboard-force-poll") {
            config.clipboardForcePoll = true;
        } else if (arg == "--enable-mpris-media-keys") {
            config.mprisMediaKeysEnabled = true;
        } else if (arg == "--disable-mpris-media-keys") {
            config.mprisMediaKeysEnabled = false;
        } else if (arg == "--mpris-player") {
            const auto value = requireValue("--mpris-player");
            if (!value) {
                return 1;
            }
            config.mprisPlayer = *value;
        } else if (arg == "--auto-connect") {
            config.autoConnectEnabled = true;
        } else if (arg == "--manual-only") {
            config.autoConnectEnabled = false;
        } else if (arg == "--clipboard-poll-ms") {
            const auto value = requireValue("--clipboard-poll-ms");
            if (!value) {
                return 1;
            }
            const auto parsed = ParsePositiveInt(*value);
            if (!parsed) {
                std::cerr << "ERR: Invalid --clipboard-poll-ms value." << std::endl;
                return 1;
            }
            config.clipboardPollMs = *parsed;
        } else if (arg == "--screen-width") {
            const auto value = requireValue("--screen-width");
            if (!value) {
                return 1;
            }
            const auto parsed = ParsePositiveInt(*value);
            if (!parsed) {
                std::cerr << "ERR: Invalid --screen-width value." << std::endl;
                return 1;
            }
            config.screenWidth = *parsed;
        } else if (arg == "--screen-height") {
            const auto value = requireValue("--screen-height");
            if (!value) {
                return 1;
            }
            const auto parsed = ParsePositiveInt(*value);
            if (!parsed) {
                std::cerr << "ERR: Invalid --screen-height value." << std::endl;
                return 1;
            }
            config.screenHeight = *parsed;
        } else if (arg == "--reconnect-initial-backoff-ms") {
            const auto value = requireValue("--reconnect-initial-backoff-ms");
            if (!value) {
                return 1;
            }
            const auto parsed = ParsePositiveInt(*value);
            if (!parsed || *parsed < 100) {
                std::cerr << "ERR: Invalid --reconnect-initial-backoff-ms value." << std::endl;
                return 1;
            }
            config.reconnectInitialBackoffMs = *parsed;
        } else if (arg == "--reconnect-max-backoff-ms") {
            const auto value = requireValue("--reconnect-max-backoff-ms");
            if (!value) {
                return 1;
            }
            const auto parsed = ParsePositiveInt(*value);
            if (!parsed || *parsed < 100) {
                std::cerr << "ERR: Invalid --reconnect-max-backoff-ms value." << std::endl;
                return 1;
            }
            config.reconnectMaxBackoffMs = *parsed;
        } else if (arg == "--reconnect-idle-retry-ms") {
            const auto value = requireValue("--reconnect-idle-retry-ms");
            if (!value) {
                return 1;
            }
            const auto parsed = ParsePositiveInt(*value);
            if (!parsed || *parsed < 100) {
                std::cerr << "ERR: Invalid --reconnect-idle-retry-ms value." << std::endl;
                return 1;
            }
            config.reconnectIdleRetryMs = *parsed;
        } else if (arg == "--latency-report") {
            config.latencyReport = true;
        } else {
            std::cerr << "ERR: Unknown run option: " << arg << std::endl;
            std::cerr << "Use `" << binary << " run --host <IP> --key <KEY>`, `"
                      << binary << " run --host <IP> --key-file <PATH>`, `"
                      << binary << " run --host <IP> --key-secret-id <ID>`, or `"
                      << binary << " <IP> <KEY>`." << std::endl;
            return 1;
        }
    }

    if (config.reconnectMaxBackoffMs < config.reconnectInitialBackoffMs) {
        config.reconnectMaxBackoffMs = config.reconnectInitialBackoffMs;
    }
    if (config.reconnectIdleRetryMs < config.reconnectMaxBackoffMs) {
        config.reconnectIdleRetryMs = config.reconnectMaxBackoffMs;
    }

    std::string keyError;
    if (!ResolveConfiguredKey(config, keyFileBaseDir, &keyError)) {
        std::cerr << "ERR: " << keyError << std::endl;
        return 1;
    }

    if (config.host.empty() || config.key.empty()) {
        std::cerr << "ERR: Both host and a security key are required. Use --key, --key-file, --key-secret-id, or a config file." << std::endl;
        return 1;
    }

    return RunClient(config, statePath);
}

int HandleDiscoverCommand(const std::string& binary, const std::vector<std::string>& args) {
    mwb::DiscoveryOptions options;
    std::filesystem::path statePath = mwb::DefaultStatePath();
    for (std::size_t index = 0; index < args.size(); ++index) {
        const std::string& arg = args[index];
        auto requireValue = [&](const char* flag) -> std::optional<std::string> {
            if (index + 1 >= args.size()) {
                std::cerr << "ERR: Missing value for " << flag << "." << std::endl;
                return std::nullopt;
            }
            return args[++index];
        };

        if (arg == "--state") {
            const auto value = requireValue("--state");
            if (!value) {
                return 1;
            }
            statePath = *value;
        } else if (arg == "--port") {
            const auto value = requireValue("--port");
            if (!value) {
                return 1;
            }
            const auto parsed = ParsePositiveInt(*value);
            if (!parsed || *parsed > std::numeric_limits<uint16_t>::max()) {
                std::cerr << "ERR: Invalid --port value." << std::endl;
                return 1;
            }
            options.port = *parsed;
        } else if (arg == "--timeout-ms") {
            const auto value = requireValue("--timeout-ms");
            if (!value) {
                return 1;
            }
            const auto parsed = ParsePositiveInt(*value);
            if (!parsed) {
                std::cerr << "ERR: Invalid --timeout-ms value." << std::endl;
                return 1;
            }
            options.connectTimeoutMs = *parsed;
        } else if (arg == "--max-hosts") {
            const auto value = requireValue("--max-hosts");
            if (!value) {
                return 1;
            }
            const auto parsed = ParsePositiveInt(*value);
            if (!parsed) {
                std::cerr << "ERR: Invalid --max-hosts value." << std::endl;
                return 1;
            }
            options.maxHostsPerSubnet = static_cast<std::size_t>(*parsed);
        } else {
            std::cerr << "ERR: Unknown discover option: " << arg << std::endl;
            return 1;
        }
    }

    std::cout << "[DISCOVERY] Scanning directly connected private IPv4 networks only." << std::endl;
    const auto candidates = mwb::DiscoverLanCandidates(options);

    mwb::AppState state;
    if (!LoadOptionalState(statePath, state)) {
        return 1;
    }
    bool stateChanged = false;
    for (const auto& candidate : candidates) {
        if (candidate.status != mwb::DiscoveryStatus::Open) {
            continue;
        }

        mwb::PeerState peer;
        peer.host = candidate.ipAddress;
        peer.port = options.port;
        if (candidate.hostNameVerified) {
            peer.name = candidate.hostName;
        }
        peer.approved = false;
        peer.lastSeenEpochSeconds = CurrentEpochSeconds();
        peer.lastConnectedEpochSeconds = 0;
        mwb::UpsertPeerState(state, peer);
        stateChanged = true;
    }
    if (stateChanged) {
        (void)SaveStateOrReport(statePath, state);
    }

    if (candidates.empty()) {
        std::cout << "[DISCOVERY] No MWB candidates found on TCP port " << options.port << "." << std::endl;
        if (options.maxHostsPerSubnet < 256) {
            std::cout << "[DISCOVERY] Scan was capped at " << options.maxHostsPerSubnet
                      << " hosts per subnet; try --max-hosts 256 for a typical /24 LAN." << std::endl;
        }
        return 0;
    }

    std::size_t openCount = 0;
    for (const auto& candidate : candidates) {
        if (candidate.status == mwb::DiscoveryStatus::Open) {
            ++openCount;
        }
    }

    if (openCount == 0) {
        std::cout << "[DISCOVERY] No reachable candidates found on TCP port " << options.port << "." << std::endl;
        if (options.maxHostsPerSubnet < 256) {
            std::cout << "[DISCOVERY] Scan was capped at " << options.maxHostsPerSubnet
                      << " hosts per subnet; try --max-hosts 256 for a typical /24 LAN." << std::endl;
        }
        return 0;
    }

    std::cout << "[DISCOVERY] Found " << openCount << " reachable candidate(s):" << std::endl;
    for (const auto& candidate : candidates) {
        if (candidate.status != mwb::DiscoveryStatus::Open) {
            continue;
        }

        std::cout << "  " << candidate.ipAddress
                  << "  name="
                  << (candidate.hostName.empty()
                          ? "(unknown)"
                          : (candidate.hostNameVerified ? candidate.hostName : candidate.hostName + " (unverified)"))
                  << "  iface=" << candidate.interfaceName;
        std::cout << std::endl;
    }

    std::cout << "[DISCOVERY] Discovery does not trust peers or use the security key." << std::endl;
    std::cout << "[DISCOVERY] Connect explicitly with: " << binary
              << " run --host <IP> --key <SECURITY_KEY>, --key-file <PATH>, or --key-secret-id <ID>" << std::endl;
    return 0;
}

int HandleDoctorCommand(const std::vector<std::string>& args) {
    std::filesystem::path configPath = mwb::DefaultConfigPath();

    for (std::size_t index = 0; index < args.size(); ++index) {
        const std::string& arg = args[index];
        auto requireValue = [&](const char* flag) -> std::optional<std::string> {
            if (index + 1 >= args.size()) {
                std::cerr << "ERR: Missing value for " << flag << "." << std::endl;
                return std::nullopt;
            }
            return args[++index];
        };

        if (arg == "--config") {
            const auto value = requireValue("--config");
            if (!value) {
                return 1;
            }
            configPath = *value;
        } else {
            std::cerr << "ERR: Unknown doctor option: " << arg << std::endl;
            return 1;
        }
    }

    std::cout << "InputFlow doctor" << std::endl;
    PrintDoctorLine("INFO", "os", OsPrettyName().value_or("unknown"));
    PrintDoctorLine("INFO", "kernel", KernelRelease());
    if (const auto enforce = ReadFirstLineFile("/sys/fs/selinux/enforce")) {
        PrintDoctorLine("INFO", "selinux", *enforce == "1" ? "enforcing" : "permissive");
    } else {
        PrintDoctorLine("INFO", "selinux", "not detected");
    }

    mwb::AppConfig config;
    std::string error;
    bool hasError = false;
    const bool configExists = std::filesystem::exists(configPath);
    if (!configExists) {
        PrintDoctorLine("WARN", "config", configPath.string() + " does not exist; run init-config or install-user-service");
    } else if (!mwb::LoadConfigFile(configPath, config, error)) {
        PrintDoctorLine("ERR", "config", error);
        hasError = true;
    } else {
        PrintDoctorLine("OK", "config", configPath.string());
        PrintDoctorLine(config.host.empty() ? "WARN" : "OK", "host", config.host.empty() ? "not configured" : config.host);
        PrintDoctorLine("INFO", "machine name", config.machineName.empty() ? "<auto>" : config.machineName);
        PrintDoctorLine("INFO", "port", std::to_string(config.port));
        PrintDoctorLine("INFO", "clipboard", std::string(config.clipboardEnabled ? "enabled" : "disabled") +
            (config.clipboardSendEnabled ? ", send enabled" : ", receive-only"));
        PrintDoctorLine("INFO", "reconnect", "initial=" + std::to_string(config.reconnectInitialBackoffMs) +
            "ms max=" + std::to_string(config.reconnectMaxBackoffMs) +
            "ms idle=" + std::to_string(config.reconnectIdleRetryMs) + "ms");

        if (!config.keySecretId.empty()) {
            PrintDoctorLine("OK", "key source", "Secret Service id '" + config.keySecretId + "'");
        } else if (!config.keyFile.empty()) {
            std::filesystem::path keyPath = config.keyFile;
            if (keyPath.is_relative()) {
                keyPath = configPath.parent_path() / keyPath;
            }
            PrintDoctorLine(std::filesystem::exists(keyPath) ? "OK" : "WARN", "key file", keyPath.string());
        } else if (!config.key.empty()) {
            PrintDoctorLine("WARN", "key source", "inline key configured; prefer key_file or key_secret_id");
        } else {
            PrintDoctorLine("WARN", "key source", "not configured");
        }

        if (config.screenWidth && config.screenHeight) {
            PrintDoctorLine("OK", "screen override", std::to_string(*config.screenWidth) + "x" + std::to_string(*config.screenHeight));
        } else {
            PrintDoctorLine("INFO", "screen override", "not configured; /sys/class/drm autodetection will be used");
        }
    }

    PrintDoctorLine("INFO", "drm", DrmSummary());

    const std::filesystem::path uinputPath("/dev/uinput");
    PrintDoctorLine(std::filesystem::exists("/sys/module/uinput") ? "OK" : "WARN", "uinput module",
        std::filesystem::exists("/sys/module/uinput") ? "loaded" : "not loaded");
    if (!std::filesystem::exists(uinputPath)) {
        PrintDoctorLine("WARN", "uinput", "/dev/uinput missing; load the uinput kernel module");
    } else {
        struct stat uinputStat {};
        const bool haveStat = stat(uinputPath.c_str(), &uinputStat) == 0;
        const std::string detail = haveStat
            ? "/dev/uinput group=" + GroupName(uinputStat.st_gid) + " mode=" + FormatMode(uinputStat.st_mode)
            : "/dev/uinput exists";
        PrintDoctorLine(access(uinputPath.c_str(), W_OK) == 0 ? "OK" : "WARN", "uinput", detail);
    }

    if (getgrnam("inputflow") == nullptr) {
        PrintDoctorLine("INFO", "inputflow group", "not present; install packaging/sysusers setup or create the group manually");
    } else if (CurrentUserInGroup("inputflow")) {
        PrintDoctorLine("OK", "inputflow group", "current user is a member");
    } else {
        PrintDoctorLine("WARN", "inputflow group", "current user is not a member");
    }

    const auto printPackagedFile = [](const char* label, const char* path) {
        const bool installed = std::filesystem::exists(path);
        PrintDoctorLine(installed ? "OK" : "INFO", label,
            std::string(installed ? "installed: " : "not installed: ") + path);
    };
    printPackagedFile("packaged sysusers", "/usr/lib/sysusers.d/mwb-client.conf");
    printPackagedFile("packaged modules-load", "/usr/lib/modules-load.d/mwb-client-uinput.conf");
    printPackagedFile("packaged udev rule", "/usr/lib/udev/rules.d/70-mwb-client-uinput.rules");

    const char* sessionType = std::getenv("XDG_SESSION_TYPE");
    if (sessionType != nullptr && *sessionType != '\0') {
        PrintDoctorLine("INFO", "session", sessionType);
    } else if (std::getenv("WAYLAND_DISPLAY") != nullptr) {
        PrintDoctorLine("INFO", "session", "wayland");
    } else if (std::getenv("DISPLAY") != nullptr) {
        PrintDoctorLine("INFO", "session", "x11");
    } else {
        PrintDoctorLine("WARN", "session", "no Wayland or X11 session variables detected");
    }

    if (const char* desktop = std::getenv("XDG_CURRENT_DESKTOP"); desktop != nullptr && *desktop != '\0') {
        PrintDoctorLine("INFO", "desktop", desktop);
    }
    PrintDoctorLine("INFO", "desktop env", "DESKTOP_SESSION=" + EnvValueOrUnset("DESKTOP_SESSION") +
        " WAYLAND_DISPLAY=" + EnvValueOrUnset("WAYLAND_DISPLAY") +
        " DISPLAY=" + EnvValueOrUnset("DISPLAY"));
    PrintDoctorLine("INFO", "runtime env", "XDG_RUNTIME_DIR=" + EnvValueOrUnset("XDG_RUNTIME_DIR") +
        " DBUS_SESSION_BUS_ADDRESS=" + EnvValueOrUnset("DBUS_SESSION_BUS_ADDRESS"));

    if (FindExecutableInPath("wl-copy") && FindExecutableInPath("wl-paste")) {
        PrintDoctorLine("OK", "clipboard helpers", "wl-clipboard");
    } else if (FindExecutableInPath("xclip")) {
        PrintDoctorLine("OK", "clipboard helpers", "xclip");
    } else if (FindExecutableInPath("xsel")) {
        PrintDoctorLine("OK", "clipboard helpers", "xsel");
    } else {
        PrintDoctorLine("WARN", "clipboard helpers", "install wl-clipboard, xclip, or xsel for clipboard sync");
    }

    const char* runtimeDir = std::getenv("XDG_RUNTIME_DIR");
    if (runtimeDir == nullptr || *runtimeDir == '\0' || !std::filesystem::exists(std::filesystem::path(runtimeDir) / "bus")) {
        PrintDoctorLine("WARN", "session bus", "D-Bus user session bus not detected");
    } else {
        PrintDoctorLine("OK", "session bus", (std::filesystem::path(runtimeDir) / "bus").string());
    }

    if (!FindExecutableInPath("busctl")) {
        PrintDoctorLine("INFO", "xdg portal", "busctl unavailable; portal service not checked");
    } else if (CommandSucceeds("busctl --user --no-pager status org.freedesktop.portal.Desktop")) {
        PrintDoctorLine("OK", "xdg portal", "org.freedesktop.portal.Desktop is reachable");
    } else if (FileExistsAny({"/usr/libexec/xdg-desktop-portal", "/usr/lib/xdg-desktop-portal"})) {
        PrintDoctorLine("WARN", "xdg portal", "portal executable is installed but the user service is not reachable");
    } else {
        PrintDoctorLine("INFO", "xdg portal", "portal service not detected");
    }

    const std::filesystem::path unitPath = DefaultUserServicePath();
    PrintDoctorLine(std::filesystem::exists(unitPath) ? "OK" : "INFO", "user service", unitPath.string());
    if (FindExecutableInPath("systemctl")) {
        if (!CommandSucceeds("systemctl --user show-environment")) {
            PrintDoctorLine("WARN", "service state", "systemctl --user is unavailable in this environment");
        } else if (CommandSucceeds("systemctl --user is-active --quiet mwb-client.service")) {
            PrintDoctorLine("OK", "service state", "mwb-client.service is active");
        } else if (CommandSucceeds("systemctl --user is-enabled --quiet mwb-client.service")) {
            PrintDoctorLine("INFO", "service state", "mwb-client.service is enabled but not active");
        } else {
            PrintDoctorLine("INFO", "service state", "mwb-client.service is not active/enabled for this user");
        }
    } else {
        PrintDoctorLine("INFO", "service state", "systemctl unavailable");
    }

    return hasError ? 1 : 0;
}

int HandleInitConfigCommand(const std::vector<std::string>& args) {
    mwb::AppConfig config;
    std::filesystem::path configPath = mwb::DefaultConfigPath();
    std::optional<std::filesystem::path> keyFileBaseDir;
    bool force = false;

    for (std::size_t index = 0; index < args.size(); ++index) {
        const std::string& arg = args[index];
        auto requireValue = [&](const char* flag) -> std::optional<std::string> {
            if (index + 1 >= args.size()) {
                std::cerr << "ERR: Missing value for " << flag << "." << std::endl;
                return std::nullopt;
            }
            return args[++index];
        };

        if (arg == "--config") {
            const auto value = requireValue("--config");
            if (!value) {
                return 1;
            }
            configPath = *value;
        } else if (arg == "--force") {
            force = true;
        } else if (arg == "--host") {
            const auto value = requireValue("--host");
            if (!value) {
                return 1;
            }
            config.host = *value;
        } else if (arg == "--key") {
            const auto value = requireValue("--key");
            if (!value) {
                return 1;
            }
            SetInlineKey(config, keyFileBaseDir, *value);
        } else if (arg == "--key-file") {
            const auto value = requireValue("--key-file");
            if (!value) {
                return 1;
            }
            SetKeyFile(config, keyFileBaseDir, *value);
        } else if (arg == "--key-secret-id") {
            const auto value = requireValue("--key-secret-id");
            if (!value) {
                return 1;
            }
            SetKeySecretId(config, keyFileBaseDir, *value);
        } else if (arg == "--name") {
            const auto value = requireValue("--name");
            if (!value) {
                return 1;
            }
            config.machineName = *value;
        } else if (arg == "--port") {
            const auto value = requireValue("--port");
            if (!value) {
                return 1;
            }
            const auto parsed = ParsePositiveInt(*value);
            if (!parsed || *parsed > std::numeric_limits<uint16_t>::max()) {
                std::cerr << "ERR: Invalid --port value." << std::endl;
                return 1;
            }
            config.port = *parsed;
        } else if (arg == "--auto-connect") {
            config.autoConnectEnabled = true;
        } else if (arg == "--manual-only") {
            config.autoConnectEnabled = false;
        } else if (arg == "--enable-mpris-media-keys") {
            config.mprisMediaKeysEnabled = true;
        } else if (arg == "--disable-mpris-media-keys") {
            config.mprisMediaKeysEnabled = false;
        } else if (arg == "--mpris-player") {
            const auto value = requireValue("--mpris-player");
            if (!value) {
                return 1;
            }
            config.mprisPlayer = *value;
        } else if (arg == "--reconnect-initial-backoff-ms") {
            const auto value = requireValue("--reconnect-initial-backoff-ms");
            if (!value) {
                return 1;
            }
            const auto parsed = ParsePositiveInt(*value);
            if (!parsed || *parsed < 100) {
                std::cerr << "ERR: Invalid --reconnect-initial-backoff-ms value." << std::endl;
                return 1;
            }
            config.reconnectInitialBackoffMs = *parsed;
        } else if (arg == "--reconnect-max-backoff-ms") {
            const auto value = requireValue("--reconnect-max-backoff-ms");
            if (!value) {
                return 1;
            }
            const auto parsed = ParsePositiveInt(*value);
            if (!parsed || *parsed < 100) {
                std::cerr << "ERR: Invalid --reconnect-max-backoff-ms value." << std::endl;
                return 1;
            }
            config.reconnectMaxBackoffMs = *parsed;
        } else if (arg == "--reconnect-idle-retry-ms") {
            const auto value = requireValue("--reconnect-idle-retry-ms");
            if (!value) {
                return 1;
            }
            const auto parsed = ParsePositiveInt(*value);
            if (!parsed || *parsed < 100) {
                std::cerr << "ERR: Invalid --reconnect-idle-retry-ms value." << std::endl;
                return 1;
            }
            config.reconnectIdleRetryMs = *parsed;
        } else {
            std::cerr << "ERR: Unknown init-config option: " << arg << std::endl;
            return 1;
        }
    }

    if (config.reconnectMaxBackoffMs < config.reconnectInitialBackoffMs) {
        config.reconnectMaxBackoffMs = config.reconnectInitialBackoffMs;
    }
    if (config.reconnectIdleRetryMs < config.reconnectMaxBackoffMs) {
        config.reconnectIdleRetryMs = config.reconnectMaxBackoffMs;
    }

    if (std::filesystem::exists(configPath) && !force) {
        std::cerr << "ERR: Config already exists: " << configPath << ". Use --force to overwrite." << std::endl;
        return 1;
    }

    std::string error;
    if (!mwb::SaveConfigFile(configPath, config, error)) {
        std::cerr << "ERR: " << error << std::endl;
        return 1;
    }

    std::cout << "Wrote config template to " << configPath << std::endl;
    return 0;
}

int HandleExportWindowsPairCommand(const std::vector<std::string>& args) {
    mwb::AppConfig loadedConfig;
    mwb::AppConfig config;
    std::filesystem::path configPath = mwb::DefaultConfigPath();
    std::optional<std::filesystem::path> keyFileBaseDir;
    std::filesystem::path outputPath;
    bool force = false;
    bool outputRequested = false;
    std::optional<std::string> linuxIpOverride;
    std::string peerPosition = "Auto";

    for (std::size_t index = 0; index < args.size(); ++index) {
        const std::string& arg = args[index];
        auto requireValue = [&](const char* flag) -> std::optional<std::string> {
            if (index + 1 >= args.size()) {
                std::cerr << "ERR: Missing value for " << flag << "." << std::endl;
                return std::nullopt;
            }
            return args[++index];
        };

        if (arg == "--config") {
            const auto value = requireValue("--config");
            if (!value) {
                return 1;
            }
            configPath = *value;
        }
    }

    if (std::filesystem::exists(configPath)) {
        std::string error;
        if (!mwb::LoadConfigFile(configPath, loadedConfig, error)) {
            std::cerr << "ERR: " << error << std::endl;
            return 1;
        }
        config = loadedConfig;
        if (!config.keyFile.empty()) {
            keyFileBaseDir = configPath.parent_path();
        }
    }

    for (std::size_t index = 0; index < args.size(); ++index) {
        const std::string& arg = args[index];
        auto requireValue = [&](const char* flag) -> std::optional<std::string> {
            if (index + 1 >= args.size()) {
                std::cerr << "ERR: Missing value for " << flag << "." << std::endl;
                return std::nullopt;
            }
            return args[++index];
        };

        if (arg == "--config") {
            ++index;
        } else if (arg == "--output") {
            const auto value = requireValue("--output");
            if (!value) {
                return 1;
            }
            outputRequested = true;
            outputPath = *value;
        } else if (arg == "--force") {
            force = true;
        } else if (arg == "--linux-ip") {
            const auto value = requireValue("--linux-ip");
            if (!value) {
                return 1;
            }
            linuxIpOverride = *value;
        } else if (arg == "--position") {
            const auto value = requireValue("--position");
            if (!value) {
                return 1;
            }
            const auto normalized = NormalizePeerPosition(*value);
            if (!normalized.has_value()) {
                std::cerr << "ERR: Invalid --position value. Use auto, top-left, top-right, bottom-left, or bottom-right." << std::endl;
                return 1;
            }
            peerPosition = *normalized;
        } else if (arg == "--key") {
            const auto value = requireValue("--key");
            if (!value) {
                return 1;
            }
            SetInlineKey(config, keyFileBaseDir, *value);
        } else if (arg == "--key-file") {
            const auto value = requireValue("--key-file");
            if (!value) {
                return 1;
            }
            SetKeyFile(config, keyFileBaseDir, *value);
        } else if (arg == "--key-secret-id") {
            const auto value = requireValue("--key-secret-id");
            if (!value) {
                return 1;
            }
            SetKeySecretId(config, keyFileBaseDir, *value);
        } else if (arg == "--name") {
            const auto value = requireValue("--name");
            if (!value) {
                return 1;
            }
            config.machineName = *value;
        } else {
            std::cerr << "ERR: Unknown export-windows-pair option: " << arg << std::endl;
            return 1;
        }
    }

    if (config.machineName.empty()) {
        std::cerr << "ERR: export-windows-pair requires a machine name via config or --name." << std::endl;
        return 1;
    }

    std::string error;
    if (!ResolveConfiguredKey(config, keyFileBaseDir, &error)) {
        std::cerr << "ERR: " << error << std::endl;
        return 1;
    }
    if (config.key.empty()) {
        std::cerr << "ERR: export-windows-pair requires a security key via config, --key, --key-file, or --key-secret-id." << std::endl;
        return 1;
    }

    std::string linuxIp;
    if (linuxIpOverride.has_value()) {
        linuxIp = *linuxIpOverride;
    } else if (const auto detected = DetectOutboundLocalIpv4(config.host, config.port); detected.has_value()) {
        linuxIp = *detected;
    } else {
        std::cerr << "ERR: Failed to detect the Linux IPv4 address for host '" << config.host
                  << "'. Pass --linux-ip explicitly." << std::endl;
        return 1;
    }

    if (!outputRequested) {
        outputPath = std::filesystem::current_path() /
                     ("inputflow-windows-pair-" + SanitizeFileStem(config.machineName) + ".ps1");
    }

    if (std::filesystem::exists(outputPath) && !force) {
        std::cerr << "ERR: Output file already exists: " << outputPath << ". Use --force to overwrite." << std::endl;
        return 1;
    }

    const std::string script = RenderWindowsPairScript(config.machineName, linuxIp, config.key, peerPosition);

    try {
        const std::filesystem::path parent = outputPath.parent_path();
        if (!parent.empty()) {
            std::filesystem::create_directories(parent);
        }

        std::ofstream file(outputPath, std::ios::out | std::ios::trunc);
        if (!file) {
            std::cerr << "ERR: Failed to open output file for writing: " << outputPath << std::endl;
            return 1;
        }
        file << script;
        if (!file) {
            std::cerr << "ERR: Failed to write output file: " << outputPath << std::endl;
            return 1;
        }
        file.close();

        std::error_code permissionError;
        std::filesystem::permissions(
            outputPath,
            std::filesystem::perms::owner_read | std::filesystem::perms::owner_write,
            std::filesystem::perm_options::replace,
            permissionError);
        if (permissionError) {
            std::cerr << "ERR: Failed to secure output file permissions for '" << outputPath
                      << "': " << permissionError.message() << std::endl;
            return 1;
        }
    } catch (const std::exception& ex) {
        std::cerr << "ERR: Failed to write " << outputPath << ": " << ex.what() << std::endl;
        return 1;
    }

    std::cout << "Wrote Windows pairing helper to " << outputPath << std::endl;
    std::cout << "Run on Windows:" << std::endl;
    std::cout << "  powershell -ExecutionPolicy Bypass -File .\\"
              << outputPath.filename().string() << " -ClosePowerToys" << std::endl;
    std::cout << "This helper synchronizes the shared key plus MachineMatrixString, MachinePool, and Name2IP for '"
              << config.machineName << "'." << std::endl;
    std::cout << "Configured peer position: " << peerPosition << std::endl;
    return 0;
}

int HandleSecretStoreCommand(const std::vector<std::string>& args) {
    mwb::AppConfig loadedConfig;
    mwb::AppConfig sourceConfig;
    std::filesystem::path configPath;
    std::optional<std::filesystem::path> keyFileBaseDir;
    bool configRequested = false;
    bool readSecretFromStdin = false;
    std::string secretId;

    for (std::size_t index = 0; index < args.size(); ++index) {
        const std::string& arg = args[index];
        auto requireValue = [&](const char* flag) -> std::optional<std::string> {
            if (index + 1 >= args.size()) {
                std::cerr << "ERR: Missing value for " << flag << "." << std::endl;
                return std::nullopt;
            }
            return args[++index];
        };

        if (arg == "--config") {
            const auto value = requireValue("--config");
            if (!value) {
                return 1;
            }
            configRequested = true;
            configPath = *value;
        }
    }

    if (configRequested && std::filesystem::exists(configPath)) {
        std::string error;
        if (!mwb::LoadConfigFile(configPath, loadedConfig, error)) {
            std::cerr << "ERR: " << error << std::endl;
            return 1;
        }
        sourceConfig = loadedConfig;
        if (!sourceConfig.keyFile.empty()) {
            keyFileBaseDir = configPath.parent_path();
        }
    }

    for (std::size_t index = 0; index < args.size(); ++index) {
        const std::string& arg = args[index];
        auto requireValue = [&](const char* flag) -> std::optional<std::string> {
            if (index + 1 >= args.size()) {
                std::cerr << "ERR: Missing value for " << flag << "." << std::endl;
                return std::nullopt;
            }
            return args[++index];
        };

        if (arg == "--config") {
            ++index;
        } else if (arg == "--secret-id") {
            const auto value = requireValue("--secret-id");
            if (!value) {
                return 1;
            }
            secretId = *value;
        } else if (arg == "--key") {
            const auto value = requireValue("--key");
            if (!value) {
                return 1;
            }
            SetInlineKey(sourceConfig, keyFileBaseDir, *value);
        } else if (arg == "--key-file") {
            const auto value = requireValue("--key-file");
            if (!value) {
                return 1;
            }
            SetKeyFile(sourceConfig, keyFileBaseDir, *value);
        } else if (arg == "--stdin") {
            readSecretFromStdin = true;
        } else {
            std::cerr << "ERR: Unknown secret-store option: " << arg << std::endl;
            return 1;
        }
    }

    if (secretId.empty()) {
        secretId = loadedConfig.keySecretId;
    }
    if (secretId.empty()) {
        std::cerr << "ERR: secret-store requires --secret-id or a config with key_secret_id." << std::endl;
        return 1;
    }

    if (readSecretFromStdin) {
        SetInlineKey(sourceConfig, keyFileBaseDir, ReadAllFromStdin());
    }

    if (sourceConfig.key.empty() && sourceConfig.keyFile.empty()) {
        std::cerr << "ERR: secret-store requires --key, --key-file, --stdin, or a config with key/key_file." << std::endl;
        return 1;
    }

    std::string error;
    if (!ResolveConfiguredKey(sourceConfig, keyFileBaseDir, &error)) {
        std::cerr << "ERR: " << error << std::endl;
        return 1;
    }

    if (!mwb::StoreSecretKey(secretId, sourceConfig.key, &error)) {
        std::cerr << "ERR: " << error << std::endl;
        return 1;
    }

    std::cout << "Stored security key in the desktop keyring under secret id '" << secretId << "'." << std::endl;

    if (configRequested) {
        mwb::AppConfig configToSave = loadedConfig;
        SetKeySecretId(configToSave, keyFileBaseDir, secretId);
        if (!mwb::SaveConfigFile(configPath, configToSave, error)) {
            std::cerr << "ERR: " << error << std::endl;
            return 1;
        }
        std::cout << "Updated config to use key_secret_id at " << configPath << std::endl;
    }

    return 0;
}

int HandleSecretClearCommand(const std::vector<std::string>& args) {
    mwb::AppConfig loadedConfig;
    std::filesystem::path configPath;
    bool configRequested = false;
    std::string secretId;

    for (std::size_t index = 0; index < args.size(); ++index) {
        const std::string& arg = args[index];
        auto requireValue = [&](const char* flag) -> std::optional<std::string> {
            if (index + 1 >= args.size()) {
                std::cerr << "ERR: Missing value for " << flag << "." << std::endl;
                return std::nullopt;
            }
            return args[++index];
        };

        if (arg == "--config") {
            const auto value = requireValue("--config");
            if (!value) {
                return 1;
            }
            configRequested = true;
            configPath = *value;
        }
    }

    if (configRequested && std::filesystem::exists(configPath)) {
        std::string error;
        if (!mwb::LoadConfigFile(configPath, loadedConfig, error)) {
            std::cerr << "ERR: " << error << std::endl;
            return 1;
        }
    }

    for (std::size_t index = 0; index < args.size(); ++index) {
        const std::string& arg = args[index];
        auto requireValue = [&](const char* flag) -> std::optional<std::string> {
            if (index + 1 >= args.size()) {
                std::cerr << "ERR: Missing value for " << flag << "." << std::endl;
                return std::nullopt;
            }
            return args[++index];
        };

        if (arg == "--config") {
            ++index;
        } else if (arg == "--secret-id") {
            const auto value = requireValue("--secret-id");
            if (!value) {
                return 1;
            }
            secretId = *value;
        } else {
            std::cerr << "ERR: Unknown secret-clear option: " << arg << std::endl;
            return 1;
        }
    }

    if (secretId.empty()) {
        secretId = loadedConfig.keySecretId;
    }
    if (secretId.empty()) {
        std::cerr << "ERR: secret-clear requires --secret-id or a config with key_secret_id." << std::endl;
        return 1;
    }

    std::string error;
    if (!mwb::ClearSecretKey(secretId, &error)) {
        std::cerr << "ERR: " << error << std::endl;
        return 1;
    }

    std::cout << "Cleared desktop keyring entry for secret id '" << secretId << "'." << std::endl;

    if (configRequested && std::filesystem::exists(configPath) && loadedConfig.keySecretId == secretId) {
        loadedConfig.keySecretId.clear();
        if (!mwb::SaveConfigFile(configPath, loadedConfig, error)) {
            std::cerr << "ERR: " << error << std::endl;
            return 1;
        }
        std::cout << "Cleared key_secret_id from " << configPath << std::endl;
    }

    return 0;
}

int HandleInstallUserServiceCommand(const std::vector<std::string>& args) {
    std::filesystem::path configPath = mwb::DefaultConfigPath();
    std::filesystem::path unitPath = DefaultUserServicePath();
    bool force = false;

    for (std::size_t index = 0; index < args.size(); ++index) {
        const std::string& arg = args[index];
        auto requireValue = [&](const char* flag) -> std::optional<std::string> {
            if (index + 1 >= args.size()) {
                std::cerr << "ERR: Missing value for " << flag << "." << std::endl;
                return std::nullopt;
            }
            return args[++index];
        };

        if (arg == "--config") {
            const auto value = requireValue("--config");
            if (!value) {
                return 1;
            }
            configPath = *value;
        } else if (arg == "--unit") {
            const auto value = requireValue("--unit");
            if (!value) {
                return 1;
            }
            unitPath = *value;
        } else if (arg == "--force") {
            force = true;
        } else {
            std::cerr << "ERR: Unknown install-user-service option: " << arg << std::endl;
            return 1;
        }
    }

    if (!std::filesystem::exists(configPath)) {
        std::string error;
        if (!mwb::SaveConfigFile(configPath, mwb::AppConfig{}, error)) {
            std::cerr << "ERR: " << error << std::endl;
            return 1;
        }
        std::cout << "Wrote sample config to " << configPath << std::endl;
    }

    if (std::filesystem::exists(unitPath) && !force) {
        std::cerr << "ERR: Service unit already exists: " << unitPath << ". Use --force to overwrite." << std::endl;
        return 1;
    }

    std::error_code mkdirError;
    std::filesystem::create_directories(unitPath.parent_path(), mkdirError);
    if (mkdirError) {
        std::cerr << "ERR: Failed to create systemd user unit directory: " << mkdirError.message() << std::endl;
        return 1;
    }

    const std::filesystem::path executablePath = ResolveExecutablePath();
    std::ofstream unit(unitPath);
    if (!unit) {
        std::cerr << "ERR: Could not write service unit: " << unitPath << std::endl;
        return 1;
    }

    unit
        << "[Unit]\n"
        << "Description=InputFlow Linux Client\n"
        << "After=network-online.target\n"
        << "Wants=network-online.target\n\n"
        << "[Service]\n"
        << "Type=simple\n"
        << "ExecStart=" << executablePath.string() << " run --config " << configPath.string() << "\n"
        << "Restart=always\n"
        << "RestartSec=3\n\n"
        << "[Install]\n"
        << "WantedBy=default.target\n";

    if (!unit) {
        std::cerr << "ERR: Failed while writing service unit." << std::endl;
        return 1;
    }
    unit.close();

    std::error_code permissionError;
    std::filesystem::permissions(
        unitPath,
        std::filesystem::perms::owner_read |
            std::filesystem::perms::owner_write |
            std::filesystem::perms::group_read |
            std::filesystem::perms::others_read,
        std::filesystem::perm_options::replace,
        permissionError);
    if (permissionError) {
        std::cerr << "ERR: Failed to set service unit permissions: " << permissionError.message() << std::endl;
        return 1;
    }

    std::cout << "Wrote user service to " << unitPath << std::endl;
    std::cout << "Next steps:" << std::endl;
    std::cout << "  1. Edit " << configPath << " and set host plus key, key_file, or key_secret_id." << std::endl;
    std::cout << "  2. systemctl --user daemon-reload" << std::endl;
    std::cout << "  3. systemctl --user enable --now mwb-client.service" << std::endl;
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    std::cout << "--- InputFlow Linux Client (C++17) ---" << std::endl;

    if (argc == 2 && (std::string(argv[1]) == "--help" || std::string(argv[1]) == "-h")) {
        PrintGeneralUsage(std::cout, argv[0]);
        return 0;
    }

    if (argc >= 3 && argc <= 4 && std::string(argv[1]) != "run" &&
        std::string(argv[1]) != "discover" &&
        std::string(argv[1]) != "doctor" &&
        std::string(argv[1]) != "init-config" &&
        std::string(argv[1]) != "export-windows-pair" &&
        std::string(argv[1]) != "install-user-service" &&
        std::string(argv[1]) != "secret-store" &&
        std::string(argv[1]) != "secret-clear" &&
        std::string(argv[1]).rfind("--", 0) != 0) {
        return HandleLegacyRun(argc, argv);
    }

    if (argc < 2) {
        PrintGeneralUsage(std::cerr, argv[0]);
        return 1;
    }

    const std::string command = argv[1];
    const std::vector<std::string> args(argv + 2, argv + argc);
    const std::string binary = std::filesystem::path(argv[0]).filename().string();

    if (command == "run") {
        return HandleRunCommand(binary, args);
    }
    if (command == "discover") {
        return HandleDiscoverCommand(binary, args);
    }
    if (command == "doctor") {
        return HandleDoctorCommand(args);
    }
    if (command == "init-config") {
        return HandleInitConfigCommand(args);
    }
    if (command == "export-windows-pair") {
        return HandleExportWindowsPairCommand(args);
    }
    if (command == "install-user-service") {
        return HandleInstallUserServiceCommand(args);
    }
    if (command == "secret-store") {
        return HandleSecretStoreCommand(args);
    }
    if (command == "secret-clear") {
        return HandleSecretClearCommand(args);
    }

    std::cerr << "ERR: Unknown command: " << command << std::endl;
    PrintGeneralUsage(std::cerr, argv[0]);
    return 1;
}
