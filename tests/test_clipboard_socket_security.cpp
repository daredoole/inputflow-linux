#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <mutex>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "ClipboardManager.h"
#include "CryptoHelper.h"
#include "NetworkManager.h"
#include "Protocol.h"

namespace {

int g_failures = 0;
int g_nextPortSeed = 28000;

void Expect(bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << std::endl;
        ++g_failures;
    }
}

bool CanBindLoopbackPort(int port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        return false;
    }

    const int enable = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &enable, sizeof(enable));

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = htons(static_cast<uint16_t>(port));

    const bool available = bind(fd, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) == 0;
    close(fd);
    return available;
}

int AllocateTestPort() {
    for (int candidate = g_nextPortSeed; candidate < 65000; candidate += 10) {
        if (CanBindLoopbackPort(candidate) && CanBindLoopbackPort(candidate - 1)) {
            g_nextPortSeed = candidate + 10;
            return candidate;
        }
    }
    return 0;
}

bool WriteAll(int fd, const uint8_t* data, std::size_t length) {
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

bool ReadExact(int fd, uint8_t* data, std::size_t length) {
    std::size_t offset = 0;
    while (offset < length) {
        const ssize_t count = read(fd, data + offset, length - offset);
        if (count < 0) {
            if (errno == EINTR) {
                continue;
            }
            return false;
        }
        if (count == 0) {
            return false;
        }
        offset += static_cast<std::size_t>(count);
    }
    return true;
}

int ConnectLocal(int port) {
    const int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        throw std::runtime_error("Failed to create socket");
    }

    sockaddr_in address {};
    address.sin_family = AF_INET;
    address.sin_port = htons(static_cast<uint16_t>(port));
    if (inet_pton(AF_INET, "127.0.0.1", &address.sin_addr) != 1) {
        close(fd);
        throw std::runtime_error("Failed to encode localhost");
    }

    if (connect(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
        close(fd);
        throw std::runtime_error("Failed to connect to localhost");
    }

    return fd;
}

void PopulateMachineName(mwb::MWBPacket& packet, const std::string& machineName) {
    char name[32];
    std::memset(name, ' ', sizeof(name));
    const std::size_t length = std::min(machineName.size(), sizeof(name));
    std::memcpy(name, machineName.data(), length);
    std::memcpy(&packet.data[16], name, sizeof(name));
}

void FinalizePacket(mwb::MWBPacket& packet, uint32_t magic) {
    packet.magic0 = (magic >> 16) & 0xFF;
    packet.magic1 = (magic >> 24) & 0xFF;
    packet.checksum = 0;

    uint8_t checksum = 0;
    auto* bytes = reinterpret_cast<uint8_t*>(&packet);
    for (std::size_t index = 2; index < mwb::kSmallPacketSize; ++index) {
        checksum = static_cast<uint8_t>(checksum + bytes[index]);
    }
    packet.checksum = checksum;
}

std::vector<uint8_t> PacketBytes(
    uint8_t type,
    uint32_t magic,
    uint32_t source,
    uint32_t destination,
    const uint8_t* data,
    std::size_t dataSize,
    const std::string& machineName,
    bool isBig) {
    mwb::MWBPacket packet {};
    packet.type = type;

    const uint32_t sourceLe = htole32(source);
    const uint32_t destinationLe = htole32(destination);
    std::memcpy(&packet.src, &sourceLe, sizeof(sourceLe));
    std::memcpy(&packet.des, &destinationLe, sizeof(destinationLe));
    if (data != nullptr && dataSize != 0) {
        std::memcpy(packet.data, data, std::min<std::size_t>(dataSize, sizeof(packet.data)));
    }
    if (isBig && mwb::bigPacketCarriesMachineName(type)) {
        PopulateMachineName(packet, machineName);
    }
    FinalizePacket(packet, magic);

    const std::size_t size = isBig ? mwb::kBigPacketSize : mwb::kSmallPacketSize;
    const auto* bytes = reinterpret_cast<const uint8_t*>(&packet);
    return std::vector<uint8_t>(bytes, bytes + size);
}

void EncryptAndSend(int fd, mwb::CryptoHelper& crypto, const std::vector<uint8_t>& plain) {
    std::vector<uint8_t> encrypted;
    if (!crypto.EncryptStream(plain, encrypted) ||
        !WriteAll(fd, encrypted.data(), encrypted.size())) {
        throw std::runtime_error("Failed to send encrypted payload");
    }
}

std::vector<uint8_t> ReadAndDecrypt(int fd, mwb::CryptoHelper& crypto, std::size_t size) {
    std::vector<uint8_t> encrypted(size);
    if (!ReadExact(fd, encrypted.data(), encrypted.size())) {
        throw std::runtime_error("Failed to read encrypted payload");
    }

    std::vector<uint8_t> plain;
    if (!crypto.DecryptStream(encrypted, plain) || plain.size() != size) {
        throw std::runtime_error("Failed to decrypt payload");
    }
    return plain;
}

struct MainSession {
    int fd{-1};
    uint32_t magic{0};
    std::unique_ptr<mwb::CryptoHelper> crypto;
};

MainSession EstablishMainSession(int port, const std::string& key, uint32_t remoteMachineId) {
    MainSession session;
    session.fd = ConnectLocal(port);
    session.crypto = std::make_unique<mwb::CryptoHelper>(key);
    session.magic = session.crypto->Get24BitHash();

    const std::vector<uint8_t> noise(16, 0);
    EncryptAndSend(session.fd, *session.crypto, noise);
    ReadAndDecrypt(session.fd, *session.crypto, 16);

    const auto handshakeBytes = ReadAndDecrypt(session.fd, *session.crypto, mwb::kBigPacketSize);
    mwb::MWBPacket handshake {};
    std::memcpy(&handshake, handshakeBytes.data(), sizeof(handshake));
    if (handshake.type != static_cast<uint8_t>(mwb::PackageType::Handshake)) {
        throw std::runtime_error("Unexpected control handshake packet");
    }

    std::array<uint8_t, mwb::kHandshakeChallengeSize> challenge {};
    std::memcpy(challenge.data(), handshake.data, challenge.size());

    std::array<uint8_t, mwb::kHandshakeChallengeSize> ackData {};
    for (std::size_t index = 0; index < challenge.size(); ++index) {
        ackData[index] = static_cast<uint8_t>(~challenge[index]);
    }
    EncryptAndSend(
        session.fd,
        *session.crypto,
        PacketBytes(
            static_cast<uint8_t>(mwb::PackageType::HandshakeAck),
            session.magic,
            remoteMachineId,
            0,
            ackData.data(),
            ackData.size(),
            "cliptest-main",
            true));

    ReadAndDecrypt(session.fd, *session.crypto, mwb::kBigPacketSize);
    return session;
}

void CloseMainSession(MainSession& session) {
    if (session.fd >= 0) {
        close(session.fd);
        session.fd = -1;
    }
}

bool TryEstablishMainSession(int port, const std::string& key, uint32_t remoteMachineId, MainSession& session) {
    try {
        session = EstablishMainSession(port, key, remoteMachineId);
        return true;
    } catch (...) {
        CloseMainSession(session);
        return false;
    }
}

void SendMouseInput(MainSession& session, uint32_t remoteMachineId, int32_t x, int32_t y, uint32_t wParam = 0x0200) {
    const mwb::MouseData mouse {x, y, 0, wParam};
    EncryptAndSend(
        session.fd,
        *session.crypto,
        PacketBytes(
            static_cast<uint8_t>(mwb::PackageType::Mouse),
            session.magic,
            remoteMachineId,
            0,
            reinterpret_cast<const uint8_t*>(&mouse),
            sizeof(mouse),
            "cliptest-mouse",
            false));
}

void SendHeartbeatEx(MainSession& session, uint32_t remoteMachineId, uint16_t width = 1920, uint16_t height = 1080) {
    std::array<uint8_t, mwb::kClipboardChunkSize> data {};
    std::memcpy(data.data(), &width, sizeof(width));
    std::memcpy(data.data() + sizeof(width), &height, sizeof(height));
    EncryptAndSend(
        session.fd,
        *session.crypto,
        PacketBytes(
            static_cast<uint8_t>(mwb::PackageType::Heartbeat_ex),
            session.magic,
            remoteMachineId,
            0,
            data.data(),
            data.size(),
            "cliptest-heartbeat",
            true));
}

void PushClipboardText(int port, const std::string& key, uint32_t remoteMachineId, const std::string& text) {
    const int fd = ConnectLocal(port);
    mwb::CryptoHelper crypto(key);
    const uint32_t magic = crypto.Get24BitHash();

    try {
        ReadAndDecrypt(fd, crypto, 16);
        const std::vector<uint8_t> noise(16, 0);
        EncryptAndSend(fd, crypto, noise);

        const auto headerBytes = ReadAndDecrypt(fd, crypto, mwb::kBigPacketSize);
        mwb::MWBPacket header {};
        std::memcpy(&header, headerBytes.data(), sizeof(header));
        if (header.type != static_cast<uint8_t>(mwb::PackageType::ClipboardPush) &&
            header.type != static_cast<uint8_t>(mwb::PackageType::Clipboard)) {
            throw std::runtime_error("Unexpected clipboard handshake packet");
        }

        const uint32_t postActionOther = 0;
        EncryptAndSend(
            fd,
            crypto,
            PacketBytes(
                static_cast<uint8_t>(mwb::PackageType::ClipboardPush),
                magic,
                remoteMachineId,
                0,
                reinterpret_cast<const uint8_t*>(&postActionOther),
                sizeof(postActionOther),
                "cliptest-push",
                true));

        const std::vector<uint8_t> payload = mwb::ClipboardManager::EncodeTextPayload(text);
        EncryptAndSend(fd, crypto, mwb::ClipboardManager::EncodeSocketHeader(payload.size(), mwb::kClipboardSocketTextLabel));

        std::vector<uint8_t> paddedPayload = payload;
        const std::size_t remainder = paddedPayload.size() % 16;
        if (remainder != 0) {
            paddedPayload.resize(paddedPayload.size() + (16 - remainder), 0);
        }
        if (!paddedPayload.empty()) {
            EncryptAndSend(fd, crypto, paddedPayload);
        }
    } catch (...) {
        close(fd);
        throw;
    }

    close(fd);
}

void TestClipboardSocketRequiresActiveSession() {
    const int kPort = AllocateTestPort();
    constexpr uint32_t kLocalMachineId = 0x11111111u;
    constexpr uint32_t kAuthorizedRemoteMachineId = 0x22222222u;
    constexpr uint32_t kUnauthorizedRemoteMachineId = 0x33333333u;
    const std::string key = "clipboard-security-key";
    if (kPort == 0) {
        Expect(false, "Test should allocate a free local port pair for clipboard security test");
        return;
    }

    mwb::NetworkManager manager("127.0.0.1", kPort, key);
    manager.SetAutoConnectEnabled(false);
    manager.SetLocalIdentity(kLocalMachineId, "cliptest-server");

    std::mutex clipboardMutex;
    std::condition_variable clipboardChanged;
    std::optional<std::string> clipboardText;

    manager.SetOnClipboardCallback([&](const std::string& text) {
        {
            std::lock_guard<std::mutex> lock(clipboardMutex);
            clipboardText = text;
        }
        clipboardChanged.notify_all();
    });

    if (!manager.Connect()) {
        Expect(false, "NetworkManager should start local listeners for clipboard security test");
        return;
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(150));

    try {
        PushClipboardText(kPort - 1, key, kUnauthorizedRemoteMachineId, "unauthorized");
    } catch (...) {
        // Rejection may close the socket mid-transfer. The assertion is the callback stays quiet.
    }

    {
        std::unique_lock<std::mutex> lock(clipboardMutex);
        const bool changed = clipboardChanged.wait_for(lock, std::chrono::milliseconds(400), [&] {
            return clipboardText.has_value();
        });
        Expect(!changed, "Clipboard socket should reject pushes without an active control session");
        clipboardText.reset();
    }

    MainSession session = EstablishMainSession(kPort, key, kAuthorizedRemoteMachineId);
    std::this_thread::sleep_for(std::chrono::milliseconds(150));

    PushClipboardText(kPort - 1, key, kAuthorizedRemoteMachineId, "authorized");

    {
        std::unique_lock<std::mutex> lock(clipboardMutex);
        const bool changed = clipboardChanged.wait_for(lock, std::chrono::seconds(2), [&] {
            return clipboardText.has_value();
        });
        Expect(changed, "Clipboard socket should accept pushes from the active control-session peer");
        Expect(clipboardText == std::optional<std::string>("authorized"),
               "Authorized clipboard push should deliver the decoded text");
    }

    CloseMainSession(session);
    manager.Stop();
}

void TestClipboardTrustRevokedAfterControlDisconnect() {
    const int kPort = AllocateTestPort();
    constexpr uint32_t kLocalMachineId = 0x62626262u;
    constexpr uint32_t kAuthorizedRemoteMachineId = 0x72727272u;
    const std::string key = "clipboard-revocation-key";
    if (kPort == 0) {
        Expect(false, "Test should allocate a free local port pair for clipboard revocation test");
        return;
    }

    mwb::NetworkManager manager("127.0.0.1", kPort, key);
    manager.SetAutoConnectEnabled(false);
    manager.SetLocalIdentity(kLocalMachineId, "clip-revoke-server");

    std::mutex clipboardMutex;
    std::condition_variable clipboardChanged;
    std::optional<std::string> clipboardText;
    manager.SetOnClipboardCallback([&](const std::string& text) {
        {
            std::lock_guard<std::mutex> lock(clipboardMutex);
            clipboardText = text;
        }
        clipboardChanged.notify_all();
    });

    std::mutex disconnectMutex;
    std::condition_variable disconnected;
    bool sawDisconnect = false;
    manager.SetOnSessionDisconnected([&]() {
        {
            std::lock_guard<std::mutex> lock(disconnectMutex);
            sawDisconnect = true;
        }
        disconnected.notify_all();
    });

    if (!manager.Connect()) {
        Expect(false, "NetworkManager should start local listeners for clipboard revocation test");
        return;
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    MainSession session = EstablishMainSession(kPort, key, kAuthorizedRemoteMachineId);
    std::this_thread::sleep_for(std::chrono::milliseconds(150));

    PushClipboardText(kPort - 1, key, kAuthorizedRemoteMachineId, "before-disconnect");
    {
        std::unique_lock<std::mutex> lock(clipboardMutex);
        const bool changed = clipboardChanged.wait_for(lock, std::chrono::seconds(2), [&] {
            return clipboardText == std::optional<std::string>("before-disconnect");
        });
        Expect(changed, "Authorized clipboard push should work before control-session disconnect");
        clipboardText.reset();
    }

    CloseMainSession(session);
    {
        std::unique_lock<std::mutex> lock(disconnectMutex);
        const bool disconnectedNow = disconnected.wait_for(lock, std::chrono::seconds(2), [&] {
            return sawDisconnect;
        });
        Expect(disconnectedNow, "Control-session disconnect callback should fire before revocation assertion");
    }

    try {
        PushClipboardText(kPort - 1, key, kAuthorizedRemoteMachineId, "after-disconnect");
    } catch (...) {
        // Rejection may close the socket mid-transfer. The callback must stay quiet.
    }

    {
        std::unique_lock<std::mutex> lock(clipboardMutex);
        const bool changed = clipboardChanged.wait_for(lock, std::chrono::milliseconds(500), [&] {
            return clipboardText.has_value();
        });
        Expect(!changed, "Clipboard socket trust should be revoked after the active control session disconnects");
    }

    manager.Stop();
}

void TestManualModeDoesNotAutoConnect() {
    const int kPort = AllocateTestPort();
    constexpr uint32_t kLocalMachineId = 0x73737373u;
    const std::string key = "manual-mode-key";
    if (kPort == 0) {
        Expect(false, "Test should allocate a free local port pair for manual-mode test");
        return;
    }

    mwb::NetworkManager manager("127.0.0.1", kPort, key);
    manager.SetAutoConnectEnabled(false);
    manager.SetLocalIdentity(kLocalMachineId, "manual-mode-server");

    std::atomic<int> establishedSessions{0};
    manager.SetOnSessionEstablished([&](const std::string&, int, const std::string&, uint32_t, uint32_t) {
        ++establishedSessions;
    });

    if (!manager.Connect()) {
        Expect(false, "NetworkManager should start local listeners for manual-mode test");
        return;
    }

    std::thread loopThread([&]() {
        manager.RunLoop();
    });

    std::this_thread::sleep_for(std::chrono::milliseconds(650));
    Expect(establishedSessions.load() == 0,
           "Manual mode should keep RunLoop inbound-only and must not auto-connect to its own listener");

    manager.Stop();
    if (loopThread.joinable()) {
        loopThread.join();
    }
}

void TestRejectsSecondInboundControlSession() {
    const int kPort = AllocateTestPort();
    constexpr uint32_t kLocalMachineId = 0x12121212u;
    constexpr uint32_t kPrimaryRemoteMachineId = 0x22223333u;
    constexpr uint32_t kSecondaryRemoteMachineId = 0x44445555u;
    const std::string key = "inbound-shadow-session-key";
    if (kPort == 0) {
        Expect(false, "Test should allocate a free local port pair for inbound shadow-session test");
        return;
    }

    mwb::NetworkManager manager("127.0.0.1", kPort, key);
    manager.SetAutoConnectEnabled(false);
    manager.SetLocalIdentity(kLocalMachineId, "shadow-test-server");

    std::mutex mouseMutex;
    std::condition_variable mouseChanged;
    std::vector<std::pair<int32_t, int32_t>> mouseEvents;
    std::mutex disconnectMutex;
    std::condition_variable disconnected;
    bool sawDisconnect = false;

    manager.SetOnMouseCallback([&](const mwb::MouseData& mouse) {
        {
            std::lock_guard<std::mutex> lock(mouseMutex);
            mouseEvents.emplace_back(mouse.x, mouse.y);
        }
        mouseChanged.notify_all();
    });
    manager.SetOnSessionDisconnected([&]() {
        {
            std::lock_guard<std::mutex> lock(disconnectMutex);
            sawDisconnect = true;
        }
        disconnected.notify_all();
    });

    if (!manager.Connect()) {
        Expect(false, "NetworkManager should start local listeners for inbound shadow-session test");
        return;
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(150));

    MainSession primary = EstablishMainSession(kPort, key, kPrimaryRemoteMachineId);
    std::this_thread::sleep_for(std::chrono::milliseconds(150));

    MainSession secondary;
    const bool secondaryEstablished =
        TryEstablishMainSession(kPort, key, kSecondaryRemoteMachineId, secondary);
    Expect(!secondaryEstablished,
           "A second authenticated inbound control session should be rejected while the first is active");

    SendMouseInput(primary, kPrimaryRemoteMachineId, 401, 501);

    {
        std::unique_lock<std::mutex> lock(mouseMutex);
        const bool changed = mouseChanged.wait_for(lock, std::chrono::seconds(2), [&] {
            return !mouseEvents.empty();
        });
        Expect(changed, "The original inbound control session should remain active after a shadow-session attempt");
        if (changed) {
            Expect(mouseEvents.back() == std::make_pair<int32_t, int32_t>(401, 501),
                   "The surviving inbound control session should still inject its own mouse input");
        }
    }

    CloseMainSession(primary);
    {
        std::unique_lock<std::mutex> lock(disconnectMutex);
        const bool released = disconnected.wait_for(lock, std::chrono::seconds(2), [&] {
            return sawDisconnect;
        });
        Expect(released, "Prior inbound control session should report disconnect before replacement is admitted");
    }

    MainSession replacement;
    const bool replacementEstablished =
        TryEstablishMainSession(kPort, key, kSecondaryRemoteMachineId, replacement);
    Expect(replacementEstablished,
           "A new inbound control session should be able to connect after the prior session closes");
    CloseMainSession(replacement);
    manager.Stop();
}

void TestInboundPeerIdentityRemainsBoundAfterHeartbeat() {
    const int kPort = AllocateTestPort();
    constexpr uint32_t kLocalMachineId = 0x31313131u;
    constexpr uint32_t kOriginalRemoteMachineId = 0x41414141u;
    constexpr uint32_t kForgedRemoteMachineId = 0x51515151u;
    const std::string key = "inbound-peer-binding-key";
    if (kPort == 0) {
        Expect(false, "Test should allocate a free local port pair for peer-binding test");
        return;
    }

    mwb::NetworkManager manager("127.0.0.1", kPort, key);
    manager.SetAutoConnectEnabled(false);
    manager.SetLocalIdentity(kLocalMachineId, "binding-test-server");

    std::mutex mouseMutex;
    std::condition_variable mouseChanged;
    std::vector<std::pair<int32_t, int32_t>> mouseEvents;

    manager.SetOnMouseCallback([&](const mwb::MouseData& mouse) {
        {
            std::lock_guard<std::mutex> lock(mouseMutex);
            mouseEvents.emplace_back(mouse.x, mouse.y);
        }
        mouseChanged.notify_all();
    });

    if (!manager.Connect()) {
        Expect(false, "NetworkManager should start local listeners for peer-binding test");
        return;
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(150));

    MainSession session = EstablishMainSession(kPort, key, kOriginalRemoteMachineId);
    std::this_thread::sleep_for(std::chrono::milliseconds(150));

    SendMouseInput(session, kOriginalRemoteMachineId, 601, 701);
    {
        std::unique_lock<std::mutex> lock(mouseMutex);
        const bool changed = mouseChanged.wait_for(lock, std::chrono::seconds(2), [&] {
            return !mouseEvents.empty();
        });
        Expect(changed, "The original inbound peer should inject before any forged heartbeat");
    }

    SendHeartbeatEx(session, kForgedRemoteMachineId);
    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    SendMouseInput(session, kForgedRemoteMachineId, 602, 702);
    SendMouseInput(session, kOriginalRemoteMachineId, 603, 703);
    std::this_thread::sleep_for(std::chrono::milliseconds(400));

    {
        std::lock_guard<std::mutex> lock(mouseMutex);
        const auto sawOriginalAfterHeartbeat =
            std::find(mouseEvents.begin(), mouseEvents.end(), std::make_pair<int32_t, int32_t>(603, 703)) != mouseEvents.end();
        const auto sawForgedAfterHeartbeat =
            std::find(mouseEvents.begin(), mouseEvents.end(), std::make_pair<int32_t, int32_t>(602, 702)) != mouseEvents.end();
        Expect(sawOriginalAfterHeartbeat,
               "The bound inbound peer should continue to inject after a forged heartbeat from another peer id");
        Expect(!sawForgedAfterHeartbeat,
               "A forged heartbeat must not retag the active inbound session to a different peer id");
    }

    CloseMainSession(session);
    manager.Stop();
}

} // namespace

int main() {
    TestClipboardSocketRequiresActiveSession();
    TestClipboardTrustRevokedAfterControlDisconnect();
    TestManualModeDoesNotAutoConnect();
    TestRejectsSecondInboundControlSession();
    TestInboundPeerIdentityRemainsBoundAfterHeartbeat();

    if (g_failures != 0) {
        std::cerr << g_failures << " clipboard socket security test(s) failed." << std::endl;
        return 1;
    }

    std::cout << "Clipboard socket security tests passed." << std::endl;
    return 0;
}
