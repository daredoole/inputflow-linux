package com.inputflow.android

import android.Manifest
import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent
import android.content.IntentFilter
import android.net.Uri
import android.os.Build
import android.os.Bundle
import android.provider.Settings
import android.view.Menu
import android.view.MenuItem
import android.view.inputmethod.InputMethodManager
import android.widget.LinearLayout
import android.widget.TextView
import androidx.appcompat.app.AppCompatActivity
import com.google.android.material.appbar.MaterialToolbar
import com.google.android.material.button.MaterialButton
import com.google.android.material.card.MaterialCardView
import com.google.android.material.textfield.TextInputEditText

class MainActivity : AppCompatActivity() {

    private lateinit var hostField: TextInputEditText
    private lateinit var portField: TextInputEditText
    private lateinit var secretField: TextInputEditText
    private lateinit var statusCard: MaterialCardView
    private lateinit var statusDot: android.view.View
    private lateinit var statusText: TextView
    private lateinit var statusDetail: TextView

    private val statusReceiver = object : BroadcastReceiver() {
        override fun onReceive(context: Context, intent: Intent) {
            val state = intent.getStringExtra(RelayForegroundService.EXTRA_STATE) ?: return
            val detail = intent.getStringExtra(RelayForegroundService.EXTRA_DETAIL)
            updateStatus(state, detail)
        }
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        setContentView(R.layout.activity_main)

        val toolbar = findViewById<MaterialToolbar>(R.id.toolbar)
        setSupportActionBar(toolbar)
        supportActionBar?.setDisplayShowTitleEnabled(false)
        toolbar.setOnMenuItemClickListener { item ->
            if (item.itemId == R.id.action_settings) {
                startActivity(Intent(this, SettingsActivity::class.java))
                true
            } else {
                false
            }
        }

        hostField = findViewById(R.id.hostField)
        portField = findViewById(R.id.portField)
        secretField = findViewById(R.id.secretField)
        statusCard = findViewById(R.id.statusCard)
        statusDot = findViewById(R.id.statusDot)
        statusText = findViewById(R.id.statusText)
        statusDetail = findViewById(R.id.statusDetail)

        val prefs = getSharedPreferences(RelayForegroundService.PREFS, Context.MODE_PRIVATE)
        applyPairingUri(intent)
        hostField.setText(prefs.getString(RelayForegroundService.KEY_HOST, ""))
        portField.setText(prefs.getInt(RelayForegroundService.KEY_PORT, 15102).toString())
        secretField.setText(prefs.getString(RelayForegroundService.KEY_SECRET, ""))

        findViewById<MaterialButton>(R.id.btnConnect).setOnClickListener {
            prefs.edit()
                .putString(RelayForegroundService.KEY_HOST, hostField.text.toString().trim())
                .putInt(RelayForegroundService.KEY_PORT, portField.text.toString().toIntOrNull() ?: 15102)
                .putString(RelayForegroundService.KEY_SECRET, secretField.text.toString().trim())
                .apply()
            requestNotificationPermission()
            startRelayService(Intent(this, RelayForegroundService::class.java))
        }

        findViewById<MaterialButton>(R.id.btnRelease).setOnClickListener {
            startRelayService(
                Intent(this, RelayForegroundService::class.java)
                    .setAction(RelayForegroundService.ACTION_RELEASE)
            )
        }

        findViewById<LinearLayout>(R.id.btnAccessibility).setOnClickListener {
            startActivity(Intent(Settings.ACTION_ACCESSIBILITY_SETTINGS))
        }

        findViewById<LinearLayout>(R.id.btnKeyboard).setOnClickListener {
            (getSystemService(INPUT_METHOD_SERVICE) as InputMethodManager).showInputMethodPicker()
        }

        findViewById<LinearLayout>(R.id.btnNotifications).setOnClickListener {
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
                startActivity(
                    Intent(Settings.ACTION_APPLICATION_DETAILS_SETTINGS, Uri.parse("package:$packageName"))
                )
            }
        }

