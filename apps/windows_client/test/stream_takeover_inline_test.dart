import 'dart:async';

import 'package:flutter/foundation.dart';
import 'package:flutter/material.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:multitap_client/app.dart';
import 'package:multitap_client/data/broker_api.dart';
import 'package:multitap_client/data/launcher_memory.dart';
import 'package:multitap_client/data/initial_viewport.dart';
import 'package:multitap_client/models/stream_options.dart';
import 'package:multitap_client/models/session_cleanup.dart';
import 'package:multitap_client/ui/app_dialog.dart';
import 'package:multitap_client/ui/workspace_shell.dart';

import 'auth_flow_test.dart' show TestLoginMemory, fixtureOrigin;
import 'pc_login_replacement_lifetime_test.dart'
    show NativePolicyBroker, QuietWatch;
import 'session_renewal_test.dart' show VirtualClock;
import 'session_recovery_test.dart' show mockPresentation;

import 'helpers/stream_connect_consent.dart';

const _desktop = BrokerDesktop(
  id: 'qa-vm',
  name: 'WorkPC',
  kind: 'vm',
  description: '',
  os: 'Windows',
  status: 'ready',
  canConnect: true,
  canStream: true,
  hostConnected: true,
  interactiveSession: true,
  virtualDisplayReady: true,
  virtualMonitorControl: true,
  display: VirtualDisplayState(supported: true, activeCount: 1, stale: false),
  monitors: [RemoteMonitor(id: '0', width: 1920, height: 1080, primary: true)],
  power: BrokerPowerSnapshot(state: 'running', stale: false),
);

class _Broker extends NativePolicyBroker {
  _Broker(super.clock);
  var assigned = <BrokerDesktop>[_desktop];
  final release = Completer<bool>();
  final calls = <String>[];
  bool stale = false;
  @override
  Future<List<BrokerDesktop>> desktops(String token) async => assigned;
  @override
  Future<BrokerConnection> connect(
    String token,
    String desktopId, {
    String mode = 'stream',
    StreamOptions? options,
    InitialViewport? initialViewport,
    int? displayScale,
    String? signinAvailabilityId,
  }) async {
    calls.add('connect:$desktopId');
    throw BrokerException(
      'desktop_busy',
      409,
      'controller_in_use',
      true,
      calls.where((v) => v.startsWith('connect:')).length > 1
          ? null
          : StreamTakeoverOffer.parse({
              'protocol': streamTakeoverProtocol,
              'context': 'C' * 43,
              'expiresInSeconds': 120,
              'desktop': {'id': desktopId, 'name': 'WorkPC'},
              'sessions': [
                {'username': 'alice', 'createdAt': null, 'deviceLabel': null},
              ],
            }),
    );
  }

  @override
  Future<bool> confirmTakeover(
    String token,
    String desktopId,
    StreamTakeoverOffer offer,
  ) async {
    expect(token, 'owner-1');
    expect(offer.desktopId, desktopId);
    calls.add('release:$desktopId');
    if (stale) throw const BrokerException('takeover_context_stale', 409);
    return release.future;
  }
}

class _CleanupBroker extends _Broker
    implements StreamTakeoverPreflightBrokerApi {
  _CleanupBroker(super.clock);
  bool retired = false, cleaned = false;
  int cleanupReads = 0;
  @override
  Future<void> checkStreamTakeover(String token, String id) async {
    calls.add('preflight:$id');
    if (!retired) {
      throw BrokerException(
        'desktop_busy',
        409,
        'controller_in_use',
        true,
        StreamTakeoverOffer.parse({
          'protocol': streamTakeoverProtocol,
          'context': 'C' * 43,
          'expiresInSeconds': 120,
          'desktop': {'id': id, 'name': 'WorkPC'},
          'sessions': [
            {'username': 'alice', 'createdAt': null, 'deviceLabel': null},
          ],
        }),
      );
    }
  }

  @override
  Future<bool> confirmTakeover(
    String token,
    String id,
    StreamTakeoverOffer offer,
  ) async {
    final result = await super.confirmTakeover(token, id, offer);
    retired = true;
    return result;
  }

  @override
  Future<List<BrokerDesktop>> desktops(String token) async {
    if (!retired) return super.desktops(token);
    cleanupReads++;
    return [
      BrokerDesktop(
        id: 'qa-vm',
        name: 'WorkPC',
        kind: 'vm',
        description: '',
        os: 'Windows',
        status: 'ready',
        canConnect: true,
        canStream: true,
        hostConnected: true,
        interactiveSession: true,
        virtualDisplayReady: true,
        virtualMonitorControl: true,
        sessionCleanup: cleaned
            ? const SessionCleanupState()
            : SessionCleanupState(
                pending: true,
                phase: 'pending',
                checkedAt: DateTime.now(),
              ),
        display: VirtualDisplayState(
          supported: true,
          activeCount: 1,
          stale: !cleaned,
        ),
        monitors: const [
          RemoteMonitor(id: '0', width: 1920, height: 1080, primary: true),
        ],
        power: const BrokerPowerSnapshot(state: 'running', stale: false),
      ),
    ];
  }

  @override
  Future<BrokerConnection> connect(
    String token,
    String id, {
    String mode = 'stream',
    StreamOptions? options,
    InitialViewport? initialViewport,
    int? displayScale,
    String? signinAvailabilityId,
  }) async {
    calls.add('connect:$id');
    throw const BrokerException('host_unavailable');
  }
}

