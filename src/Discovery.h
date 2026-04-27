#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace mwb {

enum class DiscoveryStatus {
    Open,
    NoResponse,
    Error,
};

struct DiscoveryCandidate {
    std::string ipAddress;
    std::string hostName;
    bool hostNameVerified{false};
    std::string interfaceName;
    DiscoveryStatus status{DiscoveryStatus::NoResponse};
};

struct DiscoveryOptions {
    uint16_t port{15101};
    int connectTimeoutMs{200};
    std::size_t maxHostsPerSubnet{256};
    std::size_t maxConcurrentProbes{32};
    bool resolveHostNames{true};
};

std::vector<DiscoveryCandidate> DiscoverLanCandidates(const DiscoveryOptions& options = DiscoveryOptions{});

} // namespace mwb
