#pragma once

#include <atomic>
#include <functional>
#include <memory>
#include <string>
#include <thread>

#include "Protocol.h"
#include "TopologyModel.h"

namespace mwb {

struct LocalAndroidInputBridgeOptions {
    std::shared_ptr<const TopologyModel> topology;
    std::string localMachineName;
    std::string androidPeerName;
    int desktopWidth{0};
    int desktopHeight{0};
    std::function<bool(const MouseData&)> sendMouse;
    std::function<bool(const std::string&, double, double)> sendGesture;
    std::function<bool(bool)> sendControl;
};

class LocalAndroidInputBridge {
public:
    explicit LocalAndroidInputBridge(LocalAndroidInputBridgeOptions options);
    ~LocalAndroidInputBridge();

    bool Start();
    void Stop();

private:
    void Run();

    LocalAndroidInputBridgeOptions m_options;
    std::atomic<bool> m_running{false};
    std::thread m_thread;
};

} // namespace mwb
