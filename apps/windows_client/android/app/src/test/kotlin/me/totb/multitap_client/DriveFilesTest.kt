package me.totb.multitap_client

import org.junit.Assert.assertArrayEquals
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNotNull
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Assert.fail
import org.junit.Test
import java.util.concurrent.CountDownLatch
import java.util.concurrent.Executors
import java.util.concurrent.TimeUnit

// DriveShare over a fake documents provider: what the desktop asks by path, done on documents
// with opaque ids, and every gap of such a provider turned into one fixed error. The fake models
// what Android's providers really do: ids that are paths and change on a rename (shared
// storage), a taken name silently becoming "name (1)", a case-blind volume, folders deleted with
// their content, streams that cannot seek, and data kept only when the document closes.
class DriveFilesTest {
    private class Node(var name: String, val dir: Boolean) {
        var parent: Node? = null
        val children = ArrayList<Node>()
        var data = ByteArray(0)
        var modified = 1000L
        var writable = true
        var deletable = true
        var renamable = true
        var virtual = false
        var stable = ""
    }

    private class FakeTree(
        val caseBlind: Boolean = true,
        val uniquify: Boolean = true,
        val pathIds: Boolean = true,
    ) : DriveStore {
        val root = Node("Shared", true)
        var seekable = true
        var truncatable = true
        var appendable = true
        var commitOnClose = false
        var movable = true
        var revoked = false
        var gone = false
        var broken = false
        var full = false
        var failClose = false
        var free: Pair<Long, Long>? = 5000L to 9000L
        /** The n-th rename from now fails (1 = the next one). */
        var failRename = 0
        var listings = 0
        var stats = 0
        var opens = 0
        var live = 0
        var syncs = 0
        var clock = 5000L
        private val stableIds = HashMap<String, Node>()
        private var serial = 0

        init { root.stable = "s0"; stableIds["s0"] = root }

        fun same(a: String, b: String) = if (caseBlind) a.equals(b, ignoreCase = true) else a == b

        private fun path(node: Node): String = generateSequence(node) { it.parent }.toList().asReversed().drop(1).joinToString("/") { it.name }

        fun id(node: Node) = if (pathIds) "p:" + path(node) else node.stable

        private fun attached(node: Node): Boolean = generateSequence(node) { it.parent }.last() === root

        fun node(id: String): Node {
            val found = if (pathIds) {
                var at: Node? = root
                for (part in id.removePrefix("p:").split('/').filter { it.isNotEmpty() }) at = at?.children?.firstOrNull { it.name == part }
                at
            } else stableIds[id]?.takeIf(::attached)
            return found ?: throw DriveFailure("drive_not_found")
        }

        fun doc(node: Node) = DriveDoc(id(node), node.name, node.dir, if (node.dir) 0 else node.data.size.toLong(), node.modified,
            writable = node.writable, deletable = node.deletable, renamable = node.renamable, movable = movable,
            creatable = node.dir, virtual = node.virtual)

        /** Something done on the device outside the share. */
        fun put(path: String, bytes: ByteArray? = null): Node {
            var at = root
            val parts = path.split('/')
            for ((index, part) in parts.withIndex()) {
                val last = index == parts.size - 1
                at = at.children.firstOrNull { it.name == part } ?: attach(at, Node(part, dir = !last || bytes == null))
            }
            if (bytes != null) at.data = bytes
            return at
        }

        fun find(path: String): Node? {
            var at: Node? = root
            for (part in path.split('/')) at = at?.children?.firstOrNull { it.name == part }
            return at
        }

        fun names(path: String = ""): List<String> = (if (path.isEmpty()) root else find(path)!!).children.map { it.name }

        private fun attach(parent: Node, node: Node): Node {
            node.parent = parent
            parent.children += node
            node.stable = "s" + ++serial
            stableIds[node.stable] = node
            return node
        }

        private fun free(parent: Node, name: String): String {
            // android.os.FileUtils.buildUniqueFile: "name (1).ext" while a file of that name exists
            // (on a case-blind volume the renamed file itself is one).
            fun taken(candidate: String) = parent.children.any { same(it.name, candidate) }
            if (!taken(name)) return name
            if (!uniquify) throw DriveFailure("drive_exists")
            val dot = name.lastIndexOf('.')
            val stem = if (dot > 0) name.substring(0, dot) else name
            val ext = if (dot > 0) name.substring(dot) else ""
            var n = 1
            while (taken("$stem ($n)$ext")) n++
            return "$stem ($n)$ext"
        }

        @Synchronized override fun root(): DriveDoc {
            if (revoked) throw DriveFailure("drive_denied")
            if (gone) throw DriveFailure("drive_not_found")
            if (broken) throw IllegalStateException("provider died")
            return doc(root)
        }

        @Synchronized override fun stat(id: String): DriveDoc { stats++; return doc(node(id)) }

        @Synchronized override fun children(parent: String, limit: Int): List<DriveDoc> {
            listings++
            val node = node(parent)
            if (!node.dir) throw DriveFailure("drive_not_directory")
            return node.children.take(limit).map(::doc)
        }

        @Synchronized override fun create(parent: String, name: String, dir: Boolean): DriveDoc {
            val folder = node(parent)
            return doc(attach(folder, Node(free(folder, name), dir).also { it.modified = clock }))
        }

        @Synchronized override fun rename(id: String, name: String): DriveDoc {
            val node = node(id)
            if (failRename > 0 && --failRename == 0) throw DriveFailure("drive_io_failed")
            node.name = free(node.parent!!, name)
            return doc(node)
        }

        @Synchronized override fun move(id: String, from: String, to: String): DriveDoc {
            val node = node(id)
            val target = node(to)
            if (node.parent !== node(from)) throw DriveFailure("drive_not_found")
            // FileSystemProvider.moveDocument: "Already exists".
            if (target.children.any { same(it.name, node.name) }) throw DriveFailure("drive_io_failed")
            node.parent!!.children.remove(node)
            node.parent = target
            target.children += node
            return doc(node)
        }

        @Synchronized override fun delete(id: String) {
            val node = node(id)
            // Recursive, as a real provider.
            node.parent!!.children.remove(node)
            node.parent = null
        }

        @Synchronized override fun open(id: String, mode: DriveOpenMode): DriveChannel {
            val node = node(id)
            if (node.dir) throw DriveFailure("drive_is_directory")
            if (mode == DriveOpenMode.readWrite && !seekable) throw DriveFailure(driveUnsupported)
            if (mode == DriveOpenMode.append && !appendable) throw DriveFailure(driveUnsupported)
            opens++
            live++
            return Channel(node, mode)
        }

        override fun space(): Pair<Long, Long>? = free

        inner class Channel(private val node: Node, private val mode: DriveOpenMode) : DriveChannel {
            override val seekable = this@FakeTree.seekable
            private val writing = mode != DriveOpenMode.read
            private var held: ByteArray? = if (commitOnClose && writing) (if (mode == DriveOpenMode.truncate) ByteArray(0) else node.data.copyOf()) else null
            private var cursor: Int
            private var closed = false

            init {
                if (mode == DriveOpenMode.truncate && held == null) node.data = ByteArray(0)
                cursor = if (mode == DriveOpenMode.append) bytes.size else 0
            }

            private var bytes: ByteArray
                get() = held ?: node.data
                set(value) { if (held != null) held = value else { node.data = value; node.modified = clock } }

            private fun check() { check(!closed) { "closed channel used" } }

            override fun readAt(position: Long, target: ByteArray, offset: Int, length: Int): Int = synchronized(this@FakeTree) {
                check(); check(seekable)
                val count = minOf(length.toLong(), bytes.size - position).toInt()
                if (count > 0) System.arraycopy(bytes, position.toInt(), target, offset, count)
                maxOf(count, 0)
            }

            override fun writeAt(position: Long, data: ByteArray): Unit = synchronized(this@FakeTree) {
                check(); check(seekable && writing)
                if (full) throw DriveFailure("drive_no_space")
                val out = bytes.copyOf(maxOf(bytes.size, position.toInt() + data.size))
                System.arraycopy(data, 0, out, position.toInt(), data.size)
                bytes = out
            }

            override fun readNext(target: ByteArray, offset: Int, length: Int): Int = synchronized(this@FakeTree) {
                check()
                val count = maxOf(minOf(length, bytes.size - cursor), 0)
                if (count > 0) System.arraycopy(bytes, cursor, target, offset, count)
                cursor += count
                count
            }

            override fun writeNext(data: ByteArray): Unit = synchronized(this@FakeTree) {
                check(); check(writing)
                if (full) throw DriveFailure("drive_no_space")
                bytes = bytes.copyOf(cursor) + data
                cursor += data.size
            }

            override fun truncate(size: Long): Unit = synchronized(this@FakeTree) {
                check()
                if (!truncatable || !seekable) throw DriveFailure(driveUnsupported)
                bytes = bytes.copyOf(size.toInt())
            }

            override fun size(): Long? = synchronized(this@FakeTree) { bytes.size.toLong() }

            override fun sync(): Unit = synchronized(this@FakeTree) { check(); syncs++ }

            override fun close(): Unit = synchronized(this@FakeTree) {
                if (!closed) {
                    closed = true
                    live--
                    if (failClose) throw DriveFailure("drive_io_failed")
                    held?.let { node.data = it; node.modified = clock }
                }
            }
        }
    }

