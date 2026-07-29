package com.inputflow.android

import android.content.Context
import android.view.InputEvent

/**
 * Runs inside the Shizuku-managed process (shell UID 2000), which is permitted to
 * call InputManager.injectInputEvent globally. Bound via Shizuku UserService.
 */
class ShizukuInjectorService(private val context: Context) : IInjectorService.Stub() {

    init {
        SystemInject.exempt()
    }

    override fun ping(): Boolean = true

    override fun inject(event: InputEvent?, mode: Int): Boolean {
        val e = event ?: return false
        return SystemInject.inject(context, e, mode)
    }

    // Called by Shizuku when the user service is torn down.
    fun destroy() = Unit
}
