// Real RemoteView/RemoteSession; all transport, media, clipboard and preferences
// below are fakes. No Windows API, native clipboard, file, service or network.
import 'dart:async';
import 'dart:convert';

import 'package:flutter/foundation.dart' show debugPrintSynchronously;
import 'package:flutter/material.dart';
import 'package:flutter/services.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:multitap_client/data/broker_api.dart';
import 'package:multitap_client/data/launcher_memory.dart';
import 'package:multitap_client/data/remote_session.dart';
import 'package:multitap_client/data/session_guide_memory.dart';
import 'package:multitap_client/data/transfer_notice_window.dart';
import 'package:multitap_client/models/desktop_machine.dart';
import 'package:multitap_client/models/session_display_mode.dart';
import 'package:multitap_client/ui/app_theme.dart';
import 'package:multitap_client/ui/remote_view.dart';
import 'package:multitap_client/ui/remote_viewport.dart';
import 'package:multitap_client/ui/session_guide_routes.dart';
import 'package:multitap_client/ui/session_guide_dialog.dart';
import 'package:multitap_client/ui/session_menu.dart' show SessionPopups;

import 'display_change_robustness_test.dart' show mockWindow;
import 'remote_session_test.dart' show SessionSocket, packet;
import 'viewport_session_test.dart'
    show DeferredCaptureMedia, ResizeMedia, startInfo;

Future<void> _until(WidgetTester tester, bool Function() done) async {
  for (var n = 0; n < 50 && !done(); n++) {
    await tester.pump(const Duration(milliseconds: 100));
  }
  expect(done(), isTrue, reason: 'Bounded fake guide state was not reached.');
}

Finder get _dialog => find.byKey(const Key('session-guide-dialog'));
String _key(String account) =>
    sessionGuideAccountKey(Uri.parse('https://fixture.invalid'), account)!;

/// 사용안내 where the real bar has it: a row of the 설정 panel,
/// or of the narrow bar's 메뉴. Opens the collapsed bar, then that panel or
/// menu, and returns the row to tap.
Future<Finder> _helpCommand(WidgetTester tester) async {
  final expand = find.byKey(const Key('expand-session-toolbar'));
  if (expand.hitTestable().evaluate().isNotEmpty) {
    await tester.tap(expand);
    await tester.pump(const Duration(milliseconds: 300));
  }
  final settings = find.byKey(const Key('remote-settings')).hitTestable();
  await tester.tap(
    settings.evaluate().isNotEmpty
        ? settings
        : find.byKey(const Key('session-toolbar-overflow')),
  );
  // Prime the popup's ticker, then let it come in.
  await tester.pump();
  await tester.pump(const Duration(milliseconds: 300));
  // The panel's row hands its key to its InkWell too: the first is the row.
  final help = find.byKey(const Key('remote-session-guide')).hitTestable();
  expect(help, findsWidgets);
  return help.first;
}

class _Store extends MemorySessionGuideStore {
  bool persistentCapability = true;
  @override
  bool get persistent => persistentCapability; // Test provider only; no actual file storage.
  bool failRead = false, failWrite = false;
  final gates = <String, Completer<bool>>{};
  final reads = <String>[];
  final writes = <(String, bool)>[];
  @override
  Future<bool> hidden(SessionGuideIdentity identity) async {
    reads.add(identity.storageKey);
    if (failRead) throw StateError('synthetic-read-failure');
    return gates[identity.storageKey]?.future ?? super.hidden(identity);
  }

  @override
  Future<void> setHidden(SessionGuideIdentity identity, bool hidden) async {
    writes.add((identity.storageKey, hidden));
    if (failWrite) throw StateError('synthetic-write-failure');
    await super.setHidden(identity, hidden);
  }
}

class _Preferences extends Fake
    implements LauncherMemory, SessionGuideStoreProvider {
  _Preferences(this.sessionGuideStore);
  @override
  final SessionGuideStore sessionGuideStore;
}

