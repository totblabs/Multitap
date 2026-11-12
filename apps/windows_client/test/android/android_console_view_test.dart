import 'dart:async';
import 'dart:convert';
import 'dart:io';
import 'dart:ui' as ui;

import 'package:flutter/gestures.dart';
import 'package:flutter/material.dart';
import 'package:flutter/rendering.dart';
import 'package:flutter/services.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:multitap_client/android/android_console_view.dart';
import 'package:multitap_client/android/android_session_keepalive.dart';
import 'package:multitap_client/android/android_toolbar.dart';
import 'package:multitap_client/android/android_tv.dart';
import 'package:multitap_client/android/android_application.dart';
import 'package:multitap_client/data/emergency_console_session.dart';
import 'package:multitap_client/ui/app_theme.dart';

import '../console_rfb_test.dart' as wire;
import '../emergency_console_session_test.dart' as fixture;

class Harness {
  final transport = fixture.FakeConsoleTransport();
  final released = <String>[];
  int claims = 0;
  late final session = EmergencyConsoleSession(
    origin: fixture.consoleOrigin,
    token: 'synthetic',
    acquire: () async {
      claims++;
      return fixture.testGrant();
    },
    release: (id) async {
      released.add(id);
    },
    transportFactory: (_, _) async => transport,
  );
  void init() {
    transport.ready();
    transport.incoming.add(wire.initRfb(width: 4, height: 2));
  }

  void frame() => transport.incoming.add(
    wire.rfbRect(0, 0, 4, 2, 0, [
      for (var n = 0; n < 8; n++) ...[30, 80, 170, 0],
    ]),
  );
  List<Uint8List> get input => transport.sent
      .whereType<Uint8List>()
      .where((packet) => packet[0] == 4 || packet[0] == 5)
      .toList();
}

Widget host(
  Harness h, {
  bool dark = false,
  bool tv = false,
  Future<EmergencyConsoleSession> Function()? reconnect,
  ConsoleImageDecoder? decoder,
}) {
  final view = decoder == null
      ? AndroidConsoleView(
          session: h.session,
          desktopName: '테스트 VM · 콘솔',
          isTelevision: tv,
          onReconnect: reconnect,
        )
      : AndroidConsoleView(
          session: h.session,
          desktopName: '테스트 VM · 콘솔',
          isTelevision: tv,
          onReconnect: reconnect,
          decodeImage: decoder,
        );
  return MaterialApp(
    theme: buildAppTheme(brightness: dark ? Brightness.dark : Brightness.light),
    home: AndroidTvScope(
      television: tv,
      child: RepaintBoundary(key: const Key('capture'), child: view),
    ),
  );
}

Future<void> paint(WidgetTester tester, Harness h) async {
  h.init();
  h.frame();
  await tester.runAsync(
    () => Future<void>.delayed(const Duration(milliseconds: 20)),
  );
  await tester.pumpAndSettle();
}

Future<ui.Image> decodePixels(Uint8List bytes, int width, int height) {
  final result = Completer<ui.Image>();
  ui.decodeImageFromPixels(
    bytes,
    width,
    height,
    ui.PixelFormat.rgba8888,
    result.complete,
  );
  return result.future;
}

