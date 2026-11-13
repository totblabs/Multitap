package me.totb.multitap_client

import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

// AudioFocusPolicy (AndroidAudioFocus.kt): the VM sound follows audio focus by volume only.
class AudioFocusPolicyTest {
    private val p = AudioFocusPolicy()

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
    fun aLossWhileTheAppIsInFrontIsAskedBackWhenTheUserReturns() {
        // Another player took focus permanently while the stream was on screen: muted, but not for the
        // rest of the session. Back in the app (window focus), or new VM sound while the user is in it,
        // asks again, and the grant unmutes.
        p.requested(AudioFocusPolicy.REQUEST_GRANTED)
        p.changed(AudioFocusPolicy.LOSS)
        assertEquals(0f, p.volume)
        assertTrue("the user came back to the app", p.askAgain(wanted = true, userHere = true))
        p.requested(AudioFocusPolicy.REQUEST_GRANTED)
        assertEquals(1f, p.volume)
        assertFalse("held again: nothing to ask", p.askAgain(wanted = true, userHere = true))
    }

    @Test
    fun aLossIsNotAskedBackWhileTheUserIsElsewhereOrForACall() {
        p.requested(AudioFocusPolicy.REQUEST_GRANTED)
        p.changed(AudioFocusPolicy.LOSS)
        assertFalse("the user is in another window (split screen)", p.askAgain(wanted = true, userHere = false))
        assertFalse("no stream wants sound", p.askAgain(wanted = false, userHere = true))
        p.changed(AudioFocusPolicy.GAIN)
        p.changed(AudioFocusPolicy.LOSS_TRANSIENT)
        assertFalse("a call: the platform gives focus back itself", p.askAgain(wanted = true, userHere = true))
        assertEquals(0f, p.volume)
        p.changed(AudioFocusPolicy.GAIN)
        assertEquals(1f, p.volume)
    }

    @Test
    fun releaseForgetsTheFocus() {
        p.requested(AudioFocusPolicy.REQUEST_GRANTED)
        p.changed(AudioFocusPolicy.LOSS_TRANSIENT)
        p.released()
        assertEquals(1f, p.volume)
        assertTrue(p.shouldRequest)
    }
}
