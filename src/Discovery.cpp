#include "Discovery.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <deque>
#include <fcntl.h>
#include <future>
#include <ifaddrs.h>
#include <memory>
#include <net/if.h>
#include <poll.h>
#include <set>
#include <sstream>
#include <string>
#include <tuple>
#include <unordered_set>
#include <utility>
#include <vector>

#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>

namespace mwb {
namespace {

constexpr std::size_t kAbsoluteMaxHostsPerSubnet = 4096;
constexpr std::size_t kAbsoluteMaxConcurrentProbes = 128;
constexpr int kMinimumConnectTimeoutMs = 25;
constexpr int kMinimumHostNameLookupTimeoutMs = 1000;
constexpr uint16_t kMdnsPort = 5353;
constexpr uint16_t kNetbiosNameServicePort = 137;

struct InterfaceSubnet {
    std::string interfaceName;
    uint32_t address;
    uint32_t netmask;
    uint32_t network;
    uint32_t broadcast;
};

struct ProbeTarget {
    std::string interfaceName;
    uint32_t address;
};

class SocketHandle {
public:
    explicit SocketHandle(int fd) : m_fd(fd) {
    }

    ~SocketHandle() {
        if (m_fd >= 0) {
            close(m_fd);
        }
    }

    SocketHandle(const SocketHandle&) = delete;
    SocketHandle& operator=(const SocketHandle&) = delete;

    SocketHandle(SocketHandle&& other) noexcept : m_fd(other.m_fd) {
        other.m_fd = -1;
    }

    SocketHandle& operator=(SocketHandle&& other) noexcept {
        if (this == &other) {
            return *this;
        }
        if (m_fd >= 0) {
            close(m_fd);
        }
        m_fd = other.m_fd;
        other.m_fd = -1;
        return *this;
    }

    int Get() const {
        return m_fd;
    }

private:
    int m_fd{-1};
};

struct IfAddrsDeleter {
    void operator()(ifaddrs* value) const {
        if (value != nullptr) {
            freeifaddrs(value);
        }
    }
};

bool IsPrivateIpv4(uint32_t address) {
    return (address & 0xff000000u) == 0x0a000000u ||
           (address & 0xfff00000u) == 0xac100000u ||
           (address & 0xffff0000u) == 0xc0a80000u;
}

int MinimumPrivatePrefix(uint32_t address) {
    if ((address & 0xff000000u) == 0x0a000000u) {
        return 8;
    }
    if ((address & 0xfff00000u) == 0xac100000u) {
        return 12;
    }
    if ((address & 0xffff0000u) == 0xc0a80000u) {
        return 16;
    }
    return 33;
}

bool IsContiguousNetmask(uint32_t netmask) {
    const uint32_t inverted = ~netmask;
    return (inverted & (inverted + 1u)) == 0;
}

int CountMaskBits(uint32_t netmask) {
    int bits = 0;
    while (netmask != 0) {
        bits += static_cast<int>(netmask & 1u);
        netmask >>= 1u;
    }
    return bits;
}

std::string FormatIpv4Address(uint32_t address) {
    struct in_addr ipv4{};
    ipv4.s_addr = htonl(address);

    std::array<char, INET_ADDRSTRLEN> buffer{};
    if (inet_ntop(AF_INET, &ipv4, buffer.data(), buffer.size()) == nullptr) {
        return "unknown";
    }
    return std::string(buffer.data());
}

bool SetNonBlocking(int fd) {
    const int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0) {
        return false;
    }
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0;
}

DiscoveryStatus ClassifyConnectError(int errorCode) {
    switch (errorCode) {
        case 0:
            return DiscoveryStatus::Open;
        case ECONNREFUSED:
        case ECONNRESET:
        case EHOSTDOWN:
        case EHOSTUNREACH:
        case ENETDOWN:
        case ENETUNREACH:
        case ETIMEDOUT:
            return DiscoveryStatus::NoResponse;
        default:
            return DiscoveryStatus::Error;
    }
}

DiscoveryStatus ProbeTcpPort(uint32_t address, uint16_t port, int timeoutMs) {
    SocketHandle socketHandle(socket(AF_INET, SOCK_STREAM, 0));
    if (socketHandle.Get() < 0) {
        return DiscoveryStatus::Error;
    }

    if (!SetNonBlocking(socketHandle.Get())) {
        return DiscoveryStatus::Error;
    }

    struct sockaddr_in socketAddress{};
    socketAddress.sin_family = AF_INET;
    socketAddress.sin_port = htons(port);
    socketAddress.sin_addr.s_addr = htonl(address);

    const int connectResult = connect(
        socketHandle.Get(),
        reinterpret_cast<const struct sockaddr*>(&socketAddress),
        sizeof(socketAddress));
    if (connectResult == 0) {
        return DiscoveryStatus::Open;
    }

    if (errno != EINPROGRESS && errno != EINTR) {
        return ClassifyConnectError(errno);
    }

    struct pollfd pollDescriptor{};
    pollDescriptor.fd = socketHandle.Get();
    pollDescriptor.events = POLLOUT;

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    while (true) {
        const auto now = std::chrono::steady_clock::now();
        if (now >= deadline) {
            return DiscoveryStatus::NoResponse;
        }

        const int remainingMs = static_cast<int>(
            std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now).count());
        const int pollResult = poll(&pollDescriptor, 1, remainingMs);
        if (pollResult == 0) {
            return DiscoveryStatus::NoResponse;
        }
        if (pollResult < 0) {
            if (errno == EINTR) {
                continue;
            }
            return DiscoveryStatus::Error;
        }

        int socketError = 0;
        socklen_t socketErrorLength = sizeof(socketError);
        if (getsockopt(socketHandle.Get(), SOL_SOCKET, SO_ERROR, &socketError, &socketErrorLength) != 0) {
            return DiscoveryStatus::Error;
        }
        return ClassifyConnectError(socketError);
    }
}

