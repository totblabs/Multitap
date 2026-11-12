import 'dart:async';

import 'package:flutter/material.dart';
import 'package:flutter/services.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:multitap_client/android/android_remote.dart';
import 'package:multitap_client/android/android_session_keepalive.dart';
import 'package:multitap_client/data/broker_api.dart';
import 'package:multitap_client/models/stream_options.dart';

/// A start time written in this machine's own zone, so the card's local
/// "11월 11일 10:20부터" is the same wherever the test runs.
String androidStarted(int day, int hour, int minute) =>
    DateTime(2026, 11, day, hour, minute).toUtc().toIso8601String();

StreamTakeoverOffer androidOffer({
  DateTime? receivedAt,
  String id = 'pve-149',
  List<(String, String?)>? sessions,
}) => StreamTakeoverOffer.parse(
  {
    'protocol': streamTakeoverProtocol,
    'context': 'a' * 43,
    'expiresInSeconds': 120,
    'desktop': {'id': id, 'name': 'TESTER'},
    'sessions': [
      for (final (username, createdAt)
          in sessions ?? [('alice', androidStarted(6, 13, 0))])
        {'username': username, 'createdAt': createdAt, 'deviceLabel': null},
    ],
  },
  expectedDesktopId: id,
  receivedAt: receivedAt,
);

/// What the card says about [androidOffer]'s default connection.
const androidOfferLine = '연결 중인 계정 alice · 11월 6일 13:00부터';

final _takeover = find.byKey(const Key('android-takeover'));

class AndroidTakeoverFixture {
  int loads = 0, releases = 0;
  bool offered = true;
  late StreamTakeoverOffer offer = androidOffer();

  /// Holds the release open (a request in flight) until completed.
  Completer<void>? pending;

  /// The view's own authority for the request it sent.
  bool Function()? authority;

  Future<void> mount(
    WidgetTester tester, {
    Size size = const Size(1080, 2400),
    double pixelRatio = 3,
    double textScale = 1,
  }) async {
    tester.view.physicalSize = size;
    tester.view.devicePixelRatio = pixelRatio;
    addTearDown(tester.view.reset);
    await tester.pumpWidget(
      MaterialApp(
        builder: (context, child) => MediaQuery(
          data: MediaQuery.of(context)
              .copyWith(textScaler: TextScaler.linear(textScale)),
          child: child!,
        ),
        home: AndroidRemoteView(
          name: 'TESTER',
          desktopId: 'pve-149',
          server: 'https://synthetic.example.com',
          token: 'synthetic-session',
          options: const StreamOptions(),
          load: (_, current, _) async {
            loads++;
            throw BrokerException(
              'desktop_busy',
              409,
              null,
              true,
              offered ? offer : null,
            );
          },
          takeover: (offered, current) async {
            expect(identical(offered, offer), isTrue);
            expect(current(), isTrue);
            releases++;
            authority = current;
            await pending?.future;
          },
        ),
      ),
    );
    await tester.pumpAndSettle();
  }
}

