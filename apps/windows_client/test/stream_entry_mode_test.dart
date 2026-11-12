// UI/presentation-order coverage only; native window and transport are fixtures.
import 'dart:async';

import 'package:flutter/material.dart';
import 'package:flutter/services.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:multitap_client/data/broker_api.dart';
import 'package:multitap_client/models/session_display_mode.dart';
import 'package:multitap_client/models/stream_options.dart';
import 'package:multitap_client/ui/app_theme.dart';
import 'package:multitap_client/ui/remote_view.dart';
import 'package:multitap_client/ui/workspace_shell.dart';

import 'session_recovery_test.dart' show machine;
import 'helpers/stream_connect_consent.dart';

void main() {
  TestWidgetsFlutterBinding.ensureInitialized();
  final messenger =
      TestDefaultBinaryMessengerBinding.instance.defaultBinaryMessenger;
  const window = MethodChannel('multitap/window');
  const events = MethodChannel('multitap/window/events');

  void native(Future<Object?> Function(MethodCall) callback) {
    messenger.setMockMethodCallHandler(window, callback);
    messenger.setMockMethodCallHandler(events, (_) async => null);
    addTearDown(() {
      messenger.setMockMethodCallHandler(window, null);
      messenger.setMockMethodCallHandler(events, null);
    });
  }

  Future<void> finish(WidgetTester tester) async {
    await tester.pumpWidget(const SizedBox());
    await tester.pump();
  }

  for (final mode in [
    SessionDisplayMode.windowed,
    SessionDisplayMode.fullscreen,
  ]) {
    testWidgets(
      'Streaming ${mode.name} menu confirms and awaits native presentation before connect',
      (tester) async {
        tester.view.physicalSize = const Size(1440, 1000);
        tester.view.devicePixelRatio = 1;
        addTearDown(tester.view.resetPhysicalSize);
        addTearDown(tester.view.resetDevicePixelRatio);
        final applied = Completer<Object?>();
        final calls = <String>[];
        var connects = 0;
        native((call) async {
          calls.add(call.method);
          if (call.method == 'fullscreen') {
            expect(call.arguments, mode == SessionDisplayMode.fullscreen);
            return applied.future;
          }
          return null;
        });
        await tester.pumpWidget(
          MaterialApp(
            theme: buildAppTheme(),
            home: WorkspaceShell(
              displayName: 'fixture',
              serverOrigin: 'https://fixture.invalid',
              onSignOut: () {},
              onRefresh: () {},
              machines: const [machine],
              localMonitors: 2,
              onConnect: (_, {options = const StreamOptions()}) async {
                expect(applied.isCompleted, isTrue);
                expect(options.monitor, '0');
                connects++;
                calls.add('connect');
                throw const BrokerException('network_error');
              },
            ),
          ),
        );
        await tester.pumpAndSettle();
        await tester.tap(find.byKey(const Key('stream-modes-qa')));
        await tester.pumpAndSettle();
        expect(connects, 0);
        expect(calls, isEmpty);
        expect(
          tester
              .widget<MenuItemButton>(
                find.byKey(const Key('stream-mode-allMonitors')),
              )
              .onPressed,
          isNull,
        );
        await tester.tapAt(const Offset(1400, 900));
        await tester.pumpAndSettle();
        expect(find.byKey(const Key('stream-mode-windowed')), findsNothing);
        expect(connects, 0);
        expect(calls, isEmpty);
        await tester.tap(find.byKey(const Key('stream-modes-qa')));
        await tester.pumpAndSettle();
        await tester.tap(find.byKey(Key('stream-mode-${mode.name}')));
      await acceptStreamConnect(tester, 'qa');
        // Native presentation is intentionally unresolved: settling its
        // loading spinner would advance to the real presentation timeout.
        await tester.pump();
        expect(connects, 0);
        await tester.pump();
        await tester.pump(const Duration(milliseconds: 200));
        expect(calls, ['fullscreen']);
        expect(connects, 0);
        expect(
          tester.widget<RemoteView>(find.byType(RemoteView)).initialDisplayMode,
          mode,
        );
        applied.complete(mode == SessionDisplayMode.fullscreen);
        // Presentation ACK is followed by an actual viewport frame before
        // transport starts; the elapsed label is not a completion signal.
        for (var n = 0; n < 10 && connects == 0; n++) {
          await tester.pump(const Duration(milliseconds: 20));
        }
        expect(
          calls,
          ['fullscreen', 'connect'],
          reason: tester
              .widgetList<Text>(find.byWidgetPredicate((w) => w is Text))
              .map((w) => w.data ?? w.textSpan?.toPlainText())
              .join(' | '),
        );
        expect(connects, 1);
        expect(tester.takeException(), isNull);
        await finish(tester);
        expect(calls.last, 'restore');
      },
    );
  }

  testWidgets(
    'Closing while native presentation is pending never starts transport',
    (tester) async {
      final applied = Completer<Object?>();
      var connects = 0;
      native(
        (call) async => call.method == 'fullscreen' ? applied.future : null,
      );
      await tester.pumpWidget(
        MaterialApp(
          home: RemoteView(
            machine: machine,
            load: () async {
              connects++;
              throw const BrokerException('network_error');
            },
          ),
        ),
      );
      await tester.pump();
      await finish(tester);
      applied.complete(true);
      await tester.pump();
      expect(connects, 0);
      expect(tester.takeException(), isNull);
    },
  );

  testWidgets(
    'Unsupported dual mode cannot bypass menu or use a composite fallback',
    (tester) async {
      final calls = <String>[];
      var connects = 0;
      native((call) async {
        calls.add(call.method);
        return null;
      });
      await tester.pumpWidget(
        MaterialApp(
          home: RemoteView(
            machine: machine,
            initialDisplayMode: SessionDisplayMode.allMonitors,
            load: () async {
              connects++;
              throw const BrokerException('network_error');
            },
          ),
        ),
      );
      await tester.pumpAndSettle();
      expect(calls, isEmpty);
      expect(connects, 0);
      expect(find.textContaining('모든 모니터로 연결하지 못했어요.'), findsOneWidget);
      await finish(tester);
    },
  );

  testWidgets(
    'Native presentation error stays in the session UI without contacting broker',
    (tester) async {
      var connects = 0;
      native((call) async {
        if (call.method == 'fullscreen') {
          throw PlatformException(code: 'presentation_failed');
        }
        return null;
      });
      await tester.pumpWidget(
        MaterialApp(
          home: RemoteView(
            machine: machine,
            load: () async {
              connects++;
              throw const BrokerException('network_error');
            },
          ),
        ),
      );
      await tester.pumpAndSettle();
      expect(connects, 0);
      expect(find.textContaining('화면 모드를 적용하지 못했어요.'), findsOneWidget);
      expect(tester.takeException(), isNull);
      await finish(tester);
    },
  );
}
