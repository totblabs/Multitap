package me.totb.multitap_client

import kotlin.math.PI
import kotlin.math.cos
import kotlin.math.max
import kotlin.math.min
import kotlin.math.roundToInt

// The Android client's audio playout, its pure parts (no Android types), so
// the JVM tests (app/src/test AudioJitterBufferTest, AudioPlayoutTest) run
// this production code. AndroidAudioOut.kt feeds an AudioTrack from it.

/**
 * [AudioJitterBuffer]'s settings, in 48 kHz frames and microseconds. The
 * defaults are the Windows client's (windows/runner/audio_jitter.h, Config);
 * [ANDROID] is what the Android engine plays with.
 */
internal data class AudioJitterConfig(
    val minTarget: Int = AudioJitterBuffer.ms(30),
    val startTarget: Int = AudioJitterBuffer.ms(40),
    val maxTarget: Int = AudioJitterBuffer.ms(200),
    val grow: Int = AudioJitterBuffer.ms(20),
    val shrink: Int = AudioJitterBuffer.ms(5),
    val headroom: Int = AudioJitterBuffer.ms(40),
    val trimBand: Int = AudioJitterBuffer.ms(3),
    val insertBand: Int = AudioJitterBuffer.ms(5),
    val calmUs: Long = 10_000_000L,
    val smoothUs: Long = 2_000_000L,
    val gapUs: Long = 50_000L,
    val correctEvery: Int = 200,
    val concealBelow: Int = AudioJitterBuffer.ms(15),
    /**
     * Android: frames of the fade-out tail made when the buffer runs dry
     * while playing (0: none, as the Windows client's PCM path).
     */
    val tailFrames: Int = 0,
    /** Android: fade-in after silence or a tail, and crossfade over an overrun cut. */
    val fadeFrames: Int = 120,
) {
    companion object {
        /**
         * The Android engine. The floor is 40 ms rather than 30: the Host
         * sends its 10 ms frames in a 1-2-1-2 cadence about every 15.6 ms,
         * the track is looked at every 5 ms (AndroidAudioOut), and a track
         * on the normal mixer path is read in periods of up to ~20 ms; at
         * 30 ms the level between arrivals came within the starving band.
         * A dry device gets one 10 ms fade-out tail (no Opus PLC on
         * Android; AndroidOpusStream says why).
         */
        val ANDROID = AudioJitterConfig(minTarget = AudioJitterBuffer.ms(40), tailFrames = AudioJitterBuffer.ms(10))
    }
}

/**
 * Received 48 kHz stereo s16 PCM and when to hand it to the track: a port of
 * the Windows client's AudioJitterBuffer (windows/runner/audio_jitter.h; its
 * rules are written there and hold here unchanged), not thread-safe (its
 * owner locks it). The level is what waits here plus what the track still
 * holds; the target is the level aimed for.
 *
 * Android additions, each inert with the Windows defaults and the
 * arguments the Windows client has:
 *  - [pull] takes the track's own underrun count. A rise while the track
 *    still holds audio (a normal-path mixer skips a period it cannot fill
 *    whole: a gap the level alone does not show) is an underrun and grows
 *    the target when audio was waiting here; when nothing more had come, the
 *    next audio's PTS says whether the sound went on (an underrun) or paused
 *    (a silence, primed again), as for a track that ran dry.
 *  - [AudioJitterConfig.tailFrames], on a track read in small steps
 *    ([tailsAllowed]): when the level falls under concealBelow while
 *    playing, one tail is queued: the newest received
 *    audio mirrored in time (it goes on from the last sample with no edge)
 *    and faded to silence. Never more than one per dry spell, so no frame is
 *    ever repeated twice. Audio that then comes before the track ran dry is
 *    late when it goes on from the received sound (the target grows, as
 *    NoteLate) and new sound after a pause otherwise (primed again, as
 *    NotePause).
 *  - The first received frames after silence or a tail fade in over
 *    [AudioJitterConfig.fadeFrames]; an overrun cut while playing crossfades
 *    from the audio it dropped into the audio it kept instead of an edge.
 *  - [restartDevice] (a new track: primed again, nothing counted),
 *    [raiseFloor] (the level the track waits for before it plays) and
 *    [followDevice] (a track whose buffer is the target).
 */
