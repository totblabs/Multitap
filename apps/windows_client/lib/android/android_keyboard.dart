import 'package:flutter/services.dart';

/// One Windows key press (down then up) with Shift when [shift].
class AndroidKeyStroke {
  const AndroidKeyStroke(this.vk, {this.shift = false, this.extended = false});
  final int vk;
  final bool shift, extended;

  static const backspace = AndroidKeyStroke(0x08);
  static const enter = AndroidKeyStroke(0x0d);
  static const tab = AndroidKeyStroke(0x09);
  static const space = AndroidKeyStroke(0x20);
  static const left = AndroidKeyStroke(0x25, extended: true);
  static const right = AndroidKeyStroke(0x27, extended: true);

  @override
  bool operator ==(Object other) =>
      other is AndroidKeyStroke &&
      other.vk == vk &&
      other.shift == shift &&
      other.extended == extended;

  @override
  int get hashCode => Object.hash(vk, shift, extended);

  @override
  String toString() => '${shift ? 'Shift+' : ''}0x${vk.toRadixString(16)}';
}

/// Keys that Windows reports with the extended flag (the Windows client and
/// the Android hardware keyboard path use the same set).
const androidExtendedKeys = {
  0x5b, 0x5c, 0xa3, 0xa5, 0x21, 0x22, 0x23, 0x24, //
  0x25, 0x26, 0x27, 0x28, 0x2d, 0x2e,
};

// US / Korean keyboard positions (Korean keyboards use the US OEM layout).
const _oem = {
  '-': 0xbd,
  '=': 0xbb,
  '[': 0xdb,
  ']': 0xdd,
  r'\': 0xdc,
  ';': 0xba,
  "'": 0xde,
  ',': 0xbc,
  '.': 0xbe,
  '/': 0xbf,
  '`': 0xc0,
};
const _shifted = {
  '!': '1',
  '@': '2',
  '#': '3',
  r'$': '4',
  '%': '5',
  '^': '6',
  '&': '7',
  '*': '8',
  '(': '9',
  ')': '0',
  '_': '-',
  '+': '=',
  '{': '[',
  '}': ']',
  '|': r'\',
  ':': ';',
  '"': "'",
  '<': ',',
  '>': '.',
  '?': '/',
  '~': '`',
};

AndroidKeyStroke? _ascii(String c) {
  final code = c.codeUnitAt(0);
  if (code >= 0x61 && code <= 0x7a) return AndroidKeyStroke(code - 0x20);
  if (code >= 0x41 && code <= 0x5a) {
    return AndroidKeyStroke(code, shift: true);
  }
  if (code >= 0x30 && code <= 0x39) return AndroidKeyStroke(code);
  final oem = _oem[c];
  if (oem != null) return AndroidKeyStroke(oem);
  final base = _shifted[c];
  if (base != null) {
    final stroke = _ascii(base)!;
    return AndroidKeyStroke(stroke.vk, shift: true);
  }
  return switch (c) {
    ' ' => AndroidKeyStroke.space,
    '\n' => AndroidKeyStroke.enter,
    '\t' => AndroidKeyStroke.tab,
    _ => null,
  };
}

