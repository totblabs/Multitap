package me.totb.multitap_client

import android.app.Activity
import android.content.ActivityNotFoundException
import android.content.ContentResolver
import android.content.Context
import android.content.Intent
import android.database.Cursor
import android.net.Uri
import android.os.Build
import android.os.CancellationSignal
import android.os.Environment
import android.os.Handler
import android.os.Looper
import android.os.OperationCanceledException
import android.os.ParcelFileDescriptor
import android.os.StatFs
import android.os.storage.StorageManager
import android.provider.DocumentsContract
import android.provider.DocumentsContract.Document
import android.provider.Settings
import android.system.ErrnoException
import android.system.Os
import android.system.OsConstants
import android.webkit.MimeTypeMap
import io.flutter.embedding.engine.FlutterEngine
import io.flutter.plugin.common.MethodCall
import io.flutter.plugin.common.MethodChannel
import java.io.File
import java.io.FileNotFoundException
import java.io.IOException
import java.util.Locale
import java.util.concurrent.Executors
import java.util.concurrent.LinkedBlockingQueue
import java.util.concurrent.ScheduledExecutorService
import java.util.concurrent.ThreadPoolExecutor
import java.util.concurrent.TimeUnit

// drive-v1 on Android (docs/drive-redirection.md "Android", lib/android/android_drive.dart): a
// folder the person picks with Android's own folder picker (ACTION_OPEN_DOCUMENT_TREE) is shared
// with the desktop for this connection. The app asks for no storage permission: it reaches only
// the picked tree, through the Storage Access Framework, with the grant the picker gave.
//
// Channel `multitap/android/drive` (the methods of the Windows `multitap/drive` worker):
//   pickFolder                       -> {path, name, where, whole} | null (cancelled)
//   open {share, path, readOnly}     -> {name}
//   close {share}
//   status {share}                   -> {deleted, offline, revoked, path, name}
//   stat / list / read / write / setSize / flush / create / rename / delete / touch
// `path` of a share is the tree URI the picker returned; only URIs this picker returned in this
// run are ever opened. Failures are PlatformException with one fixed drive_* code.
//
// Every provider call runs on a small pool, never on the main thread; a listing or an open the
// provider does not finish is cancelled after [providerTimeoutMs]. File data moves in pieces of
// at most 256 KiB and is never kept beyond one request.
class AndroidDrive(private val activity: Activity, engine: FlutterEngine) {
    private val channel = MethodChannel(engine.dartExecutor.binaryMessenger, "multitap/android/drive")
    private val main = Handler(Looper.getMainLooper())
    private val resolver: ContentResolver = activity.contentResolver
    // As many as the session admits at once (driveMaxInFlight): a provider that hangs holds one.
    private val pool = ThreadPoolExecutor(8, 8, 30, TimeUnit.SECONDS, LinkedBlockingQueue()) { task ->
        Thread(task, "multitap-drive").apply { isDaemon = true }
    }.apply { allowCoreThreadTimeOut(true) }
    private val ordered = KeyedSerial(pool)
    private val timer: ScheduledExecutorService = Executors.newSingleThreadScheduledExecutor { task ->
        Thread(task, "multitap-drive-timer").apply { isDaemon = true }
    }
    /** One open share: its picked tree, or (tree null) its folder of all-files access. */
    private class Shared(val tree: Uri?, val path: String, val share: DriveShare)
    private val shares = HashMap<String, Shared>()
    /** Trees the picker returned in this run, and whether Android kept the grant for each. */
    private val picked = HashMap<Uri, Boolean>()
    /** Folders the person chose in this app's own folder list in this run (all-files access). */
    private val pickedFolders = HashSet<String>()
    private var picking: MethodChannel.Result? = null
    private var pickShown = false
    private var disposed = false

    companion object {
        const val pickRequest = 7322
        const val providerTimeoutMs = 35000L
        private const val external = "com.android.externalstorage.documents"
        private const val pickShownTimeoutMs = 3000L
        private const val pickOff = 1
        private const val pickNone = 2
        private const val pickRefused = 3
        private const val pickNotStarted = 4
        private const val pickNotShown = 5
        // 6 is the Dart side's: this app has no folder engine.
        private const val pickOffPlatform = 7
        private const val pickSilent = 8
        /** The platform's folder picker (DocumentsUI), as Google and AOSP ship it. */
        private val pickerPackages = listOf("com.google.android.documentsui", "com.android.documentsui")
    }

    init {
        // Nothing of an earlier run is shared again: grants it left behind are given back.
        pool.execute { releaseAll() }
        timer.scheduleWithFixedDelay({ sweep() }, 1, 1, TimeUnit.SECONDS)
        channel.setMethodCallHandler(::handle)
    }

