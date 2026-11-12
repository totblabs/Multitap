package me.totb.multitap_client

import java.io.File
import java.nio.file.Files
import java.util.concurrent.CountDownLatch
import java.util.concurrent.Executors
import java.util.concurrent.TimeUnit
import java.util.concurrent.atomic.AtomicInteger

/** JVM checks of the production admission/cleanup state machine. No Android installer is run. */
object AndroidUpdateOwnershipCheck {
    @JvmStatic fun main(args: Array<String>) {
        var checks = 0
        fun verify(value: Boolean, label: String) { check(value) { label }; checks++ }
        val root = Files.createTempDirectory("multitap-update-owner").toFile()
        try {
            val file = File(root, "pending.apk").apply { writeText("verified-owner-bytes") }
            val gate = AndroidUpdateOwnership()
            val first = gate.acquire()!!
            verify(gate.busy() && gate.owns(first), "admitted before any worker/session")
            verify(gate.acquire() == null, "second queued request refused before first session exists")
            verify(file.readText() == "verified-owner-bytes", "busy rejection leaves original APK intact")
            verify(!gate.accepts(-1) && !gate.accepts(42), "unbound callback refused")
            verify(!gate.bind(first, -1), "invalid session refused")
            verify(gate.bind(first, 42), "worker binds exact session")
            verify(!gate.bind(first, 43), "session cannot be rebound")
            verify(!gate.finish(43), "foreign terminal callback refused")
            verify(gate.runIfOwned(first) {}, "current owner may commit")
            verify(gate.workerFinished(first, failed = false) {}, "worker completion admitted")
            verify(gate.busy() && gate.acquire() == null, "successful worker retains lease while Android confirmation pending")
            verify(gate.finish(42), "terminal callback finishes pending install")
            verify(!gate.finish(42) && !gate.accepts(42), "duplicate terminal callback refused")
            val second = gate.acquire()!!
            verify(!gate.owns(first), "new generation rejects old owner")
            var lateCleanup = 0
            verify(!gate.workerFinished(first, failed = true) { lateCleanup++ } && lateCleanup == 0,
                "old finally cannot clean a newer request's APK")
            verify(!gate.bind(first, 44) && !gate.runIfOwned(first) {}, "old worker cannot bind/commit")
            verify(gate.bind(second, 44), "new generation owns new session")
            verify(!gate.finish(42), "old session status cannot terminate new owner")
            verify(gate.finish(44) && gate.busy(), "early terminal status retains lease until worker cleanup")
            var cleaned = 0
            verify(gate.workerFinished(second, failed = false) { cleaned++ }, "early terminal waits for owner cleanup")
            verify(cleaned == 1 && !gate.busy(), "both terminal and cleanup release")
            verify(!gate.workerFinished(second, failed = true) { cleaned++ } && cleaned == 1, "cleanup exactly once")

            for (failure in listOf("permission", "enqueue", "archive", "copy", "commit")) {
                val owner = gate.acquire()!!
                verify(gate.workerFinished(owner, failed = true) {}, "$failure failure retires exact owner")
                verify(!gate.busy(), "$failure failure permits a later retry")
            }

            val disposedGate = AndroidUpdateOwnership()
            val queued = disposedGate.acquire()!!
            disposedGate.dispose()
            var commits = 0
            verify(!disposedGate.owns(queued) && !disposedGate.runIfOwned(queued) { commits++ }, "dispose prevents queued commit")
            verify(disposedGate.acquire() == null && commits == 0, "disposed instance never admits new work")
            verify(!disposedGate.bind(queued, 55) && !disposedGate.finish(55), "dispose rejects bind/status")
            verify(disposedGate.workerFinished(queued, failed = true) { cleaned++ }, "disposed worker may clean only its retired APK")
            verify(!disposedGate.workerFinished(queued, failed = true) { cleaned++ }, "disposed cleanup once")

            val concurrent = AndroidUpdateOwnership()
            val start = CountDownLatch(1)
            val pool = Executors.newFixedThreadPool(12)
            val winners = AtomicInteger()
            val leases = java.util.Collections.synchronizedList(mutableListOf<AndroidUpdateOwnership.Lease>())
            repeat(48) { pool.execute { start.await(); concurrent.acquire()?.let { winners.incrementAndGet(); leases.add(it) } } }
            start.countDown(); pool.shutdown()
            verify(pool.awaitTermination(5, TimeUnit.SECONDS), "parallel callers complete within deadline")
            verify(winners.get() == 1 && leases.size == 1, "48 concurrent callers yield one owner")
            val owner = leases.single()
            val cleanupEntered = CountDownLatch(1)
            val cleanupRelease = CountDownLatch(1)
            val cleanupThread = Thread {
                concurrent.workerFinished(owner, failed = true) { cleanupEntered.countDown(); check(cleanupRelease.await(5, TimeUnit.SECONDS)) }
            }
            cleanupThread.start()
            verify(cleanupEntered.await(5, TimeUnit.SECONDS), "cleanup reached protected admission boundary")
            val acquired = CountDownLatch(1)
            val nextThread = Thread { check(concurrent.acquire() != null); acquired.countDown() }
            nextThread.start()
            verify(!acquired.await(100, TimeUnit.MILLISECONDS), "new owner waits until old file cleanup completes")
            cleanupRelease.countDown(); cleanupThread.join(5000); nextThread.join(5000)
            verify(!cleanupThread.isAlive && !nextThread.isAlive && acquired.count == 0L, "cleanup releases new admission without deadlock")

            val throwing = AndroidUpdateOwnership()
            val broken = throwing.acquire()!!
            try { throwing.workerFinished(broken, failed = true) { throw IllegalStateException("cleanup") } }
            catch (_: IllegalStateException) {}
            verify(!throwing.busy() && throwing.acquire() != null, "cleanup exception cannot latch failed ownership")

            val oldActivity = AndroidUpdateOwnership.shared()
            val newActivity = AndroidUpdateOwnership.shared()
            val oldDirectory = File(root, "old-generation").apply { mkdirs() }
            val newDirectory = File(root, "new-generation").apply { mkdirs() }
            val name = "Multitap-1.0.4-android-arm64-v8a.apk"
            val oldApk = File(oldDirectory, name).apply { writeText("old-verified") }
            val newApk = File(newDirectory, name).apply { writeText("new-verified") }
            val oldLease = oldActivity.acquire()!!
            verify(oldActivity.bind(oldLease, 70), "old Activity binds session")
            oldActivity.dispose()
            verify(newActivity.busy() && newActivity.acquire() == null,
                "new Activity cannot reuse shared admission before old worker cleanup")
            verify(!newActivity.accepts(70) && !newActivity.finish(70), "new receiver cannot adopt old Activity session")
            verify(!newActivity.workerFinished(oldLease, failed = true) { oldApk.delete() }, "new Activity cannot clean old worker")
            verify(!AndroidUpdatePolicy.inside(newDirectory, oldApk), "new handler rejects late old-view discard path")
            verify(newApk.readText() == "new-verified" && oldApk.readText() == "old-verified", "rejected old path changes neither APK")
            verify(AndroidUpdatePolicy.inside(newDirectory, newApk), "current generation normal install/discard admitted")
            verify(oldActivity.workerFinished(oldLease, failed = true) { oldApk.delete() }, "disposed old owner cleans old APK")
            val newLease = newActivity.acquire()!!
            verify(newApk.readText() == "new-verified", "new file survives old finally")
            verify(!oldActivity.workerFinished(oldLease, failed = true) { newApk.delete() }, "late old finally cannot delete new APK")
            verify(newActivity.bind(newLease, 71), "new Activity owns new session")
            newActivity.workerFinished(newLease, failed = false) {}
            var abandoned = 0
            verify(!newActivity.abort(70) { abandoned++ } && abandoned == 0, "no foreign session abandonment")
            try { newActivity.abort(71) { throw IllegalStateException("OS state unknown") } }
            catch (_: IllegalStateException) {}
            verify(newActivity.busy() && newActivity.accepts(71), "abandon failure retains unverified busy session")
            verify(newActivity.abort(71) { abandoned++ }, "exact session abandonment permits terminal release")
            verify(abandoned == 1 && !newActivity.busy(), "confirmed abandonment releases once")
            newActivity.dispose()

            val queuedActivity = AndroidUpdateOwnership.shared()
            val replacement = AndroidUpdateOwnership.shared()
            val queueOwner = queuedActivity.acquire()!!
            val hold = CountDownLatch(1)
            val executor = Executors.newSingleThreadExecutor()
            val didCommit = AtomicInteger()
            executor.execute { check(hold.await(5, TimeUnit.SECONDS)) }
            executor.execute {
                try { queuedActivity.runIfOwned(queueOwner) { didCommit.incrementAndGet() } }
                finally { queuedActivity.workerFinished(queueOwner, failed = true) {} }
            }
            queuedActivity.dispose()
            executor.shutdown() // Production shutdown keeps queued cleanup runnable.
            verify(replacement.acquire() == null, "disposed queued worker retains shared admission")
            hold.countDown()
            verify(executor.awaitTermination(5, TimeUnit.SECONDS) && didCommit.get() == 0,
                "actual executor runs disposed cleanup without late commit")
            val replacementLease = replacement.acquire()!!
            verify(replacement.owns(replacementLease), "queued disposal cleanup permits replacement")
            replacement.workerFinished(replacementLease, failed = false) {}
            replacement.dispose()
            val last = AndroidUpdateOwnership.shared()
            val finalLease = last.acquire()!!
            verify(last.owns(finalLease), "dispose after completed cleanup releases shared admission")
            last.dispose()
            verify(last.workerFinished(finalLease, failed = true) {}, "enqueue failure after disposal still releases shared admission")

            val pendingActivity = AndroidUpdateOwnership.shared()
            val pendingLease = pendingActivity.acquire()!!
            pendingActivity.bind(pendingLease, 80)
            pendingActivity.workerFinished(pendingLease, failed = false) {}
            pendingActivity.dispose()
            val nextActivity = AndroidUpdateOwnership.shared()
            verify(nextActivity.acquire() == null, "disposed pending OS confirmation cannot silently release admission")
            try { nextActivity.recoverRetired { throw IllegalStateException("abandon failed") } }
            catch (_: IllegalStateException) {}
            verify(nextActivity.busy() && nextActivity.acquire() == null, "failed retired-session abandonment retains busy barrier")
            var exactRetired = -1
            verify(nextActivity.recoverRetired { exactRetired = it } && exactRetired == 80,
                "replacement may abandon only recorded retired session")
            verify(!pendingActivity.finish(80), "old terminal callback cannot act after retirement cleanup")
            val active = nextActivity.acquire()!!
            verify(!nextActivity.recoverRetired { error("must not abandon active owner") }, "active newer owner never swept")
            nextActivity.workerFinished(active, failed = true) {}
            nextActivity.dispose()

            val racingActivity = AndroidUpdateOwnership.shared()
            val creating = racingActivity.acquire()!!
            racingActivity.dispose()
            verify(!racingActivity.bind(creating, 81), "session creation racing disposal cannot continue install")
            verify(racingActivity.session() == 81, "racing created session remains bound for exact abandonment")
            racingActivity.workerFinished(creating, failed = false) {}
            val afterCreation = AndroidUpdateOwnership.shared()
            verify(afterCreation.busy(), "unverified racing session holds admission after file cleanup")
            verify(afterCreation.recoverRetired { verify(it == 81, "recovery uses actual racing session ID") },
                "racing session cleanup clears exact retired proof")
            afterCreation.dispose()

            val refused = AndroidUpdateOwnership()
            val refusedLease = refused.acquire()!!
            val stoppedExecutor = Executors.newSingleThreadExecutor().apply { shutdown() }
            var rejected = false
            try { stoppedExecutor.execute { error("shutdown executor must not run") } }
            catch (_: java.util.concurrent.RejectedExecutionException) {
                rejected = true
                refused.workerFinished(refusedLease, failed = true) {}
            }
            verify(rejected && !refused.busy(), "actual enqueue rejection releases exact reserved owner")
        } finally { root.deleteRecursively() }
        println("{\"checks\":$checks,\"productionUpdateOwnership\":true,\"packageInstallerExecuted\":false}")
    }
}
