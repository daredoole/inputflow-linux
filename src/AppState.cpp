#include "AppState.h"
#include "SecureFile.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <optional>
#include <random>
#include <sstream>

namespace mwb {
namespace {

constexpr const char* kStateDirName = "mwb-client";
constexpr const char* kStateFileName = "state.ini";

std::string Trim(std::string value) {
    const auto notSpace = [](unsigned char ch) { return !std::isspace(ch); };
    const auto begin = std::find_if(value.begin(), value.end(), notSpace);
    if (begin == value.end()) {
        return {};
    }
    const auto end = std::find_if(value.rbegin(), value.rend(), notSpace).base();
    return std::string(begin, end);
}

std::optional<bool> ParseBool(const std::string& value) {
    std::string normalized = value;
    std::transform(normalized.begin(), normalized.end(), normalized.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });

    if (normalized == "1" || normalized == "true" || normalized == "yes" || normalized == "on") {
        return true;
    }
    if (normalized == "0" || normalized == "false" || normalized == "no" || normalized == "off") {
        return false;
    }
    return std::nullopt;
}

std::optional<int> ParsePort(const std::string& value) {
    try {
        size_t end = 0;
        const long parsed = std::stol(value, &end);
        if (end != value.size() || parsed <= 0 || parsed > 65535) {
            return std::nullopt;
        }
        return static_cast<int>(parsed);
    } catch (...) {
        return std::nullopt;
    }
}

std::optional<std::int64_t> ParseInt64(const std::string& value) {
    try {
        size_t end = 0;
        const long long parsed = std::stoll(value, &end);
        if (end != value.size()) {
            return std::nullopt;
        }
        return static_cast<std::int64_t>(parsed);
    } catch (...) {
        return std::nullopt;
    }
}

std::optional<uint32_t> ParseMachineId(const std::string& value) {
    std::string normalized = value;
    if (normalized.rfind("0x", 0) == 0 || normalized.rfind("0X", 0) == 0) {
        normalized.erase(0, 2);
    }

    if (normalized.empty()) {
        return std::nullopt;
    }

    try {
        size_t end = 0;
        const unsigned long parsed = std::stoul(normalized, &end, 16);
        if (end != normalized.size() || parsed > 0xFFFFFFFFUL) {
            return std::nullopt;
        }
        return static_cast<uint32_t>(parsed);
    } catch (...) {
        return std::nullopt;
    }
}

std::filesystem::path ResolveStateRoot() {
    if (const char* xdg = std::getenv("XDG_STATE_HOME"); xdg != nullptr && *xdg != '\0') {
        return std::filesystem::path(xdg);
    }
    if (const char* home = std::getenv("HOME"); home != nullptr && *home != '\0') {
        return std::filesystem::path(home) / ".local" / "state";
    }
    return std::filesystem::current_path();
}

std::int64_t NowEpochSeconds() {
    return std::chrono::duration_cast<std::chrono::seconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

std::string NormalizePeerName(std::string value) {
    value = Trim(std::move(value));
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return value;
}

} // namespace

std::filesystem::path DefaultStatePath() {
    return ResolveStateRoot() / kStateDirName / kStateFileName;
}

bool LoadAppState(const std::filesystem::path& path, AppState& state, std::string& errorMessage) {
    std::ifstream input(path);
    if (!input) {
        errorMessage = "Could not open state file: " + path.string();
        return false;
    }

    AppState parsed;
    std::string line;
    int lineNumber = 0;
    while (std::getline(input, line)) {
        ++lineNumber;
        const std::string trimmed = Trim(line);
        if (trimmed.empty() || trimmed[0] == '#' || trimmed[0] == ';') {
            continue;
        }

        const std::size_t separator = trimmed.find('=');
        if (separator == std::string::npos) {
            errorMessage = "Invalid state line " + std::to_string(lineNumber) + ": missing '='.";
            return false;
        }

        const std::string key = Trim(trimmed.substr(0, separator));
        const std::string value = Trim(trimmed.substr(separator + 1));

        if (key == "local_machine_id") {
            const auto parsedId = ParseMachineId(value);
            if (!parsedId) {
                errorMessage = "Invalid local_machine_id on line " + std::to_string(lineNumber) + ".";
                return false;
            }
            parsed.localMachineId = *parsedId;
            continue;
        }

        if (key == "peer") {
            std::vector<std::string> fields;
            std::stringstream fieldStream(value);
            std::string field;
            while (std::getline(fieldStream, field, '\t')) {
                fields.push_back(field);
            }

            if (fields.size() != 6 && fields.size() != 7) {
                errorMessage = "Invalid peer record on line " + std::to_string(lineNumber) + ".";
                return false;
            }

            PeerState peer;
            peer.host = fields[0];
            peer.name = fields[1];

            const auto port = ParsePort(fields[2]);
            const auto approved = ParseBool(fields[3]);
            const auto connectedNow = (fields.size() == 7) ? ParseBool(fields[4]) : std::optional<bool>(false);
            const auto lastSeen = ParseInt64(fields[fields.size() == 7 ? 5 : 4]);
            const auto lastConnected = ParseInt64(fields[fields.size() == 7 ? 6 : 5]);
            if (!port || !approved || !connectedNow || !lastSeen || !lastConnected) {
                errorMessage = "Invalid peer values on line " + std::to_string(lineNumber) + ".";
                return false;
            }

            peer.port = *port;
            peer.approved = *approved;
            peer.connectedNow = *connectedNow;
            peer.lastSeenEpochSeconds = *lastSeen;
            peer.lastConnectedEpochSeconds = *lastConnected;
            parsed.peers.push_back(std::move(peer));
        }
    }

    state = std::move(parsed);
    return true;
}

bool SaveAppState(const std::filesystem::path& path, const AppState& state, std::string& errorMessage) {
    std::ostringstream output;
    output << "local_machine_id=0x" << std::hex << state.localMachineId << std::dec << "\n";
    for (const auto& peer : state.peers) {
        output << "peer="
               << peer.host << '\t'
               << peer.name << '\t'
               << peer.port << '\t'
               << (peer.approved ? "true" : "false") << '\t'
               << (peer.connectedNow ? "true" : "false") << '\t'
               << peer.lastSeenEpochSeconds << '\t'
               << peer.lastConnectedEpochSeconds << "\n";
    }

    return WritePrivateFileAtomically(path, output.str(), errorMessage);
}

uint32_t EnsureLocalMachineId(AppState& state) {
    if (state.localMachineId != 0) {
        return state.localMachineId;
    }

    std::random_device randomDevice;
    uint32_t value = (static_cast<uint32_t>(randomDevice()) << 16) ^ static_cast<uint32_t>(randomDevice());
    if (value == 0) {
        value = 0xDEAD0001U;
    }
    state.localMachineId = value;
    return state.localMachineId;
}

void UpsertPeerState(AppState& state, const PeerState& peer) {
    auto existing = std::find_if(state.peers.begin(), state.peers.end(), [&](const PeerState& current) {
        return current.host == peer.host && current.port == peer.port;
    });

    if (existing == state.peers.end()) {
        PeerState toInsert = peer;
        if (toInsert.lastSeenEpochSeconds == 0) {
            toInsert.lastSeenEpochSeconds = NowEpochSeconds();
        }
        state.peers.push_back(std::move(toInsert));
        return;
    }

    if (!peer.name.empty()) {
        existing->name = peer.name;
    }
    existing->approved = existing->approved || peer.approved;
    existing->connectedNow = peer.connectedNow;
    existing->lastSeenEpochSeconds = std::max(existing->lastSeenEpochSeconds, peer.lastSeenEpochSeconds);
    existing->lastConnectedEpochSeconds = std::max(existing->lastConnectedEpochSeconds, peer.lastConnectedEpochSeconds);
}

std::size_t RemoveStalePeerAddressesForName(
    AppState& state,
    const std::string& name,
    const std::string& currentHost,
    int port) {
    const std::string normalizedName = NormalizePeerName(name);
    if (normalizedName.empty() || currentHost.empty()) {
        return 0;
    }

    const auto originalSize = state.peers.size();
    state.peers.erase(
        std::remove_if(state.peers.begin(), state.peers.end(), [&](const PeerState& peer) {
            return peer.port == port &&
                   peer.host != currentHost &&
                   NormalizePeerName(peer.name) == normalizedName;
        }),
        state.peers.end());
    return originalSize - state.peers.size();
}

void ClearConnectedPeers(AppState& state) {
    for (auto& peer : state.peers) {
        peer.connectedNow = false;
    }
}

void SetPeerConnectedState(AppState& state, const std::string& host, int port, bool connected) {
    auto existing = std::find_if(state.peers.begin(), state.peers.end(), [&](const PeerState& current) {
        return current.host == host && current.port == port;
    });
    if (existing == state.peers.end()) {
        return;
    }

    existing->connectedNow = connected;
}

void MarkSessionEstablished(
    AppState& state,
    const std::string& host,
    int port,
    const std::string& remoteName,
    uint32_t localMachineId,
    std::int64_t epochSeconds) {
    if (localMachineId != 0) {
        state.localMachineId = localMachineId;
    }

    ClearConnectedPeers(state);
    RemoveStalePeerAddressesForName(state, remoteName, host, port);

    PeerState peer;
    peer.host = host;
    peer.port = port;
    peer.name = remoteName;
    peer.approved = true;
    peer.connectedNow = true;
    peer.lastSeenEpochSeconds = epochSeconds;
    peer.lastConnectedEpochSeconds = epochSeconds;
    UpsertPeerState(state, peer);
}

void MarkSessionDisconnected(AppState& state) {
    ClearConnectedPeers(state);
}

} // namespace mwb
