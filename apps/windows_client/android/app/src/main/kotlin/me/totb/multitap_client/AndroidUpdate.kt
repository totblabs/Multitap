package me.totb.multitap_client

import android.app.PendingIntent
import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent
import android.content.IntentFilter
import android.content.pm.PackageInfo
import android.content.pm.PackageInstaller
import android.content.pm.PackageManager
import android.net.Uri
import android.os.Build
import android.os.Handler
import android.os.Looper
import android.provider.Settings
import io.flutter.embedding.engine.FlutterEngine
import io.flutter.plugin.common.MethodCall
import io.flutter.plugin.common.MethodChannel
import java.io.File
import java.util.concurrent.Executors
import java.util.UUID

private class UpdateError(val code: String) : Exception(code)

/**
 * In-app update hand-off. Dart downloads the configured server's catalog APK
 * into app-private cache and checks its size and SHA-256; this class checks
 * package, split, version and signer, copies exactly the hashed bytes into a
 * PackageInstaller session and commits it. Android always shows its own
 * install confirmation (user action required); nothing installs silently.
 */
class AndroidUpdate(private val activity: MainActivity, engine: FlutterEngine) {
    private val channel = MethodChannel(engine.dartExecutor.binaryMessenger, "multitap/android/update")
    private val main = Handler(Looper.getMainLooper())
    private val worker = Executors.newSingleThreadExecutor()
    private val action = "${activity.packageName}.UPDATE_STATUS"
    private val permission = "${activity.packageName}.UPDATE_STATUS_PERMISSION"
    // A late old view can never download/discard a newer Activity's APK.
    private val directory = File(activity.cacheDir, "updates/${UUID.randomUUID()}")
    private val ownership = AndroidUpdateOwnership.shared()
    @Volatile private var disposed = false

    // Only this app (signature permission, and not exported on Android 13+)
    // can deliver a status: a forged one could otherwise launch any intent.
    private val receiver = object : BroadcastReceiver() {
        override fun onReceive(context: Context, intent: Intent) {
            if (intent.action != action) return
            val id = intent.getIntExtra(PackageInstaller.EXTRA_SESSION_ID, -1)
            if (!ownership.accepts(id)) return
            val status = intent.getIntExtra(PackageInstaller.EXTRA_STATUS, PackageInstaller.STATUS_FAILURE)
            if (status == PackageInstaller.STATUS_PENDING_USER_ACTION) {
                val confirm = if (Build.VERSION.SDK_INT >= 33) {
                    intent.getParcelableExtra(Intent.EXTRA_INTENT, Intent::class.java)
                } else {
                    @Suppress("DEPRECATION") intent.getParcelableExtra(Intent.EXTRA_INTENT)
                }
                var opened = false
                try {
                    if (confirm == null) throw IllegalStateException()
                    activity.startActivity(confirm)
                    opened = true
                } catch (_: Exception) {
                    try {
                        if (ownership.abort(id) { activity.packageManager.packageInstaller.abandonSession(id) }) {
                            if (!disposed) channel.invokeMethod("installStatus", mapOf("status" to "installer_start_failed"))
                        }
                    } catch (_: Exception) {
                        // OS state remains unverified: do not release another install into this session.
                        if (!disposed && ownership.accepts(id)) {
                            channel.invokeMethod("installStatus", mapOf("status" to "installer_state_unverified"))
                        }
                    }
                }
                // A step, not a result: Android's confirmation is open and waits for the person.
                if (opened && !disposed) channel.invokeMethod("installStatus", mapOf("status" to "confirm_opened"))
                return
            }
            finish(id, AndroidUpdatePolicy.statusCode(status))
        }
    }

