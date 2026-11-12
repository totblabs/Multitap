package me.totb.multitap_client

import android.view.KeyEvent

/**
 * A hardware keyboard's Android key events as the Windows key events the Host
 * injects (`{"event":"key","vk",...}`), so a Bluetooth/USB keyboard works like
 * the PC's: Ctrl/Alt/Shift/Win combinations, function keys, the numpad and the
 * Korean 한/영 and 한자 keys (HID LANG1/LANG2, which AOSP Generic.kl maps to
 * KANA/EISU) reach the remote, whose own IME composes Hangul. Android
 * Back/Home/volume and keys without a Windows meaning stay local (null).
 *
 * Android's window manager keeps some events from the app: the Meta (Win)
 * key's own down and up always, Alt's release after Alt+Tab opened Recents.
 * So before every other key the modifiers its metaState reports are made true
 * on the remote: one missing is pressed for that key only (Win+E arrives as E
 * with META_META_ON) and released after it; one no longer held is released.
 * A stale Alt or Win released with no other key since its press, here or when
 * capture ends, gets the menu mask key first, so the remote never sees a lone
 * Alt (menu bar) or Win (Start) tap the user did not make.
 */
class AndroidKeyMap {
    data class Key(val vk: Int, val down: Boolean, val extended: Boolean = vk in EXTENDED)

    private class Modifier(val mask: Int, val left: Int, val right: Int, val leftVk: Int, val rightVk: Int)

    private val held = LinkedHashSet<Int>()     // modifier VKs down on the remote
    private val borrowed = LinkedHashSet<Int>() // ...of those, pressed for one key only
    private val pressed = HashMap<Int, Key>()   // keyCode -> the VK its down sent
    private var bare = false                    // a modifier went down, no other key since

    fun map(keyCode: Int, down: Boolean, metaState: Int): List<Key>? {
        val modifier = MODIFIER_KEYS[keyCode]
        if (modifier != null) {
            borrowed.remove(modifier)
            if (!down) held.remove(modifier) else if (held.add(modifier)) bare = true
            return listOf(Key(modifier, down))
        }
        val key = (if (down) null else pressed.remove(keyCode)) ?: vk(keyCode, metaState) ?: return null
        val out = ArrayList<Key>()
        if (down) {
            pressed[keyCode] = key
            sync(metaState, out)
            bare = false
        }
        out += key.copy(down = down)
        if (!down) {
            for (vk in borrowed) out += Key(vk, false)
            held.removeAll(borrowed)
            borrowed.clear()
        }
        return out
    }

    /** Input capture ended or changed (the Host releases every key): what to send first, then forget. */
    fun release(): List<Key> {
        val out = if (bare && held.any { it in MASKED }) mask() else emptyList()
        held.clear(); borrowed.clear(); pressed.clear(); bare = false
        return out
    }

    private fun sync(metaState: Int, out: MutableList<Key>) {
        val lift = ArrayList<Int>()
        val press = ArrayList<Int>()
        for (m in MODIFIERS) {
            val mine = listOf(m.leftVk, m.rightVk).filter { it in held }
            if (metaState and m.mask == 0) lift += mine
            else if (mine.isEmpty()) press += if (metaState and m.right != 0 && metaState and m.left == 0) m.rightVk else m.leftVk
        }
        if (bare && lift.any { it in MASKED }) out += mask()
        for (vk in lift) out += Key(vk, false)
        for (vk in press) out += Key(vk, true)
        held.removeAll(lift.toSet()); borrowed.removeAll(lift.toSet())
        held += press; borrowed += press
    }

    private fun mask() = listOf(Key(MASK_VK, true), Key(MASK_VK, false))

