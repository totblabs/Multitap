import 'dart:async';
import 'dart:ui' as ui;

import 'package:flutter/foundation.dart';
import 'package:flutter/material.dart';
import 'package:flutter/services.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:multitap_client/data/console_keyboard.dart';
import 'package:multitap_client/data/emergency_console_session.dart';
import 'package:multitap_client/data/session_windows.dart';
import 'package:multitap_client/ui/app_theme.dart';
import 'package:multitap_client/ui/emergency_console_view.dart';
import 'package:multitap_client/ui/session_toolbar.dart';

import 'console_rfb_test.dart' as wire;
import 'emergency_console_session_test.dart'
    show FakeConsoleTransport, consoleOrigin, testGrant;

class _Keyboard extends ConsoleKeyboardCapture {
  @override
  bool captured = false;
  @override
  bool get supported => true;
  int captures = 0;
  @override
  Future<bool> acquire(int generation) async {
    onReset?.call();
    return true;
  }

  @override
  Future<bool> capture(bool enabled) async {
    captures++;
    captured = enabled;
    notifyListeners();
    return enabled;
  }

  @override
  Future<void> release() async {
    captured = false;
    onReset?.call();
  }
}

Future<ui.Image> _image(int width, int height) async {
  final recorder = ui.PictureRecorder();
  Canvas(recorder).drawRect(
    Rect.fromLTWH(0, 0, width.toDouble(), height.toDouble()),
    Paint()..color = Colors.grey,
  );
  final picture = recorder.endRecording();
  try {
    return await picture.toImage(width, height);
  } finally {
    picture.dispose();
  }
}

List<Uint8List> _input(FakeConsoleTransport transport) => transport.sent
    .whereType<Uint8List>()
    .where(
      (bytes) =>
          bytes.isNotEmpty &&
          (bytes[0] == 4 || bytes[0] == 5 || bytes[0] == 255),
    )
    .toList();

void _rawFrame(FakeConsoleTransport transport, int width, int height) {
  final frame = wire.rfbRect(
    0,
    0,
    width,
    height,
    0,
    List<int>.filled(width * height * 4, 0),
  );
  for (var at = 0; at < frame.length; at += 256 * 1024) {
    final end = at + 256 * 1024 < frame.length ? at + 256 * 1024 : frame.length;
    transport.incoming.add(Uint8List.sublistView(frame, at, end));
  }
}

Future<EmergencyConsoleSession> _mount(
  WidgetTester tester,
  FakeConsoleTransport transport, {
  required Future<ui.Image> Function(Uint8List, int, int) decode,
  ConsoleKeyboardCapture? keyboard,
  bool qemuKeyboard = false,
}) async {
  final session = EmergencyConsoleSession(
    origin: consoleOrigin,
    token: 'synthetic',
    acquire: () async => testGrant(),
    release: (_) async {},
    transportFactory: (_, _) async => transport,
  );
  await tester.pumpWidget(
    MaterialApp(
      theme: buildAppTheme(brightness: Brightness.dark),
      home: EmergencyConsoleView(
        name: 'Memory console',
        create: () => session,
        windowActions: SessionWindowActions(closeWindow: () async {}),
        keyboard: keyboard ?? NoConsoleKeyboardCapture(),
        decodeRgba: decode,
      ),
    ),
  );
  await tester.pump();
  await tester.pump();
  expect(
    transport.sent,
    isNotEmpty,
    reason: 'memory session authenticated before feeding protocol',
  );
  transport.ready(keyboardProtocol: qemuKeyboard ? 'qemu-keyboard-v1' : null);
  transport.incoming.add(wire.initRfb(width: 640, height: 480));
  if (qemuKeyboard) {
    transport.incoming.add(wire.rfbRect(0, 0, 640, 480, -258, []));
    transport.incoming.add(wire.rfbRect(0, 0, 1, 1, -261, [0]));
  }
  _rawFrame(transport, 640, 480);
  await tester.pump();
  return session;
}

SessionToolbarMenu _command(WidgetTester tester, String key) => tester
    .widget<SessionToolbar>(find.byType(SessionToolbar))
    .actions
    .whereType<SessionToolbarMenu>()
    .firstWhere((item) => item.key == Key(key));

