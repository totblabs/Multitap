// 마우스 잠금 (formerly 게임 마우스) over USB on the Dart side: the virtual mouse is offered through
// the session's usb-v1 channel exactly while the option is on and the VM can
// take USB, with the descriptors the Host and the broker know (the shared
// vector); its frames go to the runner and the runner's answers to the relay;
// an older Host, a refusal, a detach or the channel's end leave the injected
// path. Fake relay socket and fake runner: no device, VM or real input.
import 'dart:async';
import 'dart:convert';
import 'dart:typed_data';

import 'package:flutter_test/flutter_test.dart';
import 'package:multitap_client/data/usb_game_mouse.dart';
import 'package:multitap_client/data/usb_redirection.dart';

import 'usb_redirection_test.dart'
    show FakeSocket, settle, started, vector, vectors;

const _vector =
    'multitap game mouse (report-protocol hid, made up by the client)';

class _Native implements UsbGameMouseNative {
  final calls = <String>[];
  final frames = <Uint8List>[];
  String answer = 'ok';
  Completer<String>? pendingFrame;
  void Function(Uint8List)? send;
  void Function()? confirmed;
  bool disposed = false;

  @override
  Future<void> attached(int device) async => calls.add('attached:$device');
  @override
  Future<void> detached() async => calls.add('detached');
  @override
  Future<String> frame(Uint8List bytes) async {
    frames.add(bytes);
    final pending = pendingFrame;
    if (pending != null) return pending.future;
    return answer;
  }

  @override
  void listen({
    required void Function(Uint8List frame) send,
    required void Function() confirmed,
  }) {
    this.send = send;
    this.confirmed = confirmed;
  }

  @override
  void dispose() => disposed = true;
}

UsbFrame _read(int device, int seq) => UsbFrame(
  type: usbFrameSubmit,
  device: device,
  endpoint: 0x81,
  transfer: 3,
  seq: seq,
  length: 7,
);

Future<(UsbSession, FakeSocket, UsbGameMouse, _Native)> _attached() async {
  final (usb, socket, _) = await started();
  final native = _Native();
  final mouse = UsbGameMouse(usb: usb, native: native)..enabled = true;
  final attach = await socket.text('usb.attach');
  socket.emit({'type': 'usb.attached', 'device': attach['device']});
  await settle();
  return (usb, socket, mouse, native);
}