    fun dispose() {
        if (disposed) return
        disposed = true
        channel.setMethodCallHandler(null)
        picking?.success(null)
        picking = null
        val open = synchronized(shares) { shares.values.map { it.share }.also { shares.clear() } }
        timer.shutdownNow()
        pool.execute {
            for (share in open) try { share.close() } catch (_: Exception) {}
            releaseAll()
        }
        pool.shutdown()
    }

    /**
     * Opens Android's folder picker for [reply]. The request carries no flags: the picker's own
     * result brings the grant (the form the platform documents; with grant flags on the request
     * the picker never opens on a Galaxy Fold).
     *
     * What went wrong travels as the error's details, for the words and the record:
     * 1 the picker app (Google's) is turned off, 2 this device has none (some TVs), 3 Android
     * refused, 4 it could not be started, 5 it was started and never came to the front, 7 the
     * picker app (AOSP's, the only one) is turned off, 8 the picker app is on and did not answer.
     * A Galaxy Fold answers 1 when its 파일 app is turned off.
     */
    private fun pick(reply: MethodChannel.Result) {
        picking = reply
        pickShown = false
        var doubt = 0
        val request = Intent(Intent.ACTION_OPEN_DOCUMENT_TREE)
        // Asked first, never tried: a start that nothing answers is refused, and Android then
        // hands this activity a cancelled result, pausing and resuming it around that. On a
        // Galaxy Fold the app is left "not in front" by it (lifecycle inactive 13 ms after 폴더
        // 추가, until the person goes Home and back) and takes no input.
        val answered = try { request.resolveActivity(activity.packageManager) != null } catch (_: Exception) { true }
        if (!answered) return pickFailed(reply, noPicker())
        try {
            activity.startActivityForResult(request, pickRequest)
        } catch (_: ActivityNotFoundException) {
            return pickFailed(reply, noPicker())
        } catch (_: SecurityException) {
            return pickFailed(reply, pickRefused)
        } catch (_: Exception) {
            // Thrown around the start, not by its refusal: the picker may be opening all the same.
            doubt = pickNotStarted
        }
        // A picker that opened took the front from this activity (paused()). One that never does
        // must not hold the next 폴더 추가 back for good.
        main.postDelayed({
            if (picking === reply && !pickShown) pickFailed(reply, if (doubt != 0) doubt else pickNotShown)
        }, pickShownTimeoutMs)
    }

    /** Why nothing answers the picker's request: its app is turned off, absent, or on and silent. */
    private fun noPicker(): Int {
        val (name, enabled) = pickerState()
        return when {
            name == null -> pickNone
            enabled -> pickSilent
            name == pickerPackages.first() -> pickOff
            else -> pickOffPlatform
        }
    }

    private fun pickFailed(reply: MethodChannel.Result, why: Int) {
        if (picking === reply) picking = null
        reply.error("drive_unavailable", null, why)
    }

    /** MainActivity.onPause: something took the front; with a pick outstanding, its picker. */
    fun paused() { if (picking != null) pickShown = true }

    /**
     * MainActivity.onResume. A picker's result is delivered before this (Activity.onActivityResult:
     * "immediately before onResume()"), so a pick still outstanding here has no picker any more:
     * it ends as cancelled instead of answering every later 폴더 추가 with drive_busy.
     */
    fun resumed() {
        val reply = picking ?: return
        if (!pickShown) return
        picking = null
        reply.success(null)
    }

    /**
     * The system's folder picker app on this device and whether it is turned on: Google's where
     * it is installed (its stand-in from AOSP may sit beside it, switched off by the maker),
     * else AOSP's; (null, false) with neither.
     */
    private fun pickerState(): Pair<String?, Boolean> {
        for (name in pickerPackages) {
            val info = try { activity.packageManager.getApplicationInfo(name, 0) } catch (_: Exception) { continue }
            return name to info.enabled
        }
        return null to false
    }

    /**
     * `pickerSettings`: Android's own page of the picker app that is turned off, where the
     * person turns it on (this app cannot). False when there is no such app or no such page.
     */
    private fun openPickerSettings(): Boolean {
        val (name, enabled) = pickerState()
        if (name == null || enabled) return false
        return try {
            activity.startActivity(Intent(Settings.ACTION_APPLICATION_DETAILS_SETTINGS, Uri.parse("package:$name")))
            true
        } catch (_: Exception) { false }
    }

    /** MainActivity.onActivityResult: true when the result was the folder picker's. */
    fun activityResult(request: Int, result: Int, data: Intent?): Boolean {
        if (request != pickRequest) return false
        val reply = picking ?: return true
        picking = null
        pickShown = true
        val tree = if (result == Activity.RESULT_OK) data?.data else null
        if (tree == null || disposed) { reply.success(null); return true }
        pool.execute {
            val answer = try { describe(tree, data?.flags ?: 0) } catch (failure: DriveFailure) { failure }
            main.post {
                if (answer is DriveFailure) reply.error(answer.code, null, null) else reply.success(answer)
            }
        }
        return true
    }

