package me.totb.multitap_client

import me.totb.multitap_client.AudioFocusPolicy.Moment
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

// AudioFocusPolicy (AndroidAudioFocus.kt): the VM sound follows audio focus by volume only, and a session
// is never left muted for good: a focus that is not held is asked for again while the app is on screen.
class AudioFocusPolicyTest {
    private val p = AudioFocusPolicy()
    private val second = 1_000_000_000L

    @Test
    fun aCallMutesAndItsEndRestores() {
        p.requested(AudioFocusPolicy.REQUEST_GRANTED)
        assertEquals(1f, p.volume)
        p.changed(AudioFocusPolicy.LOSS_TRANSIENT)
        assertEquals(0f, p.volume)
        assertFalse(p.shouldRequest)
        p.changed(AudioFocusPolicy.GAIN)
        assertEquals(1f, p.volume)
    }

    @Test
    fun duckingIsThePlatformsAndKeepsFullVolume() {
        p.requested(AudioFocusPolicy.REQUEST_GRANTED)
        p.changed(AudioFocusPolicy.LOSS_TRANSIENT_CAN_DUCK)
        assertEquals(1f, p.volume)
    }

    @Test
    fun anotherPlayerMutesUntilAskedAgain() {
        p.requested(AudioFocusPolicy.REQUEST_GRANTED)
        p.changed(AudioFocusPolicy.LOSS)
        assertEquals(0f, p.volume)
        assertTrue(p.shouldRequest)
        p.requested(AudioFocusPolicy.REQUEST_GRANTED)
        assertEquals(1f, p.volume)
    }

    @Test
    fun aDelayedGrantDuringACallWaitsSilently() {
        p.requested(AudioFocusPolicy.REQUEST_DELAYED)
        assertEquals(0f, p.volume)
        assertFalse(p.shouldRequest)
        p.changed(AudioFocusPolicy.GAIN)
        assertEquals(1f, p.volume)
    }

    @Test
    fun aRefusedRequestNeverSilencesTheStream() {
        p.requested(AudioFocusPolicy.REQUEST_FAILED)
        assertEquals(1f, p.volume)
        assertTrue(p.shouldRequest)
        p.changed(AudioFocusPolicy.LOSS_TRANSIENT_CAN_DUCK)
        assertEquals(1f, p.volume)
    }

    @Test
    fun aLossIsAskedBackWhenTheUserReturns() {
        p.requested(AudioFocusPolicy.REQUEST_GRANTED, 0)
        p.changed(AudioFocusPolicy.LOSS, 1 * second)
        assertEquals(0f, p.volume)
        assertTrue("the user came back to the app", p.askAgain(wanted = true, visible = true, moment = Moment.BACK, now = 2 * second))
        p.asking(2 * second)
        p.requested(AudioFocusPolicy.REQUEST_GRANTED, 2 * second, again = true)
        assertEquals(1f, p.volume)
        assertFalse(p.blocked)
        assertFalse("held again: nothing to ask", p.askAgain(true, true, Moment.BACK, 10 * second))
        assertFalse("held again", p.askAgain(true, true, Moment.SOUND, 10 * second))
    }

    @Test
    fun lostWithoutWindowFocusIsAskedBackWhenSoundPlaysOnScreen() {
        // Split screen or a pop-up view: another window has input focus, the app is still on screen. The VM's
        // sound asks again (no longer only the window's focus), at most every RETRY_NANOS.
        p.requested(AudioFocusPolicy.REQUEST_GRANTED, 0)
        p.changed(AudioFocusPolicy.LOSS, 0)
        assertTrue(p.askAgain(wanted = true, visible = true, moment = Moment.SOUND, now = second))
        p.asking(second)
        p.requested(AudioFocusPolicy.REQUEST_GRANTED, second, again = true)
        assertEquals(1f, p.volume)
        // Taken again by the other window's player: the next ask waits out the retry gap.
        p.changed(AudioFocusPolicy.LOSS, 2 * second)
        assertFalse(p.askAgain(true, true, Moment.SOUND, 3 * second))
        assertTrue(p.askAgain(true, true, Moment.SOUND, second + AudioFocusPolicy.RETRY_NANOS))
        // Not on screen (the app went to the background) or no stream wants sound: never.
        assertFalse(p.askAgain(true, false, Moment.SOUND, 60 * second))
        assertFalse(p.askAgain(false, true, Moment.SOUND, 60 * second))
    }

