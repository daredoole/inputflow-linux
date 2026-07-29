#include "GuiMainWindow.h"
#include "AppConfig.h"
#include "AppState.h"
#include "Discovery.h"
#include "MonitorLayoutWidget.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <string>

namespace mwb {

namespace {

constexpr const char* kServiceName = "mwb-client.service";
constexpr const char* kSystemctlPath = "/usr/bin/systemctl";
constexpr const char* kInputFlowCss = R"CSS(
.inputflow-window {
  background-color: #f3f8f7;
  color: #12252c;
}
.inputflow-window notebook > header {
  background-color: #e4f0ed;
  border-bottom: 1px solid #bcd7d2;
}
.inputflow-window notebook > header tab {
  padding: 10px 14px;
  color: #355158;
}
.inputflow-window notebook > header tab:checked {
  color: #0b6f6b;
  border-bottom: 3px solid #0b7f7a;
}
.inputflow-hero {
  background-color: #092f35;
  border: 1px solid #15545a;
  border-radius: 14px;
  padding: 18px;
}
.inputflow-hero label {
  color: #f4fbf9;
}
.inputflow-eyebrow {
  color: #72d4c6;
  font-size: 10px;
  font-weight: 700;
  letter-spacing: 1.5px;
}
.inputflow-status-title {
  font-size: 22px;
  font-weight: 700;
}
.inputflow-status-detail {
  color: #b9d9d4;
}
.inputflow-section-title {
  color: #0b6f6b;
  font-size: 14px;
  font-weight: 700;
}
.inputflow-panel {
  background-color: #ffffff;
  border: 1px solid #bcd7d2;
  border-radius: 10px;
}
.inputflow-window button {
  border-radius: 8px;
  padding: 7px 12px;
}
.inputflow-window button.suggested-action {
  background-color: #0b7f7a;
  color: #ffffff;
}
.inputflow-window button.destructive-action {
  color: #a73540;
}
.inputflow-window textview {
  background-color: #102a31;
  color: #d9eeea;
  font-family: monospace;
}
)CSS";

void InstallDesignSystem(GtkWidget* window) {
    GtkCssProvider* provider = gtk_css_provider_new();
    GError* error = nullptr;
    if (!gtk_css_provider_load_from_data(provider, kInputFlowCss, -1, &error)) {
        if (error != nullptr) {
            std::fprintf(stderr, "InputFlow UI theme could not be loaded: %s\n", error->message);
            g_error_free(error);
        }
        g_object_unref(provider);
        return;
    }
    gtk_style_context_add_provider_for_screen(
        gtk_widget_get_screen(window),
        GTK_STYLE_PROVIDER(provider),
        GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    gtk_style_context_add_class(gtk_widget_get_style_context(window), "inputflow-window");
    g_object_unref(provider);
}

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
    const gchar* argv[] = {
        kSystemctlPath,
        "--user",
        action.c_str(),
        kServiceName,
        nullptr,
    };
    GError* error = nullptr;
    if (!g_spawn_async(
            nullptr,
            const_cast<gchar**>(argv),
            nullptr,
            static_cast<GSpawnFlags>(G_SPAWN_STDOUT_TO_DEV_NULL | G_SPAWN_STDERR_TO_DEV_NULL),
            nullptr,
            nullptr,
            nullptr,
            &error)) {
        if (error != nullptr) {
            std::fprintf(stderr, "InputFlow service action failed: %s\n", error->message);
            g_error_free(error);
        }
    }
}

void OnStartService(GtkButton*, gpointer)   { RunServiceAction("start");   }
void OnStopService(GtkButton*, gpointer)    { RunServiceAction("stop");    }
void OnRestartService(GtkButton*, gpointer) { RunServiceAction("restart"); }

bool IsServiceActive() {
    if (access(kSystemctlPath, X_OK) != 0) {
        return false;
    }
    const gchar* argv[] = {
        kSystemctlPath,
        "--user",
        "is-active",
        "--quiet",
        kServiceName,
        nullptr,
    };
    gint waitStatus = -1;
    GError* error = nullptr;
    const gboolean launched = g_spawn_sync(
        nullptr,
        const_cast<gchar**>(argv),
        nullptr,
        static_cast<GSpawnFlags>(G_SPAWN_STDOUT_TO_DEV_NULL | G_SPAWN_STDERR_TO_DEV_NULL),
        nullptr,
        nullptr,
        nullptr,
        nullptr,
        &waitStatus,
        &error);
    if (error != nullptr) {
        g_error_free(error);
    }
    return launched && g_spawn_check_wait_status(waitStatus, nullptr);
}

// After saving settings, a running daemon keeps its old config until restarted.
// Offer to apply immediately so users are never silently editing a no-op.
void PromptApplyRestart(GuiMainWindow* win) {
    if (!IsServiceActive()) {
        return;
    }
    GtkWindow* parent = (win && win->window) ? GTK_WINDOW(win->window) : nullptr;
    GtkWidget* dlg = gtk_message_dialog_new(
        parent,
        GTK_DIALOG_MODAL,
        GTK_MESSAGE_QUESTION,
        GTK_BUTTONS_NONE,
        "Settings saved. Restart the InputFlow service now to apply them?");
    gtk_dialog_add_button(GTK_DIALOG(dlg), "Later", GTK_RESPONSE_CANCEL);
    GtkWidget* restartBtn = gtk_dialog_add_button(GTK_DIALOG(dlg), "Restart now", GTK_RESPONSE_ACCEPT);
    gtk_style_context_add_class(gtk_widget_get_style_context(restartBtn), "suggested-action");
    const gint resp = gtk_dialog_run(GTK_DIALOG(dlg));
    gtk_widget_destroy(dlg);
    if (resp == GTK_RESPONSE_ACCEPT) {
        RunServiceAction("restart");
    }
}

void ShowSaveError(GuiMainWindow* win, const std::string& err) {
    GtkWindow* parent = (win && win->window) ? GTK_WINDOW(win->window) : nullptr;
    GtkWidget* dlg = gtk_message_dialog_new(
        parent,
        GTK_DIALOG_MODAL,
        GTK_MESSAGE_ERROR,
        GTK_BUTTONS_CLOSE,
        "Failed to save settings: %s",
        err.empty() ? "unknown error" : err.c_str());
    gtk_dialog_run(GTK_DIALOG(dlg));
    gtk_widget_destroy(dlg);
}

ConnectionMode ActiveConnectionMode(GuiMainWindow* win) {
    if (!win->connectionModeCombo) {
        return ConnectionMode::PowerToys;
    }
    const gchar* activeId = gtk_combo_box_get_active_id(GTK_COMBO_BOX(win->connectionModeCombo));
    if (activeId == nullptr) {
        return ConnectionMode::PowerToys;
    }
    return ParseConnectionMode(activeId).value_or(ConnectionMode::PowerToys);
}

void ApplyModeSensitivity(GuiMainWindow* win) {
    const ConnectionMode mode = ActiveConnectionMode(win);
    const gboolean powerToysEnabled = PowerToysCompatibilityEnabled(mode);
    const gboolean inputFlowEnabled = InputFlowPeersEnabled(mode);

    GtkWidget* powerToysWidgets[] = {
        win->hostEntry,
        win->portSpin,
        win->keyEntry,
        win->autoConnectSwitch,
        win->clipboardSwitch,
        win->mprisSwitch,
        win->mprisPlayerEntry,
    };
    for (GtkWidget* widget : powerToysWidgets) {
        if (widget) {
            gtk_widget_set_sensitive(widget, powerToysEnabled);
        }
    }

    GtkWidget* inputFlowWidgets[] = {
        win->androidSwitch,
        win->androidPortSpin,
        win->androidSecretEntry,
        win->androidNameEntry,
        win->androidBackendCombo,
        win->androidWidthSpin,
        win->androidHeightSpin,
    };
    for (GtkWidget* widget : inputFlowWidgets) {
        if (widget) {
            gtk_widget_set_sensitive(widget, inputFlowEnabled);
        }
    }
}

void OnConnectionModeChanged(GtkComboBox*, gpointer data) {
    ApplyModeSensitivity(static_cast<GuiMainWindow*>(data));
}

// ---- discovered-peer picker ------------------------------------------------

void OnPickDiscoveredPeer(GtkButton*, gpointer data) {
    auto* win = static_cast<GuiMainWindow*>(data);
    const int port = win->portSpin
                         ? static_cast<int>(gtk_spin_button_get_value(GTK_SPIN_BUTTON(win->portSpin)))
                         : 15101;

    DiscoveryOptions opts;
    opts.port = static_cast<uint16_t>(port);
    opts.connectTimeoutMs = 300;
    opts.maxHostsPerSubnet = 256;
    const auto candidates = DiscoverLanCandidates(opts);

    GtkWindow* parent = (win && win->window) ? GTK_WINDOW(win->window) : nullptr;
    GtkWidget* dlg = gtk_dialog_new_with_buttons(
        "Discovered peers", parent, GTK_DIALOG_MODAL,
        "Cancel", GTK_RESPONSE_CANCEL, nullptr);
    gtk_window_set_default_size(GTK_WINDOW(dlg), 380, 300);
    GtkWidget* content = gtk_dialog_get_content_area(GTK_DIALOG(dlg));

    GtkWidget* hint = gtk_label_new("Selecting a named peer is recommended — it lets InputFlow\nfollow the peer automatically if its IP changes.");
    gtk_widget_set_halign(hint, GTK_ALIGN_START);
    gtk_box_pack_start(GTK_BOX(content), hint, FALSE, FALSE, 6);

    GtkWidget* listbox = gtk_list_box_new();
    int openCount = 0;
    for (const auto& candidate : candidates) {
        if (candidate.status != DiscoveryStatus::Open) {
            continue;
        }
        ++openCount;
        const std::string display = candidate.hostName.empty()
                                        ? candidate.ipAddress
                                        : (candidate.hostName + "   (" + candidate.ipAddress + ")");
        // Prefer the resolved name so the saved host survives IP changes.
        const std::string chosen = candidate.hostName.empty() ? candidate.ipAddress : candidate.hostName;

        GtkWidget* row = gtk_list_box_row_new();
        GtkWidget* lbl = gtk_label_new(display.c_str());
        gtk_widget_set_halign(lbl, GTK_ALIGN_START);
        gtk_container_add(GTK_CONTAINER(row), lbl);
        g_object_set_data_full(G_OBJECT(row), "peer-host",
                               new std::string(chosen),
                               [](gpointer p) { delete static_cast<std::string*>(p); });
        gtk_list_box_insert(GTK_LIST_BOX(listbox), row, -1);
    }
    if (openCount == 0) {
        GtkWidget* empty = gtk_label_new("No peers found on the local network.");
        gtk_widget_set_halign(empty, GTK_ALIGN_START);
        gtk_list_box_insert(GTK_LIST_BOX(listbox), empty, -1);
    }

    g_signal_connect(listbox, "row-activated",
        G_CALLBACK(+[](GtkListBox*, GtkListBoxRow* row, gpointer d) {
            auto* w = static_cast<GuiMainWindow*>(d);
            auto* val = static_cast<std::string*>(g_object_get_data(G_OBJECT(row), "peer-host"));
            if (val && w->hostEntry) {
                gtk_entry_set_text(GTK_ENTRY(w->hostEntry), val->c_str());
            }
            GtkWidget* top = gtk_widget_get_toplevel(GTK_WIDGET(row));
            if (GTK_IS_DIALOG(top)) {
                gtk_dialog_response(GTK_DIALOG(top), GTK_RESPONSE_OK);
            }
        }), win);

    GtkWidget* scroll = gtk_scrolled_window_new(nullptr, nullptr);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_widget_set_vexpand(scroll, TRUE);
    gtk_container_add(GTK_CONTAINER(scroll), listbox);
    gtk_box_pack_start(GTK_BOX(content), scroll, TRUE, TRUE, 0);

    gtk_widget_show_all(dlg);
    gtk_dialog_run(GTK_DIALOG(dlg));
    gtk_widget_destroy(dlg);
}

// ---- settings save ---------------------------------------------------------

void OnSaveSettings(GtkButton*, gpointer data) {
    auto* win = static_cast<GuiMainWindow*>(data);

    AppConfig cfg;
    std::string err;
    (void)LoadAppConfig(win->configPath, cfg, &err);
    cfg.connectionMode = ActiveConnectionMode(win);
    cfg.host         = gtk_entry_get_text(GTK_ENTRY(win->hostEntry));
    cfg.port         = static_cast<int>(gtk_spin_button_get_value(GTK_SPIN_BUTTON(win->portSpin)));
    cfg.machineName  = gtk_entry_get_text(GTK_ENTRY(win->nameEntry));
    cfg.key          = gtk_entry_get_text(GTK_ENTRY(win->keyEntry));
    cfg.autoConnectEnabled = gtk_switch_get_active(GTK_SWITCH(win->autoConnectSwitch));
    cfg.lockOnDisconnect   = gtk_switch_get_active(GTK_SWITCH(win->lockOnDisconnectSwitch));
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
    const gchar* backendId = gtk_combo_box_get_active_id(GTK_COMBO_BOX(win->androidBackendCombo));
    if (backendId) cfg.androidCaptureBackend = backendId;

    const bool saved = WriteAppConfig(win->configPath, cfg, &err);
    if (!saved) {
        ShowSaveError(win, err);
        return;
    }

    if (win->onSettingsSaved) win->onSettingsSaved(cfg);

    // Unlock monitor tab if topology just enabled
    if (cfg.topologyRuntimeEnabled && win->layoutStack) {
        gtk_stack_set_visible_child_name(GTK_STACK(win->layoutStack), "canvas");
    }

    // Apply now rather than silently waiting for a manual restart.
    PromptApplyRestart(win);
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

std::string FormatRelativeAge(std::int64_t epochSeconds) {
    if (epochSeconds <= 0) {
        return "never";
    }
    const std::int64_t now = static_cast<std::int64_t>(std::time(nullptr));
    std::int64_t delta = now - epochSeconds;
    if (delta < 0) {
        delta = 0;
    }
    if (delta < 60) {
        return std::to_string(delta) + "s ago";
    }
    if (delta < 3600) {
        return std::to_string(delta / 60) + "m ago";
    }
    if (delta < 86400) {
        return std::to_string(delta / 3600) + "h ago";
    }
    return std::to_string(delta / 86400) + "d ago";
}

void RefreshPeersList(GuiMainWindow* win) {
    if (!win || !win->peersList) {
        return;
    }
    // Clear existing rows.
    GList* children = gtk_container_get_children(GTK_CONTAINER(win->peersList));
    for (GList* it = children; it != nullptr; it = it->next) {
        gtk_widget_destroy(GTK_WIDGET(it->data));
    }
    g_list_free(children);

    AppState state;
    std::string err;
    const auto statePath = DefaultStatePath();
    auto addRow = [&](const std::string& text) {
        GtkWidget* row = gtk_list_box_row_new();
        GtkWidget* lbl = gtk_label_new(text.c_str());
        gtk_widget_set_halign(lbl, GTK_ALIGN_START);
        gtk_container_add(GTK_CONTAINER(row), lbl);
        gtk_list_box_insert(GTK_LIST_BOX(win->peersList), row, -1);
    };

    if (!std::filesystem::exists(statePath) || !LoadAppState(statePath, state, err)) {
        addRow("No known peers yet.");
        gtk_widget_show_all(win->peersList);
        return;
    }
    if (state.peers.empty()) {
        addRow("No known peers yet.");
        gtk_widget_show_all(win->peersList);
        return;
    }

    for (const auto& peer : state.peers) {
        const char* dot = peer.connectedNow ? "🟢" : (peer.approved ? "⚪" : "🔴");
        const std::string name = peer.name.empty() ? "(unnamed)" : peer.name;
        std::string line = std::string(dot) + "  " + name + "   " + peer.host + ":" + std::to_string(peer.port);
        if (peer.connectedNow) {
            line += "   — connected";
        } else {
            line += "   — seen " + FormatRelativeAge(peer.lastSeenEpochSeconds);
        }
        if (!peer.approved) {
            line += "  (unapproved)";
        }
        addRow(line);
    }
    gtk_widget_show_all(win->peersList);
}

void OnRefreshPeers(GtkButton*, gpointer data) {
    RefreshPeersList(static_cast<GuiMainWindow*>(data));
}

GtkWidget* BuildStatusTab(GuiMainWindow* win) {
    GtkWidget* box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
    gtk_container_set_border_width(GTK_CONTAINER(box), 20);

    GtkWidget* hero = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_style_context_add_class(gtk_widget_get_style_context(hero), "inputflow-hero");
    GtkWidget* eyebrow = gtk_label_new("TRUSTED CONNECTION");
    gtk_label_set_xalign(GTK_LABEL(eyebrow), 0.0f);
    gtk_style_context_add_class(gtk_widget_get_style_context(eyebrow), "inputflow-eyebrow");
    gtk_box_pack_start(GTK_BOX(hero), eyebrow, FALSE, FALSE, 0);

    GtkWidget* stateRow = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    win->dotArea = gtk_drawing_area_new();
    gtk_widget_set_size_request(win->dotArea, 18, 18);
    atk_object_set_name(gtk_widget_get_accessible(win->dotArea), "Connection status");
    g_signal_connect(win->dotArea, "draw", G_CALLBACK(OnDotDraw), win);
    gtk_box_pack_start(GTK_BOX(stateRow), win->dotArea, FALSE, FALSE, 0);
    win->stateLabel = gtk_label_new("Checking...");
    gtk_style_context_add_class(gtk_widget_get_style_context(win->stateLabel), "inputflow-status-title");
    gtk_box_pack_start(GTK_BOX(stateRow), win->stateLabel, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(hero), stateRow, FALSE, FALSE, 0);

    win->detailLabel = gtk_label_new("");
    gtk_label_set_xalign(GTK_LABEL(win->detailLabel), 0.0f);
    gtk_label_set_line_wrap(GTK_LABEL(win->detailLabel), TRUE);
    gtk_style_context_add_class(gtk_widget_get_style_context(win->detailLabel), "inputflow-status-detail");
    gtk_box_pack_start(GTK_BOX(hero), win->detailLabel, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), hero, FALSE, FALSE, 0);

    // Buttons
    GtkWidget* btnRow = gtk_button_box_new(GTK_ORIENTATION_HORIZONTAL);
    gtk_button_box_set_layout(GTK_BUTTON_BOX(btnRow), GTK_BUTTONBOX_START);
    gtk_box_set_spacing(GTK_BOX(btnRow), 6);

    GtkWidget* btnStart   = gtk_button_new_with_label("Start");
    GtkWidget* btnStop    = gtk_button_new_with_label("Stop");
    GtkWidget* btnRestart = gtk_button_new_with_label("Restart");
    gtk_style_context_add_class(gtk_widget_get_style_context(btnStart), "suggested-action");
    gtk_style_context_add_class(gtk_widget_get_style_context(btnStop), "destructive-action");
    gtk_widget_set_tooltip_text(btnStart, "Start the InputFlow background service");
    gtk_widget_set_tooltip_text(btnStop, "Stop the InputFlow background service");
    gtk_widget_set_tooltip_text(btnRestart, "Reload settings by restarting the service");
    g_signal_connect(btnStart,   "clicked", G_CALLBACK(OnStartService),   win);
    g_signal_connect(btnStop,    "clicked", G_CALLBACK(OnStopService),    win);
    g_signal_connect(btnRestart, "clicked", G_CALLBACK(OnRestartService), win);
    gtk_container_add(GTK_CONTAINER(btnRow), btnStart);
    gtk_container_add(GTK_CONTAINER(btnRow), btnStop);
    gtk_container_add(GTK_CONTAINER(btnRow), btnRestart);
    gtk_box_pack_start(GTK_BOX(box), btnRow, FALSE, FALSE, 4);

    // Peers (live status from saved state: connected now / last seen)
    GtkWidget* peersHeaderRow = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    GtkWidget* peersHeader = MakeLabel("Connected peers");
    gtk_style_context_add_class(gtk_widget_get_style_context(peersHeader), "inputflow-section-title");
    gtk_box_pack_start(GTK_BOX(peersHeaderRow), peersHeader, FALSE, FALSE, 0);
    GtkWidget* refreshPeersBtn = gtk_button_new_with_label("Refresh");
    gtk_widget_set_halign(refreshPeersBtn, GTK_ALIGN_END);
    g_signal_connect(refreshPeersBtn, "clicked", G_CALLBACK(OnRefreshPeers), win);
    gtk_box_pack_end(GTK_BOX(peersHeaderRow), refreshPeersBtn, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), peersHeaderRow, FALSE, FALSE, 0);

