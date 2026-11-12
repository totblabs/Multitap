import '../helpers/stream_connect_consent.dart'
    show acceptStreamConnect, expectAndroidHandOverQuestion;
import 'dart:async';

import 'package:flutter_test/flutter_test.dart';
import 'package:flutter/material.dart';
import 'package:flutter/services.dart';
import 'package:multitap_client/android/android_app.dart';
import 'package:multitap_client/android/android_remote.dart';
import 'package:multitap_client/android/android_session_keepalive.dart';
import 'package:multitap_client/android/android_store.dart';
import 'package:multitap_client/android/android_stream_preparation.dart';
import 'package:multitap_client/data/broker_api.dart';

import '../auth_flow_test.dart' show TestBroker;
import 'android_takeover_context_test.dart'
    show androidOffer, androidOfferLine;
import 'android_stream_preparation_test.dart'
    show FakeCurrentApi, desktop, state;
import 'android_launcher_parity_test.dart' show ConsoleFake, afterRdp, pumpApp;

import 'package:multitap_client/models/stream_options.dart';
import 'package:multitap_client/ui/app_dialog.dart';

import 'android_launcher_test.dart'
    show FakeBroker, MemoryStore, vm, mainButton;

class _AuthorityBroker extends FakeBroker
    implements ConfirmedTakeoverBrokerApi, SessionRenewalBrokerApi {
  _AuthorityBroker(this.clock, {this.idle = false})
    : super([vm(ownedOutput: true)]);
  final DateTime Function() clock;
  final bool idle;
  int confirmed = 0, renewals = 0;
  @override
  Future<BrokerSession> login(String username, String password) async =>
      BrokerSession(
        token: 'synthetic-authority',
        username: username,
        expiresAt: clock().add(Duration(minutes: idle ? 120 : 2)),
        sessionAccess: idle
            ? SessionAccess(
                serverTime: clock(),
                idleDeadline: clock().add(const Duration(minutes: 30)),
                idleTimeout: const Duration(minutes: 30),
                warning: const Duration(minutes: 1),
              )
            : null,
      );
  @override
  Future<DateTime> renewSession(String token) async {
    renewals++;
    return clock().add(const Duration(minutes: 5));
  }

  @override
  void recordSessionRenewal(
    String token, {
    required int stage,
    required bool ok,
    String? code,
  }) {}
  @override
  Future<BrokerConnection> connect(
    String token,
    String desktopId, {
    String mode = 'stream',
    StreamOptions? options,
  }) async {
    calls.add('connect');
    throw BrokerException('desktop_busy', 409, null, true, androidOffer());
  }

  @override
  Future<bool> confirmTakeover(
    String token,
    String desktopId,
    StreamTakeoverOffer offer,
  ) async {
    confirmed++;
    return true;
  }
}

Future<AndroidRemoteView> _authorityView(
  WidgetTester tester,
  _AuthorityBroker broker,
  DateTime Function() clock,
) async {
  tester.view.physicalSize = const Size(1080, 2400);
  tester.view.devicePixelRatio = 3;
  addTearDown(tester.view.reset);
  final messenger =
      TestDefaultBinaryMessengerBinding.instance.defaultBinaryMessenger;
  messenger.setMockMethodCallHandler(
    AndroidSessionKeepalive.channel,
    (call) async => call.method == 'start' ? true : null,
  );
  messenger.setMockMethodCallHandler(
    const MethodChannel('multitap/pen-input'),
    (_) async => null,
  );
  addTearDown(() {
    for (final channel in [
      AndroidStore.channel,
      AndroidSessionKeepalive.channel,
      const MethodChannel('multitap/pen-input'),
    ]) {
      messenger.setMockMethodCallHandler(channel, null);
    }
  });
  MemoryStore(
    origin: 'https://vdi.example',
    username: 'tester',
    prefs: {'launcher.view': 'cards'},
  ).install();
  await tester.pumpWidget(
    AndroidVdiApp(
      apiFactory: (_) => broker,
      clock: clock,
      sessionRenewalLead: const Duration(minutes: 1),
    ),
  );
  await tester.pumpAndSettle();
  await tester.enterText(
    find.byKey(const Key('sign-in-password')),
    'synthetic-only',
  );
  await tester.ensureVisible(find.byKey(const Key('login-submit')));
  await tester.tap(find.byKey(const Key('login-submit')));
  await tester.pumpAndSettle();
  await tester.ensureVisible(mainButton('pve-149'));
  await tester.tap(mainButton('pve-149'));
      await acceptStreamConnect(tester, 'pve-149');
  await tester.pumpAndSettle();
  return tester.widget<AndroidRemoteView>(find.byType(AndroidRemoteView));
}

