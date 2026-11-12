package me.totb.multitap_client

import android.Manifest
import android.app.Activity
import android.app.Notification
import android.app.NotificationChannel
import android.app.NotificationManager
import android.app.PendingIntent
import android.app.Service
import android.content.Context
import android.content.Intent
import android.content.pm.PackageManager
import android.content.pm.ServiceInfo
import android.os.Build
import android.os.Handler
import android.os.IBinder
import android.os.Looper
import android.net.Uri

/** User-started remote desktop connection lifetime only; no socket/secret/VM data. */
class AndroidRemoteSessionService : Service() {
    companion object {
        private const val CHANNEL = "remote-session"
        private const val NOTICE = 7321
        private const val LEASE = "lease"
        private const val DISCONNECT = "me.totb.multitap_client.DISCONNECT_REMOTE"
        private var wanted: String? = null
        private var active: String? = null
        private var pending: ((String) -> Unit)? = null
        private val main = Handler(Looper.getMainLooper())
        private var running: AndroidRemoteSessionService? = null
        private var bridgeToken: Any? = null
        private var disconnectBridge: ((String) -> Unit)? = null
        private var disconnecting: String? = null
        private var disconnectFailed: String? = null

        fun attachDisconnectBridge(send: (String) -> Unit): Any = Any().also {
            bridgeToken = it
            disconnectBridge = send
        }

        fun detachDisconnectBridge(token: Any?) {
            if (token !== bridgeToken) return
            bridgeToken = null
            disconnectBridge = null
        }

        fun ensureNotificationChannel(context: Context) {
            context.getSystemService(NotificationManager::class.java).createNotificationChannel(
                NotificationChannel(CHANNEL, MultitapText.get(context, R.string.session_channel), NotificationManager.IMPORTANCE_LOW).apply {
                    description = MultitapText.get(context, R.string.session_channel_description)
                    setShowBadge(false)
                })
        }

        fun notificationsVisible(context: Context): Boolean {
            ensureNotificationChannel(context)
            val notices = context.getSystemService(NotificationManager::class.java)
            return (Build.VERSION.SDK_INT < 33 || context.checkSelfPermission(Manifest.permission.POST_NOTIFICATIONS) == PackageManager.PERMISSION_GRANTED) &&
                notices.areNotificationsEnabled() && notices.getNotificationChannel(CHANNEL)?.importance != NotificationManager.IMPORTANCE_NONE
        }

        private fun requestDisconnect(lease: String) {
            if (wanted != lease || disconnecting == lease) return
            disconnecting = lease
            disconnectFailed = null
            running?.refreshNotification(lease)
            try { disconnectBridge?.invoke(lease) } catch (_: Exception) {}
            // Keep the notification/service until the owning Dart session
            // releases its lease. Do not pretend a request ended the relay.
            main.postDelayed({
                if (wanted == lease && disconnecting == lease) {
                    disconnecting = null
                    disconnectFailed = lease
                    running?.refreshNotification(lease)
                }
            }, 15000)
        }

        // All entry points and Service callbacks run on the Android main thread.
        fun start(activity: Activity, foreground: Boolean, lease: String, reply: (String) -> Unit) {
            if (!Regex("[a-f0-9]{32}").matches(lease)) { reply("failed"); return }
            if (!foreground || activity.isFinishing || activity.isDestroyed) { reply("background"); return }
            pending?.invoke("superseded")
            wanted = lease
            disconnecting = null
            disconnectFailed = null
            pending = reply
            try {
                activity.startForegroundService(Intent(activity, AndroidRemoteSessionService::class.java).putExtra(LEASE, lease))
                main.postDelayed({
                    if (wanted == lease && pending != null) fail(activity.applicationContext, lease)
                }, 10000)
            } catch (_: Exception) {
                fail(activity.applicationContext, lease)
            }
        }

        fun stop(context: Context, lease: String): Boolean {
            if (wanted != lease) return false
            wanted = null
            active = null
            disconnecting = null
            disconnectFailed = null
            val callback = pending
            pending = null
            callback?.invoke("superseded")
            context.stopService(Intent(context, AndroidRemoteSessionService::class.java))
            return true
        }

        private fun fail(context: Context, lease: String) {
            if (wanted != lease) return
            val callback = pending
            pending = null
            wanted = null
            active = null
            context.stopService(Intent(context, AndroidRemoteSessionService::class.java))
            callback?.invoke("failed")
        }
    }

