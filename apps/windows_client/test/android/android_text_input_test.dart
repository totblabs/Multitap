// text-input-v1 on the client: IME edit sequences (Gboard-style syllable
// composition, Samsung-style word composition, recomposition on Backspace,
// finished compositions) become committed text and keys in order; chunking
// and the broker grant parsing. Simulated TextEditingValue sequences; no
// device, IME or Host.
import 'package:flutter/services.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:multitap_client/android/android_keyboard.dart';
import 'package:multitap_client/data/broker_api.dart';
import 'package:multitap_client/data/text_input.dart';

const _s = AndroidTypingBridge.sentinel;

/// The field after typing [typed]; [composeFrom] marks the composing tail.
TextEditingValue _v(String typed, {int? composeFrom, int? composeTo}) {
  final text = '$_s$typed';
  return TextEditingValue(
    text: text,
    selection: TextSelection.collapsed(offset: text.length),
    composing: composeFrom == null
        ? TextRange.empty
        : TextRange(
            start: _s.length + composeFrom,
            end: _s.length + (composeTo ?? typed.length),
          ),
  );
}

/// Actions as a readable string: «text», ⌫, ⏎, ⇥, ←, →.
String _act(AndroidTypingUpdate update) => update.actions
    .map(
      (a) => a is String
          ? '«$a»'
          : switch ((a as AndroidKeyStroke).vk) {
              0x08 => '⌫',
              0x0d => '⏎',
              0x09 => '⇥',
              0x25 => '←',
              0x27 => '→',
              _ => '?',
            },
    )
    .join();

/// What the remote shows after [update]'s actions, starting from [shown].
String _apply(String shown, AndroidTypingUpdate update) {
  var runes = shown.runes.toList();
  for (final action in update.actions) {
    if (action is String) {
      runes = [...runes, ...action.runes];
    } else {
      switch ((action as AndroidKeyStroke).vk) {
        case 0x08:
          if (runes.isNotEmpty) runes = runes.sublist(0, runes.length - 1);
        case 0x0d:
          runes = [...runes, 0x0a];
        case 0x09:
          runes = [...runes, 0x09];
      }
    }
  }
  return String.fromCharCodes(runes);
}

/// Feeds [steps] and checks after each one that the remote shows exactly
/// the field, composition included.
String _mirror(AndroidTypingBridge bridge, List<TextEditingValue> steps) {
  var remote = '';
  final actions = <String>[];
  for (final step in steps) {
    final update = bridge.update(step);
    actions.add(_act(update));
    remote = _apply(remote, update);
    expect(remote, step.text.replaceAll('​', ''), reason: 'after ${step.text}');
  }
  return actions.join();
}

