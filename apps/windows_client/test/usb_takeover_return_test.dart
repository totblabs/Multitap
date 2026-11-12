import 'dart:async';

import 'package:flutter/services.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:multitap_client/data/native_usb.dart';
import 'package:multitap_client/data/product_log.dart';
import 'package:multitap_client/data/usb_redirection.dart';

import 'usb_redirection_test.dart' as fixture;

// Substitute every platform call on dedicated UNIT-TEST method channels. The
// two window actors have independent native query/device lanes, but one shared
// fake physical device. No real multitap/usb handler, driver, device, VM, socket,
// process or user file is used. ProductLog is an uninitialized memory-only log.
const _deviceId = 'UNIT-TEST-DEVICE';

Future<void> _until(bool Function() condition) async {
  final end = DateTime.now().add(const Duration(seconds: 2));
  while (!condition()) {
    if (DateTime.now().isAfter(end)) fail('UNIT-TEST gate was not reached');
    await Future<void>.delayed(const Duration(milliseconds: 1));
  }
}

class _Lane {
  Future<void> _tail = Future.value();
  Future<Object?> run(Future<Object?> Function() work) {
    final result = Completer<Object?>();
    _tail = _tail.then((_) async {
      try {
        result.complete(await work());
      } catch (error, stack) {
        result.completeError(error, stack);
      }
    });
    return result.future;
  }
}

class _Physical {
  String? owner;
  int? handle;
  bool windowsStarted = true;
  final history = <String>[];

  /// The process-wide view of the windows (the runner's channel registry).
  final windows = <String, _Window>{};
}

class _ObservedBackend extends NativeUsbBackend {
  _ObservedBackend(super.channel, super.window);
  final observedCaptures = <UsbCapture>[];
  @override
  Future<UsbCapture> capture(UsbLocalDevice device) async {
    final capture = await super.capture(device);
    observedCaptures.add(capture);
    return capture;
  }
}

class _Window {
  _Window(this.name, this.physical)
    : channel = MethodChannel('UNIT-TEST/usb-return/$name') {
    TestDefaultBinaryMessengerBinding.instance.defaultBinaryMessenger
        .setMockMethodCallHandler(channel, (call) {
          calls.add(call);
          final lane = call.method == 'capture' || call.method == 'release'
              ? deviceLane
              : queryLane;
          // Answered at once by the runner, outside both lanes.
          if (call.method == 'holder' || call.method == 'handOver') {
            return _run(call);
          }
          return lane.run(() => _run(call));
        });
    backend = _ObservedBackend(channel, name);
    physical.windows[name] = this;
  }
  final String name;
  final _Physical physical;
  final MethodChannel channel;
  late final _ObservedBackend backend;
  final queryLane = _Lane(), deviceLane = _Lane();
  final calls = <MethodCall>[];
  int _nextHandle = 0;
  Completer<void>? releaseGate, returnedGate;
  final returnedEntered = Completer<void>();

  Future<Object?> _run(MethodCall call) async {
    final args = call.arguments as Map? ?? const {};
    switch (call.method) {
      case 'environment':
        return {'driver': 'ready'};
      case 'list':
        return [
          {
            'id': _deviceId,
            'name': 'UNIT-TEST storage',
            'kind': 'storage',
            'descriptors': fixture.vector('mass storage stick'),
            'speed': 'high',
            'busy': physical.owner == name,
            'busyElsewhere': physical.owner != null && physical.owner != name,
            'arrivalMs': 100,
            if (physical.owner != null && physical.owner != name)
              'otherWindow': physical.owner,
          },
        ];
      case 'holder':
        return {'held': physical.owner != null && physical.owner != name};
      case 'handOver':
        final holder = physical.windows[physical.owner];
        if (holder == null || holder == this) return {'asked': false};
        physical.history.add('$name:ask:${holder.name}');
        unawaited(
          TestDefaultBinaryMessengerBinding.instance.defaultBinaryMessenger
              .handlePlatformMessage(
                holder.channel.name,
                const StandardMethodCodec().encodeMethodCall(
                  MethodCall('hand_over', {
                    'id': args['id'],
                    'window': args['window'],
                  }),
                ),
                (_) {},
              ),
        );
        return {'asked': true};
      case 'watch' || 'unwatch':
        return null;
      case 'prepare':
        return {'ready': true, 'checked': true, 'blocker': ''};
      case 'capture':
        if (physical.owner != null || !physical.windowsStarted) {
          throw PlatformException(code: 'usb_device_busy');
        }
        final handle = ++_nextHandle;
        physical
          ..owner = name
          ..handle = handle
          ..windowsStarted = false;
        physical.history.add('$name:capture:$handle');
        return handle;
      case 'release':
        physical.history.add('$name:release-enter:${args['handle']}');
        await releaseGate?.future;
        if (physical.owner == name && physical.handle == args['handle']) {
          physical
            ..owner = null
            ..handle = null
            ..windowsStarted = true;
        }
        physical.history.add('$name:release-done:${args['handle']}');
        return {'urbs': 0};
      case 'returned':
        physical.history.add('$name:probe-enter');
        if (!returnedEntered.isCompleted) returnedEntered.complete();
        await returnedGate?.future;
        final started = physical.owner == null && physical.windowsStarted;
        physical.history.add('$name:probe:${started ? 1 : 2}');
        return {'state': started ? 1 : 2};
      case 'cancel':
        physical.history.add('$name:cancel:${args['handle']}');
        return null;
      default:
        throw StateError('Unexpected UNIT-TEST method ${call.method}');
    }
  }

