import 'dart:async';
import 'dart:convert';

import 'package:fake_async/fake_async.dart';
import 'package:flutter/services.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:multitap_client/data/clipboard_limits.dart';
import 'package:multitap_client/data/clipboard_pull.dart';
import 'package:multitap_client/data/native_clipboard.dart';

import 'clipboard_onpaste_test.dart' as onpaste;
import 'clipboard_pull_test.dart' as fixture;

class _Copy {
  _Copy(this.time, {onpaste.OnPasteClipboard? withBridge, bool legacy = false})
    : bridge = withBridge ?? onpaste.OnPasteClipboard() {
    pulls = ClipboardPulls(
      owner: 'private-test-owner',
      bridge: bridge,
      onPaste: legacy ? null : bridge,
      send: sent.add,
      limits: () => ClipboardLimits.onPasteRevision,
      idPrefix: 'pullc',
      changed: () => changes++,
      started: () {},
      failed: failures.add,
      mayServe: (_) => true,
    )..start();
    publish('a');
  }
  final FakeAsync time;
  final onpaste.OnPasteClipboard bridge;
  late final ClipboardPulls pulls;
  final sent = <Map<String, dynamic>>[];
  final failures = <String>[];
  int changes = 0;
  void publish(String id) => pulls.offerPublished(id, [
    fixture.tree('one.bin', 3),
    fixture.tree('two.bin', 3),
  ]);
  void operation(String offer, bool start, {int epoch = 1, String? failure}) {
    bridge.platform(
      ClipboardPullOperation(
        start,
        offerId: offer,
        operationId: epoch,
        failureCode: failure,
      ),
    );
    time.flushMicrotasks();
  }

  void request(String offer, int range, {int index = 0}) {
    bridge.platform(ClipboardPullRequest(range, offer, index, 0));
    time.flushMicrotasks();
  }

  void finish() {
    final request = sent.lastWhere((row) => row['type'] == 'clipboard.pull');
    pulls.handle({
      'type': 'clipboard.chunk',
      'requestId': request['requestId'],
      'index': request['index'],
      'offset': 0,
      'data': base64Encode([1, 2, 3]),
    });
    time.flushMicrotasks();
    pulls.handle({
      'type': 'clipboard.item.end',
      'requestId': request['requestId'],
      'index': request['index'],
      'sha256': 'a' * 64,
    });
    time.flushMicrotasks();
  }

  void close() {
    pulls.close('clipboard_session_closed');
    time.flushMicrotasks();
  }
}

class _DelayedFailure extends onpaste.OnPasteClipboard {
  final gate = Completer<void>();
  @override
  Future<void> pullFail(int rangeId, String reason) async {
    if (rangeId == 21) await gate.future;
    await super.pullFail(rangeId, reason);
  }
}

