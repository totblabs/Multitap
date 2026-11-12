package me.totb.multitap_client

import android.view.KeyEvent
import org.junit.Assert.assertEquals
import org.junit.Assert.assertNull
import org.junit.Test

// AndroidKeyMap without a device, a keyboard or a VM: Android key events from
// a Bluetooth/USB keyboard (keyCode, down, metaState) become the Windows key
// events the Host injects. Key codes and meta bits are the android.jar
// constants; AOSP Generic.kl maps HID LANG1/LANG2 (한/영, 한자) to KANA/EISU.
class AndroidKeyMapTest {
    private val ctrl = KeyEvent.META_CTRL_ON or KeyEvent.META_CTRL_LEFT_ON
    private val alt = KeyEvent.META_ALT_ON or KeyEvent.META_ALT_LEFT_ON
    private val shift = KeyEvent.META_SHIFT_ON or KeyEvent.META_SHIFT_LEFT_ON
    private val meta = KeyEvent.META_META_ON or KeyEvent.META_META_LEFT_ON
    private val numLock = KeyEvent.META_NUM_LOCK_ON

    /** "+A2 +43 -43 -A2": + down, - up, hex VK, * extended. */
    private fun AndroidKeyMap.press(code: Int, metaState: Int = 0) =
        listOf(true, false).flatMap { down -> map(code, down, metaState) ?: emptyList() }

    private fun AndroidKeyMap.down(code: Int, metaState: Int = 0) = map(code, true, metaState)
    private fun AndroidKeyMap.up(code: Int, metaState: Int = 0) = map(code, false, metaState)

    private fun show(keys: List<AndroidKeyMap.Key>?) = keys?.joinToString(" ") {
        (if (it.down) "+" else "-") + Integer.toHexString(it.vk).uppercase() + (if (it.extended) "*" else "")
    }

    @Test fun koreanLanguageKeys() {
        val map = AndroidKeyMap()
        // HID LANG1 (한/영) arrives as KANA, LANG2 (한자) as EISU: VK_HANGUL, VK_HANJA.
        assertEquals("+15 -15", show(map.press(KeyEvent.KEYCODE_KANA)))
        assertEquals("+19 -19", show(map.press(KeyEvent.KEYCODE_EISU)))
        assertEquals("+15 -15", show(map.press(KeyEvent.KEYCODE_LANGUAGE_SWITCH)))
    }

    @Test fun modifierCombinationsWithTheirOwnEvents() {
        val map = AndroidKeyMap()
        // Ctrl+C as Windows sends it: the modifier's own events, in order.
        assertEquals("+A2", show(map.down(KeyEvent.KEYCODE_CTRL_LEFT, ctrl)))
        assertEquals("+43", show(map.down(KeyEvent.KEYCODE_C, ctrl)))
        assertEquals("-43", show(map.up(KeyEvent.KEYCODE_C, ctrl)))
        assertEquals("-A2", show(map.up(KeyEvent.KEYCODE_CTRL_LEFT)))
        // Ctrl+Shift+Esc, Right Alt and Right Ctrl keep their side and extended flag.
        assertEquals("+A2 +A0 +1B -1B -A0 -A2", show(
            map.down(KeyEvent.KEYCODE_CTRL_LEFT, ctrl)!! + map.down(KeyEvent.KEYCODE_SHIFT_LEFT, ctrl or shift)!! +
                map.press(KeyEvent.KEYCODE_ESCAPE, ctrl or shift) + map.up(KeyEvent.KEYCODE_SHIFT_LEFT, ctrl)!! +
                map.up(KeyEvent.KEYCODE_CTRL_LEFT)!!))
        assertEquals("+A5*", show(map.down(KeyEvent.KEYCODE_ALT_RIGHT, KeyEvent.META_ALT_ON or KeyEvent.META_ALT_RIGHT_ON)))
        assertEquals("-A5*", show(map.up(KeyEvent.KEYCODE_ALT_RIGHT)))
        // A held key repeats like on Windows (no extra modifier events).
        assertEquals("+A3*", show(map.down(KeyEvent.KEYCODE_CTRL_RIGHT, KeyEvent.META_CTRL_ON or KeyEvent.META_CTRL_RIGHT_ON)))
        assertEquals("+41 +41 -41", show(
            map.down(KeyEvent.KEYCODE_A, KeyEvent.META_CTRL_ON or KeyEvent.META_CTRL_RIGHT_ON)!! +
                map.down(KeyEvent.KEYCODE_A, KeyEvent.META_CTRL_ON or KeyEvent.META_CTRL_RIGHT_ON)!! +
                map.up(KeyEvent.KEYCODE_A, KeyEvent.META_CTRL_ON or KeyEvent.META_CTRL_RIGHT_ON)!!))
        assertEquals("-A3*", show(map.up(KeyEvent.KEYCODE_CTRL_RIGHT)))
    }