class _Notice extends Fake implements TransferNoticeWindow {
  @override
  VoidCallback? onCancel, onClose;
  @override
  Future<void> update(TransferNoticeContent content) async {}
  @override
  Future<void> hide() async {}
  @override
  Future<void> taskbar(TaskbarProgressMode mode, int percent) async {}
}

class _Stream {
  _Stream(
    int index, {
    bool viewport = false,
    ResizeMedia? native,
    int? scale,
    bool scaleControl = false,
  }) : media = native ?? ResizeMedia() {
    session = RemoteSession(
      connection: BrokerConnection(
        sessionId: 'guide-$index',
        webSocketUrl: Uri.parse('wss://fixture.invalid/sessions/guide-$index'),
        epochMedia: true,
        viewportProvisioning: viewport,
        displayScaleControl: scaleControl,
        displayScale: scale,
      ),
      token: 'synthetic-only',
      dialer: (_, _) async => socket,
      media: media,
    );
  }
  final socket = SessionSocket();
  final ResizeMedia media;
  late final RemoteSession session;
  Future<void> metadata(WidgetTester tester) async {
    await _until(tester, () => socket.incoming.hasListener);
    socket.incoming.add(jsonEncode(startInfo));
    await _until(tester, () => media.dimensions.isNotEmpty);
    await tester.pump();
  }

  Future<void> ready(WidgetTester tester, {bool metadataSent = false}) async {
    if (!metadataSent) await metadata(tester);
    socket.incoming.add(packet());
    await _until(tester, () => session.frameReady);
    await tester.pump();
  }
}

class _View {
  _View(
    this.store, {
    this.provider = true,
    this.mediaFactory,
    this.scaled = false,
    this.scaleGranted = true,
  }) {
    accountKey ??= _key('account-A');
    preferences = _Preferences(store);
  }
  final _Store store;
  String? accountKey;
  final bool provider;
  final bool viewport = false;
  final bool scaled;
  final bool scaleGranted;
  final scaleRequests = <int>[];
  final ResizeMedia Function(int)? mediaFactory;
  late final _Preferences preferences;
  final streams = <_Stream>[];
  final notice = _Notice();
  final routes = SessionGuideRoutes();
  _Stream create({int? scale}) {
    final value = _Stream(
      streams.length,
      viewport: viewport,
      native: mediaFactory?.call(streams.length),
      scale: scaleGranted ? scale : null,
      scaleControl: scaled && scaleGranted,
    );
    streams.add(value);
    return value;
  }

  RemoteSession get session => streams.last.session;
  SessionGuideIdentity get identity =>
      SessionGuideIdentity(accountKey: accountKey!, desktopId: 'pve-149');
  Widget tree() => MaterialApp(
    theme: buildAppTheme(brightness: Brightness.dark),
    navigatorObservers: [routes],
    home: RemoteView(
      machine: const DesktopMachine(
        id: 'pve-149',
        name: '안내 검사 VM',
        description: '',
        kind: MachineKind.vm,
        status: MachineStatus.ready,
        specification: 'Windows',
        accent: Colors.blue,
      ),
      guideAccountKey: accountKey,
      sessionGuideEnabled: true,
      pointerMemory: provider ? preferences : null,
      transferNoticeWindow: notice,
      initialDisplayMode: SessionDisplayMode.windowed,
      load: () async =>
          streams.isEmpty ? create().session : streams.first.session,
      onReconnect: (_) async => create().session,
      onConnectScaled: !scaled
          ? null
          : (_, _, scale) async {
              scaleRequests.add(scale);
              return create(scale: scale).session;
            },
      onRefreshDesktop: () async => const DesktopMachine(
        id: 'pve-149',
        name: '안내 검사 VM',
        description: '',
        kind: MachineKind.vm,
        status: MachineStatus.ready,
        specification: 'Windows',
        accent: Colors.blue,
        hostOnline: true,
      ),
    ),
  );
  Future<void> mount(WidgetTester tester) async {
    tester.view.devicePixelRatio = 1;
    tester.view.physicalSize = const Size(1366, 768);
    addTearDown(tester.view.resetDevicePixelRatio);
    addTearDown(tester.view.resetPhysicalSize);
    tester.binding.handleAppLifecycleStateChanged(AppLifecycleState.resumed);
    await mockWindow(tester);
    addTearDown(() async {
      for (final pending in store.gates.values) {
        if (!pending.isCompleted) pending.complete(false);
      }
      await tester.pumpWidget(const SizedBox.shrink());
      for (final stream in streams) {
        unawaited(stream.session.close());
      }
      await tester.pump(const Duration(seconds: 3));
      for (final stream in streams) {
        unawaited(stream.media.events.close());
      }
      routes.dispose();
      tester.binding.handleAppLifecycleStateChanged(AppLifecycleState.resumed);
    });
    await tester.pumpWidget(tree());
    await _until(
      tester,
      () => streams.isNotEmpty && streams.first.socket.incoming.hasListener,
    );
  }

