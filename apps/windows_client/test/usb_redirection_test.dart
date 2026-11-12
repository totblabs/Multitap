import 'dart:async';
import 'dart:convert';
import 'dart:io';
import 'dart:typed_data';

import 'package:flutter_test/flutter_test.dart';
import 'package:multitap_client/data/product_log.dart';
import 'package:multitap_client/data/usb_redirection.dart';

// usb-v1 on the client: the descriptor policy (the same vectors as the broker
// and the Host), the relay frames, and the session with a fake relay and a
// fake capture backend: a device stays on this PC until the VM took it,
// requests run on the captured device, every request gets exactly one answer,
// and every device goes back to Windows however the session ends.
const sessionId = 'usb-test';

Uint8List hex(String text) => Uint8List.fromList([
  for (var i = 0; i < text.length; i += 2)
    int.parse(text.substring(i, i + 2), radix: 16),
]);

Map<String, dynamic> vectors() => jsonDecode(
  File('../../server/test/fixtures/usb-descriptor-vectors.json')
      .readAsStringSync(),
) as Map<String, dynamic>;
Uint8List vector(String name) =>
    hex((vectors()[name] as Map)['descriptors'] as String);

class FakeSocket extends Fake implements WebSocket {
  final incoming = StreamController<dynamic>();
  final texts = <Map<String, dynamic>>[];
  final frames = <UsbFrame>[];
  bool wasClosed = false;
  @override
  Duration? pingInterval;
  @override
  StreamSubscription<dynamic> listen(
    void Function(dynamic)? onData, {
    Function? onError,
    void Function()? onDone,
    bool? cancelOnError,
  }) => incoming.stream.listen(
    onData,
    onError: onError,
    onDone: onDone,
    cancelOnError: cancelOnError,
  );
  @override
  void add(dynamic data) {
    if (data is String) {
      texts.add(jsonDecode(data) as Map<String, dynamic>);
    } else {
      frames.add(UsbFrame.decode(Uint8List.fromList(data as List<int>))!);
    }
  }

  void emit(Map<String, dynamic> value) => incoming.add(jsonEncode(value));
  void emitFrame(UsbFrame frame) => incoming.add(frame.encode());
  @override
  Future<void> close([int? code, String? reason]) async {
    wasClosed = true;
    unawaited(incoming.close());
  }

  Future<Map<String, dynamic>> text(String type) async {
    for (var i = 0; i < 1000; i++) {
      final index = texts.indexWhere((value) => value['type'] == type);
      if (index >= 0) return texts.removeAt(index);
      await Future<void>.delayed(const Duration(milliseconds: 1));
    }
    throw StateError('No $type message');
  }

  Future<UsbFrame> frame(int type, {int? seq}) async {
    for (var i = 0; i < 1000; i++) {
      final index = frames.indexWhere(
        (value) => value.type == type && (seq == null || value.seq == seq),
      );
      if (index >= 0) return frames.removeAt(index);
      await Future<void>.delayed(const Duration(milliseconds: 1));
    }
    throw StateError('No frame of type $type');
  }
}

class FakeCapture implements UsbCapture {
  final transfers = <UsbTransfer>[];
  final cancelled = <int>[];
  final pending = <int, Completer<UsbTransferResult>>{};
  final goneCompleter = Completer<void>();
  bool released = false;
  int releaseCalls = 0;
  Completer<void>? releaseGate;
  Object? releaseFailure;
  UsbTransferResult Function(UsbTransfer)? handler;
  bool hold = false;
  bool ignoreCancel = false;
  @override
  Future<UsbTransferResult> transfer(UsbTransfer transfer) {
    transfers.add(transfer);
    if (hold) return (pending[transfer.token] = Completer()).future;
    final run = handler;
    if (run == null) {
      return Future.value(UsbTransferResult(0, Uint8List(transfer.length)));
    }
    return Future.value(run(transfer));
  }

  @override
  void cancel(int token) {
    cancelled.add(token);
    if (ignoreCancel) return;
    pending
        .remove(token)
        ?.complete(UsbTransferResult(usbStatusReset, Uint8List(0)));
  }

  @override
  Future<void> get gone => goneCompleter.future;
  @override
  Future<UsbStats> release() async {
    releaseCalls++;
    await releaseGate?.future;
    if (releaseFailure != null) throw releaseFailure!;
    released = true;
    return stats;
  }

  UsbStats stats = const UsbStats();
}

class FakeBackend extends UsbBackend {
  UsbDriverState driver = UsbDriverState.ready;
  UsbPrepareResult prepareResult = const UsbPrepareResult();
  final prepared = <String>[];
  Completer<void>? prepareGate;
  final changeController = StreamController<void>.broadcast();
  int watching = 0;
  @override
  Future<UsbPrepareResult> prepare(UsbLocalDevice device) async {
    prepared.add(device.id);
    await prepareGate?.future;
    return prepareResult;
  }

  @override
  Future<void> watch(bool on) async {
    watching += on ? 1 : -1;
  }

  @override
  Stream<void> get changes => changeController.stream;

  List<UsbLocalDevice> devices = [];
  final captures = <FakeCapture>[];
  Completer<void>? captureGate;
  Object? captureFailure;
  Object? installFailure;
  bool installed = false;
  int captureCalls = 0;
  int returnedState = 1;
  final returnedDevices = <String>[];
  Future<int> Function(UsbLocalDevice)? returnHandler;
  int detail = 0;
  @override
  int returnDetail(UsbLocalDevice device) => detail;
  @override
  Future<int> returned(UsbLocalDevice device) async {
    returnedDevices.add(device.id);
    return returnHandler == null ? returnedState : await returnHandler!(device);
  }

  @override
  Future<UsbEnvironment> environment() async => UsbEnvironment(driver);
  @override
  Future<List<UsbLocalDevice>> list() async => devices;
  @override
  Future<UsbCapture> capture(UsbLocalDevice device) async {
    captureCalls++;
    await captureGate?.future;
    if (captureFailure != null) throw captureFailure!;
    final capture = FakeCapture();
    captures.add(capture);
    return capture;
  }

  @override
  Future<void> installDriver() async {
    if (installFailure != null) throw installFailure!;
    installed = true;
    driver = UsbDriverState.ready;
  }

  // Another remote window of this client (이 창으로 옮기기).
  final handOverController =
      StreamController<({String id, String window})>.broadcast();
  final handOverAsks = <String>[];
  bool askAnswer = true;
  bool Function() holdsElsewhere = () => false;
  int holderChecks = 0;
  @override
  Stream<({String id, String window})> get handOvers =>
      handOverController.stream;
  @override
  Future<bool> handOver(UsbLocalDevice device) async {
    handOverAsks.add(device.id);
    return askAnswer;
  }

  @override
  Future<bool> heldElsewhere(UsbLocalDevice device) async {
    holderChecks++;
    return holdsElsewhere();
  }
}

