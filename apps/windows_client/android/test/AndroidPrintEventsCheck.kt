package me.totb.multitap_client

/** Fake callback checks only; no Android runtime, document or print service. */
object AndroidPrintEventsCheck {
    @JvmStatic fun main(args: Array<String>) {
        val id = "a".repeat(32)
        val sent = mutableListOf<Map<String, Any>>()
        val events = AndroidPrintEvents(id) { sent.add(it) }
        for (stage in 1..8) events.stage(stage)
        check(sent.size == 8)
        sent.forEachIndexed { index, row ->
            check(row.keys == setOf("requestId", "sequence", "stage"))
            check(row["requestId"] == id)
            check(row["sequence"] == index + 1)
            check(row["stage"] == index + 1)
        }
        events.close()
        events.stage(9)
        check(sent.size == 8)
        val before = sent.size
        for (invalid in listOf(null, "", "path/private/document.pdf", "f".repeat(33), "g".repeat(32))) {
            AndroidPrintEvents(invalid) { sent.add(it) }.stage(1)
        }
        check(sent.size == before)
        val bounded = AndroidPrintEvents("b".repeat(32)) { sent.add(it) }
        bounded.stage(0)
        bounded.stage(10)
        repeat(1000) { bounded.stage(6) }
        check(sent.size == before + 64)
        check(sent.last()["sequence"] == 64)
        var attempted = 0
        val unavailable = AndroidPrintEvents(id) { attempted++; throw IllegalStateException("fake callback") }
        unavailable.stage(1)
        unavailable.stage(2)
        unavailable.close()
        unavailable.stage(3)
        check(attempted == 2)
        println("Android print stage checks passed: ordering, fixed fields, request scope, bound, close, callback failure")
    }
}
