#include <iostream>
#include <optional>
#include <string>
#include <vector>

#include "MediaKeyBridge.h"

namespace {

int g_failures = 0;

void Expect(bool condition, const std::string& message) {
    if (!condition) {
        ++g_failures;
        std::cerr << "FAIL: " << message << std::endl;
    }
}

void TestMediaVirtualKeyMapping() {
    Expect(mwb::MediaActionForVirtualKey(0xB0) == mwb::MediaKeyAction::Next,
           "VK_MEDIA_NEXT_TRACK should map to Next");
    Expect(mwb::MediaActionForVirtualKey(0xB1) == mwb::MediaKeyAction::Previous,
           "VK_MEDIA_PREV_TRACK should map to Previous");
    Expect(mwb::MediaActionForVirtualKey(0xB2) == mwb::MediaKeyAction::Stop,
           "VK_MEDIA_STOP should map to Stop");
    Expect(mwb::MediaActionForVirtualKey(0xB3) == mwb::MediaKeyAction::PlayPause,
           "VK_MEDIA_PLAY_PAUSE should map to PlayPause");
    Expect(!mwb::MediaActionForVirtualKey(0x41).has_value(),
           "Non-media virtual keys should not map to MPRIS actions");
}

void TestPlayerctlCommandNames() {
    Expect(std::string(mwb::PlayerctlCommandForAction(mwb::MediaKeyAction::Next)) == "next",
           "Next should map to playerctl next");
    Expect(std::string(mwb::PlayerctlCommandForAction(mwb::MediaKeyAction::Previous)) == "previous",
           "Previous should map to playerctl previous");
    Expect(std::string(mwb::PlayerctlCommandForAction(mwb::MediaKeyAction::Stop)) == "stop",
           "Stop should map to playerctl stop");
    Expect(std::string(mwb::PlayerctlCommandForAction(mwb::MediaKeyAction::PlayPause)) == "play-pause",
           "PlayPause should map to playerctl play-pause");
}

void TestPlayerctlArgumentConstruction() {
    const auto defaultArgs = mwb::BuildPlayerctlArguments(mwb::MediaKeyAction::PlayPause, "");
    Expect(defaultArgs == std::vector<std::string>({"playerctl", "play-pause"}),
           "Default playerctl args should target the active MPRIS player");

    const auto namedArgs = mwb::BuildPlayerctlArguments(mwb::MediaKeyAction::Next, "spotify");
    Expect(namedArgs == std::vector<std::string>({"playerctl", "--player", "spotify", "next"}),
           "Named playerctl args should honor MWB_MPRIS_PLAYER-style selection");
}

} // namespace

int main() {
    TestMediaVirtualKeyMapping();
    TestPlayerctlCommandNames();
    TestPlayerctlArgumentConstruction();

    if (g_failures != 0) {
        std::cerr << g_failures << " media key bridge test(s) failed." << std::endl;
        return 1;
    }

    std::cout << "Media key bridge tests passed." << std::endl;
    return 0;
}
