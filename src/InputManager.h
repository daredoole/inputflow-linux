#pragma once
#include "Protocol.h"
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <unordered_set>
#include <vector>

namespace mwb {

class InputManager {
public:
    struct MouseTraceEntry {
        uint64_t sequence{0};
        std::chrono::steady_clock::time_point capturedAt;
        MouseData raw{};
        bool relativeMove{false};
        int decodedDx{0};
        int decodedDy{0};
        int screenWidth{0};
        int screenHeight{0};
        int cursorBeforeX{0};
        int cursorBeforeY{0};
        int cursorAfterX{0};
        int cursorAfterY{0};
        uint16_t buttonCode{0};
        int buttonValue{-1};
        int wheelDeltaV{0};
        int wheelDeltaH{0};
        int wheelResidueV{0};
        int wheelResidueH{0};
        int emittedEventCount{0};
        bool writeOk{false};
    };

    InputManager();
    ~InputManager();

    // Sets up secure uinput kernel file descriptor mirroring Input Leap capabilities
    bool Initialize();

    // Sets the local desktop size used to scale MWB absolute coordinates.
    void SetScreenSize(int width, int height);

    // Maps MWB Windows coordinates / flags to EV_ABS, EV_REL wheel, and EV_KEY sequences.
    void InjectMouse(const MouseData& data);

    // Translates Virtual Key Codes to EV_KEY definitions
    void InjectKeyboard(const KeyboardData& data);

    void ReleaseAllInput();
    void ReleaseAllKeys();
    void ReleaseAllMouseButtons();
    bool MouseTraceEnabled() const;
    std::vector<MouseTraceEntry> MouseTraceSnapshot() const;
    static std::string FormatMouseTrace(const std::vector<MouseTraceEntry>& trace);

private:
    void EmitAbsMotion(int px, int py);
    void SetInternalCursorToCenter();
    void RecordMouseTrace(const MouseTraceEntry& entry);
    void KeyRepeatLoop();
    void StopRepeatThread();

    int m_mouseFd;
    int m_keyboardFd;
    int m_screenWidth{1920};
    int m_screenHeight{1080};
    int m_cursorAbsX{0};
    int m_cursorAbsY{0};
    int m_wheelResidueV{0};
    int m_wheelResidueH{0};
    std::size_t m_mouseTraceLimit{0};
    uint64_t m_mouseTraceSequence{0};
    std::deque<MouseTraceEntry> m_mouseTrace;
    int m_repeatDelayMs{250};
    int m_repeatPeriodMs{33};
    std::mutex m_keyStateMutex;
    mutable std::mutex m_mouseStateMutex;
    std::condition_variable m_keyRepeatCv;
    std::unordered_set<uint16_t> m_pressedKeys;
    std::unordered_set<uint16_t> m_pressedMouseButtons;
    std::optional<uint16_t> m_repeatKey;
    uint64_t m_repeatGeneration{0};
    bool m_repeatThreadRunning{true};
    std::thread m_repeatThread;
};

}
