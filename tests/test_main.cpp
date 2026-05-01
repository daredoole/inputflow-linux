#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

#include "AndroidRelay.h"
#include "AppConfig.h"
#include "AppState.h"
#include "Discovery.h"
#include "PeerRecovery.h"
#include "ReconnectPolicy.h"
#include "ScreenGeometry.h"

namespace {

int g_failures = 0;

void Expect(bool condition, const std::string& message) {
    if (!condition) {
        ++g_failures;
        std::cerr << "FAIL: " << message << std::endl;
    }
}

void ExpectRenderedLine(const std::string& rendered,
                        const std::string& key,
                        const std::string& value,
                        const std::string& message) {
    Expect(rendered.find(key + "=" + value + "\n") != std::string::npos, message);
}

std::filesystem::path MakeTempPath(const std::string& name) {
    return std::filesystem::temp_directory_path() / name;
}

void TestAppConfigRoundTrip() {
    mwb::AppConfig config;
    config.host = "192.0.2.107";
    config.key = "secret";
    config.machineName = "fedora";
    config.port = 15101;
    config.autoConnectEnabled = false;
    config.reconnectInitialBackoffMs = 1500;
    config.reconnectMaxBackoffMs = 45000;
    config.reconnectIdleRetryMs = 120000;
    config.clipboardEnabled = false;
    config.clipboardSendEnabled = false;
    config.clipboardForcePoll = true;
    config.clipboardPollMs = 2500;
    config.screenWidth = 2560;
    config.screenHeight = 1600;
    config.mprisMediaKeysEnabled = false;
    config.mprisPlayer = "spotify";
    config.latencyReport = true;
    config.topologyRuntimeEnabled = true;
    config.topologyFile = "topology.conf";
    config.androidPeersEnabled = true;
    config.androidRelayPort = 15112;
    config.androidRelaySecret = "android-secret";
    config.androidPeerName = "pixel-8";
    config.androidCaptureBackend = "evdev";

    const std::filesystem::path path = MakeTempPath("mwb-config-test.ini");
    std::string error;
    Expect(mwb::SaveConfigFile(path, config, error), "SaveConfigFile should succeed");
    const std::string rendered = mwb::RenderAppConfig(config);
    ExpectRenderedLine(rendered, "key", "secret", "Rendered config should keep the inline key");
    ExpectRenderedLine(rendered, "key_file", "", "Rendered config should keep an empty key_file entry for inline keys");
    ExpectRenderedLine(rendered, "key_secret_id", "",
                       "Rendered config should keep an empty key_secret_id entry for inline keys");
    ExpectRenderedLine(rendered, "auto_connect_enabled", "false",
                       "Rendered config should keep auto_connect_enabled");
    ExpectRenderedLine(rendered, "reconnect_initial_backoff_ms", "1500",
                       "Rendered config should keep reconnect_initial_backoff_ms");
    ExpectRenderedLine(rendered, "reconnect_max_backoff_ms", "45000",
                       "Rendered config should keep reconnect_max_backoff_ms");
    ExpectRenderedLine(rendered, "reconnect_idle_retry_ms", "120000",
                       "Rendered config should keep reconnect_idle_retry_ms");
    ExpectRenderedLine(rendered, "mpris_media_keys_enabled", "false",
                       "Rendered config should keep mpris_media_keys_enabled");
    ExpectRenderedLine(rendered, "mpris_player", "spotify",
                       "Rendered config should keep mpris_player");
    ExpectRenderedLine(rendered, "latency_report", "true",
                       "Rendered config should keep latency_report");
    ExpectRenderedLine(rendered, "topology_enabled", "true",
                       "Rendered config should keep topology_enabled");
    ExpectRenderedLine(rendered, "topology_file", "topology.conf",
                       "Rendered config should keep topology_file");
    ExpectRenderedLine(rendered, "android_peers_enabled", "true",
                       "Rendered config should keep android_peers_enabled");
    ExpectRenderedLine(rendered, "android_relay_port", "15112",
                       "Rendered config should keep android_relay_port");
    ExpectRenderedLine(rendered, "android_relay_secret", "android-secret",
                       "Rendered config should keep android_relay_secret");
    ExpectRenderedLine(rendered, "android_peer_name", "pixel-8",
                       "Rendered config should keep android_peer_name");
    ExpectRenderedLine(rendered, "android_capture_backend", "evdev",
                       "Rendered config should keep android_capture_backend");

    mwb::AppConfig loaded;
    Expect(mwb::LoadConfigFile(path, loaded, error), "LoadConfigFile should succeed");
    Expect(loaded.host == config.host, "Config host round-trip");
    Expect(loaded.key == config.key, "Config key round-trip");
    Expect(loaded.keyFile.empty(), "Config key_file should stay empty for inline keys");
    const std::string loadedRendered = mwb::RenderAppConfig(loaded);
    ExpectRenderedLine(loadedRendered, "key", "secret", "Loaded config should keep the inline key");
    ExpectRenderedLine(loadedRendered, "key_file", "", "Loaded config should keep an empty key_file entry");
    ExpectRenderedLine(loadedRendered, "key_secret_id", "",
                       "Loaded config should keep an empty key_secret_id entry");
    ExpectRenderedLine(loadedRendered, "auto_connect_enabled", "false",
                       "Loaded config should keep auto_connect_enabled");
    ExpectRenderedLine(loadedRendered, "reconnect_initial_backoff_ms", "1500",
                       "Loaded config should keep reconnect_initial_backoff_ms");
    ExpectRenderedLine(loadedRendered, "reconnect_max_backoff_ms", "45000",
                       "Loaded config should keep reconnect_max_backoff_ms");
    ExpectRenderedLine(loadedRendered, "reconnect_idle_retry_ms", "120000",
                       "Loaded config should keep reconnect_idle_retry_ms");
    ExpectRenderedLine(loadedRendered, "mpris_media_keys_enabled", "false",
                       "Loaded config should keep mpris_media_keys_enabled");
    ExpectRenderedLine(loadedRendered, "mpris_player", "spotify",
                       "Loaded config should keep mpris_player");
    ExpectRenderedLine(loadedRendered, "latency_report", "true",
                       "Loaded config should keep latency_report");
    ExpectRenderedLine(loadedRendered, "topology_enabled", "true",
                       "Loaded config should keep topology_enabled");
    ExpectRenderedLine(loadedRendered, "topology_file", "topology.conf",
                       "Loaded config should keep topology_file");
    ExpectRenderedLine(loadedRendered, "android_peers_enabled", "true",
                       "Loaded config should keep android_peers_enabled");
    ExpectRenderedLine(loadedRendered, "android_relay_port", "15112",
                       "Loaded config should keep android_relay_port");
    ExpectRenderedLine(loadedRendered, "android_relay_secret", "android-secret",
                       "Loaded config should keep android_relay_secret");
    ExpectRenderedLine(loadedRendered, "android_peer_name", "pixel-8",
                       "Loaded config should keep android_peer_name");
    ExpectRenderedLine(loadedRendered, "android_capture_backend", "evdev",
                       "Loaded config should keep android_capture_backend");
    Expect(loaded.machineName == config.machineName, "Config machine_name round-trip");
    Expect(loaded.port == config.port, "Config port round-trip");
    Expect(loaded.autoConnectEnabled == config.autoConnectEnabled, "Config autoConnectEnabled round-trip");
    Expect(loaded.reconnectInitialBackoffMs == config.reconnectInitialBackoffMs,
           "Config reconnectInitialBackoffMs round-trip");
    Expect(loaded.reconnectMaxBackoffMs == config.reconnectMaxBackoffMs,
           "Config reconnectMaxBackoffMs round-trip");
    Expect(loaded.reconnectIdleRetryMs == config.reconnectIdleRetryMs,
           "Config reconnectIdleRetryMs round-trip");
    Expect(loaded.clipboardEnabled == config.clipboardEnabled, "Config clipboardEnabled round-trip");
    Expect(loaded.clipboardSendEnabled == config.clipboardSendEnabled, "Config clipboardSendEnabled round-trip");
    Expect(loaded.clipboardForcePoll == config.clipboardForcePoll, "Config clipboardForcePoll round-trip");
    Expect(loaded.clipboardPollMs == config.clipboardPollMs, "Config clipboardPollMs round-trip");
    Expect(loaded.screenWidth == config.screenWidth, "Config screenWidth round-trip");
    Expect(loaded.screenHeight == config.screenHeight, "Config screenHeight round-trip");
    Expect(loaded.mprisMediaKeysEnabled == config.mprisMediaKeysEnabled,
           "Config mprisMediaKeysEnabled round-trip");
    Expect(loaded.mprisPlayer == config.mprisPlayer, "Config mprisPlayer round-trip");
    Expect(loaded.latencyReport == config.latencyReport, "Config latencyReport round-trip");
    Expect(loaded.topologyRuntimeEnabled == config.topologyRuntimeEnabled, "Config topologyRuntimeEnabled round-trip");
    Expect(loaded.topologyFile == config.topologyFile, "Config topologyFile round-trip");
    Expect(loaded.androidPeersEnabled == config.androidPeersEnabled, "Config androidPeersEnabled round-trip");
    Expect(loaded.androidRelayPort == config.androidRelayPort, "Config androidRelayPort round-trip");
    Expect(loaded.androidRelaySecret == config.androidRelaySecret, "Config androidRelaySecret round-trip");
    Expect(loaded.androidPeerName == config.androidPeerName, "Config androidPeerName round-trip");
    Expect(loaded.androidCaptureBackend == config.androidCaptureBackend, "Config androidCaptureBackend round-trip");
    std::error_code ignore;
    std::filesystem::remove(path, ignore);
}

void TestAndroidRelayFrames() {
    const mwb::MouseData mouse{123, 456, -120, 0x020A};
    const std::string mouseFrame = mwb::BuildAndroidMouseFrame(mouse);
    Expect(mouseFrame.find("\"type\":\"mouse\"") != std::string::npos, "Android mouse frame should include type");
    Expect(mouseFrame.find("\"x\":123") != std::string::npos, "Android mouse frame should include x");
    Expect(mouseFrame.find("\"y\":456") != std::string::npos, "Android mouse frame should include y");
    Expect(mouseFrame.find("\"mouseData\":-120") != std::string::npos, "Android mouse frame should include mouseData");
    Expect(mouseFrame.find("\"wParam\":522") != std::string::npos, "Android mouse frame should include wParam");

    const mwb::KeyboardData keyboard{65, mwb::kLlkhfUp};
    const std::string keyboardFrame = mwb::BuildAndroidKeyboardFrame(keyboard);
    Expect(keyboardFrame.find("\"type\":\"keyboard\"") != std::string::npos, "Android keyboard frame should include type");
    Expect(keyboardFrame.find("\"vkCode\":65") != std::string::npos, "Android keyboard frame should include vkCode");
    Expect(keyboardFrame.find("\"flags\":128") != std::string::npos, "Android keyboard frame should include flags");
}

void TestAppConfigKeyFileRoundTrip() {
    mwb::AppConfig config;
    config.host = "192.0.2.108";
    config.keyFile = "secrets/mwb.key";
    config.machineName = "fedora";
    config.port = 15102;

    const std::filesystem::path path = MakeTempPath("mwb-config-key-file-test.ini");
    std::string error;
    Expect(mwb::SaveConfigFile(path, config, error), "SaveConfigFile should succeed for key_file config");

    const std::string rendered = mwb::RenderAppConfig(config);
    ExpectRenderedLine(rendered, "key", "", "Rendered config should keep an empty key entry when using key_file");
    ExpectRenderedLine(rendered, "key_file", "secrets/mwb.key", "Rendered config should include key_file");
    ExpectRenderedLine(rendered, "key_secret_id", "",
                       "Rendered config should keep an empty key_secret_id entry when using key_file");

    mwb::AppConfig loaded;
    Expect(mwb::LoadConfigFile(path, loaded, error), "LoadConfigFile should succeed for key_file config");
    Expect(loaded.host == config.host, "Config host round-trip with key_file");
    Expect(loaded.key.empty(), "Config key should remain empty when key_file is used");
    Expect(loaded.keyFile == config.keyFile, "Config key_file round-trip");
    const std::string loadedRendered = mwb::RenderAppConfig(loaded);
    ExpectRenderedLine(loadedRendered, "key", "", "Loaded config should keep an empty key entry for key_file");
    ExpectRenderedLine(loadedRendered, "key_file", "secrets/mwb.key", "Loaded config should keep key_file");
    ExpectRenderedLine(loadedRendered, "key_secret_id", "",
                       "Loaded config should keep an empty key_secret_id entry for key_file");
    Expect(loaded.machineName == config.machineName, "Config machine_name round-trip with key_file");
    Expect(loaded.port == config.port, "Config port round-trip with key_file");
    std::error_code ignore;
    std::filesystem::remove(path, ignore);
}

void TestAppConfigKeySecretIdRoundTrip() {
    mwb::AppConfig config;
    std::string error;
    const std::string text =
        "host=192.0.2.110\n"
        "key=\n"
        "key_file=\n"
        "key_secret_id=org.freedesktop.secrets/mwb-linux\n"
        "machine_name=fedora\n"
        "port=15103\n";

    Expect(mwb::ParseAppConfig(text, config, &error), "ParseAppConfig should accept key_secret_id");

    const std::filesystem::path path = MakeTempPath("mwb-config-key-secret-id-test.ini");
    Expect(mwb::SaveConfigFile(path, config, error), "SaveConfigFile should succeed for key_secret_id config");

    const std::string rendered = mwb::RenderAppConfig(config);
    ExpectRenderedLine(rendered, "key", "", "Rendered config should keep an empty key entry when using key_secret_id");
    ExpectRenderedLine(rendered, "key_file", "",
                       "Rendered config should keep an empty key_file entry when using key_secret_id");
    ExpectRenderedLine(rendered, "key_secret_id", "org.freedesktop.secrets/mwb-linux",
                       "Rendered config should include key_secret_id");

    mwb::AppConfig loaded;
    Expect(mwb::LoadConfigFile(path, loaded, error), "LoadConfigFile should succeed for key_secret_id config");
    Expect(loaded.host == config.host, "Config host round-trip with key_secret_id");
    Expect(loaded.key.empty(), "Config key should remain empty when key_secret_id is used");
    Expect(loaded.keyFile.empty(), "Config key_file should remain empty when key_secret_id is used");
    const std::string loadedRendered = mwb::RenderAppConfig(loaded);
    ExpectRenderedLine(loadedRendered, "key", "", "Loaded config should keep an empty key entry for key_secret_id");
    ExpectRenderedLine(loadedRendered, "key_file", "",
                       "Loaded config should keep an empty key_file entry for key_secret_id");
    ExpectRenderedLine(loadedRendered, "key_secret_id", "org.freedesktop.secrets/mwb-linux",
                       "Loaded config should keep key_secret_id");
    Expect(loaded.machineName == config.machineName, "Config machine_name round-trip with key_secret_id");
    Expect(loaded.port == config.port, "Config port round-trip with key_secret_id");
    std::error_code ignore;
    std::filesystem::remove(path, ignore);
}

void TestAppConfigConnectionPolicyRoundTrip() {
    mwb::AppConfig config;
    std::string error;
    const std::string text =
        "host=192.0.2.111\n"
        "key=secret\n"
        "auto_connect_enabled=false\n"
        "reconnect_initial_backoff_ms=1250\n"
        "reconnect_max_backoff_ms=60000\n"
        "reconnect_idle_retry_ms=180000\n";

    Expect(mwb::ParseAppConfig(text, config, &error), "ParseAppConfig should accept connection policy keys");
    Expect(config.host == "192.0.2.111", "Config host should parse with connection policy keys");
    Expect(config.key == "secret", "Config key should parse with connection policy keys");
    Expect(!config.autoConnectEnabled, "auto_connect_enabled should parse as false");
    Expect(config.reconnectInitialBackoffMs == 1250, "reconnect_initial_backoff_ms should parse");
    Expect(config.reconnectMaxBackoffMs == 60000, "reconnect_max_backoff_ms should parse");
    Expect(config.reconnectIdleRetryMs == 180000, "reconnect_idle_retry_ms should parse");

    const std::filesystem::path path = MakeTempPath("mwb-config-connection-policy-test.ini");
    Expect(mwb::SaveConfigFile(path, config, error), "SaveConfigFile should succeed for connection policy config");

    const std::string rendered = mwb::RenderAppConfig(config);
    ExpectRenderedLine(rendered, "auto_connect_enabled", "false",
                       "Rendered config should include auto_connect_enabled");
    ExpectRenderedLine(rendered, "reconnect_initial_backoff_ms", "1250",
                       "Rendered config should include reconnect_initial_backoff_ms");
    ExpectRenderedLine(rendered, "reconnect_max_backoff_ms", "60000",
                       "Rendered config should include reconnect_max_backoff_ms");
    ExpectRenderedLine(rendered, "reconnect_idle_retry_ms", "180000",
                       "Rendered config should include reconnect_idle_retry_ms");

    mwb::AppConfig loaded;
    Expect(mwb::LoadConfigFile(path, loaded, error), "LoadConfigFile should succeed for connection policy config");
    Expect(loaded.autoConnectEnabled == config.autoConnectEnabled,
           "Config autoConnectEnabled round-trip for connection policy config");
    Expect(loaded.reconnectInitialBackoffMs == config.reconnectInitialBackoffMs,
           "Config reconnectInitialBackoffMs round-trip for connection policy config");
    Expect(loaded.reconnectMaxBackoffMs == config.reconnectMaxBackoffMs,
           "Config reconnectMaxBackoffMs round-trip for connection policy config");
    Expect(loaded.reconnectIdleRetryMs == config.reconnectIdleRetryMs,
           "Config reconnectIdleRetryMs round-trip for connection policy config");

    const std::string loadedRendered = mwb::RenderAppConfig(loaded);
    ExpectRenderedLine(loadedRendered, "auto_connect_enabled", "false",
                       "Loaded config should keep auto_connect_enabled");
    ExpectRenderedLine(loadedRendered, "reconnect_initial_backoff_ms", "1250",
                       "Loaded config should keep reconnect_initial_backoff_ms");
    ExpectRenderedLine(loadedRendered, "reconnect_max_backoff_ms", "60000",
                       "Loaded config should keep reconnect_max_backoff_ms");
    ExpectRenderedLine(loadedRendered, "reconnect_idle_retry_ms", "180000",
                       "Loaded config should keep reconnect_idle_retry_ms");

    std::error_code ignore;
    std::filesystem::remove(path, ignore);
}

void TestParseAppConfigKeyFileOverridesInlineKey() {
    mwb::AppConfig config;
    std::string error;
    const std::string text =
        "host=192.0.2.109\n"
        "key=inline-secret\n"
        "key_file=../keys/mwb.key\n";

    Expect(mwb::ParseAppConfig(text, config, &error), "ParseAppConfig should accept key_file");
    Expect(config.host == "192.0.2.109", "ParseAppConfig should keep host when using key_file");
    Expect(config.key.empty(), "key_file should override inline key text");
    Expect(config.keyFile == "../keys/mwb.key", "ParseAppConfig should store key_file path");
    const std::string rendered = mwb::RenderAppConfig(config);
    ExpectRenderedLine(rendered, "key", "", "Rendered config should clear the inline key after key_file precedence");
    ExpectRenderedLine(rendered, "key_file", "../keys/mwb.key", "Rendered config should keep key_file");
    ExpectRenderedLine(rendered, "key_secret_id", "",
                       "Rendered config should leave key_secret_id empty when only key_file is used");
}

void TestParseAppConfigKeySecretIdOverridesKeyAndKeyFile() {
    mwb::AppConfig config;
    std::string error;
    const std::string text =
        "host=192.0.2.110\n"
        "key=inline-secret\n"
        "key_file=../keys/mwb.key\n"
        "key_secret_id=org.freedesktop.secrets/mwb-linux\n";

    Expect(mwb::ParseAppConfig(text, config, &error), "ParseAppConfig should accept key_secret_id");
    Expect(config.host == "192.0.2.110", "ParseAppConfig should keep host when using key_secret_id");
    Expect(config.key.empty(), "key_secret_id should override inline key text");
    Expect(config.keyFile.empty(), "key_secret_id should override key_file");
    const std::string rendered = mwb::RenderAppConfig(config);
    ExpectRenderedLine(rendered, "key", "", "Rendered config should clear the inline key after key_secret_id precedence");
    ExpectRenderedLine(rendered, "key_file", "", "Rendered config should clear key_file after key_secret_id precedence");
    ExpectRenderedLine(rendered, "key_secret_id", "org.freedesktop.secrets/mwb-linux",
                       "Rendered config should store key_secret_id");
}

void TestAppStateRoundTrip() {
    mwb::AppState state;
    state.localMachineId = 0x1234abcdU;
    state.peers.push_back(mwb::PeerState{
        "192.0.2.107",
        "windows-box",
        15101,
        true,
        true,
        111,
        222,
    });

    const std::filesystem::path path = MakeTempPath("mwb-state-test.ini");
    std::string error;
    Expect(mwb::SaveAppState(path, state, error), "SaveAppState should succeed");

    mwb::AppState loaded;
    Expect(mwb::LoadAppState(path, loaded, error), "LoadAppState should succeed");
    Expect(loaded.localMachineId == state.localMachineId, "State machine id round-trip");
    Expect(loaded.peers.size() == 1, "State peer count round-trip");
    if (!loaded.peers.empty()) {
        const auto& peer = loaded.peers.front();
        Expect(peer.host == "192.0.2.107", "State peer host round-trip");
        Expect(peer.name == "windows-box", "State peer name round-trip");
        Expect(peer.approved, "State peer approved round-trip");
        Expect(peer.connectedNow, "State peer connectedNow round-trip");
        Expect(peer.lastSeenEpochSeconds == 111, "State peer lastSeen round-trip");
        Expect(peer.lastConnectedEpochSeconds == 222, "State peer lastConnected round-trip");
    }
    std::error_code ignore;
    std::filesystem::remove(path, ignore);
}

void TestEnsureLocalMachineIdStable() {
    mwb::AppState state;
    const uint32_t first = mwb::EnsureLocalMachineId(state);
    const uint32_t second = mwb::EnsureLocalMachineId(state);
    Expect(first != 0, "EnsureLocalMachineId should return non-zero id");
    Expect(first == second, "EnsureLocalMachineId should be stable once assigned");
}

void TestUpsertPeerState() {
    mwb::AppState state;
    mwb::UpsertPeerState(state, mwb::PeerState{"192.0.2.10", "", 15101, false, false, 100, 0});
    mwb::UpsertPeerState(state, mwb::PeerState{"192.0.2.10", "desktop", 15101, true, true, 200, 300});
    Expect(state.peers.size() == 1, "UpsertPeerState should merge by host/port");
    if (!state.peers.empty()) {
        const auto& peer = state.peers.front();
        Expect(peer.name == "desktop", "UpsertPeerState should update peer name");
        Expect(peer.approved, "UpsertPeerState should retain approval");
        Expect(peer.connectedNow, "UpsertPeerState should update connected status");
        Expect(peer.lastSeenEpochSeconds == 200, "UpsertPeerState should keep newest lastSeen");
        Expect(peer.lastConnectedEpochSeconds == 300, "UpsertPeerState should keep newest lastConnected");
    }
}

void TestSessionStateTransitions() {
    mwb::AppState state;
    state.localMachineId = 0x11111111u;
    state.peers.push_back(mwb::PeerState{"192.0.2.10", "old", 15101, true, true, 100, 200});
    state.peers.push_back(mwb::PeerState{"192.0.2.11", "other", 15101, true, true, 100, 200});

    mwb::MarkSessionEstablished(state, "192.0.2.12", 15101, "fresh", 0x22222222u, 300);

    Expect(state.localMachineId == 0x22222222u, "Session establishment should update the local machine id");
    int connectedPeers = 0;
    const mwb::PeerState* connectedPeer = nullptr;
    for (const auto& peer : state.peers) {
        if (peer.connectedNow) {
            ++connectedPeers;
            connectedPeer = &peer;
        }
    }
    Expect(connectedPeers == 1, "Session establishment should leave exactly one connected peer");
    if (connectedPeer != nullptr) {
        Expect(connectedPeer->host == "192.0.2.12", "Session establishment should mark the connected host");
        Expect(connectedPeer->name == "fresh", "Session establishment should store the remote name");
        Expect(connectedPeer->approved, "Session establishment should approve the authenticated peer");
        Expect(connectedPeer->lastConnectedEpochSeconds == 300,
               "Session establishment should update last connected time");
    }

    mwb::MarkSessionDisconnected(state);
    for (const auto& peer : state.peers) {
        Expect(!peer.connectedNow, "Session disconnect should clear every connected flag");
    }
}

void TestReconnectPolicyProgression() {
    const auto policy = mwb::NormalizeReconnectPolicy(50, 250, 200);
    Expect(policy.initialBackoffMs == mwb::kMinimumReconnectBackoffMs,
           "Reconnect policy should clamp initial backoff");
    Expect(policy.maxBackoffMs == 250, "Reconnect policy should preserve a valid max backoff");
    Expect(policy.idleRetryMs == 200, "Reconnect policy should preserve a shorter idle retry");

    auto state = mwb::InitialReconnectState(policy);
    Expect(mwb::ScheduledReconnectDelayMs(policy, state) == 100,
           "Initial reconnect delay should start at the normalized initial backoff");

    state = mwb::AdvanceReconnectAfterFailure(policy, state);
    Expect(state.nextBackoffMs == 200 && !state.useIdleRetry,
           "Reconnect backoff should grow after the first failure");
    Expect(mwb::ScheduledReconnectDelayMs(policy, state) == 200,
           "Second reconnect delay should use the grown backoff");

    state = mwb::AdvanceReconnectAfterFailure(policy, state);
    Expect(state.nextBackoffMs == 250 && state.useIdleRetry,
           "Reconnect backoff should clamp at max and enter idle retry mode");
    Expect(mwb::ScheduledReconnectDelayMs(policy, state) == 200,
           "Idle retry should schedule the idle retry interval");

    state = mwb::AdvanceReconnectAfterFailure(policy, state);
    Expect(state.nextBackoffMs == 100 && state.useIdleRetry,
           "Idle retry should reset the next active backoff while staying in idle mode");

    state = mwb::ResetReconnectAfterSuccess(policy);
    Expect(state.nextBackoffMs == 100 && !state.useIdleRetry,
           "Successful reconnect should reset active backoff state");
}

void TestReconnectPolicyCapsLongRetryDelays() {
    const auto policy = mwb::NormalizeReconnectPolicy(1000, 300000, 900000);
    Expect(policy.initialBackoffMs == 1000, "Reconnect policy should preserve a valid initial delay");
    Expect(policy.maxBackoffMs == mwb::kMaximumReconnectBackoffMs,
           "Reconnect policy should cap excessive max backoff");
    Expect(policy.idleRetryMs == mwb::kMaximumReconnectBackoffMs,
           "Reconnect policy should cap excessive idle retry");
}

void TestCollectRecoveryPeerNamesForConfiguredIpv4() {
    mwb::AppState state;
    state.peers.push_back(mwb::PeerState{"192.0.2.107", "WIN-PC", 15101, true, false, 100, 200});
    state.peers.push_back(mwb::PeerState{"192.0.2.156", "WIN-PC", 15101, true, false, 150, 250});
    state.peers.push_back(mwb::PeerState{"192.0.2.122", "OTHER-HOST", 15101, true, false, 175, 300});

    const auto names = mwb::CollectRecoveryPeerNames(state, "192.0.2.107", 15101);
    Expect(names.size() == 1, "Recovery names for an IPv4-configured peer should come from the configured host only");
    if (!names.empty()) {
        Expect(names.front() == "WIN-PC", "Recovery names should preserve the authenticated peer name");
    }
}

void TestCollectRecoveryPeerHostsUsesVerifiedCacheOnly() {
    mwb::AppState state;
    state.peers.push_back(mwb::PeerState{"192.0.2.107", "WIN-PC", 15101, true, false, 100, 100});
    state.peers.push_back(mwb::PeerState{"192.0.2.156", "WIN-PC", 15101, true, false, 120, 300});
    state.peers.push_back(mwb::PeerState{"192.0.2.200", "WIN-PC", 15101, true, false, 110, 200});
    state.peers.push_back(mwb::PeerState{"192.0.2.122", "OTHER-HOST", 15101, true, false, 130, 400});

    const auto hosts = mwb::CollectRecoveryPeerHosts(state, {"WIN-PC"}, "192.0.2.107", 15101);
    Expect(hosts.size() == 2, "Recovery hosts should only include authenticated cached IPs for the same peer name");
    if (hosts.size() >= 2) {
        Expect(hosts[0] == "192.0.2.156", "Recovery hosts should prefer the most recently connected verified IP");
        Expect(hosts[1] == "192.0.2.200", "Recovery hosts should include older verified IPs for the same peer");
    }
}

void TestCollectRecoveryPeerNamesForConfiguredHostname() {
    mwb::AppState state;
    state.peers.push_back(mwb::PeerState{"192.0.2.156", "WIN-PC", 15101, true, false, 100, 200});

    const auto names = mwb::CollectRecoveryPeerNames(state, "WIN-PC", 15101);
    Expect(!names.empty(), "Recovery names for a hostname-configured peer should not be empty");
    if (!names.empty()) {
        Expect(names.front() == "WIN-PC", "Recovery names should keep the configured hostname identity");
    }
}

void TestCollectRecoveryCandidateHostsForConfiguredIpv4() {
    mwb::AppState state;
    state.peers.push_back(mwb::PeerState{"192.0.2.107", "WIN-PC", 15101, true, false, 100, 100});
    state.peers.push_back(mwb::PeerState{"192.0.2.156", "WIN-PC", 15101, true, false, 120, 300});
    state.peers.push_back(mwb::PeerState{"192.0.2.156", "WIN-PC", 15101, true, false, 140, 250});
    state.peers.push_back(mwb::PeerState{"192.0.2.200", "WIN-PC", 15101, true, false, 110, 200});
    state.peers.push_back(mwb::PeerState{"desktop.local", "WIN-PC", 15101, true, false, 130, 400});
    state.peers.push_back(mwb::PeerState{"192.0.2.210", "WIN-PC", 15101, false, false, 150, 500});

    const auto hosts = mwb::CollectRecoveryCandidateHosts(state, "192.0.2.107", 15101);
    Expect(hosts.size() == 2, "Recovery candidates should exclude the configured IPv4, duplicates, unapproved peers, and non-IPv4 hosts");
    if (hosts.size() >= 2) {
        Expect(hosts[0] == "192.0.2.156", "Recovery candidates should prefer the newest verified alternate IP");
        Expect(hosts[1] == "192.0.2.200", "Recovery candidates should keep older verified alternate IPs");
    }
}

void TestCollectRecoveryCandidateHostsForConfiguredHostname() {
    mwb::AppState state;
    state.peers.push_back(mwb::PeerState{"192.0.2.156", "WIN-PC.office", 15101, true, false, 120, 300});
    state.peers.push_back(mwb::PeerState{"192.0.2.200", "WIN-PC", 15101, true, false, 110, 200});
    state.peers.push_back(mwb::PeerState{"desktop.local", "WIN-PC", 15101, true, false, 130, 400});
    state.peers.push_back(mwb::PeerState{"192.0.2.210", "OTHER-HOST", 15101, true, false, 140, 500});
    state.peers.push_back(mwb::PeerState{"192.0.2.211", "WIN-PC", 15102, true, false, 150, 600});

    const auto hosts = mwb::CollectRecoveryCandidateHosts(state, "WIN-PC", 15101);
    Expect(hosts.size() == 2, "Hostname recovery candidates should include verified IPv4 peers on the configured port only");
    if (hosts.size() >= 2) {
        Expect(hosts[0] == "192.0.2.156", "Hostname recovery candidates should sort by newest verified connection");
        Expect(hosts[1] == "192.0.2.200", "Hostname recovery candidates should keep additional verified IPv4 peers");
    }
}

void TestCollectRecoveryDiscoveredHostsUsesApprovedNamesOnly() {
    mwb::AppState state;
    state.peers.push_back(mwb::PeerState{"192.0.2.107", "WIN-PC", 15101, true, false, 100, 300});
    state.peers.push_back(mwb::PeerState{"192.0.2.108", "OTHER-PC", 15101, true, false, 110, 400});

    std::vector<mwb::DiscoveryCandidate> candidates;
    candidates.push_back(mwb::DiscoveryCandidate{
        "192.0.2.156",
        "WIN-PC.local",
        true,
        "eth0",
        mwb::DiscoveryStatus::Open,
    });
    candidates.push_back(mwb::DiscoveryCandidate{
        "192.0.2.157",
        "WIN-PC",
        false,
        "eth0",
        mwb::DiscoveryStatus::Open,
    });
    candidates.push_back(mwb::DiscoveryCandidate{
        "192.0.2.158",
        "OTHER-PC",
        true,
        "eth0",
        mwb::DiscoveryStatus::Open,
    });

    const auto hosts = mwb::CollectRecoveryDiscoveredHosts(state, "192.0.2.107", 15101, candidates);
    Expect(hosts.size() == 2, "Discovery recovery should include discovered names matching the approved peer");
    if (hosts.size() >= 2) {
        Expect(hosts.front() == "192.0.2.156", "Discovery recovery should return the moved approved peer IP");
        Expect(hosts[1] == "192.0.2.157", "Discovery recovery can try unverified names because the session key authenticates");
    }
}

void TestDiscoveryZeroHosts() {
    mwb::DiscoveryOptions options;
    options.maxHostsPerSubnet = 0;
    const auto results = mwb::DiscoverLanCandidates(options);
    Expect(results.empty(), "DiscoverLanCandidates should return no results when maxHostsPerSubnet=0");
}

void TestKScreenDoctorSingleOutputGeometry() {
    const std::string output =
        "Output: 1 eDP-1 abcdef\n"
        "  enabled\n"
        "  connected\n"
        "  Geometry: 0,0 2048x1280\n"
        "  Scale: 1.25\n";

    const auto size = mwb::ParseKScreenDoctorLogicalScreenSize(output);
    Expect(size == std::optional<std::pair<int, int>>({2048, 1280}),
           "KScreen single-output geometry should parse the logical screen size");
}

void TestKScreenDoctorMultiOutputBoundingBox() {
    const std::string output =
        "Output: 1 eDP-1 abcdef\n"
        "  enabled\n"
        "  connected\n"
        "  Geometry: 0,0 1728x1117\n"
        "Output: 2 HDMI-A-1 123456\n"
        "  enabled\n"
        "  connected\n"
        "  Geometry: 1728,0 1920x1080\n";

    const auto size = mwb::ParseKScreenDoctorLogicalScreenSize(output);
    Expect(size == std::optional<std::pair<int, int>>({3648, 1117}),
           "KScreen multi-output geometry should produce the bounding box size");
}

void TestKScreenDoctorParserIgnoresAnsiSequences() {
    const std::string output =
        "\x1b[01;32mOutput: \x1b[0;0m1 eDP-1 abcdef\n"
        "\t\x1b[01;32menabled\x1b[0;0m\n"
        "\t\x1b[01;32mconnected\x1b[0;0m\n"
        "\t\x1b[01;33mGeometry: \x1b[0;0m0,0 2048x1280\n";

    const auto size = mwb::ParseKScreenDoctorLogicalScreenSize(output);
    Expect(size == std::optional<std::pair<int, int>>({2048, 1280}),
           "KScreen parser should ignore ANSI color codes");
}

} // namespace