class ConfirmedAndroidBroker extends TestBroker
    implements TakeoverBrokerApi, ConfirmedTakeoverBrokerApi {
  int legacy = 0, confirmed = 0;
  bool released = true;
  Completer<bool>? pending;
  StreamTakeoverOffer? received;
  @override
  Future<bool> takeover(String token, String desktopId) async {
    legacy++;
    return true;
  }

  @override
  Future<bool> confirmTakeover(
    String token,
    String desktopId,
    StreamTakeoverOffer offer,
  ) async {
    confirmed++;
    received = offer;
    if (pending != null) return pending!.future;
    return released;
  }
}

class _OccupiedCountApi extends FakeCurrentApi
    implements StreamTakeoverPreflightBrokerApi {
  _OccupiedCountApi() {
    inventory = () async => [
      desktop(
        count: single ? 1 : 2,
        display: single ? state() : null,
        cleanup: cleanup,
      ),
    ];
    apply = () async {
      if (occupied) {
        throw const BrokerException('desktop_busy', 409, null, true);
      }
      single = true;
      return state();
    };
  }
  bool occupied = true, single = false, cleanup = false;
  bool refuseFinalConnect = false;
  int confirms = 0;
  Completer<void>? preflightReply;
  final offer = androidOffer();

  @override
  Future<void> checkStreamTakeover(String token, String desktopId) async {
    calls.add('preflight');
    if (preflightReply != null) await preflightReply!.future;
    if (occupied) {
      throw BrokerException('desktop_busy', 409, null, true, offer);
    }
  }

  @override
  Future<bool> confirmTakeover(
    String token,
    String desktopId,
    StreamTakeoverOffer offer,
  ) async {
    expect(identical(offer, this.offer), isTrue);
    calls.add('confirmed');
    confirms++;
    occupied = false;
    cleanup = true;
    return true;
  }

  @override
  Future<BrokerConnection> connect(
    String token,
    String desktopId, {
    String mode = 'stream',
    StreamOptions? options,
  }) async {
    if (refuseFinalConnect) {
      calls.add('connect');
      throw const BrokerException('stream_unavailable', 409);
    }
    return super.connect(token, desktopId, mode: mode, options: options);
  }
}

class _OccupiedConsoleApi extends ConsoleFake
    implements StreamTakeoverPreflightBrokerApi, ConfirmedTakeoverBrokerApi {
  _OccupiedConsoleApi() : super([afterRdp()]);
  int confirms = 0;
  final offer = androidOffer();
  @override
  Future<void> checkStreamTakeover(String token, String desktopId) async {
    calls.add('preflight');
    throw BrokerException('desktop_busy', 409, null, true, offer);
  }

  @override
  Future<bool> confirmTakeover(
    String token,
    String desktopId,
    StreamTakeoverOffer offer,
  ) async {
    confirms++;
    return true;
  }
}

void _preflightChannels(WidgetTester tester) {
  final messenger =
      TestDefaultBinaryMessengerBinding.instance.defaultBinaryMessenger;
  for (final channel in [
    const MethodChannel('multitap/pen-input'),
    AndroidSessionKeepalive.channel,
  ]) {
    messenger.setMockMethodCallHandler(channel, (_) async => true);
    addTearDown(() => messenger.setMockMethodCallHandler(channel, null));
  }
}