internal class AudioJitterBuffer(private val config: AudioJitterConfig = AudioJitterConfig()) {
    companion object {
        const val RATE = 48_000
        const val FRAME_BYTES = 4
        /** 200 ms; a larger packet is malformed. */
        const val MAX_PACKET_BYTES = 38_400
        fun ms(ms: Int) = ms * (RATE / 1000)
        fun valid(size: Int) = size > 0 && size % FRAME_BYTES == 0 && size <= MAX_PACKET_BYTES
        fun framesUs(frames: Long) = frames * 1_000_000L / RATE
        private const val SPARE_BYTES = 1920
        private const val MAX_SPARE = 64
    }

    class Counters {
        var underruns = 0L; var silences = 0L; var overruns = 0L; var overrunFrames = 0L
        var insertedFrames = 0L; var trimmedFrames = 0L; var writtenFrames = 0L
        var concealedFrames = 0L; var lateConcealments = 0L
        /** Android: underruns seen only by the track's own count (included in [underruns]). */
        var trackUnderruns = 0L
        /** Android: fade-out tails queued (their frames are in [concealedFrames]). */
        var tails = 0L
    }

    private class Chunk(val ptsUs: Long, val bytes: ByteArray, val frames: Int, val concealed: Boolean)

    val counters = Counters()
    private val chunks = ArrayDeque<Chunk>()
    private val spare = ArrayDeque<ByteArray>()
    private var queued = 0
    private var front = 0
    private var device = 0
    var target = config.startTarget
        private set
    private var floor = 0
    private var sinceCorrection = 0
    private var endedPts = 0L
    private var continuedPts = 0L
    private var pulledAt = 0L
    private var calmSince = 0L
    private var smoothedAt = 0L
    private var smoothed = 0.0
    private var correction = 0
    var playing = false
        private set
    private var awaitingResume = false
    private var started = false
    private var haveLast = false
    private var concealing = false
    private var lateNoted = false
    private val last = ByteArray(FRAME_BYTES)
    private var lastTrackUnderruns = -1L
    private var stalled = false
    /**
     * Android: fade-out tails may be made (the track is read in small steps,
     * the fast mixer path). A track on the normal mixer path is read a whole
     * period (~20 ms) at a time, so its level dips under concealBelow after
     * every read without running dry: a tail there would be a blip in sound
     * that had no gap.
     */
    var tailsAllowed = true
    // The newest received frames (for the tail) and where the received sound ends.
    private val history = ByteArray(max(config.tailFrames, 1) * FRAME_BYTES)
    private var historyFrames = 0
    private var receivedEndPts = 0L
    // Fade-in owed to the next received frames; crossfade source of an overrun cut.
    private var fadePending = true
    private var fadeDone = 0
    private val cross = ByteArray(max(config.fadeFrames, 1) * FRAME_BYTES)
    private var crossFrames = 0
    private var crossDone = 0

    /** A received packet: its Host capture-clock PTS and this device's time (µs). */
    fun push(ptsUs: Long, bytes: ByteArray, offset: Int, size: Int, nowUs: Long): Boolean {
        if (!valid(size) || offset < 0 || offset + size > bytes.size) return false
        val frames = size / FRAME_BYTES
        if (stalled) {
            // The track went silent with some audio still in it (less than its mixer reads at once).
            stalled = false; concealing = false
            if (ptsUs > continuedPts + config.gapUs) { counters.silences++; playing = false; fadePending = true }
            else { counters.underruns++; counters.trackUnderruns++; grow(nowUs) }
        }
        // A tail is still playing: the audio it stood in for came late, or new sound after a pause.
        if (concealing && playing && !awaitingResume) {
            if (ptsUs > continuedPts + config.gapUs) notePause() else noteLate(nowUs)
        }
        if (awaitingResume) { awaitingResume = false; classify(ptsUs, nowUs) }
        concealing = false
        val copy = if (size == SPARE_BYTES) spare.removeLastOrNull() ?: ByteArray(size) else ByteArray(size)
        System.arraycopy(bytes, offset, copy, 0, size)
        chunks.addLast(Chunk(ptsUs, copy, frames, false))
        queued += frames
        remember(bytes, offset, frames)
        receivedEndPts = ptsUs + framesUs(frames.toLong())
        if (queued.toLong() + device > target.toLong() + config.headroom) {
            val keep = if (target > device) target - device else 0
            val drop = queued - min(queued, keep)
            if (playing && haveLast && crossDone >= crossFrames) takeCross(drop)
            counters.overruns++
            counters.overrunFrames += skip(drop)
        }
        return true
    }