    private fun describe(tree: Uri, flags: Int): Map<String, Any?> {
        if (!DocumentsContract.isTreeUri(tree)) throw DriveFailure("drive_invalid_path")
        val modes = Intent.FLAG_GRANT_READ_URI_PERMISSION or Intent.FLAG_GRANT_WRITE_URI_PERMISSION
        // Kept by Android across a rotation or the app being stopped behind another one; given
        // back when the share ends (releaseAll) so grants never pile up.
        val kept = try { resolver.takePersistableUriPermission(tree, flags and modes); true } catch (_: Exception) { false }
        synchronized(picked) { picked[tree] = kept }
        val store = SafDriveStore(activity, tree, kept, timer)
        val root = store.root()
        val id = DocumentsContract.getTreeDocumentId(tree)
        val volume = id.substringBefore(':', "")
        val inside = id.substringAfter(':', "")
        val local = tree.authority == external && id.contains(':')
        val where = when {
            local && volume == "primary" -> if (inside.isEmpty()) internalName() else "${internalName()}/$inside"
            local -> if (inside.isEmpty()) externalName() else "${externalName()}/$inside"
            else -> listOfNotNull(providerLabel(tree), root.name.ifEmpty { null }).joinToString("/")
        }
        return mapOf("path" to tree.toString(), "name" to root.name, "where" to where.take(300), "whole" to (local && inside.isEmpty()))
    }

    private fun providerLabel(tree: Uri): String? = try {
        val authority = tree.authority ?: return null
        activity.packageManager.resolveContentProvider(authority, 0)?.loadLabel(activity.packageManager)?.toString()?.take(60)
    } catch (_: Exception) { null }

    private fun releaseAll() {
        try {
            for (grant in resolver.persistedUriPermissions) {
                if (!DocumentsContract.isTreeUri(grant.uri)) continue
                val held = synchronized(shares) { shares.values.any { it.tree == grant.uri } }
                if (held) continue
                try {
                    resolver.releasePersistableUriPermission(grant.uri,
                        (if (grant.isReadPermission) Intent.FLAG_GRANT_READ_URI_PERMISSION else 0) or
                            (if (grant.isWritePermission) Intent.FLAG_GRANT_WRITE_URI_PERMISSION else 0))
                } catch (_: Exception) {}
            }
        } catch (_: Exception) {}
    }

    private fun sweep() {
        val open = synchronized(shares) { shares.values.map { it.share } }
        for (share in open) try { share.sweep() } catch (_: Exception) {}
    }

    private fun share(call: MethodCall): DriveShare {
        val id = call.argument<String>("share") ?: throw DriveFailure("drive_share_unknown")
        return synchronized(shares) { shares[id]?.share } ?: throw DriveFailure("drive_share_unknown")
    }

    // ---- all-files access: this app's own folder list, where Android's picker cannot open ----
    //
    // Android 11+ only (Environment.isExternalStorageManager). The app
    // asks for the access only when the person takes this way; the desktop still sees only the
    // folder they chose, and only a folder chosen in this run is ever opened.

    private fun allFiles(): Boolean = Build.VERSION.SDK_INT >= 30 && Environment.isExternalStorageManager()

    /** The mounted shared-storage volumes: (root, primary). */
    private fun volumes(): List<Pair<File, Boolean>> {
        if (Build.VERSION.SDK_INT < 30) return emptyList()
        val manager = activity.getSystemService(Context.STORAGE_SERVICE) as? StorageManager ?: return emptyList()
        return manager.storageVolumes.mapNotNull { volume ->
            val root = volume.directory ?: return@mapNotNull null
            val state = volume.state
            if (state != Environment.MEDIA_MOUNTED && state != Environment.MEDIA_MOUNTED_READ_ONLY) return@mapNotNull null
            root to volume.isPrimary
        }
    }

    /** [path] as a folder inside one of the volumes: (folder, its volume's root, primary), else drive_invalid_path. */
    private fun located(path: String?): Triple<File, File, Boolean> {
        if (path == null || !path.startsWith("/") || path.length > 4096) throw DriveFailure("drive_invalid_path")
        val folder = try { File(path).canonicalFile } catch (_: Exception) { throw DriveFailure("drive_invalid_path") }
        for ((root, primary) in volumes()) {
            val base = try { root.canonicalPath } catch (_: Exception) { continue }
            if (folder.path == base || folder.path.startsWith("$base/")) return Triple(folder, File(base), primary)
        }
        throw DriveFailure("drive_invalid_path")
    }

    private fun internalName() = MultitapText.get(activity, R.string.storage_internal)
    private fun externalName() = MultitapText.get(activity, R.string.storage_external)

    private fun whereOf(folder: File, root: File, primary: Boolean): String {
        val inside = folder.path.removePrefix(root.path).trimStart('/')
        val volume = if (primary) internalName() else externalName()
        return (if (inside.isEmpty()) volume else "$volume/$inside").take(300)
    }

