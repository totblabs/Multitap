import 'dart:async';

import 'package:flutter_test/flutter_test.dart';
import 'package:multitap_client/data/remote_input_gate.dart';

Map<String, dynamic> key(int vk, bool down) => {
  'type': 'input',
  'event': 'key',
  'vk': vk,
  'scan': vk,
  'down': down,
};
const reset = <String, dynamic>{'type': 'input.reset'};

class InputFixture {
  final sent = <Map<String, dynamic>>[];
  final completion = Completer<bool>();
  var enabled = true, blocked = 0, prepares = 0;
  late final gate = RemoteInputGate(
    send: (event) => sent.add(Map<String, dynamic>.from(event)),
    preparePaste: () {
      prepares++;
      return completion.future;
    },
    pasteEnabled: () => enabled,
    onPasteBlocked: () => blocked++,
    pasteTimeout: const Duration(milliseconds: 250),
  );
  void beginPaste() {
    gate.add(key(0x11, true));
    gate.add(key(0x56, true));
  }

  void releaseKeys() {
    gate.add(key(0x56, false));
    gate.add(key(0x11, false));
  }

  void finish([bool success = true]) {
    if (!completion.isCompleted) completion.complete(success);
  }
}

void main() {
  testWidgets('immediate paste: Shift+Insert waits for publication ACK', (
    tester,
  ) async {
    final f = InputFixture();
    f.gate.add(key(0xa1, true));
    final insert = key(0x2d, true)..['extended'] = true;
    f.gate.add(insert);
    f.gate.add(key(0x2d, false)..['extended'] = true);
    f.gate.add(key(0xa1, false));
    expect(f.prepares, 1);
    expect(f.sent, [key(0xa1, true), reset]);
    await tester.pump(const Duration(milliseconds: 200));
    expect(f.sent.any((event) => event['vk'] == 0x2d), isFalse);
    f.finish();
    await tester.pump();
    expect(f.sent, [
      key(0xa1, true),
      reset,
      key(0xa1, true),
      insert,
      key(0x2d, false)..['extended'] = true,
      key(0xa1, false),
    ]);
    expect(f.blocked, 0);
    expect(f.gate.preparingPaste, isFalse);
  });

  for (final end in ['failure', 'timeout', 'permission', 'reset']) {
    testWidgets('immediate paste: Shift+Insert drops late ACK after $end', (
      tester,
    ) async {
      final f = InputFixture();
      f.gate.add(key(0x10, true));
      f.gate.add(key(0x2d, true));
      f.gate.add(key(0x2d, false));
      f.gate.add(key(0x10, false));
      if (end == 'timeout') {
        await tester.pump(const Duration(milliseconds: 251));
      }
      if (end == 'permission') f.enabled = false;
      if (end == 'reset') f.gate.add(Map.from(reset));
      f.finish(end != 'failure');
      await tester.pump();
      expect(f.prepares, 1);
      expect(f.sent.any((event) => event['vk'] == 0x2d), isFalse);
      expect(f.gate.preparingPaste, isFalse);
      expect(f.blocked, end == 'reset' ? 0 : 1);
      expect(f.sent.last, reset);
    });
  }

  testWidgets('immediate paste: non-paste input keeps its existing path', (
    tester,
  ) async {
    final f = InputFixture();
    f.gate.add(key(0x2d, true)); // Insert.
    f.gate.add(key(0x2d, false));
    f.gate.add(key(0x11, true)); // Ctrl+Insert is Copy.
    f.gate.add(key(0x2d, true));
    f.gate.add(key(0x2d, false));
    f.gate.add(key(0x11, false));
    f.gate.add(key(0x12, true)); // Alt+Shift+Insert is application input.
    f.gate.add(key(0x10, true));
    f.gate.add(key(0x2d, true));
    final rightClick = {
      'type': 'input',
      'event': 'button',
      'button': 'right',
      'down': true,
    };
    f.gate.add(rightClick);
    expect(f.prepares, 0);
    expect(f.sent.last, rightClick);
    f.gate.reset();
    f.sent.clear();
    f.enabled = false;
    f.gate.add(key(0x10, true));
    f.gate.add(key(0x2d, true));
    f.gate.add(key(0x2d, false));
    f.gate.add(key(0x10, false));
    expect(f.prepares, 0);
    expect(f.sent, [
      key(0x10, true),
      key(0x2d, true),
      key(0x2d, false),
      key(0x10, false),
    ]);
    f.gate.reset();
    f.finish();
    await tester.pump();
  });

  testWidgets(
    'a modal block preserves only the current paste and releases its keys',
    (tester) async {
      final f = InputFixture();
      f.beginPaste();
      f.gate.add(key(0x41, true));
      f.gate.setClipboardBlocked(true);
      f.gate.add(key(0x42, true));
      f.releaseKeys();
      expect(f.gate.preparingPaste, isTrue);
      f.finish();
      await tester.pump();
      expect(f.sent, [
        key(0x11, true),
        reset,
        key(0x11, true),
        key(0x56, true),
        reset,
      ]);
      expect(f.blocked, 0);
      expect(f.gate.preparingPaste, isFalse);
      f.gate.setClipboardBlocked(false);
      f.gate.add(key(0x56, true));
      expect(
        f.prepares,
        1,
        reason: 'a swallowed Ctrl-up cannot leave a stale modifier',
      );
    },
  );

  for (final revoked in ['reset', 'permission', 'failure']) {
    testWidgets('a modal block never preserves paste after $revoked', (
      tester,
    ) async {
      final f = InputFixture();
      f.beginPaste();
      f.gate.setClipboardBlocked(true);
      if (revoked == 'reset') f.gate.add(Map.from(reset));
      if (revoked == 'permission') f.enabled = false;
      f.finish(revoked != 'failure');
      await tester.pump();
      expect(f.sent.any((event) => event['vk'] == 0x56), isFalse);
      expect(f.gate.preparingPaste, isFalse);
    });
  }

  testWidgets(
    'blocked native Escape cancels while other input remains blocked',
    (tester) async {
      var cancels = 0, settled = 0;
      final sent = <Map<String, dynamic>>[];
      final completion = Completer<bool>();
      final gate = RemoteInputGate(
        send: sent.add,
        preparePaste: () => completion.future,
        pasteEnabled: () => true,
        onPasteBlocked: () {},
        onPasteSettled: () => settled++,
        onBlockedEscape: () => cancels++,
      );
      gate.add(key(0x11, true));
      gate.add(key(0x56, true));
      gate.setClipboardBlocked(true);
      gate.add(key(0x1b, true));
      gate.add(key(0x1b, false));
      gate.add({'type': 'input', 'event': 'button', 'down': true});
      expect(cancels, 1);
      expect(
        gate.preparingPaste,
        isFalse,
        reason: 'Escape revokes immediately, before cancellation can race a success',
      );
      completion.complete(true);
      await tester.pump();
      expect(settled, 1);
      expect(
        sent.any((event) => event['vk'] == 0x56 || event['event'] == 'button'),
        isFalse,
      );
    },
  );

  testWidgets(
    'file copy must finish before replaying Ctrl+V and its releases in order',
    (tester) async {
      final f = InputFixture();
      f.beginPaste();
      f.releaseKeys();
      expect(f.prepares, 1);
      expect(f.sent, [key(0x11, true), reset]);
      await tester.pump(const Duration(milliseconds: 200));
      expect(f.sent.any((event) => event['vk'] == 0x56), isFalse);
      f.finish();
      await tester.pump();
      expect(f.sent, [
        key(0x11, true),
        reset,
        key(0x11, true),
        key(0x56, true),
        key(0x56, false),
        key(0x11, false),
      ]);
      expect(f.blocked, 0);
    },
  );

  testWidgets('failed copy drops queued paste and releases all remote input', (
    tester,
  ) async {
    final f = InputFixture();
    f.beginPaste();
    f.releaseKeys();
    f.finish(false);
    await tester.pump();
    expect(f.sent, [key(0x11, true), reset, reset]);
    expect(f.blocked, 1);
    expect(f.sent.any((event) => event['vk'] == 0x56), isFalse);
  });

  testWidgets(
    'timeout cannot flush late file completion into a remote desktop',
    (tester) async {
      final f = InputFixture();
      f.beginPaste();
      f.releaseKeys();
      await tester.pump(const Duration(milliseconds: 251));
      expect(f.blocked, 1);
      final afterTimeout = List<Map<String, dynamic>>.from(f.sent);
      f.finish();
      await tester.pump();
      expect(f.sent, afterTimeout);
      expect(f.sent.any((event) => event['vk'] == 0x56), isFalse);
    },
  );

  testWidgets('focus loss or revocation resets ownership before late success', (
    tester,
  ) async {
    final f = InputFixture();
    f.beginPaste();
    f.releaseKeys();
    f.enabled = false;
    f.gate.add(Map.from(reset));
    final afterReset = List<Map<String, dynamic>>.from(f.sent);
    f.finish();
    await tester.pump();
    expect(f.sent, afterReset);
    expect(f.sent.any((event) => event['vk'] == 0x56), isFalse);
    expect(
      f.blocked,
      0,
      reason: 'an explicit ownership reset is not a failed paste',
    );
  });

  testWidgets(
    'permission changed during copy blocks replay even without an explicit reset',
    (tester) async {
      final f = InputFixture();
      f.beginPaste();
      f.releaseKeys();
      f.enabled = false;
      f.finish();
      await tester.pump();
      expect(f.blocked, 1);
      expect(f.sent.last, reset);
      expect(f.sent.any((event) => event['vk'] == 0x56), isFalse);
    },
  );

  testWidgets(
    '128 queued events are bounded and overflow discards the entire pending paste',
    (tester) async {
      final f = InputFixture();
      f.beginPaste();
      // Initial replay contains the saved Ctrl-down and V-down (two events).
      for (var i = 0; i < 126; i++) {
        f.gate.add({'type': 'input', 'event': 'wheel', 'delta': i});
      }
      expect(f.blocked, 0);
      f.gate.add({'type': 'input', 'event': 'wheel', 'delta': 127});
      expect(f.blocked, 1);
      expect(f.sent.last, reset);
      final afterOverflow = List<Map<String, dynamic>>.from(f.sent);
      f.finish();
      await tester.pump();
      expect(f.sent, afterOverflow);
    },
  );

  testWidgets(
    'a new paste generation is never flushed by the old preparation result',
    (tester) async {
      final completions = [Completer<bool>(), Completer<bool>()],
          sent = <Map<String, dynamic>>[];
      var prepares = 0, blocked = 0;
      final gate = RemoteInputGate(
        send: (event) => sent.add(Map.from(event)),
        preparePaste: () => completions[prepares++].future,
        pasteEnabled: () => true,
        onPasteBlocked: () => blocked++,
      );
      gate.add(key(0x11, true));
      gate.add(key(0x56, true));
      gate.add(Map.from(reset));
      gate.add(key(0x11, true));
      gate.add(key(0x56, true));
      gate.add(key(0x56, false));
      gate.add(key(0x11, false));
      final before = List<Map<String, dynamic>>.from(sent);
      completions[0].complete(true);
      await tester.pump();
      expect(sent, before);
      completions[1].complete(true);
      await tester.pump();
      expect(sent.skip(before.length), [
        key(0x11, true),
        key(0x56, true),
        key(0x56, false),
        key(0x11, false),
      ]);
      expect(blocked, 0);
    },
  );

  testWidgets(
    'disabled sharing and non-paste shortcuts never capture clipboard content',
    (tester) async {
      final f = InputFixture()..enabled = false;
      f.beginPaste();
      f.releaseKeys();
      expect(f.prepares, 0);
      expect(f.sent, [
        key(0x11, true),
        key(0x56, true),
        key(0x56, false),
        key(0x11, false),
      ]);
      f.enabled = true;
      f.sent.clear();
      f.gate.add(key(0x12, true));
      f.gate.add(key(0x11, true));
      f.gate.add(key(0x56, true));
      expect(f.prepares, 0);
      expect(f.sent.last, key(0x56, true));
      f.gate.reset();
      f.finish();
      await tester.pump();
    },
  );

  testWidgets(
    'input events are snapshotted and an ownership reset during replay stops remaining keys',
    (tester) async {
      final completion = Completer<bool>(), sent = <Map<String, dynamic>>[];
      late RemoteInputGate gate;
      gate = RemoteInputGate(
        send: (event) {
          sent.add(Map.from(event));
          if (event['vk'] == 0x56 && event['down'] == true) gate.reset();
        },
        preparePaste: () => completion.future,
        pasteEnabled: () => true,
        onPasteBlocked: () {},
      );
      gate.add(key(0x11, true));
      final down = key(0x56, true);
      gate.add(down);
      down['vk'] = 0x41;
      gate.add(key(0x56, false));
      gate.add(key(0x11, false));
      completion.complete(true);
      await tester.pump();
      expect(sent.last, key(0x56, true));
      expect(sent.any((event) => event['vk'] == 0x41), isFalse);
      expect(
        sent.any((event) => event['vk'] == 0x56 && event['down'] == false),
        isFalse,
      );
    },
  );
}