Future<void> _mount(
  WidgetTester tester,
  _Broker api,
  VirtualClock clock,
) async {
  tester.view.physicalSize = const Size(1440, 900);
  tester.view.devicePixelRatio = 1;
  addTearDown(tester.view.reset);
  mockPresentation();
  final memory = SessionLauncherMemory();
  await memory.save(
    fixtureOrigin.toString(),
    const LauncherPreferences(showPreviews: false),
  );
  await tester.pumpWidget(
    MultitapApp(
      api: api,
      serverOrigin: fixtureOrigin,
      clock: clock.call,
      loginMemory: TestLoginMemory(),
      launcherMemory: memory,
      desktopWatchFactory: (_, _) => QuietWatch(),
      updateCheck: (_) async => null,
    ),
  );
  await tester.pumpAndSettle();
  await tester.enterText(find.byKey(const Key('sign-in-id')), 'alice');
  await tester.enterText(
    find.byKey(const Key('sign-in-password')),
    'synthetic-password',
  );
  await tester.tap(find.byKey(const Key('login-submit')));
  await tester.pumpAndSettle();
  await tester.tap(find.byKey(const Key('open-qa-vm')));
  await tester.pumpAndSettle();
  await acceptStreamConnect(tester, 'qa-vm');
  await tester.pumpAndSettle();
  // The launcher asked once. What follows is the card, not another dialog:
  // it names the connection its button ends and waits for that press.
  expect(_takeover, findsOneWidget);
  expect(find.text('연결 중인 계정 alice'), findsOneWidget);
  _expectNoQuestion();
}

/// No question over the card. (The embedded remote view is itself a
/// full-screen Dialog route, so the app's own dialog type is what is absent.)
void _expectNoQuestion() {
  expect(find.byType(AppDialog), findsNothing);
  expect(find.byType(Dialog), findsOneWidget);
}

final _takeover = find.byKey(const Key('takeover-remote-connection'));

void main() {
  testWidgets(
    'real App inline takeover waits for fresh Host cleanup before one ordinary reconnect',
    (tester) async {
      final previous = debugDefaultTargetPlatformOverride;
      debugDefaultTargetPlatformOverride = TargetPlatform.linux;
      final clock = VirtualClock();
      final api = _CleanupBroker(clock);
      try {
        await _mount(tester, api, clock);
        expect(api.calls, ['preflight:qa-vm']);
        await tester.tap(_takeover);
        await tester.pump();
        _expectNoQuestion();
        expect(api.calls, ['preflight:qa-vm', 'release:qa-vm']);
        api.release.complete(true);
        await tester.pump();
        await tester.pump(const Duration(milliseconds: 700));
        expect(api.cleanupReads, greaterThan(1));
        expect(api.calls.where((call) => call.startsWith('connect:')), isEmpty);
        api.cleaned = true;
        await tester.pump(const Duration(milliseconds: 600));
        await tester.pumpAndSettle();
        expect(api.calls, [
          'preflight:qa-vm',
          'release:qa-vm',
          'preflight:qa-vm',
          'connect:qa-vm',
        ]);
        expect(tester.takeException(), isNull);
      } finally {
        await tester.pumpWidget(const SizedBox.shrink());
        await tester.pumpAndSettle();
        debugDefaultTargetPlatformOverride = previous;
      }
    },
  );
  for (final scenario in ['leave', 'revoked', 'stale', 'success']) {
    testWidgets(
      'real App inline takeover $scenario preserves authority and ordering',
      (tester) async {
        final previous = debugDefaultTargetPlatformOverride;
        debugDefaultTargetPlatformOverride = TargetPlatform.linux;
        final clock = VirtualClock();
        final api = _Broker(clock);
        try {
          await _mount(tester, api, clock);
          expect(api.calls, ['connect:qa-vm']);
          if (scenario == 'leave') {
            // Declining is leaving the card: the other connection stays.
            await tester.tap(find.byKey(const Key('leave-remote-connection')));
            await tester.pumpAndSettle();
            expect(_takeover, findsNothing);
            expect(api.calls, ['connect:qa-vm']);
          } else {
            if (scenario == 'revoked') {
              api.assigned = [];
              tester
                  .widget<WorkspaceShell>(find.byType(WorkspaceShell))
                  .onRefresh();
              await tester.pumpAndSettle();
            }
            api.stale = scenario == 'stale';
            await tester.tap(_takeover);
            await tester.pump();
            _expectNoQuestion();
            if (scenario == 'revoked') {
              expect(api.calls, ['connect:qa-vm']);
            } else {
              expect(api.calls, ['connect:qa-vm', 'release:qa-vm']);
              if (scenario == 'success') {
                api.release.complete(true);
                await tester.pumpAndSettle();
                expect(api.calls, [
                  'connect:qa-vm',
                  'release:qa-vm',
                  'connect:qa-vm',
                ]);
              } else {
                await tester.pumpAndSettle();
                expect(api.calls, ['connect:qa-vm', 'release:qa-vm']);
              }
            }
          }
          expect(tester.takeException(), isNull);
        } finally {
          await tester.pumpWidget(const SizedBox.shrink());
          await tester.pumpAndSettle();
          debugDefaultTargetPlatformOverride = previous;
        }
      },
    );
  }
}
