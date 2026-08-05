#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <utility>
#include <vector>

namespace mwb {

struct PeerState {
    PeerState() = default;
    PeerState(std::string hostValue,
              std::string nameValue,
              int portValue,
              bool approvedValue,
              bool connectedNowValue,
              std::int64_t lastSeenValue,
              std::int64_t lastConnectedValue,
              uint32_t remoteMachineIdValue = 0,
              std::vector<std::string> previousHostsValue = {})
        : host(std::move(hostValue)),
          name(std::move(nameValue)),
          port(portValue),
          approved(approvedValue),
          connectedNow(connectedNowValue),
          lastSeenEpochSeconds(lastSeenValue),
          lastConnectedEpochSeconds(lastConnectedValue),
          remoteMachineId(remoteMachineIdValue),
          previousHosts(std::move(previousHostsValue)) {}

    std::string host;
    std::string name;
    int port{15101};
    bool approved{false};
    bool connectedNow{false};
    std::int64_t lastSeenEpochSeconds{0};
    std::int64_t lastConnectedEpochSeconds{0};
    uint32_t remoteMachineId{0};
    std::vector<std::string> previousHosts;
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
std::size_t RemoveStalePeerAddressesForName(
    AppState& state,
    const std::string& name,
    const std::string& currentHost,
    int port);
void ClearConnectedPeers(AppState& state);
void SetPeerConnectedState(AppState& state, const std::string& host, int port, bool connected);
void MarkSessionEstablished(
    AppState& state,
    const std::string& host,
    int port,
    const std::string& remoteName,
    uint32_t remoteMachineId,
    uint32_t localMachineId,
    std::int64_t epochSeconds);
void MarkSessionDisconnected(AppState& state);

} // namespace mwb
