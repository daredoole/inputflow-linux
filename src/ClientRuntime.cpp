#include "ClientRuntime.h"
#include "MediaKeyBridge.h"
#include "ScreenGeometry.h"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sys/wait.h>
#include <unistd.h>

namespace mwb {
namespace {

constexpr int kFallbackScreenWidth = 1920;
constexpr int kFallbackScreenHeight = 1080;

std::optional<int> ParsePositiveInt(const std::string& value) {
    try {
        size_t end = 0;
        const int parsed = std::stoi(value, &end);
        if (end != value.size() || parsed <= 0) {
            return std::nullopt;
        }
        return parsed;
    } catch (...) {
        return std::nullopt;
    }
}

std::optional<std::string> ReadFirstLine(const std::filesystem::path& path) {
    std::ifstream file(path);
    std::string line;
    if (!file || !std::getline(file, line)) {
        return std::nullopt;
    }
    return line;
}

std::optional<std::pair<int, int>> ParseMode(const std::string& mode) {
    const std::size_t separator = mode.find('x');
    if (separator == std::string::npos) {
        return std::nullopt;
    }

    const auto width = ParsePositiveInt(mode.substr(0, separator));
    const auto height = ParsePositiveInt(mode.substr(separator + 1));
    if (!width || !height) {
        return std::nullopt;
    }

    return std::pair<int, int>{*width, *height};
}

std::string SelectTopologySourceDisplay(const TopologyModel& topology,
                                        const std::string& localMachineName,
                                        const ClientRuntime::ScreenSize& screenSize) {
    const Display* firstLocal = nullptr;
    const Display* firstAny = nullptr;

    for (const auto& display : topology.displays()) {
        if (firstAny == nullptr) {
            firstAny = &display;
        }
        if (!localMachineName.empty() && display.machineId == localMachineName) {
            if (display.width == screenSize.width && display.height == screenSize.height) {
                return display.id;
            }
            if (firstLocal == nullptr) {
                firstLocal = &display;
            }
        }
    }

    if (firstLocal != nullptr) {
        return firstLocal->id;
    }
    if (localMachineName.empty() && firstAny != nullptr) {
        return firstAny->id;
    }
    return {};
}

std::optional<ClientRuntime::ScreenSize> ReadScreenSizeFromDrm() {
    namespace fs = std::filesystem;

    const fs::path drmRoot("/sys/class/drm");
    if (!fs::exists(drmRoot)) {
        return std::nullopt;
    }

    int totalWidth = 0;
    int maxHeight = 0;
    int activeOutputs = 0;

    try {
        for (const auto& entry : fs::directory_iterator(drmRoot)) {
            if (!entry.is_directory()) {
                continue;
            }

            const auto status = ReadFirstLine(entry.path() / "status");
            if (!status || *status != "connected") {
                continue;
            }

            const auto enabled = ReadFirstLine(entry.path() / "enabled");
            if (enabled && *enabled == "disabled") {
                continue;
            }

            std::ifstream modes(entry.path() / "modes");
            std::string mode;
            while (std::getline(modes, mode)) {
                const auto parsedMode = ParseMode(mode);
                if (!parsedMode) {
                    continue;
                }

                totalWidth += parsedMode->first;
                maxHeight = std::max(maxHeight, parsedMode->second);
                ++activeOutputs;
                break;
            }
        }
    } catch (const fs::filesystem_error&) {
        return std::nullopt;
    }

    if (activeOutputs == 0 || totalWidth <= 0 || maxHeight <= 0) {
        return std::nullopt;
    }

    return ClientRuntime::ScreenSize{totalWidth, maxHeight, ClientRuntime::ScreenSize::Source::Drm};
}

std::optional<std::string> FindTrustedExecutable(const std::string& executable) {
    if (executable.empty()) {
        return std::nullopt;
    }
    if (executable.front() == '/' && access(executable.c_str(), X_OK) == 0) {
        return executable;
    }

    constexpr std::array<const char*, 3> kTrustedPrefixes = {
        "/usr/bin/",
        "/bin/",
        "/usr/local/bin/",
    };
    for (const char* prefix : kTrustedPrefixes) {
        const std::string candidate = std::string(prefix) + executable;
        if (access(candidate.c_str(), X_OK) == 0) {
            return candidate;
        }
    }
    return std::nullopt;
}

std::optional<std::string> RunCommandCapture(const std::vector<std::string>& argv) {
    if (argv.empty()) {
        return std::nullopt;
    }

    int stdoutPipe[2];
    if (pipe(stdoutPipe) != 0) {
        return std::nullopt;
    }

    const pid_t pid = fork();
    if (pid < 0) {
        close(stdoutPipe[0]);
        close(stdoutPipe[1]);
        return std::nullopt;
    }

    if (pid == 0) {
        dup2(stdoutPipe[1], STDOUT_FILENO);
        close(stdoutPipe[0]);
        close(stdoutPipe[1]);

        std::vector<char*> execArgv;
        execArgv.reserve(argv.size() + 1);
        for (const std::string& arg : argv) {
            execArgv.push_back(const_cast<char*>(arg.c_str()));
        }
        execArgv.push_back(nullptr);
        execv(execArgv[0], execArgv.data());
        _exit(127);
    }

    close(stdoutPipe[1]);
    std::string output;
    std::array<char, 4096> buffer{};
    while (true) {
        const ssize_t count = read(stdoutPipe[0], buffer.data(), buffer.size());
        if (count < 0) {
            if (errno == EINTR) {
                continue;
            }
            close(stdoutPipe[0]);
            int status = 0;
            while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {
            }
            return std::nullopt;
        }
        if (count == 0) {
            break;
        }
        output.append(buffer.data(), static_cast<std::size_t>(count));
    }
    close(stdoutPipe[0]);

    int status = 0;
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {
    }
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        return std::nullopt;
    }