        refreshStatus()
    }

    override fun onResume() {
        super.onResume()
        val filter = IntentFilter(RelayForegroundService.ACTION_STATUS_BROADCAST)
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
            registerReceiver(statusReceiver, filter, RECEIVER_NOT_EXPORTED)
        } else {
            @Suppress("UnspecifiedRegisterReceiverFlag")
            registerReceiver(statusReceiver, filter)
        }
        refreshStatus()
    }

    override fun onPause() {
        super.onPause()
        try { unregisterReceiver(statusReceiver) } catch (_: IllegalArgumentException) {}
    }

    override fun onOptionsItemSelected(item: MenuItem): Boolean {
        if (item.itemId == R.id.action_settings) {
            startActivity(Intent(this, SettingsActivity::class.java))
            return true
        }
        return super.onOptionsItemSelected(item)
    }

    override fun onCreateOptionsMenu(menu: Menu): Boolean {
        menuInflater.inflate(R.menu.main_menu, menu)
        return true
    }

    override fun onNewIntent(intent: Intent) {
        super.onNewIntent(intent)
        setIntent(intent)
        applyPairingUri(intent)
        val prefs = getSharedPreferences(RelayForegroundService.PREFS, Context.MODE_PRIVATE)
        hostField.setText(prefs.getString(RelayForegroundService.KEY_HOST, ""))
        portField.setText(prefs.getInt(RelayForegroundService.KEY_PORT, 15102).toString())
        secretField.setText(prefs.getString(RelayForegroundService.KEY_SECRET, ""))
    }

    private fun refreshStatus() {
        val state = if (InputFlowAccessibilityService.instance != null)
            RelayForegroundService.STATE_CONNECTED
        else
            RelayForegroundService.STATE_DISCONNECTED
        updateStatus(state, null)
    }

    private fun updateStatus(state: String, detail: String?) {
        val cardBgRes: Int
        val accentColorRes: Int
        val labelRes: Int
        when (state) {
            RelayForegroundService.STATE_CONNECTED -> {
                cardBgRes = R.color.colorConnectedBg
                accentColorRes = R.color.colorConnected
                labelRes = R.string.status_connected
            }
            RelayForegroundService.STATE_CONNECTING -> {
                cardBgRes = R.color.colorConnectingBg
                accentColorRes = R.color.colorConnecting
                labelRes = R.string.status_connecting
            }
            else -> {
                cardBgRes = R.color.colorDisconnectedBg
                accentColorRes = R.color.colorDisconnected
                labelRes = R.string.status_disconnected
            }
        }
        val accentColor = getColor(accentColorRes)
        statusCard.setCardBackgroundColor(getColor(cardBgRes))
        statusDot.background.setTint(accentColor)
        statusText.setTextColor(accentColor)
        statusText.setText(labelRes)
        if (detail != null) {
            statusDetail.visibility = android.view.View.VISIBLE
            statusDetail.text = detail
            statusDetail.setTextColor(accentColor)
        } else {
            statusDetail.visibility = android.view.View.GONE
        }
    }

    private fun requestNotificationPermission() {
        if (Build.VERSION.SDK_INT >= 33) {
            requestPermissions(arrayOf(Manifest.permission.POST_NOTIFICATIONS), 20)
        }
    }

    private fun applyPairingUri(intent: Intent?) {
        val data = intent?.data ?: return
        if (data.scheme != "inputflow" || data.host != "android-peer") return
        val host = data.getQueryParameter("host").orEmpty()
        val port = data.getQueryParameter("port")?.toIntOrNull() ?: 15102
        val secret = data.getQueryParameter("secret").orEmpty().trim()
        if (host.isBlank() || secret.isBlank()) return
        getSharedPreferences(RelayForegroundService.PREFS, Context.MODE_PRIVATE)
            .edit()
            .putString(RelayForegroundService.KEY_HOST, host)
            .putInt(RelayForegroundService.KEY_PORT, port)
            .putString(RelayForegroundService.KEY_SECRET, secret)
            .apply()
    }

    private fun startRelayService(intent: Intent) {
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
            startForegroundService(intent)
        } else {
            startService(intent)
        }
    }
}