    /** `storageFolders {path}`: the volumes (no path), else the folders inside [path], by name. */
    private fun storageFolders(path: String?): Map<String, Any?> {
        if (!allFiles()) throw DriveFailure("drive_denied")
        if (path == null) {
            return mapOf("folders" to volumes().map { (root, primary) ->
                mapOf("path" to root.path, "name" to if (primary) internalName() else externalName())
            })
        }
        val (folder, root, primary) = located(path)
        val listed = folder.listFiles() ?: throw DriveFailure(if (folder.isDirectory) "drive_denied" else "drive_not_found")
        val folders = listed.asSequence().filter { it.isDirectory && !it.name.startsWith(".") }
            .sortedBy { it.name.lowercase(Locale.ROOT) }.take(2000)
            .map { mapOf("path" to it.path, "name" to it.name) }.toList()
        return mapOf("folders" to folders, "path" to folder.path, "where" to whereOf(folder, root, primary),
            "parent" to if (folder.path == root.path) null else folder.parentFile?.path)
    }

    /** `storagePick {path}`: the folder the person chose in the list, as the picker answers one. */
    private fun storagePick(path: String?): Map<String, Any?> {
        if (!allFiles()) throw DriveFailure("drive_denied")
        val (folder, root, primary) = located(path)
        if (!folder.isDirectory) throw DriveFailure("drive_not_found")
        synchronized(pickedFolders) { pickedFolders.add(folder.path) }
        val whole = folder.path == root.path
        return mapOf("path" to folder.path, "name" to if (whole) (if (primary) internalName() else MultitapText.get(activity, R.string.storage_device)) else folder.name,
            "where" to whereOf(folder, root, primary), "whole" to whole)
    }

    /** `storageSettings`: Android's page where the person allows (or takes back) all-files access. */
    private fun openStorageSettings(): Boolean {
        if (Build.VERSION.SDK_INT < 30) return false
        for (intent in listOf(
            Intent(Settings.ACTION_MANAGE_APP_ALL_FILES_ACCESS_PERMISSION, Uri.parse("package:${activity.packageName}")),
            Intent(Settings.ACTION_MANAGE_ALL_FILES_ACCESS_PERMISSION),
        )) {
            try { activity.startActivity(intent); return true } catch (_: Exception) {}
        }
        return false
    }

    private fun handle(call: MethodCall, result: MethodChannel.Result) {
        if (call.method == "pickFolder") {
            if (picking != null) { result.error("drive_busy", null, null); return }
            pick(result)
            return
        }
        if (call.method == "pickerSettings") { result.success(openPickerSettings()); return }
        if (call.method == "storageAccess") {
            result.success(mapOf("supported" to (Build.VERSION.SDK_INT >= 30), "granted" to allFiles()))
            return
        }
        if (call.method == "storageSettings") { result.success(openStorageSettings()); return }
        val path = call.argument<String>("path")
        val shareId = call.argument<String>("share")
        // One file's pieces in the order they came, and one share's opening and closing in the
        // order they came (after a stream resume a share is closed and opened again under the
        // same id at once); everything else side by side.
        val key = when (call.method) {
            "open", "close" -> shareId?.let { "share:$it" }
            "read", "write", "setSize", "flush" -> if (shareId != null && path != null) shareId + "\n" + path.lowercase(Locale.ROOT) else null
            else -> null
        }
        ordered.execute(key) {
            val answer: Any? = try { run(call) } catch (failure: DriveFailure) { failure } catch (_: Throwable) { DriveFailure("drive_io_failed") }
            main.post {
                if (answer is DriveFailure) result.error(answer.code, null, null) else result.success(answer)
            }
        }
    }

    private fun entry(value: DriveEntry): Map<String, Any?> = mapOf(
        "name" to value.name, "dir" to value.dir, "size" to value.size, "modified" to value.modified,
        // Documents carry no creation time: the modified one stands in.
        "created" to value.modified, "readOnly" to value.readOnly, "free" to value.free, "total" to value.total,
    )

    private fun number(call: MethodCall, name: String): Long = when (val value = call.argument<Any>(name)) {
        is Int -> value.toLong()
        is Long -> value
        else -> throw DriveFailure("drive_invalid_path")
    }