    private var time = 0L
    private var tags = 0

    private fun share(tree: FakeTree, readOnly: Boolean = false) =
        DriveShare(tree, readOnly, now = { time }, temporaryTag = { "t" + ++tags }).also { it.open() }

    private fun code(action: () -> Unit): String {
        try { action() } catch (failure: DriveFailure) { return failure.code }
        fail("expected a drive failure")
        return ""
    }

    private fun bytes(text: String) = text.toByteArray()

    // ---- names and paths ----------------------------------------------------------------------

    @Test fun safeNamesFollowTheSharedRule() {
        for (name in listOf("a", "보고서.txt", "a b.c", "COM10", "con1", ".git", "a.b.c", "😀.png")) assertTrue(name, driveSafeName(name))
        for (name in listOf("", ".", "..", "a.", "a ", "a/b", "a\\b", "a:b", "a*b", "a?b", "a<b", "a|b", "a\"b", "a\u0001b",
            "CON", "con.txt", "NUL", "COM1", "lpt9.log", "COM¹", "CONIN$", "\ud83d", "a\udc00")) assertFalse(name, driveSafeName(name))
        assertFalse(driveSafeName("a".repeat(256)))
        assertTrue(driveSafeName("a".repeat(255)))
        assertEquals(emptyList<String>(), driveSegments(""))
        assertEquals(listOf("a", "b c"), driveSegments("a/b c"))
        assertEquals("drive_invalid_path", code { driveSegments("a//b") })
        assertEquals("drive_invalid_path", code { driveSegments("../a") })
        assertEquals("drive_invalid_path", code { driveSegments(List(65) { "a" }.joinToString("/")) })
    }

    @Test fun aPathThatIsNotPlainIsRefusedBeforeTheProviderIsAsked() {
        val tree = FakeTree()
        val files = share(tree)
        val before = tree.listings
        assertEquals("drive_invalid_path", code { files.stat("a/../b") })
        assertEquals("drive_invalid_path", code { files.read("a\\b", 0, 1) })
        assertEquals("drive_invalid_path", code { files.rename("a", "b/", false) })
        assertEquals(before, tree.listings)
    }

    // ---- stat and list ------------------------------------------------------------------------

    @Test fun theRootIsAFolderWithTheVolumeSpaceWhereItIsKnown() {
        val tree = FakeTree()
        val root = share(tree).stat("")
        assertEquals("", root.name)
        assertTrue(root.dir)
        assertEquals(5000L, root.free)
        assertEquals(9000L, root.total)
        tree.free = null
        assertNull(share(tree).stat("").free)
        // More free than there is: not believed.
        tree.free = 10L to 5L
        assertNull(share(tree).stat("").total)
    }

    @Test fun statGivesTheDocumentsOwnSizeAndTimeEvenWhenTheListingIsOld() {
        val tree = FakeTree()
        val node = tree.put("docs/a.txt", bytes("hello"))
        node.modified = 4242
        val files = share(tree)
        assertEquals(5L, files.stat("docs/a.txt").size)
        // Changed on the device after the folder was listed.
        node.data = bytes("hello world")
        node.modified = 9999
        val entry = files.stat("docs/a.txt")
        assertEquals(11L, entry.size)
        assertEquals(9999L, entry.modified)
        assertFalse(entry.dir)
        assertFalse(entry.readOnly)
        node.writable = false
        assertTrue(files.stat("docs/a.txt").readOnly)
        assertTrue(files.stat("docs").dir)
        assertEquals(0L, files.stat("docs").size)
    }