    return output;
}

std::optional<ClientRuntime::ScreenSize> ReadScreenSizeFromKScreen() {
    if (std::getenv("WAYLAND_DISPLAY") == nullptr) {
        return std::nullopt;
    }

    const char* desktop = std::getenv("XDG_CURRENT_DESKTOP");
    if (desktop == nullptr || std::string(desktop).find("KDE") == std::string::npos) {
        return std::nullopt;
    }

    const auto kscreenDoctor = FindTrustedExecutable("kscreen-doctor");
    if (!kscreenDoctor.has_value()) {
        return std::nullopt;
    }

    const auto output = RunCommandCapture({*kscreenDoctor, "-o"});
    if (!output.has_value()) {
        return std::nullopt;
    }

    const auto size = ParseKScreenDoctorLogicalScreenSize(*output);
    if (!size.has_value()) {
        return std::nullopt;
    }

    return ClientRuntime::ScreenSize{size->first, size->second, ClientRuntime::ScreenSize::Source::WaylandLogical};
}

} // namespace

ClientRuntime::ClientRuntime(RuntimeOptions options)
    : m_options(std::move(options)),
      m_input(),
      m_latencyStats(m_options.latencyReport ? std::make_shared<InputLatencyStats>(true) : nullptr),
      m_dispatcher(m_input, m_latencyStats) {
    ConfigureMediaKeyBridge(m_options.mprisMediaKeysEnabled, m_options.mprisPlayer);
}

ClientRuntime::~ClientRuntime() {
    Stop();
}

ClientRuntime::ScreenSize ClientRuntime::DetectScreenSize() const {
    if (m_options.screenWidth && m_options.screenHeight) {
        return ScreenSize{*m_options.screenWidth, *m_options.screenHeight, ScreenSize::Source::Explicit};
    }

    if (auto logical = ReadScreenSizeFromKScreen()) {
        return *logical;
    }

    if (auto drm = ReadScreenSizeFromDrm()) {
        return *drm;
    }

    return ScreenSize{kFallbackScreenWidth, kFallbackScreenHeight, ScreenSize::Source::Fallback};
}

