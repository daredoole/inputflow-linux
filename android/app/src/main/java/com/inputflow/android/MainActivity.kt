package com.inputflow.android

import android.Manifest
import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent
import android.content.IntentFilter
import android.content.pm.PackageManager
import android.net.Uri
import android.os.Build
import android.os.Bundle
import android.provider.Settings
import android.view.Menu
import android.view.MenuItem
import android.view.inputmethod.InputMethodManager
import android.widget.LinearLayout
import android.widget.TextView
import androidx.appcompat.app.AlertDialog
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
        portField.setText(getString(R.string.port_number, prefs.getInt(RelayForegroundService.KEY_PORT, 15102)))
        secretField.setText(PairingSecretStore.read(this))

        // Auto-connect on launch when already configured and not already running,
        // mirroring the desktop client's auto_connect. Saves the user a tap after
        // every app start (and after a reinstall) and keeps the relay self-healing.
        val savedHost = prefs.getString(RelayForegroundService.KEY_HOST, "").orEmpty()
        val savedSecret = PairingSecretStore.read(this)
        if (savedHost.isNotBlank() && savedSecret.isNotBlank() &&
            RelayForegroundService.instance == null
        ) {
            startRelayService(Intent(this, RelayForegroundService::class.java))
        }

        findViewById<MaterialButton>(R.id.btnConnect).setOnClickListener {
            val secret = secretField.text.toString().trim()
            if (!PairingSecretStore.save(this, secret)) {
                secretField.error = getString(R.string.secret_storage_error)
                return@setOnClickListener
            }
            prefs.edit()
                .putString(RelayForegroundService.KEY_HOST, hostField.text.toString().trim())
                .putInt(RelayForegroundService.KEY_PORT, portField.text.toString().toIntOrNull() ?: 15102)
                .apply()
            requestNotificationPermission()
            startRelayService(Intent(this, RelayForegroundService::class.java))
            promptMissingPermissions()
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
        // Returning from a settings screen: re-check and surface the next missing
        // permission, but only once the user has actually started a connection.
        if (RelayForegroundService.currentState != RelayForegroundService.STATE_DISCONNECTED) {
            promptMissingPermissions()
        }
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
        portField.setText(getString(R.string.port_number, prefs.getInt(RelayForegroundService.KEY_PORT, 15102)))
        secretField.setText(PairingSecretStore.read(this))
    }

    private fun refreshStatus() {
        val service = RelayForegroundService.instance
        if (service != null) {
            updateStatus(RelayForegroundService.currentState, RelayForegroundService.currentDetail)
            return
        }

        val prefs = getSharedPreferences(RelayForegroundService.PREFS, Context.MODE_PRIVATE)
        val savedState = prefs.getString(
            RelayForegroundService.KEY_STATUS_STATE,
            RelayForegroundService.STATE_DISCONNECTED
        ) ?: RelayForegroundService.STATE_DISCONNECTED
        val updatedAt = prefs.getLong(RelayForegroundService.KEY_STATUS_UPDATED_MS, 0L)
        val recentlyUpdated = System.currentTimeMillis() - updatedAt < 5000L
        if (!recentlyUpdated || savedState == RelayForegroundService.STATE_DISCONNECTED) {
            updateStatus(RelayForegroundService.STATE_DISCONNECTED, null)
            return
        }

        val detail = if (prefs.getBoolean(RelayForegroundService.KEY_STATUS_HAS_DETAIL, false)) {
            prefs.getString(RelayForegroundService.KEY_STATUS_DETAIL, null)
        } else {
            null
        }
        updateStatus(savedState, detail)
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

    private data class MissingPermission(
        val label: String,
        val rationale: String,
        val open: () -> Unit,
    )

    private fun isAccessibilityEnabled(): Boolean {
        val flat = Settings.Secure.getString(
            contentResolver, Settings.Secure.ENABLED_ACCESSIBILITY_SERVICES
        ) ?: return false
        return flat.split(':').any {
            it.startsWith(packageName) && it.contains("InputFlowAccessibilityService")
        }
    }

    private fun isNotificationListenerEnabled(): Boolean {
        val flat = Settings.Secure.getString(
            contentResolver, "enabled_notification_listeners"
        ) ?: return false
        return flat.split(':').any {
            it.startsWith(packageName) && it.contains("InputFlowNotificationListenerService")
        }
    }

    /** Required grants that Android cannot prompt for inline (deep-link only). */
    private fun missingRequiredPermissions(): List<MissingPermission> {
        val prefs = getSharedPreferences(RelayForegroundService.PREFS, Context.MODE_PRIVATE)
        val missing = mutableListOf<MissingPermission>()

        if (!isAccessibilityEnabled()) {
            missing += MissingPermission(
                "Accessibility access",
                "Lets InputFlow show the remote cursor and deliver taps from your computer.",
            ) { startActivity(Intent(Settings.ACTION_ACCESSIBILITY_SETTINGS)) }
        }

        if (prefs.getBoolean(RelayForegroundService.KEY_NOTIFICATION_SYNC_ENABLED, false) &&
            !isNotificationListenerEnabled()
        ) {
            missing += MissingPermission(
                "Notification access",
                "Lets InputFlow mirror this device's notifications to your computer.",
            ) { startActivity(Intent(Settings.ACTION_NOTIFICATION_LISTENER_SETTINGS)) }
        }

        if (Build.VERSION.SDK_INT >= 33 &&
            checkSelfPermission(Manifest.permission.POST_NOTIFICATIONS) !=
            PackageManager.PERMISSION_GRANTED
        ) {
            missing += MissingPermission(
                "Show notifications",
                "Lets InputFlow display your computer's notifications on this device.",
            ) { requestNotificationPermission() }
        }

        return missing
    }

    private var permissionDialogShowing = false

    /**
     * Surfaces the first still-missing required permission as a dialog that
     * deep-links to the relevant settings page. Re-runs on [onResume], so the
     * user is walked through each missing grant one screen at a time.
     */
    private fun promptMissingPermissions() {
        if (permissionDialogShowing) return
        val first = missingRequiredPermissions().firstOrNull() ?: return
        val remaining = missingRequiredPermissions().size
        permissionDialogShowing = true
        AlertDialog.Builder(this)
            .setTitle("Permission needed")
            .setMessage(
                "${first.label}\n\n${first.rationale}" +
                    if (remaining > 1) "\n\n${remaining - 1} more after this." else ""
            )
            .setPositiveButton("Open settings") { d, _ ->
                permissionDialogShowing = false
                d.dismiss()
                first.open()
            }
            .setNegativeButton("Not now") { d, _ ->
                permissionDialogShowing = false
                d.dismiss()
            }
            .setOnCancelListener { permissionDialogShowing = false }
            .show()
    }

    private fun applyPairingUri(intent: Intent?) {
        val data = intent?.data ?: return
        intent.data = null
        if (data.scheme != "inputflow" || data.host != "android-peer") return
        val host = data.getQueryParameter("host").orEmpty()
        val port = data.getQueryParameter("port")?.toIntOrNull() ?: 15102
        val secret = data.getQueryParameter("secret").orEmpty().trim()
        if (host.isBlank() || secret.isBlank()) return
        if (!PairingSecretStore.save(this, secret)) return
        getSharedPreferences(RelayForegroundService.PREFS, Context.MODE_PRIVATE)
            .edit()
            .putString(RelayForegroundService.KEY_HOST, host)
            .putInt(RelayForegroundService.KEY_PORT, port)
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
