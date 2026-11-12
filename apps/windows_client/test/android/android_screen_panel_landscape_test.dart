// The user now asks for bounded resolution choices and five Windows scales.
// Panels stay anchored beside the cluster and every control remains reachable
// in short landscape; scrolling is allowed instead of clipping controls.
// Widget renders with an
// in-process session and the product font: no broker, Host, MediaCodec or
// device.
import 'dart:convert';

import 'package:flutter/material.dart';
import 'package:flutter/services.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:multitap_client/android/android_remote.dart';
import 'package:multitap_client/android/android_session_keepalive.dart';
import 'package:multitap_client/android/android_store.dart';
import 'package:multitap_client/android/android_toolbar.dart';
import 'package:multitap_client/data/broker_api.dart';
import 'package:multitap_client/data/remote_session.dart';
import 'package:multitap_client/models/stream_options.dart';
import 'package:multitap_client/ui/app_theme.dart';

import '../remote_session_test.dart' show SessionSocket;
import 'android_large_desktop_test.dart'
    show openLargeDesktopStream, openLargeDesktopScreenPanel;
import 'android_session_contract_test.dart' show AndroidNativeFake;

const _pen = MethodChannel('multitap/pen-input');

Uint8List _video(int pts) {
  final bytes = Uint8List(17);
  bytes[0] = 1;
  bytes[1] = 1;
  for (var i = 0; i < 8; i++) {
    bytes[4 + i] = (pts >> (8 * i)) & 255;
  }
  bytes.setAll(12, [0, 0, 0, 1, 0x65]);
  return bytes;
}

/// The 화면 panel open over a connected session at [size] (dp, ratio 1),
/// from a Host without 화면 배율 and 해상도 (as in the capture fixture), with
/// the trackpad's 포인터 속도.
Future<void> _plainPanel(WidgetTester t, Size size) async {
  t.view.devicePixelRatio = 1;
  t.view.physicalSize = size;
  addTearDown(t.view.reset);
  AndroidNativeFake().install();
  addTearDown(AndroidNativeFake.uninstall);
  final sockets = <SessionSocket>[];
  await t.pumpWidget(
    MaterialApp(
      theme: buildAppTheme(brightness: Brightness.dark),
      home: AndroidRemoteView(
        name: 'TESTER',
        token: 'not-persisted',
        options: const StreamOptions(),
        density: () async => null,
        settings: const AndroidSessionSettings(
          helpSeen: true,
          touch: AndroidTouchMode.trackpad,
        ),
        load: (_, _, _) async => BrokerConnection(
          sessionId: 'android-session',
          webSocketUrl: Uri.parse(
            'wss://fixture.invalid/api/v1/sessions/android-session',
          ),
        ),
        sessionFactory: (connection, media, _) => RemoteSession(
          connection: connection,
          token: 'not-persisted',
          media: media,
          dialer: (_, _) async {
            final socket = SessionSocket();
            sockets.add(socket);
            return socket;
          },
        ),
        pictureBuilder: (_) => const ColoredBox(color: Color(0xFF1B4F9C)),
      ),
    ),
  );
  for (var i = 0; i < 4; i++) {
    await t.pump(const Duration(milliseconds: 20));
  }
  sockets.last.incoming.add(
    jsonEncode({
      'type': 'stream.info',
      'codec': 'h264',
      'width': size.width.toInt() ~/ 2 * 2,
      'height': size.height.toInt() ~/ 2 * 2,
    }),
  );
  await t.pump(const Duration(milliseconds: 20));
  sockets.last.incoming.add(_video(16667));
  for (var i = 0; i < 6; i++) {
    await t.pump(const Duration(milliseconds: 40));
  }
  await openLargeDesktopScreenPanel(t);
}

/// The same over a Host that also offers 화면 배율 and 해상도.
Future<void> _fullPanel(WidgetTester t, Size size) async {
  await openLargeDesktopStream(
    t,
    physical: size,
    resolution: AndroidResolution.auto,
  );
  await openLargeDesktopScreenPanel(t);
}

final _panelFinder = find.byKey(const Key('android-screen-panel'));

/// How far the panel's own content would have to scroll.
double _overflow(WidgetTester t) => t
    .state<ScrollableState>(
      find.descendant(of: _panelFinder, matching: find.byType(Scrollable)),
    )
    .position
    .maxScrollExtent;

