// Phone keyboard edits as ordinary Windows key presses: US/Korean layout
// for ASCII, 2-set keys for Hangul. Pure Dart; no IME or host.
import 'package:flutter/services.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:multitap_client/android/android_keyboard.dart';

const _s = AndroidTypingBridge.sentinel;

TextEditingValue _v(String typed, {int? composeFrom}) {
  final text = '$_s$typed';
  return TextEditingValue(
    text: text,
    selection: TextSelection.collapsed(offset: text.length),
    composing: composeFrom == null
        ? TextRange.empty
        : TextRange(start: _s.length + composeFrom, end: text.length),
  );
}

String _keys(List<AndroidKeyStroke> strokes) => strokes
    .map(
      (s) => switch (s.vk) {
        0x08 => '⌫',
        0x0d => '⏎',
        0x20 => '␣',
        0x25 => '←',
        0x27 => '→',
        _ =>
          s.vk >= 0x41 && s.vk <= 0x5a
              ? (s.shift
                    ? String.fromCharCode(s.vk)
                    : String.fromCharCode(s.vk + 0x20))
              : '${s.shift ? '⇧' : ''}${s.vk.toRadixString(16)}',
      },
    )
    .join();

void main() {
  test('ASCII uses the US/Korean key positions', () {
    expect(_keys(androidStrokesForRune('a'.runes.first)!), 'a');
    expect(_keys(androidStrokesForRune('Z'.runes.first)!), 'Z');
    expect(androidStrokesForRune('7'.runes.first), [
      const AndroidKeyStroke(0x37),
    ]);
    expect(androidStrokesForRune('!'.runes.first), [
      const AndroidKeyStroke(0x31, shift: true),
    ]);
    expect(androidStrokesForRune('?'.runes.first), [
      const AndroidKeyStroke(0xbf, shift: true),
    ]);
    expect(androidStrokesForRune(';'.runes.first), [
      const AndroidKeyStroke(0xba),
    ]);
    expect(androidStrokesForRune(0x1f600), isNull); // emoji
    expect(androidStrokesForRune('中'.runes.first), isNull);
  });

  test('Hangul becomes the 2-set keys a Korean keyboard would press', () {
    String keys(String s) => androidHangulKeys(s.runes.first)!;
    expect(keys('한'), 'gks');
    expect(keys('글'), 'rmf');
    expect(keys('까'), 'Rk');
    expect(keys('왜'), 'dho');
    expect(keys('닭'), 'ekfr');
    expect(keys('ㅋ'), 'z');
    expect(keys('ㅘ'), 'hk');
    expect(_keys(androidStrokesForRune('쌍'.runes.first)!), 'Tkd');
  });

  test('typing appends keys; Enter is a key and retains IME edit history', () {
    final bridge = AndroidTypingBridge();
    expect(_keys(bridge.update(_v('h')).strokes), 'h');
    expect(_keys(bridge.update(_v('hi')).strokes), 'i');
    expect(_keys(bridge.update(_v('hi!')).strokes), '⇧31');
    final enter = bridge.update(_v('hi!\n'));
    expect(_keys(enter.strokes), '⏎');
    expect(enter.reset, isNull);
    // A finish snapshot of the same transaction brings nothing more.
    expect(bridge.update(_v('hi!\n')).strokes, isEmpty);
    bridge.clear(); // An actual keyboard/session owner boundary starts over.
    expect(bridge.update(AndroidTypingBridge.initial).strokes, isEmpty);
  });

  test('Korean composition streams jamo by jamo and back again', () {
    final bridge = AndroidTypingBridge();
    final out = StringBuffer();
    for (final step in [
      _v('ㅎ', composeFrom: 0),
      _v('하', composeFrom: 0),
      _v('한', composeFrom: 0),
      _v('한ㄱ', composeFrom: 1),
      _v('한그', composeFrom: 1),
      _v('한글', composeFrom: 1),
    ]) {
      final update = bridge.update(step);
      expect(update.hangul, true);
      out.write(_keys(update.strokes));
    }
    expect(out.toString(), 'gksrmf');
    expect(bridge.remoteComposing, true);
    // Backspace inside the syllable being composed removes one jamo.
    expect(_keys(bridge.update(_v('한그', composeFrom: 1)).strokes), '⌫');
    // The syllable splits when a vowel follows its final consonant.
    final split = AndroidTypingBridge();
    split.update(_v('한', composeFrom: 0));
    expect(_keys(split.update(_v('하나', composeFrom: 1)).strokes), '⌫sk');
  });

  test('deleting a whole syllable still being composed deletes its jamo', () {
    final bridge = AndroidTypingBridge();
    bridge.update(_v('가', composeFrom: 0));
    bridge.update(_v('가한', composeFrom: 1));
    // The composing 한 removed at once: three keys to take back.
    expect(_keys(bridge.update(_v('가')).strokes), '⌫⌫⌫');
    // The committed 가 is one character.
    expect(_keys(bridge.update(_v('')).strokes), '⌫');
  });

  test('Backspace on an empty field still reaches the remote', () {
    final bridge = AndroidTypingBridge();
    final update = bridge.update(
      const TextEditingValue(
        text: '​',
        selection: TextSelection.collapsed(offset: 1),
      ),
    );
    expect(_keys(update.strokes), '⌫');
    expect(update.reset, AndroidTypingBridge.initial);
  });

  test('a keyboard cursor swipe becomes arrow keys', () {
    final bridge = AndroidTypingBridge();
    bridge.update(_v('abc'));
    final moved = bridge.update(
      _v('abc').copyWith(selection: const TextSelection.collapsed(offset: 3)),
    );
    expect(_keys(moved.strokes), '←←');
    expect(moved.reset, AndroidTypingBridge.initial);
  });

  test('characters the key protocol cannot type are dropped and noted', () {
    final bridge = AndroidTypingBridge();
    final update = bridge.update(_v('ok😀'));
    expect(_keys(update.strokes), 'ok');
    expect(update.unsupported, true);
    expect(update.reset, isNotNull);
  });

  test('hardware-style keys from the phone keyboard map to Windows keys', () {
    expect(
      androidStrokeForKey(LogicalKeyboardKey.arrowLeft),
      const AndroidKeyStroke(0x25, extended: true),
    );
    expect(androidStrokeForKey(LogicalKeyboardKey.enter)?.vk, 0x0d);
    expect(androidStrokeForKey(LogicalKeyboardKey.f5)?.vk, 0x74);
    expect(androidStrokeForKey(LogicalKeyboardKey.keyA), isNull);
  });
}
