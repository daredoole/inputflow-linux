#include <linux/input-event-codes.h>
#include <linux/uinput.h>
#include <sys/ioctl.h>

#include <iostream>
#include <vector>

namespace {

struct IoctlCall {
    unsigned long request;
    int value;
};

std::vector<IoctlCall> g_ioctlCalls;

} // namespace

extern "C" int test_ioctl(int, unsigned long request, ...) {
    __builtin_va_list args;
    __builtin_va_start(args, request);
    const int value = __builtin_va_arg(args, int);
    __builtin_va_end(args);
    g_ioctlCalls.push_back({request, value});
    return 0;
}

#define ioctl test_ioctl
#include "InputDeviceCapabilities.h"
#undef ioctl

namespace {

int g_failures = 0;

bool SawIoctl(unsigned long request, int value) {
    for (const auto& call : g_ioctlCalls) {
        if (call.request == request && call.value == value) {
            return true;
        }
    }
    return false;
}

void Expect(bool condition, const char* message) {
    if (!condition) {
        ++g_failures;
        std::cerr << "FAIL: " << message << std::endl;
    }
}

void TestAbsoluteMouseAdvertisesAbsoluteAxes() {
    g_ioctlCalls.clear();
    mwb::ConfigureAbsoluteMouseDevice(-1, 2048, 1280);

    Expect(SawIoctl(UI_SET_EVBIT, EV_ABS),  "absolute mouse advertises EV_ABS");
    Expect(SawIoctl(UI_SET_ABSBIT, ABS_X),  "absolute mouse advertises ABS_X");
    Expect(SawIoctl(UI_SET_ABSBIT, ABS_Y),  "absolute mouse advertises ABS_Y");
    Expect(!SawIoctl(UI_SET_RELBIT, REL_X), "absolute mouse does not advertise REL_X");
    Expect(!SawIoctl(UI_SET_RELBIT, REL_Y), "absolute mouse does not advertise REL_Y");
}

void TestAbsoluteMouseAdvertisesWheelAndButtons() {
    g_ioctlCalls.clear();
    mwb::ConfigureAbsoluteMouseDevice(-1, 2048, 1280);

    Expect(SawIoctl(UI_SET_EVBIT, EV_REL),     "absolute mouse advertises EV_REL for wheel");
    Expect(SawIoctl(UI_SET_RELBIT, REL_WHEEL),  "absolute mouse advertises REL_WHEEL");
    Expect(SawIoctl(UI_SET_RELBIT, REL_HWHEEL), "absolute mouse advertises REL_HWHEEL");
    Expect(SawIoctl(UI_SET_EVBIT, EV_KEY),      "absolute mouse advertises EV_KEY");
    Expect(SawIoctl(UI_SET_KEYBIT, BTN_LEFT),   "absolute mouse advertises BTN_LEFT");
    Expect(SawIoctl(UI_SET_KEYBIT, BTN_RIGHT),  "absolute mouse advertises BTN_RIGHT");
    Expect(SawIoctl(UI_SET_KEYBIT, BTN_MIDDLE), "absolute mouse advertises BTN_MIDDLE");
}

void TestAbsoluteMousePopulatesLegacyAxisRanges() {
    struct uinput_user_dev device {};
    mwb::PopulateAbsoluteMouseSetup(device, 2048, 1280);

    Expect(device.absmin[ABS_X] == 0, "legacy ABS_X min is zero");
    Expect(device.absmax[ABS_X] == 2047, "legacy ABS_X max matches screen width minus one");
    Expect(device.absmin[ABS_Y] == 0, "legacy ABS_Y min is zero");
    Expect(device.absmax[ABS_Y] == 1279, "legacy ABS_Y max matches screen height minus one");
}

} // namespace

int main() {
    TestAbsoluteMouseAdvertisesAbsoluteAxes();
    TestAbsoluteMouseAdvertisesWheelAndButtons();
    TestAbsoluteMousePopulatesLegacyAxisRanges();

    if (g_failures == 0) {
        std::cout << "Input device capability tests passed." << std::endl;
        return 0;
    }

    std::cerr << g_failures << " input device capability test(s) failed." << std::endl;
    return 1;
}