    private fun run(call: MethodCall): Any? {
        val path = call.argument<String>("path")
        when (call.method) {
            "storageFolders" -> return storageFolders(path)
            "storagePick" -> return storagePick(path)
            "open" -> {
                val id = call.argument<String>("share") ?: throw DriveFailure("drive_invalid_path")
                if (!Regex("^[A-Za-z0-9_-]{1,32}$").matches(id)) throw DriveFailure("drive_invalid_path")
                if (path != null && path.startsWith("/")) {
                    // A folder of all-files access: only one chosen in this app's list in this run.
                    if (!synchronized(pickedFolders) { pickedFolders.contains(path) }) throw DriveFailure("drive_denied")
                    val share = DriveShare(FileDriveStore(File(path), ::allFiles), call.argument<Boolean>("readOnly") == true)
                    val name = share.open()
                    val before = synchronized(shares) { shares.put(id, Shared(null, path, share)) }
                    before?.share?.close()
                    return mapOf("name" to name)
                }
                val tree = Uri.parse(path ?: throw DriveFailure("drive_invalid_path"))
                // Only a folder the person picked in this run; never a URI from anywhere else.
                var kept = synchronized(picked) { picked[tree] } ?: throw DriveFailure("drive_denied")
                if (!kept) {
                    // Opened again after its share ended (다시 시도, a resumed stream): kept by
                    // Android again where the picker's grant still allows it, so its loss shows.
                    kept = try {
                        resolver.takePersistableUriPermission(tree,
                            Intent.FLAG_GRANT_READ_URI_PERMISSION or Intent.FLAG_GRANT_WRITE_URI_PERMISSION)
                        true
                    } catch (_: Exception) { false }
                    synchronized(picked) { picked[tree] = kept }
                }
                val share = DriveShare(SafDriveStore(activity, tree, kept, timer), call.argument<Boolean>("readOnly") == true)
                val name = share.open()
                val before = synchronized(shares) { shares.put(id, Shared(tree, tree.toString(), share)) }
                before?.share?.close()
                return mapOf("name" to name)
            }
            "close" -> {
                val gone = synchronized(shares) { shares.remove(call.argument<String>("share")) } ?: return null
                gone.share.close()
                val tree = gone.tree ?: return null
                val used = synchronized(shares) { shares.values.any { it.tree == tree } }
                if (!used) try {
                    resolver.releasePersistableUriPermission(tree,
                        Intent.FLAG_GRANT_READ_URI_PERMISSION or Intent.FLAG_GRANT_WRITE_URI_PERMISSION)
                    // Still usable for 다시 시도 while this activity lives; no longer kept by Android.
                    synchronized(picked) { if (picked.containsKey(tree)) picked[tree] = false }
                } catch (_: Exception) {}
                return null
            }
            "status" -> {
                val id = call.argument<String>("share")
                val found = synchronized(shares) { shares[id] } ?: throw DriveFailure("drive_share_unknown")
                val state = found.share.status()
                return mapOf("deleted" to state.deleted, "offline" to state.offline, "revoked" to state.revoked,
                    "path" to found.path, "name" to state.name)
            }
        }
        val share = share(call)
        val relative = path ?: throw DriveFailure("drive_invalid_path")
        return when (call.method) {
            "stat" -> entry(share.stat(relative))
            "list" -> mapOf("entries" to share.list(relative).map(::entry))
            "read" -> share.read(relative, number(call, "offset"), number(call, "length").toInt())
            "write" -> { share.write(relative, number(call, "offset"), call.argument<ByteArray>("data") ?: throw DriveFailure("drive_invalid_path"), call.argument<Boolean>("truncate") == true); null }
            "setSize" -> { share.setSize(relative, number(call, "size")); null }
            "flush" -> { share.flush(relative); null }
            "create" -> { share.create(relative, call.argument<Boolean>("dir") == true, call.argument<Boolean>("truncate") == true); null }
            "rename" -> { share.rename(relative, call.argument<String>("to") ?: throw DriveFailure("drive_invalid_path"), call.argument<Boolean>("replace") == true); null }
            "delete" -> { share.delete(relative); null }
            "touch" -> { share.touch(relative); null }
            else -> throw DriveFailure("drive_unavailable")
        }
    }
}