std::string TrimDiscoveredName(std::string value) {
    while (!value.empty() && (value.back() == '\n' || value.back() == '\r' || value.back() == ' ' || value.back() == '\t')) {
        value.pop_back();
    }
    std::size_t start = 0;
    while (start < value.size() && (value[start] == ' ' || value[start] == '\t')) {
        ++start;
    }
    if (start > 0) {
        value.erase(0, start);
    }
    return value;
}

std::string NormalizeDiscoveredName(std::string value) {
    for (char& ch : value) {
        const unsigned char raw = static_cast<unsigned char>(ch);
        if (raw <= 0x20u || ch == '|') {
            ch = '-';
        }
    }
    return value;
}

std::string SelectCorroboratedDiscoveredName(const std::array<std::string, 4>& names) {
    for (std::size_t left = 0; left < names.size(); ++left) {
        if (names[left].empty()) {
            continue;
        }
        for (std::size_t right = left + 1; right < names.size(); ++right) {
            if (names[left] == names[right]) {
                return names[left];
            }
        }
    }
    return {};
}

std::string SelectFirstDiscoveredName(const std::array<std::string, 4>& names) {
    for (const auto& name : names) {
        if (!name.empty()) {
            return name;
        }
    }
    return {};
}

std::string AvahiResolveAddress(uint32_t address) {
    constexpr const char* kTimeoutPath = "/usr/bin/timeout";
    constexpr const char* kAvahiResolveAddressPath = "/usr/bin/avahi-resolve-address";
    if (access(kTimeoutPath, X_OK) != 0 || access(kAvahiResolveAddressPath, X_OK) != 0) {
        return {};
    }

    const std::string ipAddress = FormatIpv4Address(address);
    const std::string command = std::string(kTimeoutPath) + " 1s " + kAvahiResolveAddressPath + " -4 -a " + ipAddress + " 2>/dev/null";
    std::unique_ptr<FILE, int (*)(FILE*)> pipe(popen(command.c_str(), "r"), pclose);
    if (!pipe) {
        return {};
    }

    std::array<char, 512> buffer{};
    if (fgets(buffer.data(), static_cast<int>(buffer.size()), pipe.get()) == nullptr) {
        return {};
    }

    std::string line(buffer.data());
    const std::size_t separator = line.find_first_of(" \t");
    if (separator == std::string::npos) {
        return {};
    }

    std::string name = TrimDiscoveredName(line.substr(separator + 1));
    if (name == ipAddress) {
        return {};
    }
    return NormalizeDiscoveredName(name);
}

