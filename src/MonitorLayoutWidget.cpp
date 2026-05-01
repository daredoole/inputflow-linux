#include "MonitorLayoutWidget.h"
#include "AppConfig.h"
#include "TopologyModel.h"

#include <cmath>
#include <cstdio>
#include <fstream>
#include <sstream>

namespace mwb {

namespace {

constexpr double kGridStep   = 48.0;
constexpr double kPad        = 8.0;
constexpr double kRadius     = 6.0;
constexpr double kLocalR     = 0.10, kLocalG  = 0.40, kLocalB  = 0.75;  // blue
constexpr double kRemoteR    = 0.23, kRemoteG = 0.30, kRemoteB = 0.38;  // slate

void RoundedRect(cairo_t* cr, double x, double y, double w, double h, double r) {
    cairo_new_sub_path(cr);
    cairo_arc(cr, x + w - r, y + r,     r,  -G_PI / 2,  0);
    cairo_arc(cr, x + w - r, y + h - r, r,   0,          G_PI / 2);
    cairo_arc(cr, x + r,     y + h - r, r,   G_PI / 2,   G_PI);
    cairo_arc(cr, x + r,     y + r,     r,   G_PI,        3 * G_PI / 2);
    cairo_close_path(cr);
}

gboolean OnDraw(GtkWidget* widget, cairo_t* cr, gpointer data) {
    auto* mlw = static_cast<MonitorLayoutWidget*>(data);
    int W = gtk_widget_get_allocated_width(widget);
    int H = gtk_widget_get_allocated_height(widget);

    // Background
    cairo_set_source_rgb(cr, 0.039, 0.086, 0.157);  // #0A1628
    cairo_paint(cr);

    // Grid lines
    cairo_set_source_rgba(cr, 0.102, 0.157, 0.251, 0.6);  // #1A2840
    cairo_set_line_width(cr, 1.0);
    for (double x = 0; x < W; x += kGridStep) {
        cairo_move_to(cr, x, 0); cairo_line_to(cr, x, H);
    }
    for (double y = 0; y < H; y += kGridStep) {
        cairo_move_to(cr, 0, y); cairo_line_to(cr, W, y);
    }
    cairo_stroke(cr);

    // Machines
    for (const auto& m : mlw->machines) {
        if (m.isLocal) {
            cairo_set_source_rgb(cr, kLocalR, kLocalG, kLocalB);
        } else {
            cairo_set_source_rgb(cr, kRemoteR, kRemoteG, kRemoteB);
        }
        RoundedRect(cr, m.x, m.y, m.w, m.h, kRadius);
        cairo_fill(cr);

        // Border
        cairo_set_source_rgba(cr, 1, 1, 1, 0.15);
        cairo_set_line_width(cr, 1.0);
        RoundedRect(cr, m.x, m.y, m.w, m.h, kRadius);
        cairo_stroke(cr);

        // Label
        cairo_set_source_rgb(cr, 1, 1, 1);
        cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);
        cairo_set_font_size(cr, 12.0);
        cairo_text_extents_t te;
        cairo_text_extents(cr, m.label.c_str(), &te);
        cairo_move_to(cr,
            m.x + (m.w - te.width) / 2 - te.x_bearing,
            m.y + m.h / 2 - te.height / 2 - te.y_bearing);
        cairo_show_text(cr, m.label.c_str());

        if (m.isLocal) {
            cairo_set_font_size(cr, 9.0);
            const char* badge = "THIS DEVICE";
            cairo_text_extents(cr, badge, &te);
            cairo_move_to(cr,
                m.x + (m.w - te.width) / 2 - te.x_bearing,
                m.y + m.h / 2 + 14);
            cairo_set_source_rgba(cr, 1, 1, 1, 0.6);
            cairo_show_text(cr, badge);
        }
    }

