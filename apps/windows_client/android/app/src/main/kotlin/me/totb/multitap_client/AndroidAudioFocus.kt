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
 * stops for focus; only the track's volume follows it (Android's rules for a player):
 *  - a transient loss (a call rings, a voice call app, an exclusive prompt) mutes it until focus returns;
 *  - a delayed grant (asked during a call) mutes it until the platform grants it;
 *  - ducking is left to the platform (willPauseWhenDucked false: Android lowers the track itself);
 *  - a permanent loss (the user started another player) mutes it;
 *  - a refused request leaves the sound playing without focus (never silent for a refusal alone), except
 *    that a refusal while a transient loss or a delayed grant still stands keeps that mute (a call goes on).
 * A session is never left muted for good: while the app is on screen and the VM's sound plays, a focus that
 * is not held is asked for again ([askAgain]): when the user comes back to the app (shown again, or its window
 * focused again) at once, and while sound plays at most every [AudioFocusPolicy.RETRY_NANOS], after a transient
 * loss or a delayed grant only once it lasted [AudioFocusPolicy.TRANSIENT_GRACE_NANOS] (the platform normally
 * gives it back within moments; a call refuses it and stays muted). An ask again that leaves the sound muted is
 * [AudioFocusPolicy.blocked]: the app says so once on screen (android_remote.dart).
 * A session restart (a resize, a reconnect) releases and takes the track again within moments; focus is given up
 * only [RELEASE_DELAY_MS] after the last track went, so another player does not resume for a moment in between.
 */
internal class AudioFocusPolicy {
    enum class State { NONE, HELD, WAITING, PAUSED, LOST }
    /** When focus is asked for again: the user came back to the app, or the VM's sound plays. */
    enum class Moment { BACK, SOUND }

    var state = State.NONE
        private set

    /** An ask again did not bring focus back and the sound is muted (a call, another app holds it). */
    var blocked = false
        private set

    // When the current muted state began, and the last ask again (0: none since).
    private var mutedAt = 0L
    private var askedAt = 0L
    private var asked = false

    /** The answer of requestAudioFocus ([again]: asked again after a loss, not at a track's start). */
    fun requested(result: Int, now: Long = 0L, again: Boolean = false) {
        val before = state
        val next = when (result) {
            REQUEST_GRANTED -> State.HELD
            REQUEST_DELAYED -> State.WAITING
            // A call (or another transient owner) still holds it: its mute stands.
            else -> if (before == State.PAUSED || before == State.WAITING) before else State.NONE
        }
        enter(next, now)
        blocked = again && volume == 0f
    }

    /** An OnAudioFocusChangeListener change. */
    fun changed(change: Int, now: Long = 0L) {
        val next = when (change) {
            GAIN, GAIN_TRANSIENT, GAIN_TRANSIENT_MAY_DUCK, GAIN_TRANSIENT_EXCLUSIVE -> State.HELD
            LOSS_TRANSIENT -> State.PAUSED
            LOSS_TRANSIENT_CAN_DUCK -> if (state == State.NONE) State.NONE else State.HELD
            LOSS -> State.LOST
            else -> state
        }
        enter(next, now)
        if (volume != 0f) blocked = false
    }

    fun released() { state = State.NONE; blocked = false; asked = false }

    /** The track's volume. Ducking is the platform's, so a held focus is always full volume. */
    val volume: Float get() = when (state) { State.NONE, State.HELD -> 1f; else -> 0f }

    /** Not held and not promised: a request at a track's start may take it. */
    val shouldRequest: Boolean get() = state == State.NONE || state == State.LOST

