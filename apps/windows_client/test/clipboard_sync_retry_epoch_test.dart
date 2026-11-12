// Actual ClipboardSync with memory-only metadata/callbacks and fake time.
// This fixture does not read a clipboard, use MethodChannel, manipulate focus,
// create files or call a native API. Delayed NotStarted is a contract boundary,
// not evidence of the cause of the user's field latency.
import 'dart:async';

import 'package:flutter_test/flutter_test.dart';
import 'package:multitap_client/data/clipboard_policy.dart';
import 'package:multitap_client/data/clipboard_sync.dart';
import 'package:multitap_client/data/native_clipboard.dart';

const _owner = 'retry-epoch-private-fixture';

class _NotStarted implements Exception, ClipboardNotStarted {
  const _NotStarted();
}

class _MetadataBridge implements ClipboardObservationBridge {
  final events = StreamController<ClipboardChange>.broadcast(sync: true);
  int sequence = 1;
  @override
  int? lastCommittedSequence;
  @override
  Stream<ClipboardChange> get changes => events.stream;
  @override
  Future<int> watch(String owner, bool enabled) async => sequence;
  void changed(int sequence) {
    this.sequence = sequence;
    events.add(ClipboardChange(_owner, sequence));
  }
}

class _Fixture {
  _Fixture({this.throwDelayHook = false}) {
    sync = ClipboardSync(
      owner: _owner,
      bridge: bridge,
      policy: const ClipboardPolicy.all(),
      remoteQuiet: const Duration(milliseconds: 80),
      sendLocal: () async {
        final sequence = bridge.sequence;
        actions.add('local:$sequence');
        sync.capturedLocal(sequence);
        sync.confirmedLocal(sequence);
      },
      receiveRemote: () async {
        final sequence = remoteSequence;
        attempts.add(sequence);
        final old = oldReceive;
        if (sequence == 10 && old != null && !oldEntered) {
          oldEntered = true;
          await old.future;
          throw const _NotStarted();
        }
        final remaining = failuresRemaining[sequence] ?? 0;
        if (remaining > 0) {
          failuresRemaining[sequence] = remaining - 1;
          throw const _NotStarted();
        }
        actions.add('remote:$sequence');
      },
      waitIdle: () async {},
      watchRemote: (_) {},
      cancelOlder: () {},
      onRemoteDelay: (_) {
        delayCalls++;
        if (throwDelayHook) throw StateError('private observational failure');
      },
      onIgnored: (reason, remote) => ignored.add((reason, remote)),
      onFailure: () => failures++,
    );
  }

  final bridge = _MetadataBridge();
  late final ClipboardSync sync;
  final bool throwDelayHook;
  final actions = <String>[];
  final attempts = <int>[];
  final ignored = <(int, bool)>[];
  final failuresRemaining = <int, int>{};
  int remoteSequence = 0, delayCalls = 0, failures = 0;
  bool oldEntered = false;
  Completer<void>? oldReceive;

  Future<void> activate(WidgetTester tester) async {
    await sync.setActive(true);
    await tester.pump(const Duration(seconds: 1));
    actions.clear();
    attempts.clear();
  }

  void remote(int sequence) {
    remoteSequence = sequence;
    sync.remoteChanged(sequence);
  }

  void local(int sequence) => bridge.changed(sequence);

  void releaseOld() {
    final gate = oldReceive;
    if (gate != null && !gate.isCompleted) gate.complete();
  }

  Future<void> finish(WidgetTester tester) async {
    releaseOld();
    // Drain each separately scheduled retry and any obsolete replay before
    // assertions. A single large pump would only reach its first timer.
    for (var n = 0; n < 12; n++) {
      await tester.pump(const Duration(milliseconds: 100));
    }
    await tester.runAsync(() async {
      await sync.close();
      await bridge.events.close();
    });
    await tester.pump();
  }
}

void main() {
  testWidgets('remote-delay diagnostics failure cannot discard a remote copy', (
    tester,
  ) async {
    final fixture = _Fixture(throwDelayHook: true);
    try {
      await fixture.activate(tester);
      fixture.remote(20);
    } finally {
      await fixture.finish(tester);
    }
    expect(fixture.delayCalls, 1, reason: 'the actual observational hook ran');
    expect(fixture.actions, ['remote:20']);
    expect(fixture.failures, 0);
  });

  testWidgets('late old NotStarted cannot replay after a newer local intent', (
    tester,
  ) async {
    final fixture = _Fixture()..oldReceive = Completer<void>();
    var held = false;
    try {
      await fixture.activate(tester);
      fixture.remote(10);
      await tester.pump(const Duration(milliseconds: 100));
      held = fixture.oldEntered && fixture.actions.isEmpty;
      fixture.local(2);
      fixture.releaseOld();
    } finally {
      await fixture.finish(tester);
    }
    expect(
      held,
      isTrue,
      reason: 'the actual receive callback awaited the gate',
    );
    expect(fixture.actions, ['local:2']);
    expect(fixture.attempts.where((value) => value == 10).length, 1);
    expect(fixture.failures, 0);
  });

  testWidgets(
    'late old NotStarted cannot consume a newer remote retry budget',
    (tester) async {
      final fixture = _Fixture()
        ..oldReceive = Completer<void>()
        ..failuresRemaining[20] = 3;
      var held = false;
      try {
        await fixture.activate(tester);
        fixture.remote(10);
        await tester.pump(const Duration(milliseconds: 100));
        held = fixture.oldEntered && fixture.actions.isEmpty;
        fixture.remote(20);
        fixture.releaseOld();
      } finally {
        await fixture.finish(tester);
      }
      expect(
        held,
        isTrue,
        reason: 'the obsolete receive was still awaiting return',
      );
      expect(fixture.actions, ['remote:20']);
      expect(
        fixture.attempts.where((value) => value == 20).length,
        4,
        reason: 'the newer intent owns its initial attempt and three retries',
      );
      expect(
        fixture.ignored.where((value) => value.$1 == ClipboardIgnored.gaveUp),
        isEmpty,
      );
      expect(fixture.failures, 0);
    },
  );
}