// The standard Korean 2-set (두벌식) keyboard: the key for each jamo.
const _jamoKeys = {
  'ㄱ': 'r', 'ㄲ': 'R', 'ㄳ': 'rt', 'ㄴ': 's', 'ㄵ': 'sw', 'ㄶ': 'sg', //
  'ㄷ': 'e', 'ㄸ': 'E', 'ㄹ': 'f', 'ㄺ': 'fr', 'ㄻ': 'fa', 'ㄼ': 'fq',
  'ㄽ': 'ft', 'ㄾ': 'fx', 'ㄿ': 'fv', 'ㅀ': 'fg', 'ㅁ': 'a', 'ㅂ': 'q',
  'ㅃ': 'Q', 'ㅄ': 'qt', 'ㅅ': 't', 'ㅆ': 'T', 'ㅇ': 'd', 'ㅈ': 'w',
  'ㅉ': 'W', 'ㅊ': 'c', 'ㅋ': 'z', 'ㅌ': 'x', 'ㅍ': 'v', 'ㅎ': 'g',
  'ㅏ': 'k', 'ㅐ': 'o', 'ㅑ': 'i', 'ㅒ': 'O', 'ㅓ': 'j', 'ㅔ': 'p',
  'ㅕ': 'u', 'ㅖ': 'P', 'ㅗ': 'h', 'ㅘ': 'hk', 'ㅙ': 'ho', 'ㅚ': 'hl',
  'ㅛ': 'y', 'ㅜ': 'n', 'ㅝ': 'nj', 'ㅞ': 'np', 'ㅟ': 'nl', 'ㅠ': 'b',
  'ㅡ': 'm', 'ㅢ': 'ml', 'ㅣ': 'l',
};
const _initials = 'ㄱㄲㄴㄷㄸㄹㅁㅂㅃㅅㅆㅇㅈㅉㅊㅋㅌㅍㅎ';
const _medials = 'ㅏㅐㅑㅒㅓㅔㅕㅖㅗㅘㅙㅚㅛㅜㅝㅞㅟㅠㅡㅢㅣ';
const _finals = [
  '', 'ㄱ', 'ㄲ', 'ㄳ', 'ㄴ', 'ㄵ', 'ㄶ', 'ㄷ', 'ㄹ', 'ㄺ', 'ㄻ', 'ㄼ', 'ㄽ', //
  'ㄾ', 'ㄿ', 'ㅀ', 'ㅁ', 'ㅂ', 'ㅄ', 'ㅅ', 'ㅆ', 'ㅇ', 'ㅈ', 'ㅊ', 'ㅋ',
  'ㅌ', 'ㅍ', 'ㅎ',
];

bool androidIsHangul(int rune) =>
    (rune >= 0xac00 && rune <= 0xd7a3) || (rune >= 0x3131 && rune <= 0x3163);

/// The 2-set keys (as US letters) that type [rune] on a Windows Korean IME
/// in Hangul mode; null for anything that is not a Hangul syllable or jamo.
String? androidHangulKeys(int rune) {
  if (rune >= 0x3131 && rune <= 0x3163) {
    return _jamoKeys[String.fromCharCode(rune)];
  }
  if (rune < 0xac00 || rune > 0xd7a3) return null;
  final code = rune - 0xac00;
  return '${_jamoKeys[_initials[code ~/ 588]]}'
      '${_jamoKeys[_medials[(code % 588) ~/ 28]]}'
      '${code % 28 == 0 ? '' : _jamoKeys[_finals[code % 28]]}';
}

/// Key presses that type [rune]: ASCII on the US/Korean layout, Hangul as
/// 2-set keys (the remote IME composes them in Hangul mode). Null when the
/// existing key protocol cannot type it (no Unicode input message exists).
List<AndroidKeyStroke>? androidStrokesForRune(int rune) {
  if (rune < 0x80) {
    final stroke = _ascii(String.fromCharCode(rune));
    return stroke == null ? null : [stroke];
  }
  final keys = androidHangulKeys(rune);
  if (keys == null) return null;
  return [for (final key in keys.split('')) _ascii(key)!];
}

/// What one edit of the hidden text field means for the remote.
class AndroidTypingUpdate {
  const AndroidTypingUpdate(
    this.actions, {
    this.reset,
    this.hangul = false,
    this.unsupported = false,
  });

  /// In order: [AndroidKeyStroke] presses and, with text-input-v1, committed
  /// [String] text.
  final List<Object> actions;

  /// The key presses among [actions].
  List<AndroidKeyStroke> get strokes => [
    for (final action in actions)
      if (action is AndroidKeyStroke) action,
  ];

  /// The committed text among [actions], joined.
  String get text => [
    for (final action in actions)
      if (action is String) action,
  ].join();

