package com.inputflow.android

import android.content.Context
import android.view.InputEvent
import org.lsposed.hiddenapibypass.HiddenApiBypass

/**
 * Performs the actual system-level event injection. Only works when called from
 * a privileged process (Shizuku shell UID or libsu root), where the InputManager
 * caller is permitted to inject globally — including secure fields, unlike the
 * AccessibilityService gesture path.
 */
object SystemInject {
    // INJECT_INPUT_EVENT_MODE_ASYNC
    private const val MODE_ASYNC = 0

    fun exempt() {
        try {
            HiddenApiBypass.addHiddenApiExemptions("")
        } catch (_: Throwable) {
        }
    }

    fun inject(context: Context, event: InputEvent): Boolean {
        return try {
            val im = context.getSystemService(Context.INPUT_SERVICE) ?: return false
            HiddenApiBypass.invoke(im.javaClass, im, "injectInputEvent", event, MODE_ASYNC)
            true
        } catch (_: Throwable) {
            false
        }
    }
}