    /**
     * The track thread: up to [room] frames into [out] for a track holding
     * [deviceFrames]; returns the frames written. [trackUnderruns] is the
     * track's cumulative underrun count (-1: unknown).
     */
    fun pull(out: ByteArray, room: Int, deviceFrames: Int, nowUs: Long, trackUnderruns: Long = -1): Int {
        if (!started) { started = true; calmSince = nowUs }
        if (trackUnderruns >= 0) {
            val rose = lastTrackUnderruns in 0 until trackUnderruns
            lastTrackUnderruns = trackUnderruns
            if (rose && playing && deviceFrames > 0) {
                // Audio was waiting here, or the track held half the target or more and still was
                // not mixed: its mixer reads more at once than the target leaves. That is the
                // device, not the network: the grown target is this stream's floor, or calm would
                // shrink it into the gap again.
                if (queued > 0 || deviceFrames * 2 >= target) {
                    counters.underruns++; counters.trackUnderruns++; grow(nowUs); floor = max(floor, target)
                }
                // Nothing more had come: the next audio says whether the sound went on (late) or paused.
                else stalled = true
            }
        }
        if (playing && deviceFrames == 0) {
            playing = false
            stalled = false
            fadePending = true
            if (queued > 0) classify(chunks.first().ptsUs + framesUs(front.toLong()), nowUs) else awaitingResume = true
        }
        device = deviceFrames; pulledAt = nowUs
        if (!playing) {
            if (queued == 0 || queued + deviceFrames < target) return 0
            playing = true; smoothed = (queued + deviceFrames).toDouble(); smoothedAt = nowUs
        }
        if (nowUs - calmSince >= config.calmUs) {
            target = max(max(config.minTarget, floor), if (target > config.shrink) target - config.shrink else 0)
            calmSince = nowUs
        }
        if (config.tailFrames > 0 && tailsAllowed && !concealing && historyFrames > 0 && starving(deviceFrames)) tail()
        val fill = min(target.toLong(), deviceFrames.toLong() + max(room, 0)).toInt()
        val want = min(if (fill > deviceFrames) fill - deviceFrames else 0, out.size / FRAME_BYTES)
        var written = 0
        while (written < want) {
            val due = sinceCorrection >= config.correctEvery
            if (due && correction < 0 && queued > 1) { skip(1); counters.trimmedFrames++; sinceCorrection = 0; continue }
            if (due && correction > 0 && haveLast) {
                System.arraycopy(last, 0, out, written * FRAME_BYTES, FRAME_BYTES)
                written++; counters.insertedFrames++; sinceCorrection = 0; continue
            }
            if (queued == 0) break
            val chunk = chunks.first()
            val at = written * FRAME_BYTES
            System.arraycopy(chunk.bytes, front * FRAME_BYTES, out, at, FRAME_BYTES)
            if (chunk.concealed) fadePending = true else shape(out, at)
            System.arraycopy(out, at, last, 0, FRAME_BYTES); haveLast = true
            endedPts = chunk.ptsUs + framesUs(front + 1L)
            skip(1); written++; sinceCorrection++
        }
        counters.writtenFrames += written; device = deviceFrames + written
        val level = queued.toDouble() + device
        smoothed += (level - smoothed) * min(1.0, (nowUs - smoothedAt).toDouble() / config.smoothUs)
        smoothedAt = nowUs
        correction = if (smoothed > target.toDouble() + config.trimBand) -1 else if (smoothed < target.toDouble() - config.insertBand) 1 else 0
        return written
    }

