package com.inputflow.android

import android.os.SystemClock
import android.util.DisplayMetrics
import android.view.InputDevice
import android.view.KeyCharacterMap
import android.view.KeyEvent
import android.view.MotionEvent
import org.json.JSONObject

/**
 * Builds real MotionEvent/KeyEvent objects from relay frames and ships them to a
 * privileged IInjectorService (Shizuku shell UID or libsu root) for system-level
 * injection. This is the "native-grade" path — a true system cursor, real key
 * events, and it works in fields the AccessibilityService gesture path cannot.
 */
class NativeInjector(
    private val service: IInjectorService,
    private val metricsProvider: () -> DisplayMetrics,
    private val sensitivityProvider: () -> Float,
) {
    private var px = 0f
    private var py = 0f
    private var primed = false
    private var metaState = 0

    fun ping(): Boolean = try { service.ping() } catch (_: Throwable) { false }

    private fun inject(event: android.view.InputEvent): Boolean =
        try { service.inject(event) } catch (_: Throwable) { false }

    fun handleMouse(frame: JSONObject) {
        val m = metricsProvider()
        if (!primed) {
            px = m.widthPixels / 2f
            py = m.heightPixels / 2f
            primed = true
        }
        when (frame.optInt("wParam")) {
            WM_MOUSEMOVE -> {
                val sens = sensitivityProvider()
                val baseX = frame.optInt("x") / 65535f * m.widthPixels
                val baseY = frame.optInt("y") / 65535f * m.heightPixels
                val cx = m.widthPixels / 2f
                val cy = m.heightPixels / 2f
                px = (cx + (baseX - cx) * sens).coerceIn(0f, m.widthPixels - 1f)
                py = (cy + (baseY - cy) * sens).coerceIn(0f, m.heightPixels - 1f)
                inject(pointerEvent(MotionEvent.ACTION_HOVER_MOVE, 0))
            }
            WM_LBUTTONUP -> click(MotionEvent.BUTTON_PRIMARY)
            WM_RBUTTONUP -> click(MotionEvent.BUTTON_SECONDARY)
            WM_MBUTTONUP -> click(MotionEvent.BUTTON_TERTIARY)
            WM_MOUSEWHEEL -> scroll(frame.optInt("mouseData"))
        }
    }

    private fun click(button: Int) {
        val down = SystemClock.uptimeMillis()
        inject(pointerEvent(MotionEvent.ACTION_DOWN, button, down, down))
        inject(pointerEvent(MotionEvent.ACTION_BUTTON_PRESS, button, down, down))
        inject(pointerEvent(MotionEvent.ACTION_BUTTON_RELEASE, 0, down, SystemClock.uptimeMillis()))
        inject(pointerEvent(MotionEvent.ACTION_UP, 0, down, SystemClock.uptimeMillis()))
    }

    /** Continuous trackpad-style scroll at the cursor (native ACTION_SCROLL). */
    fun scrollBy(dx: Float, dy: Float) {
        val now = SystemClock.uptimeMillis()
        val props = MotionEvent.PointerProperties().apply {
            id = 0; toolType = MotionEvent.TOOL_TYPE_MOUSE
        }
        val coords = MotionEvent.PointerCoords().apply {
            x = px; y = py; pressure = 1f; size = 1f
            // Android scroll axes: positive vscroll = away/up. Trackpad dy>0 = down.
            setAxisValue(MotionEvent.AXIS_VSCROLL, -dy / SCROLL_DIVISOR)
            setAxisValue(MotionEvent.AXIS_HSCROLL, dx / SCROLL_DIVISOR)
        }
        val ev = MotionEvent.obtain(
            now, now, MotionEvent.ACTION_SCROLL, 1,
            arrayOf(props), arrayOf(coords), 0, 0, 1f, 1f, 0, 0,
            InputDevice.SOURCE_MOUSE, 0)
        inject(ev)
        ev.recycle()
    }

    private fun scroll(mouseData: Int) {
        val now = SystemClock.uptimeMillis()
        val props = MotionEvent.PointerProperties().apply {
            id = 0; toolType = MotionEvent.TOOL_TYPE_MOUSE
        }
        val coords = MotionEvent.PointerCoords().apply {
            x = px; y = py; pressure = 1f; size = 1f
            setAxisValue(MotionEvent.AXIS_VSCROLL, (mouseData / 120f))
        }
        val ev = MotionEvent.obtain(
            now, now, MotionEvent.ACTION_SCROLL, 1,
            arrayOf(props), arrayOf(coords), 0, 0, 1f, 1f, 0, 0,
            InputDevice.SOURCE_MOUSE, 0)
        inject(ev)
        ev.recycle()
    }

    private fun pointerEvent(
        action: Int,
        buttonState: Int,
        downTime: Long = SystemClock.uptimeMillis(),
        eventTime: Long = SystemClock.uptimeMillis(),
    ): MotionEvent {
        val props = MotionEvent.PointerProperties().apply {
            id = 0; toolType = MotionEvent.TOOL_TYPE_MOUSE
        }
        val coords = MotionEvent.PointerCoords().apply {
            x = px; y = py; pressure = 1f; size = 1f
        }
        return MotionEvent.obtain(
            downTime, eventTime, action, 1,
            arrayOf(props), arrayOf(coords), 0, buttonState, 1f, 1f, 0, 0,
            InputDevice.SOURCE_MOUSE, 0)
    }

    /** Returns true if the key was injected natively; false to let a fallback handle it. */
    fun handleKeyboard(frame: JSONObject): Boolean {
        val vk = frame.optInt("vkCode")
        val flags = frame.optInt("flags")
        val up = (flags and LLKHF_UP) != 0
        trackMeta(vk, up)
        val keycode = VkMap.toAndroid(vk) ?: return false
        val now = SystemClock.uptimeMillis()
        val action = if (up) KeyEvent.ACTION_UP else KeyEvent.ACTION_DOWN
        val ev = KeyEvent(
            now, now, action, keycode, 0, metaState,
            KeyCharacterMap.VIRTUAL_KEYBOARD, 0, 0, InputDevice.SOURCE_KEYBOARD)
        return inject(ev)
    }

    private fun trackMeta(vk: Int, up: Boolean) {
        val bit = when (vk) {
            0x10 -> KeyEvent.META_SHIFT_ON or KeyEvent.META_SHIFT_LEFT_ON
            0x11 -> KeyEvent.META_CTRL_ON or KeyEvent.META_CTRL_LEFT_ON
            0x12 -> KeyEvent.META_ALT_ON or KeyEvent.META_ALT_LEFT_ON
            0x5B, 0x5C -> KeyEvent.META_META_ON or KeyEvent.META_META_LEFT_ON
            else -> return
        }
        metaState = if (up) metaState and bit.inv() else metaState or bit
    }

    companion object {
        private const val WM_MOUSEMOVE = 0x0200
        private const val WM_LBUTTONUP = 0x0202
        private const val WM_RBUTTONUP = 0x0205
        private const val WM_MBUTTONUP = 0x0208
        private const val WM_MOUSEWHEEL = 0x020A
        private const val LLKHF_UP = 0x80
        // Trackpad pixel-delta → scroll-unit scale; smaller = faster scroll.
        private const val SCROLL_DIVISOR = 40f
    }
}
