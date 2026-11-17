import 'dart:convert';
import 'dart:io';

import 'package:flutter/material.dart';
import 'package:flutter/services.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:multitap_client/app.dart';
import 'package:multitap_client/data/broker_api.dart';
import 'package:multitap_client/data/input_method_state.dart';
import 'package:multitap_client/data/launcher_memory.dart';
import 'package:multitap_client/data/login_memory.dart';
import 'package:multitap_client/ui/password_input_scope.dart';
import 'package:multitap_client/ui/sign_in_screen.dart';

import 'auth_flow_test.dart' show TestBroker, fixtureOrigin;
import 'client_diagnostics_test.dart' show DiagnosticClient;
import 'helpers/pump_work.dart';

const _channel = MethodChannel('multitap/input');
final _windows = TargetPlatformVariant.only(TargetPlatform.windows);

/// A healthy Korean ID field: context, open, Hangul, focus, foreground, layout.
const _healthy = 1 | 2 | 4 | 8 | 16 | 128;

/// The runner's side of multitap/input: every call it got, in order. No
/// expect() in the handler (a guarded tester call would see it as a failure
/// of the channel).
List<String> _runner(Object? Function() state) {
  final calls = <String>[];
  final messenger =
      TestDefaultBinaryMessengerBinding.instance.defaultBinaryMessenger;
  messenger.setMockMethodCallHandler(_channel, (call) async {
    calls.add(
      call.method == 'passwordMode'
          ? 'passwordMode:${(call.arguments as Map)['enabled']}'
          : call.method,
    );
    return call.method == 'inputMethodState' ? state() : null;
  });
  addTearDown(() => messenger.setMockMethodCallHandler(_channel, null));
  PasswordInputScope.debugReset();
  return calls;
}

int _asked(List<String> calls) =>
    calls.where((call) => call == 'inputMethodState').length;

Widget _screen({String? error, ValueChanged<int>? onState}) => MaterialApp(
  home: SignInScreen(
    serverOrigin: fixtureOrigin,
    loginMemory: const NoopLoginMemory(),
    onServerChanged: (_) async {},
    onLogin: (_, _) async => SignInResult.failed,
    error: error,
    onInputMethodState: onState,
  ),
);

/// A broker whose diagnostics take the sign-in screen's observations.
class _Broker extends TestBroker implements InputMethodDiagnosticsBrokerApi {
  final rows = <({String token, int bits, DateTime time})>[];
  bool reporter = true;
  @override
  bool recordInputMethodState(String token, int bits, DateTime time) {
    if (!reporter) return false;
    rows.add((token: token, bits: bits, time: time));
    return true;
  }
}