    /** Playing, and what is left here and in a track that has not run dry yet is under concealBelow. */
    fun starving(deviceFrames: Int): Boolean =
        playing && deviceFrames > 0 && queued.toLong() + deviceFrames < config.concealBelow

    /** Made-up audio standing in for what has not come (its PTS goes on from the received sound). */
    fun pushConcealed(ptsUs: Long, bytes: ByteArray, offset: Int, size: Int): Boolean {
        if (!valid(size) || offset < 0 || offset + size > bytes.size) return false
        if (!concealing) { concealing = true; lateNoted = false }
        chunks.addLast(Chunk(ptsUs, bytes.copyOfRange(offset, offset + size), size / FRAME_BYTES, true))
        queued += size / FRAME_BYTES; counters.concealedFrames += size / FRAME_BYTES
        return true
    }

    /** Audio concealed since the last received audio came after all: the network was late. */
    fun noteLate(nowUs: Long) {
        if (!concealing || lateNoted) return
        lateNoted = true; counters.lateConcealments++; grow(nowUs)
    }

    /** Concealed audio stood in for a pause of the VM's sound: a silence, primed again. */
    fun notePause() {
        if (!playing) return
        counters.silences++; playing = false; fadePending = true
    }

    /** The Host capture-clock PTS at the track's play head now, while playing. */
    fun playingPtsUs(nowUs: Long): Long? {
        if (!playing || device == 0) return null
        val elapsed = min(nowUs - pulledAt, framesUs(device.toLong()))
        return endedPts - framesUs(device.toLong()) + elapsed
    }

    /** A new stream (the counters go on: they are the app's totals). */
    fun clear() {
        while (chunks.isNotEmpty()) recycle(chunks.removeFirst())
        queued = 0; front = 0; device = 0
        playing = false; awaitingResume = false; started = false; haveLast = false; concealing = false; lateNoted = false
        correction = 0; sinceCorrection = 0; floor = 0; target = config.startTarget
        lastTrackUnderruns = -1; stalled = false; historyFrames = 0; fadePending = true; fadeDone = 0; crossFrames = 0; crossDone = 0
    }

    /** A new track replaced the one this buffer fed: primed again, its emptiness is not an underrun. */
    fun restartDevice() {
        playing = false; awaitingResume = false; device = 0; lastTrackUnderruns = -1; stalled = false
        fadePending = true; crossFrames = 0; crossDone = 0
    }

    /** The track waits for [frames] before it plays (its start threshold): never aim lower. */
    fun raiseFloor(frames: Int) {
        floor = max(frames, 0)
        if (target < floor) target = floor
    }

    /** A track whose buffer is the target (before API 31): the target is what the track took. */
    fun followDevice(frames: Int) {
        if (frames <= 0) return
        target = frames
    }

    val queuedFrames get() = queued
    /** Frames, as of the last pull or push. */
    val level get() = queued + device

    // The sound that ran dry goes on (underrun) or starts again later (silence).
    private fun classify(ptsUs: Long, nowUs: Long) {
        if (ptsUs > continuedPts + config.gapUs) { counters.silences++; return }
        counters.underruns++; grow(nowUs)
    }

    private fun grow(nowUs: Long) {
        target = min(max(config.maxTarget, floor), target + config.grow); calmSince = nowUs
    }

    /** Removes up to [frames] of the oldest queued frames; returns how many. */
    private fun skip(frames: Int): Int {
        var removed = 0
        while (removed < frames && chunks.isNotEmpty()) {
            val chunk = chunks.first()
            val left = chunk.frames - front
            val step = min(left, frames - removed)
            removed += step; queued -= step
            if (!chunk.concealed) continuedPts = chunk.ptsUs + framesUs((front + step).toLong())
            if (step == left) { recycle(chunks.removeFirst()); front = 0 } else front += step
        }
        return removed
    }

