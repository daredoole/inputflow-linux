#include "NetworkManager.h"

#include <algorithm>
#include <arpa/inet.h>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <endian.h>
#include <execinfo.h>
#include <iostream>
#include <iomanip>
#include <limits>
#include <linux/netlink.h>
#include <linux/rtnetlink.h>
#include <netdb.h>
#include <netinet/tcp.h>
#include <optional>
#include <poll.h>
#include <random>
#include <sstream>
#include <sys/socket.h>
#include <unistd.h>
#include <vector>

#include <openssl/rand.h>

#include "ClipboardManager.h"
#include "ReconnectPolicy.h"

namespace mwb {
namespace {

constexpr int kMaxConcurrentInboundSessions = 4;
thread_local std::string g_lastConnectError;

enum class ReceivePacketStatus {
    Ok,
    Timeout,
    Closed,
    Error,
    Stopped,
};

thread_local ReceivePacketStatus g_lastReadStatus = ReceivePacketStatus::Error;
thread_local ReceivePacketStatus g_lastReceivePacketStatus = ReceivePacketStatus::Error;

const std::string& GetLastConnectError() {
    return g_lastConnectError;
}

void SetLastConnectError(const std::string& message) {
    g_lastConnectError = message;
}

bool IsTruthyEnv(const char* name) {
    const char* value = std::getenv(name);
    if (value == nullptr) {
        return false;
    }

    const std::string text(value);
    return text == "1" || text == "true" || text == "TRUE" || text == "yes" || text == "YES" || text == "on" || text == "ON";
}

bool DebugNetworkLoggingEnabled() {
    static const bool enabled = IsTruthyEnv("MWB_DEBUG_NETWORK");
    return enabled;
}

bool DebugSkipIdentityEnabled() {
    static const bool enabled = IsTruthyEnv("MWB_DEBUG_SKIP_IDENTITY");
    return enabled;
}

bool DebugPacketBytesLoggingEnabled() {
    static const bool enabled = IsTruthyEnv("MWB_DEBUG_PACKET_BYTES");
    return enabled;
}

ReceivePacketStatus LastReceivePacketStatus() {
    return g_lastReceivePacketStatus;
}

const char* PackageTypeName(uint8_t type) {
    switch (static_cast<PackageType>(type)) {
        case PackageType::Handshake: return "Handshake";
        case PackageType::HandshakeAck: return "HandshakeAck";
        case PackageType::Hello: return "Hello";
        case PackageType::Heartbeat: return "Heartbeat";
        case PackageType::Mouse: return "Mouse";
        case PackageType::Keyboard: return "Keyboard";
        case PackageType::Clipboard: return "Clipboard";
        case PackageType::ClipboardPush: return "ClipboardPush";
        case PackageType::ClipboardAsk: return "ClipboardAsk";
        case PackageType::ClipboardText: return "ClipboardText";
        case PackageType::ClipboardImage: return "ClipboardImage";
        case PackageType::ClipboardDataEnd: return "ClipboardDataEnd";
        case PackageType::Awake: return "Awake";
        case PackageType::Heartbeat_ex: return "Heartbeat_ex";
        case PackageType::Heartbeat_ex_l2: return "Heartbeat_ex_l2";
        case PackageType::Heartbeat_ex_l3: return "Heartbeat_ex_l3";
        case PackageType::Heartbeat_v2: return "Heartbeat_v2";
        default: return "Unknown";
    }
}

std::string HexDump(const uint8_t* data, std::size_t length) {
    std::ostringstream stream;
    stream << std::hex << std::setfill('0');
    for (std::size_t index = 0; index < length; ++index) {
        if (index != 0) {
            if (index % 16 == 0) {
                stream << " | ";
            } else {
                stream << ' ';
            }
        }
        stream << std::setw(2) << static_cast<int>(data[index]);
    }
    return stream.str();
}

std::string extractMachineName(const MWBPacket& packet);

std::string DescribePacket(const MWBPacket& packet, bool isBig) {
    std::ostringstream stream;
    stream << PackageTypeName(packet.type)
           << "(type=" << static_cast<int>(packet.type) << ")"
           << " size=" << (isBig ? kBigPacketSize : kSmallPacketSize)
           << " id=0x" << std::hex << le32toh(packet.id)
           << " src=0x" << le32toh(packet.src)
           << " des=0x" << le32toh(packet.des)
           << " magic=0x" << static_cast<int>(packet.magic0) << static_cast<int>(packet.magic1)
           << " checksum=0x" << static_cast<int>(packet.checksum)
           << std::dec;
    if (isBig && bigPacketCarriesMachineName(packet.type)) {
        const std::string machineName = extractMachineName(packet);
        if (!machineName.empty()) {
            stream << " name=" << machineName;
        }
    }
    return stream.str();
}

bool FillRandomBytes(void* data, std::size_t size) {
    return RAND_bytes(reinterpret_cast<unsigned char*>(data), static_cast<int>(size)) == 1;
}

void handle_sig(int sig) {
    void* array[10];
    const size_t size = backtrace(array, 10);
    fprintf(stderr, "FATAL: Signal %d caught. Backtrace:\n", sig);
    backtrace_symbols_fd(array, size, STDERR_FILENO);
    _exit(1);
}

void closeSocket(int& fd) {
    if (fd >= 0) {
        shutdown(fd, SHUT_RDWR);
        close(fd);
        fd = -1;
    }
}

int AddReconnectJitter(int delayMs) {
    static thread_local std::mt19937 generator(std::random_device{}());
    const int jitterRange = std::max(1, delayMs / 5);
    std::uniform_int_distribution<int> distribution(0, jitterRange);
    return delayMs + distribution(generator);
}

bool SleepWithStop(const std::atomic<bool>& running, int delayMs) {
    const int boundedDelay = std::max(0, delayMs);
    const int sliceMs = 100;
    int remaining = boundedDelay;
    while (running && remaining > 0) {
        const int step = std::min(sliceMs, remaining);
        std::this_thread::sleep_for(std::chrono::milliseconds(step));
        remaining -= step;
    }
    return running;
}

bool writeAll(int fd, const uint8_t* data, std::size_t length) {
    std::size_t offset = 0;
    while (offset < length) {
        const ssize_t written = write(fd, data + offset, length - offset);
        if (written < 0) {
            if (errno == EINTR) {
                continue;
            }
            return false;
        }
        if (written == 0) {
            return false;
        }
        offset += static_cast<std::size_t>(written);
    }
    return true;
}

bool readExact(int fd, uint8_t* data, std::size_t length, const std::atomic<bool>& running) {
    g_lastReadStatus = ReceivePacketStatus::Ok;
    std::size_t offset = 0;
    while (offset < length && running) {
        const ssize_t count = read(fd, data + offset, length - offset);
        if (count < 0) {
            if (errno == EINTR) {
                continue;
            }
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                g_lastReadStatus = ReceivePacketStatus::Timeout;
            } else {
                g_lastReadStatus = ReceivePacketStatus::Error;
            }
            if (DebugNetworkLoggingEnabled()) {
                std::cerr << "[RECV] readExact error fd=" << fd
                          << " offset=" << offset
                          << "/" << length
                          << " errno=" << errno
                          << " (" << std::strerror(errno) << ")"
                          << std::endl;
            }
            return false;
        }
        if (count == 0) {
            g_lastReadStatus = ReceivePacketStatus::Closed;
            if (DebugNetworkLoggingEnabled()) {
                std::cerr << "[RECV] readExact EOF fd=" << fd
                          << " offset=" << offset
                          << "/" << length
                          << std::endl;
            }
            return false;
        }
        offset += static_cast<std::size_t>(count);
    }
    if (!running) {
        g_lastReadStatus = ReceivePacketStatus::Stopped;
    }
    return offset == length;
}

void ConfigureConnectedSocket(int fd) {
    struct timeval timeout;
    timeout.tv_sec = 5;
    timeout.tv_usec = 0;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout), sizeof(timeout));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&timeout), sizeof(timeout));

    int enabled = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &enabled, sizeof(enabled));
    setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &enabled, sizeof(enabled));
#ifdef TCP_NODELAY
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &enabled, sizeof(enabled));
#endif
}

std::optional<uint32_t> ResolveConfiguredHostAddress(const std::string& host) {
    struct addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;

    struct addrinfo* results = nullptr;
    if (getaddrinfo(host.c_str(), nullptr, &hints, &results) != 0 || results == nullptr) {
        return std::nullopt;
    }

    std::optional<uint32_t> resolvedAddress;
    for (const struct addrinfo* current = results; current != nullptr; current = current->ai_next) {
        if (current->ai_family != AF_INET || current->ai_addr == nullptr) {
            continue;
        }

        const auto* resolved = reinterpret_cast<const sockaddr_in*>(current->ai_addr);
        resolvedAddress = resolved->sin_addr.s_addr;
        break;
    }

    freeaddrinfo(results);
    return resolvedAddress;
}

std::optional<uint32_t> GetPeerIpv4Address(int fd) {
    if (fd < 0) {
        return std::nullopt;
    }

    sockaddr_in address{};
    socklen_t length = sizeof(address);
    if (getpeername(fd, reinterpret_cast<sockaddr*>(&address), &length) != 0 ||
        address.sin_family != AF_INET) {
        return std::nullopt;
    }

    return address.sin_addr.s_addr;
}

bool AddressMatchesConfiguredHost(
    const sockaddr_in& address,
    const std::string& host,
    uint32_t pinnedPeerAddress) {
    if (pinnedPeerAddress != 0) {
        return address.sin_addr.s_addr == pinnedPeerAddress;
    }

    const auto resolvedAddress = ResolveConfiguredHostAddress(host);
    return resolvedAddress.has_value() && *resolvedAddress == address.sin_addr.s_addr;
}

std::string FormatIpv4Address(const struct in_addr& address) {
    char buffer[INET_ADDRSTRLEN] = {};
    if (inet_ntop(AF_INET, &address, buffer, sizeof(buffer)) == nullptr) {
        return "unknown";
    }
    return buffer;
}

void populateMachineName(MWBPacket& packet, const std::string& machineName) {
    char name[32];
    std::memset(name, ' ', sizeof(name));
    const std::size_t length = std::min(machineName.size(), sizeof(name));
    std::memcpy(name, machineName.data(), length);
    std::memcpy(&packet.data[16], name, sizeof(name));
}

std::string extractMachineName(const MWBPacket& packet) {
    std::string name(reinterpret_cast<const char*>(&packet.data[16]), 32);
    while (!name.empty() && (name.back() == ' ' || name.back() == '\0')) {
        name.pop_back();
    }
    return name;
}

bool packetHasPlausibleMachineName(const MWBPacket& packet) {
    if (!bigPacketCarriesMachineName(packet.type)) {
        return true;
    }

    bool hasVisibleCharacter = false;
    for (std::size_t index = 16; index < sizeof(packet.data); ++index) {
        const uint8_t ch = packet.data[index];
        if (ch == ' ' || ch == '\0') {
            continue;
        }
        if (ch < 0x21 || ch > 0x7e) {
            return false;
        }
        hasVisibleCharacter = true;
    }
    return hasVisibleCharacter;
}

void finalizePacket(MWBPacket& packet, uint32_t magic) {
    packet.magic0 = (magic >> 16) & 0xFF;
    packet.magic1 = (magic >> 24) & 0xFF;
    packet.checksum = 0;

    uint8_t checksum = 0;
    auto* bytes = reinterpret_cast<uint8_t*>(&packet);
    for (std::size_t index = 2; index < kSmallPacketSize; ++index) {
        checksum = static_cast<uint8_t>(checksum + bytes[index]);
    }
    packet.checksum = checksum;
}

bool packetHasValidMagicAndChecksum(const std::vector<uint8_t>& plain, uint32_t magic) {
    if (plain.size() < kSmallPacketSize) {
        return false;
    }

    if (plain[2] != ((magic >> 16) & 0xFF) ||
        plain[3] != ((magic >> 24) & 0xFF)) {
        return false;
    }

    uint8_t checksum = 0;
    for (std::size_t index = 2; index < kSmallPacketSize; ++index) {
        checksum = static_cast<uint8_t>(checksum + plain[index]);
    }
    return checksum == plain[1];
}