void ClientRuntime::ConfigureTopologyPreview(const ScreenSize& screenSize) {
    m_dispatcher.SetTopologyPreview(nullptr, {}, false);
    m_dispatcher.SetTopologyHandoff({}, 0, 0, false, {});
    m_topology.reset();

    if (!m_options.topologyRuntimeEnabled) {
        return;
    }

    if (m_options.topologyFilePath.empty()) {
        std::cerr << "WARN: Topology runtime enabled but topology_file is empty; using default pointer behavior." << std::endl;
        return;
    }

    TopologyModel loaded;
    std::string error;
    if (!LoadTopologyConfig(m_options.topologyFilePath, loaded, &error)) {
        std::cerr << "WARN: Failed to load topology config '" << m_options.topologyFilePath.string()
                  << "': " << error << "; using default pointer behavior." << std::endl;
        return;
    }

    const auto issues = loaded.validate();
    if (!issues.empty()) {
        std::cerr << "WARN: Invalid topology config '" << m_options.topologyFilePath.string()
                  << "': " << topologyIssueCodeName(issues.front().code)
                  << ": " << issues.front().message
                  << "; using default pointer behavior." << std::endl;
        return;
    }

    const std::string sourceDisplayId = SelectTopologySourceDisplay(
        loaded,
        m_options.localMachineName,
        screenSize);
    if (sourceDisplayId.empty()) {
        std::cerr << "WARN: Topology config '" << m_options.topologyFilePath.string()
                  << "' has no display for local machine '" << m_options.localMachineName
                  << "'; using default pointer behavior." << std::endl;
        return;
    }

    m_topology = std::make_shared<TopologyModel>(std::move(loaded));
    m_dispatcher.SetTopologyPreview(m_topology, sourceDisplayId, true);
    m_dispatcher.SetTopologyHandoff(
        m_options.localMachineName,
        screenSize.width,
        screenSize.height,
        true,
        [this](const MouseData& mouse,
               const TopologyPointerTransition&,
               const std::string& targetMachineId) {
            if (m_androidRelay &&
                targetMachineId == m_options.androidRelay.peerName &&
                TrySetAndroidControlActive(true) &&
                TrySendAndroidMouse(mouse)) {
                return true;
            }
            return m_options.powerToysCompatibilityEnabled && m_network && m_network->SendMouse(mouse);
        });
    std::cout << "[TOPOLOGY] Loaded topology from "
              << m_options.topologyFilePath.string()
              << " using source display " << sourceDisplayId
              << " with cross-machine handoff enforcement enabled." << std::endl;
}

