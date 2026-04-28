#include "InputDispatcher.h"

#include <algorithm>
#include <optional>

namespace mwb {
namespace {

constexpr int kMoveMouseRelative = 100000;
constexpr std::size_t kMaxQueuedEvents = 2048;

bool IsMouseMoveOnly(const MouseData& data) {
    return data.wParam == 0x0200;
}

bool IsRelativeMouseMove(const MouseData& data) {
    return IsMouseMoveOnly(data) &&
           std::abs(data.x) >= kMoveMouseRelative &&
           std::abs(data.y) >= kMoveMouseRelative;
}

int DecodeRelativeAxis(int value) {
    return (value < 0) ? (value + kMoveMouseRelative) : (value - kMoveMouseRelative);
}

int EncodeRelativeAxis(int delta) {
    return delta + (delta < 0 ? -kMoveMouseRelative : kMoveMouseRelative);
}

bool TryMergeMouseMove(MouseData& base, const MouseData& next) {
    if (!IsMouseMoveOnly(base) || !IsMouseMoveOnly(next)) {
        return false;
    }

    const bool baseRelative = IsRelativeMouseMove(base);
    const bool nextRelative = IsRelativeMouseMove(next);
    if (baseRelative != nextRelative) {
        return false;
    }

    if (!baseRelative) {
        base = next;
        return true;
    }

    base.x = EncodeRelativeAxis(DecodeRelativeAxis(base.x) + DecodeRelativeAxis(next.x));
    base.y = EncodeRelativeAxis(DecodeRelativeAxis(base.y) + DecodeRelativeAxis(next.y));
    return true;
}

} // namespace

InputDispatcher::InputDispatcher(InputManager& input, std::shared_ptr<InputLatencyStats> latencyStats)
    : m_input(input),
      m_latencyStats(std::move(latencyStats)) {}

InputDispatcher::~InputDispatcher() {
    Stop();
}

void InputDispatcher::Start() {
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_running) {
            return;
        }
        m_running = true;
    }

    m_thread = std::thread([this]() { Run(); });
}

void InputDispatcher::Stop() {
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (!m_running) {
            return;
        }
        m_running = false;
        m_queue.clear();
    }

    m_cv.notify_all();
    if (m_thread.joinable()) {
        m_thread.join();
    }
    m_input.ReleaseAllInput();
}

void InputDispatcher::ResetInputState() {
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_queue.clear();
    }
    m_input.ReleaseAllInput();
}

void InputDispatcher::SubmitMouse(const MouseData& mouse) {
    Enqueue(InputEvent{
        InputEvent::Kind::Mouse,
        mouse,
        {},
        m_latencyStats ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{}
    });
}

void InputDispatcher::SubmitKeyboard(const KeyboardData& keyboard) {
    Enqueue(InputEvent{
        InputEvent::Kind::Keyboard,
        {},
        keyboard,
        m_latencyStats ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{}
    });
}

void InputDispatcher::Enqueue(InputEvent event) {
    std::size_t queueDepth = 0;
    const auto enqueuedKind =
        (event.kind == InputEvent::Kind::Mouse) ? InputLatencyStats::InputKind::Mouse : InputLatencyStats::InputKind::Keyboard;
    std::optional<std::pair<InputLatencyStats::InputKind, bool>> dropped;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (!m_running) {
            return;
        }
        if (m_queue.size() >= kMaxQueuedEvents) {
            auto drop = std::find_if(m_queue.begin(), m_queue.end(), [](const InputEvent& queued) {
                return queued.kind == InputEvent::Kind::Mouse && IsMouseMoveOnly(queued.mouse);
            });
            if (drop != m_queue.end()) {
                dropped = std::pair{InputLatencyStats::InputKind::Mouse, true};
                m_queue.erase(drop);
            } else {
                const InputEvent& droppedEvent = m_queue.front();
                dropped = std::pair{
                    (droppedEvent.kind == InputEvent::Kind::Mouse)
                        ? InputLatencyStats::InputKind::Mouse
                        : InputLatencyStats::InputKind::Keyboard,
                    droppedEvent.kind == InputEvent::Kind::Mouse && IsMouseMoveOnly(droppedEvent.mouse)
                };
                m_queue.pop_front();
            }
        }
        m_queue.push_back(std::move(event));
        queueDepth = m_queue.size();
    }

    if (m_latencyStats) {
        if (dropped.has_value()) {
            m_latencyStats->RecordDrop(dropped->first, dropped->second);
        }
        m_latencyStats->RecordEnqueue(enqueuedKind, queueDepth);
    }
    m_cv.notify_one();
}

bool InputDispatcher::PopNext(InputEvent& event) {
    std::size_t mergedMoves = 0;
    std::unique_lock<std::mutex> lock(m_mutex);
    m_cv.wait(lock, [&]() { return !m_running || !m_queue.empty(); });
    if (m_queue.empty()) {
        return false;
    }

    event = std::move(m_queue.front());
    m_queue.pop_front();

    if (event.kind == InputEvent::Kind::Mouse && IsMouseMoveOnly(event.mouse)) {
        while (!m_queue.empty() &&
               m_queue.front().kind == InputEvent::Kind::Mouse &&
               TryMergeMouseMove(event.mouse, m_queue.front().mouse)) {
            m_queue.pop_front();
            ++mergedMoves;
        }
    }

    if (m_latencyStats && mergedMoves != 0) {
        m_latencyStats->RecordMerge(mergedMoves);
    }
    return true;
}

void InputDispatcher::Run() {
    while (true) {
        InputEvent event{};
        if (!PopNext(event)) {
            break;
        }

        const auto kind =
            (event.kind == InputEvent::Kind::Mouse) ? InputLatencyStats::InputKind::Mouse : InputLatencyStats::InputKind::Keyboard;
        const auto dispatchStarted = std::chrono::steady_clock::now();
        if (m_latencyStats && event.enqueuedAt.time_since_epoch().count() != 0) {
            m_latencyStats->RecordQueueWait(kind, dispatchStarted - event.enqueuedAt);
        }

        if (event.kind == InputEvent::Kind::Mouse) {
            m_input.InjectMouse(event.mouse);
        } else {
            m_input.InjectKeyboard(event.keyboard);
        }

        if (m_latencyStats) {
            m_latencyStats->RecordInjectDuration(kind, std::chrono::steady_clock::now() - dispatchStarted);
        }
    }
}

} // namespace mwb
