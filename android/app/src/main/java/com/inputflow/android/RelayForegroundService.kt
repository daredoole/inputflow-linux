package com.inputflow.android

import android.app.Notification
import android.app.NotificationChannel
import android.app.NotificationManager
import android.app.Service
import android.content.Context
import android.content.Intent
import android.os.Build
import android.os.IBinder
import android.util.Log
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
    private val outputLock = Any()
    @Volatile
    private var remoteControlActive = false
    private var deliveredMouseFrames = 0
    private var droppedMouseFrames = 0

    override fun onCreate() {
        super.onCreate()
        instance = this
        createNotificationChannel()
        InjectorManager.init(this)
    }

    override fun onDestroy() {
        if (instance === this) instance = null
        running.set(false)
        worker?.interrupt()
        output = null
        deactivateRemoteControl()
        broadcastStatus(STATE_DISCONNECTED, "Stopped")
        super.onDestroy()
    }

    // Android 10+ requires (and Android 14+ enforces) an explicit foreground
    // service type. connectedDevice avoids the Android 15 dataSync time-cap.
    private fun startRelayForeground(text: String) {
        val notif = notification(text)
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.Q) {
            startForeground(
                NOTIFICATION_ID,
                notif,
                android.content.pm.ServiceInfo.FOREGROUND_SERVICE_TYPE_CONNECTED_DEVICE)
        } else {
            startForeground(NOTIFICATION_ID, notif)
        }
    }

    override fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int {
        when (intent?.action) {
            ACTION_RELEASE -> {
                if (!running.get()) {
                    startRelayForeground("Release requested")
                    deactivateRemoteControl()
                    stopSelf()
                } else {
                    sendRelease()
                    deactivateRemoteControl()
                }
            }
            ACTION_STOP -> {
                if (!running.get()) {
                    startRelayForeground("Stopping")
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
        writeFrame(JSONObject().put("type", "topology_update").put("layout", layout))
    }

    fun isConnectedForWrites(): Boolean =
        currentState == STATE_CONNECTED && output != null

    fun sendNotificationUpsert(
        stableId: String,
        app: String,
        packageName: String,
        title: String,
        body: String,
        postedAtMs: Long
    ): Boolean =
        writeFrame(
            JSONObject()
                .put("type", "notification_upsert")
                .put("stable_id", stableId)
                .put("origin", "android")
                .put("app", app)
                .put("package", packageName)
                .put("title", title)
                .put("body", body)
                .put("posted_at_ms", postedAtMs)
        )

    fun sendNotificationDismiss(stableId: String, packageName: String): Boolean =
        writeFrame(
            JSONObject()
                .put("type", "notification_dismiss")
                .put("stable_id", stableId)
                .put("origin", "android")
                .put("package", packageName)
        )

    private fun startRelay() {
        if (!running.compareAndSet(false, true)) return
        InjectorManager.start(this)
        startRelayForeground("Connecting")
        broadcastStatus(STATE_CONNECTING, null)
        worker = Thread({ relayLoop() }, "inputflow-relay").also { it.start() }
    }

    private fun dispatchFrame(frame: JSONObject, prefs: android.content.SharedPreferences) {
        when (frame.optString("type")) {
            "control" -> setRemoteControlActive(frame.optBoolean("active", false))
            "mouse" -> if (remoteControlActive) {
                // Prefer native injection (Shizuku/root); fall back to accessibility.
                val accessibility = InputFlowAccessibilityService.instance
                val isMove = frame.optInt("wParam") == WM_MOUSEMOVE
                if (isMove && InjectorManager.handleMouse(frame)) {
                    // Native pointer motion succeeded.
                } else if (accessibility != null) {
                    deliveredMouseFrames += 1
                    accessibility.handleMouse(frame)
                } else if (!InjectorManager.handleMouse(frame)) {
                    if (droppedMouseFrames < 5) {
                        Log.w(TAG, "mouse frame dropped: no injector active")
                    }
                    droppedMouseFrames += 1
                }
            }
            "keyboard" -> {
                val cb = keyCaptureCallback
                if (cb != null) {
                    cb(frame.optInt("vkCode"), frame.optInt("flags"))
                } else if (remoteControlActive) {
                    val accessibility = InputFlowAccessibilityService.instance
                    if (accessibility?.handleMappedKeyboard(frame) == true) {
                        // handled by a user-defined key mapping
                    } else if (InjectorManager.handleKeyboard(frame)) {
                        // injected natively (Shizuku/root)
                    } else {
                        val laptopTypingEnabled = prefs.getBoolean(KEY_LAPTOP_TYPING_ENABLED, false)
                        if (!laptopTypingEnabled || InputFlowImeService.instance?.handleKeyboard(frame) != true) {
                            accessibility?.handleKeyboard(frame)
                        }
                    }
                }
            }
            "gesture" -> if (remoteControlActive) {
                val accessibility = InputFlowAccessibilityService.instance
                if (accessibility != null) {
                    accessibility.handleGesture(frame)
                } else if (frame.optString("kind") == "scroll") {
                    InjectorManager.handleScroll(frame)
                }
            }
            "devices_info" -> handleDevicesInfo(frame)
            "notification_upsert" -> NotificationSyncBridge.showMirroredNotification(this, frame)
            "notification_dismiss" -> NotificationSyncBridge.cancelMirroredNotification(this, frame)
        }
    }

    private fun relayLoop() {
        val prefs = getSharedPreferences(PREFS, Context.MODE_PRIVATE)
        while (running.get()) {
            val host = prefs.getString(KEY_HOST, "").orEmpty()
            val secret = prefs.getString(KEY_SECRET, "").orEmpty()
            val port = prefs.getInt(KEY_PORT, 15102)
            if (host.isBlank() || secret.isBlank()) {
                startRelayForeground("Missing pairing settings")
                broadcastStatus(STATE_DISCONNECTED, "Missing host or secret")
                sleepQuietly(2000)
                continue
            }

            try {
                // Resolve the host fresh on every attempt so a hostname follows
                // the Linux box across IP changes (DHCP/VPN), same self-healing
                // model as the desktop client. An IP literal resolves to itself.
                val address = java.net.InetAddress.getByName(host)
                Log.i(TAG, "relay connecting: $host -> ${address.hostAddress}:$port")
                java.net.Socket().use { socket ->
                    socket.tcpNoDelay = true
                    socket.connect(java.net.InetSocketAddress(address, port), 8000)
                    val device = "${Build.MANUFACTURER} ${Build.MODEL}".trim()
                    val streams = RelayProtocol.authenticate(socket, secret, device)
                    output = streams.second
                    deliveredMouseFrames = 0
                    droppedMouseFrames = 0
                    Log.i(TAG, "relay connected to $host:$port as $device")
                    deactivateRemoteControl()
                    startRelayForeground("Connected to $host:$port")
                    broadcastStatus(STATE_CONNECTED, "$host:$port")
                    while (running.get()) {
                        var frame = RelayProtocol.readFrame(streams.first)
                        // Collapse a backlog of mouse-move frames to the newest so the
                        // cursor tracks live with no lag under load. Non-move frames are
                        // dispatched in order; only intermediate moves are dropped.
                        while (frame.optString("type") == "mouse" &&
                            frame.optInt("wParam") == WM_MOUSEMOVE &&
                            streams.first.available() > 4
                        ) {
                            val next = RelayProtocol.readFrame(streams.first)
                            if (next.optString("type") == "mouse" &&
                                next.optInt("wParam") == WM_MOUSEMOVE
                            ) {
                                frame = next
                            } else {
                                dispatchFrame(frame, prefs)
                                frame = next
                                break
                            }
                        }
                        dispatchFrame(frame, prefs)
                    }
                    deactivateRemoteControl()
                }
            } catch (e: Exception) {
                Log.w(TAG, "relay disconnected; retrying", e)
                output = null
                deactivateRemoteControl()
                startRelayForeground("Disconnected; retrying")
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
        writeFrame(JSONObject().put("type", "release"))
    }

    private fun writeFrame(frame: JSONObject): Boolean {
        return try {
            synchronized(outputLock) {
                output?.let {
                    RelayProtocol.writeFrame(it, frame)
                    true
                } ?: run {
                    Log.w(TAG, "failed to write relay frame type=${frame.optString("type")}: no relay output stream")
                    false
                }
            }
        } catch (e: Exception) {
            Log.w(TAG, "failed to write relay frame type=${frame.optString("type")}", e)
            output = null
            broadcastStatus(STATE_DISCONNECTED, "Retrying…")
            false
        }
    }

    private fun setRemoteControlActive(active: Boolean) {
        val changed = remoteControlActive != active
        remoteControlActive = active
        val accessibility = InputFlowAccessibilityService.instance
        if (changed) {
            Log.i(TAG, "remote control active=$active accessibility=${accessibility != null}")
        }
        if (active && accessibility == null) {
            Log.w(TAG, "remote control requested but accessibility service is not active")
        }
        accessibility?.setRemoteControlActive(active)
        if (!active) {
            InputFlowImeService.restorePreviousKeyboard()
        }
    }

    private fun deactivateRemoteControl() {
        setRemoteControlActive(false)
    }

    private fun broadcastStatus(state: String, detail: String?) {
        currentState = state
        currentDetail = detail
        getSharedPreferences(PREFS, Context.MODE_PRIVATE).edit()
            .putString(KEY_STATUS_STATE, state)
            .putString(KEY_STATUS_DETAIL, detail.orEmpty())
            .putBoolean(KEY_STATUS_HAS_DETAIL, detail != null)
            .putLong(KEY_STATUS_UPDATED_MS, System.currentTimeMillis())
            .apply()
        sendBroadcast(
            Intent(ACTION_STATUS_BROADCAST)
                .setPackage(packageName)
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
        private const val WM_MOUSEMOVE = 0x0200
        const val PREFS = "inputflow"
        const val KEY_HOST = "host"
        const val KEY_PORT = "port"
        const val KEY_SECRET = "secret"
        const val KEY_LAPTOP_TYPING_ENABLED = "laptop_typing_enabled"
        const val KEY_NOTIFICATION_SYNC_ENABLED = "notification_sync_enabled"
        const val KEY_STATUS_STATE = "status_state"
        const val KEY_STATUS_DETAIL = "status_detail"
        const val KEY_STATUS_HAS_DETAIL = "status_has_detail"
        const val KEY_STATUS_UPDATED_MS = "status_updated_ms"
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
        private const val TAG = "InputFlowRelay"

        @Volatile var instance: RelayForegroundService? = null
        @Volatile var currentState: String = STATE_DISCONNECTED
        @Volatile var currentDetail: String? = null
        @Volatile var cachedDevicesJson: String? = null
        @Volatile var keyCaptureCallback: ((Int, Int) -> Unit)? = null
    }
}
