#include "GuiMainWindow.h"
#include "AppConfig.h"
#include "MonitorLayoutWidget.h"

#include <cstdio>
#include <cstring>
#include <string>

namespace mwb {

namespace {

constexpr const char* kServiceName = "mwb-client.service";
constexpr const char* kSystemctlPath = "/usr/bin/systemctl";

// ---- colour helpers --------------------------------------------------------

struct Rgb { double r, g, b; };

Rgb StateColor(const std::string& state) {
    if (state == "active")    return {0.30, 0.80, 0.40};  // green
    if (state == "activating" || state == "reloading") return {1.0, 0.65, 0.0}; // amber
    if (state == "failed")    return {0.90, 0.25, 0.20};  // red
    if (state == "inactive")  return {0.55, 0.55, 0.55};  // grey
    return {0.40, 0.40, 0.40};
}

// ---- status dot drawing ----------------------------------------------------

gboolean OnDotDraw(GtkWidget* widget, cairo_t* cr, gpointer data) {
    auto* win = static_cast<GuiMainWindow*>(data);
    int w = gtk_widget_get_allocated_width(widget);
    int h = gtk_widget_get_allocated_height(widget);
    auto [r, g, b] = StateColor(win->connectionState);
    cairo_arc(cr, w / 2.0, h / 2.0, w / 2.0 - 1, 0, 2 * G_PI);
    cairo_set_source_rgb(cr, r, g, b);
    cairo_fill(cr);
    return FALSE;
}

// ---- service control -------------------------------------------------------

void RunServiceAction(const std::string& action) {
    if (access(kSystemctlPath, X_OK) != 0) return;
    std::string cmd = std::string(kSystemctlPath) + " --user " + action + " " + kServiceName + " &";
    (void)std::system(cmd.c_str());
}

void OnStartService(GtkButton*, gpointer)   { RunServiceAction("start");   }
void OnStopService(GtkButton*, gpointer)    { RunServiceAction("stop");    }
void OnRestartService(GtkButton*, gpointer) { RunServiceAction("restart"); }

// ---- settings save ---------------------------------------------------------

void OnSaveSettings(GtkButton*, gpointer data) {
    auto* win = static_cast<GuiMainWindow*>(data);

    AppConfig cfg;
    cfg.host         = gtk_entry_get_text(GTK_ENTRY(win->hostEntry));
    cfg.port         = static_cast<int>(gtk_spin_button_get_value(GTK_SPIN_BUTTON(win->portSpin)));
    cfg.machineName  = gtk_entry_get_text(GTK_ENTRY(win->nameEntry));
    cfg.key          = gtk_entry_get_text(GTK_ENTRY(win->keyEntry));
    cfg.autoConnectEnabled = gtk_switch_get_active(GTK_SWITCH(win->autoConnectSwitch));
    cfg.clipboardEnabled   = gtk_switch_get_active(GTK_SWITCH(win->clipboardSwitch));
    cfg.mprisMediaKeysEnabled = gtk_switch_get_active(GTK_SWITCH(win->mprisSwitch));
    cfg.mprisPlayer    = gtk_entry_get_text(GTK_ENTRY(win->mprisPlayerEntry));
    cfg.latencyReport  = gtk_switch_get_active(GTK_SWITCH(win->latencySwitch));
    cfg.topologyRuntimeEnabled = gtk_switch_get_active(GTK_SWITCH(win->topologySwitch));

    // Android
    cfg.androidPeersEnabled = gtk_switch_get_active(GTK_SWITCH(win->androidSwitch));
    cfg.androidRelayPort    = static_cast<int>(gtk_spin_button_get_value(GTK_SPIN_BUTTON(win->androidPortSpin)));
    cfg.androidRelaySecret  = gtk_entry_get_text(GTK_ENTRY(win->androidSecretEntry));
    cfg.androidPeerName     = gtk_entry_get_text(GTK_ENTRY(win->androidNameEntry));
    cfg.androidDeviceWidth  = static_cast<int>(gtk_spin_button_get_value(GTK_SPIN_BUTTON(win->androidWidthSpin)));
    cfg.androidDeviceHeight = static_cast<int>(gtk_spin_button_get_value(GTK_SPIN_BUTTON(win->androidHeightSpin)));
    const gchar* backendText = gtk_combo_box_text_get_active_text(GTK_COMBO_BOX_TEXT(win->androidBackendCombo));
    if (backendText) cfg.androidCaptureBackend = backendText;

    std::string err;
    WriteAppConfig(win->configPath, cfg, &err);

    if (win->onSettingsSaved) win->onSettingsSaved(cfg);

    // Unlock monitor tab if topology just enabled
    if (cfg.topologyRuntimeEnabled && win->layoutStack) {
        gtk_stack_set_visible_child_name(GTK_STACK(win->layoutStack), "canvas");
    }
}

// ---- topology enable button ------------------------------------------------

void OnEnableTopology(GtkButton*, gpointer data) {
    auto* win = static_cast<GuiMainWindow*>(data);
    if (win->topologySwitch) {
        gtk_switch_set_active(GTK_SWITCH(win->topologySwitch), TRUE);
    }
    OnSaveSettings(nullptr, win);
}

// ---- layout helpers --------------------------------------------------------

GtkWidget* MakeLabel(const char* text, bool secondary = false) {
    GtkWidget* lbl = gtk_label_new(text);
    gtk_label_set_xalign(GTK_LABEL(lbl), 0.0f);
    if (secondary) {
        gtk_style_context_add_class(gtk_widget_get_style_context(lbl), "dim-label");
    }
    return lbl;
}

GtkWidget* MakeSectionHeader(const char* text) {
    GtkWidget* lbl = gtk_label_new(nullptr);
    char* markup = g_markup_printf_escaped("<b>%s</b>", text);
    gtk_label_set_markup(GTK_LABEL(lbl), markup);
    g_free(markup);
    gtk_label_set_xalign(GTK_LABEL(lbl), 0.0f);
    gtk_widget_set_margin_top(lbl, 12);
    gtk_widget_set_margin_bottom(lbl, 4);
    return lbl;
}

void GridAttach(GtkWidget* grid, GtkWidget* label, GtkWidget* widget, int row) {
    gtk_widget_set_hexpand(widget, TRUE);
    gtk_grid_attach(GTK_GRID(grid), label,  0, row, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), widget, 1, row, 1, 1);
}

// ---- build tabs ------------------------------------------------------------

GtkWidget* BuildStatusTab(GuiMainWindow* win) {
    GtkWidget* box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_container_set_border_width(GTK_CONTAINER(box), 16);

    // Dot + state row
    GtkWidget* stateRow = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    win->dotArea = gtk_drawing_area_new();
    gtk_widget_set_size_request(win->dotArea, 18, 18);
    g_signal_connect(win->dotArea, "draw", G_CALLBACK(OnDotDraw), win);
    gtk_box_pack_start(GTK_BOX(stateRow), win->dotArea, FALSE, FALSE, 0);
    win->stateLabel = gtk_label_new("Checking...");
    gtk_box_pack_start(GTK_BOX(stateRow), win->stateLabel, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), stateRow, FALSE, FALSE, 0);

