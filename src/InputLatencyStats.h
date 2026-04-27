#pragma once

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <deque>
#include <iomanip>
#include <limits>
#include <mutex>
#include <sstream>
#include <string>
#include <vector>

namespace mwb {

class InputLatencyStats {
public:
    enum class InputKind {
        Mouse,
        Keyboard
    };

    struct DurationSummary {
        uint64_t count{0};
        uint64_t averageUs{0};
        uint64_t p95Us{0};
        uint64_t p99Us{0};
        uint64_t maxUs{0};
        std::size_t recentSampleCount{0};
    };

    struct Summary {
        bool enabled{false};
        uint64_t enqueuedMouse{0};
        uint64_t enqueuedKeyboard{0};
        uint64_t droppedMouseMoves{0};
        uint64_t droppedOtherEvents{0};
        uint64_t mergedMouseMoves{0};
        std::size_t maxQueueDepth{0};
        DurationSummary mouseQueueWait;
        DurationSummary keyboardQueueWait;
        DurationSummary mouseInject;
        DurationSummary keyboardInject;
    };

    explicit InputLatencyStats(bool enabled = true)
        : m_enabled(enabled) {}

    bool Enabled() const {
        return m_enabled;
    }

    void RecordEnqueue(InputKind kind, std::size_t queueDepth) {
        if (!m_enabled) {
            return;
        }

        std::lock_guard<std::mutex> lock(m_mutex);
        if (kind == InputKind::Mouse) {
            ++m_enqueuedMouse;
        } else {
            ++m_enqueuedKeyboard;
        }
        m_maxQueueDepth = std::max(m_maxQueueDepth, queueDepth);
    }

    void RecordDrop(InputKind kind, bool mouseMoveOnly) {
        if (!m_enabled) {
            return;
        }

        std::lock_guard<std::mutex> lock(m_mutex);
        if (kind == InputKind::Mouse && mouseMoveOnly) {
            ++m_droppedMouseMoves;
        } else {
            ++m_droppedOtherEvents;
        }
    }

    void RecordMerge(std::size_t mergedMoves) {
        if (!m_enabled || mergedMoves == 0) {
            return;
        }

        std::lock_guard<std::mutex> lock(m_mutex);
        m_mergedMouseMoves += mergedMoves;
    }

    void RecordQueueWait(InputKind kind, std::chrono::steady_clock::duration duration) {
        RecordDuration(kind == InputKind::Mouse ? m_mouseQueueWait : m_keyboardQueueWait, duration);
    }

    void RecordInjectDuration(InputKind kind, std::chrono::steady_clock::duration duration) {
        RecordDuration(kind == InputKind::Mouse ? m_mouseInject : m_keyboardInject, duration);
    }

    Summary Snapshot() const {
        std::lock_guard<std::mutex> lock(m_mutex);

        Summary summary;
        summary.enabled = m_enabled;
        summary.enqueuedMouse = m_enqueuedMouse;
        summary.enqueuedKeyboard = m_enqueuedKeyboard;
        summary.droppedMouseMoves = m_droppedMouseMoves;
        summary.droppedOtherEvents = m_droppedOtherEvents;
        summary.mergedMouseMoves = m_mergedMouseMoves;
        summary.maxQueueDepth = m_maxQueueDepth;
        summary.mouseQueueWait = m_mouseQueueWait.Snapshot();
        summary.keyboardQueueWait = m_keyboardQueueWait.Snapshot();
        summary.mouseInject = m_mouseInject.Snapshot();
        summary.keyboardInject = m_keyboardInject.Snapshot();
        return summary;
    }

