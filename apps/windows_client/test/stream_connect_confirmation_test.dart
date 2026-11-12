// PRIVATE universal-confirmation RED. Actual WorkspaceShell, fake boundaries.
import 'package:flutter/material.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:multitap_client/data/broker_api.dart';
import 'package:multitap_client/data/remote_session.dart';
import 'package:multitap_client/data/session_windows.dart' show SessionWindowOpenResult;
import 'package:multitap_client/models/desktop_machine.dart';
import 'package:multitap_client/models/session_cleanup.dart';
import 'package:multitap_client/models/session_display_mode.dart';
import 'package:multitap_client/models/stream_options.dart';
import 'package:multitap_client/ui/app_theme.dart';
import 'package:multitap_client/ui/stream_settings_dialog.dart';
import 'package:multitap_client/ui/workspace_shell.dart';

import 'helpers/stream_connect_consent.dart' show acceptStreamConnect;
import 'session_recovery_test.dart' show mockPresentation;

DesktopMachine _machine({bool pending = false, bool off = false}) => DesktopMachine(
  id: 'qa', name: 'ToTBCode', description: '', kind: MachineKind.vm,
  status: off ? MachineStatus.poweredOff : MachineStatus.ready,
  specification: 'Windows', accent: Colors.blue, hostOnline: !off,
  interactiveSession: !off, virtualDisplayReady: !off,
  powerState: off ? 'stopped' : 'running', powerStale: false,
  sessionCleanup: pending ? SessionCleanupState(pending: true, phase: 'pending',
    checkedAt: DateTime.now()) : const SessionCleanupState(),
);

class _Fixture {
  _Fixture({this.detached = true, bool pending = false, bool off = false})
      : machines = [_machine(pending: pending, off: off)];
  final bool detached;
  List<DesktopMachine> machines;
  String server = 'https://fixture.invalid', account = 'account-a';
  final changed = ValueNotifier(0);
  int opens = 0, connects = 0;
  SessionDisplayMode? openedMode;
  Future<RemoteSession> connect(String id, {StreamOptions options = const StreamOptions()}) async {
    connects++;
    throw const BrokerException('host_unavailable');
  }
  Future<SessionWindowOpenResult?> open(DesktopMachine machine, SessionDisplayMode mode, StreamOptions options) async {
    opens++;
    openedMode = mode;
    return null;
  }
  Future<void> mount(WidgetTester t) async {
    t.view.physicalSize = const Size(1440, 1000);
    t.view.devicePixelRatio = 1;
    addTearDown(t.view.reset);
    mockPresentation();
    await t.pumpWidget(MaterialApp(theme: buildAppTheme(),
      home: ValueListenableBuilder<int>(valueListenable: changed,
        builder: (_, value, _) => WorkspaceShell(
          displayName: 'tester', guideAccountKey: account, serverOrigin: server,
          onSignOut: () {}, onRefresh: () {}, machines: machines,
          localMonitors: 1, onConnect: connect,
          onOpenWindow: detached ? open : null,
        ),
      ),
    ));
    await t.pumpAndSettle();
  }
  Future<void> enter(WidgetTester t, String entry) async {
    if (entry == 'fullscreen' || entry == 'quality') {
      await t.tap(find.byKey(const Key('stream-modes-qa')));
      await t.pumpAndSettle();
      await t.tap(find.byKey(Key(entry == 'fullscreen' ? 'stream-mode-fullscreen' : 'stream-settings-qa')));
      await t.pumpAndSettle();
      if (entry == 'quality') {
        expect(find.byType(StreamSettingsDialog), findsOneWidget);
        await t.tap(find.byKey(const Key('apply-stream-settings')));
      }
    } else {
      await t.tap(find.byKey(const Key('open-qa')));
    }
    // A queued cleanup progress indicator need not settle.
    await t.pump(const Duration(milliseconds: 350));
  }
  Future<void> close(WidgetTester t) async {
    await t.pumpWidget(const SizedBox.shrink());
    await t.pumpAndSettle();
    changed.dispose();
  }
}

void main() {
  for (final entry in ['card-detached', 'card-inline', 'fullscreen', 'quality', 'queued']) {
    testWidgets('Windows $entry fresh Connect asks before any launch or reservation', (t) async {
      final f = _Fixture(detached: entry != 'card-inline', pending: entry == 'queued');
      try {
        await f.mount(t);
        await f.enter(t, entry);
        final dialog = find.byKey(const Key('connect-confirm-dialog'));
        expect(dialog, findsOneWidget, reason: 'Idle and occupied fresh stream intents both require the first question');
        expect(find.text('ToTBCode에 연결할까요?'), findsOneWidget);
        expect((f.opens, f.connects), (0, 0));
        expect(find.byKey(const Key('cancel-start-qa')), findsNothing);
        await t.tap(find.byKey(const Key('cancel-connect-qa')));
        await t.pump();
        await t.pump(const Duration(milliseconds: 350));
        await t.pump();
        expect(dialog, findsNothing);
        expect((f.opens, f.connects), (0, 0));
        expect(find.byKey(const Key('cancel-start-qa')), findsNothing);
        await f.enter(t, entry);
        await acceptStreamConnect(t, 'qa');
        if (entry == 'queued') {
          expect((f.opens, f.connects), (0, 0));
          expect(find.byKey(const Key('cancel-start-qa')), findsOneWidget);
          f.machines = [_machine()];
          f.changed.value++;
          await t.pump();
          await t.pump(const Duration(milliseconds: 350));
        }
        await t.pump(const Duration(milliseconds: 350));
        expect((f.opens, f.connects), entry == 'card-inline' ? (0, 1) : (1, 0));
        expect(dialog, findsNothing, reason: 'Accepted queued fulfillment and continuation do not ask twice');
        if (entry == 'fullscreen') expect(f.openedMode, SessionDisplayMode.fullscreen);
      } finally { await f.close(t); }
    });
  }

  testWidgets('Windows pending confirmation cannot survive an owner/origin round trip', (t) async {
    final f = _Fixture();
    try {
      await f.mount(t);
      await f.enter(t, 'card-detached');
      expect(find.byKey(const Key('connect-confirm-dialog')), findsOneWidget);
      f.server = 'https://other.invalid'; f.account = 'account-b'; f.changed.value++;
      await t.pump();
      f.server = 'https://fixture.invalid'; f.account = 'account-a'; f.changed.value++;
      await t.pump();
      await acceptStreamConnect(t, 'qa');
      expect((f.opens, f.connects), (0, 0));
    } finally { await f.close(t); }
  });

  testWidgets('Windows stopped target has no stream dispatch or streaming question', (t) async {
    final f = _Fixture(off: true);
    try {
      await f.mount(t);
      expect(find.byKey(const Key('open-qa')), findsNothing);
      expect(find.byKey(const Key('connect-confirm-dialog')), findsNothing);
      expect((f.opens, f.connects), (0, 0));
    } finally { await f.close(t); }
  });
}
