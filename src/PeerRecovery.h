#pragma once

#include <string>
#include <string_view>
#include <vector>

#include "AppState.h"

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

} // namespace mwb