void main() {
  test(
    'The descriptors are the shared vector the Host and the broker know',
    () {
      expect(gameMouseDescriptors, vector(_vector));
      final parsed = UsbDescriptors.parse(gameMouseDescriptors)!;
      final decision = usbDecide(parsed);
      expect(decision.allowed, isTrue, reason: 'report-protocol HID');
      expect(decision.requires, isEmpty, reason: 'no class permission');
      // The boot mouse the policy names stays denied.
      expect(
        usbDecide(UsbDescriptors.parse(vector('boot mouse'))!).allowed,
        isFalse,
      );
      expect((vectors()[_vector] as Map)['expect'], {
        'allowed': true,
        'requires': <Object>[],
        'classes': [3],
      });
    },
  );

  test('Offered only while the option is on and the VM can take USB', () async {
    final (usb, socket, _) = await started(hostReady: false);
    final native = _Native();
    final mouse = UsbGameMouse(usb: usb, native: native);
    await settle();
    expect(socket.texts.where((t) => t['type'] == 'usb.attach'), isEmpty);
    mouse.enabled = true;
    await settle();
    expect(
      socket.texts.where((t) => t['type'] == 'usb.attach'),
      isEmpty,
      reason: 'the VM controller is not ready',
    );
    socket.emit({'type': 'usb.status', 'state': 'ready'});
    final attach = await socket.text('usb.attach');
    expect(attach, {
      'type': 'usb.attach',
      'device': attach['device'],
      'descriptors': base64Encode(gameMouseDescriptors),
      'speed': 'full',
    });
    // The panel never lists it, and it is not counted as a shared device.
    expect(usb.sharedCount, 0);
    expect(usb.entries.every((entry) => entry.id != attach['device']), isTrue);
    expect(mouse.active, isFalse, reason: 'not attached yet');
    socket.emit({'type': 'usb.attached', 'device': attach['device']});
    await settle();
    expect(native.calls, ['attached:${attach['device']}']);
    mouse.dispose();
    await usb.close();
  });

  test(
    'Frames go to the runner as they came, its answers to the relay',
    () async {
      final (usb, socket, mouse, native) = await _attached();
      final id = int.parse(native.calls.single.split(':').last);
      final notices = <bool>[];
      mouse.addListener(() => notices.add(mouse.active));
      final read = _read(id, 41).encode();
      socket.incoming.add(read);
      await settle();
      expect(native.frames.single, read);
      expect(mouse.active, isFalse, reason: 'no read-ahead seen yet');
      native.confirmed!();
      expect(mouse.active, isTrue);
      expect(notices, [true]);
      final answer = UsbFrame(
        type: usbFrameReturn,
        device: id,
        endpoint: 0x81,
        transfer: 3,
        seq: 41,
        length: 7,
        payload: Uint8List.fromList([1, 2, 0, 3, 0, 0, 0]),
      ).encode();
      native.send!(answer);
      final sent = await socket.frame(usbFrameReturn, seq: 41);
      expect(sent.payload, [1, 2, 0, 3, 0, 0, 0]);
      expect(sent.device, id);
      // Another device's frames are not the runner's.
      socket.incoming.add(_read(id + 1, 42).encode());
      await settle();
      expect(native.frames, hasLength(1));
      mouse.dispose();
      await usb.close();
    },
  );

  test(
    'Turning the option off takes the device back; on again offers a new one',
    () async {
      final (usb, socket, mouse, native) = await _attached();
      final first = int.parse(native.calls.single.split(':').last);
      native.confirmed!();
      mouse.enabled = false;
      final detach = await socket.text('usb.detach');
      expect(detach, {
        'type': 'usb.detach',
        'device': first,
        'reason': 'usb_user_stopped',
      });
      expect(native.calls.last, 'detached');
      expect(mouse.active, isFalse);
      // A late frame for it is not the runner's any more.
      socket.incoming.add(_read(first, 7).encode());
      await settle();
      expect(native.frames, isEmpty);
      mouse.enabled = true;
      final again = await socket.text('usb.attach');
      expect(
        again['device'],
        isNot(first),
        reason: 'device numbers are never reused',
      );
      mouse.dispose();
      await usb.close();
    },
  );

  test('An older Host gives the device up for the session', () async {
    final (usb, socket, mouse, native) = await _attached();
    final id = int.parse(native.calls.single.split(':').last);
    native.answer = 'older';
    socket.incoming.add(
      UsbFrame(
        type: usbFrameSubmit,
        device: id,
        endpoint: 0x80,
        transfer: 0,
        seq: 1,
        length: 255,
        setup: Uint8List.fromList([0x80, 6, 2, 3, 9, 4, 255, 0]),
      ).encode(),
    );
    final detach = await socket.text('usb.detach');
    expect(detach['device'], id);
    expect(mouse.active, isFalse);
    expect(mouse.reason, 'usb_host_older');
    // Not offered again, even when the option is switched.
    mouse.enabled = false;
    mouse.enabled = true;
    await settle();
    expect(socket.texts.where((t) => t['type'] == 'usb.attach'), isEmpty);
    mouse.dispose();
    await usb.close();
  });

  for (final lateResult in ['older', 'invalid']) {
    test(
      'A late $lateResult frame result cannot detach a replacement mouse',
      () async {
        final (usb, socket, mouse, native) = await _attached();
        addTearDown(() async {
          mouse.dispose();
          await usb.close();
        });
        final first = int.parse(native.calls.single.split(':').last);
        final pending = Completer<String>();
        native.pendingFrame = pending;
        socket.incoming.add(_read(first, 51).encode());
        await settle();
        expect(native.frames, hasLength(1));

        mouse.enabled = false;
        expect((await socket.text('usb.detach'))['device'], first);
        mouse.enabled = true;
        final replacement = await socket.text('usb.attach');
        final second = replacement['device'] as int;
        expect(second, isNot(first));
        socket.emit({'type': 'usb.attached', 'device': second});
        await settle();
        native.confirmed!();
        expect(mouse.active, isTrue);
        expect(mouse.reason, isNull);
        var notices = 0;
        mouse.addListener(() => notices++);
        final callsBefore = List<String>.of(native.calls);

        pending.complete(lateResult);
        await settle();
        expect(
          mouse.active,
          isTrue,
          reason: 'the result belongs to the retired device',
        );
        expect(mouse.reason, isNull);
        expect(
          notices,
          0,
          reason: 'old results cannot change replacement UI state',
        );
        expect(native.calls, callsBefore);
        expect(socket.texts.where((t) => t['type'] == 'usb.detach'), isEmpty);

        // In particular, a stale "older" result must not poison this session's
        // ability to offer another mouse after a later intentional toggle.
        mouse.enabled = false;
        expect((await socket.text('usb.detach'))['device'], second);
        mouse.enabled = true;
        final third = await socket.text('usb.attach');
        expect(third['device'], isNot(second));
      },
    );
  }

  test('A current invalid frame result still retires its own mouse', () async {
    final (usb, socket, mouse, native) = await _attached();
    addTearDown(() async {
      mouse.dispose();
      await usb.close();
    });
    final id = int.parse(native.calls.single.split(':').last);
    final pending = Completer<String>();
    native.pendingFrame = pending;
    native.confirmed!();
    socket.incoming.add(_read(id, 52).encode());
    await settle();
    pending.complete('invalid');
    final detach = await socket.text('usb.detach');
    expect(detach['device'], id);
    expect(mouse.active, isFalse);
    expect(mouse.reason, 'usb_invalid_message');
    expect(native.calls.last, 'detached');
  });

  test('A refusal or a Host detach leaves the injected path until the option is switched', () async {
    final (usb, socket, _) = await started();
    final native = _Native();
    final mouse = UsbGameMouse(usb: usb, native: native)..enabled = true;
    final attach = await socket.text('usb.attach');
    socket.emit({
      'type': 'usb.rejected',
      'device': attach['device'],
      'reason': 'usb_limit_reached',
    });
    await settle();
    expect(mouse.reason, 'usb_limit_reached');
    mouse.enabled = true;
    await settle();
    expect(socket.texts.where((t) => t['type'] == 'usb.attach'), isEmpty);
    mouse.enabled = false;
    mouse.enabled = true;
    final retry = await socket.text('usb.attach');
    socket.emit({'type': 'usb.attached', 'device': retry['device']});
    await settle();
    native.confirmed!();
    expect(mouse.active, isTrue);
    socket.emit({
      'type': 'usb.detach',
      'device': retry['device'],
      'reason': 'usb_vm_detached',
    });
    await settle();
    expect(mouse.active, isFalse);
    expect(native.calls.last, 'detached');
    // The Host's own detach is not answered with another.
    expect(socket.texts.where((t) => t['type'] == 'usb.detach'), isEmpty);
    mouse.dispose();
    await usb.close();
  });

  test(
    'The channel ending ends the device; closing says so to the relay',
    () async {
      final (usb, socket, mouse, native) = await _attached();
      final id = int.parse(native.calls.single.split(':').last);
      native.confirmed!();
      await usb.close();
      expect(mouse.active, isFalse);
      expect(native.calls.last, 'detached');
      expect(
        socket.texts.where(
          (t) => t['type'] == 'usb.detach' && t['device'] == id,
        ),
        hasLength(1),
      );
      final (other, otherSocket, otherMouse, otherNative) = await _attached();
      otherNative.confirmed!();
      otherSocket.emit({'type': 'usb.error', 'reason': 'usb_policy_changed'});
      await settle();
      expect(otherMouse.active, isFalse);
      expect(otherMouse.reason, 'usb_policy_changed');
      mouse.dispose();
      otherMouse.dispose();
      await other.close();
    },
  );

  test('Disposing the view takes the device back once', () async {
    final (usb, socket, mouse, native) = await _attached();
    mouse.dispose();
    mouse.dispose();
    expect(socket.texts.where((t) => t['type'] == 'usb.detach'), hasLength(1));
    expect(native.disposed, isTrue);
    await usb.close();
  });
}
