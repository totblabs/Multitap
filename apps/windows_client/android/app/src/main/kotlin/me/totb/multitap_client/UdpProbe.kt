package me.totb.multitap_client

/**
 * udp-media-v1 probe schedule (docs/remote-session-protocol.md, "UDP media"),
 * windows/runner/udp_probe_core.h ported as written: BIND through the relays,
 * the PING burst, keepalives and the report. Pure and clock-injected. The
 * socket that sends BIND is the one the relay pins, so this runs on the leg
 * that goes on to carry media. Any failure ends the probe only.
 */
internal class UdpProbe(
    relays: Int,
    private val id: ByteArray,
    private val bind: ByteArray,
    private val ack: ByteArray,
    nowUs: Long,
    private val send: (relay: Int, data: ByteArray, size: Int) -> Boolean,
) {
    /** Fixed numbers only; -1 is an absent field, code 0 an open outcome. */
    data class Report(var code: Int = 0, var relay: Int = -1, var rttP50Ms: Int = -1, var rttP95Ms: Int = -1,
                      var lossPct: Int = -1, var keepSent: Long = 0, var keepLost: Long = 0)

    private enum class Stage { BINDING, PINGING, DRAINING, IDLE, ANSWERING, OVER }

    private val ready = BooleanArray(relays)
    private var stage = Stage.BINDING
    private var timerAt = nowUs
    private val deadlineAt = nowUs + BIND_BUDGET_US
    private var ticks = 0
    private var current = 0
    private var bindSent = false
    private var socketFailed = false
    private var waiting = false
    private var next = 0L
    private val pending = HashMap<Long, Long>()  // PINGs awaiting a PONG: sequence -> stamp
    private val rttsMs = ArrayList<Double>()
    private val report = Report()

    fun report(): Report = report
    fun over() = stage == Stage.OVER
    fun bound() = report.relay >= 0
    fun chosen() = report.relay
    /** When [tick] next has work: the timer, or the end of the BIND budget. */
    fun nextUs(): Long = when (stage) {
        Stage.OVER -> Long.MAX_VALUE
        Stage.BINDING -> minOf(timerAt, deadlineAt)
        else -> timerAt
    }

    /** A relay's name resolved: it takes its turn, at once if this is its turn. */
    fun resolved(relay: Int) {
        ready[relay] = true
        if (stage == Stage.BINDING && ticks != 0 && relay == current) sendBind(relay)
    }

    fun tick(nowUs: Long) {
        if (stage == Stage.BINDING && nowUs >= deadlineAt) return bindExpired()
        while (stage != Stage.OVER && nowUs >= timerAt) {
            val at = timerAt
            fire(at)
            if (timerAt <= at) break
        }
    }

    fun received(relay: Int, data: ByteArray, size: Int, nowUs: Long) {
        if (stage == Stage.OVER) return
        if (!bound()) { if (size == ack.size && matches(data)) start(relay, nowUs); return }
        if (relay == chosen()) pong(data, size, nowUs)
    }

    /** The socket is gone: what was measured stays. */
    fun socketLost() {
        if (stage == Stage.OVER) return
        if (report.code == 0) report.code = PROBE_SOCKET_ERROR else report.keepLost += pending.size
        stage = Stage.OVER
    }

    private fun fire(at: Long) {
        when (stage) {
            Stage.BINDING -> {
                val tick = ticks++
                current = (tick / BINDS_PER_RELAY) % ready.size
                if (ready[current]) sendBind(current)
                timerAt = at + BIND_INTERVAL_US
            }
            Stage.PINGING -> {
                if (waiting && next == HOST_WAIT_PINGS) { noHost(at); return }
                ping(at)
                timerAt = at + if (next < BURST_PINGS) BURST_INTERVAL_US else PONG_WINDOW_US
                if (next >= BURST_PINGS) stage = Stage.DRAINING
            }
            Stage.DRAINING -> burstDone(at)
            Stage.IDLE -> {
                ++report.keepSent; ping(at)
                stage = Stage.ANSWERING; timerAt = at + PONG_WINDOW_US
            }
            Stage.ANSWERING -> {
                report.keepLost += pending.size; pending.clear()
                stage = Stage.IDLE; timerAt = at + KEEPALIVE_INTERVAL_US - PONG_WINDOW_US
            }
            Stage.OVER -> {}
        }
    }
    private fun matches(data: ByteArray): Boolean {
        var difference = 0
        for (n in ack.indices) difference = difference or (data[n].toInt() xor ack[n].toInt())
        return difference == 0
    }
    private fun sendBind(relay: Int) { if (send(relay, bind, bind.size)) bindSent = true else socketFailed = true }
    private fun bindExpired() {
        report.code = if (bindSent) PROBE_BIND_TIMEOUT else if (socketFailed) PROBE_SOCKET_ERROR else PROBE_LOOKUP_FAILED
        stage = Stage.OVER
    }
    private fun start(relay: Int, nowUs: Long) {
        report.relay = relay
        waiting = true; stage = Stage.PINGING
        ping(nowUs); timerAt = nowUs + BURST_INTERVAL_US
    }
    private fun ping(nowUs: Long) {
        val sequence = next++
        val datagram = ByteArray(PROBE_BYTES)
        datagram[0] = 3; System.arraycopy(id, 0, datagram, 1, UdpCrypto.LEG_ID_BYTES); datagram[9] = 1
        Udp.put32(datagram, 13, sequence); Udp.put64(datagram, 17, nowUs)
        pending[sequence] = nowUs
        send(chosen(), datagram, datagram.size)
    }
    // The DATA header carries the Host's leg ID, which this leg never learns.
    private fun pong(data: ByteArray, size: Int, nowUs: Long) {
        if (size != PROBE_BYTES || data[0].toInt() != 3 || data[9].toInt() != 2 || data[10].toInt() != 0 || data[11].toInt() != 0 || data[12].toInt() != 0) return
        val sequence = Udp.get32(data, 13)
        val stamp = Udp.get64(data, 17)
        val found = pending[sequence]
        if (found == null || found != stamp || nowUs - stamp > PONG_WINDOW_US) return
        pending.remove(sequence)
        if (waiting) {
            waiting = false
            // A later PING was the first to reach the Host, so the ones before it say
            // nothing about the path. Clearing them leaves a late PONG of the old
            // numbering nothing to match: no entry, or a new entry's stamp.
            if (sequence != 0L) { pending.clear(); next = 0; stage = Stage.PINGING; ping(nowUs); timerAt = nowUs + BURST_INTERVAL_US; return }
        }
        if (sequence < BURST_PINGS) rttsMs.add((nowUs - stamp) / 1000.0)
    }
    // No burst ran, so no share of 25 means anything. Keepalives go on, numbered
    // as they would be after one.
    private fun noHost(at: Long) {
        waiting = false; pending.clear(); next = BURST_PINGS
        report.lossPct = 100; report.code = PROBE_NO_PONG
        stage = Stage.IDLE; timerAt = at + KEEPALIVE_INTERVAL_US - BURST_INTERVAL_US
    }
    private fun burstDone(at: Long) {
        report.lossPct = (pending.size * 100 / BURST_PINGS).toInt(); pending.clear()
        if (rttsMs.isNotEmpty()) { report.rttP50Ms = percentile(0.5); report.rttP95Ms = percentile(0.95) }
        report.code = if (rttsMs.isEmpty()) PROBE_NO_PONG else PROBE_REACHABLE
        stage = Stage.IDLE; timerAt = at + KEEPALIVE_INTERVAL_US - PONG_WINDOW_US
    }
    private fun percentile(fraction: Double): Int {
        rttsMs.sort()
        val rank = Math.ceil(fraction * rttsMs.size).toInt().coerceIn(1, rttsMs.size)
        return Math.round(rttsMs[rank - 1]).toInt()
    }

    companion object {
        const val PROOF_BYTES = 41
        const val PROBE_BYTES = 25
        const val PROBE_REACHABLE = 1
        const val PROBE_SOCKET_ERROR = 2
        const val PROBE_LOOKUP_FAILED = 3
        const val PROBE_BIND_TIMEOUT = 4
        const val PROBE_NO_PONG = 5
        const val BIND_INTERVAL_US = 200000L
        const val BIND_BUDGET_US = 5000000L
        const val BINDS_PER_RELAY = 3
        const val BURST_PINGS = 25L
        const val HOST_WAIT_PINGS = 15L  // 3 s at the burst's rate
        const val BURST_INTERVAL_US = 200000L
        const val PONG_WINDOW_US = 1000000L
        const val KEEPALIVE_INTERVAL_US = 20000000L
    }
}