std::string NmblookupResolveAddress(uint32_t address, int timeoutMs) {
    constexpr const char* kTimeoutPath = "/usr/bin/timeout";
    constexpr const char* kNmblookupPath = "/usr/bin/nmblookup";
    if (access(kTimeoutPath, X_OK) != 0 || access(kNmblookupPath, X_OK) != 0) {
        return {};
    }

    const std::string ipAddress = FormatIpv4Address(address);
    const int timeoutSeconds = std::max(1, (std::min(timeoutMs, 3000) + 999) / 1000);
    const std::string command = std::string(kTimeoutPath) + " " + std::to_string(timeoutSeconds) + "s " +
        kNmblookupPath + " -A " + ipAddress + " 2>/dev/null";
    std::unique_ptr<FILE, int (*)(FILE*)> pipe(popen(command.c_str(), "r"), pclose);
    if (!pipe) {
        return {};
    }

    std::array<char, 512> buffer{};
    std::string fallbackName;
    while (fgets(buffer.data(), static_cast<int>(buffer.size()), pipe.get()) != nullptr) {
        const std::string line = TrimDiscoveredName(buffer.data());
        if (line.find("<GROUP>") != std::string::npos) {
            continue;
        }
        if (line.find("<00>") == std::string::npos && line.find("<20>") == std::string::npos) {
            continue;
        }

        std::istringstream stream(line);
        std::string name;
        stream >> name;
        if (name.empty() || name == "MAC") {
            continue;
        }
        if (line.find("<00>") != std::string::npos) {
            return NormalizeDiscoveredName(name);
        }
        if (fallbackName.empty()) {
            fallbackName = NormalizeDiscoveredName(name);
        }
    }

    return fallbackName;
}

void AppendUint16(std::vector<uint8_t>& bytes, uint16_t value) {
    bytes.push_back(static_cast<uint8_t>((value >> 8) & 0xffu));
    bytes.push_back(static_cast<uint8_t>(value & 0xffu));
}

uint16_t ReadUint16(const uint8_t* data) {
    return static_cast<uint16_t>((static_cast<uint16_t>(data[0]) << 8u) | static_cast<uint16_t>(data[1]));
}

void AppendNetbiosWildcardName(std::vector<uint8_t>& packet) {
    std::array<uint8_t, 16> rawName{};
    rawName.fill(' ');
    rawName[0] = '*';
    rawName[15] = 0x00;

    packet.push_back(32);
    for (uint8_t value : rawName) {
        packet.push_back(static_cast<uint8_t>('A' + ((value >> 4u) & 0x0fu)));
        packet.push_back(static_cast<uint8_t>('A' + (value & 0x0fu)));
    }
    packet.push_back(0);
}

std::vector<uint8_t> BuildNetbiosNodeStatusQuery(uint16_t transactionId) {
    std::vector<uint8_t> packet;
    packet.reserve(50);
    AppendUint16(packet, transactionId);
    AppendUint16(packet, 0x0000);
    AppendUint16(packet, 0x0001);
    AppendUint16(packet, 0x0000);
    AppendUint16(packet, 0x0000);
    AppendUint16(packet, 0x0000);
    AppendNetbiosWildcardName(packet);
    AppendUint16(packet, 0x0021);
    AppendUint16(packet, 0x0001);
    return packet;
}

