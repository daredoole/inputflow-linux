package com.inputflow.android

import android.accessibilityservice.AccessibilityService
import android.accessibilityservice.GestureDescription
import android.content.Context
import android.graphics.Color
import android.graphics.Path
import android.graphics.PixelFormat
import android.graphics.drawable.GradientDrawable
import android.os.Bundle
import android.os.Handler
import android.os.Looper
import android.media.AudioManager
import android.os.Build
import android.os.PowerManager
import android.util.Log
import android.view.Gravity
import android.view.View
import android.view.WindowManager
import android.view.accessibility.AccessibilityEvent
import android.view.accessibility.AccessibilityNodeInfo
import org.json.JSONObject
import kotlin.math.abs
import kotlin.math.max
import kotlin.math.min

class InputFlowAccessibilityService : AccessibilityService() {
    private val mainHandler = Handler(Looper.getMainLooper())
    private var pointerX = 0f
    private var pointerY = 0f
    private var cursorView: View? = null
    private var cursorParams: WindowManager.LayoutParams? = null
    private var windowManager: WindowManager? = null
    private var shiftDown = false
    private var ctrlDown = false
    private var altDown = false
    private var metaDown = false
    private var pendingScrollDx = 0.0
    private var pendingScrollDy = 0.0
    private var scrollFlushScheduled = false
    @Suppress("DEPRECATION")
    private var wakeLock: PowerManager.WakeLock? = null

    override fun onServiceConnected() {
        instance = this
        val metrics = resources.displayMetrics
        pointerX = metrics.widthPixels / 2f
        pointerY = metrics.heightPixels / 2f
        @Suppress("DEPRECATION")
        wakeLock = (getSystemService(PowerManager::class.java))
            .newWakeLock(
                PowerManager.FULL_WAKE_LOCK or PowerManager.ACQUIRE_CAUSES_WAKEUP or PowerManager.ON_AFTER_RELEASE,
                "inputflow:input"
            )
    }

    override fun onAccessibilityEvent(event: AccessibilityEvent?) = Unit

    override fun onInterrupt() = Unit

    override fun onDestroy() {
        hideCursorOverlay()
        wakeLock?.let { if (it.isHeld) it.release() }
        if (instance === this) instance = null
        super.onDestroy()
    }

    fun handleMouse(frame: JSONObject) {
        ensureScreenOn()
        val wParam = frame.optInt("wParam")
        val x = frame.optInt("x")
        val y = frame.optInt("y")
        val mouseData = frame.optInt("mouseData")
        when (wParam) {
            WM_MOUSEMOVE -> moveToNormalized(x, y)
            WM_LBUTTONUP -> tap(pointerX, pointerY)
            WM_RBUTTONUP -> mainHandler.post { performGlobalAction(GLOBAL_ACTION_BACK) }
            WM_MBUTTONUP -> mainHandler.post { performGlobalAction(GLOBAL_ACTION_HOME) }
            WM_MOUSEWHEEL -> scroll(mouseData)
        }
    }

    fun setRemoteControlActive(active: Boolean) {
        mainHandler.post {
            if (active) {
                showCursorOverlay()
            } else {
                pendingScrollDx = 0.0
                pendingScrollDy = 0.0
                scrollFlushScheduled = false
                shiftDown = false
                ctrlDown = false
                altDown = false
                metaDown = false
                hideCursorOverlay()
            }
        }
    }

    fun handleGesture(frame: JSONObject) {
        val dx = frame.optDouble("dx")
        val dy = frame.optDouble("dy")
        ensureScreenOn()
        when (frame.optString("kind")) {
            "scroll" -> touchpadScroll(dx, dy)
            "swipe2" -> handleSwipe2(dx, dy)
            "swipe3" -> handleSwipe3(dx, dy)
            "swipe4" -> handleSwipe4(dx, dy)
            "pinch"  -> handlePinch(dx)
            "zoom"   -> handlePinch(dx)
            "tap2"   -> handleTap2()
        }
    }