void main() {
  test(
    'Gboard-style Korean: the syllable appears remotely while composing',
    () {
      final bridge = AndroidTypingBridge(text: true);
      expect(
        _mirror(bridge, [
          _v('ㅇ', composeFrom: 0),
          _v('아', composeFrom: 0),
          _v('안', composeFrom: 0),
          _v('안ㄴ', composeFrom: 1), // 안 committed
          _v('안녀', composeFrom: 1),
          _v('안녕', composeFrom: 1),
          _v('안녕 '), // space commits 녕
        ]),
        '«ㅇ»⌫«아»⌫«안»«ㄴ»⌫«녀»⌫«녕»« »',
      );
      // No 2-set keys: the remote IME mode no longer matters.
      expect(bridge.update(_v('안녕 ')).actions, isEmpty);
    },
  );

  test('Samsung-style word composition is mirrored letter by letter', () {
    final bridge = AndroidTypingBridge(text: true);
    expect(
      _mirror(bridge, [
        _v('h', composeFrom: 0),
        _v('he', composeFrom: 0),
        _v('hel', composeFrom: 0),
        _v('hello', composeFrom: 0), // a suggestion picked
        _v('hello '),
        _v('hello w', composeFrom: 6),
        _v('hello wo', composeFrom: 6),
      ]),
      '«h»«e»«l»«lo»« »«w»«o»',
    );
    expect(bridge.pending, '', reason: 'already on the remote');
  });

  test('Backspace inside the composition removes it remotely too', () {
    final bridge = AndroidTypingBridge(text: true);
    expect(
      _mirror(bridge, [
        _v('가', composeFrom: 0),
        _v('가ㄴ', composeFrom: 1),
        _v('가', composeFrom: 1, composeTo: 1), // ㄴ removed while composing
        _v(''),
      ]),
      '«가»«ㄴ»⌫⌫',
    );
    // Backspace on the empty field (the sentinel) still deletes remotely.
    final empty = bridge.update(
      const TextEditingValue(
        text: '​',
        selection: TextSelection.collapsed(offset: 1),
      ),
    );
    expect(_act(empty), '⌫');
    expect(empty.reset, AndroidTypingBridge.initial);
  });

  test('recomposition on Backspace: the committed syllable is replaced', () {
    // Gboard may reopen the last committed syllable: 안 -> composing 아.
    final bridge = AndroidTypingBridge(text: true);
    expect(
      _mirror(bridge, [_v('안'), _v('아', composeFrom: 0), _v('아 ')]),
      '«안»⌫«아»« »',
    );
  });

  test(
    'a finished composition with the same characters sends nothing more',
    () {
      final bridge = AndroidTypingBridge(text: true);
      expect(_mirror(bridge, [_v('하', composeFrom: 0), _v('하')]), '«하»');
    },
  );
  test(
    'Enter and Tab are keys between text; emoji and other scripts are text',
    () {
      final bridge = AndroidTypingBridge(text: true);
      final update = bridge.update(_v('a\tb😀中\n'));
      expect(_act(update), '«a»⇥«b😀中»⏎');
      expect(
        update.reset,
        isNull,
        reason: 'the active IME retains edit history',
      );
      expect(update.unsupported, false);
    },
  );

  test('a keyboard cursor swipe becomes arrows in text mode too', () {
    final bridge = AndroidTypingBridge(text: true);
    bridge.update(_v('abc'));
    final moved = bridge.update(
      _v('abc').copyWith(selection: const TextSelection.collapsed(offset: 3)),
    );
    expect(_act(moved), '←←');
  });

  test('nothing is left pending: the composition is already on the remote', () {
    final text = AndroidTypingBridge(text: true);
    text.update(_v('한글', composeFrom: 1));
    expect(text.pending, '');
    final keys = AndroidTypingBridge();
    keys.update(_v('한', composeFrom: 0));
    expect(keys.pending, '');
  });

  test('chunks stay within 256 UTF-16 units and never split a pair', () {
    expect(textInputChunks('안녕'), ['안녕']);
    expect(textInputChunks('a\u0000b\u007fc\u0085d'), ['abcd']);
    expect(textInputChunks('a\ud800b'), ['ab'], reason: 'lone surrogate');
    final long = 'x' * 255 + '😀' + 'y';
    final chunks = textInputChunks(long);
    expect(chunks, hasLength(2));
    expect(chunks.first.length, 255);
    expect(chunks.last, '😀y');
    expect(chunks.every((c) => c.length <= textInputMaxUnits), true);
    expect(textInputMessages('a'), [
      {'type': 'input', 'event': 'text', 'text': 'a'},
    ]);
    expect(textInputMessages('\n'), isEmpty);
  });

  test('the broker grant is exact and only follows a request', () {
    final base = {
      'sessionId': 'abc-session',
      'webSocketUrl': 'wss://vdi.example/api/v1/sessions/abc-session',
    };
    final origin = Uri.parse('https://vdi.example');
    expect(
      BrokerConnection.fromJson(
        {...base, 'textInputProtocol': 'text-input-v1'},
        origin,
        requestedTextInput: true,
      ).textInput,
      true,
    );
    expect(BrokerConnection.fromJson(base, origin).textInput, false);
    for (final (data, requested) in [
      ({...base, 'textInputProtocol': 'text-input-v1'}, false),
      ({...base, 'textInputProtocol': 'text-input-v2'}, true),
    ]) {
      expect(
        () => BrokerConnection.fromJson(
          data,
          origin,
          requestedTextInput: requested,
        ),
        throwsA(isA<BrokerException>()),
      );
    }
    final desktop = BrokerDesktop.fromJson({
      'id': 'pve-1',
      'name': 'VM',
      'kind': 'vm',
      'description': '',
      'os': '',
      'status': 'online',
      'canConnect': true,
      'host': {
        'connected': true,
        'capabilities': {'textInputProtocol': 'text-input-v1'},
      },
    });
    expect(desktop.textInput, true);
  });
}
