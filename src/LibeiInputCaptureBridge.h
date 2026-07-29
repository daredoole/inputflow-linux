#pragma once

#include <atomic>
#include <functional>
#include <memory>
#include <string>
#include <thread>

#include "Protocol.h"
#include "TopologyModel.h"

namespace mwb {

// The portal handshake can display an interactive desktop permission dialog.
// Never repeat it automatically: a timeout or dismissal must require an
// explicit service restart before another prompt is requested.
inline constexpr int kLibeiInputCaptureAutomaticSetupAttempts = 1;

struct LibeiInputCaptureBridgeOptions {
    int desktopWidth{0};
    int desktopHeight{0};
    std::shared_ptr<const TopologyModel> topology;
    std::string localMachineName;
    std::string androidPeerName;
    std::function<bool(const MouseData&)> sendMouse;
    std::function<bool(const MouseData&, const std::string&)> sendMouseToMachine;
    std::function<bool(const KeyboardData&)> sendKeyboard;
    std::function<bool(const KeyboardData&, const std::string&)> sendKeyboardToMachine;
    std::function<bool(const std::string&, double, double)> sendGesture;
    std::function<bool(bool)> sendControl;
    std::function<bool(bool, const std::string&)> sendControlForMachine;
};

class LibeiInputCaptureBridge {
public:
    explicit LibeiInputCaptureBridge(LibeiInputCaptureBridgeOptions options);
    ~LibeiInputCaptureBridge();

    bool Start();
    void Stop();

private:
    void Run();
    void RunLibinputGestureMonitor();

    LibeiInputCaptureBridgeOptions m_options;
    std::atomic<bool> m_running{false};
    std::thread m_thread;
    std::thread m_gestureThread;
};

} // namespace mwb