/** The documents of one picked tree through the Storage Access Framework. */
internal class SafDriveStore(
    private val context: Context,
    private val tree: Uri,
    /** Android keeps this grant (takePersistableUriPermission worked): its loss can be seen. */
    private val kept: Boolean,
    private val timer: ScheduledExecutorService,
) : DriveStore {
    private val resolver = context.contentResolver
    private val columns = arrayOf(Document.COLUMN_DOCUMENT_ID, Document.COLUMN_DISPLAY_NAME, Document.COLUMN_MIME_TYPE,
        Document.COLUMN_SIZE, Document.COLUMN_LAST_MODIFIED, Document.COLUMN_FLAGS)

    private fun uri(id: String): Uri = DocumentsContract.buildDocumentUriUsingTree(tree, id)

    /** [action] with a signal that cancels it after the provider timeout; its exceptions as drive codes. */
    private fun <T> guarded(action: (CancellationSignal) -> T): T {
        val signal = CancellationSignal()
        val watchdog = try {
            timer.schedule({ signal.cancel() }, AndroidDrive.providerTimeoutMs, TimeUnit.MILLISECONDS)
        } catch (_: Exception) { null }
        try { return action(signal) } catch (failure: DriveFailure) { throw failure } catch (error: Exception) { throw DriveFailure(driveCode(error)) } finally {
            watchdog?.cancel(false)
        }
    }

    private fun doc(cursor: Cursor): DriveDoc? {
        val id = cursor.getString(0) ?: return null
        val dir = cursor.getString(2) == Document.MIME_TYPE_DIR
        val flags = if (cursor.isNull(5)) 0 else cursor.getInt(5)
        return DriveDoc(
            id = id,
            name = cursor.getString(1).orEmpty(),
            dir = dir,
            // "null if unknown" for both (DocumentsContract.Document).
            size = if (dir || cursor.isNull(3)) 0 else cursor.getLong(3).coerceAtLeast(0),
            modified = if (cursor.isNull(4)) 0 else cursor.getLong(4).coerceAtLeast(0),
            writable = flags and Document.FLAG_SUPPORTS_WRITE != 0,
            deletable = flags and Document.FLAG_SUPPORTS_DELETE != 0,
            renamable = flags and Document.FLAG_SUPPORTS_RENAME != 0,
            movable = flags and Document.FLAG_SUPPORTS_MOVE != 0,
            creatable = dir && flags and Document.FLAG_DIR_SUPPORTS_CREATE != 0,
            virtual = flags and Document.FLAG_VIRTUAL_DOCUMENT != 0 ||
                (Build.VERSION.SDK_INT >= 29 && flags and Document.FLAG_PARTIAL != 0),
        )
    }

    override fun root(): DriveDoc {
        if (kept) {
            val granted = try {
                resolver.persistedUriPermissions.any { it.uri == tree && it.isReadPermission }
            } catch (_: Exception) { true }
            if (!granted) throw DriveFailure("drive_denied")
        }
        return stat(DocumentsContract.getTreeDocumentId(tree))
    }

    override fun stat(id: String): DriveDoc = guarded { signal ->
        resolver.query(uri(id), columns, null, null, null, signal)?.use { cursor ->
            if (cursor.moveToFirst()) doc(cursor) else null
        } ?: throw DriveFailure("drive_not_found")
    }

    override fun children(parent: String, limit: Int): List<DriveDoc> = guarded { signal ->
        val out = ArrayList<DriveDoc>()
        resolver.query(DocumentsContract.buildChildDocumentsUriUsingTree(tree, parent), columns, null, null, null, signal)?.use { cursor ->
            while (out.size < limit && cursor.moveToNext()) doc(cursor)?.let(out::add)
        } ?: throw DriveFailure("drive_not_found")
        out
    }

    override fun create(parent: String, name: String, dir: Boolean): DriveDoc {
        val made = guarded {
            // The type its extension names, so a provider has no reason to add another extension.
            val type = if (dir) Document.MIME_TYPE_DIR else MimeTypeMap.getSingleton()
                .getMimeTypeFromExtension(name.substringAfterLast('.', "").lowercase(Locale.ROOT)) ?: "application/octet-stream"
            DocumentsContract.createDocument(resolver, uri(parent), type, name)
        } ?: throw DriveFailure("drive_io_failed")
        return stat(DocumentsContract.getDocumentId(made))
    }

    override fun rename(id: String, name: String): DriveDoc {
        val out = guarded { DocumentsContract.renameDocument(resolver, uri(id), name) } ?: throw DriveFailure("drive_io_failed")
        return stat(DocumentsContract.getDocumentId(out))
    }

    override fun move(id: String, from: String, to: String): DriveDoc {
        val out = guarded { DocumentsContract.moveDocument(resolver, uri(id), uri(from), uri(to)) } ?: throw DriveFailure("drive_io_failed")
        return stat(DocumentsContract.getDocumentId(out))
    }

    override fun delete(id: String) {
        if (!guarded { DocumentsContract.deleteDocument(resolver, uri(id)) }) throw DriveFailure("drive_io_failed")
    }

    private fun descriptor(id: String, mode: String): ParcelFileDescriptor? {
        val signal = CancellationSignal()
        val watchdog = try {
            timer.schedule({ signal.cancel() }, AndroidDrive.providerTimeoutMs, TimeUnit.MILLISECONDS)
        } catch (_: Exception) { null }
        try { return resolver.openFileDescriptor(uri(id), mode, signal) } finally { watchdog?.cancel(false) }
    }

    /**
     * ContentResolver.openFileDescriptor: "r" and "w" may be a pipe; "rw" "implies a file on disk
     * that supports seeking"; "w" "may or may not truncate", so emptying asks for "wt"/"rwt".
     */
    override fun open(id: String, mode: DriveOpenMode): DriveChannel {
        val attempts = when (mode) {
            DriveOpenMode.read -> listOf("r")
            DriveOpenMode.readWrite -> listOf("rw")
            DriveOpenMode.truncate -> listOf("rwt", "wt")
            DriveOpenMode.append -> listOf("wa")
        }
        var last: Exception? = null
        for (attempt in attempts) {
            try {
                return SafChannel(descriptor(id, attempt) ?: throw FileNotFoundException())
            } catch (error: SecurityException) {
                throw DriveFailure("drive_denied")
            } catch (error: OperationCanceledException) {
                throw DriveFailure("drive_io_failed")
            } catch (error: Exception) {
                last = error
            }
        }
        if (mode == DriveOpenMode.read) throw DriveFailure(driveCode(last!!))
        // A provider refuses a way of opening it does not have in its own words (often "not
        // found"): the document still being there is what tells the two apart.
        stat(id)
        throw DriveFailure(driveUnsupported)
    }

    /** Only where it can be told without guessing: the device's own shared storage. */
    override fun space(): Pair<Long, Long>? {
        if (tree.authority != "com.android.externalstorage.documents" ||
            !DocumentsContract.getTreeDocumentId(tree).startsWith("primary:")) return null
        val path = context.getExternalFilesDir(null)?.path ?: return null
        val stat = StatFs(path)
        return stat.availableBytes to stat.totalBytes
    }
}

