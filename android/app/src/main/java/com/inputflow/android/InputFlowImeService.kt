package com.inputflow.android

import android.inputmethodservice.InputMethodService
import android.os.Build
import android.os.Handler
import android.os.Looper
import android.os.SystemClock
import android.util.Log
import android.view.KeyEvent
import android.view.View
import org.json.JSONObject
import java.util.concurrent.CountDownLatch
import java.util.concurrent.TimeUnit
import java.util.concurrent.atomic.AtomicBoolean

class InputFlowImeService : InputMethodService() {
    private val mainHandler = Handler(Looper.getMainLooper())
    private var shiftDown = false
    private var ctrlDown = false
    private var altDown = false
    private var metaDown = false

    override fun onCreate() {
        super.onCreate()
        instance = this
    }

    override fun onDestroy() {
        if (instance === this) {
            instance = null
        }
        super.onDestroy()
    }

    override fun onEvaluateInputViewShown(): Boolean = false

    override fun onCreateInputView(): View {
        return View(this)
    }

    fun handleKeyboard(frame: JSONObject): Boolean {
        if (Looper.myLooper() == Looper.getMainLooper()) {
            return handleKeyboardOnMain(frame)
        }

        val handled = AtomicBoolean(false)
        val latch = CountDownLatch(1)
        mainHandler.post {
            handled.set(handleKeyboardOnMain(frame))
            latch.countDown()
        }
        latch.await(250, TimeUnit.MILLISECONDS)
        return handled.get()
    }

    private fun handleKeyboardOnMain(frame: JSONObject): Boolean {
        val vkCode = frame.optInt("vkCode")
        val flags = frame.optInt("flags")
        val keyUp = (flags and LLKHF_UP) != 0
        updateModifier(vkCode, keyUp)

        if (keyUp || altDown || metaDown || isCommandKey(vkCode)) {
            return false
        }

        val action = inputAction(vkCode) ?: return false
        val connection = currentInputConnection
        if (connection == null) {
            Log.i(TAG, "keyboard ignored: no current input connection")
            return false
        }
        val handled = when (action) {
            is InputAction.Commit -> connection.commitText(action.text, 1)
            InputAction.Backspace -> connection.deleteSurroundingText(1, 0)
            InputAction.Enter -> {
                connection.sendKeyEvent(KeyEvent(KeyEvent.ACTION_DOWN, KeyEvent.KEYCODE_ENTER)) &&
                    connection.sendKeyEvent(KeyEvent(KeyEvent.ACTION_UP, KeyEvent.KEYCODE_ENTER))
            }
            is InputAction.Key -> {
                connection.sendKeyEvent(KeyEvent(KeyEvent.ACTION_DOWN, action.keyCode)) &&
                    connection.sendKeyEvent(KeyEvent(KeyEvent.ACTION_UP, action.keyCode))
            }
            is InputAction.ModifiedKey -> sendModifiedKey(connection, action.keyCode, action.metaState)
            is InputAction.Menu -> connection.performContextMenuAction(action.id)
        }
        Log.i(TAG, "keyboard action=${action.name} vk=$vkCode handled=$handled")
        return handled
    }

    private fun updateModifier(vkCode: Int, keyUp: Boolean) {
        val down = !keyUp
        when (vkCode) {
            VK_SHIFT -> shiftDown = down
            VK_CONTROL -> ctrlDown = down
            VK_MENU -> altDown = down
            VK_LWIN, VK_RWIN -> metaDown = down
        }
    }

    private fun isCommandKey(vkCode: Int): Boolean {
        return vkCode == VK_ESCAPE ||
            vkCode == VK_LWIN ||
            vkCode == VK_RWIN
    }