    private fun recycle(chunk: Chunk) {
        if (chunk.bytes.size == SPARE_BYTES && spare.size < MAX_SPARE) spare.addLast(chunk.bytes)
    }

    private fun remember(bytes: ByteArray, offset: Int, frames: Int) {
        if (config.tailFrames <= 0) return
        val capacity = config.tailFrames
        if (frames >= capacity) {
            System.arraycopy(bytes, offset + (frames - capacity) * FRAME_BYTES, history, 0, capacity * FRAME_BYTES)
            historyFrames = capacity
            return
        }
        val keep = min(historyFrames, capacity - frames)
        System.arraycopy(history, (historyFrames - keep) * FRAME_BYTES, history, 0, keep * FRAME_BYTES)
        System.arraycopy(bytes, offset, history, keep * FRAME_BYTES, frames * FRAME_BYTES)
        historyFrames = keep + frames
    }

    /** One fade-out tail after the newest received audio: mirrored in time, faded to silence. */
    private fun tail() {
        val frames = historyFrames
        val bytes = ByteArray(frames * FRAME_BYTES)
        PcmFade.mirrorFadeOut(history, frames, bytes)
        pushConcealed(receivedEndPts, bytes, 0, bytes.size)
        counters.tails++
    }

    /** The first frames of what an overrun drops: what would have followed the last frame played. */
    private fun takeCross(drop: Int) {
        val frames = min(min(drop, config.fadeFrames), queued)
        var copied = 0
        var index = 0
        var offset = front
        while (copied < frames && index < chunks.size) {
            val chunk = chunks[index]
            val step = min(chunk.frames - offset, frames - copied)
            System.arraycopy(chunk.bytes, offset * FRAME_BYTES, cross, copied * FRAME_BYTES, step * FRAME_BYTES)
            copied += step; index++; offset = 0
        }
        crossFrames = copied; crossDone = 0
    }

    /** A received frame at [at] of [out]: crossfaded after a cut, faded in after silence or a tail. */
    private fun shape(out: ByteArray, at: Int) {
        if (crossDone < crossFrames) {
            val weight = (crossDone + 1f) / (crossFrames + 1f)
            for (c in 0 until 2) {
                val mixed = PcmFade.sample(cross, crossDone * FRAME_BYTES + c * 2) * (1f - weight) + PcmFade.sample(out, at + c * 2) * weight
                PcmFade.put(out, at + c * 2, mixed.roundToInt())
            }
            crossDone++
        }
        if (fadePending) { fadePending = false; fadeDone = 0 }
        if (fadeDone < config.fadeFrames) {
            val gain = (fadeDone + 1f) / (config.fadeFrames + 1f)
            for (c in 0 until 2) PcmFade.put(out, at + c * 2, (PcmFade.sample(out, at + c * 2) * gain).roundToInt())
            fadeDone++
        }
    }
}

/** s16 stereo little-endian helpers of the playout and the Opus concealment. */
internal object PcmFade {
    fun sample(bytes: ByteArray, at: Int): Int = (bytes[at].toInt() and 0xff) or (bytes[at + 1].toInt() shl 8)

    fun put(bytes: ByteArray, at: Int, value: Int) {
        val clipped = value.coerceIn(-32768, 32767)
        bytes[at] = clipped.toByte(); bytes[at + 1] = (clipped shr 8).toByte()
    }

    /**
     * [frames] stereo frames of [from] (from offset 0) into [to] (from offset
     * 0) backwards in time, under a half-cosine fade from 1 to 0: the first
     * frame out is the last frame in, so the sound goes on without an edge,
     * and the last frame out is silent.
     */
    fun mirrorFadeOut(from: ByteArray, frames: Int, to: ByteArray) {
        for (n in 0 until frames) {
            val gain = 0.5 * (1 + cos(PI * (n + 1) / frames))
            val source = (frames - 1 - n) * 4
            for (c in 0 until 2) put(to, n * 4 + c * 2, (sample(from, source + c * 2) * gain).roundToInt())
        }
    }
}