void AppendDnsName(std::vector<uint8_t>& packet, const std::vector<std::string>& labels) {
    for (const std::string& label : labels) {
        packet.push_back(static_cast<uint8_t>(std::min<std::size_t>(label.size(), 63)));
        packet.insert(packet.end(), label.begin(), label.begin() + std::min<std::size_t>(label.size(), 63));
    }
    packet.push_back(0);
}

std::vector<uint8_t> BuildMdnsReversePtrQuery(uint32_t address) {
    std::vector<uint8_t> packet;
    packet.reserve(80);
    AppendUint16(packet, 0x0000);
    AppendUint16(packet, 0x0000);
    AppendUint16(packet, 0x0001);
    AppendUint16(packet, 0x0000);
    AppendUint16(packet, 0x0000);
    AppendUint16(packet, 0x0000);
    AppendDnsName(packet, {
        std::to_string(address & 0xffu),
        std::to_string((address >> 8u) & 0xffu),
        std::to_string((address >> 16u) & 0xffu),
        std::to_string((address >> 24u) & 0xffu),
        "in-addr",
        "arpa",
    });
    AppendUint16(packet, 0x000c);
    AppendUint16(packet, 0x8001);
    return packet;
}

bool ReadDnsName(const uint8_t* data, std::size_t size, std::size_t& offset, std::string& name) {
    std::size_t cursor = offset;
    std::size_t nextOffset = offset;
    bool compressed = false;
    std::vector<std::string> labels;

    for (std::size_t hops = 0; hops < 128 && cursor < size; ++hops) {
        const uint8_t length = data[cursor];
        if ((length & 0xc0u) == 0xc0u) {
            if (cursor + 1 >= size) {
                return false;
            }
            const std::size_t pointer = (static_cast<std::size_t>(length & 0x3fu) << 8u) | data[cursor + 1];
            if (pointer >= size) {
                return false;
            }
            if (!compressed) {
                nextOffset = cursor + 2;
                compressed = true;
            }
            cursor = pointer;
            continue;
        }
        if ((length & 0xc0u) != 0) {
            return false;
        }

        ++cursor;
        if (length == 0) {
            if (!compressed) {
                nextOffset = cursor;
            }
            offset = nextOffset;
            name.clear();
            for (const std::string& label : labels) {
                if (!name.empty()) {
                    name.push_back('.');
                }
                name += label;
            }
            return true;
        }
        if (cursor + length > size) {
            return false;
        }
        labels.emplace_back(reinterpret_cast<const char*>(data + cursor), length);
        cursor += length;
        if (!compressed) {
            nextOffset = cursor;
        }
    }

    return false;
}

bool SkipDnsName(const uint8_t* data, std::size_t size, std::size_t& offset) {
    std::string ignored;
    return ReadDnsName(data, size, offset, ignored);
}

std::string ParseDnsPtrResponse(const uint8_t* data, std::size_t size) {
    if (size < 12) {
        return {};
    }

    const uint16_t questionCount = ReadUint16(data + 4);
    const uint16_t answerCount = ReadUint16(data + 6);
    const uint16_t authorityCount = ReadUint16(data + 8);
    const uint16_t additionalCount = ReadUint16(data + 10);

    std::size_t offset = 12;
    for (uint16_t i = 0; i < questionCount; ++i) {
        if (!SkipDnsName(data, size, offset) || offset + 4 > size) {
            return {};
        }
        offset += 4;
    }

    const uint32_t recordCount = static_cast<uint32_t>(answerCount) + authorityCount + additionalCount;
    for (uint32_t record = 0; record < recordCount; ++record) {
        if (!SkipDnsName(data, size, offset) || offset + 10 > size) {
            return {};
        }

        const uint16_t type = ReadUint16(data + offset);
        offset += 2;
        offset += 2; // class
        offset += 4; // TTL
        const uint16_t dataLength = ReadUint16(data + offset);
        offset += 2;
        if (offset + dataLength > size) {
            return {};
        }

        if (type == 0x000c) {
            std::size_t nameOffset = offset;
            std::string ptrName;
            if (ReadDnsName(data, size, nameOffset, ptrName) && !ptrName.empty()) {
                return ptrName;
            }
        }

        offset += dataLength;
    }

    return {};
}

