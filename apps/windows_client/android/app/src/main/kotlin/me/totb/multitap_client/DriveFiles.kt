package me.totb.multitap_client

import java.util.Locale
import java.util.concurrent.Executor

// drive-v1 on Android (docs/drive-redirection.md "Android"): this device's side of a folder the
// person shared with the desktop. The desktop names files by path; Android's Storage Access
// Framework knows only documents with opaque ids under a tree the person picked. This file maps
// one to the other and is portable (no Android types), so the mapping is tested on the JVM with a
// fake provider (DriveFilesTest). The Android half is AndroidDrive.kt.
//
// What a documents provider cannot do becomes one fixed drive_* error for the desktop, never a
// wait and never another file than the one asked for:
//  * no path lookup: every folder on the way is listed once and remembered (bounded); a name the
//    listing does not have is "not found" for at most [DriveShare.negativeTrustMs] before the
//    provider is asked again; a remembered document the provider no longer has is looked up again.
//  * names: an exact match first, else the only one that differs in capitalisation; two such
//    names are both listed and only an exact name opens either.
//  * a provider that renames what it creates or renames ("name (1)") is undone and refused
//    (drive_exists / drive_invalid_path); replacing a file keeps the old one aside until the new
//    one has its name.
//  * no random access (a pipe): data is taken in order only; anything else is drive_denied.
//  * deleting a folder is recursive in a provider: a folder with anything in it is refused first.
//  * no modified-time setter: `touch` is accepted and changes nothing (the time is the write's).

internal open class DriveFailure(val code: String) : Exception(code)

/** A name the folder's own listing does not have (never a reason to ask the provider again). */
private class DriveMissing : DriveFailure("drive_not_found")

/** Never on the wire: the provider cannot do this at all (no random access, no truncation). */
internal const val driveUnsupported = "drive_unsupported"

internal const val driveChunkBytes = 262144
internal const val driveMaxListEntries = 20000
internal const val driveMaxSize = 9007199254740991L

/** One document as its provider lists it; [id] is the provider's and means nothing here. */
internal data class DriveDoc(
    val id: String,
    val name: String,
    val dir: Boolean,
    val size: Long = 0,
    val modified: Long = 0,
    val writable: Boolean = true,
    val deletable: Boolean = true,
    val renamable: Boolean = true,
    val movable: Boolean = true,
    /** A folder that takes new documents. */
    val creatable: Boolean = true,
    /** No bytes of its own (a virtual or still downloading document): never offered. */
    val virtual: Boolean = false,
)

internal enum class DriveOpenMode { read, readWrite, truncate, append }

/** One open document. Every call may block; failures are [DriveFailure]. */
internal interface DriveChannel {
    /** Positions can be chosen ([readAt], [writeAt], [truncate]); else only the stream order. */
    val seekable: Boolean
    fun readAt(position: Long, target: ByteArray, offset: Int, length: Int): Int
    fun writeAt(position: Long, data: ByteArray)
    fun readNext(target: ByteArray, offset: Int, length: Int): Int
    fun writeNext(data: ByteArray)
    /** [driveUnsupported] where the document cannot be cut or grown in place. */
    fun truncate(size: Long)
    fun size(): Long?
    fun sync()
    fun close()
}

/** The documents under one picked tree. Every call may block; failures are [DriveFailure]. */
internal interface DriveStore {
    /** The picked folder itself: drive_denied once access was taken away, drive_not_found once it is gone. */
    fun root(): DriveDoc
    fun stat(id: String): DriveDoc
    fun children(parent: String, limit: Int): List<DriveDoc>
    fun create(parent: String, name: String, dir: Boolean): DriveDoc
    fun rename(id: String, name: String): DriveDoc
    fun move(id: String, from: String, to: String): DriveDoc
    fun delete(id: String)
    /** [driveUnsupported] when the provider has no such way of opening it. */
    fun open(id: String, mode: DriveOpenMode): DriveChannel
    /** Free and total bytes of the volume, where it can be told. */
    fun space(): Pair<Long, Long>?
}

internal class DriveEntry(
    val name: String,
    val dir: Boolean,
    val size: Long,
    val modified: Long,
    val readOnly: Boolean,
    val free: Long? = null,
    val total: Long? = null,
)

/** Where the picked folder stands (see [DriveShare.status]). */
internal class DriveRootState(val deleted: Boolean, val offline: Boolean, val revoked: Boolean, val name: String)

