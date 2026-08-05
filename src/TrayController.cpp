#include <gtk/gtk.h>
#include <libayatana-appindicator/app-indicator.h>

#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <iostream>
#include <mutex>
#include <optional>
#include <string>
#include <sys/file.h>
#include <thread>
#include <unistd.h>
#include <vector>

// Data/runtime headers are GTK-free and are referenced by helper functions
// that live outside the GUI guard, so they must be included unconditionally.
#include "AppConfig.h"
#include "AppState.h"
#include "ClientRuntime.h"
#include "PeerRecovery.h"

#ifdef MWB_HAVE_GTK_GUI
#include "TrayController.h"
#include "GuiMainWindow.h"
#endif

namespace {

constexpr const char* kAppName = "InputFlow";
constexpr const char* kServiceName = "mwb-client.service";
constexpr const char* kControllerScriptName = "mwb-desktop-ui.sh";
constexpr const char* kIndicatorId = "inputflow-tray";
constexpr const char* kTrayLockFileName = "inputflow-tray.lock";
constexpr const char* kBashPath = "/bin/bash";
constexpr const char* kSystemctlPath = "/usr/bin/systemctl";

struct TrayContext {
    AppIndicator* indicator{nullptr};
    GtkWidget* statusItem{nullptr};
    GtkWidget* openControllerItem{nullptr};
    GtkWidget* editSettingsItem{nullptr};
    GtkWidget* editConnectionItem{nullptr};
    GtkWidget* healthCheckItem{nullptr};
    GtkWidget* diagnosticsBundleItem{nullptr};
    GtkWidget* connectionQualityItem{nullptr};
    GtkWidget* guidedPairingItem{nullptr};
    GtkWidget* discoverPeersItem{nullptr};
    GtkWidget* showPeersItem{nullptr};
    GtkWidget* trayHelpItem{nullptr};
    GtkWidget* installDesktopEntriesItem{nullptr};
    GtkWidget* showStatusItem{nullptr};
    GtkWidget* startItem{nullptr};
    GtkWidget* stopItem{nullptr};
    GtkWidget* restartItem{nullptr};
    std::string controllerPath;
    std::string iconThemePath;
    std::string lastState;
#ifdef MWB_HAVE_GTK_GUI
    mwb::GuiMainWindow* mainWindow{nullptr};
    std::atomic<bool>* stopFlag{nullptr};
#endif
};

std::optional<std::string> RunCommandCapture(const std::vector<std::string>& argv) {
    if (argv.empty()) {
        return std::nullopt;
    }
    std::vector<char*> args;
    args.reserve(argv.size() + 1);
    for (const auto& arg : argv) {
        args.push_back(const_cast<char*>(arg.c_str()));
    }
    args.push_back(nullptr);

    gchar* stdoutData = nullptr;
    gchar* stderrData = nullptr;
    gint waitStatus = 0;
    GError* error = nullptr;
    const gboolean spawned = g_spawn_sync(
        nullptr,
        args.data(),
        nullptr,
        G_SPAWN_SEARCH_PATH,
        nullptr,
        nullptr,
        &stdoutData,
        &stderrData,
        &waitStatus,
        &error);
    std::string output = stdoutData == nullptr ? "" : stdoutData;
    g_free(stdoutData);
    g_free(stderrData);
    if (!spawned) {
        if (error != nullptr) {
            g_error_free(error);
        }
        return std::nullopt;
    }
    GError* waitError = nullptr;
    const bool succeeded = g_spawn_check_wait_status(waitStatus, &waitError);
    if (waitError != nullptr) {
        g_error_free(waitError);
    }
    if (!succeeded && output.empty()) {
        return std::nullopt;
    }

    while (!output.empty() && (output.back() == '\n' || output.back() == '\r')) {
        output.pop_back();
    }
    return output;
}

bool ServiceProcessAppearsRunning() {
    const auto result = RunCommandCapture({"pgrep", "-f", "[/]mwb_client run --config"});
    return result.has_value() && !result->empty();
}

std::int64_t CurrentEpochSeconds() {
    return std::chrono::duration_cast<std::chrono::seconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

void SaveStateOrLog(const std::string& statePath, const mwb::AppState& state) {
    std::string error;
    if (!mwb::SaveAppState(statePath, state, error)) {
        std::cerr << "WARN: Failed to save state " << statePath << ": " << error << std::endl;
    }
}

// Lock the local desktop session. Tries logind first (works headless and on
// most modern desktops), then falls back to common screen lockers.
void LockLocalSession() {
    const std::vector<std::vector<std::string>> commands{
        {"loginctl", "lock-session"},
        {"loginctl", "lock-sessions"},
        {"xdg-screensaver", "lock"},
        {"qdbus", "org.freedesktop.ScreenSaver", "/ScreenSaver", "Lock"},
    };
    for (const auto& command : commands) {
        std::vector<char*> args;
        args.reserve(command.size() + 1);
        for (const auto& arg : command) {
            args.push_back(const_cast<char*>(arg.c_str()));
        }
        args.push_back(nullptr);
        gint waitStatus = 0;
        GError* error = nullptr;
        const gboolean spawned = g_spawn_sync(
            nullptr,
            args.data(),
            nullptr,
            static_cast<GSpawnFlags>(
                G_SPAWN_SEARCH_PATH |
                G_SPAWN_STDOUT_TO_DEV_NULL |
                G_SPAWN_STDERR_TO_DEV_NULL),
            nullptr,
            nullptr,
            nullptr,
            nullptr,
            &waitStatus,
            &error);
        if (error != nullptr) {
            g_error_free(error);
        }
        GError* waitError = nullptr;
        const bool succeeded =
            spawned && g_spawn_check_wait_status(waitStatus, &waitError);
        if (waitError != nullptr) {
            g_error_free(waitError);
        }
        if (succeeded) {
            return;
        }
    }
}

bool SpawnAsync(const std::vector<std::string>& argv) {
    std::vector<char*> args;
    args.reserve(argv.size() + 1);
    for (const auto& arg : argv) {
        args.push_back(const_cast<char*>(arg.c_str()));
    }
    args.push_back(nullptr);

    GError* error = nullptr;
    const gboolean ok = g_spawn_async(
        nullptr,
        args.data(),
        nullptr,
        G_SPAWN_DEFAULT,
        nullptr,
        nullptr,
        nullptr,
        &error);
    if (!ok) {
        if (error != nullptr) {
            g_warning("Failed to spawn command: %s", error->message);
            g_error_free(error);
        }
        return false;
    }
    return true;
}

void ShowMessageDialog(GtkMessageType type, const std::string& primary, const std::string& secondary = {}) {
    GtkWidget* dialog = gtk_message_dialog_new(
        nullptr,
        static_cast<GtkDialogFlags>(GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT),
        type,
        GTK_BUTTONS_CLOSE,
        "%s",
        primary.c_str());
    if (!secondary.empty()) {
        gtk_message_dialog_format_secondary_text(GTK_MESSAGE_DIALOG(dialog), "%s", secondary.c_str());
    }
    gtk_dialog_run(GTK_DIALOG(dialog));
    gtk_widget_destroy(dialog);
}

std::optional<std::filesystem::path> ResolveExecutablePath() {
    std::error_code error;
    std::filesystem::path exePath = std::filesystem::read_symlink("/proc/self/exe", error);
    if (!error && !exePath.empty()) {
        return exePath;
    }

    exePath = std::filesystem::canonical("/proc/self/exe", error);
    if (!error && !exePath.empty()) {
        return exePath;
    }

    return std::nullopt;
}

bool IsRegularFile(const std::filesystem::path& path) {
    std::error_code error;
    return std::filesystem::is_regular_file(path, error);
}

bool IsDirectory(const std::filesystem::path& path) {
    std::error_code error;
    return std::filesystem::is_directory(path, error);
}

std::filesystem::path RuntimeLockPath() {
    if (const char* runtimeDir = std::getenv("XDG_RUNTIME_DIR")) {
        if (runtimeDir[0] != '\0') {
            return std::filesystem::path(runtimeDir) / kTrayLockFileName;
        }
    }

    return std::filesystem::temp_directory_path() /
           (std::string(kTrayLockFileName) + "." + std::to_string(getuid()));
}

int AcquireSingleInstanceLock() {
    const auto lockPath = RuntimeLockPath();
    const int fd = open(lockPath.c_str(), O_CREAT | O_RDWR | O_CLOEXEC, 0600);
    if (fd < 0) {
        std::cerr << "WARN: Could not open tray lock " << lockPath << ": " << std::strerror(errno) << std::endl;
        return -1;
    }

    if (flock(fd, LOCK_EX | LOCK_NB) == 0) {
        return fd;
    }

    const int lockError = errno;
    close(fd);
    if (lockError == EWOULDBLOCK || lockError == EAGAIN) {
        std::cerr << "InputFlow tray is already running." << std::endl;
        return -2;
    }

    std::cerr << "WARN: Could not lock tray instance " << lockPath << ": " << std::strerror(lockError) << std::endl;
    return -1;
}

[[maybe_unused]] std::string ResolveControllerPath() {
    std::vector<std::filesystem::path> candidates;
    if (const auto exePath = ResolveExecutablePath()) {
        const auto exeDir = exePath->parent_path();
        candidates.emplace_back(exeDir / kControllerScriptName);
        if (!exeDir.parent_path().empty()) {
            candidates.emplace_back(exeDir.parent_path() / kControllerScriptName);
        }
    }

    std::error_code error;
    const auto cwd = std::filesystem::current_path(error);
    if (!error && !cwd.empty()) {
        candidates.emplace_back(cwd / kControllerScriptName);
    }

    for (const auto& candidate : candidates) {
        if (!IsRegularFile(candidate)) {
            continue;
        }

        const auto absolutePath = std::filesystem::absolute(candidate, error);
        if (!error && !absolutePath.empty()) {
            return absolutePath.string();
        }
        return candidate.string();
    }

    return {};
}

std::string ResolveIconThemePath() {
    std::vector<std::filesystem::path> candidates;
    if (const auto exePath = ResolveExecutablePath()) {
        const auto exeDir = exePath->parent_path();
        candidates.emplace_back(exeDir / "assets" / "icons");
        if (!exeDir.parent_path().empty()) {
            candidates.emplace_back(exeDir.parent_path() / "assets" / "icons");
        }
    }

    std::error_code error;
    const auto cwd = std::filesystem::current_path(error);
    if (!error && !cwd.empty()) {
        candidates.emplace_back(cwd / "assets" / "icons");
    }

    for (const auto& candidate : candidates) {
        if (!IsDirectory(candidate) || !IsRegularFile(candidate / "inputflow-tray.svg")) {
            continue;
        }

        const auto absolutePath = std::filesystem::absolute(candidate, error);
        if (!error && !absolutePath.empty()) {
            return absolutePath.string();
        }
        return candidate.string();
    }

    return {};
}

std::string QueryServiceState() {
    const bool daemonRunning = ServiceProcessAppearsRunning();
    if (access(kSystemctlPath, X_OK) != 0) {
        return daemonRunning ? "active" : "inactive";
    }
    const auto result = RunCommandCapture(
        {kSystemctlPath, "--user", "is-active", kServiceName});
    if (!result.has_value() || result->empty() || *result == "unknown") {
        return daemonRunning ? "active" : "inactive";
    }
    if ((*result == "active" || *result == "deactivating") && !daemonRunning) {
        return "inactive";
    }
    if (*result == "inactive" && daemonRunning) {
        return "active";
    }
    return *result;
}

std::string DescribeState(const std::string& state) {
    if (state == "active") {
        return "Running";
    }
    if (state == "activating") {
        return "Starting";
    }
    if (state == "deactivating") {
        return "Stopping";
    }
    if (state == "reloading") {
        return "Restarting";
    }
    if (state == "failed") {
        return "Needs Attention";
    }
    if (state == "inactive") {
        return "Stopped";
    }
    if (state == "unknown" || state.empty()) {
        return "Unavailable";
    }
    return state;
}

std::string IndicatorLabel(const std::string& state) {
    if (state == "failed") {
        return "IF!";
    }
    if (state == "activating" || state == "reloading") {
        return "IF...";
    }
    if (state == "deactivating") {
        return "IF-";
    }
    return "IF";
}

std::string IndicatorDescription(const std::string& displayState) {
    return std::string(kServiceName) + ": " + displayState;
}

std::string TrayVisibilityHelpText(const TrayContext& context) {
    std::string helpText =
        "The tray is running, but some desktops hide indicators in the overflow area.\n\n"
        "Tips:\n"
        "• On KDE Plasma, open the system tray popup and set InputFlow to Always Shown.\n"
        "• Look for a teal InputFlow icon with two small screens and an IF label.\n"
        "• Left click or right click the tray icon to open controls.\n";

    if (!context.controllerPath.empty()) {
        helpText += "• If you still do not see the tray, open the controller directly from:\n";
        helpText += context.controllerPath;
    } else {
        helpText += "• If you still do not see the tray, run mwb-desktop-ui.sh directly.";
    }

    return helpText;
}

bool ShouldShowStartupHint() {
    const char* value = std::getenv("MWB_TRAY_STARTUP_HINT");
    if (value != nullptr) {
        return std::string(value) != "0";
    }

    return isatty(STDIN_FILENO) != 0;
}

[[maybe_unused]] void MaybeShowStartupHint(const TrayContext& context) {
    if (!ShouldShowStartupHint()) {
        return;
    }

    ShowMessageDialog(
        GTK_MESSAGE_INFO,
        "InputFlow tray is running.",
        TrayVisibilityHelpText(context));
}

void ShowTrayVisibilityHelp(const TrayContext& context) {
    ShowMessageDialog(
        GTK_MESSAGE_INFO,
        "How to find the InputFlow tray",
        TrayVisibilityHelpText(context));
}

std::string IndicatorAccessibleLabel(const std::string& state) {
    if (state == "failed") {
        return "IF attention";
    }
    return "IF";
}

const char* FallbackIndicatorIconName(const std::string& state) {
    if (state == "active") {
        return "network-transmit-receive-symbolic";
    }
    if (state == "activating" || state == "deactivating" || state == "reloading") {
        return "view-refresh-symbolic";
    }
    if (state == "failed") {
        return "dialog-warning-symbolic";
    }
    if (state == "inactive") {
        return "network-offline-symbolic";
    }
    return "network-idle-symbolic";
}

const char* BundledIndicatorIconName(const std::string& state) {
    if (state == "activating" || state == "deactivating" || state == "reloading") {
        return "inputflow-tray-busy";
    }
    if (state == "failed") {
        return "inputflow-tray-attention";
    }
    if (state == "inactive") {
        return "inputflow-tray-offline";
    }
    return "inputflow-tray";
}

AppIndicatorStatus IndicatorStatus(const std::string& state) {
    if (state == "failed") {
        return APP_INDICATOR_STATUS_ATTENTION;
    }
    return APP_INDICATOR_STATUS_ACTIVE;
}

void UpdateIndicatorVisuals(TrayContext* context, const std::string& state) {
    if (context->lastState == state) {
        return;
    }
    context->lastState = state;

    const std::string displayState = DescribeState(state);
    const std::string statusText = "Service: " + displayState;
    const std::string indicatorDescription = IndicatorDescription(displayState);
    gtk_menu_item_set_label(GTK_MENU_ITEM(context->statusItem), statusText.c_str());

    const bool active = (state == "active");
    const bool starting = (state == "activating" || state == "reloading");
    const bool stopping = (state == "deactivating");

    gtk_widget_set_sensitive(context->startItem, !active && !starting && !stopping);
    gtk_widget_set_sensitive(context->stopItem, active || starting);
    gtk_widget_set_sensitive(context->restartItem, active);

#ifdef MWB_HAVE_GTK_GUI
    const bool controllerAvailable = (context->mainWindow != nullptr);
#else
    const bool controllerAvailable = !context->controllerPath.empty();
#endif
    gtk_widget_set_sensitive(context->editSettingsItem, controllerAvailable);
    gtk_widget_set_sensitive(context->editConnectionItem, controllerAvailable);
    gtk_widget_set_sensitive(context->healthCheckItem, controllerAvailable);
    gtk_widget_set_sensitive(context->diagnosticsBundleItem, controllerAvailable);
    gtk_widget_set_sensitive(context->connectionQualityItem, controllerAvailable);
    gtk_widget_set_sensitive(context->guidedPairingItem, controllerAvailable);
    gtk_widget_set_sensitive(context->discoverPeersItem, controllerAvailable);
    gtk_widget_set_sensitive(context->showPeersItem, controllerAvailable);
    gtk_widget_set_sensitive(context->trayHelpItem, TRUE);
    gtk_widget_set_sensitive(context->installDesktopEntriesItem, controllerAvailable);
    gtk_widget_set_sensitive(context->showStatusItem, controllerAvailable);

    const std::string shortLabel = IndicatorLabel(state);
    const std::string accessibleLabel = IndicatorAccessibleLabel(state);

    app_indicator_set_status(context->indicator, IndicatorStatus(state));
    app_indicator_set_title(context->indicator, kAppName);
    app_indicator_set_label(context->indicator, shortLabel.c_str(), accessibleLabel.c_str());
    if (!context->iconThemePath.empty()) {
        app_indicator_set_attention_icon_full(context->indicator, "inputflow-tray-attention", "InputFlow needs attention");
        app_indicator_set_icon_full(context->indicator, BundledIndicatorIconName(state), indicatorDescription.c_str());
    } else {
        app_indicator_set_icon_full(context->indicator, FallbackIndicatorIconName(state), indicatorDescription.c_str());
    }
}

bool LaunchController(TrayContext* context, const std::vector<std::string>& controllerArgs = {}) {
    if (context->controllerPath.empty()) {
        ShowMessageDialog(
            GTK_MESSAGE_ERROR,
            "InputFlow controller not found.",
            "Expected to find mwb-desktop-ui.sh next to the tray binary or in the repository root.");
        return false;
    }

    if (access(kBashPath, X_OK) != 0) {
        ShowMessageDialog(
            GTK_MESSAGE_ERROR,
            "bash is not available.",
            "The tray expected to find /bin/bash to launch mwb-desktop-ui.sh.");
        return false;
    }

    std::vector<std::string> argv = {kBashPath, context->controllerPath};
    argv.insert(argv.end(), controllerArgs.begin(), controllerArgs.end());
    if (SpawnAsync(argv)) {
        return true;
    }

    ShowMessageDialog(
        GTK_MESSAGE_ERROR,
        "Failed to launch the InputFlow controller.",
        "The tray could not start mwb-desktop-ui.sh through bash.");
    return false;
}

gboolean RefreshStatusOnce(gpointer userData);

void RunServiceAction(TrayContext* context, const std::string& action, const std::string& optimisticState) {
    if (access(kSystemctlPath, X_OK) == 0 &&
        SpawnAsync({kSystemctlPath, "--user", action, kServiceName})) {
        UpdateIndicatorVisuals(context, optimisticState);
        g_timeout_add_seconds(1, RefreshStatusOnce, context);
        g_timeout_add_seconds(3, RefreshStatusOnce, context);
        g_timeout_add_seconds(8, RefreshStatusOnce, context);
        return;
    }

    ShowMessageDialog(
        GTK_MESSAGE_ERROR,
        "Failed to run the requested service action.",
        "systemctl --user could not be started from the tray.");
    UpdateIndicatorVisuals(context, QueryServiceState());
}

gboolean RefreshStatus(gpointer userData) {
    auto* context = static_cast<TrayContext*>(userData);
    UpdateIndicatorVisuals(context, QueryServiceState());
    return G_SOURCE_CONTINUE;
}

gboolean RefreshStatusOnce(gpointer userData) {
    auto* context = static_cast<TrayContext*>(userData);
    UpdateIndicatorVisuals(context, QueryServiceState());
    return G_SOURCE_REMOVE;
}

void OnOpenController(GtkMenuItem*, gpointer userData) {
    auto* context = static_cast<TrayContext*>(userData);
#ifdef MWB_HAVE_GTK_GUI
    if (context->mainWindow) { context->mainWindow->ShowTab(0); return; }
#endif
    (void)LaunchController(context);
}

void OnEditSettings(GtkMenuItem*, gpointer userData) {
    auto* context = static_cast<TrayContext*>(userData);
#ifdef MWB_HAVE_GTK_GUI
    if (context->mainWindow) { context->mainWindow->ShowTab(1); return; }
#endif
    (void)LaunchController(context, {"settings"});
}

void OnEditConnectionBehavior(GtkMenuItem*, gpointer userData) {
    auto* context = static_cast<TrayContext*>(userData);
#ifdef MWB_HAVE_GTK_GUI
    if (context->mainWindow) { context->mainWindow->ShowTab(1); return; }
#endif
    (void)LaunchController(context, {"connection"});
}

void OnHealthCheck(GtkMenuItem*, gpointer userData) {
    auto* context = static_cast<TrayContext*>(userData);
#ifdef MWB_HAVE_GTK_GUI
    if (context->mainWindow) { context->mainWindow->ShowTab(0); return; }
#endif
    (void)LaunchController(context, {"health-check"});
}

void OnDiagnosticsBundle(GtkMenuItem*, gpointer userData) {
    auto* context = static_cast<TrayContext*>(userData);
#ifdef MWB_HAVE_GTK_GUI
    if (context->mainWindow) { context->mainWindow->ShowTab(0); return; }
#endif
    (void)LaunchController(context, {"diagnostics-bundle"});
}

void OnConnectionQuality(GtkMenuItem*, gpointer userData) {
    auto* context = static_cast<TrayContext*>(userData);
#ifdef MWB_HAVE_GTK_GUI
    if (context->mainWindow) { context->mainWindow->ShowTab(0); return; }
#endif
    (void)LaunchController(context, {"connection-quality"});
}

void OnGuidedPairing(GtkMenuItem*, gpointer userData) {
    auto* context = static_cast<TrayContext*>(userData);
#ifdef MWB_HAVE_GTK_GUI
    if (context->mainWindow) { context->mainWindow->ShowTab(1); return; }
#endif
    (void)LaunchController(context, {"guided-pairing"});
}

void OnDiscoverPeers(GtkMenuItem*, gpointer userData) {
    auto* context = static_cast<TrayContext*>(userData);
#ifdef MWB_HAVE_GTK_GUI
    if (context->mainWindow) { context->mainWindow->ShowTab(0); return; }
#endif
    (void)LaunchController(context, {"discover"});
}

void OnShowPeers(GtkMenuItem*, gpointer userData) {
    auto* context = static_cast<TrayContext*>(userData);
#ifdef MWB_HAVE_GTK_GUI
    if (context->mainWindow) { context->mainWindow->ShowTab(0); return; }
#endif
    (void)LaunchController(context, {"peers"});
}

void OnStartService(GtkMenuItem*, gpointer userData) {
    auto* context = static_cast<TrayContext*>(userData);
    RunServiceAction(context, "start", "activating");
}

void OnStopService(GtkMenuItem*, gpointer userData) {
    auto* context = static_cast<TrayContext*>(userData);
    RunServiceAction(context, "stop", "deactivating");
}

void OnRestartService(GtkMenuItem*, gpointer userData) {
    auto* context = static_cast<TrayContext*>(userData);
    RunServiceAction(context, "restart", "reloading");
}

void OnShowStatus(GtkMenuItem*, gpointer userData) {
    auto* context = static_cast<TrayContext*>(userData);
#ifdef MWB_HAVE_GTK_GUI
    if (context->mainWindow) { context->mainWindow->ShowTab(0); return; }
#endif
    (void)LaunchController(context, {"status"});
}

void OnShowTrayHelp(GtkMenuItem*, gpointer userData) {
    auto* context = static_cast<TrayContext*>(userData);
    ShowTrayVisibilityHelp(*context);
}

void OnInstallDesktopEntries(GtkMenuItem*, gpointer userData) {
    auto* context = static_cast<TrayContext*>(userData);
#ifdef MWB_HAVE_GTK_GUI
    if (context->mainWindow) { context->mainWindow->ShowTab(1); return; }
#endif
    (void)LaunchController(context, {"install-desktop-entry"});
}

void OnOpenMonitorLayout(GtkMenuItem*, gpointer userData) {
    auto* context = static_cast<TrayContext*>(userData);
#ifdef MWB_HAVE_GTK_GUI
    if (context->mainWindow) { context->mainWindow->ShowTab(2); return; }
#endif
}

void OnLockScreen(GtkMenuItem*, gpointer) {
    LockLocalSession();
}

void OnQuit(GtkMenuItem*, gpointer userData) {
#ifdef MWB_HAVE_GTK_GUI
    auto* context = static_cast<TrayContext*>(userData);
    if (context->stopFlag) context->stopFlag->store(true);
#else
    (void)userData;
#endif
    gtk_main_quit();
}

GtkWidget* AddMenuItem(GtkWidget* menu, const char* label, GCallback callback, gpointer userData) {
    GtkWidget* item = gtk_menu_item_new_with_label(label);
    g_signal_connect(item, "activate", callback, userData);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), item);
    return item;
}

} // namespace