  /// The field's next value, when it should be emptied (a cursor move,
  /// a deleted sentinel or text the protocol cannot type).
  final TextEditingValue? reset;

  /// Hangul was typed as keys (it appears only when the remote IME is in
  /// Hangul mode) / something could not be typed and was dropped.
  final bool hangul, unsupported;
}

/// Mirrors the phone keyboard's edits of an invisible field.
///
/// The field always starts with [sentinel] (two zero-width spaces) so a
/// Backspace with nothing typed still reaches us as a deletion. A cursor
/// move by the keyboard (e.g. a space-bar swipe) becomes arrow keys.
///
/// With [text] (text-input-v1 granted): the field's text is mirrored as
/// Unicode text, so Korean, other scripts and emoji arrive exactly as typed
/// whatever the remote IME mode. What the keyboard is still composing is
/// mirrored live too, like typing on the PC (there is no local
/// "입력 중" chip): a changing syllable is replaced with
/// Backspace + the new syllable (하 → 한). Deleting text, Enter and Tab are
/// keys.
///
/// Without it (an older Host): additions become key presses on the US/Korean
/// layout and Hangul becomes 2-set keys; the syllable the remote IME is still
/// composing changes jamo by jamo (one Backspace removes one jamo there).
class AndroidTypingBridge {
  AndroidTypingBridge({this.text = false});
  final bool text;

  static const sentinel = '​​';
  static const initial = TextEditingValue(
    text: sentinel,
    selection: TextSelection.collapsed(offset: sentinel.length),
  );

  String _sent = '';
  bool _remoteComposing = false;
  TextEditingValue _last = initial;

  /// The remote IME is composing the last syllable it was sent (keys mode).
  bool get remoteComposing => _remoteComposing;

  /// Something other than typing ended the remote composition (a click, a
  /// key from the key row); the field starts over.
  void commitRemote() {
    _remoteComposing = false;
  }

  void clear() {
    _sent = '';
    _remoteComposing = false;
    _last = initial;
  }

  /// Text still to send before the field is emptied (a key-row key, a click,
  /// hiding the keyboard): none, the composition is already on the remote.
  String get pending => '';

  static bool _composing(TextEditingValue value) =>
      value.composing.isValid && !value.composing.isCollapsed;

  static String _visible(String value) => value.replaceAll('​', '');

  static String _canonicalNewlines(String value) =>
      value.replaceAll('\r\n', '\n').replaceAll('\r', '\n');

  /// Committed text (outside the composing range), composing text and all of
  /// it in field order, after the sentinel.
  static ({String committed, String composing, String full, int missing})
  _parts(TextEditingValue value) {
    final text = value.text;
    var lead = 0;
    while (lead < sentinel.length &&
        lead < text.length &&
        text.codeUnitAt(lead) == 0x200b) {
      lead++;
    }
    var start = text.length, end = text.length;
    if (_composing(value)) {
      start = value.composing.start.clamp(lead, text.length);
      end = value.composing.end.clamp(start, text.length);
    }
    return (
      committed: _visible(text.substring(lead, start) + text.substring(end)),
      composing: _visible(text.substring(start, end)),
      full: _visible(text.substring(lead)),
      missing: sentinel.length - lead,
    );
  }

  /// A keyboard cursor move (text unchanged): arrow keys, then start over.
  AndroidTypingUpdate? _caret(TextEditingValue value, bool composing) {
    final previous = _last;
    final before = previous.selection, after = value.selection;
    _last = value;
    if (composing ||
        !before.isValid ||
        !after.isValid ||
        !after.isCollapsed ||
        after.baseOffset == before.baseOffset) {
      return null;
    }
    final int delta;
    if (text) {
      if (before.baseOffset > previous.text.length ||
          before.extentOffset > previous.text.length ||
          after.baseOffset > value.text.length ||
          after.extentOffset > value.text.length) {
        return null;
      }
      int logicalOffset(String raw, int offset) =>
          _canonicalNewlines(raw.substring(0, offset)).length;
      delta =
          logicalOffset(value.text, after.baseOffset) -
          logicalOffset(previous.text, before.baseOffset);
    } else {
      delta = after.baseOffset - before.baseOffset;
    }
    // Two raw caret positions inside/after CRLF can name the same remote
    // position. Keep the editing history instead of resetting or sending keys.
    if (delta == 0) return null;
    _sent = '';
    _remoteComposing = false;
    _last = initial;
    return AndroidTypingUpdate([
      for (var i = 0; i < delta.abs().clamp(0, 64); i++)
        delta < 0 ? AndroidKeyStroke.left : AndroidKeyStroke.right,
    ], reset: initial);
  }

