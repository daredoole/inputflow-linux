#pragma once

#include <chrono>
#include <condition_variable>
#include <deque>
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

    InputManager& m_input;
    std::shared_ptr<InputLatencyStats> m_latencyStats;
    std::shared_ptr<const TopologyModel> m_topology;
    std::string m_topologySourceDisplayId;
    bool m_topologyTraceEnabled{false};
    std::mutex m_mutex;
    std::condition_variable m_cv;
    std::deque<InputEvent> m_queue;
    bool m_running{false};
    std::thread m_thread;
};

} // namespace mwb