  Future<void> dispose() async {
    await backend.dispose();
    TestDefaultBinaryMessengerBinding.instance.defaultBinaryMessenger
        .setMockMethodCallHandler(channel, null);
  }
}

Future<(UsbSession, fixture.FakeSocket)> _start(
  String id,
  UsbBackend backend,
) async {
  final socket = fixture.FakeSocket();
  final session = UsbSession(
    url: Uri.parse('wss://UNIT-TEST.invalid/sessions/$id/usb'),
    token: 'UNIT-TEST token never transmitted',
    sessionId: id,
    backend: backend,
    cleanupWait: const Duration(milliseconds: 1),
    dialer: (_, _) async => socket,
  );
  await session.start();
  socket
    ..emit({...fixture.ready, 'sessionId': id})
    ..emit({'type': 'usb.status', 'state': 'ready'});
  await _until(() => session.ready && session.entries.isNotEmpty);
  return (session, socket);
}

Future<UsbEntry> _share(UsbSession session, fixture.FakeSocket socket) async {
  final entry = session.entries.single;
  expect(session.share(entry), isNull);
  final attach = await socket.text('usb.attach');
  socket.emit({'type': 'usb.attached', 'device': attach['device']});
  await _until(() => entry.state == UsbEntryState.shared);
  return entry;
}