bool sendPacketOnSocket(
    int fd,
    CryptoHelper& crypto,
    uint32_t magic,
    const std::string& machineName,
    MWBPacket packet,
    bool isBig,
    uint32_t defaultId,
    uint32_t defaultSrc,
    uint32_t defaultDes) {
    if (fd < 0) {
        return false;
    }

    if (packet.id == 0 && defaultId != 0) {
        const uint32_t value = htole32(defaultId);
        std::memcpy(&packet.id, &value, sizeof(value));
    }
    if (packet.src == 0 && defaultSrc != 0) {
        const uint32_t value = htole32(defaultSrc);
        std::memcpy(&packet.src, &value, sizeof(value));
    }
    if (packet.des == 0 && defaultDes != 0) {
        const uint32_t value = htole32(defaultDes);
        std::memcpy(&packet.des, &value, sizeof(value));
    }
    if (isBig && bigPacketCarriesMachineName(packet.type)) {
        populateMachineName(packet, machineName);
    }

    finalizePacket(packet, magic);

    std::vector<uint8_t> plain(isBig ? kBigPacketSize : kSmallPacketSize);
    std::memcpy(plain.data(), &packet, plain.size());

    if (DebugPacketBytesLoggingEnabled()) {
        std::cout << "[SEND][PLAIN] " << DescribePacket(packet, isBig)
                  << " bytes=" << HexDump(plain.data(), plain.size())
                  << std::endl;
    }

    std::vector<uint8_t> encrypted;
    if (!crypto.EncryptStream(plain, encrypted)) {
        if (DebugNetworkLoggingEnabled()) {
            std::cerr << "[SEND] EncryptStream failed for "
                      << DescribePacket(packet, isBig) << std::endl;
        }
        return false;
    }
    if (!writeAll(fd, encrypted.data(), encrypted.size())) {
        if (DebugNetworkLoggingEnabled()) {
            std::cerr << "[SEND] writeAll failed for "
                      << DescribePacket(packet, isBig) << std::endl;
        }
        return false;
    }
    return true;
}

std::optional<std::vector<uint8_t>> receivePacketOnSocket(
    int fd,
    CryptoHelper& crypto,
    uint32_t magic,
    const std::atomic<bool>& running) {
    g_lastReceivePacketStatus = ReceivePacketStatus::Error;
    uint8_t firstBlock[kSmallPacketSize];
    if (!readExact(fd, firstBlock, sizeof(firstBlock), running)) {
        g_lastReceivePacketStatus = g_lastReadStatus;
        if (DebugNetworkLoggingEnabled() && running) {
            std::cerr << "[RECV] Failed to read first packet block." << std::endl;
        }
        return std::nullopt;
    }

    std::vector<uint8_t> first(firstBlock, firstBlock + sizeof(firstBlock));
    std::vector<uint8_t> plain;
    if (!crypto.DecryptStream(first, plain)) {
        g_lastReceivePacketStatus = ReceivePacketStatus::Error;
        if (DebugNetworkLoggingEnabled()) {
            std::cerr << "[RECV] Failed to decrypt first packet block." << std::endl;
        }
        return std::nullopt;
    }
    if (plain.size() != kSmallPacketSize) {
        g_lastReceivePacketStatus = ReceivePacketStatus::Error;
        if (DebugNetworkLoggingEnabled()) {
            std::cerr << "[RECV] First packet block decrypted to unexpected size "
                      << plain.size() << std::endl;
        }
        return std::nullopt;
    }
    if (!packetHasValidMagicAndChecksum(plain, magic)) {
        const uint8_t type = plain.empty() ? 0 : plain[0];
        // PowerToys MWB sometimes uses zero magic for clipboard socket handshake packets.
        const bool isClipboardSocketHandshake = (type == static_cast<uint8_t>(PackageType::Clipboard) ||
                                                  type == static_cast<uint8_t>(PackageType::ClipboardPush) ||
                                                  type == static_cast<uint8_t>(PackageType::ClipboardAsk));
        const bool hasZeroMagic = (plain[2] == 0 && plain[3] == 0);

        if (isClipboardSocketHandshake && hasZeroMagic) {
             // Validate checksum at least
             uint8_t expectedChecksum = 0;
             for (std::size_t index = 2; index < kSmallPacketSize; ++index) {
                 expectedChecksum = static_cast<uint8_t>(expectedChecksum + plain[index]);
             }
             if (expectedChecksum == plain[1]) {
                 if (DebugNetworkLoggingEnabled()) {
                     std::cout << "[RECV] Accepting clipboard handshake packet with zero magic." << std::endl;
                 }
             } else {
                 g_lastReceivePacketStatus = ReceivePacketStatus::Error;
                 return std::nullopt;
             }
        } else {
            g_lastReceivePacketStatus = ReceivePacketStatus::Error;
            if (DebugNetworkLoggingEnabled()) {
                std::cerr << "[RECV] First packet block failed magic/checksum validation."
                          << " type=" << static_cast<int>(type)
                          << " magicBytes=0x" << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(plain[2])
                          << std::setw(2) << std::setfill('0') << static_cast<int>(plain[3])
                          << " expected=0x" << std::setw(2) << std::setfill('0') << ((magic >> 16) & 0xFF)
                          << std::setw(2) << std::setfill('0') << ((magic >> 24) & 0xFF)
                          << " checksum=0x" << std::setw(2) << std::setfill('0') << static_cast<int>(plain[1])
                          << std::dec << std::endl;
                if (DebugPacketBytesLoggingEnabled()) {
                    std::cerr << "[RECV][INVALID] bytes=" << HexDump(plain.data(), plain.size())
                              << std::endl;
                }
            }
            return std::nullopt;
        }
    }

    if (!isBigPackage(plain[0])) {
        g_lastReceivePacketStatus = ReceivePacketStatus::Ok;
        return plain;
    }

    uint8_t secondBlock[kSmallPacketSize];
    if (!readExact(fd, secondBlock, sizeof(secondBlock), running)) {
        g_lastReceivePacketStatus = g_lastReadStatus;
        if (DebugNetworkLoggingEnabled() && running) {
            std::cerr << "[RECV] Failed to read second packet block for big packet type "
                      << static_cast<int>(plain[0]) << std::endl;
        }
        return std::nullopt;
    }

    std::vector<uint8_t> second(secondBlock, secondBlock + sizeof(secondBlock));
    std::vector<uint8_t> secondPlain;
    if (!crypto.DecryptStream(second, secondPlain)) {
        g_lastReceivePacketStatus = ReceivePacketStatus::Error;
        if (DebugNetworkLoggingEnabled()) {
            std::cerr << "[RECV] Failed to decrypt second packet block for type "
                      << static_cast<int>(plain[0]) << std::endl;
        }
        return std::nullopt;
    }
    if (secondPlain.size() != kSmallPacketSize) {
        g_lastReceivePacketStatus = ReceivePacketStatus::Error;
        if (DebugNetworkLoggingEnabled()) {
            std::cerr << "[RECV] Second packet block decrypted to unexpected size "
                      << secondPlain.size()
                      << " for type " << static_cast<int>(plain[0]) << std::endl;
        }
        return std::nullopt;
    }

    plain.insert(plain.end(), secondPlain.begin(), secondPlain.end());
    MWBPacket packet;
    std::memset(&packet, 0, sizeof(packet));
    std::memcpy(&packet, plain.data(), std::min(plain.size(), sizeof(packet)));
    if (!packetHasPlausibleMachineName(packet)) {
        g_lastReceivePacketStatus = ReceivePacketStatus::Error;
        if (DebugNetworkLoggingEnabled()) {
            std::cerr << "[RECV] Packet failed machine-name plausibility check."
                      << " type=" << static_cast<int>(packet.type) << std::endl;
        }
        return std::nullopt;
    }
    g_lastReceivePacketStatus = ReceivePacketStatus::Ok;
    return plain;
}

std::optional<MWBPacket> copyPacketFromBytes(const std::vector<uint8_t>& bytes) {
    if (bytes.size() < kSmallPacketSize) {
        return std::nullopt;
    }

    MWBPacket packet;
    std::memset(&packet, 0, sizeof(packet));
    std::memcpy(&packet, bytes.data(), std::min(bytes.size(), sizeof(packet)));
    return packet;
}

bool connectToRemoteSocket(const std::string& host, int port, int& fd) {
    fd = -1;
    g_lastConnectError.clear();

    struct addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;

    struct addrinfo* results = nullptr;
    const std::string service = std::to_string(port);
    const int resolveStatus = getaddrinfo(host.c_str(), service.c_str(), &hints, &results);
    if (resolveStatus != 0 || results == nullptr) {
        if (resolveStatus != 0) {
            SetLastConnectError(std::string("name resolution failed: ") + gai_strerror(resolveStatus));
        } else {
            SetLastConnectError("name resolution returned no usable IPv4 addresses");
        }
        errno = EINVAL;
        return false;
    }

    int lastError = EINVAL;
    for (const struct addrinfo* current = results; current != nullptr; current = current->ai_next) {
        int candidate = socket(current->ai_family, current->ai_socktype, current->ai_protocol);
        if (candidate < 0) {
            lastError = errno;
            continue;
        }

        ConfigureConnectedSocket(candidate);
        if (connect(candidate, current->ai_addr, current->ai_addrlen) == 0) {
            fd = candidate;
            freeaddrinfo(results);
            return true;
        }

        lastError = errno;
        closeSocket(candidate);
    }

    freeaddrinfo(results);
    errno = lastError;
    SetLastConnectError(std::string("connect failed: ") + std::strerror(lastError));
    return false;
}

bool exchangeClipboardHandshake(
    int fd,
    CryptoHelper& crypto,
    uint32_t magic,
    const std::string& machineName,
    uint32_t machineId,
    uint32_t desId,
    const std::atomic<bool>& running,
    bool advertisePush,
    MWBPacket& remoteHeader,
    bool& remoteRequestsPush,
    std::string& remoteName) {
    if (DebugNetworkLoggingEnabled()) {
        std::cout << "[CLIPBOARD] Starting handshake, advertisePush=" << advertisePush << std::endl;
    }
    std::vector<uint8_t> noise(16);
    if (!FillRandomBytes(noise.data(), noise.size())) {
        return false;
    }

    std::vector<uint8_t> encryptedNoise;
    if (!crypto.EncryptStream(noise, encryptedNoise) ||
        !writeAll(fd, encryptedNoise.data(), encryptedNoise.size())) {
        if (DebugNetworkLoggingEnabled()) std::cerr << "[CLIPBOARD] Handshake: Failed to write noise" << std::endl;
        return false;
    }

    uint8_t peerNoise[16];
    if (!readExact(fd, peerNoise, sizeof(peerNoise), running)) {
        if (DebugNetworkLoggingEnabled()) std::cerr << "[CLIPBOARD] Handshake: Failed to read noise" << std::endl;
        return false;
    }

    std::vector<uint8_t> peerNoiseVec(peerNoise, peerNoise + sizeof(peerNoise));
    std::vector<uint8_t> ignoredNoise;
    if (!crypto.DecryptStream(peerNoiseVec, ignoredNoise)) {
        if (DebugNetworkLoggingEnabled()) std::cerr << "[CLIPBOARD] Handshake: Failed to decrypt noise" << std::endl;
        return false;
    }

    MWBPacket header;
    std::memset(&header, 0, sizeof(header));
    header.type = static_cast<uint8_t>(advertisePush ? PackageType::ClipboardPush : PackageType::Clipboard);
    const uint32_t src = htole32(machineId);
    std::memcpy(&header.src, &src, sizeof(src));
    const uint32_t des = htole32(desId != 0 ? desId : kBroadcastMachineId);
    std::memcpy(&header.des, &des, sizeof(des));
    std::memset(&header.data[0], 0, sizeof(uint32_t)); // ClipboardPostAction::Other

    if (!sendPacketOnSocket(fd, crypto, magic, machineName, header, true, 0, 0, 0)) {
        if (DebugNetworkLoggingEnabled()) std::cerr << "[CLIPBOARD] Handshake: Failed to send packet" << std::endl;
        return false;
    }

    auto packet = receivePacketOnSocket(fd, crypto, magic, running);
    if (!packet) {
        if (DebugNetworkLoggingEnabled()) std::cerr << "[CLIPBOARD] Handshake: Failed to receive packet" << std::endl;
        return false;
    }
    if (packet->size() < kBigPacketSize) {
        if (DebugNetworkLoggingEnabled()) std::cerr << "[CLIPBOARD] Handshake: Received packet too small (" << packet->size() << ")" << std::endl;
        return false;
    }

    const auto remote = copyPacketFromBytes(*packet);
    if (!remote ||
        (remote->type != static_cast<uint8_t>(PackageType::Clipboard) &&
         remote->type != static_cast<uint8_t>(PackageType::ClipboardPush))) {
        if (DebugNetworkLoggingEnabled()) {
             std::cerr << "[CLIPBOARD] Handshake: Received unexpected packet type "
                       << (remote ? static_cast<int>(remote->type) : -1) << std::endl;
        }
        return false;
    }

    remoteHeader = *remote;
    remoteRequestsPush = (remote->type == static_cast<uint8_t>(PackageType::ClipboardPush));
    remoteName = extractMachineName(*remote);
    if (DebugNetworkLoggingEnabled()) {
        std::cout << "[CLIPBOARD] Handshake success! remoteName=" << remoteName << std::endl;
    }
    return true;
}

