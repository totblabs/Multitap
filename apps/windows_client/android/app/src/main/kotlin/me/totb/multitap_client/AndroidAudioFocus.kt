package me.totb.multitap_client

import android.content.Context
import android.media.AudioAttributes
import android.media.AudioFocusRequest
import android.media.AudioManager
import android.os.Handler

/**
 * Audio focus of a stream's VM sound. Without it a phone call, a voice call app or another player's sound
 * mixes with the VM's at full volume.
 *
 * A stream with audio holds AUDIOFOCUS_GAIN like a video player (another player pauses). The stream never
 * stops for focus; only the track's volume follows it:
 *  - a transient loss (a call rings, a voice call app, an exclusive prompt) mutes it until focus returns;
 *  - ducking is left to the platform (willPauseWhenDucked false: Android lowers the track itself);
 *  - a permanent loss (the user started another player) mutes it, and the app asks again when it is shown;
 *  - a refused request leaves the sound playing without focus (never silent for a refusal alone).
 * A session restart (a resize, a reconnect) releases and takes the track again within moments; focus is given up
 * only [RELEASE_DELAY_MS] after the last track went, so another player does not resume for a moment in between.
 */
internal class AudioFocusPolicy {
    enum class State { NONE, HELD, WAITING, PAUSED, LOST }

    var state = State.NONE
        private set

    /** The answer of requestAudioFocus. */
    fun requested(result: Int) {
        state = when (result) {
            REQUEST_GRANTED -> State.HELD
            REQUEST_DELAYED -> State.WAITING
            else -> State.NONE
        }
    }

    /** An OnAudioFocusChangeListener change. */
    fun changed(change: Int) {
        state = when (change) {
            GAIN, GAIN_TRANSIENT, GAIN_TRANSIENT_MAY_DUCK, GAIN_TRANSIENT_EXCLUSIVE -> State.HELD
            LOSS_TRANSIENT -> State.PAUSED
            LOSS_TRANSIENT_CAN_DUCK -> if (state == State.NONE) State.NONE else State.HELD
            LOSS -> State.LOST
            else -> state
        }
    }

    fun released() { state = State.NONE }

    /** The track's volume. Ducking is the platform's, so a held focus is always full volume. */
    val volume: Float get() = when (state) { State.NONE, State.HELD -> 1f; else -> 0f }

    /** Not held and not promised: a request (again) may take it. */
    val shouldRequest: Boolean get() = state == State.NONE || state == State.LOST

    companion object {
        // AudioManager's values (fixed platform constants), so the policy is a plain JVM class.
        const val GAIN = 1
        const val GAIN_TRANSIENT = 2
        const val GAIN_TRANSIENT_MAY_DUCK = 3
        const val GAIN_TRANSIENT_EXCLUSIVE = 4
        const val LOSS = -1
        const val LOSS_TRANSIENT = -2
        const val LOSS_TRANSIENT_CAN_DUCK = -3
        const val REQUEST_FAILED = 0
        const val REQUEST_GRANTED = 1
        const val REQUEST_DELAYED = 2
    }
}

/** Main thread only. [apply] receives every volume change (the owner sets it on its track's thread). */
internal class AndroidAudioFocus(context: Context, private val main: Handler, private val apply: (Float) -> Unit) {
    private val manager = context.getSystemService(Context.AUDIO_SERVICE) as AudioManager?
    private val policy = AudioFocusPolicy()
    private var wanted = false

    /** The volume a new track starts with (read on the track's thread). */
    @Volatile var volume = 1f
        private set

    private val request: AudioFocusRequest = AudioFocusRequest.Builder(AudioManager.AUDIOFOCUS_GAIN)
        .setAudioAttributes(AudioAttributes.Builder().setUsage(AudioAttributes.USAGE_MEDIA)
            .setContentType(AudioAttributes.CONTENT_TYPE_MOVIE).build())
        .setAcceptsDelayedFocusGain(true)
        .setWillPauseWhenDucked(false)
        .setOnAudioFocusChangeListener({ change -> policy.changed(change); publish() }, main)
        .build()

    private val abandon = Runnable {
        if (wanted) return@Runnable
        try { manager?.abandonAudioFocusRequest(request) } catch (_: Exception) {}
        policy.released(); publish()
    }

    /** A track with sound started. */
    fun acquire() {
        wanted = true
        main.removeCallbacks(abandon)
        if (policy.shouldRequest) {
            val result = try { manager?.requestAudioFocus(request) ?: AudioFocusPolicy.REQUEST_FAILED } catch (_: Exception) { AudioFocusPolicy.REQUEST_FAILED }
            policy.requested(result)
        }
        publish()
    }

    /** The track went (session end or restart). */
    fun release() {
        wanted = false
        main.removeCallbacks(abandon)
        main.postDelayed(abandon, RELEASE_DELAY_MS)
    }

    /** The app is visible again: a focus another player took is asked for again. */
    fun foreground() { if (wanted && policy.state == AudioFocusPolicy.State.LOST) acquire() }

    /** audio-path-v1 diagnostics (any thread): AudioFocusPolicy.State's ordinal (0 none .. 4 lost). */
    @Volatile var stateCode = 0
        private set

    private fun publish() {
        stateCode = policy.state.ordinal
        val next = policy.volume
        if (next == volume) return
        volume = next
        apply(next)
    }

    companion object {
        const val RELEASE_DELAY_MS = 2000L
    }
}
