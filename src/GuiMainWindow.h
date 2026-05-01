#pragma once

#include <gtk/gtk.h>
#include <functional>
#include <string>

namespace mwb {

struct AppConfig;

// Callback invoked on the GTK main thread when the user saves settings.
using OnSettingsSaved = std::function<void(const AppConfig&)>;

struct GuiMainWindow {
    GtkWidget* window{nullptr};
    // Tabs
    GtkWidget* notebook{nullptr};

    // Status tab widgets
    GtkWidget* dotArea{nullptr};      // GtkDrawingArea for connection dot
    GtkWidget* stateLabel{nullptr};
    GtkWidget* detailLabel{nullptr};
    GtkWidget* logView{nullptr};
    GtkTextBuffer* logBuf{nullptr};

    // Settings tab — connection
    GtkWidget* hostEntry{nullptr};
    GtkWidget* portSpin{nullptr};
    GtkWidget* nameEntry{nullptr};
    GtkWidget* keyEntry{nullptr};

    // Settings tab — behavior
    GtkWidget* autoConnectSwitch{nullptr};
    GtkWidget* clipboardSwitch{nullptr};
    GtkWidget* mprisSwitch{nullptr};
    GtkWidget* mprisPlayerEntry{nullptr};
    GtkWidget* latencySwitch{nullptr};

    // Settings tab — topology toggle
    GtkWidget* topologySwitch{nullptr};

    // Monitor layout tab
    GtkWidget* layoutStack{nullptr};    // GtkStack: locked vs. canvas
    GtkWidget* layoutCanvas{nullptr};   // GtkDrawingArea (MonitorLayoutWidget)

    // Android tab
    GtkWidget* androidSwitch{nullptr};
    GtkWidget* androidPortSpin{nullptr};
    GtkWidget* androidSecretEntry{nullptr};
    GtkWidget* androidNameEntry{nullptr};
    GtkWidget* androidBackendCombo{nullptr};
    GtkWidget* androidWidthSpin{nullptr};
    GtkWidget* androidHeightSpin{nullptr};

    // State
    std::string connectionState;   // "active", "inactive", "failed", etc.
    std::string connectionDetail;
    std::string configPath;
    OnSettingsSaved onSettingsSaved;

    void UpdateStatus(const std::string& state, const std::string& detail);
    void ShowTab(int index);
    void AppendLog(const std::string& line);
};

GuiMainWindow* CreateMainWindow(const AppConfig& config,
                                const std::string& configPath,
                                OnSettingsSaved onSettingsSaved);

} // namespace mwb