    private var ownedLease: String? = null

    override fun onBind(intent: Intent?): IBinder? = null

    override fun onCreate() {
        super.onCreate()
        running = this
    }

    private fun notification(lease: String): Notification {
        ensureNotificationChannel(this)
        val back = Intent(this, MainActivity::class.java)
            .setAction(Intent.ACTION_MAIN).addCategory(Intent.CATEGORY_LAUNCHER)
            .addFlags(Intent.FLAG_ACTIVITY_SINGLE_TOP or Intent.FLAG_ACTIVITY_CLEAR_TOP)
        val open = PendingIntent.getActivity(this, 0, back, PendingIntent.FLAG_UPDATE_CURRENT or PendingIntent.FLAG_IMMUTABLE)
        val stop = PendingIntent.getService(this, 0,
            Intent(this, AndroidRemoteSessionService::class.java).setAction(DISCONNECT)
                .setData(Uri.parse("multitap://disconnect/$lease")).putExtra(LEASE, lease),
            PendingIntent.FLAG_UPDATE_CURRENT or PendingIntent.FLAG_IMMUTABLE)
        return Notification.Builder(this, CHANNEL)
            .setSmallIcon(R.drawable.multitap_notification)
            .setContentTitle(MultitapText.get(this, if (disconnecting == lease) R.string.session_disconnecting else R.string.session_running))
            .setContentText(MultitapText.get(this, if (disconnectFailed == lease) R.string.session_disconnect_failed else R.string.session_tap_to_return))
            .setContentIntent(open)
            .addAction(Notification.Action.Builder(null, MultitapText.get(this, R.string.session_action_open), open).build())
            .addAction(Notification.Action.Builder(null, MultitapText.get(this, R.string.session_action_disconnect), stop).build())
            .setOngoing(true).setOnlyAlertOnce(true).setShowWhen(false)
            .setCategory(Notification.CATEGORY_SERVICE)
            .setVisibility(Notification.VISIBILITY_PRIVATE)
            .build()
    }

    private fun refreshNotification(lease: String) {
        if (wanted == lease && ownedLease == lease) {
            try { getSystemService(NotificationManager::class.java).notify(NOTICE, notification(lease)) }
            catch (_: Exception) {}
        }
    }

    override fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int {
        val lease = intent?.getStringExtra(LEASE)
        if (intent?.action == DISCONNECT) {
            if (lease != null) requestDisconnect(lease)
            if (active == null && wanted == null) stopSelfResult(startId)
            return START_NOT_STICKY
        }
        // No sticky restart, boot receiver or replay of a retired start intent.
        if (lease == null || lease != wanted) {
            if (active == null) stopSelfResult(startId)
            return START_NOT_STICKY
        }
        try {
            val notification = notification(lease)
            if (Build.VERSION.SDK_INT >= 34) {
                startForeground(NOTICE, notification, ServiceInfo.FOREGROUND_SERVICE_TYPE_SPECIAL_USE)
            } else {
                startForeground(NOTICE, notification)
            }
            active = lease
            ownedLease = lease
            val callback = pending
            pending = null
            // Notification permission is NOT an FGS prerequisite. Android's
            // task manager still exposes the service if the drawer is blocked.
            val visible = notificationsVisible(this)
            callback?.invoke(if (visible) "started" else "startedWithoutNotificationPermission")
        } catch (_: Exception) {
            fail(applicationContext, lease)
            stopSelfResult(startId)
        }
        return START_NOT_STICKY
    }

    override fun onDestroy() {
        if (running === this) running = null
        // OS stop ends this lifetime. Never schedule an automatic restart.
        val owned = ownedLease
        if (owned != null && active == owned) active = null
        // A delayed destruction of A must not retire a newly requested B.
        if (owned != null && wanted == owned) {
            val callback = pending
            pending = null
            wanted = null
            callback?.invoke("failed")
        }
        super.onDestroy()
    }
}
