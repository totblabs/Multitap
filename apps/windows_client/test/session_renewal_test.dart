import 'dart:async';

import 'package:flutter/material.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:multitap_client/app.dart';
import 'package:multitap_client/data/broker_api.dart';
import 'package:multitap_client/data/login_memory.dart';
import 'package:multitap_client/data/session_renewal.dart';
import 'package:multitap_client/data/session_windows.dart';
import 'package:multitap_client/data/window_close_guard.dart';
import 'package:multitap_client/models/session_display_mode.dart';
import 'package:multitap_client/models/stream_options.dart';
import 'package:multitap_client/ui/session_window.dart';

import 'auth_flow_test.dart' show TestBroker, TestLoginMemory, fixtureOrigin;
import 'session_recovery_test.dart' show mockPresentation;

/// Virtual time for session lifetimes: pumped fake timers never move the real
/// clock, so the app, the windows and the broker all read this one.
class VirtualClock {
  DateTime now = DateTime.utc(2026, 1, 1);
  DateTime call() => now;
  Future<void> advance(WidgetTester tester, Duration total) async {
    for (var left = total; left > Duration.zero;) {
      final step = left < const Duration(seconds: 15)
          ? left
          : const Duration(seconds: 15);
      now = now.add(step);
      await tester.pump(step);
      left -= step;
    }
  }
}

/// A broker whose sessions can be renewed (same token, later expiry) or not.
class RenewingBroker extends TestBroker implements SessionRenewalBrokerApi {
  RenewingBroker(this.clock);
  final VirtualClock clock;
  Duration lifetime = const Duration(minutes: 10);
  bool renewSupported = true, renewRejected = false, networkDown = false;
  int renewals = 0;
  final tokens = <String>[];
  final records = <String>[];
  final loggedInAs = <String>[];

  @override
  Future<BrokerSession> login(String username, String password) async {
    loggedInAs.add('$username:$password');
    if (networkDown) throw const BrokerException('network_error');
    final session = await super.login(username, password);
    final token = 'token-${tokens.length + 1}';
    tokens.add(token);
    return BrokerSession(
      token: token,
      expiresAt: clock.now.add(lifetime),
      username: session.username,
    );
  }

  @override
  Future<DateTime> renewSession(String token) async {
    if (!renewSupported) throw const BrokerException('not_found', 404);
    if (renewRejected) throw const BrokerException('session_expired', 401);
    renewals++;
    return clock.now.add(lifetime);
  }

  @override
  void recordSessionRenewal(
    String token, {
    required int stage,
    required bool ok,
    String? code,
  }) => records.add('$stage:${ok ? 'ok' : code}');

  @override
  Future<List<BrokerDesktop>> desktops(String token) async {
    seenTokens.add(token);
    if (expiredTokens.remove(token)) {
      throw const BrokerException('session_expired', 401);
    }
    return super.desktops(token);
  }

  final seenTokens = <String>[];
  final expiredTokens = <String>{};
}

