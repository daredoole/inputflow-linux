#include "AndroidRelay.h"

#include "AppConfig.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <optional>
#include <sstream>
#include <string_view>
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>
#include <utility>

#include <openssl/hmac.h>
#include <openssl/rand.h>

namespace mwb {
namespace {

constexpr std::size_t kMaxFrameBytes = 64 * 1024;

void CloseFd(int& fd) {
    if (fd >= 0) {
        shutdown(fd, SHUT_RDWR);
        close(fd);
        fd = -1;
    }
}

bool ReadExact(int fd, void* data, std::size_t size) {
    auto* out = static_cast<uint8_t*>(data);
    std::size_t offset = 0;
    while (offset < size) {
        const ssize_t count = read(fd, out + offset, size - offset);
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

bool WriteExact(int fd, const void* data, std::size_t size) {
    const auto* in = static_cast<const uint8_t*>(data);
    std::size_t offset = 0;
    while (offset < size) {
        const ssize_t count = write(fd, in + offset, size - offset);
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

std::optional<std::string> ReadFrame(int fd) {
    uint32_t networkLength = 0;
    if (!ReadExact(fd, &networkLength, sizeof(networkLength))) {
        return std::nullopt;
    }
    const uint32_t length = ntohl(networkLength);
    if (length == 0 || length > kMaxFrameBytes) {
        return std::nullopt;
    }

    std::string payload(length, '\0');
    if (!ReadExact(fd, payload.data(), payload.size())) {
        return std::nullopt;
    }
    return payload;
}

std::string HexEncode(const uint8_t* data, std::size_t size) {
    std::ostringstream out;
    out << std::hex << std::setfill('0');
    for (std::size_t index = 0; index < size; ++index) {
        out << std::setw(2) << static_cast<unsigned int>(data[index]);
    }
    return out.str();
}

std::string RandomNonceHex() {
    std::array<uint8_t, 16> bytes{};
    if (RAND_bytes(bytes.data(), static_cast<int>(bytes.size())) != 1) {
        return {};
    }
    return HexEncode(bytes.data(), bytes.size());
}

std::string HmacSha256Hex(const std::string& secret, const std::string& message) {
    std::array<unsigned char, EVP_MAX_MD_SIZE> digest{};
    unsigned int digestLength = 0;
    HMAC(
        EVP_sha256(),
        secret.data(),
        static_cast<int>(secret.size()),
        reinterpret_cast<const unsigned char*>(message.data()),
        message.size(),
        digest.data(),
        &digestLength);
    return HexEncode(digest.data(), digestLength);
}

std::optional<std::string> JsonStringValue(const std::string& json, const std::string& key) {
    const std::string marker = "\"" + key + "\":\"";
    const std::size_t start = json.find(marker);
    if (start == std::string::npos) {
        return std::nullopt;
    }
    const std::size_t valueStart = start + marker.size();
    const std::size_t valueEnd = json.find('"', valueStart);
    if (valueEnd == std::string::npos) {
        return std::nullopt;
    }
    return json.substr(valueStart, valueEnd - valueStart);
}

std::string EscapeJson(std::string_view value) {
    std::string escaped;
    escaped.reserve(value.size());
    for (char ch : value) {
        switch (ch) {
            case '\\':
                escaped += "\\\\";
                break;
            case '"':
                escaped += "\\\"";
                break;
            case '\n':
                escaped += "\\n";
                break;
            case '\r':
                escaped += "\\r";
                break;
            case '\t':
                escaped += "\\t";
                break;
            default:
                escaped.push_back(ch);
                break;
        }
    }
    return escaped;
}

} // namespace

AndroidRelayServer::AndroidRelayServer(AndroidRelayOptions options)
    : m_options(std::move(options)) {
}

AndroidRelayServer::~AndroidRelayServer() {
    Stop();
}

bool AndroidRelayServer::Start() {
    if (!m_options.enabled) {
        return true;
    }
    if (m_options.secret.empty()) {
        std::cerr << "WARN: Android relay enabled but android_relay_secret is empty; relay disabled." << std::endl;
        return true;
    }
    // The relay can drive native (Shizuku/root) input injection on the phone, so
    // a weak shared secret is a privilege-escalation risk. Fail closed.
    if (!IsStrongRelaySecret(m_options.secret)) {
        std::cerr << "ERROR: android_relay_secret is too weak (need >= "
                  << kMinRelaySecretLength
                  << " chars with enough variety); relay disabled. "
                     "Generate a strong one with: mwb_client android-pair --generate"
                  << std::endl;
        return true;
    }
    if (m_options.port <= 0 || m_options.port > 65535) {
        std::cerr << "WARN: Android relay port is invalid; relay disabled." << std::endl;
        return true;
    }
    if (m_running.exchange(true)) {
        return true;
    }

    m_listenFd = socket(AF_INET, SOCK_STREAM, 0);
    if (m_listenFd < 0) {
        std::cerr << "WARN: Android relay socket failed: " << std::strerror(errno) << std::endl;
        m_running = false;
        return false;
    }

    int enabled = 1;
    setsockopt(m_listenFd, SOL_SOCKET, SO_REUSEADDR, &enabled, sizeof(enabled));

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_ANY);
    address.sin_port = htons(static_cast<uint16_t>(m_options.port));
    if (bind(m_listenFd, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) != 0 ||
        listen(m_listenFd, 4) != 0) {
        std::cerr << "WARN: Android relay listen failed on port " << m_options.port
                  << ": " << std::strerror(errno) << std::endl;
        CloseFd(m_listenFd);
        m_running = false;
        return false;
    }

    m_acceptThread = std::thread([this]() { AcceptLoop(); });
    std::cout << "[ANDROID] Relay listening on port " << m_options.port
              << " for peer '" << m_options.peerName << "'." << std::endl;
    return true;
}

void AndroidRelayServer::Stop() {
    if (!m_running.exchange(false)) {
        return;
    }

    CloseFd(m_listenFd);
    {
        std::lock_guard<std::mutex> lock(m_clientsMutex);
        for (auto& client : m_clients) {
            CloseFd(client->fd);
        }
    }
    if (m_acceptThread.joinable()) {
        m_acceptThread.join();
    }
    for (auto& thread : m_clientThreads) {
        if (thread.joinable()) {
            thread.join();
        }
    }
    m_clientThreads.clear();
    {
        std::lock_guard<std::mutex> lock(m_clientsMutex);
        m_clients.clear();
    }
}

bool AndroidRelayServer::HasAuthenticatedClient() const {
    std::lock_guard<std::mutex> lock(m_clientsMutex);
    return std::any_of(m_clients.begin(), m_clients.end(), [](const auto& client) {
        return client->authenticated;
    });
}

bool AndroidRelayServer::SendMouse(const MouseData& mouse) {
    return BroadcastFrame(BuildAndroidMouseFrame(mouse));
}

bool AndroidRelayServer::SendKeyboard(const KeyboardData& keyboard) {
    return BroadcastFrame(BuildAndroidKeyboardFrame(keyboard));
}

bool AndroidRelayServer::SendGesture(const std::string& kind, double dx, double dy) {
    return BroadcastFrame(BuildAndroidGestureFrame(kind, dx, dy));
}

bool AndroidRelayServer::SendControl(bool active) {
    return BroadcastFrame(BuildAndroidControlFrame(active));
}

void AndroidRelayServer::SetOnReleaseRequested(std::function<void()> callback) {
    std::lock_guard<std::mutex> lock(m_callbackMutex);
    m_onReleaseRequested = std::move(callback);
}

void AndroidRelayServer::SetOnTopologyUpdate(std::function<void(const std::string&)> callback) {
    std::lock_guard<std::mutex> lock(m_callbackMutex);
    m_onTopologyUpdate = std::move(callback);
}

void AndroidRelayServer::SetLocalDeviceInfo(const std::string& machineName, int screenWidth, int screenHeight) {
    m_localMachineName = machineName;
    m_localScreenWidth = screenWidth;
    m_localScreenHeight = screenHeight;
}

void AndroidRelayServer::AcceptLoop() {
    while (m_running) {
        int clientFd = accept(m_listenFd, nullptr, nullptr);
        if (clientFd < 0) {
            if (errno == EINTR) {
                continue;
            }
            if (m_running) {
                std::cerr << "WARN: Android relay accept failed: " << std::strerror(errno) << std::endl;
            }
            break;
        }

        auto session = std::make_shared<ClientSession>();
        session->fd = clientFd;
        {
            std::lock_guard<std::mutex> lock(m_clientsMutex);
            m_clients.push_back(session);
            m_clientThreads.emplace_back([this, session]() { HandleClient(session); });
        }
    }
}

void AndroidRelayServer::HandleClient(std::shared_ptr<ClientSession> session) {
    if (!AuthenticateClient(session)) {
        RemoveClient(session);
        return;
    }

    while (m_running) {
        const auto frame = ReadFrame(session->fd);
        if (!frame.has_value()) {
            break;
        }
        const auto type = JsonStringValue(*frame, "type");
        if (!type.has_value()) {
            continue;
        }
        if (*type == "release") {
            std::function<void()> callback;
            {
                std::lock_guard<std::mutex> lock(m_callbackMutex);
                callback = m_onReleaseRequested;
            }
            if (callback) {
                callback();
            }
        } else if (*type == "topology_update") {
            std::function<void(const std::string&)> callback;
            {
                std::lock_guard<std::mutex> lock(m_callbackMutex);
                callback = m_onTopologyUpdate;
            }
            if (callback) {
                callback(*frame);
            }
        }
    }

    RemoveClient(session);
}

bool AndroidRelayServer::AuthenticateClient(const std::shared_ptr<ClientSession>& session) {
    const std::string nonce = RandomNonceHex();
    if (nonce.empty()) {
        return false;
    }
    const std::string hello = "{\"type\":\"hello\",\"version\":1,\"nonce\":\"" + nonce +
        "\",\"peer\":\"" + EscapeJson(m_options.peerName) + "\"}";
    if (!SendFrame(session, hello)) {
        return false;
    }

    const auto auth = ReadFrame(session->fd);
    if (!auth.has_value()) {
        return false;
    }
    const auto type = JsonStringValue(*auth, "type");
    const auto hmac = JsonStringValue(*auth, "hmac");
    if (!type.has_value() || *type != "auth" || !hmac.has_value()) {
        return false;
    }

    const std::string expected = HmacSha256Hex(m_options.secret, nonce);
    if (*hmac != expected) {
        std::cerr << "WARN: Android relay rejected client with invalid auth." << std::endl;
        return false;
    }

    const std::string deviceName = JsonStringValue(*auth, "device").value_or("android");
    {
        std::lock_guard<std::mutex> lock(m_clientsMutex);
        session->deviceName = deviceName;
        session->authenticated = true;
    }
    std::cout << "[ANDROID] Relay client authenticated: " << deviceName << std::endl;
    if (!SendFrame(session, "{\"type\":\"ready\"}")) {
        return false;
    }

    // Send device layout info so the Android client can show the layout editor.
    if (!m_localMachineName.empty()) {
        std::ostringstream devInfo;
        devInfo << "{\"type\":\"devices_info\","
                << "\"devices\":["
                << "{\"id\":\"linux\",\"name\":\"" << EscapeJson(m_localMachineName) << "\","
                << "\"width\":" << m_localScreenWidth << ",\"height\":" << m_localScreenHeight << "},"
                << "{\"id\":\"android\",\"name\":\"" << EscapeJson(deviceName) << "\","
                << "\"width\":0,\"height\":0}"
                << "],"
                << "\"layout\":["
                << "{\"id\":\"linux\",\"x\":0,\"y\":0},"
                << "{\"id\":\"android\",\"x\":" << m_localScreenWidth << ",\"y\":0}"
                << "]}";
        SendFrame(session, devInfo.str());
    }
    return true;
}

bool AndroidRelayServer::SendFrame(const std::shared_ptr<ClientSession>& session, const std::string& payload) {
    if (session->fd < 0 || payload.empty() || payload.size() > kMaxFrameBytes) {
        return false;
    }

    std::lock_guard<std::mutex> lock(session->writeMutex);
    const uint32_t length = htonl(static_cast<uint32_t>(payload.size()));
    return WriteExact(session->fd, &length, sizeof(length)) &&
           WriteExact(session->fd, payload.data(), payload.size());
}

bool AndroidRelayServer::BroadcastFrame(const std::string& payload) {
    std::vector<std::shared_ptr<ClientSession>> clients;
    {
        std::lock_guard<std::mutex> lock(m_clientsMutex);
        for (const auto& client : m_clients) {
            if (client->authenticated) {
                clients.push_back(client);
            }
        }
    }
    if (clients.empty()) {
        return false;
    }

    bool delivered = false;
    for (const auto& client : clients) {
        delivered = SendFrame(client, payload) || delivered;
    }
    return delivered;
}

void AndroidRelayServer::RemoveClient(const std::shared_ptr<ClientSession>& session) {
    CloseFd(session->fd);
    std::lock_guard<std::mutex> lock(m_clientsMutex);
    m_clients.erase(
        std::remove_if(m_clients.begin(), m_clients.end(), [&](const auto& current) {
            return current == session;
        }),
        m_clients.end());
}

std::string BuildAndroidMouseFrame(const MouseData& mouse) {
    std::ostringstream out;
    out << "{\"type\":\"mouse\",\"x\":" << mouse.x
        << ",\"y\":" << mouse.y
        << ",\"mouseData\":" << mouse.mouseData
        << ",\"wParam\":" << mouse.wParam
        << "}";
    return out.str();
}

std::string BuildAndroidKeyboardFrame(const KeyboardData& keyboard) {
    std::ostringstream out;
    out << "{\"type\":\"keyboard\",\"vkCode\":" << keyboard.vkCode
        << ",\"flags\":" << keyboard.flags
        << "}";
    return out.str();
}

std::string BuildAndroidGestureFrame(const std::string& kind, double dx, double dy) {
    std::ostringstream out;
    out << "{\"type\":\"gesture\",\"kind\":\"" << EscapeJson(kind)
        << "\",\"dx\":" << dx
        << ",\"dy\":" << dy
        << "}";
    return out.str();
}

std::string BuildAndroidControlFrame(bool active) {
    return std::string("{\"type\":\"control\",\"active\":") + (active ? "true" : "false") + "}";
}

} // namespace mwb
