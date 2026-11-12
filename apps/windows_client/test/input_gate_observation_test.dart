import 'dart:async';

import 'package:flutter_test/flutter_test.dart';
import 'package:multitap_client/data/remote_input_gate.dart';

import 'remote_input_gate_test.dart' show key;

void main() {
  test('enabling observation during an existing paste does not invent its start time', () async {
    var enabled = false;
    final completion = Completer<bool>();
    final sent = <Map<String, dynamic>>[];
    final gate = RemoteInputGate(
      send: sent.add,
      preparePaste: () => completion.future,
      pasteEnabled: () => true,
      onPasteBlocked: () => fail('not blocked'),
      observationEnabled: () => enabled,
      nowUs: () => 5000,
      onWaitObserved: (_, _) => fail('unknown start must remain unavailable'),
    );
    gate.add(key(0x11, true));
    gate.add(key(0x56, true));
    enabled = true;
    expect(gate.preparingPaste, true);
    expect(gate.observedWaitUs, null);
    completion.complete(true);
    await Future<void>.delayed(Duration.zero);
    expect(gate.preparingPaste, false);
    expect(sent.last['vk'], 0x56);
  });
  test(
    'paste wait timing does not change input ordering and completes once',
    () async {
      var now = 1000;
      final completion = Completer<bool>();
      final sent = <Map<String, dynamic>>[], observed = <(int, int)>[];
      final gate = RemoteInputGate(
        send: sent.add,
        preparePaste: () => completion.future,
        pasteEnabled: () => true,
        onPasteBlocked: () => fail('not blocked'),
        observationEnabled: () => true,
        nowUs: () => now,
        onWaitObserved: (us, queued) => observed.add((us, queued)),
      );
      gate.add(key(0x11, true));
      gate.add(key(0x56, true));
      gate.add(key(0x56, false));
      now = 81000;
      expect(gate.observedWaitUs, 80000);
      expect(gate.queuedPackets, 3);
      completion.complete(true);
      await Future<void>.delayed(Duration.zero);
      expect(observed, [(80000, 3)]);
      expect(sent.map((e) => e['type']), [
        'input',
        'input.reset',
        'input',
        'input',
        'input',
      ]);
      gate.reset();
      expect(observed, hasLength(1));
    },
  );
  test(
    'reset cancels observed owner, and a late paste result cannot replay',
    () async {
      var now = 0;
      final completion = Completer<bool>();
      final sent = <Map<String, dynamic>>[], observed = <int>[];
      final gate = RemoteInputGate(
        send: sent.add,
        preparePaste: () => completion.future,
        pasteEnabled: () => true,
        onPasteBlocked: () {},
        observationEnabled: () => true,
        nowUs: () => now,
        onWaitObserved: (us, _) => observed.add(us),
      );
      gate.add(key(0x11, true));
      gate.add(key(0x56, true));
      now = 12000;
      gate.reset();
      final count = sent.length;
      completion.complete(true);
      await Future<void>.delayed(Duration.zero);
      expect(sent, hasLength(count));
      expect(observed, [12000]);
    },
  );
  test(
    'OFF never samples clock and a failing observer cannot block paste',
    () async {
      for (final enabled in [false, true]) {
        final sent = <Map<String, dynamic>>[];
        final gate = RemoteInputGate(
          send: sent.add,
          preparePaste: () async => true,
          pasteEnabled: () => true,
          onPasteBlocked: () => fail('not blocked'),
          observationEnabled: () => enabled,
          nowUs: () {
            if (!enabled) fail('OFF called clock');
            return 0;
          },
          onWaitObserved: (_, _) => throw StateError('observer failure'),
        );
        gate.add(key(0x11, true));
        gate.add(key(0x56, true));
        await Future<void>.delayed(Duration.zero);
        expect(gate.preparingPaste, false);
        expect(sent.last['vk'], 0x56);
      }
    },
  );
}
