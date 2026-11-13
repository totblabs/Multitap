package me.totb.multitap_client

import android.content.Context
import android.hardware.display.DisplayManager
import android.media.AudioManager
import android.media.AudioTimestamp
import android.media.AudioTrack
import android.media.MediaCodec
import android.media.MediaFormat
import android.os.Build
import android.os.Handler
import android.os.HandlerThread
import android.os.Process
import android.system.Os
import android.system.OsConstants
import android.view.Display
import android.view.KeyEvent
import android.view.Surface
import android.view.SurfaceHolder
import android.view.SurfaceView
import android.view.View
import android.view.ViewGroup
import android.view.WindowManager
import io.flutter.embedding.engine.FlutterEngine
import io.flutter.plugin.common.BinaryMessenger
import io.flutter.plugin.common.EventChannel
import io.flutter.plugin.common.MethodCall
import io.flutter.plugin.common.MethodChannel
import io.flutter.view.TextureRegistry
import java.nio.ByteBuffer
import kotlin.math.roundToInt

/**
 * One bounded decoder per authorized Dart session. This class has no network client.
 *
 * Media packets arrive on the binary channel `multitap/remote/media`
 * (media-wire-v1, AndroidMediaPacing.kt), answered on a background queue: in
 * this Flutter the Dart session runs on the Android main thread, and the
 * former `push` method decoded and copied every packet there. A packet is
 * copied into a reused buffer, admitted to a small bounded queue and answered
 * at once; a full queue answers `media_queue_full` (the session waits and
 * offers the packet again), a decoder failure is answered on the next packet.
 *
 * One worker thread owns the decoder: MediaCodec runs in asynchronous mode, so
 * input is queued the moment the decoder frees a buffer and every decoded
 * frame arrives as a callback; nothing waits or polls, and the thread sleeps
 * while no packets arrive. The decoder is created as soon as the session
 * starts (its parameter sets follow as codec-config input with the key frame),
 * so a new picture size does not wait for decoder creation; a decoder that
 * refuses that is created at the key frame with them in its format.
 *
 * Frames are paced to Flutter (AndroidFramePacer): at most one rendered frame
 * waits for the texture, newer decoded frames replace a held one, and the
 * newest is always shown. Statistics use the Windows client's field names
 * (texture-pacing-v1) where they mean the same.
 *
 * A texture outlives its decoder until the next decoder's first frame (or a
 * timeout), so the picture never goes blank or shows a stretched old frame
 * while the remote resolution changes: Dart keeps drawing the old texture at
 * its own size until the new decoder has produced a frame.
 *
 * udp-media-v1: the UDP leg (AndroidUdp.kt) reaches the decoder only through
 * [gate], attached while a lease's decoder runs, and a WSS packet marked for
 * the path rule passes the same gate (windows/runner/remote_media_session.h).
 *
 * audio-opus-v1: a start with `audioFormat: "opus"` (stream.info said so for
 * a granted connection) takes its audio packets as sequence-numbered 10 ms
 * Opus packets (AndroidOpusAudio.kt), decoded by AndroidOpusDecoder into the
 * same track path as PCM packets. Any other start is the released PCM. The
 * Opus decoder is made on [setup], never on the worker (AndroidOpusPipe).
 *
 * Sound, PCM or decoded Opus, goes from the worker into a jitter buffer
 * (AudioJitterBuffer, the Windows client's rules) and from there to the
 * AudioTrack on a thread of its own at audio priority (AndroidAudioOut): the
 * worker never writes to the track.
 *
 * While the app is not visible ([visible] false: the Activity stopped) the
 * decoder goes on (the H.264 reference chain needs every packet, and a client
 * cannot ask for a key frame on WebSocket: only the relay sends
 * stream.keyframe), but no frame is rendered to the texture: the newest one
 * is held and rendered when the app is visible again (AndroidFramePacer).
 * Audio plays on as before, at the volume its audio focus allows (AndroidAudioFocus: muted during a call).
 */