private val driveForbidden = Regex("[\\x00-\\x1f<>:\"/\\\\|?*]")
private val driveDevice = Regex("^(?:CON|PRN|AUX|NUL|CONIN\\$|CONOUT\\$|(?:COM|LPT)[1-9¹²³])$")

private fun driveWellFormed(text: String): Boolean {
    var i = 0
    while (i < text.length) {
        val unit = text[i]
        if (unit.isHighSurrogate()) {
            if (++i == text.length || !text[i].isLowSurrogate()) return false
        } else if (unit.isLowSurrogate()) return false
        i++
    }
    return true
}

/** One plain Windows file name: the rule of server/src/drive.mjs `safeDriveName` and lib/data/drive_rules.dart. */
internal fun driveSafeName(name: String, max: Int = 255): Boolean {
    if (name.isEmpty() || name.length > max || !driveWellFormed(name) || driveForbidden.containsMatchIn(name) ||
        name.endsWith('.') || name.endsWith(' ') || name == "." || name == "..") return false
    return !driveDevice.matches(name.substringBefore('.').trimEnd(' ', '.').uppercase(Locale.ROOT))
}

/** `''` (the share root) or 1..64 safe segments joined with `/`, at most 1024 units. */
internal fun driveSegments(path: String): List<String> {
    if (path.isEmpty()) return emptyList()
    val segments = path.split('/')
    if (path.length > 1024 || segments.size > 64 || !segments.all { driveSafeName(it) }) throw DriveFailure("drive_invalid_path")
    return segments
}

private fun driveFold(name: String) = name.lowercase(Locale.ROOT)

/**
 * One shared folder. Thread-safe: its bookkeeping is guarded by one lock that is never held
 * across a provider call, so a slow or hung provider stalls only the request that waits for it.
 */
