package com.inputflow.android

import android.view.KeyEvent
import org.junit.Assert.assertEquals
import org.junit.Assert.assertNull
import org.junit.Test

/**
 * Pure-JVM test: android.view.KeyEvent.KEYCODE_* are compile-time int constants,
 * so VkMap resolves without an Android runtime.
 */
class VkMapTest {
    @Test
    fun mapsLetters() {
        assertEquals(KeyEvent.KEYCODE_A, VkMap.toAndroid(0x41)) // VK_A
        assertEquals(KeyEvent.KEYCODE_Z, VkMap.toAndroid(0x5A)) // VK_Z
    }

    @Test
    fun mapsDigits() {
        assertEquals(KeyEvent.KEYCODE_0, VkMap.toAndroid(0x30))
        assertEquals(KeyEvent.KEYCODE_9, VkMap.toAndroid(0x39))
    }

    @Test
    fun mapsCommonKeys() {
        assertEquals(KeyEvent.KEYCODE_ENTER, VkMap.toAndroid(0x0D))
        assertEquals(KeyEvent.KEYCODE_DEL, VkMap.toAndroid(0x08))    // Backspace
        assertEquals(KeyEvent.KEYCODE_SPACE, VkMap.toAndroid(0x20))
        assertEquals(KeyEvent.KEYCODE_DPAD_LEFT, VkMap.toAndroid(0x25))
    }

    @Test
    fun unmappedReturnsNull() {
        assertNull(VkMap.toAndroid(0x00))
        assertNull(VkMap.toAndroid(0xFFFF))
    }
}
