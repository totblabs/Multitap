// Real popup routing/geometry, with an unstarted memory-only session.
import 'package:flutter/material.dart';
import 'package:flutter/services.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:multitap_client/data/broker_api.dart';
import 'package:multitap_client/data/remote_session.dart';
import 'package:multitap_client/ui/app_theme.dart';
import 'package:multitap_client/ui/primary_region.dart';
import 'package:multitap_client/ui/session_guide_dialog.dart';
import 'package:multitap_client/ui/session_menu.dart';

import 'remote_session_test.dart' show SessionMedia;

class _Panel {
  final popups = SessionPopups();
  final presentation = ValueNotifier(
    const SessionGuidePresentation(canRemember: true),
  );
  final media = SessionMedia();
  late final session = RemoteSession(
    connection: BrokerConnection(
      sessionId: 'synthetic-guide-panel',
      webSocketUrl: Uri.parse('wss://fixture.invalid/guide'),
    ),
    token: 'synthetic-only',
    media: media,
    dialer: (_, _) async => throw StateError('No network in guide fixture'),
  );
  late BuildContext context;

  Future<void> mount(
    WidgetTester tester, {
    required Size size,
    double scale = 1,
    Rect? primary,
  }) async {
    tester.view.devicePixelRatio = 1;
    tester.view.physicalSize = size;
    addTearDown(tester.view.resetDevicePixelRatio);
    addTearDown(tester.view.resetPhysicalSize);
    await tester.pumpWidget(
      MaterialApp(
        theme: buildAppTheme(brightness: Brightness.dark),
        builder: (context, child) => MediaQuery(
          data: MediaQuery.of(context)
              .copyWith(textScaler: TextScaler.linear(scale)),
          child: PrimaryRegionScope(child: child!),
        ),
        home: Scaffold(
          body: Builder(
            builder: (value) {
              context = value;
              return const SizedBox.expand();
            },
          ),
        ),
      ),
    );
    if (primary != null) {
      PrimaryRegionScope.maybeOf(context)!.value = primary;
      await tester.pump();
    }
    addTearDown(() async {
      popups.closeAll();
      await tester.pumpWidget(const SizedBox.shrink());
      await tester.runAsync(() async {
        await session.close();
        await media.events.close();
      });
      presentation.dispose();
    });
  }

  Future<bool?> open(Rect anchor) =>
      Navigator.of(context, rootNavigator: true).push(
        createSessionGuidePanelRoute(
          context,
          anchor: anchor,
          machine: '주 모니터의 사용안내',
          session: session,
          presentation: presentation,
          popups: popups,
        ),
      );
}

void main() {
  for (final narrow in [false, true]) {
    for (final beforeBuild in [false, true]) {
      testWidgets(
        'guide ${narrow ? 'narrow' : 'wide'} panel popup scope closes beforeBuild=$beforeBuild',
        (tester) async {
          final panel = _Panel();
          await panel.mount(tester, size: Size(narrow ? 320 : 1366, 768));
          final result = panel.open(const Rect.fromLTWH(140, 8, 40, 40));
          if (beforeBuild) {
            panel.popups.closeAll();
          } else {
            await tester.pumpAndSettle();
            expect(panel.popups.isEmpty, isFalse);
            expect(
              find.byKey(const Key('session-guide-dialog')),
              findsOneWidget,
            );
            expect(find.byType(Dialog), findsNothing);
            final bounds = tester.getRect(
              find.byKey(const Key('session-guide-panel')),
            );
            expect(bounds.left, greaterThanOrEqualTo(8));
            expect(bounds.right, lessThanOrEqualTo((narrow ? 320 : 1366) - 8));
            expect(
              find.byKey(const Key('close-session-guide')).hitTestable(),
              findsOneWidget,
            );
            expect(
              find.byKey(const Key('confirm-session-guide')).hitTestable(),
              findsOneWidget,
            );
            panel.popups.closeAll();
          }
          await tester.pumpAndSettle();
          expect(await result, isNull);
          expect(panel.popups.isEmpty, isTrue);
          expect(find.byKey(const Key('session-guide-dialog')), findsNothing);
          expect(tester.takeException(), isNull);
        },
      );
    }
  }

  for (final scale in [1.0, 2.0]) {
    testWidgets(
      'anchored guide remains in the primary monitor at text scale $scale',
      (tester) async {
        final panel = _Panel();
        const primary = Rect.fromLTWH(960, 0, 960, 1080);
        await panel.mount(
          tester,
          size: const Size(1920, 1080),
          primary: primary,
          scale: scale,
        );
        final result = panel.open(const Rect.fromLTWH(1860, 10, 40, 36));
        await tester.pumpAndSettle();
        final bounds = tester.getRect(
          find.byKey(const Key('session-guide-panel')),
        );
        expect(bounds.left, greaterThanOrEqualTo(primary.left + 8));
        expect(bounds.right, lessThanOrEqualTo(primary.right - 8));
        expect(bounds.bottom, lessThanOrEqualTo(primary.bottom - 8));
        expect(
          find.byKey(const Key('close-session-guide')).hitTestable(),
          findsOneWidget,
        );
        expect(
          find.byKey(const Key('confirm-session-guide')).hitTestable(),
          findsOneWidget,
        );
        await tester.ensureVisible(find.byKey(const Key('session-guide-hide')));
        await tester.pump();
        await tester.tap(find.byKey(const Key('session-guide-hide')));
        await tester.pump();
        await tester.tap(find.byKey(const Key('confirm-session-guide')));
        await tester.pumpAndSettle();
        expect(await result, isTrue);
        expect(panel.popups.isEmpty, isTrue);
        expect(tester.takeException(), isNull);
      },
    );
  }

  testWidgets(
    'anchored guide Escape closes without committing the preference',
    (tester) async {
      final panel = _Panel();
      await panel.mount(tester, size: const Size(960, 640));
      final result = panel.open(const Rect.fromLTWH(680, 8, 40, 36));
      await tester.pumpAndSettle();
      await tester.sendKeyEvent(LogicalKeyboardKey.escape);
      await tester.pumpAndSettle();
      expect(await result, isNull);
      expect(panel.popups.isEmpty, isTrue);
      expect(tester.takeException(), isNull);
    },
  );
}
