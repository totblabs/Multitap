import 'dart:async';
import 'dart:convert';
import 'dart:io';
import 'dart:ui' as ui;

import 'package:flutter/material.dart';
import 'package:flutter/rendering.dart';
import 'package:flutter/services.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:multitap_client/android/android_remote.dart';
import 'package:multitap_client/android/android_session_keepalive.dart';
import 'package:multitap_client/android/android_session_ui.dart';
import 'package:multitap_client/android/android_store.dart';
import 'package:multitap_client/data/broker_api.dart';
import 'package:multitap_client/data/remote_session.dart';
import 'package:multitap_client/models/stream_options.dart';
import 'package:multitap_client/ui/app_theme.dart';
import 'package:multitap_client/ui/connection_wait_status.dart';

import '../remote_session_test.dart' show SessionSocket;
import 'android_session_contract_test.dart' show AndroidNativeFake;

bool? _captureShadows;

class _CleanupSession extends RemoteSession {
  _CleanupSession({
    required super.connection,
    required super.media,
    required super.token,
    required super.dialer,
    super.statsSink,
  });
  Completer<void>? cleanup;
  int cleanupCalls = 0;
  @override
  Future<void> close({bool notifyHost = true}) async {
    cleanupCalls++;
    await cleanup?.future;
    await super.close(notifyHost: notifyHost);
  }
}

class _Fixture {
  final boundary = GlobalKey();
  final requests = <StreamOptions>[];
  final saved = <AndroidSessionSettings>[];
  final savedQuality = <StreamOptions>[];
  final sessions = <_CleanupSession>[];
  late SessionSocket socket;
  Completer<void>? loadGate;
  Object? failure;
}

Future<_Fixture> _open(
  WidgetTester t, {
  Object? failure,
  Completer<void>? loadGate,
  double textScale = 1,
  Size size = const Size(360, 800),
  Brightness brightness = Brightness.dark,
}) async {
  if (Platform.environment['ANDROID_UNSLOP_CAPTURE_DIR'] != null) {
    _captureShadows ??= debugDisableShadows;
    debugDisableShadows = false;
  }
  t.view.physicalSize = size;
  t.view.devicePixelRatio = 1;
  addTearDown(t.view.reset);
  AndroidNativeFake().install();
  addTearDown(AndroidNativeFake.uninstall);
  final h = _Fixture()
    ..failure = failure
    ..loadGate = loadGate;
  await t.pumpWidget(
    RepaintBoundary(
      key: h.boundary,
      child: MaterialApp(
        theme: buildAppTheme(brightness: brightness),
        builder: (context, child) => MediaQuery(
          data: MediaQuery.of(context)
              .copyWith(textScaler: TextScaler.linear(textScale)),
          child: child!,
        ),
        home: AndroidRemoteView(
          name: '실제 연결 단계 테스트',
          token: 'fixture',
          options: const StreamOptions(fps: 30, bitrate: 37000000, audio: true),
          settings: const AndroidSessionSettings(helpSeen: true),
          onSettings: h.saved.add,
          onOptions: h.savedQuality.add,
          load: (options, _, _) async {
            h.requests.add(options);
            await h.loadGate?.future;
            if (h.failure != null) throw h.failure!;
            return BrokerConnection(
              sessionId: 'feedback',
              webSocketUrl: Uri.parse('wss://fixture.invalid/feedback'),
              options: options,
            );
          },
          sessionFactory: (connection, media, stats) {
            final session = _CleanupSession(
              connection: connection,
              media: media,
              token: 'fixture',
              statsSink: stats,
              dialer: (_, _) async => h.socket = SessionSocket(),
            );
            h.sessions.add(session);
            return session;
          },
          pictureBuilder: (_) => const ColoredBox(
            key: Key('qualified-picture'),
            color: Colors.black,
          ),
        ),
      ),
    ),
  );
  await _pump(t);
  return h;
}

