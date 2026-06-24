#pragma once

#include <gtk/gtk.h>
#include <string>
#include <vector>

namespace mwb {

struct AppConfig;

struct LayoutMachine {
    std::string id;
    std::string label;
    bool isLocal{false};
    // position in canvas pixels (mutable, drag target)
    double x{0}, y{0};
    double w{160}, h{100};
};

struct MonitorLayoutWidget {
    GtkWidget* widget{nullptr};  // GtkDrawingArea
    std::vector<LayoutMachine> machines;
    std::string configPath;
    std::string topologyPath;
    int dragIndex{-1};
    double dragOffX{0}, dragOffY{0};
};

// Creates a MonitorLayoutWidget populated from cfg.topologyFile (if set).
// Returns heap-allocated; caller owns it (kept alive by GtkWidget lifetime via
// g_object_set_data).
MonitorLayoutWidget* CreateMonitorLayoutWidget(const AppConfig& cfg, const std::string& configPath);

// Callbacks wired to GTK buttons (signature matches GCallback).
void MonitorLayoutWidgetApply(GtkButton* btn, gpointer data);
void MonitorLayoutWidgetReset(GtkButton* btn, gpointer data);

} // namespace mwb