  Future<void> manual(WidgetTester tester) async {
    // Open the actual collapsed toolbar, its 설정 panel, then the real help
    // command in it.
    await tester.tap(await _helpCommand(tester));
    await _until(tester, () => _dialog.evaluate().isNotEmpty);
  }
}

Future<void> _confirm(WidgetTester tester, {bool toggle = false}) async {
  if (toggle) {
    await tester.ensureVisible(find.byKey(const Key('session-guide-hide')));
    await tester.pump();
    await tester.tap(find.byKey(const Key('session-guide-hide')));
    await tester.pump();
  }
  await tester.tap(find.byKey(const Key('confirm-session-guide')));
  await _until(tester, () => _dialog.evaluate().isEmpty);
  await tester.pump();
}

// Measured fake-frame progression through the real toolbar/manual command.
// These values are not wall-clock input latency or installed-window evidence.
Future<List<Map<String, Object?>>> _manualResponseProbe(
  WidgetTester tester,
) async {
  final help = await _helpCommand(tester);
  final start = tester.binding.clock.now();
  await tester.tap(help);
  final result = <Map<String, Object?>>[];
  for (final delta in [
    Duration.zero,
    const Duration(milliseconds: 16),
    const Duration(milliseconds: 84),
  ]) {
    await tester.pump(delta);
    final elements = _dialog.evaluate().toList();
    final animation = elements.isEmpty
        ? null
        : ModalRoute.of(elements.single)?.animation?.value;
    result.add({
      'fakeElapsedUs': tester.binding.clock
          .now()
          .difference(start)
          .inMicroseconds,
      'dialogs': elements.length,
      'routeAnimation': animation,
      'renderable': elements.length == 1 && (animation ?? 1) > 0,
    });
  }
  debugPrintSynchronously('guide_manual_response ${jsonEncode(result)}');
  return result;
}

Future<void> _closeProbeGuide(WidgetTester tester) async {
  if (_dialog.evaluate().isNotEmpty) {
    await tester.tap(find.byKey(const Key('close-session-guide')));
  }
  // Prime the exit ticker without advancing fake time, then observe the
  // bounded close while any fake native acknowledgement remains outstanding.
  await tester.pump();
  await tester.pump(const Duration(milliseconds: 400));
  await tester.pump();
}

