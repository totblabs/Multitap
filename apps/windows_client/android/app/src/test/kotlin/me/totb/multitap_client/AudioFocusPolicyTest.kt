package me.totb.multitap_client

import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

// AudioFocusPolicy (AndroidAudioFocus.kt, 2026-11-10): the VM sound follows audio focus by volume only.
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
    fun releaseForgetsTheFocus() {
        p.requested(AudioFocusPolicy.REQUEST_GRANTED)
        p.changed(AudioFocusPolicy.LOSS_TRANSIENT)
        p.released()
        assertEquals(1f, p.volume)
        assertTrue(p.shouldRequest)
    }
}
