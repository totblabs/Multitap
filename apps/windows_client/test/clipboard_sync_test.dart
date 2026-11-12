import 'dart:async';

import 'package:flutter_test/flutter_test.dart';
import 'package:multitap_client/data/clipboard_policy.dart';
import 'package:multitap_client/data/clipboard_sync.dart';
import 'package:multitap_client/data/native_clipboard.dart';
import 'package:multitap_client/data/remote_input_gate.dart';

// Metadata-only fake: no MethodChannel, platform clipboard or OS input calls.
class NativeObservationFake implements ClipboardObservationBridge {
  final events = StreamController<ClipboardChange>.broadcast(sync: true);
  final watches = <({String owner, bool enabled})>[];
  final pendingWatches = <Completer<int>>[];
  int sequence = 1;
  Completer<int>? nextWatch;
  Object? watchFailure;
  @override
  int? lastCommittedSequence;
  @override
  Stream<ClipboardChange> get changes => events.stream;
  @override
  Future<int> watch(String owner, bool enabled) {
    watches.add((owner: owner, enabled: enabled));
    if (watchFailure case final Object error) return Future.error(error);
    final pending = nextWatch;
    nextWatch = null;
    if (pending != null) {
      pendingWatches.add(pending);
      return pending.future;
    }
    return Future.value(sequence);
  }

  void change(int value, {String owner = 'owned-session'}) {
    sequence = value;
    events.add(ClipboardChange(owner, value));
  }
}

/// A receive refused before it started (another transfer was running).
class _NotStarted implements Exception, ClipboardNotStarted {
  const _NotStarted();
}

class SyncFixture {
  // Earlier fixtures assumed one 80 ms debounce for both directions.
  SyncFixture({
    ClipboardPolicy policy = const ClipboardPolicy.all(),
    Duration remoteQuiet = const Duration(milliseconds: 80),
    bool observeInitialLocalOnly = false,
    int? receivedRemoteSequence,
  }) {
    sync = ClipboardSync(
      owner: 'owned-session',
      bridge: bridge,
      policy: policy,
      observeInitialLocalOnly: observeInitialLocalOnly,
      receivedRemoteSequence: receivedRemoteSequence,
      sendLocal: () async {
        final captured = bridge.sequence;
        if (!sync.allowsAutomaticLocal(captured)) return;
        captures.add(captured);
        sync.capturedLocal(captured);
        final pending = copyGate;
        if (pending != null) await pending.future;
        if (copyFailure case final Object error) throw error;
        if (confirmCopies) sync.confirmedLocal(captured);
        completedCopies.add(captured);
      },
      receiveRemote: () async {
        if (receiveFailures > 0) {
          receiveFailures--;
          throw const _NotStarted();
        }
        receives++;
      },
      onIgnored: (reason, remote) => ignored.add((reason, remote)),
      watchRemote: remoteWatches.add,
      waitIdle: () async {
        idleWaits++;
        final pending = idleGate;
        if (pending != null) await pending.future;
      },
      onFailure: () => failures++,
      remoteQuiet: remoteQuiet,
    );
  }
  final bridge = NativeObservationFake();
  late final ClipboardSync sync;
  final captures = <int>[], completedCopies = <int>[], remoteWatches = <bool>[];
  final ignored = <(int, bool)>[];
  int receiveFailures = 0;
  Completer<void>? copyGate, idleGate;
  Object? copyFailure;
  bool confirmCopies = true;
  int receives = 0, failures = 0, idleWaits = 0;
  Future<void> close(WidgetTester tester) async {
    if (copyGate case final Completer<void> pending) {
      if (!pending.isCompleted) pending.complete();
    }
    if (idleGate case final Completer<void> pending) {
      if (!pending.isCompleted) pending.complete();
    }
    for (final pending in bridge.pendingWatches) {
      if (!pending.isCompleted) pending.complete(bridge.sequence);
    }
    await tester.pump();
    await tester.runAsync(() async {
      await sync.close();
      await bridge.events.close();
    });
  }
}

Future<void> debounce(WidgetTester tester) =>
    tester.pump(const Duration(milliseconds: 81));

