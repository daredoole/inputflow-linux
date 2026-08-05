#pragma once

#include <atomic>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

#include "AndroidRelay.h"
#include "ClipboardManager.h"
#include "InputDispatcher.h"
#include "InputLatencyStats.h"
#include "InputManager.h"
#include "LibeiInputCaptureBridge.h"
#include "LocalAndroidInputBridge.h"
#include "NetworkManager.h"
#include "TopologyModel.h"

namespace mwb {

struct RuntimeOptions {
    std::string host;
    std::string key;
    int port{15101};
    bool clipboardEnabled{true};
    bool clipboardSendEnabled{true};
    bool clipboardForcePoll{false};
    int clipboardPollMs{1000};
    bool autoConnectEnabled{true};
    int reconnectInitialBackoffMs{1000};
    int reconnectMaxBackoffMs{30000};
    int reconnectIdleRetryMs{30000};
    std::optional<int> screenWidth;
    std::optional<int> screenHeight;
    bool mprisMediaKeysEnabled{true};
    std::string mprisPlayer;
    std::optional<uint32_t> localMachineId;
    uint32_t expectedRemoteMachineId{0};
    std::string localMachineName;
    bool debugInputLogging{false};
    bool debugKeyLogging{false};
    bool debugShortcutLogging{false};
    bool latencyReport{false};
    bool powerToysCompatibilityEnabled{true};
    bool inputFlowPeersEnabled{false};
    bool topologyRuntimeEnabled{false};
    std::filesystem::path topologyFilePath;
    std::string androidCaptureBackend{"none"};
    AndroidRelayOptions androidRelay;
    std::function<void(const std::string&, int, const std::string&, uint32_t, uint32_t)> onSessionEstablished;
    std::function<void()> onSessionDisconnected;
    // Discovers candidate endpoints for the intended approved peer. The
    // network layer authenticates each candidate's machine id before use.
    std::function<PeerHostResolution()> resolveHost;
};

class ClientRuntime {
public:
    struct ScreenSize {
        enum class Source {
            Explicit,
            WaylandLogical,
            Drm,
            Fallback
        };

        int width{1920};
        int height{1080};
        Source source{Source::Fallback};
    };

    explicit ClientRuntime(RuntimeOptions options);
    ~ClientRuntime();

    int Run();
    void Stop();

private:
    ScreenSize DetectScreenSize() const;
    void ConfigureTopologyPreview(const ScreenSize& screenSize);
    void StartClipboardWatcher();
    void StopClipboardWatcher();
    void StartLocalAndroidInputBridge(const ScreenSize& screenSize);
    void StopLocalAndroidInputBridge();
    bool TrySendAndroidMouse(const MouseData& mouse);
    bool TrySendAndroidKeyboard(const KeyboardData& keyboard);
    bool TrySendAndroidGesture(const std::string& kind, double dx, double dy);
    bool TrySetAndroidControlActive(bool active);
    void ApplyAndroidTopologyUpdate(const std::string& layoutJson);

    RuntimeOptions m_options;
    std::atomic<bool> m_stopRequested{false};
    std::atomic<bool> m_androidRelayActive{false};
    InputManager m_input;
    std::shared_ptr<InputLatencyStats> m_latencyStats;
    InputDispatcher m_dispatcher;
    std::shared_ptr<TopologyModel> m_topology;
    std::unique_ptr<NetworkManager> m_network;
    std::unique_ptr<AndroidRelayServer> m_androidRelay;
    std::unique_ptr<LibeiInputCaptureBridge> m_libeiInputCaptureBridge;
    std::unique_ptr<LocalAndroidInputBridge> m_localAndroidInputBridge;
    std::unique_ptr<ClipboardManager> m_clipboard;
    std::atomic<bool> m_clipboardWatcherRunning{false};
    std::thread m_clipboardWatcher;
    std::mutex m_clipboardStateMutex;
    std::optional<ClipboardPayload> m_lastClipboardPayload;
    std::string m_clipboardBackendName;
};

} // namespace mwb
