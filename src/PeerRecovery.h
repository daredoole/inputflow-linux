#pragma once

#include <string>
#include <string_view>
#include <optional>
#include <vector>

#include "AppConfig.h"
#include "AppState.h"
#include "Discovery.h"

namespace mwb {

struct PeerRecoveryPlan {
    uint32_t expectedRemoteMachineId{0};
    std::vector<std::string> candidateHosts;
};

bool IsIpv4Literal(std::string_view host);
std::string NormalizeHostLabel(std::string_view value);
bool HostLabelsMatch(std::string_view lhs, std::string_view rhs);

std::vector<std::string> CollectRecoveryPeerNames(const AppState& state,
                                                  std::string_view configuredHost,
                                                  int port);
std::vector<std::string> CollectRecoveryPeerHosts(const AppState& state,
                                                  const std::vector<std::string>& recoveryNames,
                                                  std::string_view configuredHost,
                                                  int port);
std::vector<std::string> CollectRecoveryCandidateHosts(const AppState& state,
                                                       std::string_view configuredHost,
                                                       int port);
std::vector<std::string> CollectRecoveryDiscoveredHosts(const AppState& state,
                                                       std::string_view configuredHost,
                                                       int port,
                                                       const std::vector<DiscoveryCandidate>& candidates);
std::vector<std::string> CollectRecoveryUnidentifiedHosts(const AppState& state,
                                                          std::string_view configuredHost,
                                                          int port,
                                                          const std::vector<DiscoveryCandidate>& candidates);
uint32_t FindExpectedRemoteMachineId(const AppState& state,
                                     std::string_view configuredHost,
                                     int port);
PeerRecoveryPlan BuildPeerRecoveryPlanFromCandidates(
    const AppConfig& config,
    const AppState& state,
    const std::vector<DiscoveryCandidate>& candidates);
PeerRecoveryPlan BuildPeerRecoveryPlan(const AppConfig& config, const AppState& state);
std::optional<std::string> RecoverConfiguredHostFromKnownPeers(const AppConfig& config,
                                                               const AppState& state);

} // namespace mwb