  AndroidTypingUpdate update(TextEditingValue value) =>
      text ? _updateText(value) : _updateKeys(value);

  AndroidTypingUpdate _updateText(TextEditingValue value) {
    final composing = _composing(value);
    final parts = _parts(value);
    // Windows Enter represents one logical newline. Keep the same projection
    // for diff/history so CRLF split across edits or a later LF finish cannot
    // emit another Enter or replay already delivered text.
    final full = _canonicalNewlines(parts.full);
    // The remote already shows the whole field (a finished composition with
    // the same characters is nothing new): a caret move or nothing.
    if (value.text == _last.text && full == _sent && parts.missing == 0) {
      return _caret(value, composing) ?? const AndroidTypingUpdate([]);
    }
    final actions = <Object>[];
    var unsupported = false;
    final old = _sent.runes.toList(), next = full.runes.toList();
    var k = 0;
    while (k < old.length && k < next.length && old[k] == next[k]) {
      k++;
    }
    // Deleted committed text (and a Backspace on the empty field) are keys.
    for (var i = k; i < old.length + parts.missing; i++) {
      actions.add(AndroidKeyStroke.backspace);
    }
    final buffer = StringBuffer();
    void flush() {
      if (buffer.isEmpty) return;
      actions.add(buffer.toString());
      buffer.clear();
    }

    for (final rune in next.skip(k)) {
      if (rune == 0x0a || rune == 0x0d) {
        flush();
        actions.add(AndroidKeyStroke.enter);
      } else if (rune == 0x09) {
        flush();
        actions.add(AndroidKeyStroke.tab);
      } else if (rune < 0x20 || (rune >= 0x7f && rune <= 0x9f)) {
        unsupported = true;
      } else {
        buffer.writeCharCode(rune);
      }
    }
    flush();
    _sent = full;
    _last = value;
    TextEditingValue? reset;
    if (!composing && parts.missing > 0) {
      // A deleted sentinel needs a new editing origin. Ordinary commits,
      // including Enter and long text, retain history while the IME owns the
      // field: a later finish/selection snapshot is still the same edit.
      _sent = '';
      _last = reset = initial;
    }
    return AndroidTypingUpdate(actions, reset: reset, unsupported: unsupported);
  }

