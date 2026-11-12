// Real ClipboardSync with fake time and metadata-only callbacks. No channel,
// OS clipboard, files, input, guest or network is accessed.
import 'dart:async';

import 'package:flutter_test/flutter_test.dart';
import 'package:multitap_client/data/clipboard_policy.dart';
import 'package:multitap_client/data/clipboard_sync.dart';
import 'package:multitap_client/data/native_clipboard.dart';

const _owner = 'quiet-race-fixture';

class _Metadata implements ClipboardObservationBridge {
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

class _NotStarted implements Exception, ClipboardNotStarted {}

class _Fixture {
  final bridge = _Metadata();
  final actions = <String>[], fastAttempts = <int>[], plainAttempts = <int>[];
  final ignored = <int>[];
  int remoteSequence = 0, idleWaits = 0, failures = 0;
  Completer<void>? idle, receive;
  late final ClipboardSync sync = ClipboardSync(
    owner: _owner,
    bridge: bridge,
    policy: const ClipboardPolicy.all(),
    sendLocal: () async {
      final sequence = bridge.sequence;
      actions.add('local:$sequence');
      sync.capturedLocal(sequence);
      sync.confirmedLocal(sequence);
    },
    receiveRemote: () async {
      final sequence = remoteSequence;
      plainAttempts.add(sequence);
      if (failures > 0) {
        failures--;
        throw _NotStarted();
      }
      await receive?.future;
      actions.add('plain:$sequence');
    },
    receiveSettledRemote: (sequence) async {
      fastAttempts.add(sequence);
      if (failures > 0) {
        failures--;
        throw _NotStarted();
      }
      await receive?.future;
      actions.add('settled:$sequence');
    },
    waitIdle: () async {
      final pending = idle;
      if (pending != null && !pending.isCompleted) {
        idleWaits++;
        await pending.future;
      }
    },
    watchRemote: (_) {},
    onIgnored: (reason, _) => ignored.add(reason),
    onFailure: () {},
  );

  void remote(int sequence, {bool settled = false}) {
    remoteSequence = sequence;
    sync.remoteChanged(sequence, settled: settled);
  }

  void releaseIdle() {
    final pending = idle;
    if (pending != null && !pending.isCompleted) pending.complete();
  }

  void releaseReceive() {
    final pending = receive;
    if (pending != null && !pending.isCompleted) pending.complete();
  }

