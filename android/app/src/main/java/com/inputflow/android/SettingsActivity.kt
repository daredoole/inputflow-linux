package com.inputflow.android

import android.content.Context
import android.content.Intent
import android.os.Bundle
import android.view.inputmethod.InputMethodManager
import android.widget.RadioGroup
import android.widget.TextView
import androidx.appcompat.app.AppCompatActivity
import com.google.android.material.appbar.MaterialToolbar
import com.google.android.material.button.MaterialButton
import com.google.android.material.button.MaterialButtonToggleGroup
import com.google.android.material.chip.ChipGroup
import com.google.android.material.materialswitch.MaterialSwitch
import com.google.android.material.slider.Slider

class SettingsActivity : AppCompatActivity() {

    private lateinit var cursorSizeGroup: MaterialButtonToggleGroup
    private lateinit var cursorColorGroup: ChipGroup
    private lateinit var sensitivitySlider: Slider
    private lateinit var laptopTypingSwitch: MaterialSwitch
    private lateinit var keyboardModeGroup: MaterialButtonToggleGroup
    private lateinit var connectionModeGroup: RadioGroup
    private lateinit var modeHelpText: TextView

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        setContentView(R.layout.activity_settings)

        val toolbar = findViewById<MaterialToolbar>(R.id.toolbar)
        setSupportActionBar(toolbar)
        supportActionBar?.setDisplayShowTitleEnabled(false)
        toolbar.setNavigationOnClickListener { finish() }

        val prefs = getSharedPreferences(RelayForegroundService.PREFS, Context.MODE_PRIVATE)

        cursorSizeGroup = findViewById(R.id.cursorSizeGroup)
        cursorColorGroup = findViewById(R.id.cursorColorGroup)
        sensitivitySlider = findViewById(R.id.sensitivitySlider)
        laptopTypingSwitch = findViewById(R.id.laptopTypingSwitch)
        keyboardModeGroup = findViewById(R.id.keyboardModeGroup)
        connectionModeGroup = findViewById(R.id.connectionModeGroup)
        modeHelpText = findViewById(R.id.modeHelpText)

        // Restore cursor size
        when (prefs.getInt(KEY_CURSOR_SIZE, 2)) {
            1 -> cursorSizeGroup.check(R.id.cursorSizeSmall)
            3 -> cursorSizeGroup.check(R.id.cursorSizeLarge)
            else -> cursorSizeGroup.check(R.id.cursorSizeMedium)
        }

        // Restore cursor color
        when (prefs.getString(KEY_CURSOR_COLOR, "white")) {
            "blue" -> cursorColorGroup.check(R.id.colorBlue)
            "red" -> cursorColorGroup.check(R.id.colorRed)
            "black" -> cursorColorGroup.check(R.id.colorBlack)
            else -> cursorColorGroup.check(R.id.colorWhite)
        }

        // Restore sensitivity
        sensitivitySlider.value = prefs.getFloat(KEY_SENSITIVITY, 1.0f).coerceIn(0.5f, 3.0f)

        laptopTypingSwitch.isChecked = prefs.getBoolean(RelayForegroundService.KEY_LAPTOP_TYPING_ENABLED, false)

        // Restore keyboard mode
        when (prefs.getString(KEY_KEYBOARD_MODE, "accessibility")) {
            "ime" -> keyboardModeGroup.check(R.id.kbIme)
            else -> keyboardModeGroup.check(R.id.kbAccessibility)
        }

        // Restore connection mode
        when (prefs.getString(KEY_CONNECTION_MODE, "powertoys")) {
            "direct" -> connectionModeGroup.check(R.id.modeDirect)
            else -> connectionModeGroup.check(R.id.modePowerToys)
        }
        updateModeHelp(prefs.getString(KEY_CONNECTION_MODE, "powertoys") ?: "powertoys")

        connectionModeGroup.setOnCheckedChangeListener { _, checkedId ->
            val mode = if (checkedId == R.id.modeDirect) "direct" else "powertoys"
            updateModeHelp(mode)
        }

        laptopTypingSwitch.setOnCheckedChangeListener { _, checked ->
            saveSettings()
            if (checked) {
                showKeyboardPicker()
            } else {
                restoreTabletKeyboard()
            }
        }

        findViewById<MaterialButton>(R.id.btnEditLayout).setOnClickListener {
            startActivity(Intent(this, LayoutEditorActivity::class.java))
        }

        findViewById<MaterialButton>(R.id.btnEditShortcuts).setOnClickListener {
            startActivity(Intent(this, KeyMapperActivity::class.java))
        }

        findViewById<MaterialButton>(R.id.btnChooseKeyboard).setOnClickListener {
            saveSettings()
            showKeyboardPicker()
        }

        findViewById<MaterialButton>(R.id.btnRestoreKeyboard).setOnClickListener {
            laptopTypingSwitch.isChecked = false
            saveSettings()
            restoreTabletKeyboard()
        }
    }

    override fun onStop() {
        super.onStop()
        saveSettings()
    }

    private fun saveSettings() {
        val prefs = getSharedPreferences(RelayForegroundService.PREFS, Context.MODE_PRIVATE)
        val cursorSize = when (cursorSizeGroup.checkedButtonId) {
            R.id.cursorSizeSmall -> 1
            R.id.cursorSizeLarge -> 3
            else -> 2
        }
        val cursorColor = when (cursorColorGroup.checkedChipId) {
            R.id.colorBlue -> "blue"
            R.id.colorRed -> "red"
            R.id.colorBlack -> "black"
            else -> "white"
        }
        val keyboardMode = if (keyboardModeGroup.checkedButtonId == R.id.kbIme) "ime" else "accessibility"
        val connectionMode = if (connectionModeGroup.checkedRadioButtonId == R.id.modeDirect) "direct" else "powertoys"

        prefs.edit()
            .putInt(KEY_CURSOR_SIZE, cursorSize)
            .putString(KEY_CURSOR_COLOR, cursorColor)
            .putFloat(KEY_SENSITIVITY, sensitivitySlider.value)
            .putBoolean(RelayForegroundService.KEY_LAPTOP_TYPING_ENABLED, laptopTypingSwitch.isChecked)
            .putString(KEY_KEYBOARD_MODE, keyboardMode)
            .putString(KEY_CONNECTION_MODE, connectionMode)
            .apply()
    }

    private fun showKeyboardPicker() {
        (getSystemService(INPUT_METHOD_SERVICE) as InputMethodManager).showInputMethodPicker()
    }

    private fun restoreTabletKeyboard() {
        if (!InputFlowImeService.restorePreviousKeyboard()) {
            showKeyboardPicker()
        }
    }

    private fun updateModeHelp(mode: String) {
        modeHelpText.setText(
            if (mode == "direct") R.string.mode_help_direct else R.string.mode_help_powertoys
        )
    }

    companion object {
        const val KEY_CURSOR_SIZE = "cursor_size"
        const val KEY_CURSOR_COLOR = "cursor_color"
        const val KEY_SENSITIVITY = "cursor_sensitivity"
        const val KEY_KEYBOARD_MODE = "keyboard_mode"
        const val KEY_CONNECTION_MODE = "connection_mode"
    }
}