    win->detailLabel = gtk_label_new("");
    gtk_label_set_xalign(GTK_LABEL(win->detailLabel), 0.0f);
    gtk_style_context_add_class(gtk_widget_get_style_context(win->detailLabel), "dim-label");
    gtk_box_pack_start(GTK_BOX(box), win->detailLabel, FALSE, FALSE, 0);

    // Buttons
    GtkWidget* btnRow = gtk_button_box_new(GTK_ORIENTATION_HORIZONTAL);
    gtk_button_box_set_layout(GTK_BUTTON_BOX(btnRow), GTK_BUTTONBOX_START);
    gtk_box_set_spacing(GTK_BOX(btnRow), 6);

    GtkWidget* btnStart   = gtk_button_new_with_label("Start");
    GtkWidget* btnStop    = gtk_button_new_with_label("Stop");
    GtkWidget* btnRestart = gtk_button_new_with_label("Restart");
    g_signal_connect(btnStart,   "clicked", G_CALLBACK(OnStartService),   win);
    g_signal_connect(btnStop,    "clicked", G_CALLBACK(OnStopService),    win);
    g_signal_connect(btnRestart, "clicked", G_CALLBACK(OnRestartService), win);
    gtk_container_add(GTK_CONTAINER(btnRow), btnStart);
    gtk_container_add(GTK_CONTAINER(btnRow), btnStop);
    gtk_container_add(GTK_CONTAINER(btnRow), btnRestart);
    gtk_box_pack_start(GTK_BOX(box), btnRow, FALSE, FALSE, 4);