class AndroidMedia internal constructor(private val activity: MainActivity, private val engine: FlutterEngine, private val gate: UdpPathGate,
    private val registerChannels: Boolean = true) {
    private val main = Handler(activity.mainLooper)
    private val thread = HandlerThread("Multitap decoder", Process.THREAD_PRIORITY_URGENT_DISPLAY).apply { start() }
    private val worker = Handler(thread.looper)
    // Lists, makes and releases Opus decoders off the worker (AndroidOpusDecoder).
    private val setupThread = HandlerThread("Multitap audio setup").apply { start() }
    private val setup = Handler(setupThread.looper)
    // The Activity is stopped: frames are held, not rendered (any thread; read by the worker).
    @Volatile private var hidden = false
    private val textures = engine.renderer
    private val messenger = engine.dartExecutor.binaryMessenger
    private val ownership = AndroidMediaOwnership()
    private val sizeObservation = StreamSizeObservation()
    private var sizeQueryPending = false // main-thread, at most one read-only query
    private var sizeStartPending = false // admitted metadata owner, not released by observer cancellation
    private val lease get() = ownership.current
    @Volatile private var captured = false
    private val keyMap = AndroidKeyMap() // main thread
    @Volatile private var disposed = false
    // AndroidAudioFocus.kt: the VM sound's audio focus (main thread); its volume goes to the track (any thread).
    private val audioFocus: AndroidAudioFocus = AndroidAudioFocus(activity, main) { volume -> audioOut.setVolume(volume) }
    // AndroidAudioJitter.kt: received sound waits here (the decoder worker pushes, the audio thread pulls, both
    // under its lock). One per engine: its counters are the app's totals (audio-stats-v1 deltas, AndroidRemoteMedia).
    private val playout: AudioJitterBuffer = AudioJitterBuffer(AudioJitterConfig.ANDROID)
    // AndroidAudioOut.kt: the stream's track, fed from [playout] by its own audio-priority thread.
    private val audioOut: AndroidAudioOut = AndroidAudioOut(playout, { audioFocus.volume },
        onTrack = { main.post { audioFocus.acquire() } }, onSound = { main.post { audioFocus.soundStarted() } })
    // AndroidStreamWifi.kt: the Wi-Fi low-latency lock of a stream that is shown (main thread; not for a track's own decoder).
    private val wifi = if (registerChannels) AndroidStreamWifi(activity) else null
    // AndroidThermal.kt: the device's thermal status while a stream runs, for the statistics only.
    private val thermal = if (registerChannels) AndroidThermal(activity) else null
    private var sink: EventChannel.EventSink? = null
    private class TrackLane(val track: AndroidTrack, val decoder: AndroidMedia, gate: UdpPathGate) {
        val path = AndroidTrackPath(gate)
        var texture = -1L
    }
    private class TrackState(val lease: Long, val configuration: AndroidTracks, val lanes: List<TrackLane>)
    @Volatile private var trackState: TrackState? = null
    // Packets accepted from Dart or the UDP leg and not yet handed to the decoder (any thread).
    private val queue = AndroidMediaAdmission(MAX_QUEUED_PACKETS, MAX_QUEUED_BYTES, MAX_QUEUED_AUDIO_PACKETS)
    // The lease whose decoder failed: its next packet is refused.
    @Volatile private var failedLease = 0L
    // The lease started with audio-opus-v1 audio (0: none); admission checks its packets as Opus.
    @Volatile private var opusLease = 0L
    @Volatile private var error = 0
    private val pool = AndroidBufferPool()
    // Main thread: retired textures, released once a newer texture has shown
    // its first frame or after RETIRED_TEXTURE_MS; the display mode a stream
    // replaced.
    private val retired = ArrayList<TextureRegistry.SurfaceTextureEntry>()
    private var replacedMode: Int? = null
    @Volatile private var display: Display? = null

    // --- worker thread only --------------------------------------------------
    private var texture: TextureRegistry.SurfaceTextureEntry? = null
    private var surface: Surface? = null
    private var decoder: MediaCodec? = null
    private var callbacks: Callbacks? = null
    private var name = ""
    private var hardware: Boolean? = null
    /** What the decoder was configured with (avcTuningFlags, AvcVendorKeys code); null before one started. */
    private var lowLatency: Int? = null
    private var vendorKeys: Int? = null
    /** The decoder got the plain format because it failed tuned earlier in this app run (AvcFailureMemory). */
    private var plainFallback = false
    /** The decoder was started ahead of the key frame (parameter sets as codec config). */
    private var early = false
    /** SPS+PPS (Annex B) to queue as codec config before the first frame; awaited until the key frame. */
    private var parameterSets: ByteArray? = null
    private var awaitingKeyFrame = false
    private var firstFrame = true
    private val pending = ArrayDeque<Packet>()
    private val inputs = ArrayDeque<Int>()
    private var pacer = AndroidFramePacer()
    private val clock = AndroidDecodeClock()
    private val appCpu = AppCpuMeter(Runtime.getRuntime().availableProcessors(), try { Os.sysconf(OsConstants._SC_CLK_TCK) } catch (_: Exception) { 100L })
    private var checkPosted = false
    private val frameWatch = AndroidInputFrameWatch()
    private var audioEnabled = false
    private var audioOpus = false
    private var opus: AndroidOpusDecoder? = null
    // Opus frames concealed by earlier decoders of this engine (audio-stats-v1 counts for the app, as [playout]).
    private var concealedBefore = 0L
    // [playout]'s dropped frames (overrun cuts and drift trims) when this stream started: audioDroppedFrames is per stream.
    private var droppedBefore = 0L
    // [playout]'s underruns when this stream started: audioStreamUnderruns is per stream (the 연결 통계 dialog).
    private var underrunsBefore = 0L
    // The device's media volume, for the 연결 통계 dialog (a phone at 0 plays nothing).
    private val audioManager = activity.getSystemService(Context.AUDIO_SERVICE) as AudioManager?
    private var width = 0
    private var height = 0
    private var targetFps = 60
    private var allowHardware = true
    private var exactSizes = false
    private var decoded = 0L
    private var decodeCalls = 0L
    private var decodeMs = 0.0
    private var decodeMaxMs = 0.0
    private var droppedPackets = 0L
    // The last second with frames, for the 연결 통계 dialog: queued to the codec -> decoded, the packet's
    // arrival -> decoded (the wait for a decoder input included), and decoded -> taken by Flutter.
    private val decodeRecent = RecentWindow()
    private val arrivalRecent = RecentWindow()
    private val presentRecent = RecentWindow()

    private val frames = object : AndroidFrameSink {
        override fun render(frame: Int) { decoder?.releaseOutputBuffer(frame, true) }
        override fun discard(frame: Int) { decoder?.releaseOutputBuffer(frame, false) }
    }
    private val pacingCheck = Runnable {
        checkPosted = false
        try { pacer.check(System.nanoTime(), frames) } catch (_: Exception) { fail(lease) }
        scheduleCheck()
    }
    // One status byte per answer (direct, as the engine requires; never changed).
    private val answers = Array(5) { ByteBuffer.allocateDirect(1).put(it.toByte()) }

    private class Packet(val lease: Long, val bytes: ByteArray, val length: Int, val watch: Boolean, val accepted: Long) {
        val video get() = bytes[0].toInt() == 1
        val key get() = bytes[1].toInt() and 1 != 0
        var nonReference = false
    }

    init {
      if (registerChannels) {
        EventChannel(engine.dartExecutor.binaryMessenger, "multitap/remote/input")
            .setStreamHandler(object : EventChannel.StreamHandler {
                override fun onListen(arguments: Any?, events: EventChannel.EventSink) { sink = events }
                override fun onCancel(arguments: Any?) { sink = null }
            })
        MethodChannel(engine.dartExecutor.binaryMessenger, "multitap/remote")
            .setMethodCallHandler(::call)
        messenger.setMessageHandler(MEDIA_CHANNEL, ::media, messenger.makeBackgroundTaskQueue())
      }
        // The platform's codec list is read once per process, here rather than at the first Opus stream.
        setup.post { AndroidOpusDecoder.names() }
    }

    /**
     * Main thread: the Activity started (true) or stopped (false). Inactive
     * (a dialog, the notification shade) is still visible and changes nothing.
     */
    fun visible(shown: Boolean) {
        trackState?.lanes?.forEach { it.decoder.visible(shown) }
        wifi?.visible(shown)
        audioFocus.visible(shown)
        if (hidden == !shown) return
        hidden = !shown
        worker.post {
            if (!shown) pacer.background()
            else {
                try { pacer.foreground(System.nanoTime(), frames) } catch (_: Exception) { fail(lease) }
                scheduleCheck()
            }
        }
    }

    /**
     * Main thread: the Activity's window gained or lost input focus (the user
     * is in the app, or went to another window in split screen, a dialog or
     * the shade). A permanent audio-focus loss is asked back when the user
     * returns (AndroidAudioFocus.windowFocus).
     */
    fun windowFocus(focused: Boolean) {
        trackState?.lanes?.forEach { it.decoder.windowFocus(focused) }
        audioFocus.windowFocus(focused)
    }

    /** Background task queue: one media-wire-v1 packet. */
    private fun media(message: ByteBuffer?, reply: BinaryMessenger.BinaryReply) {
        val status = try {
            val wire = message?.let(::androidMediaWire)
            if (message == null || wire == null) AndroidMediaStatus.INVALID_PACKET
            else if (trackState != null) {
                val bytes = ByteArray(wire.length)
                message.position(wire.offset); message.get(bytes)
                offerTrack(wire.lease, bytes)
            }
            else {
                val video = message.get(wire.offset).toInt() == 1
                val admit = {
                    offer(wire.lease, wire.watch, video, wire.length) { target ->
                        message.position(wire.offset)
                        message.get(target, 0, wire.length)
                    }
                }
                if (!wire.path) admit()
                else {
                    val head = ByteArray(minOf(wire.length, 16))
                    message.position(wire.offset)
                    message.get(head)
                    gate.offerWss(wire.lease, head, wire.length, admit)
                }
            }
        } catch (_: Exception) {
            AndroidMediaStatus.INVALID_PACKET
        }
        reply.reply(answers[status.toInt()])
    }

    /**
     * Any thread. Admits one media packet of [current] to the bounded queue
     * ([video]: its kind byte is 1); [copy] fills the reused buffer with
     * exactly [length] bytes.
     */
    private inline fun offer(current: Long, watch: Boolean, video: Boolean, length: Int, copy: (ByteArray) -> Unit): Byte {
        if (disposed || !ownership.owns(current)) return AndroidMediaStatus.SESSION_REPLACED
        if (failedLease == current) return AndroidMediaStatus.DECODER_FAILED
        if (queue.full(video, length)) return AndroidMediaStatus.QUEUE_FULL
        val bytes = pool.take(length)
        copy(bytes)
        if (!androidMediaPacketValid(bytes, length, opus = opusLease == current) || (watch && bytes[0].toInt() != 1) || (bytes[0].toInt() == 1) != video) {
            pool.give(bytes)
            return AndroidMediaStatus.INVALID_PACKET
        }
        queue.added(video, length)
        val packet = Packet(current, bytes, length, watch, System.nanoTime())
        worker.post { receive(packet) }
        return AndroidMediaStatus.OK
    }

    private fun call(call: MethodCall, result: MethodChannel.Result) {
        when (call.method) {
            "startTracks" -> beginTracks(call, result)
            "start" -> {
                beginStart(call, result)
            }
            "validateStreamSize" -> {
                val request = streamSizeRequest(call.arguments)
                if (request == null) { result.success(streamSizeReply(emptyList(), "invalid_request")); return }
                if (disposed) { result.success(streamSizeReply(request.tracks, "engine_closed")); return }
                if (sizeQueryPending) { result.success(streamSizeReply(request.tracks, "operation_busy")); return }
                sizeQueryPending = true
                val observed = sizeObservation.snapshot()
                val posted = worker.post {
                    val reply = try { AvcStreamSizes.validate(request, activity) }
                        catch (_: Exception) { streamSizeReply(request.tracks, "capability_unknown") }
                    main.post {
                        sizeQueryPending = false
                        result.success(when {
                            disposed -> streamSizeReply(request.tracks, "engine_closed")
                            !sizeObservation.current(observed) -> streamSizeReply(request.tracks, "session_replaced")
                            else -> reply
                        })
                    }
                }
                if (!posted) { sizeQueryPending = false; result.success(streamSizeReply(request.tracks, "engine_closed")) }
            }
            "decoderLimits" -> {
                // stream-size-v1: what the AVC decoder this engine creates
                // declares it can take. Null when Android cannot say.
                result.success(try { AvcStreamSizes.informationalLimits() } catch (_: Exception) { null })
            }
            "sharpPicture" -> {
                // 선명하게: the size and Windows scale for a picture area, from what the decoders
                // this engine would start take at that size and rate (read-only metadata).
                val request = sharpPictureRequest(call.arguments)
                if (request == null || disposed) { result.success(null); return }
                val posted = worker.post {
                    val plan = try {
                        sharpPicturePlan(request, AvcStreamSizes.candidates(request.hardware), AvcStreamSizes.memoryBudget(activity))
                    } catch (_: Exception) { null }
                    if (plan != null) SharpPictures.stats.planned(plan)
                    main.post { if (!disposed) result.success(plan?.let(::sharpPictureReply)) }
                }
                if (!posted) result.success(null)
            }
            "sharpPictureFallback" -> {
                call.argument<String>("reason")?.let(SharpPictures.stats::hostFallback)
                result.success(null)
            }
            "sharpPictureOff" -> {
                SharpPictures.stats.off()
                result.success(null)
            }
            "push" -> {
                // The released map envelope; the session sends media-wire-v1.
                val current = call.argument<Number>("lease")?.toLong() ?: 0
                if (!ownership.owns(current)) { result.error("session_replaced", null, null); return }
                val message = androidMediaPacket(call.arguments)
                if (message == null || message.lease != current) {
                    result.error("invalid_packet", "Invalid media packet", null); return
                }
                val bytes = message.bytes
                val status = if (trackState != null) offerTrack(current, bytes) else
                    offer(current, message.watch, bytes[0].toInt() == 1, bytes.size) { System.arraycopy(bytes, 0, it, 0, bytes.size) }
                when (status) {
                    AndroidMediaStatus.OK -> result.success(null)
                    AndroidMediaStatus.QUEUE_FULL -> result.error("media_queue_full", null, null)
                    AndroidMediaStatus.SESSION_REPLACED -> result.error("session_replaced", null, null)
                    AndroidMediaStatus.DECODER_FAILED -> result.error("decoder_failed", "Android media decoder failed", null)
                    else -> result.error("invalid_packet", "Invalid media packet", null)
                }
            }
            "capture" -> {
                val current = call.argument<Number>("lease")?.toLong() ?: 0
                if (!ownership.owns(current)) { result.error("session_replaced", null, null); return }
                captured = call.argument<Boolean>("enabled") == true && lease > 0
                // Either way the remote starts with no key held (Dart sends input.reset).
                keyMap.release()
                result.success(captured)
            }
            "stats" -> {
                val current = call.argument<Number>("lease")?.toLong() ?: 0
                if (!ownership.owns(current)) { result.error("session_replaced", null, null); return }
                val tracks = trackState
                if (tracks != null) { trackStats(tracks, result); return }
                worker.post {
                    if (!ownership.owns(current)) { main.post { if (!disposed) result.error("session_replaced", null, null) }; return@post }
                    val stats = stats()
                    main.post {
                        if (!disposed) {
                            if (ownership.owns(current)) result.success(stats)
                            else result.error("session_replaced", null, null)
                        }
                    }
                }
            }
            "stop" -> {
                val current = call.argument<Number>("lease")?.toLong() ?: 0
                // A late old stop is idempotent and cannot release new input,
                // clear KEEP_SCREEN_ON or enqueue closeEngine for a new owner.
                if (!ownership.owns(current)) { result.success(null); return }
                closeTracks()
                gate.attach(0, null)
                releaseInput(); ownership.end(current); sizeObservation.changed()
                activity.window.clearFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON)
                preferStreamRefresh(false)
                worker.post { closeEngine(); main.post { if (!disposed) result.success(null) } }
            }
            else -> result.notImplemented()
        }
    }

    /** Retire gates before the decoder workers: late sockets cannot attach to a successor. */
    private fun closeTracks() {
        val old = trackState ?: return
        trackState = null
        for (lane in old.lanes) lane.path.detach()
        for (lane in old.lanes) lane.decoder.dispose()
    }

    internal fun trackGate(binding: AndroidTrackBinding): UdpPathGate? {
        val state = trackState ?: return null
        if (!ownership.owns(state.lease) || state.configuration.topology != binding.topology) return null
        val lane = state.lanes.find { it.track.id == binding.track && it.track.epoch == binding.epoch && it.track.primary == binding.primary } ?: return null
        if (binding.epoch !in 1..65535 || lane.texture < 0) return null
        val fresh = UdpPathGate()
        fresh.attach(state.lease) { bytes, video ->
            if (!androidTrackUdpPacket(binding, bytes, state.configuration.opus)) AndroidMediaStatus.INVALID_PACKET
            else lane.decoder.offer(state.lease, video, video, bytes.size) { System.arraycopy(bytes, 0, it, 0, bytes.size) }
        }
        if (!lane.path.replace(fresh) { trackState === state && ownership.owns(state.lease) && lane.texture >= 0 }) {
            fresh.attach(0, null); return null
        }
        return fresh
    }

    private fun offerTrack(current: Long, bytes: ByteArray): Byte {
        val state = trackState ?: return AndroidMediaStatus.SESSION_REPLACED
        if (!ownership.owns(current) || state.lease != current) return AndroidMediaStatus.SESSION_REPLACED
        val packet = androidTrackPacket(state.configuration, bytes) ?: return AndroidMediaStatus.INVALID_PACKET
        val lane = state.lanes.find { it.track.id == packet.track.id } ?: return AndroidMediaStatus.INVALID_PACKET
        return lane.path.offerWss(current, packet.bytes, { trackState === state && ownership.owns(current) }) {
            lane.decoder.offer(current, false, packet.video, packet.bytes.size) {
                System.arraycopy(packet.bytes, 0, it, 0, packet.bytes.size)
            }
        }
    }

    private fun beginTracks(call: MethodCall, result: MethodChannel.Result) {
        val configuration = androidTracks(call.argument<Any>("configuration"))
        val next = udpInteger(call.argument<Any>("lease")) ?: 0
        val fps = udpInteger(call.argument<Any>("fps"))?.toInt() ?: 60
        if (disposed || !registerChannels) { result.error("engine_closed", null, null); return }
        if (sizeStartPending) { result.error("media_operation_pending", null, null); return }
        if (configuration == null || next <= 0 || fps !in listOf(30, 60)) { result.error("invalid_geometry", null, null); return }
        if (!ownership.canBegin(next)) { result.error("session_replaced", null, null); return }
        val hardware = call.argument<Boolean>("hardwareDecode") ?: true
        val request = streamSizeRequest(mapOf("schema" to 1, "hardware" to hardware,
            "tracks" to configuration.tracks.map { mapOf("width" to it.width, "height" to it.height, "fps" to fps,
                "sourceWidth" to it.sourceWidth, "sourceHeight" to it.sourceHeight) }))
        if (request == null || (streamSizeMemoryBytes(request.tracks) ?: Long.MAX_VALUE) > AvcStreamSizes.memoryBudget(activity)) {
            result.error("decoder_size_unsupported", null, null); return
        }
        closeTracks(); gate.attach(0, null); releaseInput()
        check(ownership.begin(next)); sizeObservation.changed()
        worker.post { closeEngine() }
        val state = TrackState(next, configuration, configuration.tracks.map { track ->
            val gate = UdpPathGate()
            TrackLane(track, AndroidMedia(activity, engine, gate, false), gate)
        })
        trackState = state
        targetFps = fps; display = currentDisplay(); preferStreamRefresh(true)
        activity.window.addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON)
        var remaining = state.lanes.size
        var answered = false
        fun failed(code: String, message: String?) {
            if (answered) return
            answered = true
            if (trackState === state) { closeTracks(); ownership.end(next); sizeObservation.changed(); preferStreamRefresh(false) }
            result.error(code, message, null)
        }
        for (lane in state.lanes) {
            val track = lane.track
            lane.decoder.visible(!hidden)
            val arguments = mapOf("lease" to next, "width" to track.width, "height" to track.height,
                "sourceWidth" to track.sourceWidth, "sourceHeight" to track.sourceHeight,
                "fps" to fps, "hardwareDecode" to hardware, "streamSizeV2" to true,
                "audioEnabled" to (configuration.audio && track.primary),
                "audioFormat" to if (configuration.opus) "opus" else "pcm")
            lane.decoder.beginStart(MethodCall("start", arguments), object : MethodChannel.Result {
                override fun success(value: Any?) {
                    if (answered) return
                    if (disposed || trackState !== state || !ownership.owns(next)) { failed("session_replaced", null); return }
                    val texture = (value as? Number)?.toLong()
                    if (texture == null || texture < 0) { failed("surface_failed", null); return }
                    lane.texture = texture
                    if (--remaining == 0) {
                        answered = true
                        result.success(state.lanes.associate { it.track.id to it.texture })
                    }
                }
                override fun error(code: String, message: String?, details: Any?) = failed(code, message)
                override fun notImplemented() = failed("decoder_failed", null)
            })
        }
    }

    private fun trackStats(state: TrackState, result: MethodChannel.Result) {
        val readings = LinkedHashMap<String, Map<String, Any?>>()
        for (lane in state.lanes) lane.decoder.worker.post {
            val reading = lane.decoder.stats()
            main.post {
                readings[lane.track.id] = reading
                if (readings.size != state.lanes.size) return@post
                if (disposed || trackState !== state || !ownership.owns(state.lease)) { result.error("session_replaced", null, null); return@post }
                val primary = state.lanes.first { it.track.primary }
                val value = HashMap(readings.getValue(primary.track.id))
                for (key in listOf("decodedFrames", "textureFrames", "decodeCalls", "queuedPackets", "queuedBytes", "skippedDecodedFrames")) {
                    value[key] = readings.values.sumOf { (it[key] as? Number)?.toLong() ?: 0L }
                }
                for (key in listOf("decodeTotalMs", "decodeBusyMs")) value[key] = readings.values.sumOf { (it[key] as? Number)?.toDouble() ?: 0.0 }
                for (key in listOf("queueOldestMs", "decodeMaxMs", "presentQueueOldestMs")) value[key] = readings.values.mapNotNull { (it[key] as? Number)?.toDouble() }.maxOrNull()
                value["hardwareDecoder"] = when {
                    readings.values.any { it["hardwareDecoder"] == false } -> false
                    readings.values.all { it["hardwareDecoder"] == true } -> true
                    else -> null
                }
                value["error"] = readings.values.minOf { (it["error"] as? Number)?.toInt() ?: 0 }
                value["mediaProtocol"] = "tracks-v1"; value["topologyEpoch"] = state.configuration.topology
                value.putAll(environmentStats())
                value["tracksReady"] = readings.values.all { ((it["decodedFrames"] as? Number)?.toLong() ?: 0L) > 0L && it["error"] == 0 }
                value["tracks"] = state.lanes.map { output ->
                    HashMap(readings.getValue(output.track.id)).apply {
                        put("id", output.track.id); put("epoch", output.track.epoch)
                        put("width", output.track.sourceWidth); put("height", output.track.sourceHeight)
                        put("encodedWidth", output.track.width); put("encodedHeight", output.track.height)
                        put("textureId", output.texture)
                    }
                }
                result.success(value)
            }
        }
    }

    private fun beginStart(call: MethodCall, result: MethodChannel.Result) {
        if (disposed) { result.error("engine_closed", null, null); return }
        if (sizeStartPending) { result.error("media_operation_pending", null, null); return }
        val exact = call.argument<Boolean>("streamSizeV2") == true
        if (exact && call.argument<Any>("fps") == null) { result.error("invalid_geometry", null, null); return }
        val track = mutableMapOf<String, Any?>("width" to call.argument<Any>("width"), "height" to call.argument<Any>("height"),
            "fps" to (call.argument<Any>("fps") ?: 30))
        if (call.hasArgument("sourceWidth") || call.hasArgument("sourceHeight")) {
            track["sourceWidth"] = call.argument<Any>("sourceWidth"); track["sourceHeight"] = call.argument<Any>("sourceHeight")
        }
        val request = streamSizeRequest(mapOf("schema" to 1,
            "tracks" to listOf(track), "hardware" to (call.argument<Any>("hardwareDecode") ?: true)))
        val next = when (val raw = call.argument<Any>("lease")) { is Int -> raw.toLong(); is Long -> raw; else -> 0L }
        if (request == null || next <= 0) { result.error("invalid_geometry", null, null); return }
        if (!exact && !legacyStreamSize(request.tracks.single())) { result.error("invalid_geometry", null, null); return }
        if (!ownership.canBegin(next)) { result.error("session_replaced", null, null); return }
        closeTracks()
        if (call.argument<Boolean>("keepCapture") != true) releaseInput()
        // First: after this no UDP message reaches the decoder this start replaces.
        gate.attach(0, null)
        // Before the lease is owned, so its first packet is already checked as its format.
        opusLease = if (call.argument<Any>("audioFormat") == "opus") next else 0L
        check(ownership.begin(next)); sizeObservation.changed()
        sizeStartPending = true
        val observed = sizeObservation.snapshot()
        // Reserve the real lease BEFORE asynchronous metadata work so stop or
        // replacement can retire it; a late query cannot create a new surface.
        val posted = worker.post {
            val supported = try {
                if (exact) AvcStreamSizes.validate(request, activity)["accepted"] == true
                else streamSizeMemoryBytes(request.tracks)?.let { it <= AvcStreamSizes.memoryBudget(activity) } == true
            } catch (_: Exception) { false }
            main.post {
                sizeStartPending = false
                if (disposed || !ownership.owns(next) || !sizeObservation.current(observed)) {
                    result.error("session_replaced", null, null)
                } else if (!supported) {
                    releaseInput(); ownership.end(next); sizeObservation.changed()
                    activity.window.clearFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON); preferStreamRefresh(false)
                    worker.post { if (lease == 0L) closeEngine() }
                    result.error("decoder_size_unsupported", null, null)
                } else createSurface(call, result, request, next, exact)
            }
        }
        if (!posted) { sizeStartPending = false; releaseInput(); ownership.end(next); sizeObservation.changed(); result.error("engine_closed", null, null) }
    }

    private fun createSurface(call: MethodCall, result: MethodChannel.Result, request: StreamSizeRequest, next: Long, exact: Boolean) {
        val track = request.tracks.single()
        val w = track.width; val h = track.height
        val entry: TextureRegistry.SurfaceTextureEntry
        val nextSurface: Surface
        try {
            entry = textures.createSurfaceTexture()
            try { entry.surfaceTexture().setDefaultBufferSize(w, h); nextSurface = Surface(entry.surfaceTexture()) }
            catch (failure: Exception) { entry.release(); throw failure }
        } catch (_: Exception) {
            releaseInput(); ownership.end(next); sizeObservation.changed()
            activity.window.clearFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON); preferStreamRefresh(false)
            worker.post { if (lease == 0L) closeEngine() }
            result.error("surface_failed", null, null); return
        }
        entry.setOnFrameConsumedListener { val at = System.nanoTime(); worker.post { taken(entry, at) } }
        val nextAudioEnabled = call.argument<Boolean>("audioEnabled") == true
        val nextAudioOpus = call.argument<Any>("audioFormat") == "opus"
        activity.window.addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON)
        display = currentDisplay(); preferStreamRefresh(true)
        val posted = worker.post {
            if (disposed || lease != next) {
                nextSurface.release(); main.post { entry.release(); result.error("session_replaced", null, null) }; return@post
            }
            closeEngine(); audioEnabled = nextAudioEnabled; audioOpus = nextAudioOpus
            texture = entry; surface = nextSurface; width = w; height = h; targetFps = track.fps; allowHardware = request.hardware; exactSizes = exact
            decoded = 0; droppedPackets = 0
            error = 0; decodeCalls = 0; decodeMs = 0.0; decodeMaxMs = 0.0; clock.resetBusy()
            decodeRecent.reset(); arrivalRecent.reset(); presentRecent.reset()
            droppedBefore = synchronized(playout) { playout.counters.overrunFrames + playout.counters.trimmedFrames }
            underrunsBefore = synchronized(playout) { playout.counters.underruns }
            if (audioEnabled) audioOut.open()
            // Asked for, not made here: the start answer and the video decoder do not wait for it.
            if (audioOpus && audioOut.active) opus = AndroidOpusDecoder(worker, setup, ::opusFrame).also { it.prepare() }
            main.post {
                if (disposed || lease != next) result.error("session_replaced", null, null)
                else {
                    gate.attach(next) { message, video -> offer(next, video, video, message.size) { System.arraycopy(message, 0, it, 0, message.size) } }
                    result.success(entry.id())
                }
            }
            worker.post { if (lease == next && texture === entry && decoder == null) prepareDecoder(next) }
        }
        if (!posted) {
            nextSurface.release(); entry.release(); releaseInput(); ownership.end(next); sizeObservation.changed()
            result.error("engine_closed", null, null)
        }
    }

    /** The media stream's volume now and its top (null where it cannot be read). */
    private fun mediaVolume(): Int? = try { audioManager?.getStreamVolume(AudioManager.STREAM_MUSIC) } catch (_: Exception) { null }
    private fun mediaVolumeMax(): Int? = try { audioManager?.getStreamMaxVolume(AudioManager.STREAM_MUSIC)?.takeIf { it > 0 } } catch (_: Exception) { null }

    /** Worker. Statistics of the current decoder (fixed fields, no content). */
    private fun stats(): Map<String, Any?> {
        // A feeding thread that ended anyway is started again (AudioThreadKeeper's rate).
        if (audioEnabled) audioOut.keepRunning()
        val track = audioOut.track
        val played = try { (track?.playbackHeadPosition?.toLong() ?: 0L) and 0xffffffffL } catch (_: Exception) { 0L }
        val oldest = pending.firstOrNull()?.accepted
        val now = System.nanoTime()
        // Without frame-consumed callbacks (the pacer stopped holding before
        // Flutter took any frame), rendered frames stand in for taken ones.
        val textureFrames = if (pacer.taken > 0 || pacer.gated) pacer.taken else pacer.rendered
        val trackMs = if (track == null) 0.0 else (audioOut.trackWritten - played).coerceAtLeast(0) / 48.0
        // One consistent reading of the jitter buffer (the audio thread changes it every 5 ms).
        val sound = synchronized(playout) {
            val c = playout.counters
            longArrayOf(c.underruns, c.overruns, c.trimmedFrames, c.insertedFrames, c.overrunFrames + c.trimmedFrames,
                c.concealedFrames, playout.level.toLong(), playout.target.toLong(), if (playout.playing) 1 else 0)
        }
        val playing = audioEnabled && sound[8] == 1L
        val depthMs = if (playing) sound[6] / 48.0 else 0.0
        val decoder = opus
        val audioError = audioOut.error
        return mapOf(
            "decodedFrames" to decoded,
            "textureFrames" to textureFrames,
            // texture-pacing-v1 counterparts: decoded frames never shown
            // (replaced by a newer one), frames that waited behind one Flutter
            // had not taken, decoded -> taken by Flutter, GPU surface texture.
            "skippedDecodedFrames" to pacer.skipped,
            "textureQueued" to pacer.queued,
            "textureWaitTotalMs" to pacer.waitTotalNanos / 1e6,
            "textureWaitMaxMs" to pacer.waitMaxNanos / 1e6,
            "presentQueuePackets" to pacer.presentQueuePackets,
            "presentQueueOldestMs" to pacer.presentQueueOldestNanos(now)?.let { it / 1e6 },
            "texturePacing" to pacer.gated,
            "texturePath" to TEXTURE_PATH_SURFACE,
            "displayHz" to displayHz(),
            // This stream: the current track's play head, frames written to its tracks, and frames received
            // and not played (overrun cuts and drift trims of the jitter buffer); what the track holds.
            "audioPlayedFrames" to played, "audioWrittenFrames" to audioOut.written,
            "audioDroppedFrames" to (sound[4] - droppedBefore).coerceAtLeast(0),
            "audioQueuedMs" to trackMs,
            // Frames the drift correction dropped, in 10 ms (the unit the former packet trim counted in).
            "audioTrimmed" to sound[2] / 480,
            "audioTargetMs" to sound[7] / 48.0,
            // audio-stats-v1 (lib/data/audio_stats.dart), the Windows engine's keys and meanings
            // (AudioJitterBuffer is its AudioJitterBuffer): the level (jitter buffer + track) while sound
            // plays, underruns the sound went on through (the track's raw count also counts a silent VM),
            // backlogs cut to the target, frames trimmed / repeated by the drift correction, and
            // written -> presented by the track's own timestamp. No A/V offset estimate yet.
            "audioEnabled" to audioEnabled,
            "audioPlaying" to playing,
            "audioDepthMs" to depthMs,
            "audioLatencyMs" to latencyMs(track),
            // The track's effective buffer (the most that can be queued, and before API 31 the level
            // it starts at) and whether Android gave it the fast mixer path it was asked for.
            "audioBufferMs" to audioBufferMs(track),
            "audioFastTrack" to audioFastTrack(track),
            "audioUnderruns" to sound[0],
            "audioTrackUnderruns" to audioOut.trackUnderruns(),
            "audioOverruns" to sound[1],
            "audioTrimmedFrames" to sound[2],
            "audioInsertedFrames" to sound[3],
            // An Opus stream without a working decoder has no sound, as one without a track.
            "audioError" to (if (decoder?.failed == true) -1 else audioError), "error" to error,
            // audio-opus-v1 streams, the app's total of 10 ms frames made up: for lost packets
            // (AndroidOpusStream: one mirrored fade-out, then silence) and fade-out tails where the
            // jitter buffer ran dry (counted by their 10 ms).
            "audioConcealed" to (if (audioOpus) concealedBefore + (decoder?.stream?.concealed ?: 0L) + sound[5] / 480 else null),
            // audio-path-v1, fixed numbers only: which decoder plays the sound (0 none: PCM or
            // no sound; 1 the platform's software Opus decoder, 2 another Opus decoder, 3 none could be used),
            // the format it stated, the track's start level and whether its buffer is the jitter target, tracks
            // made again (dead / written to and not playing), audio focus (AudioFocusPolicy.State ordinal:
            // 0 none, 1 held, 2 waiting, 3 paused, 4 lost) and whether it mutes the track, and the API level.
            "audioDecoder" to when {
                !audioOpus || !audioEnabled -> 0
                decoder == null || decoder.failed -> 3
                decoder.software == true -> 1
                decoder.software == false -> 2
                else -> 0
            },
            "audioOutRate" to decoder?.outputRate?.takeIf { it > 0 },
            "audioOutChannels" to decoder?.outputChannels?.takeIf { it > 0 },
            "audioOutFloat" to decoder?.takeIf { it.outputRate > 0 }?.outputFloat,
            "audioStartMs" to (if (track == null) null else audioOut.startLevel / 48.0),
            "audioBufferFollows" to (if (track == null) null else audioOut.follows),
            "audioTrackRestarts" to audioOut.restarts,
            "audioTrackStuck" to audioOut.stuck,
            "audioFocus" to audioFocus.stateCode,
            "audioMuted" to (audioFocus.volume == 0f),
            "audioOpusUnusable" to AndroidOpusDecoder.unusableThisRun,
            // Why (1 the platform lists no Opus decoder, 2 none it lists plays), and fresh Opus decoders this
            // stream asked for after failures (AndroidOpusPipe's rest).
            "audioOpusUnusableReason" to AndroidOpusDecoder.unusableReason,
            // The stream's sound is Opus (audioDecoder 0 then means its decoder is still being made, not PCM).
            "audioOpus" to (audioEnabled && audioOpus),
            "audioOpusRetries" to (decoder?.retries ?: 0L),
            // An ask for focus again left the sound muted (AndroidAudioFocus.blocked: a call, another app).
            "audioFocusBlocked" to audioFocus.blocked,
            // The audio thread: looks that threw (an Error too) and threads started again (AndroidAudioOut).
            "audioThreadErrors" to audioOut.threadErrors,
            "audioThreadRestarts" to audioOut.threadRestarts,
            // This stream's underruns (the sound went on through them), and the device's media volume.
            "audioStreamUnderruns" to (sound[0] - underrunsBefore).coerceAtLeast(0),
            "mediaVolume" to mediaVolume(),
            "mediaVolumeMax" to mediaVolumeMax(),
            "androidApi" to Build.VERSION.SDK_INT,
            // Decoder latency: queued to the codec -> its decoded output, per frame.
            "decodeCalls" to decodeCalls, "decodeTotalMs" to decodeMs, "decodeMaxMs" to decodeMaxMs,
            // Utilisation: wall time with an input inside the codec (the sum
            // of decodeTotalMs overlaps for a pipelined decoder).
            "decodeBusyMs" to clock.busyNanos / 1e6,
            // The last second with frames (RecentWindow; null before one): decode time, the packet's
            // arrival at the engine -> its decoded picture, and decoded -> taken by Flutter.
            "decodeRecentMs" to decodeRecent.meanMs(now), "decodeRecentMaxMs" to decodeRecent.maxMs(now),
            "frameArrivalMs" to arrivalRecent.meanMs(now), "frameArrivalMaxMs" to arrivalRecent.maxMs(now),
            "presentWaitMs" to presentRecent.meanMs(now), "presentWaitMaxMs" to presentRecent.maxMs(now),
            "queuedPackets" to queue.packets,
            "queuedBytes" to queue.bytes,
            "queueOldestMs" to (if (oldest == null) 0.0 else ((now - oldest) / 1e6).coerceAtLeast(0.0)),
            "droppedPackets" to droppedPackets,
            "decoder" to name,
            "decoderEarly" to early,
            "inputFrames" to frameWatch.take(),
            "hardwareDecoder" to hardware,
            // Low-latency configuration the decoder accepted (avcTuningFlags bits)
            // and the vendor key set its name matched (AvcVendorKeys code, 0 none).
            "decoderLowLatency" to lowLatency,
            "decoderVendorKeys" to vendorKeys,
            // AvcFailureMemory: this decoder runs the plain format after it failed tuned, and how many
            // decoders this app run moved to the plain format or to the end of the trial order.
            "decoderPlainFallback" to plainFallback,
            "decoderFallbacks" to AvcDecoders.fallbacks,
            // client-load-v1: this app's CPU in tenths of a percent of the whole
            // device (-1 unknown). Android hides the system-wide figure.
            "appCpuPct10" to appCpuTenths(),
        ) + environmentStats()
    }

    /**
     * Any thread. What the stream runs in rather than what it decodes: the
     * Wi-Fi lock (AndroidStreamWifi: 0 not held, 1 low latency, 2 high
     * performance) and how often this app run was refused one; the device's
     * thermal status now and the highest of this stream (AndroidThermal:
     * 0 none .. 6 shutdown, null below API 29). Empty for a track's own
     * decoder; its owner adds them.
     */
    private fun environmentStats(): Map<String, Any?> {
        val lock = wifi ?: return emptyMap()
        return mapOf("wifiLock" to lock.stat, "wifiLockRefused" to lock.refused,
            "thermalStatus" to thermal?.watch?.status, "thermalStatusMax" to thermal?.watch?.highest) +
            // 선명하게 (SharpPicture.kt): what was planned and how often the automatic size was used instead.
            SharpPictures.stats.values()
    }

    /** Worker. The track's effective buffer in ms of 48 kHz audio, null without a track. */
    private fun audioBufferMs(track: AudioTrack?): Double? = try { track?.bufferSizeInFrames?.let { it / 48.0 } } catch (_: Exception) { null }

    /** Worker. True when the track runs on the fast mixer path (PERFORMANCE_MODE_LOW_LATENCY was granted). */
    private fun audioFastTrack(track: AudioTrack?): Boolean? =
        try { track?.let { it.performanceMode == AudioTrack.PERFORMANCE_MODE_LOW_LATENCY } } catch (_: Exception) { null }

    private val timestamp = AudioTimestamp()

    /** Worker. Written -> presented (ms) by the track's timestamp, null where unknown. */
    private fun latencyMs(track: AudioTrack?): Double? = try {
        if (track != null && track.getTimestamp(timestamp)) {
            val frames = audioOut.trackWritten - timestamp.framePosition
            (frames / 48.0 - (System.nanoTime() - timestamp.nanoTime) / 1e6).takeIf { it in 0.0..60000.0 }
        } else null
    } catch (_: Exception) { null }

    private fun appCpuTenths(): Int {
        val ticks = try { parseSelfStatTicks(java.io.File("/proc/self/stat").readText()) } catch (_: Exception) { null }
        return appCpu.sample(ticks, System.nanoTime())
    }

    private fun displayHz(): Int? = try {
        display?.refreshRate?.roundToInt()?.takeIf { it in 1..1000 }
    } catch (_: Exception) { null }

    /** Worker. Every admitted packet ends here or in [done] exactly once. */
    private fun receive(packet: Packet) {
        if (lease != packet.lease || failedLease == packet.lease) { done(packet); return }
        if (!packet.video) {
            try { if (audioOpus) playOpus(packet) else play(packet) } catch (_: Exception) {}
            done(packet)
            return
        }
        try {
            if (decoder == null || awaitingKeyFrame) begin(packet)
        } catch (_: Exception) {
            done(packet)
            fail(packet.lease, byCodec = false)
            return
        }
        packet.nonReference = h264NonReference(packet.bytes, 12, packet.length)
        pending.addLast(packet)
        droppedPackets += dropStaleNonReference(pending, System.nanoTime(), { it.accepted }, { it.nonReference }, ::done)
        try { feed() } catch (_: Exception) { fail(packet.lease) }
    }

    /** Worker. Returns the packet's buffer and its queue share. */
    private fun done(packet: Packet) {
        queue.removed(packet.video, packet.length)
        pool.give(packet.bytes)
    }

    private fun play(packet: Packet) = playPcm(packet.bytes, 12, packet.length - 12, androidMediaPts(packet.bytes))

    /** audio-opus-v1: the packet goes to the decoder; its PCM comes back through [opusFrame]. */
    private fun playOpus(packet: Packet) {
        if (!audioEnabled || !audioOut.active) return
        opus?.packet(packet.bytes, 12, packet.length - 12, androidMediaPts(packet.bytes), packet.accepted)
    }

    /** Worker, from the Opus decoder's output: decoded or concealed 10 ms, in play order. */
    @Suppress("UNUSED_PARAMETER")
    private fun opusFrame(pcm: ByteArray, bytes: Int, pts: Long, accepted: Long) {
        try { playPcm(pcm, 0, bytes, pts) } catch (_: Exception) {}
    }

    /**
     * Worker. 48 kHz s16 stereo PCM ([bytes] at [offset]) with Host PTS [pts]:
     * a PCM packet's payload, or an Opus frame. It waits in [playout] until the
     * audio thread (AndroidAudioOut) hands it to the track.
     */
    private fun playPcm(pcm: ByteArray, offset: Int, bytes: Int, pts: Long) {
        if (!audioEnabled || !audioOut.active) return
        val now = System.nanoTime() / 1000
        synchronized(playout) { playout.push(pts, pcm, offset, bytes, now) }
    }

    /** Worker, after the start answer: the decoder before its key frame arrives. */
    private fun prepareDecoder(owner: Long) {
        val output = surface ?: return
        val handlers = Callbacks(owner)
        val started = try {
            AvcDecoders.start(width, height, null, null, output, handlers, worker, targetFps, allowHardware, AvcStreamSizes.memoryBudget(activity), exactSizes)
        } catch (_: Exception) { null }
        // Otherwise the key frame starts one with its parameter sets in the format.
        if (started == null) return
        adopt(started, handlers, early = true)
        awaitingKeyFrame = true
    }

    /** Worker. The first video packet of a decoder: a key frame with its parameter sets. */
    private fun begin(packet: Packet) {
        require(packet.key)
        val units = annexBUnits(packet.bytes, 12, packet.length)
        val sps = units.firstOrNull { it.size > 4 && it[4].toInt() and 31 == 7 }
            ?: throw IllegalArgumentException("Missing SPS")
        val pps = units.firstOrNull { it.size > 4 && it[4].toInt() and 31 == 8 }
            ?: throw IllegalArgumentException("Missing PPS")
        if (decoder != null) {
            // Started early: the parameter sets go first, as codec config.
            parameterSets = sps + pps
            awaitingKeyFrame = false
            return
        }
        val output = surface ?: throw IllegalStateException("No surface")
        val handlers = Callbacks(packet.lease)
        val started = AvcDecoders.start(width, height, sps, pps, output, handlers, worker, targetFps, allowHardware, AvcStreamSizes.memoryBudget(activity), exactSizes)
            ?: throw IllegalStateException("No H.264 decoder")
        adopt(started, handlers, early = false)
    }

    private fun adopt(started: AvcStarted, handlers: Callbacks, early: Boolean) {
        val codec = started.codec
        decoder = codec
        callbacks = handlers
        this.early = early
        name = try { codec.name } catch (_: Exception) { "" }
        hardware = AvcDecoders.hardware(codec)
        lowLatency = started.lowLatency
        vendorKeys = started.vendorKeys
        plainFallback = started.plainFallback
        firstFrame = true
        pacer = AndroidFramePacer().also { if (hidden) it.background() }
        clock.clear()
        inputs.clear()
    }

    /** Worker. Queues waiting packets into the input buffers the decoder has freed. */
    private fun feed() {
        val codec = decoder ?: return
        if (awaitingKeyFrame) return
        while (inputs.isNotEmpty()) {
            val config = parameterSets
            if (config != null) {
                val index = inputs.removeFirst()
                val input = codec.getInputBuffer(index) ?: throw IllegalStateException()
                input.clear(); input.put(config)
                codec.queueInputBuffer(index, 0, config.size, 0, MediaCodec.BUFFER_FLAG_CODEC_CONFIG)
                parameterSets = null
                continue
            }
            val packet = pending.removeFirstOrNull() ?: return
            val index = inputs.removeFirst()
            try {
                val size = packet.length - 12
                val input = codec.getInputBuffer(index) ?: throw IllegalStateException()
                check(input.capacity() >= size)
                input.clear(); input.put(packet.bytes, 12, size)
                val pts = androidMediaPts(packet.bytes)
                codec.queueInputBuffer(index, 0, size, pts, if (packet.key) MediaCodec.BUFFER_FLAG_KEY_FRAME else 0)
                clock.queued(pts, System.nanoTime(), packet.accepted)
                // input-latency-v1 measures from the packet's acceptance, as
                // the Windows engine measures from its queue entry.
                if (packet.watch) frameWatch.watch(pts, packet.accepted)
            } finally {
                done(packet)
            }
        }
    }

    private inner class Callbacks(private val owner: Long) : MediaCodec.Callback() {
        override fun onInputBufferAvailable(codec: MediaCodec, index: Int) {
            if (codec !== decoder || callbacks !== this) return
            inputs.addLast(index)
            try { feed() } catch (_: Exception) { fail(owner) }
        }

        override fun onOutputBufferAvailable(codec: MediaCodec, index: Int, info: MediaCodec.BufferInfo) {
            if (codec !== decoder || callbacks !== this) return
            try { output(codec, index, info, owner) } catch (_: Exception) { fail(owner) }
        }

        override fun onOutputFormatChanged(codec: MediaCodec, format: MediaFormat) {
            if (codec !== decoder || callbacks !== this) return
            try {
                val outputWidth = if (format.containsKey("crop-right")) format.getInteger("crop-right") + 1 - format.getInteger("crop-left") else format.getInteger(MediaFormat.KEY_WIDTH)
                val outputHeight = if (format.containsKey("crop-bottom")) format.getInteger("crop-bottom") + 1 - format.getInteger("crop-top") else format.getInteger(MediaFormat.KEY_HEIGHT)
                if (outputWidth != width || outputHeight != height) fail(owner)
            } catch (_: Exception) { fail(owner) }
        }

        override fun onError(codec: MediaCodec, e: MediaCodec.CodecException) {
            if (codec === decoder && callbacks === this) fail(owner)
        }
    }

    private fun output(codec: MediaCodec, index: Int, info: MediaCodec.BufferInfo, owner: Long) {
        if (info.flags and MediaCodec.BUFFER_FLAG_CODEC_CONFIG != 0 || lease != owner) {
            codec.releaseOutputBuffer(index, false)
            return
        }
        val now = System.nanoTime()
        decoded++
        val latency = clock.decoded(info.presentationTimeUs, now)
        if (latency >= 0) {
            val ms = latency / 1e6
            decodeCalls++; decodeMs += ms; if (ms > decodeMaxMs) decodeMaxMs = ms
            decodeRecent.add(latency, now)
            val arrival = clock.lastArrival
            if (arrival >= 0) arrivalRecent.add(now - arrival, now)
        }
        // input-latency-v1: the frame is now handed to the texture's pacing,
        // as the Windows engine publishes a decoded frame.
        frameWatch.rendered(info.presentationTimeUs, now)
        if (firstFrame) {
            firstFrame = false
            // Older textures are no longer needed once this one has a frame
            // (Dart switches when the resize that created it has its first
            // decoded frame).
            main.postDelayed({ if (!disposed) releaseRetired() }, RETIRED_GRACE_MS)
        }
        pacer.decoded(index, now, frames)
        scheduleCheck()
    }

    /** Worker. Flutter took a frame from [entry] at [at]. */
    private fun taken(entry: TextureRegistry.SurfaceTextureEntry, at: Long) {
        if (texture !== entry || decoder == null) return
        val before = pacer.taken
        val waited = pacer.waitTotalNanos
        try { pacer.consumed(at, frames) } catch (_: Exception) { fail(lease) }
        if (pacer.taken > before) presentRecent.add(pacer.waitTotalNanos - waited, at)
    }

    private fun scheduleCheck() {
        val due = pacer.checkAt()
        if (due < 0 || checkPosted) return
        checkPosted = true
        worker.postDelayed(pacingCheck, ((due - System.nanoTime()) / 1_000_000L).coerceAtLeast(1L))
    }

    /**
     * Worker. The decoder of [owner] failed: its next packet is answered
     * decoder_failed. [byCodec] false: the stream itself was unusable (no
     * key frame or parameter sets), which says nothing about the decoder.
     */
    private fun fail(owner: Long, byCodec: Boolean = true) {
        if (owner <= 0) return
        val codec = decoder
        if (byCodec && owner == lease && codec != null && firstFrame && name.isNotEmpty()) {
            AvcDecoders.failedBeforeFirstFrame(name, early, lowLatency)
            if (early && awaitingKeyFrame) {
                // Started early and failed before any input: the key frame
                // starts a decoder with its parameter sets in the format.
                decoder = null
                callbacks = null
                inputs.clear()
                awaitingKeyFrame = false
                pacer.reset()
                try { codec.stop() } catch (_: Exception) {}
                try { codec.release() } catch (_: Exception) {}
                return
            }
        }
        error = -1
        failedLease = owner
    }

    /** Main thread. */
    private fun releaseRetired() {
        for (entry in retired) try { entry.release() } catch (_: Exception) {}
        retired.clear()
    }

    private fun closeEngine() {
        worker.removeCallbacks(pacingCheck)
        checkPosted = false
        pacer.reset()
        while (pending.isNotEmpty()) done(pending.removeFirst())
        inputs.clear()
        parameterSets = null
        awaitingKeyFrame = false
        val codec = decoder
        decoder = null
        callbacks = null
        try { codec?.stop() } catch (_: Exception) {}
        try { codec?.release() } catch (_: Exception) {}
        clock.clear()
        frameWatch.clear()
        opus?.let { concealedBefore += it.stream.concealed; it.close() }; opus = null
        if (audioOut.active) main.post { audioFocus.release() }
        audioOut.close()
        surface?.release(); surface = null
        val old = texture; texture = null
        if (old != null) main.post {
            if (disposed) { try { old.release() } catch (_: Exception) {}; return@post }
            // Its last frame stays visible until a newer texture has one.
            retired.add(old)
            if (retired.size > 4) { try { retired.removeAt(0).release() } catch (_: Exception) {} }
            main.postDelayed({ if (retired.remove(old)) try { old.release() } catch (_: Exception) {} }, RETIRED_TEXTURE_MS)
        }
    }

    private fun currentDisplay(): Display? = try {
        if (Build.VERSION.SDK_INT >= 30) activity.display
        else @Suppress("DEPRECATION") activity.windowManager.defaultDisplay
    } catch (_: Exception) { null }

    // Main thread: what the refresh request last applied (see refreshChanged).
    private var streamingRefresh = false
    private var appliedMode: Int? = null
    private var appliedSurfaces: Int? = null
    private var displayListener: DisplayManager.DisplayListener? = null
    private val hookedSurfaces = java.util.WeakHashMap<SurfaceView, Boolean>()

    /**
     * Main thread. While a stream is shown, the panel's highest 60/120 Hz mode
     * at the same resolution is held as the preferred display mode
     * (androidStreamHoldMode, also when the display is already in it) and the
     * Flutter surface declares 60 fps with FRAME_RATE_COMPATIBILITY_FIXED_SOURCE
     * (Surface.setFrameRate, API 30+), so an adaptive-refresh panel (Samsung)
     * does not drop to 30 Hz for what it takes to be static content. The
     * request is applied again by [refreshChanged]; the previous preference and
     * frame rate return when the stream ends.
     */
    private fun preferStreamRefresh(streaming: Boolean) {
        if (!registerChannels) return
        // With the display request: the stream's Wi-Fi lock is held while it runs and is shown.
        wifi?.streaming(streaming)
        thermal?.streaming(streaming)
        try {
            val window = activity.window
            val attributes = window.attributes
            streamingRefresh = streaming
            if (!streaming) {
                stopDisplayListener()
                applySurfaceFrameRate(null)
                appliedMode = null; appliedSurfaces = null
                val previous = replacedMode ?: return
                replacedMode = null
                attributes.preferredDisplayModeId = previous
                window.attributes = attributes
                return
            }
            startDisplayListener()
            val shown = display ?: return
            val mode = shown.mode
            val current = AndroidDisplayMode(mode.modeId, mode.physicalWidth, mode.physicalHeight, mode.refreshRate)
            val modes = shown.supportedModes.map { AndroidDisplayMode(it.modeId, it.physicalWidth, it.physicalHeight, it.refreshRate) }
            val wanted = androidStreamHoldMode(current, modes)
            if (wanted != null && attributes.preferredDisplayModeId != wanted) {
                if (replacedMode == null) replacedMode = attributes.preferredDisplayModeId
                attributes.preferredDisplayModeId = wanted
                window.attributes = attributes
            }
            appliedMode = wanted
            applySurfaceFrameRate(androidStreamFrameRate(modes, current))
        } catch (_: Exception) {}
    }

    /**
     * Main thread: fold, unfold, rotation, a window focus change or a display
     * change. The display and the Flutter surface may be different ones now, so
     * the request is made again when it no longer holds
     * (androidRefreshNeedsReapply).
     */
    fun refreshChanged() {
        if (disposed || !streamingRefresh) return
        try {
            display = currentDisplay()
            val shown = display ?: return
            val mode = shown.mode
            val current = AndroidDisplayMode(mode.modeId, mode.physicalWidth, mode.physicalHeight, mode.refreshRate)
            val modes = shown.supportedModes.map { AndroidDisplayMode(it.modeId, it.physicalWidth, it.physicalHeight, it.refreshRate) }
            val wanted = androidStreamHoldMode(current, modes)
            val surfaces = surfaceViews().map { System.identityHashCode(it.holder.surface) }.fold(0) { a, b -> a * 31 + b }
            if (androidRefreshNeedsReapply(appliedMode, appliedSurfaces, wanted, surfaces, activity.window.attributes.preferredDisplayModeId)) {
                preferStreamRefresh(true)
            }
        } catch (_: Exception) {}
    }

    private fun startDisplayListener() {
        if (displayListener != null) return
        try {
            val manager = activity.getSystemService(Context.DISPLAY_SERVICE) as DisplayManager
            val listener = object : DisplayManager.DisplayListener {
                override fun onDisplayAdded(displayId: Int) { refreshChanged() }
                override fun onDisplayRemoved(displayId: Int) { refreshChanged() }
                override fun onDisplayChanged(displayId: Int) { refreshChanged() }
            }
            manager.registerDisplayListener(listener, main)
            displayListener = listener
        } catch (_: Exception) {}
    }

    private fun stopDisplayListener() {
        val listener = displayListener ?: return
        displayListener = null
        try { (activity.getSystemService(Context.DISPLAY_SERVICE) as DisplayManager).unregisterDisplayListener(listener) } catch (_: Exception) {}
    }

    private fun surfaceViews(): List<SurfaceView> {
        val found = ArrayList<SurfaceView>()
        fun walk(view: View) {
            if (view is SurfaceView) found.add(view)
            if (view is ViewGroup) for (index in 0 until view.childCount) walk(view.getChildAt(index))
        }
        walk(activity.window.decorView)
        return found
    }

    /** Main thread. [rate] null clears the declaration (stream ended). */
    private fun applySurfaceFrameRate(rate: Float?) {
        if (Build.VERSION.SDK_INT < 30) return
        val views = surfaceViews()
        for (view in views) {
            try {
                val surface = view.holder.surface
                if (surface.isValid) {
                    if (rate != null) surface.setFrameRate(rate, Surface.FRAME_RATE_COMPATIBILITY_FIXED_SOURCE)
                    else surface.setFrameRate(0f, Surface.FRAME_RATE_COMPATIBILITY_DEFAULT)
                }
                if (rate != null && hookedSurfaces.put(view, true) == null) {
                    view.holder.addCallback(object : SurfaceHolder.Callback {
                        override fun surfaceCreated(holder: SurfaceHolder) { if (streamingRefresh) main.post { preferStreamRefresh(true) } }
                        override fun surfaceChanged(holder: SurfaceHolder, format: Int, width: Int, height: Int) {}
                        override fun surfaceDestroyed(holder: SurfaceHolder) {}
                    })
                }
            } catch (_: Exception) {}
        }
        appliedSurfaces = views.map { System.identityHashCode(it.holder.surface) }.fold(0) { a, b -> a * 31 + b }
    }
    fun releaseInput() {
        if (!captured) return
        sendKeys(keyMap.release())
        captured = false
        sink?.success(mapOf("type" to "capture.released", "lease" to lease))
    }

    /** 게임 마우스's moveRelative path (AndroidGameMouse): one position-less input, in order with the keys, while input is captured. */
    fun pointerInput(event: Map<String, Any>) {
        if (!captured || lease <= 0) return
        sink?.success(event + ("lease" to lease))
    }

    fun key(event: KeyEvent): Boolean {
        if (!captured || lease <= 0 || event.action !in listOf(KeyEvent.ACTION_DOWN, KeyEvent.ACTION_UP)) return false
        val code = event.keyCode
        if (code == KeyEvent.KEYCODE_F12 && event.isCtrlPressed && event.isAltPressed && event.isShiftPressed) {
            releaseInput()
            return true
        }
        val keys = keyMap.map(code, event.action == KeyEvent.ACTION_DOWN, event.metaState) ?: return false
        sendKeys(keys)
        return true
    }

    private fun sendKeys(keys: List<AndroidKeyMap.Key>) {
        for (key in keys) sink?.success(mapOf("lease" to lease, "type" to "input", "event" to "key", "vk" to key.vk,
            "scan" to 0, "down" to key.down, "extended" to key.extended))
    }

    fun dispose() {
        closeTracks()
        gate.attach(0, null)
        releaseInput(); disposed = true; ownership.clear(); sizeObservation.changed(); sink = null
        if (registerChannels) messenger.setMessageHandler(MEDIA_CHANNEL, null)
        preferStreamRefresh(false)
        releaseRetired()
        // The setup thread first releases what closeEngine handed it.
        worker.post { closeEngine(); setupThread.quitSafely(); thread.quitSafely() }
    }

    companion object {
        /** media-wire-v1 packets from the Dart session (AndroidRemoteMedia). */
        const val MEDIA_CHANNEL = "multitap/remote/media"
        /** Packets accepted ahead of the decoder (Dart waits when it is full). */
        private const val MAX_QUEUED_PACKETS = 8
        /** Audio's own share: 320 ms of 10 ms chunks, none of them a video slot. */
        private const val MAX_QUEUED_AUDIO_PACKETS = 32
        private const val MAX_QUEUED_BYTES = 16L * 1024 * 1024
        /** Older textures stay this long after a newer one shows its first frame. */
        private const val RETIRED_GRACE_MS = 600L
        /**
         * ...and at most this long without one: a resize allows 15 s, and a
         * quiet reconnect keeps showing the last picture for up to 90 s.
         */
        private const val RETIRED_TEXTURE_MS = 120000L
        /** stream-stats texturePath: 1 = GPU surface texture (0 is the Windows CPU pixel buffer). */
        private const val TEXTURE_PATH_SURFACE = 1
    }
}