    @Test fun aMissingNameIsNotFoundWithoutListingTheFolderAgainForAWhile() {
        val tree = FakeTree()
        tree.put("docs/a.txt", bytes("x"))
        val files = share(tree)
        assertEquals("drive_not_found", code { files.stat("docs/desktop.ini") })
        val listed = tree.listings
        // What Explorer keeps asking for costs nothing more.
        repeat(20) { assertEquals("drive_not_found", code { files.stat("docs/Thumbs.db") }) }
        assertEquals(listed, tree.listings)
        assertEquals("drive_not_found", code { files.stat("nowhere/a.txt") })
        assertEquals("drive_not_found", code { files.stat("docs/a.txt/b") })
    }

    @Test fun aFileAddedOnTheDeviceIsFoundOnceTheListingIsNotFresh() {
        val tree = FakeTree()
        tree.put("docs/a.txt", bytes("x"))
        val files = share(tree)
        assertEquals("drive_not_found", code { files.stat("docs/new.txt") })
        tree.put("docs/new.txt", bytes("fresh"))
        time += DriveShare.negativeTrustMs + 1
        assertEquals(5L, files.stat("docs/new.txt").size)
    }

    @Test fun aFileDeletedOrReplacedOnTheDeviceIsNotServedFromMemory() {
        val tree = FakeTree(pathIds = false)
        val node = tree.put("a.txt", bytes("old"))
        val files = share(tree)
        assertEquals(3L, files.stat("a.txt").size)
        // Deleted outside and made anew: another document under the same name.
        tree.delete(tree.id(node))
        tree.put("a.txt", bytes("newer"))
        assertEquals(5L, files.stat("a.txt").size)
        assertArrayEquals(bytes("newer"), files.read("a.txt", 0, 100))
        tree.delete(tree.id(tree.find("a.txt")!!))
        assertEquals("drive_not_found", code { files.stat("a.txt") })
        assertEquals("drive_not_found", code { files.read("a.txt", 0, 1) })
    }

    @Test fun aFolderRenamedOnTheDeviceIsLookedUpAgain() {
        val tree = FakeTree()
        tree.put("old/a.txt", bytes("x"))
        val files = share(tree)
        assertEquals(1L, files.stat("old/a.txt").size)
        tree.find("old")!!.name = "new"
        assertEquals("drive_not_found", code { files.stat("old/a.txt") })
        assertEquals(1L, files.stat("new/a.txt").size)
    }

    @Test fun listingLeavesOutWhatTheDesktopCannotOpenAndShowsANameOnce() {
        val tree = FakeTree(caseBlind = false)
        tree.put("a.txt", bytes("1"))
        tree.put("sub/x", bytes("1"))
        tree.put("con.txt", bytes("1"))
        tree.put("trailing.", bytes("1"))
        tree.put("doc", bytes("1")).virtual = true
        // Two documents with one name (a cloud folder): the first is the one shown.
        tree.root.children += tree.put("twin.txt", bytes("first")).let { first ->
            Node("twin.txt", false).also { it.parent = tree.root; it.data = bytes("second!"); it.stable = "dup" }
        }
        val entries = share(tree).list("")
        assertEquals(listOf("a.txt", "sub", "twin.txt"), entries.map { it.name })
        assertEquals(5L, entries.single { it.name == "twin.txt" }.size)
        assertTrue(entries.single { it.name == "sub" }.dir)
        assertEquals("drive_not_directory", code { share(tree).list("a.txt") })
        assertEquals("drive_not_found", code { share(tree).list("missing") })
    }

    @Test fun aListingIsAlwaysAskedAnewAndCutAtTheLimit() {
        val tree = FakeTree()
        repeat(30) { tree.put("f$it", bytes("x")) }
        val files = share(tree)
        assertEquals(30, files.list("").size)
        tree.put("later", bytes("x"))
        assertEquals(31, files.list("").size)
        assertEquals(31, tree.children(tree.id(tree.root), driveMaxListEntries).size)
        assertEquals(5, tree.children(tree.id(tree.root), 5).size)
    }

    @Test fun anExactNameWinsAndOnlyAUniqueOtherCapitalisationOpens() {
        val tree = FakeTree(caseBlind = false)
        tree.put("Report.txt", bytes("upper"))
        tree.put("notes.txt", bytes("n"))
        val files = share(tree)
        // Windows asks in whatever case the person typed.
        assertArrayEquals(bytes("upper"), files.read("report.TXT", 0, 10))
        assertEquals("Report.txt", files.stat("REPORT.txt").name)
        // Now two names differ only in capitalisation: each opens by its exact name only.
        tree.put("report.txt", bytes("lower"))
        time += DriveShare.negativeTrustMs + 1
        assertEquals(setOf("Report.txt", "report.txt", "notes.txt"), files.list("").map { it.name }.toSet())
        assertArrayEquals(bytes("upper"), files.read("Report.txt", 0, 10))
        assertArrayEquals(bytes("lower"), files.read("report.txt", 0, 10))
        assertEquals("drive_not_found", code { files.read("REPORT.TXT", 0, 10) })
    }

    // ---- read ---------------------------------------------------------------------------------

    @Test fun rangedReadsOfALargeFileGiveExactlyTheAskedBytes() {
        val tree = FakeTree()
        val data = ByteArray(3 * driveChunkBytes + 1000) { (it * 31 + it / 251).toByte() }
        tree.put("big.bin", data)
        val files = share(tree)
        // Out of order, as the desktop's read-ahead asks.
        for (block in listOf(2, 0, 3, 1)) {
            val offset = block.toLong() * driveChunkBytes
            val expected = data.copyOfRange(offset.toInt(), minOf(data.size, offset.toInt() + driveChunkBytes))
            assertArrayEquals(expected, files.read("big.bin", offset, driveChunkBytes))
        }
        assertArrayEquals(data.copyOfRange(12345, 12345 + 777), files.read("big.bin", 12345, 777))
        // Fewer bytes only at the end; nothing beyond it.
        assertEquals(1000, files.read("big.bin", 3L * driveChunkBytes, driveChunkBytes).size)
        assertEquals(0, files.read("big.bin", data.size + 5L, 10).size)
        // One open document served them all.
        assertEquals(1, tree.opens)
    }

