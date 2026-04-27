#include "InputManager.h"
#include "InputDeviceCapabilities.h"
#include "MediaKeyBridge.h"
#include <array>
#include <cerrno>
#include <algorithm>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <fcntl.h>
#include <cstring>
#include <linux/uinput.h>
#include <sstream>
#include <unordered_map>
#include <vector>
#include <unistd.h>

namespace mwb {
namespace {

constexpr int kMoveMouseRelative = 100000;
constexpr int kDefaultRepeatDelayMs = 250;
constexpr int kDefaultRepeatPeriodMs = 33;
constexpr int kMaxScreenDimension = 65535;
constexpr int kMaxWheelDelta = 120 * 10;
constexpr int kMaxMouseTraceEntries = 1024;

uint16_t DecodeLinuxXButtonKey(int32_t mouseData) {
    switch (decodeXButtonPayload(mouseData)) {
        case 0x0001:
            return BTN_SIDE;
        case 0x0002:
            return BTN_EXTRA;
        default:
            return 0;
    }
}

bool IsTruthyEnv(const char* name) {
    const char* value = std::getenv(name);
    if (value == nullptr) {
        return false;
    }

    const std::string text(value);
    return text == "1" || text == "true" || text == "TRUE" || text == "yes" || text == "YES" || text == "on" || text == "ON";
}

bool DebugKeyLoggingEnabled() {
    static const bool enabled = IsTruthyEnv("MWB_DEBUG_KEYS");
    return enabled;
}

bool DebugShortcutLoggingEnabled() {
    static const bool enabled = IsTruthyEnv("MWB_DEBUG_SHORTCUTS");
    return enabled;
}

int ReadPositiveEnvOrDefault(const char* name, int fallback) {
    const char* value = std::getenv(name);
    if (value == nullptr || *value == '\0') {
        return fallback;
    }

    char* end = nullptr;
    const long parsed = std::strtol(value, &end, 10);
    if (end == value || *end != '\0' || parsed <= 0) {
        return fallback;
    }

    return static_cast<int>(parsed);
}

std::size_t ReadMouseTraceLimit() {
    const char* value = std::getenv("MWB_MOUSE_TRACE");
    if (value == nullptr || *value == '\0') {
        return 0;
    }

    char* end = nullptr;
    const long parsed = std::strtol(value, &end, 10);
    if (end == value || *end != '\0' || parsed <= 0) {
        return 0;
    }

    return static_cast<std::size_t>(std::clamp(parsed, 1L, static_cast<long>(kMaxMouseTraceEntries)));
}

void CloseDevice(int& fd) {
    if (fd >= 0) {
        close(fd);
        fd = -1;
    }
}

void PrintUinputOpenHelp(int err) {
    std::cerr << "ERR: Failed to open /dev/uinput: " << std::strerror(err) << std::endl;

    if (err == ENOENT) {
        std::cerr << "  /dev/uinput is missing. Load the kernel module with 'sudo modprobe uinput'." << std::endl;
        std::cerr << "  To persist it across reboots, add 'uinput' to /etc/modules-load.d/uinput.conf." << std::endl;
        return;
    }

    if (err == EACCES) {
        std::cerr << "  Permission denied. Add your user to the group that owns /dev/uinput and install a udev rule such as" << std::endl;
        std::cerr << "  KERNEL==\"uinput\", GROUP=\"inputflow\", MODE=\"0660\", OPTIONS+=\"static_node=uinput\"." << std::endl;
        std::cerr << "  In containers, pass '--device /dev/uinput:/dev/uinput'. Fedora/Podman may also require" << std::endl;
        std::cerr << "  '--security-opt label=disable' and '--group-add keep-groups'." << std::endl;
        return;
    }

    std::cerr << "  Verify that the uinput kernel module is loaded and that the current user or container can access /dev/uinput." << std::endl;
}

bool WriteInputEvents(int fd, const struct input_event* events, std::size_t count) {
    const auto* data = reinterpret_cast<const uint8_t*>(events);
    std::size_t remaining = sizeof(struct input_event) * count;

    while (remaining > 0) {
        const ssize_t written = write(fd, data, remaining);
        if (written < 0) {
            if (errno == EINTR) {
                continue;
            }
            std::cerr << "ERR: Failed to write uinput event: " << std::strerror(errno) << std::endl;
            return false;
        }

        data += written;
        remaining -= static_cast<std::size_t>(written);
    }

    return true;
}

bool HasRelativeAxis(int32_t value) {
    return value >= kMoveMouseRelative || value <= -kMoveMouseRelative;
}

int DecodeRelativeAxis(int32_t value) {
    return (value < 0) ? (value + kMoveMouseRelative) : (value - kMoveMouseRelative);
}

int ClampWheelDelta(int delta) {
    return std::clamp(delta, -kMaxWheelDelta, kMaxWheelDelta);
}

template <std::size_t N>
void AppendRelEvent(std::array<input_event, N>& events, int& count, uint16_t code, int value) {
    if (value == 0) {
        return;
    }
    if (count < 0 || static_cast<std::size_t>(count) >= N - 1) {
        return;
    }

    events[count].type = EV_REL;
    events[count].code = code;
    events[count].value = value;
    ++count;
}

template <std::size_t N>
void AppendKeyEvent(std::array<input_event, N>& events, int& count, uint16_t code, int value) {
    if (count < 0 || static_cast<std::size_t>(count) >= N - 1) {
        return;
    }

    events[count].type = EV_KEY;
    events[count].code = code;
    events[count].value = value;
    ++count;
}

bool CreateKeyboardDevice(int fd) {
    ioctl(fd, UI_SET_EVBIT, EV_KEY);

    for (int i = 0; i < 256; ++i) {
        ioctl(fd, UI_SET_KEYBIT, i);
    }

    struct uinput_user_dev uidev;
    memset(&uidev, 0, sizeof(uidev));
    std::strncpy(uidev.name, "InputFlow Virtual Keyboard", sizeof(uidev.name) - 1);
    uidev.id.bustype = BUS_USB;
    uidev.id.vendor  = 0x045E;
    uidev.id.product = 0x0002;

    if (write(fd, &uidev, sizeof(uidev)) < 0) {
        std::cerr << "ERR: Failed to write virtual keyboard setup: " << std::strerror(errno) << std::endl;
        return false;
    }

    if (ioctl(fd, UI_DEV_CREATE) < 0) {
        std::cerr << "ERR: Failed to create virtual keyboard device: " << std::strerror(errno) << std::endl;
        return false;
    }

    return true;
}

bool CreateMouseDevice(int fd, int screenW, int screenH) {
    ConfigureAbsoluteMouseDevice(fd, screenW, screenH);

    struct uinput_user_dev uidev;
    memset(&uidev, 0, sizeof(uidev));
    std::strncpy(uidev.name, "InputFlow Virtual Mouse", sizeof(uidev.name) - 1);
    uidev.id.bustype = BUS_USB;
    uidev.id.vendor  = 0x045E;
    uidev.id.product = 0x0001;
    PopulateAbsoluteMouseSetup(uidev, screenW, screenH);

    if (write(fd, &uidev, sizeof(uidev)) < 0) {
        std::cerr << "ERR: Failed to write virtual mouse setup: " << std::strerror(errno) << std::endl;
        return false;
    }

    if (ioctl(fd, UI_DEV_CREATE) < 0) {
        std::cerr << "ERR: Failed to create virtual mouse device: " << std::strerror(errno) << std::endl;
        return false;
    }

    return true;
}

bool IsModifierKey(uint16_t key) {
    switch (key) {
        case KEY_LEFTSHIFT:
        case KEY_RIGHTSHIFT:
        case KEY_LEFTCTRL:
        case KEY_RIGHTCTRL:
        case KEY_LEFTALT:
        case KEY_RIGHTALT:
        case KEY_LEFTMETA:
        case KEY_RIGHTMETA:
        case KEY_CAPSLOCK:
        case KEY_NUMLOCK:
        case KEY_SCROLLLOCK:
            return true;
        default:
            return false;
    }
}

bool IsMediaKey(uint16_t key) {
    switch (key) {
        case KEY_NEXTSONG:
        case KEY_PREVIOUSSONG:
        case KEY_STOPCD:
        case KEY_PLAYPAUSE:
            return true;
        default:
            return false;
    }
}

void LogKeyDecision(const char* stage, int vkCode, uint16_t key, uint32_t flags, const char* action) {
    if (!DebugKeyLoggingEnabled() && !DebugShortcutLoggingEnabled()) {
        return;
    }

    std::cout << "[KEY] " << stage
              << " vk=0x" << std::hex << vkCode
              << " key=0x" << key
              << " flags=0x" << flags
              << std::dec << " action=" << action << std::endl;
}

} // namespace

static std::unordered_map<uint16_t, uint16_t> s_vkToKey = {
    {0x08, KEY_BACKSPACE},
    {0x09, KEY_TAB},
    {0x0D, KEY_ENTER},
    {0x10, KEY_LEFTSHIFT},
    {0x11, KEY_LEFTCTRL},
    {0x12, KEY_LEFTALT},
    {0x13, KEY_PAUSE},
    {0x14, KEY_CAPSLOCK},
    {0x1B, KEY_ESC},
    {0x20, KEY_SPACE},
    {0x21, KEY_PAGEUP},
    {0x22, KEY_PAGEDOWN},
    {0x23, KEY_END},
    {0x24, KEY_HOME},
    {0x25, KEY_LEFT},
    {0x26, KEY_UP},
    {0x27, KEY_RIGHT},
    {0x28, KEY_DOWN},
    {0x2C, KEY_SYSRQ},
    {0x2D, KEY_INSERT},
    {0x2E, KEY_DELETE},
    {0x30, KEY_0},
    {0x31, KEY_1},
    {0x32, KEY_2},
    {0x33, KEY_3},
    {0x34, KEY_4},
    {0x35, KEY_5},
    {0x36, KEY_6},
    {0x37, KEY_7},
    {0x38, KEY_8},
    {0x39, KEY_9},
    {0x41, KEY_A},
    {0x42, KEY_B},
    {0x43, KEY_C},
    {0x44, KEY_D},
    {0x45, KEY_E},
    {0x46, KEY_F},
    {0x47, KEY_G},
    {0x48, KEY_H},
    {0x49, KEY_I},
    {0x4A, KEY_J},
    {0x4B, KEY_K},
    {0x4C, KEY_L},
    {0x4D, KEY_M},
    {0x4E, KEY_N},
    {0x4F, KEY_O},
    {0x50, KEY_P},
    {0x51, KEY_Q},
    {0x52, KEY_R},
    {0x53, KEY_S},
    {0x54, KEY_T},
    {0x55, KEY_U},
    {0x56, KEY_V},
    {0x57, KEY_W},
    {0x58, KEY_X},
    {0x59, KEY_Y},
    {0x5A, KEY_Z},
    {0x5B, KEY_LEFTMETA},
    {0x5C, KEY_RIGHTMETA},
    {0x5D, KEY_MENU},
    {0x60, KEY_KP0},
    {0x61, KEY_KP1},
    {0x62, KEY_KP2},
    {0x63, KEY_KP3},
    {0x64, KEY_KP4},
    {0x65, KEY_KP5},
    {0x66, KEY_KP6},
    {0x67, KEY_KP7},
    {0x68, KEY_KP8},
    {0x69, KEY_KP9},
    {0x6A, KEY_KPASTERISK},
    {0x6B, KEY_KPPLUS},
    {0x6D, KEY_KPMINUS},
    {0x6E, KEY_KPDOT},
    {0x6F, KEY_KPSLASH},
    {0x70, KEY_F1},
    {0x71, KEY_F2},
    {0x72, KEY_F3},
    {0x73, KEY_F4},
    {0x74, KEY_F5},
    {0x75, KEY_F6},
    {0x76, KEY_F7},
    {0x77, KEY_F8},
    {0x78, KEY_F9},
    {0x79, KEY_F10},
    {0x7A, KEY_F11},
    {0x7B, KEY_F12},
    {0x90, KEY_NUMLOCK},
    {0x91, KEY_SCROLLLOCK},
    {0xA0, KEY_LEFTSHIFT},
    {0xA1, KEY_RIGHTSHIFT},
    {0xA2, KEY_LEFTCTRL},
    {0xA3, KEY_RIGHTCTRL},
    {0xA4, KEY_LEFTALT},
    {0xA5, KEY_RIGHTALT},
    {0xB0, KEY_NEXTSONG},
    {0xB1, KEY_PREVIOUSSONG},
    {0xB2, KEY_STOPCD},
    {0xB3, KEY_PLAYPAUSE},
    {0xBA, KEY_SEMICOLON},
    {0xBB, KEY_EQUAL},
    {0xBC, KEY_COMMA},
    {0xBD, KEY_MINUS},
    {0xBE, KEY_DOT},
    {0xBF, KEY_SLASH},
    {0xC0, KEY_GRAVE},
    {0xDB, KEY_LEFTBRACE},
    {0xDC, KEY_BACKSLASH},
    {0xDD, KEY_RIGHTBRACE},
    {0xDE, KEY_APOSTROPHE},
    {0xE2, KEY_102ND}
};

InputManager::InputManager()
    : m_mouseFd(-1),
      m_keyboardFd(-1),
      m_mouseTraceLimit(ReadMouseTraceLimit()),
      m_repeatDelayMs(ReadPositiveEnvOrDefault("MWB_KEY_REPEAT_DELAY_MS", kDefaultRepeatDelayMs)),
      m_repeatPeriodMs(ReadPositiveEnvOrDefault("MWB_KEY_REPEAT_PERIOD_MS", kDefaultRepeatPeriodMs)),
      m_repeatThread(&InputManager::KeyRepeatLoop, this) {}

InputManager::~InputManager() {
    StopRepeatThread();
    ReleaseAllInput();

    if (m_keyboardFd >= 0) {
        ioctl(m_keyboardFd, UI_DEV_DESTROY);
        close(m_keyboardFd);
    }
    if (m_mouseFd >= 0) {
        ioctl(m_mouseFd, UI_DEV_DESTROY);
        close(m_mouseFd);
    }
}

bool InputManager::Initialize() {
    m_keyboardFd = open("/dev/uinput", O_WRONLY);
    if (m_keyboardFd < 0) {
        PrintUinputOpenHelp(errno);
        return false;
    }

    if (!CreateKeyboardDevice(m_keyboardFd)) {
        CloseDevice(m_keyboardFd);
        return false;
    }

    m_mouseFd = open("/dev/uinput", O_WRONLY);
    if (m_mouseFd < 0) {
        PrintUinputOpenHelp(errno);
        ioctl(m_keyboardFd, UI_DEV_DESTROY);
        CloseDevice(m_keyboardFd);
        return false;
    }

    if (!CreateMouseDevice(m_mouseFd, m_screenWidth, m_screenHeight)) {
        CloseDevice(m_mouseFd);
        ioctl(m_keyboardFd, UI_DEV_DESTROY);
        CloseDevice(m_keyboardFd);
        return false;
    }

    SetInternalCursorToCenter();
    return true;
}

void InputManager::SetScreenSize(int width, int height) {
    const int newW = (width  > 0) ? std::clamp(width,  1, kMaxScreenDimension) : m_screenWidth;
    const int newH = (height > 0) ? std::clamp(height, 1, kMaxScreenDimension) : m_screenHeight;

    if (newW == m_screenWidth && newH == m_screenHeight) {
        return;
    }

    m_screenWidth  = newW;
    m_screenHeight = newH;

    if (m_mouseFd >= 0) {
        ioctl(m_mouseFd, UI_DEV_DESTROY);
        CloseDevice(m_mouseFd);

        m_mouseFd = open("/dev/uinput", O_WRONLY);
        if (m_mouseFd < 0) {
            std::cerr << "ERR: Failed to reopen /dev/uinput after screen resize: "
                      << std::strerror(errno) << std::endl;
            return;
        }
        if (!CreateMouseDevice(m_mouseFd, m_screenWidth, m_screenHeight)) {
            CloseDevice(m_mouseFd);
        }

        SetInternalCursorToCenter();
    }
}

void InputManager::EmitAbsMotion(int px, int py) {
    if (m_mouseFd < 0) {
        return;
    }

    std::array<input_event, 3> events{};
    events[0].type = EV_ABS;
    events[0].code = ABS_X;
    events[0].value = px;
    events[1].type = EV_ABS;
    events[1].code = ABS_Y;
    events[1].value = py;
    events[2].type = EV_SYN;
    events[2].code = SYN_REPORT;
    events[2].value = 0;

    WriteInputEvents(m_mouseFd, events.data(), 3);
}

void InputManager::SetInternalCursorToCenter() {
    const int screenW = std::max(1, m_screenWidth);
    const int screenH = std::max(1, m_screenHeight);
    m_cursorAbsX = screenW / 2;
    m_cursorAbsY = screenH / 2;
}

bool InputManager::MouseTraceEnabled() const {
    return m_mouseTraceLimit > 0;
}

void InputManager::RecordMouseTrace(const MouseTraceEntry& entry) {
    if (!MouseTraceEnabled()) {
        return;
    }

    std::lock_guard<std::mutex> lock(m_mouseStateMutex);
    MouseTraceEntry stored = entry;
    stored.sequence = ++m_mouseTraceSequence;
    m_mouseTrace.push_back(stored);
    while (m_mouseTrace.size() > m_mouseTraceLimit) {
        m_mouseTrace.pop_front();
    }
}

std::vector<InputManager::MouseTraceEntry> InputManager::MouseTraceSnapshot() const {
    std::lock_guard<std::mutex> lock(m_mouseStateMutex);
    return {m_mouseTrace.begin(), m_mouseTrace.end()};
}

std::string InputManager::FormatMouseTrace(const std::vector<MouseTraceEntry>& trace) {
    std::ostringstream out;
    out << "[MOUSE_TRACE] Last " << trace.size() << " mouse packet(s)\n";
    if (trace.empty()) {
        return out.str();
    }

    for (const MouseTraceEntry& entry : trace) {
        const auto monotonicMs = std::chrono::duration_cast<std::chrono::milliseconds>(
            entry.capturedAt.time_since_epoch()).count();
        out << "[MOUSE_TRACE] #"
            << entry.sequence
            << " t_ms=" << monotonicMs
            << " kind=" << (entry.relativeMove ? "relative" : "absolute")
            << " wParam=0x" << std::hex << entry.raw.wParam << std::dec
            << " raw=(" << entry.raw.x << "," << entry.raw.y << ")"
            << " decoded=(" << entry.decodedDx << "," << entry.decodedDy << ")"
            << " screen=" << entry.screenWidth << "x" << entry.screenHeight
            << " cursor=" << entry.cursorBeforeX << "," << entry.cursorBeforeY
            << "->" << entry.cursorAfterX << "," << entry.cursorAfterY
            << " button=" << entry.buttonCode << "/" << entry.buttonValue
            << " wheel=" << entry.wheelDeltaV << "," << entry.wheelDeltaH
            << " residue=" << entry.wheelResidueV << "," << entry.wheelResidueH
            << " events=" << entry.emittedEventCount
            << " write=" << (entry.writeOk ? "ok" : "fail")
            << "\n";
    }
    return out.str();
}

void InputManager::InjectMouse(const MouseData& data) {
    if (m_mouseFd < 0) return;

    // Wire format (from real server observation):
    //   data.x     = absolute x (0-65535 across virtual desktop)
    //   data.y     = absolute y (0-65535 across virtual desktop)
    //   data.wheel = scroll delta
    //   data.wParam = WM_* mouse message code (0x0200=MOUSEMOVE etc.)
    //
    // Relative-motion encoding: wParam==0x0200 with |x|>=100000 and |y|>=100000.
    // Absolute coordinates span the virtual desktop; we map to [0, screenDim-1].
    // Device is EV_ABS — position is authoritative, libinput accel not applied.

    const int screenW = std::max(1, m_screenWidth);
    const int screenH = std::max(1, m_screenHeight);

    std::array<input_event, 16> ev{};
    int count = 0;
    MouseTraceEntry trace;
    trace.capturedAt = std::chrono::steady_clock::now();
    trace.raw = data;
    trace.screenWidth = screenW;
    trace.screenHeight = screenH;
    trace.cursorBeforeX = m_cursorAbsX;
    trace.cursorBeforeY = m_cursorAbsY;

    if (DebugShortcutLoggingEnabled() && data.wParam != 0x0200) {
        std::cout << "[MOUSE] inject wParam=0x" << std::hex << data.wParam
                  << " x=" << std::dec << data.x
                  << " y=" << data.y
                  << " mouseData=" << data.mouseData
                  << std::endl;
    }

    if (data.wParam == 0x0200 &&
        HasRelativeAxis(data.x) &&
        HasRelativeAxis(data.y)) {
        const int dx = std::clamp(DecodeRelativeAxis(data.x), -screenW, screenW);
        const int dy = std::clamp(DecodeRelativeAxis(data.y), -screenH, screenH);
        trace.relativeMove = true;
        trace.decodedDx = dx;
        trace.decodedDy = dy;
        m_cursorAbsX = std::clamp(m_cursorAbsX + dx, 0, screenW - 1);
        m_cursorAbsY = std::clamp(m_cursorAbsY + dy, 0, screenH - 1);
    } else {
        const int absX = std::clamp(static_cast<int>(data.x), 0, 65535);
        const int absY = std::clamp(static_cast<int>(data.y), 0, 65535);
        m_cursorAbsX = static_cast<int>(static_cast<int64_t>(absX) * (screenW - 1) / 65535);
        m_cursorAbsY = static_cast<int>(static_cast<int64_t>(absY) * (screenH - 1) / 65535);
    }

    ev[count].type  = EV_ABS;
    ev[count].code  = ABS_X;
    ev[count].value = m_cursorAbsX;
    ++count;
    ev[count].type  = EV_ABS;
    ev[count].code  = ABS_Y;
    ev[count].value = m_cursorAbsY;
    ++count;

    uint16_t mouseButton = 0;
    int mouseButtonValue = 0;
    trace.buttonValue = -1;

    // Button and wheel events via WM_* message code in wParam.
    switch (data.wParam) {
        case 0x0201: case 0x0203: // WM_LBUTTONDOWN / WM_LBUTTONDBLCLK
            mouseButton = BTN_LEFT; mouseButtonValue = 1; AppendKeyEvent(ev, count, BTN_LEFT, 1); break;
        case 0x0202:              // WM_LBUTTONUP
            mouseButton = BTN_LEFT; mouseButtonValue = 0; AppendKeyEvent(ev, count, BTN_LEFT, 0); break;
        case 0x0204: case 0x0206: // WM_RBUTTONDOWN / WM_RBUTTONDBLCLK
            mouseButton = BTN_RIGHT; mouseButtonValue = 1; AppendKeyEvent(ev, count, BTN_RIGHT, 1); break;
        case 0x0205:              // WM_RBUTTONUP
            mouseButton = BTN_RIGHT; mouseButtonValue = 0; AppendKeyEvent(ev, count, BTN_RIGHT, 0); break;
        case 0x0207: case 0x0209: // WM_MBUTTONDOWN / WM_MBUTTONDBLCLK
            mouseButton = BTN_MIDDLE; mouseButtonValue = 1; AppendKeyEvent(ev, count, BTN_MIDDLE, 1); break;
        case 0x0208:              // WM_MBUTTONUP
            mouseButton = BTN_MIDDLE; mouseButtonValue = 0; AppendKeyEvent(ev, count, BTN_MIDDLE, 0); break;
        case 0x020B: case 0x020D: // WM_XBUTTONDOWN / WM_XBUTTONDBLCLK
            mouseButton = DecodeLinuxXButtonKey(data.mouseData);
            if (mouseButton != 0) {
                mouseButtonValue = 1;
                AppendKeyEvent(ev, count, mouseButton, 1);
            }
            break;
        case 0x020C:              // WM_XBUTTONUP
            mouseButton = DecodeLinuxXButtonKey(data.mouseData);
            if (mouseButton != 0) {
                mouseButtonValue = 0;
                AppendKeyEvent(ev, count, mouseButton, 0);
            }
            break;
        case 0x020A: {            // WM_MOUSEWHEEL
            const int delta = ClampWheelDelta(data.mouseData);
            trace.wheelDeltaV = delta;
            m_wheelResidueV += delta;
#ifdef REL_WHEEL_HI_RES
            AppendRelEvent(ev, count, REL_WHEEL_HI_RES, delta);
#endif
            while (m_wheelResidueV >= 120) {
                AppendRelEvent(ev, count, REL_WHEEL, 1);
                m_wheelResidueV -= 120;
            }
            while (m_wheelResidueV <= -120) {
                AppendRelEvent(ev, count, REL_WHEEL, -1);
                m_wheelResidueV += 120;
            }
            break;
        }
        case 0x020E: {            // WM_MOUSEHWHEEL
            const int delta = ClampWheelDelta(data.mouseData);
            trace.wheelDeltaH = delta;
            m_wheelResidueH += delta;
#ifdef REL_HWHEEL_HI_RES
            AppendRelEvent(ev, count, REL_HWHEEL_HI_RES, delta);
#endif
            while (m_wheelResidueH >= 120) {
                AppendRelEvent(ev, count, REL_HWHEEL, 1);
                m_wheelResidueH -= 120;
            }
            while (m_wheelResidueH <= -120) {
                AppendRelEvent(ev, count, REL_HWHEEL, -1);
                m_wheelResidueH += 120;
            }
            break;
        }
        default: break;           // WM_MOUSEMOVE (0x0200) and unknowns: movement only
    }

    trace.cursorAfterX = m_cursorAbsX;
    trace.cursorAfterY = m_cursorAbsY;
    trace.buttonCode = mouseButton;
    if (mouseButton != 0) {
        trace.buttonValue = mouseButtonValue;
    }
    trace.wheelResidueV = m_wheelResidueV;
    trace.wheelResidueH = m_wheelResidueH;

    if (count > 0) {
        ev[count].type = EV_SYN;
        ev[count].code = SYN_REPORT;
        ev[count].value = 0;
        count++;
        trace.emittedEventCount = count;
        trace.writeOk = WriteInputEvents(m_mouseFd, ev.data(), static_cast<std::size_t>(count));
        if (trace.writeOk && mouseButton != 0) {
            std::lock_guard<std::mutex> lock(m_mouseStateMutex);
            if (mouseButtonValue != 0) {
                m_pressedMouseButtons.insert(mouseButton);
            } else {
                m_pressedMouseButtons.erase(mouseButton);
            }
        }
    }

    RecordMouseTrace(trace);
}

void InputManager::InjectKeyboard(const KeyboardData& data) {
    if (data.vkCode < 0 || data.vkCode > 0xFFFF) {
        LogKeyDecision("inject", data.vkCode, 0, data.flags, "drop-invalid-vk");
        return;
    }

    const bool keyUp = (data.flags & kLlkhfUp) != 0;
    if (!keyUp) {
        const auto mediaAction = MediaActionForVirtualKey(data.vkCode);
        if (mediaAction.has_value() && DispatchMediaKeyAction(*mediaAction)) {
            LogKeyDecision("inject", data.vkCode, 0, data.flags, "mpris-media");
            return;
        }
    }

    if (m_keyboardFd < 0) return;

    uint16_t key = 0;
    auto it = s_vkToKey.find(static_cast<uint16_t>(data.vkCode));
    if (it != s_vkToKey.end()) {
        key = it->second;
    } else {
        if (IsTruthyEnv("MWB_DEBUG_INPUT") || DebugKeyLoggingEnabled() || DebugShortcutLoggingEnabled()) {
            std::cout << "[INPUT] Unmapped key: VK_0x" << std::hex << data.vkCode
                      << " flags=0x" << data.flags << std::dec << std::endl;
        }
        return;
    }

    int keyValue = 0;
    {
        std::lock_guard<std::mutex> lock(m_keyStateMutex);
        if (keyUp) {
            if (m_pressedKeys.erase(key) == 0) {
                LogKeyDecision("inject", data.vkCode, key, data.flags, "drop-release-not-held");
                return;
            }
            keyValue = 0;
            if (m_repeatKey && *m_repeatKey == key) {
                m_repeatKey.reset();
                ++m_repeatGeneration;
                m_keyRepeatCv.notify_all();
            }
        } else {
            const auto [_, inserted] = m_pressedKeys.insert(key);
            if (!inserted) {
                LogKeyDecision("inject", data.vkCode, key, data.flags, "drop-duplicate-down");
                return;
            }
            keyValue = 1;
            if (!IsModifierKey(key) && !IsMediaKey(key)) {
                m_repeatKey = key;
                ++m_repeatGeneration;
                m_keyRepeatCv.notify_all();
            }
        }
    }

    struct input_event ev[2];
    memset(ev, 0, sizeof(ev));
    ev[0].type = EV_KEY;
    ev[0].code = key;
    ev[0].value = keyValue;

    ev[1].type = EV_SYN;
    ev[1].code = SYN_REPORT;
    ev[1].value = 0;

    LogKeyDecision("inject", data.vkCode, key, data.flags, keyValue == 0 ? "release" : "press");
    WriteInputEvents(m_keyboardFd, ev, sizeof(ev) / sizeof(ev[0]));
}

void InputManager::ReleaseAllInput() {
    ReleaseAllKeys();
    ReleaseAllMouseButtons();
}

void InputManager::ReleaseAllKeys() {
    if (m_keyboardFd < 0) {
        std::lock_guard<std::mutex> lock(m_keyStateMutex);
        m_pressedKeys.clear();
        m_repeatKey.reset();
        return;
    }

    std::vector<uint16_t> pressedKeys;
    {
        std::lock_guard<std::mutex> lock(m_keyStateMutex);
        if (m_pressedKeys.empty()) {
            m_repeatKey.reset();
            return;
        }
        pressedKeys.assign(m_pressedKeys.begin(), m_pressedKeys.end());
        m_pressedKeys.clear();
        m_repeatKey.reset();
        ++m_repeatGeneration;
        m_keyRepeatCv.notify_all();
    }

    for (uint16_t key : pressedKeys) {
        struct input_event ev[2];
        memset(ev, 0, sizeof(ev));
        ev[0].type = EV_KEY;
        ev[0].code = key;
        ev[0].value = 0;
        ev[1].type = EV_SYN;
        ev[1].code = SYN_REPORT;
        ev[1].value = 0;
        if (!WriteInputEvents(m_keyboardFd, ev, sizeof(ev) / sizeof(ev[0]))) {
            break;
        }
    }
}

void InputManager::ReleaseAllMouseButtons() {
    if (m_mouseFd < 0) {
        std::lock_guard<std::mutex> lock(m_mouseStateMutex);
        m_pressedMouseButtons.clear();
        return;
    }

    std::vector<uint16_t> pressedButtons;
    {
        std::lock_guard<std::mutex> lock(m_mouseStateMutex);
        if (m_pressedMouseButtons.empty()) {
            return;
        }
        pressedButtons.assign(m_pressedMouseButtons.begin(), m_pressedMouseButtons.end());
        m_pressedMouseButtons.clear();
    }

    for (uint16_t button : pressedButtons) {
        struct input_event ev[2];
        memset(ev, 0, sizeof(ev));
        ev[0].type = EV_KEY;
        ev[0].code = button;
        ev[0].value = 0;
        ev[1].type = EV_SYN;
        ev[1].code = SYN_REPORT;
        ev[1].value = 0;
        if (!WriteInputEvents(m_mouseFd, ev, sizeof(ev) / sizeof(ev[0]))) {
            break;
        }
    }
}

void InputManager::KeyRepeatLoop() {
    std::unique_lock<std::mutex> lock(m_keyStateMutex);

    while (m_repeatThreadRunning) {
        m_keyRepeatCv.wait(lock, [&]() {
            return !m_repeatThreadRunning || m_repeatKey.has_value();
        });
        if (!m_repeatThreadRunning) {
            break;
        }

        const uint16_t key = *m_repeatKey;
        const uint64_t generation = m_repeatGeneration;

        const auto stateChanged = [&]() {
            return !m_repeatThreadRunning ||
                   !m_repeatKey.has_value() ||
                   *m_repeatKey != key ||
                   m_repeatGeneration != generation;
        };

        if (m_keyRepeatCv.wait_for(lock, std::chrono::milliseconds(m_repeatDelayMs), stateChanged)) {
            continue;
        }

        while (!stateChanged()) {
            const int keyboardFd = m_keyboardFd;
            lock.unlock();

            if (keyboardFd >= 0) {
                struct input_event ev[2];
                memset(ev, 0, sizeof(ev));
                ev[0].type = EV_KEY;
                ev[0].code = key;
                ev[0].value = 2;
                ev[1].type = EV_SYN;
                ev[1].code = SYN_REPORT;
                ev[1].value = 0;
                if (DebugKeyLoggingEnabled()) {
                    std::cout << "[KEY] repeat key=0x" << std::hex << key << std::dec << std::endl;
                }
                WriteInputEvents(keyboardFd, ev, sizeof(ev) / sizeof(ev[0]));
            }

            lock.lock();
            m_keyRepeatCv.wait_for(lock, std::chrono::milliseconds(m_repeatPeriodMs), stateChanged);
        }
    }
}

void InputManager::StopRepeatThread() {
    {
        std::lock_guard<std::mutex> lock(m_keyStateMutex);
        if (!m_repeatThreadRunning) {
            return;
        }
        m_repeatThreadRunning = false;
        m_repeatKey.reset();
        ++m_repeatGeneration;
    }
    m_keyRepeatCv.notify_all();
    if (m_repeatThread.joinable()) {
        m_repeatThread.join();
    }
}

}