std::vector<uint8_t> padToCipherBlock(std::vector<uint8_t> data) {
    const std::size_t remainder = data.size() % 16;
    if (remainder != 0) {
        data.resize(data.size() + (16 - remainder), 0);
    }
    return data;
}

bool receiveClipboardPayload(
    int fd,
    CryptoHelper& crypto,
    const std::atomic<bool>& running,
    std::size_t& payloadSize,
    std::string& kind,
    std::vector<uint8_t>& payload) {
    std::vector<uint8_t> encryptedHeader(kClipboardSocketHeaderSize);
    if (!readExact(fd, encryptedHeader.data(), encryptedHeader.size(), running)) {
        return false;
    }

    std::vector<uint8_t> header;
    if (!crypto.DecryptStream(encryptedHeader, header) ||
        !ClipboardManager::DecodeSocketHeader(header, payloadSize, kind)) {
        return false;
    }

    if (kind != kClipboardSocketTextLabel && kind != kClipboardSocketImageLabel) {
        return false;
    }

    const std::size_t maxPayloadSize = kClipboardSocketMaxSize;
    if (payloadSize > maxPayloadSize) {
        return false;
    }

    if (payloadSize > (std::numeric_limits<std::size_t>::max() - 15)) {
        return false;
    }

    const std::size_t paddedSize = ((payloadSize + 15) / 16) * 16;
    std::vector<uint8_t> encryptedPayload(paddedSize);
    if (!encryptedPayload.empty() &&
        !readExact(fd, encryptedPayload.data(), encryptedPayload.size(), running)) {
        return false;
    }

    if (!encryptedPayload.empty()) {
        if (!crypto.DecryptStream(encryptedPayload, payload)) {
            return false;
        }
        payload.resize(payloadSize);
    } else {
        payload.clear();
    }

    return true;
}

bool sendClipboardPayload(
    int fd,
    CryptoHelper& crypto,
    const std::vector<uint8_t>& payload,
    const std::string& kind) {
    std::vector<uint8_t> header = ClipboardManager::EncodeSocketHeader(payload.size(), kind);
    std::vector<uint8_t> encryptedHeader;
    if (!crypto.EncryptStream(header, encryptedHeader) ||
        !writeAll(fd, encryptedHeader.data(), encryptedHeader.size())) {
        return false;
    }

    std::vector<uint8_t> encryptedPayload;
    std::vector<uint8_t> paddedPayload = padToCipherBlock(payload);
    if (!paddedPayload.empty()) {
        if (!crypto.EncryptStream(paddedPayload, encryptedPayload) ||
            !writeAll(fd, encryptedPayload.data(), encryptedPayload.size())) {
            return false;
        }
    }

    return true;
}

bool isSessionControlPacket(uint8_t type) {
    return type == static_cast<uint8_t>(PackageType::Handshake) ||
           type == static_cast<uint8_t>(PackageType::HandshakeAck) ||
           type == static_cast<uint8_t>(PackageType::Hello) ||
           type == static_cast<uint8_t>(PackageType::Awake) ||
           type == static_cast<uint8_t>(PackageType::Heartbeat) ||
           type == static_cast<uint8_t>(PackageType::Heartbeat_ex) ||
           type == static_cast<uint8_t>(PackageType::Heartbeat_ex_l2) ||
           type == static_cast<uint8_t>(PackageType::Heartbeat_ex_l3);
}

} // namespace

NetworkManager::NetworkManager(const std::string& host, int port, const std::string& key)
    : m_host(host),
      m_port(port),
      m_clipboardPort(std::max(1, port + kClipboardPortOffset)),
      m_key(key),
      m_crypto(key),
      m_socket(-1) {
    std::signal(SIGSEGV, handle_sig);
    std::signal(SIGABRT, handle_sig);
    std::signal(SIGPIPE, SIG_IGN);
}

NetworkManager::~NetworkManager() {
    Stop();
    if (m_heartbeatThread.joinable()) {
        m_heartbeatThread.join();
    }
    if (m_serverThread.joinable()) {
        m_serverThread.join();
    }
    if (m_clipboardServerThread.joinable()) {
        m_clipboardServerThread.join();
    }
    if (m_netlinkThread.joinable()) {
        m_netlinkThread.join();
    }
}

void NetworkManager::Stop() {
    m_running = false;
    closeSocket(m_serverFd);
    closeSocket(m_clipboardServerFd);
    closeSocket(m_socket);
    closeSocket(m_netlinkFd);
    ShutdownSessionSockets();
    WaitForInboundSessionsToFinish();
}

