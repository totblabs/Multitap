package me.totb.multitap_client

import android.Manifest
import android.app.Notification
import android.app.NotificationChannel
import android.app.NotificationManager
import android.app.PendingIntent
import android.content.Context
import android.content.Intent
import android.content.pm.PackageManager
import android.os.Build

/**
 * 공지 (notice-v1) that arrived while the app was not in front: one ordinary
 * notification on its own channel. It is posted only when notifications are
 * already allowed; this never asks for the permission. Title and text only:
 * nothing in a notice is linked or opened, and a tap just opens the app.
 */
object AndroidNotices {
    private const val CHANNEL = "notices"
    private const val BASE = 7400

    fun post(context: Context, title: String?, body: String?, id: Int?): Boolean {
        if (title.isNullOrBlank() || title.length > 260 || (body?.length ?: 0) > 2400) return false
        val notices = context.getSystemService(NotificationManager::class.java) ?: return false
        if (Build.VERSION.SDK_INT >= 33 && context.checkSelfPermission(Manifest.permission.POST_NOTIFICATIONS) != PackageManager.PERMISSION_GRANTED) return false
        if (!notices.areNotificationsEnabled()) return false
        notices.createNotificationChannel(
            NotificationChannel(CHANNEL, MultitapText.get(context, R.string.notices_channel), NotificationManager.IMPORTANCE_DEFAULT).apply {
                description = MultitapText.get(context, R.string.notices_channel_description)
            })
        if (notices.getNotificationChannel(CHANNEL)?.importance == NotificationManager.IMPORTANCE_NONE) return false
        val back = Intent(context, MainActivity::class.java)
            .setAction(Intent.ACTION_MAIN).addCategory(Intent.CATEGORY_LAUNCHER)
            .addFlags(Intent.FLAG_ACTIVITY_SINGLE_TOP or Intent.FLAG_ACTIVITY_CLEAR_TOP)
        val open = PendingIntent.getActivity(context, 0, back, PendingIntent.FLAG_UPDATE_CURRENT or PendingIntent.FLAG_IMMUTABLE)
        val text = body?.takeIf { it.isNotBlank() }
        val builder = Notification.Builder(context, CHANNEL)
            .setSmallIcon(R.drawable.multitap_notification)
            .setContentTitle(title)
            .setContentIntent(open)
            .setAutoCancel(true)
            .setCategory(Notification.CATEGORY_MESSAGE)
            .setVisibility(Notification.VISIBILITY_PRIVATE)
        if (text != null) builder.setContentText(text).setStyle(Notification.BigTextStyle().bigText(text))
        return try {
            notices.notify(BASE + ((id ?: 0) and 0x3ff), builder.build())
            true
        } catch (_: Exception) {
            false
        }
    }
}