    // Log
    gtk_box_pack_start(GTK_BOX(box), MakeLabel("Recent events:"), FALSE, FALSE, 0);
    win->logBuf = gtk_text_buffer_new(nullptr);
    win->logView = gtk_text_view_new_with_buffer(win->logBuf);
    gtk_text_view_set_editable(GTK_TEXT_VIEW(win->logView), FALSE);
    gtk_text_view_set_monospace(GTK_TEXT_VIEW(win->logView), TRUE);
    gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(win->logView), GTK_WRAP_CHAR);
    GtkWidget* scroll = gtk_scrolled_window_new(nullptr, nullptr);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll),
                                   GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
    gtk_widget_set_size_request(scroll, -1, 150);
    gtk_container_add(GTK_CONTAINER(scroll), win->logView);
    gtk_box_pack_start(GTK_BOX(box), scroll, TRUE, TRUE, 0);

    return box;
}

GtkWidget* BuildSettingsTab(GuiMainWindow* win, const AppConfig& cfg) {
    GtkWidget* outerScroll = gtk_scrolled_window_new(nullptr, nullptr);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(outerScroll),
                                   GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    GtkWidget* box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    gtk_container_set_border_width(GTK_CONTAINER(box), 16);
    gtk_container_add(GTK_CONTAINER(outerScroll), box);

    int row = 0;
    GtkWidget* grid = gtk_grid_new();
    gtk_grid_set_column_spacing(GTK_GRID(grid), 12);
    gtk_grid_set_row_spacing(GTK_GRID(grid), 6);

    // Section: Connection
    gtk_box_pack_start(GTK_BOX(box), MakeSectionHeader("Connection"), FALSE, FALSE, 0);

    win->hostEntry = gtk_entry_new();
    gtk_entry_set_text(GTK_ENTRY(win->hostEntry), cfg.host.c_str());
    gtk_entry_set_placeholder_text(GTK_ENTRY(win->hostEntry), "hostname or IP");
    GridAttach(grid, MakeLabel("Host"), win->hostEntry, row++);

    win->portSpin = gtk_spin_button_new_with_range(1, 65535, 1);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(win->portSpin), cfg.port);
    GridAttach(grid, MakeLabel("Port"), win->portSpin, row++);

    win->nameEntry = gtk_entry_new();
    gtk_entry_set_text(GTK_ENTRY(win->nameEntry), cfg.machineName.c_str());
    gtk_entry_set_placeholder_text(GTK_ENTRY(win->nameEntry), "this machine's name");
    GridAttach(grid, MakeLabel("Machine name"), win->nameEntry, row++);

    win->keyEntry = gtk_entry_new();
    gtk_entry_set_text(GTK_ENTRY(win->keyEntry), cfg.key.c_str());
    gtk_entry_set_visibility(GTK_ENTRY(win->keyEntry), FALSE);
    gtk_entry_set_placeholder_text(GTK_ENTRY(win->keyEntry), "security key");
    GridAttach(grid, MakeLabel("Key"), win->keyEntry, row++);

    gtk_box_pack_start(GTK_BOX(box), grid, FALSE, FALSE, 0);

    // Section: Behaviour
    GtkWidget* grid2 = gtk_grid_new();
    gtk_grid_set_column_spacing(GTK_GRID(grid2), 12);
    gtk_grid_set_row_spacing(GTK_GRID(grid2), 6);
    int r2 = 0;

    gtk_box_pack_start(GTK_BOX(box), MakeSectionHeader("Behaviour"), FALSE, FALSE, 0);

    win->autoConnectSwitch = gtk_switch_new();
    gtk_switch_set_active(GTK_SWITCH(win->autoConnectSwitch), cfg.autoConnectEnabled);
    GridAttach(grid2, MakeLabel("Auto-connect"), win->autoConnectSwitch, r2++);

    win->clipboardSwitch = gtk_switch_new();
    gtk_switch_set_active(GTK_SWITCH(win->clipboardSwitch), cfg.clipboardEnabled);
    GridAttach(grid2, MakeLabel("Clipboard sync"), win->clipboardSwitch, r2++);

    win->mprisSwitch = gtk_switch_new();
    gtk_switch_set_active(GTK_SWITCH(win->mprisSwitch), cfg.mprisMediaKeysEnabled);
    GridAttach(grid2, MakeLabel("Media keys (MPRIS)"), win->mprisSwitch, r2++);

    win->mprisPlayerEntry = gtk_entry_new();
    gtk_entry_set_text(GTK_ENTRY(win->mprisPlayerEntry), cfg.mprisPlayer.c_str());
    gtk_entry_set_placeholder_text(GTK_ENTRY(win->mprisPlayerEntry), "e.g. spotify (blank = any)");
    GridAttach(grid2, MakeLabel("MPRIS player"), win->mprisPlayerEntry, r2++);

    win->latencySwitch = gtk_switch_new();
    gtk_switch_set_active(GTK_SWITCH(win->latencySwitch), cfg.latencyReport);
    GridAttach(grid2, MakeLabel("Latency report"), win->latencySwitch, r2++);

    gtk_box_pack_start(GTK_BOX(box), grid2, FALSE, FALSE, 0);

    // Section: Topology
    GtkWidget* grid3 = gtk_grid_new();
    gtk_grid_set_column_spacing(GTK_GRID(grid3), 12);
    gtk_grid_set_row_spacing(GTK_GRID(grid3), 6);

    gtk_box_pack_start(GTK_BOX(box), MakeSectionHeader("Monitor Layout"), FALSE, FALSE, 0);

    win->topologySwitch = gtk_switch_new();
    gtk_switch_set_active(GTK_SWITCH(win->topologySwitch), cfg.topologyRuntimeEnabled);
    GridAttach(grid3, MakeLabel("Enable topology mode"), win->topologySwitch, 0);

    GtkWidget* topoHint = MakeLabel("Required for the monitor configurator and cross-device edge transitions.", true);
    gtk_label_set_line_wrap(GTK_LABEL(topoHint), TRUE);
    gtk_grid_attach(GTK_GRID(grid3), topoHint, 0, 1, 2, 1);

    gtk_box_pack_start(GTK_BOX(box), grid3, FALSE, FALSE, 0);

    // Save button
    gtk_box_pack_start(GTK_BOX(box), gtk_separator_new(GTK_ORIENTATION_HORIZONTAL), FALSE, FALSE, 8);
    GtkWidget* saveBtn = gtk_button_new_with_label("Save Settings");
    gtk_style_context_add_class(gtk_widget_get_style_context(saveBtn), "suggested-action");
    gtk_widget_set_halign(saveBtn, GTK_ALIGN_START);
    g_signal_connect(saveBtn, "clicked", G_CALLBACK(OnSaveSettings), win);
    gtk_box_pack_start(GTK_BOX(box), saveBtn, FALSE, FALSE, 0);

    return outerScroll;
}

