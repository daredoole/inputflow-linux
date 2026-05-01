package com.inputflow.android

import android.content.Context
import android.os.Bundle
import android.view.Gravity
import android.view.LayoutInflater
import android.view.View
import android.view.ViewGroup
import android.widget.TextView
import androidx.appcompat.app.AppCompatActivity
import androidx.recyclerview.widget.LinearLayoutManager
import androidx.recyclerview.widget.RecyclerView
import com.google.android.material.appbar.MaterialToolbar
import com.google.android.material.chip.Chip
import com.google.android.material.dialog.MaterialAlertDialogBuilder

class KeyMapperActivity : AppCompatActivity() {

    private lateinit var adapter: ShortcutAdapter

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        setContentView(R.layout.activity_key_mapper)

        val toolbar = findViewById<MaterialToolbar>(R.id.toolbar)
        setSupportActionBar(toolbar)
        supportActionBar?.setDisplayShowTitleEnabled(false)
        toolbar.setNavigationOnClickListener { finish() }

        val prefs = getSharedPreferences(RelayForegroundService.PREFS, Context.MODE_PRIVATE)
        adapter = ShortcutAdapter(prefs) { action, vk, mods ->
            KeyMap.save(prefs, action.id, vk, mods)
        }

        val recycler = findViewById<RecyclerView>(R.id.shortcutsList)
        recycler.layoutManager = LinearLayoutManager(this)
        recycler.adapter = adapter
    }

    override fun onDestroy() {
        RelayForegroundService.keyCaptureCallback = null
        super.onDestroy()
    }

    private fun openBindingDialog(action: KeyMap.Action, onSaved: (Int, Int) -> Unit) {
        RelayForegroundService.keyCaptureCallback = null

        val tvHint = TextView(this).apply {
            text = getString(R.string.shortcut_listen_hint)
            textSize = 18f
            gravity = Gravity.CENTER
            setPadding(64, 48, 64, 16)
        }

        var capturedVk = 0
        var currentMods = 0

        val dialog = MaterialAlertDialogBuilder(this)
            .setTitle(action.label)
            .setView(tvHint)
            .setNegativeButton(android.R.string.cancel) { _, _ ->
                RelayForegroundService.keyCaptureCallback = null
            }
            .setNeutralButton(R.string.shortcut_clear) { _, _ ->
                RelayForegroundService.keyCaptureCallback = null
                onSaved(0, 0)
            }
            .setPositiveButton(android.R.string.ok) { _, _ ->
                RelayForegroundService.keyCaptureCallback = null
                if (capturedVk != 0) onSaved(capturedVk, currentMods)
            }
            .create()

        dialog.setOnDismissListener {
            RelayForegroundService.keyCaptureCallback = null
        }

        RelayForegroundService.keyCaptureCallback = { vk, flags ->
            val down = (flags and 0x80) == 0
            when (vk) {
                0x10 -> currentMods = if (down) currentMods or KeyMap.MOD_SHIFT else currentMods and KeyMap.MOD_SHIFT.inv()
                0x11 -> currentMods = if (down) currentMods or KeyMap.MOD_CTRL  else currentMods and KeyMap.MOD_CTRL.inv()
                0x12 -> currentMods = if (down) currentMods or KeyMap.MOD_ALT   else currentMods and KeyMap.MOD_ALT.inv()
                0x5B, 0x5C -> currentMods = if (down) currentMods or KeyMap.MOD_WIN else currentMods and KeyMap.MOD_WIN.inv()
                else -> if (down) {
                    capturedVk = vk
                    runOnUiThread { tvHint.text = KeyMap.bindingText(vk, currentMods) }
                }
            }
        }

        dialog.show()
    }

    inner class ShortcutAdapter(
        private val prefs: android.content.SharedPreferences,
        private val onSaved: (KeyMap.Action, Int, Int) -> Unit
    ) : RecyclerView.Adapter<ShortcutAdapter.VH>() {

        inner class VH(view: View) : RecyclerView.ViewHolder(view) {
            val tvAction: TextView = view.findViewById(R.id.tvAction)
            val chipBinding: Chip = view.findViewById(R.id.chipBinding)
        }

        override fun onCreateViewHolder(parent: ViewGroup, viewType: Int): VH =
            VH(LayoutInflater.from(parent.context).inflate(R.layout.row_shortcut, parent, false))

        override fun getItemCount() = KeyMap.ACTIONS.size

        override fun onBindViewHolder(holder: VH, position: Int) {
            val action = KeyMap.ACTIONS[position]
            holder.tvAction.text = action.label
            holder.chipBinding.text = KeyMap.bindingText(
                KeyMap.getVk(prefs, action),
                KeyMap.getMods(prefs, action)
            )
            holder.chipBinding.setOnClickListener {
                openBindingDialog(action) { vk, mods ->
                    onSaved(action, vk, mods)
                    notifyItemChanged(position)
                }
            }
        }
    }
}