Rect _rect(WidgetTester t, String key) => t.getRect(find.byKey(Key(key)));

Future<void> _close(WidgetTester t) async {
  await t.pumpWidget(const SizedBox.shrink());
  // RemoteSession closes its fake wires with an existing bounded2s timeout.
  await t.pump(const Duration(seconds: 3));
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
    messenger.setMockMethodCallHandler(_pen, (_) async => null);
    messenger.setMockMethodCallHandler(
      AndroidSessionKeepalive.channel,
      (_) async => null,
    );
  });
  tearDown(() {
    final messenger =
        TestDefaultBinaryMessengerBinding.instance.defaultBinaryMessenger;
    for (final channel in [
      _pen,
      AndroidSessionKeepalive.channel,
      AndroidStore.channel,
    ]) {
      messenger.setMockMethodCallHandler(channel, null);
    }
  });

  // Resolution is now a separate bounded source choice where resize is granted.
  // 배율 is in this panel where the Host sets it, between the view rows and
  // the resolution as in the Windows 화면 panel; pointer speed
  // stays under input mode.
  const trimmed = ['android-screen-fit'];
  const gone = [
    'android-screen-zoom',
    'android-screen-zoom-in',
    'android-screen-zoom-out',
    'android-screen-fill',
    'android-screen-orientation-auto',
    'android-screen-scale-0',
    'android-speed-fast',
  ];
  for (final size in const [
    Size(412, 915),
    Size(915, 412),
    Size(800, 360),
    Size(1280, 800),
  ]) {
    final label = '${size.width.toInt()}x${size.height.toInt()}';
    for (final full in [false, true]) {
      testWidgets(
        '$label${full ? ' (Host with 해상도)' : ''}: source choices and view controls stay reachable beside the cluster',
        (t) async {
          try {
            if (full) {
              await _fullPanel(t, size);
            } else {
              await _plainPanel(t, size);
            }
            for (final key in trimmed) {
              expect(find.byKey(Key(key)), findsOneWidget, reason: key);
            }
            expect(
              find.byKey(const Key('android-session-move')),
              findsOneWidget,
            );
            for (final key in gone) {
              expect(find.byKey(Key(key)), findsNothing, reason: key);
            }
            final resolution = find.byKey(
              const Key('android-screen-resolution'),
            );
            expect(resolution, full ? findsOneWidget : findsNothing);
            if (full) {
              final choices = t.widget<AndroidPanelChoices>(resolution);
              expect(choices.items.length, inInclusiveRange(2, 5));
              expect(
                choices.items.where((item) => item.selected),
                hasLength(1),
              );
              for (final item in choices.items) {
                final button = find.byKey(item.key);
                await t.ensureVisible(button);
                await t.pumpAndSettle();
                expect(button.hitTestable(), findsOneWidget);
              }
            }
            // 배율: Windows' five steps, only where the Host sets the scale,
            // after the view rows and before the resolution.
            final scale = find.byKey(const Key('android-screen-scale'));
            expect(scale, full ? findsOneWidget : findsNothing);
            expect(find.byKey(const Key('android-scale-panel')), findsNothing);
            if (full) {
              final steps = t.widget<AndroidPanelChoices>(scale);
              expect(
                [for (final item in steps.items) item.key],
                [
                  for (final percent in [100, 125, 150, 175, 200])
                    Key('android-screen-scale-$percent'),
                ],
              );
              expect(
                steps.items.where((item) => item.selected).length,
                lessThanOrEqualTo(1),
              );
              expect(
                t.getRect(scale).top,
                greaterThanOrEqualTo(
                  _rect(t, 'android-session-move').bottom - .01,
                ),
              );
              expect(
                t.getRect(resolution).top,
                greaterThanOrEqualTo(t.getRect(scale).bottom - .01),
              );
              expect(
                _rect(t, 'android-screen-fit').top,
                greaterThanOrEqualTo(
                  _rect(t, 'android-screen-observed-state').bottom - .01,
                ),
              );
              for (final item in steps.items) {
                final button = find.byKey(item.key);
                await t.ensureVisible(button);
                await t.pumpAndSettle();
                expect(button.hitTestable(), findsOneWidget);
              }
            }
            for (final key in [...trimmed, 'android-session-move']) {
              final button = find.byKey(Key(key));
              await t.ensureVisible(button);
              await t.pumpAndSettle();
              expect(button.hitTestable(), findsOneWidget);
            }
            final panel = t.getRect(_panelFinder);
            expect(
              panel.right,
              lessThanOrEqualTo(_rect(t, 'session-bubble').left),
            );
            expect(panel.top, greaterThanOrEqualTo(0));
            expect(panel.bottom, lessThanOrEqualTo(size.height));
            expect(_overflow(t), greaterThanOrEqualTo(0));
            expect(_overflow(t).isFinite, isTrue);
            expect(t.takeException(), isNull);
          } finally {
            await _close(t);
          }
        },
      );
    }
  }

  for (final size in const [Size(412, 915), Size(915, 412)]) {
    testWidgets(
      '${size.width.toInt()}x${size.height.toInt()}: a long press on 입력 '
      '방식 opens its panel; 포인터 속도 only in pointer mode',
      (t) async {
        try {
          await _plainPanel(t, size);
          // The menu goes; the cluster stays as it is.
          await t.tap(find.byKey(const Key('expand-session-toolbar')));
          await t.pumpAndSettle();
          expect(_panelFinder, findsNothing);
          final mode = find.byKey(
            const Key('android-remote-touch-mode-button'),
          );
          await t.longPress(mode);
          await t.pumpAndSettle();
          final input = find.byKey(const Key('android-input-panel'));
          expect(input, findsOneWidget);
          expect(find.byKey(const Key('android-speed-fast')), findsOneWidget);
          final rect = t.getRect(input);
          expect(
            rect.right,
            lessThanOrEqualTo(_rect(t, 'session-bubble').left),
          );
          expect(rect.bottom, lessThanOrEqualTo(size.height));
          await t.tap(find.byKey(const Key('android-speed-fast')));
          await t.pumpAndSettle();
          await t.tap(find.byKey(const Key('android-input-direct')));
          await t.pumpAndSettle();
          expect(find.byKey(const Key('android-speed-fast')), findsNothing);
          expect(t.takeException(), isNull);
        } finally {
          await _close(t);
        }
      },
    );
  }

  for (final size in const [Size(412, 915), Size(915, 412), Size(1280, 800)]) {
    testWidgets(
      '${size.width.toInt()}x${size.height.toInt()}: 화면 holds Windows\' scale (배율) at the menu\'s spot, with a way back',
      (t) async {
        try {
          await openLargeDesktopStream(
            t,
            physical: size,
            resolution: AndroidResolution.auto,
          );
          await t.tap(find.byKey(const Key('expand-session-toolbar')));
          await t.pumpAndSettle();
          // 배율 has no row or panel of its own: it is inside 화면.
          expect(find.byKey(const Key('android-session-scale')), findsNothing);
          await t.tap(find.byKey(const Key('android-session-screen')));
          await t.pumpAndSettle();
          expect(find.byKey(const Key('android-scale-panel')), findsNothing);
          final panel = _panelFinder;
          expect(panel, findsOneWidget);
          expect(find.byKey(const Key('android-screen-scale')), findsOneWidget);
          expect(find.byKey(const Key('android-screen-scale-0')), findsNothing);
          final choices = t.widget<AndroidPanelChoices>(
            find.byKey(const Key('android-screen-scale')),
          );
          expect(choices.items, hasLength(5));
          expect(choices.items.where((item) => item.selected), hasLength(1));
          for (final percent in [100, 125, 150, 175, 200]) {
            final button = find.byKey(Key('android-screen-scale-$percent'));
            await t.ensureVisible(button);
            await t.pumpAndSettle();
            expect(button.hitTestable(), findsOneWidget);
          }
          final rect = t.getRect(panel);
          expect(
            rect.right,
            lessThanOrEqualTo(_rect(t, 'session-bubble').left),
          );
          expect(rect.top, greaterThanOrEqualTo(0));
          expect(rect.bottom, lessThanOrEqualTo(size.height));
          await t.ensureVisible(find.byKey(const Key('session-menu-back')));
          await t.tap(find.byKey(const Key('session-menu-back')));
          await t.pumpAndSettle();
          expect(find.byKey(const Key('session-command-bar')), findsOneWidget);
          expect(t.takeException(), isNull);
        } finally {
          await _close(t);
        }
      },
    );
  }
}
