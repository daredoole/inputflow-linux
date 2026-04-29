#pragma once

#include <chrono>
#include <condition_variable>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

#include "InputManager.h"
#include "InputLatencyStats.h"
#include "Protocol.h"
#include "TopologyModel.h"

namespace mwb {

class InputDispatcher {
public:
    using TopologyHandoffCallback =
        std::function<bool(const MouseData&, const TopologyPointerTransition&, const std::string&)>;

    explicit InputDispatcher(InputManager& input, std::shared_ptr<InputLatencyStats> latencyStats = nullptr);
    ~InputDispatcher();

    void Start();
    void Stop();
    void ResetInputState();
    void SubmitMouse(const MouseData& mouse);
    void SubmitKeyboard(const KeyboardData& keyboard);
    void SetTopologyPreview(std::shared_ptr<const TopologyModel> topology,
                            std::string sourceDisplayId,
                            bool traceEnabled = true);
    void SetTopologyHandoff(std::string localMachineId,
                            int desktopWidth,
                            int desktopHeight,
                            bool enabled,
                            TopologyHandoffCallback callback);

private:
    struct InputEvent {
        enum class Kind {
            Mouse,
            Keyboard
        };

        Kind kind;
        MouseData mouse;
        KeyboardData keyboard;
        std::chrono::steady_clock::time_point enqueuedAt{};
    };

    void Enqueue(InputEvent event);
    bool PopNext(InputEvent& event);
    void Run();
    std::optional<TopologyPointerTransition> ResolveTopologyPreviewTransition(const MouseData& mouse) const;
    void TraceTopologyPreviewTransition(const TopologyPointerTransition& transition) const;
    bool TryEnforceTopologyHandoff(const MouseData& mouse, const TopologyPointerTransition& transition);

    InputManager& m_input;
    std::shared_ptr<InputLatencyStats> m_latencyStats;
    std::shared_ptr<const TopologyModel> m_topology;
    std::string m_topologySourceDisplayId;
    std::string m_topologyLocalMachineId;
    int m_topologyDesktopWidth{0};
    int m_topologyDesktopHeight{0};
    bool m_topologyTraceEnabled{false};
    bool m_topologyHandoffEnabled{false};
    TopologyHandoffCallback m_topologyHandoffCallback;
    std::mutex m_mutex;
    std::condition_variable m_cv;
    std::deque<InputEvent> m_queue;
    bool m_running{false};
    std::thread m_thread;
};

} // namespace mwb
