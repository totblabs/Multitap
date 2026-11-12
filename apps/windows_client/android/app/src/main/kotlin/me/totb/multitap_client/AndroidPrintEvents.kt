package me.totb.multitap_client

/** Fixed, bounded native stages; no document names, paths or content. */
class AndroidPrintEvents(requestId: String?, private val send: (Map<String, Any>) -> Unit) {
    private val request = requestId?.takeIf { it.matches(Regex("^[a-f0-9]{32}$")) }
    private var sequence = 0
    private var closed = false

    fun stage(stage: Int) {
        val id = request ?: return
        if (closed || sequence >= 64 || stage !in 1..9) return
        sequence++
        try {
            send(mapOf("requestId" to id, "sequence" to sequence, "stage" to stage))
        } catch (_: Exception) {
            // Observational only: a diagnostic callback cannot fail printing.
        }
    }

    fun close() { closed = true }
}