  Future<void> run(
    WidgetTester tester,
    Future<void> Function() scenario,
  ) async {
    try {
      await sync.setActive(true);
      await tester.pump(const Duration(seconds: 1));
      actions.clear();
      await scenario();
    } finally {
      releaseIdle();
      releaseReceive();
      await tester.runAsync(() async {
        await sync.close();
        await bridge.events.close();
      });
      await tester.pump(Duration.zero);
    }
  }
}

void main() {
  for (final firstSettled in [true, false]) {
    testWidgets(
      'quiet race: a held ${firstSettled ? 'settled' : 'legacy'} drain cannot read a newer plain copy early',
      (tester) async {
        final f = _Fixture();
        await f.run(tester, () async {
          f.idle = Completer<void>();
          f.remote(10, settled: firstSettled);
          await tester.pump(
            firstSettled ? Duration.zero : const Duration(milliseconds: 300),
          );
          expect(f.idleWaits, 1);
          f.remote(11);
          await tester.pump(const Duration(milliseconds: 200));
          f.releaseIdle();
          await tester.pump(Duration.zero);
          expect(
            f.actions,
            isEmpty,
            reason: 'the new observation still needs 100 ms',
          );
          await tester.pump(const Duration(milliseconds: 99));
          expect(f.actions, isEmpty);
          await tester.pump(const Duration(milliseconds: 1));
          expect(
            f.actions,
            ['plain:11'],
            reason:
                'keep the original deadline instead of adding another 300 ms',
          );
        });
      },
    );
  }

  testWidgets(
    'quiet race: a newer local copy keeps its own 80 ms quiet window',
    (tester) async {
      final f = _Fixture();
      await f.run(tester, () async {
        f.idle = Completer<void>();
        f.remote(10, settled: true);
        await tester.pump(Duration.zero);
        f.bridge.changed(2);
        await tester.pump(const Duration(milliseconds: 40));
        f.releaseIdle();
        await tester.pump(Duration.zero);
        expect(f.actions, isEmpty);
        await tester.pump(const Duration(milliseconds: 39));
        expect(f.actions, isEmpty);
        await tester.pump(const Duration(milliseconds: 1));
        expect(f.actions, ['local:2']);
      });
    },
  );

  testWidgets(
    'quiet race: elapsed plain quiet during a transfer is not charged twice',
    (tester) async {
      final f = _Fixture();
      await f.run(tester, () async {
        f.receive = Completer<void>();
        f.remote(10, settled: true);
        await tester.pump(Duration.zero);
        expect(f.fastAttempts, [10]);
        f.remote(11);
        await tester.pump(const Duration(milliseconds: 300));
        expect(f.plainAttempts, isEmpty);
        f.releaseReceive();
        await tester.pump(Duration.zero);
        await tester.pump(const Duration(milliseconds: 1));
        expect(f.actions, ['settled:10', 'plain:11']);
      });
    },
  );

  testWidgets(
    'quiet race: a transfer completing before new quiet leaves the remaining timer intact',
    (tester) async {
      final f = _Fixture();
      await f.run(tester, () async {
        f.receive = Completer<void>();
        f.remote(10, settled: true);
        await tester.pump(Duration.zero);
        f.remote(11);
        await tester.pump(const Duration(milliseconds: 200));
        f.releaseReceive();
        await tester.pump(Duration.zero);
        expect(f.actions, ['settled:10']);
        await tester.pump(const Duration(milliseconds: 99));
        expect(f.actions, ['settled:10']);
        await tester.pump(const Duration(milliseconds: 1));
        expect(f.actions, ['settled:10', 'plain:11']);
      });
    },
  );

  testWidgets(
    'quiet race: a new quiet window already elapsed behind idle proceeds immediately',
    (tester) async {
      final f = _Fixture();
      await f.run(tester, () async {
        f.idle = Completer<void>();
        f.remote(10, settled: true);
        await tester.pump(Duration.zero);
        f.remote(11);
        await tester.pump(const Duration(milliseconds: 300));
        f.releaseIdle();
        await tester.pump(Duration.zero);
        expect(f.actions, ['plain:11']);
      });
    },
  );

  testWidgets(
    'quiet race: the newest settled hint retains its source sequence',
    (tester) async {
      final f = _Fixture();
      await f.run(tester, () async {
        f.idle = Completer<void>();
        f.remote(10, settled: true);
        await tester.pump(Duration.zero);
        f.remote(11, settled: true);
        await tester.pump(Duration.zero);
        f.releaseIdle();
        await tester.pump(Duration.zero);
        expect(f.actions, ['settled:11']);
        expect(f.fastAttempts, [11]);
      });
    },
  );

  testWidgets(
    'quiet race: an old focus generation cannot erase the new completed quiet window',
    (tester) async {
      final f = _Fixture();
      await f.run(tester, () async {
        f.idle = Completer<void>();
        f.remote(10, settled: true);
        await tester.pump(Duration.zero);
        expect(f.idleWaits, 1);
        await f.sync.setActive(false);
        await f.sync.setActive(true);
        f.remote(11);
        await tester.pump(const Duration(milliseconds: 300));
        f.releaseIdle();
        await tester.pump(Duration.zero);
        await tester.pump(const Duration(milliseconds: 1));
        expect(f.actions, ['plain:11']);
        expect(f.plainAttempts, [11]);
      });
    },
  );

  testWidgets(
    'quiet race: a refused fast read retries at ordinary quiet and stays bounded',
    (tester) async {
      final f = _Fixture();
      await f.run(tester, () async {
        f.failures = 10;
        f.remote(10, settled: true);
        await tester.pump(Duration.zero);
        expect(f.fastAttempts, [10]);
        expect(f.plainAttempts, isEmpty);
        await tester.pump(const Duration(milliseconds: 299));
        expect(f.plainAttempts, isEmpty);
        await tester.pump(const Duration(milliseconds: 1));
        expect(f.plainAttempts, [10]);
        await tester.pump(const Duration(milliseconds: 300));
        await tester.pump(const Duration(milliseconds: 300));
        await tester.pump(const Duration(seconds: 1));
        expect(f.fastAttempts, [10]);
        expect(f.plainAttempts, [10, 10, 10]);
        expect(f.actions, isEmpty);
        expect(f.ignored, contains(ClipboardIgnored.gaveUp));
      });
    },
  );
}