UsbLocalDevice stick({
  String id = 'stick',
  bool busy = false,
  bool noReadableDrive = false,
  int arrivalMs = 0,
  String? otherWindow,
}) => UsbLocalDevice(
  id: id,
  name: 'SanDisk Cruzer',
  descriptors: vector('mass storage stick'),
  speed: 'high',
  busy: busy,
  noReadableDrive: noReadableDrive,
  arrivalMs: arrivalMs,
  otherWindow: otherWindow,
);

const ready = {
  'type': 'usb.ready',
  'sessionId': sessionId,
  'protocol': 'usb-v1',
  'limits': {
    'maxUrbBytes': 262144,
    'windowBytes': 1048576,
    'maxInFlight': 32,
    'maxDevices': 4,
    'maxAttaches': 64,
    'descriptorBytes': 4096,
  },
};

Future<(UsbSession, FakeSocket, FakeBackend)> started({
  List<UsbLocalDevice>? devices,
  bool hostReady = true,
  Duration captureTimeout = const Duration(seconds: 20),
  Duration prepareTimeout = const Duration(seconds: 15),
  Duration cleanupWait = const Duration(seconds: 5),
  Duration stuckCancel = const Duration(seconds: 3),
  Duration moveWait = const Duration(seconds: 8),
}) async {
  final socket = FakeSocket();
  final backend = FakeBackend()..devices = devices ?? [stick()];
  final session = UsbSession(
    url: Uri.parse('wss://vdi.example/api/v1/sessions/$sessionId/usb'),
    token: 'token',
    sessionId: sessionId,
    backend: backend,
    captureTimeout: captureTimeout,
    prepareTimeout: prepareTimeout,
    cleanupWait: cleanupWait,
    stuckCancel: stuckCancel,
    moveWait: moveWait,
    dialer: (url, token) async => socket,
  );
  await session.start();
  socket.emit(ready);
  if (hostReady) socket.emit({'type': 'usb.status', 'state': 'ready'});
  for (
    var i = 0;
    i < 200 && (session.entries.isEmpty || !session.ready && hostReady);
    i++
  ) {
    await Future<void>.delayed(const Duration(milliseconds: 1));
  }
  return (session, socket, backend);
}

Future<void> settle() => Future<void>.delayed(const Duration(milliseconds: 20));

/// The device is offered, the Host takes it and the capture is made.
Future<(UsbEntry, FakeCapture)> shared(
  UsbSession session,
  FakeSocket socket,
  FakeBackend backend,
) async {
  final entry = session.entries.first;
  expect(session.share(entry), isNull);
  final attach = await socket.text('usb.attach');
  socket.emit({'type': 'usb.attached', 'device': attach['device']});
  for (var i = 0; i < 200 && backend.captures.isEmpty; i++) {
    await Future<void>.delayed(const Duration(milliseconds: 1));
  }
  await settle();
  return (entry, backend.captures.first);
}