std::string MdnsReverseLookup(uint32_t address, int timeoutMs) {
    const std::vector<uint8_t> query = BuildMdnsReversePtrQuery(address);
    SocketHandle socketHandle(socket(AF_INET, SOCK_DGRAM, 0));
    if (socketHandle.Get() < 0 || !SetNonBlocking(socketHandle.Get())) {
        return {};
    }

    const uint8_t ttl = 255;
    (void)setsockopt(socketHandle.Get(), IPPROTO_IP, IP_MULTICAST_TTL, &ttl, sizeof(ttl));

    struct sockaddr_in socketAddress{};
    socketAddress.sin_family = AF_INET;
    socketAddress.sin_port = htons(kMdnsPort);
    if (inet_pton(AF_INET, "224.0.0.251", &socketAddress.sin_addr) != 1) {
        return {};
    }

    const ssize_t sent = sendto(
        socketHandle.Get(),
        query.data(),
        query.size(),
        0,
        reinterpret_cast<const struct sockaddr*>(&socketAddress),
        sizeof(socketAddress));
    if (sent < 0) {
        return {};
    }

    const int boundedTimeoutMs = std::max(kMinimumConnectTimeoutMs, std::min(timeoutMs, 1000));
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(boundedTimeoutMs);
    while (std::chrono::steady_clock::now() < deadline) {
        const auto now = std::chrono::steady_clock::now();
        const int remainingMs = static_cast<int>(
            std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now).count());

        struct pollfd pollDescriptor{};
        pollDescriptor.fd = socketHandle.Get();
        pollDescriptor.events = POLLIN;
        const int pollResult = poll(&pollDescriptor, 1, remainingMs);
        if (pollResult == 0) {
            return {};
        }
        if (pollResult < 0) {
            if (errno == EINTR) {
                continue;
            }
            return {};
        }

        std::array<uint8_t, 1500> response{};
        const ssize_t received = recvfrom(socketHandle.Get(), response.data(), response.size(), 0, nullptr, nullptr);
        if (received < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
                continue;
            }
            return {};
        }

        const std::string name = ParseDnsPtrResponse(response.data(), static_cast<std::size_t>(received));
        if (!name.empty()) {
            return name;
        }
    }

    return {};
}

std::string TrimNetbiosName(const uint8_t* data) {
    std::string name(reinterpret_cast<const char*>(data), 15);
    while (!name.empty() && (name.back() == ' ' || name.back() == '\0')) {
        name.pop_back();
    }
    for (char ch : name) {
        if (static_cast<unsigned char>(ch) < 0x20u) {
            return {};
        }
    }
    return name;
}

std::string ParseNetbiosNodeStatusNames(const uint8_t* data, std::size_t size, uint16_t transactionId) {
    if (size < 12 || ReadUint16(data) != transactionId) {
        return {};
    }

    const uint16_t questionCount = ReadUint16(data + 4);
    const uint16_t answerCount = ReadUint16(data + 6);
    const uint16_t authorityCount = ReadUint16(data + 8);
    const uint16_t additionalCount = ReadUint16(data + 10);

    std::size_t offset = 12;
    for (uint16_t i = 0; i < questionCount; ++i) {
        if (!SkipDnsName(data, size, offset) || offset + 4 > size) {
            return {};
        }
        offset += 4;
    }

    const uint32_t recordCount = static_cast<uint32_t>(answerCount) + authorityCount + additionalCount;
    for (uint32_t record = 0; record < recordCount; ++record) {
        if (!SkipDnsName(data, size, offset) || offset + 10 > size) {
            return {};
        }

        const uint16_t type = ReadUint16(data + offset);
        offset += 2;
        offset += 2; // class
        offset += 4; // TTL
        const uint16_t dataLength = ReadUint16(data + offset);
        offset += 2;
        if (offset + dataLength > size) {
            return {};
        }

        if (type == 0x0021 && dataLength >= 1) {
            const uint8_t nameCount = data[offset];
            const std::size_t entriesOffset = offset + 1;
            const std::size_t entriesEnd = offset + dataLength;
            std::string fallbackName;

            for (uint8_t i = 0; i < nameCount; ++i) {
                const std::size_t entryOffset = entriesOffset + static_cast<std::size_t>(i) * 18u;
                if (entryOffset + 18 > entriesEnd) {
                    break;
                }

                const uint8_t suffix = data[entryOffset + 15];
                const uint16_t flags = ReadUint16(data + entryOffset + 16);
                const bool isGroupName = (flags & 0x8000u) != 0;
                const std::string name = TrimNetbiosName(data + entryOffset);
                if (isGroupName || name.empty()) {
                    continue;
                }
                if (suffix == 0x00) {
                    return name;
                }
                if (fallbackName.empty() || suffix == 0x20) {
                    fallbackName = name;
                }
            }

            return fallbackName;
        }

        offset += dataLength;
    }

    return {};
}

