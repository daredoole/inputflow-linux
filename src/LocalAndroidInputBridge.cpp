#include "LocalAndroidInputBridge.h"

#include <X11/Xlib.h>
#include <fcntl.h>
#include <linux/input.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstring>
#include <fstream>
#include <iostream>
#include <limits>
#include <optional>
#include <sstream>
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

int ClampNormalized(int value) {
    return std::max(0, std::min(65535, value));
}

std::string Lower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return value;
}

struct DeviceState {
    int fd{-1};
    std::string path;
    std::string name;
    int relDx{0};
    int relDy{0};
    int wheel{0};
    bool hasAbsX{false};
    bool hasAbsY{false};
    bool hasTouchState{false};
    bool touchActive{true};
    int absXMin{0};
    int absXMax{0};
    int absYMin{0};
    int absYMax{0};
    std::optional<int> pendingAbsX;
    std::optional<int> pendingAbsY;
    std::optional<int> lastAbsX;
    std::optional<int> lastAbsY;
};

struct PointerDeviceCandidate {
    std::string path;
    std::string name;
};

std::vector<PointerDeviceCandidate> DiscoverPointerEventDevices() {
    std::ifstream input("/proc/bus/input/devices");
    std::vector<PointerDeviceCandidate> devices;
    std::string line;
    std::string name;
    std::string handlers;

    auto flush = [&]() {
        const std::string lowerName = Lower(name);
        const bool pointer =
            lowerName.find("mouse") != std::string::npos ||
            lowerName.find("touchpad") != std::string::npos ||
            lowerName.find("trackpad") != std::string::npos ||
            lowerName.find("pointer") != std::string::npos;
        const bool virtualInputFlow = lowerName.find("inputflow virtual") != std::string::npos;
        if (pointer && !virtualInputFlow) {
            std::istringstream stream(handlers);
            std::string token;
            while (stream >> token) {
                if (token.rfind("event", 0) == 0) {
                    devices.push_back(PointerDeviceCandidate{"/dev/input/" + token, name});
                }
            }
        }
        name.clear();
        handlers.clear();
    };

    while (std::getline(input, line)) {
        if (line.empty()) {
            flush();
            continue;
        }
        if (line.rfind("N: Name=", 0) == 0) {
            name = line.substr(8);
        } else if (line.rfind("H: Handlers=", 0) == 0) {
            handlers = line.substr(12);
        }
    }
    flush();
    return devices;
}

void LoadAbsInfo(DeviceState& device) {
    input_absinfo info{};
    if (ioctl(device.fd, EVIOCGABS(ABS_X), &info) == 0 && info.maximum > info.minimum) {
        device.hasAbsX = true;
        device.absXMin = info.minimum;
        device.absXMax = info.maximum;
    }
    if (ioctl(device.fd, EVIOCGABS(ABS_Y), &info) == 0 && info.maximum > info.minimum) {
        device.hasAbsY = true;
        device.absYMin = info.minimum;
        device.absYMax = info.maximum;
    }
}

std::vector<DeviceState> OpenPointerDevices() {
    std::vector<DeviceState> devices;
    for (const auto& candidate : DiscoverPointerEventDevices()) {
        const int fd = open(candidate.path.c_str(), O_RDONLY | O_NONBLOCK | O_CLOEXEC);
        if (fd < 0) {
            continue;
        }
        DeviceState device;
        device.fd = fd;
        device.path = candidate.path;
        device.name = candidate.name;
        LoadAbsInfo(device);
        devices.push_back(std::move(device));
    }
    return devices;
}

void ClosePointerDevices(std::vector<DeviceState>& devices) {
    for (auto& device : devices) {
        if (device.fd >= 0) {
            close(device.fd);
            device.fd = -1;
        }
    }
}

bool MovementMatchesEdge(EdgeDirection edge, int dx, int dy) {
    switch (edge) {
    case EdgeDirection::Left:
        return dx < 0;
    case EdgeDirection::Right:
        return dx > 0;
    case EdgeDirection::Up:
        return dy < 0;
    case EdgeDirection::Down:
        return dy > 0;
    }
    return false;
}

std::optional<EdgeDirection> DominantMovementEdge(int dx, int dy) {
    if (std::abs(dx) >= std::abs(dy) && dx != 0) {
        return dx < 0 ? EdgeDirection::Left : EdgeDirection::Right;
    }
    if (dy != 0) {
        return dy < 0 ? EdgeDirection::Up : EdgeDirection::Down;
    }
    return std::nullopt;
}

