// Launcher tiles with last-frame pictures, and the sign-in/sign-out wiring
// that scopes and deletes them. Widget checks with injected sources: no
// native writer, file, broker or session is involved.
import 'dart:async';
import 'dart:io';
import 'dart:typed_data';

import 'package:flutter/gestures.dart';
import 'package:flutter/material.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:multitap_client/app.dart';
import 'package:multitap_client/data/broker_api.dart';
import 'package:multitap_client/data/desktop_thumbnails.dart';
import 'package:multitap_client/data/launcher_memory.dart';
import 'package:multitap_client/models/desktop_machine.dart';
import 'package:multitap_client/models/stream_options.dart';
import 'package:multitap_client/ui/app_theme.dart';
import 'package:multitap_client/ui/desktop_art.dart';
import 'package:multitap_client/ui/remote_view.dart';
import 'package:multitap_client/ui/workspace_shell.dart';

import 'auth_flow_test.dart' show TestBroker;
import 'desktop_tile_test.dart' show vm;
import 'helpers/stream_connect_consent.dart';

/// A 1x1 transparent PNG.
final pixel = Uint8List.fromList([
  0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A, 0x00, 0x00, 0x00, 0x0D, //
  0x49, 0x48, 0x44, 0x52, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01,
  0x08, 0x06, 0x00, 0x00, 0x00, 0x1F, 0x15, 0xC4, 0x89, 0x00, 0x00, 0x00,
  0x0A, 0x49, 0x44, 0x41, 0x54, 0x78, 0x9C, 0x63, 0x00, 0x01, 0x00, 0x00,
  0x05, 0x00, 0x01, 0x0D, 0x0A, 0x2D, 0xB4, 0x00, 0x00, 0x00, 0x00, 0x49,
  0x45, 0x4E, 0x44, 0xAE, 0x42, 0x60, 0x82,
]);

class FakeThumbnails implements ThumbnailSource {
  final pictures = <String, DesktopThumbnail>{};
  final reads = <String>[];
  final refreshes = <String>[];
  final changes = StreamController<String>.broadcast();
  @override
  Stream<String> get updates => changes.stream;
  @override
  Future<DesktopThumbnail?> read(
    String desktopId, {
    bool refresh = false,
  }) async {
    reads.add(desktopId);
    if (refresh) refreshes.add(desktopId);
    return pictures[desktopId];
  }
}

DesktopThumbnail picture(Duration age, {int seed = 0}) => DesktopThumbnail(
  // Distinct bytes instances are distinct image identities.
  image: MemoryImage(Uint8List.fromList([...pixel, ...List.filled(seed, 0)])),
  modified: DateTime.now().subtract(age),
);

Widget shell(List<DesktopMachine> machines, ThumbnailSource? thumbnails) =>
    MaterialApp(
      theme: buildAppTheme(brightness: Brightness.dark),
      home: WorkspaceShell(
        onSignOut: () {},
        onRefresh: () {},
        displayName: 'fixture',
        serverOrigin: 'https://vdi.example.com',
        machines: machines,
        launcherMemory: SessionLauncherMemory(),
        thumbnails: thumbnails,
        onConnect: (_, {options = const StreamOptions()}) async =>
            throw StateError('fixture only'),
      ),
    );

DesktopArt art(WidgetTester t, String id) => t.widget<DesktopArt>(
  find.descendant(
    of: find.byKey(Key('card-$id')),
    matching: find.byType(DesktopArt),
  ),
);

String? caption(WidgetTester t, String id) => t
    .widget<Text>(
      find.descendant(
        of: find.byKey(Key('thumbnail-caption-$id')),
        // Any Text, including the word-wrapping KeepAllText.
        matching: find.byWidgetPredicate((widget) => widget is Text),
      ),
    )
    .data;

class RecordingStore extends DesktopThumbnails {
  RecordingStore() : super(Directory('unused-thumbnail-root'));
  final retained = <String>[], forgotten = <String>[];
  @override
  Future<void> retainOnly(String key) async => retained.add(key);
  @override
  Future<void> forget(String key) async => forgotten.add(key);
  @override
  Future<DesktopThumbnail?> read(
    String key,
    String desktopId, {
    bool refresh = false,
  }) async => null;
}

class AccountBroker extends TestBroker {
  String account = 'account-one';
  @override
  Future<BrokerSession> login(String username, String password) async {
    await super.login(username, password);
    return BrokerSession(
      token: 'test-token',
      expiresAt: DateTime.now().add(const Duration(hours: 1)),
      username: username,
      accountId: account,
    );
  }
}

