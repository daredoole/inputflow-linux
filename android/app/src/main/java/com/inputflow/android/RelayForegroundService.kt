package com.inputflow.android

import android.app.Notification
import android.app.NotificationChannel
import android.app.NotificationManager
import android.app.Service
import android.content.Context
import android.content.Intent
import android.os.Build
import android.os.IBinder
import org.json.JSONArray
import org.json.JSONObject
import java.io.DataOutputStream
import java.net.Socket
import java.util.concurrent.atomic.AtomicBoolean

class RelayForegroundService : Service() {
    private val running = AtomicBoolean(false)
    private var worker: Thread? = null

    @Volatile
    private var output: DataOutputStream? = null
    @Volatile
    private var remoteControlActive = false

    override fun onCreate() {
        super.onCreate()
        instance = this
        createNotificationChannel()
    }

    override fun onDestroy() {
        if (instance === this) instance = null
        running.set(false)
        worker?.interrupt()
        output = null
        deactivateRemoteControl()
        super.onDestroy()
    }

    override fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int {
        when (intent?.action) {
            ACTION_RELEASE -> {
                if (!running.get()) {
                    startForeground(NOTIFICATION_ID, notification("Release requested"))
                    deactivateRemoteControl()
                    stopSelf()
                } else {
                    sendRelease()
                    deactivateRemoteControl()
                }
            }
            ACTION_STOP -> {
                if (!running.get()) {
                    startForeground(NOTIFICATION_ID, notification("Stopping"))
                }
                deactivateRemoteControl()
                stopSelf()
            }
            else -> startRelay()
        }
        return START_STICKY
    }

    override fun onBind(intent: Intent?): IBinder? = null

    fun sendTopologyUpdate(layout: JSONArray) {
        try {
            output?.let {
                RelayProtocol.writeFrame(it, JSONObject().put("type", "topology_update").put("layout", layout))
            }
        } catch (_: Exception) {}
    }

    private fun startRelay() {
        if (!running.compareAndSet(false, true)) return
        startForeground(NOTIFICATION_ID, notification("Connecting"))
        broadcastStatus(STATE_CONNECTING, null)
        worker = Thread({ relayLoop() }, "inputflow-relay").also { it.start() }
    }

    private fun relayLoop() {
        val prefs = getSharedPreferences(PREFS, Context.MODE_PRIVATE)
        while (running.get()) {
            val host = prefs.getString(KEY_HOST, "").orEmpty()
            val secret = prefs.getString(KEY_SECRET, "").orEmpty()
            val port = prefs.getInt(KEY_PORT, 15102)
            if (host.isBlank() || secret.isBlank()) {
                startForeground(NOTIFICATION_ID, notification("Missing pairing settings"))
                broadcastStatus(STATE_DISCONNECTED, "Missing host or secret")
                sleepQuietly(2000)
                continue
            }

            try {
                Socket(host, port).use { socket ->
                    socket.tcpNoDelay = true
                    val device = "${Build.MANUFACTURER} ${Build.MODEL}".trim()
                    val streams = RelayProtocol.authenticate(socket, secret, device)
                    output = streams.second
                    deactivateRemoteControl()
                    startForeground(NOTIFICATION_ID, notification("Connected to $host:$port"))
                    broadcastStatus(STATE_CONNECTED, "$host:$port")
                    while (running.get()) {
                        val frame = RelayProtocol.readFrame(streams.first)
                        when (frame.optString("type")) {
                            "control" -> setRemoteControlActive(frame.optBoolean("active", false))
                            "mouse" -> if (remoteControlActive) {
                                InputFlowAccessibilityService.instance?.handleMouse(frame)
                            }
                            "keyboard" -> {
                                val cb = keyCaptureCallback
                                if (cb != null) {
                                    cb(frame.optInt("vkCode"), frame.optInt("flags"))
                                } else if (remoteControlActive) {
                                    val accessibility = InputFlowAccessibilityService.instance
                                    if (accessibility?.handleMappedKeyboard(frame) != true) {
                                        val laptopTypingEnabled = prefs.getBoolean(KEY_LAPTOP_TYPING_ENABLED, false)
                                        if (!laptopTypingEnabled || InputFlowImeService.instance?.handleKeyboard(frame) != true) {
                                            accessibility?.handleKeyboard(frame)
                                        }
                                    }
                                }
                            }
                            "gesture" -> if (remoteControlActive) {
                                InputFlowAccessibilityService.instance?.handleGesture(frame)
                            }
                            "devices_info" -> handleDevicesInfo(frame)
                        }
                    }
                    deactivateRemoteControl()
                }
            } catch (_: Exception) {
                output = null
                deactivateRemoteControl()
                startForeground(NOTIFICATION_ID, notification("Disconnected; retrying"))
                broadcastStatus(STATE_DISCONNECTED, "Retrying…")
                sleepQuietly(1500)
            }
        }
    }