    private fun inputAction(vkCode: Int): InputAction? {
        if (ctrlDown) {
            return when (vkCode) {
                VK_A -> InputAction.Menu(android.R.id.selectAll)
                VK_C -> InputAction.Menu(android.R.id.copy)
                VK_V -> InputAction.Menu(android.R.id.paste)
                VK_X -> InputAction.Menu(android.R.id.cut)
                VK_Z -> InputAction.ModifiedKey(KeyEvent.KEYCODE_Z, KeyEvent.META_CTRL_ON)
                VK_Y -> InputAction.ModifiedKey(KeyEvent.KEYCODE_Y, KeyEvent.META_CTRL_ON)
                else -> keyEventAction(vkCode)?.let { InputAction.ModifiedKey(it, KeyEvent.META_CTRL_ON) }
            }
        }
        return when (vkCode) {
            VK_BACK -> InputAction.Backspace
            VK_DELETE -> InputAction.Key(KeyEvent.KEYCODE_FORWARD_DEL)
            VK_RETURN -> InputAction.Enter
            VK_TAB -> InputAction.Commit("\t")
            VK_SPACE -> InputAction.Commit(" ")
            VK_LEFT -> InputAction.Key(KeyEvent.KEYCODE_DPAD_LEFT)
            VK_UP -> InputAction.Key(KeyEvent.KEYCODE_DPAD_UP)
            VK_RIGHT -> InputAction.Key(KeyEvent.KEYCODE_DPAD_RIGHT)
            VK_DOWN -> InputAction.Key(KeyEvent.KEYCODE_DPAD_DOWN)
            VK_HOME -> InputAction.Key(KeyEvent.KEYCODE_MOVE_HOME)
            VK_END -> InputAction.Key(KeyEvent.KEYCODE_MOVE_END)
            VK_PRIOR -> InputAction.Key(KeyEvent.KEYCODE_PAGE_UP)
            VK_NEXT -> InputAction.Key(KeyEvent.KEYCODE_PAGE_DOWN)
            in VK_0..VK_9 -> InputAction.Commit(keyChar(vkCode))
            in VK_A..VK_Z -> InputAction.Commit(keyChar(vkCode))
            in VK_NUMPAD0..VK_NUMPAD9 -> InputAction.Commit((vkCode - VK_NUMPAD0).toString())
            VK_MULTIPLY, VK_ADD, VK_SUBTRACT, VK_DECIMAL, VK_DIVIDE -> InputAction.Commit(keyChar(vkCode))
            VK_OEM_COMMA, VK_OEM_PERIOD, VK_OEM_MINUS, VK_OEM_PLUS,
            VK_OEM_1, VK_OEM_2, VK_OEM_3, VK_OEM_4, VK_OEM_5, VK_OEM_6, VK_OEM_7 ->
                InputAction.Commit(keyChar(vkCode))
            else -> null
        }
    }

    private fun keyEventAction(vkCode: Int): Int? {
        return when (vkCode) {
            VK_TAB -> KeyEvent.KEYCODE_TAB
            VK_LEFT -> KeyEvent.KEYCODE_DPAD_LEFT
            VK_UP -> KeyEvent.KEYCODE_DPAD_UP
            VK_RIGHT -> KeyEvent.KEYCODE_DPAD_RIGHT
            VK_DOWN -> KeyEvent.KEYCODE_DPAD_DOWN
            VK_HOME -> KeyEvent.KEYCODE_MOVE_HOME
            VK_END -> KeyEvent.KEYCODE_MOVE_END
            VK_PRIOR -> KeyEvent.KEYCODE_PAGE_UP
            VK_NEXT -> KeyEvent.KEYCODE_PAGE_DOWN
            in VK_A..VK_Z -> KeyEvent.KEYCODE_A + (vkCode - VK_A)
            else -> null
        }
    }