    return FALSE;
}

int HitTest(const MonitorLayoutWidget* mlw, double px, double py) {
    for (int i = static_cast<int>(mlw->machines.size()) - 1; i >= 0; --i) {
        const auto& m = mlw->machines[i];
        if (px >= m.x && px <= m.x + m.w && py >= m.y && py <= m.y + m.h) {
            return i;
        }
    }
    return -1;
}

gboolean OnButtonPress(GtkWidget*, GdkEventButton* ev, gpointer data) {
    auto* mlw = static_cast<MonitorLayoutWidget*>(data);
    if (ev->button != 1) return FALSE;
    int idx = HitTest(mlw, ev->x, ev->y);
    if (idx < 0) return FALSE;
    mlw->dragIndex  = idx;
    mlw->dragOffX   = ev->x - mlw->machines[idx].x;
    mlw->dragOffY   = ev->y - mlw->machines[idx].y;
    return TRUE;
}

gboolean OnMotion(GtkWidget* widget, GdkEventMotion* ev, gpointer data) {
    auto* mlw = static_cast<MonitorLayoutWidget*>(data);
    if (mlw->dragIndex < 0) return FALSE;
    auto& m = mlw->machines[mlw->dragIndex];
    m.x = ev->x - mlw->dragOffX;
    m.y = ev->y - mlw->dragOffY;
    gtk_widget_queue_draw(widget);
    return TRUE;
}

gboolean OnButtonRelease(GtkWidget* widget, GdkEventButton* ev, gpointer data) {
    auto* mlw = static_cast<MonitorLayoutWidget*>(data);
    if (ev->button != 1 || mlw->dragIndex < 0) return FALSE;
    // Snap to grid
    auto& m = mlw->machines[mlw->dragIndex];
    m.x = std::round(m.x / kGridStep) * kGridStep;
    m.y = std::round(m.y / kGridStep) * kGridStep;
    mlw->dragIndex = -1;
    gtk_widget_queue_draw(widget);
    return TRUE;
}

void PopulateFromConfig(MonitorLayoutWidget* mlw, const AppConfig& cfg) {
    mlw->machines.clear();

    TopologyModel topo;
    if (!cfg.topologyFile.empty()) {
        std::string err;
        (void)LoadTopologyConfig(cfg.topologyFile, topo, &err);
    }

    const auto& machines = topo.machines();
    const auto& displays = topo.displays();

    if (machines.empty()) {
        // No topology — add two placeholder machines
        LayoutMachine local;
        local.id      = "linux";
        local.label   = cfg.machineName.empty() ? "This PC" : cfg.machineName;
        local.isLocal = true;
        local.x = kGridStep; local.y = kGridStep;
        local.w = 192; local.h = 120;
        mlw->machines.push_back(local);

        if (cfg.androidPeersEnabled) {
            LayoutMachine android;
            android.id    = "android";
            android.label = cfg.androidPeerName.empty() ? "Android" : cfg.androidPeerName;
            android.x = kGridStep + 192 + kGridStep;
            android.y = kGridStep;
            android.w = 128; android.h = 120;
            mlw->machines.push_back(android);
        }
        return;
    }

    // Map machine ID → first display for position
    double offsetX = kGridStep;
    for (const auto& mach : machines) {
        LayoutMachine lm;
        lm.id    = mach.id;
        lm.label = mach.id;

        // Sum display widths/heights for bounding box
        int maxW = 0, maxH = 0;
        int baseX = 0, baseY = 0;
        bool first = true;
        for (const auto& disp : displays) {
            if (disp.machineId != mach.id) continue;
            if (first) { baseX = disp.x; baseY = disp.y; first = false; }
            maxW = std::max(maxW, disp.x - baseX + disp.width);
            maxH = std::max(maxH, disp.y - baseY + disp.height);
        }

        const double scale = 0.1;
        lm.w = std::max(96.0, maxW * scale);
        lm.h = std::max(60.0, maxH * scale);
        lm.x = offsetX;
        lm.y = kGridStep;
        lm.isLocal = (mach.id == "linux" || (!cfg.machineName.empty() && mach.id == cfg.machineName));
        offsetX += lm.w + kGridStep;
        mlw->machines.push_back(lm);
    }
}

// Build a minimal topology INI string from current machine positions
std::string BuildTopologyIni(const MonitorLayoutWidget* mlw, const AppConfig& cfg) {
    std::ostringstream oss;
    oss << "[wrap]\nmode=none\n\n";

    for (const auto& m : mlw->machines) {
        oss << "[machine]\nid=" << m.id << "\n\n";
    }

    // Compute pixel coordinates (scale canvas back to screen space)
    // Use a nominal 1920 width for the local machine
    const double scale = 10.0;  // 0.1 scale used in PopulateFromConfig
    for (const auto& m : mlw->machines) {
        const int px = static_cast<int>(m.x * scale);
        const int py = static_cast<int>(m.y * scale);
        const int pw = m.isLocal ? 1920 : cfg.androidDeviceWidth;
        const int ph = m.isLocal ? 1080 : cfg.androidDeviceHeight;
        oss << "[display]\n"
            << "id=" << m.id << "-1\n"
            << "machine=" << m.id << "\n"
            << "x=" << px << "\ny=" << py << "\n"
            << "width=" << pw << "\nheight=" << ph << "\n\n";
    }

    // Add links between adjacent machines (sharing an edge within tolerance)
    const double tol = kGridStep * 2;
    for (std::size_t i = 0; i < mlw->machines.size(); ++i) {
        for (std::size_t j = i + 1; j < mlw->machines.size(); ++j) {
            const auto& a = mlw->machines[i];
            const auto& b = mlw->machines[j];
            // b is to the right of a
            if (std::abs((a.x + a.w) - b.x) < tol &&
                std::abs(a.y - b.y) < tol) {
                oss << "[link]\nsource=" << a.id << "-1\nexit=right\n"
                    << "target=" << b.id << "-1\nentry=left\n\n";
                oss << "[link]\nsource=" << b.id << "-1\nexit=left\n"
                    << "target=" << a.id << "-1\nentry=right\n\n";
            }
            // b is to the left of a
            if (std::abs((b.x + b.w) - a.x) < tol &&
                std::abs(a.y - b.y) < tol) {
                oss << "[link]\nsource=" << b.id << "-1\nexit=right\n"
                    << "target=" << a.id << "-1\nentry=left\n\n";
                oss << "[link]\nsource=" << a.id << "-1\nexit=left\n"
                    << "target=" << b.id << "-1\nentry=right\n\n";
            }
            // b is below a
            if (std::abs((a.y + a.h) - b.y) < tol &&
                std::abs(a.x - b.x) < tol) {
                oss << "[link]\nsource=" << a.id << "-1\nexit=down\n"
                    << "target=" << b.id << "-1\nentry=up\n\n";
                oss << "[link]\nsource=" << b.id << "-1\nexit=up\n"
                    << "target=" << a.id << "-1\nentry=down\n\n";
            }
        }
    }

    return oss.str();
}

} // namespace