void main() {
  testWidgets(
    'uncertain recovery observes the native baseline, then sends a genuine new copy',
    (tester) async {
      final f = SyncFixture(observeInitialLocalOnly: true);
      await f.sync.setActive(true);
      await debounce(tester);
      expect(f.captures, isEmpty);
      expect(await f.sync.preparePaste(), isFalse);
      f.bridge.change(2);
      await debounce(tester);
      expect(f.captures, [2]);
      expect(await f.sync.preparePaste(), isTrue);
      await f.close(tester);
    },
  );

  testWidgets(
    'baseline waits for the native watch result and retains a newer event during that wait',
    (tester) async {
      final f = SyncFixture(observeInitialLocalOnly: true);
      final gate = Completer<int>();
      f.bridge.nextWatch = gate;
      final active = f.sync.setActive(true);
      await tester.pump();
      f.bridge.change(2);
      await debounce(tester);
      expect(f.captures, isEmpty);
      gate.complete(1); // the actual native snapshot preceded the genuine copy
      await active;
      await debounce(tester);
      expect(f.captures, [2]);
      await f.close(tester);
    },
  );

  testWidgets(
    'a late old notification cannot resend the actual baseline snapshot',
    (tester) async {
      final f = SyncFixture(observeInitialLocalOnly: true);
      f.bridge.sequence = 2;
      await f.sync.setActive(true);
      f.bridge.events.add(const ClipboardChange('owned-session', 1));
      await debounce(tester);
      expect(f.captures, isEmpty);
      f.bridge.change(3);
      await debounce(tester);
      expect(f.captures, [3]);
      await f.close(tester);
    },
  );

  testWidgets(
    'an empty remote with no observed baseline does not lose its first genuine copy',
    (tester) async {
      final f = SyncFixture(observeInitialLocalOnly: true);
      await f.sync.setActive(true);
      f.sync.remoteChanged(1);
      await debounce(tester);
      expect(f.receives, 1);
      await f.close(tester);
    },
  );

  testWidgets(
    'only the remote sequence already read before recovery is suppressed',
    (tester) async {
      final f = SyncFixture(
        observeInitialLocalOnly: true,
        receivedRemoteSequence: 20,
      );
      await f.sync.setActive(true);
      f.sync.remoteChanged(20);
      await debounce(tester);
      expect(f.receives, 0);
      f.sync.remoteChanged(21);
      await debounce(tester);
      expect(f.receives, 1);
      await f.close(tester);
    },
  );

  testWidgets(
    'inactive, unfocused and default-denied sharing never captures content',
    (tester) async {
      final inactive = SyncFixture();
      inactive.bridge.change(2);
      inactive.sync.remoteChanged(2);
      await debounce(tester);
      expect(inactive.captures, isEmpty);
      expect(inactive.receives, 0);
      expect(inactive.bridge.watches, isEmpty);
      await inactive.sync.setActive(true);
      await inactive.sync.setActive(false);
      // Local content is never captured once the window is left, and a VM
      // copy announced after the grace is not pulled.
      inactive.bridge.change(3);
      await tester.pump(const Duration(seconds: 3));
      inactive.sync.remoteChanged(3);
      await debounce(tester);
      expect(inactive.captures, isEmpty);
      expect(inactive.receives, 0);
      await inactive.close(tester);
      final denied = SyncFixture(policy: const ClipboardPolicy());
      await denied.sync.setActive(true);
      denied.bridge.change(4);
      denied.sync.remoteChanged(4);
      await debounce(tester);
      expect(denied.captures, isEmpty);
      expect(denied.receives, 0);
      expect(denied.bridge.watches.every((call) => !call.enabled), isTrue);
      expect(denied.remoteWatches.where((enabled) => enabled), isEmpty);
      await denied.close(tester);
    },
  );

  testWidgets(
    'permitted metadata bursts capture only their latest local sequence',
    (tester) async {
      final f = SyncFixture();
      await f.sync.setActive(true);
      await debounce(tester);
      expect(f.captures, [1]);
      f.bridge.change(2);
      f.bridge.change(3);
      f.bridge.change(4);
      await tester.pump(const Duration(milliseconds: 79));
      expect(f.captures, [1]);
      await tester.pump(const Duration(milliseconds: 2));
      expect(f.captures, [1, 4]);
      expect(f.completedCopies, [1, 4]);
      await f.close(tester);
    },
  );

  testWidgets(
    'DWORD wrap is a new sequence and duplicates or foreign owners never recapture',
    (tester) async {
      final f = SyncFixture();
      f.bridge.sequence = 0xfffffffe;
      await f.sync.setActive(true);
      await debounce(tester);
      f.bridge.change(0xffffffff);
      await debounce(tester);
      f.bridge.change(0);
      await debounce(tester);
      expect(f.captures, [0xfffffffe, 0xffffffff, 0]);
      f.bridge.change(0);
      f.bridge.change(5, owner: 'another-session');
      f.bridge.change(-1);
      f.bridge.change(0x100000000);
      await debounce(tester);
      expect(f.captures, [0xfffffffe, 0xffffffff, 0]);
      await f.close(tester);
    },
  );

  testWidgets(
    'received clipboard application suppresses its local echo and duplicate remote notifications',
    (tester) async {
      final f = SyncFixture();
      await f.sync.setActive(true);
      await debounce(tester);
      f.sync.appliedLocal(2);
      f.bridge.change(2);
      await debounce(tester);
      expect(f.captures, [1]);
      f.sync.remoteChanged(0xffffffff);
      f.sync.remoteChanged(0);
      await debounce(tester);
      expect(f.receives, 1);
      f.sync.remoteChanged(0);
      f.sync.remoteChanged(-1);
      f.sync.remoteChanged(0x100000000);
      await debounce(tester);
      expect(f.receives, 1);
      expect(f.captures, [1]);
      await f.close(tester);
    },
  );

  testWidgets(
    'directional policy enables only the permitted observation and transfer path',
    (tester) async {
      final receive = SyncFixture(
        policy: const ClipboardPolicy(filesToLocal: true),
      );
      await receive.sync.setActive(true);
      receive.bridge.change(2);
      receive.sync.remoteChanged(7);
      await debounce(tester);
      expect(receive.captures, isEmpty);
      expect(receive.receives, 1);
      expect(receive.bridge.watches.every((call) => !call.enabled), isTrue);
      expect(receive.remoteWatches, [true]);
      await receive.close(tester);
      final send = SyncFixture(
        policy: const ClipboardPolicy(textToRemote: true),
      );
      await send.sync.setActive(true);
      send.sync.remoteChanged(3);
      await debounce(tester);
      expect(send.captures, [1]);
      expect(send.receives, 0);
      expect(send.remoteWatches.where((enabled) => enabled), isEmpty);
      await send.close(tester);
    },
  );

  testWidgets(
    'late watch result cannot schedule an old sequence after focus generation changes',
    (tester) async {
      final f = SyncFixture(), lateWatch = Completer<int>();
      f.bridge.nextWatch = lateWatch;
      final first = f.sync.setActive(true);
      await tester.pump();
      final inactive = f.sync.setActive(false);
      f.bridge.sequence = 3;
      final resumed = f.sync.setActive(true);
      lateWatch.complete(99);
      await tester.pump();
      await Future.wait([first, inactive, resumed]);
      await debounce(tester);
      expect(f.captures, [3]);
      expect(f.remoteWatches.where((enabled) => enabled).length, 1);
      expect(f.bridge.watches.map((call) => call.enabled), [true, false, true]);
      await f.close(tester);
    },
  );

  testWidgets(
    'closing during a pending watch prevents late capture and remote watch activation',
    (tester) async {
      final f = SyncFixture(), lateWatch = Completer<int>();
      f.bridge.nextWatch = lateWatch;
      final activation = f.sync.setActive(true);
      await tester.pump();
      late Future<void> closing;
      await tester.runAsync(() async {
        closing = f.sync.close();
      });
      lateWatch.complete(5);
      await tester.pump();
      await activation;
      await tester.runAsync(() async {
        await closing;
      });
      await debounce(tester);
      f.bridge.change(6);
      f.sync.remoteChanged(6);
      await debounce(tester);
      expect(f.sync.active, isFalse);
      expect(f.captures, isEmpty);
      expect(f.receives, 0);
      expect(f.remoteWatches.where((enabled) => enabled), isEmpty);
      await f.close(tester);
    },
  );

  testWidgets(
    'waiting on an old native transfer cannot capture while focus is gone',
    (tester) async {
      final f = SyncFixture();
      await f.sync.setActive(true);
      final idle = Completer<void>();
      f.idleGate = idle;
      await debounce(tester);
      expect(f.captures, isEmpty);
      await f.sync.setActive(false);
      idle.complete();
      await tester.pump();
      await debounce(tester);
      expect(f.captures, isEmpty);
      expect(f.receives, 0);
      await f.close(tester);
    },
  );

  testWidgets(
    'preparePaste waits for an acknowledged file commit before any V key is replayed',
    (tester) async {
      final f = SyncFixture();
      f.copyGate = Completer<void>();
      await f.sync.setActive(true);
      final sent = <Map<String, dynamic>>[];
      final gate = RemoteInputGate(
        send: (event) => sent.add(Map.from(event)),
        preparePaste: f.sync.preparePaste,
        pasteEnabled: () => f.sync.active,
        onPasteBlocked: () => fail('approved commit was blocked'),
      );
      Map<String, dynamic> key(int vk, bool down) => {
        'type': 'input',
        'event': 'key',
        'vk': vk,
        'down': down,
      };
      gate.add(key(0x11, true));
      gate.add(key(0x56, true));
      gate.add(key(0x56, false));
      gate.add(key(0x11, false));
      await tester.pump();
      expect(f.captures, [1]);
      expect(f.completedCopies, isEmpty);
      expect(sent.any((event) => event['vk'] == 0x56), isFalse);
      f.copyGate!.complete();
      await tester.pump();
      expect(f.completedCopies, [1]);
      expect(sent.skip(2), [
        key(0x11, true),
        key(0x56, true),
        key(0x56, false),
        key(0x11, false),
      ]);
      gate.reset();
      await f.close(tester);
    },
  );

  testWidgets(
    'capturing without a confirmed server completion can never authorize paste',
    (tester) async {
      final f = SyncFixture()..confirmCopies = false;
      await f.sync.setActive(true);
      await debounce(tester);
      expect(f.captures, [1]);
      expect(await f.sync.preparePaste(), isFalse);
      expect(f.captures, [
        1,
      ], reason: 'an unconfirmed capture must not retry content indefinitely');
      await f.close(tester);
    },
  );

  testWidgets(
    'preparePaste waits for native idle even when the applied sequence already matches',
    (tester) async {
      final f = SyncFixture();
      await f.sync.setActive(true);
      await debounce(tester);
      f.bridge.sequence = 2;
      f.sync.appliedLocal(2);
      final nativeTail = Completer<void>();
      f.idleGate = nativeTail;
      bool? allowed;
      final preparation = f.sync.preparePaste().then(
        (value) => allowed = value,
      );
      await tester.pump();
      expect(allowed, isNull);
      expect(f.captures, [1]);
      nativeTail.complete();
      await tester.pump();
      await preparation;
      expect(allowed, isTrue);
      expect(f.captures, [1]);
      await f.close(tester);
    },
  );

  testWidgets(
    'focus loss during file copy rejects old paste and late completion cannot replace new metadata',
    (tester) async {
      final f = SyncFixture();
      final commit = Completer<void>();
      f.copyGate = commit;
      await f.sync.setActive(true);
      bool? oldPaste;
      final preparation = f.sync.preparePaste().then(
        (value) => oldPaste = value,
      );
      await tester.pump();
      expect(f.captures, [1]);
      await f.sync.setActive(false);
      f.bridge.sequence = 2;
      await f.sync.setActive(true);
      commit.complete();
      await tester.pump();
      await preparation;
      expect(oldPaste, isFalse);
      await debounce(tester);
      expect(f.captures, [1, 2]);
      expect(f.remoteWatches.last, isTrue);
      await f.close(tester);
    },
  );

  testWidgets(
    'observation errors are contained and closing fences later failures',
    (tester) async {
      final f = SyncFixture();
      f.bridge.watchFailure = StateError('fixture observation failure');
      await f.sync.setActive(true);
      expect(f.failures, 1);
      await debounce(tester);
      expect(f.captures, isEmpty);
      await f.sync.setActive(false);
      expect(f.failures, 1);
      await f.close(tester);
    },
  );

  testWidgets(
    'every announced VM change that does not become a transfer leaves a reason',
    (tester) async {
      final f = SyncFixture();
      f.sync.remoteChanged(2);
      expect(f.ignored, [(ClipboardIgnored.inactive, true)]);
      await f.sync.setActive(true);
      await debounce(tester);
      f.sync.remoteChanged(7);
      await debounce(tester);
      expect(f.receives, 1);
      f.sync.remoteChanged(7);
      expect(f.ignored.last, (ClipboardIgnored.duplicate, true));
      await f.close(tester);
      final denied = SyncFixture(
        policy: const ClipboardPolicy(textToRemote: true),
      );
      await denied.sync.setActive(true);
      await debounce(tester);
      denied.sync.remoteChanged(3);
      expect(denied.ignored, [(ClipboardIgnored.denied, true)]);
      await denied.close(tester);
    },
  );

  testWidgets(
    'a receive that could not start is tried again instead of being forgotten',
    (tester) async {
      final f = SyncFixture()..receiveFailures = 2;
      await f.sync.setActive(true);
      await debounce(tester);
      f.sync.remoteChanged(9);
      await debounce(tester);
      expect(f.receives, 0);
      for (var n = 0; n < 6 && f.receives == 0; n++) {
        await debounce(tester);
      }
      expect(f.receives, 1, reason: 'the third attempt reads the same change');
      expect(f.receiveFailures, 0);
      expect(f.ignored.where((e) => e.$1 == ClipboardIgnored.gaveUp), isEmpty);
      await f.close(tester);
      final stuck = SyncFixture()..receiveFailures = 99;
      await stuck.sync.setActive(true);
      await debounce(tester);
      stuck.sync.remoteChanged(4);
      for (var n = 0; n < 8; n++) {
        await debounce(tester);
      }
      expect(stuck.receives, 0);
      expect(
        stuck.ignored.where((e) => e.$1 == ClipboardIgnored.gaveUp),
        [(ClipboardIgnored.gaveUp, true)],
        reason:
            'a refusal that never ends is recorded once, not retried forever',
      );
      await stuck.close(tester);
    },
  );

  testWidgets(
    'a copy made in the VM just before leaving the window is still pulled; one after the grace is not',
    (tester) async {
      final f = SyncFixture();
      await f.sync.setActive(true);
      await debounce(tester);
      expect(f.remoteWatches, [true]);
      // Announced while here but still settling when the user leaves.
      f.sync.remoteChanged(11);
      await f.sync.setActive(false);
      expect(f.remoteWatches, [
        true,
      ], reason: 'the remote watch stays on for the grace');
      await debounce(tester);
      expect(f.receives, 1);
      // Announced after leaving, inside the grace (Host quiet period, relay delay).
      await tester.pump(const Duration(milliseconds: 1000));
      f.sync.remoteChanged(12);
      await debounce(tester);
      expect(f.receives, 2);
      expect(
        f.ignored.where((e) => e.$1 == ClipboardIgnored.readAfterLeaving),
        hasLength(1),
      );
      expect(
        f.captures.length,
        1,
        reason: 'only the first activation captured local content',
      );
      // The grace ends: the watch is turned off and later copies are not pulled.
      await tester.pump(const Duration(seconds: 2));
      expect(f.remoteWatches, [true, false]);
      f.sync.remoteChanged(13);
      await debounce(tester);
      expect(f.receives, 2);
      expect(f.ignored.last, (ClipboardIgnored.inactive, true));
      await f.close(tester);
      // Coming back inside the grace keeps the watch (no off/on flap) and ends it.
      final back = SyncFixture();
      await back.sync.setActive(true);
      await debounce(tester);
      await back.sync.setActive(false);
      await tester.pump(const Duration(milliseconds: 500));
      await back.sync.setActive(true);
      await tester.pump(const Duration(seconds: 3));
      expect(back.remoteWatches.contains(false), isFalse);
      await back.close(tester);
      // Closing never reads and turns the watch off at once.
      final closing = SyncFixture();
      await closing.sync.setActive(true);
      await debounce(tester);
      closing.sync.remoteChanged(13);
      await closing.close(tester);
      await tester.pump(const Duration(seconds: 3));
      expect(closing.receives, 0);
      expect(closing.remoteWatches.last, isFalse);
    },
  );
}
