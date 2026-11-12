import 'dart:async';

import 'package:flutter_test/flutter_test.dart';
import 'package:multitap_client/data/clipboard_policy.dart';
import 'package:multitap_client/data/clipboard_sync.dart';
import 'package:multitap_client/data/native_clipboard.dart';

// Controlled subscription completion only; no MethodChannel, OS or network.
class _Subscription extends Fake
    implements StreamSubscription<ClipboardChange> {
  _Subscription(this.callback, this.done);
  final void Function(ClipboardChange)? callback;
  final Completer<void> done;
  int cancels = 0;
  @override
  Future<void> cancel() {
    cancels++;
    return done.future;
  }
}

class _Changes extends Stream<ClipboardChange> {
  final cancellation = Completer<void>();
  final subscriptions = <_Subscription>[];
  @override
  StreamSubscription<ClipboardChange> listen(
    void Function(ClipboardChange)? onData, {
    Function? onError,
    void Function()? onDone,
    bool? cancelOnError,
  }) {
    final subscription = _Subscription(onData, cancellation);
    subscriptions.add(subscription);
    return subscription;
  }
}

class _Observation implements ClipboardObservationBridge {
  final stream = _Changes();
  final watches = <bool>[];
  @override
  Stream<ClipboardChange> get changes => stream;
  @override
  int? get lastCommittedSequence => null;
  @override
  Future<int> watch(String owner, bool enabled) async {
    watches.add(enabled);
    return 10;
  }
}

ClipboardSync _syncFor(_Observation bridge, {void Function()? changed}) =>
    ClipboardSync(
      owner: 'fixture-owner',
      bridge: bridge,
      policy: const ClipboardPolicy(textToRemote: true),
      observeInitialLocalOnly: true,
      sendLocal: () async {},
      receiveRemote: () async {},
      waitIdle: () async {},
      watchRemote: (_) {},
      onFailure: () {},
      onLocalCopy: changed,
    );

void main() {
  test('close joins a cancellation already started by focus loss and is idempotent', () async {
    final bridge = _Observation();
    final current = _syncFor(bridge);
    await current.setActive(true);
    await current.setActive(false);
    var closed = false;
    final closing = current.close();
    closing.then((_) {
      closed = true;
    });
    expect(identical(closing, current.close()), true);
    await Future<void>.delayed(Duration.zero);
    expect(closed, false);
    expect(bridge.stream.subscriptions.single.cancels, 1);
    bridge.stream.cancellation.complete();
    await closing;
    expect(closed, true);
    expect(bridge.watches, [true, false]);
  });

  test('a queued old watch callback cannot create a copy after a new watch generation', () async {
    final bridge = _Observation();
    var copies = 0;
    final sync = _syncFor(
      bridge,
      changed: () {
        copies++;
      },
    );
    await sync.setActive(true);
    final old = bridge.stream.subscriptions.single;
    await sync.setActive(false);
    await sync.setActive(true);
    old.callback!(const ClipboardChange('fixture-owner', 41));
    expect(copies, 0);
    bridge.stream.subscriptions.last.callback!(
      const ClipboardChange('fixture-owner', 42),
    );
    expect(copies, 1);
    final closing = sync.close();
    bridge.stream.cancellation.complete();
    await closing;
    old.callback!(const ClipboardChange('fixture-owner', 43));
    expect(copies, 1);
  });
}
