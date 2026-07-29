#include "AndroidRelay.h"

#include "AppConfig.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <functional>
#include <iomanip>
#include <iostream>
#include <limits>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <sys/socket.h>
#include <sys/time.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include <utility>
#include <vector>

#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <openssl/rand.h>

#if defined(MWB_HAVE_LIBEI_INPUT_CAPTURE)
#include <gio/gio.h>
#include <map>
#endif

namespace mwb {
namespace {

constexpr std::size_t kMaxFrameBytes = 64 * 1024;
constexpr std::size_t kEncryptedFrameOverhead = 4 + 8 + 16;
constexpr std::size_t kMaxWireFrameBytes = kMaxFrameBytes + kEncryptedFrameOverhead;
constexpr std::size_t kMaxRelayClients = 8;
constexpr std::string_view kEncryptedFrameMagic = "IFP1";
constexpr std::array<unsigned char, 4> kServerToClientIvPrefix{'I', 'F', 'S', 'O'};
constexpr std::array<unsigned char, 4> kClientToServerIvPrefix{'I', 'F', 'C', 'O'};
using RelayKey = std::array<unsigned char, 32>;

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
    if (length == 0 || length > kMaxWireFrameBytes) {
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

RelayKey HmacSha256(const std::string& secret, const std::string& message) {
    RelayKey digest{};
    unsigned int digestLength = 0;
    if (HMAC(
        EVP_sha256(),
        secret.data(),
        static_cast<int>(secret.size()),
        reinterpret_cast<const unsigned char*>(message.data()),
        message.size(),
        digest.data(),
        &digestLength) == nullptr ||
        digestLength != digest.size()) {
        digest.fill(0);
    }
    return digest;
}

std::string HmacSha256Hex(const std::string& secret, const std::string& message) {
    const auto digest = HmacSha256(secret, message);
    return HexEncode(digest.data(), digest.size());
}

RelayKey DeriveRelayKey(
    const std::string& secret,
    const std::string& nonce,
    std::string_view direction) {
    return HmacSha256(
        secret,
        "inputflow-relay-v1/" + std::string(direction) + "/" + nonce);
}

void AppendSequence(std::string& output, uint64_t sequence) {
    for (int shift = 56; shift >= 0; shift -= 8) {
        output.push_back(static_cast<char>((sequence >> shift) & 0xffu));
    }
}

uint64_t ReadSequence(std::string_view input) {
    uint64_t sequence = 0;
    for (const unsigned char byte : input) {
        sequence = (sequence << 8u) | byte;
    }
    return sequence;
}

std::array<unsigned char, 12> BuildRelayIv(
    const std::array<unsigned char, 4>& prefix,
    uint64_t sequence) {
    std::array<unsigned char, 12> iv{};
    std::copy(prefix.begin(), prefix.end(), iv.begin());
    for (int index = 0; index < 8; ++index) {
        iv[4 + index] = static_cast<unsigned char>(
            (sequence >> (56 - index * 8)) & 0xffu);
    }
    return iv;
}

std::optional<std::string> EncryptRelayFrame(
    const std::string& payload,
    const RelayKey& key,
    uint64_t sequence,
    const std::array<unsigned char, 4>& ivPrefix) {
    if (payload.empty() || payload.size() > kMaxFrameBytes) {
        return std::nullopt;
    }

    std::string header{kEncryptedFrameMagic};
    AppendSequence(header, sequence);
    const auto iv = BuildRelayIv(ivPrefix, sequence);
    EVP_CIPHER_CTX* context = EVP_CIPHER_CTX_new();
    if (context == nullptr) {
        return std::nullopt;
    }

    std::string ciphertext(payload.size() + EVP_MAX_BLOCK_LENGTH, '\0');
    int outputLength = 0;
    int totalLength = 0;
    bool ok =
        EVP_EncryptInit_ex(context, EVP_aes_256_gcm(), nullptr, nullptr, nullptr) == 1 &&
        EVP_CIPHER_CTX_ctrl(context, EVP_CTRL_GCM_SET_IVLEN, iv.size(), nullptr) == 1 &&
        EVP_EncryptInit_ex(context, nullptr, nullptr, key.data(), iv.data()) == 1 &&
        EVP_EncryptUpdate(
            context,
            nullptr,
            &outputLength,
            reinterpret_cast<const unsigned char*>(header.data()),
            static_cast<int>(header.size())) == 1 &&
        EVP_EncryptUpdate(
            context,
            reinterpret_cast<unsigned char*>(ciphertext.data()),
            &outputLength,
            reinterpret_cast<const unsigned char*>(payload.data()),
            static_cast<int>(payload.size())) == 1;
    if (ok) {
        totalLength = outputLength;
        ok = EVP_EncryptFinal_ex(
                 context,
                 reinterpret_cast<unsigned char*>(ciphertext.data()) + totalLength,
                 &outputLength) == 1;
        totalLength += outputLength;
    }

    std::array<unsigned char, 16> tag{};
    if (ok) {
        ok = EVP_CIPHER_CTX_ctrl(
                 context,
                 EVP_CTRL_GCM_GET_TAG,
                 tag.size(),
                 tag.data()) == 1;
    }
    EVP_CIPHER_CTX_free(context);
    if (!ok) {
        return std::nullopt;
    }

    ciphertext.resize(static_cast<std::size_t>(totalLength));
    std::string envelope = header + ciphertext;
    envelope.append(
        reinterpret_cast<const char*>(tag.data()),
        tag.size());
    return envelope;
}

std::optional<std::string> DecryptRelayFrame(
    const std::string& envelope,
    const RelayKey& key,
    uint64_t expectedSequence,
    const std::array<unsigned char, 4>& ivPrefix) {
    if (envelope.size() <= kEncryptedFrameOverhead ||
        envelope.size() > kMaxWireFrameBytes ||
        envelope.compare(0, kEncryptedFrameMagic.size(), kEncryptedFrameMagic) != 0) {
        return std::nullopt;
    }

    constexpr std::size_t headerSize = 12;
    const uint64_t sequence = ReadSequence(
        std::string_view(envelope).substr(kEncryptedFrameMagic.size(), 8));
    if (sequence != expectedSequence) {
        return std::nullopt;
    }

    const std::size_t ciphertextSize = envelope.size() - headerSize - 16;
    if (ciphertextSize == 0 || ciphertextSize > kMaxFrameBytes) {
        return std::nullopt;
    }
    const auto iv = BuildRelayIv(ivPrefix, sequence);
    const auto* ciphertext =
        reinterpret_cast<const unsigned char*>(envelope.data() + headerSize);
    const auto* tag =
        reinterpret_cast<const unsigned char*>(envelope.data() + headerSize + ciphertextSize);

    EVP_CIPHER_CTX* context = EVP_CIPHER_CTX_new();
    if (context == nullptr) {
        return std::nullopt;
    }
    std::string plaintext(ciphertextSize + EVP_MAX_BLOCK_LENGTH, '\0');
    int outputLength = 0;
    int totalLength = 0;
    bool ok =
        EVP_DecryptInit_ex(context, EVP_aes_256_gcm(), nullptr, nullptr, nullptr) == 1 &&
        EVP_CIPHER_CTX_ctrl(context, EVP_CTRL_GCM_SET_IVLEN, iv.size(), nullptr) == 1 &&
        EVP_DecryptInit_ex(context, nullptr, nullptr, key.data(), iv.data()) == 1 &&
        EVP_DecryptUpdate(
            context,
            nullptr,
            &outputLength,
            reinterpret_cast<const unsigned char*>(envelope.data()),
            headerSize) == 1 &&
        EVP_DecryptUpdate(
            context,
            reinterpret_cast<unsigned char*>(plaintext.data()),
            &outputLength,
            ciphertext,
            static_cast<int>(ciphertextSize)) == 1;
    if (ok) {
        totalLength = outputLength;
        ok = EVP_CIPHER_CTX_ctrl(
                 context,
                 EVP_CTRL_GCM_SET_TAG,
                 16,
                 const_cast<unsigned char*>(tag)) == 1 &&
             EVP_DecryptFinal_ex(
                 context,
                 reinterpret_cast<unsigned char*>(plaintext.data()) + totalLength,
                 &outputLength) == 1;
        totalLength += outputLength;
    }
    EVP_CIPHER_CTX_free(context);
    if (!ok || totalLength <= 0 ||
        static_cast<std::size_t>(totalLength) > kMaxFrameBytes) {
        return std::nullopt;
    }
    plaintext.resize(static_cast<std::size_t>(totalLength));
    return plaintext;
}

std::optional<std::string> JsonStringValue(const std::string& json, const std::string& key) {
    const std::string marker = "\"" + key + "\":\"";
    const std::size_t start = json.find(marker);
    if (start == std::string::npos) {
        return std::nullopt;
    }
    const std::size_t valueStart = start + marker.size();
    std::string value;
    bool escaping = false;
    for (std::size_t index = valueStart; index < json.size(); ++index) {
        const char ch = json[index];
        if (escaping) {
            switch (ch) {
                case '"':
                case '\\':
                case '/':
                    value.push_back(ch);
                    break;
                case 'n':
                    value.push_back('\n');
                    break;
                case 'r':
                    value.push_back('\r');
                    break;
                case 't':
                    value.push_back('\t');
                    break;
                default:
                    value.push_back(ch);
                    break;
            }
            escaping = false;
            continue;
        }
        if (ch == '\\') {
            escaping = true;
            continue;
        }
        if (ch == '"') {
            return value;
        }
        value.push_back(ch);
    }
    return std::nullopt;
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

std::vector<unsigned char> Base64Decode(const std::string& input) {
    static constexpr char kAlphabet[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::array<int, 256> table{};
    table.fill(-1);
    for (int i = 0; i < 64; ++i) {
        table[static_cast<unsigned char>(kAlphabet[i])] = i;
    }

    std::vector<unsigned char> out;
    int value = 0;
    int bits = -8;
    for (const unsigned char c : input) {
        if (c == '=') {
            break;
        }
        if (table[c] == -1) {
            continue;  // skip whitespace/newlines
        }
        value = (value << 6) + table[c];
        bits += 6;
        if (bits >= 0) {
            out.push_back(static_cast<unsigned char>((value >> bits) & 0xFF));
            bits -= 8;
        }
    }
    return out;
}

// Decodes a base64 PNG into a per-session temp file and returns its path, so
// notify-send can render the real Android app icon. Returns empty on failure.
std::string WriteNotificationIcon(const std::string& base64, const std::string& stableId) {
    if (base64.empty()) {
        return {};
    }
    const std::vector<unsigned char> bytes = Base64Decode(base64);
    if (bytes.empty()) {
        return {};
    }

    const char* runtimeDir = std::getenv("XDG_RUNTIME_DIR");
    const std::string dir =
        (runtimeDir != nullptr ? std::string(runtimeDir) : std::string("/tmp")) + "/inputflow-icons";
    mkdir(dir.c_str(), 0700);

    const std::string path =
        dir + "/icon-" + std::to_string(std::hash<std::string>{}(stableId)) + ".png";
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    if (!file) {
        return {};
    }
    file.write(reinterpret_cast<const char*>(bytes.data()),
               static_cast<std::streamsize>(bytes.size()));
    if (!file) {
        return {};
    }
    return path;
}

void ShowLinuxNotification(const std::string& appName,
                           const std::string& title,
                           const std::string& body,
                           const std::string& iconPath) {
    if (title.empty() && body.empty()) {
        return;
    }

    // Show the originating Android app as the notification source (-a) with its
    // real icon (-i), and keep the title clean, so it reads like a native Android
    // notification on the desktop rather than a generic "InputFlow" message.
    const std::string app = appName.empty() ? "Android" : appName;
    const std::string displayTitle = title.empty() ? app : title;
    const std::string icon = iconPath.empty() ? "inputflow" : iconPath;

    const pid_t child = fork();
    if (child == 0) {
        execlp(
            "notify-send",
            "notify-send",
            "-a",
            app.c_str(),
            "-i",
            icon.c_str(),
            displayTitle.c_str(),
            body.c_str(),
            static_cast<char*>(nullptr));
        _exit(127);
    }
    if (child > 0) {
        int status = 0;
        (void)waitpid(child, &status, 0);
    }
}

#if defined(MWB_HAVE_LIBEI_INPUT_CAPTURE)
// Presents notifications via the org.freedesktop.Notifications DBus service so we
// can attach an inline-reply action (KDE capability) and receive the reply the
// user types on the desktop. Runs its own GLib main loop thread to dispatch the
// NotificationReplied/Closed signals. One instance per process.
class NotificationPresenter {
public:
    using ReplyCallback = std::function<void(const std::string&, const std::string&)>;

    bool Start(ReplyCallback callback) {
        m_callback = std::move(callback);
        GError* error = nullptr;
        m_connection = g_bus_get_sync(G_BUS_TYPE_SESSION, nullptr, &error);
        if (m_connection == nullptr) {
            if (error != nullptr) {
                std::cerr << "WARN: notification DBus connect failed: " << error->message << std::endl;
                g_error_free(error);
            }
            return false;
        }
        m_context = g_main_context_new();
        m_loop = g_main_loop_new(m_context, FALSE);
        g_main_context_push_thread_default(m_context);
        m_repliedSub = g_dbus_connection_signal_subscribe(
            m_connection, "org.freedesktop.Notifications", "org.freedesktop.Notifications",
            "NotificationReplied", "/org/freedesktop/Notifications", nullptr,
            G_DBUS_SIGNAL_FLAGS_NONE, &NotificationPresenter::OnReplied, this, nullptr);
        m_closedSub = g_dbus_connection_signal_subscribe(
            m_connection, "org.freedesktop.Notifications", "org.freedesktop.Notifications",
            "NotificationClosed", "/org/freedesktop/Notifications", nullptr,
            G_DBUS_SIGNAL_FLAGS_NONE, &NotificationPresenter::OnClosed, this, nullptr);
        g_main_context_pop_thread_default(m_context);
        m_thread = std::thread([this]() {
            g_main_context_push_thread_default(m_context);
            g_main_loop_run(m_loop);
            g_main_context_pop_thread_default(m_context);
        });
        return true;
    }

    void Notify(const std::string& app, const std::string& title, const std::string& body,
                const std::string& iconPath, bool replyable, const std::string& stableId) {
        if (m_connection == nullptr) {
            return;
        }
        const std::string summary = title.empty() ? app : title;

        GVariantBuilder actions;
        g_variant_builder_init(&actions, G_VARIANT_TYPE("as"));
        if (replyable) {
            g_variant_builder_add(&actions, "s", "inline-reply");
            g_variant_builder_add(&actions, "s", "Reply");
        }

        GVariantBuilder hints;
        g_variant_builder_init(&hints, G_VARIANT_TYPE("a{sv}"));
        if (replyable) {
            g_variant_builder_add(&hints, "{sv}", "x-kde-reply-placeholder-text",
                                  g_variant_new_string("Reply"));
        }

        GError* error = nullptr;
        GVariant* reply = g_dbus_connection_call_sync(
            m_connection, "org.freedesktop.Notifications", "/org/freedesktop/Notifications",
            "org.freedesktop.Notifications", "Notify",
            g_variant_new("(susssasa{sv}i)", app.c_str(), 0u, iconPath.c_str(),
                          summary.c_str(), body.c_str(), &actions, &hints, -1),
            G_VARIANT_TYPE("(u)"), G_DBUS_CALL_FLAGS_NONE, -1, nullptr, &error);
        if (reply == nullptr) {
            if (error != nullptr) {
                g_error_free(error);
            }
            return;
        }
        guint32 id = 0;
        g_variant_get(reply, "(u)", &id);
        g_variant_unref(reply);
        if (replyable && id != 0) {
            std::lock_guard<std::mutex> lock(m_mapMutex);
            m_idToStable[id] = stableId;
        }
    }

    ~NotificationPresenter() {
        if (m_loop != nullptr) {
            g_main_loop_quit(m_loop);
        }
        if (m_thread.joinable()) {
            m_thread.join();
        }
        if (m_connection != nullptr) {
            if (m_repliedSub != 0) g_dbus_connection_signal_unsubscribe(m_connection, m_repliedSub);
            if (m_closedSub != 0) g_dbus_connection_signal_unsubscribe(m_connection, m_closedSub);
        }
        if (m_loop != nullptr) g_main_loop_unref(m_loop);
        if (m_context != nullptr) g_main_context_unref(m_context);
        if (m_connection != nullptr) g_object_unref(m_connection);
    }

private:
    static void OnReplied(GDBusConnection*, const gchar*, const gchar*, const gchar*,
                          const gchar*, GVariant* params, gpointer userData) {
        auto* self = static_cast<NotificationPresenter*>(userData);
        guint32 id = 0;
        const gchar* text = nullptr;
        g_variant_get(params, "(u&s)", &id, &text);
        std::string stableId;
        {
            std::lock_guard<std::mutex> lock(self->m_mapMutex);
            auto it = self->m_idToStable.find(id);
            if (it != self->m_idToStable.end()) {
                stableId = it->second;
                self->m_idToStable.erase(it);
            }
        }
        if (!stableId.empty() && self->m_callback) {
            self->m_callback(stableId, text != nullptr ? std::string(text) : std::string());
        }
    }

    static void OnClosed(GDBusConnection*, const gchar*, const gchar*, const gchar*,
                         const gchar*, GVariant* params, gpointer userData) {
        auto* self = static_cast<NotificationPresenter*>(userData);
        guint32 id = 0;
        guint32 reason = 0;
        g_variant_get(params, "(uu)", &id, &reason);
        std::lock_guard<std::mutex> lock(self->m_mapMutex);
        self->m_idToStable.erase(id);
    }

    GDBusConnection* m_connection{nullptr};
    GMainContext* m_context{nullptr};
    GMainLoop* m_loop{nullptr};
    std::thread m_thread;
    guint m_repliedSub{0};
    guint m_closedSub{0};
    std::mutex m_mapMutex;
    std::map<guint32, std::string> m_idToStable;
    ReplyCallback m_callback;
};

// Presents a synced notification, using the DBus presenter (inline reply capable)
// when available, falling back to notify-send otherwise.
void PresentNotification(AndroidRelayServer* server, const std::string& app,
                         const std::string& title, const std::string& body,
                         const std::string& iconPath, bool canReply,
                         const std::string& stableId) {
    if (title.empty() && body.empty()) {
        return;
    }
    static NotificationPresenter presenter;
    static bool started = presenter.Start(
        [server](const std::string& sid, const std::string& text) {
            server->DeliverNotificationReply(sid, text);
        });
    if (started) {
        presenter.Notify(app.empty() ? "Android" : app, title, body, iconPath, canReply, stableId);
    } else {
        ShowLinuxNotification(app, title, body, iconPath);
    }
}
#else
void PresentNotification(AndroidRelayServer*, const std::string& app, const std::string& title,
                         const std::string& body, const std::string& iconPath, bool,
                         const std::string&) {
    ShowLinuxNotification(app, title, body, iconPath);
}
#endif

} // namespace

bool VerifyAndroidRelayHmac(
    const std::string& secret,
    const std::string& nonce,
    const std::string& suppliedHmac) {
    const std::string expected = HmacSha256Hex(secret, nonce);
    return suppliedHmac.size() == expected.size() &&
           CRYPTO_memcmp(suppliedHmac.data(), expected.data(), expected.size()) == 0;
}

#if defined(MWB_TESTING)
std::optional<std::string> EncryptAndroidRelayFrameForTest(
    const std::string& payload,
    const std::string& secret,
    const std::string& nonce,
    uint64_t sequence,
    bool serverToClient) {
    return EncryptRelayFrame(
        payload,
        DeriveRelayKey(
            secret,
            nonce,
            serverToClient ? "server-to-client" : "client-to-server"),
        sequence,
        serverToClient ? kServerToClientIvPrefix : kClientToServerIvPrefix);
}

std::optional<std::string> DecryptAndroidRelayFrameForTest(
    const std::string& envelope,
    const std::string& secret,
    const std::string& nonce,
    uint64_t expectedSequence,
    bool serverToClient) {
    return DecryptRelayFrame(
        envelope,
        DeriveRelayKey(
            secret,
            nonce,
            serverToClient ? "server-to-client" : "client-to-server"),
        expectedSequence,
        serverToClient ? kServerToClientIvPrefix : kClientToServerIvPrefix);
}
#endif

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
    std::cout << "[ANDROID] Relay listening on the configured port." << std::endl;
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

        {
            std::lock_guard<std::mutex> lock(m_clientsMutex);
            if (m_clients.size() >= kMaxRelayClients) {
                close(clientFd);
                std::cerr << "WARN: Android relay client limit reached." << std::endl;
                continue;
            }
        }

        // Enable TCP keepalive so a half-open peer (Wi-Fi drop, app killed, device
        // sleep) is detected by the kernel within ~11s. Without this the blocking
        // ReadFrame never returns, the dead session lingers as authenticated, and
        // BroadcastFrame keeps shipping input into the void instead of the live
        // client. TCP_NODELAY keeps mouse latency low.
        const int on = 1;
        setsockopt(clientFd, SOL_SOCKET, SO_KEEPALIVE, &on, sizeof(on));
#ifdef TCP_KEEPIDLE
        const int idle = 5, intvl = 2, cnt = 3;
        setsockopt(clientFd, IPPROTO_TCP, TCP_KEEPIDLE, &idle, sizeof(idle));
        setsockopt(clientFd, IPPROTO_TCP, TCP_KEEPINTVL, &intvl, sizeof(intvl));
        setsockopt(clientFd, IPPROTO_TCP, TCP_KEEPCNT, &cnt, sizeof(cnt));
#endif
#ifdef TCP_NODELAY
        setsockopt(clientFd, IPPROTO_TCP, TCP_NODELAY, &on, sizeof(on));
#endif
        const timeval authenticationTimeout{5, 0};
        setsockopt(
            clientFd,
            SOL_SOCKET,
            SO_RCVTIMEO,
            &authenticationTimeout,
            sizeof(authenticationTimeout));

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
    const timeval noReadTimeout{0, 0};
    setsockopt(
        session->fd,
        SOL_SOCKET,
        SO_RCVTIMEO,
        &noReadTimeout,
        sizeof(noReadTimeout));

    while (m_running) {
        if (session->readSequence == std::numeric_limits<uint64_t>::max()) {
            break;
        }
        const auto wireFrame = ReadFrame(session->fd);
        if (!wireFrame.has_value()) {
            break;
        }
        const auto frame = DecryptRelayFrame(
            *wireFrame,
            session->readKey,
            session->readSequence,
            kClientToServerIvPrefix);
        if (!frame.has_value()) {
            std::cerr << "WARN: Android relay rejected an invalid encrypted frame." << std::endl;
            break;
        }
        ++session->readSequence;
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
        } else if (*type == "notification_upsert") {
            if (m_options.notificationSyncEnabled) {
                const std::string stableId = JsonStringValue(*frame, "stable_id").value_or("");
                const std::string iconPath = WriteNotificationIcon(
                    JsonStringValue(*frame, "icon_png").value_or(""), stableId);
                const bool canReply = frame->find("\"can_reply\":true") != std::string::npos;
                PresentNotification(
                    this,
                    JsonStringValue(*frame, "app").value_or("Android"),
                    JsonStringValue(*frame, "title").value_or(""),
                    JsonStringValue(*frame, "body").value_or(""),
                    iconPath,
                    canReply,
                    stableId);
            }
        } else if (*type == "notification_dismiss") {
            if (m_options.notificationSyncEnabled) {
                std::cout << "[ANDROID] Notification dismissed." << std::endl;
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
    const std::string hello =
        "{\"type\":\"hello\",\"version\":2,\"cipher\":\"AES-256-GCM\",\"nonce\":\"" +
        nonce + "\"}";
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

    if (!VerifyAndroidRelayHmac(m_options.secret, nonce, *hmac)) {
        std::cerr << "WARN: Android relay rejected client with invalid auth." << std::endl;
        return false;
    }

    const std::string deviceName = JsonStringValue(*auth, "device").value_or("android");
    session->deviceName = deviceName;
    session->readKey = DeriveRelayKey(m_options.secret, nonce, "client-to-server");
    session->writeKey = DeriveRelayKey(m_options.secret, nonce, "server-to-client");
    session->encryptionReady = true;
    if (!SendFrame(session, "{\"type\":\"ready\"}")) {
        return false;
    }
    {
        std::lock_guard<std::mutex> lock(m_clientsMutex);
        session->authenticated = true;
    }
    std::cout << "[ANDROID] Encrypted relay client authenticated." << std::endl;

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
    std::string wirePayload = payload;
    if (session->encryptionReady) {
        if (session->writeSequence == std::numeric_limits<uint64_t>::max()) {
            return false;
        }
        const auto encrypted = EncryptRelayFrame(
            payload,
            session->writeKey,
            session->writeSequence,
            kServerToClientIvPrefix);
        if (!encrypted.has_value()) {
            return false;
        }
        wirePayload = *encrypted;
        ++session->writeSequence;
    }
    const uint32_t length = htonl(static_cast<uint32_t>(wirePayload.size()));
    return WriteExact(session->fd, &length, sizeof(length)) &&
           WriteExact(session->fd, wirePayload.data(), wirePayload.size());
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
    std::vector<std::shared_ptr<ClientSession>> dead;
    for (const auto& client : clients) {
        if (SendFrame(client, payload)) {
            delivered = true;
        } else {
            // A failed write means the peer is gone (EPIPE/ECONNRESET). Drop it now
            // so input stops being shipped to a dead socket and the live client
            // remains the only broadcast target.
            dead.push_back(client);
        }
    }
    for (const auto& client : dead) {
        RemoveClient(client);
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

std::string BuildAndroidNotificationReplyFrame(const std::string& stableId, const std::string& text) {
    return std::string("{\"type\":\"notification_reply\",\"stable_id\":\"") + EscapeJson(stableId) +
           "\",\"text\":\"" + EscapeJson(text) + "\"}";
}

void AndroidRelayServer::DeliverNotificationReply(const std::string& stableId, const std::string& text) {
    BroadcastFrame(BuildAndroidNotificationReplyFrame(stableId, text));
}

} // namespace mwb
