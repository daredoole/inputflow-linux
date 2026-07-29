#include "PeerRecovery.h"

#include <algorithm>
#include <arpa/inet.h>
#include <cctype>
#include <cerrno>
#include <cstdint>
#include <fcntl.h>
#include <iostream>
#include <limits>
#include <netdb.h>
#include <optional>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

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

    struct addrinfo hints {};
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

        struct pollfd descriptor {};
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

std::optional<std::string> RecoverConfiguredHostFromKnownPeers(const AppConfig& config,
                                                               const AppState& state) {
    const bool configuredHostIsIpv4 = IsIpv4Literal(config.host);
    const auto knownPeerHosts = CollectRecoveryCandidateHosts(state, config.host, config.port);
    for (const auto& host : knownPeerHosts) {
        if (auto reachable = ProbeReachableIpv4Host(host, config.port, 250)) {
            std::cout << "[RECOVERY] Found a verified approved peer address";
            if (configuredHostIsIpv4) {
                std::cout << "; using name-priority recovery before trusting the configured IP";
            } else {
                std::cout << "; reusing verified peer address";
            }
            std::cout << std::endl;
            return reachable;
        }
    }

    if (configuredHostIsIpv4 && ProbeReachableIpv4Host(config.host, config.port, 200).has_value()) {
        return std::nullopt;
    }

    DiscoveryOptions discoveryOptions;
    discoveryOptions.port = static_cast<uint16_t>(config.port);
    discoveryOptions.connectTimeoutMs = 200;
    discoveryOptions.maxHostsPerSubnet = 256;
    const auto candidates = DiscoverLanCandidates(discoveryOptions);
    if (!IsIpv4Literal(config.host)) {
        for (const auto& candidate : candidates) {
            if (candidate.status != DiscoveryStatus::Open ||
                candidate.hostName.empty() ||
                !IsIpv4Literal(candidate.ipAddress) ||
                !HostLabelsMatch(candidate.hostName, config.host)) {
                continue;
            }
            std::cout << "[RECOVERY] Resolved the approved peer through LAN discovery." << std::endl;
            return candidate.ipAddress;
        }
    }
    for (const auto& host : CollectRecoveryDiscoveredHosts(state, config.host, config.port, candidates)) {
        std::cout << "[RECOVERY] Using the verified address of an approved peer." << std::endl;
        return host;
    }

    // Final fallback for IPv4-literal configs whose address went stale and has
    // no peer record tied to that exact IP: match ANY approved, named peer
    // against live discovery, preferring the most recently connected one. This
    // recovers the common case where the user hardcoded a Windows host IP, the
    // peer moved (DHCP), and the only link is the approved peer's name.
    // Only act when there is exactly ONE approved named peer for this port: a
    // bare stale IP does not identify which peer was intended, so guessing among
    // several risks handing input to the wrong machine. The single-peer case is
    // unambiguous and safe.
    if (configuredHostIsIpv4) {
        const PeerState* solePeer = nullptr;
        std::size_t approvedCount = 0;
        for (const auto& peer : state.peers) {
            if (peer.approved && peer.port == config.port && !peer.name.empty()) {
                ++approvedCount;
                solePeer = &peer;
            }
        }
        if (approvedCount == 1 && solePeer != nullptr) {
            for (const auto& candidate : candidates) {
                if (candidate.status != DiscoveryStatus::Open ||
                    candidate.hostName.empty() ||
                    !IsIpv4Literal(candidate.ipAddress) ||
                    candidate.ipAddress == config.host) {
                    continue;
                }
                if (HostLabelsMatch(candidate.hostName, solePeer->name)) {
                    std::cout << "[RECOVERY] Rediscovered the sole approved peer." << std::endl;
                    return candidate.ipAddress;
                }
            }
        }
    }

    return std::nullopt;
}

} // namespace mwb