    /**
     * Whether to ask for focus again at [now]: only while a stream wants sound ([wanted]) and the app is on
     * screen ([visible]). [Moment.BACK] (the app shown again, its window focused again): any focus not held, at
     * once (at most every [BACK_GAP_NANOS]). [Moment.SOUND] (the VM's sound plays, also while another window has
     * input focus, as in split screen or a pop-up view): a muted state only, at most every [RETRY_NANOS], and a
     * transient loss or a delayed grant only after it lasted [TRANSIENT_GRACE_NANOS]. Call [asking] before asking.
     */
    fun askAgain(wanted: Boolean, visible: Boolean, moment: Moment, now: Long): Boolean {
        if (!wanted || !visible || state == State.HELD) return false
        val gap = if (moment == Moment.BACK) BACK_GAP_NANOS else RETRY_NANOS
        if (asked && now - askedAt < gap) return false
        return when (state) {
            State.NONE -> moment == Moment.BACK
            State.LOST -> true
            State.PAUSED, State.WAITING -> moment == Moment.BACK || now - mutedAt >= TRANSIENT_GRACE_NANOS
            State.HELD -> false
        }
    }

    /** A request again goes out at [now]. */
    fun asking(now: Long) { asked = true; askedAt = now }

    private fun enter(next: State, now: Long) {
        val mutedBefore = volume == 0f
        state = next
        // A mute that goes on in another form (a call's pause, then its delayed grant) keeps its start.
        if (volume == 0f && !mutedBefore) mutedAt = now
    }

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
        /** While sound plays and focus is not held: asked again at most this often. */
        const val RETRY_NANOS = 5_000_000_000L
        /** A transient loss or a delayed grant is the platform's to end; asked again only after this long. */
        const val TRANSIENT_GRACE_NANOS = 15_000_000_000L
        /** The user coming back asks at once, but not twice within this. */
        const val BACK_GAP_NANOS = 1_000_000_000L
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
        .setOnAudioFocusChangeListener({ change -> policy.changed(change, System.nanoTime()); publish() }, main)
        .build()

    private val abandon = Runnable {
        if (wanted) return@Runnable
        try { manager?.abandonAudioFocusRequest(request) } catch (_: Exception) {}
        policy.released(); publish()
    }

    private fun ask(again: Boolean) {
        val now = System.nanoTime()
        if (again) policy.asking(now)
        val result = try { manager?.requestAudioFocus(request) ?: AudioFocusPolicy.REQUEST_FAILED } catch (_: Exception) { AudioFocusPolicy.REQUEST_FAILED }
        policy.requested(result, now, again)
    }

    /** A track with sound started. */
    fun acquire() {
        wanted = true
        main.removeCallbacks(abandon)
        if (policy.shouldRequest) ask(again = false)
        publish()
    }

    /** The track went (session end or restart). */
    fun release() {
        wanted = false
        main.removeCallbacks(abandon)
        main.postDelayed(abandon, RELEASE_DELAY_MS)
    }

    // The Activity is started (on screen, also behind a dialog or in split screen).
    private var visible = true

    private fun askAgain(moment: AudioFocusPolicy.Moment) {
        if (!policy.askAgain(wanted, visible, moment, System.nanoTime())) return
        ask(again = true)
        publish()
    }

    /** The Activity started (true) or stopped (false); being shown again is the user coming back. */
    fun visible(shown: Boolean) {
        visible = shown
        if (shown) askAgain(AudioFocusPolicy.Moment.BACK)
    }

    /** The Activity's window gained or lost input focus; gaining it is the user coming back. */
    fun windowFocus(focused: Boolean) { if (focused) askAgain(AudioFocusPolicy.Moment.BACK) }

    /** The VM's sound plays (its start, after a silence, and once a second while it goes on). */
    fun soundStarted() = askAgain(AudioFocusPolicy.Moment.SOUND)

    /** audio-path-v1 diagnostics (any thread): AudioFocusPolicy.State's ordinal (0 none .. 4 lost). */
    @Volatile var stateCode = 0
        private set
    /** An ask again left the sound muted (AudioFocusPolicy.blocked), while a stream wants sound. */
    @Volatile var blocked = false
        private set

    private fun publish() {
        stateCode = policy.state.ordinal
        blocked = wanted && policy.blocked
        val next = policy.volume
        if (next == volume) return
        volume = next
        apply(next)
    }

    companion object {
        const val RELEASE_DELAY_MS = 2000L
    }
}
