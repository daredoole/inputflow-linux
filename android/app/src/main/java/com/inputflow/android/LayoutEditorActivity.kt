package com.inputflow.android

import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent
import android.content.IntentFilter
import android.os.Build
import android.os.Bundle
import android.view.View
import android.widget.TextView
import androidx.appcompat.app.AppCompatActivity
import com.google.android.material.appbar.MaterialToolbar
import com.google.android.material.button.MaterialButton
import org.json.JSONArray
import org.json.JSONObject

class LayoutEditorActivity : AppCompatActivity() {

    private lateinit var editorView: LayoutEditorView
    private lateinit var notConnectedHint: TextView
    private lateinit var btnApply: MaterialButton

    private val devicesReceiver = object : BroadcastReceiver() {
        override fun onReceive(context: Context, intent: Intent) {
            val json = intent.getStringExtra(RelayForegroundService.EXTRA_DEVICES_JSON) ?: return
            applyDevicesJson(json)
        }
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        setContentView(R.layout.activity_layout_editor)

        val toolbar = findViewById<MaterialToolbar>(R.id.toolbar)
        setSupportActionBar(toolbar)
        supportActionBar?.setDisplayShowTitleEnabled(false)
        toolbar.setNavigationOnClickListener { finish() }

        editorView = findViewById(R.id.editorView)
        notConnectedHint = findViewById(R.id.notConnectedHint)
        btnApply = findViewById(R.id.btnApplyLayout)

        val cached = RelayForegroundService.cachedDevicesJson
        if (cached != null) {
            applyDevicesJson(cached)
        } else {
            showNotConnected()
        }

        btnApply.setOnClickListener {
            val layout = editorView.getLayoutJson()
            RelayForegroundService.instance?.sendTopologyUpdate(layout)
        }
    }

    override fun onResume() {
        super.onResume()
        val filter = IntentFilter(RelayForegroundService.ACTION_DEVICES_BROADCAST)
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
            registerReceiver(devicesReceiver, filter, RECEIVER_NOT_EXPORTED)
        } else {
            @Suppress("UnspecifiedRegisterReceiverFlag")
            registerReceiver(devicesReceiver, filter)
        }
    }

    override fun onPause() {
        super.onPause()
        try { unregisterReceiver(devicesReceiver) } catch (_: IllegalArgumentException) {}
    }

    private fun applyDevicesJson(json: String) {
        try {
            val obj = JSONObject(json)
            val devArr = obj.getJSONArray("devices")
            val layoutArr = obj.optJSONArray("layout") ?: JSONArray()
            val layoutMap = mutableMapOf<String, Pair<Int, Int>>()
            for (i in 0 until layoutArr.length()) {
                val item = layoutArr.getJSONObject(i)
                layoutMap[item.getString("id")] = item.getInt("x") to item.getInt("y")
            }

            val cell = (64 * resources.displayMetrics.density).toInt().coerceAtLeast(1)
            val localId = "android"
            val rects = mutableListOf<DeviceRect>()

            for (i in 0 until devArr.length()) {
                val dev = devArr.getJSONObject(i)
                val id = dev.getString("id")
                val name = dev.getString("name")
                val screenW = dev.optInt("width", 1920).coerceAtLeast(1)
                val screenH = dev.optInt("height", 1080).coerceAtLeast(1)
                val gridW = (screenW / cell).coerceAtLeast(4)
                val gridH = (screenH / cell).coerceAtLeast(3)
                val pos = layoutMap[id]
                val gridX = ((pos?.first ?: 0) / cell)
                val gridY = ((pos?.second ?: 0) / cell)
                rects.add(DeviceRect(id, name, gridX, gridY, gridW, gridH, id == localId))
            }

            editorView.devices = rects
            editorView.visibility = View.VISIBLE
            notConnectedHint.visibility = View.GONE
            btnApply.isEnabled = true
        } catch (_: Exception) {
            showNotConnected()
        }
    }

    private fun showNotConnected() {
        editorView.visibility = View.INVISIBLE
        notConnectedHint.visibility = View.VISIBLE
        btnApply.isEnabled = false
    }
}
