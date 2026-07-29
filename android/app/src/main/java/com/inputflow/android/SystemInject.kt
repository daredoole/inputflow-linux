package com.inputflow.android

import android.content.Context
import android.os.Build
import android.view.InputEvent
import org.lsposed.hiddenapibypass.HiddenApiBypass

/**
 * Performs the actual system-level event injection. Only works when called from
 * a privileged process (Shizuku shell UID or libsu root), where the InputManager
 * caller is permitted to inject globally — including secure fields, unlike the
 * AccessibilityService gesture path.
 */
object SystemInject {
    // InputManager.INJECT_INPUT_EVENT_MODE_*
    const val MODE_ASYNC = 0
    const val MODE_WAIT_FOR_FINISH = 2

    fun exempt() {
        if (Build.VERSION.SDK_INT < Build.VERSION_CODES.P) return
        try {
            HiddenApiBypass.addHiddenApiExemptions("")
        } catch (_: Throwable) {
        }
    }

    fun inject(context: Context, event: InputEvent, mode: Int = MODE_ASYNC): Boolean {
        return try {
            val im = context.getSystemService(Context.INPUT_SERVICE) ?: return false
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.P) {
                HiddenApiBypass.invoke(im.javaClass, im, "injectInputEvent", event, mode)
            } else {
                val method = im.javaClass.getDeclaredMethod(
                    "injectInputEvent",
                    InputEvent::class.java,
                    Int::class.javaPrimitiveType,
                )
                method.isAccessible = true
                method.invoke(im, event, mode)
            }
            true
        } catch (_: Throwable) {
            false
        }
    }
}
