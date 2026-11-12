import 'dart:async';

import 'package:flutter/services.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:multitap_client/data/native_clipboard.dart';

const owner = 'publication-fixture';
const channel = MethodChannel('multitap/clipboard');
const items = [ClipboardItem(size: 1, kind: 'text')];

void main() {
  TestWidgetsFlutterBinding.ensureInitialized();
  final messenger =
      TestDefaultBinaryMessengerBinding.instance.defaultBinaryMessenger;
  final calls = <MethodCall>[];
  final held = <String, Completer<Object?>>{};
  final nativeBoundTokens = <String>{};
  late NativeClipboardBridge bridge;

  String key(MethodCall call) {
    final args = call.arguments as Map;
    return '${call.method}/${args['requestId'] ?? args['offerId'] ?? args['sourceSequence'] ?? args['epoch'] ?? ''}';
  }

  Completer<Object?> hold(String operation) =>
      held[operation] = Completer<Object?>();

  Future<ClipboardPendingLease> pending(int source, {int epoch = 1}) async {
    return (await bridge.beginPending(
      owner,
      expectedLocalSequence: 10 + epoch,
      sourceSequence: source,
      kind: 'text',
      epoch: epoch,
    ))!;
  }

  Future<void> settle() => Future<void>.delayed(Duration.zero);

  setUp(() {
    calls.clear();
    held.clear();
    nativeBoundTokens.clear();
    bridge = NativeClipboardBridge(allowedText: true);
    messenger.setMockMethodCallHandler(channel, (call) async {
      calls.add(
        call,
      ); // Dispatch/record only; assertions run outside callbacks.
      final args = call.arguments as Map;
      // Match native's awaiting -> bound CAS, including a held first reply.
      if (call.method == 'bindPending' &&
          !nativeBoundTokens.add(args['pendingToken'] as String)) {
        return false;
      }
      final waiting = held[key(call)];
      if (waiting != null) return waiting.future;
      return switch (call.method) {
        'observePendingSequence' => 10 + (args['epoch'] as int),
        'beginPending' => {
          'token': 'lease-${args['epoch']}-${args['sourceSequence']}',
          'sequence': 100 + (args['sourceSequence'] as int),
          'sourceSequence': args['sourceSequence'],
          'kind': args['kind'],
          'epoch': args['epoch'],
        },
        'bindPending' => true,
        'commit' => {
          'sequence': 281,
          'readable': true,
          'readError': 0,
          'readFlags': 0,
        },
        'publishOffer' || 'publishPendingOffer' => {'sequence': 282},
        'claim' => {'claimed': true, 'sequence': 283},
        'begin' || 'cancelPending' || 'cancelPendingOwner' => null,
        'retireSource' => {'retired': true},
        'published' => {'owned': true, 'sequence': 999},
        _ => throw StateError('Unexpected ${call.method}'),
      };
    });
  });

  tearDown(() => messenger.setMockMethodCallHandler(channel, null));

  test(
    'late pending commit cannot overwrite a newer pending announcement',
    () async {
      await bridge.observePendingSequence(owner, epoch: 1);
      final first = await pending(41);
      expect(await bridge.bindPending(owner, first, 'first'), isTrue);
      final reply = hold('commit/first');
      final old = bridge.commit(owner, 'first');
      await settle();
      final second = await pending(42);
      expect(bridge.lastCommittedSequence, second.sequence);
      reply.complete({
        'sequence': 711,
        'readable': false,
        'readError': 5,
        'readFlags': 2,
      });
      await old;
      expect(bridge.lastCommittedSequence, second.sequence);
      expect(bridge.lastPublishCheck, isNull);
      expect(bridge.publicationMatches(owner, 'first', lease: first), isFalse);
    },
  );

  test('late pending offer cannot replace the next publication tag', () async {
    await bridge.observePendingSequence(owner, epoch: 1);
    final first = await pending(41);
    expect(await bridge.bindPending(owner, first, 'first'), isTrue);
    final reply = hold('publishPendingOffer/first');
    final old = bridge.publishOfferOnPaste(owner, 'first', 'text', items);
    await settle();
    final second = await pending(42);
    expect(await bridge.bindPending(owner, second, 'second'), isTrue);
    await bridge.commit(owner, 'second');
    reply.complete({'sequence': 712});
    await old;
    expect(bridge.lastCommittedSequence, 281);
    expect(bridge.lastPublishCheck, (readable: true, error: 0, flags: 0));
    expect(bridge.publicationMatches(owner, 'second', lease: second), isTrue);
    expect(bridge.publicationMatches(owner, 'first', lease: first), isFalse);
  });

  test(
    'late old bind cancels only its lease and preserves the newer binding',
    () async {
      await bridge.observePendingSequence(owner, epoch: 1);
      final first = await pending(41);
      final reply = hold('bindPending/first');
      final old = bridge.bindPending(owner, first, 'first');
      await settle();
      final second = await pending(42);
      expect(await bridge.bindPending(owner, second, 'second'), isTrue);
      reply.complete(true);
      expect(await old, isFalse);
      await bridge.begin(owner, 'second', 'text', items);
      final data = calls.last.arguments as Map;
      expect(data['pendingToken'], second.token);
      expect(data['requestId'], 'second');
      final cancelled = calls.where((call) => call.method == 'cancelPending');
      expect(cancelled.length, 1);
      expect((cancelled.single.arguments as Map)['pendingToken'], first.token);
      await bridge.commit(owner, 'second');
      expect(bridge.publicationMatches(owner, 'second', lease: second), isTrue);
    },
  );

  test('late observation cannot move a confirmed epoch backwards', () async {
    final reply = hold('observePendingSequence/1');
    final old = bridge.observePendingSequence(owner, epoch: 1);
    final rejected = expectLater(
      old,
      throwsA(
        isA<PlatformException>().having(
          (e) => e.code,
          'code',
          'clipboard_changed',
        ),
      ),
    );
    await settle();
    expect(await bridge.observePendingSequence(owner, epoch: 2), 12);
    reply.complete(11);
    await rejected;
    final second = await pending(42, epoch: 2);
    expect(second.epoch, 2);
    expect(bridge.lastCommittedSequence, second.sequence);
  });

  test(
    'cancelling an old lease or epoch leaves the newer publication usable',
    () async {
      await bridge.observePendingSequence(owner, epoch: 1);
      final first = await pending(41);
      expect(await bridge.bindPending(owner, first, 'first'), isTrue);
      await bridge.observePendingSequence(owner, epoch: 2);
      final second = await pending(42, epoch: 2);
      expect(await bridge.bindPending(owner, second, 'second'), isTrue);
      await bridge.commit(owner, 'second');
      await bridge.cancelPending(owner, first);
      await bridge.cancelPendingOwner(owner, 1);
      expect(bridge.publicationMatches(owner, 'second', lease: second), isTrue);
      await bridge.begin(owner, 'second', 'text', items);
      expect((calls.last.arguments as Map)['pendingToken'], second.token);
    },
  );

  test(
    'publication tag requires exact owner request and all lease fields',
    () async {
      await bridge.observePendingSequence(owner, epoch: 1);
      final lease = await pending(41);
      expect(await bridge.bindPending(owner, lease, 'first'), isTrue);
      await bridge.commit(owner, 'first');
      expect(bridge.publicationMatches(owner, 'first', lease: lease), isTrue);
      expect(
        bridge.publicationMatches('other-owner', 'first', lease: lease),
        isFalse,
      );
      expect(
        bridge.publicationMatches(owner, 'other-request', lease: lease),
        isFalse,
      );
      expect(bridge.publicationMatches(owner, 'first'), isFalse);
      for (var field = 0; field < 5; field++) {
        final wrong = ClipboardPendingLease(
          token: field == 0 ? 'other-token' : lease.token,
          sequence: lease.sequence + (field == 1 ? 1 : 0),
          sourceSequence: lease.sourceSequence + (field == 2 ? 1 : 0),
          kind: field == 3 ? 'image' : lease.kind,
          epoch: lease.epoch + (field == 4 ? 1 : 0),
        );
        expect(
          bridge.publicationMatches(owner, 'first', lease: wrong),
          isFalse,
        );
      }
    },
  );

  test(
    'tombstoned bound request is refused before ordinary native fallback',
    () async {
      await bridge.observePendingSequence(owner, epoch: 1);
      final lease = await pending(41);
      expect(await bridge.bindPending(owner, lease, 'first'), isTrue);
      await bridge.cancelPending(owner, lease);
      final before = calls.length;
      await expectLater(
        () => bridge.begin(owner, 'first', 'text', items),
        throwsA(
          isA<PlatformException>().having(
            (e) => e.code,
            'code',
            'clipboard_changed',
          ),
        ),
      );
      expect(calls.length, before);
    },
  );

  test(
    'late claim is not accepted after a newer clipboard publication',
    () async {
      final reply = hold('claim/first');
      final old = bridge.claim(owner, 'first');
      await settle();
      await bridge.publishOffer(owner, 'second', const []);
      reply.complete({'claimed': true, 'sequence': 713});
      expect(await old, isNull);
      expect(bridge.lastCommittedSequence, 282);
      expect(bridge.publicationMatches(owner, 'second'), isTrue);
    },
  );

  test(
    'ordinary cross-owner result cannot replace newer commit diagnostics',
    () async {
      final reply = hold('publishOffer/first');
      final old = bridge.publishOffer(owner, 'first', const []);
      await settle();
      await bridge.commit('other-owner', 'second');
      reply.complete({'sequence': 714});
      await old;
      expect(bridge.lastCommittedSequence, 281);
      expect(bridge.lastPublishCheck, (readable: true, error: 0, flags: 0));
      expect(bridge.publicationMatches('other-owner', 'second'), isTrue);
    },
  );

  test(
    'source retirement and read-only ownership probe preserve publication tag',
    () async {
      await bridge.commit(owner, 'first');
      expect(
        await bridge.retireSource(const ClipboardSourceReceipt(owner, 1)),
        isTrue,
      );
      expect(await bridge.published(owner), (owned: true, sequence: 999));
      expect(bridge.lastCommittedSequence, 281);
      expect(bridge.publicationMatches(owner, 'first'), isTrue);
    },
  );

  test(
    'rejected rebinding preserves the accepted lease and publication',
    () async {
      await bridge.observePendingSequence(owner, epoch: 1);
      final lease = await pending(41);
      expect(await bridge.bindPending(owner, lease, 'first'), isTrue);
      await bridge.commit(owner, 'first');
      expect(await bridge.bindPending(owner, lease, 'second'), isFalse);
      expect(bridge.publicationMatches(owner, 'first', lease: lease), isTrue);
      await bridge.begin(owner, 'first', 'text', items);
      expect((calls.last.arguments as Map)['pendingToken'], lease.token);
      expect(calls.where((call) => call.method == 'bindPending').length, 1);
    },
  );

  test(
    'repeated binding cannot supersede an in-flight first lease bind',
    () async {
      await bridge.observePendingSequence(owner, epoch: 1);
      final lease = await pending(41);
      final reply = hold('bindPending/first');
      final first = bridge.bindPending(owner, lease, 'first');
      await settle();
      expect(await bridge.bindPending(owner, lease, 'second'), isFalse);
      reply.complete(true);
      expect(await first, isTrue);
      await bridge.commit(owner, 'first');
      expect(bridge.publicationMatches(owner, 'first', lease: lease), isTrue);
      expect(calls.any((call) => call.method == 'cancelPending'), isFalse);
      expect(calls.where((call) => call.method == 'bindPending').length, 1);
    },
  );
}