void main() {
  group('SessionRenewer', () {
    test('renews before the expiry and keeps renewing', () async {
      final expiries = <DateTime>[];
      var renews = 0;
      final renewer = SessionRenewer(
        lead: const Duration(milliseconds: 150),
        retry: const Duration(milliseconds: 20),
        renew: () async {
          renews++;
          return DateTime.now().add(const Duration(milliseconds: 400));
        },
        onRenewed: expiries.add,
        onLapse: (_) => fail('no lapse expected'),
        minimumGain: const Duration(milliseconds: 100),
      )..start(DateTime.now().add(const Duration(milliseconds: 200)));
      await Future<void>.delayed(const Duration(milliseconds: 600));
      renewer.stop();
      expect(renews, greaterThanOrEqualTo(2));
      expect(expiries.length, renews);
    });

    test('reports each way renewal cannot work exactly once', () async {
      Future<RenewalLapse> lapse(Future<DateTime> Function() renew) async {
        final done = Completer<RenewalLapse>();
        SessionRenewer(
          lead: Duration.zero,
          retry: const Duration(milliseconds: 10),
          minimumGain: const Duration(minutes: 1),
          renew: renew,
          onRenewed: (_) {},
          onLapse: done.complete,
        ).start(DateTime.now().add(const Duration(milliseconds: 50)));
        return done.future.timeout(const Duration(seconds: 2));
      }

      expect(
        await lapse(() async => throw const BrokerException('x', 404)),
        RenewalLapse.unsupported,
      );
      expect(
        await lapse(() async => throw const BrokerException('x', 401)),
        RenewalLapse.rejected,
      );
      final capped = DateTime.now().add(const Duration(milliseconds: 50));
      expect(await lapse(() async => capped), RenewalLapse.exhausted);
      expect(
        await lapse(() async => throw const BrokerException('network_error')),
        RenewalLapse.unreachable,
      );
    });
  });

  group('signed-in client', () {
    late VirtualClock clock;
    setUp(() => clock = VirtualClock());
    Future<void> start(
      WidgetTester tester,
      RenewingBroker api,
      LoginMemory memory,
    ) async {
      tester.view.physicalSize = const Size(1440, 900);
      tester.view.devicePixelRatio = 1;
      addTearDown(tester.view.resetPhysicalSize);
      addTearDown(tester.view.resetDevicePixelRatio);
      await tester.pumpWidget(
        MultitapApp(
          api: api,
          serverOrigin: fixtureOrigin,
          loginMemory: memory,
          sessionRetryDelay: const Duration(seconds: 1),
          clock: clock.call,
        ),
      );
      await tester.pumpAndSettle();
    }

    Future<void> signIn(WidgetTester tester, {bool remember = true}) async {
      await tester.enterText(find.byKey(const Key('sign-in-id')), 'alice');
      await tester.enterText(
        find.byKey(const Key('sign-in-password')),
        'secret-pass',
      );
      if (remember) {
        await tester.tap(find.byKey(const Key('remember-password')));
      }
      await tester.tap(find.byKey(const Key('login-submit')));
      await tester.pumpAndSettle();
    }

    bool signedIn(WidgetTester tester) =>
        find.byKey(const Key('workspace-account')).evaluate().isNotEmpty;

    String fieldText(WidgetTester tester, String key) =>
        tester.widget<TextField>(find.byKey(Key(key))).controller!.text;

    testWidgets('renews the same token before it expires, no new sign-in', (
      tester,
    ) async {
      final api = RenewingBroker(clock);
      await start(tester, api, TestLoginMemory());
      await signIn(tester);
      expect(signedIn(tester), isTrue);
      // 10 min lifetime, renewed 5 min before: past the original expiry the
      // client is still signed in with the very same token.
      await clock.advance(tester, const Duration(minutes: 5, seconds: 1));
      expect(api.renewals, 1);
      expect(api.records, contains('1:ok'));
      await clock.advance(tester, const Duration(minutes: 8));
      expect(api.renewals, greaterThanOrEqualTo(2));
      expect(signedIn(tester), isTrue);
      expect(api.loggedInAs, hasLength(1));
      expect(api.seenTokens.toSet(), {'token-1'});
    });

    testWidgets(
      'authoritative 401 retires the login without password recovery',
      (tester) async {
        final api = RenewingBroker(clock);
        await start(tester, api, TestLoginMemory());
        await signIn(tester);
        // A definitive account-auth rejection cannot be repaired by saved login.
        api.expiredTokens.add('token-1');
        await clock.advance(tester, const Duration(seconds: 11));
        api.expiredTokens.clear();
        await clock.advance(tester, const Duration(seconds: 1));
        expect(signedIn(tester), isFalse);
        expect(find.byKey(const ValueKey('session-end-ack')), findsOneWidget);
        expect(api.loggedInAs, hasLength(1));
      },
    );

    testWidgets(
      'without renewal the remembered password signs in again silently',
      (tester) async {
        final api = RenewingBroker(clock)..renewSupported = false;
        final memory = TestLoginMemory();
        await start(tester, api, memory);
        await signIn(tester);
        expect(api.loggedInAs, ['alice:secret-pass']);
        await clock.advance(tester, const Duration(minutes: 5, seconds: 1));
        await tester.pumpAndSettle();
        expect(signedIn(tester), isTrue);
        expect(api.loggedInAs, ['alice:secret-pass', 'alice:secret-pass']);
        expect(api.records, contains('1:request_failed'));
        expect(api.records, contains('2:ok'));
        // Requests now use the new token.
        await clock.advance(tester, const Duration(seconds: 11));
        expect(api.seenTokens.last, 'token-2');
      },
    );

    testWidgets('rejected account renewal never signs in silently', (
      tester,
    ) async {
      final api = RenewingBroker(clock)..renewRejected = true;
      await start(tester, api, TestLoginMemory());
      await signIn(tester);
      api.expiredTokens.add('token-1');
      await clock.advance(tester, const Duration(seconds: 11));
      await tester.pumpAndSettle();
      expect(signedIn(tester), isFalse);
      expect(find.byKey(const ValueKey('session-end-ack')), findsOneWidget);
      expect(api.loggedInAs, hasLength(1));
    });

    testWidgets(
      'no remembered password: stays signed in until the real expiry, then '
      'shows the sign-in screen with the ID',
      (tester) async {
        final api = RenewingBroker(clock)..renewSupported = false;
        await start(tester, api, TestLoginMemory());
        await signIn(tester, remember: false);
        await clock.advance(tester, const Duration(minutes: 6));
        // Still inside the session's life: nothing was cut early.
        expect(signedIn(tester), isTrue);
        await clock.advance(tester, const Duration(minutes: 5));
        await tester.pumpAndSettle();
        expect(signedIn(tester), isFalse);
        await tester.tap(find.byKey(const ValueKey('session-end-ack')));
        await tester.pumpAndSettle();
        expect(fieldText(tester, 'sign-in-id'), 'alice');
        expect(api.loggedInAs, hasLength(1));
      },
    );

    testWidgets(
      'when silent sign-in is refused the screen shows the ID, forgets the '
      'refused password',
      (tester) async {
        final api = RenewingBroker(clock)..renewSupported = false;
        final memory = TestLoginMemory();
        await start(tester, api, memory);
        await signIn(tester);
        api.rejectLogin = true;
        await clock.advance(tester, const Duration(minutes: 5, seconds: 1));
        await tester.pumpAndSettle();
        expect(signedIn(tester), isFalse);
        await tester.tap(find.byKey(const ValueKey('session-end-ack')));
        await tester.pumpAndSettle();
        expect(fieldText(tester, 'sign-in-id'), 'alice');
        expect(fieldText(tester, 'sign-in-password'), '');
        expect(memory.entries[fixtureOrigin.origin]?.password, isNull);
        expect(api.records, contains('2:authentication_required'));
      },
    );

    testWidgets(
      'broker unreachable at expiry preserves vault and fills it for a manual sign-in',
      (tester) async {
        final api = RenewingBroker(clock)..renewSupported = false;
        final memory = TestLoginMemory();
        await start(tester, api, memory);
        await signIn(tester);
        api.networkDown = true;
        api.expiredTokens.add('token-1');
        // The 401 arrives; silent sign-in cannot reach the broker.
        await clock.advance(tester, const Duration(seconds: 11));
        for (var i = 0; i < 12; i++) {
          await clock.advance(tester, const Duration(seconds: 1));
        }
        await tester.pumpAndSettle();
        expect(signedIn(tester), isFalse);
        await tester.tap(find.byKey(const ValueKey('session-end-ack')));
        await tester.pumpAndSettle();
        expect(fieldText(tester, 'sign-in-id'), 'alice');
        expect(fieldText(tester, 'sign-in-password'), 'secret-pass');
        expect(
          tester
              .widget<Switch>(find.byKey(const Key('remember-password')))
              .value,
          isTrue,
        );
        expect(api.loggedInAs, hasLength(1));
        expect(memory.entries[fixtureOrigin.origin]?.password, 'secret-pass');
      },
    );
  });

  group('remote window', () {
    testWidgets('keeps running across token renewal and past the old expiry', (
      tester,
    ) async {
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
      final clock = VirtualClock();
      final api = _WindowApi(clock);
      final guard = _Guard();
      await tester.pumpWidget(
        MaterialApp(
          home: SessionWindow(
            launch: SessionWindowLaunch(
              origin: Uri.parse('https://fixture.example'),
              token: 'fixture-token',
              expiresAt: clock.now.add(const Duration(minutes: 8)),
              desktopId: 'pve-149',
              name: 'TESTER',
              mode: SessionDisplayMode.windowed,
              options: const StreamOptions(),
            ),
            api: api,
            guard: guard,
            clock: clock.call,
          ),
        ),
      );
      await tester.pump();
      await clock.advance(tester, const Duration(minutes: 3, seconds: 1));
      expect(api.renewedTokens, ['fixture-token']);
      await clock.advance(tester, const Duration(minutes: 9));
      // Past the launch's original expiry: not retired, not closed.
      expect(guard.closes, 0);
      expect(api.renewedTokens.length, greaterThanOrEqualTo(2));
      await tester.pumpWidget(const SizedBox());
    });

    testWidgets('a definitive rejected renewal closes its owned window', (
      tester,
    ) async {
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
      final clock = VirtualClock();
      final api = _WindowApi(clock)..reject = true;
      final guard = _Guard();
      await tester.pumpWidget(
        MaterialApp(
          home: SessionWindow(
            launch: SessionWindowLaunch(
              origin: Uri.parse('https://fixture.example'),
              token: 'fixture-token',
              expiresAt: clock.now.add(const Duration(minutes: 8)),
              desktopId: 'pve-149',
              name: 'TESTER',
              mode: SessionDisplayMode.windowed,
              options: const StreamOptions(),
            ),
            api: api,
            guard: guard,
            clock: clock.call,
          ),
        ),
      );
      await tester.pump();
      await clock.advance(tester, const Duration(minutes: 12));
      expect(guard.closes, 1);
      expect(api.renewedTokens, isEmpty);
      await tester.pumpWidget(const SizedBox());
    });
  });
}

class _WindowApi extends Fake implements BrokerApi, SessionRenewalBrokerApi {
  _WindowApi(this.clock);
  final VirtualClock clock;
  final renewedTokens = <String>[];
  final records = <String>[];
  bool reject = false;

  @override
  Future<DateTime> renewSession(String token) async {
    if (reject) throw const BrokerException('session_expired', 401);
    renewedTokens.add(token);
    return clock.now.add(const Duration(minutes: 8));
  }

  @override
  void recordSessionRenewal(
    String token, {
    required int stage,
    required bool ok,
    String? code,
  }) => records.add('$stage:${ok ? 'ok' : code}');

  @override
  Future<List<BrokerDesktop>> desktops(String token) async => [
    const BrokerDesktop(
      id: 'pve-149',
      name: 'TESTER',
      kind: 'vm',
      description: '',
      os: 'Windows',
      status: 'ready',
      canConnect: true,
      hostConnected: true,
    ),
  ];

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

class _Guard implements WindowCloseGuard {
  int closes = 0;
  @override
  void attach(Future<void> Function() value) {}
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