  AndroidTypingUpdate _updateKeys(TextEditingValue value) {
    final strokes = <AndroidKeyStroke>[];
    final text = value.text;
    final composing = _composing(value);
    if (text == _last.text) {
      return _caret(value, composing) ?? const AndroidTypingUpdate([]);
    }
    var lead = 0;
    while (lead < sentinel.length &&
        lead < text.length &&
        text.codeUnitAt(lead) == 0x200b) {
      lead++;
    }
    final missing = sentinel.length - lead;
    var unsupported = false, hangul = false;
    final typed = <int>[];
    for (final rune in text.substring(lead).runes) {
      if (rune == 0x200b) continue;
      if (androidStrokesForRune(rune) == null) {
        unsupported = true;
        continue;
      }
      typed.add(rune);
    }
    final old = _sent.runes.toList();
    var k = 0;
    while (k < old.length && k < typed.length && old[k] == typed[k]) {
      k++;
    }
    if (_remoteComposing &&
        k == old.length - 1 &&
        k < typed.length &&
        androidIsHangul(old[k]) &&
        androidIsHangul(typed[k])) {
      // The syllable being composed changed: send only the jamo difference.
      final a = androidStrokesForRune(old[k])!;
      final b = androidStrokesForRune(typed[k])!;
      var j = 0;
      while (j < a.length && j < b.length && a[j] == b[j]) {
        j++;
      }
      for (var i = j; i < a.length; i++) {
        strokes.add(AndroidKeyStroke.backspace);
      }
      strokes.addAll(b.skip(j));
      hangul = true;
      _remoteComposing = b.isNotEmpty;
      k++;
    } else {
      for (var i = old.length - 1; i >= k; i--) {
        // While the remote composes the last syllable, Backspace removes one
        // jamo; afterwards one character.
        final count =
            i == old.length - 1 && _remoteComposing && androidIsHangul(old[i])
            ? androidStrokesForRune(old[i])!.length
            : 1;
        for (var n = 0; n < count; n++) {
          strokes.add(AndroidKeyStroke.backspace);
        }
        _remoteComposing = false;
      }
    }
    // Backspace with nothing typed removed a sentinel character.
    for (var i = 0; i < missing; i++) {
      strokes.add(AndroidKeyStroke.backspace);
      _remoteComposing = false;
    }
    for (final rune in typed.skip(k)) {
      strokes.addAll(androidStrokesForRune(rune)!);
      final jamo = androidIsHangul(rune);
      hangul = hangul || jamo;
      _remoteComposing = jamo;
    }
    _sent = String.fromCharCodes(typed);
    _last = value;
    TextEditingValue? reset;
    if (!composing && (missing > 0 || unsupported)) {
      // Start the field over; the remote keeps what it was sent.
      _sent = '';
      _last = reset = initial;
      if (typed.isNotEmpty && androidIsHangul(typed.last)) {
        _remoteComposing = false;
      }
    }
    return AndroidTypingUpdate(
      strokes,
      reset: reset,
      hangul: hangul,
      unsupported: unsupported,
    );
  }
}

/// A key on the remote key row that a hardware key event from the phone
/// keyboard also produces (Enter, Tab, Escape, arrows, editing keys, F1-F12).
AndroidKeyStroke? androidStrokeForKey(LogicalKeyboardKey key) {
  final vk = switch (key) {
    LogicalKeyboardKey.enter || LogicalKeyboardKey.numpadEnter => 0x0d,
    LogicalKeyboardKey.tab => 0x09,
    LogicalKeyboardKey.escape => 0x1b,
    LogicalKeyboardKey.delete => 0x2e,
    LogicalKeyboardKey.arrowLeft => 0x25,
    LogicalKeyboardKey.arrowUp => 0x26,
    LogicalKeyboardKey.arrowRight => 0x27,
    LogicalKeyboardKey.arrowDown => 0x28,
    LogicalKeyboardKey.home => 0x24,
    LogicalKeyboardKey.end => 0x23,
    LogicalKeyboardKey.pageUp => 0x21,
    LogicalKeyboardKey.pageDown => 0x22,
    LogicalKeyboardKey.insert => 0x2d,
    LogicalKeyboardKey.f1 => 0x70,
    LogicalKeyboardKey.f2 => 0x71,
    LogicalKeyboardKey.f3 => 0x72,
    LogicalKeyboardKey.f4 => 0x73,
    LogicalKeyboardKey.f5 => 0x74,
    LogicalKeyboardKey.f6 => 0x75,
    LogicalKeyboardKey.f7 => 0x76,
    LogicalKeyboardKey.f8 => 0x77,
    LogicalKeyboardKey.f9 => 0x78,
    LogicalKeyboardKey.f10 => 0x79,
    LogicalKeyboardKey.f11 => 0x7a,
    LogicalKeyboardKey.f12 => 0x7b,
    _ => null,
  };
  return vk == null
      ? null
      : AndroidKeyStroke(vk, extended: androidExtendedKeys.contains(vk));
}