void main() {
  TestWidgetsFlutterBinding.ensureInitialized();
  test(
    'native IPC preserves offer/epoch and accepts only fixed outcomes',
    () async {
      final bridge = NativeClipboardBridge();
      final events = <ClipboardPullEvent>[];
      final subscription = bridge.pullEvents.listen(events.add);
      Future<void> event(
        String phase, {
        String? offer = 'private-offer',
        int epoch = 17,
      }) async {
        await TestDefaultBinaryMessengerBinding.instance.defaultBinaryMessenger
            .handlePlatformMessage(
              'multitap/clipboard',
              const StandardMethodCodec().encodeMethodCall(
                MethodCall('pull', {
                  'kind': 'operation',
                  'rangeId': epoch,
                  'offerId': offer,
                  'reason': phase,
                }),
              ),
              (_) {},
            );
        await Future<void>.delayed(Duration.zero);
      }

      await event('start');
      await event('end-failed');
      await event('end-cancelled');
      await event('arbitrary-private-error');
      await event('start', offer: null);
      await event('start', epoch: 0);
      expect(events, hasLength(3));
      final operations = events.cast<ClipboardPullOperation>().toList();
      expect(
        operations.map((event) => event.offerId),
        everyElement('private-offer'),
      );
      expect(operations.map((event) => event.rangeId), everyElement(17));
      expect(operations.map((event) => event.failureCode), [
        null,
        'clipboard_transfer_failed',
        'clipboard_cancelled',
      ]);
      await subscription.cancel();
    },
  );
  test('same-offer newer epoch survives an older End failure awaiting native cleanup', () {
    fakeAsync((time) {
      final bridge = _DelayedFailure();
      final copy = _Copy(time, withBridge: bridge);
      copy.operation('a', true);
      copy.request('a', 21);
      copy.operation('a', false, failure: 'clipboard_transfer_failed');
      // Native cleanup is pending. The unawaited event listener can admit a new
      // extraction of this same offer before the old End continuation runs.
      copy.operation('a', true, epoch: 2);
      copy.request('a', 22, index: 1);
      expect(copy.pulls.active, isTrue);
      expect(copy.pulls.userWaiting, isTrue);
      bridge.gate.complete();
      time.flushMicrotasks();
      expect(
        copy.failures,
        isEmpty,
        reason: 'the old failure must not overwrite the fresh epoch',
      );
      expect(copy.pulls.active, isTrue);
      expect(copy.pulls.userWaiting, isTrue);
      expect(
        copy.sent.where((row) => row['type'] == 'clipboard.pull'),
        hasLength(2),
      );
      copy.finish();
      expect(
        copy.pulls.active,
        isTrue,
        reason: 'the fresh extraction still owns its file gap',
      );
      copy.operation('a', false, epoch: 2);
      expect(copy.pulls.active, isFalse);
      expect(copy.pulls.userWaiting, isFalse);
      copy.close();
    });
  });
  for (final superseding in [
    'cancel',
    'close',
    'withdraw',
    'new epoch completed',
    'withdraw and republish',
  ]) {
    test('pending old End has no failed/changed effect after $superseding', () {
      fakeAsync((time) {
        final bridge = _DelayedFailure();
        final copy = _Copy(time, withBridge: bridge);
        copy.operation('a', true);
        copy.request('a', 21);
        copy.operation('a', false, failure: 'clipboard_transfer_failed');
        switch (superseding) {
          case 'cancel':
            copy.pulls.cancel();
            time.flushMicrotasks();
          case 'close':
            copy.close();
          case 'withdraw':
            copy.pulls.offerWithdrawn('a');
          case 'new epoch completed':
            copy.operation('a', true, epoch: 2);
            copy.request('a', 22, index: 1);
            copy.finish();
            copy.operation('a', false, epoch: 2);
          case 'withdraw and republish':
            copy.pulls.offerWithdrawn('a');
            copy.publish('a');
            copy.operation('a', true, epoch: 2);
            copy.operation('a', false, epoch: 2);
        }
        final notifications = copy.changes;
        bridge.gate.complete();
        time.flushMicrotasks();
        expect(copy.failures, isEmpty);
        expect(
          copy.changes,
          notifications,
          reason: 'an obsolete continuation must not notify the new lifecycle',
        );
        expect(copy.pulls.active, isFalse);
        expect(copy.pulls.userWaiting, isFalse);
        copy.close();
      });
    });
  }
  test('old End cleanup preserves a queued other-offer fallback gap', () {
    fakeAsync((time) {
      final bridge = _DelayedFailure();
      final copy = _Copy(time, withBridge: bridge);
      copy.operation('a', true);
      copy.request('a', 21);
      copy.publish('b');
      copy.request('b', 22);
      copy.operation('a', false, failure: 'clipboard_transfer_failed');
      expect(
        copy.sent.where((row) => row['type'] == 'clipboard.pull'),
        hasLength(2),
        reason: 'queued current work must not wait for obsolete native cleanup',
      );
      copy.finish();
      time.elapse(const Duration(milliseconds: 100));
      bridge.gate.complete();
      time.flushMicrotasks();
      expect(copy.failures, isEmpty);
      expect(
        copy.pulls.active,
        isTrue,
        reason: 'the old End must not erase the newer fallback linger',
      );
      time.elapse(const Duration(seconds: 1));
      expect(copy.pulls.active, isFalse);
      copy.close();
    });
  });
  test(
    'cancel still releases detached old legacy ranges after awaited cleanup',
    () {
      fakeAsync((time) {
        final bridge = _DelayedFailure();
        final copy = _Copy(time, withBridge: bridge, legacy: true);
        copy.operation('a', true);
        copy.request('a', 21);
        copy.request('a', 23, index: 1);
        copy.operation('a', false, failure: 'clipboard_transfer_failed');
        copy.pulls.cancel();
        time.flushMicrotasks();
        final notifications = copy.changes;
        bridge.gate.complete();
        time.flushMicrotasks();
        expect(bridge.aborts, 0, reason: 'legacy has no owner-wide abort');
        expect(bridge.failedRanges.map((range) => range.$1), [21, 23]);
        expect(copy.failures, isEmpty);
        expect(copy.changes, notifications);
        expect(copy.pulls.active, isFalse);
        copy.close();
      });
    },
  );
  test('metadata operation alone stays silent and never locks interaction', () {
    fakeAsync((time) {
      final copy = _Copy(time);
      copy.operation('a', true);
      time.elapse(const Duration(seconds: 4));
      expect(copy.pulls.active, isFalse);
      expect(copy.pulls.userWaiting, isFalse);
      expect(copy.pulls.progress, isNull);
      expect(copy.sent, isEmpty);
      copy.operation('a', false);
      expect(copy.failures, isEmpty);
      copy.close();
    });
  });
  test(
    'actual on-paste operation holds a long file gap until EndOperation',
    () {
      fakeAsync((time) {
        final copy = _Copy(time);
        copy.operation('a', true);
        copy.request('a', 10);
        copy.finish();
        time.elapse(const Duration(seconds: 4));
        expect(copy.pulls.active, isTrue);
        expect(copy.pulls.userWaiting, isTrue);
        expect(copy.pulls.progress!.asyncConsumer, isFalse);
        copy.request('a', 11, index: 1);
        copy.finish();
        time.elapse(const Duration(seconds: 4));
        expect(
          copy.pulls.active,
          isTrue,
          reason: 'all file bytes precede the destination extraction result',
        );
        copy.operation('a', false);
        expect(copy.pulls.active, isFalse);
        expect(copy.pulls.userWaiting, isFalse);
        copy.close();
      });
    },
  );
  test('duplicate starts and stale epochs never retain or release another operation', () {
    fakeAsync((time) {
      final copy = _Copy(time);
      copy.operation('a', true);
      copy.operation('a', true);
      copy.request('a', 12);
      copy.finish();
      copy.operation('a', false);
      expect(copy.pulls.active, isFalse);
      copy.operation('a', true, epoch: 2);
      copy.request('a', 13, index: 1);
      copy.finish();
      copy.operation('a', false, failure: 'clipboard_transfer_failed');
      expect(copy.pulls.active, isTrue);
      expect(copy.failures, isEmpty);
      copy.operation('a', false, epoch: 2);
      expect(copy.pulls.active, isFalse);
      copy.operation('a', true);
      expect(copy.pulls.active, isFalse);
      copy.close();
    });
  });
  test(
    'old offer End failure cannot replace the new operation lock or error',
    () {
      fakeAsync((time) {
        final copy = _Copy(time);
        copy.operation('a', true);
        copy.request('a', 14);
        copy.finish();
        copy.publish('b');
        copy.operation('b', true, epoch: 2);
        copy.request('b', 15);
        copy.finish();
        copy.operation('a', false, failure: 'clipboard_transfer_failed');
        expect(copy.pulls.active, isTrue);
        expect(copy.pulls.userWaiting, isTrue);
        expect(copy.failures, isEmpty);
        copy.operation('b', false, epoch: 2);
        expect(copy.pulls.active, isFalse);
        copy.close();
      });
    },
  );
  test('End failure stops only its ranges and reports a fixed failure', () {
    fakeAsync((time) {
      final copy = _Copy(time);
      copy.operation('a', true);
      copy.request('a', 16);
      copy.operation('a', false, failure: 'clipboard_transfer_failed');
      expect(copy.pulls.active, isFalse);
      expect(copy.pulls.userWaiting, isFalse);
      expect(copy.failures, ['clipboard_transfer_failed']);
      expect(copy.bridge.failedRanges, [(16, 'clipboard_transfer_failed')]);
      expect(copy.sent.last['type'], 'clipboard.cancel');
      copy.close();
    });
  });
  test('cancel retires its epoch; later old starts/ends are harmless', () {
    fakeAsync((time) {
      final copy = _Copy(time);
      copy.operation('a', true);
      copy.request('a', 17);
      copy.finish();
      copy.pulls.cancel();
      time.flushMicrotasks();
      copy.operation('a', true);
      copy.operation('a', false, failure: 'clipboard_cancelled');
      time.elapse(const Duration(seconds: 5));
      expect(copy.pulls.active, isFalse);
      expect(copy.pulls.userWaiting, isFalse);
      expect(copy.failures, isEmpty);
      copy.operation('a', true, epoch: 2);
      copy.request('a', 18, index: 1);
      expect(copy.pulls.active, isTrue);
      copy.close();
    });
  });
  test('withdraw and close discard operation locks and every late epoch', () {
    fakeAsync((time) {
      final copy = _Copy(time);
      copy.operation('a', true);
      copy.request('a', 19);
      copy.finish();
      copy.pulls.offerWithdrawn('a');
      copy.operation('a', true, epoch: 2);
      copy.operation('a', false, failure: 'clipboard_transfer_failed');
      expect(copy.pulls.active, isFalse);
      copy.publish('b');
      copy.operation('b', true, epoch: 3);
      copy.request('b', 20);
      copy.close();
      copy.operation(
        'b',
        false,
        epoch: 3,
        failure: 'clipboard_transfer_failed',
      );
      time.elapse(const Duration(seconds: 5));
      expect(copy.pulls.active, isFalse);
      expect(copy.pulls.userWaiting, isFalse);
      expect(copy.failures, isEmpty);
    });
  });
}