    init {
        val filter = IntentFilter(action)
        if (Build.VERSION.SDK_INT >= 33) {
            activity.registerReceiver(receiver, filter, permission, null, Context.RECEIVER_NOT_EXPORTED)
        } else {
            activity.registerReceiver(receiver, filter, permission, null)
        }
        // Do not sweep sessions of another Activity/process. Retired in-process ownership
        // is cleaned by exact recorded session ID when a new mutable request arrives.
        channel.setMethodCallHandler(::call)
    }

    private fun finish(id: Int, code: String) {
        if (!ownership.finish(id)) return
        if (!disposed) channel.invokeMethod("installStatus", mapOf("status" to code))
    }

    private fun versionCode(info: PackageInfo): Long =
        if (Build.VERSION.SDK_INT >= 28) info.longVersionCode else @Suppress("DEPRECATION") info.versionCode.toLong()

    private fun signers(info: PackageInfo?): Set<String> {
        if (info == null) return emptySet()
        val signatures = if (Build.VERSION.SDK_INT >= 28) {
            info.signingInfo?.apkContentsSigners
        } else {
            @Suppress("DEPRECATION") info.signatures
        }
        return signatures?.map { AndroidUpdatePolicy.hex(it.toByteArray()) }?.toSet() ?: emptySet()
    }

    private fun flags(): Int =
        if (Build.VERSION.SDK_INT >= 28) PackageManager.GET_SIGNING_CERTIFICATES else @Suppress("DEPRECATION") PackageManager.GET_SIGNATURES

    private fun installed(): PackageInfo = activity.packageManager.getPackageInfo(activity.packageName, flags())

    private fun retirePrevious() {
        try {
            ownership.recoverRetired { activity.packageManager.packageInstaller.abandonSession(it) }
        } catch (_: Exception) { throw UpdateError("installer_state_unverified") }
    }

    private fun call(call: MethodCall, result: MethodChannel.Result) {
        try {
            if (disposed) throw UpdateError("cancelled")
            when (call.method) {
                "abi" -> result.success(AndroidUpdatePolicy.installedAbi(versionCode(installed()), Build.SUPPORTED_ABIS.toList()))
                "directory" -> {
                    retirePrevious()
                    if (ownership.busy()) throw UpdateError("update_busy")
                    val dir = directory
                    if (!dir.isDirectory && !dir.mkdirs()) throw UpdateError("storage_failed")
                    // A previous, unfinished download never lingers.
                    dir.listFiles()?.filter { it.isFile }?.forEach { it.delete() }
                    result.success(dir.canonicalPath)
                }
                "canInstall" -> result.success(activity.packageManager.canRequestPackageInstalls())
                "openInstallSettings" -> {
                    activity.startActivity(
                        Intent(Settings.ACTION_MANAGE_UNKNOWN_APP_SOURCES, Uri.parse("package:${activity.packageName}")),
                    )
                    result.success(true)
                }
                "discard" -> {
                    retirePrevious()
                    if (ownership.busy()) throw UpdateError("update_busy")
                    val file = File(call.argument<String>("path") ?: "")
                    if (AndroidUpdatePolicy.inside(directory, file)) file.delete()
                    result.success(null)
                }
                "install" -> install(call, result)
                else -> result.notImplemented()
            }
        } catch (error: UpdateError) {
            result.error(error.code, null, null)
        } catch (_: Exception) {
            result.error("installer_start_failed", null, null)
        }
    }

