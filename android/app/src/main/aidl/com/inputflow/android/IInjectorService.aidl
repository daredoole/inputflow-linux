package com.inputflow.android;

import android.view.InputEvent;

// Implemented by a privileged process (Shizuku shell-UID UserService or libsu
// root RootService). The app builds MotionEvent/KeyEvent and ships them here to
// be injected at system level via InputManager.injectInputEvent.
interface IInjectorService {
    boolean ping();
    boolean inject(in InputEvent event);
}
