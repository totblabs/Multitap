package me.totb.multitap_client

import android.media.AudioAttributes
import android.media.AudioFormat
import android.media.AudioTrack
import android.os.Build
import android.os.Process
import java.util.concurrent.locks.LockSupport

/**
 * One stream's AudioTrack, fed from [playout] (AudioJitterBuffer, shared with
 * the decoder worker that pushes into it under its lock) by a thread of its
 * own at audio priority (THREAD_PRIORITY_URGENT_AUDIO) every [TICK_NANOS]:
 * the decoder thread also decodes video and no longer decides when sound is
 * written. Each look is AudioFeed.tick (AndroidAudioJitter.kt): the track's
 * level (frames written minus its play head) and its own underrun count go
 * to the buffer, what it gives is written without blocking, and what the
 * track did not take is written first at the next look.
 *
 * Before API 31 (or when the start threshold does not take) the track's
 * buffer is the jitter target (audioBufferFollowsTarget). A track the
 * platform lost, or one written to that does not play (AudioStartWatch), is
 * made again within [RemakeLimiter]'s rate; a refused remake is asked for
 * again at a later look, so the sound comes back after a later route change.
 *
 * [onTrack] runs on the feeding thread when a track starts (its owner takes
 * audio focus), [onSound] when sound starts after priming (the stream's
 * start or the end of a silence).
 */