    private fun install(call: MethodCall, result: MethodChannel.Result) {
        retirePrevious()
        val request = AndroidUpdatePolicy.request(call.arguments, directory) ?: throw UpdateError("invalid_update")
        val lease = ownership.acquire() ?: throw UpdateError("update_busy")
        try {
            if (!activity.packageManager.canRequestPackageInstalls()) throw UpdateError("install_permission_required")
            worker.execute {
                var id = -1
                var failed = true
                var installer: PackageInstaller? = null
                try {
                    if (!ownership.owns(lease)) throw UpdateError("cancelled")
                    val activeInstaller = activity.packageManager.packageInstaller
                    installer = activeInstaller
                    val current = installed()
                    val archive = activity.packageManager.getPackageArchiveInfo(request.file.path, flags())
                        ?: throw UpdateError("package_mismatch")
                    if (archive.packageName != activity.packageName || archive.versionName != request.version) {
                        throw UpdateError("package_mismatch")
                    }
                    if (!AndroidUpdatePolicy.acceptableVersionCode(versionCode(current), versionCode(archive))) {
                        throw UpdateError("not_newer")
                    }
                    if (!AndroidUpdatePolicy.signersAcceptable(signers(current), signers(archive))) {
                        throw UpdateError("package_mismatch")
                    }
                    val params = PackageInstaller.SessionParams(PackageInstaller.SessionParams.MODE_FULL_INSTALL).apply {
                        setAppPackageName(activity.packageName)
                        setSize(request.size)
                        setInstallReason(PackageManager.INSTALL_REASON_USER)
                        if (Build.VERSION.SDK_INT >= 31) {
                            setRequireUserAction(PackageInstaller.SessionParams.USER_ACTION_REQUIRED)
                        }
                    }
                    id = activeInstaller.createSession(params)
                    if (!ownership.bind(lease, id)) throw UpdateError("cancelled")
                    activeInstaller.openSession(id).use { open ->
                        val hashed = request.file.inputStream().use { input ->
                            open.openWrite("Multitap.apk", 0, request.size).use { output ->
                                // The dialog's bar: bytes written of the known size, about once a
                                // mebibyte and at the end. Nothing else in this hand-off has a measure.
                                var reported = 0L
                                val value = AndroidUpdatePolicy.copyVerified(input, output, request.size) { copied ->
                                    if (copied - reported >= 1024 * 1024 || copied == request.size) {
                                        reported = copied
                                        main.post {
                                            if (!disposed) {
                                                channel.invokeMethod("installProgress", mapOf("copied" to copied, "size" to request.size))
                                            }
                                        }
                                    }
                                }
                                open.fsync(output)
                                value
                            }
                        }
                        if (hashed != request.sha256) throw UpdateError("hash_mismatch")
                        val intent = Intent(action).setPackage(activity.packageName)
                        val pending = PendingIntent.getBroadcast(
                            activity, id, intent,
                            PendingIntent.FLAG_UPDATE_CURRENT or (if (Build.VERSION.SDK_INT >= 31) PendingIntent.FLAG_MUTABLE else 0),
                        )
                        if (!ownership.runIfOwned(lease) { open.commit(pending.intentSender) }) {
                            throw UpdateError("cancelled")
                        }
                    }
                    failed = false
                    main.post { if (!disposed) result.success("handed_off") }
                } catch (error: Exception) {
                    var abandoned = true
                    if (id >= 0) try {
                        checkNotNull(installer).abandonSession(id)
                    } catch (_: Exception) { abandoned = false }
                    // A failed commit/abandon can leave OS state unknown. Keep its owner until
                    // an actual terminal callback or disposal, rather than admitting another install.
                    failed = abandoned
                    val code = if (!abandoned) "installer_state_unverified"
                        else (error as? UpdateError)?.code ?: "installer_start_failed"
                    main.post { if (!disposed) result.error(code, null, null) }
                } finally {
                    // Handed to the session, or failed: the downloaded copy is not kept.
                    ownership.workerFinished(lease, failed) { request.file.delete() }
                }
            }
        } catch (error: Exception) {
            ownership.workerFinished(lease, failed = true) { request.file.delete() }
            throw error
        }
    }

    fun dispose() {
        disposed = true
        val session = ownership.session()
        if (session >= 0) try {
            ownership.abort(session) { activity.packageManager.packageInstaller.abandonSession(session) }
        } catch (_: Exception) { /* Exact pending OS state stays busy until independently abandoned. */ }
        ownership.dispose()
        try { activity.unregisterReceiver(receiver) } catch (_: Exception) {}
        worker.shutdown()
    }
}