void main() {
  TestWidgetsFlutterBinding.ensureInitialized();
  final keepaliveCalls = <MethodCall>[];
  setUpAll(() async {
    final fonts = FontLoader('Pretendard');
    for (final weight in ['Regular', 'Medium', 'SemiBold', 'Bold']) {
      fonts.addFont(rootBundle.load('assets/fonts/Pretendard-$weight.otf'));
    }
    await fonts.load();
    final icons = FontLoader('MaterialIcons')
      ..addFont(rootBundle.load('fonts/MaterialIcons-Regular.otf'));
    await icons.load();
  });
  setUp(() {
    keepaliveCalls.clear();
    TestDefaultBinaryMessengerBinding.instance.defaultBinaryMessenger
        .setMockMethodCallHandler(AndroidSessionKeepalive.channel, (
          call,
        ) async {
          keepaliveCalls.add(call);
          return call.method == 'start' ? 'started' : true;
        });
  });
  tearDown(() {
    TestDefaultBinaryMessengerBinding.instance.defaultBinaryMessenger
        .setMockMethodCallHandler(AndroidSessionKeepalive.channel, null);
  });

  testWidgets(
    'broker ready and server init never claim a displayed first frame',
    (tester) async {
      final h = Harness();
      await tester.pumpWidget(host(h));
      await tester.pump();
      h.init();
      await tester.pump();
      expect(find.byKey(const Key('console-frame')), findsNothing);
      expect(find.text('콘솔 화면을 기다리고 있어요'), findsOneWidget);
      expect(find.text('연결됨'), findsNothing);
      expect(h.session.connected, isFalse);
      await tester.pumpWidget(const SizedBox.shrink());
      await tester.pump();
      expect(h.session.closed, isTrue);
    },
  );

  // The console's size changes when a Host park ends or Windows signs in; the
  // tools (the keyboard and the menu among them) stay, as the PC's toolbar
  // does, and only their actions wait.
  testWidgets('the session tools stay while the console waits or changes size', (
    tester,
  ) async {
    final h = Harness();
    await tester.pumpWidget(host(h));
    await tester.pump();
    h.init();
    await tester.pump();
    expect(find.text('콘솔 화면을 기다리고 있어요'), findsOneWidget);
    expect(find.byType(AndroidSessionBar), findsOneWidget);
    h.frame();
    await tester.runAsync(
      () => Future<void>.delayed(const Duration(milliseconds: 20)),
    );
    await tester.pumpAndSettle();
    expect(h.session.connected, isTrue);
    expect(find.byType(AndroidSessionBar), findsOneWidget);
    expect(
      find.byKey(const Key('console-keyboard-floating')),
      findsOneWidget,
    );
    h.transport.incoming.add(wire.rfbRect(0, 0, 3, 1, -223, []));
    await tester.pump();
    expect(h.session.connected, isFalse);
    expect(find.byType(AndroidSessionBar), findsOneWidget);
    h.transport.incoming.add(
      wire.rfbRect(0, 0, 3, 1, 0, [
        for (var n = 0; n < 3; n++) ...[30, 80, 170, 0],
      ]),
    );
    await tester.runAsync(
      () => Future<void>.delayed(const Duration(milliseconds: 20)),
    );
    await tester.pumpAndSettle();
    expect(h.session.connected, isTrue);
    expect(find.byType(AndroidSessionBar), findsOneWidget);
    expect(
      find.byKey(const Key('console-keyboard-floating')),
      findsOneWidget,
    );
    await tester.pumpWidget(const SizedBox.shrink());
    await tester.pump();
  });

  testWidgets(
    'fit tap, drag, secondary click and wheel map into actual source bounds',
    (tester) async {
      final h = Harness();
      await tester.pumpWidget(host(h));
      await tester.pump();
      await paint(tester, h);
      final surface = find.byKey(const Key('console-input-surface'));
      final center = tester.getCenter(surface);
      h.transport.sent.clear();
      await tester.tapAt(center);
      await tester.pump();
      expect(h.input.where((p) => p[0] == 5).map((p) => p[1]), [1, 0]);
      final pointer = h.input.first;
      expect(ByteData.sublistView(pointer).getUint16(2), 2);
      expect(ByteData.sublistView(pointer).getUint16(4), 1);
      h.transport.sent.clear();
      final drag = await tester.startGesture(center);
      await drag.moveBy(const Offset(60, 0));
      await tester.pump();
      await drag.up();
      await tester.pump();
      expect(h.input.where((p) => p[0] == 5).last[1], 0);
      h.transport.sent.clear();
      final mouse = await tester.createGesture(
        kind: PointerDeviceKind.mouse,
        buttons: kSecondaryMouseButton,
      );
      await mouse.addPointer(location: center);
      await mouse.down(center);
      await mouse.up();
      expect(h.input.where((p) => p[0] == 5).any((p) => p[1] == 4), isTrue);
      h.transport.sent.clear();
      tester.binding.handlePointerEvent(
        PointerScrollEvent(
          position: center,
          scrollDelta: const Offset(0, 120),
          kind: PointerDeviceKind.mouse,
        ),
      );
      expect(h.input.where((p) => p[0] == 5).map((p) => p[1]), [16, 0]);
      await tester.pumpWidget(const SizedBox.shrink());
      await tester.pump();
    },
  );

  testWidgets(
    'background releases hardware keys and drag without reconnecting',
    (tester) async {
      final h = Harness();
      await tester.pumpWidget(host(h));
      await tester.pump();
      await paint(tester, h);
      await tester.tapAt(
        tester.getCenter(find.byKey(const Key('console-input-surface'))),
      );
      await tester.sendKeyDownEvent(LogicalKeyboardKey.controlLeft);
      final gesture = await tester.startGesture(
        const Offset(400, 300),
        kind: PointerDeviceKind.mouse,
      );
      h.transport.sent.clear();
      tester.binding.handleAppLifecycleStateChanged(AppLifecycleState.inactive);
      await tester.pump();
      expect(h.input.where((p) => p[0] == 4).any((p) => p[1] == 0), isTrue);
      expect(h.input.where((p) => p[0] == 5).last[1], 0);
      tester.binding.handleAppLifecycleStateChanged(AppLifecycleState.resumed);
      await tester.pump();
      h.transport.sent.clear();
      await gesture.moveBy(const Offset(20, 0));
      await gesture.up();
      expect(h.input.where((p) => p[0] == 5).every((p) => p[1] == 0), isTrue);
      await tester.sendKeyUpEvent(LogicalKeyboardKey.controlLeft);
      expect(h.claims, 1);
      expect(h.session.closed, isFalse);
      await tester.pumpWidget(const SizedBox.shrink());
      await tester.pump();
    },
  );

  testWidgets(
    'close cancellation preserves connection; approval retires exact attempt',
    (tester) async {
      final h = Harness();
      await tester.pumpWidget(host(h));
      await tester.pump();
      await paint(tester, h);
      await tester.binding.handlePopRoute();
      await tester.pumpAndSettle();
      expect(find.text('콘솔 연결을 끊을까요?'), findsOneWidget);
      await tester.tap(find.byKey(const Key('console-close-cancel')));
      await tester.pumpAndSettle();
      expect(h.session.closed, isFalse);
      await tester.binding.handlePopRoute();
      await tester.pumpAndSettle();
      await tester.tap(find.byKey(const Key('console-close-confirm')));
      await tester.pumpAndSettle();
      expect(h.session.closed, isTrue);
      expect(h.released, [fixture.grantId]);
      await tester.pumpWidget(const SizedBox.shrink());
      await tester.pump();
    },
  );

  testWidgets('permission end has no retry or automatic new grant', (
    tester,
  ) async {
    final h = Harness();
    var retries = 0;
    await tester.pumpWidget(
      host(
        h,
        reconnect: () async {
          retries++;
          return Harness().session;
        },
      ),
    );
    await tester.pump();
    await paint(tester, h);
    h.transport.incoming.add(
      jsonEncode({'type': 'console-ended', 'code': 'authorization_revoked'}),
    );
    await tester.pumpAndSettle();
    expect(find.byKey(const Key('console-retry')), findsNothing);
    expect(find.textContaining('Console 권한'), findsOneWidget);
    expect(retries, 0);
    expect(h.claims, 1);
    await tester.pumpWidget(const SizedBox.shrink());
    await tester.pump();
  });

  testWidgets('late decode after disposal is retired and never painted', (
    tester,
  ) async {
    final h = Harness(), gate = Completer<ui.Image>();
    await tester.pumpWidget(host(h, decoder: (_, _, _) => gate.future));
    await tester.pump();
    h.init();
    h.frame();
    await tester.pump();
    expect(find.byKey(const Key('console-frame')), findsNothing);
    final recorder = ui.PictureRecorder();
    ui.Canvas(recorder).drawColor(const Color(0xff112233), ui.BlendMode.src);
    final picture = recorder.endRecording();
    final image = await tester.runAsync(() => picture.toImage(4, 2));
    picture.dispose();
    await tester.pumpWidget(const SizedBox.shrink());
    await tester.pump();
    gate.complete(image!);
    await tester.pump();
    expect(image.debugDisposed, isTrue);
    expect(tester.takeException(), isNull);
  });

  testWidgets(
    'late reconnect result after close is disposed before any grant',
    (tester) async {
      final h = Harness(), next = Harness();
      final gate = Completer<EmergencyConsoleSession>();
      await tester.pumpWidget(host(h, reconnect: () => gate.future));
      await tester.pump();
      await paint(tester, h);
      h.transport.incoming.add(
        jsonEncode({
          'type': 'console-ended',
          'code': 'console_transport_closed',
        }),
      );
      await tester.pumpAndSettle();
      await tester.tap(find.byKey(const Key('console-retry')));
      await tester.pump();
      await tester.pumpWidget(const SizedBox.shrink());
      await tester.pump();
      gate.complete(next.session);
      await tester.pump();
      expect(next.session.closed, isTrue);
      expect(next.claims, 0);
      expect(tester.takeException(), isNull);
    },
  );

  testWidgets('explicit keyboard commits Unicode keysyms without clipboard', (
    tester,
  ) async {
    final h = Harness();
    await tester.pumpWidget(host(h));
    await tester.pump();
    await paint(tester, h);
    h.transport.sent.clear();
    await tester.tap(find.byKey(const Key('console-keyboard-floating')));
    await tester.pumpAndSettle();
    await tester.enterText(find.byKey(const Key('console-text-input')), '가A');
    await tester.tap(find.byKey(const Key('console-text-send')));
    await tester.pumpAndSettle();
    final keys = h.input.where((p) => p[0] == 4).toList();
    expect(keys.map((p) => ByteData.sublistView(p).getUint32(4)), [
      0x0100ac00,
      0x0100ac00,
      65,
      65,
    ]);
    expect(keys.map((p) => p[1]), [1, 0, 1, 0]);
    await tester.pumpWidget(const SizedBox.shrink());
    await tester.pump();
  });

  testWidgets(
    'TV directions and OK use pointer input; menu releases the held drag',
    (tester) async {
      final h = Harness();
      await tester.pumpWidget(host(h, tv: true));
      await tester.pump();
      await paint(tester, h);
      await tester.pump();
      h.transport.sent.clear();
      await tester.sendKeyDownEvent(LogicalKeyboardKey.arrowRight);
      await tester.pump(const Duration(milliseconds: 80));
      await tester.sendKeyUpEvent(LogicalKeyboardKey.arrowRight);
      await tester.sendKeyEvent(LogicalKeyboardKey.select);
      expect(h.input.where((p) => p[0] == 5).any((p) => p[1] == 1), isTrue);
      expect(h.input.where((p) => p[0] == 4), isEmpty);
      await tester.sendKeyEvent(LogicalKeyboardKey.contextMenu);
      await tester.pumpAndSettle();
      expect(find.text('콘솔 도구'), findsOneWidget);
      await tester.pumpWidget(const SizedBox.shrink());
      await tester.pump();
      expect(tester.takeException(), isNull);
    },
  );

  testWidgets(
    'actual cursor mask and explicit hidden cursor have no fallback arrow',
    (tester) async {
      final h = Harness();
      await tester.pumpWidget(host(h));
      await tester.pump();
      await paint(tester, h);
      expect(find.byKey(const Key('console-cursor')), findsNothing);
      h.transport.incoming.add(
        wire.rfbRect(0, 0, 1, 1, -239, [0, 0, 255, 0, 128]),
      );
      final mouse = await tester.createGesture(kind: PointerDeviceKind.mouse);
      await mouse.addPointer(location: const Offset(400, 300));
      // The cursor rectangle is decoded off the test clock: wait for it (up to
      // 2 s) instead of a fixed 20 ms, which a busy machine can miss.
      for (var i = 0;
          i < 100 && find.byKey(const Key('console-cursor')).evaluate().isEmpty;
          i++) {
        await tester.runAsync(
          () => Future<void>.delayed(const Duration(milliseconds: 20)),
        );
        await mouse.moveTo(Offset(410 + (i % 2).toDouble(), 300));
        await tester.pump();
      }
      expect(find.byKey(const Key('console-cursor')), findsOneWidget);
      h.transport.incoming.add(wire.rfbRect(0, 0, 0, 0, -239, []));
      await tester.pump();
      expect(find.byKey(const Key('console-cursor')), findsNothing);
      await tester.pumpWidget(const SizedBox.shrink());
      await tester.pump();
    },
  );

  testWidgets(
    'original size retains pixel coordinates and fit is not a resolution request',
    (tester) async {
      final h = Harness();
      await tester.pumpWidget(host(h));
      await tester.pump();
      await paint(tester, h);
      await tester.tap(find.byKey(const Key('expand-session-toolbar')));
      await tester.pumpAndSettle();
      await tester.tap(find.byTooltip('화면 맞춤 · 원본 크기'));
      await tester.pumpAndSettle();
      await tester.tap(find.text('원본 크기'));
      await tester.pumpAndSettle();
      final image = find.byKey(const Key('console-frame'));
      expect(tester.getSize(image), const Size(4, 2));
      h.transport.sent.clear();
      await tester.tapAt(tester.getTopLeft(image) + const Offset(.5, .5));
      await tester.pump();
      final pointer = h.input.firstWhere((p) => p[0] == 5);
      expect(ByteData.sublistView(pointer).getUint16(2), 0);
      expect(ByteData.sublistView(pointer).getUint16(4), 0);
      expect([h.session.rfb.width, h.session.rfb.height], [4, 2]);
      await tester.pumpWidget(const SizedBox.shrink());
      await tester.pump();
    },
  );

  testWidgets('the console menu has no caption: 연결 품질, 화면 (its group\'s only '
      'row), then its own 키보드 and 키 조합, 연결 끊기 last', (tester) async {
    final h = Harness();
    await tester.pumpWidget(host(h));
    await tester.pump();
    await paint(tester, h);
    await tester.tap(find.byKey(const Key('expand-session-toolbar')));
    await tester.pumpAndSettle();
    final menu = find.byKey(const Key('session-command-bar'));
    expect(
      [
        for (final caption in tester.widgetList<AndroidMenuCaption>(
          find.descendant(of: menu, matching: find.byType(AndroidMenuCaption)),
        ))
          caption.label,
      ],
      // A single row named like its group gets no caption over it.
      isEmpty,
    );
    expect(
      find.descendant(of: menu, matching: find.text('화면')),
      findsOneWidget,
    );
    // 연결 품질 is the card's first row. 화면 is in the shared vocabulary;
    // the console's own 키보드 and 키 조합 keep their order after it.
    expect(
      [
        for (final row in tester.widgetList<AndroidBarMenuRow>(
          find.descendant(of: menu, matching: find.byType(AndroidBarMenuRow)),
        ))
          row.item.label,
      ],
      ['연결 품질', '화면', '키보드', '키 조합', '연결 끊기'],
    );
    for (final gone in ['설정', '공유', '화면·영상', '내 기기 공유', '입력·도움말']) {
      expect(
        find.descendant(of: menu, matching: find.text(gone)),
        findsNothing,
        reason: gone,
      );
    }
    Rect row(String label) => tester.getRect(
      find.descendant(
        of: menu,
        matching: find.byWidgetPredicate(
          (widget) => widget is AndroidBarMenuRow && widget.item.label == label,
        ),
      ),
    );
    // A gap comes only with a caption: the rows follow each other at once.
    expect(row('화면').top, closeTo(row('연결 품질').bottom, .01));
    expect(row('키보드').top, closeTo(row('화면').bottom, .01));
    expect(row('키 조합').top, closeTo(row('키보드').bottom, .01));
    expect(row('연결 끊기').top, greaterThan(row('키 조합').bottom));
    await tester.pumpWidget(const SizedBox.shrink());
    await tester.pump();
  });

  testWidgets('old held mouse cannot begin a drag in a replacement session', (
    tester,
  ) async {
    final a = Harness(), b = Harness();
    await tester.pumpWidget(host(a));
    await tester.pump();
    await paint(tester, a);
    final mouse = await tester.startGesture(
      const Offset(400, 300),
      kind: PointerDeviceKind.mouse,
    );
    await tester.pumpWidget(host(b));
    await tester.pump();
    await paint(tester, b);
    b.transport.sent.clear();
    await mouse.moveBy(const Offset(30, 0));
    await mouse.up();
    await tester.pump();
    expect(b.input.where((p) => p[0] == 5).any((p) => p[1] != 0), isFalse);
    expect(a.session.closed, isTrue);
    expect(b.claims, 1);
    await tester.pumpWidget(const SizedBox.shrink());
    await tester.pump();
  });

  testWidgets('retry retires old attempt before starting a fresh grant', (
    tester,
  ) async {
    final a = Harness(), b = Harness();
    await tester.pumpWidget(
      host(
        a,
        reconnect: () async {
          expect(a.session.closed, isTrue);
          expect(a.released, [fixture.grantId]);
          return b.session;
        },
      ),
    );
    await tester.pump();
    await paint(tester, a);
    a.transport.incoming.add(
      jsonEncode({'type': 'console-ended', 'code': 'console_transport_closed'}),
    );
    await tester.pumpAndSettle();
    await tester.tap(find.byKey(const Key('console-retry')));
    await tester.pump();
    await paint(tester, b);
    expect(b.claims, 1);
    expect(b.session.connected, isTrue);
    expect(find.byKey(const Key('console-frame')), findsOneWidget);
    await tester.pumpWidget(const SizedBox.shrink());
    await tester.pump();
  });

  testWidgets('decode watchdog retires held input and disposes a late image', (
    tester,
  ) async {
    final h = Harness(), gate = Completer<ui.Image>();
    var calls = 0;
    await tester.pumpWidget(
      host(
        h,
        decoder: (bytes, w, height) =>
            ++calls == 1 ? decodePixels(bytes, w, height) : gate.future,
      ),
    );
    await tester.pump();
    await paint(tester, h);
    await tester.tapAt(const Offset(400, 300));
    await tester.sendKeyDownEvent(LogicalKeyboardKey.controlLeft);
    final mouse = await tester.startGesture(
      const Offset(400, 300),
      kind: PointerDeviceKind.mouse,
    );
    h.frame();
    await tester.pump();
    h.transport.sent.clear();
    await tester.pump(const Duration(seconds: 5));
    await tester.pumpAndSettle();
    expect(h.session.closed, isTrue);
    expect(h.released, [fixture.grantId]);
    expect(keepaliveCalls.any((call) => call.method == 'stop'), isTrue);
    expect(h.input.where((p) => p[0] == 4).any((p) => p[1] == 0), isTrue);
    expect(h.input.where((p) => p[0] == 5).any((p) => p[1] == 0), isTrue);
    expect(find.text('Console 화면을 받지 못했어요. 다시 연결하세요.'), findsOneWidget);
    final image = await tester.runAsync(
      () => decodePixels(Uint8List(32), 4, 2),
    );
    gate.complete(image!);
    await tester.pump();
    expect(image.debugDisposed, isTrue);
    await mouse.up();
    await tester.sendKeyUpEvent(LogicalKeyboardKey.controlLeft);
    await tester.pumpWidget(const SizedBox.shrink());
    await tester.pump();
    expect(tester.takeException(), isNull);
  });

  testWidgets('old decode watchdog cannot close a replacement session', (
    tester,
  ) async {
    final a = Harness(), b = Harness(), gate = Completer<ui.Image>();
    await tester.pumpWidget(host(a, decoder: (_, _, _) => gate.future));
    await tester.pump();
    a.init();
    a.frame();
    await tester.pump();
    await tester.pumpWidget(host(b));
    await tester.pump();
    await paint(tester, b);
    await tester.pump(const Duration(seconds: 6));
    expect(a.session.closed, isTrue);
    expect(b.session.closed, isFalse);
    expect(find.byKey(const Key('console-frame')), findsOneWidget);
    final image = await tester.runAsync(
      () => decodePixels(Uint8List(32), 4, 2),
    );
    gate.complete(image!);
    await tester.pump();
    expect(image.debugDisposed, isTrue);
    await tester.pumpWidget(const SizedBox.shrink());
    await tester.pump();
  });

  testWidgets(
    'phone and native TV text entry are always masked without suggestions',
    (tester) async {
      final nativeArgs = <Map<Object?, Object?>>[];
      TestDefaultBinaryMessengerBinding.instance.defaultBinaryMessenger
          .setMockMethodCallHandler(AndroidApplication.channel, (call) async {
            if (call.method == 'tvEditText') {
              nativeArgs.add(Map<Object?, Object?>.from(call.arguments as Map));
              return null;
            }
            return null;
          });
      addTearDown(
        () => TestDefaultBinaryMessengerBinding.instance.defaultBinaryMessenger
            .setMockMethodCallHandler(AndroidApplication.channel, null),
      );
      for (final tv in [false, true]) {
        final h = Harness();
        await tester.pumpWidget(host(h, tv: tv));
        await tester.pump();
        await paint(tester, h);
        await tester.tap(find.byKey(const Key('console-keyboard-floating')));
        await tester.pumpAndSettle();
        final field = tester.widget<TextField>(
          find.byKey(const Key('console-text-input')),
        );
        expect(field.obscureText, isTrue);
        expect(field.enableSuggestions, isFalse);
        expect(field.autocorrect, isFalse);
        expect(field.maxLines, 1);
        expect(field.maxLength, 1024);
        expect(field.readOnly, tv);
        expect(
          tester
              .widget<AndroidTvTextEntry>(find.byType(AndroidTvTextEntry))
              .obscureText,
          isTrue,
        );
        if (tv) {
          field.onTap!();
          await tester.pump(const Duration(milliseconds: 10));
          await tester.pumpAndSettle();
          expect(nativeArgs.single['type'], 'password');
          expect(nativeArgs.single['maxLength'], 1024);
        }
        await tester.pumpWidget(const SizedBox.shrink());
        await tester.pumpAndSettle();
      }
      expect(tester.takeException(), isNull);
    },
  );

  for (final size in [
    const Size(320, 720),
    const Size(390, 844),
    const Size(800, 390),
    const Size(960, 540),
    const Size(1280, 800),
  ]) {
    for (final dark in [false, true]) {
      for (final scale in [1.0, 1.5, if (size.width == 320) 2.0]) {
        final name =
            '${size.width.toInt()}x${size.height.toInt()}-${dark ? 'dark' : 'light'}-$scale';
        testWidgets('console render $name', (tester) async {
          tester.view.physicalSize = size;
          tester.view.devicePixelRatio = 1;
          tester.platformDispatcher.textScaleFactorTestValue = scale;
          addTearDown(tester.view.resetPhysicalSize);
          addTearDown(tester.view.resetDevicePixelRatio);
          addTearDown(tester.platformDispatcher.clearTextScaleFactorTestValue);
          final h = Harness();
          await tester.pumpWidget(host(h, dark: dark, tv: size.width == 960));
          await tester.pump();
          await paint(tester, h);
          expect(find.byKey(const Key('console-frame')), findsOneWidget);
          expect(find.text('콘솔 화면을 기다리고 있어요'), findsNothing);
          await tester.tap(find.byKey(const Key('expand-session-toolbar')));
          await tester.pumpAndSettle();
          expect(tester.takeException(), isNull);
          final output = Platform.environment['ANDROID_CONSOLE_CAPTURE_DIR'];
          if (output != null) {
            await tester.runAsync(() async {
              final boundary = tester.renderObject<RenderRepaintBoundary>(
                find.byKey(const Key('capture')),
              );
              final image = await boundary.toImage(pixelRatio: 1.5);
              final bytes = await image.toByteData(
                format: ui.ImageByteFormat.png,
              );
              final file = File('$output/console-$name.png');
              await file.parent.create(recursive: true);
              await file.writeAsBytes(bytes!.buffer.asUint8List());
              image.dispose();
            });
          }
          await tester.binding.handlePopRoute();
          await tester.pumpAndSettle();
          expect(find.byKey(const Key('console-close-cancel')), findsOneWidget);
          expect(tester.takeException(), isNull);
          await tester.tap(find.byKey(const Key('console-close-cancel')));
          await tester.pumpAndSettle();
          await tester.pumpWidget(const SizedBox.shrink());
          await tester.pump();
        });
      }
    }
  }
}