/**
 * The folders and files under one folder of all-files access, by path: a document's id is its
 * absolute path. The same contract as [SafDriveStore]. Nothing here is recursive and nothing
 * replaces a file that is already there (rename(2) would): [DriveShare] decides those.
 */
internal class FileDriveStore(private val root: File, private val allowed: () -> Boolean) : DriveStore {
    private inline fun <T> guarded(action: () -> T): T = try { action() } catch (failure: DriveFailure) { throw failure } catch (error: ErrnoException) {
        throw DriveFailure(errnoCode(error.errno))
    } catch (error: Exception) { throw DriveFailure(driveCode(error)) }

    /** Null for what is not there or is neither a folder nor an ordinary file. */
    private fun doc(file: File, parentWritable: Boolean? = null): DriveDoc? {
        val stat = try { Os.stat(file.path) } catch (error: ErrnoException) {
            if (error.errno == OsConstants.ENOENT || error.errno == OsConstants.ENOTDIR) return null
            throw error
        }
        val dir = OsConstants.S_ISDIR(stat.st_mode)
        if (!dir && !OsConstants.S_ISREG(stat.st_mode)) return null
        val changeable = parentWritable ?: (file.parentFile?.canWrite() == true)
        val writable = file.canWrite()
        return DriveDoc(id = file.path, name = file.name, dir = dir, size = if (dir) 0 else stat.st_size.coerceAtLeast(0),
            modified = (stat.st_mtime * 1000).coerceAtLeast(0), writable = writable, deletable = changeable,
            renamable = changeable, movable = changeable, creatable = dir && writable)
    }

    override fun root(): DriveDoc {
        if (!allowed()) throw DriveFailure("drive_denied")
        return stat(root.path)
    }

    override fun stat(id: String): DriveDoc = guarded { doc(File(id)) ?: throw DriveFailure("drive_not_found") }

    override fun children(parent: String, limit: Int): List<DriveDoc> = guarded {
        val folder = File(parent)
        val listed = folder.listFiles() ?: throw DriveFailure(
            if (!folder.exists()) "drive_not_found" else if (!folder.isDirectory) "drive_not_directory" else "drive_denied")
        val changeable = folder.canWrite()
        val out = ArrayList<DriveDoc>(minOf(listed.size, limit))
        for (file in listed) {
            if (out.size >= limit) break
            // One entry this app may not look at does not hide the rest of its folder.
            try { doc(file, changeable)?.let(out::add) } catch (_: ErrnoException) {}
        }
        out
    }

    override fun create(parent: String, name: String, dir: Boolean): DriveDoc = guarded {
        val target = File(parent, name)
        if (dir) Os.mkdir(target.path, "770".toInt(8))
        else Os.close(Os.open(target.path, OsConstants.O_CREAT or OsConstants.O_EXCL or OsConstants.O_WRONLY, "660".toInt(8)))
        doc(target) ?: throw DriveFailure("drive_io_failed")
    }

    private fun same(a: File, b: File): Boolean = try {
        val x = Os.stat(a.path)
        val y = Os.stat(b.path)
        x.st_dev == y.st_dev && x.st_ino == y.st_ino
    } catch (_: ErrnoException) { false }

    /** Only onto a free name, or onto itself (a change of capitals where names ignore them). */
    private fun relocate(from: File, to: File): DriveDoc {
        if (to.exists() && !same(from, to)) throw DriveFailure("drive_exists")
        Os.rename(from.path, to.path)
        return doc(to) ?: throw DriveFailure("drive_io_failed")
    }

    override fun rename(id: String, name: String): DriveDoc = guarded {
        val from = File(id)
        relocate(from, File(from.parentFile, name))
    }