/// The card is the only question: no dialog, and no other route, over it.
void expectNoTakeoverDialog(WidgetTester tester) {
  expect(find.byType(Dialog), findsNothing);
  expect(
    tester.state<NavigatorState>(find.byType(Navigator)).canPop(),
    isFalse,
  );
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
      (call) async => call.method == 'start' ? 'started' : true,
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
  testWidgets(
    'busy card names the other account and its start time before any release; leaving it preserves the old connection',
    (tester) async {
      final fixture = AndroidTakeoverFixture()
        ..offer = androidOffer(
          sessions: [('kim.minji', androidStarted(11, 10, 20))],
        );
      await fixture.mount(tester);
      expect(_takeover, findsOneWidget);
      expect(find.textContaining('그쪽 연결을 끊고 여기서 이어서'), findsOneWidget);
      expect(find.text('연결 중인 계정 kim.minji · 11월 11일 10:20부터'), findsOneWidget);
      // What the old second dialog added and nobody could use.
      expect(find.text('기기 정보 없음'), findsNothing);
      expect(find.textContaining('synthetic.example.com'), findsNothing);
      expectNoTakeoverDialog(tester);
      await tester.pump(const Duration(seconds: 5));
      expect(fixture.releases, 0);
      expect(fixture.loads, 1);
      await tester.tap(find.byKey(const Key('android-leave')));
      await tester.pump();
      await tester.pump(const Duration(seconds: 1));
      expect(fixture.releases, 0);
      expect(fixture.loads, 1);
    },
  );

  testWidgets('every connection in the offer is listed; an unknown start '
      'time is left out, not invented', (tester) async {
    final fixture = AndroidTakeoverFixture()
      ..offer = androidOffer(
        sessions: [('kim.minji', androidStarted(11, 10, 20)), ('legacy', null)],
      );
    await fixture.mount(tester);
    expect(
      find.text(
        '연결 중인 계정 kim.minji · 11월 11일 10:20부터\n'
        '연결 중인 계정 legacy',
      ),
      findsOneWidget,
    );
    expect(find.textContaining('확인할 수 없음'), findsNothing);
    expect(fixture.releases, 0);
  });

  testWidgets(
    'busy stream without negotiated context never offers a legacy release',
    (tester) async {
      final fixture = AndroidTakeoverFixture()..offered = false;
      await fixture.mount(tester);
      expect(_takeover, findsNothing);
      expect(find.textContaining('연결 중인 계정'), findsNothing);
      expect(fixture.releases, 0);
      expect(fixture.loads, 1);
    },
  );

  testWidgets(
    'one press on the card releases once, opens no dialog, then returns to normal load',
    (tester) async {
      final fixture = AndroidTakeoverFixture();
      await fixture.mount(tester);
      expect(find.text(androidOfferLine), findsOneWidget);
      expect(fixture.releases, 0);
      await tester.tap(_takeover);
      await tester.pump();
      expectNoTakeoverDialog(tester);
      await tester.pumpAndSettle();
      expect(fixture.releases, 1);
      expect(fixture.loads, 2);
      expectNoTakeoverDialog(tester);
      expect(tester.takeException(), isNull);
    },
  );

  testWidgets('a second press while the request runs sends nothing more', (
    tester,
  ) async {
    final fixture = AndroidTakeoverFixture()..pending = Completer<void>();
    await fixture.mount(tester);
    // Two presses in one frame: the button is still on screen for both.
    await tester.tap(_takeover);
    await tester.tap(_takeover, warnIfMissed: false);
    await tester.pump();
    expect(fixture.releases, 1);
    await tester.pump(const Duration(milliseconds: 300));
    expect(fixture.releases, 1);
    expect(fixture.loads, 1);
    fixture.pending!.complete();
    await tester.pumpAndSettle();
    expect(fixture.releases, 1);
    expect(fixture.loads, 2);
  });

  for (final state in ['expired', 'different-target']) {
    testWidgets('$state context is never an actionable takeover', (
      tester,
    ) async {
      final fixture = AndroidTakeoverFixture();
      fixture.offer = state == 'expired'
          ? androidOffer(
              receivedAt: DateTime.now().subtract(const Duration(minutes: 3)),
            )
          : androidOffer(id: 'pve-150');
      await fixture.mount(tester);
      expect(_takeover, findsNothing);
      expect(fixture.releases, 0);
      expect(fixture.loads, 1);
    });
  }

  testWidgets(
    'an offer that runs out while the card is up sends no release when pressed',
    (tester) async {
      // Three seconds of its 120 are left when the card appears.
      final fixture = AndroidTakeoverFixture()
        ..offer = androidOffer(
          receivedAt: DateTime.now().subtract(const Duration(seconds: 117)),
        );
      await fixture.mount(tester);
      expect(_takeover, findsOneWidget);
      // The offer's clock is the real one.
      await tester.runAsync(
        () => Future<void>.delayed(const Duration(milliseconds: 3300)),
      );
      await tester.tap(_takeover);
      await tester.pumpAndSettle();
      expect(fixture.releases, 0);
      expect(fixture.loads, 1);
      expect(find.textContaining('연결 전환 확인 시간이 끝났어요'), findsOneWidget);
      expect(_takeover, findsNothing);
      expect(find.byKey(const Key('android-retry')), findsOneWidget);
      expectNoTakeoverDialog(tester);
    },
  );

  testWidgets('a press that arrives while the app is not in front releases '
      'nothing', (tester) async {
    final fixture = AndroidTakeoverFixture();
    await fixture.mount(tester);
    final press = tester.widget<FilledButton>(_takeover).onPressed!;
    tester.binding.handleAppLifecycleStateChanged(AppLifecycleState.inactive);
    tester.binding.handleAppLifecycleStateChanged(AppLifecycleState.hidden);
    tester.binding.handleAppLifecycleStateChanged(AppLifecycleState.paused);
    press();
    await tester.pump(const Duration(milliseconds: 700));
    expect(fixture.releases, 0);
    expect(fixture.loads, 1);
    tester.binding.handleAppLifecycleStateChanged(AppLifecycleState.hidden);
    tester.binding.handleAppLifecycleStateChanged(AppLifecycleState.inactive);
    tester.binding.handleAppLifecycleStateChanged(AppLifecycleState.resumed);
    await tester.pump(const Duration(milliseconds: 700));
    expect(fixture.releases, 0);
  });

  testWidgets(
    'leaving the foreground while the request runs withdraws its authority',
    (tester) async {
      final fixture = AndroidTakeoverFixture()..pending = Completer<void>();
      await fixture.mount(tester);
      await tester.tap(_takeover);
      await tester.pump();
      expect(fixture.releases, 1);
      expect(fixture.authority!(), isTrue);
      tester.binding.handleAppLifecycleStateChanged(AppLifecycleState.inactive);
      tester.binding.handleAppLifecycleStateChanged(AppLifecycleState.hidden);
      tester.binding.handleAppLifecycleStateChanged(AppLifecycleState.paused);
      expect(fixture.authority!(), isFalse);
      tester.binding.handleAppLifecycleStateChanged(AppLifecycleState.hidden);
      tester.binding.handleAppLifecycleStateChanged(AppLifecycleState.inactive);
      tester.binding.handleAppLifecycleStateChanged(AppLifecycleState.resumed);
      await tester.pump(const Duration(milliseconds: 700));
      // Coming back does not hand the old request its authority again.
      expect(fixture.authority!(), isFalse);
      expect(fixture.releases, 1);
      fixture.pending!.complete();
      await tester.pump();
      await tester.pumpWidget(const SizedBox.shrink());
      await tester.pump(const Duration(seconds: 1));
    },
  );

  testWidgets('disposing the takeover card cannot release or reconnect', (
    tester,
  ) async {
    final fixture = AndroidTakeoverFixture();
    await fixture.mount(tester);
    expect(_takeover, findsOneWidget);
    await tester.pumpWidget(const SizedBox.shrink());
    await tester.pumpAndSettle();
    expect(fixture.releases, 0);
    expect(fixture.loads, 1);
  });

  testWidgets('five connections at 200 percent text on a small phone keep the '
      'card scrollable and its buttons reachable', (tester) async {
    final fixture = AndroidTakeoverFixture()
      ..offer = androidOffer(
        sessions: [
          for (var i = 0; i < 5; i++)
            ('account-$i', androidStarted(11, 10, 20 + i)),
        ],
      );
    await fixture.mount(
      tester,
      size: const Size(360, 640),
      pixelRatio: 1,
      textScale: 2,
    );
    expect(tester.takeException(), isNull);
    expect(find.textContaining('연결 중인 계정 account-4'), findsOneWidget);
    final leave = find.byKey(const Key('android-leave'));
    for (final button in [leave, _takeover]) {
      await tester.ensureVisible(button);
      await tester.pumpAndSettle();
      expect(button.hitTestable(), findsOneWidget);
      expect(tester.getSize(button).height, greaterThanOrEqualTo(48));
    }
    expect(tester.takeException(), isNull);
    await tester.tap(_takeover);
    await tester.pumpAndSettle();
    expect(fixture.releases, 1);
  });
}