void main() {
  for (final size in [const Size(320, 240), const Size(640, 480)]) {
    testWidgets(
      'late old-size decode cannot paint or capture after DesktopSize to $size',
      (tester) async {
        tester.view.physicalSize = const Size(1440, 1080);
        tester.view.devicePixelRatio = 1;
        addTearDown(tester.view.resetPhysicalSize);
        addTearDown(tester.view.resetDevicePixelRatio);
        final oldImage = (await tester.runAsync(() => _image(640, 480)))!;
        final nextImage = (await tester.runAsync(
          () => _image(size.width.toInt(), size.height.toInt()),
        ))!;
        final oldDecode = Completer<ui.Image>();
        final nextDecode = Completer<ui.Image>();
        final decodes = <Size>[];
        final transport = FakeConsoleTransport(), keyboard = _Keyboard();
        final session = await _mount(
          tester,
          transport,
          keyboard: keyboard,
          decode: (_, width, height) {
            decodes.add(Size(width.toDouble(), height.toDouble()));
            return decodes.length == 1 ? oldDecode.future : nextDecode.future;
          },
        );
        expect(decodes, [const Size(640, 480)]);
        transport.incoming.add(
          wire.rfbRect(0, 0, size.width.toInt(), size.height.toInt(), -223, []),
        );
        _rawFrame(transport, size.width.toInt(), size.height.toInt());
        expect(session.rfb.frameValid, isTrue);
        oldDecode.complete(oldImage);
        await tester.pump();
        await tester.pump();
        expect(decodes, [const Size(640, 480), size]);
        expect(find.byKey(const Key('console-picture')), findsNothing);
        expect(keyboard.captures, 0);
        nextDecode.complete(nextImage);
        await tester.pump();
        await tester.pump();
        await tester.pump();
        final picture = find.byKey(const Key('console-picture'));
        expect(picture, findsOneWidget);
        final displayed = tester.widget<RawImage>(
          find.descendant(of: picture, matching: find.byType(RawImage)).first,
        );
        expect(identical(displayed.image, nextImage), isTrue);
        await tester.tap(picture);
        await tester.pump();
        expect(keyboard.captured, isTrue);
        await tester.pumpWidget(const SizedBox.shrink());
        await tester.pump();
        expect(tester.takeException(), isNull);
      },
    );
  }

  testWidgets(
    'DesktopSize releases tracked QEMU modifiers and drag once before any new image',
    (tester) async {
      tester.view.physicalSize = const Size(1440, 1080);
      tester.view.devicePixelRatio = 1;
      addTearDown(tester.view.resetPhysicalSize);
      addTearDown(tester.view.resetDevicePixelRatio);
      final image = (await tester.runAsync(() => _image(640, 480)))!;
      final transport = FakeConsoleTransport(), keyboard = _Keyboard();
      final session = await _mount(
        tester,
        transport,
        keyboard: keyboard,
        qemuKeyboard: true,
        decode: (_, _, _) async => image,
      );
      await tester.pump();
      await tester.pump();
      final picture = find.byKey(const Key('console-picture'));
      await tester.tap(picture);
      await tester.pump();
      _command(tester, 'console-secure-attention').onPressed!();
      final attention = _input(transport).where((b) => b[0] == 4).toList();
      expect(
        attention.map((b) => (b[1], ByteData.sublistView(b).getUint32(4))),
        [
          (1, 0xffe3),
          (1, 0xffe9),
          (1, 0xffff),
          (0, 0xffff),
          (0, 0xffe9),
          (0, 0xffe3),
        ],
      );
      await tester.pump();
      final drag = await tester.startGesture(tester.getCenter(picture));
      await tester.pump();
      for (final key in [
        const ConsoleKeyStroke(84, 0xffe1, true, 0x2a),
        const ConsoleKeyStroke(58, 0xffe4, true, 0x9d),
        const ConsoleKeyStroke(112, 0xffe9, true, 0x38),
      ]) {
        keyboard.onPhysicalKey?.call(key);
      }
      transport.sent.clear();
      transport.incoming.add(wire.rfbRect(0, 0, 320, 240, -223, []));
      expect(session.connected, isFalse);
      expect(session.closed, isFalse);
      await tester.pump();
      final released = _input(transport);
      final keys = released.where((b) => b[0] == 255).toList();
      expect(keys.map((b) => ByteData.sublistView(b).getUint16(2)), [0, 0, 0]);
      expect(keys.map((b) => ByteData.sublistView(b).getUint32(8)), [
        0x2a,
        0x9d,
        0x38,
      ]);
      expect(released.where((b) => b[0] == 5).map((b) => b[1]), [0]);
      final releaseCount = released.length;
      keyboard.onPhysicalKey?.call(
        const ConsoleKeyStroke(60, 0x61, true, 0x1e),
      );
      keyboard.onPhysicalKey?.call(
        const ConsoleKeyStroke(84, 0xffe1, false, 0x2a),
      );
      tester.binding.handleAppLifecycleStateChanged(AppLifecycleState.inactive);
      await drag.up();
      await tester.pump();
      expect(_input(transport).length, releaseCount);
      tester.binding.handleAppLifecycleStateChanged(AppLifecycleState.resumed);
      await tester.pumpWidget(const SizedBox.shrink());
      await tester.pump();
      expect(tester.takeException(), isNull);
    },
    variant: TargetPlatformVariant.only(TargetPlatform.windows),
  );

  testWidgets(
    'negotiated physical input still waits for paint/focus and releases on focus loss',
    (tester) async {
      tester.view.physicalSize = const Size(1440, 1080);
      tester.view.devicePixelRatio = 1;
      addTearDown(tester.view.resetPhysicalSize);
      addTearDown(tester.view.resetDevicePixelRatio);
      final image = (await tester.runAsync(() => _image(640, 480)))!;
      final pending = Completer<ui.Image>();
      final keyboard = _Keyboard(), transport = FakeConsoleTransport();
      final session = await _mount(
        tester,
        transport,
        keyboard: keyboard,
        qemuKeyboard: true,
        decode: (_, _, _) => pending.future,
      );
      expect(session.rfb.qemuKeyboardReady, isTrue);
      keyboard.onPhysicalKey?.call(
        const ConsoleKeyStroke(60, 0x61, true, 0x1e),
      );
      expect(_input(transport), isEmpty);
      expect(keyboard.captures, 0);
      pending.complete(image);
      await tester.pump();
      await tester.pump();
      await tester.tap(find.byKey(const Key('console-picture')));
      await tester.pump();
      for (final key in [
        const ConsoleKeyStroke(84, 0xffe1, true, 0x2a),
        const ConsoleKeyStroke(60, 0x61, true, 0x1e),
        const ConsoleKeyStroke(484, 0xff31, true, 0x72),
      ]) {
        keyboard.onPhysicalKey?.call(key);
      }
      var extended = _input(transport).where((b) => b[0] == 255).toList();
      expect(extended.length, 3);
      expect(extended.map((b) => ByteData.sublistView(b).getUint16(2)), [
        1,
        1,
        1,
      ]);
      tester.binding.handleAppLifecycleStateChanged(AppLifecycleState.inactive);
      await tester.pump();
      extended = _input(transport).where((b) => b[0] == 255).toList();
      expect(extended.length, 6);
      expect(
        extended.skip(3).map((b) => ByteData.sublistView(b).getUint16(2)),
        [0, 0, 0],
      );
      keyboard.onPhysicalKey?.call(
        const ConsoleKeyStroke(60, 0x61, true, 0x1e),
      );
      expect(_input(transport).where((b) => b[0] == 255).length, 6);
      tester.binding.handleAppLifecycleStateChanged(AppLifecycleState.resumed);
      await tester.pumpWidget(const SizedBox.shrink());
      await tester.pump();
      expect(tester.takeException(), isNull);
    },
    variant: TargetPlatformVariant.only(TargetPlatform.windows),
  );

  for (final dpr in [1.0, 1.5, 2.0]) {
    test(
      'fit/original use physical guest pixels at DPR$dpr without implicit enlargement',
      () {
        const pixels = Size(640, 480);
        final viewport = Size(1440 / dpr, 1080 / dpr);
        expect(consolePictureScale(viewport, pixels, dpr) * dpr, 1);
        expect(
          consolePictureScale(viewport, pixels, dpr, original: true) * dpr,
          1,
        );
        expect(
          consolePictureScale(const Size(320, 240), pixels, dpr),
          lessThanOrEqualTo(1 / dpr),
        );
      },
    );
    testWidgets(
      'actual picture is device-pixel aligned with the same inverse pointer at DPR$dpr',
      (tester) async {
        tester.view.devicePixelRatio = dpr;
        tester.view.physicalSize = const Size(1440, 1080);
        addTearDown(tester.view.resetDevicePixelRatio);
        addTearDown(tester.view.resetPhysicalSize);
        final image = (await tester.runAsync(() => _image(640, 480)))!;
        final transport = FakeConsoleTransport();
        await _mount(tester, transport, decode: (_, _, _) async => image);
        await tester.pump();
        await tester.pump();
        final picture = find.byKey(const Key('console-picture'));
        final rect = tester.getRect(picture);
        expect(rect.width * dpr, closeTo(640, 0.0001));
        expect(rect.height * dpr, closeTo(480, 0.0001));
        expect(
          rect.left * dpr,
          closeTo((rect.left * dpr).roundToDouble(), 0.0001),
        );
        expect(
          rect.top * dpr,
          closeTo((rect.top * dpr).roundToDouble(), 0.0001),
        );
        expect(
          tester
              .widget<RawImage>(
                find
                    .descendant(of: picture, matching: find.byType(RawImage))
                    .first,
              )
              .filterQuality,
          FilterQuality.none,
        );
        await tester.tapAt(rect.topLeft + Offset(123.5 / dpr, 71.5 / dpr));
        await tester.pump();
        final pointers = _input(transport)
            .where((bytes) => bytes[0] == 5)
            .toList();
        expect(pointers, isNotEmpty);
        final point = ByteData.sublistView(pointers.last);
        expect(point.getUint16(2), 123);
        expect(point.getUint16(4), 71);
        expect(pointers.last[1], 0);
        await tester.pumpWidget(const SizedBox.shrink());
        await tester.pump();
        expect(tester.takeException(), isNull);
      },
    );
  }

  testWidgets(
    'shrinking samples appropriately; explicit original mode stays device-pixel sized',
    (tester) async {
      tester.view.devicePixelRatio = 1;
      tester.view.physicalSize = const Size(600, 360);
      addTearDown(tester.view.resetDevicePixelRatio);
      addTearDown(tester.view.resetPhysicalSize);
      final image = (await tester.runAsync(() => _image(640, 480)))!;
      await _mount(
        tester,
        FakeConsoleTransport(),
        decode: (_, _, _) async => image,
      );
      await tester.pump();
      await tester.pump();
      final picture = find.byKey(const Key('console-picture'));
      expect(tester.getSize(picture), const Size(480, 360));
      expect(
        tester
            .widget<RawImage>(
              find
                  .descendant(of: picture, matching: find.byType(RawImage))
                  .first,
            )
            .filterQuality,
        FilterQuality.medium,
      );
      _command(tester, 'console-fit').onPressed!();
      await tester.pump();
      expect(tester.getSize(picture), const Size(640, 480));
      await tester.pumpWidget(const SizedBox.shrink());
      await tester.pump();
      expect(tester.takeException(), isNull);
    },
  );

  testWidgets(
    'broker/frame readiness alone cannot enable keys or attention before the first image paint',
    (tester) async {
      tester.view.physicalSize = const Size(1440, 1080);
      tester.view.devicePixelRatio = 1;
      addTearDown(tester.view.resetPhysicalSize);
      addTearDown(tester.view.resetDevicePixelRatio);
      final image = (await tester.runAsync(() => _image(640, 480)))!;
      final pending = Completer<ui.Image>();
      final keyboard = _Keyboard(), transport = FakeConsoleTransport();
      final session = await _mount(
        tester,
        transport,
        keyboard: keyboard,
        decode: (_, _, _) => pending.future,
      );
      expect(session.connected, isTrue);
      expect(find.byKey(const Key('console-picture')), findsNothing);
      expect(_command(tester, 'console-secure-attention').onPressed, isNull);
      keyboard.onKey?.call(0xff31, true);
      keyboard.onKey?.call(0xff31, false);
      expect(_input(transport), isEmpty);
      expect(keyboard.captures, 0);
      pending.complete(image);
      await tester.pump();
      await tester.pump();
      expect(_command(tester, 'console-secure-attention').onPressed, isNotNull);
      final gesture = await tester.startGesture(
        tester.getCenter(find.byKey(const Key('console-picture'))),
      );
      await tester.pump();
      expect(
        _input(transport)
            .where((bytes) => bytes[0] == 5)
            .map((bytes) => bytes[1])
            .toList(),
        [1],
        reason: 'keyboard claim reset must not release a held pointer edge',
      );
      await gesture.up();
      await tester.pump();
      keyboard.onKey?.call(0xff31, true);
      keyboard.onKey?.call(0xff31, false);
      final count = _input(transport).where((bytes) => bytes[0] == 4).length;
      expect(count, 2);
      await tester.sendKeyEvent(LogicalKeyboardKey.keyA);
      await tester.pump();
      expect(
        _input(transport).where((bytes) => bytes[0] == 4).length,
        count,
        reason: 'native owner is the sole producer',
      );
      tester.binding.handleAppLifecycleStateChanged(AppLifecycleState.inactive);
      await tester.pump();
      keyboard.onKey?.call(0xffeb, true);
      expect(_input(transport).where((bytes) => bytes[0] == 4).length, count);
      tester.binding.handleAppLifecycleStateChanged(AppLifecycleState.resumed);
      await tester.pumpWidget(const SizedBox.shrink());
      await tester.pump();
      expect(tester.takeException(), isNull);
    },
  );

  testWidgets(
    'a held image completion after view disposal cannot paint or enable input',
    (tester) async {
      final image = (await tester.runAsync(() => _image(640, 480)))!;
      final pending = Completer<ui.Image>();
      final transport = FakeConsoleTransport(), keyboard = _Keyboard();
      await _mount(
        tester,
        transport,
        keyboard: keyboard,
        decode: (_, _, _) => pending.future,
      );
      await tester.pumpWidget(const SizedBox.shrink());
      pending.complete(image);
      await tester.pump();
      await tester.pump();
      expect(find.byKey(const Key('console-picture')), findsNothing);
      expect(keyboard.captures, 0);
      expect(tester.takeException(), isNull);
    },
  );
}
