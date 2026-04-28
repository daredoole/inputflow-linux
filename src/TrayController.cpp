#include <gtk/gtk.h>
#include <libayatana-appindicator/app-indicator.h>

#include <array>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <iostream>
#include <optional>
#include <string>
#include <sys/file.h>
#include <unistd.h>
#include <vector>

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
};

std::optional<std::string> RunCommandCapture(const std::string& command) {
    std::array<char, 512> buffer{};
    std::string output;

    FILE* pipe = popen(command.c_str(), "r");
    if (pipe == nullptr) {
        return std::nullopt;
    }

    while (fgets(buffer.data(), static_cast<int>(buffer.size()), pipe) != nullptr) {
        output.append(buffer.data());
    }

    const int rc = pclose(pipe);
    if (rc != 0 && output.empty()) {
        return std::nullopt;
    }

    while (!output.empty() && (output.back() == '\n' || output.back() == '\r')) {
        output.pop_back();
    }
    return output;
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

std::string ResolveControllerPath() {
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
    if (access(kSystemctlPath, X_OK) != 0) {
        return "unknown";
    }
    const auto result = RunCommandCapture(std::string(kSystemctlPath) + " --user is-active " + std::string(kServiceName) + " 2>/dev/null");
    return result.value_or("unknown");
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

void MaybeShowStartupHint(const TrayContext& context) {
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

    const bool controllerAvailable = !context->controllerPath.empty();
    gtk_widget_set_sensitive(context->editSettingsItem, controllerAvailable);
    gtk_widget_set_sensitive(context->editConnectionItem, controllerAvailable);
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

void RunServiceAction(TrayContext* context, const std::string& action, const std::string& optimisticState) {
    if (access(kSystemctlPath, X_OK) == 0 &&
        SpawnAsync({kSystemctlPath, "--user", action, kServiceName})) {
        UpdateIndicatorVisuals(context, optimisticState);
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

void OnOpenController(GtkMenuItem*, gpointer userData) {
    auto* context = static_cast<TrayContext*>(userData);
    (void)LaunchController(context);
}

void OnEditSettings(GtkMenuItem*, gpointer userData) {
    auto* context = static_cast<TrayContext*>(userData);
    (void)LaunchController(context, {"settings"});
}

void OnEditConnectionBehavior(GtkMenuItem*, gpointer userData) {
    auto* context = static_cast<TrayContext*>(userData);
    (void)LaunchController(context, {"connection"});
}

void OnDiscoverPeers(GtkMenuItem*, gpointer userData) {
    auto* context = static_cast<TrayContext*>(userData);
    (void)LaunchController(context, {"discover"});
}

void OnShowPeers(GtkMenuItem*, gpointer userData) {
    auto* context = static_cast<TrayContext*>(userData);
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
    (void)LaunchController(context, {"status"});
}

void OnShowTrayHelp(GtkMenuItem*, gpointer userData) {
    auto* context = static_cast<TrayContext*>(userData);
    ShowTrayVisibilityHelp(*context);
}

void OnInstallDesktopEntries(GtkMenuItem*, gpointer userData) {
    auto* context = static_cast<TrayContext*>(userData);
    (void)LaunchController(context, {"install-desktop-entry"});
}

void OnQuit(GtkMenuItem*, gpointer) {
    gtk_main_quit();
}

GtkWidget* AddMenuItem(GtkWidget* menu, const char* label, GCallback callback, gpointer userData) {
    GtkWidget* item = gtk_menu_item_new_with_label(label);
    g_signal_connect(item, "activate", callback, userData);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), item);
    return item;
}

} // namespace

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
    context.iconThemePath = ResolveIconThemePath();

    GtkWidget* menu = gtk_menu_new();
    context.statusItem = gtk_menu_item_new_with_label("Service: Checking...");
    gtk_widget_set_sensitive(context.statusItem, FALSE);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), context.statusItem);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), gtk_separator_menu_item_new());

    context.editSettingsItem = AddMenuItem(menu, "Settings", G_CALLBACK(OnEditSettings), &context);
    context.editConnectionItem = AddMenuItem(menu, "Connection Behavior", G_CALLBACK(OnEditConnectionBehavior), &context);
    context.discoverPeersItem = AddMenuItem(menu, "Discover Peers", G_CALLBACK(OnDiscoverPeers), &context);
    context.showPeersItem = AddMenuItem(menu, "Known Peers", G_CALLBACK(OnShowPeers), &context);

    gtk_menu_shell_append(GTK_MENU_SHELL(menu), gtk_separator_menu_item_new());
    context.startItem = AddMenuItem(menu, "Start Service", G_CALLBACK(OnStartService), &context);
    context.stopItem = AddMenuItem(menu, "Stop Service", G_CALLBACK(OnStopService), &context);
    context.restartItem = AddMenuItem(menu, "Restart Service", G_CALLBACK(OnRestartService), &context);

    gtk_menu_shell_append(GTK_MENU_SHELL(menu), gtk_separator_menu_item_new());

    context.showStatusItem = AddMenuItem(menu, "Show Service Details", G_CALLBACK(OnShowStatus), &context);
    context.installDesktopEntriesItem = AddMenuItem(menu, "Install Desktop Entries", G_CALLBACK(OnInstallDesktopEntries), &context);
    context.trayHelpItem = AddMenuItem(menu, "Tray Visibility Help", G_CALLBACK(OnShowTrayHelp), &context);

    gtk_menu_shell_append(GTK_MENU_SHELL(menu), gtk_separator_menu_item_new());
    AddMenuItem(menu, "Quit", G_CALLBACK(OnQuit), &context);
    gtk_widget_show_all(menu);

    context.indicator = app_indicator_new(
        kIndicatorId,
        context.iconThemePath.empty() ? "network-idle-symbolic" : "inputflow-tray",
        APP_INDICATOR_CATEGORY_APPLICATION_STATUS);
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
    g_timeout_add_seconds(30, RefreshStatus, &context);

    gtk_main();
    if (instanceLockFd >= 0) {
        close(instanceLockFd);
    }
    return 0;
}