std::string NetbiosNodeStatusLookup(uint32_t address, int timeoutMs) {
    const uint16_t transactionId = static_cast<uint16_t>((address & 0xffffu) ^ (address >> 16u) ^ 0x4e42u);
    const std::vector<uint8_t> query = BuildNetbiosNodeStatusQuery(transactionId);

    SocketHandle socketHandle(socket(AF_INET, SOCK_DGRAM, 0));
    if (socketHandle.Get() < 0 || !SetNonBlocking(socketHandle.Get())) {
        return {};
    }

    struct sockaddr_in socketAddress{};
    socketAddress.sin_family = AF_INET;
    socketAddress.sin_port = htons(kNetbiosNameServicePort);
    socketAddress.sin_addr.s_addr = htonl(address);

    const ssize_t sent = sendto(
        socketHandle.Get(),
        query.data(),
        query.size(),
        0,
        reinterpret_cast<const struct sockaddr*>(&socketAddress),
        sizeof(socketAddress));
    if (sent < 0) {
        return {};
    }

    const int boundedTimeoutMs = std::max(kMinimumConnectTimeoutMs, std::min(timeoutMs, 1000));
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(boundedTimeoutMs);
    while (std::chrono::steady_clock::now() < deadline) {
        const auto now = std::chrono::steady_clock::now();
        const int remainingMs = static_cast<int>(
            std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now).count());

        struct pollfd pollDescriptor{};
        pollDescriptor.fd = socketHandle.Get();
        pollDescriptor.events = POLLIN;
        const int pollResult = poll(&pollDescriptor, 1, remainingMs);
        if (pollResult <= 0) {
            return {};
        }

        std::array<uint8_t, 1024> response{};
        const ssize_t received = recvfrom(socketHandle.Get(), response.data(), response.size(), 0, nullptr, nullptr);
        if (received < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
                continue;
            }
            return {};
        }

        const std::string name = ParseNetbiosNodeStatusNames(
            response.data(),
            static_cast<std::size_t>(received),
            transactionId);
        if (!name.empty()) {
            return name;
        }
    }

    return {};
}