bool MovementReturnsFromEdge(EdgeDirection entryEdge, int x, int y, int dx, int dy) {
    switch (entryEdge) {
    case EdgeDirection::Left:
        return x <= 0 && dx < 0;
    case EdgeDirection::Right:
        return x >= 65535 && dx > 0;
    case EdgeDirection::Up:
        return y <= 0 && dy < 0;
    case EdgeDirection::Down:
        return y >= 65535 && dy > 0;
    }
    return false;
}

} // namespace

LocalAndroidInputBridge::LocalAndroidInputBridge(LocalAndroidInputBridgeOptions options)
    : m_options(std::move(options)) {}

LocalAndroidInputBridge::~LocalAndroidInputBridge() {
    Stop();
}

bool LocalAndroidInputBridge::Start() {
    if (m_running.exchange(true)) {
        return true;
    }
    m_thread = std::thread([this]() {
        Run();
    });
    return true;
}

void LocalAndroidInputBridge::Stop() {
    if (!m_running.exchange(false)) {
        return;
    }
    if (m_thread.joinable()) {
        m_thread.join();
    }
}

void LocalAndroidInputBridge::Run() {
    ::Display* display = XOpenDisplay(nullptr);
    if (display == nullptr) {
        std::cerr << "WARN: Local Android input bridge could not open X display; Fedora-to-Android edge capture disabled." << std::endl;
        m_running = false;
        return;
    }

    auto devices = OpenPointerDevices();
    if (devices.empty()) {
        std::cerr << "WARN: Local Android input bridge found no readable pointer devices; Fedora-to-Android edge capture disabled." << std::endl;
        XCloseDisplay(display);
        m_running = false;
        return;
    }

    std::vector<pollfd> polls;
    polls.reserve(devices.size());
    for (const auto& device : devices) {
        polls.push_back(pollfd{device.fd, POLLIN, 0});
    }

    std::cout << "[ANDROID] Local Fedora pointer bridge watching " << devices.size()
              << " device(s); push through configured Android edge to control Android." << std::endl;

    bool active = false;
    EdgeDirection activeEntryEdge = EdgeDirection::Left;
    int androidX = 0;
    int androidY = 0;

    int localX = m_options.desktopWidth / 2;
    int localY = m_options.desktopHeight / 2;
    {
        Window rootReturn{};
        Window childReturn{};
        int rootX = 0;
        int rootY = 0;
        int windowX = 0;
        int windowY = 0;
        unsigned int mask = 0;
        if (XQueryPointer(display,
                          DefaultRootWindow(display),
                          &rootReturn,
                          &childReturn,
                          &rootX,
                          &rootY,
                          &windowX,
                          &windowY,
                          &mask)) {
            localX = std::max(0, std::min(m_options.desktopWidth - 1, rootX));
            localY = std::max(0, std::min(m_options.desktopHeight - 1, rootY));
        }
    }

    auto queryTransition = [&](EdgeDirection pushedEdge) -> std::optional<TopologyPointerTransition> {
        constexpr int edgeSlopPixels = 64;
        for (const auto& displayInfo : m_options.topology->displays()) {
            if (displayInfo.machineId != m_options.localMachineName) {
                continue;
            }

            bool atEdge = false;
            int coordinate = 0;
            switch (pushedEdge) {
            case EdgeDirection::Left:
                atEdge = localX <= displayInfo.x + edgeSlopPixels &&
                         localY >= displayInfo.y &&
                         localY < displayInfo.y + displayInfo.height;
                coordinate = localY - displayInfo.y;
                break;
            case EdgeDirection::Right:
                atEdge = localX >= displayInfo.x + displayInfo.width - 1 - edgeSlopPixels &&
                         localY >= displayInfo.y &&
                         localY < displayInfo.y + displayInfo.height;
                coordinate = localY - displayInfo.y;
                break;
            case EdgeDirection::Up:
                atEdge = localY <= displayInfo.y + edgeSlopPixels &&
                         localX >= displayInfo.x &&
                         localX < displayInfo.x + displayInfo.width;
                coordinate = localX - displayInfo.x;
                break;
            case EdgeDirection::Down:
                atEdge = localY >= displayInfo.y + displayInfo.height - 1 - edgeSlopPixels &&
                         localX >= displayInfo.x &&
                         localX < displayInfo.x + displayInfo.width;
                coordinate = localX - displayInfo.x;
                break;
            }

            if (!atEdge) {
                continue;
            }

            const auto transition = m_options.topology->transitionFromEdge(displayInfo.id, pushedEdge, coordinate);
            if (!transition.has_value()) {
                continue;
            }
            return TopologyPointerTransition{
                displayInfo.id,
                pushedEdge,
                transition->targetDisplayId,
                transition->entryEdge,
                transition->coordinate,
            };
        }
        return std::nullopt;
    };

    auto queryConfiguredTransition = [&](EdgeDirection pushedEdge) -> std::optional<TopologyPointerTransition> {
        for (const auto& displayInfo : m_options.topology->displays()) {
            if (displayInfo.machineId != m_options.localMachineName) {
                continue;
            }

            int coordinate = 0;
            switch (pushedEdge) {
            case EdgeDirection::Left:
            case EdgeDirection::Right:
                coordinate = std::max(0, std::min(displayInfo.height - 1, localY - displayInfo.y));
                break;
            case EdgeDirection::Up:
            case EdgeDirection::Down:
                coordinate = std::max(0, std::min(displayInfo.width - 1, localX - displayInfo.x));
                break;
            }

            const auto transition = m_options.topology->transitionFromEdge(displayInfo.id, pushedEdge, coordinate);
            if (!transition.has_value()) {
                continue;
            }
            return TopologyPointerTransition{
                displayInfo.id,
                pushedEdge,
                transition->targetDisplayId,
                transition->entryEdge,
                transition->coordinate,
            };
        }
        return std::nullopt;
    };

    auto sendMove = [&]() {
        MouseData mouse{androidX, androidY, 0, WM_MOUSEMOVE};
        return m_options.sendMouse && m_options.sendMouse(mouse);
    };

    std::optional<EdgeDirection> pressureEdge;
    int edgePressure = 0;

    auto processMotion = [&](std::size_t deviceIndex, int normalizedDx, int normalizedDy) {
        if (normalizedDx == 0 && normalizedDy == 0) {
            return;
        }

        if (!active) {
            localX = std::max(0, std::min(m_options.desktopWidth - 1,
                                          localX + static_cast<int>((static_cast<int64_t>(normalizedDx) * m_options.desktopWidth) / 65535)));
            localY = std::max(0, std::min(m_options.desktopHeight - 1,
                                          localY + static_cast<int>((static_cast<int64_t>(normalizedDy) * m_options.desktopHeight) / 65535)));
            const auto pushedEdge = DominantMovementEdge(normalizedDx, normalizedDy);
            if (!pushedEdge.has_value()) {
                return;
            }

            const int pressureDelta = std::max(std::abs(normalizedDx), std::abs(normalizedDy));
            if (pressureEdge.has_value() && *pressureEdge == *pushedEdge) {
                edgePressure = std::min(30000, edgePressure + pressureDelta);
            } else {
                pressureEdge = pushedEdge;
                edgePressure = pressureDelta;
            }

            auto transition = queryTransition(*pushedEdge);
            if (!transition.has_value() && edgePressure >= 4500) {
                transition = queryConfiguredTransition(*pushedEdge);
            }
            if (!transition.has_value() || !MovementMatchesEdge(transition->exitEdge, normalizedDx, normalizedDy)) {
                return;
            }
            const auto targetPoint = MapTransitionToTargetNormalizedPoint(*m_options.topology, *transition);
            const auto targetMachineId = m_options.topology->machineIdForDisplay(transition->targetDisplayId);
            if (!targetPoint.has_value() ||
                !targetMachineId.has_value() ||
                *targetMachineId != m_options.androidPeerName) {
                return;
            }
            active = true;
            edgePressure = 0;
            activeEntryEdge = transition->entryEdge;
            androidX = targetPoint->x;
            androidY = targetPoint->y;
            std::cout << "[ANDROID] Local pointer entered Android from "
                      << edgeDirectionName(transition->exitEdge)
                      << " edge using " << devices[deviceIndex].name
                      << " (" << devices[deviceIndex].path << ")." << std::endl;
        }

        androidX = ClampNormalized(androidX + normalizedDx);
        androidY = ClampNormalized(androidY + normalizedDy);
        if (!sendMove()) {
            active = false;
            return;
        }
        if (MovementReturnsFromEdge(activeEntryEdge, androidX, androidY, normalizedDx, normalizedDy)) {
            active = false;
            std::cout << "[ANDROID] Local pointer returned to Fedora." << std::endl;
        }
    };

    auto sendButton = [&](std::size_t deviceIndex, uint32_t down, uint32_t up, int value) {
        (void)deviceIndex;
        if (!active || value == 2) {
            return;
        }
        MouseData mouse{androidX, androidY, 0, value ? down : up};
        if (m_options.sendMouse) {
            m_options.sendMouse(mouse);
        }
    };

    while (m_running) {
        const int rc = poll(polls.data(), polls.size(), 100);
        if (rc < 0) {
            if (errno == EINTR) {
                continue;
            }
            break;
        }
        if (rc == 0) {
            continue;
        }

        for (std::size_t index = 0; index < devices.size(); ++index) {
            if ((polls[index].revents & POLLIN) == 0) {
                continue;
            }

            input_event event{};
            while (read(devices[index].fd, &event, sizeof(event)) == sizeof(event)) {
                auto& device = devices[index];
                if (event.type == EV_REL) {
                    if (event.code == REL_X) {
                        device.relDx += event.value;
                    } else if (event.code == REL_Y) {
                        device.relDy += event.value;
                    } else if (event.code == REL_WHEEL) {
                        device.wheel += event.value;
                    }
                } else if (event.type == EV_ABS) {
                    if (event.code == ABS_X && device.hasAbsX) {
                        device.pendingAbsX = event.value;
                    } else if (event.code == ABS_Y && device.hasAbsY) {
                        device.pendingAbsY = event.value;
                    }
                } else if (event.type == EV_KEY) {
                    if (event.code == BTN_TOUCH || event.code == BTN_TOOL_FINGER) {
                        device.hasTouchState = true;
                        device.touchActive = event.value != 0;
                        if (!device.touchActive) {
                            device.lastAbsX.reset();
                            device.lastAbsY.reset();
                            device.pendingAbsX.reset();
                            device.pendingAbsY.reset();
                        }
                    } else if (event.code == BTN_LEFT) {
                        sendButton(index, WM_LBUTTONDOWN, WM_LBUTTONUP, event.value);
                    } else if (event.code == BTN_RIGHT) {
                        sendButton(index, WM_RBUTTONDOWN, WM_RBUTTONUP, event.value);
                    } else if (event.code == BTN_MIDDLE) {
                        sendButton(index, WM_MBUTTONDOWN, WM_MBUTTONUP, event.value);
                    }
                } else if (event.type == EV_SYN && event.code == SYN_REPORT) {
                    int normalizedDx = 0;
                    int normalizedDy = 0;
                    if (device.relDx != 0) {
                        normalizedDx += static_cast<int>((static_cast<int64_t>(device.relDx) * 65535) / std::max(1, m_options.desktopWidth));
                    }
                    if (device.relDy != 0) {
                        normalizedDy += static_cast<int>((static_cast<int64_t>(device.relDy) * 65535) / std::max(1, m_options.desktopHeight));
                    }
                    if ((!device.hasTouchState || device.touchActive) && device.pendingAbsX.has_value()) {
                        if (device.lastAbsX.has_value() && device.absXMax > device.absXMin) {
                            normalizedDx += static_cast<int>((static_cast<int64_t>(*device.pendingAbsX - *device.lastAbsX) * 65535) /
                                                             (device.absXMax - device.absXMin));
                        }
                        device.lastAbsX = *device.pendingAbsX;
                    }
                    if ((!device.hasTouchState || device.touchActive) && device.pendingAbsY.has_value()) {
                        if (device.lastAbsY.has_value() && device.absYMax > device.absYMin) {
                            normalizedDy += static_cast<int>((static_cast<int64_t>(*device.pendingAbsY - *device.lastAbsY) * 65535) /
                                                             (device.absYMax - device.absYMin));
                        }
                        device.lastAbsY = *device.pendingAbsY;
                    }
                    processMotion(index, normalizedDx, normalizedDy);
                    if (active && device.wheel != 0 && m_options.sendMouse) {
                        MouseData wheel{androidX, androidY, device.wheel * 120, WM_MOUSEWHEEL};
                        m_options.sendMouse(wheel);
                    }
                    device.relDx = 0;
                    device.relDy = 0;
                    device.wheel = 0;
                }
            }
        }
    }

    ClosePointerDevices(devices);
    XCloseDisplay(display);
}

} // namespace mwb