void NetworkManager::WaitForInboundSessionsToFinish() {
    constexpr int kMaxWaitIterations = 300;
    for (int attempt = 0; attempt < kMaxWaitIterations; ++attempt) {
        if (m_activeServerSessions.load() == 0 &&
            m_activeClipboardSessions.load() == 0 &&
            m_activeClipboardWorkers.load() == 0) {
            return;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    std::cerr << "WARN: Timed out waiting for inbound sessions to finish." << std::endl;
}

void NetworkManager::TrackSessionSocket(int fd) {
    std::lock_guard<std::mutex> lock(m_sessionSocketMutex);
    if (fd >= 0) {
        m_sessionSockets.insert(fd);
    }
}

void NetworkManager::UntrackSessionSocket(int fd) {
    std::lock_guard<std::mutex> lock(m_sessionSocketMutex);
    m_sessionSockets.erase(fd);
}

void NetworkManager::RegisterActiveSessionPeer(uint32_t remoteMachineId) {
    if (remoteMachineId == 0) {
        return;
    }

    std::lock_guard<std::mutex> lock(m_activeSessionPeerMutex);
    ++m_activeSessionPeers[remoteMachineId];
}

void NetworkManager::UnregisterActiveSessionPeer(uint32_t remoteMachineId) {
    if (remoteMachineId == 0) {
        return;
    }

    std::lock_guard<std::mutex> lock(m_activeSessionPeerMutex);
    const auto it = m_activeSessionPeers.find(remoteMachineId);
    if (it == m_activeSessionPeers.end()) {
        return;
    }

    if (--it->second <= 0) {
        m_activeSessionPeers.erase(it);
    }
}

bool NetworkManager::HasActiveSessionPeer(uint32_t remoteMachineId) {
    if (remoteMachineId == 0) {
        return false;
    }

    std::lock_guard<std::mutex> lock(m_activeSessionPeerMutex);
    return m_activeSessionPeers.find(remoteMachineId) != m_activeSessionPeers.end();
}

bool NetworkManager::TryAcquireInboundControlSession(int fd, uint32_t remoteMachineId) {
    std::lock_guard<std::mutex> lock(m_inboundControlSessionMutex);
    if (m_activeInboundControlFd >= 0 && m_activeInboundControlFd != fd) {
        std::cerr << "[SERVER] Rejecting authenticated inbound control session for peer 0x"
                  << std::hex << remoteMachineId
                  << ": another inbound control session is already active for this host."
                  << std::dec << std::endl;
        return false;
    }

    if (DebugNetworkLoggingEnabled()) {
        std::cout << "[SERVER] Acquired inbound control session for peer 0x"
                  << std::hex << remoteMachineId << std::dec << std::endl;
    }
    m_activeInboundControlFd = fd;
    m_activeInboundControlRemoteMachineId = remoteMachineId;
    return true;
}

void NetworkManager::ReleaseInboundControlSession(int fd) {
    std::lock_guard<std::mutex> lock(m_inboundControlSessionMutex);
    if (m_activeInboundControlFd != fd) {
        return;
    }

    m_activeInboundControlFd = -1;
    m_activeInboundControlRemoteMachineId = 0;
}

void NetworkManager::ShutdownSessionSockets() {
    std::lock_guard<std::mutex> lock(m_sessionSocketMutex);
    for (int fd : m_sessionSockets) {
        if (fd >= 0) {
            shutdown(fd, SHUT_RDWR);
        }
    }
}

void NetworkManager::SetOnMouseCallback(std::function<void(const MouseData&)> cb) { m_onMouse = std::move(cb); }
void NetworkManager::SetOnKeyboardCallback(std::function<void(const KeyboardData&)> cb) { m_onKeyboard = std::move(cb); }
void NetworkManager::SetOnClipboardCallback(std::function<void(const ClipboardPayload&)> cb) { m_onClipboard = std::move(cb); }
void NetworkManager::SetOnSessionEstablished(std::function<void(const std::string&, int, const std::string&, uint32_t, uint32_t)> cb) { m_onSessionEstablished = std::move(cb); }
void NetworkManager::SetOnSessionDisconnected(std::function<void()> cb) { m_onSessionDisconnected = std::move(cb); }
void NetworkManager::SetClipboardProvider(std::function<std::optional<ClipboardPayload>()> provider) { m_clipboardProvider = std::move(provider); }

void NetworkManager::SetReconnectBackoff(int initialBackoffMs, int maxBackoffMs, int idleRetryMs) {
    const ReconnectPolicy policy = NormalizeReconnectPolicy(initialBackoffMs, maxBackoffMs, idleRetryMs);
    m_reconnectInitialBackoffMs = policy.initialBackoffMs;
    m_reconnectMaxBackoffMs = policy.maxBackoffMs;
    m_reconnectIdleRetryMs = policy.idleRetryMs;
}

void NetworkManager::SetHostResolver(std::function<std::optional<std::string>()> resolver) {
    m_hostResolver = std::move(resolver);
}

std::string NetworkManager::HostSnapshot() {
    std::lock_guard<std::mutex> lock(m_hostMutex);
    return m_host;
}

bool NetworkManager::TryRefreshHostFromResolver() {
    if (!m_hostResolver) {
        return false;
    }

    const auto resolved = m_hostResolver();
    if (!resolved || resolved->empty()) {
        return false;
    }

    {
        std::lock_guard<std::mutex> lock(m_hostMutex);
        if (m_host == *resolved) {
            return false;
        }
        std::cout << "[RECONNECT] Peer address changed to " << *resolved
                  << " via rediscovery; resetting backoff." << std::endl;
        m_host = *resolved;
    }

    if (const auto resolvedAddress = ResolveConfiguredHostAddress(*resolved); resolvedAddress.has_value()) {
        m_expectedPeerAddress = *resolvedAddress;
    } else {
        m_expectedPeerAddress = 0;
    }
    return true;
}

void NetworkManager::StartNetworkChangeWatcher() {
    if (m_netlinkThread.joinable()) {
        return;
    }

    m_netlinkFd = socket(AF_NETLINK, SOCK_RAW | SOCK_CLOEXEC, NETLINK_ROUTE);
    if (m_netlinkFd < 0) {
        std::cerr << "WARN: Network-change watcher unavailable (netlink socket failed: "
                  << std::strerror(errno) << "); falling back to backoff-only rediscovery." << std::endl;
        return;
    }

    struct sockaddr_nl addr {};
    addr.nl_family = AF_NETLINK;
    // Link up/down and IPv4/IPv6 address changes are the signals that a peer or
    // this host may have moved (DHCP lease, VPN up, resume-from-suspend).
    addr.nl_groups = RTMGRP_LINK | RTMGRP_IPV4_IFADDR | RTMGRP_IPV6_IFADDR;
    if (bind(m_netlinkFd, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) < 0) {
        std::cerr << "WARN: Network-change watcher bind failed: " << std::strerror(errno) << std::endl;
        closeSocket(m_netlinkFd);
        m_netlinkFd = -1;
        return;
    }

    m_netlinkThread = std::thread(&NetworkManager::NetworkChangeWatcherLoop, this);
}

void NetworkManager::NetworkChangeWatcherLoop() {
    char buffer[4096];
    while (m_running) {
        struct pollfd pfd {};
        pfd.fd = m_netlinkFd;
        pfd.events = POLLIN;
        const int ready = poll(&pfd, 1, 500);
        if (ready < 0) {
            if (errno == EINTR) {
                continue;
            }
            break;
        }
        if (ready == 0 || !(pfd.revents & POLLIN)) {
            continue;
        }

        const ssize_t received = recv(m_netlinkFd, buffer, sizeof(buffer), MSG_DONTWAIT);
        if (received <= 0) {
            continue;
        }

        bool relevant = false;
        int len = static_cast<int>(received);
        for (struct nlmsghdr* nh = reinterpret_cast<struct nlmsghdr*>(buffer);
             NLMSG_OK(nh, len);
             nh = NLMSG_NEXT(nh, len)) {
            if (nh->nlmsg_type == NLMSG_DONE) {
                break;
            }
            if (nh->nlmsg_type == RTM_NEWLINK || nh->nlmsg_type == RTM_DELLINK ||
                nh->nlmsg_type == RTM_NEWADDR || nh->nlmsg_type == RTM_DELADDR) {
                relevant = true;
            }
        }

        if (relevant && !m_networkChanged.exchange(true)) {
            std::cout << "[RECONNECT] Network change detected; will force immediate peer rediscovery." << std::endl;
        }
    }
}

bool NetworkManager::ReconnectSleep(int delayMs) {
    const int sliceMs = 100;
    int remaining = std::max(0, delayMs);
    while (m_running && remaining > 0) {
        // Wake early when the network topology changed so we re-resolve the peer
        // immediately instead of waiting out the backoff.
        if (m_networkChanged.load()) {
            return m_running;
        }
        const int step = std::min(sliceMs, remaining);
        std::this_thread::sleep_for(std::chrono::milliseconds(step));
        remaining -= step;
    }
    return m_running;
}

void NetworkManager::SetLocalIdentity(uint32_t machineId, const std::string& machineName) {
    if (machineId != 0) {
        m_myId = machineId;
    }
    if (!machineName.empty()) {
        m_myName = machineName;
    }
}

void NetworkManager::PrimeLocalClipboardPayload(const ClipboardPayload& payload) {
    UpdatePendingClipboardPayload(payload);
}

bool NetworkManager::Connect() {
    if (m_myName.empty()) {
        char hostname[256];
        gethostname(hostname, sizeof(hostname));
        hostname[sizeof(hostname) - 1] = '\0';
        m_myName = hostname;
    }
    if (m_myId == 0) {
        if (!FillRandomBytes(&m_myId, sizeof(m_myId))) {
            std::cerr << "ERR: Failed to generate a machine identifier." << std::endl;
            return false;
        }
        if (m_myId == 0) {
            m_myId = 0xDEAD0001;
        }
        printf("[IDENT] Permanent machine ID: %08x  name: %s\n", m_myId, m_myName.c_str());
        fflush(stdout);
    }

    if (const auto resolvedAddress = ResolveConfiguredHostAddress(m_host); resolvedAddress.has_value()) {
        m_expectedPeerAddress = *resolvedAddress;
    } else {
        m_expectedPeerAddress = 0;
    }

    m_running = true;
    StartNetworkChangeWatcher();
    StartServerListener();
    StartClipboardServerListener();
    if (m_serverFd < 0 || m_clipboardServerFd < 0) {
        Stop();
        return false;
    }
    return true;
}

bool NetworkManager::ConnectOutbound(std::array<uint8_t, 16>& handshakeChallenge) {
    closeSocket(m_socket);

    {
        std::lock_guard<std::mutex> lock(m_sendMutex);
        m_sessionId = 0;
        m_srcId = m_myId;
        m_desId = 0;
        m_handshakeDone = false;
        m_remoteName.clear();
        m_crypto.Reset();
    }

    {
        std::lock_guard<std::mutex> lock(m_clipboardMutex);
        m_inlineClipboardBuffer.clear();
        m_inlineClipboardType = 0;
        m_discardInlineClipboard = false;
    }

    const std::string host = HostSnapshot();
    if (!connectToRemoteSocket(host, m_port, m_socket)) {
        std::cerr << "[OUTBOUND] connect to " << host << ":" << m_port
                  << " failed: " << GetLastConnectError() << std::endl;
        return false;
    }

    printf("[OUTBOUND] Connected to %s:%d\n", host.c_str(), m_port);
    fflush(stdout);

    if (const auto peerAddress = GetPeerIpv4Address(m_socket); peerAddress.has_value()) {
        m_expectedPeerAddress = *peerAddress;
    }

    m_magic = m_crypto.Get24BitHash();

    std::vector<uint8_t> noise(16);
    if (!FillRandomBytes(noise.data(), noise.size())) {
        std::cerr << "ERR: Failed to generate handshake entropy." << std::endl;
        closeSocket(m_socket);
        return false;
    }
    std::vector<uint8_t> encryptedNoise;
    if (!m_crypto.EncryptStream(noise, encryptedNoise) ||
        !writeAll(m_socket, encryptedNoise.data(), encryptedNoise.size())) {
        closeSocket(m_socket);
        return false;
    }

    MWBPacket packet;
    std::memset(&packet, 0, sizeof(packet));
    packet.type = static_cast<uint8_t>(PackageType::Handshake);
    if (!FillRandomBytes(handshakeChallenge.data(), handshakeChallenge.size())) {
        std::cerr << "ERR: Failed to generate handshake payload." << std::endl;
        closeSocket(m_socket);
        return false;
    }
    std::memcpy(packet.data, handshakeChallenge.data(), handshakeChallenge.size());
    if (!SendPacket(packet, true)) {
        closeSocket(m_socket);
        return false;
    }
    return true;
}

bool NetworkManager::SendPacket(MWBPacket& packet, bool isBig) {
    std::lock_guard<std::mutex> lock(m_sendMutex);
    if (packet.id == 0) {
        uint32_t nextPacketId = m_nextPacketId.fetch_add(1);
        if (nextPacketId == 0) {
            nextPacketId = m_nextPacketId.fetch_add(1);
        }
        const uint32_t value = htole32(nextPacketId);
        std::memcpy(&packet.id, &value, sizeof(value));
    }
    return sendPacketOnSocket(
        m_socket,
        m_crypto,
        m_magic,
        m_myName,
        packet,
        isBig,
        0,
        m_srcId,
        m_desId);
}

bool NetworkManager::SendMouse(const MouseData& mouse) {
    MWBPacket packet;
    std::memset(&packet, 0, sizeof(packet));
    packet.type = static_cast<uint8_t>(PackageType::Mouse);
    std::memcpy(packet.data, &mouse, sizeof(mouse));
    return SendPacket(packet, false);
}

bool NetworkManager::SendKeyboard(const KeyboardData& keyboard) {
    MWBPacket packet;
    std::memset(&packet, 0, sizeof(packet));
    packet.type = static_cast<uint8_t>(PackageType::Keyboard);
    std::memcpy(packet.data, &keyboard, sizeof(keyboard));
    return SendPacket(packet, false);
}

void NetworkManager::SendHello() {
    if (DebugSkipIdentityEnabled()) {
        if (DebugNetworkLoggingEnabled()) {
            printf("[IDENT] Skipping hello packet due to MWB_DEBUG_SKIP_IDENTITY=1\n");
            fflush(stdout);
        }
        return;
    }

    MWBPacket hello;
    std::memset(&hello, 0, sizeof(hello));
    hello.type = static_cast<uint8_t>(PackageType::Hello);

    const uint32_t broadcast = htole32(kBroadcastMachineId);
    std::memcpy(&hello.des, &broadcast, sizeof(broadcast));

    if (DebugNetworkLoggingEnabled()) {
        printf("[IDENT] Sending hello: name=%s\n", m_myName.c_str());
        fflush(stdout);
    }
    SendPacket(hello, true);
}

void NetworkManager::SendHeartbeat(bool initial) {
    if (DebugSkipIdentityEnabled()) {
        if (DebugNetworkLoggingEnabled()) {
            printf("[IDENT] Skipping heartbeat packet due to MWB_DEBUG_SKIP_IDENTITY=1\n");
            fflush(stdout);
        }
        return;
    }

    MWBPacket heartbeat;
    std::memset(&heartbeat, 0, sizeof(heartbeat));
    heartbeat.type = static_cast<uint8_t>(
        initial ? PackageType::Heartbeat_ex : PackageType::Heartbeat);

    const uint32_t broadcast = htole32(kBroadcastMachineId);
    std::memcpy(&heartbeat.des, &broadcast, sizeof(broadcast));

    if (DebugNetworkLoggingEnabled()) {
        printf("[IDENT] Sending %s: name=%s\n",
               initial ? "heartbeat_ex (initial)" : "heartbeat",
               m_myName.c_str());
        fflush(stdout);
    }
    SendPacket(heartbeat, true);
}

void NetworkManager::SendIdentity() {
    SendHeartbeat(true);
}

void NetworkManager::SendSmallControlPacket(PackageType type) {
    MWBPacket packet;
    std::memset(&packet, 0, sizeof(packet));
    packet.type = static_cast<uint8_t>(type);
    const uint32_t broadcast = htole32(kBroadcastMachineId);
    std::memcpy(&packet.des, &broadcast, sizeof(broadcast));
    SendPacket(packet, false);
}

void NetworkManager::HeartbeatLoop() {
    while (m_running) {
        std::this_thread::sleep_for(std::chrono::seconds(2));
        if (!m_running || m_sessionId == 0) {
            continue;
        }
        SendHeartbeat();
    }
}

void NetworkManager::FinalizeInlineClipboardTransfer() {
    std::vector<uint8_t> payload;
    uint8_t type = 0;

    {
        std::lock_guard<std::mutex> lock(m_clipboardMutex);
        payload.swap(m_inlineClipboardBuffer);
        type = m_inlineClipboardType;
        m_inlineClipboardType = 0;
        m_discardInlineClipboard = false;
    }

    if (payload.empty()) {
        return;
    }

    std::thread([this, payload = std::move(payload), type]() mutable {
        if (type == static_cast<uint8_t>(PackageType::ClipboardText)) {
            const auto decoded = ClipboardManager::DecodePayload(payload);
            if (decoded.has_value()) {
                DeliverClipboardPayload(*decoded);
            } else {
                std::cerr << "WARN: Failed to decode inline clipboard text payload." << std::endl;
            }
            return;
        }

        if (type == static_cast<uint8_t>(PackageType::ClipboardImage)) {
            auto imageBytes = ClipboardManager::DecodeImagePayload(payload);
            if (imageBytes.has_value()) {
                // PowerToys often pads images with null bytes up to the next block size.
                while (!imageBytes->empty() && imageBytes->back() == 0) {
                    imageBytes->pop_back();
                }
                DeliverClipboardPayload(ClipboardManager::MakeImagePayload("image/png", std::move(*imageBytes)));
            } else {
                std::cerr << "WARN: Failed to decode inline clipboard image payload." << std::endl;
            }
        }
    }).detach();
}

void NetworkManager::DeliverClipboardPayload(const ClipboardPayload& payload) {
    {
        std::lock_guard<std::mutex> lock(m_clipboardMutex);
        m_suppressedClipboardPayload = payload;
        m_pendingClipboardPayloadStruct = payload;
        if (payload.image) {
            m_pendingClipboardPayload = ClipboardManager::EncodeImagePayload(payload.image->bytes);
        } else if (payload.plainText) {
            m_pendingClipboardPayload = ClipboardManager::EncodeTextPayload(*payload.plainText);
        }
    }

    if (m_onClipboard) {
        m_onClipboard(payload);
    }
}

bool NetworkManager::UpdatePendingClipboardPayload(const ClipboardPayload& payload) {
    std::lock_guard<std::mutex> lock(m_clipboardMutex);

    if (!payload.plainText && !payload.image) {
        m_pendingClipboardPayloadStruct.reset();
        m_pendingClipboardPayload.clear();
        return false;
    }

    auto payloadsEqual = [](const ClipboardPayload& a, const ClipboardPayload& b) {
        if (a.plainText != b.plainText) return false;
        if (a.image.has_value() != b.image.has_value()) return false;
        if (a.image && b.image && a.image->bytes != b.image->bytes) return false;
        return true;
    };

    if (m_suppressedClipboardPayload && payloadsEqual(*m_suppressedClipboardPayload, payload)) {
        m_suppressedClipboardPayload.reset();
        return false;
    }

    if (m_pendingClipboardPayloadStruct && payloadsEqual(*m_pendingClipboardPayloadStruct, payload)) {
        return false;
    }

    m_pendingClipboardPayloadStruct = payload;
    if (payload.image) {
        m_pendingClipboardPayload = ClipboardManager::EncodeImagePayload(payload.image->bytes);
    } else if (payload.plainText) {
        m_pendingClipboardPayload = ClipboardManager::EncodeTextPayload(*payload.plainText);
    }
    return true;
}

std::optional<std::vector<uint8_t>> NetworkManager::SnapshotClipboardPayload(bool refreshFromProvider) {
    std::lock_guard<std::mutex> lock(m_clipboardMutex);

    if (refreshFromProvider && m_clipboardProvider) {
        auto payload = m_clipboardProvider();
        if (payload.has_value()) {
            // Re-using logic from UpdatePendingClipboardPayload essentially
            // but we can't call it while holding the lock if it were to use the lock.
            // Let's just do it manually here.

            auto payloadsEqual = [](const ClipboardPayload& a, const ClipboardPayload& b) {
                if (a.plainText != b.plainText) return false;
                if (a.image.has_value() != b.image.has_value()) return false;
                if (a.image && b.image && a.image->bytes != b.image->bytes) return false;
                return true;
            };

            if (m_suppressedClipboardPayload && payloadsEqual(*m_suppressedClipboardPayload, *payload)) {
                m_suppressedClipboardPayload.reset();
                return std::nullopt;
            }

            if (!m_pendingClipboardPayloadStruct || !payloadsEqual(*m_pendingClipboardPayloadStruct, *payload)) {
                m_pendingClipboardPayloadStruct = payload;
                if (payload->image) {
                    m_pendingClipboardPayload = ClipboardManager::EncodeImagePayload(payload->image->bytes);
                } else if (payload->plainText) {
                    m_pendingClipboardPayload = ClipboardManager::EncodeTextPayload(*payload->plainText);
                }
            }
        }
    }

    if (m_pendingClipboardPayload.empty() || (m_pendingClipboardPayloadStruct && m_pendingClipboardPayloadStruct->image)) {
        // Text/HTML snapshot doesn't return image payload
        return std::nullopt;
    }
    return m_pendingClipboardPayload;
}

std::optional<std::vector<uint8_t>> NetworkManager::SnapshotClipboardImagePayload(bool refreshFromProvider) {
    std::lock_guard<std::mutex> lock(m_clipboardMutex);

    if (refreshFromProvider && m_clipboardProvider) {
        auto payload = m_clipboardProvider();
        if (payload.has_value()) {
            auto payloadsEqual = [](const ClipboardPayload& a, const ClipboardPayload& b) {
                if (a.plainText != b.plainText) return false;
                if (a.image.has_value() != b.image.has_value()) return false;
                if (a.image && b.image && a.image->bytes != b.image->bytes) return false;
                return true;
            };

            if (m_suppressedClipboardPayload && payloadsEqual(*m_suppressedClipboardPayload, *payload)) {
                m_suppressedClipboardPayload.reset();
                return std::nullopt;
            }

            if (!m_pendingClipboardPayloadStruct || !payloadsEqual(*m_pendingClipboardPayloadStruct, *payload)) {
                m_pendingClipboardPayloadStruct = payload;
                if (payload->image) {
                    m_pendingClipboardPayload = ClipboardManager::EncodeImagePayload(payload->image->bytes);
                } else if (payload->plainText) {
                    m_pendingClipboardPayload = ClipboardManager::EncodeTextPayload(*payload->plainText);
                }
            }
        }
    }

    if (m_pendingClipboardPayload.empty() || !m_pendingClipboardPayloadStruct || !m_pendingClipboardPayloadStruct->image) {
        return std::nullopt;
    }
    return m_pendingClipboardPayload;
}


bool NetworkManager::SendClipboardAnnouncement() {
    MWBPacket packet;
    std::memset(&packet, 0, sizeof(packet));
    packet.type = static_cast<uint8_t>(PackageType::Clipboard);
    const uint32_t broadcast = htole32(kBroadcastMachineId);
    std::memcpy(&packet.des, &broadcast, sizeof(broadcast));
    return SendPacket(packet, true);
}

bool NetworkManager::SendClipboardAsk() {
    MWBPacket packet;
    std::memset(&packet, 0, sizeof(packet));
    packet.type = static_cast<uint8_t>(PackageType::ClipboardAsk);
    const uint32_t destination = htole32(m_desId != 0 ? m_desId : kBroadcastMachineId);
    std::memcpy(&packet.des, &destination, sizeof(destination));
    return SendPacket(packet, true);
}

bool NetworkManager::SendInlineClipboardText(const std::vector<uint8_t>& payload) {
    if (!m_handshakeDone || payload.empty()) {
        return false;
    }

    for (std::size_t index = 0; index < payload.size(); index += kClipboardChunkSize) {
        MWBPacket packet;
        std::memset(&packet, 0, sizeof(packet));
        packet.type = static_cast<uint8_t>(PackageType::ClipboardText);

        const uint32_t broadcast = htole32(kBroadcastMachineId);
        std::memcpy(&packet.des, &broadcast, sizeof(broadcast));

        const std::size_t chunkSize = std::min(kClipboardChunkSize, payload.size() - index);
        std::memcpy(packet.data, payload.data() + index, chunkSize);
        if (!SendPacket(packet, true)) {
            return false;
        }
    }

    MWBPacket endPacket;
    std::memset(&endPacket, 0, sizeof(endPacket));
    endPacket.type = static_cast<uint8_t>(PackageType::ClipboardDataEnd);
    const uint32_t broadcast = htole32(kBroadcastMachineId);
    std::memcpy(&endPacket.des, &broadcast, sizeof(broadcast));
    return SendPacket(endPacket, true);
}

bool NetworkManager::SendInlineClipboardImage(const std::vector<uint8_t>& payload) {
    if (!m_handshakeDone || payload.empty()) {
        return false;
    }

    for (std::size_t index = 0; index < payload.size(); index += kClipboardChunkSize) {
        MWBPacket packet;
        std::memset(&packet, 0, sizeof(packet));
        packet.type = static_cast<uint8_t>(PackageType::ClipboardImage);

        const uint32_t broadcast = htole32(kBroadcastMachineId);
        std::memcpy(&packet.des, &broadcast, sizeof(broadcast));

        const std::size_t chunkSize = std::min(kClipboardChunkSize, payload.size() - index);
        std::memcpy(packet.data, payload.data() + index, chunkSize);
        if (!SendPacket(packet, true)) {
            return false;
        }
    }

    MWBPacket endPacket;
    std::memset(&endPacket, 0, sizeof(endPacket));
    endPacket.type = static_cast<uint8_t>(PackageType::ClipboardDataEnd);
    const uint32_t broadcast = htole32(kBroadcastMachineId);
    std::memcpy(&endPacket.des, &broadcast, sizeof(broadcast));
    return SendPacket(endPacket, true);
}

void NetworkManager::NotifyLocalClipboardChanged() {
    if (!m_handshakeDone) {
        return;
    }

    if (m_clipboardProvider) {
        auto payload = m_clipboardProvider();
        if (payload.has_value()) {
            NotifyLocalClipboardChanged(*payload);
        }
    }
}

void NetworkManager::NotifyLocalClipboardChanged(const ClipboardPayload& payload) {
    if (!UpdatePendingClipboardPayload(payload)) {
        return;
    }

    if (!m_handshakeDone) {
        return;
    }

    if (payload.image) {
        const auto encoded = SnapshotClipboardImagePayload(false);
        if (encoded.has_value() && encoded->size() <= kClipboardInlineMaxSize && SendInlineClipboardImage(*encoded)) {
             std::cout << "[CLIPBOARD] Sent inline image payload (" << encoded->size() << " bytes)" << std::endl;
             return;
        }
    } else {
        const auto encoded = SnapshotClipboardPayload(false);
        if (encoded.has_value() && encoded->size() <= kClipboardInlineMaxSize && SendInlineClipboardText(*encoded)) {
            std::cout << "[CLIPBOARD] Sent inline text payload (" << encoded->size() << " bytes compressed)" << std::endl;
            return;
        }
    }

    if (SendClipboardAnnouncement()) {
        std::cout << "[CLIPBOARD] Advertised clipboard payload for socket transfer" << std::endl;
    }
}

void NetworkManager::HandleClipboardControlPacket(const MWBPacket& packet, uint32_t remoteMachineId) {
    const uint32_t destination = le32toh(packet.des);
    if (!packetTargetsMachine(destination, m_myId)) {
        return;
    }

    switch (static_cast<PackageType>(packet.type)) {
        case PackageType::ClipboardText:
        case PackageType::ClipboardImage: {
            std::lock_guard<std::mutex> lock(m_clipboardMutex);
            if (m_inlineClipboardType != 0 && m_inlineClipboardType != packet.type) {
                m_inlineClipboardBuffer.clear();
                m_discardInlineClipboard = false;
            }
            m_inlineClipboardType = packet.type;
            if (m_discardInlineClipboard) {
                break;
            }
            if (m_inlineClipboardBuffer.size() + kClipboardChunkSize > kClipboardInlineMaxSize) {
                m_inlineClipboardBuffer.clear();
                m_discardInlineClipboard = true;
                std::cerr << "WARN: Dropping oversized inline clipboard payload." << std::endl;
                break;
            }
            m_inlineClipboardBuffer.insert(
                m_inlineClipboardBuffer.end(),
                packet.data,
                packet.data + kClipboardChunkSize);
            break;
        }
        case PackageType::ClipboardDataEnd: {
            bool discard = false;
            {
                std::lock_guard<std::mutex> lock(m_clipboardMutex);
                discard = m_discardInlineClipboard;
                if (discard) {
                    m_inlineClipboardBuffer.clear();
                    m_inlineClipboardType = 0;
                    m_discardInlineClipboard = false;
                }
            }
            if (discard) {
                break;
            }
            FinalizeInlineClipboardTransfer();
            break;
        }
        case PackageType::Clipboard:
        case PackageType::ClipboardPush:
            if (!m_clipboardPullActive.exchange(true)) {
                m_activeClipboardWorkers.fetch_add(1);
                std::thread([this, remoteMachineId]() {
                    struct WorkerGuard {
                        std::atomic<int>& counter;
                        std::atomic<bool>& active;
                        ~WorkerGuard() {
                            active = false;
                            counter.fetch_sub(1);
                        }
                    } guard{m_activeClipboardWorkers, m_clipboardPullActive};
                    RequestRemoteClipboard(remoteMachineId);
                }).detach();
            }
            break;
        case PackageType::ClipboardAsk:
            if (!m_clipboardPushActive.exchange(true)) {
                m_activeClipboardWorkers.fetch_add(1);
                std::thread([this, remoteMachineId]() {
                    struct WorkerGuard {
                        std::atomic<int>& counter;
                        std::atomic<bool>& active;
                        ~WorkerGuard() {
                            active = false;
                            counter.fetch_sub(1);
                        }
                    } guard{m_activeClipboardWorkers, m_clipboardPushActive};
                    PushClipboardToRemote(remoteMachineId);
                }).detach();
            }
            break;
        default:
            break;
    }
}

void NetworkManager::RequestRemoteClipboard(uint32_t expectedRemoteMachineId) {
    if (expectedRemoteMachineId == 0) {
        std::cerr << "WARN: Clipboard pull requested without an active authenticated peer." << std::endl;
        return;
    }

    int fd = -1;
    if (!connectToRemoteSocket(HostSnapshot(), m_clipboardPort, fd)) {
        std::cerr << "WARN: Failed to connect to remote clipboard socket on port " << m_clipboardPort
                  << ": " << GetLastConnectError() << std::endl;
        return;
    }

    CryptoHelper crypto(m_key);
    const uint32_t magic = 0; // PowerToys often uses zero magic for secondary clipboard socket
    bool remotePush = false;
    std::string remoteName;
    MWBPacket remoteHeader;
    std::memset(&remoteHeader, 0, sizeof(remoteHeader));
    if (!exchangeClipboardHandshake(fd, crypto, magic, m_myName, m_myId, expectedRemoteMachineId, m_running, false, remoteHeader, remotePush, remoteName)) {
        std::cerr << "WARN: Clipboard pull handshake failed." << std::endl;
        closeSocket(fd);
        return;
    }

    const uint32_t remoteSource = le32toh(remoteHeader.src);
    const uint32_t remoteDestination = le32toh(remoteHeader.des);
    if (remoteSource != expectedRemoteMachineId ||
        !packetTargetsMachine(remoteDestination, m_myId)) {
        std::cerr << "WARN: Rejecting clipboard pull handshake from unexpected remote peer 0x"
                  << std::hex << remoteSource << std::dec << std::endl;
        closeSocket(fd);
        return;
    }

    std::size_t payloadSize = 0;
    std::string kind;
    std::vector<uint8_t> payload;
    if (!receiveClipboardPayload(fd, crypto, m_running, payloadSize, kind, payload)) {
        std::cerr << "WARN: Clipboard pull failed after handshake." << std::endl;
        closeSocket(fd);
        return;
    }

    closeSocket(fd);

    if (kind == kClipboardSocketTextLabel) {
        const auto decoded = ClipboardManager::DecodePayload(payload);
        if (decoded.has_value()) {
            std::cout << "[CLIPBOARD] Pulled text payload (" << payloadSize << " bytes compressed)" << std::endl;
            DeliverClipboardPayload(*decoded);
        } else {
            std::cerr << "WARN: Failed to decode socket clipboard text payload." << std::endl;
        }
        return;
    }

    if (kind == kClipboardSocketImageLabel) {
        auto imageBytes = ClipboardManager::DecodeImagePayload(payload);
        if (imageBytes.has_value()) {
            while (!imageBytes->empty() && imageBytes->back() == 0) {
                imageBytes->pop_back();
            }
            std::cout << "[CLIPBOARD] Pulled image payload (" << payloadSize << " bytes, trimmed to " << imageBytes->size() << ")" << std::endl;
            DeliverClipboardPayload(ClipboardManager::MakeImagePayload("image/png", std::move(*imageBytes)));
        } else {
            std::cerr << "WARN: Failed to decode socket clipboard image payload." << std::endl;
        }
        return;
    }

    std::cerr << "WARN: Unsupported clipboard socket payload kind '" << kind << "'." << std::endl;
}

void NetworkManager::PushClipboardToRemote(uint32_t expectedRemoteMachineId) {
    if (expectedRemoteMachineId == 0) {
        std::cerr << "WARN: Clipboard push requested without an active authenticated peer." << std::endl;
        return;
    }

    std::optional<std::vector<uint8_t>> payload;
    std::string kind;

    {
        std::lock_guard<std::mutex> lock(m_clipboardMutex);
        if (m_pendingClipboardPayloadStruct && m_pendingClipboardPayloadStruct->image) {
            payload = SnapshotClipboardImagePayload(true);
            kind = kClipboardSocketImageLabel;
        } else {
            payload = SnapshotClipboardPayload(true);
            kind = kClipboardSocketTextLabel;
        }
    }

    if (!payload.has_value()) {
        return;
    }

    int fd = -1;
    if (!connectToRemoteSocket(HostSnapshot(), m_clipboardPort, fd)) {
        std::cerr << "WARN: Failed to connect to remote clipboard socket on port " << m_clipboardPort
                  << ": " << GetLastConnectError() << std::endl;
        return;
    }

    CryptoHelper crypto(m_key);
    const uint32_t magic = 0; // PowerToys often uses zero magic for secondary clipboard socket
    bool remotePush = false;
    std::string remoteName;
    MWBPacket remoteHeader;
    std::memset(&remoteHeader, 0, sizeof(remoteHeader));
    if (!exchangeClipboardHandshake(fd, crypto, magic, m_myName, m_myId, expectedRemoteMachineId, m_running, true, remoteHeader, remotePush, remoteName)) {
        std::cerr << "WARN: Clipboard push handshake failed." << std::endl;
        closeSocket(fd);
        return;
    }

    const uint32_t remoteSource = le32toh(remoteHeader.src);
    const uint32_t remoteDestination = le32toh(remoteHeader.des);
    if (remoteSource != expectedRemoteMachineId ||
        !packetTargetsMachine(remoteDestination, m_myId)) {
        std::cerr << "WARN: Rejecting clipboard push handshake from unexpected remote peer 0x"
                  << std::hex << remoteSource << std::dec << std::endl;
        closeSocket(fd);
        return;
    }

    if (sendClipboardPayload(fd, crypto, *payload, kind)) {
        std::cout << "[CLIPBOARD] Pushed " << kind << " payload over clipboard socket (" << payload->size() << " bytes)" << std::endl;
    } else {
        std::cerr << "WARN: Failed to send clipboard payload over clipboard socket." << std::endl;
    }

    closeSocket(fd);
}

void NetworkManager::RunLoop() {
    m_heartbeatThread = std::thread(&NetworkManager::HeartbeatLoop, this);
    ReconnectPolicy reconnectPolicy =
        NormalizeReconnectPolicy(m_reconnectInitialBackoffMs, m_reconnectMaxBackoffMs, m_reconnectIdleRetryMs);
    ReconnectState reconnectState = InitialReconnectState(reconnectPolicy);
    bool idleLogged = false;
    while (m_running) {
        if (!m_autoConnectEnabled.load()) {
            closeSocket(m_socket);
            {
                std::lock_guard<std::mutex> lock(m_sendMutex);
                m_sessionId = 0;
                m_srcId = m_myId;
                m_desId = 0;
                m_handshakeDone = false;
                m_remoteName.clear();
                m_crypto.Reset();
            }

            if (!idleLogged) {
                std::cout << "[CONNECT] Auto-connect disabled; waiting for inbound sessions only." << std::endl;
                idleLogged = true;
            }
            if (!SleepWithStop(m_running, 500)) {
                break;
            }
            continue;
        }

        idleLogged = false;
        // Event-driven fast path: a netlink link/address change (DHCP lease, VPN
        // up, resume-from-suspend) means the peer may have moved. Re-resolve
        // immediately and reset backoff rather than waiting it out.
        if (m_socket < 0 && m_networkChanged.exchange(false)) {
            std::cout << "[RECONNECT] Acting on network change: forcing peer rediscovery." << std::endl;
            TryRefreshHostFromResolver();
            reconnectState = InitialReconnectState(reconnectPolicy);
        }
        std::string remoteName;
        std::array<uint8_t, 16> outboundChallenge{};
        if (m_socket < 0) {
            // Peer has been unreachable long enough that backoff saturated into
            // the idle state. Before the next slow retry, re-resolve the peer
            // address once (LAN discovery) in case its IP changed. The idle
            // cadence rate-limits this, so we never scan while connected or
            // during the initial fast-retry burst.
            if (reconnectState.useIdleRetry && TryRefreshHostFromResolver()) {
                reconnectState = InitialReconnectState(reconnectPolicy);
            }
            if (!ConnectOutbound(outboundChallenge)) {
                const int scheduledBackoffMs = ScheduledReconnectDelayMs(reconnectPolicy, reconnectState);
                const int delayMs = AddReconnectJitter(scheduledBackoffMs);
                std::cout << "[RECONNECT] Peer unavailable. Retrying in " << delayMs << " ms." << std::endl;
                reconnectState = AdvanceReconnectAfterFailure(reconnectPolicy, reconnectState);
                if (!ReconnectSleep(delayMs)) {
                    break;
                }
                continue;
            }
            // Do NOT reset reconnectState here; wait for handshake success
        }

        uint8_t serverNoise[16];
        if (!readExact(m_socket, serverNoise, sizeof(serverNoise), m_running)) {
            if (m_running) {
                fprintf(stderr, "[OUTBOUND] Failed to receive server noise\n");
            }
            closeSocket(m_socket);

            const int scheduledBackoffMs = ScheduledReconnectDelayMs(reconnectPolicy, reconnectState);
            const int delayMs = AddReconnectJitter(scheduledBackoffMs);
            std::cout << "[RECONNECT] Protocol error (noise). Retrying in " << delayMs << " ms." << std::endl;
            reconnectState = AdvanceReconnectAfterFailure(reconnectPolicy, reconnectState);
            if (!ReconnectSleep(delayMs)) {
                break;
            }
            continue;
        }

        std::vector<uint8_t> encryptedNoise(serverNoise, serverNoise + sizeof(serverNoise));
        std::vector<uint8_t> ignoredNoise;
        if (!m_crypto.DecryptStream(encryptedNoise, ignoredNoise)) {
            fprintf(stderr, "[OUTBOUND] Failed to decrypt server noise\n");
            closeSocket(m_socket);

            const int scheduledBackoffMs = ScheduledReconnectDelayMs(reconnectPolicy, reconnectState);
            const int delayMs = AddReconnectJitter(scheduledBackoffMs);
            std::cout << "[RECONNECT] Crypto error (noise). Retrying in " << delayMs << " ms." << std::endl;
            reconnectState = AdvanceReconnectAfterFailure(reconnectPolicy, reconnectState);
            if (!ReconnectSleep(delayMs)) {
                break;
            }
            continue;
        }

        printf("[OUTBOUND] Server noise received, starting handshake\n");
        fflush(stdout);

        for (int attempt = 0; attempt < 20 && m_running && !m_handshakeDone; ++attempt) {
            const auto packetBytes = receivePacketOnSocket(m_socket, m_crypto, m_magic, m_running);
            if (!packetBytes || packetBytes->size() < kSmallPacketSize) {
                if (m_running) {
                    std::cerr << "[OUTBOUND] Handshake receive failed before a valid packet arrived."
                              << " packetBytes=" << (packetBytes ? packetBytes->size() : 0)
                              << std::endl;
                }
                break;
            }

            const auto copiedPacket = copyPacketFromBytes(*packetBytes);
            if (!copiedPacket) {
                std::cerr << "[OUTBOUND] Received malformed handshake packet frame." << std::endl;
                break;
            }

            const MWBPacket& packet = *copiedPacket;
            const uint32_t sid = le32toh(packet.id);
            const uint32_t source = le32toh(packet.src);
            const uint32_t destination = le32toh(packet.des);

            if (packetBytes->size() >= kBigPacketSize && bigPacketCarriesMachineName(packet.type)) {
                const std::string name = extractMachineName(packet);
                if (!name.empty()) {
                    remoteName = name;
                }
            }

            if (packet.type == static_cast<uint8_t>(PackageType::Handshake)) {
                if (DebugNetworkLoggingEnabled() && !packetTargetsMachine(destination, m_myId)) {
                    std::cerr << "[OUTBOUND] Accepting Windows handshake with non-local destination 0x"
                              << std::hex << destination
                              << " while keeping stable local id 0x" << m_myId
                              << std::dec << std::endl;
                }

                MWBPacket ack;
                std::memset(&ack, 0, sizeof(ack));
                ack.type = static_cast<uint8_t>(PackageType::HandshakeAck);
                for (int index = 0; index < 16; ++index) {
                    ack.data[index] = static_cast<uint8_t>(~packet.data[index]);
                }
                SendPacket(ack, true);
            } else if (handshakeAckMatchesChallenge(packet, outboundChallenge) &&
                       packetTargetsMachine(destination, m_myId)) {
                if (sid != 0) {
                    m_sessionId = sid;
                }
                if (source != 0) {
                    m_desId = source;
                }
                m_handshakeDone = true;
            } else if (packet.type == static_cast<uint8_t>(PackageType::HandshakeAck)) {
                std::size_t mismatchIndex = outboundChallenge.size();
                for (std::size_t index = 0; index < outboundChallenge.size(); ++index) {
                    if (packet.data[index] != static_cast<uint8_t>(~outboundChallenge[index])) {
                        mismatchIndex = index;
                        break;
                    }
                }
                std::cerr << "[OUTBOUND] Ignoring handshake ack."
                          << " targetMatch=" << (packetTargetsMachine(destination, m_myId) ? "yes" : "no")
                          << " sid=0x" << std::hex << sid
                          << " src=0x" << source
                          << " des=0x" << destination
                          << " myId=0x" << m_myId
                          << std::dec;
                if (mismatchIndex < outboundChallenge.size()) {
                    std::cerr << " mismatchByte=" << mismatchIndex;
                } else {
                    std::cerr << " mismatchByte=none";
                }
                std::cerr << std::endl;
            } else if (packet.type == static_cast<uint8_t>(PackageType::Heartbeat_ex) ||
                       packet.type == static_cast<uint8_t>(PackageType::Heartbeat_ex_l2)) {
                if (packetTargetsMachine(destination, m_myId) && source != 0) {
                    if (sid != 0) {
                        m_sessionId = sid;
                    }
                    m_desId = source;
                }
            } else {
                std::cerr << "[OUTBOUND] Ignoring non-handshake packet during handshake."
                          << " type=" << static_cast<int>(packet.type)
                          << " sid=0x" << std::hex << sid
                          << " src=0x" << source
                          << " des=0x" << destination
                          << " myId=0x" << m_myId
                          << std::dec << std::endl;
            }
        }

        if (!m_handshakeDone) {
            fprintf(stderr, "[OUTBOUND] Handshake failed, will reconnect\n");
            closeSocket(m_socket);

            const int scheduledBackoffMs = ScheduledReconnectDelayMs(reconnectPolicy, reconnectState);
            const int delayMs = AddReconnectJitter(scheduledBackoffMs);
            std::cout << "[RECONNECT] Handshake failed. Retrying in " << delayMs << " ms." << std::endl;
            reconnectState = AdvanceReconnectAfterFailure(reconnectPolicy, reconnectState);
            if (!ReconnectSleep(delayMs)) {
                break;
            }
            continue;
        }

        printf("[SUCCESS] MWB Session Established. Entering Main Loop.\n");
        fflush(stdout);
        reconnectState = ResetReconnectAfterSuccess(reconnectPolicy);
        if (!remoteName.empty()) {
            m_remoteName = remoteName;
        }
        RegisterActiveSessionPeer(m_desId);
        if (m_onSessionEstablished) {
            std::string connectedHost = m_host;
            if (const auto peerAddress = GetPeerIpv4Address(m_socket); peerAddress.has_value()) {
                struct in_addr address {};
                address.s_addr = *peerAddress;
                connectedHost = FormatIpv4Address(address);
            }
            m_onSessionEstablished(connectedHost, m_port, m_remoteName, m_desId, m_myId);
        }
        SendHello();
        SendIdentity();

        while (m_running) {
            const auto packetBytes = receivePacketOnSocket(m_socket, m_crypto, m_magic, m_running);
            if (!packetBytes || packetBytes->size() < kSmallPacketSize) {
                if (LastReceivePacketStatus() == ReceivePacketStatus::Timeout) {
                    continue;
                }
                if (m_running) {
                    fprintf(stderr, "[OUTBOUND] recvP failed, connection lost\n");
                }
                break;
            }

            const auto copiedPacket = copyPacketFromBytes(*packetBytes);
            if (!copiedPacket) {
                break;
            }

            const MWBPacket& packet = *copiedPacket;
            const uint8_t type = packet.type;
            const uint32_t sid = le32toh(packet.id);
            const uint32_t source = le32toh(packet.src);
            const uint32_t destination = le32toh(packet.des);
            const bool targetsActiveSession =
                packetTargetsEstablishedSession(destination, source, m_myId, m_desId);

            if (DebugNetworkLoggingEnabled()) {
                std::cout << "[OUTBOUND][PKT] type=" << static_cast<int>(type)
                          << " sid=0x" << std::hex << sid
                          << " src=0x" << source
                          << " des=0x" << destination
                          << " myId=0x" << m_myId
                          << " peerId=0x" << m_desId
                          << std::dec
                          << " targetsActiveSession=" << (targetsActiveSession ? "yes" : "no")
                          << std::endl;
            }

            if (sid != 0 && (isSessionControlPacket(type) || targetsActiveSession)) {
                m_sessionId = sid;
            }
            if (source != 0 && isSessionControlPacket(type)) {
                if (m_desId != 0 && m_desId != source) {
                    UnregisterActiveSessionPeer(m_desId);
                    RegisterActiveSessionPeer(source);
                }
                m_desId = source;
            }
            if (packetBytes->size() >= kBigPacketSize && bigPacketCarriesMachineName(type)) {
                const std::string name = extractMachineName(packet);
                if (!name.empty()) {
                    m_remoteName = name;
                }
            }

            if (type == static_cast<uint8_t>(PackageType::Mouse) &&
                targetsActiveSession) {
                MouseData mouse;
                std::memcpy(&mouse, &packet.data[0], sizeof(mouse));
                if (isRecognizedMouseMessage(mouse.wParam) && m_onMouse) {
                    m_onMouse(mouse);
                } else if (!isRecognizedMouseMessage(mouse.wParam)) {
                    std::cerr << "[OUTBOUND] Dropping mouse packet with unknown message 0x"
                              << std::hex << mouse.wParam << std::dec << std::endl;
                }
            } else if (type == static_cast<uint8_t>(PackageType::Keyboard) &&
                       targetsActiveSession) {
                KeyboardData keyboard;
                std::memcpy(&keyboard, &packet.data[8], sizeof(keyboard));
                if (hasOnlyKnownKeyboardFlags(keyboard.flags) && m_onKeyboard) {
                    m_onKeyboard(keyboard);
                } else if (!hasOnlyKnownKeyboardFlags(keyboard.flags)) {
                    std::cerr << "[OUTBOUND] Dropping keyboard packet with invalid flags 0x"
                              << std::hex << keyboard.flags << std::dec << std::endl;
                }
            } else if (type == static_cast<uint8_t>(PackageType::Handshake) &&
                       packetTargetsMachine(destination, m_myId)) {
                MWBPacket ack;
                std::memset(&ack, 0, sizeof(ack));
                ack.type = static_cast<uint8_t>(PackageType::HandshakeAck);
                for (int index = 0; index < 16; ++index) {
                    ack.data[index] = static_cast<uint8_t>(~packet.data[index]);
                }
                SendPacket(ack, true);
            } else if ((type == static_cast<uint8_t>(PackageType::Hello) ||
                        type == static_cast<uint8_t>(PackageType::Awake)) &&
                       packetTargetsMachine(destination, m_myId)) {
                if (source != 0) {
                    m_desId = source;
                }
                SendHeartbeat();
            } else if (type == static_cast<uint8_t>(PackageType::Heartbeat) ||
                       type == static_cast<uint8_t>(PackageType::Heartbeat_ex)) {
                if (packetTargetsMachine(destination, m_myId) && source != 0) {
                    m_desId = source;
                    SendSmallControlPacket(PackageType::Heartbeat_ex_l2);
                }
            } else if (type == static_cast<uint8_t>(PackageType::Heartbeat_ex_l2)) {
                if (packetTargetsMachine(destination, m_myId) && source != 0) {
                    m_desId = source;
                    SendSmallControlPacket(PackageType::Heartbeat_ex_l3);
                }
            } else if (type == static_cast<uint8_t>(PackageType::Heartbeat_ex_l3)) {
                if (packetTargetsMachine(destination, m_myId) && source != 0) {
                    m_desId = source;
                }
            } else if (type == static_cast<uint8_t>(PackageType::Clipboard) ||
                       type == static_cast<uint8_t>(PackageType::ClipboardAsk) ||
                       type == static_cast<uint8_t>(PackageType::ClipboardPush) ||
                       type == static_cast<uint8_t>(PackageType::ClipboardText) ||
                       type == static_cast<uint8_t>(PackageType::ClipboardImage) ||
                       type == static_cast<uint8_t>(PackageType::ClipboardDataEnd)) {
                if (targetsActiveSession) {
                    HandleClipboardControlPacket(packet, source);
                }
            } else if (DebugNetworkLoggingEnabled()) {
                printf("[PKT] type=%d sid=%08x src=%08x des=%08x\n", type, sid, source, destination);
                fflush(stdout);
            }
        }

        const uint32_t disconnectedRemoteMachineId = m_desId;
        closeSocket(m_socket);
        UnregisterActiveSessionPeer(disconnectedRemoteMachineId);
        if (m_onSessionDisconnected) {
            m_onSessionDisconnected();
        }
        {
            std::lock_guard<std::mutex> lock(m_sendMutex);
            m_sessionId = 0;
            m_srcId = m_myId;
            m_desId = 0;
            m_handshakeDone = false;
            m_remoteName.clear();
            m_crypto.Reset();
        }
    }
}

void NetworkManager::StartServerListener() {
    m_serverFd = socket(AF_INET, SOCK_STREAM, 0);
    if (m_serverFd < 0) {
        perror("[SERVER] socket");
        return;
    }

    int opt = 1;
    setsockopt(m_serverFd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in address;
    std::memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = INADDR_ANY;
    address.sin_port = htons(m_port);

    if (bind(m_serverFd, reinterpret_cast<struct sockaddr*>(&address), sizeof(address)) < 0 ||
        listen(m_serverFd, 4) < 0) {
        perror("[SERVER] bind/listen");
        closeSocket(m_serverFd);
        return;
    }

    printf("[SERVER] Listening on port %d for incoming Windows connections\n", m_port);
    fflush(stdout);

    m_serverThread = std::thread(&NetworkManager::ServerListenerLoop, this);
}

void NetworkManager::ServerListenerLoop() {
    while (m_running) {
        struct sockaddr_in clientAddress;
        socklen_t clientLength = sizeof(clientAddress);
        int clientFd = accept(m_serverFd, reinterpret_cast<struct sockaddr*>(&clientAddress), &clientLength);
        if (clientFd < 0) {
            if (m_running) {
                perror("[SERVER] accept");
            }
            break;
        }

        if (!AddressMatchesConfiguredHost(clientAddress, HostSnapshot(), m_expectedPeerAddress.load())) {
            std::cerr << "[SERVER] Rejected inbound connection from unexpected peer "
                      << FormatIpv4Address(clientAddress.sin_addr) << std::endl;
            closeSocket(clientFd);
            continue;
        }

        if (DebugNetworkLoggingEnabled()) {
            std::cout << "[SERVER] Accepted inbound connection from "
                      << FormatIpv4Address(clientAddress.sin_addr) << std::endl;
        }

        if (m_activeServerSessions.fetch_add(1) >= kMaxConcurrentInboundSessions) {
            m_activeServerSessions.fetch_sub(1);
            std::cerr << "[SERVER] Rejecting connection from "
                      << FormatIpv4Address(clientAddress.sin_addr)
                      << ": too many concurrent inbound sessions" << std::endl;
            closeSocket(clientFd);
            continue;
        }

        ConfigureConnectedSocket(clientFd);
        std::thread(&NetworkManager::HandleWindowsConnection, this, clientFd).detach();
    }
}

void NetworkManager::HandleWindowsConnection(int fd) {
    struct SessionGuard {
        std::atomic<int>& counter;
        NetworkManager& manager;
        int trackedFd;
        ~SessionGuard() {
            manager.UntrackSessionSocket(trackedFd);
            counter.fetch_sub(1);
        }
    } sessionGuard{m_activeServerSessions, *this, fd};
    TrackSessionSocket(fd);
    bool ownsInboundControlSession = false;

    CryptoHelper crypto(m_key);
    const uint32_t magic = crypto.Get24BitHash();

    uint8_t remoteNoise[16];
    if (!readExact(fd, remoteNoise, sizeof(remoteNoise), m_running)) {
        closeSocket(fd);
        return;
    }

    std::vector<uint8_t> remoteNoiseVec(remoteNoise, remoteNoise + sizeof(remoteNoise));
    std::vector<uint8_t> ignoredNoise;
    if (!crypto.DecryptStream(remoteNoiseVec, ignoredNoise)) {
        closeSocket(fd);
        return;
    }

    std::vector<uint8_t> localNoise(16);
    if (!FillRandomBytes(localNoise.data(), localNoise.size())) {
        closeSocket(fd);
        return;
    }
    std::vector<uint8_t> encryptedNoise;
    if (!crypto.EncryptStream(localNoise, encryptedNoise) ||
        !writeAll(fd, encryptedNoise.data(), encryptedNoise.size())) {
        closeSocket(fd);
        return;
    }

    std::array<uint8_t, 16> inboundChallenge{};
    MWBPacket localHandshake;
    std::memset(&localHandshake, 0, sizeof(localHandshake));
    localHandshake.type = static_cast<uint8_t>(PackageType::Handshake);
    if (!FillRandomBytes(inboundChallenge.data(), inboundChallenge.size())) {
        closeSocket(fd);
        return;
    }
    std::memcpy(localHandshake.data, inboundChallenge.data(), inboundChallenge.size());
    const uint32_t localHandshakeSource = htole32(m_myId);
    std::memcpy(&localHandshake.src, &localHandshakeSource, sizeof(localHandshakeSource));
    if (!sendPacketOnSocket(fd, crypto, magic, m_myName, localHandshake, true, 0, 0, 0)) {
        closeSocket(fd);
        return;
    }

    int handshakePackets = 0;
    bool trusted = false;
    uint32_t remoteMachineId = 0;

    for (int attempt = 0; attempt < 30 && m_running && !trusted; ++attempt) {
        const auto packetBytes = receivePacketOnSocket(fd, crypto, magic, m_running);
        if (!packetBytes || packetBytes->size() < kSmallPacketSize) {
            break;
        }

        const auto copiedPacket = copyPacketFromBytes(*packetBytes);
        if (!copiedPacket) {
            break;
        }

        const MWBPacket& packet = *copiedPacket;
        const uint32_t source = le32toh(packet.src);
        const uint32_t destination = le32toh(packet.des);

        if (packet.type == static_cast<uint8_t>(PackageType::Handshake)) {
            ++handshakePackets;
            if (!packetTargetsMachine(destination, m_myId)) {
                std::cerr << "[SERVER] Ignoring handshake packet for unexpected destination 0x"
                          << std::hex << destination << std::dec << std::endl;
                continue;
            }
            if (source != 0) {
                remoteMachineId = source;
            }

            MWBPacket ack;
            std::memset(&ack, 0, sizeof(ack));
            ack.type = static_cast<uint8_t>(PackageType::HandshakeAck);
            for (int index = 0; index < 16; ++index) {
                ack.data[index] = static_cast<uint8_t>(~packet.data[index]);
            }
            const uint32_t myId = htole32(m_myId);
            std::memcpy(&ack.src, &myId, sizeof(myId));
            if (!sendPacketOnSocket(fd, crypto, magic, m_myName, ack, true, 0, 0, 0)) {
                break;
            }
        } else if (handshakeAckMatchesChallenge(packet, inboundChallenge) &&
                   packetTargetsMachine(destination, m_myId)) {
            if (source != 0) {
                remoteMachineId = source;
            }
            trusted = true;
        } else if (packet.type == static_cast<uint8_t>(PackageType::HandshakeAck) ||
                   packet.type == static_cast<uint8_t>(PackageType::Heartbeat_ex) ||
                   packet.type == static_cast<uint8_t>(PackageType::Heartbeat_ex_l2)) {
            std::cerr << "[SERVER] Ignoring pre-handshake packet type "
                      << static_cast<int>(packet.type) << std::endl;
        }
    }

    if (!trusted) {
        closeSocket(fd);
        return;
    }

    if (remoteMachineId == 0) {
        std::cerr << "[SERVER] Rejecting inbound control session with no authenticated remote machine id." << std::endl;
        closeSocket(fd);
        return;
    }

    if (!TryAcquireInboundControlSession(fd, remoteMachineId)) {
        closeSocket(fd);
        return;
    }
    ownsInboundControlSession = true;
    if (DebugNetworkLoggingEnabled()) {
        std::cout << "[SERVER] Inbound handshake trusted peer 0x"
                  << std::hex << remoteMachineId << std::dec << std::endl;
    }
    RegisterActiveSessionPeer(remoteMachineId);
    MWBPacket heartbeat;
    std::memset(&heartbeat, 0, sizeof(heartbeat));
    heartbeat.type = static_cast<uint8_t>(PackageType::Heartbeat);
    const uint32_t broadcast = htole32(kBroadcastMachineId);
    const uint32_t myId = htole32(m_myId);
    std::memcpy(&heartbeat.des, &broadcast, sizeof(broadcast));
    std::memcpy(&heartbeat.src, &myId, sizeof(myId));
    sendPacketOnSocket(fd, crypto, magic, m_myName, heartbeat, true, 0, 0, 0);

    while (m_running) {
        const auto packetBytes = receivePacketOnSocket(fd, crypto, magic, m_running);
        if (!packetBytes || packetBytes->size() < kSmallPacketSize) {
            if (LastReceivePacketStatus() == ReceivePacketStatus::Timeout) {
                continue;
            }
            break;
        }

        const auto copiedPacket = copyPacketFromBytes(*packetBytes);
        if (!copiedPacket) {
            break;
        }

        const MWBPacket& packet = *copiedPacket;
        const uint8_t type = packet.type;
        const uint32_t source = le32toh(packet.src);
        const uint32_t destination = le32toh(packet.des);
        const bool targetsActiveSession =
            packetTargetsEstablishedSession(destination, source, m_myId, remoteMachineId);
        const bool controlSourceMatchesBoundPeer = source == 0 || source == remoteMachineId;

        if (type == static_cast<uint8_t>(PackageType::Mouse) &&
            targetsActiveSession) {
            MouseData mouse;
            std::memcpy(&mouse, &packet.data[0], sizeof(mouse));
            if (isRecognizedMouseMessage(mouse.wParam) && m_onMouse) {
                m_onMouse(mouse);
            } else if (!isRecognizedMouseMessage(mouse.wParam)) {
                std::cerr << "[SERVER] Dropping mouse packet with unknown message 0x"
                          << std::hex << mouse.wParam << std::dec << std::endl;
            }
        } else if (type == static_cast<uint8_t>(PackageType::Keyboard) &&
                   targetsActiveSession) {
            KeyboardData keyboard;
            std::memcpy(&keyboard, &packet.data[8], sizeof(keyboard));
            if (hasOnlyKnownKeyboardFlags(keyboard.flags) && m_onKeyboard) {
                m_onKeyboard(keyboard);
            } else if (!hasOnlyKnownKeyboardFlags(keyboard.flags)) {
                std::cerr << "[SERVER] Dropping keyboard packet with invalid flags 0x"
                          << std::hex << keyboard.flags << std::dec << std::endl;
            }
        } else if (type == static_cast<uint8_t>(PackageType::Handshake) &&
                   packetTargetsMachine(destination, m_myId)) {
            if (!controlSourceMatchesBoundPeer) {
                std::cerr << "[SERVER] Ignoring control handshake that claims a different peer id 0x"
                          << std::hex << source << " for active peer 0x" << remoteMachineId
                          << std::dec << std::endl;
                continue;
            }
            MWBPacket ack;
            std::memset(&ack, 0, sizeof(ack));
            ack.type = static_cast<uint8_t>(PackageType::HandshakeAck);
            for (int index = 0; index < 16; ++index) {
                ack.data[index] = static_cast<uint8_t>(~packet.data[index]);
            }
            const uint32_t currentId = htole32(m_myId);
            std::memcpy(&ack.src, &currentId, sizeof(currentId));
            sendPacketOnSocket(fd, crypto, magic, m_myName, ack, true, 0, 0, 0);
        } else if ((type == static_cast<uint8_t>(PackageType::Hello) ||
                    type == static_cast<uint8_t>(PackageType::Awake)) &&
                   packetTargetsMachine(destination, m_myId)) {
            if (!controlSourceMatchesBoundPeer) {
                std::cerr << "[SERVER] Ignoring control hello that claims a different peer id 0x"
                          << std::hex << source << " for active peer 0x" << remoteMachineId
                          << std::dec << std::endl;
                continue;
            }
            MWBPacket heartbeat;
            std::memset(&heartbeat, 0, sizeof(heartbeat));
            heartbeat.type = static_cast<uint8_t>(PackageType::Heartbeat);
            const uint32_t currentId = htole32(m_myId);
            std::memcpy(&heartbeat.src, &currentId, sizeof(currentId));
            sendPacketOnSocket(fd, crypto, magic, m_myName, heartbeat, true, 0, 0, 0);
        } else if (type == static_cast<uint8_t>(PackageType::Heartbeat) ||
                   type == static_cast<uint8_t>(PackageType::Heartbeat_ex)) {
            if (packetTargetsMachine(destination, m_myId) && !controlSourceMatchesBoundPeer) {
                std::cerr << "[SERVER] Ignoring control heartbeat that claims a different peer id 0x"
                          << std::hex << source << " for active peer 0x" << remoteMachineId
                          << std::dec << std::endl;
                continue;
            }
            MWBPacket heartbeat;
            std::memset(&heartbeat, 0, sizeof(heartbeat));
            heartbeat.type = static_cast<uint8_t>(PackageType::Heartbeat_ex_l2);
            const uint32_t currentId = htole32(m_myId);
            std::memcpy(&heartbeat.src, &currentId, sizeof(currentId));
            sendPacketOnSocket(fd, crypto, magic, m_myName, heartbeat, false, 0, 0, 0);
        } else if (type == static_cast<uint8_t>(PackageType::Heartbeat_ex_l2)) {
            if (packetTargetsMachine(destination, m_myId) && !controlSourceMatchesBoundPeer) {
                std::cerr << "[SERVER] Ignoring control heartbeat L2 that claims a different peer id 0x"
                          << std::hex << source << " for active peer 0x" << remoteMachineId
                          << std::dec << std::endl;
                continue;
            }
            MWBPacket heartbeat;
            std::memset(&heartbeat, 0, sizeof(heartbeat));
            heartbeat.type = static_cast<uint8_t>(PackageType::Heartbeat_ex_l3);
            const uint32_t currentId = htole32(m_myId);
            std::memcpy(&heartbeat.src, &currentId, sizeof(currentId));
            sendPacketOnSocket(fd, crypto, magic, m_myName, heartbeat, false, 0, 0, 0);
        } else if (type == static_cast<uint8_t>(PackageType::Heartbeat_ex_l3)) {
            if (packetTargetsMachine(destination, m_myId) && !controlSourceMatchesBoundPeer) {
                std::cerr << "[SERVER] Ignoring control heartbeat L3 that claims a different peer id 0x"
                          << std::hex << source << " for active peer 0x" << remoteMachineId
                          << std::dec << std::endl;
                continue;
            }
        } else if (type == static_cast<uint8_t>(PackageType::Clipboard) ||
                   type == static_cast<uint8_t>(PackageType::ClipboardAsk) ||
                   type == static_cast<uint8_t>(PackageType::ClipboardPush) ||
                   type == static_cast<uint8_t>(PackageType::ClipboardText) ||
                   type == static_cast<uint8_t>(PackageType::ClipboardImage) ||
                   type == static_cast<uint8_t>(PackageType::ClipboardDataEnd)) {
            if (targetsActiveSession) {
                HandleClipboardControlPacket(packet, source);
            }
        }
    }

    if (ownsInboundControlSession) {
        if (DebugNetworkLoggingEnabled()) {
            std::cout << "[SERVER] Closing inbound control session for peer 0x"
                      << std::hex << remoteMachineId << std::dec << std::endl;
        }
        UnregisterActiveSessionPeer(remoteMachineId);
        ReleaseInboundControlSession(fd);
    }
    closeSocket(fd);
    if (ownsInboundControlSession && m_onSessionDisconnected) {
        m_onSessionDisconnected();
    }
}

void NetworkManager::StartClipboardServerListener() {
    m_clipboardServerFd = socket(AF_INET, SOCK_STREAM, 0);
    if (m_clipboardServerFd < 0) {
        perror("[CLIPBOARD] socket");
        return;
    }

    int opt = 1;
    setsockopt(m_clipboardServerFd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in address;
    std::memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = INADDR_ANY;
    address.sin_port = htons(m_clipboardPort);

    if (bind(m_clipboardServerFd, reinterpret_cast<struct sockaddr*>(&address), sizeof(address)) < 0 ||
        listen(m_clipboardServerFd, 4) < 0) {
        perror("[CLIPBOARD] bind/listen");
        closeSocket(m_clipboardServerFd);
        return;
    }

    printf("[CLIPBOARD] Listening on port %d for clipboard connections\n", m_clipboardPort);
    fflush(stdout);

    m_clipboardServerThread = std::thread(&NetworkManager::ClipboardServerListenerLoop, this);
}

void NetworkManager::ClipboardServerListenerLoop() {
    while (m_running) {
        struct sockaddr_in clientAddress;
        socklen_t clientLength = sizeof(clientAddress);
        int clientFd = accept(
            m_clipboardServerFd,
            reinterpret_cast<struct sockaddr*>(&clientAddress),
            &clientLength);
        if (clientFd < 0) {
            if (m_running) {
                perror("[CLIPBOARD] accept");
            }
            break;
        }

        if (!AddressMatchesConfiguredHost(clientAddress, HostSnapshot(), m_expectedPeerAddress.load())) {
            std::cerr << "[CLIPBOARD] Rejected inbound connection from unexpected peer "
                      << FormatIpv4Address(clientAddress.sin_addr) << std::endl;
            closeSocket(clientFd);
            continue;
        }

        if (m_activeClipboardSessions.fetch_add(1) >= kMaxConcurrentInboundSessions) {
            m_activeClipboardSessions.fetch_sub(1);
            std::cerr << "[CLIPBOARD] Rejecting connection from "
                      << FormatIpv4Address(clientAddress.sin_addr)
                      << ": too many concurrent clipboard sessions" << std::endl;
            closeSocket(clientFd);
            continue;
        }

        ConfigureConnectedSocket(clientFd);
        std::thread(&NetworkManager::HandleClipboardConnection, this, clientFd).detach();
    }
}

void NetworkManager::HandleClipboardConnection(int fd) {
    struct SessionGuard {
        std::atomic<int>& counter;
        NetworkManager& manager;
        int trackedFd;
        ~SessionGuard() {
            manager.UntrackSessionSocket(trackedFd);
            counter.fetch_sub(1);
        }
    } sessionGuard{m_activeClipboardSessions, *this, fd};
    TrackSessionSocket(fd);

    CryptoHelper crypto(m_key);
    const uint32_t magic = 0; // PowerToys often uses zero magic for secondary clipboard socket
    bool remoteRequestsPush = false;
    std::string remoteName;
    MWBPacket remoteHeader;
    std::memset(&remoteHeader, 0, sizeof(remoteHeader));
    if (!exchangeClipboardHandshake(fd, crypto, magic, m_myName, m_myId, 0, m_running, true, remoteHeader, remoteRequestsPush, remoteName)) {
        closeSocket(fd);
        return;
    }

    const uint32_t remoteSource = le32toh(remoteHeader.src);
    const uint32_t remoteDestination = le32toh(remoteHeader.des);
    if (!packetTargetsMachine(remoteDestination, m_myId) ||
        !HasActiveSessionPeer(remoteSource)) {
        std::cerr << "[CLIPBOARD] Rejected clipboard session from unexpected remote peer 0x"
                  << std::hex << remoteSource << std::dec << std::endl;
        closeSocket(fd);
        return;
    }

    if (remoteRequestsPush) {
        std::size_t payloadSize = 0;
        std::string kind;
        std::vector<uint8_t> payload;
        if (receiveClipboardPayload(fd, crypto, m_running, payloadSize, kind, payload)) {
            if (kind == kClipboardSocketTextLabel) {
                const auto decoded = ClipboardManager::DecodePayload(payload);
                if (decoded.has_value()) {
                    DeliverClipboardPayload(*decoded);
                }
            } else if (kind == kClipboardSocketImageLabel) {
                auto imageBytes = ClipboardManager::DecodeImagePayload(payload);
                if (imageBytes.has_value()) {
                    while (!imageBytes->empty() && imageBytes->back() == 0) {
                        imageBytes->pop_back();
                    }
                    DeliverClipboardPayload(ClipboardManager::MakeImagePayload("image/png", std::move(*imageBytes)));
                }
            }
        }
    } else {
        std::optional<std::vector<uint8_t>> payload;
        std::string kind;
        {
             std::lock_guard<std::mutex> lock(m_clipboardMutex);
             if (m_pendingClipboardPayloadStruct && m_pendingClipboardPayloadStruct->image) {
                 payload = SnapshotClipboardImagePayload(false);
                 kind = kClipboardSocketImageLabel;
             } else {
                 payload = SnapshotClipboardPayload(false);
                 kind = kClipboardSocketTextLabel;
             }
        }
        if (payload.has_value()) {
            sendClipboardPayload(fd, crypto, *payload, kind);
        }
    }

    closeSocket(fd);
}

} // namespace mwb
