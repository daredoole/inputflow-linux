package com.inputflow.android

import android.Manifest
import android.app.Notification
import android.app.NotificationChannel
import android.app.NotificationManager
import android.app.RemoteInput
import android.content.Context
import android.content.Intent
import android.content.pm.PackageManager
import android.graphics.Bitmap
import android.graphics.Canvas
import android.graphics.drawable.Drawable
import android.os.Build
import android.os.Bundle
import android.service.notification.NotificationListenerService
import android.service.notification.StatusBarNotification
import android.util.Base64
import android.util.Log
import java.io.ByteArrayOutputStream
import java.util.concurrent.ConcurrentHashMap
import org.json.JSONObject
import kotlin.math.absoluteValue

class InputFlowNotificationListenerService : NotificationListenerService() {
    override fun onNotificationPosted(sbn: StatusBarNotification) {
        NotificationSyncBridge.sendAndroidNotification(this, sbn)
    }

    override fun onNotificationRemoved(sbn: StatusBarNotification) {
        NotificationSyncBridge.sendAndroidNotificationDismiss(this, sbn)
    }
}

object NotificationSyncBridge {
    private const val MIRROR_CHANNEL_ID = "inputflow-mirrored"
    private const val MIRROR_CHANNEL_NAME = "Mirrored notifications"
    private val sensitivePattern = Regex(
        "\\b(otp|one[- ]?time|verification|2fa|two[- ]?factor|password|passcode|bank|credit|debit)\\b",
        RegexOption.IGNORE_CASE
    )

    // Live reply actions keyed by the synced notification's stable_id, so a reply
    // typed on the desktop can be fired back into the originating app. Bounded so a
    // long-running listener can't grow unbounded; oldest entries are evicted.
    private const val MAX_REPLY_ENTRIES = 200
    private val replyActions = ConcurrentHashMap<String, Notification.Action>()
    private val replyOrder = ArrayDeque<String>()

    fun sendAndroidNotification(context: Context, sbn: StatusBarNotification) {
        if (!isEnabled(context) || sbn.packageName == context.packageName) return

        val notification = sbn.notification ?: return
        if ((notification.flags and Notification.FLAG_GROUP_SUMMARY) != 0) return
        if ((notification.flags and Notification.FLAG_ONGOING_EVENT) != 0) return
        if (notification.visibility == Notification.VISIBILITY_SECRET) return

        val title = notification.extras.getCharSequence(Notification.EXTRA_TITLE)
            ?.toString()
            ?.trim()
            .orEmpty()
        val body = (
            notification.extras.getCharSequence(Notification.EXTRA_BIG_TEXT)
                ?: notification.extras.getCharSequence(Notification.EXTRA_TEXT)
            )
            ?.toString()
            ?.trim()
            .orEmpty()

        if (title.isBlank() && body.isBlank()) return
        if (isSensitive(title, body)) return

        val sid = stableId(sbn)
        val replyAction = findReplyAction(notification)
        if (replyAction != null) {
            rememberReplyAction(sid, replyAction)
        }

        RelayForegroundService.sendNotificationUpsert(
            stableId = sid,
            app = appLabel(context, sbn.packageName),
            packageName = sbn.packageName,
            title = title,
            body = body,
            postedAtMs = sbn.postTime,
            iconPng = appIconBase64(context, sbn.packageName),
            canReply = replyAction != null
        )
    }

    /** First notification action carrying a free-text RemoteInput (quick reply). */
    private fun findReplyAction(notification: Notification): Notification.Action? {
        val actions = notification.actions ?: return null
        return actions.firstOrNull { action ->
            action.remoteInputs?.any { it.allowFreeFormInput } == true
        }
    }

    private fun rememberReplyAction(stableId: String, action: Notification.Action) {
        synchronized(replyOrder) {
            if (replyActions.put(stableId, action) == null) {
                replyOrder.addLast(stableId)
                while (replyOrder.size > MAX_REPLY_ENTRIES) {
                    replyActions.remove(replyOrder.removeFirst())
                }
            }
        }
    }