int main() {
    TestAppConfigRoundTrip();
    TestAndroidRelayFrames();
    TestAppConfigKeyFileRoundTrip();
    TestAppConfigKeySecretIdRoundTrip();
    TestAppConfigConnectionPolicyRoundTrip();
    TestParseAppConfigKeyFileOverridesInlineKey();
    TestParseAppConfigKeySecretIdOverridesKeyAndKeyFile();
    TestAppStateRoundTrip();
    TestEnsureLocalMachineIdStable();
    TestUpsertPeerState();
    TestSessionStateTransitions();
    TestReconnectPolicyProgression();
    TestReconnectPolicyCapsLongRetryDelays();
    TestCollectRecoveryPeerNamesForConfiguredIpv4();
    TestCollectRecoveryPeerHostsUsesVerifiedCacheOnly();
    TestCollectRecoveryPeerNamesForConfiguredHostname();
    TestCollectRecoveryCandidateHostsForConfiguredIpv4();
    TestCollectRecoveryCandidateHostsForConfiguredHostname();
    TestCollectRecoveryDiscoveredHostsUsesApprovedNamesOnly();
    TestDiscoveryZeroHosts();
    TestKScreenDoctorSingleOutputGeometry();
    TestKScreenDoctorMultiOutputBoundingBox();
    TestKScreenDoctorParserIgnoresAnsiSequences();

    if (g_failures == 0) {
        std::cout << "All tests passed." << std::endl;
        return 0;
    }

    std::cerr << g_failures << " test(s) failed." << std::endl;
    return 1;
}
