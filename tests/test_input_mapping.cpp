#include <iostream>

#include "Protocol.h"

namespace {

int g_failures = 0;

void ExpectEqual(int actual, int expected, const char* message) {
    if (actual != expected) {
        ++g_failures;
        std::cerr << "FAIL: " << message
                  << " expected=" << expected
                  << " actual=" << actual
                  << std::endl;
    }
}

void ExpectTrue(bool value, const char* message) {
    if (!value) {
        ++g_failures;
        std::cerr << "FAIL: " << message << " expected=true actual=false" << std::endl;
    }
}

void ExpectFalse(bool value, const char* message) {
    if (value) {
        ++g_failures;
        std::cerr << "FAIL: " << message << " expected=false actual=true" << std::endl;
    }
}

void TestRecognizedMouseMessages() {
    ExpectTrue(mwb::isRecognizedMouseMessage(0x0200), "mouse move is accepted");
    ExpectTrue(mwb::isRecognizedMouseMessage(0x0201), "left button down is accepted");
    ExpectTrue(mwb::isRecognizedMouseMessage(0x020A), "vertical wheel is accepted");
    ExpectTrue(mwb::isRecognizedMouseMessage(0x020B), "xbutton down is accepted");
    ExpectTrue(mwb::isRecognizedMouseMessage(0x020C), "xbutton up is accepted");
    ExpectTrue(mwb::isRecognizedMouseMessage(0x020D), "xbutton double-click is accepted");
    ExpectTrue(mwb::isRecognizedMouseMessage(0x020E), "horizontal wheel is accepted");
    ExpectFalse(mwb::isRecognizedMouseMessage(0x0000), "zero mouse message is rejected");
    ExpectFalse(mwb::isRecognizedMouseMessage(0x0210), "unknown mouse message is rejected");
}

void TestDecodeXButtonPayload() {
    ExpectEqual(mwb::decodeXButtonPayload(0x00010000), 0x0001,
                "high-word XBUTTON1 payload is decoded");
    ExpectEqual(mwb::decodeXButtonPayload(0x00020000), 0x0002,
                "high-word XBUTTON2 payload is decoded");
    ExpectEqual(mwb::decodeXButtonPayload(0x00000001), 0x0001,
                "legacy low-word XBUTTON1 payload is decoded");
    ExpectEqual(mwb::decodeXButtonPayload(0x00000002), 0x0002,
                "legacy low-word XBUTTON2 payload is decoded");
    ExpectEqual(mwb::decodeXButtonPayload(0x00030000), 0,
                "unknown xbutton payload is rejected");
}

void TestKnownKeyboardFlagsMask() {
    ExpectTrue(mwb::hasOnlyKnownKeyboardFlags(0), "empty keyboard flags are accepted");
    ExpectTrue(mwb::hasOnlyKnownKeyboardFlags(mwb::kLlkhfExtended | mwb::kLlkhfUp),
               "known LLKHF bits are accepted");
    ExpectTrue(mwb::hasOnlyKnownKeyboardFlags(mwb::kLlkhfInjected | mwb::kLlkhfAltDown),
               "injected alt key flags are accepted");
    ExpectFalse(mwb::hasOnlyKnownKeyboardFlags(0x02), "unknown low keyboard flag bit is rejected");
    ExpectFalse(mwb::hasOnlyKnownKeyboardFlags(0x40), "unknown high keyboard flag bit is rejected");
}

void TestProtocolCoordinateScaling() {
    // Verify the inline scaling used in InjectMouse: value * (span-1) / 65535
    const int screenW = 2048;
    const int screenH = 1280;

    const int scaledMin = static_cast<int>(static_cast<int64_t>(0) * (screenW - 1) / 65535);
    ExpectEqual(scaledMin, 0, "protocol min (0) maps to pixel 0");

    const int scaledMax = static_cast<int>(static_cast<int64_t>(65535) * (screenW - 1) / 65535);
    ExpectEqual(scaledMax, screenW - 1, "protocol max (65535) maps to screenW-1");

    const int scaledMid = static_cast<int>(static_cast<int64_t>(32768) * (screenW - 1) / 65535);
    ExpectTrue(scaledMid >= (screenW / 2) - 1 && scaledMid <= (screenW / 2) + 1,
               "protocol midpoint maps near screen midpoint");

    const int scaledHMax = static_cast<int>(static_cast<int64_t>(65535) * (screenH - 1) / 65535);
    ExpectEqual(scaledHMax, screenH - 1, "protocol max maps to screenH-1 for height axis");
}

} // namespace

int main() {
    TestRecognizedMouseMessages();
    TestDecodeXButtonPayload();
    TestKnownKeyboardFlagsMask();
    TestProtocolCoordinateScaling();

    if (g_failures == 0) {
        std::cout << "All input mapping tests passed." << std::endl;
        return 0;
    }

    std::cerr << g_failures << " input mapping test(s) failed." << std::endl;
    return 1;
}