void main() {
  TestWidgetsFlutterBinding.ensureInitialized();

  test('independent window recapture can coexist with an old not-started observation, without damaging the new owner', () async {
    final physical = _Physical();
    final oldWindow = _Window('old-window', physical)
      ..returnedGate = Completer<void>();
    final newWindow = _Window('new-window', physical);
    UsbSession? oldToClose, newToClose;
    addTearDown(() async {
      if (!oldWindow.returnedGate!.isCompleted) {
        oldWindow.returnedGate!.complete();
      }
      await oldToClose?.close();
      await newToClose?.close();
      await oldWindow.dispose();
      await newWindow.dispose();
    });
    final (oldSession, oldSocket) = await _start(
      'old-session',
      oldWindow.backend,
    );
    oldToClose = oldSession;
    final oldEntry = await _share(oldSession, oldSocket);
    final oldCapture = oldWindow.backend.observedCaptures.single;
    final closing = oldSession.close();
    await oldWindow.returnedEntered.future;
    await closing; // bounded UI close; the old probe still owns its continuation
    expect(physical.owner, isNull);

    final (newSession, newSocket) = await _start(
      'new-session',
      newWindow.backend,
    );
    newToClose = newSession;
    final newEntry = await _share(newSession, newSocket);
    expect(physical.owner, 'new-window');
    // Both native window engines may use handle1. Method-channel/API-instance
    // scope, rather than globally unique handle integers, protects ownership.
    expect(
      oldWindow.calls.singleWhere((c) => c.method == 'release').arguments,
      {'handle': 1},
    );
    expect(newWindow.backend.observedCaptures, hasLength(1));
    oldWindow.returnedGate!.complete();
    await _until(() => oldEntry.state != UsbEntryState.returning);

    // Characterization, not a device-failure claim: Windows is intentionally
    // not started while the second VM owns this healthy fake device.
    expect(oldEntry.code, usbNotReturned);
    expect(oldEntry.notReturned, isTrue);
    expect(newEntry.state, UsbEntryState.shared);
    expect(physical.owner, 'new-window');
    final beforeLateCleanup = physical.history.length;
    oldCapture.cancel(7);
    await oldCapture.release();
    expect(physical.history.length, beforeLateCleanup);
    expect(oldWindow.calls.where((c) => c.method == 'release'), hasLength(1));
    expect(newEntry.state, UsbEntryState.shared);
    expect(
      physical.history.indexOf('old-window:release-done:1'),
      lessThan(physical.history.indexOf('old-window:probe-enter')),
    );
    expect(
      physical.history.indexOf('old-window:probe-enter'),
      lessThan(physical.history.indexOf('new-window:capture:1')),
    );
    expect(
      physical.history.indexOf('new-window:capture:1'),
      lessThan(physical.history.indexOf('old-window:probe:2')),
    );
  });

  test('late release retains same-entry ownership and a second release cannot bypass its gate', () async {
    final physical = _Physical();
    final window = _Window('late-release', physical)
      ..releaseGate = Completer<void>();
    final (session, socket) = await _start(
      'late-release-session',
      window.backend,
    );
    addTearDown(() async {
      if (!window.releaseGate!.isCompleted) window.releaseGate!.complete();
      await session.close();
      await window.dispose();
    });
    final entry = await _share(session, socket);
    final capture = window.backend.observedCaptures.single;
    await session.stop(
      entry,
    ); // cleanupWait expires; native release is still gated
    expect(entry.state, UsbEntryState.returning);
    expect(physical.owner, 'late-release');
    expect(session.share(entry), isNull); // no second attach is sent
    expect(socket.texts.where((m) => m['type'] == 'usb.attach'), isEmpty);
    var secondReleaseCompleted = false;
    final secondRelease = capture.release().then(
      (_) => secondReleaseCompleted = true,
    );
    capture.cancel(7);
    await Future<void>.delayed(Duration.zero);
    expect(secondReleaseCompleted, isFalse);
    expect(window.calls.where((c) => c.method == 'release'), hasLength(1));
    expect(window.calls.where((c) => c.method == 'cancel'), isEmpty);
    window.releaseGate!.complete();
    await secondRelease;
    await _until(() => entry.state != UsbEntryState.returning);
    expect(entry.state, UsbEntryState.available);
    expect(entry.returnUncertain, isFalse);
  });

  test(
    'a new arrival during old release gets its own read-only confirmation',
    () async {
      final (session, socket, backend) = await fixture.started(
        devices: [fixture.stick(arrivalMs: 100)],
        cleanupWait: const Duration(milliseconds: 1),
      );
      final (entry, capture) = await fixture.shared(session, socket, backend);
      capture.releaseGate = Completer<void>();
      addTearDown(() async {
        if (!capture.releaseGate!.isCompleted) capture.releaseGate!.complete();
        await session.close();
        await backend.changeController.close();
      });
      backend.returnedState = 1;
      await session.stop(entry);
      expect(backend.returnedDevices, isEmpty);
      expect(entry.state, UsbEntryState.returning);
      // This ordering is reachable with the real two-lane runner: release is on
      // its device lane, while a replug/list query uses the independent query
      // lane. We do not let list/prepare bypass a running returned query.
      backend.devices = [fixture.stick(arrivalMs: 200)];
      await session.refresh();
      expect(entry.device.arrivalMs, 200);
      capture.releaseGate!.complete();
      await fixture.settle();
      expect(capture.releaseCalls, 1);
      expect(
        backend.captureCalls,
        1,
        reason: 'read-only confirmation must not recapture',
      );
      expect(
        backend.returnedDevices,
        hasLength(2),
        reason: 'release captured the old arrival; that superseded observation must be followed by a new-arrival read-only check',
      );
      expect(entry.state, UsbEntryState.available);
      expect(entry.returnUncertain, isFalse);
      expect(entry.notReturned, isFalse);
    },
  );

  // A stick shared in one window: 연결 in another window must not just fail
  // with usb_capture_failed.
  test('이 창으로 옮기기 across two windows: the holding window detaches it through its own session, then the other one takes it', () async {
    final physical = _Physical();
    final code2 = _Window('Code2', physical);
    final workPc = _Window('WorkPC', physical);
    UsbSession? code2Session, workSession;
    addTearDown(() async {
      await code2Session?.close();
      await workSession?.close();
      await code2.dispose();
      await workPc.dispose();
    });
    final (first, firstSocket) = await _start('code2-session', code2.backend);
    code2Session = first;
    await _share(first, firstSocket);
    expect(physical.owner, 'Code2');
    final (second, secondSocket) = await _start(
      'workpc-session',
      workPc.backend,
    );
    workSession = second;
    final entry = second.entries.single;
    expect(entry.device.otherWindow, 'Code2');
    expect(second.share(entry), usbDeviceRedirected);

    expect(await second.moveHere(entry), isNull);
    final detach = await firstSocket.text('usb.detach');
    expect(detach['reason'], 'usb_user_stopped', reason: "Code2's VM is told");
    expect(first.moves, 1);
    expect(first.lastMovedTo, 'WorkPC');
    final attach = await secondSocket.text('usb.attach');
    secondSocket.emit({'type': 'usb.attached', 'device': attach['device']});
    await _until(() => entry.state == UsbEntryState.shared);
    expect(physical.owner, 'WorkPC');
    final history = physical.history;
    expect(
      history.indexOf('WorkPC:ask:Code2'),
      lessThan(history.indexOf('Code2:release-done:1')),
    );
    expect(
      history.indexOf('Code2:release-done:1'),
      lessThan(history.indexOf('WorkPC:probe:1')),
      reason: 'WorkPC checks Windows took it back after Code2 let it go',
    );
    expect(
      history.indexOf('WorkPC:probe:1'),
      lessThan(history.indexOf('WorkPC:capture:1')),
    );
    expect(
      history.where((item) => item.startsWith('Code2:probe')),
      isEmpty,
      reason: 'the giving window does not report the device as not returned',
    );
    await _until(() => first.entries.single.state == UsbEntryState.available);
  });

  test('the fake log remains memory-only without initializing runtime storage', () {
    // No initialize(directory), real diagnostic reporter or writer is started.
    final log = ProductLog();
    log.record(LogEvent.usb, {'stage': 10, 'usbBack': 0, 'ok': false});
    expect(log.entries.single.values['usbBack'], 0);
  });
}
