import 'package:flutter/material.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:multitap_client/data/broker_api.dart';
import 'package:multitap_client/data/session_windows.dart';
import 'package:multitap_client/data/window_close_guard.dart';
import 'package:multitap_client/models/session_cleanup.dart';
import 'package:multitap_client/models/stream_options.dart';
import 'package:multitap_client/ui/session_window.dart';

import 'session_recovery_test.dart' show mockPresentation;
import 'session_window_lifecycle_test.dart' show launch;

const _operation = '11111111-2222-4333-8444-555555555555';

class _Broker extends Fake
    implements
        BrokerApi,
        ConfirmedTakeoverBrokerApi,
        StreamTakeoverPreflightBrokerApi {
  final calls = <String>[];
  bool busy = true,
      cleaned = false,
      stale = false,
      configured = false,
      controlReady = true;
  int count = 2;
  BrokerDesktop get current => BrokerDesktop(
    id: 'pve-149',
    name: 'Occupied WorkPC',
    kind: 'vm',
    description: '',
    os: 'Windows',
    status: 'ready',
    canConnect: true,
    canStream: true,
    hostConnected: true,
    interactiveSession: true,
    virtualDisplayReady: true,
    virtualMonitorControl: controlReady,
    use: busy ? DesktopUse.mine : DesktopUse.idle,
    sessionCleanup: !busy && !cleaned
        ? SessionCleanupState(
            pending: true,
            phase: 'pending',
            checkedAt: DateTime.now(),
          )
        : const SessionCleanupState(),
    display: VirtualDisplayState(
      supported: controlReady,
      activeCount: count,
      stale: !busy && !cleaned,
      checkedAt: DateTime.now(),
      operation: configured
          ? VirtualDisplayOperation(
              id: _operation,
              status: 'succeeded',
              requestedCount: count,
            )
          : null,
    ),
    monitors: [
      for (var i = 0; i < count; i++)
        RemoteMonitor(id: '$i', width: 1920, height: 1080),
    ],
    power: const BrokerPowerSnapshot(state: 'running', stale: false),
  );
  StreamTakeoverOffer get offer => StreamTakeoverOffer.parse({
    'protocol': streamTakeoverProtocol,
    'context': 'C' * 43,
    'expiresInSeconds': 120,
    'desktop': {'id': 'pve-149', 'name': 'Occupied WorkPC'},
    'sessions': [
      {'username': 'alice', 'createdAt': null, 'deviceLabel': null},
    ],
  });
  @override
  Future<List<BrokerDesktop>> desktops(String token) async {
    calls.add('read');
    return [current];
  }

  @override
  Future<void> checkStreamTakeover(String token, String id) async {
    expect(token, 'fixture-token');
    expect(id, 'pve-149');
    calls.add('preflight');
    if (busy) {
      throw BrokerException(
        'desktop_busy',
        409,
        'controller_in_use',
        true,
        offer,
      );
    }
  }

  @override
  Future<bool> confirmTakeover(
    String token,
    String id,
    StreamTakeoverOffer value,
  ) async {
    expect(token, 'fixture-token');
    expect(value.desktopId, id);
    calls.add('release');
    if (stale) throw const BrokerException('takeover_context_stale', 409);
    busy = false;
    return true;
  }

  @override
  Future<VirtualDisplayState> configureDisplays(
    String token,
    String id,
    int requested,
  ) async {
    calls.add('configure:$requested');
    // The real broker refuses a live stop-v1 controller before configuring.
    if (busy) throw const BrokerException('desktop_busy', 409);
    if (!cleaned) throw const BrokerException('session_cleanup_pending', 409);
    count = requested;
    configured = true;
    return VirtualDisplayState(
      supported: true,
      activeCount: count,
      checkedAt: DateTime.now(),
      stale: false,
      operation: VirtualDisplayOperation(
        id: _operation,
        status: 'succeeded',
        requestedCount: count,
      ),
    );
  }

  @override
  Future<BrokerConnection> connect(
    String token,
    String id, {
    String mode = 'stream',
    StreamOptions? options,
  }) async {
    calls.add('connect');
    throw const BrokerException('host_unavailable');
  }

  @override
  void close() {}
}

class _Guard implements WindowCloseGuard {
  @override
  void attach(Future<void> Function() request) {}
  @override
  Future<bool> update({required bool busy, required bool dirty}) async => true;
  @override
  Future<bool> close() async => true;
  @override
  void dispose() {}
}

void main() {
  TestWidgetsFlutterBinding.ensureInitialized();
  final messenger =
      TestDefaultBinaryMessengerBinding.instance.defaultBinaryMessenger;
  setUp(() {
    mockPresentation();
    messenger.setMockMethodCallHandler(
      SessionWindows.channel,
      (_) async => null,
    );
  });
  tearDown(
    () => messenger.setMockMethodCallHandler(SessionWindows.channel, null),
  );
  for (final scenario in ['leave', 'cleanup', 'stale', 'unready']) {
    testWidgets(
      'actual SessionWindow occupied count2 offers consent before display mutation: $scenario',
      (tester) async {
        final api = _Broker()
          ..stale = scenario == 'stale'
          ..controlReady = scenario != 'unready';
        try {
          await tester.pumpWidget(
            MaterialApp(
              home: SessionWindow(launch: launch(), api: api, guard: _Guard()),
            ),
          );
          await tester.pumpAndSettle();
          expect(
            api.calls.where(
              (v) =>
                  v.startsWith('configure:') ||
                  v == 'connect' ||
                  v == 'release',
            ),
            isEmpty,
          );
          expect(api.calls.where((v) => v == 'preflight').length, 1);
          // The card is the consent: it names the connection its button
          // ends, and no dialog follows the press.
          final takeover = find.byKey(const Key('takeover-remote-connection'));
          expect(takeover, findsOneWidget);
          expect(find.text('연결 중인 계정 alice'), findsOneWidget);
          expect(find.byType(Dialog), findsNothing);
          if (scenario == 'leave' || scenario == 'unready') {
            // Declining is leaving the card ('원격 창 닫기').
            await tester.tap(find.byKey(const Key('leave-remote-connection')));
            await tester.pump();
            await tester.pump(const Duration(milliseconds: 700));
            expect(
              api.calls.where(
                (v) =>
                    v.startsWith('configure:') ||
                    v == 'connect' ||
                    v == 'release',
              ),
              isEmpty,
            );
          } else {
            await tester.tap(takeover);
            await tester.pump();
            expect(find.byType(Dialog), findsNothing);
            await tester.pump(const Duration(milliseconds: 700));
            expect(api.calls.where((v) => v == 'release').length, 1);
            expect(
              api.calls.where(
                (v) => v.startsWith('configure:') || v == 'connect',
              ),
              isEmpty,
            );
            if (scenario == 'cleanup') {
              expect(
                api.calls.where((v) => v == 'read').length,
                greaterThan(2),
              );
              api.cleaned = true;
              await tester.pump(const Duration(milliseconds: 600));
              await tester.pumpAndSettle();
              expect(
                api.calls.where((v) => v.startsWith('configure:')).toList(),
                ['configure:1'],
              );
              expect(api.calls.where((v) => v == 'connect').length, 1);
            } else {
              expect(
                find.byKey(const Key('retry-remote-connection')),
                findsOneWidget,
              );
            }
          }
          expect(tester.takeException(), isNull);
        } finally {
          await tester.pumpWidget(const SizedBox.shrink());
          await tester.pumpAndSettle();
        }
      },
    );
  }
}
