package com.inputflow.android

import android.Manifest
import android.app.Notification
import android.app.NotificationChannel
import android.app.NotificationManager
import android.content.Context
import android.content.pm.PackageManager
import android.os.Build
import android.service.notification.NotificationListenerService
import android.service.notification.StatusBarNotification
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

        RelayForegroundService.instance?.sendNotificationUpsert(
            stableId = stableId(sbn),
            app = appLabel(context, sbn.packageName),
            packageName = sbn.packageName,
            title = title,
            body = body,
            postedAtMs = sbn.postTime
        )
    }

    fun sendAndroidNotificationDismiss(context: Context, sbn: StatusBarNotification) {
        if (!isEnabled(context) || sbn.packageName == context.packageName) return
        RelayForegroundService.instance?.sendNotificationDismiss(stableId(sbn), sbn.packageName)
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
