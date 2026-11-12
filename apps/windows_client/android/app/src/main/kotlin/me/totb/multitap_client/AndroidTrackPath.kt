package me.totb.multitap_client

/** WSS queue and main-thread regrant share this lane's lookup and admission.
 * A detached legacy gate passes WSS through, so never retain it across a swap.
 * The lock covers only bounded queue admission; decoder stop runs outside it.
 */
internal class AndroidTrackPath(initial: UdpPathGate) {
    private val lock = Any()
    private var gate = initial

    fun replace(next: UdpPathGate, current: () -> Boolean): Boolean = synchronized(lock) {
        if (!current()) return@synchronized false
        gate.attach(0, null)
        gate = next
        true
    }

    fun detach() = synchronized(lock) { gate.attach(0, null) }

    fun offerWss(lease: Long, bytes: ByteArray, current: () -> Boolean, admit: () -> Byte): Byte = synchronized(lock) {
        if (!current()) AndroidMediaStatus.SESSION_REPLACED
        else gate.offerWss(lease, bytes, bytes.size, admit)
    }
}