    @Test fun winComboWhoseMetaEventsAndroidKeeps() {
        // Android's window manager consumes the Meta key's own down and up:
        // Win+E arrives as E with META_ON. It must not type a plain "e".
        val map = AndroidKeyMap()
        assertEquals("+5B* +45 -45 -5B*", show(map.press(KeyEvent.KEYCODE_E, meta)))
        // Win+Shift+S: Shift's own events arrive, Win's are borrowed for S only.
        assertEquals("+A0", show(map.down(KeyEvent.KEYCODE_SHIFT_LEFT, meta or shift)))
        assertEquals("+5B* +53 -53 -5B*", show(map.press(KeyEvent.KEYCODE_S, meta or shift)))
        assertEquals("-A0", show(map.up(KeyEvent.KEYCODE_SHIFT_LEFT, meta)))
        // Right Meta only: the right Windows key.
        assertEquals("+5C* +44 -44 -5C*", show(map.press(KeyEvent.KEYCODE_D,
            KeyEvent.META_META_ON or KeyEvent.META_META_RIGHT_ON)))
        // A modifier pressed before capture began (its down never sent) still applies.
        assertEquals("+A2 +56 -56 -A2", show(map.press(KeyEvent.KEYCODE_V, ctrl)))
        assertEquals("-A2", show(map.up(KeyEvent.KEYCODE_CTRL_LEFT)))
    }

    @Test fun altTabNeverLeavesALoneAltOnTheRemote() {
        // Alt reaches the app, Tab and Alt's release open and close Recents.
        val map = AndroidKeyMap()
        assertEquals("+A4", show(map.down(KeyEvent.KEYCODE_ALT_LEFT, alt)))
        // The window loses focus: the menu mask key goes before the Host's
        // reset lifts Alt, so the remote app's menu bar does not activate.
        assertEquals("+E8 -E8", show(map.release()))
        assertEquals("", show(map.release()))
        // Without a focus loss: the stale Alt is lifted (masked) before the next key.
        assertEquals("+A4", show(map.down(KeyEvent.KEYCODE_ALT_LEFT, alt)))
        assertEquals("+E8 -E8 -A4 +41", show(map.down(KeyEvent.KEYCODE_A)))
        assertEquals("-41", show(map.up(KeyEvent.KEYCODE_A)))
        // An Alt used in a combination needs no mask; a lone Alt tap the user
        // made on purpose stays a lone Alt tap.
        assertEquals("+A4 +73 -73", show(map.down(KeyEvent.KEYCODE_ALT_LEFT, alt)!! + map.press(KeyEvent.KEYCODE_F4, alt)))
        assertEquals("", show(map.release()))
        assertEquals("+A4 -A4", show(map.press(KeyEvent.KEYCODE_ALT_LEFT, alt)))
    }

    @Test fun releaseForgetsHeldModifiers() {
        // The Host releases every key on input.reset; the next combination presses Ctrl again.
        val map = AndroidKeyMap()
        map.down(KeyEvent.KEYCODE_CTRL_LEFT, ctrl)
        map.press(KeyEvent.KEYCODE_C, ctrl)
        map.release()
        assertEquals("+A2 +56 -56 -A2", show(map.press(KeyEvent.KEYCODE_V, ctrl)))
    }

    @Test fun functionAndEditingKeys() {
        val map = AndroidKeyMap()
        assertEquals("+70 -70", show(map.press(KeyEvent.KEYCODE_F1)))
        assertEquals("+7B -7B", show(map.press(KeyEvent.KEYCODE_F12)))
        assertEquals("+1B -1B", show(map.press(KeyEvent.KEYCODE_ESCAPE)))
        assertEquals("+2E* -2E*", show(map.press(KeyEvent.KEYCODE_FORWARD_DEL)))
        assertEquals("+2C* -2C*", show(map.press(KeyEvent.KEYCODE_SYSRQ)))
        assertEquals("+13 -13", show(map.press(KeyEvent.KEYCODE_BREAK)))
        assertEquals("+91 -91", show(map.press(KeyEvent.KEYCODE_SCROLL_LOCK)))
        assertEquals("+5D* -5D*", show(map.press(KeyEvent.KEYCODE_MENU)))
        // Search (Chromebook-style keyboards) is the Windows key, never Android search.
        assertEquals("+5B* -5B*", show(map.press(KeyEvent.KEYCODE_SEARCH)))
        // Android's own keys stay local.
        for (code in listOf(KeyEvent.KEYCODE_BACK, KeyEvent.KEYCODE_HOME, KeyEvent.KEYCODE_VOLUME_UP, KeyEvent.KEYCODE_APP_SWITCH)) {
            assertNull(map.down(code))
        }
    }

    @Test fun numpadFollowsNumLock() {
        val map = AndroidKeyMap()
        assertEquals("+61 -61", show(map.press(KeyEvent.KEYCODE_NUMPAD_1, numLock)))
        assertEquals("+6E -6E", show(map.press(KeyEvent.KEYCODE_NUMPAD_DOT, numLock)))
        assertEquals("+6F* -6F*", show(map.press(KeyEvent.KEYCODE_NUMPAD_DIVIDE, numLock)))
        assertEquals("+6A -6A +6B -6B +6D -6D", show(map.press(KeyEvent.KEYCODE_NUMPAD_MULTIPLY) +
            map.press(KeyEvent.KEYCODE_NUMPAD_ADD) + map.press(KeyEvent.KEYCODE_NUMPAD_SUBTRACT)))
        assertEquals("+D* -D*", show(map.press(KeyEvent.KEYCODE_NUMPAD_ENTER)))
        // NumLock off: the navigation key, not extended (the numpad's own Home/arrows).
        assertEquals("+24 -24", show(map.press(KeyEvent.KEYCODE_NUMPAD_7)))
        assertEquals("+2E -2E", show(map.press(KeyEvent.KEYCODE_NUMPAD_DOT)))
        // The release matches the press even when NumLock changed meanwhile.
        assertEquals("+68", show(map.down(KeyEvent.KEYCODE_NUMPAD_8, numLock)))
        assertEquals("-68", show(map.up(KeyEvent.KEYCODE_NUMPAD_8)))
    }
}