    private fun sendModifiedKey(connection: android.view.inputmethod.InputConnection, keyCode: Int, metaState: Int): Boolean {
        val now = SystemClock.uptimeMillis()
        return connection.sendKeyEvent(KeyEvent(now, now, KeyEvent.ACTION_DOWN, keyCode, 0, metaState)) &&
            connection.sendKeyEvent(KeyEvent(now, now, KeyEvent.ACTION_UP, keyCode, 0, metaState))
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
        if (vkCode in VK_NUMPAD0..VK_NUMPAD9) {
            return (vkCode - VK_NUMPAD0).toString()
        }
        return when (vkCode) {
            VK_MULTIPLY -> "*"
            VK_ADD -> "+"
            VK_SUBTRACT -> "-"
            VK_DECIMAL -> "."
            VK_DIVIDE -> "/"
            VK_OEM_COMMA -> if (shifted) "<" else ","
            VK_OEM_PERIOD -> if (shifted) ">" else "."
            VK_OEM_MINUS -> if (shifted) "_" else "-"
            VK_OEM_PLUS -> if (shifted) "+" else "="
            VK_OEM_1 -> if (shifted) ":" else ";"
            VK_OEM_2 -> if (shifted) "?" else "/"
            VK_OEM_3 -> if (shifted) "~" else "`"
            VK_OEM_4 -> if (shifted) "{" else "["
            VK_OEM_5 -> if (shifted) "|" else "\\"
            VK_OEM_6 -> if (shifted) "}" else "]"
            VK_OEM_7 -> if (shifted) "\"" else "'"
            else -> ""
        }
    }

    private sealed class InputAction {
        abstract val name: String

        data class Commit(val text: String) : InputAction() {
            override val name = "commit"
        }

        object Backspace : InputAction() {
            override val name = "backspace"
        }

        object Enter : InputAction() {
            override val name = "enter"
        }

        data class Key(val keyCode: Int) : InputAction() {
            override val name = "key"
        }

        data class ModifiedKey(val keyCode: Int, val metaState: Int) : InputAction() {
            override val name = "modified-key"
        }

        data class Menu(val id: Int) : InputAction() {
            override val name = "menu"
        }
    }

    companion object {
        private const val TAG = "InputFlowIme"
        private const val LLKHF_UP = 0x80
        private const val VK_BACK = 0x08
        private const val VK_TAB = 0x09
        private const val VK_RETURN = 0x0D
        private const val VK_SHIFT = 0x10
        private const val VK_CONTROL = 0x11
        private const val VK_MENU = 0x12
        private const val VK_ESCAPE = 0x1B
        private const val VK_SPACE = 0x20
        private const val VK_PRIOR = 0x21
        private const val VK_NEXT = 0x22
        private const val VK_END = 0x23
        private const val VK_HOME = 0x24
        private const val VK_LEFT = 0x25
        private const val VK_UP = 0x26
        private const val VK_RIGHT = 0x27
        private const val VK_DOWN = 0x28
        private const val VK_DELETE = 0x2E
        private const val VK_0 = 0x30
        private const val VK_9 = 0x39
        private const val VK_A = 0x41
        private const val VK_C = 0x43
        private const val VK_V = 0x56
        private const val VK_X = 0x58
        private const val VK_Y = 0x59
        private const val VK_Z = 0x5A
        private const val VK_LWIN = 0x5B
        private const val VK_RWIN = 0x5C
        private const val VK_NUMPAD0 = 0x60
        private const val VK_NUMPAD9 = 0x69
        private const val VK_MULTIPLY = 0x6A
        private const val VK_ADD = 0x6B
        private const val VK_SUBTRACT = 0x6D
        private const val VK_DECIMAL = 0x6E
        private const val VK_DIVIDE = 0x6F
        private const val VK_OEM_1 = 0xBA
        private const val VK_OEM_PLUS = 0xBB
        private const val VK_OEM_COMMA = 0xBC
        private const val VK_OEM_MINUS = 0xBD
        private const val VK_OEM_PERIOD = 0xBE
        private const val VK_OEM_2 = 0xBF
        private const val VK_OEM_3 = 0xC0
        private const val VK_OEM_4 = 0xDB
        private const val VK_OEM_5 = 0xDC
        private const val VK_OEM_6 = 0xDD
        private const val VK_OEM_7 = 0xDE

        @Volatile
        var instance: InputFlowImeService? = null
            private set

        fun restorePreviousKeyboard(): Boolean {
            val service = instance ?: return false
            service.mainHandler.post {
                if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.P) {
                    val handled = service.switchToPreviousInputMethod()
                    Log.i(TAG, "restore previous keyboard handled=$handled")
                } else {
                    service.requestHideSelf(0)
                    Log.i(TAG, "restore previous keyboard unavailable on this Android version")
                }
            }
            return true
        }
    }
}
