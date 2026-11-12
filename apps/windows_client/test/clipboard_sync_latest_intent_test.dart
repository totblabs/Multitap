// Latest intent means the last valid observation received by this sync owner,
// not a comparison of the independent Windows sequence counters or clocks.
// These source-first regressions do not establish the user's field cause.
// Actual ClipboardSync, memory-only sequence events/callbacks and fake time:
// no MethodChannel, clipboard, foreground, input or native runtime calls.
import 'dart:async';

import 'package:flutter_test/flutter_test.dart';
import 'package:multitap_client/data/clipboard_policy.dart';
import 'package:multitap_client/data/clipboard_sync.dart';
import 'package:multitap_client/data/native_clipboard.dart';

const _owner = 'latest-intent-fixture';

class _MetadataBridge implements ClipboardObservationBridge {
  final events = StreamController<ClipboardChange>.broadcast(sync: true);
  int sequence = 1;
  @override
  int? lastCommittedSequence;
  @override
  Stream<ClipboardChange> get changes => events.stream;
  @override
  Future<int> watch(String owner, bool enabled) async => sequence;

  void changed(int value) {
    sequence = value;
    events.add(ClipboardChange(_owner, value));
  }
}

class _IntentFixture {
  _IntentFixture() {
    sync = ClipboardSync(
      owner: _owner,
      bridge: bridge,
      policy: const ClipboardPolicy.all(),
      sendLocal: () async {
        final sequence = bridge.sequence;
        actions.add('local:$sequence');
        sync.capturedLocal(sequence);
        sync.confirmedLocal(sequence);
      },
      receiveRemote: () async => actions.add('remote:$remoteSequence'),
      watchRemote: (_) {},
      waitIdle: () async {
        final pending = idle;
        if (pending != null && !pending.isCompleted) {
          heldWaits++;
          await pending.future;
        }
      },
      cancelOlder: () => cancellations++,
      onFailure: () => failures++,
    );
  }
  final bridge = _MetadataBridge();
  late final ClipboardSync sync;
  final actions = <String>[];
  int remoteSequence = 0, heldWaits = 0, cancellations = 0, failures = 0;
  Completer<void>? idle;

  Future<void> activate(WidgetTester tester) async {
    await sync.setActive(true);
    // Finish the intentional activation baseline before the scenario starts.
    await tester.pump(const Duration(seconds: 1));
    actions.clear();
    cancellations = 0;
  }

  void local(int sequence) => bridge.changed(sequence);
  void remote(int sequence) {
    remoteSequence = sequence;
    sync.remoteChanged(sequence);
  }

  void holdIdle() {
    idle = Completer<void>();
  }

  void releaseIdle() {
    final pending = idle;
    if (pending != null && !pending.isCompleted) pending.complete();
  }

  Future<void> finish(WidgetTester tester) async {
    releaseIdle();
    // Let both ordinary debounce directions and a mistakenly replayed second
    // direction run. Assertions follow cleanup, before testWidgets invariants.
    await tester.pump(const Duration(seconds: 1));
    await tester.pump(const Duration(seconds: 1));
    await tester.runAsync(() async {
      await sync.close();
      await bridge.events.close();
    });
    await tester.pump();
  }
}

void main() {
  testWidgets('latest intent: newer remote replaces pending local', (
    tester,
  ) async {
    final fixture = _IntentFixture();
    try {
      await fixture.activate(tester);
      fixture.local(4000);
      fixture.remote(2); // A smaller independent counter is still the latest.
    } finally {
      await fixture.finish(tester);
    }
    expect(fixture.actions, ['remote:2']);
    expect(fixture.failures, 0);
  });

  testWidgets('latest intent: newer local replaces pending remote', (
    tester,
  ) async {
    final fixture = _IntentFixture();
    try {
      await fixture.activate(tester);
      fixture.remote(4000);
      fixture.local(2);
    } finally {
      await fixture.finish(tester);
    }
    expect(fixture.actions, ['local:2']);
    expect(fixture.failures, 0);
  });

  for (final newerRemote in [true, false]) {
    testWidgets('latest intent: held waitIdle keeps only newer '
        '${newerRemote ? 'remote' : 'local'} direction', (tester) async {
      final fixture = _IntentFixture();
      var actuallyHeld = false;
      try {
        await fixture.activate(tester);
        fixture.holdIdle();
        if (newerRemote) {
          fixture.local(4000);
        } else {
          fixture.remote(4000);
        }
        await tester.pump(const Duration(seconds: 1));
        actuallyHeld = fixture.heldWaits == 1 && fixture.actions.isEmpty;
        if (newerRemote) {
          fixture.remote(2);
        } else {
          fixture.local(2);
        }
      } finally {
        await fixture.finish(tester);
      }
      expect(actuallyHeld, isTrue, reason: 'actual sync awaited the fake gate');
      expect(fixture.actions, [newerRemote ? 'remote:2' : 'local:2']);
      expect(fixture.failures, 0);
    });
  }

  testWidgets(
    'latest intent: alternating arrivals during waitIdle do not replay',
    (tester) async {
      final fixture = _IntentFixture();
      var actuallyHeld = false;
      try {
        await fixture.activate(tester);
        fixture.holdIdle();
        fixture.local(4000);
        await tester.pump(const Duration(seconds: 1));
        actuallyHeld = fixture.heldWaits == 1 && fixture.actions.isEmpty;
        fixture.remote(2);
        fixture.local(4001);
        fixture.remote(3);
      } finally {
        await fixture.finish(tester);
      }
      expect(actuallyHeld, isTrue, reason: 'actual sync awaited the fake gate');
      expect(fixture.actions, ['remote:3']);
      expect(fixture.failures, 0);
    },
  );
}