    @Test fun readRefusesFoldersAndBadRanges() {
        val tree = FakeTree()
        tree.put("sub/a", bytes("x"))
        val files = share(tree)
        assertEquals("drive_is_directory", code { files.read("", 0, 1) })
        assertEquals("drive_is_directory", code { files.read("sub", 0, 1) })
        assertEquals("drive_not_found", code { files.read("sub/b", 0, 1) })
        assertEquals("drive_invalid_path", code { files.read("sub/a", 0, 0) })
        assertEquals("drive_invalid_path", code { files.read("sub/a", -1, 1) })
        assertEquals("drive_invalid_path", code { files.read("sub/a", 0, driveChunkBytes + 1) })
    }

    @Test fun aStreamThatCannotSeekIsReadForwardAndOpenedAgainToGoBack() {
        val tree = FakeTree().apply { seekable = false }
        val data = ByteArray(5000) { it.toByte() }
        tree.put("s.bin", data)
        val files = share(tree)
        assertArrayEquals(data.copyOfRange(0, 1000), files.read("s.bin", 0, 1000))
        assertArrayEquals(data.copyOfRange(1000, 2000), files.read("s.bin", 1000, 1000))
        assertEquals(1, tree.opens)
        // Forward past a gap: read and dropped.
        assertArrayEquals(data.copyOfRange(3000, 3500), files.read("s.bin", 3000, 500))
        assertEquals(1, tree.opens)
        // Back: the stream is opened again.
        assertArrayEquals(data.copyOfRange(100, 200), files.read("s.bin", 100, 100))
        assertEquals(2, tree.opens)
        assertEquals(1, tree.live)
        assertEquals(0, files.read("s.bin", 6000, 10).size)
    }

    // ---- write --------------------------------------------------------------------------------

    @Test fun writesAtAnyOffsetLandWhereTheyWereAskedAndGapsReadAsZeros() {
        val tree = FakeTree()
        val files = share(tree)
        files.write("out.bin", 0, bytes("AAAA"), truncate = true)
        // The desktop sends pieces of one write side by side: any order.
        files.write("out.bin", 12, bytes("DDDD"), truncate = false)
        files.write("out.bin", 4, bytes("BBBB"), truncate = false)
        assertArrayEquals(bytes("AAAABBBB") + ByteArray(4) + bytes("DDDD"), tree.find("out.bin")!!.data)
        files.write("out.bin", 2, bytes("xx"), truncate = false)
        assertArrayEquals(bytes("AAxxBBBB") + ByteArray(4) + bytes("DDDD"), tree.find("out.bin")!!.data)
        // truncate: the file starts over.
        files.write("out.bin", 0, bytes("Z"), truncate = true)
        assertArrayEquals(bytes("Z"), tree.find("out.bin")!!.data)
        assertEquals(1L, files.stat("out.bin").size)
    }

    @Test fun writeFollowsTheWindowsRulesForWhatMustExist() {
        val tree = FakeTree()
        tree.put("sub/keep.txt", bytes("k"))
        tree.put("locked.txt", bytes("l")).writable = false
        val files = share(tree)
        assertEquals("drive_not_found", code { files.write("new.txt", 0, bytes("x"), truncate = false) })
        assertEquals("drive_not_found", code { files.write("missing/new.txt", 0, bytes("x"), truncate = true) })
        assertEquals("drive_is_directory", code { files.write("sub", 0, bytes("x"), truncate = true) })
        assertEquals("drive_is_directory", code { files.write("", 0, bytes("x"), truncate = true) })
        assertEquals("drive_denied", code { files.write("locked.txt", 0, bytes("x"), truncate = false) })
        assertEquals("drive_invalid_path", code { files.write("sub/keep.txt", 5, bytes("x"), truncate = true) })
        assertEquals("drive_invalid_path", code { files.write("sub/keep.txt", 0, ByteArray(0), truncate = false) })
        assertEquals("drive_invalid_path", code { files.write("sub/keep.txt", 0, ByteArray(driveChunkBytes + 1), truncate = false) })
        assertArrayEquals(bytes("k"), tree.find("sub/keep.txt")!!.data)
        assertArrayEquals(bytes("l"), tree.find("locked.txt")!!.data)
        assertEquals(listOf("sub", "locked.txt"), tree.names())
    }

    @Test fun aReadOnlyShareChangesNothing() {
        val tree = FakeTree()
        tree.put("a.txt", bytes("a"))
        tree.put("d/x", bytes("x"))
        val files = share(tree, readOnly = true)
        assertEquals("drive_read_only", code { files.write("a.txt", 0, bytes("b"), truncate = true) })
        assertEquals("drive_read_only", code { files.create("b.txt", dir = false, truncate = false) })
        assertEquals("drive_read_only", code { files.rename("a.txt", "b.txt", replace = true) })
        assertEquals("drive_read_only", code { files.delete("a.txt") })
        assertEquals("drive_read_only", code { files.setSize("a.txt", 0) })
        assertEquals("drive_read_only", code { files.touch("a.txt") })
        files.flush("a.txt")
        assertArrayEquals(bytes("a"), files.read("a.txt", 0, 5))
        assertEquals(listOf("a.txt", "d"), tree.names())
        // The switch applies to a share that is open.
        files.readOnly = false
        files.write("a.txt", 0, bytes("b"), truncate = true)
        assertArrayEquals(bytes("b"), tree.find("a.txt")!!.data)
    }

    @Test fun aFullDeviceIsSaidAsNoSpace() {
        val tree = FakeTree()
        val files = share(tree)
        files.write("a.bin", 0, bytes("1234"), truncate = true)
        tree.full = true
        assertEquals("drive_no_space", code { files.write("a.bin", 4, bytes("5678"), truncate = false) })
        assertArrayEquals(bytes("1234"), tree.find("a.bin")!!.data)
    }

