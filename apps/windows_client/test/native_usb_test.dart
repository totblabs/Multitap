import 'dart:async';

import 'package:flutter/services.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:multitap_client/data/native_usb.dart';
import 'package:multitap_client/data/usb_redirection.dart';

// The Dart side of the runner's USB capture (channel multitap/usb), with the
// platform side replaced by a script. The real UsbDk path needs a test PC.
void main() {
  TestWidgetsFlutterBinding.ensureInitialized();
  const channel = MethodChannel('multitap/usb');
  final calls = <MethodCall>[];
  Object? Function(MethodCall call) script = (_) => null;

  setUp(() {
    calls.clear();
    TestDefaultBinaryMessengerBinding.instance.defaultBinaryMessenger
        .setMockMethodCallHandler(channel, (call) async {
          calls.add(call);
          final value = script(call);
          if (value is PlatformException) throw value;
          return value;
        });
  });
  tearDown(() {
    TestDefaultBinaryMessengerBinding.instance.defaultBinaryMessenger
        .setMockMethodCallHandler(channel, null);
  });

  test('the environment and the device list are read strictly', () async {
    final backend = NativeUsbBackend(channel);
    script = (call) => {'driver': 'ready'};
    expect((await backend.environment()).driver, UsbDriverState.ready);
    script = (call) => {'driver': 'unsupported'};
    expect((await backend.environment()).driver, UsbDriverState.unsupported);
    script = (call) => {'driver': 'anything else'};
    expect((await backend.environment()).driver, UsbDriverState.missing);
    final good = {
      'id': 'a',
      'name': 'Stick',
      'descriptors': Uint8List.fromList([1, 2, 3]),
      'speed': 'high',
      'busy': true,
    };
    script = (call) => [
      good,
      {...good, 'id': 5},
      {...good, 'speed': 'super'},
      {...good, 'descriptors': 'text'},
      'junk',
    ];
    final devices = await backend.list();
    expect(devices.map((d) => d.id), ['a']);
    expect(devices.single.busy, isTrue);
    await backend.dispose();
  });

  test(
    'a capture runs transfers, forwards cancellation and gives the device back',
    () async {
      final backend = NativeUsbBackend(channel);
      script = (call) => switch (call.method) {
        'capture' => 7,
        'transfer' => {
          'status': 0,
          'data': Uint8List.fromList([9, 9]),
        },
        _ => null,
      };
      final capture = await backend.capture(
        UsbLocalDevice(
          id: 'a',
          name: 'n',
          descriptors: Uint8List(0),
          speed: 'high',
        ),
      );
      expect(calls.single.arguments, {'id': 'a'});
      final result = await capture.transfer(
        UsbTransfer(
          token: 4,
          endpoint: 0x81,
          type: 2,
          length: 64,
          data: Uint8List(0),
        ),
      );
      expect(result.status, 0);
      expect(result.data, [9, 9]);
      expect(calls.last.arguments, containsPair('handle', 7));
      expect(calls.last.arguments, containsPair('token', 4));
      capture.cancel(4);
      await Future<void>.delayed(Duration.zero);
      expect(calls.last.method, 'cancel');
      // The runner reports the device left the bus.
      var gone = false;
      unawaited2(capture.gone.then((_) => gone = true));
      await TestDefaultBinaryMessengerBinding.instance.defaultBinaryMessenger
          .handlePlatformMessage(
            'multitap/usb',
            const StandardMethodCodec().encodeMethodCall(
              const MethodCall('gone', {'handle': 7}),
            ),
            (_) {},
          );
      await Future<void>.delayed(Duration.zero);
      expect(gone, isTrue);
      await capture.release();
      await capture.release();
      expect(calls.where((c) => c.method == 'release'), hasLength(1));
      await backend.dispose();
    },
  );

  test('Concurrent release waits for the same native completion and preserves uncertain failure', () async {
    for (final fail in [false, true]) {
      final backend = NativeUsbBackend(channel);
      final gate = Completer<Object?>();
      script = (call) => call.method == 'capture' ? 7 : gate.future;
      final capture = await backend.capture(
        UsbLocalDevice(
          id: 'a',
          name: 'n',
          descriptors: Uint8List(0),
          speed: 'high',
        ),
      );
      final first = capture.release(), second = capture.release();
      expect(identical(first, second), isTrue);
      var completed = false;
      final check = first.then(
        (_) {
          completed = true;
        },
        onError: (Object error) {
          expect(
            error,
            isA<UsbBackendException>().having(
              (e) => e.code,
              'code',
              usbReturnUnknown,
            ),
          );
          completed = true;
        },
      );
      await Future<void>.delayed(Duration.zero);
      expect(
        completed,
        isFalse,
        reason: 'a second release is not a completed acknowledgement',
      );
      if (fail) {
        gate.completeError(PlatformException(code: 'PRIVATE_SENTINEL'));
      } else {
        gate.complete({'urbs': 3});
      }
      await check;
      if (!fail) expect((await second).urbs, 3);
      await backend.dispose();
    }
    expect(calls.where((c) => c.method == 'release'), hasLength(2));
  });

  test('Prepare cancellation carries the exact pending generation without claiming completion', () async {
    final backend = NativeUsbBackend(channel);
    final gate = Completer<Object?>();
    script = (call) => call.method == 'prepare' ? gate.future : null;
    final device = UsbLocalDevice(
      id: 'a',
      name: 'n',
      descriptors: Uint8List(0),
      speed: 'high',
    );
    final pending = backend.prepare(device);
    await Future<void>.delayed(Duration.zero);
    backend.cancelPrepare(device);
    await Future<void>.delayed(Duration.zero);
    final prepare =
        calls.firstWhere((c) => c.method == 'prepare').arguments as Map;
    final cancel =
        calls.firstWhere((c) => c.method == 'cancelPrepare').arguments as Map;
    expect(cancel, {'id': 'a', 'generation': prepare['generation']});
    var completed = false;
    final done = pending.then((_) {
      completed = true;
    });
    await Future<void>.delayed(Duration.zero);
    expect(completed, isFalse);
    gate.complete({'ready': false, 'checked': false});
    await done;
    backend.cancelPrepare(device);
    await Future<void>.delayed(Duration.zero);
    expect(calls.where((c) => c.method == 'cancelPrepare'), hasLength(1));
    await backend.dispose();
  });

  test('native failures become fixed codes; nothing else leaks', () async {
    final backend = NativeUsbBackend(channel);
    script = (call) => PlatformException(
      code: 'usb_device_busy',
      message: 'C:\\private\\path',
    );
    await expectLater(
      backend.capture(
        UsbLocalDevice(
          id: 'a',
          name: 'n',
          descriptors: Uint8List(0),
          speed: 'high',
        ),
      ),
      throwsA(
        isA<UsbBackendException>().having(
          (e) => e.code,
          'code',
          'usb_device_busy',
        ),
      ),
    );
    script = (call) => PlatformException(code: 'Some Windows error text');
    await expectLater(
      backend.capture(
        UsbLocalDevice(
          id: 'a',
          name: 'n',
          descriptors: Uint8List(0),
          speed: 'high',
        ),
      ),
      throwsA(
        isA<UsbBackendException>().having(
          (e) => e.code,
          'code',
          'usb_capture_failed',
        ),
      ),
    );
    script = (call) => PlatformException(code: 'usbdk_signature_invalid');
    await expectLater(
      backend.installDriver(),
      throwsA(
        isA<UsbBackendException>().having(
          (e) => e.code,
          'code',
          'usbdk_signature_invalid',
        ),
      ),
    );
    script = (call) => PlatformException(code: 'usbdk_elevation_cancelled');
    await expectLater(
      backend.installDriver(),
      throwsA(
        isA<UsbBackendException>().having(
          (e) => e.code,
          'code',
          'usbdk_elevation_cancelled',
        ),
      ),
    );
    expect(usbMessage('usbdk_elevation_cancelled'), contains('관리자 승인'));
    script = (call) => PlatformException(
      code: 'usb_surprise_code_that_is_not_fixed_but_looks_ok_x',
    );
    await expectLater(
      backend.installDriver(),
      throwsA(
        isA<UsbBackendException>().having(
          (e) => e.code,
          'code',
          'usbdk_install_failed',
        ),
      ),
    );
    await backend.dispose();
  });

  test('the picker fields of the list are read strictly and bounded', () async {
    final backend = NativeUsbBackend(channel);
    final good = {
      'id': 'a',
      'name': 'Stick (E:)',
      'subtitle': 'SanDisk Cruzer Blade · 32GB',
      'vidPid': '0781:5567',
      'kind': 'storage',
      'neededByPc': false,
      'chips': ['공동인증서', 7, ''],
      'arrivalMs': 1790000000000,
      'memoryKey': '0781:5567:abc',
      'busyElsewhere': true,
      'descriptors': Uint8List.fromList([1, 2, 3]),
      'speed': 'high',
      'busy': false,
    };
    script = (call) => [
      good,
      {
        ...good,
        'id': 'b',
        'kind': 'spaceship',
        'chips': 'junk',
        'arrivalMs': 'x',
        'subtitle': 'x' * 500,
        'neededByPc': true,
      },
      {
        'id': 'c',
        'name': 'Old runner',
        'descriptors': Uint8List(3),
        'speed': 'low',
      },
    ];
    final devices = await backend.list();
    expect(devices, hasLength(3));
    final a = devices[0];
    expect(a.subtitle, 'SanDisk Cruzer Blade · 32GB');
    expect(a.vidPid, '0781:5567');
    expect(a.kind, UsbKind.storage);
    expect(a.chips, ['공동인증서']);
    expect(a.arrivalMs, 1790000000000);
    expect(a.memoryKey, '0781:5567:abc');
    expect(a.busyElsewhere, isTrue);
    expect(a.neededByPc, isFalse);
    final b = devices[1];
    expect(b.kind, isNull, reason: 'an unknown kind is not trusted');
    expect(b.chips, isEmpty);
    expect(b.arrivalMs, 0);
    expect(b.subtitle, isEmpty, reason: 'an oversized text is dropped');
    expect(b.neededByPc, isTrue);
    expect(
      devices[2].subtitle,
      isEmpty,
      reason: 'a runner without the picker fields still lists',
    );
    await backend.dispose();
  });

  // Each remote window has its own engine, so a device one window holds
  // must not just fail in another one as "usb_capture_failed".
  test('another window: the list names it, this window names itself, and a device is asked for and handed over', () async {
    final backend = NativeUsbBackend(channel, 'WorkPC');
    final good = {
      'id': 'a',
      'name': 'Stick',
      'descriptors': Uint8List.fromList([1, 2, 3]),
      'speed': 'high',
    };
    script = (call) => [
      {...good, 'otherWindow': 'Code2'},
      {...good, 'id': 'b'},
      {...good, 'id': 'c', 'otherWindow': 'x' * 500},
    ];
    final devices = await backend.list();
    expect(devices[0].otherWindow, 'Code2');
    expect(devices[1].otherWindow, isNull);
    expect(
      devices[2].otherWindow,
      isEmpty,
      reason: 'an oversized name is dropped, the fact is kept',
    );
    script = (call) => switch (call.method) {
      'handOver' => {'asked': true},
      'holder' => {'held': true},
      'capture' => 7,
      _ => null,
    };
    expect(await backend.handOver(devices[0]), isTrue);
    expect(calls.last.method, 'handOver');
    expect(calls.last.arguments, {'id': 'a', 'window': 'WorkPC'});
    expect(await backend.heldElsewhere(devices[0]), isTrue);
    expect(calls.last.method, 'holder');
    expect(calls.last.arguments, {'id': 'a'});
    await backend.capture(devices[1]);
    expect(calls.last.arguments, {'id': 'b', 'window': 'WorkPC'});
    final asked = <({String id, String window})>[];
    final subscription = backend.handOvers.listen(asked.add);
    await TestDefaultBinaryMessengerBinding.instance.defaultBinaryMessenger
        .handlePlatformMessage(
          channel.name,
          const StandardMethodCodec().encodeMethodCall(
            const MethodCall('hand_over', {'id': 'a', 'window': 'Code2'}),
          ),
          (_) {},
        );
    await Future<void>.delayed(Duration.zero);
    expect(asked, [(id: 'a', window: 'Code2')]);
    await subscription.cancel();
    // UsbDk refused it as already redirected: its own code, the Win32 error
    // only as a number.
    script = (call) => PlatformException(
      code: 'usb_device_redirected',
      message: 'C:\\private\\path',
      details: {'error': 183},
    );
    await expectLater(
      backend.capture(devices[1]),
      throwsA(
        isA<UsbBackendException>()
            .having((e) => e.code, 'code', usbDeviceRedirected)
            .having((e) => e.error, 'error', 183),
      ),
    );
    script = (call) => PlatformException(
      code: 'usb_capture_failed',
      details: {'error': 'text'},
    );
    await expectLater(
      backend.capture(devices[1]),
      throwsA(isA<UsbBackendException>().having((e) => e.error, 'error', 0)),
    );
    script = (call) => PlatformException(code: 'holder failed');
    expect(await backend.heldElsewhere(devices[0]), isFalse);
    expect(await backend.handOver(devices[0]), isFalse);
    script = (call) => null;
    await backend.dispose();
  });

  test('the hand-over check, watching and change notifications', () async {
    final backend = NativeUsbBackend(channel);
    script = (call) => {'ready': false, 'checked': true, 'blocker': 'Excel'};
    final device = UsbLocalDevice(
      id: 'a|b',
      name: 'n',
      descriptors: Uint8List(0),
      speed: 'high',
    );
    var result = await backend.prepare(device);
    expect(calls.last.method, 'prepare');
    expect(calls.last.arguments, {'id': 'a|b', 'generation': 1});
    expect(result.ready, isFalse);
    expect(result.blocker, 'Excel');
    script = (call) => {'ready': true, 'checked': false, 'blocker': 'x' * 500};
    result = await backend.prepare(device);
    expect(result.ready, isTrue);
    expect(result.checked, isFalse);
    expect(result.blocker, isEmpty, reason: 'an oversized name is dropped');
    script = (call) => PlatformException(code: 'usb_capture_failed');
    result = await backend.prepare(device);
    expect(result.ready, isTrue, reason: 'a failed check never blocks');
    expect(result.checked, isFalse);
    script = (call) => null;
    await backend.watch(true);
    await backend.watch(false);
    expect(calls.map((c) => c.method).toList().sublist(calls.length - 2), [
      'watch',
      'unwatch',
    ]);
    var changes = 0;
    final subscription = backend.changes.listen((_) => changes++);
    await TestDefaultBinaryMessengerBinding.instance.defaultBinaryMessenger
        .handlePlatformMessage(
          channel.name,
          const StandardMethodCodec().encodeMethodCall(
            const MethodCall('devices_changed'),
          ),
          (_) {},
        );
    await Future<void>.delayed(Duration.zero);
    expect(changes, 1);
    await subscription.cancel();
    await backend.dispose();
  });
}

void unawaited2(Future<void> future) {}
