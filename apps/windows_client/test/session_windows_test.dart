// Exercises actual view/controller cleanup with injected Windows/relay boundaries.
import 'dart:async';

import 'package:flutter/material.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:multitap_client/data/broker_api.dart';
import 'package:multitap_client/data/remote_session.dart';
import 'package:multitap_client/data/session_windows.dart';
import 'package:multitap_client/data/window_close_guard.dart';
import 'package:multitap_client/models/session_display_mode.dart';
import 'package:multitap_client/models/stream_options.dart';
import 'package:multitap_client/ui/app_dialog.dart';
import 'package:multitap_client/ui/remote_view.dart';
import 'package:multitap_client/ui/session_window.dart';
import 'package:multitap_client/ui/workspace_shell.dart';

import 'session_recovery_test.dart'
    show machine, session, RecoveryMedia, mockPresentation;
import 'remote_session_test.dart' show SessionSocket;
import 'helpers/stream_connect_consent.dart';

class Guard implements WindowCloseGuard {
  Future<void> Function()? request;
  int closes = 0;
  @override
  void attach(Future<void> Function() value) => request = value;
  @override
  Future<bool> update({required bool busy, required bool dirty}) async => true;
  @override
  Future<bool> close() async {
    closes++;
    return true;
  }

  @override
  void dispose() {}
}

class PendingApi extends Fake implements BrokerApi {
  final inventory = Completer<List<BrokerDesktop>>();
  int closes = 0;
  @override
  Future<List<BrokerDesktop>> desktops(String token) => inventory.future;
  @override
  void close() {
    closes++;
  }
}

SessionWindowLaunch launch() => SessionWindowLaunch(
  origin: Uri.parse('https://fixture.example'),
  token: 'fixture-ephemeral-token',
  expiresAt: DateTime.now().add(const Duration(hours: 1)),
  desktopId: 'pve-149',
  name: 'TESTER',
  mode: SessionDisplayMode.windowed,
  options: const StreamOptions(),
);

Future<void> frames(WidgetTester t) async {
  await t.pump();
  await t.pump(const Duration(milliseconds: 350));
  await t.pump();
}