void main() {
  Future<void> show(WidgetTester t, Widget widget) async {
    t.view.devicePixelRatio = 1;
    t.view.physicalSize = const Size(1440, 900);
    addTearDown(t.view.resetPhysicalSize);
    addTearDown(t.view.resetDevicePixelRatio);
    await t.pumpWidget(widget);
    await t.pump();
    await t.pump();
  }

  testWidgets('A desktop with a last frame shows it with a relative caption', (
    t,
  ) async {
    final source = FakeThumbnails();
    source.pictures['pve-149'] = picture(const Duration(minutes: 3));
    await show(t, shell([vm('pve-149'), vm('pve-150')], source));
    expect(art(t, 'pve-149').thumbnail, source.pictures['pve-149']!.image);
    expect(art(t, 'pve-149').dimmed, isFalse);
    expect(art(t, 'pve-149').snapshot, isFalse, reason: 'a recent picture');
    expect(caption(t, 'pve-149'), '마지막 화면 · 3분 전');
    // No picture: the plain placeholder, no time caption, and a hint why.
    expect(art(t, 'pve-150').thumbnail, isNull);
    expect(art(t, 'pve-150').snapshot, isFalse);
    expect(find.byKey(const Key('thumbnail-caption-pve-150')), findsNothing);
    expect(find.byKey(const Key('thumbnail-empty-pve-150')), findsOneWidget);
    expect(find.byKey(const Key('thumbnail-empty-pve-149')), findsNothing);
    expect(t.takeException(), isNull);
  });

  testWidgets(
    'An old picture of a running desktop reads as a stored snapshot; ▶ still appears on hover',
    (t) async {
      final source = FakeThumbnails();
      // Just under and at the 10-minute snapshot age.
      source.pictures['pve-149'] = picture(const Duration(minutes: 9));
      source.pictures['pve-150'] = picture(
        thumbnailSnapshotAge + const Duration(minutes: 20),
      );
      await show(t, shell([vm('pve-149'), vm('pve-150')], source));
      expect(art(t, 'pve-149').snapshot, isFalse);
      expect(caption(t, 'pve-149'), '마지막 화면 · 9분 전');
      // Slightly dimmed and desaturated, not greyed like an off desktop.
      expect(art(t, 'pve-150').snapshot, isTrue);
      expect(art(t, 'pve-150').dimmed, isFalse);
      expect(
        find.descendant(
          of: find.byKey(const Key('card-pve-150')),
          matching: find.byType(ColorFiltered),
        ),
        findsOneWidget,
      );
      expect(caption(t, 'pve-150'), '저장된 마지막 화면 · 30분 전');
      expect(
        find.descendant(
          of: find.byKey(const Key('thumbnail-caption-pve-150')),
          matching: find.byIcon(Icons.photo_outlined),
        ),
        findsOneWidget,
      );
      // The ▶ connect button still shows over the snapshot while hovered.
      double play() => t
          .widget<AnimatedOpacity>(
            find
                .ancestor(
                  of: find.byKey(const Key('play-pve-150')),
                  matching: find.byType(AnimatedOpacity),
                )
                .first,
          )
          .opacity;
      expect(play(), 0);
      final mouse = await t.createGesture(kind: PointerDeviceKind.mouse);
      await mouse.addPointer(
        location: t.getCenter(find.byKey(const Key('card-pve-150'))),
      );
      await t.pumpAndSettle();
      expect(play(), 1);
      await t.tap(find.byKey(const Key('play-pve-150')));
      await acceptStreamConnect(t, 'pve-150');
      await t.pumpAndSettle();
      expect(find.byKey(const Key('connect-confirm-dialog')), findsNothing);
      expect(find.byType(RemoteView), findsOneWidget);
      await mouse.removePointer();
      expect(t.takeException(), isNull);
    },
  );

  testWidgets('Off and failing desktops keep their picture greyed', (t) async {
    final source = FakeThumbnails();
    source.pictures['pve-151'] = picture(const Duration(hours: 2));
    source.pictures['pve-152'] = picture(const Duration(days: 3));
    await show(
      t,
      shell([
        vm('pve-151', state: 'stopped'),
        vm('pve-152', hostOnline: false),
      ], source),
    );
    for (final id in ['pve-151', 'pve-152']) {
      expect(art(t, id).thumbnail, isNotNull, reason: id);
      expect(art(t, id).dimmed, isTrue, reason: id);
      expect(art(t, id).snapshot, isTrue, reason: id);
    }
    // The caption says plainly that this is the stored last screen.
    expect(find.text('저장된 마지막 화면 · 2시간 전'), findsOneWidget);
    expect(find.text('저장된 마지막 화면 · 3일 전'), findsOneWidget);
    expect(t.takeException(), isNull);
  });

  testWidgets(
    'A powered-off desktop shows even a fresh picture as a snapshot',
    (t) async {
      final source = FakeThumbnails();
      source.pictures['pve-151'] = picture(const Duration(minutes: 2));
      await show(t, shell([vm('pve-151', state: 'stopped')], source));
      expect(art(t, 'pve-151').dimmed, isTrue);
      expect(art(t, 'pve-151').snapshot, isTrue);
      expect(caption(t, 'pve-151'), '저장된 마지막 화면 · 2분 전');
      expect(t.takeException(), isNull);
    },
  );

  testWidgets(
    'A replacement notice and the normal list refresh reload pictures',
    (t) async {
      final source = FakeThumbnails();
      await show(t, shell([vm('pve-149')], source));
      expect(art(t, 'pve-149').thumbnail, isNull);
      // A session window closed and wrote its last frame.
      source.pictures['pve-149'] = picture(Duration.zero);
      source.changes.add('pve-149');
      await t.pump();
      await t.pump();
      expect(source.refreshes, ['pve-149']);
      expect(art(t, 'pve-149').thumbnail, source.pictures['pve-149']!.image);
      expect(find.text('마지막 화면 · 방금'), findsOneWidget);
      // The list refresh (a new machines list) reads again and follows a
      // picture written in the meantime, or one that disappeared.
      final before = source.reads.length;
      source.pictures['pve-149'] = picture(const Duration(minutes: 1), seed: 1);
      await t.pumpWidget(shell([vm('pve-149')], source));
      await t.pump();
      await t.pump();
      expect(source.reads.length, greaterThan(before));
      expect(art(t, 'pve-149').thumbnail, source.pictures['pve-149']!.image);
      source.pictures.remove('pve-149');
      await t.pumpWidget(shell([vm('pve-149')], source));
      await t.pump();
      await t.pump();
      expect(art(t, 'pve-149').thumbnail, isNull);
      expect(find.byKey(const Key('thumbnail-caption-pve-149')), findsNothing);
      // Notices for other desktops or after sign-out change nothing.
      source.changes.add('pve-999');
      await t.pump();
      await t.pumpWidget(shell([vm('pve-149')], null));
      source.changes.add('pve-149');
      await t.pump();
      expect(art(t, 'pve-149').thumbnail, isNull);
      expect(source.refreshes, ['pve-149']);
      await source.changes.close();
    },
  );

  testWidgets(
    'Pictures are scoped to the signed-in account and deleted on sign-out',
    (t) async {
      final store = RecordingStore(), api = AccountBroker();
      final origin = Uri.parse('https://cloud.example.com');
      t.view.physicalSize = const Size(1440, 900);
      t.view.devicePixelRatio = 1;
      addTearDown(t.view.resetPhysicalSize);
      addTearDown(t.view.resetDevicePixelRatio);
      await t.pumpWidget(
        MultitapApp(api: api, serverOrigin: origin, thumbnails: store),
      );
      await t.pumpAndSettle();
      Future<void> login(String name) async {
        await t.enterText(find.byKey(const Key('sign-in-id')), name);
        await t.enterText(
          find.byKey(const Key('sign-in-password')),
          'test-password',
        );
        await t.tap(find.byKey(const Key('login-submit')));
        await t.pumpAndSettle();
      }

      Future<void> signOut() async {
        await t.tap(find.byKey(const Key('workspace-account')));
        await t.pumpAndSettle();
        await t.tap(find.byKey(const Key('sign-out')));
        await t.pumpAndSettle();
        await t.tap(find.byKey(const Key('confirm-sign-out')));
        await t.pumpAndSettle();
      }

      final first = thumbnailAccountKey(origin, 'account-one');
      await login('alice');
      // Signing in keeps only this account's pictures.
      expect(store.retained, [first]);
      expect(
        t.widget<WorkspaceShell>(find.byType(WorkspaceShell)).thumbnails,
        isA<AccountThumbnails>().having((v) => v.key, 'key', first),
      );
      await signOut();
      expect(store.forgotten, [first]);
      // A different account: its own key, and earlier folders are removed.
      api.account = 'account-two';
      await login('bob');
      final second = thumbnailAccountKey(origin, 'account-two');
      expect(second, isNot(first));
      expect(store.retained, [first, second]);
      expect(store.forgotten, [first]);
    },
  );

  testWidgets('A broker without account ids shows no pictures', (t) async {
    final store = RecordingStore(), api = TestBroker();
    t.view.physicalSize = const Size(1440, 900);
    t.view.devicePixelRatio = 1;
    addTearDown(t.view.resetPhysicalSize);
    addTearDown(t.view.resetDevicePixelRatio);
    await t.pumpWidget(
      MultitapApp(
        api: api,
        serverOrigin: Uri.parse('https://cloud.example.com'),
        thumbnails: store,
      ),
    );
    await t.pumpAndSettle();
    await t.enterText(find.byKey(const Key('sign-in-id')), 'alice');
    await t.enterText(find.byKey(const Key('sign-in-password')), 'pw');
    await t.tap(find.byKey(const Key('login-submit')));
    await t.pumpAndSettle();
    expect(
      t.widget<WorkspaceShell>(find.byType(WorkspaceShell)).thumbnails,
      isNull,
    );
    expect(store.retained, isEmpty);
  });
}
