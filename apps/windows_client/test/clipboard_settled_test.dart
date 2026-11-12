import 'dart:async';

import 'package:flutter_test/flutter_test.dart';
import 'package:multitap_client/data/clipboard_policy.dart';
import 'package:multitap_client/data/clipboard_sync.dart';

import 'clipboard_sync_test.dart' show NativeObservationFake;

class _NotStarted implements Exception, ClipboardNotStarted {}

void main() {
  testWidgets(
    'settled sequence enters the existing serialized drain without another 300ms wait',
    (tester) async {
      final board = NativeObservationFake(), reads = <int>[];
      final idle = Completer<void>();
      final sync = ClipboardSync(
        owner: 'owned-session',
        bridge: board,
        policy: const ClipboardPolicy(textToLocal: true),
        sendLocal: () async {},
        receiveRemote: () async => fail('settled source lost its sequence'),
        receiveSettledRemote: (sequence) async => reads.add(sequence),
        watchRemote: (_) {},
        waitIdle: () => idle.future,
        onFailure: () {},
      );
      await sync.setActive(true);
      sync.remoteChanged(41, settled: true);
      await tester.pump(Duration.zero);
      expect(reads, isEmpty);
      idle.complete();
      await tester.pump(Duration.zero);
      expect(reads, [41]);
      await tester.runAsync(() async {
        await sync.close();
        await board.events.close();
      });
    },
  );
  testWidgets(
    'legacy notifications retain 300ms; a later plain source clears the fast hint',
    (tester) async {
      final board = NativeObservationFake(), fast = <int>[];
      var plain = 0;
      final sync = ClipboardSync(
        owner: 'owned-session',
        bridge: board,
        policy: const ClipboardPolicy(textToLocal: true),
        sendLocal: () async {},
        receiveRemote: () async {
          plain++;
        },
        receiveSettledRemote: (s) async => fast.add(s),
        watchRemote: (_) {},
        waitIdle: () async {},
        onFailure: () {},
      );
      await sync.setActive(true);
      sync.remoteChanged(41, settled: true);
      sync.remoteChanged(42);
      await tester.pump(const Duration(milliseconds: 299));
      expect(plain, 0);
      expect(fast, isEmpty);
      await tester.pump(const Duration(milliseconds: 1));
      expect(plain, 1);
      expect(fast, isEmpty);
      await tester.runAsync(() async {
        await sync.close();
        await board.events.close();
      });
    },
  );
  testWidgets(
    'a newer local copy or closed lifetime discards the queued settled source',
    (tester) async {
      final board = NativeObservationFake();
      var received = 0;
      final sync = ClipboardSync(
        owner: 'owned-session',
        bridge: board,
        policy: const ClipboardPolicy.all(),
        sendLocal: () async {},
        receiveRemote: () async {
          received++;
        },
        receiveSettledRemote: (_) async {
          received++;
        },
        watchRemote: (_) {},
        waitIdle: () async {},
        onFailure: () {},
      );
      await sync.setActive(true);
      await tester.pump(const Duration(milliseconds: 81));
      sync.remoteChanged(41, settled: true);
      board.change(2);
      await tester.pump();
      expect(received, 0);
      await tester.runAsync(sync.close);
      sync.remoteChanged(42, settled: true);
      await tester.pump();
      expect(received, 0);
      await tester.runAsync(board.events.close);
    },
  );
  testWidgets(
    'unsupported peers and retries cannot turn a hint into a zero-delay spin',
    (tester) async {
      final board = NativeObservationFake();
      var plain = 0;
      final sync = ClipboardSync(
        owner: 'owned-session',
        bridge: board,
        policy: const ClipboardPolicy(textToLocal: true),
        sendLocal: () async {},
        receiveRemote: () async {
          plain++;
        },
        watchRemote: (_) {},
        waitIdle: () async {},
        onFailure: () {},
      );
      await sync.setActive(true);
      sync.remoteChanged(0, settled: true);
      await tester.pump(const Duration(milliseconds: 299));
      expect(plain, 0);
      await tester.pump(const Duration(milliseconds: 1));
      expect(plain, 1);
      await tester.runAsync(() async {
        await sync.close();
        await board.events.close();
      });
    },
  );
  testWidgets(
    'a peer that announces only after its own quiet is read at once, files too; a retry still waits',
    (tester) async {
      final board = NativeObservationFake();
      var plain = 0;
      final sync = ClipboardSync(
        owner: 'owned-session',
        bridge: board,
        policy: const ClipboardPolicy(filesToLocal: true),
        sendLocal: () async {},
        receiveRemote: () async {
          if (++plain == 1) throw _NotStarted();
        },
        remoteDebounced: true,
        watchRemote: (_) {},
        waitIdle: () async {},
        onFailure: () {},
      );
      await sync.setActive(true);
      sync.remoteChanged(42);
      await tester.pump(Duration.zero);
      expect(plain, 1);
      await tester.pump(const Duration(milliseconds: 299));
      expect(plain, 1);
      await tester.pump(const Duration(milliseconds: 1));
      expect(plain, 2);
      await tester.runAsync(() async {
        await sync.close();
        await board.events.close();
      });
    },
  );
  testWidgets(
    'not-started settled read retries through the original quiet delay',
    (tester) async {
      final board = NativeObservationFake();
      var fast = 0, plain = 0;
      final sync = ClipboardSync(
        owner: 'owned-session',
        bridge: board,
        policy: const ClipboardPolicy(textToLocal: true),
        sendLocal: () async {},
        receiveRemote: () async {
          plain++;
        },
        receiveSettledRemote: (_) async {
          fast++;
          throw _NotStarted();
        },
        watchRemote: (_) {},
        waitIdle: () async {},
        onFailure: () {},
      );
      await sync.setActive(true);
      sync.remoteChanged(41, settled: true);
      await tester.pump(Duration.zero);
      expect(fast, 1);
      expect(plain, 0);
      await tester.pump(const Duration(milliseconds: 299));
      expect(plain, 0);
      expect(fast, 1);
      await tester.pump(const Duration(milliseconds: 1));
      expect(plain, 1);
      expect(fast, 1);
      await tester.runAsync(() async {
        await sync.close();
        await board.events.close();
      });
    },
  );
}
