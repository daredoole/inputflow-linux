package com.inputflow.android

import android.view.KeyEvent

/** Maps Windows virtual-key codes (sent by the relay) to Android key codes for
 *  native KeyEvent injection. Covers the common typing + navigation set. */
object VkMap {
    private val map: Map<Int, Int> = buildMap {
        // Letters A-Z (VK 0x41-0x5A → KEYCODE_A..Z)
        for (i in 0..25) put(0x41 + i, KeyEvent.KEYCODE_A + i)
        // Digits 0-9 (VK 0x30-0x39 → KEYCODE_0..9)
        for (i in 0..9) put(0x30 + i, KeyEvent.KEYCODE_0 + i)
        put(0x08, KeyEvent.KEYCODE_DEL)        // Backspace
        put(0x09, KeyEvent.KEYCODE_TAB)
        put(0x0D, KeyEvent.KEYCODE_ENTER)
        put(0x10, KeyEvent.KEYCODE_SHIFT_LEFT)
        put(0x11, KeyEvent.KEYCODE_CTRL_LEFT)
        put(0x12, KeyEvent.KEYCODE_ALT_LEFT)
        put(0x1B, KeyEvent.KEYCODE_ESCAPE)
        put(0x20, KeyEvent.KEYCODE_SPACE)
        put(0x21, KeyEvent.KEYCODE_PAGE_UP)
        put(0x22, KeyEvent.KEYCODE_PAGE_DOWN)
        put(0x23, KeyEvent.KEYCODE_MOVE_END)
        put(0x24, KeyEvent.KEYCODE_MOVE_HOME)
        put(0x25, KeyEvent.KEYCODE_DPAD_LEFT)
        put(0x26, KeyEvent.KEYCODE_DPAD_UP)
        put(0x27, KeyEvent.KEYCODE_DPAD_RIGHT)
        put(0x28, KeyEvent.KEYCODE_DPAD_DOWN)
        put(0x2E, KeyEvent.KEYCODE_FORWARD_DEL)
        put(0x5B, KeyEvent.KEYCODE_META_LEFT)
        put(0x5C, KeyEvent.KEYCODE_META_RIGHT)
        // OEM punctuation
        put(0xBA, KeyEvent.KEYCODE_SEMICOLON)
        put(0xBB, KeyEvent.KEYCODE_EQUALS)
        put(0xBC, KeyEvent.KEYCODE_COMMA)
        put(0xBD, KeyEvent.KEYCODE_MINUS)
        put(0xBE, KeyEvent.KEYCODE_PERIOD)
        put(0xBF, KeyEvent.KEYCODE_SLASH)
        put(0xC0, KeyEvent.KEYCODE_GRAVE)
        put(0xDB, KeyEvent.KEYCODE_LEFT_BRACKET)
        put(0xDC, KeyEvent.KEYCODE_BACKSLASH)
        put(0xDD, KeyEvent.KEYCODE_RIGHT_BRACKET)
        put(0xDE, KeyEvent.KEYCODE_APOSTROPHE)
    }

    fun toAndroid(vk: Int): Int? = map[vk]
}