Future<void> _pump(WidgetTester t) async {
  for (var i = 0; i < 6; i++) {
    await t.pump(const Duration(milliseconds: 20));
  }
}

Future<void> _tap(WidgetTester t, String key) async {
  final f = find.byKey(Key(key));
  await t.ensureVisible(f);
  await t.pump();
  expect(f.hitTestable(), findsOneWidget, reason: key);
  await t.tap(f);
  await _pump(t);
}

Future<void> _end(WidgetTester t) async {
  await t.pumpWidget(const SizedBox.shrink());
  await t.pump(const Duration(seconds: 2));
  expect(t.takeException(), isNull);
  if (_captureShadows case final previous?) {
    debugDisableShadows = previous;
    _captureShadows = null;
  }
}

Future<void> _capture(WidgetTester t, _Fixture h, String name) async {
  final directory = Platform.environment['ANDROID_UNSLOP_CAPTURE_DIR'];
  if (directory == null) return;
  await t.pump(const Duration(milliseconds: 350));
  final previous = debugDisableShadows;
  try {
    debugDisableShadows = false;
    final boundary =
        h.boundary.currentContext!.findRenderObject() as RenderRepaintBoundary;
    boundary.markNeedsPaint();
    await t.pump();
    await t.runAsync(() async {
      final image = await boundary.toImage(pixelRatio: 1);
      try {
        final bytes = await image.toByteData(format: ui.ImageByteFormat.png);
        await Directory(directory).create(recursive: true);
        await File('$directory/$name.png')
            .writeAsBytes(bytes!.buffer.asUint8List());
      } finally {
        image.dispose();
      }
    });
  } finally {
    debugDisableShadows = previous;
    h.boundary.currentContext?.findRenderObject()?.markNeedsPaint();
    await t.pump();
  }
}