GtkWidget* BuildMonitorTab(GuiMainWindow* win, const AppConfig& cfg) {
    GtkWidget* outerBox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);

    win->layoutStack = gtk_stack_new();

    // Locked page
    GtkWidget* lockedBox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
    gtk_widget_set_valign(lockedBox, GTK_ALIGN_CENTER);
    gtk_widget_set_halign(lockedBox, GTK_ALIGN_CENTER);

    GtkWidget* lockLabel = gtk_label_new(nullptr);
    gtk_label_set_markup(GTK_LABEL(lockLabel),
        "<span size='xx-large'>🔒</span>\n"
        "<b>Monitor configurator is locked</b>\n"
        "<small>Enable topology mode in Settings to arrange displays\nand configure edge transitions between machines.</small>");
    gtk_label_set_justify(GTK_LABEL(lockLabel), GTK_JUSTIFY_CENTER);
    gtk_label_set_line_wrap(GTK_LABEL(lockLabel), TRUE);
    gtk_box_pack_start(GTK_BOX(lockedBox), lockLabel, FALSE, FALSE, 0);

    GtkWidget* enableBtn = gtk_button_new_with_label("Enable Topology Mode");
    gtk_widget_set_halign(enableBtn, GTK_ALIGN_CENTER);
    g_signal_connect(enableBtn, "clicked", G_CALLBACK(OnEnableTopology), win);
    gtk_box_pack_start(GTK_BOX(lockedBox), enableBtn, FALSE, FALSE, 0);

    gtk_stack_add_named(GTK_STACK(win->layoutStack), lockedBox, "locked");

    // Canvas page
    GtkWidget* canvasBox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    MonitorLayoutWidget* mlw = CreateMonitorLayoutWidget(cfg);
    win->layoutCanvas = mlw->widget;
    gtk_widget_set_size_request(win->layoutCanvas, -1, 320);
    gtk_box_pack_start(GTK_BOX(canvasBox), win->layoutCanvas, TRUE, TRUE, 0);

    GtkWidget* btnBar = gtk_button_box_new(GTK_ORIENTATION_HORIZONTAL);
    gtk_button_box_set_layout(GTK_BUTTON_BOX(btnBar), GTK_BUTTONBOX_END);
    gtk_box_set_spacing(GTK_BOX(btnBar), 6);
    gtk_container_set_border_width(GTK_CONTAINER(btnBar), 8);

    GtkWidget* resetBtn = gtk_button_new_with_label("Reset");
    g_signal_connect(resetBtn, "clicked", G_CALLBACK(MonitorLayoutWidgetReset), mlw);
    GtkWidget* applyBtn = gtk_button_new_with_label("Apply Layout");
    gtk_style_context_add_class(gtk_widget_get_style_context(applyBtn), "suggested-action");
    g_signal_connect(applyBtn, "clicked", G_CALLBACK(MonitorLayoutWidgetApply), mlw);

    gtk_container_add(GTK_CONTAINER(btnBar), resetBtn);
    gtk_container_add(GTK_CONTAINER(btnBar), applyBtn);
    gtk_box_pack_start(GTK_BOX(canvasBox), btnBar, FALSE, FALSE, 0);

    gtk_stack_add_named(GTK_STACK(win->layoutStack), canvasBox, "canvas");

    gtk_stack_set_visible_child_name(GTK_STACK(win->layoutStack),
        cfg.topologyRuntimeEnabled ? "canvas" : "locked");

    gtk_box_pack_start(GTK_BOX(outerBox), win->layoutStack, TRUE, TRUE, 0);
    return outerBox;
}

