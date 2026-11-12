// A broker update restarts the server: nginx answers 502 for a few
// seconds. Fake brokers, sockets and media for the
// Windows screens; the loopback HTTP parsing and diagnostics cases are in
// broker_gateway_recovery_test.dart (a widget binding fakes HttpClient).
import 'dart:async';
import 'dart:io';

import 'package:flutter/material.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:multitap_client/app.dart';
import 'package:multitap_client/data/broker_api.dart';
import 'package:multitap_client/data/broker_probe.dart';
import 'package:multitap_client/data/session_windows.dart';
import 'package:multitap_client/data/window_close_guard.dart';
import 'package:multitap_client/models/stream_options.dart';
import 'package:multitap_client/ui/remote_view.dart';
import 'package:multitap_client/ui/session_window.dart';
import 'package:multitap_client/ui/sign_in_screen.dart';

import 'display_change_robustness_test.dart' show ViewFixture, mockWindow;
import 'display_remote_test.dart' show machine;
import 'remote_session_test.dart' show SessionSocket;
import 'session_recovery_test.dart' show mockPresentation;
import 'session_window_lifecycle_test.dart' show desktop, launch;
import 'viewport_session_test.dart' show ResizeMedia;
import 'workspace_live_state_test.dart' show LiveUpdates, liveDesktop;
import 'workspace_updates_race_test.dart'
    show RaceBroker, drain, mount, signIn, workspace;

const _restart = BrokerException('server_restarting', 502);

/// The list read meets [restarts] proxy 502s before the broker is back.
class _RestartBroker extends RaceBroker {
  int restarts = 0;
  @override
  Future<List<BrokerDesktop>> desktops(String token) async {
    if (restarts > 0) {
      restarts--;
      reads++;
      throw _restart;
    }
    return super.desktops(token);
  }
}

class _Guard implements WindowCloseGuard {
  @override
  void attach(Future<void> Function() value) {}
  @override
  Future<bool> update({required bool busy, required bool dirty}) async => true;
  @override
  Future<bool> close() async => true;
  @override
  void dispose() {}
}

class _WindowApi extends Fake implements BrokerApi {
  int reads = 0, restarts = 1;
  @override
  Future<List<BrokerDesktop>> desktops(String token) async {
    reads++;
    if (restarts > 0) {
      restarts--;
      throw _restart;
    }
    return [desktop()];
  }

  @override
  Future<BrokerConnection> connect(
    String token,
    String id, {
    String mode = 'stream',
    StreamOptions? options,
  }) async => throw const BrokerException('host_unavailable');

  @override
  void close() {}
}