std::vector<InterfaceSubnet> EnumeratePrivateIpv4Subnets(std::unordered_set<uint32_t>& localAddresses) {
    ifaddrs* rawInterfaces = nullptr;
    if (getifaddrs(&rawInterfaces) != 0 || rawInterfaces == nullptr) {
        return {};
    }

    std::unique_ptr<ifaddrs, IfAddrsDeleter> interfaces(rawInterfaces);
    std::set<std::tuple<std::string, uint32_t, uint32_t>> seenSubnets;
    std::vector<InterfaceSubnet> subnets;

    for (const ifaddrs* entry = interfaces.get(); entry != nullptr; entry = entry->ifa_next) {
        if (entry->ifa_addr == nullptr || entry->ifa_netmask == nullptr) {
            continue;
        }
        if (entry->ifa_addr->sa_family != AF_INET || (entry->ifa_flags & IFF_UP) == 0 || (entry->ifa_flags & IFF_LOOPBACK) != 0) {
            continue;
        }

        const auto* address = reinterpret_cast<const struct sockaddr_in*>(entry->ifa_addr);
        const auto* netmask = reinterpret_cast<const struct sockaddr_in*>(entry->ifa_netmask);

        const uint32_t hostAddress = ntohl(address->sin_addr.s_addr);
        const uint32_t hostNetmask = ntohl(netmask->sin_addr.s_addr);
        if (!IsPrivateIpv4(hostAddress) || !IsContiguousNetmask(hostNetmask)) {
            continue;
        }

        const int prefixLength = CountMaskBits(hostNetmask);
        if (prefixLength < MinimumPrivatePrefix(hostAddress) || prefixLength >= 31) {
            continue;
        }

        const uint32_t network = hostAddress & hostNetmask;
        const uint32_t broadcast = network | ~hostNetmask;
        if (broadcast <= network + 1u) {
            continue;
        }

        localAddresses.insert(hostAddress);
        const auto subnetKey = std::make_tuple(std::string(entry->ifa_name), network, hostNetmask);
        if (!seenSubnets.insert(subnetKey).second) {
            continue;
        }

        subnets.push_back(InterfaceSubnet{
            entry->ifa_name,
            hostAddress,
            hostNetmask,
            network,
            broadcast,
        });
    }

    return subnets;
}

std::size_t CountLocalAddressesInRange(
    const std::unordered_set<uint32_t>& localAddresses,
    uint32_t firstHost,
    uint32_t lastHost) {
    std::size_t count = 0;
    for (uint32_t address : localAddresses) {
        if (address >= firstHost && address <= lastHost) {
            ++count;
        }
    }
    return count;
}

void AppendFullSubnetScan(
    const InterfaceSubnet& subnet,
    const std::unordered_set<uint32_t>& localAddresses,
    std::vector<ProbeTarget>& targets) {
    for (uint32_t address = subnet.network + 1u; address < subnet.broadcast; ++address) {
        if (localAddresses.find(address) != localAddresses.end()) {
            continue;
        }
        targets.push_back(ProbeTarget{subnet.interfaceName, address});
    }
}

void AppendCappedSubnetScan(
    const InterfaceSubnet& subnet,
    const std::unordered_set<uint32_t>& localAddresses,
    std::size_t maxHosts,
    std::vector<ProbeTarget>& targets) {
    std::size_t added = 0;
    int64_t lower = static_cast<int64_t>(subnet.address) - 1;
    int64_t upper = static_cast<int64_t>(subnet.address) + 1;
    const int64_t firstHost = static_cast<int64_t>(subnet.network) + 1;
    const int64_t lastHost = static_cast<int64_t>(subnet.broadcast) - 1;

    // For oversized subnets, probe addresses closest to the local host first.
    while (added < maxHosts && (lower >= firstHost || upper <= lastHost)) {
        if (lower >= firstHost) {
            const uint32_t candidate = static_cast<uint32_t>(lower--);
            if (localAddresses.find(candidate) == localAddresses.end()) {
                targets.push_back(ProbeTarget{subnet.interfaceName, candidate});
                ++added;
            }
        }
        if (added >= maxHosts) {
            break;
        }
        if (upper <= lastHost) {
            const uint32_t candidate = static_cast<uint32_t>(upper++);
            if (localAddresses.find(candidate) == localAddresses.end()) {
                targets.push_back(ProbeTarget{subnet.interfaceName, candidate});
                ++added;
            }
        }
    }
}

