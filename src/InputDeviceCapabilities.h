#pragma once

#include <algorithm>
#include <linux/uinput.h>
#include <sys/ioctl.h>

namespace mwb {

inline void PopulateAbsoluteMouseSetup(struct uinput_user_dev& device, int screenW, int screenH) {
    screenW = std::max(1, screenW);
    screenH = std::max(1, screenH);

    device.absmin[ABS_X] = 0;
    device.absmax[ABS_X] = screenW - 1;
    device.absmin[ABS_Y] = 0;
    device.absmax[ABS_Y] = screenH - 1;
}

inline bool ConfigureAbsoluteMouseDevice(int fd, int screenW, int screenH) {
    screenW = std::max(1, screenW);
    screenH = std::max(1, screenH);

    ioctl(fd, UI_SET_PROPBIT, INPUT_PROP_POINTER);

    ioctl(fd, UI_SET_EVBIT, EV_KEY);
    ioctl(fd, UI_SET_EVBIT, EV_ABS);
    ioctl(fd, UI_SET_EVBIT, EV_REL);

    ioctl(fd, UI_SET_ABSBIT, ABS_X);
    ioctl(fd, UI_SET_ABSBIT, ABS_Y);

    struct uinput_abs_setup absX{};
    absX.code = ABS_X;
    absX.absinfo.minimum = 0;
    absX.absinfo.maximum = screenW - 1;
    absX.absinfo.resolution = 1;
    ioctl(fd, UI_ABS_SETUP, &absX);

    struct uinput_abs_setup absY{};
    absY.code = ABS_Y;
    absY.absinfo.minimum = 0;
    absY.absinfo.maximum = screenH - 1;
    absY.absinfo.resolution = 1;
    ioctl(fd, UI_ABS_SETUP, &absY);

    ioctl(fd, UI_SET_RELBIT, REL_WHEEL);
    ioctl(fd, UI_SET_RELBIT, REL_HWHEEL);
#ifdef REL_WHEEL_HI_RES
    ioctl(fd, UI_SET_RELBIT, REL_WHEEL_HI_RES);
#endif
#ifdef REL_HWHEEL_HI_RES
    ioctl(fd, UI_SET_RELBIT, REL_HWHEEL_HI_RES);
#endif

    ioctl(fd, UI_SET_KEYBIT, BTN_LEFT);
    ioctl(fd, UI_SET_KEYBIT, BTN_RIGHT);
    ioctl(fd, UI_SET_KEYBIT, BTN_MIDDLE);
    ioctl(fd, UI_SET_KEYBIT, BTN_SIDE);
    ioctl(fd, UI_SET_KEYBIT, BTN_EXTRA);

    return true;
}

} // namespace mwb