void main() {
  test('ServerRestartWait: 1, 2, 4, 8 s, then 8 s for up to two minutes; '
      'anything else ends it', () {
    var now = DateTime.utc(2026, 11, 2, 7, 24);
    final wait = ServerRestartWait(clock: () => now);
    expect(
      [for (var i = 0; i < 6; i++) wait.next(_restart)?.inSeconds],
      [1, 2, 4, 8, 8, 8],
    );
    expect(wait.waiting, isTrue);
    now = now.add(ServerRestartWait.window);
    expect(wait.next(_restart), isNull, reason: 'the restart outlasted it');
    expect(wait.waiting, isTrue, reason: 'given up until a reset');
    expect(wait.next(_restart), isNull);
    wait.reset();
    expect(wait.waiting, isFalse);
    const refused = BrokerException('network_error', 0, 'connection_refused');
    const reset = BrokerException('network_error', 0, 'connection_reset');
    expect(wait.next(refused)?.inSeconds, 1);
    expect(wait.next(reset)?.inSeconds, 2);
    for (final other in <Object>[
      const BrokerException('invalid_response', 200),
      const BrokerException('invalid_response', 502),
      const BrokerException('proxmox_unreachable', 502),
      const BrokerException('network_error'),
      const BrokerException('network_error', 0, 'timeout'),
      StateError('fixture'),
    ]) {
      expect(wait.next(other), isNull, reason: '$other');
      expect(wait.waiting, isFalse, reason: 'it ends the wait: $other');
    }
    // The error text of a restart that is not waited for (or outlasted the wait).
    expect(_restart.message, '서버가 응답하지 않아요. 잠시 후 다시 시도하세요.');
    expect(_restart.serverRestarting, isTrue);
    expect(refused.serverRestarting, isTrue);
    expect(const BrokerException('network_error').serverRestarting, isFalse);
  });

  test('A reset socket is named connection_reset', () {
    for (final code in [54, 104, 10054]) {
      expect(
        brokerSocketFailureCode(
          SocketException('synthetic', osError: OSError('synthetic', code)),
        ),
        'connection_reset',
      );
    }
  });

  testWidgets(
    'A refresh pressed while the server restarts shows one line, keeps the list and retries by itself',
    (tester) async {
      final api = _RestartBroker()..assigned = [liveDesktop('running')];
      final updates = LiveUpdates();
      await mount(tester, api, (_, _) => updates);
      expect(await signIn(tester), SignInResult.success);
      updates.ready();
      await drain(tester);
      final before = api.reads;
      api.restarts = 2;
      workspace(tester).onRefresh();
      await drain(tester);
      expect(api.reads, before + 1);
      expect(workspace(tester).notice, serverRestartingMessage);
      expect(workspace(tester).error, isNull);
      expect(find.text(serverRestartingMessage), findsOneWidget);
      expect(find.byKey(const Key('workspace-error')), findsNothing);
      expect(workspace(tester).machines.single.id, 'live-vm');
      await tester.pump(const Duration(seconds: 1));
      await drain(tester);
      expect(api.reads, before + 2);
      expect(workspace(tester).notice, serverRestartingMessage);
      await tester.pump(const Duration(seconds: 2));
      await drain(tester);
      expect(api.reads, before + 3);
      expect(workspace(tester).notice, isNull);
      expect(workspace(tester).error, isNull);
      expect(find.text(serverRestartingMessage), findsNothing);
      expect(workspace(tester).machines.single.id, 'live-vm');
    },
  );

  testWidgets(
    'A quiet read while the server restarts shows no error and is tried again within a second',
    (tester) async {
      final api = _RestartBroker()..assigned = [liveDesktop('running')];
      final updates = LiveUpdates();
      await mount(tester, api, (_, _) => updates);
      expect(await signIn(tester), SignInResult.success);
      updates.ready();
      await drain(tester);
      final before = api.reads;
      api.restarts = 1;
      updates.changed();
      await drain(tester);
      expect(api.reads, before + 1);
      expect(workspace(tester).error, isNull);
      expect(workspace(tester).notice, isNull);
      expect(workspace(tester).machines.single.id, 'live-vm');
      await tester.pump(const Duration(seconds: 1));
      await drain(tester);
      expect(api.reads, greaterThanOrEqualTo(before + 2));
      expect(api.restarts, 0);
      expect(workspace(tester).error, isNull);
      expect(workspace(tester).notice, isNull);
    },
  );

  testWidgets('A restart that outlasts two minutes shows its error', (
    tester,
  ) async {
    var now = DateTime.now();
    final api = _RestartBroker()..assigned = [liveDesktop('running')];
    final updates = LiveUpdates();
    tester.view.physicalSize = const Size(1440, 900);
    tester.view.devicePixelRatio = 1;
    addTearDown(tester.view.reset);
    await tester.pumpWidget(
      MultitapApp(
        api: api,
        serverOrigin: Uri.parse('https://cloud.example.com'),
        desktopWatchFactory: (_, _) => updates,
        clock: () => now,
      ),
    );
    await tester.pumpAndSettle();
    expect(await signIn(tester), SignInResult.success);
    updates.ready();
    await drain(tester);
    api.restarts = 1000;
    workspace(tester).onRefresh();
    await drain(tester);
    expect(workspace(tester).notice, serverRestartingMessage);
    now = now.add(const Duration(minutes: 2, seconds: 1));
    await tester.pump(const Duration(seconds: 1));
    await drain(tester);
    expect(workspace(tester).notice, isNull);
    expect(workspace(tester).error, '서버가 응답하지 않아요. 잠시 후 다시 시도하세요.');
    expect(find.byKey(const Key('workspace-error')), findsOneWidget);
    api.restarts = 0;
    await tester.pumpWidget(const SizedBox.shrink());
    await drain(tester);
  });

  testWidgets(
    'A connect while the server restarts says so and connects without a second press',
    (t) async {
      t.view.physicalSize = const Size(800, 600);
      t.view.devicePixelRatio = 1;
      addTearDown(t.view.reset);
      await mockWindow(t);
      final socket = SessionSocket(), media = ResizeMedia();
      final fixture = ViewFixture(
        socket,
        media,
        ViewFixture.build(socket, media),
      );
      var loads = 0;
      await t.pumpWidget(
        MaterialApp(
          home: RemoteView(
            machine: machine(1),
            load: () async {
              if (++loads <= 2) throw _restart;
              return fixture.session;
            },
          ),
        ),
      );
      await t.pump();
      await t.pump();
      try {
        expect(loads, 1);
        expect(find.text(serverRestartingMessage), findsOneWidget);
        expect(find.byKey(const Key('retry-remote-connection')), findsNothing);
        await t.pump(const Duration(seconds: 1));
        await t.pump();
        expect(loads, 2);
        expect(find.text(serverRestartingMessage), findsOneWidget);
        await t.pump(const Duration(seconds: 2));
        await t.pump();
        expect(loads, 3);
        await fixture.initialFrame(t);
        expect(find.text(serverRestartingMessage), findsNothing);
        expect(find.byKey(const Key('retry-remote-connection')), findsNothing);
      } finally {
        await t.pumpWidget(const SizedBox());
        unawaited(fixture.session.close());
        await t.pump(const Duration(seconds: 3));
        await media.events.close();
      }
    },
  );

  testWidgets(
    'A remote window opened while the server restarts waits with one line, then shows the VM',
    (t) async {
      mockPresentation();
      final messenger =
          TestDefaultBinaryMessengerBinding.instance.defaultBinaryMessenger;
      messenger.setMockMethodCallHandler(
        SessionWindows.channel,
        (_) async => null,
      );
      addTearDown(
        () => messenger.setMockMethodCallHandler(SessionWindows.channel, null),
      );
      final api = _WindowApi();
      await t.pumpWidget(
        MaterialApp(
          home: SessionWindow(launch: launch(), api: api, guard: _Guard()),
        ),
      );
      await t.pump();
      await t.pump();
      expect(api.reads, 1);
      expect(find.text(serverRestartingMessage), findsOneWidget);
      expect(find.byType(RemoteView), findsNothing);
      await t.pump(const Duration(seconds: 1));
      await t.pump();
      expect(api.reads, greaterThanOrEqualTo(2));
      expect(find.byType(RemoteView), findsOneWidget);
      await t.pumpWidget(const SizedBox());
      await t.pump(const Duration(seconds: 1));
    },
  );
}