// ---- shared tray-building helper (used by both standalone and embedded) -----

namespace {

void BuildTrayMenu(TrayContext& context) {
    GtkWidget* menu = gtk_menu_new();
    context.statusItem = gtk_menu_item_new_with_label("Service: Checking...");
    gtk_widget_set_sensitive(context.statusItem, FALSE);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), context.statusItem);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), gtk_separator_menu_item_new());

#ifdef MWB_HAVE_GTK_GUI
    AddMenuItem(menu, "Open Dashboard", G_CALLBACK(OnOpenController), &context);
    AddMenuItem(menu, "Monitor Layout", G_CALLBACK(OnOpenMonitorLayout), &context);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), gtk_separator_menu_item_new());
#endif

    context.editSettingsItem         = AddMenuItem(menu, "Settings",            G_CALLBACK(OnEditSettings),          &context);
    context.editConnectionItem        = AddMenuItem(menu, "Connection Behavior", G_CALLBACK(OnEditConnectionBehavior),&context);
    context.healthCheckItem           = AddMenuItem(menu, "Health Check",        G_CALLBACK(OnHealthCheck),           &context);
    context.diagnosticsBundleItem     = AddMenuItem(menu, "Diagnostics Bundle",  G_CALLBACK(OnDiagnosticsBundle),     &context);
    context.connectionQualityItem     = AddMenuItem(menu, "Connection Quality",  G_CALLBACK(OnConnectionQuality),     &context);
    context.guidedPairingItem         = AddMenuItem(menu, "Guided Pairing",      G_CALLBACK(OnGuidedPairing),         &context);
    context.discoverPeersItem         = AddMenuItem(menu, "Discover Peers",      G_CALLBACK(OnDiscoverPeers),         &context);
    context.showPeersItem             = AddMenuItem(menu, "Known Peers",         G_CALLBACK(OnShowPeers),             &context);

    gtk_menu_shell_append(GTK_MENU_SHELL(menu), gtk_separator_menu_item_new());
    context.startItem   = AddMenuItem(menu, "Start Service",   G_CALLBACK(OnStartService),   &context);
    context.stopItem    = AddMenuItem(menu, "Stop Service",    G_CALLBACK(OnStopService),    &context);
    context.restartItem = AddMenuItem(menu, "Restart Service", G_CALLBACK(OnRestartService), &context);

    gtk_menu_shell_append(GTK_MENU_SHELL(menu), gtk_separator_menu_item_new());
    context.showStatusItem           = AddMenuItem(menu, "Show Service Details",    G_CALLBACK(OnShowStatus),            &context);
    context.installDesktopEntriesItem = AddMenuItem(menu, "Install Desktop Entries", G_CALLBACK(OnInstallDesktopEntries),&context);
    context.trayHelpItem              = AddMenuItem(menu, "Tray Visibility Help",    G_CALLBACK(OnShowTrayHelp),          &context);

    gtk_menu_shell_append(GTK_MENU_SHELL(menu), gtk_separator_menu_item_new());
    AddMenuItem(menu, "Lock Screen", G_CALLBACK(OnLockScreen), &context);
    AddMenuItem(menu, "Quit", G_CALLBACK(OnQuit), &context);
    gtk_widget_show_all(menu);