int ClientRuntime::Run() {
    const ScreenSize screenSize = DetectScreenSize();
    if (screenSize.source == ScreenSize::Source::Fallback) {
        std::cerr << "WARN: Screen autodetection failed; using fallback size "
                  << screenSize.width << "x" << screenSize.height
                  << ". Set screen_width and screen_height in config for accurate scaling." << std::endl;
    } else {
        const char* source = "unknown";
        switch (screenSize.source) {
            case ScreenSize::Source::Explicit:
                source = "config/cli";
                break;
            case ScreenSize::Source::WaylandLogical:
                source = "kscreen-doctor";
                break;
            case ScreenSize::Source::Drm:
                source = "/sys/class/drm";
                break;
            case ScreenSize::Source::Fallback:
                break;
        }
        std::cout << "[INFO] Screen size: " << screenSize.width << "x" << screenSize.height
                  << " (" << source << ")" << std::endl;
    }

    m_input.SetScreenSize(screenSize.width, screenSize.height);
    if (!m_input.Initialize()) {
        std::cerr << "WARN: Virtual input initialization failed. Networking will continue, but local mouse/keyboard injection is disabled until /dev/uinput is accessible." << std::endl;
    }
    ConfigureTopologyPreview(screenSize);
    m_dispatcher.Start();

    if (m_options.powerToysCompatibilityEnabled) {
        m_network = std::make_unique<NetworkManager>(m_options.host, m_options.port, m_options.key);
        m_network->SetScreenSize(screenSize.width, screenSize.height);
        m_network->SetAutoConnectEnabled(m_options.autoConnectEnabled);
        m_network->SetReconnectBackoff(
            m_options.reconnectInitialBackoffMs,
            m_options.reconnectMaxBackoffMs,
            m_options.reconnectIdleRetryMs);
        if (m_options.resolveHost) {
            m_network->SetHostResolver(m_options.resolveHost);
        }
        if (m_options.localMachineId.has_value() || !m_options.localMachineName.empty()) {
            m_network->SetLocalIdentity(m_options.localMachineId.value_or(0), m_options.localMachineName);
        }
        if (m_options.onSessionEstablished) {
            m_network->SetOnSessionEstablished(m_options.onSessionEstablished);
        }
        m_network->SetOnSessionDisconnected([this]() {
            m_dispatcher.ResetInputState();
            if (m_options.onSessionDisconnected) {
                m_options.onSessionDisconnected();
            }
        });
    } else {
        std::cout << "[MODE] PowerToys compatibility disabled; running InputFlow peer services only." << std::endl;
    }

    if (m_network && m_options.clipboardEnabled) {
        m_clipboard = ClipboardManager::CreateDefault();
    }

    if (m_options.androidRelay.enabled) {
        m_androidRelay = std::make_unique<AndroidRelayServer>(m_options.androidRelay);
        m_androidRelay->SetOnReleaseRequested([this]() {
            TrySetAndroidControlActive(false);
            std::cout << "[ANDROID] Relay release requested; returning input to local desktop." << std::endl;
        });
        m_androidRelay->SetOnTopologyUpdate([this](const std::string& layoutJson) {
            ApplyAndroidTopologyUpdate(layoutJson);
        });
        if (screenSize.width > 0 && !m_options.localMachineName.empty()) {
            m_androidRelay->SetLocalDeviceInfo(m_options.localMachineName, screenSize.width, screenSize.height);
        }
        if (!m_androidRelay->Start()) {
            std::cerr << "WARN: Android relay failed to start; Android peer handoff is disabled." << std::endl;
            m_androidRelay.reset();
        }
    }
    StartLocalAndroidInputBridge(screenSize);

    if (m_clipboard) {
        m_clipboardBackendName = m_clipboard->BackendName();
        std::cout << "[INFO] Clipboard backend: " << m_clipboardBackendName << std::endl;
        m_lastClipboardPayload = m_clipboard->GetPayload();
        if (m_lastClipboardPayload.has_value()) {
            m_network->PrimeLocalClipboardPayload(*m_lastClipboardPayload);
        }
        m_network->SetClipboardProvider(m_clipboard->MakeProvider());
        m_network->SetOnClipboardCallback([this](const ClipboardPayload& payload) {
            std::thread([this, payload]() {
                if (!m_clipboard->SetPayload(payload)) {
                    std::cerr << "WARN: Failed to write incoming clipboard payload through backend '"
                              << m_clipboard->BackendName() << "'." << std::endl;
                    return;
                }

                {
                    std::lock_guard<std::mutex> lock(m_clipboardStateMutex);
                    m_lastClipboardPayload = payload;
                }

                if (payload.image) {
                    std::cout << "[CLIPBOARD] Received image update (" << payload.image->bytes.size() << " bytes). Header: ";
                    for (std::size_t i = 0; i < std::min(payload.image->bytes.size(), static_cast<std::size_t>(8)); ++i) {
                        printf("%02x ", payload.image->bytes[i]);
                    }
                    std::cout << std::endl;
                } else if (payload.plainText) {
                    std::cout << "[CLIPBOARD] Received text update (" << payload.plainText->size() << " bytes)" << std::endl;
                }
            }).detach();
        });
    } else if (!m_options.clipboardEnabled) {
        std::cerr << "WARN: Clipboard sync disabled by configuration." << std::endl;
    } else if (!m_network) {
        std::cerr << "WARN: Clipboard sync is disabled because PowerToys compatibility transport is not active." << std::endl;
    } else {
        std::cerr << "WARN: No supported clipboard backend detected. Install wl-clipboard for Wayland or xclip/xsel for X11 to enable clipboard sync." << std::endl;
    }

    if (m_clipboard && !m_options.clipboardSendEnabled) {
        std::cerr << "WARN: Local clipboard watch disabled; clipboard is running in receive-only mode." << std::endl;
    }

    if (m_network) {
        m_network->SetOnMouseCallback([this](const MouseData& md) {
            if (m_options.debugInputLogging) {
                std::cout << "[INPUT] Mouse: x=" << md.x << " y=" << md.y << " wParam=0x" << std::hex << md.wParam << std::dec << std::endl;
            } else if (m_options.debugShortcutLogging && md.wParam != 0x0200) {
                std::cout << "[INPUT] Mouse event: wParam=0x" << std::hex << md.wParam
                          << " x=" << std::dec << md.x
                          << " y=" << md.y
                          << " mouseData=" << md.mouseData
                          << std::endl;
            }
            if (m_androidRelayActive && TrySendAndroidMouse(md)) {
                return;
            }
            m_androidRelayActive = false;
            m_dispatcher.SubmitMouse(md);
        });

        m_network->SetOnKeyboardCallback([this](const KeyboardData& kd) {
            if (m_options.debugInputLogging || m_options.debugKeyLogging || m_options.debugShortcutLogging) {
                std::cout << "[INPUT] Keyboard: vk=0x" << std::hex << kd.vkCode
                          << " flags=0x" << kd.flags << std::dec << std::endl;
            }
            if (m_androidRelayActive && TrySendAndroidKeyboard(kd)) {
                return;
            }
            m_dispatcher.SubmitKeyboard(kd);
        });
    }

    if (m_network && !m_network->Connect()) {
        std::cerr << "Terminating: Network failure." << std::endl;
        m_dispatcher.Stop();
        if (m_androidRelay) {
            m_androidRelay->Stop();
        }
        StopLocalAndroidInputBridge();
        m_network.reset();
        return 1;
    }

    if (m_network && !m_options.autoConnectEnabled) {
        std::cout << "[CONNECT] Auto-connect disabled by configuration; service is idle until you re-enable it." << std::endl;
    }

    StartClipboardWatcher();
    if (m_network) {
        m_network->RunLoop();
    } else {
        while (!m_stopRequested.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    }
    Stop();
    return 0;
}

void ClientRuntime::StartClipboardWatcher() {
    if (!m_clipboard) {
        return;
    }

    if (!m_options.clipboardSendEnabled) {
        return;
    }

    m_clipboardWatcherRunning = true;
    m_clipboardWatcher = std::thread([this]() {
        const auto handleClipboardPayload = [this](const ClipboardPayload& payload) {
            bool changed = false;
            {
                std::lock_guard<std::mutex> lock(m_clipboardStateMutex);
                if (!m_lastClipboardPayload ||
                    m_lastClipboardPayload->plainText != payload.plainText ||
                    (m_lastClipboardPayload->image.has_value() != payload.image.has_value()) ||
                    (payload.image && m_lastClipboardPayload->image && payload.image->bytes != m_lastClipboardPayload->image->bytes)) {
                    m_lastClipboardPayload = payload;
                    changed = true;
                }
            }

            if (changed && m_network) {
                if (payload.image) {
                    std::cout << "[CLIPBOARD] Local image changed (" << payload.image->bytes.size() << " bytes)" << std::endl;
                } else if (payload.plainText) {
                    std::cout << "[CLIPBOARD] Local text changed (" << payload.plainText->size() << " bytes)" << std::endl;
                }
                m_network->NotifyLocalClipboardChanged(payload);
            }
        };

        if (m_clipboard->WatchPayloadChanges(m_clipboardWatcherRunning, handleClipboardPayload)) {
            return;
        }

        if (m_clipboardBackendName.rfind("wl-clipboard", 0) == 0 && !m_options.clipboardForcePoll) {
            std::cerr
                << "WARN: wl-clipboard watch mode is unavailable on this compositor. "
                << "Local clipboard polling is disabled to avoid disrupting launcher shortcuts. "
                << "Set clipboard_force_poll=true in config to re-enable polling."
                << std::endl;
            return;
        }

        while (m_clipboardWatcherRunning) {
            const auto currentPayload = m_clipboard->GetPayload();
            if (currentPayload.has_value()) {
                handleClipboardPayload(*currentPayload);
            }

            std::this_thread::sleep_for(std::chrono::milliseconds(m_options.clipboardPollMs));
        }
    });
}

void ClientRuntime::StopClipboardWatcher() {
    m_clipboardWatcherRunning = false;
    if (m_clipboardWatcher.joinable()) {
        m_clipboardWatcher.join();
    }
}

void ClientRuntime::Stop() {
    if (m_stopRequested.exchange(true)) {
        return;
    }

    StopLocalAndroidInputBridge();
    StopClipboardWatcher();
    if (m_androidRelay) {
        m_androidRelay->Stop();
    }
    if (m_network) {
        m_network->Stop();
    }
    m_dispatcher.Stop();
    if (m_latencyStats && m_latencyStats->Enabled()) {
        std::cout << InputLatencyStats::FormatReport(m_latencyStats->Snapshot());
    }
    if (m_input.MouseTraceEnabled()) {
        std::cout << InputManager::FormatMouseTrace(m_input.MouseTraceSnapshot());
    }
    m_network.reset();
    m_androidRelay.reset();
}

void ClientRuntime::StartLocalAndroidInputBridge(const ScreenSize& screenSize) {
    if (!m_options.topologyRuntimeEnabled) {
        return;
    }

    if (m_options.androidCaptureBackend.empty() || m_options.androidCaptureBackend == "none") {
        std::cout << "[ANDROID] Local capture backend disabled; relay will accept remote/topology forwarded input only." << std::endl;
        return;
    }

    if (m_options.androidCaptureBackend == "libei") {
        if (!m_topology) {
            if (!m_androidRelay) {
                std::cerr << "WARN: libei local capture needs topology_file or Android relay; local capture disabled." << std::endl;
                return;
            }
            std::cout << "[ANDROID] Topology file is not loaded; enabling libei right-edge Android handoff." << std::endl;
        }
        LibeiInputCaptureBridgeOptions options;
        options.desktopWidth = screenSize.width;
        options.desktopHeight = screenSize.height;
        options.topology = m_topology;
        options.localMachineName = m_options.localMachineName;
        options.androidPeerName = m_options.androidRelay.peerName;
        options.sendMouse = [this](const MouseData& mouse) {
            return TrySendAndroidMouse(mouse);
        };
        options.sendMouseToMachine = [this](const MouseData& mouse, const std::string& targetMachineId) {
            if (targetMachineId == m_options.androidRelay.peerName) {
                return TrySetAndroidControlActive(true) && TrySendAndroidMouse(mouse);
            }
            return m_options.powerToysCompatibilityEnabled && m_network && m_network->SendMouse(mouse);
        };
        options.sendKeyboard = [this](const KeyboardData& keyboard) {
            return TrySendAndroidKeyboard(keyboard);
        };
        options.sendKeyboardToMachine = [this](const KeyboardData& keyboard, const std::string& targetMachineId) {
            if (targetMachineId == m_options.androidRelay.peerName) {
                return TrySendAndroidKeyboard(keyboard);
            }
            return m_options.powerToysCompatibilityEnabled && m_network && m_network->SendKeyboard(keyboard);
        };
        options.sendGesture = [this](const std::string& kind, double dx, double dy) {
            return TrySendAndroidGesture(kind, dx, dy);
        };
        options.sendControl = [this](bool active) {
            return TrySetAndroidControlActive(active);
        };
        options.sendControlForMachine = [this](bool active, const std::string& targetMachineId) {
            if (targetMachineId == m_options.androidRelay.peerName) {
                return TrySetAndroidControlActive(active);
            }
            return true;
        };
        m_libeiInputCaptureBridge = std::make_unique<LibeiInputCaptureBridge>(std::move(options));
        m_libeiInputCaptureBridge->Start();
        return;
    }

    if (!m_topology) {
        std::cerr << "WARN: Android local handoff requires a topology file for android_capture_backend="
                  << m_options.androidCaptureBackend << "." << std::endl;
        return;
    }

    if (m_options.androidCaptureBackend != "evdev") {
        std::cerr << "WARN: Unknown android_capture_backend='" << m_options.androidCaptureBackend
                  << "'; local Android handoff disabled." << std::endl;
        return;
    }

    std::cerr << "WARN: android_capture_backend=evdev is a prototype fallback. It can mirror Fedora cursor movement and is not monitor-grade on KDE Wayland." << std::endl;

    LocalAndroidInputBridgeOptions options;
    options.topology = m_topology;
    options.localMachineName = m_options.localMachineName;
    options.androidPeerName = m_options.androidRelay.peerName;
    options.desktopWidth = screenSize.width;
    options.desktopHeight = screenSize.height;
    options.sendMouse = [this](const MouseData& mouse) {
        return TrySendAndroidMouse(mouse);
    };

    m_localAndroidInputBridge = std::make_unique<LocalAndroidInputBridge>(std::move(options));
    m_localAndroidInputBridge->Start();
}

void ClientRuntime::StopLocalAndroidInputBridge() {
    if (m_libeiInputCaptureBridge) {
        m_libeiInputCaptureBridge->Stop();
        m_libeiInputCaptureBridge.reset();
    }

    if (!m_localAndroidInputBridge) {
        return;
    }
    m_localAndroidInputBridge->Stop();
    m_localAndroidInputBridge.reset();
}

bool ClientRuntime::TrySendAndroidMouse(const MouseData& mouse) {
    return m_androidRelay && m_androidRelay->SendMouse(mouse);
}

bool ClientRuntime::TrySendAndroidKeyboard(const KeyboardData& keyboard) {
    return m_androidRelayActive && m_androidRelay && m_androidRelay->SendKeyboard(keyboard);
}

bool ClientRuntime::TrySendAndroidGesture(const std::string& kind, double dx, double dy) {
    return m_androidRelayActive && m_androidRelay && m_androidRelay->SendGesture(kind, dx, dy);
}

bool ClientRuntime::TrySetAndroidControlActive(bool active) {
    const bool wasActive = m_androidRelayActive.exchange(active);
    if (!m_androidRelay || wasActive == active) {
        return m_androidRelay != nullptr;
    }
    return m_androidRelay->SendControl(active);
}

void ClientRuntime::ApplyAndroidTopologyUpdate(const std::string& frameJson) {
    if (!m_options.androidRelay.layoutEditorEnabled) {
        return;
    }

    // Minimal JSON parse: extract "layout" array positions for "linux" and "android"
    int lx = 0, ly = 0, ax = 0, ay = 0;
    bool gotLinux = false, gotAndroid = false;
    const std::string layoutMarker = "\"layout\":[";
    auto layoutStart = frameJson.find(layoutMarker);
    if (layoutStart == std::string::npos) {
        return;
    }
    std::string remaining = frameJson.substr(layoutStart + layoutMarker.size());
    while (!remaining.empty() && remaining.front() != ']') {
        auto idPos = remaining.find("\"id\":\"");
        auto xPos = remaining.find("\"x\":");
        auto yPos = remaining.find("\"y\":");
        if (idPos == std::string::npos || xPos == std::string::npos || yPos == std::string::npos) {
            break;
        }
        auto idValStart = idPos + 6;
        auto idValEnd = remaining.find('"', idValStart);
        if (idValEnd == std::string::npos) break;
        const std::string id = remaining.substr(idValStart, idValEnd - idValStart);
        const int x = std::stoi(remaining.substr(xPos + 4));
        const int y = std::stoi(remaining.substr(yPos + 4));
        if (id == "linux") { lx = x; ly = y; gotLinux = true; }
        else if (id == "android") { ax = x; ay = y; gotAndroid = true; }
        auto nextItem = remaining.find('{', idValEnd);
        if (nextItem == std::string::npos) break;
        remaining = remaining.substr(nextItem);
    }
    if (!gotLinux || !gotAndroid) {
        std::cerr << "[ANDROID] topology_update: missing linux or android position." << std::endl;
        return;
    }

    const int linuxW = m_options.screenWidth.value_or(1920);
    const int linuxH = m_options.screenHeight.value_or(1080);
    const int androidW = m_options.androidRelay.androidDeviceWidth;
    const int androidH = m_options.androidRelay.androidDeviceHeight;
    const std::string linuxMachine = m_options.localMachineName.empty() ? "linux" : m_options.localMachineName;
    const std::string androidMachine = m_options.androidRelay.peerName;

    // Determine relative placement direction
    const int dx = ax - lx;
    const int dy = ay - ly;
    std::string exitEdge, entryEdge;
    int androidAbsX = 0, androidAbsY = 0;
    if (std::abs(dx) >= std::abs(dy)) {
        if (dx >= 0) {
            exitEdge = "right"; entryEdge = "left";
            androidAbsX = linuxW;
            androidAbsY = 0;
        } else {
            exitEdge = "left"; entryEdge = "right";
            androidAbsX = -androidW;
            androidAbsY = 0;
        }
    } else {
        if (dy >= 0) {
            exitEdge = "down"; entryEdge = "up";
            androidAbsX = 0;
            androidAbsY = linuxH;
        } else {
            exitEdge = "up"; entryEdge = "down";
            androidAbsX = 0;
            androidAbsY = -androidH;
        }
    }

    std::ostringstream cfg;
    cfg << "# auto-generated from Android layout editor\n"
        << "wrap=none\n"
        << "machine=" << linuxMachine << "\n"
        << "machine=" << androidMachine << "\n"
        << "display=" << linuxMachine << "_d," << linuxMachine << ",0,0," << linuxW << "," << linuxH << "\n"
        << "display=" << androidMachine << "_d," << androidMachine << ","
        << androidAbsX << "," << androidAbsY << "," << androidW << "," << androidH << "\n"
        << "link=" << linuxMachine << "_d," << exitEdge << "," << androidMachine << "_d," << entryEdge << "\n"
        << "link=" << androidMachine << "_d," << entryEdge << "," << linuxMachine << "_d," << exitEdge << "\n";

    TopologyModel newModel;
    std::string error;
    if (!ParseTopologyConfig(cfg.str(), newModel, &error)) {
        std::cerr << "[ANDROID] Failed to parse generated topology: " << error << std::endl;
        return;
    }
    const auto issues = newModel.validate();
    if (!issues.empty()) {
        std::cerr << "[ANDROID] Generated topology invalid: " << issues.front().message << std::endl;
        return;
    }

    const ScreenSize screenSize{linuxW, linuxH, ScreenSize::Source::Explicit};
    const std::string sourceDisplayId = SelectTopologySourceDisplay(newModel, linuxMachine, screenSize);
    if (sourceDisplayId.empty()) {
        std::cerr << "[ANDROID] Generated topology has no display for local machine." << std::endl;
        return;
    }

    m_topology = std::make_shared<TopologyModel>(std::move(newModel));
    m_dispatcher.SetTopologyPreview(m_topology, sourceDisplayId, true);
    m_dispatcher.SetTopologyHandoff(
        linuxMachine,
        linuxW,
        linuxH,
        true,
        [this](const MouseData& mouse,
               const TopologyPointerTransition&,
               const std::string& targetMachineId) {
            if (m_androidRelay &&
                targetMachineId == m_options.androidRelay.peerName &&
                TrySetAndroidControlActive(true) &&
                TrySendAndroidMouse(mouse)) {
                return true;
            }
            return m_options.powerToysCompatibilityEnabled && m_network && m_network->SendMouse(mouse);
        });

    std::cout << "[ANDROID] Applied topology update: linux " << exitEdge
              << " → android." << std::endl;

    // Optionally persist to topology_file
    if (!m_options.topologyFilePath.empty()) {
        std::ofstream out(m_options.topologyFilePath);
        if (out) {
            out << cfg.str();
            std::cout << "[ANDROID] Topology saved to " << m_options.topologyFilePath << std::endl;
        }
    }
}

} // namespace mwb