Future<void> _mountOccupiedCount(
  WidgetTester tester,
  _OccupiedCountApi api,
) async {
  _preflightChannels(tester);
  tester.view.physicalSize = const Size(1080, 2400);
  tester.view.devicePixelRatio = 3;
  addTearDown(tester.view.reset);
  await tester.pumpWidget(
    MaterialApp(
      home: AndroidRemoteView(
        name: 'TESTER',
        desktopId: 'pve-149',
        token: 'synthetic-preflight',
        options: const StreamOptions(),
        load: (options, current, scale) => AndroidStreamPreparation(
          api: api,
          authorized: current,
          wait: (_) async {
            api.cleanup = false;
          },
        ).connect('synthetic-preflight', 'pve-149', options),
        takeover: (offer, current) => AndroidStreamPreparation(
          api: api,
          authorized: current,
        ).takeover('synthetic-preflight', 'pve-149', offer: offer),
      ),
    ),
  );
  await tester.pumpAndSettle();
}

void main() {
  test('occupied two-output preflight returns the typed review before count mutation', () async {
    final api = _OccupiedCountApi();
    await expectLater(
      AndroidStreamPreparation(
        api: api,
        authorized: () => true,
      ).connect('synthetic-preflight', 'pve-149', const StreamOptions()),
      throwsA(
        isA<BrokerException>().having(
          (error) => identical(error.takeoverOffer, api.offer),
          'reviewed offer retained',
          isTrue,
        ),
      ),
    );
    expect(api.calls, ['inventory', 'preflight']);
    expect(api.confirms, 0);
  });

  test('preflight response cannot authorize display when the route retires while awaiting it', () async {
    final api = _OccupiedCountApi()..occupied = false;
    api.preflightReply = Completer<void>();
    var current = true;
    final pending = AndroidStreamPreparation(
      api: api,
      authorized: () => current,
    ).connect('synthetic-preflight', 'pve-149', const StreamOptions());
    await Future<void>.delayed(Duration.zero);
    expect(api.calls, ['inventory', 'preflight']);
    current = false;
    api.preflightReply!.complete();
    await expectLater(
      pending,
      throwsA(
        isA<BrokerException>().having(
          (error) => error.status,
          'retired owner',
          401,
        ),
      ),
    );
    expect(api.calls, ['inventory', 'preflight']);
  });

  test('confirmed preflight successor keeps cleanup before count and ordinary connect', () async {
    final api = _OccupiedCountApi();
    var waits = 0;
    final preparation = AndroidStreamPreparation(
      api: api,
      authorized: () => true,
      wait: (_) async {
        waits++;
        api.cleanup = false;
      },
    );
    await preparation.takeover(
      'synthetic-preflight',
      'pve-149',
      offer: api.offer,
    );
    final connection = await preparation.connect(
      'synthetic-preflight',
      'pve-149',
      const StreamOptions(),
    );
    expect(connection.sessionId, 'fixture');
    expect(api.confirms, 1);
    expect(waits, 1);
    expect(api.calls, [
      'confirmed',
      'inventory',
      'inventory',
      'preflight',
      'count:1',
      'inventory',
      'connect',
    ]);
  });

  testWidgets(
    'occupied count preparation asks first on its card and leaving it never changes display or controller',
    (tester) async {
      final api = _OccupiedCountApi();
      await _mountOccupiedCount(tester, api);
      expect(find.byKey(const Key('android-takeover')), findsOneWidget);
      expect(api.calls, ['inventory', 'preflight']);
      // The card is the question: it names the connection its button ends.
      expect(find.text(androidOfferLine), findsOneWidget);
      expect(find.byType(Dialog), findsNothing);
      await tester.pump(const Duration(seconds: 2));
      expect(api.confirms, 0);
      await tester.tap(find.byKey(const Key('android-leave')));
      await tester.pump();
      await tester.pump(const Duration(seconds: 1));
      expect(api.calls, ['inventory', 'preflight']);
      expect(api.confirms, 0);
      await tester.pumpWidget(const SizedBox.shrink());
      await tester.pumpAndSettle();
    },
  );

  testWidgets(
    'occupied count: the card press alone enables cleanup then display and connect',
    (tester) async {
      final api = _OccupiedCountApi()..refuseFinalConnect = true;
      await _mountOccupiedCount(tester, api);
      expect(api.confirms, 0);
      await tester.tap(find.byKey(const Key('android-takeover')));
      await tester.pump();
      expect(find.byType(Dialog), findsNothing);
      await tester.pumpAndSettle();
      expect(api.confirms, 1);
      expect(api.calls, [
        'inventory',
        'preflight',
        'confirmed',
        'inventory',
        'inventory',
        'preflight',
        'count:1',
        'inventory',
        'connect',
      ]);
      await tester.pumpWidget(const SizedBox.shrink());
      await tester.pumpAndSettle();
    },
  );

  // One question (2026-11-11): the launcher cannot know the desktop is in use
  // before it asks, so its one question is the hand-over question; the
  // preflight follows the answer, and the takeover card follows the preflight.
  testWidgets(
    'occupied console handover: one launcher question, then the takeover card without a second hand-over prompt or a Windows transfer',
    (tester) async {
      _preflightChannels(tester);
      final broker = _OccupiedConsoleApi();
      await pumpApp(tester, broker);
      await tester.ensureVisible(mainButton('pve-149'));
      await tester.tap(mainButton('pve-149'));
      await expectAndroidHandOverQuestion(tester, 'pve-149');
      expect(find.byType(AppConfirmDialog), findsOneWidget);
      // Nothing is asked of the broker before the answer.
      expect(broker.calls.where((call) => call == 'preflight'), isEmpty);
      expect(broker.calls.contains('connect'), isFalse);
      expect(broker.transfers, 0);
      expect(find.byType(AndroidRemoteView), findsNothing);
      await tester.tap(
        find.byKey(const Key('confirm-console-handover-pve-149')),
      );
      await tester.pumpAndSettle();
      expect(
        find.byKey(const Key('confirm-console-handover-pve-149')),
        findsNothing,
      );
      expect(find.byType(AppConfirmDialog), findsNothing);
      expect(find.byType(AndroidRemoteView), findsOneWidget);
      expect(find.byKey(const Key('android-takeover')), findsOneWidget);
      expect(broker.transfers, 0);
      expect(broker.confirms, 0);
      expect(broker.calls.where((call) => call == 'preflight').length, 2);
      // The launcher's question was the only dialog; the card follows it.
      expect(find.text(androidOfferLine), findsOneWidget);
      expect(find.byType(Dialog), findsNothing);
      await tester.tap(find.byKey(const Key('android-leave')));
      await tester.pumpAndSettle();
      expect(find.byType(AndroidRemoteView), findsNothing);
      expect(broker.transfers, 0);
      expect(broker.confirms, 0);
      await tester.pumpWidget(const SizedBox.shrink());
      await tester.pumpAndSettle();
    },
  );

  test(
    'missing offered context cannot fall back to empty legacy takeover',
    () async {
      final api = ConfirmedAndroidBroker();
      await expectLater(
        AndroidStreamPreparation(
          api: api,
          authorized: () => true,
        ).takeover('synthetic-token', 'pve-149'),
        throwsA(isA<BrokerException>()),
      );
      expect(api.legacy, 0);
      expect(api.confirmed, 0);
    },
  );

  test('confirmed takeover forwards exact offer and requires an actual released acknowledgement', () async {
    final api = ConfirmedAndroidBroker();
    final offer = androidOffer();
    final preparation = AndroidStreamPreparation(
      api: api,
      authorized: () => true,
    );
    await preparation.takeover('synthetic-token', 'pve-149', offer: offer);
    expect(api.confirmed, 1);
    expect(api.legacy, 0);
    expect(identical(api.received, offer), isTrue);
    api.released = false;
    await expectLater(
      preparation.takeover('synthetic-token', 'pve-149', offer: offer),
      throwsA(isA<BrokerException>()),
    );
    expect(api.confirmed, 2);
    expect(api.legacy, 0);
  });

  test('wrong target, expired offer and lost authority never issue either release API', () async {
    for (final condition in ['target', 'expiry', 'authority']) {
      final api = ConfirmedAndroidBroker();
      final offer = condition == 'target'
          ? androidOffer(id: 'pve-150')
          : condition == 'expiry'
          ? androidOffer(
              receivedAt: DateTime.now().subtract(const Duration(minutes: 3)),
            )
          : androidOffer();
      await expectLater(
        AndroidStreamPreparation(
          api: api,
          authorized: () => condition != 'authority',
        ).takeover('synthetic-token', 'pve-149', offer: offer),
        throwsA(isA<BrokerException>()),
      );
      expect((api.confirmed, api.legacy), (0, 0));
    }
  });

  test(
    'authority retired during the confirmed request cannot adopt the response',
    () async {
      final api = ConfirmedAndroidBroker()..pending = Completer<bool>();
      var authorized = true;
      final waiting = AndroidStreamPreparation(
        api: api,
        authorized: () => authorized,
      ).takeover('synthetic-token', 'pve-149', offer: androidOffer());
      expect(api.confirmed, 1);
      authorized = false;
      api.pending!.complete(true);
      await expectLater(
        waiting,
        throwsA(
          isA<BrokerException>().having((error) => error.status, 'status', 401),
        ),
      );
      expect(api.legacy, 0);
    },
  );

  test('successor connect waits for ordinary cleanup observation after confirmed release', () async {
    final api = FakeCurrentApi();
    var now = DateTime.now(), cleanup = true, waits = 0;
    api.inventory = () async => [desktop(cleanup: cleanup)];
    final preparation = AndroidStreamPreparation(
      api: api,
      authorized: () => true,
      now: () => now,
      wait: (delay) async {
        expect(api.calls.contains('connect'), isFalse);
        waits++;
        cleanup = false;
        now = now.add(delay);
      },
    );
    await preparation.takeover(
      'synthetic-token',
      'pve-149',
      offer: androidOffer(receivedAt: now),
    );
    expect(api.calls, ['takeover:pve-149']);
    await preparation.connect(
      'synthetic-token',
      'pve-149',
      const StreamOptions(),
    );
    expect(waits, 1);
    expect(api.calls, [
      'takeover:pve-149',
      'inventory',
      'inventory',
      'connect',
    ]);
  });

  for (final idle in [false, true]) {
    testWidgets(
      '${idle ? 'idle' : 'absolute'} deadline blocks the real app takeover callback before timer cleanup',
      (tester) async {
        var now = DateTime.now();
        final broker = _AuthorityBroker(() => now, idle: idle);
        final view = await _authorityView(tester, broker, () => now);
        final connects = broker.calls.where((call) => call == 'connect').length;
        // No pump: the timer and asynchronous owner cleanup have not run yet.
        now = now.add(Duration(minutes: idle ? 31 : 3));
        await expectLater(
          view.takeover!(androidOffer(), () => true),
          throwsA(
            isA<BrokerException>().having(
              (error) => error.status,
              'status',
              401,
            ),
          ),
        );
        expect(broker.confirmed, 0);
        expect(
          broker.calls.where((call) => call == 'connect').length,
          connects,
        );
        await tester.pumpWidget(const SizedBox.shrink());
        await tester.pumpAndSettle();
      },
    );
  }

  testWidgets(
    'renewed current expiry admits takeover after the captured original expiry',
    (tester) async {
      final initial = DateTime.now();
      var now = initial;
      final broker = _AuthorityBroker(() => now);
      final view = await _authorityView(tester, broker, () => now);
      now = initial.add(const Duration(seconds: 70));
      await tester.pump(const Duration(seconds: 70));
      await tester.pumpAndSettle();
      expect(broker.renewals, greaterThan(0));
      now = initial.add(const Duration(seconds: 130));
      await view.takeover!(androidOffer(), () => true);
      expect(broker.confirmed, 1);
      await tester.pumpWidget(const SizedBox.shrink());
      await tester.pumpAndSettle();
    },
  );
}