void main() {
  test('Every shared vector gets the decision recorded for the broker and the Host', () {
    final file = vectors();
    expect(file.length, greaterThanOrEqualTo(15));
    for (final entry in file.entries) {
      final bytes = hex((entry.value as Map)['descriptors'] as String);
      final device = UsbDescriptors.parse(bytes);
      expect(device, isNotNull, reason: entry.key);
      final decision = usbDecide(device!);
      final expected = (entry.value as Map)['expect'] as Map;
      expect(decision.allowed, expected['allowed'], reason: entry.key);
      if (decision.allowed) {
        expect(
          decision.requires.toList()..sort(),
          (expected['requires'] as List).cast<String>(),
          reason: entry.key,
        );
        expect(
          decision.classes.toList()..sort(),
          (expected['classes'] as List).cast<int>(),
          reason: entry.key,
        );
      } else {
        expect(decision.reason, expected['reason'], reason: entry.key);
      }
    }
  });

  test('Malformed descriptor sets are refused, never partly read', () {
    final good = vector('mass storage stick');
    expect(UsbDescriptors.parse(good), isNotNull);
    Uint8List changed(int index, int value) =>
        Uint8List.fromList(good)..[index] = value;
    for (final bad in [
      good.sublist(0, 20),
      changed(0, 17),
      changed(17, 0),
      changed(17, 5),
      changed(7, 7),
      changed(20, 255),
      changed(18 + 9, 1),
      changed(18 + 9 + 4, 3),
      Uint8List.fromList([...good, 0]),
      Uint8List(UsbDescriptors.maxBytes + 1),
    ]) {
      expect(UsbDescriptors.parse(bad), isNull);
    }
    final device = UsbDescriptors.parse(good)!;
    expect([
      for (final a in [0x81, 0x02, 0x00, 0x80]) device.hasEndpoint(a),
    ], everyElement(isTrue));
    expect([
      for (final a in [0x82, 0x01, 0x03]) device.hasEndpoint(a),
    ], everyElement(isFalse));
  });

  test(
    'Relay frames are 24 little-endian bytes the broker reads the same way',
    () {
      final frame = UsbFrame(
        type: usbFrameReturn,
        device: 3,
        endpoint: 0x81,
        transfer: 2,
        seq: 0x01020304,
        length: 512,
        status: -32,
        setup: Uint8List.fromList([1, 2, 3, 4, 5, 6, 7, 8]),
        payload: Uint8List.fromList([9, 8, 7]),
      );
      final bytes = frame.encode();
      expect(bytes.length, 27);
      expect(bytes.sublist(0, 4), [2, 3, 0x81, 2]);
      expect(bytes.sublist(4, 8), [4, 3, 2, 1]);
      expect(bytes.sublist(8, 12), [0, 2, 0, 0]);
      expect(bytes.sublist(12, 16), [0xe0, 0xff, 0xff, 0xff]);
      expect(bytes.sublist(16, 24), [1, 2, 3, 4, 5, 6, 7, 8]);
      final back = UsbFrame.decode(bytes)!;
      expect(
        [
          back.type,
          back.device,
          back.endpoint,
          back.transfer,
          back.seq,
          back.length,
          back.status,
        ],
        [2, 3, 0x81, 2, 0x01020304, 512, -32],
      );
      expect(back.payload, [9, 8, 7]);
      expect(UsbFrame.decode(Uint8List(23)), isNull);
      expect(
        UsbFrame.decode(Uint8List(usbHeaderBytes + usbMaxUrbBytes + 1)),
        isNull,
      );
    },
  );

  test('Devices the policy always denies are not listed; the rest wait for the Host', () async {
    final (session, socket, _) = await started(
      devices: [
        stick(),
        UsbLocalDevice(
          id: 'kb',
          name: 'Keyboard',
          descriptors: vector('boot keyboard'),
          speed: 'low',
        ),
        UsbLocalDevice(
          id: 'hub',
          name: 'Hub',
          descriptors: vector('hub'),
          speed: 'high',
        ),
        UsbLocalDevice(
          id: 'cam',
          name: 'Webcam',
          descriptors: vector('webcam (video)'),
          speed: 'high',
        ),
        UsbLocalDevice(
          id: 'junk',
          name: 'Junk',
          descriptors: Uint8List(5),
          speed: 'high',
        ),
      ],
      hostReady: false,
    );
    expect(session.entries.map((e) => e.device.id), ['stick']);
    expect(session.preparing, isTrue);
    expect(session.share(session.entries.first), 'usb_vhci_unavailable');
    expect(socket.texts.where((m) => m['type'] == 'usb.attach'), isEmpty);
    socket.emit({
      'type': 'usb.status',
      'state': 'unavailable',
      'reason': 'usb_driver_missing',
    });
    await settle();
    expect(session.ready, isFalse);
    expect(session.unavailableReason, 'usb_driver_missing');
    expect(session.share(session.entries.first), 'usb_driver_missing');
    socket.emit({'type': 'usb.status', 'state': 'ready'});
    await settle();
    expect(session.ready, isTrue);
    await session.close();
  });

  // A USB stick in Explorer that is missing from the panel must leave a
  // record of why. An unreadable device is counted, shown as one line and
  // recorded with the listing's counts.
  test('A device whose descriptors could not be read is counted and recorded, never offered', () async {
    final (session, _, _) = await started(
      devices: [
        stick(),
        UsbLocalDevice(
          id: 'ventoy',
          name: 'USB drive',
          descriptors: Uint8List(18),
          speed: 'high',
          unreadable: true,
        ),
        UsbLocalDevice(
          id: 'kb',
          name: 'Keyboard',
          descriptors: vector('boot keyboard'),
          speed: 'low',
        ),
        UsbLocalDevice(
          id: 'cam',
          name: 'Webcam',
          descriptors: vector('webcam (video)'),
          speed: 'high',
        ),
      ],
      hostReady: false,
    );
    expect(session.entries.map((e) => e.device.id), ['stick']);
    expect(session.unreadable, 1);
    expect(session.hiddenNeeded, 1);
    final listed = ProductLog.instance.entries.lastWhere(
      (entry) => entry.event == LogEvent.usb && entry.values['stage'] == 15,
    );
    expect(listed.values, {
      'stage': 15,
      'ok': false,
      'usbFound': 4,
      'usbHidden': 1,
      'usbDenied': 1,
      'usbUnreadable': 1,
      'usbDevices': 1,
    });
    await session.close();
  });

  test('A device stays on this PC until the VM took it; then it is captured and requests run on it', () async {
    final (session, socket, backend) = await started();
    final entry = session.entries.first;
    expect(session.share(entry), isNull);
    expect(entry.state, UsbEntryState.connecting);
    final attach = await socket.text('usb.attach');
    expect(attach['device'], 1);
    expect(attach['speed'], 'high');
    expect(
      base64Decode(attach['descriptors'] as String),
      vector('mass storage stick'),
    );
    expect(attach.keys.toSet(), {
      'type',
      'device',
      'descriptors',
      'speed',
    }, reason: 'no name, ID or serial number crosses');
    await settle();
    expect(
      backend.captureCalls,
      0,
      reason: 'not captured before the VM took it',
    );
    socket.emit({'type': 'usb.attached', 'device': 1});
    await settle();
    expect(backend.captureCalls, 1);
    expect(entry.state, UsbEntryState.shared);
    final capture = backend.captures.single;

    // Bulk OUT.
    capture.handler = (t) => UsbTransferResult(0, Uint8List(0));
    socket.emitFrame(
      UsbFrame(
        type: usbFrameSubmit,
        device: 1,
        endpoint: 0x02,
        transfer: 2,
        seq: 10,
        length: 4,
        payload: Uint8List.fromList([1, 2, 3, 4]),
      ),
    );
    var answer = await socket.frame(usbFrameReturn, seq: 10);
    expect(
      [
        answer.endpoint,
        answer.transfer,
        answer.length,
        answer.status,
        answer.payload.length,
      ],
      [0x02, 2, 4, 0, 0],
    );
    expect(capture.transfers.last.data, [1, 2, 3, 4]);
    expect(capture.transfers.last.setup, isNull);

    // Bulk IN: the bytes read.
    capture.handler = (t) =>
        UsbTransferResult(0, Uint8List.fromList(List.filled(100, 9)));
    socket.emitFrame(
      UsbFrame(
        type: usbFrameSubmit,
        device: 1,
        endpoint: 0x81,
        transfer: 2,
        seq: 11,
        length: 512,
      ),
    );
    answer = await socket.frame(usbFrameReturn, seq: 11);
    expect(
      [answer.length, answer.status, answer.payload.length],
      [100, 0, 100],
    );

    // Control IN carries its setup packet.
    final setup = Uint8List.fromList([0x80, 0, 0, 0, 0, 0, 2, 0]);
    capture.handler = (t) => UsbTransferResult(0, Uint8List.fromList([0, 0]));
    socket.emitFrame(
      UsbFrame(
        type: usbFrameSubmit,
        device: 1,
        endpoint: 0x80,
        transfer: 0,
        seq: 12,
        length: 2,
        setup: setup,
      ),
    );
    answer = await socket.frame(usbFrameReturn, seq: 12);
    expect(answer.payload.length, 2);
    expect(capture.transfers.last.setup, setup);
    expect(capture.transfers.last.type, 0);

    // A stall is an answer; an unknown status or an exception becomes a fixed one.
    capture.handler = (t) =>
        UsbTransferResult(usbStatusStall, Uint8List.fromList([1]));
    socket.emitFrame(
      UsbFrame(
        type: usbFrameSubmit,
        device: 1,
        endpoint: 0x81,
        transfer: 2,
        seq: 13,
        length: 64,
      ),
    );
    answer = await socket.frame(usbFrameReturn, seq: 13);
    expect([answer.status, answer.length, answer.payload.length], [-32, 0, 0]);
    capture.handler = (t) => UsbTransferResult(-1234, Uint8List(0));
    socket.emitFrame(
      UsbFrame(
        type: usbFrameSubmit,
        device: 1,
        endpoint: 0x81,
        transfer: 2,
        seq: 14,
        length: 64,
      ),
    );
    expect(
      (await socket.frame(usbFrameReturn, seq: 14)).status,
      usbStatusProtocol,
    );
    capture.handler = (t) => throw StateError('private details');
    socket.emitFrame(
      UsbFrame(
        type: usbFrameSubmit,
        device: 1,
        endpoint: 0x81,
        transfer: 2,
        seq: 15,
        length: 64,
      ),
    );
    expect((await socket.frame(usbFrameReturn, seq: 15)).status, usbStatusIo);
    capture.handler = (t) => UsbTransferResult(0, Uint8List(65));
    socket.emitFrame(
      UsbFrame(
        type: usbFrameSubmit,
        device: 1,
        endpoint: 0x81,
        transfer: 2,
        seq: 16,
        length: 64,
      ),
    );
    answer = await socket.frame(usbFrameReturn, seq: 16);
    expect(
      [answer.status, answer.payload.length],
      [usbStatusOverflow, 0],
      reason: 'more data than asked is an overflow, never relayed',
    );

    await session.close();
    expect(capture.released, isTrue);
  });

  test(
    'Requests that arrive while the capture is being made wait for it',
    () async {
      final (session, socket, backend) = await started();
      backend.captureGate = Completer<void>();
      final entry = session.entries.first;
      session.share(entry);
      final attach = await socket.text('usb.attach');
      socket.emit({'type': 'usb.attached', 'device': attach['device']});
      socket.emitFrame(
        UsbFrame(
          type: usbFrameSubmit,
          device: 1,
          endpoint: 0x80,
          transfer: 0,
          seq: 1,
          length: 18,
          setup: Uint8List.fromList([0x80, 6, 0, 1, 0, 0, 18, 0]),
        ),
      );
      await settle();
      expect(socket.frames, isEmpty);
      backend.captureGate!.complete();
      final answer = await socket.frame(usbFrameReturn, seq: 1);
      expect(answer.status, 0);
      await session.close();
    },
  );

  test('A refused device never leaves this PC', () async {
    final (session, socket, backend) = await started();
    final entry = session.entries.first;
    session.share(entry);
    final attach = await socket.text('usb.attach');
    socket.emit({
      'type': 'usb.rejected',
      'device': attach['device'],
      'reason': 'usb_class_not_granted',
    });
    await settle();
    expect(backend.captureCalls, 0);
    expect(entry.state, UsbEntryState.refused);
    expect(entry.code, 'usb_class_not_granted');
    expect(usbMessage(entry.code), contains('관리자'));
    // It can be offered again (a new device number).
    expect(session.share(entry), isNull);
    expect((await socket.text('usb.attach'))['device'], 2);
    await session.close();
  });

  test(
    'A device the VM took but this PC cannot capture is handed back at once',
    () async {
      final (session, socket, backend) = await started();
      backend.captureFailure = const UsbBackendException('usb_device_busy');
      final entry = session.entries.first;
      session.share(entry);
      final attach = await socket.text('usb.attach');
      socket.emit({'type': 'usb.attached', 'device': attach['device']});
      final detach = await socket.text('usb.detach');
      expect(detach, {
        'type': 'usb.detach',
        'device': 1,
        'reason': 'usb_device_error',
      });
      expect(entry.state, UsbEntryState.failed);
      expect(entry.code, 'usb_device_busy');
      await session.close();
    },
  );

  test('Unlink cancels the request, answers the unlink, and the request still gets exactly one answer', () async {
    final (session, socket, backend) = await started();
    final (_, capture) = await shared(session, socket, backend);
    capture.hold = true;
    socket.emitFrame(
      UsbFrame(
        type: usbFrameSubmit,
        device: 1,
        endpoint: 0x81,
        transfer: 2,
        seq: 21,
        length: 512,
      ),
    );
    await settle();
    socket.emitFrame(
      UsbFrame(type: usbFrameUnlink, device: 1, seq: 22, length: 21),
    );
    final unlinked = await socket.frame(usbFrameUnlinkReturn, seq: 22);
    expect(unlinked.status, usbStatusReset);
    expect(capture.cancelled, [21]);
    final answer = await socket.frame(usbFrameReturn, seq: 21);
    expect(answer.status, usbStatusReset);
    expect(
      socket.frames.where((f) => f.type == usbFrameReturn && f.seq == 21),
      isEmpty,
    );
    // An unlink of something already done is answered with 0.
    socket.emitFrame(
      UsbFrame(type: usbFrameUnlink, device: 1, seq: 23, length: 21),
    );
    expect((await socket.frame(usbFrameUnlinkReturn, seq: 23)).status, 0);
    await session.close();
  });

  test(
    'A stuck cancel returns one reset and drops the eventual device answer',
    () async {
      final (session, socket, backend) = await started(
        stuckCancel: const Duration(milliseconds: 10),
      );
      final (_, capture) = await shared(session, socket, backend);
      capture
        ..hold = true
        ..ignoreCancel = true;
      socket.emitFrame(
        UsbFrame(
          type: usbFrameSubmit,
          device: 1,
          endpoint: 0x80,
          transfer: 0,
          seq: 31,
          length: 1,
          setup: Uint8List.fromList([0xa1, 0xfe, 0, 0, 0, 0, 1, 0]),
        ),
      );
      await settle();
      socket.emitFrame(
        UsbFrame(type: usbFrameUnlink, device: 1, seq: 32, length: 31),
      );
      expect(
        (await socket.frame(usbFrameUnlinkReturn, seq: 32)).status,
        usbStatusReset,
      );
      expect(
        (await socket.frame(usbFrameReturn, seq: 31)).status,
        usbStatusReset,
      );
      expect(capture.cancelled, [
        31,
        31,
      ], reason: 'one initial abort and one watchdog abort');
      capture.pending
          .remove(31)!
          .complete(UsbTransferResult(0, Uint8List.fromList([0])));
      await settle();
      expect(
        socket.frames.where((f) => f.seq == 31),
        isEmpty,
        reason: 'the late real result must not answer twice',
      );
      expect(session.closed, isFalse);
      capture.hold = false;
      socket.emitFrame(
        UsbFrame(
          type: usbFrameSubmit,
          device: 1,
          endpoint: 0x81,
          transfer: 2,
          seq: 33,
          length: 8,
        ),
      );
      expect(
        (await socket.frame(usbFrameReturn, seq: 33)).status,
        0,
        reason: 'the device remains usable',
      );
      await session.close();
    },
  );

  test('An unlink before capture never submits the cancelled request to the device', () async {
    for (final watchdogFirst in [false, true]) {
      final (session, socket, backend) = await started(
        stuckCancel: watchdogFirst
            ? const Duration(milliseconds: 10)
            : const Duration(seconds: 1),
      );
      backend.captureGate = Completer<void>();
      session.share(session.entries.first);
      final attach = await socket.text('usb.attach');
      socket.emit({'type': 'usb.attached', 'device': attach['device']});
      socket.emitFrame(
        UsbFrame(
          type: usbFrameSubmit,
          device: 1,
          endpoint: 0x02,
          transfer: 2,
          seq: 41,
          length: 4,
          payload: Uint8List.fromList([1, 2, 3, 4]),
        ),
      );
      await settle();
      socket.emitFrame(
        UsbFrame(type: usbFrameUnlink, device: 1, seq: 42, length: 41),
      );
      expect(
        (await socket.frame(usbFrameUnlinkReturn, seq: 42)).status,
        usbStatusReset,
      );
      if (watchdogFirst) {
        expect(
          (await socket.frame(usbFrameReturn, seq: 41)).status,
          usbStatusReset,
        );
      }
      backend.captureGate!.complete();
      if (!watchdogFirst) {
        expect(
          (await socket.frame(usbFrameReturn, seq: 41)).status,
          usbStatusReset,
        );
      }
      await settle();
      expect(
        backend.captures.single.transfers,
        isEmpty,
        reason: 'a cancelled OUT must not change the device later',
      );
      expect(socket.frames.where((f) => f.seq == 41), isEmpty);
      await session.close();
    }
  });

  test(
    'Capture timeout detaches the VM and releases a capture that arrives late',
    () async {
      final (session, socket, backend) = await started(
        captureTimeout: const Duration(milliseconds: 10),
      );
      backend.captureGate = Completer<void>();
      final entry = session.entries.first;
      session.share(entry);
      final attach = await socket.text('usb.attach');
      socket.emit({'type': 'usb.attached', 'device': attach['device']});
      socket.emitFrame(
        UsbFrame(
          type: usbFrameSubmit,
          device: 1,
          endpoint: 0x81,
          transfer: 2,
          seq: 51,
          length: 8,
        ),
      );
      expect(await socket.text('usb.detach'), {
        'type': 'usb.detach',
        'device': 1,
        'reason': 'usb_device_error',
      });
      expect(entry.state, UsbEntryState.returning);
      expect(entry.code, 'usb_capture_timeout');
      expect(session.sharedCount, 0);
      backend.captureGate!.complete();
      await settle();
      expect(backend.captures.single.released, isTrue);
      expect(backend.captures.single.releaseCalls, 1);
      expect(entry.state, UsbEntryState.failed);
      expect(backend.captures.single.transfers, isEmpty);
      expect(socket.frames, isEmpty);
      expect(
        session.closed,
        isFalse,
        reason: 'a failed capture does not close the session',
      );
      await session.close();
    },
  );

  test(
    'Cancelled prepare cannot remove or offer a newer prepare attempt',
    () async {
      final (session, socket, backend) = await started();
      final first = Completer<void>(), second = Completer<void>();
      backend.prepareGate = first;
      final entry = session.entries.single;
      session.share(entry);
      await settle();
      await session.stop(entry);
      expect(entry.state, UsbEntryState.returning);
      backend.prepareGate = second;
      session.share(entry);
      await settle();
      expect(backend.prepared, [
        'stick',
      ], reason: 'the original native query still owns its locks');
      first.complete();
      await settle();
      expect(socket.texts.where((m) => m['type'] == 'usb.attach'), isEmpty);
      expect(entry.state, UsbEntryState.available);
      session.share(entry);
      await settle();
      expect(entry.state, UsbEntryState.connecting);
      second.complete();
      expect((await socket.text('usb.attach'))['device'], 1);
      expect(backend.prepared, ['stick', 'stick']);
      await session.close();
    },
  );

  test('Timed-out prepare stays failed until a deliberate retry and drops late success', () async {
    final (session, socket, backend) = await started(
      prepareTimeout: const Duration(milliseconds: 10),
    );
    final late = Completer<void>();
    backend.prepareGate = late;
    final entry = session.entries.single;
    session.share(entry);
    await settle();
    expect(entry.state, UsbEntryState.returning);
    expect(entry.code, 'usb_prepare_timeout');
    expect(socket.texts.where((m) => m['type'] == 'usb.attach'), isEmpty);
    session.share(entry);
    expect(socket.texts.where((m) => m['type'] == 'usb.attach'), isEmpty);
    late.complete();
    await settle();
    expect(entry.state, UsbEntryState.failed);
    expect(socket.texts.where((m) => m['type'] == 'usb.attach'), isEmpty);
    backend.prepareGate = null;
    session.share(entry);
    expect((await socket.text('usb.attach'))['device'], 1);
    expect(entry.state, UsbEntryState.connecting);
    await session.close();
  });

  test('Return owns the device until release really completes, even beyond the UI wait', () async {
    final (session, socket, backend) = await started(
      cleanupWait: const Duration(milliseconds: 10),
    );
    final oldReturn = Completer<int>();
    backend.returnHandler = (_) => oldReturn.future;
    final (entry, capture) = await shared(session, socket, backend);
    capture.releaseGate = Completer<void>();
    await session.stop(entry);
    expect(entry.state, UsbEntryState.returning);
    expect(capture.releaseCalls, 1);
    expect(capture.released, isFalse);
    session.share(entry);
    expect(socket.texts.where((m) => m['type'] == 'usb.attach'), isEmpty);
    backend.devices = [];
    await session.refresh();
    expect(
      session.entries.single,
      same(entry),
      reason: 'returning device must remain visible',
    );
    capture.releaseGate!.complete();
    await settle();
    expect(
      entry.state,
      UsbEntryState.returning,
      reason: 'native release alone is not Windows return',
    );
    session.share(entry);
    expect(socket.texts.where((m) => m['type'] == 'usb.attach'), isEmpty);
    oldReturn.complete(1);
    await settle();
    expect(entry.state, UsbEntryState.available);
    backend.devices = [stick()];
    await session.refresh();
    session.share(entry);
    expect((await socket.text('usb.attach'))['device'], 2);
    await settle();
    expect(entry.notReturned, isFalse);
    expect(entry.state, UsbEntryState.connecting);
    await session.close();
  });

  test('Cancel during native capture owns and releases the late handle once before retry', () async {
    final (session, socket, backend) = await started(
      cleanupWait: const Duration(milliseconds: 10),
    );
    final pending = Completer<void>();
    backend.captureGate = pending;
    final entry = session.entries.single;
    session.share(entry);
    final offered = await socket.text('usb.attach');
    socket.emit({'type': 'usb.attached', 'device': offered['device']});
    await settle();
    await session.stop(entry);
    expect(entry.state, UsbEntryState.returning);
    session.share(entry);
    expect(socket.texts.where((m) => m['type'] == 'usb.attach'), isEmpty);
    pending.complete();
    await settle();
    expect(backend.captures.single.released, isTrue);
    expect(backend.captures.single.releaseCalls, 1);
    expect(backend.captures.single.transfers, isEmpty);
    expect(entry.state, UsbEntryState.available);
    session.share(entry);
    expect((await socket.text('usb.attach'))['device'], 2);
    await session.close();
  });

  test(
    'Release checks Windows return and a real replug clears the failure',
    () async {
      for (final state in [0, 1, 2]) {
        final (session, socket, backend) = await started();
        backend.returnedState = state;
        final (entry, capture) = await shared(session, socket, backend);
        await session.stop(entry);
        await settle();
        expect(capture.released, isTrue);
        expect(backend.returnedDevices, ['stick']);
        expect(
          entry.notReturned,
          state == 2,
          reason: 'unknown does not pretend return failed',
        );
        if (state == 2) {
          backend.returnedState = 1;
          backend.devices = [stick(arrivalMs: 1)];
          await session.refresh();
          await settle();
          expect(identical(session.entries.single, entry), isTrue);
          expect(
            entry.notReturned,
            isFalse,
            reason: 'Windows saw a new arrival',
          );
        }
        await session.close();
      }
    },
  );

  test('Unknown or failed Windows return blocks recapture until a read-only recheck succeeds', () async {
    for (final returned in [0, 2]) {
      final (session, socket, backend) = await started();
      backend.returnedState = returned;
      final (entry, _) = await shared(session, socket, backend);
      await session.stop(entry);
      expect(entry.state, UsbEntryState.failed);
      expect(entry.returnUncertain, isTrue);
      expect(entry.notReturned, returned == 2);
      expect(
        session.share(entry),
        returned == 2 ? usbNotReturned : usbReturnUnknown,
      );
      expect(socket.texts.where((m) => m['type'] == 'usb.attach'), isEmpty);
      final calls = backend.captureCalls;
      backend.returnedState = 1;
      await session.recheckReturn(entry);
      expect(
        backend.captureCalls,
        calls,
        reason: 'return check must not capture',
      );
      expect(entry.state, UsbEntryState.available);
      expect(entry.returnUncertain, isFalse);
      session.share(entry);
      expect((await socket.text('usb.attach'))['device'], 2);
      await session.close();
    }
  });

  test('PC로 되돌리기: a device Windows did not take back is taken and given back once, then checked', () async {
    final (session, socket, backend) = await started();
    backend.returnedState = 2;
    final (entry, _) = await shared(session, socket, backend);
    await session.stop(entry);
    expect(entry.notReturned, isTrue);
    expect(backend.captureCalls, 1);
    // Windows starts it after the cycle.
    backend.returnedState = 1;
    await session.bringBack(entry);
    expect(backend.captureCalls, 2, reason: 'one more capture, never more');
    expect(backend.captures.last.released, isTrue);
    expect(
      backend.captures.last.transfers,
      isEmpty,
      reason: 'nothing runs on it',
    );
    expect(entry.state, UsbEntryState.available);
    expect(entry.returnUncertain, isFalse);
    expect(entry.notReturned, isFalse);
    expect(
      socket.texts.where((m) => m['type'] == 'usb.attach'),
      isEmpty,
      reason: 'the VM is not offered it',
    );
    // Only for a device whose return is uncertain.
    await session.bringBack(entry);
    expect(backend.captureCalls, 2);
    await session.close();
  });

  test('The diagnostics carry the raw USBD status of the first failure and why Windows did not take a device back', () async {
    final (session, socket, backend) = await started();
    backend
      ..returnedState = 2
      ..detail = 143; // Windows problem code 43 on the device node
    final (entry, capture) = await shared(session, socket, backend);
    capture.stats = const UsbStats(
      urbs: 137,
      stalls: 1,
      firstClass: 0x8006,
      firstStatus: 32,
      firstUsbd: 0x80000800,
    );
    final before = ProductLog.instance.entries.length;
    await session.stop(entry);
    final usb = ProductLog.instance.entries
        .skip(before)
        .where((e) => e.event == LogEvent.usb)
        .map((e) => e.values)
        .toList();
    final summary = usb.singleWhere((v) => v['stage'] == 9);
    expect(summary['usbUsbd'], 0x80000800);
    expect(summary['error'], 32);
    final back = usb.singleWhere((v) => v['stage'] == 10);
    expect(back['usbBack'], 2);
    expect(back['error'], 143);
    await session.close();
  });

  test('PC로 되돌리기 that cannot take the device only checks, and a failed return stays failed', () async {
    final (session, socket, backend) = await started();
    backend.returnedState = 2;
    final (entry, _) = await shared(session, socket, backend);
    await session.stop(entry);
    backend.captureFailure = const UsbBackendException('usb_capture_failed');
    final checks = backend.returnedDevices.length;
    await session.bringBack(entry);
    expect(
      backend.returnedDevices.length,
      checks + 1,
      reason: 'the read-only check still runs',
    );
    expect(entry.state, UsbEntryState.failed);
    expect(entry.code, usbNotReturned);
    expect(session.share(entry), usbNotReturned);
    // Back on this PC (files open: the capture refuses it): the check says so.
    backend.captureFailure = const UsbBackendException('usb_device_busy');
    backend.returnedState = 1;
    await session.bringBack(entry);
    expect(entry.state, UsbEntryState.available);
    expect(entry.returnUncertain, isFalse);
    await session.close();
  });

  test('A release exception keeps ownership uncertain and never advertises available', () async {
    final (session, socket, backend) = await started();
    final (entry, capture) = await shared(session, socket, backend);
    capture.releaseFailure = StateError('private native release failure');
    await session.stop(entry);
    expect(entry.state, UsbEntryState.failed);
    expect(entry.code, usbReturnUnknown);
    expect(entry.returnUncertain, isTrue);
    expect(backend.returnedDevices, isEmpty);
    expect(session.share(entry), usbReturnUnknown);
    expect(socket.texts.where((m) => m['type'] == 'usb.attach'), isEmpty);
    await session.recheckReturn(entry);
    expect(
      entry.state,
      UsbEntryState.available,
      reason:
          'a later confirmed Windows restart permits an explicit new attempt',
    );
    await session.close();
  });

  test('Stopping gives the device back and tells the VM; the VM or the broker detaching does too', () async {
    final (session, socket, backend) = await started(
      devices: [
        stick(),
        stick(id: 'other'),
      ],
    );
    final first = session.entries[0], second = session.entries[1];
    session.share(first);
    session.share(second);
    final a = await socket.text('usb.attach'),
        b = await socket.text('usb.attach');
    socket.emit({'type': 'usb.attached', 'device': a['device']});
    socket.emit({'type': 'usb.attached', 'device': b['device']});
    await settle();
    expect(backend.captures.length, 2);
    await session.stop(first);
    expect(await socket.text('usb.detach'), {
      'type': 'usb.detach',
      'device': 1,
      'reason': 'usb_user_stopped',
    });
    expect(backend.captures[0].released, isTrue);
    expect(first.state, UsbEntryState.available);
    expect(backend.captures[1].released, isFalse);
    // The broker revokes the permission: the other device goes back too.
    socket.emit({
      'type': 'usb.detach',
      'device': 2,
      'reason': 'usb_policy_changed',
    });
    await settle();
    expect(backend.captures[1].released, isTrue);
    expect(second.state, UsbEntryState.failed);
    expect(second.code, 'usb_policy_changed');
    expect(session.sharedCount, 0);
    // A late request for a device that is gone is ignored.
    socket.emitFrame(
      UsbFrame(
        type: usbFrameSubmit,
        device: 2,
        endpoint: 0x81,
        transfer: 2,
        seq: 5,
        length: 8,
      ),
    );
    await settle();
    expect(socket.frames, isEmpty);
    await session.close();
  });

  test(
    'A device unplugged from this PC is reported to the VM and released',
    () async {
      final (session, socket, backend) = await started();
      final (entry, capture) = await shared(session, socket, backend);
      capture.goneCompleter.complete();
      expect(await socket.text('usb.detach'), {
        'type': 'usb.detach',
        'device': 1,
        'reason': 'usb_device_removed',
      });
      await settle();
      expect(capture.released, isTrue);
      expect(entry.code, 'usb_device_removed');
      await session.close();
    },
  );

  // Pulling out a connected stick must not wait 10 s for it to come back
  // and then say "장치가 PC로 돌아오지 않았어요".
  test(
    'A device pulled out while connected keeps that reason: no wait for its return, no not-returned state, and the list is read again',
    () async {
      final (session, socket, backend) = await started();
      final (entry, capture) = await shared(session, socket, backend);
      // Windows would never report it back: the wait would end in 2.
      backend.returnedState = 2;
      backend.devices = [];
      capture.goneCompleter.complete();
      await socket.text('usb.detach');
      await settle();
      await settle();
      expect(capture.released, isTrue);
      expect(backend.returnedDevices, isEmpty);
      expect(entry.state, UsbEntryState.failed);
      expect(entry.code, 'usb_device_removed');
      expect(entry.notReturned, isFalse);
      expect(entry.returnUncertain, isFalse);
      expect(usbMessage(entry.code), '장치가 PC에서 분리됐어요.');
      expect(session.entries, isEmpty, reason: 'the list no longer has it');
      await session.close();
    },
  );

  test(
    'A device plugged in again after being pulled out can be connected again',
    () async {
      final (session, socket, backend) = await started();
      final (entry, capture) = await shared(session, socket, backend);
      backend.returnedState = 2;
      backend.devices = [stick(arrivalMs: 5000)];
      capture.goneCompleter.complete();
      await socket.text('usb.detach');
      await settle();
      await settle();
      expect(session.entries.single, same(entry));
      expect(entry.code, 'usb_device_removed');
      expect(entry.returnUncertain, isFalse);
      backend.returnedState = 1;
      expect(session.share(entry), isNull);
      await session.close();
    },
  );

  // A USB 3 stick in a USB 3 port (bMaxPacketSize0 = 9) must not be
  // counted as unreadable, with the panel saying to plug it in again.
  test(
    'A SuperSpeed device is refused as before but counted on its own, not as unreadable',
    () async {
      final fast = Uint8List.fromList(vector('mass storage stick'))..[7] = 9;
      expect(UsbDescriptors.parse(fast), isNull);
      expect(UsbDescriptors.superSpeed(fast), isTrue);
      expect(UsbDescriptors.superSpeed(vector('mass storage stick')), isFalse);
      final (session, _, _) = await started(
        devices: [
          stick(),
          UsbLocalDevice(
            id: 'fast',
            name: 'USB 3 stick',
            descriptors: fast,
            speed: 'super',
          ),
        ],
      );
      expect(session.entries.map((e) => e.device.id), ['stick']);
      expect(session.superSpeed, 1);
      expect(session.unreadable, 0);
      final listed = ProductLog.instance.entries.lastWhere(
        (entry) => entry.event == LogEvent.usb && entry.values['stage'] == 15,
      );
      expect(listed.values['usbUnreadable'], 1, reason: 'the record is as before');
      await session.close();
    },
  );

  test(
    'Ending the session gives every device back, whichever way it ends',
    () async {
      // The user closes the session.
      var (session, socket, backend) = await started();
      var (_, capture) = await shared(session, socket, backend);
      capture.hold = true;
      socket.emitFrame(
        UsbFrame(
          type: usbFrameSubmit,
          device: 1,
          endpoint: 0x81,
          transfer: 2,
          seq: 1,
          length: 8,
        ),
      );
      await settle();
      await session.close();
      expect(capture.released, isTrue);
      expect(capture.cancelled, [
        1,
      ], reason: 'requests in flight are cancelled');
      expect(socket.wasClosed, isTrue);
      expect(
        socket.texts.any(
          (m) =>
              m['type'] == 'usb.detach' && m['reason'] == 'usb_session_closed',
        ),
        isTrue,
      );
      // The relay connection drops.
      (session, socket, backend) = await started();
      (_, capture) = await shared(session, socket, backend);
      await socket.incoming.close();
      await settle();
      expect(capture.released, isTrue);
      expect(session.closed, isTrue);
      expect(session.error, 'usb_transport_closed');
      // The relay reports an error.
      (session, socket, backend) = await started();
      (_, capture) = await shared(session, socket, backend);
      socket.emit({'type': 'usb.error', 'reason': 'usb_policy_changed'});
      await settle();
      expect(capture.released, isTrue);
      expect(session.error, 'usb_policy_changed');
      expect(usbEndedMessage(session), contains('관리자'));
      // Garbage ends the channel and returns the device.
      (session, socket, backend) = await started();
      (_, capture) = await shared(session, socket, backend);
      socket.incoming.add(jsonEncode({'type': 'usb.format'}));
      await settle();
      expect(capture.released, isTrue);
      expect(session.error, 'usb_invalid_message');
      (session, socket, backend) = await started();
      (_, capture) = await shared(session, socket, backend);
      socket.incoming.add(Uint8List(5));
      await settle();
      expect(capture.released, isTrue);
      // The wrong session id never starts anything.
      final bad = FakeSocket();
      final other = UsbSession(
        url: Uri.parse('wss://vdi.example/x'),
        token: 't',
        sessionId: sessionId,
        backend: FakeBackend(),
        dialer: (u, t) async => bad,
      );
      await other.start();
      bad.emit({...ready, 'sessionId': 'another'});
      await settle();
      expect(other.closed, isTrue);
    },
  );

  // Tuning #7: the channel's end summary (stage 5; 24 failures in 24 h were
  // ended sessions) and a device the VM let go (stage 3) are ok when the
  // session or an administrator ended them; the code says which.
  test('USB end records are ok only for normal ends', () async {
    ProductLogEntry last(int stage) => ProductLog.instance.entries.lastWhere(
      (e) => e.event == LogEvent.usb && e.values['stage'] == stage,
    );
    for (final (reason, ok) in [
      ('session_ended', true),
      ('usb_policy_changed', true),
      ('authorization_revoked', true),
      ('usb_rate_limit', false),
    ]) {
      final (session, socket, backend) = await started();
      await shared(session, socket, backend);
      socket.emit({'type': 'usb.error', 'reason': reason});
      await settle();
      expect(session.closed, isTrue);
      expect(last(5).code, reason);
      expect(last(5).values['ok'], ok, reason: reason);
    }
    var (session, socket, backend) = await started();
    await shared(session, socket, backend);
    await socket.incoming.close();
    await settle();
    expect(last(5).code, 'usb_transport_closed');
    expect(last(5).values['ok'], isFalse);
    for (final (reason, ok) in [
      ('usb_policy_changed', true),
      ('usb_user_stopped', true),
      ('usb_vm_detached', false),
    ]) {
      (session, socket, backend) = await started();
      await shared(session, socket, backend);
      socket.emit({'type': 'usb.detach', 'device': 1, 'reason': reason});
      await settle();
      expect(last(3).code, reason);
      expect(last(3).values['ok'], ok, reason: reason);
      await session.close();
    }
    // The user's own close stays an ok summary without a code.
    expect(last(5).values['ok'], isTrue);
    expect(last(5).code, isNull);
  });

  test('Limits: four at a time, a busy device is not offered, and ids are not reused', () async {
    final devices = [
      for (var i = 0; i < 6; i++) stick(id: 'd$i'),
      stick(id: 'busy', busy: true),
    ];
    final (session, socket, backend) = await started(devices: devices);
    for (var i = 0; i < 4; i++) {
      expect(session.share(session.entries[i]), isNull);
    }
    await settle(); // a drive is checked for open files before it is offered
    expect(session.share(session.entries[4]), 'usb_limit_reached');
    expect(session.share(session.entries[6]), 'usb_device_busy');
    expect(socket.texts.where((m) => m['type'] == 'usb.attach').length, 4);
    // A refusal frees the slot but not the number.
    socket.emit({
      'type': 'usb.rejected',
      'device': 1,
      'reason': 'usb_attach_failed',
    });
    await settle();
    expect(session.share(session.entries[4]), isNull);
    await settle();
    final numbers = socket.texts
        .where((m) => m['type'] == 'usb.attach')
        .map((m) => m['device'])
        .toList();
    expect(numbers, [1, 2, 3, 4, 5]);
    expect(backend.captureCalls, 0);
    await session.close();
  });

  test('The capture driver is installed only when asked, and the outcome is a fixed code', () async {
    final (session, _, backend) = await started();
    backend.driver = UsbDriverState.missing;
    await session.refresh();
    expect(session.driver, UsbDriverState.missing);
    expect(session.entries, isEmpty);
    expect(
      backend.installed,
      isFalse,
      reason: 'nothing is installed on its own',
    );
    backend.installFailure = const UsbBackendException(
      'usbdk_signature_invalid',
    );
    expect(await session.installDriver(), 'usbdk_signature_invalid');
    expect(backend.installed, isFalse);
    backend.installFailure = null;
    expect(await session.installDriver(), isNull);
    expect(backend.installed, isTrue);
    expect(session.driver, UsbDriverState.ready);
    expect(session.entries.length, 1);
    await session.close();
  });

  // A stick shared in one window must not fail in another window with
  // "내 PC에서 장치를 가져오지 못했어요".
  test('A device another window holds is not offered; it moves here only after that window let it go and Windows took it back', () async {
    final (session, socket, backend) = await started(
      devices: [stick(otherWindow: 'Code2')],
    );
    final entry = session.entries.single;
    expect(entry.device.otherWindow, 'Code2');
    expect(session.share(entry), usbDeviceRedirected);
    var held = true;
    backend.holdsElsewhere = () => held;
    backend.devices = [stick()]; // listed as this PC's once it is let go
    final moving = session.moveHere(entry);
    await settle();
    expect(entry.moving, isTrue);
    expect(backend.handOverAsks, ['stick']);
    expect(backend.holderChecks, greaterThan(0));
    expect(
      backend.returnedDevices,
      isEmpty,
      reason: 'not before the other window let it go',
    );
    expect(socket.texts.where((m) => m['type'] == 'usb.attach'), isEmpty);
    held = false;
    expect(await moving, isNull);
    expect(entry.moving, isFalse);
    expect(backend.returnedDevices, ['stick'], reason: 'Windows took it back');
    final attach = await socket.text('usb.attach');
    socket.emit({'type': 'usb.attached', 'device': attach['device']});
    await settle();
    expect(backend.captureCalls, 1);
    expect(entry.state, UsbEntryState.shared);
    await session.close();
  });

  test('Moving is bounded: a window that keeps the device, or a device Windows did not take back, is said and nothing is offered', () async {
    final (session, socket, backend) = await started(
      devices: [stick(otherWindow: 'Code2')],
      moveWait: const Duration(milliseconds: 300),
    );
    final entry = session.entries.single;
    backend.holdsElsewhere = () => true;
    expect(await session.moveHere(entry), usbMoveTimeout);
    expect(entry.moving, isFalse);
    expect(entry.state, UsbEntryState.available);
    expect(backend.returnedDevices, isEmpty);
    expect(usbMessage(usbMoveTimeout), contains('돌려받지 못했어요'));
    final record = ProductLog.instance.entries.lastWhere(
      (e) => e.event == LogEvent.usb && e.values['stage'] == 13,
    );
    expect(record.code, 'usb_device_busy', reason: 'a code the broker keeps');
    backend.holdsElsewhere = () => false;
    backend.returnedState = 2;
    expect(await session.moveHere(entry), usbNotReturned);
    expect(entry.notReturned, isTrue);
    expect(socket.texts.where((m) => m['type'] == 'usb.attach'), isEmpty);
    expect(backend.captureCalls, 0);
    await session.close();
  });

  test('The window that has the device gives it back through its own session when another window asks, and says where it went', () async {
    final (session, socket, backend) = await started();
    final (entry, capture) = await shared(session, socket, backend);
    backend.handOverController.add((id: 'unknown', window: 'WorkPC'));
    await settle();
    expect(session.moves, 0);
    expect(capture.released, isFalse);
    backend.handOverController.add((id: 'stick', window: 'WorkPC'));
    final detach = await socket.text('usb.detach');
    expect(detach['reason'], 'usb_user_stopped', reason: 'its VM is told');
    await settle();
    expect(capture.released, isTrue);
    expect(session.moves, 1);
    expect(session.lastMovedTo, 'WorkPC');
    expect(entry.state, UsbEntryState.available);
    expect(
      backend.returnedDevices,
      isEmpty,
      reason: 'the window taking it checks Windows took it back',
    );
    await session.close();
  });

  test('UsbDk refusing a device it already redirects has its own message; the diagnostics keep a broker code and the Win32 error', () async {
    final (session, socket, backend) = await started();
    backend.captureFailure = const UsbBackendException(
      usbDeviceRedirected,
      error: 183,
    );
    final entry = session.entries.single;
    expect(session.share(entry), isNull);
    final attach = await socket.text('usb.attach');
    socket.emit({'type': 'usb.attached', 'device': attach['device']});
    await settle();
    expect(entry.state, UsbEntryState.failed);
    expect(entry.code, usbDeviceRedirected);
    expect(usbMessage(entry.code), isNot(usbMessage('usb_capture_failed')));
    final capture = ProductLog.instance.entries.lastWhere(
      (e) => e.event == LogEvent.usb && e.values['stage'] == 2,
    );
    expect(capture.code, 'usb_device_busy');
    expect(capture.values['error'], 183);
    final summary = ProductLog.instance.entries.lastWhere(
      (e) => e.event == LogEvent.usb && e.values['stage'] == 9,
    );
    expect(summary.code, 'usb_device_busy');
    await session.close();
  });

  test('The log keeps counts and fixed codes only', () async {
    final records = <ProductLogEntry>[];
    final log = ProductLog(recordSink: records.add);
    expect(log, isNotNull);
    final entry = ProductLogEntry.safeValues({
      'usbDevices': 1,
      'usbUrbs': 5,
      'usbKib': 7,
      'usbClass': 8,
      'vendor': 4660,
      'name': 'SanDisk',
      'serial': 'ABC',
    });
    expect(entry, {'usbDevices': 1, 'usbUrbs': 5, 'usbKib': 7, 'usbClass': 8});
    for (final code in [
      'usb_class_denied',
      'usb_hid_boot_denied',
      'usb_isochronous_denied',
      'usb_descriptor_invalid',
      'usb_class_not_granted',
      'usb_vhci_unavailable',
      'usb_limit_reached',
      'usb_attach_failed',
      'usb_device_removed',
      'usb_vm_detached',
      'usb_policy_changed',
      'usb_device_busy',
      'usb_capture_failed',
      'usb_device_error',
      'usbdk_missing',
      'usbdk_install_failed',
      'usbdk_signature_invalid',
    ]) {
      expect(usbMessage(code), isNotEmpty);
    }
  });
}