std::vector<ProbeTarget> BuildProbeTargets(
    const std::vector<InterfaceSubnet>& subnets,
    const std::unordered_set<uint32_t>& localAddresses,
    std::size_t maxHostsPerSubnet) {
    std::vector<ProbeTarget> targets;

    for (const InterfaceSubnet& subnet : subnets) {
        const uint32_t firstHost = subnet.network + 1u;
        const uint32_t lastHost = subnet.broadcast - 1u;
        const uint64_t totalHosts = static_cast<uint64_t>(lastHost) - static_cast<uint64_t>(firstHost) + 1u;
        const std::size_t localCount = CountLocalAddressesInRange(localAddresses, firstHost, lastHost);
        const uint64_t remoteHosts = totalHosts > localCount ? totalHosts - localCount : 0u;
        if (remoteHosts == 0 || maxHostsPerSubnet == 0) {
            continue;
        }

        if (remoteHosts <= maxHostsPerSubnet) {
            AppendFullSubnetScan(subnet, localAddresses, targets);
            continue;
        }

        AppendCappedSubnetScan(subnet, localAddresses, maxHostsPerSubnet, targets);
    }

    return targets;
}

DiscoveryCandidate ProbeCandidate(const ProbeTarget& target, const DiscoveryOptions& options) {
    DiscoveryCandidate candidate;
    candidate.ipAddress = FormatIpv4Address(target.address);
    candidate.interfaceName = target.interfaceName;
    candidate.status = ProbeTcpPort(target.address, options.port, options.connectTimeoutMs);
    if (options.resolveHostNames && candidate.status == DiscoveryStatus::Open) {
        const int nameLookupTimeoutMs = std::max(options.connectTimeoutMs, kMinimumHostNameLookupTimeoutMs);
        std::array<std::string, 4> resolvedNames = {
            NormalizeDiscoveredName(AvahiResolveAddress(target.address)),
            NormalizeDiscoveredName(MdnsReverseLookup(target.address, nameLookupTimeoutMs)),
            NormalizeDiscoveredName(NetbiosNodeStatusLookup(target.address, nameLookupTimeoutMs)),
            NormalizeDiscoveredName(NmblookupResolveAddress(target.address, nameLookupTimeoutMs)),
        };
        candidate.hostName = SelectCorroboratedDiscoveredName(resolvedNames);
        candidate.hostNameVerified = !candidate.hostName.empty();
        if (!candidate.hostNameVerified) {
            candidate.hostName = SelectFirstDiscoveredName(resolvedNames);
        }
    }
    return candidate;
}

} // namespace

std::vector<DiscoveryCandidate> DiscoverLanCandidates(const DiscoveryOptions& options) {
    DiscoveryOptions normalized = options;
    normalized.connectTimeoutMs = std::max(normalized.connectTimeoutMs, kMinimumConnectTimeoutMs);
    normalized.maxHostsPerSubnet = std::min(normalized.maxHostsPerSubnet, kAbsoluteMaxHostsPerSubnet);
    normalized.maxConcurrentProbes = std::min(
        std::max<std::size_t>(normalized.maxConcurrentProbes, 1u),
        kAbsoluteMaxConcurrentProbes);

    std::unordered_set<uint32_t> localAddresses;
    const std::vector<InterfaceSubnet> subnets = EnumeratePrivateIpv4Subnets(localAddresses);
    const std::vector<ProbeTarget> targets = BuildProbeTargets(subnets, localAddresses, normalized.maxHostsPerSubnet);

    std::vector<DiscoveryCandidate> candidates;
    candidates.reserve(targets.size());

    std::deque<std::future<DiscoveryCandidate>> pending;
    for (const ProbeTarget& target : targets) {
        try {
            pending.emplace_back(std::async(std::launch::async, [target, normalized]() {
                return ProbeCandidate(target, normalized);
            }));
        } catch (...) {
            candidates.push_back(ProbeCandidate(target, normalized));
            continue;
        }

        if (pending.size() >= normalized.maxConcurrentProbes) {
            candidates.push_back(pending.front().get());
            pending.pop_front();
        }
    }

    while (!pending.empty()) {
        candidates.push_back(pending.front().get());
        pending.pop_front();
    }

    return candidates;
}

} // namespace mwb