/**
 * One look at the track by its feeding thread (AndroidAudioOut, every 5 ms),
 * against a [Sink] (the AudioTrack there, a simulated track in the tests).
 * The track's level is frames written minus its play head. What the track did
 * not take of a write stays here and is written first at the next look; no
 * new audio is pulled until it is in, so nothing is dropped or reordered.
 * Where the track's buffer is the target ([follows]), a changed target is
 * applied to it, and what the platform took is the target.
 */
internal class AudioFeed(private val playout: AudioJitterBuffer, capacityFrames: Int) {
    /** The track as the feed uses it. */
    interface Sink {
        /** The play head: the platform's 32-bit frame counter. */
        fun head(): Int
        /** The track's cumulative underrun count, -1 where unknown. */
        fun underruns(): Long
        /** A non-blocking write: bytes taken, or a negative error. */
        fun write(buffer: ByteArray, offset: Int, bytes: Int): Int
        /** setBufferSizeInFrames: the buffer the platform applied (<= 0: refused). */
        fun bufferSize(frames: Int): Int
    }

    private val scratch = ByteArray(capacityFrames * AudioJitterBuffer.FRAME_BYTES)
    private var capacity = capacityFrames
    private var pendingOffset = 0
    private var pendingBytes = 0
    private var appliedTarget = 0
    /** The track's effective buffer (frames). */
    var bufferFrames = capacityFrames
        private set
    /** The buffer is the jitter target (audioBufferFollowsTarget). */
    var follows = false
        private set
    /** The level the track waits for before it plays (frames). */
    var startLevel = 0
        private set
    /** Frames written to the current track, and to every track of the stream. */
    @Volatile var trackWritten = 0L
        private set
    @Volatile var written = 0L
        private set
    /** As of the last look: the play head, the track's level, whether sound plays and whether it started then. */
    var head = 0
        private set
    var device = 0
        private set
    var playing = false
        private set
    var soundStarted = false
        private set
    /** Bytes the track has not taken yet (written first at the next look). */
    val pending get() = pendingBytes

    /** A new stream: nothing written yet. */
    fun newStream() { written = 0; trackWritten = 0; pendingBytes = 0 }

    /**
     * A new track: its buffer is the target ([follows]) or its whole capacity
     * ([bufferFrames]); it waits for [startLevel] before it plays; [fineReads]:
     * its mixer reads it in small steps (the fast path), where a fade-out tail
     * can be in time (AudioJitterBuffer.tailsAllowed).
     */
    fun track(follows: Boolean, bufferFrames: Int, capacityFrames: Int, startLevel: Int, fineReads: Boolean) {
        this.follows = follows
        this.bufferFrames = bufferFrames
        capacity = min(capacityFrames, scratch.size / AudioJitterBuffer.FRAME_BYTES)
        this.startLevel = startLevel
        appliedTarget = if (follows) bufferFrames else 0
        trackWritten = 0; pendingBytes = 0; device = 0; head = 0; playing = false
        synchronized(playout) {
            playout.restartDevice()
            playout.tailsAllowed = fineReads
            if (follows) playout.followDevice(bufferFrames) else playout.raiseFloor(startLevel)
        }
    }

    /** One look at [nowNanos]: 0, or the negative error of a write (the track is lost). */
    fun tick(sink: Sink, nowNanos: Long): Int {
        soundStarted = false
        if (pendingBytes > 0) {
            val n = sink.write(scratch, pendingOffset, pendingBytes)
            if (n < 0) return n
            took(n); pendingOffset += n; pendingBytes -= n
            if (pendingBytes > 0) return 0
        }
        head = sink.head()
        device = audioDeviceFrames(trackWritten, head, capacity)
        val underruns = sink.underruns()
        val frames: Int
        val target: Int
        synchronized(playout) {
            val was = playout.playing
            frames = playout.pull(scratch, (bufferFrames - device).coerceAtLeast(0), device, nowNanos / 1000, underruns)
            playing = playout.playing
            soundStarted = playing && !was
            target = playout.target
        }
        if (follows && target != appliedTarget) {
            appliedTarget = target
            val applied = sink.bufferSize(target)
            if (applied > 0) {
                bufferFrames = applied; startLevel = applied; appliedTarget = applied
                if (applied != target) synchronized(playout) { playout.followDevice(applied) }
            }
        }
        if (frames > 0) {
            val bytes = frames * AudioJitterBuffer.FRAME_BYTES
            val n = sink.write(scratch, 0, bytes)
            if (n < 0) return n
            took(n)
            if (n < bytes) { pendingOffset = n; pendingBytes = bytes - n }
        }
        return 0
    }