internal class DriveShare(
    private val store: DriveStore,
    @Volatile var readOnly: Boolean,
    private val now: () -> Long = { System.nanoTime() / 1_000_000 },
    private val temporaryTag: () -> String = { java.lang.Long.toHexString(java.util.concurrent.ThreadLocalRandom.current().nextLong() ushr 24) },
) {
    companion object {
        /** A listing that lacks a name is believed this long before the provider is asked again. */
        const val negativeTrustMs = 2000L
        const val readIdleMs = 5000L
        /** Closing a written document is what makes some providers keep it (see [flush]). */
        const val writeIdleMs = 2000L
        const val maxListings = 64
        const val maxListedDocs = 60000
        const val maxOpenPerKind = 4
        /** A stream that cannot seek is read forward at most this far to reach an offset. */
        const val maxSkipBytes = 64L * 1024 * 1024
    }

    private class Listing(val at: Long, docs: List<DriveDoc>) {
        val exact = LinkedHashMap<String, DriveDoc>()
        val folded = HashMap<String, MutableList<DriveDoc>>()
        init { for (doc in docs) add(doc) }
        val size get() = exact.size
        fun add(doc: DriveDoc) {
            // The first of two documents with one name is the one the desktop sees.
            if (exact.containsKey(doc.name)) return
            exact[doc.name] = doc
            folded.getOrPut(driveFold(doc.name)) { ArrayList(1) }.add(doc)
        }
        fun remove(doc: DriveDoc) {
            if (exact[doc.name]?.id != doc.id) return
            exact.remove(doc.name)
            folded[driveFold(doc.name)]?.let { list -> list.removeAll { it.id == doc.id }; if (list.isEmpty()) folded.remove(driveFold(doc.name)) }
        }
        fun find(name: String): DriveDoc? = exact[name] ?: folded[driveFold(name)]?.singleOrNull()
    }

    private class Open(val id: String, var channel: DriveChannel, val write: Boolean) {
        var cursor = 0L
        var used = 0L
        var busy = 0
        var dead = false
        val gate = Any()
    }

    private class Found(val parent: DriveDoc, val doc: DriveDoc?)

    @Suppress("PLATFORM_CLASS_MAPPED_TO_KOTLIN")
    private val lock = Object()
    private val listings = LinkedHashMap<String, Listing>(16, .75f, true)
    private var listedDocs = 0
    /** Counts this share's own changes, so a listing asked before one is not believed over it. */
    private var changes = 0L
    private val reads = LinkedHashMap<String, Open>(8, .75f, true)
    private val writes = LinkedHashMap<String, Open>(8, .75f, true)
    @Volatile private var rootDoc: DriveDoc? = null
    private var misses = 0
    @Volatile private var closed = false

    /** Checks the folder (an existing directory this app may read) and returns its name. */
    fun open(): String {
        val doc = edge { store.root() }
        if (!doc.dir) throw DriveFailure("drive_not_directory")
        rootDoc = doc
        return doc.name
    }

    /** Lets go of every open document; later calls fail drive_share_unknown. */
    fun close() {
        closed = true
        val open = synchronized(lock) {
            listings.clear(); listedDocs = 0
            (reads.values + writes.values).also { reads.clear(); writes.clear() }
        }
        for (item in open) retire(item, wait = false)
    }

    /**
     * Whether the picked folder is still there. Access taken away ends the share at once
     * (`revoked`); a folder the provider does not find is offline for one check and gone at the
     * second in a row (a provider that restarts is not a deleted folder); a provider that does
     * not answer is offline, never deleted.
     */
    fun status(): DriveRootState {
        val name = rootDoc?.name.orEmpty()
        return try {
            val doc = store.root()
            if (!doc.dir) return DriveRootState(deleted = true, offline = false, revoked = false, name = name)
            rootDoc = doc
            synchronized(lock) { misses = 0 }
            DriveRootState(deleted = false, offline = false, revoked = false, name = doc.name)
        } catch (failure: DriveFailure) {
            when (failure.code) {
                "drive_denied" -> DriveRootState(deleted = true, offline = false, revoked = true, name = name)
                "drive_not_found" -> {
                    val gone = synchronized(lock) { ++misses >= 2 }
                    DriveRootState(deleted = gone, offline = !gone, revoked = false, name = name)
                }
                else -> DriveRootState(deleted = false, offline = true, revoked = false, name = name)
            }
        } catch (_: RuntimeException) {
            DriveRootState(deleted = false, offline = true, revoked = false, name = name)
        }
    }

    // ---- reading ------------------------------------------------------------------------------

    fun stat(path: String): DriveEntry = edge {
        val segments = driveSegments(path)
        if (segments.isEmpty()) {
            val doc = store.root().also { rootDoc = it }
            val space = try { store.space() } catch (_: Exception) { null }
            val known = space != null && space.first >= 0 && space.first <= space.second && space.second <= driveMaxSize
            return@edge DriveEntry("", true, 0, doc.modified, false, if (known) space?.first else null, if (known) space?.second else null)
        }
        at(segments) { found ->
            val listed = found.doc ?: throw DriveMissing()
            // The listing may be old: the size and time are the document's own, asked now.
            val doc = store.stat(listed.id)
            if (driveFold(doc.name) != driveFold(listed.name) || doc.dir != listed.dir) throw DriveFailure("drive_not_found")
            relist(found.parent.id) { it.remove(listed); it.add(doc) }
            entry(doc)
        }
    }

    /** The whole folder at once, at most [driveMaxListEntries] entries, always asked anew. */
    fun list(path: String): List<DriveEntry> = edge {
        val segments = driveSegments(path)
        val listed = if (segments.isEmpty()) listing(root(), 0) else at(segments) { found ->
            val doc = found.doc ?: throw DriveMissing()
            if (!doc.dir) throw DriveFailure("drive_not_directory")
            listing(doc, 0)
        }
        synchronized(lock) { listed.exact.values.toList() }.map { entry(it) }
    }

    fun read(path: String, offset: Long, length: Int): ByteArray = edge {
        if (length < 1 || length > driveChunkBytes || offset < 0 || offset > driveMaxSize) throw DriveFailure("drive_invalid_path")
        val segments = driveSegments(path)
        if (segments.isEmpty()) throw DriveFailure("drive_is_directory")
        at(segments) { found ->
            val doc = found.doc ?: throw DriveMissing()
            if (doc.dir) throw DriveFailure("drive_is_directory")
            using(readable(doc)) { open ->
                val buffer = ByteArray(length)
                var total = 0
                if (open.channel.seekable) {
                    while (total < length) {
                        val count = open.channel.readAt(offset + total, buffer, total, length - total)
                        if (count <= 0) break
                        total += count
                    }
                } else synchronized(open.gate) {
                    if (offset < open.cursor) {
                        // A stream cannot go back: it is opened again and read forward.
                        try { open.channel.close() } catch (_: Exception) {}
                        open.channel = store.open(doc.id, DriveOpenMode.read)
                        open.cursor = 0
                    }
                    if (offset - open.cursor > maxSkipBytes) throw DriveFailure("drive_io_failed")
                    val scratch = if (offset > open.cursor) ByteArray(65536) else buffer
                    var ended = false
                    while (open.cursor < offset && !ended) {
                        val count = open.channel.readNext(scratch, 0, minOf(scratch.size.toLong(), offset - open.cursor).toInt())
                        if (count <= 0) ended = true else open.cursor += count
                    }
                    while (!ended && total < length) {
                        val count = open.channel.readNext(buffer, total, length - total)
                        if (count <= 0) break
                        total += count
                        open.cursor += count
                    }
                }
                if (total == length) buffer else buffer.copyOf(total)
            }
        }
    }

    // ---- writing ------------------------------------------------------------------------------

    fun write(path: String, offset: Long, data: ByteArray, truncate: Boolean): Unit = edge {
        if (readOnly) throw DriveFailure("drive_read_only")
        if (data.isEmpty() || data.size > driveChunkBytes || offset < 0 || (truncate && offset != 0L) || offset > driveMaxSize - data.size) {
            throw DriveFailure("drive_invalid_path")
        }
        val segments = driveSegments(path)
        if (segments.isEmpty()) throw DriveFailure("drive_is_directory")
        at(segments) { found ->
            var doc = found.doc
            if (doc != null && doc.dir) throw DriveFailure("drive_is_directory")
            var cut = truncate
            if (doc == null) {
                if (!truncate) throw DriveMissing()
                doc = createIn(found.parent, segments.last(), dir = false)
                cut = false // new, so already empty
            } else if (!doc.writable) throw DriveFailure("drive_denied")
            using(if (cut) reopened(doc, DriveOpenMode.truncate) else writable(doc, offset)) { open ->
                if (open.channel.seekable) open.channel.writeAt(offset, data)
                else synchronized(open.gate) {
                    // In order only; a gap or a step back would silently write another file.
                    if (offset != open.cursor) throw DriveFailure("drive_denied")
                    open.channel.writeNext(data)
                    open.cursor += data.size
                }
            }
        }
    }

    /** Sets a file's length exactly, or refuses: never a file of another length. */
    fun setSize(path: String, size: Long): Unit = edge {
        if (readOnly) throw DriveFailure("drive_read_only")
        if (size < 0 || size > driveMaxSize) throw DriveFailure("drive_invalid_path")
        val segments = driveSegments(path)
        if (segments.isEmpty()) throw DriveFailure("drive_is_directory")
        at(segments) { found ->
            val doc = found.doc ?: throw DriveMissing()
            if (doc.dir) throw DriveFailure("drive_is_directory")
            if (!doc.writable) throw DriveFailure("drive_denied")
            val open = try { writable(doc, null) } catch (failure: DriveFailure) {
                if (failure.code != driveUnsupported) throw failure
                null
            }
            if (open == null) {
                // No random access at all: only "empty it" and "as it is" can be done truthfully.
                if (size == 0L) using(reopened(doc, DriveOpenMode.truncate)) {}
                else if (store.stat(doc.id).size != size) throw DriveFailure("drive_denied")
                return@at
            }
            var emptied = false
            using(open) {
                if (!open.channel.seekable) {
                    val same = synchronized(open.gate) { open.cursor == size }
                    if (same) return@using
                    if (size != 0L) throw DriveFailure("drive_denied")
                    emptied = true
                    return@using
                }
                try { open.channel.truncate(size) } catch (failure: DriveFailure) {
                    if (failure.code != driveUnsupported) throw failure
                    val length = open.channel.size() ?: throw DriveFailure("drive_denied")
                    when {
                        size == length -> Unit
                        // Writing the last byte grows the file; what lies between reads as zeros.
                        size > length -> open.channel.writeAt(size - 1, byteArrayOf(0))
                        size == 0L -> emptied = true
                        else -> throw DriveFailure("drive_denied")
                    }
                }
            }
            if (emptied) using(reopened(doc, DriveOpenMode.truncate)) {}
        }
    }

    /**
     * Makes earlier writes durable: the written document is synced and closed (a provider that
     * stores elsewhere keeps the data when the document closes). The root, a folder and a
     * read-only share have nothing to keep.
     */
    fun flush(path: String): Unit = edge {
        val segments = driveSegments(path)
        if (segments.isEmpty()) return@edge
        at(segments) { found ->
            val doc = found.doc ?: throw DriveMissing()
            if (readOnly || doc.dir) return@at
            val open = synchronized(lock) { writes.remove(doc.id) } ?: return@at
            retire(open, wait = true, sync = true, strict = true)
        }
    }

    fun create(path: String, dir: Boolean, truncate: Boolean): Unit = edge {
        if (readOnly) throw DriveFailure("drive_read_only")
        if (dir && truncate) throw DriveFailure("drive_invalid_path")
        val segments = driveSegments(path)
        if (segments.isEmpty()) throw DriveFailure("drive_exists")
        at(segments) { found ->
            val doc = found.doc
            if (dir) {
                if (doc != null) throw DriveFailure("drive_exists")
                createIn(found.parent, segments.last(), dir = true)
            } else if (doc != null) {
                if (doc.dir) throw DriveFailure(if (truncate) "drive_is_directory" else "drive_exists")
                if (!truncate) throw DriveFailure("drive_exists")
                if (!doc.writable) throw DriveFailure("drive_denied")
                using(reopened(doc, DriveOpenMode.truncate)) {}
            } else createIn(found.parent, segments.last(), dir = false)
        }
    }

    /** A file or an empty folder (a provider deletes a folder with everything in it). */
    fun delete(path: String): Unit = edge {
        if (readOnly) throw DriveFailure("drive_read_only")
        val segments = driveSegments(path)
        if (segments.isEmpty()) throw DriveFailure("drive_denied")
        at(segments) { found ->
            val doc = found.doc ?: throw DriveMissing()
            if (!doc.deletable) throw DriveFailure("drive_denied")
            if (doc.dir && store.children(doc.id, 1).isNotEmpty()) throw DriveFailure("drive_not_empty")
            letGo(doc.id)
            store.delete(doc.id)
            relist(found.parent.id) { it.remove(doc) }
            forget(doc.id)
        }
    }

    /** Documents have no settable modified time: accepted for an existing file, nothing changes. */
    fun touch(path: String): Unit = edge {
        if (readOnly) throw DriveFailure("drive_read_only")
        val segments = driveSegments(path)
        if (segments.isEmpty()) throw DriveFailure("drive_is_directory")
        at(segments) { found ->
            val doc = found.doc ?: throw DriveMissing()
            if (doc.dir) throw DriveFailure("drive_is_directory")
        }
    }

    /**
     * Renames or moves within the share. Each provider step is checked for the name it really
     * gave, and what was done is put back when a later step fails. Replacing keeps the old
     * document under a temporary name until the new one is in place.
     */
    fun rename(path: String, to: String, replace: Boolean): Unit = edge {
        if (readOnly) throw DriveFailure("drive_read_only")
        val from = driveSegments(path)
        val target = driveSegments(to)
        if (from.isEmpty() || target.isEmpty()) throw DriveFailure("drive_invalid_path")
        // Nothing is renamed on the word of a remembered listing.
        val source = walk(from, fresh = true)
        val moving = source.doc ?: throw DriveFailure("drive_not_found")
        val destination = walk(target, fresh = true)
        val existing = destination.doc
        val same = existing != null && existing.id == moving.id
        val leaf = target.last()
        val across = source.parent.id != destination.parent.id
        if (same && !across && moving.name == leaf) return@edge
        if (existing != null && !same) {
            if (!replace) throw DriveFailure("drive_exists")
            if (existing.dir || moving.dir || !existing.renamable || !existing.deletable) throw DriveFailure("drive_denied")
        }
        // A folder cannot go into itself.
        if (moving.dir && target.size > from.size && from.indices.all { driveFold(from[it]) == driveFold(target[it]) }) {
            throw DriveFailure("drive_denied")
        }
        if (across && !moving.movable) throw DriveFailure("drive_denied")
        if (moving.name != leaf && !moving.renamable) throw DriveFailure("drive_denied")
        letGo(moving.id)
        if (existing != null && !same) letGo(existing.id)
        forget(source.parent.id)
        forget(destination.parent.id)
        val undo = ArrayList<() -> Unit>()
        try {
            var kept: DriveDoc? = null
            if (existing != null && !same) {
                kept = renamed(existing, temporary(existing.name))
                val aside = kept
                undo += { store.rename(aside.id, existing.name) }
            }
            var doc = moving
            if (across) {
                // A move keeps the name: one already taken in the target folder is avoided first.
                val taken = child(destination.parent, doc.name, fresh = true)
                if (taken != null && taken.id != doc.id) {
                    val before = doc
                    doc = renamed(doc, temporary(doc.name))
                    val current = doc
                    undo += { store.rename(current.id, before.name) }
                }
                val before = doc
                val moved = store.move(doc.id, source.parent.id, destination.parent.id)
                undo += { store.move(moved.id, destination.parent.id, source.parent.id) }
                if (moved.name != before.name || moved.dir != before.dir) throw DriveFailure("drive_io_failed")
                doc = moved
            }
            if (doc.name != leaf) {
                if (driveFold(doc.name) == driveFold(leaf)) {
                    // Capitalisation only: providers on a case-blind volume see the new name as
                    // taken and invent another, so it goes through a name that is neither.
                    val direct = store.rename(doc.id, leaf)
                    if (direct.name != leaf) {
                        val before = doc
                        val back = if (direct.name == before.name) direct else store.rename(direct.id, before.name)
                        if (back.name != before.name) throw DriveFailure("drive_io_failed")
                        val through = renamed(back, temporary(leaf))
                        undo += { store.rename(through.id, before.name) }
                        renamed(through, leaf)
                    }
                } else renamed(doc, leaf)
            }
            // The replaced document goes only now; one that cannot be deleted stays under its temporary name.
            kept?.let { try { store.delete(it.id) } catch (_: Exception) {} }
        } catch (failure: Exception) {
            for (step in undo.asReversed()) try { step() } catch (_: Exception) {}
            // After the first step a missing document is this rename's own failure, not a stale listing.
            throw if (failure is DriveFailure && failure.code != "drive_not_found") failure else DriveFailure("drive_io_failed")
        } finally {
            forget(source.parent.id)
            forget(destination.parent.id)
        }
    }

    // ---- housekeeping -------------------------------------------------------------------------

    /** Closes documents nobody used for a while (a written one is kept by its provider then). */
    fun sweep() {
        val time = now()
        val idle = synchronized(lock) {
            val out = ArrayList<Open>()
            for ((map, limit) in listOf(reads to readIdleMs, writes to writeIdleMs)) {
                val each = map.values.iterator()
                while (each.hasNext()) {
                    val open = each.next()
                    if (open.busy == 0 && time - open.used >= limit) { each.remove(); out += open }
                }
            }
            out
        }
        for (open in idle) retire(open, wait = false, sync = open.write)
    }

    val openDocuments: Int get() = synchronized(lock) { reads.size + writes.size }

    // ---- internals ----------------------------------------------------------------------------

    /** The one place an internal code or an unexpected exception becomes a wire code. */
    private inline fun <T> edge(action: () -> T): T {
        if (closed) throw DriveFailure("drive_share_unknown")
        try { return action() } catch (failure: DriveFailure) {
            throw if (failure.code == driveUnsupported) DriveFailure("drive_denied") else DriveFailure(failure.code)
        } catch (_: RuntimeException) {
            throw DriveFailure("drive_io_failed")
        }
    }

    private fun root(): DriveDoc = rootDoc ?: store.root().also { rootDoc = it }

    private fun entry(doc: DriveDoc): DriveEntry {
        val open = if (doc.dir) null else synchronized(lock) { writes[doc.id] }
        // While it is being written here its length is this side's, not the provider's last word.
        val size = open?.let { if (it.channel.seekable) try { it.channel.size() } catch (_: Exception) { null } else synchronized(it.gate) { it.cursor } }
        return DriveEntry(doc.name, doc.dir, if (doc.dir) 0 else (size ?: doc.size).coerceIn(0, driveMaxSize), doc.modified.coerceAtLeast(0),
            readOnly = !doc.dir && !doc.writable)
    }

    private fun listing(parent: DriveDoc, maxAgeMs: Long): Listing {
        if (maxAgeMs > 0) synchronized(lock) { listings[parent.id]?.let { if (now() - it.at <= maxAgeMs) return it } }
        val before = synchronized(lock) { changes }
        val docs = store.children(parent.id, driveMaxListEntries).filter { !it.virtual && driveSafeName(it.name) }
        synchronized(lock) {
            // Something changed here while the provider answered: what this listing lacks is
            // asked again rather than believed.
            val listing = Listing(if (changes == before) now() else now() - negativeTrustMs - 1, docs)
            listings.remove(parent.id)?.let { listedDocs -= it.size }
            listings[parent.id] = listing
            listedDocs += listing.size
            val eldest = listings.entries.iterator()
            while ((listings.size > maxListings || listedDocs > maxListedDocs) && eldest.hasNext()) {
                val entry = eldest.next()
                if (entry.value === listing) continue
                listedDocs -= entry.value.size
                eldest.remove()
            }
            return listing
        }
    }

    private fun forget(parent: String) = synchronized(lock) { changes++; listings.remove(parent)?.let { listedDocs -= it.size } }

    /** This share's own change, applied to the remembered listing of [parent] (if any). */
    private inline fun relist(parent: String, change: (Listing) -> Unit) = synchronized(lock) {
        changes++
        listings[parent]?.let { val before = it.size; change(it); listedDocs += it.size - before }
    }

    private fun child(parent: DriveDoc, name: String, fresh: Boolean): DriveDoc? {
        val listing = listing(parent, if (fresh) 0 else Long.MAX_VALUE)
        synchronized(lock) { listing.find(name) }?.let { return it }
        if (fresh || now() - listing.at <= negativeTrustMs) return null
        val again = listing(parent, 0)
        return synchronized(lock) { again.find(name) }
    }

    private fun walk(segments: List<String>, fresh: Boolean): Found {
        var parent = if (fresh) store.root().also { rootDoc = it } else root()
        for (index in 0 until segments.size - 1) {
            val next = child(parent, segments[index], fresh) ?: throw DriveMissing()
            if (!next.dir) throw DriveMissing()
            parent = next
        }
        return Found(parent, child(parent, segments.last(), fresh))
    }

    /**
     * [action] on what [segments] names. A provider that no longer has a remembered document
     * (deleted or replaced outside this share) is asked once more from the root; a name the
     * listings do not have is simply not found.
     */
    private inline fun <T> at(segments: List<String>, action: (Found) -> T): T {
        try { return action(walk(segments, fresh = false)) } catch (failure: DriveFailure) {
            if (failure is DriveMissing || failure.code != "drive_not_found") throw failure
        }
        return action(walk(segments, fresh = true))
    }

    private fun temporary(name: String): String {
        val tag = ".~mt" + temporaryTag()
        return if (name.length + tag.length <= 255) name + tag else name.take(255 - tag.length).trimEnd(' ', '.') + tag
    }

    /** [doc] under exactly [name], or as it was and a failure. */
    private fun renamed(doc: DriveDoc, name: String): DriveDoc {
        val out = store.rename(doc.id, name)
        if (out.name == name) return out
        // The provider chose another name (the wanted one is taken or not allowed there).
        try { if (out.name != doc.name) store.rename(out.id, doc.name) } catch (_: Exception) {}
        throw DriveFailure("drive_exists")
    }

    private fun createIn(parent: DriveDoc, name: String, dir: Boolean): DriveDoc {
        if (!parent.creatable) throw DriveFailure("drive_denied")
        val made = store.create(parent.id, name, dir)
        if (made.name != name || made.dir != dir) {
            // Never another name than the one asked for: what the provider made instead goes again.
            try { store.delete(made.id) } catch (_: Exception) {}
            forget(parent.id)
            throw DriveFailure(if (child(parent, name, fresh = true) != null) "drive_exists" else "drive_invalid_path")
        }
        relist(parent.id) { it.add(made) }
        return made
    }

    private fun admit(map: LinkedHashMap<String, Open>, open: Open): Open {
        val (winner, extra) = synchronized(lock) {
            val present = map[open.id]
            if (present != null) { present.busy++; return@synchronized present to listOf(open) }
            open.busy++
            open.used = now()
            map[open.id] = open
            val out = ArrayList<Open>()
            val eldest = map.values.iterator()
            while (map.size > maxOpenPerKind && eldest.hasNext()) {
                val item = eldest.next()
                if (item.busy == 0) { eldest.remove(); out += item }
            }
            open to out
        }
        for (item in extra) retire(item, wait = false, sync = item.write)
        return winner
    }

    /** A channel to read [doc] from: the one it is written through when that can seek. */
    private fun readable(doc: DriveDoc): Open {
        synchronized(lock) {
            (writes[doc.id]?.takeIf { it.channel.seekable } ?: reads[doc.id])?.let { it.busy++; return it }
        }
        return admit(reads, Open(doc.id, store.open(doc.id, DriveOpenMode.read), write = false))
    }

    /**
     * A channel to write [doc] through at [offset] (null: any position, so it must seek).
     * Without random access the provider's append stream serves a write that continues the file.
     */
    private fun writable(doc: DriveDoc, offset: Long?): Open {
        synchronized(lock) { writes[doc.id]?.let { it.busy++; return it } }
        val channel = try {
            store.open(doc.id, DriveOpenMode.readWrite).also {
                // "Read and write" that cannot seek has no position to trust.
                if (!it.seekable) { try { it.close() } catch (_: Exception) {}; throw DriveFailure(driveUnsupported) }
            }
        } catch (failure: DriveFailure) {
            if (failure.code != driveUnsupported || offset == null) throw failure
            val length = store.stat(doc.id).size
            if (offset != length) throw DriveFailure("drive_denied")
            val stream = store.open(doc.id, DriveOpenMode.append)
            return admit(writes, Open(doc.id, stream, write = true).also { it.cursor = length })
        }
        return admit(writes, Open(doc.id, channel, write = true))
    }

    /** Lets go of [doc]'s channels and opens it anew in [mode] (emptying it). */
    private fun reopened(doc: DriveDoc, mode: DriveOpenMode): Open {
        letGo(doc.id)
        return admit(writes, Open(doc.id, store.open(doc.id, mode), write = true))
    }

    private inline fun <T> using(open: Open, action: (Open) -> T): T {
        try { return action(open) } finally {
            val close = synchronized(lock) {
                open.busy--
                open.used = now()
                lock.notifyAll()
                open.dead && open.busy == 0
            }
            if (close) try { open.channel.close() } catch (_: Exception) {}
        }
    }

    private fun letGo(id: String) {
        val open = synchronized(lock) { listOfNotNull(reads.remove(id), writes.remove(id)) }
        for (item in open) retire(item, wait = true, sync = item.write)
    }

    /**
     * Closes [open], already taken out of its map. [wait]: until no request uses it (bounded);
     * otherwise a request still using it closes it when done. [strict]: a failed sync or close
     * is this call's failure (the data may not have been kept).
     */
    private fun retire(open: Open, wait: Boolean, sync: Boolean = false, strict: Boolean = false) {
        val free = synchronized(lock) {
            val deadline = now() + 30000
            while (wait && open.busy > 0) {
                val left = deadline - now()
                if (left <= 0) break
                lock.wait(left)
            }
            if (open.busy > 0) { open.dead = true; false } else true
        }
        if (!free) {
            if (strict) throw DriveFailure("drive_busy")
            return
        }
        var failure: DriveFailure? = null
        if (sync) try { open.channel.sync() } catch (error: Exception) { failure = error as? DriveFailure ?: DriveFailure("drive_io_failed") }
        try { open.channel.close() } catch (error: Exception) { failure = failure ?: (error as? DriveFailure ?: DriveFailure("drive_io_failed")) }
        if (strict && failure != null) throw failure
    }
}

/**
 * Runs the tasks of one key in the order they were given, one at a time, and different keys side
 * by side on [pool]. The desktop sends several pieces of one file at once; a document that takes
 * data only in order needs them in order.
 */
internal class KeyedSerial(private val pool: Executor) {
    private val queues = HashMap<String, ArrayDeque<Runnable>>()

    fun execute(key: String?, task: Runnable) {
        if (key == null) { pool.execute(task); return }
        synchronized(queues) {
            val waiting = queues[key]
            if (waiting != null) { waiting.addLast(task); return }
            queues[key] = ArrayDeque()
        }
        pool.execute { drain(key, task) }
    }

    private fun drain(key: String, first: Runnable) {
        var task: Runnable? = first
        while (task != null) {
            try { task.run() } catch (_: Throwable) {}
            task = synchronized(queues) {
                val waiting = queues[key]
                val next = waiting?.removeFirstOrNull()
                if (next == null) queues.remove(key)
                next
            }
        }
    }

    val pending: Int get() = synchronized(queues) { queues.size }
}
