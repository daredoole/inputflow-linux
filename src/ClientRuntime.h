#pragma once

#include <atomic>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <functional>

#include "ClipboardManager.h"
#include "InputDispatcher.h"
#include "InputLatencyStats.h"
#include "InputManager.h"
#include "NetworkManager.h"

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
    std::string localMachineName;
    bool debugInputLogging{false};
    bool debugKeyLogging{false};
    bool debugShortcutLogging{false};
    bool latencyReport{false};
    std::function<void(const std::string&, int, const std::string&, uint32_t, uint32_t)> onSessionEstablished;
    std::function<void()> onSessionDisconnected;
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
    void StartClipboardWatcher();
    void StopClipboardWatcher();

    RuntimeOptions m_options;
    std::atomic<bool> m_stopRequested{false};
    InputManager m_input;
    std::shared_ptr<InputLatencyStats> m_latencyStats;
    InputDispatcher m_dispatcher;
    std::unique_ptr<NetworkManager> m_network;
    std::unique_ptr<ClipboardManager> m_clipboard;
    std::atomic<bool> m_clipboardWatcherRunning{false};
    std::thread m_clipboardWatcher;
    std::mutex m_clipboardStateMutex;
    std::optional<std::string> m_lastClipboardText;
    std::string m_clipboardBackendName;
};

} // namespace mwb
