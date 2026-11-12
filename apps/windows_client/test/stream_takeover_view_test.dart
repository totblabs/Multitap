import 'dart:async';

import 'package:flutter/material.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:multitap_client/data/broker_api.dart';
import 'package:multitap_client/data/remote_session.dart';
import 'package:multitap_client/models/stream_options.dart';
import 'package:multitap_client/ui/app_theme.dart';
import 'package:multitap_client/ui/remote_view.dart';

import 'session_recovery_test.dart' show machine, mockPresentation;

/// A start time written in this machine's own zone, so the card's local
/// "11-11 10:20부터" is the same wherever the test runs.
String _started(int day, int hour, int minute) =>
    DateTime(2026, 11, day, hour, minute).toUtc().toIso8601String();

StreamTakeoverOffer _offer({
  String desktop = 'qa',
  DateTime? receivedAt,
  List<(String, String?)> sessions = const [('alice', null)],
}) => StreamTakeoverOffer.parse({
  'protocol': streamTakeoverProtocol,
  'context': 'C' * 43,
  'expiresInSeconds': 120,
  'desktop': {'id': desktop, 'name': '연결 검사 VM'},
  'sessions': [
    for (final (username, createdAt) in sessions)
      {'username': username, 'createdAt': createdAt, 'deviceLabel': null},
  ],
}, receivedAt: receivedAt);

final _takeover = find.byKey(const Key('takeover-remote-connection'));

/// The card is the only question: no dialog, and no other route, over it.
void _expectNoDialog(WidgetTester tester) {
  expect(find.byType(Dialog), findsNothing);
  expect(
    tester.state<NavigatorState>(find.byType(Navigator)).canPop(),
    isFalse,
  );
}

Future<void> _mount(
  WidgetTester tester, {
  StreamTakeoverOffer? offer,
  bool legacyOffer = false,
  required Future<void> Function(StreamTakeoverOffer, bool Function()) confirm,
  Future<RemoteSession> Function(StreamOptions)? retry,
  ValueNotifier<String>? owner,
  ValueNotifier<String>? server,
}) async {
  mockPresentation();
  final account = owner ?? ValueNotifier('account-a');
  if (owner == null) addTearDown(account.dispose);
  final origin = server ?? ValueNotifier('https://server.fixture.invalid');
  if (server == null) addTearDown(origin.dispose);
  await tester.pumpWidget(
    MaterialApp(
      theme: buildAppTheme(),
      home: Scaffold(
        body: ListenableBuilder(
          listenable: Listenable.merge([account, origin]),
          builder: (_, _) => RemoteView(
            key: const Key('takeover-view'),
            machine: machine,
            guideAccountKey: account.value,
            takeoverServer: origin.value,
            load: () => Future.error(
              BrokerException(
                'desktop_busy',
                409,
                'controller_in_use',
                true,
                legacyOffer ? null : offer ?? _offer(),
              ),
            ),
            onReconnect: retry,
            onConfirmTakeover: confirm,
            onTakeover: () async =>
                fail('Legacy empty-body fallback must never run'),
          ),
        ),
      ),
    ),
  );
  await tester.pumpAndSettle();
}