    @Test fun aProviderWithoutRandomAccessTakesAFileInOrderOnly() {
        val tree = FakeTree().apply { seekable = false }
        val files = share(tree)
        files.write("s.txt", 0, bytes("one "), truncate = true)
        files.write("s.txt", 4, bytes("two "), truncate = false)
        files.write("s.txt", 8, bytes("three"), truncate = false)
        assertArrayEquals(bytes("one two three"), tree.find("s.txt")!!.data)
        assertEquals(13L, files.stat("s.txt").size)
        // A gap or a step back would write another file than the one asked for.
        assertEquals("drive_denied", code { files.write("s.txt", 20, bytes("x"), truncate = false) })
        assertEquals("drive_denied", code { files.write("s.txt", 2, bytes("x"), truncate = false) })
        assertArrayEquals(bytes("one two three"), tree.find("s.txt")!!.data)
        // After the document was closed (idle), a write that continues it appends.
        time += DriveShare.writeIdleMs
        files.sweep()
        assertEquals(0, tree.live)
        files.write("s.txt", 13, bytes("!"), truncate = false)
        assertArrayEquals(bytes("one two three!"), tree.find("s.txt")!!.data)
        time += DriveShare.writeIdleMs
        files.sweep()
        assertEquals("drive_denied", code { files.write("s.txt", 3, bytes("x"), truncate = false) })
        tree.appendable = false
        assertEquals("drive_denied", code { files.write("s.txt", 14, bytes("x"), truncate = false) })
        assertArrayEquals(bytes("one two three!"), tree.find("s.txt")!!.data)
    }

    @Test fun whatIsBeingWrittenReadsBackAndHasItsNewLength() {
        val tree = FakeTree().apply { commitOnClose = true }
        tree.put("a.txt", bytes("old"))
        val files = share(tree)
        files.write("a.txt", 0, bytes("new content"), truncate = true)
        // The provider keeps it only when the document closes; this side already knows better.
        assertArrayEquals(bytes("old"), tree.find("a.txt")!!.data)
        assertEquals(11L, files.stat("a.txt").size)
        assertArrayEquals(bytes("content"), files.read("a.txt", 4, 20))
        files.flush("a.txt")
        assertArrayEquals(bytes("new content"), tree.find("a.txt")!!.data)
        assertEquals(1, tree.syncs)
        assertEquals(0, tree.live)
    }

    @Test fun flushSaysSoWhenTheProviderDidNotKeepTheData() {
        val tree = FakeTree().apply { commitOnClose = true }
        val files = share(tree)
        files.write("a.txt", 0, bytes("data"), truncate = true)
        tree.failClose = true
        assertEquals("drive_io_failed", code { files.flush("a.txt") })
        tree.failClose = false
        // Nothing to keep: a folder, the root, a file nobody wrote.
        files.flush("")
        files.flush("a.txt")
        tree.put("d/x", bytes("x"))
        time += DriveShare.negativeTrustMs + 1
        files.flush("d")
        assertEquals("drive_not_found", code { files.flush("missing.txt") })
    }

    // ---- setsize ------------------------------------------------------------------------------

    @Test fun setSizeCutsAndGrowsWithZeros() {
        val tree = FakeTree()
        tree.put("a.bin", bytes("0123456789"))
        val files = share(tree)
        files.setSize("a.bin", 4)
        assertArrayEquals(bytes("0123"), tree.find("a.bin")!!.data)
        files.setSize("a.bin", 8)
        assertArrayEquals(bytes("0123") + ByteArray(4), tree.find("a.bin")!!.data)
        files.setSize("a.bin", 0)
        assertEquals(0, tree.find("a.bin")!!.data.size)
        assertEquals("drive_is_directory", code { files.setSize("", 1) })
        assertEquals("drive_not_found", code { files.setSize("nope", 1) })
        assertEquals("drive_invalid_path", code { files.setSize("a.bin", -1) })
    }

    @Test fun setSizeIsExactOrRefusedWhereADocumentCannotBeCut() {
        val tree = FakeTree().apply { truncatable = false }
        tree.put("a.bin", bytes("0123456789"))
        val files = share(tree)
        // Growing: the last byte is written, the gap reads as zeros.
        files.setSize("a.bin", 14)
        assertArrayEquals(bytes("0123456789") + ByteArray(4), tree.find("a.bin")!!.data)
        files.setSize("a.bin", 14)
        // Shrinking to a length in between cannot be done: refused, the file as it was.
        assertEquals("drive_denied", code { files.setSize("a.bin", 5) })
        assertEquals(14, tree.find("a.bin")!!.data.size)
        // Emptying can: the document is opened anew, empty.
        files.setSize("a.bin", 0)
        assertEquals(0, tree.find("a.bin")!!.data.size)
    }

    @Test fun setSizeWithoutRandomAccessOnlyEmptiesOrConfirms() {
        val tree = FakeTree().apply { seekable = false }
        tree.put("a.bin", bytes("0123456789"))
        val files = share(tree)
        files.setSize("a.bin", 10)
        assertEquals("drive_denied", code { files.setSize("a.bin", 20) })
        assertEquals("drive_denied", code { files.setSize("a.bin", 3) })
        assertArrayEquals(bytes("0123456789"), tree.find("a.bin")!!.data)
        files.setSize("a.bin", 0)
        assertEquals(0, tree.find("a.bin")!!.data.size)
        // While it is written in order, its length so far is confirmed and nothing else.
        files.write("a.bin", 0, bytes("abc"), truncate = true)
        files.setSize("a.bin", 3)
        assertEquals("drive_denied", code { files.setSize("a.bin", 9) })
        assertArrayEquals(bytes("abc"), tree.find("a.bin")!!.data)
    }

    // ---- create and delete --------------------------------------------------------------------