    fun handleKeyboard(frame: JSONObject) {
        ensureScreenOn()
        mainHandler.post {
            val vkCode = frame.optInt("vkCode")
            val flags = frame.optInt("flags")
            val keyUp = (flags and LLKHF_UP) != 0
            updateModifier(vkCode, keyUp)
            if (keyUp && handleCommandKeyUp(vkCode)) return@post
            if (keyUp) return@post
            if (checkKeyMap(vkCode)) return@post
            if (handleCommandKey(vkCode)) return@post
            when (vkCode) {
                VK_BACK   -> editFocusedText { it.dropLast(1) }
                VK_ESCAPE -> performGlobalAction(GLOBAL_ACTION_BACK)
                VK_RETURN -> editFocusedText { "$it\n" }
                VK_SPACE  -> editFocusedText { "$it " }
                VK_LEFT   -> navigateDirection(forward = false, vertical = false)
                VK_UP     -> navigateDirection(forward = false, vertical = true)
                VK_RIGHT  -> navigateDirection(forward = true,  vertical = false)
                VK_DOWN   -> navigateDirection(forward = true,  vertical = true)
                VK_DELETE -> forwardDelete()
                VK_TAB    -> navigateFocus(!shiftDown)
                VK_HOME   -> moveCursorToLineEdge(toEnd = false)
                VK_END    -> moveCursorToLineEdge(toEnd = true)
                VK_PRIOR  -> pageScroll(false)
                VK_NEXT   -> pageScroll(true)
                in VK_0..VK_9 -> appendText(keyChar(vkCode))
                in VK_A..VK_Z -> appendText(keyChar(vkCode))
                VK_OEM_COMMA, VK_OEM_PERIOD, VK_OEM_MINUS, VK_OEM_PLUS,
                VK_OEM_1, VK_OEM_2, VK_OEM_3, VK_OEM_4, VK_OEM_5, VK_OEM_6, VK_OEM_7 ->
                    appendText(keyChar(vkCode))
            }
        }
    }

    fun handleMappedKeyboard(frame: JSONObject): Boolean {
        val vkCode = frame.optInt("vkCode")
        val flags = frame.optInt("flags")
        if ((flags and LLKHF_UP) != 0 || isModifierKey(vkCode)) {
            return false
        }
        val action = findMappedAction(vkCode) ?: return false
        mainHandler.post {
            executeMappedAction(action)
        }
        return true
    }

    private fun ensureScreenOn() {
        val pm = getSystemService(PowerManager::class.java)
        if (!pm.isInteractive) {
            wakeLock?.acquire(5000L)
        }
    }

    private fun updateModifier(vkCode: Int, keyUp: Boolean) {
        val down = !keyUp
        when (vkCode) {
            VK_SHIFT   -> shiftDown = down
            VK_CONTROL -> ctrlDown = down
            VK_MENU    -> altDown = down
            VK_LWIN, VK_RWIN -> metaDown = down
        }
    }

    private fun isModifierKey(vkCode: Int): Boolean {
        return vkCode == VK_SHIFT ||
            vkCode == VK_CONTROL ||
            vkCode == VK_MENU ||
            vkCode == VK_LWIN ||
            vkCode == VK_RWIN
    }

    private fun checkKeyMap(vkCode: Int): Boolean {
        val action = findMappedAction(vkCode) ?: return false
        executeMappedAction(action)
        return true
    }

    private fun findMappedAction(vkCode: Int): KeyMap.Action? {
        val prefs = applicationContext.getSharedPreferences(RelayForegroundService.PREFS, MODE_PRIVATE)
        val mods = KeyMap.currentMods(shiftDown, ctrlDown, altDown, metaDown)
        return KeyMap.ACTIONS.firstOrNull { action ->
            KeyMap.getVk(prefs, action) == vkCode && KeyMap.getMods(prefs, action) == mods
        }
    }

    private fun executeMappedAction(action: KeyMap.Action) {
        when (action.id) {
            "back" -> performGlobalActionLogged(GLOBAL_ACTION_BACK, "map:${action.id}")
            "home" -> performGlobalActionLogged(GLOBAL_ACTION_HOME, "map:${action.id}")
            "recents" -> performGlobalActionLogged(GLOBAL_ACTION_RECENTS, "map:${action.id}")
            "notifications" -> performGlobalActionLogged(GLOBAL_ACTION_NOTIFICATIONS, "map:${action.id}")
            "quick_settings" -> performGlobalActionLogged(GLOBAL_ACTION_QUICK_SETTINGS, "map:${action.id}")
            "volume_up" -> adjustVolume(AudioManager.ADJUST_RAISE)
            "volume_down" -> adjustVolume(AudioManager.ADJUST_LOWER)
            "screenshot" -> takeScreenshotAction()
            "swipe_left" -> dispatchDirectionalSwipe(-1f, 0f)
            "swipe_right" -> dispatchDirectionalSwipe(1f, 0f)
            "swipe_up" -> dispatchDirectionalSwipe(0f, -1f)
            "swipe_down" -> dispatchDirectionalSwipe(0f, 1f)
            "zoom_in" -> handlePinch(1.25)
            "zoom_out" -> handlePinch(0.75)
        }
    }