GtkWidget* BuildAndroidTab(GuiMainWindow* win, const AppConfig& cfg) {
    GtkWidget* outerScroll = gtk_scrolled_window_new(nullptr, nullptr);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(outerScroll),
                                   GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    GtkWidget* box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    gtk_container_set_border_width(GTK_CONTAINER(box), 16);
    gtk_container_add(GTK_CONTAINER(outerScroll), box);

    gtk_box_pack_start(GTK_BOX(box), MakeSectionHeader("Android Relay"), FALSE, FALSE, 0);

    GtkWidget* grid = gtk_grid_new();
    gtk_grid_set_column_spacing(GTK_GRID(grid), 12);
    gtk_grid_set_row_spacing(GTK_GRID(grid), 6);
    int row = 0;

    win->androidSwitch = gtk_switch_new();
    gtk_switch_set_active(GTK_SWITCH(win->androidSwitch), cfg.androidPeersEnabled);
    GridAttach(grid, MakeLabel("Enable Android peers"), win->androidSwitch, row++);

    win->androidPortSpin = gtk_spin_button_new_with_range(1, 65535, 1);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(win->androidPortSpin), cfg.androidRelayPort);
    GridAttach(grid, MakeLabel("Relay port"), win->androidPortSpin, row++);

    win->androidSecretEntry = gtk_entry_new();
    gtk_entry_set_text(GTK_ENTRY(win->androidSecretEntry), cfg.androidRelaySecret.c_str());
    gtk_entry_set_visibility(GTK_ENTRY(win->androidSecretEntry), FALSE);
    gtk_entry_set_placeholder_text(GTK_ENTRY(win->androidSecretEntry), "shared secret");
    GridAttach(grid, MakeLabel("Secret"), win->androidSecretEntry, row++);

    win->androidNameEntry = gtk_entry_new();
    gtk_entry_set_text(GTK_ENTRY(win->androidNameEntry), cfg.androidPeerName.c_str());
    gtk_entry_set_placeholder_text(GTK_ENTRY(win->androidNameEntry), "android");
    GridAttach(grid, MakeLabel("Peer name"), win->androidNameEntry, row++);

    win->androidBackendCombo = gtk_combo_box_text_new();
    gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(win->androidBackendCombo), "none",  "none");
    gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(win->androidBackendCombo), "libei", "libei");
    gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(win->androidBackendCombo), "local", "local");
    gtk_combo_box_set_active_id(GTK_COMBO_BOX(win->androidBackendCombo),
                                cfg.androidCaptureBackend.empty() ? "none" : cfg.androidCaptureBackend.c_str());
    GridAttach(grid, MakeLabel("Capture backend"), win->androidBackendCombo, row++);

    gtk_box_pack_start(GTK_BOX(box), MakeSectionHeader("Device Dimensions"), FALSE, FALSE, 0);

    win->androidWidthSpin  = gtk_spin_button_new_with_range(0, 7680, 1);
    win->androidHeightSpin = gtk_spin_button_new_with_range(0, 4320, 1);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(win->androidWidthSpin),  cfg.androidDeviceWidth);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(win->androidHeightSpin), cfg.androidDeviceHeight);
    GridAttach(grid, MakeLabel("Device width"),  win->androidWidthSpin,  row++);
    GridAttach(grid, MakeLabel("Device height"), win->androidHeightSpin, row++);

    gtk_box_pack_start(GTK_BOX(box), grid, FALSE, FALSE, 0);

    gtk_box_pack_start(GTK_BOX(box), gtk_separator_new(GTK_ORIENTATION_HORIZONTAL), FALSE, FALSE, 8);
    GtkWidget* saveBtn = gtk_button_new_with_label("Save Settings");
    gtk_style_context_add_class(gtk_widget_get_style_context(saveBtn), "suggested-action");
    gtk_widget_set_halign(saveBtn, GTK_ALIGN_START);
    g_signal_connect(saveBtn, "clicked", G_CALLBACK(OnSaveSettings), win);
    gtk_box_pack_start(GTK_BOX(box), saveBtn, FALSE, FALSE, 0);

    return outerScroll;
}

} // namespace

