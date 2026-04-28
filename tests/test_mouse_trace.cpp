#include "InputManager.h"

#include <iostream>
#include <string>
#include <vector>

namespace {

int g_failures = 0;

void ExpectContains(const std::string& text, const std::string& needle, const char* message) {
    if (text.find(needle) == std::string::npos) {
        ++g_failures;
        std::cerr << "FAIL: " << message << " missing='" << needle << "'" << std::endl;
    }
}

void TestEmptyTraceFormats() {
    const std::string formatted = mwb::InputManager::FormatMouseTrace({});
    ExpectContains(formatted, "Last 0 mouse packet", "empty mouse trace should report zero packets");
}

void TestMouseTraceFormatsPacketDetails() {
    mwb::InputManager::MouseTraceEntry entry;
    entry.sequence = 7;
    entry.raw = mwb::MouseData{65535, 32768, 120, 0x020A};
    entry.relativeMove = false;
    entry.screenWidth = 2048;
    entry.screenHeight = 1280;
    entry.cursorBeforeX = 1024;
    entry.cursorBeforeY = 640;
    entry.cursorAfterX = 2047;
    entry.cursorAfterY = 639;
    entry.wheelDeltaV = 120;
    entry.wheelResidueV = 0;
    entry.emittedEventCount = 5;
    entry.writeOk = true;

    const std::string formatted = mwb::InputManager::FormatMouseTrace({entry});
    ExpectContains(formatted, "#7", "mouse trace should include sequence");
    ExpectContains(formatted, "wParam=0x20a", "mouse trace should include hex wParam");
    ExpectContains(formatted, "raw=(65535,32768)", "mouse trace should include raw coordinates");
    ExpectContains(formatted, "screen=2048x1280", "mouse trace should include screen dimensions");
    ExpectContains(formatted, "cursor=1024,640->2047,639", "mouse trace should include cursor movement");
    ExpectContains(formatted, "wheel=120,0", "mouse trace should include wheel deltas");
    ExpectContains(formatted, "events=5", "mouse trace should include emitted event count");
    ExpectContains(formatted, "write=ok", "mouse trace should include write result");
}

} // namespace

int main() {
    TestEmptyTraceFormats();
    TestMouseTraceFormatsPacketDetails();

    if (g_failures == 0) {
        std::cout << "Mouse trace tests passed." << std::endl;
        return 0;
    }

    std::cerr << g_failures << " mouse trace test(s) failed." << std::endl;
    return 1;
}
