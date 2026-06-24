#pragma once

#include <string>
#include <string_view>
#include <optional>
#include <vector>

#include "AppConfig.h"
#include "AppState.h"
#include "Discovery.h"

namespace mwb {

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
std::optional<std::string> RecoverConfiguredHostFromKnownPeers(const AppConfig& config,
                                                               const AppState& state);

} // namespace mwb
