import 'dart:async';

import 'package:flutter/services.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:multitap_client/data/native_clipboard.dart';

void main() {
  TestWidgetsFlutterBinding.ensureInitialized();
  const channel = MethodChannel('multitap/clipboard');
  final calls = <MethodCall>[];
  Completer<Map<String, dynamic>>? held;
  setUp(() {
    calls.clear();
    held = null;
    TestDefaultBinaryMessengerBinding.instance.defaultBinaryMessenger
        .setMockMethodCallHandler(channel, (call) async {
          calls.add(call);
          final a = Map<String, dynamic>.from(call.arguments as Map);
          switch (call.method) {
            case 'observePendingSequence':
              return 100;
            case 'beginPending':
              if (held != null) return held!.future;
              return {
                'token': 'lease${a['sourceSequence']}',
                'sequence': 101,
                'sourceSequence': a['sourceSequence'],
                'kind': a['kind'],
                'epoch': a['epoch'],
              };
            case 'bindPending':
              return true;
            case 'commit':
            case 'publishPendingOffer':
            case 'publishOffer':
              return {'sequence': 102};
            default:
              return null;
          }
        });
  });
  tearDown(() {
    TestDefaultBinaryMessengerBinding.instance.defaultBinaryMessenger
        .setMockMethodCallHandler(channel, null);
  });
  Future<ClipboardPendingLease> lease(
    NativeClipboardBridge bridge, {
    int source = 7,
  }) async {
    await bridge.observePendingSequence('owner', epoch: 1);
    return (await bridge.beginPending(
      'owner',
      expectedLocalSequence: 100,
      sourceSequence: source,
      kind: 'text',
      epoch: 1,
    ))!;
  }

  test(
    'bound receiving calls carry one exact native lease and release keeps it',
    () async {
      final bridge = NativeClipboardBridge();
      final p = await lease(bridge);
      expect(await bridge.bindPending('owner', p, 'request'), isTrue);
      await bridge.begin('owner', 'request', 'text', [
        const ClipboardItem(size: 1),
      ]);
      await bridge.append('owner', 'request', 0, 0, Uint8List.fromList([65]));
      await bridge.finish('owner', 'request', 0, List.filled(64, 'a').join());
      await bridge.commit('owner', 'request');
      await bridge.release('owner');
      for (final call in calls.where(
        (c) => {'begin', 'append', 'finish', 'commit'}.contains(c.method),
      )) {
        final a = call.arguments as Map;
        expect(a['pendingToken'], p.token);
        expect(a['pendingEpoch'], 1);
        expect(a['pendingSourceSequence'], 7);
        expect(a['pendingSequence'], 101);
      }
      expect(
        calls.where(
          (c) =>
              c.method == 'cancelPending' || c.method == 'cancelPendingOwner',
        ),
        isEmpty,
      );
    },
  );
  test('late old request never falls back after newer binding', () async {
    final bridge = NativeClipboardBridge();
    final a = await lease(bridge);
    await bridge.bindPending('owner', a, 'old');
    final b = await bridge.beginPending(
      'owner',
      expectedLocalSequence: 101,
      sourceSequence: 8,
      kind: 'text',
      epoch: 1,
    );
    await bridge.bindPending('owner', b!, 'new');
    final before = calls.length;
    expect(
      () =>
          bridge.begin('owner', 'old', 'text', [const ClipboardItem(size: 1)]),
      throwsA(isA<PlatformException>()),
    );
    expect(
      () => bridge.append('owner', 'old', 0, 0, Uint8List.fromList([65])),
      throwsA(isA<PlatformException>()),
    );
    expect(calls, hasLength(before));
    await bridge.release('owner');
    await bridge.begin('owner', 'new', 'text', [const ClipboardItem(size: 1)]);
    expect((calls.last.arguments as Map)['pendingToken'], b.token);
  });
  test(
    'closed epoch cancels its delayed begin response by exact token',
    () async {
      final bridge = NativeClipboardBridge();
      await bridge.observePendingSequence('owner', epoch: 1);
      held = Completer<Map<String, dynamic>>();
      final pending = bridge.beginPending(
        'owner',
        expectedLocalSequence: 100,
        sourceSequence: 7,
        kind: 'text',
        epoch: 1,
      );
      await Future<void>.value();
      await bridge.cancelPendingOwner('owner', 1);
      held!.complete({
        'token': 'late',
        'sequence': 101,
        'sourceSequence': 7,
        'kind': 'text',
        'epoch': 1,
      });
      expect(await pending, isNull);
      expect(
        calls.where((c) => c.method == 'cancelPending').single.arguments,
        containsPair('pendingToken', 'late'),
      );
      expect(
        () => bridge.observePendingSequence('owner', epoch: 1),
        throwsA(isA<PlatformException>()),
      );
    },
  );
  test('bound large content uses delegate RPC while files retain normal on-paste path', () async {
    final bridge = NativeClipboardBridge();
    final p = await lease(bridge);
    await bridge.bindPending('owner', p, 'offer');
    await bridge.publishOfferOnPaste('owner', 'offer', 'text', [
      const ClipboardItem(size: 100000, kind: 'text'),
    ]);
    expect(calls.last.method, 'publishPendingOffer');
    expect((calls.last.arguments as Map)['pendingToken'], p.token);
    await bridge.publishOfferOnPaste('owner', 'files', 'tree', [
      const ClipboardItem(size: 1, kind: 'file', name: 'file.txt'),
    ]);
    expect(calls.last.method, 'publishOffer');
    expect((calls.last.arguments as Map).containsKey('pendingToken'), isFalse);
  });
  test('only exact explicit manual receive may use unbound text after watch closed', () async {
    final bridge = NativeClipboardBridge();
    final p = await lease(bridge);
    await bridge.bindPending('owner', p, 'old');
    await bridge.cancelPendingOwner('owner', 1);
    final before = calls.length;
    expect(
      () => bridge.begin('owner', 'unbound-auto', 'text', [
        const ClipboardItem(size: 1),
      ]),
      throwsA(isA<PlatformException>()),
    );
    expect(calls, hasLength(before));
    await bridge.allowManualRequest('owner', 'manual');
    await bridge.begin('owner', 'manual', 'text', [
      const ClipboardItem(size: 1),
    ]);
    expect((calls.last.arguments as Map).containsKey('pendingToken'), isFalse);
    expect(
      () => bridge.begin('owner', 'manual', 'text', [
        const ClipboardItem(size: 1),
      ]),
      throwsA(isA<PlatformException>()),
    );
    final afterManual = calls.length;
    expect(
      () =>
          bridge.begin('owner', 'old', 'text', [const ClipboardItem(size: 1)]),
      throwsA(isA<PlatformException>()),
    );
    expect(calls, hasLength(afterManual));
  });
}
