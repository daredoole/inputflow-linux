#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace mwb {

struct PeerState {
    std::string host;
    std::string name;
    int port{15101};
    bool approved{false};
    bool connectedNow{false};
    std::int64_t lastSeenEpochSeconds{0};
    std::int64_t lastConnectedEpochSeconds{0};
};

struct AppState {
    uint32_t localMachineId{0};
    std::vector<PeerState> peers;
};

std::filesystem::path DefaultStatePath();
bool LoadAppState(const std::filesystem::path& path, AppState& state, std::string& errorMessage);
bool SaveAppState(const std::filesystem::path& path, const AppState& state, std::string& errorMessage);
uint32_t EnsureLocalMachineId(AppState& state);
void UpsertPeerState(AppState& state, const PeerState& peer);
void ClearConnectedPeers(AppState& state);
void SetPeerConnectedState(AppState& state, const std::string& host, int port, bool connected);
void MarkSessionEstablished(
    AppState& state,
    const std::string& host,
    int port,
    const std::string& remoteName,
    uint32_t localMachineId,
    std::int64_t epochSeconds);
void MarkSessionDisconnected(AppState& state);

} // namespace mwb
