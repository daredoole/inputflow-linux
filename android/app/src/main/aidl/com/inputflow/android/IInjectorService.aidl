package com.inputflow.android;

import android.view.InputEvent;

// Implemented by a privileged process (Shizuku shell-UID UserService or libsu
// root RootService). The app builds MotionEvent/KeyEvent and ships them here to
// be injected at system level via InputManager.injectInputEvent.
interface IInjectorService {
    boolean ping();
    // mode = InputManager.INJECT_INPUT_EVENT_MODE_* (0=ASYNC, 2=WAIT_FOR_FINISH).
    boolean inject(in InputEvent event, int mode);
}