    private fun adjustVolume(direction: Int) {
        getSystemService(AudioManager::class.java)
            ?.adjustStreamVolume(AudioManager.STREAM_MUSIC, direction, AudioManager.FLAG_SHOW_UI)
    }

    private fun takeScreenshotAction() {
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.P) {
            performGlobalActionLogged(GLOBAL_ACTION_TAKE_SCREENSHOT, "map:screenshot")
        }
    }

    private fun dispatchDirectionalSwipe(dx: Float, dy: Float) {
        val metrics = resources.displayMetrics
        val distance = 260f * metrics.density
        val fromX = pointerX.coerceIn(1f, metrics.widthPixels - 2f)
        val fromY = pointerY.coerceIn(1f, metrics.heightPixels - 2f)
        val toX = (fromX + dx * distance).coerceIn(1f, metrics.widthPixels - 2f)
        val toY = (fromY + dy * distance).coerceIn(1f, metrics.heightPixels - 2f)
        gesture(fromX, fromY, toX, toY, 180)
    }

    private fun handleCommandKey(vkCode: Int): Boolean {
        if (vkCode == VK_LWIN || vkCode == VK_RWIN) {
            performGlobalActionLogged(GLOBAL_ACTION_HOME, "meta-down")
            return true
        }
        if (vkCode == VK_TAB && altDown) {
            performGlobalActionLogged(GLOBAL_ACTION_RECENTS, "alt-tab")
            return true
        }
        if (vkCode == VK_LEFT && altDown) {
            performGlobalActionLogged(GLOBAL_ACTION_BACK, "alt-left")
            return true
        }
        if (vkCode == VK_N && ctrlDown && altDown) {
            performGlobalActionLogged(GLOBAL_ACTION_NOTIFICATIONS, "ctrl-alt-n")
            return true
        }
        if (vkCode == VK_Q && ctrlDown && altDown) {
            performGlobalActionLogged(GLOBAL_ACTION_QUICK_SETTINGS, "ctrl-alt-q")
            return true
        }
        return false
    }

    private fun handleCommandKeyUp(vkCode: Int): Boolean {
        if (vkCode == VK_LWIN || vkCode == VK_RWIN) {
            performGlobalActionLogged(GLOBAL_ACTION_HOME, "meta-up")
            return true
        }
        return false
    }

    private fun performGlobalActionLogged(action: Int, source: String): Boolean {
        val handled = performGlobalAction(action)
        Log.i(TAG, "global action source=$source action=$action handled=$handled")
        return handled
    }

    // ── Gesture handlers ─────────────────────────────────────────────────────

    private fun handleSwipe2(dx: Double, dy: Double) {
        touchpadScroll(dx, dy)
    }

    private fun handleSwipe3(dx: Double, dy: Double) {
        if (!isAccurateVerticalSwipe(dx, dy)) return
        mainHandler.post {
            if (dy < 0) {
                performGlobalActionLogged(GLOBAL_ACTION_HOME, "gesture:3-up")
            } else {
                performGlobalActionLogged(GLOBAL_ACTION_RECENTS, "gesture:3-down")
            }
        }
    }

    private fun handleSwipe4(dx: Double, dy: Double) {
        if (!isAccurateVerticalSwipe(dx, dy)) return
        mainHandler.post {
            if (dy < 0) {
                openAppDrawerGesture()
            } else {
                performGlobalActionLogged(GLOBAL_ACTION_QUICK_SETTINGS, "gesture:4-down")
            }
        }
    }

    private fun isAccurateVerticalSwipe(dx: Double, dy: Double): Boolean {
        val absX = abs(dx)
        val absY = abs(dy)
        return absY >= 32.0 && absY >= absX * 1.45
    }

    private fun openAppDrawerGesture() {
        performGlobalActionLogged(GLOBAL_ACTION_HOME, "gesture:4-up-home")
        mainHandler.postDelayed({
            val metrics = resources.displayMetrics
            val x = metrics.widthPixels / 2f
            val fromY = metrics.heightPixels * 0.88f
            val toY = metrics.heightPixels * 0.28f
            gesture(x, fromY, x, toY, 260)
            Log.i(TAG, "global action source=gesture:4-up action=app-drawer handled=true")
        }, 220)
    }

    private fun handlePinch(scale: Double) {
        val metrics = resources.displayMetrics
        val cx = pointerX
        val cy = pointerY
        val baseR = 120f * metrics.density
        val pinchIn = scale < 1.0
        val startR = if (pinchIn) baseR else baseR * 0.4f
        val endR   = if (pinchIn) baseR * 0.4f else baseR
        val path1 = Path().apply { moveTo(cx - startR, cy); lineTo(cx - endR, cy) }
        val path2 = Path().apply { moveTo(cx + startR, cy); lineTo(cx + endR, cy) }
        mainHandler.post {
            dispatchGesture(
                GestureDescription.Builder()
                    .addStroke(GestureDescription.StrokeDescription(path1, 0, 300))
                    .addStroke(GestureDescription.StrokeDescription(path2, 0, 300))
                    .build(),
                null, null
            )
        }
    }

    private fun handleTap2() {
        mainHandler.post {
            val path = Path().apply { moveTo(pointerX, pointerY) }
            dispatchGesture(
                GestureDescription.Builder()
                    .addStroke(GestureDescription.StrokeDescription(path, 0, 600L))
                    .build(),
                null, null
            )
        }
    }

    // ── Keyboard navigation helpers ──────────────────────────────────────────

    private fun navigateDirection(forward: Boolean, vertical: Boolean) {
        val root = rootInActiveWindow
        val focused = root?.findFocus(AccessibilityNodeInfo.FOCUS_INPUT)
            ?.takeIf { it.isEditable }
            ?: findFocusedNode(root)
        if (focused != null) {
            val granularity = if (vertical) AccessibilityNodeInfo.MOVEMENT_GRANULARITY_LINE
                              else AccessibilityNodeInfo.MOVEMENT_GRANULARITY_CHARACTER
            val action = if (forward) AccessibilityNodeInfo.ACTION_NEXT_AT_MOVEMENT_GRANULARITY
                         else AccessibilityNodeInfo.ACTION_PREVIOUS_AT_MOVEMENT_GRANULARITY
            focused.performAction(action, Bundle().apply {
                putInt(AccessibilityNodeInfo.ACTION_ARGUMENT_MOVEMENT_GRANULARITY_INT, granularity)
                putBoolean(AccessibilityNodeInfo.ACTION_ARGUMENT_EXTEND_SELECTION_BOOLEAN, shiftDown)
            })
        } else {
            // No text field: directional swipe
            val m = resources.displayMetrics
            val d = 240f * m.density
            val (dx, dy) = if (!vertical) Pair(if (forward) d else -d, 0f)
                           else Pair(0f, if (forward) d else -d)
            val toX = (pointerX + dx).coerceIn(0f, m.widthPixels - 1f)
            val toY = (pointerY + dy).coerceIn(0f, m.heightPixels - 1f)
            gesture(pointerX, pointerY, toX, toY, 150)
        }
    }

    private fun forwardDelete() {
        val root = rootInActiveWindow
        val focused = root?.findFocus(AccessibilityNodeInfo.FOCUS_INPUT)
            ?.takeIf { it.isEditable }
            ?: findFocusedNode(root)
        if (focused == null) return
        val text = focused.text?.toString() ?: return
        val selStart = focused.textSelectionStart
        val selEnd = focused.textSelectionEnd
        if (selStart < 0) return
        val newText = if (selStart != selEnd) {
            text.removeRange(minOf(selStart, selEnd), maxOf(selStart, selEnd))
        } else {
            if (selStart >= text.length) return
            text.removeRange(selStart, selStart + 1)
        }
        val cursor = if (selStart != selEnd) minOf(selStart, selEnd) else selStart
        focused.performAction(AccessibilityNodeInfo.ACTION_SET_TEXT, Bundle().apply {
            putCharSequence(AccessibilityNodeInfo.ACTION_ARGUMENT_SET_TEXT_CHARSEQUENCE, newText)
        })
        focused.performAction(AccessibilityNodeInfo.ACTION_SET_SELECTION, Bundle().apply {
            putInt(AccessibilityNodeInfo.ACTION_ARGUMENT_SELECTION_START_INT, cursor)
            putInt(AccessibilityNodeInfo.ACTION_ARGUMENT_SELECTION_END_INT, cursor)
        })
    }

    private fun navigateFocus(forward: Boolean) {
        val root = rootInActiveWindow ?: return
        val focusables = mutableListOf<AccessibilityNodeInfo>()
        collectFocusable(root, focusables)
        if (focusables.isEmpty()) return
        val current = root.findFocus(AccessibilityNodeInfo.FOCUS_INPUT)
            ?: root.findFocus(AccessibilityNodeInfo.FOCUS_ACCESSIBILITY)
        val idx = if (current != null) focusables.indexOfFirst { it == current } else -1
        val next = if (forward) {
            focusables.getOrNull(if (idx + 1 < focusables.size) idx + 1 else 0)
        } else {
            focusables.getOrNull(if (idx - 1 >= 0) idx - 1 else focusables.size - 1)
        }
        next?.performAction(AccessibilityNodeInfo.ACTION_ACCESSIBILITY_FOCUS)
    }

    private fun collectFocusable(node: AccessibilityNodeInfo?, out: MutableList<AccessibilityNodeInfo>) {
        if (node == null) return
        if (node.isFocusable && node.isVisibleToUser) out.add(node)
        for (i in 0 until node.childCount) collectFocusable(node.getChild(i), out)
    }

    private fun moveCursorToLineEdge(toEnd: Boolean) {
        val root = rootInActiveWindow
        val focused = root?.findFocus(AccessibilityNodeInfo.FOCUS_INPUT)
            ?.takeIf { it.isEditable }
            ?: findFocusedNode(root)
        if (focused == null) return
        val action = if (toEnd) AccessibilityNodeInfo.ACTION_NEXT_AT_MOVEMENT_GRANULARITY
                     else AccessibilityNodeInfo.ACTION_PREVIOUS_AT_MOVEMENT_GRANULARITY
        focused.performAction(action, Bundle().apply {
            putInt(AccessibilityNodeInfo.ACTION_ARGUMENT_MOVEMENT_GRANULARITY_INT,
                AccessibilityNodeInfo.MOVEMENT_GRANULARITY_LINE)
            putBoolean(AccessibilityNodeInfo.ACTION_ARGUMENT_EXTEND_SELECTION_BOOLEAN, shiftDown)
        })
    }

    private fun pageScroll(down: Boolean) {
        val metrics = resources.displayMetrics
        val dist = metrics.heightPixels * 0.75f
        val fromY = if (down) metrics.heightPixels * 0.75f else metrics.heightPixels * 0.25f
        val toY   = if (down) metrics.heightPixels * 0.25f else metrics.heightPixels * 0.75f
        gesture(pointerX, fromY, pointerX, toY, 250)
    }

    // ── Text helpers ─────────────────────────────────────────────────────────

    private fun appendText(value: String) {
        if (value.isEmpty()) return
        editFocusedText { current -> current + value }
    }

    private fun keyChar(vkCode: Int): String {
        val shifted = shiftDown
        if (vkCode in VK_A..VK_Z) {
            val c = vkCode.toChar()
            return if (shifted) c.toString() else c.lowercaseChar().toString()
        }
        if (vkCode in VK_0..VK_9) {
            val normal = "0123456789"[vkCode - VK_0]
            val shiftedChars = ")!@#$%^&*("[vkCode - VK_0]
            return (if (shifted) shiftedChars else normal).toString()
        }
        return when (vkCode) {
            VK_OEM_COMMA  -> if (shifted) "<" else ","
            VK_OEM_PERIOD -> if (shifted) ">" else "."
            VK_OEM_MINUS  -> if (shifted) "_" else "-"
            VK_OEM_PLUS   -> if (shifted) "+" else "="
            VK_OEM_1      -> if (shifted) ":" else ";"
            VK_OEM_2      -> if (shifted) "?" else "/"
            VK_OEM_3      -> if (shifted) "~" else "`"
            VK_OEM_4      -> if (shifted) "{" else "["
            VK_OEM_5      -> if (shifted) "|" else "\\"
            VK_OEM_6      -> if (shifted) "}" else "]"
            VK_OEM_7      -> if (shifted) "\"" else "'"
            else -> ""
        }
    }

    // ── Pointer + cursor ─────────────────────────────────────────────────────

    private fun moveToNormalized(x: Int, y: Int) {
        val prefs = applicationContext.getSharedPreferences(RelayForegroundService.PREFS, MODE_PRIVATE)
        val sens = prefs.getFloat(SettingsActivity.KEY_SENSITIVITY, 1.0f).coerceIn(0.5f, 3.0f)
        val metrics = resources.displayMetrics
        val baseX = x / 65535f * metrics.widthPixels
        val baseY = y / 65535f * metrics.heightPixels
        val cx = metrics.widthPixels / 2f
        val cy = metrics.heightPixels / 2f
        val nextX = min(metrics.widthPixels - 1f, max(0f, cx + (baseX - cx) * sens))
        val nextY = min(metrics.heightPixels - 1f, max(0f, cy + (baseY - cy) * sens))
        mainHandler.post {
            pointerX = nextX
            pointerY = nextY
            updateCursorOverlay()
        }
    }

    private fun tap(x: Float, y: Float) {
        mainHandler.post { gesture(x, y, x, y, 40) }
    }

    private fun gesture(fromX: Float, fromY: Float, toX: Float, toY: Float, durationMs: Long) {
        val path = Path().apply {
            moveTo(fromX, fromY)
            lineTo(toX, toY)
        }
        val stroke = GestureDescription.StrokeDescription(path, 0, durationMs)
        dispatchGesture(GestureDescription.Builder().addStroke(stroke).build(), null, null)
    }

    private fun scroll(mouseData: Int) {
        val dy = if (mouseData < 0) 1.0 else -1.0
        touchpadScroll(0.0, dy)
    }

    private fun touchpadScroll(dx: Double, dy: Double) {
        if (abs(dx) < 0.01 && abs(dy) < 0.01) return
        mainHandler.post {
            pendingScrollDx += dx
            pendingScrollDy += dy
            if (!scrollFlushScheduled) {
                scrollFlushScheduled = true
                mainHandler.postDelayed({ flushTouchpadScroll() }, 48)
            }
        }
    }

    private fun flushTouchpadScroll() {
        scrollFlushScheduled = false
        val dx = pendingScrollDx
        val dy = pendingScrollDy
        pendingScrollDx = 0.0
        pendingScrollDy = 0.0
        if (abs(dx) < 0.05 && abs(dy) < 0.05) return
        val metrics = resources.displayMetrics
        val scale = 34f * metrics.density
        val maxStep = 170f * metrics.density
        val stepX = (dx * scale).toFloat().coerceIn(-maxStep, maxStep)
        val stepY = (dy * scale).toFloat().coerceIn(-maxStep, maxStep)
        val toX = min(metrics.widthPixels - 1f, max(0f, pointerX - stepX))
        val toY = min(metrics.heightPixels - 1f, max(0f, pointerY - stepY))
        gesture(pointerX, pointerY, toX, toY, 62)
    }

    private fun showCursorOverlay() {
        if (cursorView != null) return
        val prefs = applicationContext.getSharedPreferences(RelayForegroundService.PREFS, MODE_PRIVATE)
        val sizeDp = when (prefs.getInt(SettingsActivity.KEY_CURSOR_SIZE, 2)) {
            1 -> 14f
            3 -> 26f
            else -> 18f
        }
        val cursorColor = when (prefs.getString(SettingsActivity.KEY_CURSOR_COLOR, "white")) {
            "blue"  -> Color.argb(220, 22, 100, 192)
            "red"   -> Color.argb(220, 244, 67, 54)
            "black" -> Color.argb(220, 20, 20, 20)
            else    -> Color.argb(210, 255, 255, 255)
        }
        val strokeColor = if (prefs.getString(SettingsActivity.KEY_CURSOR_COLOR, "white") == "white")
            Color.argb(180, 0, 0, 0) else Color.argb(180, 255, 255, 255)
        val size = (sizeDp * resources.displayMetrics.density).toInt().coerceAtLeast(12)
        val view = View(this).apply {
            background = GradientDrawable().apply {
                shape = GradientDrawable.OVAL
                setColor(cursorColor)
                setStroke((2 * resources.displayMetrics.density).toInt().coerceAtLeast(2), strokeColor)
            }
            elevation = 20f
        }
        val params = WindowManager.LayoutParams(
            size, size,
            WindowManager.LayoutParams.TYPE_ACCESSIBILITY_OVERLAY,
            WindowManager.LayoutParams.FLAG_NOT_FOCUSABLE or
                WindowManager.LayoutParams.FLAG_NOT_TOUCHABLE or
                WindowManager.LayoutParams.FLAG_LAYOUT_NO_LIMITS,
            PixelFormat.TRANSLUCENT
        ).apply {
            gravity = Gravity.TOP or Gravity.START
        }
        windowManager = getSystemService(WindowManager::class.java)
        cursorView = view
        cursorParams = params
        windowManager?.addView(view, params)
        updateCursorOverlay()
    }

    private fun updateCursorOverlay() {
        val view = cursorView ?: return
        val params = cursorParams ?: return
        params.x = (pointerX - params.width / 2f).toInt()
        params.y = (pointerY - params.height / 2f).toInt()
        windowManager?.updateViewLayout(view, params)
    }

    private fun hideCursorOverlay() {
        val view = cursorView ?: return
        try { windowManager?.removeView(view) } catch (_: IllegalArgumentException) {}
        cursorView = null
        cursorParams = null
    }

    private fun editFocusedText(transform: (String) -> String) {
        val root = rootInActiveWindow
        val focused = root
            ?.findFocus(AccessibilityNodeInfo.FOCUS_INPUT)
            ?.takeIf { it.isEditable }
            ?: findFocusedNode(root)
        if (focused == null) {
            Log.i(TAG, "keyboard text ignored: no focused editable node")
            return
        }
        val current = focused.text?.toString().orEmpty()
        val args = Bundle().apply {
            putCharSequence(AccessibilityNodeInfo.ACTION_ARGUMENT_SET_TEXT_CHARSEQUENCE, transform(current))
        }
        if (!focused.performAction(AccessibilityNodeInfo.ACTION_SET_TEXT, args)) {
            Log.i(TAG, "keyboard text ignored: ACTION_SET_TEXT failed")
        }
    }

    private fun findFocusedNode(node: AccessibilityNodeInfo?): AccessibilityNodeInfo? {
        if (node == null) return null
        if (node.isFocused && node.isEditable) return node
        for (index in 0 until node.childCount) {
            val found = findFocusedNode(node.getChild(index))
            if (found != null) return found
        }
        return null
    }

    companion object {
        private const val TAG = "InputFlowAccessibility"
        private const val WM_MOUSEMOVE = 0x0200
        private const val WM_LBUTTONUP = 0x0202
        private const val WM_RBUTTONUP = 0x0205
        private const val WM_MBUTTONUP = 0x0208
        private const val WM_MOUSEWHEEL = 0x020A
        private const val LLKHF_UP = 0x80
        private const val VK_BACK    = 0x08
        private const val VK_TAB     = 0x09
        private const val VK_RETURN  = 0x0D
        private const val VK_SHIFT   = 0x10
        private const val VK_CONTROL = 0x11
        private const val VK_MENU    = 0x12
        private const val VK_ESCAPE  = 0x1B
        private const val VK_PRIOR   = 0x21  // PageUp
        private const val VK_NEXT    = 0x22  // PageDown
        private const val VK_END     = 0x23
        private const val VK_HOME    = 0x24
        private const val VK_LEFT    = 0x25
        private const val VK_UP      = 0x26
        private const val VK_RIGHT   = 0x27
        private const val VK_DOWN    = 0x28
        private const val VK_SPACE   = 0x20
        private const val VK_DELETE  = 0x2E
        private const val VK_0  = 0x30
        private const val VK_9  = 0x39
        private const val VK_A  = 0x41
        private const val VK_N  = 0x4E
        private const val VK_Q  = 0x51
        private const val VK_Z  = 0x5A
        private const val VK_LWIN = 0x5B
        private const val VK_RWIN = 0x5C
        private const val VK_OEM_1      = 0xBA
        private const val VK_OEM_PLUS   = 0xBB
        private const val VK_OEM_COMMA  = 0xBC
        private const val VK_OEM_MINUS  = 0xBD
        private const val VK_OEM_PERIOD = 0xBE
        private const val VK_OEM_2 = 0xBF
        private const val VK_OEM_3 = 0xC0
        private const val VK_OEM_4 = 0xDB
        private const val VK_OEM_5 = 0xDC
        private const val VK_OEM_6 = 0xDD
        private const val VK_OEM_7 = 0xDE

        @Volatile
        var instance: InputFlowAccessibilityService? = null
            private set
    }
}