#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
#endif
    context.indicator = app_indicator_new(
        kIndicatorId,
        context.iconThemePath.empty() ? "network-idle-symbolic" : "inputflow-tray",
        APP_INDICATOR_CATEGORY_APPLICATION_STATUS);
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif
    if (!context.iconThemePath.empty()) {
        app_indicator_set_icon_theme_path(context.indicator, context.iconThemePath.c_str());
        app_indicator_set_attention_icon_full(context.indicator, "inputflow-tray-attention", "InputFlow needs attention");
    } else {
        app_indicator_set_attention_icon_full(context.indicator, "dialog-warning-symbolic", "InputFlow needs attention");
    }
    app_indicator_set_title(context.indicator, kAppName);
    app_indicator_set_label(context.indicator, "IF", "IF");
    app_indicator_set_menu(context.indicator, GTK_MENU(menu));
    app_indicator_set_secondary_activate_target(context.indicator, context.editSettingsItem);

    UpdateIndicatorVisuals(&context, QueryServiceState());
    app_indicator_set_status(context.indicator, APP_INDICATOR_STATUS_ACTIVE);
    g_timeout_add_seconds(5, RefreshStatus, &context);
}

} // namespace

// ---- standalone mwb_tray entry point (no embedded runtime) -----------------

#ifndef MWB_HAVE_GTK_GUI
int main(int argc, char** argv) {
    const int instanceLockFd = AcquireSingleInstanceLock();
    if (instanceLockFd == -2) {
        return 0;
    }

    g_set_prgname(kIndicatorId);
    g_set_application_name(kAppName);

    gtk_init(&argc, &argv);

    TrayContext context;
    context.controllerPath = ResolveControllerPath();
    context.iconThemePath  = ResolveIconThemePath();

    BuildTrayMenu(context);
    MaybeShowStartupHint(context);

    gtk_main();
    if (instanceLockFd >= 0) {
        close(instanceLockFd);
    }
    return 0;
}
#endif  // !MWB_HAVE_GTK_GUI

