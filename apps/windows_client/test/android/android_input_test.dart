import 'dart:async';

import 'package:flutter_test/flutter_test.dart';
import 'package:multitap_client/android/android_input.dart';

void main() {
  test('explicit paste sends Ctrl+V down/up then releases input', () async {
    final sent = <Map<String, dynamic>>[], captures = <bool>[];
    var captured = false;
    final actions = AndroidInputActions(
      ready: () => true,
      acquired: () => captured,
      capture: (value) async {
        captures.add(value);
        captured = value;
      },
      send: sent.add,
    );
    expect(await actions.chord('paste'), true);
    expect(sent.map((e) => [e['vk'], e['down']]).toList(), [
      [0xa2, true],
      [0x56, true],
      [0x56, false],
      [0xa2, false],
    ]);
    expect(captures, [false, true, false]);
    expect(captured, false);
    expect(
      sent.every((e) => e['type'] == 'input' && e['event'] == 'key'),
      true,
    );
  });

  test('undo is an ordinary Ctrl+Z chord', () async {
    final sent = <Map<String, dynamic>>[];
    var captured = false;
    final actions = AndroidInputActions(
      ready: () => true,
      acquired: () => captured,
      capture: (value) async => captured = value,
      send: sent.add,
    );
    expect(await actions.chord('undo'), true);
    expect(sent.map((e) => [e['vk'], e['down']]).toList(), [
      [0xa2, true],
      [0x5a, true],
      [0x5a, false],
      [0xa2, false],
    ]);
  });

  test('native capture rejection does not report a sent key', () async {
    final sent = <Map<String, dynamic>>[];
    final actions = AndroidInputActions(
      ready: () => true,
      acquired: () => false,
      capture: (_) async {},
      send: sent.add,
    );
    expect(await actions.chord('windows'), false);
    expect(sent, isEmpty);
  });

  test('focus loss during capture cannot send a delayed key chord', () async {
    final acquisition = Completer<void>();
    final captures = <bool>[], sent = <Map<String, dynamic>>[];
    final actions = AndroidInputActions(
      ready: () => true,
      acquired: () => true,
      capture: (value) async {
        captures.add(value);
        if (value) await acquisition.future;
      },
      send: sent.add,
    );
    final request = actions.chord('altTab');
    await Future<void>.delayed(Duration.zero);
    actions.invalidate();
    acquisition.complete();
    expect(await request, false);
    expect(sent, isEmpty);
    expect(captures.last, false);
    expect(actions.busy, false);
  });

  test(
    'duplicate action and unsupported secure attention are rejected',
    () async {
      final acquisition = Completer<void>();
      final sent = <Map<String, dynamic>>[];
      final actions = AndroidInputActions(
        ready: () => true,
        acquired: () => true,
        capture: (value) async {
          if (value) await acquisition.future;
        },
        send: sent.add,
      );
      final first = actions.chord('windows');
      expect(await actions.chord('windows'), false);
      expect(await actions.chord('ctrlAltDelete'), false);
      acquisition.complete();
      expect(await first, true);
      expect(sent.map((e) => e['vk']), [0x5b, 0x5b]);
      expect(sent.every((e) => e['extended'] == true), true);
    },
  );

  test('mid-chord revocation stops further keys and resets input', () async {
    var ready = true;
    final sent = <Map<String, dynamic>>[], captures = <bool>[];
    final actions = AndroidInputActions(
      ready: () => ready,
      acquired: () => true,
      capture: (value) async => captures.add(value),
      send: (event) {
        sent.add(event);
        ready = false;
      },
    );
    expect(await actions.chord('paste'), false);
    expect(sent.length, 1);
    expect(captures.last, false);
  });

  test(
    'cleanup exception does not permanently lock the input controls',
    () async {
      var release = 0;
      final actions = AndroidInputActions(
        ready: () => true,
        acquired: () => true,
        capture: (value) async {
          if (!value && ++release == 2) throw StateError('test');
        },
        send: (_) {},
      );
      await expectLater(actions.chord('hangul'), throwsStateError);
      expect(actions.busy, false);
      expect(await actions.chord('hangul'), true);
    },
  );
}