void main() {
  group('readInputMethodState', () {
    testWidgets('answers the runner\'s bits on Windows', (tester) async {
      final calls = _runner(() => _healthy);
      expect(await readInputMethodState(), _healthy);
      expect(calls, ['inputMethodState']);
    }, variant: _windows);

    testWidgets('is null without a usable answer', (tester) async {
      Object? answer = 'not a number';
      final calls = _runner(() => answer);
      expect(await readInputMethodState(), isNull);
      answer = null;
      expect(await readInputMethodState(), isNull);
      _runner(() => throw PlatformException(code: 'failed'));
      expect(await readInputMethodState(), isNull);
      // A runner without the method (an older one, a design preview).
      _runner(() => throw MissingPluginException());
      expect(await readInputMethodState(), isNull);
      expect(_asked(calls), 2);
    }, variant: _windows);

    testWidgets('asks nothing on another platform', (tester) async {
      final calls = _runner(() => _healthy);
      expect(await readInputMethodState(), isNull);
      expect(calls, isEmpty);
    });
  });

  group('Sign-in ID field', () {
    testWidgets('asks once per focus gain, after password mode is released', (
      tester,
    ) async {
      final calls = _runner(() => _healthy), seen = <int>[];
      await tester.pumpWidget(_screen(onState: seen.add));
      await tester.pumpAndSettle();
      expect(_asked(calls), 0, reason: 'Nothing is asked before the focus.');

      await tester.tap(find.byKey(const Key('sign-in-id')));
      await tester.pump();
      expect(_asked(calls), 1);
      expect(seen, [_healthy]);
      // Typing and repainting are not focus gains.
      await tester.enterText(find.byKey(const Key('sign-in-id')), '한글아이디');
      await tester.pump();
      await tester.tap(find.byKey(const Key('sign-in-id')));
      await tester.pump();
      expect(_asked(calls), 1);

      await tester.tap(find.byKey(const Key('sign-in-password')));
      await tester.pump();
      expect(calls.last, 'passwordMode:true');
      expect(_asked(calls), 1, reason: 'The password field is not observed.');
      await tester.tap(find.byKey(const Key('sign-in-id')));
      await tester.pump();
      // The runner must hear "password mode off" first, or it would answer
      // that the context is held away and repair nothing.
      expect(calls.sublist(calls.length - 2), [
        'passwordMode:false',
        'inputMethodState',
      ]);
      expect(seen, [_healthy, _healthy]);
    }, variant: _windows);

    testWidgets('marks an observation made under an error line', (
      tester,
    ) async {
      // Context missing and repaired, as after the reported sign-out.
      const repaired = 8 | 16 | 128 | 256;
      _runner(() => repaired);
      final seen = <int>[];
      await tester.pumpWidget(
        _screen(error: '오랫동안 사용하지 않아 로그아웃되었어요.', onState: seen.add),
      );
      await tester.pumpAndSettle();
      await tester.tap(find.byKey(const Key('sign-in-id')));
      await tester.pump();
      expect(seen, [repaired | InputMethodState.afterSignOut]);
      expect(InputMethodState.afterSignOut, 512);
    }, variant: _windows);

    testWidgets('asks (the repair) even when nobody listens', (tester) async {
      final calls = _runner(() => _healthy);
      await tester.pumpWidget(_screen());
      await tester.pumpAndSettle();
      await tester.tap(find.byKey(const Key('sign-in-id')));
      await tester.pump();
      expect(_asked(calls), 1);
      expect(tester.takeException(), isNull);
    }, variant: _windows);

    testWidgets('a runner without an answer records nothing and breaks nothing', (
      tester,
    ) async {
      final seen = <int>[];
      Future<void> focusAgain() async {
        await tester.tap(find.byKey(const Key('sign-in-password')));
        await tester.pump();
        await tester.tap(find.byKey(const Key('sign-in-id')));
        await tester.pump();
      }

      // A runner without the method.
      _runner(() => throw MissingPluginException());
      await tester.pumpWidget(_screen(onState: seen.add));
      await tester.pumpAndSettle();
      await tester.tap(find.byKey(const Key('sign-in-id')));
      await tester.pump();
      // An answer that is nothing, then a failed call.
      final calls = _runner(() => null);
      await focusAgain();
      _runner(() => throw PlatformException(code: 'failed'));
      await focusAgain();
      expect(_asked(calls), 1);
      expect(seen, isEmpty);
      expect(tester.takeException(), isNull);
      await tester.enterText(find.byKey(const Key('sign-in-id')), 'alice');
      expect(
        tester
            .widget<TextField>(find.byKey(const Key('sign-in-id')))
            .controller!
            .text,
        'alice',
      );
    }, variant: _windows);
  });

  group('Observations waiting for a sign-in', () {
    late Directory directory;
    late File file;
    setUp(() {
      directory = Directory.systemTemp.createTempSync('totb-input-method-');
      file = File('${directory.path}/launcher.json');
    });
    tearDown(() => directory.deleteSync(recursive: true));

    test('are kept in the launcher file beside what it already holds', () async {
      final memory = FileLauncherMemory(file);
      expect(await memory.loadInputMethodObservations(), isEmpty);
      await memory.saveInputMethodObservations(const []);
      expect(file.existsSync(), isFalse, reason: 'Nothing to keep or drop.');

      await memory.saveTrayNoticeShown();
      await memory.save(
        'https://one.example',
        const LauncherPreferences(lastDesktop: 'pve-149'),
      );
      final first = DateTime.utc(2026, 11, 17, 3, 4, 5, 678);
      await memory.saveInputMethodObservations([
        InputMethodObservation(_healthy | 512, first),
        InputMethodObservation(8 | 16, first.add(const Duration(minutes: 1))),
      ]);
      final kept = await FileLauncherMemory(file).loadInputMethodObservations();
      expect([for (final item in kept) item.bits], [_healthy | 512, 8 | 16]);
      expect(kept.first.time, first);
      expect(kept.first.time.isUtc, isTrue);
      expect(await memory.loadTrayNoticeShown(), isTrue);
      expect(
        (await memory.load('https://one.example')).lastDesktop,
        'pve-149',
      );
      // The other writers of the file leave them alone.
      await memory.saveTrayNoticeShown();
      await memory.save('https://two.example', const LauncherPreferences());
      expect(await memory.loadInputMethodObservations(), hasLength(2));

      await memory.saveInputMethodObservations(const []);
      expect(await memory.loadInputMethodObservations(), isEmpty);
      final client = (jsonDecode(await file.readAsString()) as Map)['_client'];
      expect(client, {'trayNoticeShown': true});
      expect(
        (await memory.load('https://one.example')).lastDesktop,
        'pve-149',
      );
    });

    test('read back only well-formed entries, a few at most', () async {
      await file.writeAsString(
        jsonEncode({
          '_client': {
            'inputMethod': [
              {'bits': 1, 'time': '2026-11-17T03:04:05.678Z'},
              {'bits': 'x', 'time': '2026-11-17T03:04:05.678Z'},
              {'bits': 2, 'time': 'yesterday'},
              {'bits': -1, 'time': '2026-11-17T03:04:05.678Z'},
              'text',
              for (var bits = 3; bits < 9; bits++)
                {'bits': bits, 'time': '2026-11-17T03:04:05.678Z'},
            ],
          },
        }),
      );
      final kept = await FileLauncherMemory(file).loadInputMethodObservations();
      expect([for (final item in kept) item.bits], [1, 3, 4, 5]);
      await file.writeAsString(jsonEncode({'_client': {'inputMethod': 7}}));
      expect(
        await FileLauncherMemory(file).loadInputMethodObservations(),
        isEmpty,
      );
    });
  });

  group('Diagnostics', () {
    test('the row is signIn stage 3 with the bits, at the time it was seen', () async {
      final client = DiagnosticClient();
      final api = HttpBrokerApi(
        origin: Uri.parse('https://configured.invalid'),
        client: client,
        enableDiagnostics: true,
      );
      addTearDown(api.close);
      final seen = DateTime.utc(2026, 11, 17, 3, 4, 5, 678);
      expect(
        api.recordInputMethodState('token', _healthy, seen),
        isFalse,
        reason: 'No request of this token has started its reporter.',
      );
      await api.desktops('token');
      expect(api.recordInputMethodState('other-token', _healthy, seen), isFalse);
      expect(api.recordInputMethodState('token', _healthy | 512, seen), isTrue);
      await api.flushDiagnostics();
      final rows = [
        for (final report in client.reports)
          for (final row in report['events'] as List)
            if (row['event'] == 'signIn') row,
      ];
      expect(rows, hasLength(1));
      expect(rows.single['time'], '2026-11-17T03:04:05.678Z');
      expect(rows.single['values'], {
        'ok': true,
        'stage': 3,
        'error': _healthy | 512,
      });
      expect(rows.single.containsKey('code'), isFalse);

      await api.logout('token');
      expect(
        api.recordInputMethodState('token', _healthy, seen),
        isFalse,
        reason: 'A signed-out token has no reporter: the row is offered again.',
      );
    });

    Future<void> start(
      WidgetTester tester,
      _Broker broker,
      LauncherMemory memory,
    ) async {
      tester.view.physicalSize = const Size(1440, 900);
      tester.view.devicePixelRatio = 1;
      addTearDown(tester.view.resetPhysicalSize);
      addTearDown(tester.view.resetDevicePixelRatio);
      await tester.pumpWidget(
        MultitapApp(
          api: broker,
          serverOrigin: fixtureOrigin,
          launcherMemory: memory,
        ),
      );
      await tester.pumpAndSettle();
    }

    Future<void> signIn(WidgetTester tester) async {
      await tester.enterText(find.byKey(const Key('sign-in-id')), 'alice');
      await tester.enterText(
        find.byKey(const Key('sign-in-password')),
        'test-password',
      );
      await tester.tap(find.byKey(const Key('login-submit')));
      await pumpWork(tester);
    }

    testWidgets(
      'a sign-in uploads what an earlier run saw, then this one, and forgets both',
      (tester) async {
        _runner(() => _healthy);
        final earlier = DateTime.utc(2026, 11, 16, 23, 30);
        const broken = 8 | 16 | 128 | 512;
        final memory = SessionLauncherMemory();
        await memory.saveInputMethodObservations([
          InputMethodObservation(broken, earlier),
        ]);
        final broker = _Broker();
        await start(tester, broker, memory);

        final before = DateTime.now().toUtc();
        await tester.tap(find.byKey(const Key('sign-in-id')));
        await tester.pump();
        // Saved when it is made: a restart before the sign-in keeps it.
        final waiting = await memory.loadInputMethodObservations();
        expect([for (final item in waiting) item.bits], [broken, _healthy]);
        expect(waiting.last.time.isBefore(before), isFalse);
        expect(broker.rows, isEmpty, reason: 'Nobody is signed in yet.');

        await signIn(tester);
        expect(find.byType(SignInScreen), findsNothing);
        expect([for (final row in broker.rows) row.bits], [broken, _healthy]);
        expect(broker.rows.first.time, earlier);
        expect(broker.rows.last.time, waiting.last.time);
        expect(broker.rows.every((row) => row.token == 'test-token'), isTrue);
        expect(await memory.loadInputMethodObservations(), isEmpty);
      },
      variant: _windows,
    );

    testWidgets(
      'observations stay saved while no reporter takes them, a few at most',
      (tester) async {
        var state = 0;
        _runner(() => ++state);
        final memory = SessionLauncherMemory();
        final broker = _Broker()..reporter = false;
        await start(tester, broker, memory);
        for (var gain = 0; gain < 6; gain++) {
          await tester.tap(find.byKey(const Key('sign-in-id')));
          await tester.pump();
          await tester.tap(find.byKey(const Key('sign-in-password')));
          await tester.pump();
        }
        // The first ones are the evidence; a full list replaces its newest.
        expect(
          [
            for (final item in await memory.loadInputMethodObservations())
              item.bits,
          ],
          [1, 2, 3, 6],
        );
        await signIn(tester);
        expect(find.byType(SignInScreen), findsNothing);
        expect(broker.rows, isEmpty);
        expect(await memory.loadInputMethodObservations(), hasLength(4));
      },
      variant: _windows,
    );

    testWidgets('a sign-in without an observation uploads nothing', (
      tester,
    ) async {
      final memory = SessionLauncherMemory();
      final broker = _Broker();
      await start(tester, broker, memory);
      await signIn(tester);
      expect(find.byType(SignInScreen), findsNothing);
      expect(broker.rows, isEmpty);
    });
  });
}