    @Test
    fun aPauseWhoseGainNeverCameIsAskedBack() {
        // A transient loss is the platform's to end: not asked back at once, but sound that keeps playing
        // asks after the grace period, and the user coming back asks at once.
        p.requested(AudioFocusPolicy.REQUEST_GRANTED, 0)
        p.changed(AudioFocusPolicy.LOSS_TRANSIENT, second)
        assertEquals(0f, p.volume)
        assertFalse("a call: the platform gives focus back itself", p.askAgain(true, true, Moment.SOUND, 2 * second))
        assertFalse(p.askAgain(true, true, Moment.SOUND, second + AudioFocusPolicy.TRANSIENT_GRACE_NANOS - 1))
        val later = second + AudioFocusPolicy.TRANSIENT_GRACE_NANOS
        assertTrue("the gain never came: asked again", p.askAgain(true, true, Moment.SOUND, later))
        p.asking(later)
        p.requested(AudioFocusPolicy.REQUEST_GRANTED, later, again = true)
        assertEquals(1f, p.volume)
        assertFalse(p.blocked)
        // Coming back to the app asks at once.
        p.changed(AudioFocusPolicy.LOSS_TRANSIENT, later + second)
        assertTrue(p.askAgain(true, true, Moment.BACK, later + 3 * second))
    }

    @Test
    fun aPauseDuringACallStaysMutedAndSaysSo() {
        // The call still holds focus: the ask again is refused (or delayed); the call's mute stands and the
        // app tells the user once (blocked), then the platform's gain ends it.
        p.requested(AudioFocusPolicy.REQUEST_GRANTED, 0)
        p.changed(AudioFocusPolicy.LOSS_TRANSIENT, 0)
        val later = AudioFocusPolicy.TRANSIENT_GRACE_NANOS
        assertTrue(p.askAgain(true, true, Moment.SOUND, later))
        p.asking(later)
        p.requested(AudioFocusPolicy.REQUEST_FAILED, later, again = true)
        assertEquals("a refusal during the call keeps the call's mute", AudioFocusPolicy.State.PAUSED, p.state)
        assertEquals(0f, p.volume)
        assertTrue(p.blocked)
        assertFalse("rate limited", p.askAgain(true, true, Moment.SOUND, later + second))
        assertTrue(p.askAgain(true, true, Moment.SOUND, later + AudioFocusPolicy.RETRY_NANOS))
        p.asking(later + AudioFocusPolicy.RETRY_NANOS)
        p.requested(AudioFocusPolicy.REQUEST_DELAYED, later + AudioFocusPolicy.RETRY_NANOS, again = true)
        assertEquals(AudioFocusPolicy.State.WAITING, p.state)
        assertTrue(p.blocked)
        p.changed(AudioFocusPolicy.GAIN, later + 20 * second)
        assertEquals(1f, p.volume)
        assertFalse("sound again: nothing to say", p.blocked)
    }

    @Test
    fun aDelayedGrantThatNeverCameIsAskedBack() {
        p.requested(AudioFocusPolicy.REQUEST_DELAYED, 0)
        assertEquals(AudioFocusPolicy.State.WAITING, p.state)
        assertFalse(p.askAgain(true, true, Moment.SOUND, 5 * second))
        assertTrue(p.askAgain(true, true, Moment.SOUND, AudioFocusPolicy.TRANSIENT_GRACE_NANOS))
        p.asking(AudioFocusPolicy.TRANSIENT_GRACE_NANOS)
        p.requested(AudioFocusPolicy.REQUEST_GRANTED, AudioFocusPolicy.TRANSIENT_GRACE_NANOS, again = true)
        assertEquals(1f, p.volume)
        // The user coming back asks a waiting grant at once.
        val q = AudioFocusPolicy()
        q.requested(AudioFocusPolicy.REQUEST_DELAYED, 0)
        assertTrue(q.askAgain(true, true, Moment.BACK, second))
        q.asking(second)
        assertFalse("not twice within a second", q.askAgain(true, true, Moment.BACK, second + 1))
    }

    @Test
    fun aRefusalAfterALossPlaysWithoutFocus() {
        // Another player took it for good and the ask again is refused: the sound plays without focus
        // (never silent for a refusal alone) and nothing is said.
        p.requested(AudioFocusPolicy.REQUEST_GRANTED, 0)
        p.changed(AudioFocusPolicy.LOSS, 0)
        p.asking(second)
        p.requested(AudioFocusPolicy.REQUEST_FAILED, second, again = true)
        assertEquals(AudioFocusPolicy.State.NONE, p.state)
        assertEquals(1f, p.volume)
        assertFalse(p.blocked)
        // Playing without focus: the user coming back asks once more, sound alone does not.
        assertFalse(p.askAgain(true, true, Moment.SOUND, 30 * second))
        assertTrue(p.askAgain(true, true, Moment.BACK, 30 * second))
    }

    @Test
    fun releaseForgetsTheFocus() {
        p.requested(AudioFocusPolicy.REQUEST_GRANTED)
        p.changed(AudioFocusPolicy.LOSS_TRANSIENT)
        p.released()
        assertEquals(1f, p.volume)
        assertTrue(p.shouldRequest)
        assertFalse(p.blocked)
    }
}