// ---- public API ------------------------------------------------------------

GuiMainWindow* CreateMainWindow(const AppConfig& config,
                                const std::string& cfgPath,
                                OnSettingsSaved onSettingsSaved) {
    auto* win = new GuiMainWindow();
    win->configPath = cfgPath;
    win->onSettingsSaved = std::move(onSettingsSaved);

    win->window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(win->window), "InputFlow");
    gtk_window_set_default_size(GTK_WINDOW(win->window), 560, 500);
    gtk_window_set_resizable(GTK_WINDOW(win->window), TRUE);

    g_signal_connect(win->window, "delete-event",
        G_CALLBACK(+[](GtkWidget* w, GdkEvent*, gpointer) -> gboolean {
            gtk_widget_hide(w);
            return TRUE;  // don't destroy, just hide
        }), nullptr);

    win->notebook = gtk_notebook_new();
    gtk_container_add(GTK_CONTAINER(win->window), win->notebook);

    gtk_notebook_append_page(GTK_NOTEBOOK(win->notebook),
        BuildStatusTab(win), gtk_label_new("Status"));
    gtk_notebook_append_page(GTK_NOTEBOOK(win->notebook),
        BuildSettingsTab(win, config), gtk_label_new("Settings"));
    gtk_notebook_append_page(GTK_NOTEBOOK(win->notebook),
        BuildMonitorTab(win, config), gtk_label_new("Monitor Layout"));
    gtk_notebook_append_page(GTK_NOTEBOOK(win->notebook),
        BuildAndroidTab(win, config), gtk_label_new("Android"));

    gtk_widget_show_all(win->window);
    gtk_widget_hide(win->window);  // start hidden; shown from tray
    return win;
}