    @Test fun createFollowsTheWindowsRules() {
        val tree = FakeTree()
        tree.put("sub/a.txt", bytes("content"))
        val files = share(tree)
        files.create("new.txt", dir = false, truncate = false)
        files.create("folder", dir = true, truncate = false)
        files.create("folder/inner.txt", dir = false, truncate = false)
        assertEquals(listOf("sub", "new.txt", "folder"), tree.names())
        assertTrue(tree.find("folder")!!.dir)
        assertEquals(listOf("inner.txt"), tree.names("folder"))
        assertEquals("drive_exists", code { files.create("new.txt", dir = false, truncate = false) })
        assertEquals("drive_exists", code { files.create("NEW.TXT", dir = false, truncate = false) })
        assertEquals("drive_exists", code { files.create("folder", dir = true, truncate = false) })
        assertEquals("drive_exists", code { files.create("folder", dir = false, truncate = false) })
        assertEquals("drive_is_directory", code { files.create("folder", dir = false, truncate = true) })
        assertEquals("drive_exists", code { files.create("new.txt", dir = true, truncate = false) })
        assertEquals("drive_exists", code { files.create("", dir = true, truncate = false) })
        assertEquals("drive_not_found", code { files.create("missing/x", dir = false, truncate = false) })
        assertEquals("drive_invalid_path", code { files.create("x", dir = true, truncate = true) })
        // truncate: an existing file is emptied, a missing one made.
        files.create("sub/a.txt", dir = false, truncate = true)
        assertEquals(0, tree.find("sub/a.txt")!!.data.size)
        files.create("sub/b.txt", dir = false, truncate = true)
        assertEquals(listOf("a.txt", "b.txt"), tree.names("sub"))
        // What was just made is known without listing the folder again.
        val listed = tree.listings
        assertEquals(0L, files.stat("sub/b.txt").size)
        assertEquals(listed, tree.listings)
    }

    @Test fun aNameTheProviderChangesIsUndoneAndRefused() {
        val tree = FakeTree()
        tree.put("a.txt", bytes("x"))
        val files = share(tree)
        assertEquals("drive_not_found", code { files.stat("report.txt") })
        // Made on the device a moment ago: the listing still says the name is free, and the
        // provider would make "report (1).txt" instead.
        tree.put("report.txt", bytes("theirs"))
        assertEquals("drive_exists", code { files.create("report.txt", dir = false, truncate = false) })
        assertEquals(listOf("a.txt", "report.txt"), tree.names())
        assertArrayEquals(bytes("theirs"), tree.find("report.txt")!!.data)
    }

    @Test fun deleteTakesAFileOrAnEmptyFolderOnly() {
        val tree = FakeTree()
        tree.put("full/a.txt", bytes("a"))
        tree.put("full/sub/b.txt", bytes("b"))
        tree.put("empty")
        tree.put("gone.txt", bytes("g"))
        tree.put("kept.txt", bytes("k")).deletable = false
        val files = share(tree)
        // A provider deletes a folder with everything in it; the desktop only ever means an empty one.
        assertEquals("drive_not_empty", code { files.delete("full") })
        assertEquals(listOf("a.txt", "sub"), tree.names("full"))
        files.delete("empty")
        files.delete("gone.txt")
        assertEquals(listOf("full", "kept.txt"), tree.names())
        assertEquals("drive_not_found", code { files.stat("gone.txt") })
        assertEquals("drive_not_found", code { files.delete("gone.txt") })
        assertEquals("drive_denied", code { files.delete("kept.txt") })
        assertEquals("drive_denied", code { files.delete("") })
        files.delete("full/sub/b.txt")
        files.delete("full/sub")
        files.delete("full/a.txt")
        files.delete("full")
        assertEquals(listOf("kept.txt"), tree.names())
    }

    @Test fun touchIsAcceptedAndChangesNothing() {
        val tree = FakeTree()
        tree.put("a.txt", bytes("a")).modified = 777
        tree.put("d/x", bytes("x"))
        val files = share(tree)
        files.touch("a.txt")
        assertEquals(777L, files.stat("a.txt").modified)
        assertEquals("drive_is_directory", code { files.touch("d") })
        assertEquals("drive_is_directory", code { files.touch("") })
        assertEquals("drive_not_found", code { files.touch("nope") })
    }

    // ---- rename and move ----------------------------------------------------------------------

    @Test fun renameInAFolderKeepsTheContentAndTheOldNameIsGone() {
        val tree = FakeTree()
        tree.put("docs/a.txt", bytes("content"))
        val files = share(tree)
        assertEquals(7L, files.stat("docs/a.txt").size)
        files.rename("docs/a.txt", "docs/b.txt", replace = false)
        assertEquals(listOf("b.txt"), tree.names("docs"))
        assertArrayEquals(bytes("content"), files.read("docs/b.txt", 0, 100))
        assertEquals("drive_not_found", code { files.stat("docs/a.txt") })
        assertEquals("drive_not_found", code { files.rename("docs/a.txt", "docs/c.txt", replace = false) })
        assertEquals("drive_not_found", code { files.rename("docs/b.txt", "missing/c.txt", replace = false) })
        assertEquals("drive_invalid_path", code { files.rename("", "x", replace = false) })
        assertEquals("drive_invalid_path", code { files.rename("docs", "", replace = false) })
    }

    @Test fun renameOntoAnExistingNameNeedsReplaceAndNeverInventsAnotherName() {
        val tree = FakeTree()
        tree.put("new.tmp", bytes("new content"))
        tree.put("doc.txt", bytes("old"))
        val files = share(tree)
        assertEquals("drive_exists", code { files.rename("new.tmp", "doc.txt", replace = false) })
        assertEquals(listOf("new.tmp", "doc.txt"), tree.names())
        // An editor's safe save: write aside, then replace.
        files.rename("new.tmp", "doc.txt", replace = true)
        assertEquals(listOf("doc.txt"), tree.names())
        assertArrayEquals(bytes("new content"), tree.find("doc.txt")!!.data)
        assertArrayEquals(bytes("new content"), files.read("doc.txt", 0, 100))
    }

    @Test fun aReplaceThatFailsHalfwayPutsTheOldFileBack() {
        val tree = FakeTree()
        tree.put("new.tmp", bytes("new content"))
        tree.put("doc.txt", bytes("old"))
        val files = share(tree)
        // The old file goes aside first (rename 1); the new one then fails to take its name (rename 2).
        tree.failRename = 2
        assertEquals("drive_io_failed", code { files.rename("new.tmp", "doc.txt", replace = true) })
        assertEquals(setOf("new.tmp", "doc.txt"), tree.names().toSet())
        assertArrayEquals(bytes("old"), tree.find("doc.txt")!!.data)
        assertArrayEquals(bytes("new content"), tree.find("new.tmp")!!.data)
        assertArrayEquals(bytes("old"), files.read("doc.txt", 0, 100))
    }

