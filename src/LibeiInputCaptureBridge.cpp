#include "LibeiInputCaptureBridge.h"
#include "SecureFile.h"

#include <linux/input-event-codes.h>

#if defined(MWB_HAVE_LIBEI_INPUT_CAPTURE)
#include <gio/gio.h>
#include <gio/gunixfdlist.h>
#include <libei.h>
#endif

#if defined(MWB_HAVE_LIBINPUT_GESTURES)
#include <fcntl.h>
#include <libinput.h>
#endif

#if defined(MWB_HAVE_LIBEI_INPUT_CAPTURE) || defined(MWB_HAVE_LIBINPUT_GESTURES)
#include <poll.h>
#endif

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cctype>
#include <cstdio>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <cmath>
#include <iostream>
#include <map>
#include <optional>
#include <sstream>
#include <string>
#include <sys/wait.h>
#include <unistd.h>
#include <thread>
#include <vector>

namespace mwb {
namespace {

constexpr uint32_t WM_MOUSEMOVE = 0x0200;
constexpr uint32_t WM_LBUTTONDOWN = 0x0201;
constexpr uint32_t WM_LBUTTONUP = 0x0202;
constexpr uint32_t WM_RBUTTONDOWN = 0x0204;
constexpr uint32_t WM_RBUTTONUP = 0x0205;
constexpr uint32_t WM_MBUTTONDOWN = 0x0207;
constexpr uint32_t WM_MBUTTONUP = 0x0208;
constexpr uint32_t WM_MOUSEWHEEL = 0x020A;
constexpr uint32_t LLKHF_UP = 0x80;

int ClampNormalized(int value) {
    return std::max(0, std::min(65535, value));
}

std::optional<int32_t> LinuxKeyToVirtualKey(uint32_t key) {
    // Linux letter keycodes are in QWERTY layout order, not alphabetical — must map each explicitly.
    switch (key) {
    case KEY_Q: return 'Q';
    case KEY_W: return 'W';
    case KEY_E: return 'E';
    case KEY_R: return 'R';
    case KEY_T: return 'T';
    case KEY_Y: return 'Y';
    case KEY_U: return 'U';
    case KEY_I: return 'I';
    case KEY_O: return 'O';
    case KEY_P: return 'P';
    case KEY_A: return 'A';
    case KEY_S: return 'S';
    case KEY_D: return 'D';
    case KEY_F: return 'F';
    case KEY_G: return 'G';
    case KEY_H: return 'H';
    case KEY_J: return 'J';
    case KEY_K: return 'K';
    case KEY_L: return 'L';
    case KEY_Z: return 'Z';
    case KEY_X: return 'X';
    case KEY_C: return 'C';
    case KEY_V: return 'V';
    case KEY_B: return 'B';
    case KEY_N: return 'N';
    case KEY_M: return 'M';
    default: break;
    }

    if (key >= KEY_1 && key <= KEY_9) {
        return static_cast<int32_t>('1' + (key - KEY_1));
    }
    if (key == KEY_0) {
        return static_cast<int32_t>('0');
    }
    if (key >= KEY_F1 && key <= KEY_F12) {
        return 0x70 + static_cast<int32_t>(key - KEY_F1);
    }

    switch (key) {
    case KEY_BACKSPACE:
        return 0x08;
    case KEY_TAB:
        return 0x09;
    case KEY_ENTER:
        return 0x0D;
    case KEY_ESC:
        return 0x1B;
    case KEY_SPACE:
        return 0x20;
    case KEY_PAGEUP:
        return 0x21;
    case KEY_PAGEDOWN:
        return 0x22;
    case KEY_END:
        return 0x23;
    case KEY_HOME:
        return 0x24;
    case KEY_LEFT:
        return 0x25;
    case KEY_UP:
        return 0x26;
    case KEY_RIGHT:
        return 0x27;
    case KEY_DOWN:
        return 0x28;
    case KEY_INSERT:
        return 0x2D;
    case KEY_DELETE:
        return 0x2E;
    case KEY_LEFTSHIFT:
    case KEY_RIGHTSHIFT:
        return 0x10;
    case KEY_LEFTCTRL:
    case KEY_RIGHTCTRL:
        return 0x11;
    case KEY_LEFTALT:
    case KEY_RIGHTALT:
        return 0x12;
    case KEY_LEFTMETA:
        return 0x5B;
    case KEY_RIGHTMETA:
        return 0x5C;
    case KEY_KP0:
        return 0x60;
    case KEY_KP1:
        return 0x61;
    case KEY_KP2:
        return 0x62;
    case KEY_KP3:
        return 0x63;
    case KEY_KP4:
        return 0x64;
    case KEY_KP5:
        return 0x65;
    case KEY_KP6:
        return 0x66;
    case KEY_KP7:
        return 0x67;
    case KEY_KP8:
        return 0x68;
    case KEY_KP9:
        return 0x69;
    case KEY_KPASTERISK:
        return 0x6A;
    case KEY_KPPLUS:
        return 0x6B;
    case KEY_KPENTER:
        return 0x0D;
    case KEY_KPMINUS:
        return 0x6D;
    case KEY_KPDOT:
        return 0x6E;
    case KEY_KPSLASH:
        return 0x6F;
    case KEY_COMMA:
        return 0xBC;
    case KEY_DOT:
        return 0xBE;
    case KEY_SLASH:
        return 0xBF;
    case KEY_SEMICOLON:
        return 0xBA;
    case KEY_APOSTROPHE:
        return 0xDE;
    case KEY_LEFTBRACE:
        return 0xDB;
    case KEY_RIGHTBRACE:
        return 0xDD;
    case KEY_MINUS:
        return 0xBD;
    case KEY_EQUAL:
        return 0xBB;
    case KEY_GRAVE:
        return 0xC0;
    case KEY_BACKSLASH:
        return 0xDC;
    default:
        return std::nullopt;
    }
}

std::string ToLower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return value;
}

std::optional<std::string> FindTouchpadEventDevice() {
    const std::filesystem::path inputClass{"/sys/class/input"};
    std::error_code ec;
    for (const auto& entry : std::filesystem::directory_iterator(inputClass, ec)) {
        if (ec) {
            break;
        }
        const auto eventName = entry.path().filename().string();
        if (eventName.size() <= 5 ||
            eventName.rfind("event", 0) != 0 ||
            !std::all_of(eventName.begin() + 5, eventName.end(), [](unsigned char ch) {
                return std::isdigit(ch) != 0;
            })) {
            continue;
        }
        std::ifstream nameFile(entry.path() / "device" / "name");
        std::string name;
        if (!std::getline(nameFile, name)) {
            continue;
        }
        const auto lower = ToLower(name);
        if (lower.find("touchpad") != std::string::npos) {
            return "/dev/input/" + eventName;
        }
    }
    return std::nullopt;
}

std::optional<std::pair<double, double>> ParseDeltaToken(const std::string& token) {
    const auto slash = token.find('/');
    if (slash == std::string::npos) {
        return std::nullopt;
    }
    try {
        return std::pair<double, double>{
            std::stod(token.substr(0, slash)),
            std::stod(token.substr(slash + 1)),
        };
    } catch (...) {
        return std::nullopt;
    }
}

bool EmitClassifiedSwipe(
    const std::function<bool(const std::string&, double, double)>& sendGesture,
    int fingers,
    double dx,
    double dy,
    bool cancelled) {
    if (!sendGesture || cancelled || fingers < 3) {
        return false;
    }

    const double absX = std::abs(dx);
    const double absY = std::abs(dy);
    const bool vertical = absY >= 36.0 && absY >= absX * 1.35;
    const bool horizontal = absX >= 36.0 && absX >= absY * 1.35;
    if (!vertical && !horizontal) {
        return false;
    }

    return sendGesture(fingers >= 4 ? "swipe4" : "swipe3", dx, dy);
}

#if defined(MWB_HAVE_LIBINPUT_GESTURES)

int OpenLibinputDevice(const char* path, int flags, void*) {
    const int fd = open(path, flags);
    return fd < 0 ? -errno : fd;
}

void CloseLibinputDevice(int fd, void*) {
    close(fd);
}

const libinput_interface kLibinputInterface{
    OpenLibinputDevice,
    CloseLibinputDevice,
};

bool RunNativeLibinputGestureMonitor(
    const std::string& device,
    const std::atomic<bool>& running,
    const std::function<bool(const std::string&, double, double)>& sendGesture) {
    libinput* context = libinput_path_create_context(&kLibinputInterface, nullptr);
    if (!context) {
        std::cerr << "WARN: Failed to create native libinput gesture context." << std::endl;
        return false;
    }

    libinput_device* inputDevice = libinput_path_add_device(context, device.c_str());
    if (!inputDevice) {
        std::cerr << "WARN: Failed to open the selected device through native libinput."
                  << std::endl;
        libinput_unref(context);
        return false;
    }

    std::cout << "[ANDROID] Native libinput gesture monitor started." << std::endl;

    bool swipeActive = false;
    int swipeFingers = 0;
    double swipeDx = 0.0;
    double swipeDy = 0.0;
    bool swipeCancelled = false;

    bool pinchActive = false;
    double pinchScale = 1.0;
    bool pinchCancelled = false;

    while (running) {
        libinput_dispatch(context);
        while (libinput_event* event = libinput_get_event(context)) {
            const auto type = libinput_event_get_type(event);
            if (type < LIBINPUT_EVENT_GESTURE_SWIPE_BEGIN ||
                type > LIBINPUT_EVENT_GESTURE_HOLD_END) {
                libinput_event_destroy(event);
                continue;
            }
            switch (type) {
            case LIBINPUT_EVENT_GESTURE_SWIPE_BEGIN: {
                libinput_event_gesture* gesture = libinput_event_get_gesture_event(event);
                swipeActive = true;
                swipeFingers = libinput_event_gesture_get_finger_count(gesture);
                swipeDx = 0.0;
                swipeDy = 0.0;
                swipeCancelled = false;
                break;
            }
            case LIBINPUT_EVENT_GESTURE_SWIPE_UPDATE: {
                if (swipeActive) {
                    libinput_event_gesture* gesture = libinput_event_get_gesture_event(event);
                    swipeDx += libinput_event_gesture_get_dx(gesture);
                    swipeDy += libinput_event_gesture_get_dy(gesture);
                }
                break;
            }
            case LIBINPUT_EVENT_GESTURE_SWIPE_END: {
                if (swipeActive) {
                    libinput_event_gesture* gesture = libinput_event_get_gesture_event(event);
                    swipeCancelled = libinput_event_gesture_get_cancelled(gesture) != 0;
                    EmitClassifiedSwipe(sendGesture, swipeFingers, swipeDx, swipeDy, swipeCancelled);
                    swipeActive = false;
                }
                break;
            }
            case LIBINPUT_EVENT_GESTURE_PINCH_BEGIN:
                pinchActive = true;
                pinchScale = 1.0;
                pinchCancelled = false;
                break;
            case LIBINPUT_EVENT_GESTURE_PINCH_UPDATE: {
                if (pinchActive) {
                    libinput_event_gesture* gesture = libinput_event_get_gesture_event(event);
                    pinchScale = libinput_event_gesture_get_scale(gesture);
                }
                break;
            }
            case LIBINPUT_EVENT_GESTURE_PINCH_END: {
                if (pinchActive) {
                    libinput_event_gesture* gesture = libinput_event_get_gesture_event(event);
                    pinchCancelled = libinput_event_gesture_get_cancelled(gesture) != 0;
                    if (!pinchCancelled && (pinchScale < 0.92 || pinchScale > 1.08) && sendGesture) {
                        sendGesture("pinch", pinchScale, 0.0);
                    }
                    pinchActive = false;
                }
                break;
            }
            default:
                break;
            }
            libinput_event_destroy(event);
        }

        pollfd fd{libinput_get_fd(context), POLLIN, 0};
        poll(&fd, 1, 250);
    }

    libinput_path_remove_device(inputDevice);
    libinput_unref(context);
    return true;
}

#endif

#if defined(MWB_HAVE_LIBEI_INPUT_CAPTURE)

constexpr const char* kPortalBusName = "org.freedesktop.portal.Desktop";
constexpr const char* kInputCapturePath = "/org/freedesktop/portal/desktop";
constexpr const char* kInputCaptureInterface = "org.freedesktop.portal.InputCapture";
constexpr uint32_t kPersistentInputCapturePortalVersion = 2;
constexpr uint32_t kPersistUntilRevoked = 2;
constexpr uint32_t kCapabilityKeyboard = 1;
constexpr uint32_t kCapabilityPointer = 2;
constexpr uint32_t kCapabilityMask = kCapabilityKeyboard | kCapabilityPointer;

constexpr uint32_t kLeftBarrierId = 1;
constexpr uint32_t kRightBarrierId = 2;
constexpr uint32_t kUpBarrierId = 3;
constexpr uint32_t kDownBarrierId = 4;

struct CaptureTarget {
    uint32_t barrierId{kRightBarrierId};
    EdgeDirection exitEdge{EdgeDirection::Right};
    EdgeDirection entryEdge{EdgeDirection::Left};
    std::string machineId;
    int startX{0};
    int startY{32767};
};

struct RequestResult {
    bool done{false};
    uint32_t response{2};
    GVariant* results{nullptr};
};

std::string UniqueToken(const char* prefix) {
    const auto now = std::chrono::steady_clock::now().time_since_epoch().count();
    return std::string(prefix) + "_" + std::to_string(getpid()) + "_" + std::to_string(now);
}

void FreeError(GError* error, const char* context) {
    if (!error) {
        return;
    }
    std::cerr << "WARN: libei input capture " << context << " failed: " << error->message << std::endl;
    g_error_free(error);
}

void OnRequestResponse(GDBusConnection*,
                       const gchar*,
                       const gchar*,
                       const gchar*,
                       const gchar*,
                       GVariant* parameters,
                       gpointer userData) {
    auto* result = static_cast<RequestResult*>(userData);
    uint32_t response = 2;
    GVariant* results = nullptr;
    g_variant_get(parameters, "(u@a{sv})", &response, &results);
    result->response = response;
    result->results = results;
    result->done = true;
}

guint SubscribeToRequest(GDBusConnection* connection,
                         const char* requestPath,
                         RequestResult& result) {
    return g_dbus_connection_signal_subscribe(
        connection,
        kPortalBusName,
        "org.freedesktop.portal.Request",
        "Response",
        requestPath,
        nullptr,
        G_DBUS_SIGNAL_FLAGS_NO_MATCH_RULE,
        OnRequestResponse,
        &result,
        nullptr);
}

bool WaitForSubscribedRequest(GDBusConnection* connection,
                              const char* requestPath,
                              RequestResult& result,
                              guint subscription,
                              int timeoutMs) {
    if (subscription == 0) {
        std::cerr << "WARN: Could not subscribe to libei input capture request: " << requestPath
                  << std::endl;
        return false;
    }

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    while (!result.done && std::chrono::steady_clock::now() < deadline) {
        while (!result.done && g_main_context_pending(nullptr)) {
            g_main_context_iteration(nullptr, FALSE);
        }
        if (!result.done) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    }

    g_dbus_connection_signal_unsubscribe(connection, subscription);
    if (!result.done) {
        std::cerr << "WARN: libei input capture request timed out: " << requestPath << std::endl;
        return false;
    }
    if (result.response != 0) {
        std::cerr << "WARN: libei input capture request was denied/cancelled: " << requestPath
                  << " response=" << result.response << std::endl;
        return false;
    }
    return true;
}

bool WaitForRequest(GDBusConnection* connection,
                    const char* requestPath,
                    RequestResult& result,
                    int timeoutMs) {
    return WaitForSubscribedRequest(
        connection,
        requestPath,
        result,
        SubscribeToRequest(connection, requestPath, result),
        timeoutMs);
}

std::string ExpectedRequestPath(GDBusConnection* connection, const std::string& token) {
    const char* uniqueName = g_dbus_connection_get_unique_name(connection);
    if (!uniqueName || uniqueName[0] == '\0') {
        return {};
    }

    std::string sender = uniqueName[0] == ':' ? uniqueName + 1 : uniqueName;
    std::replace(sender.begin(), sender.end(), '.', '_');
    return "/org/freedesktop/portal/desktop/request/" + sender + "/" + token;
}

uint32_t InputCapturePortalVersion(GDBusConnection* connection) {
    GError* error = nullptr;
    GVariant* reply = g_dbus_connection_call_sync(
        connection,
        kPortalBusName,
        kInputCapturePath,
        "org.freedesktop.DBus.Properties",
        "Get",
        g_variant_new("(ss)", kInputCaptureInterface, "version"),
        G_VARIANT_TYPE("(v)"),
        G_DBUS_CALL_FLAGS_NONE,
        1000,
        nullptr,
        &error);
    if (!reply) {
        FreeError(error, "portal version query");
        return 1;
    }

    GVariant* value = nullptr;
    g_variant_get(reply, "(@v)", &value);
    GVariant* unboxedValue = g_variant_get_variant(value);
    const uint32_t version = g_variant_get_uint32(unboxedValue);
    g_variant_unref(unboxedValue);
    g_variant_unref(value);
    g_variant_unref(reply);
    return version;
}

std::filesystem::path RestoreTokenPath() {
    const char* stateDirectory = g_get_user_state_dir();
    if (!stateDirectory || stateDirectory[0] == '\0') {
        return {};
    }
    return std::filesystem::path(stateDirectory) / "inputflow" / "input-capture.restore-token";
}

std::string LoadRestoreToken() {
    const std::filesystem::path path = RestoreTokenPath();
    if (path.empty()) {
        return {};
    }

    std::error_code error;
    const auto status = std::filesystem::symlink_status(path, error);
    if (error || !std::filesystem::is_regular_file(status)) {
        return {};
    }

    std::ifstream input(path, std::ios::binary);
    std::string token;
    std::getline(input, token);
    if (!input && !input.eof()) {
        return {};
    }
    if (token.size() > 16384) {
        std::cerr << "WARN: Ignoring oversized libei input capture restore token." << std::endl;
        return {};
    }
    return token;
}

void StoreRestoreToken(const std::string& token) {
    const std::filesystem::path path = RestoreTokenPath();
    if (path.empty()) {
        std::cerr << "WARN: Cannot persist libei input capture permission: user state directory unavailable."
                  << std::endl;
        return;
    }

    std::string error;
    if (!WritePrivateFileAtomically(path, token, error)) {
        std::cerr << "WARN: Cannot persist libei input capture permission: " << error << std::endl;
    }
}

std::optional<std::string> CreateLegacySession(GDBusConnection* connection) {
    GVariantBuilder options;
    g_variant_builder_init(&options, G_VARIANT_TYPE("a{sv}"));
    const std::string token = UniqueToken("inputflow_android");
    g_variant_builder_add(&options, "{sv}", "session_handle_token", g_variant_new_string(token.c_str()));
    g_variant_builder_add(&options, "{sv}", "capabilities", g_variant_new_uint32(kCapabilityMask));

    GError* error = nullptr;
    GVariant* reply = g_dbus_connection_call_sync(
        connection,
        kPortalBusName,
        kInputCapturePath,
        kInputCaptureInterface,
        "CreateSession",
        g_variant_new("(sa{sv})", "", &options),
        G_VARIANT_TYPE("(o)"),
        G_DBUS_CALL_FLAGS_NONE,
        -1,
        nullptr,
        &error);
    if (!reply) {
        FreeError(error, "CreateSession");
        return std::nullopt;
    }

    const char* requestPath = nullptr;
    g_variant_get(reply, "(&o)", &requestPath);
    const std::string request(requestPath ? requestPath : "");
    g_variant_unref(reply);
    if (request.empty()) {
        return std::nullopt;
    }

    RequestResult result;
    if (!WaitForRequest(connection, request.c_str(), result, 30000)) {
        if (result.results) {
            g_variant_unref(result.results);
        }
        return std::nullopt;
    }

    const char* sessionHandle = nullptr;
    const bool ok = result.results && g_variant_lookup(result.results, "session_handle", "&o", &sessionHandle);
    std::optional<std::string> out;
    if (ok && sessionHandle) {
        out = sessionHandle;
    } else {
        std::cerr << "WARN: libei input capture CreateSession returned no session_handle." << std::endl;
    }
    if (result.results) {
        g_variant_unref(result.results);
    }
    return out;
}

std::optional<std::string> CreatePersistentSession(GDBusConnection* connection) {
    GVariantBuilder options;
    g_variant_builder_init(&options, G_VARIANT_TYPE("a{sv}"));
    const std::string token = UniqueToken("inputflow_android");
    g_variant_builder_add(&options, "{sv}", "session_handle_token", g_variant_new_string(token.c_str()));

    GError* error = nullptr;
    GVariant* reply = g_dbus_connection_call_sync(
        connection,
        kPortalBusName,
        kInputCapturePath,
        kInputCaptureInterface,
        "CreateSession2",
        g_variant_new("(a{sv})", &options),
        G_VARIANT_TYPE("(a{sv})"),
        G_DBUS_CALL_FLAGS_NONE,
        -1,
        nullptr,
        &error);
    if (!reply) {
        FreeError(error, "CreateSession2");
        return std::nullopt;
    }

    GVariant* results = nullptr;
    g_variant_get(reply, "(@a{sv})", &results);
    const char* sessionHandle = nullptr;
    const bool ok = results && g_variant_lookup(results, "session_handle", "&o", &sessionHandle);
    std::optional<std::string> out;
    if (ok && sessionHandle) {
        out = sessionHandle;
    } else {
        std::cerr << "WARN: libei input capture CreateSession2 returned no session_handle." << std::endl;
    }
    if (results) {
        g_variant_unref(results);
    }
    g_variant_unref(reply);
    return out;
}

bool StartPersistentSession(GDBusConnection* connection, const std::string& sessionHandle) {
    GVariantBuilder options;
    g_variant_builder_init(&options, G_VARIANT_TYPE("a{sv}"));
    const std::string requestToken = UniqueToken("start");
    g_variant_builder_add(
        &options, "{sv}", "handle_token", g_variant_new_string(requestToken.c_str()));
    g_variant_builder_add(&options, "{sv}", "capabilities", g_variant_new_uint32(kCapabilityMask));
    g_variant_builder_add(&options, "{sv}", "persist_mode", g_variant_new_uint32(kPersistUntilRevoked));

    const std::string restoreToken = LoadRestoreToken();
    if (!restoreToken.empty()) {
        g_variant_builder_add(
            &options, "{sv}", "restore_token", g_variant_new_string(restoreToken.c_str()));
        std::cerr << "INFO: Restoring the saved input capture permission." << std::endl;
    } else {
        std::cerr << "INFO: In the one-time Input Capture dialog, select 'Allow restoring on "
                     "future sessions' before choosing Allow."
                  << std::endl;
    }

    // A restored portal session may answer immediately. Subscribe to the
    // predictable request path before Start so that response cannot be lost
    // between the method reply and signal subscription.
    RequestResult result;
    const std::string expectedRequest = ExpectedRequestPath(connection, requestToken);
    guint subscription = expectedRequest.empty()
                             ? 0
                             : SubscribeToRequest(connection, expectedRequest.c_str(), result);

    GError* error = nullptr;
    GVariant* reply = g_dbus_connection_call_sync(
        connection,
        kPortalBusName,
        kInputCapturePath,
        kInputCaptureInterface,
        "Start",
        g_variant_new("(osa{sv})", sessionHandle.c_str(), "", &options),
        G_VARIANT_TYPE("(o)"),
        G_DBUS_CALL_FLAGS_NONE,
        -1,
        nullptr,
        &error);
    if (!reply) {
        if (subscription != 0) {
            g_dbus_connection_signal_unsubscribe(connection, subscription);
        }
        FreeError(error, "Start");
        return false;
    }

    const char* requestPath = nullptr;
    g_variant_get(reply, "(&o)", &requestPath);
    const std::string request(requestPath ? requestPath : "");
    g_variant_unref(reply);

    if (request.empty()) {
        if (subscription != 0) {
            g_dbus_connection_signal_unsubscribe(connection, subscription);
        }
        return false;
    }
    if (request != expectedRequest) {
        if (subscription != 0) {
            g_dbus_connection_signal_unsubscribe(connection, subscription);
        }
        result = {};
        subscription = SubscribeToRequest(connection, request.c_str(), result);
    }

    const bool ok = WaitForSubscribedRequest(
        connection, request.c_str(), result, subscription, 30000);
    if (ok && result.results) {
        const char* newRestoreToken = nullptr;
        if (g_variant_lookup(result.results, "restore_token", "&s", &newRestoreToken) &&
            newRestoreToken && newRestoreToken[0] != '\0') {
            StoreRestoreToken(newRestoreToken);
            std::cerr << "INFO: Input capture permission saved for future sessions." << std::endl;
        } else {
            if (!restoreToken.empty()) {
                // A successfully used token is single-use. Do not retain a
                // stale token when the portal declines to issue a replacement.
                StoreRestoreToken({});
            }
            std::cerr << "WARN: Portal granted input capture without a persistent restore token; "
                         "the desktop may prompt again after restart. Select 'Allow restoring on "
                         "future sessions' in the permission dialog."
                      << std::endl;
        }
    }
    if (result.results) {
        g_variant_unref(result.results);
    }
    return ok;
}

struct ZoneInfo {
    uint32_t width{0};
    uint32_t height{0};
    int32_t x{0};
    int32_t y{0};
    uint32_t zoneSet{0};
};

std::optional<ZoneInfo> GetZones(GDBusConnection* connection, const std::string& sessionHandle) {
    GVariantBuilder options;
    g_variant_builder_init(&options, G_VARIANT_TYPE("a{sv}"));
    g_variant_builder_add(&options, "{sv}", "handle_token", g_variant_new_string(UniqueToken("zones").c_str()));

    GError* error = nullptr;
    GVariant* reply = g_dbus_connection_call_sync(
        connection,
        kPortalBusName,
        kInputCapturePath,
        kInputCaptureInterface,
        "GetZones",
        g_variant_new("(oa{sv})", sessionHandle.c_str(), &options),
        G_VARIANT_TYPE("(o)"),
        G_DBUS_CALL_FLAGS_NONE,
        -1,
        nullptr,
        &error);
    if (!reply) {
        FreeError(error, "GetZones");
        return std::nullopt;
    }
    const char* requestPath = nullptr;
    g_variant_get(reply, "(&o)", &requestPath);
    const std::string request(requestPath ? requestPath : "");
    g_variant_unref(reply);

    RequestResult result;
    if (request.empty() || !WaitForRequest(connection, request.c_str(), result, 30000)) {
        if (result.results) {
            g_variant_unref(result.results);
        }
        return std::nullopt;
    }

    ZoneInfo selected;
    g_variant_lookup(result.results, "zone_set", "u", &selected.zoneSet);
    GVariant* zones = g_variant_lookup_value(result.results, "zones", G_VARIANT_TYPE("a(uuii)"));
    if (!zones || g_variant_n_children(zones) == 0) {
        std::cerr << "WARN: libei input capture portal returned no zones." << std::endl;
        if (zones) {
            g_variant_unref(zones);
        }
        g_variant_unref(result.results);
        return std::nullopt;
    }

    uint64_t bestArea = 0;
    for (gsize index = 0; index < g_variant_n_children(zones); ++index) {
        uint32_t width = 0;
        uint32_t height = 0;
        int32_t x = 0;
        int32_t y = 0;
        g_variant_get_child(zones, index, "(uuii)", &width, &height, &x, &y);
        const uint64_t area = static_cast<uint64_t>(width) * height;
        if (area > bestArea) {
            bestArea = area;
            selected.width = width;
            selected.height = height;
            selected.x = x;
            selected.y = y;
        }
    }
    g_variant_unref(zones);
    g_variant_unref(result.results);
    return selected;
}

uint32_t BarrierIdForEdge(EdgeDirection edge) {
    switch (edge) {
    case EdgeDirection::Left:
        return kLeftBarrierId;
    case EdgeDirection::Right:
        return kRightBarrierId;
    case EdgeDirection::Up:
        return kUpBarrierId;
    case EdgeDirection::Down:
        return kDownBarrierId;
    }
    return kRightBarrierId;
}

void AddBarrierForEdge(GVariantBuilder& barriers, const ZoneInfo& zone, EdgeDirection edge) {
    GVariantBuilder barrier;
    g_variant_builder_init(&barrier, G_VARIANT_TYPE("a{sv}"));
    g_variant_builder_add(&barrier, "{sv}", "barrier_id", g_variant_new_uint32(BarrierIdForEdge(edge)));

    int32_t x1 = zone.x;
    int32_t y1 = zone.y;
    int32_t x2 = zone.x;
    int32_t y2 = zone.y;
    switch (edge) {
    case EdgeDirection::Left:
        x1 = x2 = zone.x;
        y1 = zone.y;
        y2 = zone.y + static_cast<int32_t>(zone.height) - 1;
        break;
    case EdgeDirection::Right:
        x1 = x2 = zone.x + static_cast<int32_t>(zone.width);
        y1 = zone.y;
        y2 = zone.y + static_cast<int32_t>(zone.height) - 1;
        break;
    case EdgeDirection::Up:
        x1 = zone.x;
        x2 = zone.x + static_cast<int32_t>(zone.width) - 1;
        y1 = y2 = zone.y;
        break;
    case EdgeDirection::Down:
        x1 = zone.x;
        x2 = zone.x + static_cast<int32_t>(zone.width) - 1;
        y1 = y2 = zone.y + static_cast<int32_t>(zone.height);
        break;
    }
    g_variant_builder_add(&barrier, "{sv}", "position", g_variant_new("(iiii)", x1, y1, x2, y2));
    g_variant_builder_add_value(&barriers, g_variant_builder_end(&barrier));
}

bool SetPointerBarriers(GDBusConnection* connection,
                        const std::string& sessionHandle,
                        const ZoneInfo& zone,
                        const std::vector<CaptureTarget>& targets) {
    if (targets.empty()) {
        return false;
    }

    GVariantBuilder options;
    g_variant_builder_init(&options, G_VARIANT_TYPE("a{sv}"));
    g_variant_builder_add(&options, "{sv}", "handle_token", g_variant_new_string(UniqueToken("barrier").c_str()));

    GVariantBuilder barriers;
    g_variant_builder_init(&barriers, G_VARIANT_TYPE("aa{sv}"));
    std::vector<EdgeDirection> addedEdges;
    for (const auto& target : targets) {
        if (std::find(addedEdges.begin(), addedEdges.end(), target.exitEdge) != addedEdges.end()) {
            continue;
        }
        AddBarrierForEdge(barriers, zone, target.exitEdge);
        addedEdges.push_back(target.exitEdge);
    }

    GError* error = nullptr;
    GVariant* reply = g_dbus_connection_call_sync(
        connection,
        kPortalBusName,
        kInputCapturePath,
        kInputCaptureInterface,
        "SetPointerBarriers",
        g_variant_new("(oa{sv}aa{sv}u)", sessionHandle.c_str(), &options, &barriers, zone.zoneSet),
        G_VARIANT_TYPE("(o)"),
        G_DBUS_CALL_FLAGS_NONE,
        -1,
        nullptr,
        &error);
    if (!reply) {
        FreeError(error, "SetPointerBarriers");
        return false;
    }
    const char* requestPath = nullptr;
    g_variant_get(reply, "(&o)", &requestPath);
    const std::string request(requestPath ? requestPath : "");
    g_variant_unref(reply);

    RequestResult result;
    const bool ok = !request.empty() && WaitForRequest(connection, request.c_str(), result, 30000);
    if (ok && result.results) {
        GVariant* failed = g_variant_lookup_value(result.results, "failed_barriers", G_VARIANT_TYPE("au"));
        if (failed && g_variant_n_children(failed) > 0) {
            std::cerr << "WARN: libei input capture portal rejected one or more topology barriers." << std::endl;
            g_variant_unref(failed);
            g_variant_unref(result.results);
            return false;
        }
        if (failed) {
            g_variant_unref(failed);
        }
    }
    if (result.results) {
        g_variant_unref(result.results);
    }
    return ok;
}

std::optional<EdgeDirection> DominantEdgeFromDelta(double dx, double dy) {
    if (std::abs(dx) >= std::abs(dy)) {
        if (dx > 0.0) return EdgeDirection::Right;
        if (dx < 0.0) return EdgeDirection::Left;
    } else {
        if (dy > 0.0) return EdgeDirection::Down;
        if (dy < 0.0) return EdgeDirection::Up;
    }
    return std::nullopt;
}

// Normalized margin the pointer is seeded *inside* the entry edge when capture
// engages. Entering exactly on the edge (0 or 65535) sits on the release
// threshold, so the tiniest opposite jitter would instantly bounce control back
// to the local desktop. Seeding inside this margin gives the handoff hysteresis.
constexpr int kEntryMargin = 1500;

int NudgeInward(int value) {
    if (value <= 0) {
        return kEntryMargin;
    }
    if (value >= 65535) {
        return 65535 - kEntryMargin;
    }
    return value;
}

bool MovementReturnsToLocal(const CaptureTarget& target, int x, int y, double dx, double dy) {
    switch (target.entryEdge) {
    case EdgeDirection::Left:
        return x <= 0 && dx < 0.0;
    case EdgeDirection::Right:
        return x >= 65535 && dx > 0.0;
    case EdgeDirection::Up:
        return y <= 0 && dy < 0.0;
    case EdgeDirection::Down:
        return y >= 65535 && dy > 0.0;
    }
    return false;
}

std::vector<CaptureTarget> BuildTopologyCaptureTargets(const LibeiInputCaptureBridgeOptions& options) {
    std::vector<CaptureTarget> targets;
    if (!options.topology || options.localMachineName.empty() ||
        options.desktopWidth <= 0 || options.desktopHeight <= 0) {
        return targets;
    }

    const std::pair<EdgeDirection, std::pair<int, int>> probes[] = {
        {EdgeDirection::Left,  {0, 32767}},
        {EdgeDirection::Right, {65535, 32767}},
        {EdgeDirection::Up,    {32767, 0}},
        {EdgeDirection::Down,  {32767, 65535}},
    };

    for (const auto& probe : probes) {
        const auto transition = ResolveTopologyPointerTransitionForMachine(
            *options.topology,
            options.localMachineName,
            options.desktopWidth,
            options.desktopHeight,
            probe.second.first,
            probe.second.second);
        if (!transition.has_value()) {
            continue;
        }
        const auto targetMachine = options.topology->machineIdForDisplay(transition->targetDisplayId);
        const auto targetPoint = MapTransitionToTargetNormalizedPoint(*options.topology, *transition);
        if (!targetMachine.has_value() || !targetPoint.has_value() || *targetMachine == options.localMachineName) {
            continue;
        }
        targets.push_back(CaptureTarget{
            BarrierIdForEdge(transition->exitEdge),
            transition->exitEdge,
            transition->entryEdge,
            *targetMachine,
            targetPoint->x,
            targetPoint->y,
        });
    }

    return targets;
}

std::vector<CaptureTarget> BuildFallbackAndroidCaptureTarget(const LibeiInputCaptureBridgeOptions& options) {
    if (options.sendMouse || options.sendMouseToMachine) {
        return {CaptureTarget{
            kRightBarrierId,
            EdgeDirection::Right,
            EdgeDirection::Left,
            options.androidPeerName.empty() ? "android" : options.androidPeerName,
            0,
            32767,
        }};
    }
    return {};
}

std::optional<int> ConnectToEis(GDBusConnection* connection, const std::string& sessionHandle) {
    GVariantBuilder options;
    g_variant_builder_init(&options, G_VARIANT_TYPE("a{sv}"));

    GUnixFDList* outFds = nullptr;
    GError* error = nullptr;
    GVariant* reply = g_dbus_connection_call_with_unix_fd_list_sync(
        connection,
        kPortalBusName,
        kInputCapturePath,
        kInputCaptureInterface,
        "ConnectToEIS",
        g_variant_new("(oa{sv})", sessionHandle.c_str(), &options),
        G_VARIANT_TYPE("(h)"),
        G_DBUS_CALL_FLAGS_NONE,
        -1,
        nullptr,
        &outFds,
        nullptr,
        &error);
    if (!reply) {
        FreeError(error, "ConnectToEIS");
        return std::nullopt;
    }

    int fdIndex = -1;
    g_variant_get(reply, "(h)", &fdIndex);
    g_variant_unref(reply);
    if (!outFds || fdIndex < 0) {
        if (outFds) {
            g_object_unref(outFds);
        }
        return std::nullopt;
    }
    int fd = g_unix_fd_list_get(outFds, fdIndex, &error);
    g_object_unref(outFds);
    if (fd < 0) {
        FreeError(error, "ConnectToEIS fd");
        return std::nullopt;
    }
    return fd;
}

bool EnableCapture(GDBusConnection* connection, const std::string& sessionHandle) {
    GVariantBuilder options;
    g_variant_builder_init(&options, G_VARIANT_TYPE("a{sv}"));
    GError* error = nullptr;
    GVariant* reply = g_dbus_connection_call_sync(
        connection,
        kPortalBusName,
        kInputCapturePath,
        kInputCaptureInterface,
        "Enable",
        g_variant_new("(oa{sv})", sessionHandle.c_str(), &options),
        nullptr,
        G_DBUS_CALL_FLAGS_NONE,
        -1,
        nullptr,
        &error);
    if (!reply) {
        FreeError(error, "Enable");
        return false;
    }
    g_variant_unref(reply);
    return true;
}

void ReleaseCapture(GDBusConnection* connection, const std::string& sessionHandle) {
    GVariantBuilder options;
    g_variant_builder_init(&options, G_VARIANT_TYPE("a{sv}"));
    GError* error = nullptr;
    GVariant* reply = g_dbus_connection_call_sync(
        connection,
        kPortalBusName,
        kInputCapturePath,
        kInputCaptureInterface,
        "Release",
        g_variant_new("(oa{sv})", sessionHandle.c_str(), &options),
        nullptr,
        G_DBUS_CALL_FLAGS_NONE,
        1000,
        nullptr,
        &error);
    if (reply) {
        g_variant_unref(reply);
    } else {
        FreeError(error, "Release");
    }
}

void CloseSession(GDBusConnection* connection, const std::string& sessionHandle) {
    if (sessionHandle.empty()) {
        return;
    }
    GError* error = nullptr;
    GVariant* reply = g_dbus_connection_call_sync(
        connection,
        kPortalBusName,
        sessionHandle.c_str(),
        "org.freedesktop.portal.Session",
        "Close",
        nullptr,
        nullptr,
        G_DBUS_CALL_FLAGS_NONE,
        1000,
        nullptr,
        &error);
    if (reply) {
        g_variant_unref(reply);
    } else {
        FreeError(error, "Session.Close");
    }
}

#endif

} // namespace