void GuiMainWindow::UpdateStatus(const std::string& state, const std::string& detail) {
    connectionState  = state;
    connectionDetail = detail;

    if (stateLabel) {
        std::string text;
        if (state == "active")      text = "Running";
        else if (state == "activating") text = "Connecting…";
        else if (state == "inactive")   text = "Stopped";
        else if (state == "failed")     text = "Error";
        else                            text = state.empty() ? "Unknown" : state;
        gtk_label_set_text(GTK_LABEL(stateLabel), text.c_str());
    }
    if (detailLabel && !detail.empty()) {
        gtk_label_set_text(GTK_LABEL(detailLabel), detail.c_str());
    }
    if (dotArea) {
        gtk_widget_queue_draw(dotArea);
    }
}

void GuiMainWindow::ShowTab(int index) {
    gtk_widget_show(window);
    gtk_window_present(GTK_WINDOW(window));
    gtk_notebook_set_current_page(GTK_NOTEBOOK(notebook), index);
}

void GuiMainWindow::AppendLog(const std::string& line) {
    if (!logBuf) return;
    GtkTextIter end;
    gtk_text_buffer_get_end_iter(logBuf, &end);
    gtk_text_buffer_insert(logBuf, &end, (line + "\n").c_str(), -1);
    // keep last 200 lines
    int lineCount = gtk_text_buffer_get_line_count(logBuf);
    if (lineCount > 200) {
        GtkTextIter start, cut;
        gtk_text_buffer_get_start_iter(logBuf, &start);
        gtk_text_buffer_get_iter_at_line(logBuf, &cut, lineCount - 200);
        gtk_text_buffer_delete(logBuf, &start, &cut);
    }
    // scroll to bottom
    GtkTextIter endIter;
    gtk_text_buffer_get_end_iter(logBuf, &endIter);
    gtk_text_view_scroll_to_iter(GTK_TEXT_VIEW(logView), &endIter, 0.0, FALSE, 0.0, 1.0);
}

} // namespace mwb