    private fun vk(code: Int, meta: Int): Key? {
        val numLock = meta and KeyEvent.META_NUM_LOCK_ON != 0
        val vk = when (code) {
            in KeyEvent.KEYCODE_A..KeyEvent.KEYCODE_Z -> 0x41 + code - KeyEvent.KEYCODE_A
            in KeyEvent.KEYCODE_0..KeyEvent.KEYCODE_9 -> 0x30 + code - KeyEvent.KEYCODE_0
            in KeyEvent.KEYCODE_F1..KeyEvent.KEYCODE_F12 -> 0x70 + code - KeyEvent.KEYCODE_F1
            // The numpad as Android reads it: digits with NumLock, its own (not extended) navigation keys without.
            in KeyEvent.KEYCODE_NUMPAD_0..KeyEvent.KEYCODE_NUMPAD_9 -> return Key(
                if (numLock) 0x60 + code - KeyEvent.KEYCODE_NUMPAD_0 else NUMPAD_NAVIGATION[code - KeyEvent.KEYCODE_NUMPAD_0], true, false)
            KeyEvent.KEYCODE_NUMPAD_DOT -> return Key(if (numLock) 0x6E else 0x2E, true, false)
            KeyEvent.KEYCODE_NUMPAD_ENTER -> return Key(0x0D, true, true)
            KeyEvent.KEYCODE_NUMPAD_MULTIPLY -> 0x6A; KeyEvent.KEYCODE_NUMPAD_ADD -> 0x6B
            KeyEvent.KEYCODE_NUMPAD_SUBTRACT -> 0x6D; KeyEvent.KEYCODE_NUMPAD_DIVIDE -> 0x6F
            KeyEvent.KEYCODE_ENTER -> 0x0D
            KeyEvent.KEYCODE_TAB -> 0x09; KeyEvent.KEYCODE_ESCAPE -> 0x1B
            KeyEvent.KEYCODE_DEL -> 0x08; KeyEvent.KEYCODE_FORWARD_DEL -> 0x2E
            KeyEvent.KEYCODE_SPACE -> 0x20; KeyEvent.KEYCODE_CAPS_LOCK -> 0x14
            KeyEvent.KEYCODE_DPAD_LEFT -> 0x25; KeyEvent.KEYCODE_DPAD_UP -> 0x26
            KeyEvent.KEYCODE_DPAD_RIGHT -> 0x27; KeyEvent.KEYCODE_DPAD_DOWN -> 0x28
            KeyEvent.KEYCODE_MOVE_HOME -> 0x24; KeyEvent.KEYCODE_MOVE_END -> 0x23
            KeyEvent.KEYCODE_PAGE_UP -> 0x21; KeyEvent.KEYCODE_PAGE_DOWN -> 0x22
            KeyEvent.KEYCODE_INSERT -> 0x2D; KeyEvent.KEYCODE_MINUS -> 0xBD
            KeyEvent.KEYCODE_EQUALS -> 0xBB; KeyEvent.KEYCODE_LEFT_BRACKET -> 0xDB
            KeyEvent.KEYCODE_RIGHT_BRACKET -> 0xDD; KeyEvent.KEYCODE_BACKSLASH -> 0xDC
            KeyEvent.KEYCODE_SEMICOLON -> 0xBA; KeyEvent.KEYCODE_APOSTROPHE -> 0xDE
            KeyEvent.KEYCODE_GRAVE -> 0xC0; KeyEvent.KEYCODE_COMMA -> 0xBC
            KeyEvent.KEYCODE_PERIOD -> 0xBE; KeyEvent.KEYCODE_SLASH -> 0xBF
            KeyEvent.KEYCODE_SYSRQ -> 0x2C; KeyEvent.KEYCODE_BREAK -> 0x13
            KeyEvent.KEYCODE_SCROLL_LOCK -> 0x91; KeyEvent.KEYCODE_MENU -> 0x5D
            // VK_HANGUL / VK_HANJA: the remote IME's own 한/영 and 한자.
            KeyEvent.KEYCODE_KANA, KeyEvent.KEYCODE_LANGUAGE_SWITCH -> 0x15
            KeyEvent.KEYCODE_EISU -> 0x19
            // A Chromebook-style Search key is that keyboard's Windows key; left
            // to Android it would open Android search over the session.
            KeyEvent.KEYCODE_SEARCH -> 0x5B
            else -> return null // Android Back/Home/volume and unsupported keys stay local.
        }
        return Key(vk, true)
    }

    companion object {
        private val EXTENDED = setOf(0x5B, 0x5C, 0x5D, 0xA3, 0xA5, 0x25, 0x26, 0x27, 0x28, 0x21, 0x22, 0x23, 0x24, 0x2C, 0x2D, 0x2E, 0x6F)
        /** An unassigned VK (AutoHotkey's menu mask key): pressed between Alt/Win and its release, nothing happens. */
        private const val MASK_VK = 0xE8
        private val MASKED = setOf(0xA4, 0xA5, 0x5B, 0x5C)
        // NumLock off: Insert, End, Down, Page Down, Left, Clear, Right, Home, Up, Page Up.
        private val NUMPAD_NAVIGATION = intArrayOf(0x2D, 0x23, 0x28, 0x22, 0x25, 0x0C, 0x27, 0x24, 0x26, 0x21)
        private val MODIFIER_KEYS = mapOf(
            KeyEvent.KEYCODE_SHIFT_LEFT to 0xA0, KeyEvent.KEYCODE_SHIFT_RIGHT to 0xA1,
            KeyEvent.KEYCODE_CTRL_LEFT to 0xA2, KeyEvent.KEYCODE_CTRL_RIGHT to 0xA3,
            KeyEvent.KEYCODE_ALT_LEFT to 0xA4, KeyEvent.KEYCODE_ALT_RIGHT to 0xA5,
            KeyEvent.KEYCODE_META_LEFT to 0x5B, KeyEvent.KEYCODE_META_RIGHT to 0x5C,
        )
        private val MODIFIERS = listOf(
            Modifier(KeyEvent.META_CTRL_MASK, KeyEvent.META_CTRL_LEFT_ON, KeyEvent.META_CTRL_RIGHT_ON, 0xA2, 0xA3),
            Modifier(KeyEvent.META_ALT_MASK, KeyEvent.META_ALT_LEFT_ON, KeyEvent.META_ALT_RIGHT_ON, 0xA4, 0xA5),
            Modifier(KeyEvent.META_SHIFT_MASK, KeyEvent.META_SHIFT_LEFT_ON, KeyEvent.META_SHIFT_RIGHT_ON, 0xA0, 0xA1),
            Modifier(KeyEvent.META_META_MASK, KeyEvent.META_META_LEFT_ON, KeyEvent.META_META_RIGHT_ON, 0x5B, 0x5C),
        )
    }
}