    /**
     * Fires a desktop-typed reply back into the originating app via its
     * RemoteInput. Returns true if the reply was dispatched.
     */
    fun replyToNotification(context: Context, stableId: String, text: String): Boolean {
        val action = replyActions[stableId] ?: run {
            Log.w("NotificationSync", "No live reply action is available")
            return false
        }
        val remoteInputs = action.remoteInputs?.takeIf { it.isNotEmpty() } ?: return false
        return try {
            val intent = Intent()
            val results = Bundle()
            for (ri in remoteInputs) {
                results.putCharSequence(ri.resultKey, text)
            }
            RemoteInput.addResultsToIntent(remoteInputs, intent, results)
            action.actionIntent.send(context, 0, intent)
            // A reply consumes the action; drop it so a stale id can't re-fire.
            synchronized(replyOrder) {
                if (replyActions.remove(stableId) != null) replyOrder.remove(stableId)
            }
            true
        } catch (_: Exception) {
            Log.w("NotificationSync", "Failed to send notification reply")
            false
        }
    }

    /** App launcher icon as a base64 PNG, so the desktop can show the real icon. */
    private fun appIconBase64(context: Context, packageName: String): String? {
        return try {
            val drawable: Drawable = context.packageManager.getApplicationIcon(packageName)
            val size = 96
            val bitmap = Bitmap.createBitmap(size, size, Bitmap.Config.ARGB_8888)
            val canvas = Canvas(bitmap)
            drawable.setBounds(0, 0, size, size)
            drawable.draw(canvas)
            val out = ByteArrayOutputStream()
            bitmap.compress(Bitmap.CompressFormat.PNG, 100, out)
            bitmap.recycle()
            Base64.encodeToString(out.toByteArray(), Base64.NO_WRAP)
        } catch (_: Exception) {
            null
        }
    }

    fun sendAndroidNotificationDismiss(context: Context, sbn: StatusBarNotification) {
        if (!isEnabled(context) || sbn.packageName == context.packageName) return
        val sid = stableId(sbn)
        synchronized(replyOrder) {
            if (replyActions.remove(sid) != null) replyOrder.remove(sid)
        }
        RelayForegroundService.sendNotificationDismiss(sid, sbn.packageName)
    }

    fun showMirroredNotification(context: Context, frame: JSONObject) {
        if (!isEnabled(context)) return
        if (Build.VERSION.SDK_INT >= 33 &&
            context.checkSelfPermission(Manifest.permission.POST_NOTIFICATIONS) !=
                PackageManager.PERMISSION_GRANTED
        ) {
            return
        }

        val title = frame.optString("title", "InputFlow notification")
        val body = frame.optString("body", "")
        val app = frame.optString("app", "")
        val stableId = frame.optString("stable_id", "$app:$title:$body")
        val manager = context.getSystemService(NotificationManager::class.java)
        ensureMirrorChannel(manager)

        val notification = Notification.Builder(context, MIRROR_CHANNEL_ID)
            .setSmallIcon(R.drawable.ic_notification)
            .setContentTitle(if (app.isBlank()) title else "$app: $title")
            .setContentText(body)
            .setStyle(Notification.BigTextStyle().bigText(body))
            .setAutoCancel(true)
            .build()

        manager.notify(notificationId(stableId), notification)
    }

    fun cancelMirroredNotification(context: Context, frame: JSONObject) {
        val stableId = frame.optString("stable_id", "")
        if (stableId.isBlank()) return
        context.getSystemService(NotificationManager::class.java)
            .cancel(notificationId(stableId))
    }

    private fun isEnabled(context: Context): Boolean =
        context.getSharedPreferences(RelayForegroundService.PREFS, Context.MODE_PRIVATE)
            .getBoolean(RelayForegroundService.KEY_NOTIFICATION_SYNC_ENABLED, false)

    private fun isSensitive(title: String, body: String): Boolean =
        sensitivePattern.containsMatchIn(title) || sensitivePattern.containsMatchIn(body)

    private fun stableId(sbn: StatusBarNotification): String {
        val tag = sbn.tag ?: ""
        return "${sbn.packageName}:${sbn.id}:$tag:${sbn.postTime}"
    }

    private fun appLabel(context: Context, packageName: String): String {
        return try {
            val pm = context.packageManager
            val info = pm.getApplicationInfo(packageName, 0)
            pm.getApplicationLabel(info).toString()
        } catch (_: Exception) {
            packageName
        }
    }

    private fun ensureMirrorChannel(manager: NotificationManager) {
        if (Build.VERSION.SDK_INT < Build.VERSION_CODES.O) return
        if (manager.getNotificationChannel(MIRROR_CHANNEL_ID) != null) return
        manager.createNotificationChannel(
            NotificationChannel(
                MIRROR_CHANNEL_ID,
                MIRROR_CHANNEL_NAME,
                NotificationManager.IMPORTANCE_DEFAULT
            )
        )
    }

    private fun notificationId(stableId: String): Int =
        (stableId.hashCode().toLong().absoluteValue % 900_000L).toInt() + 100_000
}