internal class AndroidAudioOut(
    private val playout: AudioJitterBuffer,
    private val volume: () -> Float,
    private val onTrack: () -> Unit,
    private val onSound: () -> Unit,
) {
    @Volatile var track: AudioTrack? = null
        private set
    /** -1 after a track could not be made or a write failed, 0 again once a track is made. */
    @Volatile var error = 0
        private set
    /** audio-path-v1: tracks made again this stream (lost by the platform / not playing). */
    @Volatile var restarts = 0L
        private set
    @Volatile var stuck = 0L
        private set
    /** The stream has a feeding thread (sound was asked for and the stream has not ended). */
    @Volatile var active = false
        private set

    private val feed = AudioFeed(playout, CAPACITY_FRAMES)
    val trackWritten get() = feed.trackWritten
    val written get() = feed.written
    val follows get() = feed.follows
    val startLevel get() = feed.startLevel

    private var thread: Thread? = null
    @Volatile private var running = false
    // The feeding thread's own state.
    private var sink: AudioFeed.Sink? = null
    private val limiter = RemakeLimiter()
    private val startWatch = AudioStartWatch()
    private var forceFollow = false

    /** The decoder worker, at a stream's start: a track and its feeding thread. */
    fun open() {
        close()
        synchronized(playout) { playout.clear() }
        feed.newStream()
        restarts = 0; stuck = 0; forceFollow = false; error = 0
        limiter.reset()
        create()
        running = true; active = true
        thread = Thread({ loop() }, "Multitap audio").apply { start() }
    }

    /** The decoder worker, at the stream's end: the thread stops, the track is released. */
    fun close() {
        val feeding = thread
        running = false
        thread = null
        if (feeding != null) {
            LockSupport.unpark(feeding)
            try { feeding.join(500) } catch (_: InterruptedException) { Thread.currentThread().interrupt() }
        }
        release()
        active = false
        synchronized(playout) { playout.clear() }
    }

    /** Any thread: the volume audio focus allows. */
    fun setVolume(value: Float) { try { track?.setVolume(value) } catch (_: Exception) {} }

    /** Any thread: the track's own cumulative underrun count, 0 where unknown. */
    fun trackUnderruns(): Long = try { track?.underrunCount?.toLong() ?: 0L } catch (_: Exception) { 0L }

    private fun loop() {
        try { Process.setThreadPriority(Process.THREAD_PRIORITY_URGENT_AUDIO) } catch (_: Exception) {}
        var next = System.nanoTime()
        while (running) {
            try { tick(System.nanoTime()) } catch (_: Exception) { error = -1 }
            next += TICK_NANOS
            val now = System.nanoTime()
            // Behind by more than a look (the thread was not run): no burst of looks to catch up.
            if (next < now - TICK_NANOS) next = now
            if (next > now) LockSupport.parkNanos(next - now)
        }
    }

    private fun tick(now: Long) {
        val output = sink
        if (track == null || output == null) {
            if (limiter.allow(now)) { restarts++; create() }
            return
        }
        val code = feed.tick(output, now)
        if (code < 0) {
            error = -1
            // The platform lost the track: every later write would fail too.
            if (code == AudioTrack.ERROR_DEAD_OBJECT || code == AudioTrack.ERROR) release()
            return
        }
        if (feed.soundStarted) onSound()
        // A track that holds the level it waits for and whose head does not move is made again.
        val full = feed.playing && feed.device > 0 && feed.device >= feed.startLevel
        if (startWatch.stuck(now, feed.head.toLong() and 0xffffffffL, full) && limiter.allow(now)) {
            stuck++; forceFollow = true
            release()
            create()
        }
    }

    private fun create() {
        try {
            val minimum = AudioTrack.getMinBufferSize(48000, AudioFormat.CHANNEL_OUT_STEREO, AudioFormat.ENCODING_PCM_16BIT)
            check(minimum > 0)
            val made = AudioTrack.Builder()
                .setAudioAttributes(AudioAttributes.Builder().setUsage(AudioAttributes.USAGE_MEDIA)
                    .setContentType(AudioAttributes.CONTENT_TYPE_MOVIE).build())
                .setAudioFormat(AudioFormat.Builder().setSampleRate(48000)
                    .setChannelMask(AudioFormat.CHANNEL_OUT_STEREO).setEncoding(AudioFormat.ENCODING_PCM_16BIT).build())
                .setBufferSizeInBytes(maxOf(minimum, CAPACITY_FRAMES * AudioJitterBuffer.FRAME_BYTES))
                .setTransferMode(AudioTrack.MODE_STREAM)
                // The platform's fast mixer path where it can; otherwise a normal track.
                .setPerformanceMode(AudioTrack.PERFORMANCE_MODE_LOW_LATENCY)
                .build()
            try {
                val capacity = (try { made.bufferCapacityInFrames } catch (_: Exception) { CAPACITY_FRAMES }).coerceIn(1, CAPACITY_FRAMES)
                var threshold = -1
                if (Build.VERSION.SDK_INT >= 31 && !forceFollow) {
                    threshold = try { made.setStartThresholdInFrames(START_FRAMES) } catch (_: Exception) { -1 }
                }
                val follows = audioBufferFollowsTarget(Build.VERSION.SDK_INT, threshold > 0) || forceFollow
                val target = synchronized(playout) { playout.target }
                val applied = try { made.setBufferSizeInFrames(if (follows) target else capacity) } catch (_: Exception) { -1 }
                val buffer = (try { made.bufferSizeInFrames } catch (_: Exception) { applied }).let { if (it > 0) it else capacity }
                val fast = try { made.performanceMode == AudioTrack.PERFORMANCE_MODE_LOW_LATENCY } catch (_: Exception) { false }
                feed.track(follows, buffer, capacity, if (follows) buffer else threshold.coerceAtLeast(0), fineReads = fast)
                startWatch.reset()
                try { made.setVolume(volume()) } catch (_: Exception) {}
                made.play()
                sink = TrackSink(made)
                track = made
            } catch (failure: Exception) { try { made.release() } catch (_: Exception) {}; throw failure }
            error = 0
            onTrack()
        } catch (_: Exception) {
            track = null; sink = null; error = -1
        }
    }

    private fun release() {
        val old = track ?: return
        track = null; sink = null
        try { old.pause(); old.flush() } catch (_: Exception) {}
        try { old.release() } catch (_: Exception) {}
    }

    /** The AudioTrack as AudioFeed uses it. */
    private class TrackSink(private val track: AudioTrack) : AudioFeed.Sink {
        override fun head(): Int = try { track.playbackHeadPosition } catch (_: Exception) { 0 }
        override fun underruns(): Long = try { track.underrunCount.toLong() } catch (_: Exception) { -1L }
        override fun write(buffer: ByteArray, offset: Int, bytes: Int): Int =
            track.write(buffer, offset, bytes, AudioTrack.WRITE_NON_BLOCKING)
        override fun bufferSize(frames: Int): Int = try { track.setBufferSizeInFrames(frames) } catch (_: Exception) { -1 }
    }

    companion object {
        /** How often the track is looked at: well inside a mixer period, so the next audio is in before it is read. */
        const val TICK_NANOS = 5_000_000L
        /** 48 kHz stereo frames: the track's capacity, the jitter buffer's largest target. */
        const val CAPACITY_FRAMES = 48 * 200
        /** From API 31: the track plays once this much is in it (below the 40 ms the buffer primes to). */
        const val START_FRAMES = 48 * 30
    }
}