void main() {
  testWidgets(
    'stream entry and manual close never open help or read old opt-out',
    (tester) async {
      final view = _View(_Store());
      await view.mount(tester);
      await view.streams.first.ready(tester);
      await tester.pump(const Duration(seconds: 3));
      expect(_dialog, findsNothing);
      expect(view.store.reads, isEmpty);
      await view.manual(tester);
      expect(_dialog, findsOneWidget);
      expect(find.byKey(const Key('session-guide-hide')), findsNothing);
      await _confirm(tester);
      await tester.pump(const Duration(seconds: 3));
      expect(_dialog, findsNothing);
      expect(view.store.writes, isEmpty);
      expect(view.session.closed, isFalse);
    },
    variant: TargetPlatformVariant.only(TargetPlatform.windows),
  );

  testWidgets(
    'manual help can reopen while previous native release is pending',
    (tester) async {
      final native = DeferredCaptureMedia();
      final view = _View(
        _Store(),
        provider: false,
        mediaFactory: (_) => native,
      );
      await view.mount(tester);
      await view.streams.first.ready(tester);
      await tester.tap(find.byType(RemoteViewport));
      await tester.pump();
      final release = Completer<void>();
      native.release = release;
      try {
        final first = await _manualResponseProbe(tester);
        expect(first.any((row) => row['renderable'] == true), isTrue);
        await _closeProbeGuide(tester);
        final second = await _manualResponseProbe(tester);
        expect(second.any((row) => row['renderable'] == true), isTrue);
        expect(release.isCompleted, isFalse);
        expect(view.store.reads, isEmpty);
        await _closeProbeGuide(tester);
        expect(view.session.closed, isFalse);
        expect(tester.takeException(), isNull);
      } finally {
        release.complete();
        await tester.pump();
      }
    },
    variant: TargetPlatformVariant.only(TargetPlatform.windows),
  );

  testWidgets(
    'Windows first and later scale selections keep the granted session',
    (tester) async {
      final view = _View(_Store(), provider: false, scaled: true);
      await view.mount(tester);
      await view.streams.first.ready(tester);
      Future<void> openScale() async {
        if (find
            .byKey(const Key('expand-session-toolbar'))
            .hitTestable()
            .evaluate()
            .isNotEmpty) {
          await tester.tap(find.byKey(const Key('expand-session-toolbar')));
          await tester.pump(const Duration(milliseconds: 300));
        }
        // 배율 is in the 화면 panel, under the screen modes.
        await tester.tap(find.byKey(const Key('stream-menu-화면')));
        await tester.pump();
        await tester.pump(const Duration(milliseconds: 200));
        expect(find.byKey(const Key('session-screen-panel')), findsOneWidget);
        expect(find.byKey(const Key('remote-display-scale')), findsOneWidget);
        expect(find.byKey(const Key('display-scale-panel')), findsOneWidget);
        expect(
          find.descendant(
            of: find.byKey(const Key('display-scale-steps')),
            matching: find.byType(TextButton),
          ),
          findsNWidgets(5),
        );
        expect(find.byKey(const Key('display-resolution-125')), findsNothing);
      }

      await openScale();
      final current = view.session;
      expect(current.connection.displayScale, 100);
      expect(current.displayScaleWanted, 100);
      await tester.tap(find.byKey(const Key('display-scale-125')));
      await tester.pump();
      expect(view.streams, hasLength(1));
      expect(identical(current, view.session), isTrue);
      expect(current.closed, isFalse);
      expect(view.scaleRequests, [100]);
      expect(current.displayScaleWanted, 125);
      await openScale();
      await tester.tap(find.byKey(const Key('display-scale-150')));
      await tester.pump();
      expect(identical(current, view.session), isTrue);
      expect(view.streams, hasLength(1));
      expect(view.scaleRequests, [100]);
      expect(view.session.displayScaleWanted, 150);
      final messages = view.streams.last.socket.sent;
      expect(
        messages.where((value) => value['type'] == 'display.scale').toList(),
        [
          {'type': 'display.scale', 'scale': 125},
          {'type': 'display.scale', 'scale': 150},
        ],
      );
      expect(tester.takeException(), isNull);
    },
    variant: TargetPlatformVariant.only(TargetPlatform.windows),
  );

  testWidgets(
    'Windows ungranted scale stays disabled without reconnect attempts',
    (tester) async {
      // A reconnect callback alone is not a current-session broker grant.
      final view = _View(
        _Store(),
        provider: false,
        scaled: true,
        scaleGranted: false,
      );
      await view.mount(tester);
      await view.streams.first.ready(tester);
      final current = view.session;
      expect(current.connection.displayScale, isNull);
      expect(current.displayScaleWanted, isNull);
      for (var attempt = 0; attempt < 2; attempt++) {
        if (find
            .byKey(const Key('expand-session-toolbar'))
            .hitTestable()
            .evaluate()
            .isNotEmpty) {
          await tester.tap(find.byKey(const Key('expand-session-toolbar')));
          await tester.pump(const Duration(milliseconds: 300));
        }
        // 배율 is in the 화면 panel, under the screen modes.
        await tester.tap(find.byKey(const Key('stream-menu-화면')));
        await tester.pump();
        await tester.pump(const Duration(milliseconds: 300));
        for (final scale in [100, 125, 150, 175, 200]) {
          final button = tester.widget<TextButton>(
            find.byKey(Key('display-scale-$scale')),
          );
          expect(button.onPressed, isNull);
        }
        expect(
          find.text('이 연결에서는 배율을 바꿀 수 없어요.'),
          findsOneWidget,
          reason: 'the steps stay, greyed, with the reason',
        );
        Navigator.of(
          tester.element(find.byKey(const Key('display-scale-panel'))),
        ).pop();
        await tester.pump(const Duration(milliseconds: 300));
      }
      expect(view.streams, hasLength(1));
      expect(view.scaleRequests, [100]);
      expect(identical(current, view.session), isTrue);
      expect(current.closed, isFalse);
      expect(tester.takeException(), isNull);
    },
    variant: TargetPlatformVariant.only(TargetPlatform.windows),
  );

  testWidgets('manual panel fallback keeps content-only preference policy', (
    tester,
  ) async {
    final view = _View(_Store(), provider: false);
    await view.mount(tester);
    await view.streams.first.ready(tester);
    tester.view.physicalSize = const Size(320, 640);
    await tester.pump();
    final presentation = ValueNotifier(
      const SessionGuidePresentation(canRemember: true, hidden: true),
    );
    final popups = SessionPopups();
    addTearDown(presentation.dispose);
    addTearDown(popups.closeAll);
    final context = tester.element(find.byType(RemoteView));
    final route = createSessionGuidePanelRoute(
      context,
      anchor: null,
      machine: '안내 검사 VM',
      session: view.session,
      presentation: presentation,
      popups: popups,
      showPreference: false,
    );
    bool? result;
    var completed = false;
    unawaited(
      Navigator.of(context, rootNavigator: true).push(route).then<void>((
        value,
      ) {
        result = value;
        completed = true;
      }),
    );
    await tester.pump();
    await tester.pump(const Duration(milliseconds: 400));
    expect(find.byType(Dialog), findsOneWidget);
    expect(find.byKey(const Key('session-guide-hide')), findsNothing);
    await _confirm(tester);
    await tester.pump(const Duration(milliseconds: 400));
    expect(completed, isTrue);
    expect(result, isNull);
    expect(view.store.reads, isEmpty);
    expect(view.store.writes, isEmpty);
    expect(tester.takeException(), isNull);
  }, variant: TargetPlatformVariant.only(TargetPlatform.windows));

  testWidgets(
    'narrow overflow opens manual guide without waiting for its outgoing menu',
    (tester) async {
      final view = _View(_Store(), provider: false);
      await view.mount(tester);
      tester.view.physicalSize = const Size(320, 640);
      await tester.pump();
      await view.streams.first.ready(tester);
      await tester.tap(find.byKey(const Key('expand-session-toolbar')));
      await tester.pump(const Duration(milliseconds: 300));
      expect(find.byKey(const Key('remote-session-guide')), findsNothing);
      await tester.tap(find.byKey(const Key('session-toolbar-overflow')));
      // Prime the incoming popup's ticker before waiting for its clipped row.
      // A keyed PopupMenuItem can already exist outside the painted hit area.
      await tester.pump();
      final help = find.byKey(const Key('remote-session-guide'));
      await _until(tester, () => help.hitTestable().evaluate().length == 1);
      final outgoing = ModalRoute.of(tester.element(help))!;
      expect(outgoing, isA<PopupRoute<dynamic>>());
      final reverse =
          (outgoing as PopupRoute<dynamic>).reverseTransitionDuration;
      expect(reverse, greaterThan(const Duration(milliseconds: 16)));
      var outgoingCompleted = false;
      outgoing.completed.then((_) => outgoingCompleted = true);
      expect(outgoingCompleted, isFalse);
      final selectedAt = tester.binding.clock.now();
      await tester.tap(help.hitTestable());
      await tester.pump();
      await tester.pump(const Duration(milliseconds: 16));
      expect(_dialog, findsOneWidget);
      expect(
        tester.binding.clock.now().difference(selectedAt),
        lessThan(reverse),
        reason: 'Manual help opens before a natural outgoing reverse transition can finish; cancelling the prior owned popup is allowed.',
      );
      expect(
        find.byType(Dialog),
        findsNothing,
        reason: 'Manual help adapts its anchored width instead of becoming a modal.',
      );
      final bounds = tester.getRect(
        find.byKey(const Key('session-guide-panel')),
      );
      expect(bounds.left, greaterThanOrEqualTo(8));
      expect(bounds.right, lessThanOrEqualTo(312));
      expect(bounds.bottom, lessThanOrEqualTo(632));
      expect(find.byKey(const Key('session-guide-hide')), findsNothing);
      expect(view.store.reads, isEmpty);
      expect(
        find.byKey(const Key('close-session-guide')).hitTestable(),
        findsOneWidget,
      );
      expect(
        find.byKey(const Key('confirm-session-guide')).hitTestable(),
        findsOneWidget,
      );
      await _closeProbeGuide(tester);
      expect(view.store.writes, isEmpty);
      expect(view.session.closed, isFalse);
      expect(tester.takeException(), isNull);
    },
    variant: TargetPlatformVariant.only(TargetPlatform.windows),
  );

  for (final exit in ['escape', 'outside', 'resize', 'focus', 'disconnect']) {
    testWidgets(
      'manual anchored guide cleans up on $exit without restoring stale input',
      (tester) async {
        final view = _View(_Store(), provider: false);
        await view.mount(tester);
        await view.streams.first.ready(tester);
        await view.manual(tester);
        await tester.pump(const Duration(milliseconds: 300));
        final panel = find.byKey(const Key('session-guide-panel'));
        expect(panel, findsOneWidget);
        final bounds = tester.getRect(panel);
        expect(bounds.left, greaterThanOrEqualTo(8));
        expect(bounds.right, lessThanOrEqualTo(1366 - 8));
        expect(bounds.bottom, lessThanOrEqualTo(768 - 8));
        switch (exit) {
          case 'escape':
            await tester.sendKeyEvent(LogicalKeyboardKey.escape);
          case 'outside':
            await tester.tapAt(const Offset(12, 744));
          case 'resize':
            tester.view.physicalSize = const Size(1000, 640);
          case 'focus':
            tester.binding.handleAppLifecycleStateChanged(
              AppLifecycleState.inactive,
            );
          case 'disconnect':
            await view.session.close();
        }
        await _until(tester, () => _dialog.evaluate().isEmpty);
        await tester.pump(const Duration(milliseconds: 400));
        expect(view.session.captured, isFalse);
        expect(view.store.writes, isEmpty);
        expect(view.session.closed, exit == 'disconnect');
        if (exit == 'focus') {
          tester.binding.handleAppLifecycleStateChanged(
            AppLifecycleState.resumed,
          );
        }
        expect(tester.takeException(), isNull);
      },
      variant: TargetPlatformVariant.only(TargetPlatform.windows),
    );
  }
}