void main() {
  TestWidgetsFlutterBinding.ensureInitialized();
  final messenger =
      TestDefaultBinaryMessengerBinding.instance.defaultBinaryMessenger;
  setUp(() {
    messenger.setMockMethodCallHandler(
      SessionWindows.channel,
      (_) async => null,
    );
  });
  tearDown(
    () => messenger.setMockMethodCallHandler(SessionWindows.channel, null),
  );

  test(
    'Native bootstrap carries only in-memory launch and validates server/mode',
    () async {
      final value = launch();
      messenger.setMockMethodCallHandler(
        SessionWindows.channel,
        (call) async => call.method == 'bootstrap' ? value.toMap() : null,
      );
      final read = await SessionWindows.bootstrap();
      expect(read!.desktopId, value.desktopId);
      expect(read.token, value.token);
      expect(
        () => SessionWindowLaunch.fromMap({
          ...value.toMap(),
          'origin': 'http://fixture.example',
        }),
        throwsA(anything),
      );
      expect(
        () => SessionWindowLaunch.fromMap({
          ...value.toMap(),
          'desktopId': '../other',
        }),
        throwsFormatException,
      );
      expect(
        SessionWindowLaunch.fromMap({...value.toMap(), 'mode': 'allMonitors'})
            .mode,
        SessionDisplayMode.allMonitors,
      );
      // Expiry is handled by cleanup, not an uncaught error before runApp.
      expect(
        SessionWindowLaunch.fromMap({
          ...value.toMap(),
          'expiresAt': '2020-01-01T00:00:00Z',
        }).expiresAt.year,
        2020,
      );
    },
  );

  testWidgets('Streaming opens a child and keeps the workspace visible', (
    t,
  ) async {
    final opened = <SessionDisplayMode>[];
    await t.pumpWidget(
      MaterialApp(
        home: WorkspaceShell(
          onSignOut: () {},
          onRefresh: () {},
          machines: const [machine],
          displayName: 'fixture',
          serverOrigin: 'https://fixture.example',
          onOpenWindow: (vm, mode, options) async {
            opened.add(mode);
            return null;
          },
          onConnect: (_, {options = const StreamOptions()}) async =>
              throw StateError('Dashboard must not own media'),
        ),
      ),
    );
    await t.tap(find.byKey(const Key('stream-modes-qa')));
    await frames(t);
    await t.tap(find.byKey(const Key('stream-mode-windowed')));
      await acceptStreamConnect(t, 'qa');
    await frames(t);
    expect(opened, [SessionDisplayMode.windowed]);
    expect(find.byType(WorkspaceShell), findsOneWidget);
    expect(find.byType(RemoteView), findsNothing);
  });

  testWidgets(
    'Stream X cancel preserves session; accept releases transport before child closes',
    (t) async {
      mockPresentation();
      final socket = SessionSocket(), media = RecoveryMedia();
      final current = session(socket, media);
      var closes = 0;
      final actions = SessionWindowActions(
        closeWindow: () async {
          expectSync(current.closed, isTrue);
          expectSync(media.stopped, isTrue);
          expectSync(socket.wasClosed, isTrue);
          closes++;
        },
      );
      await t.pumpWidget(
        MaterialApp(
          home: RemoteView(
            machine: machine,
            load: () async => current,
            initialDisplayMode: SessionDisplayMode.windowed,
            windowActions: actions,
          ),
        ),
      );
      await t.pump();
      await t.pump();
      await t.pump(const Duration(milliseconds: 20));
      unawaited(actions.requestClose());
      unawaited(actions.requestClose());
      await frames(t);
      expect(find.byType(AppConfirmDialog), findsOneWidget);
      await t.tap(find.text('취소'));
      await frames(t);
      expect(current.closed, isFalse);
      expect(closes, 0);
      unawaited(actions.requestClose());
      await frames(t);
      await t.tap(find.widgetWithText(FilledButton, '연결 끊기'));
      await frames(t);
      expect(closes, 1);
      expect(
        socket.sent.where((value) => value['type'] == 'session.stop'),
        hasLength(1),
      );
      await t.pumpWidget(const SizedBox());
      await t.pump();
    },
  );

  testWidgets('"원격 창 닫기" after a failed stream closes without asking', (
    t,
  ) async {
    mockPresentation();
    var closes = 0;
    final actions = SessionWindowActions(
      closeWindow: () async {
        closes++;
      },
    );
    await t.pumpWidget(
      MaterialApp(
        home: RemoteView(
          machine: machine,
          load: () async => throw const BrokerException('desktop_offline'),
          initialDisplayMode: SessionDisplayMode.windowed,
          windowActions: actions,
        ),
      ),
    );
    await frames(t);
    await t.tap(find.text('원격 창 닫기'));
    await frames(t);
    expect(find.byType(AppConfirmDialog), findsNothing);
    expect(closes, 1);
    await t.pumpWidget(const SizedBox());
    await t.pump();
  });

  testWidgets('Closing a loading stream cannot start a late session', (
    t,
  ) async {
    mockPresentation();
    final ready = Completer<RemoteSession>();
    final socket = SessionSocket(), media = RecoveryMedia();
    final late = session(socket, media);
    var closes = 0;
    final actions = SessionWindowActions(
      closeWindow: () async {
        closes++;
      },
    );
    await t.pumpWidget(
      MaterialApp(
        home: RemoteView(
          machine: machine,
          load: () => ready.future,
          initialDisplayMode: SessionDisplayMode.windowed,
          windowActions: actions,
        ),
      ),
    );
    await t.pump();
    await t.pump();
    await t.pump(const Duration(milliseconds: 20));
    await actions.retire();
    ready.complete(late);
    await t.pump();
    await t.pump();
    expect(closes, 1);
    expect(late.closed, isTrue);
    expect(media.starts, 0);
    await t.pumpWidget(const SizedBox());
    await t.pump();
  });

  testWidgets(
    'Child close during inventory leaves auth service local and rejects late inventory',
    (t) async {
      final api = PendingApi(), guard = Guard();
      await t.pumpWidget(
        MaterialApp(
          home: SessionWindow(launch: launch(), api: api, guard: guard),
        ),
      );
      // Still loading, nothing to end: the close is not asked.
      unawaited(guard.request!());
      await frames(t);
      expect(find.byType(AppConfirmDialog), findsNothing);
      expect(guard.closes, 1);
      expect(api.closes, 1);
      api.inventory.complete([]);
      await t.pump();
      expect(find.byType(RemoteView), findsNothing);
      await t.pumpWidget(const SizedBox());
      await t.pump();
    },
  );
}