    @Test fun replacingRefusesFoldersAndWhatCannotBeRemoved() {
        val tree = FakeTree()
        tree.put("a.txt", bytes("a"))
        tree.put("dir/x", bytes("x"))
        tree.put("fixed.txt", bytes("f")).deletable = false
        val files = share(tree)
        assertEquals("drive_denied", code { files.rename("a.txt", "dir", replace = true) })
        assertEquals("drive_denied", code { files.rename("dir", "a.txt", replace = true) })
        assertEquals("drive_denied", code { files.rename("a.txt", "fixed.txt", replace = true) })
        tree.find("a.txt")!!.renamable = false
        assertEquals("drive_denied", code { files.rename("a.txt", "b.txt", replace = false) })
        assertEquals(setOf("a.txt", "dir", "fixed.txt"), tree.names().toSet())
        assertArrayEquals(bytes("f"), tree.find("fixed.txt")!!.data)
    }

    @Test fun aChangeOfCapitalisationOnlyEndsWithExactlyTheNewName() {
        // Shared storage: case-blind, and its provider would answer "Report (1).txt".
        val tree = FakeTree(caseBlind = true, uniquify = true)
        tree.put("report.txt", bytes("r"))
        val files = share(tree)
        files.rename("report.txt", "Report.txt", replace = false)
        assertEquals(listOf("Report.txt"), tree.names())
        assertArrayEquals(bytes("r"), files.read("Report.txt", 0, 10))
        assertEquals("Report.txt", files.stat("report.txt").name)
        // A provider that just does it is not taken through a temporary name.
        val plain = FakeTree(caseBlind = false)
        plain.put("report.txt", bytes("r"))
        share(plain).rename("report.txt", "Report.txt", replace = false)
        assertEquals(listOf("Report.txt"), plain.names())
    }

    @Test fun moveToAnotherFolderWithOrWithoutANewName() {
        val tree = FakeTree()
        tree.put("a/one.txt", bytes("1"))
        tree.put("a/two.txt", bytes("2"))
        tree.put("a/deep/three.txt", bytes("3"))
        tree.put("b")
        val files = share(tree)
        files.rename("a/one.txt", "b/one.txt", replace = false)
        files.rename("a/two.txt", "b/second.txt", replace = false)
        files.rename("a/deep", "b/deep", replace = false)
        assertEquals(emptyList<String>(), tree.names("a"))
        assertEquals(listOf("one.txt", "second.txt", "deep"), tree.names("b"))
        assertArrayEquals(bytes("3"), files.read("b/deep/three.txt", 0, 5))
        assertEquals("drive_not_found", code { files.stat("a/one.txt") })
        // The moved file's own name is taken there by another: it travels under a temporary one.
        tree.put("a/one.txt", bytes("other one"))
        files.rename("a/one.txt", "b/first.txt", replace = false)
        assertEquals(setOf("one.txt", "second.txt", "deep", "first.txt"), tree.names("b").toSet())
        assertArrayEquals(bytes("other one"), tree.find("b/first.txt")!!.data)
        assertArrayEquals(bytes("1"), tree.find("b/one.txt")!!.data)
        // Onto an existing file there: only with replace.
        tree.put("a/one.txt", bytes("third one"))
        assertEquals("drive_exists", code { files.rename("a/one.txt", "b/one.txt", replace = false) })
        files.rename("a/one.txt", "b/one.txt", replace = true)
        assertArrayEquals(bytes("third one"), tree.find("b/one.txt")!!.data)
        assertEquals(4, tree.names("b").size)
        assertEquals(emptyList<String>(), tree.names("a"))
    }

    @Test fun aProviderThatCannotMoveSaysDeniedAndAFolderNeverGoesIntoItself() {
        val tree = FakeTree().apply { movable = false }
        tree.put("a/one.txt", bytes("1"))
        tree.put("b")
        val files = share(tree)
        assertEquals("drive_denied", code { files.rename("a/one.txt", "b/one.txt", replace = false) })
        assertEquals(listOf("one.txt"), tree.names("a"))
        // In place it still renames.
        files.rename("a/one.txt", "a/uno.txt", replace = false)
        assertEquals(listOf("uno.txt"), tree.names("a"))
        tree.movable = true
        assertEquals("drive_denied", code { files.rename("a", "a/inside", replace = false) })
        assertEquals("drive_denied", code { files.rename("a", "A/inside", replace = false) })
        assertEquals(setOf("a", "b"), tree.names().toSet())
    }

    @Test fun aMoveThatFailsAfterItsFirstStepIsPutBack() {
        val tree = FakeTree()
        tree.put("a/one.txt", bytes("mine"))
        tree.put("b/one.txt", bytes("theirs"))
        val files = share(tree)
        // To b/first.txt: renamed aside (1), moved, then the final rename (2) fails.
        tree.failRename = 2
        assertEquals("drive_io_failed", code { files.rename("a/one.txt", "b/first.txt", replace = false) })
        assertEquals(listOf("one.txt"), tree.names("a"))
        assertEquals(listOf("one.txt"), tree.names("b"))
        assertArrayEquals(bytes("mine"), tree.find("a/one.txt")!!.data)
        assertArrayEquals(bytes("theirs"), tree.find("b/one.txt")!!.data)
    }

    // ---- the picked folder itself -------------------------------------------------------------

    @Test fun accessTakenAwayEndsTheShareAtOnce() {
        val tree = FakeTree()
        tree.put("a.txt", bytes("a"))
        val files = share(tree)
        assertFalse(files.status().deleted)
        tree.revoked = true
        val state = files.status()
        assertTrue(state.deleted)
        assertTrue(state.revoked)
        assertEquals("Shared", state.name)
    }

    @Test fun aFolderThatIsGoneIsOfflineOnceAndEndedAtTheSecondCheck() {
        val tree = FakeTree()
        val files = share(tree)
        tree.gone = true
        val first = files.status()
        assertTrue(first.offline)
        assertFalse(first.deleted)
        // The provider only restarted: nothing ends.
        tree.gone = false
        assertFalse(files.status().offline)
        tree.gone = true
        assertTrue(files.status().offline)
        val second = files.status()
        assertTrue(second.deleted)
        assertFalse(second.revoked)
    }

    @Test fun aProviderThatDoesNotAnswerIsOfflineNeverDeleted() {
        val tree = FakeTree()
        val files = share(tree)
        tree.broken = true
        repeat(5) {
            val state = files.status()
            assertTrue(state.offline)
            assertFalse(state.deleted)
        }
        tree.broken = false
        assertFalse(files.status().offline)
    }

