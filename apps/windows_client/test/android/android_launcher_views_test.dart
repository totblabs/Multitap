import '../helpers/stream_connect_consent.dart' show acceptStreamConnect;
import 'package:flutter/material.dart';
import 'package:flutter/services.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:multitap_client/android/android_store.dart';
import 'package:multitap_client/android/android_session_keepalive.dart';
import 'package:multitap_client/ui/desktop_tile.dart';

import 'android_launcher_test.dart' show MemoryStore, signedIn, vm, mainButton;

/// Arrangements an earlier version could save ('launcher.view'): all open
/// the one card grid now.
const _legacyViews = [
  'cards',
  'list',
  'mini',
  'explorer',
  'monitorWall',
  'docking',
];

Future<void> _dispose(WidgetTester tester) async {
  await tester.pumpWidget(const SizedBox.shrink());
  await tester.pumpAndSettle();
}

void main() {
  setUp(() {
    final messenger =
        TestDefaultBinaryMessengerBinding.instance.defaultBinaryMessenger;
    messenger.setMockMethodCallHandler(
      const MethodChannel('multitap/pen-input'),
      (_) async => null,
    );
    messenger.setMockMethodCallHandler(
      AndroidSessionKeepalive.channel,
      (call) async => call.method == 'start' ? true : null,
    );
  });
  tearDown(() {
    final messenger =
        TestDefaultBinaryMessengerBinding.instance.defaultBinaryMessenger;
    messenger.setMockMethodCallHandler(
      const MethodChannel('multitap/pen-input'),
      null,
    );
    messenger.setMockMethodCallHandler(AndroidSessionKeepalive.channel, null);
  });
  TestWidgetsFlutterBinding.ensureInitialized();
  tearDown(() {
    TestDefaultBinaryMessengerBinding.instance.defaultBinaryMessenger
        .setMockMethodCallHandler(AndroidStore.channel, null);
  });

  testWidgets(
    'the explicit card action asks before opening its stream',
    (tester) async {
      final broker = await signedIn(tester, [vm()]);
      await _tapVisible(tester, mainButton('pve-149'));
      await acceptStreamConnect(tester, 'pve-149');
      await tester.pumpAndSettle();
      expect(find.byKey(const Key('connect-confirm-dialog')), findsNothing);
      expect(find.byKey(const Key('android-session-panel')), findsOneWidget);
      expect(broker.calls.where((c) => c.startsWith('power:')), isEmpty);
      await _dispose(tester);
    },
  );

  testWidgets(
    'fixed cards preserve preview and session preferences across launches without writing a layout',
    (tester) async {
      final memory = MemoryStore(
        origin: 'https://vdi.example',
        username: 'tester',
        prefs: {
          'launcher.view': 'explorer',
          'launcher.previews': 'off',
          'stream.quality': 'fixture-quality',
          'session.input': 'direct',
          'organize.https://vdi.example|tester':
              '{"chip":"all","status":"all","sort":"number"}',
        },
      );
      final preserved = Map<String, String>.of(memory.prefs);
      for (var launch = 0; launch < 2; launch++) {
        final broker = await signedIn(tester, [vm()], store: memory);
        expect(find.byKey(const Key('cards-view')), findsOneWidget);
        expect(
          tester.widget<DesktopTile>(find.byType(DesktopTile)).showPreview,
          isFalse,
        );
        expect(find.textContaining('카드 크기'), findsNothing);
        for (final entry in preserved.entries) {
          expect(memory.prefs[entry.key], entry.value, reason: entry.key);
        }
        expect(
          memory.calls.where(
            (call) =>
                call.method == 'savePreference' &&
                [
                  'launcher.view',
                  'launcher.cardSize',
                ].contains((call.arguments as Map)['key']),
          ),
          isEmpty,
        );
        expect(
          broker.calls.where((c) => c == 'connect' || c.startsWith('power:')),
          isEmpty,
        );
        await _dispose(tester);
      }
    },
  );

  for (final legacy in _legacyViews) {
    testWidgets(
      'saved $legacy opens cards with all actions and preview-only options',
      (tester) async {
        final memory = MemoryStore(
          origin: 'https://vdi.example',
          username: 'tester',
          prefs: {'launcher.view': legacy},
        );
        final broker = await signedIn(tester, [
          vm(),
          vm(id: 'pve-150'),
        ], store: memory);
        expect(find.byKey(const Key('cards-view')), findsOneWidget);
        expect(mainButton('pve-149'), findsOneWidget);
        expect(mainButton('pve-150'), findsOneWidget);
        // Only the 미리보기 toggle: no arrangement or card size choice.
        for (final old in _legacyViews) {
          expect(find.byKey(Key('android-view-$old')), findsNothing);
        }
        for (final size in ['small', 'medium', 'large']) {
          expect(find.byKey(Key('android-card-size-$size')), findsNothing);
        }
        expect(find.byKey(const Key('desktop-previews')), findsOneWidget);
        expect(
          broker.calls.where((c) => c == 'connect' || c.startsWith('power:')),
          isEmpty,
        );
        await _dispose(tester);
      },
    );
  }

  testWidgets('sorting and removal keep card identity and never connect', (
    tester,
  ) async {
    final broker = await signedIn(tester, [
      vm(id: 'pve-101', name: '문서 작업'),
      vm(id: 'pve-102', name: '개발 환경'),
      vm(id: 'pve-103', name: '검증 VM'),
      vm(id: 'pve-104', name: '작업 데스크톱'),
    ]);
    await _tapVisible(tester, find.byKey(const Key('desktop-name-pve-102')));
    await _tapVisible(tester, find.byKey(const Key('desktop-sort')));
    await tester.pumpAndSettle();
    await _tapVisible(tester, find.byKey(const Key('sort-name')));
    await tester.pumpAndSettle();
    final sorted = tester
        .widgetList<DesktopTile>(find.byType(DesktopTile))
        .map((c) => c.machine.id)
        .toList();
    expect(sorted.indexOf('pve-102'), lessThan(sorted.indexOf('pve-101')));
    broker.inventory = [broker.inventory.first];
    await _tapVisible(tester, find.byKey(const Key('refresh-desktops')));
    await tester.pumpAndSettle();
    expect(find.byKey(const Key('card-pve-101')), findsOneWidget);
    expect(find.byKey(const Key('card-pve-102')), findsNothing);
    expect(
      broker.calls.where((c) => c == 'connect' || c.startsWith('power:')),
      isEmpty,
    );
    await _dispose(tester);
  });

  testWidgets(
    'empty list and unknown legacy preference use the fixed card workspace',
    (tester) async {
      final memory = MemoryStore(
        origin: 'https://vdi.example',
        username: 'tester',
        prefs: {'launcher.view': 'future-style'},
      );
      final broker = await signedIn(tester, [], store: memory);
      expect(find.byKey(const Key('desktop-previews')), findsOneWidget);
      expect(find.textContaining('카드 크기'), findsNothing);
      expect(find.byType(DesktopTile), findsNothing);
      expect(find.text('아직 배정된 데스크톱이 없어요'), findsOneWidget);
      expect(memory.prefs['launcher.view'], 'future-style');
      expect(
        broker.calls.where((c) => c == 'connect' || c.startsWith('power:')),
        isEmpty,
      );
      await _dispose(tester);
    },
  );
}

Future<void> _tapVisible(WidgetTester tester, Finder target) async {
  await tester.ensureVisible(target);
  await tester.pumpAndSettle();
  await tester.tap(target);
}
