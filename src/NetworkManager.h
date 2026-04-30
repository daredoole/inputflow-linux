#pragma once

#include <atomic>
#include <array>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "CryptoHelper.h"
#include "Protocol.h"
#include "ClipboardManager.h"

namespace mwb {

class NetworkManager {
public:
    NetworkManager(const std::string& host, int port, const std::string& key);
    ~NetworkManager();

    void SetOnMouseCallback(std::function<void(const MouseData&)> cb);
    void SetOnKeyboardCallback(std::function<void(const KeyboardData&)> cb);
    void SetOnClipboardCallback(std::function<void(const ClipboardPayload&)> cb);
    void SetOnSessionEstablished(std::function<void(const std::string&, int, const std::string&, uint32_t, uint32_t)> cb);
    void SetOnSessionDisconnected(std::function<void()> cb);
    void SetClipboardProvider(std::function<std::optional<ClipboardPayload>()> provider);
    void SetLocalIdentity(uint32_t machineId, const std::string& machineName = {});
    void PrimeLocalClipboardPayload(const ClipboardPayload& payload);
    void NotifyLocalClipboardChanged();
    void NotifyLocalClipboardChanged(const ClipboardPayload& payload);

    void SetAutoConnectEnabled(bool enabled) { m_autoConnectEnabled = enabled; }
    void SetReconnectBackoff(int initialBackoffMs, int maxBackoffMs, int idleRetryMs);
    bool Connect();
    void RunLoop();
    bool SendMouse(const MouseData& mouse);
    bool SendPacket(MWBPacket& packet, bool isBig);
    void Stop();
    void SetScreenSize(int w, int h) { m_screenW = w; m_screenH = h; }

private:
    std::string m_host;
    int m_port;
    int m_clipboardPort;
    std::string m_key;
    CryptoHelper m_crypto;
    int m_socket;
    uint32_t m_magic{0};
    uint32_t m_sessionId{0};
    uint32_t m_srcId{0};
    uint32_t m_desId{0};
    std::string m_myName;

    std::atomic<bool> m_running{false};
    std::atomic<bool> m_autoConnectEnabled{true};
    std::atomic<uint32_t> m_nextPacketId{1};
    std::atomic<bool> m_clipboardPullActive{false};
    std::atomic<bool> m_clipboardPushActive{false};
    std::atomic<int> m_activeServerSessions{0};
    std::atomic<int> m_activeClipboardSessions{0};
    std::atomic<int> m_activeClipboardWorkers{0};
    std::atomic<uint32_t> m_expectedPeerAddress{0};
    bool m_handshakeDone{false};
    std::thread m_heartbeatThread;
    std::mutex m_sendMutex;
    std::mutex m_clipboardMutex;
    std::mutex m_sessionSocketMutex;
    std::mutex m_activeSessionPeerMutex;
    std::mutex m_inboundControlSessionMutex;

    uint32_t m_myId{0};
    int m_screenW{1920};
    int m_screenH{1080};

    int m_serverFd{-1};
    std::thread m_serverThread;
    int m_clipboardServerFd{-1};
    std::thread m_clipboardServerThread;

    std::function<void(const MouseData&)> m_onMouse;
    std::function<void(const KeyboardData&)> m_onKeyboard;
    std::function<void(const ClipboardPayload&)> m_onClipboard;
    std::function<void(const std::string&, int, const std::string&, uint32_t, uint32_t)> m_onSessionEstablished;
    std::function<void()> m_onSessionDisconnected;
    std::function<std::optional<ClipboardPayload>()> m_clipboardProvider;
    std::optional<ClipboardPayload> m_pendingClipboardPayloadStruct;
    std::vector<uint8_t> m_pendingClipboardPayload;
    std::vector<uint8_t> m_inlineClipboardBuffer;
    uint8_t m_inlineClipboardType{0};
    bool m_discardInlineClipboard{false};
    std::optional<ClipboardPayload> m_suppressedClipboardPayload;
    std::string m_remoteName;
    std::unordered_map<uint32_t, int> m_activeSessionPeers;
    std::unordered_set<int> m_sessionSockets;
    int m_activeInboundControlFd{-1};
    uint32_t m_activeInboundControlRemoteMachineId{0};
    int m_reconnectInitialBackoffMs{1000};
    int m_reconnectMaxBackoffMs{30000};
    int m_reconnectIdleRetryMs{30000};

    void HeartbeatLoop();
    void SendHello();
    void SendHeartbeat(bool initial = false);
    void SendIdentity();
    void SendSmallControlPacket(PackageType type);
    bool ConnectOutbound(std::array<uint8_t, 16>& handshakeChallenge);
    void StartServerListener();
    void ServerListenerLoop();
    void HandleWindowsConnection(int fd);

    void StartClipboardServerListener();
    void ClipboardServerListenerLoop();
    void HandleClipboardConnection(int fd);
    void WaitForInboundSessionsToFinish();
    void TrackSessionSocket(int fd);
    void UntrackSessionSocket(int fd);
    void ShutdownSessionSockets();
    bool TryAcquireInboundControlSession(int fd, uint32_t remoteMachineId);
    void ReleaseInboundControlSession(int fd);
    void RegisterActiveSessionPeer(uint32_t remoteMachineId);
    void UnregisterActiveSessionPeer(uint32_t remoteMachineId);
    bool HasActiveSessionPeer(uint32_t remoteMachineId);

    void HandleClipboardControlPacket(const MWBPacket& packet, uint32_t remoteMachineId);
    void FinalizeInlineClipboardTransfer();
    void DeliverClipboardPayload(const ClipboardPayload& payload);
    void RequestRemoteClipboard(uint32_t expectedRemoteMachineId);
    void PushClipboardToRemote(uint32_t expectedRemoteMachineId);

    bool UpdatePendingClipboardPayload(const ClipboardPayload& payload);
    std::optional<std::vector<uint8_t>> SnapshotClipboardPayload(bool refreshFromProvider);
    std::optional<std::vector<uint8_t>> SnapshotClipboardImagePayload(bool refreshFromProvider);
    bool SendClipboardAnnouncement();
    bool SendClipboardAsk();
    bool SendInlineClipboardText(const std::vector<uint8_t>& payload);
    bool SendInlineClipboardImage(const std::vector<uint8_t>& payload);
};

} // namespace mwb