    override fun move(id: String, from: String, to: String): DriveDoc = guarded {
        val file = File(id)
        relocate(file, File(to, file.name))
    }

    /** A file, or a folder with nothing in it (ENOTEMPTY otherwise). */
    override fun delete(id: String) = guarded { Os.remove(id) }

    override fun open(id: String, mode: DriveOpenMode): DriveChannel = guarded {
        SafChannel(ParcelFileDescriptor.open(File(id), when (mode) {
            DriveOpenMode.read -> ParcelFileDescriptor.MODE_READ_ONLY
            DriveOpenMode.readWrite -> ParcelFileDescriptor.MODE_READ_WRITE
            DriveOpenMode.truncate -> ParcelFileDescriptor.MODE_READ_WRITE or ParcelFileDescriptor.MODE_TRUNCATE
            DriveOpenMode.append -> ParcelFileDescriptor.MODE_WRITE_ONLY or ParcelFileDescriptor.MODE_APPEND
        }))
    }

    override fun space(): Pair<Long, Long>? = try {
        val stat = StatFs(root.path)
        stat.availableBytes to stat.totalBytes
    } catch (_: Exception) { null }
}

private fun errnoOf(error: Throwable): Int? {
    var cause: Throwable? = error
    repeat(6) {
        (cause as? ErrnoException)?.let { return it.errno }
        cause = cause?.cause ?: return null
    }
    return null
}

private fun errnoCode(errno: Int): String = when (errno) {
    OsConstants.ENOSPC, OsConstants.EDQUOT -> "drive_no_space"
    OsConstants.EACCES, OsConstants.EPERM, OsConstants.EROFS -> "drive_denied"
    OsConstants.ENOENT, OsConstants.ENODEV, OsConstants.ENXIO -> "drive_not_found"
    OsConstants.ENOTEMPTY -> "drive_not_empty"
    OsConstants.EEXIST -> "drive_exists"
    OsConstants.EISDIR -> "drive_is_directory"
    OsConstants.ENOTDIR -> "drive_not_directory"
    OsConstants.ESPIPE -> driveUnsupported
    else -> "drive_io_failed"
}

/** The fixed code of what a provider (or the system on its behalf) threw. */
internal fun driveCode(error: Throwable): String {
    errnoOf(error)?.let { return errnoCode(it) }
    return when (error) {
        is FileNotFoundException -> "drive_not_found"
        is SecurityException -> "drive_denied"
        is UnsupportedOperationException -> "drive_denied"
        else -> "drive_io_failed"
    }
}

private class SafChannel(private val descriptor: ParcelFileDescriptor) : DriveChannel {
    private val fd = descriptor.fileDescriptor
    override val seekable: Boolean = try { Os.lseek(fd, 0, OsConstants.SEEK_CUR); true } catch (_: ErrnoException) { false }

    private inline fun <T> io(action: () -> T): T = try { action() } catch (error: ErrnoException) {
        throw DriveFailure(errnoCode(error.errno))
    } catch (error: IOException) {
        throw DriveFailure(driveCode(error))
    }

    override fun readAt(position: Long, target: ByteArray, offset: Int, length: Int): Int = io { Os.pread(fd, target, offset, length, position) }

    override fun writeAt(position: Long, data: ByteArray) = io {
        var done = 0
        while (done < data.size) {
            val count = Os.pwrite(fd, data, done, data.size - done, position + done)
            if (count <= 0) throw DriveFailure("drive_io_failed")
            done += count
        }
    }

    override fun readNext(target: ByteArray, offset: Int, length: Int): Int = io { Os.read(fd, target, offset, length) }

    override fun writeNext(data: ByteArray) = io {
        var done = 0
        while (done < data.size) {
            val count = Os.write(fd, data, done, data.size - done)
            if (count <= 0) throw DriveFailure("drive_io_failed")
            done += count
        }
    }

    override fun truncate(size: Long) {
        try { Os.ftruncate(fd, size) } catch (error: ErrnoException) {
            // A provider's own file descriptor (not a file on disk) has no way to be cut.
            throw DriveFailure(if (error.errno == OsConstants.ENOSPC || error.errno == OsConstants.EDQUOT) "drive_no_space" else driveUnsupported)
        }
    }

    override fun size(): Long? = try { Os.fstat(fd).st_size } catch (_: ErrnoException) { null }

    override fun sync() {
        try { Os.fsync(fd) } catch (error: ErrnoException) {
            // A pipe or a provider's descriptor has nothing to sync; closing it is what keeps the data.
            if (error.errno != OsConstants.EINVAL && error.errno != OsConstants.ENOSYS && error.errno != OsConstants.EROFS) {
                throw DriveFailure(errnoCode(error.errno))
            }
        }
    }

    override fun close() {
        try { descriptor.close() } catch (_: IOException) { throw DriveFailure("drive_io_failed") }
    }
}