    @Test fun onlyAFolderCanBeSharedAndAnEndedShareAnswersNothing() {
        val tree = FakeTree()
        tree.revoked = true
        assertEquals("drive_denied", code { DriveShare(tree, false).open() })
        tree.revoked = false
        tree.gone = true
        assertEquals("drive_not_found", code { DriveShare(tree, false).open() })
        tree.gone = false
        tree.put("a.txt", bytes("a"))
        val files = share(tree)
        files.write("a.txt", 0, bytes("b"), truncate = true)
        files.read("a.txt", 0, 1)
        files.close()
        assertEquals(0, tree.live)
        assertEquals("drive_share_unknown", code { files.stat("a.txt") })
        assertEquals("drive_share_unknown", code { files.write("a.txt", 0, bytes("c"), truncate = true) })
        assertArrayEquals(bytes("b"), tree.find("a.txt")!!.data)
    }

    // ---- bounds -------------------------------------------------------------------------------

    @Test fun openDocumentsAndRememberedListingsStayBounded() {
        val tree = FakeTree()
        repeat(DriveShare.maxListings + 20) { tree.put("d$it/f", bytes("x")) }
        val files = share(tree)
        repeat(DriveShare.maxListings + 20) { assertEquals(1, files.read("d$it/f", 0, 1).size) }
        assertTrue(tree.live <= DriveShare.maxOpenPerKind)
        assertEquals(tree.live, files.openDocuments)
        repeat(12) { files.write("w$it", 0, bytes("x"), truncate = true) }
        assertTrue(files.openDocuments <= 2 * DriveShare.maxOpenPerKind)
        // Each was synced and closed as it made room; all twelve are on the device.
        repeat(12) { assertArrayEquals(bytes("x"), tree.find("w$it")!!.data) }
        // A folder that fell out of memory is simply listed again.
        val listed = tree.listings
        assertEquals(1L, files.stat("d0/f").size)
        assertTrue(tree.listings > listed)
    }

    @Test fun idleDocumentsAreClosed() {
        val tree = FakeTree()
        tree.put("a", bytes("a"))
        val files = share(tree)
        files.read("a", 0, 1)
        files.write("b", 0, bytes("b"), truncate = true)
        assertEquals(2, tree.live)
        files.sweep()
        assertEquals(2, tree.live)
        time += DriveShare.writeIdleMs
        files.sweep()
        assertEquals(1, tree.live)
        assertEquals(1, tree.syncs)
        time += DriveShare.readIdleMs
        files.sweep()
        assertEquals(0, tree.live)
        // And opened again when needed.
        assertArrayEquals(bytes("a"), files.read("a", 0, 1))
    }

    @Test fun anUnexpectedProviderErrorIsOneFixedCode() {
        val tree = object : DriveStore by FakeTree() {
            override fun children(parent: String, limit: Int): List<DriveDoc> = throw IllegalStateException("boom")
        }
        val files = DriveShare(tree, false).also { it.open() }
        assertEquals("drive_io_failed", code { files.list("") })
        assertEquals("drive_io_failed", code { files.stat("a") })
    }

    // ---- order --------------------------------------------------------------------------------

    @Test fun piecesOfOneFileRunInTheOrderGivenAndOtherFilesSideBySide() {
        val pool = Executors.newFixedThreadPool(4)
        try {
            val serial = KeyedSerial(pool)
            val order = java.util.Collections.synchronizedList(ArrayList<Int>())
            val done = CountDownLatch(200)
            repeat(200) { index ->
                serial.execute("file") {
                    if (index % 7 == 0) Thread.sleep(1)
                    order += index
                    done.countDown()
                }
            }
            assertTrue(done.await(20, TimeUnit.SECONDS))
            assertEquals((0 until 200).toList(), order.toList())
            // Another key is not held up by one that waits.
            val gate = CountDownLatch(1)
            val other = CountDownLatch(1)
            val held = CountDownLatch(1)
            serial.execute("slow") { gate.await(); held.countDown() }
            serial.execute("slow") { held.countDown() }
            serial.execute("fast") { other.countDown() }
            serial.execute(null) { other.countDown() }
            assertTrue(other.await(10, TimeUnit.SECONDS))
            assertEquals(1L, held.count)
            gate.countDown()
            assertTrue(held.await(10, TimeUnit.SECONDS))
            // A task that throws does not stop its key.
            val after = CountDownLatch(1)
            serial.execute("bad") { throw IllegalStateException() }
            serial.execute("bad") { after.countDown() }
            assertTrue(after.await(10, TimeUnit.SECONDS))
            Thread.sleep(50)
            assertEquals(0, serial.pending)
        } finally {
            pool.shutdownNow()
        }
    }

    @Test fun manyRequestsAtOnceOnOneShareLeaveEveryFileWhole() {
        val tree = FakeTree()
        val files = share(tree)
        val pool = Executors.newFixedThreadPool(8)
        try {
            val failures = java.util.Collections.synchronizedList(ArrayList<Throwable>())
            val done = CountDownLatch(8)
            repeat(8) { worker ->
                pool.execute {
                    try {
                        val name = "dir$worker/file.bin"
                        files.create("dir$worker", dir = true, truncate = false)
                        val data = ByteArray(40000) { (it + worker).toByte() }
                        files.write(name, 0, data.copyOfRange(0, 10000), truncate = true)
                        for (piece in listOf(3, 1, 2)) files.write(name, piece * 10000L, data.copyOfRange(piece * 10000, (piece + 1) * 10000), truncate = false)
                        files.flush(name)
                        assertArrayEquals(data, files.read(name, 0, 40000))
                        assertEquals(40000L, files.stat(name).size)
                        files.rename(name, "dir$worker/renamed.bin", replace = false)
                        assertNotNull(files.list("dir$worker").singleOrNull { it.name == "renamed.bin" })
                    } catch (error: Throwable) { failures += error } finally { done.countDown() }
                }
            }
            assertTrue(done.await(30, TimeUnit.SECONDS))
            assertEquals(emptyList<Throwable>(), failures.toList())
            assertEquals(8, tree.names().size)
        } finally {
            pool.shutdownNow()
        }
    }
}