    private fun took(bytes: Int) {
        val frames = (bytes / AudioJitterBuffer.FRAME_BYTES).toLong()
        trackWritten += frames; written += frames
    }
}

/**
 * Whether the track's effective buffer (setBufferSizeInFrames) is the jitter
 * target, instead of the track's whole capacity.
 *
 * AOSP, Track::isReady (frameworks/av/services/audioflinger/Tracks.cpp): a
 * streaming track that is filling (after start(), and again after the
 * mixer disabled it for running dry too long, when the client's next write
 * starts it again) plays only once framesReady() reaches the effective buffer
 * size before Android 12, and min(start threshold, buffer size) from Android 12
 * (setStartThresholdInFrames, API 31). With the whole capacity as the buffer
 * before API 31, a track kept at the target would never start again after a
 * pause. So there the buffer is the target, and priming writes the whole
 * target at once: the restart level is met by the first write. The same holds
 * on API 31 and later when the start threshold could not be set.
 */
internal fun audioBufferFollowsTarget(api: Int, startThresholdApplied: Boolean): Boolean =
    api < 31 || !startThresholdApplied

/**
 * Frames still in the track: written minus its play head. The head is the
 * platform's 32-bit frame counter (AudioTrack.getPlaybackHeadPosition), so
 * the difference is taken modulo 2^32; anything beyond [capacity] is a head
 * that does not belong to these writes (a flushed or new track) and reads 0.
 */
internal fun audioDeviceFrames(written: Long, head: Int, capacity: Int): Int {
    val frames = ((written and 0xffffffffL) - (head.toLong() and 0xffffffffL)) and 0xffffffffL
    return if (frames > capacity) 0 else frames.toInt()
}

/**
 * A track that is written to and does not play: its play head stays the same
 * for [stuckNanos] while it holds the level it waits for ([full]).
 * AndroidAudioOut then makes the track again with its buffer following the
 * target: whatever level the platform waits for, the new track reaches it.
 */
internal class AudioStartWatch(private val stuckNanos: Long = 1_000_000_000L) {
    private var played = -1L
    private var since = -1L

    fun reset() { played = -1; since = -1 }

    /** One look at [now]: the track's play head and whether it holds its start level. */
    fun stuck(now: Long, played: Long, full: Boolean): Boolean {
        if (played != this.played || !full || since < 0) { this.played = played; since = now; return false }
        return now - since >= stuckNanos
    }
}

/**
 * How often a stream's track may be made again (the platform lost it, or it
 * was written to and did not play): at most [perWindow] times in any
 * [windowNanos], and [gapNanos] apart. A refused remake is asked for again
 * on a later look, so a stream whose route changed after a run of failures
 * still gets its sound back.
 */
internal class RemakeLimiter(
    private val perWindow: Int = 3,
    private val windowNanos: Long = 60_000_000_000L,
    private val gapNanos: Long = 1_000_000_000L,
) {
    private val recent = ArrayDeque<Long>()

    fun reset() = recent.clear()

    /** True when a remake at [now] is allowed (and counted). */
    fun allow(now: Long): Boolean {
        while (recent.isNotEmpty() && now - recent.first() >= windowNanos) recent.removeFirst()
        if (recent.size >= perWindow) return false
        if (recent.isNotEmpty() && now - recent.last() < gapNanos) return false
        recent.addLast(now)
        return true
    }
}