LibeiInputCaptureBridge::LibeiInputCaptureBridge(LibeiInputCaptureBridgeOptions options)
    : m_options(std::move(options)) {}

LibeiInputCaptureBridge::~LibeiInputCaptureBridge() {
    Stop();
}

bool LibeiInputCaptureBridge::Start() {
    if (m_running.exchange(true)) {
        return true;
    }
    m_thread = std::thread([this]() {
        Run();
    });
    m_gestureThread = std::thread([this]() {
        RunLibinputGestureMonitor();
    });
    return true;
}

void LibeiInputCaptureBridge::Stop() {
    m_running = false;
    if (m_thread.joinable()) {
        m_thread.join();
    }
    if (m_gestureThread.joinable()) {
        m_gestureThread.join();
    }
}

void LibeiInputCaptureBridge::Run() {
#if defined(MWB_HAVE_LIBEI_INPUT_CAPTURE)
    GError* error = nullptr;
    GDBusConnection* connection = g_bus_get_sync(G_BUS_TYPE_SESSION, nullptr, &error);
    if (!connection) {
        FreeError(error, "session bus");
        m_running = false;
        return;
    }

    // Portal v2 returns a single-use restore token, allowing later service
    // starts to restore the user's permission without showing another dialog.
    // Older portals retain the one-prompt-per-process legacy behavior.
    const bool persistentPortal =
        InputCapturePortalVersion(connection) >= kPersistentInputCapturePortalVersion;
    std::optional<std::string> sessionHandle;
    std::vector<CaptureTarget> captureTargets;
    ei* context = nullptr;
    std::optional<int> eisFd;
    bool captureReady = false;

    for (int attempt = 1;
         attempt <= kLibeiInputCaptureAutomaticSetupAttempts && m_running;
         ++attempt) {
        sessionHandle = persistentPortal ? CreatePersistentSession(connection)
                                         : CreateLegacySession(connection);
        if (sessionHandle.has_value() &&
            (!persistentPortal || StartPersistentSession(connection, *sessionHandle))) {
            captureTargets = BuildTopologyCaptureTargets(m_options);
            if (captureTargets.empty()) {
                captureTargets = BuildFallbackAndroidCaptureTarget(m_options);
            }

            const auto zone = GetZones(connection, *sessionHandle);
            if (zone.has_value() &&
                SetPointerBarriers(connection, *sessionHandle, *zone, captureTargets)) {
                eisFd = ConnectToEis(connection, *sessionHandle);
                if (eisFd.has_value()) {
                    context = ei_new_receiver(this);
                    if (context && ei_setup_backend_fd(context, *eisFd) >= 0 &&
                        EnableCapture(connection, *sessionHandle)) {
                        captureReady = true;
                        break;
                    }
                    if (context) {
                        ei_unref(context);
                        context = nullptr;
                    }
                    close(*eisFd);
                    eisFd.reset();
                }
            }
            CloseSession(connection, *sessionHandle);
            sessionHandle.reset();
        }
        if (sessionHandle.has_value()) {
            CloseSession(connection, *sessionHandle);
            sessionHandle.reset();
        }

    }

    if (!captureReady) {
        if (context) {
            ei_unref(context);
        }
        if (eisFd.has_value()) {
            close(*eisFd);
        }
        g_object_unref(connection);
        std::cerr << "ERROR: libei input capture handshake failed; automatic retry "
                     "suppressed to avoid repeated permission prompts. Restart InputFlow "
                     "to try again."
                  << std::endl;
        m_running = false;
        return;
    }

    std::cout << "[TOPOLOGY] libei input capture enabled with " << captureTargets.size()
              << " topology barrier(s)." << std::endl;
    int remoteX = captureTargets.front().startX;
    int remoteY = captureTargets.front().startY;
    bool remoteControlActive = false;
    std::optional<CaptureTarget> activeTarget;
    const std::string androidMachineId = m_options.androidPeerName.empty() ? "android" : m_options.androidPeerName;
    const auto setRemoteControlActive = [&](bool active) {
        if (remoteControlActive == active) {
            return;
        }
        remoteControlActive = active;
        if (activeTarget.has_value() && m_options.sendControlForMachine) {
            m_options.sendControlForMachine(active, activeTarget->machineId);
        } else if (m_options.sendControl) {
            m_options.sendControl(active);
        }
    };
    const auto sendMouse = [&](const MouseData& mouse) {
        if (activeTarget.has_value() && m_options.sendMouseToMachine) {
            return m_options.sendMouseToMachine(mouse, activeTarget->machineId);
        }
        return m_options.sendMouse ? m_options.sendMouse(mouse) : false;
    };
    const auto sendKeyboard = [&](const KeyboardData& keyboard) {
        if (activeTarget.has_value() && m_options.sendKeyboardToMachine) {
            return m_options.sendKeyboardToMachine(keyboard, activeTarget->machineId);
        }
        return m_options.sendKeyboard ? m_options.sendKeyboard(keyboard) : false;
    };
    const auto selectTarget = [&](double dx, double dy) -> bool {
        if (activeTarget.has_value()) {
            return true;
        }
        const auto edge = DominantEdgeFromDelta(dx, dy);
        if (!edge.has_value()) {
            return false;
        }
        const auto it = std::find_if(captureTargets.begin(), captureTargets.end(),
            [&](const CaptureTarget& target) { return target.exitEdge == *edge; });
        if (it == captureTargets.end()) {
            return false;
        }
        activeTarget = *it;
        remoteX = NudgeInward(activeTarget->startX);
        remoteY = NudgeInward(activeTarget->startY);
        std::cout << "[TOPOLOGY] Captured local pointer through "
                  << edgeDirectionName(activeTarget->exitEdge)
                  << " edge for target " << activeTarget->machineId << "." << std::endl;
        return true;
    };
    while (m_running) {
        pollfd fds[1] = {
            pollfd{ei_get_fd(context), POLLIN, 0},
        };
        const int rc = poll(fds, 1, 100);
        while (g_main_context_pending(nullptr)) {
            g_main_context_iteration(nullptr, FALSE);
        }
        if (rc < 0) {
            continue;
        }
        if (rc == 0) {
            continue;
        }
        ei_dispatch(context);

        ei_event* event = nullptr;
        while ((event = ei_get_event(context)) != nullptr) {
            const auto type = ei_event_get_type(event);
            switch (type) {
            case EI_EVENT_CONNECT:
                break;
            case EI_EVENT_DISCONNECT:
                std::cerr << "WARN: libei input capture disconnected." << std::endl;
                setRemoteControlActive(false);
                m_running = false;
                break;
            case EI_EVENT_SEAT_ADDED: {
                ei_seat* seat = ei_event_get_seat(event);
                ei_seat_bind_capabilities(seat,
                                          EI_DEVICE_CAP_POINTER,
                                          EI_DEVICE_CAP_BUTTON,
                                          EI_DEVICE_CAP_SCROLL,
                                          EI_DEVICE_CAP_KEYBOARD,
                                          nullptr);
                break;
            }
            case EI_EVENT_DEVICE_ADDED: {
                ei_device* device = ei_event_get_device(event);
                if (device &&
                    !ei_device_has_capability(device, EI_DEVICE_CAP_POINTER) &&
                    !ei_device_has_capability(device, EI_DEVICE_CAP_BUTTON) &&
                    !ei_device_has_capability(device, EI_DEVICE_CAP_SCROLL) &&
                    !ei_device_has_capability(device, EI_DEVICE_CAP_KEYBOARD)) {
                    ei_device_close(device);
                }
                break;
            }
            case EI_EVENT_POINTER_MOTION: {
                const double dx = ei_event_pointer_get_dx(event);
                const double dy = ei_event_pointer_get_dy(event);
                if (!selectTarget(dx, dy)) {
                    break;
                }
                remoteX = ClampNormalized(remoteX + static_cast<int>(std::lround(dx * 40.0)));
                remoteY = ClampNormalized(remoteY + static_cast<int>(std::lround(dy * 40.0)));
                if (MovementReturnsToLocal(*activeTarget, remoteX, remoteY, dx, dy)) {
                    setRemoteControlActive(false);
                    ReleaseCapture(connection, *sessionHandle);
                    std::cout << "[TOPOLOGY] Released input back to Fedora from "
                              << activeTarget->machineId << "." << std::endl;
                    activeTarget.reset();
                    break;
                }
                setRemoteControlActive(true);
                MouseData mouse{remoteX, remoteY, 0, WM_MOUSEMOVE};
                sendMouse(mouse);
                break;
            }
            case EI_EVENT_BUTTON_BUTTON: {
                const uint32_t button = ei_event_button_get_button(event);
                const bool press = ei_event_button_get_is_press(event);
                uint32_t message = 0;
                if (button == BTN_LEFT) {
                    message = press ? WM_LBUTTONDOWN : WM_LBUTTONUP;
                } else if (button == BTN_RIGHT) {
                    message = press ? WM_RBUTTONDOWN : WM_RBUTTONUP;
                } else if (button == BTN_MIDDLE) {
                    message = press ? WM_MBUTTONDOWN : WM_MBUTTONUP;
                }
                if (message != 0 && activeTarget.has_value()) {
                    MouseData mouse{remoteX, remoteY, 0, message};
                    sendMouse(mouse);
                }
                break;
            }
            case EI_EVENT_SCROLL_DELTA: {
                const double dx = ei_event_scroll_get_dx(event);
                const double dy = ei_event_scroll_get_dy(event);
                if ((std::abs(dx) > 0.01 || std::abs(dy) > 0.01) &&
                    activeTarget.has_value() &&
                    activeTarget->machineId == androidMachineId &&
                    m_options.sendGesture) {
                    m_options.sendGesture("scroll", dx, dy);
                } else if (std::abs(dy) > 0.01 && activeTarget.has_value()) {
                    MouseData mouse{remoteX, remoteY, static_cast<int32_t>(std::lround(-dy * 120.0)), WM_MOUSEWHEEL};
                    sendMouse(mouse);
                }
                break;
            }
            case EI_EVENT_SCROLL_STOP: {
                break;
            }
            case EI_EVENT_KEYBOARD_KEY: {
                const uint32_t key = ei_event_keyboard_get_key(event);
                const bool press = ei_event_keyboard_get_key_is_press(event);
                const auto vk = LinuxKeyToVirtualKey(key);
                if (vk.has_value() && activeTarget.has_value()) {
                    KeyboardData keyboard{*vk, press ? 0u : LLKHF_UP};
                    sendKeyboard(keyboard);
                }
                break;
            }
            default:
                break;
            }
            ei_event_unref(event);
        }
    }

    setRemoteControlActive(false);
    ei_unref(context);
    CloseSession(connection, *sessionHandle);
    g_object_unref(connection);
#else
    std::cerr << "WARN: android_capture_backend=libei requires libei-devel and glib2-devel at build time; local Android capture is disabled." << std::endl;
#endif
    m_running = false;
}

