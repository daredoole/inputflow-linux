package com.inputflow.android

import android.content.SharedPreferences

object KeyMap {
    const val MOD_SHIFT = 1
    const val MOD_CTRL  = 2
    const val MOD_ALT   = 4
    const val MOD_WIN   = 8

    data class Action(val id: String, val label: String, val defaultVk: Int, val defaultMods: Int)

    val ACTIONS = listOf(
        Action("back",           "Back",                0x1B, 0),
        Action("home",           "Home",                0x5B, 0),
        Action("recents",        "Recent Apps",         0x09, MOD_ALT),
        Action("notifications",  "Notifications",       0x4E, MOD_CTRL or MOD_ALT),
        Action("quick_settings", "Quick Settings",      0x51, MOD_CTRL or MOD_ALT),
        Action("volume_up",      "Volume Up",           0xAF, 0),
        Action("volume_down",    "Volume Down",         0xAE, 0),
        Action("screenshot",     "Screenshot",          0x2C, 0),
        Action("swipe_left",     "Swipe Left (Back)",   0,    0),
        Action("swipe_right",    "Swipe Right",         0,    0),
        Action("swipe_up",       "Swipe Up / Home",     0,    0),
        Action("swipe_down",     "Swipe Down",          0,    0),
        Action("zoom_in",        "Zoom In",             0,    0),
        Action("zoom_out",       "Zoom Out",            0,    0),
    )

    fun getVk(prefs: SharedPreferences, action: Action): Int =
        prefs.getInt("keymap_${action.id}_vk", action.defaultVk)

    fun getMods(prefs: SharedPreferences, action: Action): Int =
        prefs.getInt("keymap_${action.id}_mods", action.defaultMods)

    fun save(prefs: SharedPreferences, actionId: String, vk: Int, mods: Int) {
        prefs.edit()
            .putInt("keymap_${actionId}_vk", vk)
            .putInt("keymap_${actionId}_mods", mods)
            .apply()
    }

    fun bindingText(vk: Int, mods: Int): String {
        if (vk == 0) return "None"
        return buildString {
            if (mods and MOD_CTRL  != 0) append("Ctrl+")
            if (mods and MOD_ALT   != 0) append("Alt+")
            if (mods and MOD_SHIFT != 0) append("Shift+")
            if (mods and MOD_WIN   != 0) append("Win+")
            append(vkName(vk))
        }
    }

    fun vkName(vk: Int): String = when (vk) {
        0x08 -> "Bksp"
        0x09 -> "Tab"
        0x0D -> "Enter"
        0x1B -> "Esc"
        0x20 -> "Space"
        0x2C -> "PrtSc"
        0x2E -> "Del"
        0x5B, 0x5C -> "Win"
        0xAE -> "Vol-"
        0xAF -> "Vol+"
        in 0x41..0x5A -> ('A' + vk - 0x41).toChar().toString()
        in 0x30..0x39 -> ('0' + vk - 0x30).toChar().toString()
        in 0x70..0x7B -> "F${vk - 0x70 + 1}"
        else -> "0x${vk.toString(16).uppercase()}"
    }

    fun currentMods(shift: Boolean, ctrl: Boolean, alt: Boolean, win: Boolean): Int {
        var m = 0
        if (shift) m = m or MOD_SHIFT
        if (ctrl)  m = m or MOD_CTRL
        if (alt)   m = m or MOD_ALT
        if (win)   m = m or MOD_WIN
        return m
    }
}
