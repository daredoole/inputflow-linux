package com.inputflow.android

import android.content.Intent
import android.os.IBinder
import android.view.InputEvent
import com.topjohnwu.superuser.ipc.RootService

/**
 * Runs as root via libsu. Root is permitted to inject input globally. Declared
 * in the manifest as required by libsu RootService.
 */
class RootInjectorService : RootService() {

    override fun onBind(intent: Intent): IBinder {
        SystemInject.exempt()
        return object : IInjectorService.Stub() {
            override fun ping(): Boolean = true
            override fun inject(event: InputEvent?): Boolean {
                val e = event ?: return false
                return SystemInject.inject(this@RootInjectorService, e)
            }
        }
    }
}