// ---- embedded GUI + runtime entry point (mwb_client gui subcommand) --------

#ifdef MWB_HAVE_GTK_GUI
namespace mwb {

namespace {

struct StatusUpdate {
    TrayContext* tray;
    GuiMainWindow* win;
    std::string state;
    std::string detail;
};

gboolean ApplyStatusOnMainThread(gpointer data) {
    auto* upd = static_cast<StatusUpdate*>(data);
    if (upd->win)  upd->win->UpdateStatus(upd->state, upd->detail);
    if (upd->tray) UpdateIndicatorVisuals(upd->tray, upd->state);
    delete upd;
    return G_SOURCE_REMOVE;
}

void PostStatus(TrayContext* tray, GuiMainWindow* win,
                const std::string& state, const std::string& detail) {
    auto* upd = new StatusUpdate{tray, win, state, detail};
    g_idle_add(ApplyStatusOnMainThread, upd);
}

} // namespace

int RunTrayAndGui(const std::string& binary,
                  const std::vector<std::string>& args,
                  const AppConfig& config,
                  const std::string& configPath,
                  const std::string& statePath) {
    (void)binary;
    (void)args;
    const int instanceLockFd = AcquireSingleInstanceLock();
    if (instanceLockFd == -2) {
        std::cerr << "InputFlow GUI is already running." << std::endl;
        return 0;
    }

    g_set_prgname(kIndicatorId);
    g_set_application_name(kAppName);

    // GTK already initialised by caller via gtk_init_check

    // Shared stop flag for daemon thread
    std::atomic<bool> stopFlag{false};

    // Build GUI window
    GuiMainWindow* mainWin = CreateMainWindow(
        config, configPath,
        [](const AppConfig&) { /* settings saved; daemon will reload on restart */ });

    // Build tray
    TrayContext context;
    context.iconThemePath = ResolveIconThemePath();
    context.mainWindow    = mainWin;
    context.stopFlag      = &stopFlag;

    BuildTrayMenu(context);

    // Seed status
    PostStatus(&context, mainWin, QueryServiceState(), "");

    // Start daemon runtime in background thread
    AppConfig runtimeConfig = config;
    std::thread daemonThread([&]() {
        AppState state;
        if (std::filesystem::exists(statePath)) {
            std::string error;
            if (!LoadAppState(statePath, state, error)) {
                std::cerr << "WARN: Failed to load state " << statePath << ": " << error << std::endl;
            }
        }
        ClearConnectedPeers(state);
        (void)EnsureLocalMachineId(state);
        SaveStateOrLog(statePath, state);

        if (PowerToysCompatibilityEnabled(runtimeConfig.connectionMode)) {
            if (const auto recoveredHost = RecoverConfiguredHostFromKnownPeers(runtimeConfig, state);
                recoveredHost.has_value()) {
                runtimeConfig.host = *recoveredHost;
            }
        }

        std::mutex stateMutex;

        RuntimeOptions options;
        options.host                   = runtimeConfig.host;
        options.key                    = runtimeConfig.key;
        options.port                   = runtimeConfig.port;
        options.clipboardEnabled       = runtimeConfig.clipboardEnabled;
        options.clipboardSendEnabled   = runtimeConfig.clipboardSendEnabled;
        options.clipboardForcePoll     = runtimeConfig.clipboardForcePoll;
        options.clipboardPollMs        = runtimeConfig.clipboardPollMs;
        options.autoConnectEnabled     = runtimeConfig.autoConnectEnabled;
        options.reconnectInitialBackoffMs = runtimeConfig.reconnectInitialBackoffMs;
        options.reconnectMaxBackoffMs  = runtimeConfig.reconnectMaxBackoffMs;
        options.reconnectIdleRetryMs   = runtimeConfig.reconnectIdleRetryMs;
        options.screenWidth            = runtimeConfig.screenWidth;
        options.screenHeight           = runtimeConfig.screenHeight;
        options.mprisMediaKeysEnabled  = runtimeConfig.mprisMediaKeysEnabled;
        options.mprisPlayer            = runtimeConfig.mprisPlayer;
        options.localMachineId         = state.localMachineId;
        options.localMachineName       = runtimeConfig.machineName;
        options.latencyReport          = runtimeConfig.latencyReport;
        options.powerToysCompatibilityEnabled = PowerToysCompatibilityEnabled(runtimeConfig.connectionMode);
        options.inputFlowPeersEnabled  = InputFlowPeersEnabled(runtimeConfig.connectionMode);
        options.topologyRuntimeEnabled = runtimeConfig.topologyRuntimeEnabled;
        options.topologyFilePath       = runtimeConfig.topologyFile;
        options.androidCaptureBackend  = runtimeConfig.androidCaptureBackend;
        options.androidRelay.enabled           = options.inputFlowPeersEnabled && runtimeConfig.androidPeersEnabled;
        options.androidRelay.port              = runtimeConfig.androidRelayPort;
        options.androidRelay.secret            = runtimeConfig.androidRelaySecret;
        options.androidRelay.peerName          = runtimeConfig.androidPeerName;
        options.androidRelay.layoutEditorEnabled = runtimeConfig.androidLayoutEditorEnabled;
        options.androidRelay.androidDeviceWidth  = runtimeConfig.androidDeviceWidth;
        options.androidRelay.androidDeviceHeight = runtimeConfig.androidDeviceHeight;
        options.androidRelay.notificationSyncEnabled = runtimeConfig.notificationSyncEnabled;

        options.onSessionEstablished = [&](const std::string& host, int port,
                                           const std::string& remoteName, uint32_t remoteMachineId,
                                           uint32_t localMachineId) {
            {
                std::lock_guard<std::mutex> lock(stateMutex);
                MarkSessionEstablished(
                    state, host, port, remoteName, remoteMachineId, localMachineId, CurrentEpochSeconds());
                SaveStateOrLog(statePath, state);
            }
            PostStatus(&context, mainWin, "active", host + ":" + std::to_string(port) + " (" + remoteName + ")");
            if (mainWin) {
                auto* logMsg = new std::string("Connected to " + host + ":" + std::to_string(port));
                g_idle_add([](gpointer p) -> gboolean {
                    auto* msg = static_cast<std::string*>(p);
                    // mainWin captured by pointer — safe since GTK window outlives runtime
                    delete msg;
                    return G_SOURCE_REMOVE;
                }, logMsg);
            }
        };
        options.onSessionDisconnected = [&]() {
            {
                std::lock_guard<std::mutex> lock(stateMutex);
                MarkSessionDisconnected(state);
                SaveStateOrLog(statePath, state);
            }
            PostStatus(&context, mainWin, "inactive", "Disconnected");
            if (runtimeConfig.lockOnDisconnect) {
                std::cout << "[SECURITY] Controlling peer disconnected; locking local session." << std::endl;
                LockLocalSession();
            }
        };

        // Self-healing reconnect: when the peer stops responding at its
        // last-known address, the network layer asks this resolver for a fresh
        // address. We re-run name-based LAN discovery against the ORIGINAL
        // configured host (config, not runtimeConfig — the latter was already
        // overwritten with the startup-resolved IP), so a peer that changed its
        // DHCP lease is rediscovered without restarting the daemon.
        if (PowerToysCompatibilityEnabled(runtimeConfig.connectionMode)) {
            const AppConfig resolverConfig = config;
            options.expectedRemoteMachineId =
                FindExpectedRemoteMachineId(state, resolverConfig.host, resolverConfig.port);
            options.resolveHost = [resolverConfig, &state, &stateMutex]() -> PeerHostResolution {
                std::lock_guard<std::mutex> lock(stateMutex);
                PeerRecoveryPlan plan = BuildPeerRecoveryPlan(resolverConfig, state);
                return PeerHostResolution{plan.expectedRemoteMachineId, std::move(plan.candidateHosts)};
            };
        }

        ClientRuntime runtime(std::move(options));

        // Stopper thread: when GTK quits it sets stopFlag → we call runtime.Stop()
        std::thread stopper([&]() {
            while (!stopFlag.load()) {
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
            runtime.Stop();
        });

        (void)runtime.Run();

        stopFlag.store(true);
        if (stopper.joinable()) stopper.join();
    });

    gtk_main();

    stopFlag.store(true);
    if (daemonThread.joinable()) {
        daemonThread.join();
    }
    if (instanceLockFd >= 0) {
        close(instanceLockFd);
    }
    delete mainWin;
    return 0;
}

} // namespace mwb
#endif  // MWB_HAVE_GTK_GUI
