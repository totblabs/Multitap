package me.totb.multitap_client

/** One install owner from admission through worker cleanup and the installer's terminal status. */
internal class AndroidUpdateOwnership private constructor(private val state: State) {
    constructor() : this(State())

    private class State { var current: Lease? = null }
    class Lease internal constructor(internal val owner: Any) {
        internal var session = -1
        internal var workerDone = false
        internal var terminal = false
        internal var disposed = false
    }

    private val identity = Any()
    private var disposed = false // guarded by shared state, including cross-instance access

    fun acquire(): Lease? = synchronized(state) {
        if (disposed || state.current != null) return@synchronized null
        Lease(identity).also { state.current = it }
    }

    fun busy(): Boolean = synchronized(state) { state.current != null }

    fun owns(lease: Lease): Boolean = synchronized(state) {
        !disposed && lease.owner === identity && state.current === lease
    }

    fun runIfOwned(lease: Lease, action: () -> Unit): Boolean = synchronized(state) {
        if (!owns(lease)) return@synchronized false
        action()
        true
    }

    fun bind(lease: Lease, session: Int): Boolean = synchronized(state) {
        if (lease.owner !== identity || state.current !== lease || session < 0 || lease.session != -1) return@synchronized false
        // Record a session just created during disposal so exact cleanup can still prove its ID.
        lease.session = session
        !disposed
    }

    fun accepts(session: Int): Boolean = synchronized(state) {
        !disposed && session >= 0 && state.current?.let {
            it.owner === identity && it.session == session && !it.terminal
        } == true
    }

    /** A terminal callback may arrive before the worker's final cleanup. */
    fun finish(session: Int): Boolean = synchronized(state) {
        val lease = state.current ?: return@synchronized false
        if (!accepts(session)) return@synchronized false
        lease.terminal = true
        if (lease.workerDone) state.current = null
        true
    }

    /** An application-side launch failure is terminal only after its exact session was abandoned. */
    fun abort(session: Int, abandon: () -> Unit): Boolean = synchronized(state) {
        if (!accepts(session)) return@synchronized false
        abandon()
        finish(session)
    }

    fun session(): Int = synchronized(state) {
        state.current?.takeIf { it.owner === identity }?.session ?: -1
    }

    /** Only in-process proof of a retired exact session permits a replacement instance to clean it. */
    fun recoverRetired(abandon: (Int) -> Unit): Boolean = synchronized(state) {
        val lease = state.current ?: return@synchronized true
        if (!lease.disposed || !lease.workerDone) return@synchronized false
        if (lease.session >= 0 && !lease.terminal) abandon(lease.session)
        state.current = null
        true
    }

    /** Cleanup stays inside admission's lock: another Activity cannot acquire the same APK first. */
    fun workerFinished(lease: Lease, failed: Boolean, cleanup: () -> Unit): Boolean = synchronized(state) {
        if (lease.workerDone || lease.owner !== identity || state.current !== lease) return@synchronized false
        try {
            cleanup()
        } finally {
            lease.workerDone = true
            if (failed || lease.terminal || (lease.disposed && lease.session < 0)) state.current = null
        }
        true
    }

    /** A disposed worker keeps shared admission busy until its own file cleanup finishes. */
    fun dispose() = synchronized(state) {
        if (disposed) return@synchronized
        disposed = true
        state.current?.takeIf { it.owner === identity }?.let {
            it.disposed = true
            if (it.workerDone && (it.terminal || it.session < 0)) state.current = null
        }
    }

    companion object {
        private val process = State()
        /** Distinct Activity owners share one in-process install/cleanup boundary. */
        fun shared(): AndroidUpdateOwnership = AndroidUpdateOwnership(process)
    }
}
