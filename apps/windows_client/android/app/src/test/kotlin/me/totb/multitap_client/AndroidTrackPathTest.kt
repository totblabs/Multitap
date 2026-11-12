package me.totb.multitap_client

import java.util.concurrent.CountDownLatch
import java.util.concurrent.TimeUnit
import java.util.concurrent.atomic.AtomicBoolean
import java.util.concurrent.atomic.AtomicInteger
import java.util.concurrent.atomic.AtomicReference
import org.junit.Assert.*
import org.junit.Test

class AndroidTrackPathTest {
    private fun gate(engine: FakeEngine, generation: Long = 1) = UdpPathGate().also {
        it.attach(TEST_LEASE, engine.offer); it.disengage(TEST_LEG); it.stream(1, generation); it.engage(true)
    }
    private fun wait(latch: CountDownLatch) { assertTrue("worker deadline", latch.await(3, TimeUnit.SECONDS)) }
    private fun join(thread: Thread) { thread.join(3000); assertFalse("worker did not finish", thread.isAlive) }

    @Test fun anInFlightReaderOfTheRetiredGateCannotBypassTheFreshPathRule() {
        val old = gate(FakeEngine())
        val path = AndroidTrackPath(old)
        val lookedUp = CountDownLatch(1); val resume = CountDownLatch(1)
        val oldAdmits = AtomicInteger(); val currentAdmits = AtomicInteger()
        val failure = AtomicReference<Throwable?>()
        val reader = Thread {
            try {
                val retainedOldGate = old // the former AndroidMedia WSS lookup
                lookedUp.countDown(); wait(resume)
                val packet = onWire(mediaPacket(1, true, 1, 50))
                // Reproduce why a detached gate is dangerous: legacy WSS passes through.
                retainedOldGate.offerWss(TEST_LEASE, packet, packet.size) { oldAdmits.incrementAndGet(); AndroidMediaStatus.OK }
                assertEquals(AndroidMediaStatus.OK, path.offerWss(TEST_LEASE, packet, { true }) {
                    currentAdmits.incrementAndGet(); AndroidMediaStatus.OK
                })
            } catch (error: Throwable) { failure.set(error) }
        }
        reader.start()
        try {
            wait(lookedUp)
            val fresh = gate(FakeEngine(), 2)
            assertEquals(UdpPathGate.Delivery.ACCEPTED, offerUdp(fresh, mediaPacket(1, true, 1, 100)))
            assertTrue(path.replace(fresh) { true })
            assertFalse(old.attached())
        } finally { resume.countDown(); join(reader) }
        failure.get()?.let { throw it }
        assertEquals("the old detached-gate interleaving was reproduced", 1, oldAdmits.get())
        assertEquals("the real lane must use the fresh gate and drop the older WSS frame", 0, currentAdmits.get())
    }

    @Test fun replacementSerializesWithAdmissionWithoutWaitingForDecoderStop() {
        val old = gate(FakeEngine()); val path = AndroidTrackPath(old)
        val admitted = CountDownLatch(1); val release = CountDownLatch(1)
        val replacing = CountDownLatch(1); val replaced = CountDownLatch(1)
        val failure = AtomicReference<Throwable?>()
        val reader = Thread {
            try {
                val packet = onWire(mediaPacket(1, false, 1, 10))
                path.offerWss(TEST_LEASE, packet, { true }) { admitted.countDown(); wait(release); AndroidMediaStatus.OK }
            } catch (error: Throwable) { failure.set(error) }
        }
        val writer = Thread {
            try { replacing.countDown(); assertTrue(path.replace(gate(FakeEngine(), 2)) { true }); replaced.countDown() }
            catch (error: Throwable) { failure.set(error) }
        }
        reader.start()
        try {
            wait(admitted); writer.start(); wait(replacing)
            assertFalse("swap must not detach a gate during WSS admission", replaced.await(40, TimeUnit.MILLISECONDS))
        } finally { release.countDown(); join(reader); if (writer.state != Thread.State.NEW) join(writer) }
        failure.get()?.let { throw it }
        wait(replaced); assertFalse(old.attached())
    }

    @Test fun aQueuedWssAdmissionRechecksRetiredStateInsideTheLaneLock() {
        val path = AndroidTrackPath(gate(FakeEngine()))
        val entered = CountDownLatch(1); val release = CountDownLatch(1); val queued = CountDownLatch(1)
        val current = AtomicBoolean(true); val admitted = AtomicInteger()
        val answer = AtomicInteger(-1); val failure = AtomicReference<Throwable?>()
        val first = Thread {
            try { val packet = onWire(mediaPacket(1, false, 1, 10)); path.offerWss(TEST_LEASE, packet, { true }) { entered.countDown(); wait(release); AndroidMediaStatus.OK } }
            catch (error: Throwable) { failure.set(error) }
        }
        val late = Thread {
            try {
                val packet = onWire(mediaPacket(1, false, 1, 20)); queued.countDown()
                answer.set(path.offerWss(TEST_LEASE, packet, { current.get() }) { admitted.incrementAndGet(); AndroidMediaStatus.OK }.toInt())
            } catch (error: Throwable) { failure.set(error) }
        }
        first.start()
        try { wait(entered); late.start(); wait(queued); current.set(false) }
        finally { release.countDown(); join(first); if (late.state != Thread.State.NEW) join(late) }
        failure.get()?.let { throw it }
        assertEquals(AndroidMediaStatus.SESSION_REPLACED.toInt(), answer.get())
        assertEquals(0, admitted.get())
    }
}