    private fun handleDevicesInfo(frame: JSONObject) {
        val json = frame.toString()
        cachedDevicesJson = json
        sendBroadcast(Intent(ACTION_DEVICES_BROADCAST).putExtra(EXTRA_DEVICES_JSON, json))
    }

    private fun sendRelease() {
        try {
            output?.let {
                RelayProtocol.writeFrame(it, JSONObject().put("type", "release"))
            }
        } catch (_: Exception) {}
    }

    private fun setRemoteControlActive(active: Boolean) {
        remoteControlActive = active
        InputFlowAccessibilityService.instance?.setRemoteControlActive(active)
        if (!active) {
            InputFlowImeService.restorePreviousKeyboard()
        }
    }

    private fun deactivateRemoteControl() {
        setRemoteControlActive(false)
    }

    private fun broadcastStatus(state: String, detail: String?) {
        sendBroadcast(
            Intent(ACTION_STATUS_BROADCAST)
                .putExtra(EXTRA_STATE, state)
                .putExtra(EXTRA_DETAIL, detail)
        )
    }

    private fun notification(text: String) =
        Notification.Builder(this, CHANNEL_ID)
            .setSmallIcon(R.drawable.ic_inputflow)
            .setContentTitle(getString(R.string.app_name))
            .setContentText(text)
            .setOngoing(true)
            .build()

    private fun createNotificationChannel() {
        if (Build.VERSION.SDK_INT < Build.VERSION_CODES.O) return
        val manager = getSystemService(NotificationManager::class.java)
        val channel = NotificationChannel(CHANNEL_ID, "InputFlow relay", NotificationManager.IMPORTANCE_LOW)
        manager.createNotificationChannel(channel)
    }

    private fun sleepQuietly(ms: Long) {
        try { Thread.sleep(ms) } catch (_: InterruptedException) {}
    }

    companion object {
        const val PREFS = "inputflow"
        const val KEY_HOST = "host"
        const val KEY_PORT = "port"
        const val KEY_SECRET = "secret"
        const val KEY_LAPTOP_TYPING_ENABLED = "laptop_typing_enabled"
        const val ACTION_RELEASE = "com.inputflow.android.RELEASE"
        const val ACTION_STOP = "com.inputflow.android.STOP"
        const val ACTION_STATUS_BROADCAST = "com.inputflow.android.STATUS"
        const val ACTION_DEVICES_BROADCAST = "com.inputflow.android.DEVICES"
        const val EXTRA_STATE = "state"
        const val EXTRA_DETAIL = "detail"
        const val EXTRA_DEVICES_JSON = "devices_json"
        const val STATE_CONNECTED = "connected"
        const val STATE_CONNECTING = "connecting"
        const val STATE_DISCONNECTED = "disconnected"
        private const val CHANNEL_ID = "inputflow-relay"
        private const val NOTIFICATION_ID = 10

        @Volatile var instance: RelayForegroundService? = null
        @Volatile var cachedDevicesJson: String? = null
        @Volatile var keyCaptureCallback: ((Int, Int) -> Unit)? = null
    }
}