    static std::string FormatReport(const Summary& summary) {
        std::ostringstream out;
        out << "[LATENCY] Input pipeline summary\n";
        if (!summary.enabled) {
            out << "[LATENCY] Collector disabled\n";
            return out.str();
        }

        out << "[LATENCY] enqueued mouse=" << summary.enqueuedMouse
            << " keyboard=" << summary.enqueuedKeyboard
            << " dropped_mouse_moves=" << summary.droppedMouseMoves
            << " dropped_other=" << summary.droppedOtherEvents
            << " merged_mouse_moves=" << summary.mergedMouseMoves
            << " max_queue_depth=" << summary.maxQueueDepth << "\n";
        out << FormatDurationLine("mouse queue wait", summary.mouseQueueWait);
        out << FormatDurationLine("mouse inject", summary.mouseInject);
        out << FormatDurationLine("keyboard queue wait", summary.keyboardQueueWait);
        out << FormatDurationLine("keyboard inject", summary.keyboardInject);
        return out.str();
    }

private:
    struct DurationWindow {
        uint64_t count{0};
        uint64_t sumUs{0};
        uint64_t maxUs{0};
        std::deque<uint32_t> recentUs;

        void Add(uint64_t microseconds) {
            ++count;
            sumUs += microseconds;
            maxUs = std::max(maxUs, microseconds);
            if (recentUs.size() >= kMaxRecentSamples) {
                recentUs.pop_front();
            }
            recentUs.push_back(static_cast<uint32_t>(std::min<uint64_t>(
                microseconds, std::numeric_limits<uint32_t>::max())));
        }

        DurationSummary Snapshot() const {
            DurationSummary summary;
            summary.count = count;
            summary.averageUs = (count == 0) ? 0 : (sumUs / count);
            summary.maxUs = maxUs;
            summary.recentSampleCount = recentUs.size();
            if (recentUs.empty()) {
                return summary;
            }

            std::vector<uint32_t> samples(recentUs.begin(), recentUs.end());
            std::sort(samples.begin(), samples.end());
            summary.p95Us = samples[PercentileIndex(samples.size(), 95.0)];
            summary.p99Us = samples[PercentileIndex(samples.size(), 99.0)];
            return summary;
        }

        static std::size_t PercentileIndex(std::size_t sampleCount, double percentile) {
            if (sampleCount == 0) {
                return 0;
            }
            const double rank = (percentile / 100.0) * static_cast<double>(sampleCount);
            return std::min<std::size_t>(
                sampleCount - 1,
                static_cast<std::size_t>(std::ceil(rank)) - 1);
        }

        static constexpr std::size_t kMaxRecentSamples = 50000;
    };

    static std::string FormatDurationLine(const char* label, const DurationSummary& summary) {
        std::ostringstream out;
        out << "[LATENCY] " << label << " n=" << summary.count;
        if (summary.count == 0) {
            out << " no_samples\n";
            return out.str();
        }

        out << " avg=" << FormatMilliseconds(summary.averageUs)
            << " p95=" << FormatMilliseconds(summary.p95Us)
            << " p99=" << FormatMilliseconds(summary.p99Us)
            << " max=" << FormatMilliseconds(summary.maxUs)
            << " recent_window=" << summary.recentSampleCount
            << "\n";
        return out.str();
    }

    static std::string FormatMilliseconds(uint64_t microseconds) {
        std::ostringstream out;
        out << std::fixed << std::setprecision(3)
            << (static_cast<double>(microseconds) / 1000.0) << "ms";
        return out.str();
    }

    void RecordDuration(DurationWindow& window, std::chrono::steady_clock::duration duration) {
        if (!m_enabled) {
            return;
        }

        const auto microseconds = std::chrono::duration_cast<std::chrono::microseconds>(duration);
        const uint64_t clamped = static_cast<uint64_t>(std::max<int64_t>(0, microseconds.count()));
        std::lock_guard<std::mutex> lock(m_mutex);
        window.Add(clamped);
    }

    bool m_enabled{false};
    mutable std::mutex m_mutex;
    uint64_t m_enqueuedMouse{0};
    uint64_t m_enqueuedKeyboard{0};
    uint64_t m_droppedMouseMoves{0};
    uint64_t m_droppedOtherEvents{0};
    uint64_t m_mergedMouseMoves{0};
    std::size_t m_maxQueueDepth{0};
    DurationWindow m_mouseQueueWait;
    DurationWindow m_keyboardQueueWait;
    DurationWindow m_mouseInject;
    DurationWindow m_keyboardInject;
};

} // namespace mwb