    win->peersList = gtk_list_box_new();
    gtk_list_box_set_selection_mode(GTK_LIST_BOX(win->peersList), GTK_SELECTION_NONE);
    GtkWidget* peersScroll = gtk_scrolled_window_new(nullptr, nullptr);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(peersScroll),
                                   GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_widget_set_size_request(peersScroll, -1, 110);
    gtk_style_context_add_class(gtk_widget_get_style_context(peersScroll), "inputflow-panel");
    gtk_container_add(GTK_CONTAINER(peersScroll), win->peersList);
    gtk_box_pack_start(GTK_BOX(box), peersScroll, FALSE, FALSE, 0);
    RefreshPeersList(win);

    // Log
    GtkWidget* eventsHeader = MakeLabel("Recent events");
    gtk_style_context_add_class(gtk_widget_get_style_context(eventsHeader), "inputflow-section-title");
    gtk_box_pack_start(GTK_BOX(box), eventsHeader, FALSE, FALSE, 0);
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

    win->connectionModeCombo = gtk_combo_box_text_new();
    gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(win->connectionModeCombo), "powertoys", "PowerToys compatibility");
    gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(win->connectionModeCombo), "inputflow", "InputFlow peers");
    gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(win->connectionModeCombo), "hybrid", "Hybrid");
    const std::string activeMode{ConnectionModeName(cfg.connectionMode)};
    gtk_combo_box_set_active_id(GTK_COMBO_BOX(win->connectionModeCombo), activeMode.c_str());
    g_signal_connect(win->connectionModeCombo, "changed", G_CALLBACK(OnConnectionModeChanged), win);
    GridAttach(grid, MakeLabel("Mode"), win->connectionModeCombo, row++);

    win->hostEntry = gtk_entry_new();
    gtk_entry_set_text(GTK_ENTRY(win->hostEntry), cfg.host.c_str());
    gtk_entry_set_placeholder_text(GTK_ENTRY(win->hostEntry), "peer name (recommended) or IP");
    GtkWidget* hostRow = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_box_pack_start(GTK_BOX(hostRow), win->hostEntry, TRUE, TRUE, 0);
    GtkWidget* discoverBtn = gtk_button_new_with_label("Discover…");
    gtk_widget_set_tooltip_text(discoverBtn, "Scan the local network and pick a peer by name");
    g_signal_connect(discoverBtn, "clicked", G_CALLBACK(OnPickDiscoveredPeer), win);
    gtk_box_pack_start(GTK_BOX(hostRow), discoverBtn, FALSE, FALSE, 0);
    GridAttach(grid, MakeLabel("Host"), hostRow, row++);

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

    win->lockOnDisconnectSwitch = gtk_switch_new();
    gtk_switch_set_active(GTK_SWITCH(win->lockOnDisconnectSwitch), cfg.lockOnDisconnect);
    gtk_widget_set_tooltip_text(win->lockOnDisconnectSwitch,
                                "Lock this Linux session when the controlling peer disconnects");
    GridAttach(grid2, MakeLabel("Lock on disconnect"), win->lockOnDisconnectSwitch, r2++);

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
    MonitorLayoutWidget* mlw = CreateMonitorLayoutWidget(cfg, win->configPath);
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
    gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(win->androidBackendCombo), "evdev", "evdev");
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
    gtk_window_set_default_size(GTK_WINDOW(win->window), 720, 620);
    gtk_window_set_resizable(GTK_WINDOW(win->window), TRUE);
    InstallDesignSystem(win->window);
    atk_object_set_name(gtk_widget_get_accessible(win->window), "InputFlow controller");

    g_signal_connect(win->window, "delete-event",
        G_CALLBACK(+[](GtkWidget* w, GdkEvent*, gpointer) -> gboolean {
            gtk_widget_hide(w);
            return TRUE;  // don't destroy, just hide
        }), nullptr);

    win->notebook = gtk_notebook_new();
    gtk_container_add(GTK_CONTAINER(win->window), win->notebook);

    gtk_notebook_append_page(GTK_NOTEBOOK(win->notebook),
        BuildStatusTab(win), gtk_label_new("Connection"));
    gtk_notebook_append_page(GTK_NOTEBOOK(win->notebook),
        BuildSettingsTab(win, config), gtk_label_new("Settings"));
    gtk_notebook_append_page(GTK_NOTEBOOK(win->notebook),
        BuildMonitorTab(win, config), gtk_label_new("Monitor layout"));
    gtk_notebook_append_page(GTK_NOTEBOOK(win->notebook),
        BuildAndroidTab(win, config), gtk_label_new("Android"));

    ApplyModeSensitivity(win);

    gtk_widget_show_all(win->window);
    gtk_notebook_set_current_page(GTK_NOTEBOOK(win->notebook), 0);
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
        atk_object_set_name(gtk_widget_get_accessible(dotArea), text.c_str());
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