void main() {
  testWidgets(
    'the busy card names the other account and its start time, and sends nothing until its button is pressed',
    (tester) async {
      var releases = 0, reconnects = 0;
      await _mount(
        tester,
        offer: _offer(sessions: [('kim.minji', _started(11, 10, 20))]),
        confirm: (_, _) async => releases++,
        retry: (_) {
          reconnects++;
          return Future.error(const BrokerException('desktop_busy'));
        },
      );
      expect(find.text('다른 곳에서 연결 중이에요'), findsOneWidget);
      expect(find.textContaining('그쪽 연결을 끊고 여기서 이어서'), findsOneWidget);
      expect(find.text('연결 중인 계정 kim.minji · 11월 11일 10:20부터'), findsOneWidget);
      // What the old second dialog added and nobody could use.
      expect(find.text('기기 정보 없음'), findsNothing);
      expect(find.textContaining('server.fixture.invalid'), findsNothing);
      expect(
        find.descendant(of: _takeover, matching: find.text('끊고 여기서 연결')),
        findsOneWidget,
      );
      _expectNoDialog(tester);
      await tester.pump(const Duration(seconds: 5));
      expect((releases, reconnects), (0, 0));
      expect(_takeover, findsOneWidget);
      await tester.pumpWidget(const SizedBox.shrink());
    },
  );

  testWidgets('every connection in the offer is listed; an unknown start time '
      'is left out, not invented', (tester) async {
    await _mount(
      tester,
      offer: _offer(
        sessions: [
          ('kim.minji', _started(11, 10, 20)),
          ('lee.jun', _started(9, 8, 5)),
          ('legacy', null),
        ],
      ),
      confirm: (_, _) async {},
    );
    expect(find.text('연결 중인 계정 kim.minji · 11월 11일 10:20부터'), findsOneWidget);
    expect(find.text('연결 중인 계정 lee.jun · 11월 9일 08:05부터'), findsOneWidget);
    expect(find.text('연결 중인 계정 legacy'), findsOneWidget);
    expect(find.textContaining('확인할 수 없음'), findsNothing);
    await tester.pumpWidget(const SizedBox.shrink());
  });

  testWidgets(
    'one press sends exactly one takeover request and opens no dialog; a second press while it runs sends nothing',
    (tester) async {
      final released = Completer<void>(), calls = <String>[];
      final offer = _offer();
      await _mount(
        tester,
        offer: offer,
        confirm: (accepted, authorized) async {
          expect(identical(accepted, offer), isTrue);
          expect(authorized(), isTrue);
          calls.add('release');
          await released.future;
          expect(authorized(), isTrue);
        },
        retry: (_) {
          calls.add('connect');
          return Future.error(const BrokerException('desktop_busy'));
        },
      );
      // Two presses in one frame: the button is still on screen for both.
      await tester.tap(_takeover);
      await tester.tap(_takeover, warnIfMissed: false);
      await tester.pump();
      expect(calls, ['release']);
      _expectNoDialog(tester);
      await tester.pump(const Duration(milliseconds: 300));
      expect(calls, ['release']);
      // The release has to finish before the ordinary reconnect starts.
      released.complete();
      await tester.pumpAndSettle();
      expect(calls, ['release', 'connect']);
      _expectNoDialog(tester);
      await tester.pumpWidget(const SizedBox.shrink());
    },
  );

  for (final changed in ['account', 'server']) {
    testWidgets(
      'another $changed taking the view over cannot accept the offer the card showed',
      (tester) async {
        final owner = ValueNotifier('account-a');
        final server = ValueNotifier('https://server.fixture.invalid');
        addTearDown(owner.dispose);
        addTearDown(server.dispose);
        var releases = 0;
        await _mount(
          tester,
          owner: owner,
          server: server,
          confirm: (_, _) async => releases++,
        );
        expect(_takeover, findsOneWidget);
        if (changed == 'account') {
          owner.value = 'account-b';
        } else {
          server.value = 'https://other.fixture.invalid';
        }
        await tester.pump();
        // The offer is gone with the login it came to: a plain retry only.
        expect(_takeover, findsNothing);
        expect(find.textContaining('연결 중인 계정'), findsNothing);
        expect(
          find.byKey(const Key('retry-remote-connection')),
          findsOneWidget,
        );
        await tester.pumpAndSettle();
        expect(releases, 0);
        await tester.pumpWidget(const SizedBox.shrink());
      },
    );
  }

  testWidgets(
    'an owner replacement while the request runs withdraws its authority and never reconnects',
    (tester) async {
      final owner = ValueNotifier('account-a');
      addTearDown(owner.dispose);
      final released = Completer<void>();
      bool Function()? authority;
      var releases = 0, reconnects = 0;
      await _mount(
        tester,
        owner: owner,
        confirm: (_, authorized) async {
          releases++;
          authority = authorized;
          await released.future;
        },
        retry: (_) {
          reconnects++;
          return Future.error(const BrokerException('desktop_busy'));
        },
      );
      await tester.tap(_takeover);
      await tester.pump();
      expect(releases, 1);
      expect(authority!(), isTrue);
      owner.value = 'account-b';
      await tester.pump();
      expect(authority!(), isFalse);
      released.complete();
      for (var i = 0; i < 4; i++) {
        await tester.pump(const Duration(milliseconds: 100));
      }
      expect((releases, reconnects), (1, 0));
      await tester.pumpWidget(const SizedBox.shrink());
    },
  );

  for (final invalid in ['missing', 'wrong-target', 'expired']) {
    testWidgets('$invalid context never falls back to an unbound release', (
      tester,
    ) async {
      var releases = 0;
      await _mount(
        tester,
        legacyOffer: invalid == 'missing',
        offer: invalid == 'wrong-target'
            ? _offer(desktop: 'other-vm')
            : invalid == 'expired'
            ? _offer(receivedAt: DateTime.utc(2020))
            : null,
        confirm: (_, _) async => releases++,
      );
      if (invalid == 'expired') {
        // The offer ran out while the card was up: the press sends nothing
        // and says so.
        await tester.tap(_takeover);
        await tester.pumpAndSettle();
        expect(find.textContaining('연결 전환 확인 시간이 끝났어요'), findsOneWidget);
        expect(_takeover, findsNothing);
      } else {
        expect(_takeover, findsNothing);
        expect(find.textContaining('연결 중인 계정'), findsNothing);
      }
      expect(releases, 0);
      if (invalid == 'missing') {
        expect(
          find.byKey(const Key('retry-remote-connection')),
          findsOneWidget,
        );
      }
      _expectNoDialog(tester);
      await tester.pumpWidget(const SizedBox.shrink());
    });
  }

  testWidgets(
    'stale server context ends at a manual retry and never reconnects itself',
    (tester) async {
      var releases = 0, reconnects = 0;
      await _mount(
        tester,
        confirm: (_, _) async {
          releases++;
          throw const BrokerException('takeover_context_stale', 409);
        },
        retry: (_) {
          reconnects++;
          return Future.error(const BrokerException('desktop_busy'));
        },
      );
      await tester.tap(_takeover);
      await tester.pumpAndSettle();
      expect((releases, reconnects), (1, 0));
      expect(find.textContaining('사용 중인 연결이 바뀌었어요'), findsOneWidget);
      expect(find.byKey(const Key('retry-remote-connection')), findsOneWidget);
      await tester.pumpWidget(const SizedBox.shrink());
    },
  );
}
