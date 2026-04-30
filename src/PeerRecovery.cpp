#include "PeerRecovery.h"

#include <algorithm>
#include <arpa/inet.h>
#include <cctype>

namespace mwb {

namespace {

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

std::string ToLowerCopy(std::string_view value) {
    std::string lowered;
    lowered.reserve(value.size());
    for (char ch : value) {
        lowered.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(ch))));
    }
    return lowered;
}

} // namespace

bool IsIpv4Literal(std::string_view host) {
    const std::string text(TrimWhitespace(host));
    if (text.empty()) {
        return false;
    }

    struct in_addr address {};
    return inet_pton(AF_INET, text.c_str(), &address) == 1;
}

std::string NormalizeHostLabel(std::string_view value) {
    std::string normalized = ToLowerCopy(TrimWhitespace(value));
    const size_t separator = normalized.find('.');
    if (separator != std::string::npos) {
        normalized.erase(separator);
    }
    return normalized;
}

bool HostLabelsMatch(std::string_view lhs, std::string_view rhs) {
    const std::string left = NormalizeHostLabel(lhs);
    const std::string right = NormalizeHostLabel(rhs);
    return !left.empty() && left == right;
}

std::vector<std::string> CollectRecoveryPeerNames(const AppState& state,
                                                  std::string_view configuredHost,
                                                  int port) {
    std::vector<const PeerState*> matches;
    matches.reserve(state.peers.size());

    for (const auto& peer : state.peers) {
        if (!peer.approved || peer.port != port || peer.name.empty()) {
            continue;
        }

        if (IsIpv4Literal(configuredHost)) {
            if (peer.host != configuredHost) {
                continue;
            }
        } else if (!HostLabelsMatch(peer.name, configuredHost) && !HostLabelsMatch(peer.host, configuredHost)) {
            continue;
        }

        matches.push_back(&peer);
    }

    std::stable_sort(matches.begin(), matches.end(), [&](const PeerState* lhs, const PeerState* rhs) {
        return lhs->lastConnectedEpochSeconds > rhs->lastConnectedEpochSeconds;
    });

    std::vector<std::string> names;
    std::vector<std::string> seen;
    for (const auto* peer : matches) {
        const std::string normalized = NormalizeHostLabel(peer->name);
        if (normalized.empty() || std::find(seen.begin(), seen.end(), normalized) != seen.end()) {
            continue;
        }

        names.push_back(peer->name);
        seen.push_back(normalized);
    }

    if (!IsIpv4Literal(configuredHost)) {
        const std::string normalizedConfiguredHost = NormalizeHostLabel(configuredHost);
        if (!normalizedConfiguredHost.empty() &&
            std::find(seen.begin(), seen.end(), normalizedConfiguredHost) == seen.end()) {
            names.insert(names.begin(), std::string(configuredHost));
        }
    }

    return names;
}

std::vector<std::string> CollectRecoveryPeerHosts(const AppState& state,
                                                  const std::vector<std::string>& recoveryNames,
                                                  std::string_view configuredHost,
                                                  int port) {
    if (recoveryNames.empty()) {
        return {};
    }

    std::vector<std::string> normalizedNames;
    normalizedNames.reserve(recoveryNames.size());
    for (const auto& name : recoveryNames) {
        const std::string normalized = NormalizeHostLabel(name);
        if (!normalized.empty() &&
            std::find(normalizedNames.begin(), normalizedNames.end(), normalized) == normalizedNames.end()) {
            normalizedNames.push_back(normalized);
        }
    }

    std::vector<const PeerState*> matches;
    matches.reserve(state.peers.size());
    for (const auto& peer : state.peers) {
        if (!peer.approved || peer.port != port || peer.name.empty() || !IsIpv4Literal(peer.host)) {
            continue;
        }
        if (peer.host == configuredHost) {
            continue;
        }

        const std::string normalized = NormalizeHostLabel(peer.name);
        if (std::find(normalizedNames.begin(), normalizedNames.end(), normalized) == normalizedNames.end()) {
            continue;
        }

        matches.push_back(&peer);
    }

    std::stable_sort(matches.begin(), matches.end(), [&](const PeerState* lhs, const PeerState* rhs) {
        return lhs->lastConnectedEpochSeconds > rhs->lastConnectedEpochSeconds;
    });

    std::vector<std::string> hosts;
    for (const auto* peer : matches) {
        if (std::find(hosts.begin(), hosts.end(), peer->host) != hosts.end()) {
            continue;
        }
        hosts.push_back(peer->host);
    }

    return hosts;
}

std::vector<std::string> CollectRecoveryCandidateHosts(const AppState& state,
                                                       std::string_view configuredHost,
                                                       int port) {
    const std::vector<std::string> recoveryNames = CollectRecoveryPeerNames(state, configuredHost, port);
    if (recoveryNames.empty()) {
        return {};
    }

    return CollectRecoveryPeerHosts(state, recoveryNames, configuredHost, port);
}

std::vector<std::string> CollectRecoveryDiscoveredHosts(const AppState& state,
                                                       std::string_view configuredHost,
                                                       int port,
                                                       const std::vector<DiscoveryCandidate>& candidates) {
    const std::vector<std::string> recoveryNames = CollectRecoveryPeerNames(state, configuredHost, port);
    if (recoveryNames.empty()) {
        return {};
    }

    std::vector<std::string> normalizedNames;
    normalizedNames.reserve(recoveryNames.size());
    for (const auto& name : recoveryNames) {
        const std::string normalized = NormalizeHostLabel(name);
        if (!normalized.empty() &&
            std::find(normalizedNames.begin(), normalizedNames.end(), normalized) == normalizedNames.end()) {
            normalizedNames.push_back(normalized);
        }
    }

    std::vector<std::string> hosts;
    for (const auto& candidate : candidates) {
        if (candidate.status != DiscoveryStatus::Open ||
            candidate.hostName.empty() ||
            !IsIpv4Literal(candidate.ipAddress) ||
            candidate.ipAddress == configuredHost) {
            continue;
        }

        const std::string normalized = NormalizeHostLabel(candidate.hostName);
        if (std::find(normalizedNames.begin(), normalizedNames.end(), normalized) == normalizedNames.end()) {
            continue;
        }
        if (std::find(hosts.begin(), hosts.end(), candidate.ipAddress) != hosts.end()) {
            continue;
        }
        hosts.push_back(candidate.ipAddress);
    }

    return hosts;
}

} // namespace mwb