void main() {
  TestWidgetsFlutterBinding.ensureInitialized();
  setUpAll(() async {
    final font = FontLoader('Pretendard');
    for (final weight in ['Regular', 'Medium', 'SemiBold', 'Bold']) {
      font.addFont(rootBundle.load('assets/fonts/Pretendard-$weight.otf'));
    }
    await font.load();
    await (FontLoader(
      'MaterialIcons',
    )..addFont(rootBundle.load('fonts/MaterialIcons-Regular.otf'))).load();
  });
  setUp(() {
    final messenger =
        TestDefaultBinaryMessengerBinding.instance.defaultBinaryMessenger;
    messenger.setMockMethodCallHandler(
      AndroidSessionKeepalive.channel,
      (call) async => call.method == 'start' ? true : null,
    );
    messenger.setMockMethodCallHandler(
      const MethodChannel('multitap/pen-input'),
      (_) async => null,
    );
  });
  tearDown(() {
    final messenger =
        TestDefaultBinaryMessengerBinding.instance.defaultBinaryMessenger;
    messenger.setMockMethodCallHandler(AndroidSessionKeepalive.channel, null);
    messenger.setMockMethodCallHandler(
      const MethodChannel('multitap/pen-input'),
      null,
    );
  });
  testWidgets(
    'observed request relay first-frame phases never declare success before a frame',
    (t) async {
      final gate = Completer<void>();
      final h = await _open(t, loadGate: gate);
      expect(find.text('연결 요청'), findsOneWidget);
      expect(find.byType(ConnectionWaitStatus), findsOneWidget);
      expect(find.text('경과 00:00'), findsOneWidget);
      expect(h.requests, hasLength(1));
      expect(find.byKey(const Key('qualified-picture')), findsNothing);
      gate.complete();
      await _pump(t);
      expect(find.text('서버 연결'), findsOneWidget);
      h.socket.incoming.add(
        jsonEncode({
          'type': 'stream.info',
          'codec': 'h264',
          'width': 360,
          'height': 800,
        }),
      );
      await _pump(t);
      expect(find.text('첫 화면 수신 대기'), findsOneWidget);
      expect(h.sessions.single.frameReady, isFalse);
      expect(find.byKey(const Key('qualified-picture')), findsNothing);
      h.socket.incoming.add(
        Uint8List.fromList([
          1,
          1,
          0,
          0,
          0,
          0,
          0,
          0,
          0,
          0,
          0,
          0,
          0,
          0,
          0,
          1,
          0x65,
        ]),
      );
      await _pump(t);
      expect(h.sessions.single.frameReady, isTrue);
      expect(find.byKey(const Key('qualified-picture')), findsOneWidget);
      expect(find.byType(ConnectionWaitStatus), findsNothing);
      expect(
        h.requests,
        hasLength(1),
        reason: 'Presentation phases must not redial.',
      );
      expect(h.saved, isEmpty);
      await _end(t);
    },
  );

  for (final spec in [
    ('client_backpressure', 'quality'),
    ('frame_layout_unsupported', 'display'),
  ]) {
    testWidgets(
      '${spec.$2} shortcut is draft-only and explicit apply waits for prior cleanup',
      (t) async {
        final h = await _open(
          t,
          failure: spec.$2 == 'quality'
              ? const BrokerException('client_backpressure')
              : null,
        );
        final old = h.sessions.isEmpty ? null : h.sessions.single;
        if (old != null) {
          h.socket.incoming.add(
            jsonEncode({
              'type': 'stream.unavailable',
              'sessionId': 'feedback',
              'reason': spec.$1,
            }),
          );
          await _pump(t);
          expect(old.closed, isTrue);
        }
        expect(find.byKey(Key('android-recovery-${spec.$2}')), findsOneWidget);
        expect(find.byType(ConnectionWaitStatus), findsNothing);
        if (old != null) old.cleanup = Completer<void>();
        final beforeClose = old?.cleanupCalls;
        await _tap(t, 'android-recovery-${spec.$2}');
        await _capture(t, h, 'recovery-${spec.$2}-360-dark');
        expect(h.requests, hasLength(1));
        expect(old?.cleanupCalls, beforeClose);
        expect(h.saved, isEmpty);
        expect(h.savedQuality, isEmpty);
        if (spec.$2 == 'quality') {
          final panel = t.widget<AndroidStreamSettingsPanel>(
            find.byType(AndroidStreamSettingsPanel),
          );
          expect(
            panel.options.bitrate,
            37000000,
            reason: 'A custom value is not reset by opening.',
          );
          await _tap(t, 'stream-bitrate-20');
          expect(h.savedQuality, isEmpty);
          await _tap(t, 'cancel-stream-settings');
        } else {
          await _tap(t, 'android-screen-resolution-l1280');
          expect(h.saved, isEmpty);
          await _tap(t, 'android-recovery-display-cancel');
        }
        expect(h.requests, hasLength(1));
        expect(old?.cleanupCalls, beforeClose);
        expect(h.saved, isEmpty);
        expect(h.savedQuality, isEmpty);
        await _tap(t, 'android-recovery-${spec.$2}');
        h.failure = null;
        if (spec.$2 == 'quality') {
          await _tap(t, 'stream-bitrate-20');
          await _tap(t, 'apply-stream-settings');
          expect(h.savedQuality.single.audio, isTrue);
          expect(h.savedQuality.single.fps, 30);
          expect(h.savedQuality.single.bitrate, 20000000);
        } else {
          await _tap(t, 'android-screen-resolution-l1280');
          await _tap(t, 'android-recovery-display-apply');
          expect(h.saved, hasLength(1));
        }
        if (old != null) {
          expect(old.cleanupCalls, greaterThan(beforeClose!));
          expect(
            h.requests,
            hasLength(1),
            reason: 'The old close future is still held.',
          );
          old.cleanup!.complete();
          await _pump(t);
        }
        expect(h.requests, hasLength(2));
        expect(h.requests.last.audio, isTrue);
        expect(h.requests.last.fps, 30);
        if (spec.$2 == 'quality') {
          expect(h.requests.last.bitrate, 20000000);
        } else {
          expect(h.requests.last.bitrate, 37000000);
        }
        await _end(t);
      },
    );
  }

  for (final failure in [
    const BrokerException('network_error'),
    const BrokerException('session_expired', 401),
    const BrokerException('desktop_not_found', 404),
    const BrokerException('desktop_busy', 409, null, true),
  ]) {
    testWidgets(
      '${failure.code} never offers a settings or automatic-resolution bypass',
      (t) async {
        final h = await _open(t, failure: failure);
        expect(find.byKey(const Key('android-recovery-quality')), findsNothing);
        expect(find.byKey(const Key('android-recovery-display')), findsNothing);
        expect(
          find.byKey(const Key('android-resolution-auto-retry')),
          findsNothing,
        );
        expect(h.requests, hasLength(1));
        expect(h.saved, isEmpty);
        expect(h.savedQuality, isEmpty);
        await _end(t);
      },
    );
  }
  testWidgets(
    'a recovery draft cannot apply after retry permission is withdrawn',
    (t) async {
      final h = await _open(t);
      final session = h.sessions.single;
      h.socket.incoming.add(
        jsonEncode({
          'type': 'stream.unavailable',
          'sessionId': 'feedback',
          'reason': 'frame_layout_unsupported',
        }),
      );
      await _pump(t);
      await _tap(t, 'android-recovery-display');
      await _tap(t, 'android-screen-resolution-l1280');
      session.canRetry = false;
      await _tap(t, 'android-recovery-display-apply');
      expect(h.requests, hasLength(1));
      expect(h.saved, isEmpty);
      expect(h.savedQuality, isEmpty);
      await _end(t);
    },
  );
  testWidgets('retry-disabled media failure cannot open settings', (t) async {
    final h = await _open(t);
    final session = h.sessions.single;
    session.canRetry = false;
    h.socket.incoming.add(
      jsonEncode({
        'type': 'stream.unavailable',
        'sessionId': 'feedback',
        'reason': 'session_expired',
      }),
    );
    await _pump(t);
    expect(find.byKey(const Key('android-recovery-quality')), findsNothing);
    expect(find.byKey(const Key('android-recovery-display')), findsNothing);
    expect(h.requests, hasLength(1));
    await _end(t);
  });
  for (final spec in [
    (const Size(360, 800), 1.0, Brightness.dark),
    (const Size(360, 800), 1.0, Brightness.light),
    (const Size(320, 780), 2.0, Brightness.dark),
    (const Size(800, 360), 2.0, Brightness.dark),
  ]) {
    testWidgets(
      'wait stage and exit remain reachable at ${spec.$1}/${spec.$2}/${spec.$3}',
      (t) async {
        final gate = Completer<void>();
        final h = await _open(
          t,
          loadGate: gate,
          size: spec.$1,
          textScale: spec.$2,
          brightness: spec.$3,
        );
        expect(find.text('연결 요청'), findsOneWidget);
        expect(find.byType(ConnectionWaitStatus), findsOneWidget);
        final leave = find.byKey(const Key('android-leave'));
        await t.ensureVisible(leave);
        await t.pump();
        final rect = t.getRect(leave);
        expect(rect.width, greaterThanOrEqualTo(48));
        expect(rect.height, greaterThanOrEqualTo(48));
        expect(rect.top, greaterThanOrEqualTo(0));
        expect(rect.bottom, lessThanOrEqualTo(spec.$1.height));
        expect(leave.hitTestable(), findsOneWidget);
        expect(t.takeException(), isNull);
        await _capture(
          t,
          h,
          'wait-${spec.$1.width.toInt()}-${spec.$3.name}-x${spec.$2}',
        );
        gate.complete();
        await _pump(t);
        await _end(t);
      },
    );
  }
}
