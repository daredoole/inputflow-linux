#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "Protocol.h"

namespace mwb {

struct AndroidRelayOptions {
    bool enabled{false};
    int port{15102};
    std::string secret;
    std::string peerName{"android"};
    bool layoutEditorEnabled{true};
    int androidDeviceWidth{1920};
    int androidDeviceHeight{1200};
    bool notificationSyncEnabled{false};
};

class AndroidRelayServer {
public:
    explicit AndroidRelayServer(AndroidRelayOptions options);
    ~AndroidRelayServer();

    bool Start();
    void Stop();
    bool HasAuthenticatedClient() const;
    bool SendMouse(const MouseData& mouse);
    bool SendKeyboard(const KeyboardData& keyboard);
    bool SendGesture(const std::string& kind, double dx, double dy);
    bool SendControl(bool active);
    void SetOnReleaseRequested(std::function<void()> callback);
    void SetOnTopologyUpdate(std::function<void(const std::string&)> callback);

    // Called after ready to populate the devices_info frame sent to Android.
    void SetLocalDeviceInfo(const std::string& machineName, int screenWidth, int screenHeight);

    const AndroidRelayOptions& Options() const {
        return m_options;
    }

private:
    struct ClientSession {
        int fd{-1};
        bool authenticated{false};
        std::string deviceName;
        std::mutex writeMutex;
    };

    void AcceptLoop();
    void HandleClient(std::shared_ptr<ClientSession> session);
    bool AuthenticateClient(const std::shared_ptr<ClientSession>& session);
    bool SendFrame(const std::shared_ptr<ClientSession>& session, const std::string& payload);
    bool BroadcastFrame(const std::string& payload);
    void RemoveClient(const std::shared_ptr<ClientSession>& session);

    AndroidRelayOptions m_options;
    std::atomic<bool> m_running{false};
    int m_listenFd{-1};
    mutable std::mutex m_clientsMutex;
    std::vector<std::shared_ptr<ClientSession>> m_clients;
    std::vector<std::thread> m_clientThreads;
    std::thread m_acceptThread;
    std::function<void()> m_onReleaseRequested;
    std::function<void(const std::string&)> m_onTopologyUpdate;
    mutable std::mutex m_callbackMutex;

    std::string m_localMachineName;
    int m_localScreenWidth{0};
    int m_localScreenHeight{0};
};

std::string BuildAndroidMouseFrame(const MouseData& mouse);
std::string BuildAndroidKeyboardFrame(const KeyboardData& keyboard);
std::string BuildAndroidGestureFrame(const std::string& kind, double dx, double dy);
std::string BuildAndroidControlFrame(bool active);

} // namespace mwb