void LibeiInputCaptureBridge::RunLibinputGestureMonitor() {
    if (!m_options.sendGesture) {
        return;
    }
    const auto device = FindTouchpadEventDevice();
    if (!device.has_value()) {
        std::cout << "[ANDROID] libinput gesture monitor skipped; no touchpad event device found." << std::endl;
        return;
    }

#if defined(MWB_HAVE_LIBINPUT_GESTURES)
    if (RunNativeLibinputGestureMonitor(*device, m_running, m_options.sendGesture)) {
        return;
    }
    std::cerr << "WARN: Falling back to libinput debug-events gesture parser." << std::endl;
#endif

    std::cout << "[ANDROID] Libinput gesture monitor started." << std::endl;
    while (m_running) {
        constexpr const char* timeoutPath = "/usr/bin/timeout";
        constexpr const char* libinputPath = "/usr/bin/libinput";
        if (access(timeoutPath, X_OK) != 0 || access(libinputPath, X_OK) != 0) {
            std::cerr << "WARN: The libinput gesture helper is unavailable." << std::endl;
            return;
        }
        int outputPipe[2]{-1, -1};
        if (pipe(outputPipe) != 0) {
            std::cerr << "WARN: Failed to start libinput gesture monitor." << std::endl;
            return;
        }
        const pid_t child = fork();
        if (child < 0) {
            close(outputPipe[0]);
            close(outputPipe[1]);
            std::cerr << "WARN: Failed to start libinput gesture monitor." << std::endl;
            return;
        }
        if (child == 0) {
            close(outputPipe[0]);
            dup2(outputPipe[1], STDOUT_FILENO);
            if (outputPipe[1] > STDERR_FILENO) {
                close(outputPipe[1]);
            }
            const int devNull = open("/dev/null", O_WRONLY);
            if (devNull >= 0) {
                dup2(devNull, STDERR_FILENO);
                if (devNull > STDERR_FILENO) {
                    close(devNull);
                }
            }
            const long reportedOpenMax = sysconf(_SC_OPEN_MAX);
            const int openMax = reportedOpenMax > 3
                ? static_cast<int>(std::min<long>(reportedOpenMax, 4096))
                : 1024;
            for (int fd = 3; fd < openMax; ++fd) {
                close(fd);
            }
            execl(
                timeoutPath,
                timeoutPath,
                "1s",
                libinputPath,
                "debug-events",
                "--device",
                device->c_str(),
                static_cast<char*>(nullptr));
            _exit(127);
        }
        close(outputPipe[1]);
        FILE* output = fdopen(outputPipe[0], "r");
        if (output == nullptr) {
            close(outputPipe[0]);
            int status = 0;
            (void)waitpid(child, &status, 0);
            std::cerr << "WARN: Failed to read libinput gesture monitor." << std::endl;
            return;
        }

        char buffer[512];
        bool swipeActive = false;
        bool pinchActive = false;
        int fingers = 0;
        double swipeDx = 0.0;
        double swipeDy = 0.0;
        double pinchScale = 1.0;

        while (m_running && fgets(buffer, sizeof(buffer), output) != nullptr) {
            const std::string line(buffer);
            std::istringstream in(line);
            std::string deviceToken;
            std::string type;
            std::string timeToken;
            in >> deviceToken >> type >> timeToken;

            if (type == "GESTURE_SWIPE_BEGIN") {
                in >> fingers;
                swipeActive = true;
                swipeDx = 0.0;
                swipeDy = 0.0;
            } else if (type == "GESTURE_SWIPE_UPDATE" && swipeActive) {
                int updateFingers = 0;
                std::string delta;
                in >> updateFingers >> delta;
                if (auto parsed = ParseDeltaToken(delta); parsed.has_value()) {
                    fingers = updateFingers;
                    swipeDx += parsed->first;
                    swipeDy += parsed->second;
                }
            } else if (type == "GESTURE_SWIPE_END" && swipeActive) {
                EmitClassifiedSwipe(m_options.sendGesture, fingers, swipeDx, swipeDy, false);
                swipeActive = false;
            } else if (type == "GESTURE_PINCH_BEGIN") {
                in >> fingers;
                pinchActive = true;
                pinchScale = 1.0;
            } else if (type == "GESTURE_PINCH_UPDATE" && pinchActive) {
                std::string token;
                double lastReasonableScale = 1.0;
                while (in >> token) {
                    try {
                        const double value = std::stod(token);
                        if (value > 0.25 && value < 4.0) {
                            lastReasonableScale = value;
                        }
                    } catch (...) {
                    }
                }
                pinchScale *= lastReasonableScale;
            } else if (type == "GESTURE_PINCH_END" && pinchActive) {
                if (pinchScale < 0.92 || pinchScale > 1.08) {
                    m_options.sendGesture("pinch", pinchScale, 0.0);
                }
                pinchActive = false;
            }
        }

        fclose(output);
        int status = 0;
        while (waitpid(child, &status, 0) < 0 && errno == EINTR) {
        }
    }
}

} // namespace mwb
