package me.totb.multitap_client

import io.flutter.embedding.engine.FlutterEngine
import io.flutter.plugin.common.MethodCall
import io.flutter.plugin.common.MethodChannel
import java.net.InetAddress
import java.net.InetSocketAddress

/**
 * `multitap/udp` (udp-media-v1, windows/runner/udp_media_channel.h): Dart stays
 * in charge of control and starts, reads and stops the leg here. Every call
 * carries the leg's number, so a late call of a closed session cannot reach
 * its successor. Reading is Dart's timer, which bounds the rate of arrival
 * reports. Main thread; no call here waits for the socket.
 */
class AndroidUdp(engine: FlutterEngine) {
    internal val gate = UdpPathGate()
    private val channel = MethodChannel(engine.dartExecutor.binaryMessenger, "multitap/udp")
    internal var trackGate: ((AndroidTrackBinding) -> UdpPathGate?)? = null
    private class Slot(val track: String?, val gate: UdpPathGate, val leg: UdpMediaLeg)
    private val registry = LinkedHashMap<Long, Slot>()
    private val pending = LinkedHashMap<Long, Pair<Int, Long>>()
    private var legs = 0L

    init { channel.setMethodCallHandler(::call) }

    private fun call(call: MethodCall, result: MethodChannel.Result) {
        val args = call.arguments as? Map<*, *>
        val called = args?.let { udpInteger(it["leg"]) } ?: 0L
        if (args == null || called < 1) { result.error("invalid_arguments", "UDP leg required.", null); return }
        when (call.method) {
            "start" -> {
                val grant = udpGrant(args)
                val clockUs = udpInteger(args["clockUs"])
                if (grant == null || clockUs == null) {
                    (args["ticket"] as? ByteArray)?.fill(0)
                    result.error("invalid_arguments", "UDP grant required.", null); return
                }
                val track = args["track"] as? String
                val receiveGate = if (args.containsKey("track")) {
                    val topology = udpInteger(args["topologyEpoch"]); val epoch = udpInteger(args["epoch"])
                    val generation = udpInteger(args["generation"]); val primary = args["primary"] as? Boolean
                    if (track !in listOf("0", "1") || topology == null || topology !in 1..0xffffffffL || epoch == null || epoch !in 1..65535 || generation == null || generation < 1 || primary == null) {
                        grant.ticket.fill(0); result.error("invalid_arguments", "Output binding required.", null); return
                    }
                    trackGate?.invoke(AndroidTrackBinding(track!!, topology, epoch, primary))
                } else gate
                if (receiveGate == null) { grant.ticket.fill(0); result.error("session_replaced", "Output decoder replaced.", null); return }
                val obsolete = registry.filterValues { track == null || it.track == null || it.track == track }.keys.toList()
                for (key in obsolete) registry.remove(key)?.let { it.gate.disengage(); it.leg.stop() }
                val token = ++legs
                receiveGate.disengage(token)
                pending.remove(called)?.let { receiveGate.stream(it.first, it.second) }
                try { registry[called] = Slot(track, receiveGate, UdpMediaLeg(grant, receiveGate, token, clockUs)); result.success(null) }
                catch (_: Throwable) { grant.ticket.fill(0); receiveGate.disengage(); result.error("udp_leg_failed", "The UDP leg did not start.", null) }
                return
            }
            "stream" -> {
                val epoch = udpInteger(args["epoch"])
                val generation = udpInteger(args["generation"])
                if (epoch == null || epoch < 0 || epoch > 65535 || generation == null || generation < 1) {
                    result.error("invalid_arguments", "Stream epoch required.", null); return
                }
                val slot = registry[called]
                if (slot != null) slot.gate.stream(epoch.toInt(), generation)
                else { if (pending.size >= 3) pending.remove(pending.keys.first()); pending[called] = epoch.toInt() to generation }
                result.success(null)
                return
            }
        }
        val slot = registry[called]
        if (slot == null) { pending.remove(called); result.success(null); return }
        val current = slot.leg
        when (call.method) {
            "input" -> {
                val sequence=udpInteger(args["sequence"]);val epoch=udpInteger(args["epoch"]);val bytes=args["bytes"] as? ByteArray
                if(sequence==null||sequence<1||sequence>0xffffffffL||epoch==null||epoch<0||epoch>65535||bytes==null||bytes.isEmpty()||bytes.size>Udp.MAX_PLAINTEXT-9){result.error("invalid_arguments","Direct input required.",null);return}
                current.input(sequence,epoch.toInt(),bytes);result.success(null)
            }
            "peer" -> {
                val rows=args["candidates"] as? List<*>
                val candidates=ArrayList<InetSocketAddress>()
                if(rows==null || rows.size>5){result.error("invalid_arguments","Peer candidates required.",null);return}
                for(value in rows){
                    val row=value as? Map<*,*>;val address=row?.get("address") as? String;val port=row?.let { udpInteger(it["port"]) }
                    val parts=address?.split('.')
                    if(parts==null||parts.size!=4||parts.any { !Regex("^(0|[1-9][0-9]{0,2})$").matches(it)||(it.toIntOrNull()?:256)>255 }||port==null||port<1||port>65535){result.error("invalid_arguments","Peer candidate required.",null);return}
                    candidates.add(InetSocketAddress(InetAddress.getByAddress(parts.map { it.toInt().toByte() }.toByteArray()),port.toInt()))
                }
                current.peer(candidates);result.success(null)
            }
            "poll" -> {
                val reading = current.read()
                // A failed reading is what Dart reports as udpLegFailed; the session stays on WSS.
                if (reading.failed) result.error("udp_leg_failed", "The UDP leg failed its crypto self-check.", null)
                else result.success(udpReadingValue(reading))
            }
            "hostKey" -> {
                val key = args["key"] as? ByteArray
                val keyed = args["keyed"] as? Boolean
                if (key == null || key.size != UdpCrypto.KEY_BYTES || keyed == null) { result.error("invalid_arguments", "Host key required.", null); return }
                // Before the leg can use the key: from this call on every WSS push follows the rule.
                slot.gate.engage(keyed)
                current.hostKey(key)
                result.success(null)
            }
            "stop" -> { slot.gate.disengage(); current.stop(); registry.remove(called); result.success(null) }
            else -> result.notImplemented()
        }
    }

    fun dispose() {
        channel.setMethodCallHandler(null)
        for (slot in registry.values) { slot.gate.disengage(); slot.leg.stop() }
        registry.clear(); pending.clear(); trackGate = null; gate.disengage()
    }
}
