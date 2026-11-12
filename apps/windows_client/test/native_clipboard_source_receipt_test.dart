import 'package:flutter/services.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:multitap_client/data/native_clipboard.dart';

const owner = 'clipboard-fixture-owner';
const serial = 9007199254740993; // Above 2^53: use the real MethodCodec int64.
const channel = MethodChannel('multitap/clipboard');

Map<String, Object?> capture({Object? receipt, bool withReceipt = true}) => {
  'format': 'text',
  'items': [
    {'size': 1, 'units': 1, 'kind': 'text'},
  ],
  'sequence': 10,
  if (withReceipt) 'sourceReceipt': receipt,
};

void main() {
  TestWidgetsFlutterBinding.ensureInitialized();
  final messenger =
      TestDefaultBinaryMessengerBinding.instance.defaultBinaryMessenger;
  final calls = <MethodCall>[];
  late Future<Object?> Function(MethodCall) respond;
  late NativeClipboardBridge bridge;
  final failure = isA<PlatformException>().having(
    (value) => value.code,
    'code',
    'clipboard_transfer_failed',
  );

  setUp(() {
    calls.clear();
    respond = (call) async => throw StateError('Unexpected ${call.method}');
    messenger.setMockMethodCallHandler(channel, (call) {
      calls.add(
        call,
      ); // Route and record only; assertions stay outside callbacks.
      return respond(call);
    });
    bridge = NativeClipboardBridge(allowedText: true);
  });
  tearDown(() => messenger.setMockMethodCallHandler(channel, null));

  test('legacy native capture stays usable without a source receipt', () async {
    respond = (_) async => capture(withReceipt: false);
    final result = await bridge.capture(owner);
    expect(result.format, 'text');
    expect(result.items.single.size, 1);
    expect(result.sourceReceipt, isNull);
    expect(calls.map((call) => call.method), ['capture']);
  });

  test(
    'normal, own and offer captures keep the exact native int64 receipt',
    () async {
      final identity = {'owner': owner, 'captureSerial': serial};
      for (final kind in ['normal', 'own', 'offer']) {
        respond = (_) async => {
          ...capture(receipt: identity),
          if (kind == 'own') 'own': true,
          if (kind == 'offer') 'offer': true,
        };
        final result = kind == 'offer'
            ? await bridge.captureWithOffer(owner, 'offer-fixture')
            : await bridge.capture(owner, skipOwn: kind == 'own');
        expect(result.sourceReceipt!.owner, owner);
        expect(result.sourceReceipt!.captureSerial, serial);
        expect(result.own, kind == 'own');
        expect(result.offer, kind == 'offer');
        if (kind == 'own') expect(result.items, isEmpty);
      }
      expect(calls.map((call) => call.method), [
        'capture',
        'capture',
        'capture',
      ]);
    },
  );

  test(
    'present malformed or wrong-owner receipts fail without legacy cleanup',
    () async {
      final bad = <Object?>[
        null,
        {'owner': 'other-owner', 'captureSerial': serial},
        {'owner': owner, 'captureSerial': 0},
        {'owner': owner, 'captureSerial': -1},
        {'owner': owner, 'captureSerial': 1.0},
        {'owner': owner, 'captureSerial': '1'},
        {'owner': owner},
        {'owner': owner, 'captureSerial': serial, 'extra': true},
        {'owner': '$owner\n', 'captureSerial': serial},
      ];
      for (final value in bad) {
        respond = (_) async => capture(receipt: value);
        await expectLater(bridge.capture(owner), throwsA(failure));
      }
      expect(calls.length, bad.length);
      expect(calls.every((call) => call.method == 'capture'), isTrue);
    },
  );

  test(
    'exact retirement has only owner and serial and preserves native result',
    () async {
      var retired = true;
      respond = (_) async => {'retired': retired};
      const source = ClipboardSourceReceipt(owner, serial);
      expect(await bridge.retireSource(source), isTrue);
      retired = false;
      expect(await bridge.retireSource(source), isFalse);
      expect(calls.map((call) => call.method), [
        'retireSource',
        'retireSource',
      ]);
      for (final call in calls) {
        expect(call.arguments, {'owner': owner, 'captureSerial': serial});
      }
    },
  );

  test('invalid local receipts are rejected before any native call', () async {
    for (final value in [
      const ClipboardSourceReceipt(owner, 0),
      const ClipboardSourceReceipt(owner, -1),
      const ClipboardSourceReceipt('bad/owner', serial),
      const ClipboardSourceReceipt('bad\n', serial),
      ClipboardSourceReceipt('a' * 81, serial),
    ]) {
      await expectLater(bridge.retireSource(value), throwsA(failure));
      await expectLater(
        bridge.sourceChunkScoped(value, 0, 0, 1),
        throwsA(failure),
      );
      await expectLater(bridge.sourceDigestScoped(value, 0), throwsA(failure));
    }
    expect(calls, isEmpty);
  });

  test(
    'scoped reads send exact identity and do not retry legacy reads',
    () async {
      const source = ClipboardSourceReceipt(owner, serial);
      final bytes = Uint8List.fromList([97]);
      final digest = 'a' * 64;
      respond = (call) async =>
          call.method == 'sourceChunkScoped' ? bytes : digest;
      expect(await bridge.sourceChunkScoped(source, 2, 3, 1), bytes);
      expect(await bridge.sourceDigestScoped(source, 2), digest);
      expect(calls[0].arguments, {
        'owner': owner,
        'captureSerial': serial,
        'index': 2,
        'offset': 3,
        'length': 1,
      });
      expect(calls[1].arguments, {
        'owner': owner,
        'captureSerial': serial,
        'index': 2,
      });
      respond = (_) async => throw PlatformException(code: 'clipboard_changed');
      await expectLater(
        bridge.sourceChunkScoped(source, 0, 0, 1),
        throwsA(
          isA<PlatformException>().having(
            (value) => value.code,
            'code',
            'clipboard_changed',
          ),
        ),
      );
      expect(calls.map((call) => call.method), [
        'sourceChunkScoped',
        'sourceDigestScoped',
        'sourceChunkScoped',
      ]);
    },
  );

  test(
    'malformed native scoped results fail without an owner-only release',
    () async {
      const source = ClipboardSourceReceipt(owner, serial);
      for (final value in <Object?>[
        null,
        false,
        {'retired': 'yes'},
        {'retired': true, 'extra': 1},
      ]) {
        respond = (_) async => value;
        await expectLater(bridge.retireSource(source), throwsA(failure));
      }
      respond = (_) async => [97];
      await expectLater(
        bridge.sourceChunkScoped(source, 0, 0, 1),
        throwsA(failure),
      );
      respond = (_) async => 'A' * 64;
      await expectLater(bridge.sourceDigestScoped(source, 0), throwsA(failure));
      expect(calls.any((call) => call.method == 'release'), isFalse);
    },
  );

  test(
    'source retirement leaves an existing exact pending binding intact',
    () async {
      respond = (call) async => switch (call.method) {
        'observePendingSequence' => 10,
        'beginPending' => {
          'token': 'pending-fixture',
          'sequence': 11,
          'sourceSequence': 41,
          'kind': 'text',
          'epoch': 1,
        },
        'bindPending' => true,
        'retireSource' => {'retired': true},
        'begin' => null,
        _ => throw StateError('Unexpected ${call.method}'),
      };
      await bridge.observePendingSequence(owner, epoch: 1);
      final lease = (await bridge.beginPending(
        owner,
        expectedLocalSequence: 10,
        sourceSequence: 41,
        kind: 'text',
        epoch: 1,
      ))!;
      expect(
        await bridge.bindPending(owner, lease, 'incoming-fixture'),
        isTrue,
      );
      expect(
        await bridge.retireSource(const ClipboardSourceReceipt(owner, serial)),
        isTrue,
      );
      await bridge.begin(owner, 'incoming-fixture', 'text', const [
        ClipboardItem(size: 1, kind: 'text'),
      ]);
      final cleanup = calls.singleWhere(
        (call) => call.method == 'retireSource',
      );
      expect(cleanup.arguments, {'owner': owner, 'captureSerial': serial});
      final incoming = calls.last.arguments as Map;
      expect(incoming['requestId'], 'incoming-fixture');
      expect(incoming['pendingToken'], 'pending-fixture');
      expect(incoming['pendingEpoch'], 1);
      expect(incoming['pendingSourceSequence'], 41);
      expect(
        calls.any(
          (call) =>
              call.method == 'release' ||
              call.method.startsWith('cancelPending'),
        ),
        isFalse,
      );
    },
  );
}