MonitorLayoutWidget* CreateMonitorLayoutWidget(const AppConfig& cfg) {
    auto* mlw = new MonitorLayoutWidget();
    mlw->configPath = cfg.topologyFile;

    PopulateFromConfig(mlw, cfg);

    mlw->widget = gtk_drawing_area_new();
    gtk_widget_add_events(mlw->widget,
        GDK_BUTTON_PRESS_MASK | GDK_BUTTON_RELEASE_MASK | GDK_POINTER_MOTION_MASK);

    g_signal_connect(mlw->widget, "draw",                 G_CALLBACK(OnDraw),          mlw);
    g_signal_connect(mlw->widget, "button-press-event",   G_CALLBACK(OnButtonPress),   mlw);
    g_signal_connect(mlw->widget, "button-release-event", G_CALLBACK(OnButtonRelease), mlw);
    g_signal_connect(mlw->widget, "motion-notify-event",  G_CALLBACK(OnMotion),        mlw);

    // Attach mlw lifetime to widget so it's freed when widget is destroyed
    g_object_set_data_full(G_OBJECT(mlw->widget), "mlw", mlw,
        [](gpointer p) { delete static_cast<MonitorLayoutWidget*>(p); });

    return mlw;
}

void MonitorLayoutWidgetApply(GtkButton*, gpointer data) {
    auto* mlw = static_cast<MonitorLayoutWidget*>(data);
    if (mlw->configPath.empty()) return;

    // Load current config to preserve other settings
    AppConfig cfg;
    std::string err;
    (void)LoadAppConfig(mlw->configPath, cfg, &err);

    const std::string ini = BuildTopologyIni(mlw, cfg);

    // Write to topology file (use config path parent / topology.ini if not set)
    std::string topoPath = cfg.topologyFile;
    if (topoPath.empty()) {
        topoPath = std::filesystem::path(mlw->configPath).parent_path() / "topology.ini";
        cfg.topologyFile = topoPath;
        (void)WriteAppConfig(mlw->configPath, cfg, &err);
    }

    std::ofstream f(topoPath);
    if (f) f << ini;
}

void MonitorLayoutWidgetReset(GtkButton*, gpointer data) {
    auto* mlw = static_cast<MonitorLayoutWidget*>(data);
    AppConfig cfg;
    std::string err;
    (void)LoadAppConfig(mlw->configPath, cfg, &err);
    PopulateFromConfig(mlw, cfg);
    gtk_widget_queue_draw(mlw->widget);
}

} // namespace mwb
