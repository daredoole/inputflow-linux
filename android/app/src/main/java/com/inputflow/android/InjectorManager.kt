package com.inputflow.android

import android.content.ComponentName
import android.content.Context
import android.content.Intent
import android.content.ServiceConnection
import android.content.pm.PackageManager
import android.os.IBinder
import android.util.DisplayMetrics
import android.util.Log
import android.view.WindowManager
import com.topjohnwu.superuser.Shell
import com.topjohnwu.superuser.ipc.RootService
import org.json.JSONObject
import rikka.shizuku.Shizuku

/**
 * Selects and drives the active input-injection backend:
 *   - accessibility: no-root gesture/overlay path (fallback, always available)
 *   - shizuku:       shell-UID native injectInputEvent (no root)
 *   - root:          libsu root native injectInputEvent
 * Auto prefers root > shizuku > accessibility based on runtime availability.
 */
object InjectorManager {
    const val KEY_INJECT_BACKEND = "inject_backend"
    // Explicit, separate opt-in: native (Shizuku/root) injection can control the
    // device at system level, so selecting a native backend is not enough — the
    // user must also turn this on. Defends against a peer gaining native control
    // from backend selection alone.
    const val KEY_NATIVE_CONSENT = "native_injection_allowed"
    const val BACKEND_AUTO = "auto"
    const val BACKEND_ACCESSIBILITY = "accessibility"
    const val BACKEND_SHIZUKU = "shizuku"
    const val BACKEND_ROOT = "root"

    private const val TAG = "InjectorManager"

    @Volatile private var native: NativeInjector? = null
    @Volatile var activeBackend: String = BACKEND_ACCESSIBILITY
        private set
    private var appContext: Context? = null

    fun init(context: Context) {
        appContext = context.applicationContext
    }

    fun desiredBackend(context: Context): String {
        val prefs = context.getSharedPreferences(RelayForegroundService.PREFS, Context.MODE_PRIVATE)
        return prefs.getString(KEY_INJECT_BACKEND, BACKEND_AUTO) ?: BACKEND_AUTO
    }

    fun shizukuAvailable(): Boolean = try {
        Shizuku.pingBinder() &&
            Shizuku.checkSelfPermission() == PackageManager.PERMISSION_GRANTED
    } catch (_: Throwable) {
        false
    }

    fun rootAvailable(): Boolean = try {
        Shell.isAppGrantedRoot() == true
    } catch (_: Throwable) {
        false
    }

    fun nativeConsentGranted(context: Context): Boolean {
        val prefs = context.getSharedPreferences(RelayForegroundService.PREFS, Context.MODE_PRIVATE)
        return prefs.getBoolean(KEY_NATIVE_CONSENT, false)
    }

    /** Bind the selected (or best-available) native backend. Safe to call repeatedly. */
    fun start(context: Context) {
        // Native injection requires explicit consent in addition to a backend choice.
        if (!nativeConsentGranted(context)) {
            if (desiredBackend(context) != BACKEND_ACCESSIBILITY) {
                Log.w(TAG, "native injection not consented; using accessibility")
            }
            useAccessibility()
            return
        }
        when (desiredBackend(context)) {
            BACKEND_SHIZUKU -> bindShizuku(context)
            BACKEND_ROOT -> bindRoot(context)
            BACKEND_AUTO -> when {
                rootAvailable() -> bindRoot(context)
                shizukuAvailable() -> bindShizuku(context)
                else -> useAccessibility()
            }
            else -> useAccessibility()
        }
    }

    fun stop() {
        native = null
        useAccessibility()
    }

    private fun useAccessibility() {
        native = null
        activeBackend = BACKEND_ACCESSIBILITY
    }

    private fun setService(binder: IBinder?, kind: String) {
        val ctx = appContext ?: return
        val svc = IInjectorService.Stub.asInterface(binder) ?: return
        val injector = NativeInjector(svc, { displayMetrics(ctx) }, { sensitivity(ctx) })
        if (injector.ping()) {
            native = injector
            activeBackend = kind
            Log.i(TAG, "native injection active via $kind")
        } else {
            Log.w(TAG, "$kind injector failed ping; staying on accessibility")
        }
    }

    private val shizukuConn = object : ServiceConnection {
        override fun onServiceConnected(name: ComponentName?, binder: IBinder?) =
            setService(binder, BACKEND_SHIZUKU)
        override fun onServiceDisconnected(name: ComponentName?) = useAccessibility()
    }

    private val rootConn = object : ServiceConnection {
        override fun onServiceConnected(name: ComponentName?, binder: IBinder?) =
            setService(binder, BACKEND_ROOT)
        override fun onServiceDisconnected(name: ComponentName?) = useAccessibility()
    }

    private fun bindShizuku(context: Context) {
        if (!shizukuAvailable()) {
            useAccessibility()
            return
        }
        try {
            val args = Shizuku.UserServiceArgs(
                ComponentName(context.packageName, ShizukuInjectorService::class.java.name))
                .daemon(false)
                .processNameSuffix("inject")
                .version(2)
            Shizuku.bindUserService(args, shizukuConn)
        } catch (t: Throwable) {
            Log.w(TAG, "shizuku bind failed", t)
            useAccessibility()
        }
    }

    private fun bindRoot(context: Context) {
        try {
            RootService.bind(Intent(context, RootInjectorService::class.java), rootConn)
        } catch (t: Throwable) {
            Log.w(TAG, "root bind failed", t)
            useAccessibility()
        }
    }

    /** @return true if handled natively; false to let the accessibility path handle it. */
    fun handleMouse(frame: JSONObject): Boolean {
        val n = native ?: return false
        return n.handleMouse(frame)
    }

    fun handleKeyboard(frame: JSONObject): Boolean {
        val n = native ?: return false
        return n.handleKeyboard(frame)
    }

    /** Native continuous scroll for 2-finger trackpad gestures. */
    fun handleScroll(frame: JSONObject): Boolean {
        val n = native ?: return false
        return n.scrollBy(frame.optDouble("dx", 0.0).toFloat(), frame.optDouble("dy", 0.0).toFloat())
    }

    private fun sensitivity(context: Context): Float {
        val prefs = context.getSharedPreferences(RelayForegroundService.PREFS, Context.MODE_PRIVATE)
        return prefs.getFloat(SettingsActivity.KEY_SENSITIVITY, 1.0f).coerceIn(0.5f, 3.0f)
    }

    @Suppress("DEPRECATION")
    private fun displayMetrics(context: Context): DisplayMetrics {
        val metrics = DisplayMetrics()
        val wm = context.getSystemService(Context.WINDOW_SERVICE) as WindowManager
        wm.defaultDisplay.getRealMetrics(metrics)
        return metrics
    }
}
