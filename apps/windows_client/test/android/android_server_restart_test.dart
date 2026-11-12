// Android shares the server-restart wait (field diagnostics 2026-11-02: nginx
// answers 502 while a broker update restarts it). Fake broker, store, relay
// socket and native media only.
import 'package:flutter/material.dart';
import 'package:flutter/services.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:multitap_client/android/android_app.dart';
import 'package:multitap_client/android/android_remote.dart';
import 'package:multitap_client/android/android_session_keepalive.dart';
import 'package:multitap_client/android/android_store.dart';
import 'package:multitap_client/data/broker_api.dart';
import 'package:multitap_client/data/remote_session.dart';
import 'package:multitap_client/models/stream_options.dart';
import 'package:multitap_client/ui/app_theme.dart';

import '../remote_session_test.dart' show SessionSocket;
import 'android_launcher_test.dart' show FakeBroker, MemoryStore, vm;
import 'android_session_contract_test.dart' show AndroidNativeFake;

const _restart = BrokerException('server_restarting', 502);

class _RestartBroker extends FakeBroker {
  _RestartBroker(super.inventory);
  int restarts = 0;
  int get reads => calls.where((call) => call == 'desktops').length;
  @override
  Future<List<BrokerDesktop>> desktops(String token) async {
    if (restarts > 0) {
      restarts--;
      calls.add('desktops');
      throw _restart;
    }
    return super.desktops(token);
  }
}

Future<_RestartBroker> _signedIn(
  WidgetTester tester, {
  DateTime Function() clock = DateTime.now,
}) async {
  tester.view.physicalSize = const Size(1080, 2400);
  tester.view.devicePixelRatio = 3;
  addTearDown(tester.view.reset);
  final store = MemoryStore(origin: 'https://vdi.example', username: 'tester');
  store.prefs['launcher.view'] = 'cards';
  store.install();
  addTearDown(
    () => TestDefaultBinaryMessengerBinding.instance.defaultBinaryMessenger
        .setMockMethodCallHandler(AndroidStore.channel, null),
  );
  final broker = _RestartBroker([vm()]);
  await tester.pumpWidget(
    AndroidVdiApp(apiFactory: (_) => broker, clock: clock),
  );
  await tester.pumpAndSettle();
  await tester.enterText(
    find.byKey(const Key('sign-in-password')),
    'secret-pass',
  );
  final submit = find.byKey(const Key('login-submit'));
  await tester.ensureVisible(submit);
  await tester.pumpAndSettle();
  await tester.tap(submit);
  await tester.pumpAndSettle();
  return broker;
}

Future<void> _refresh(WidgetTester tester) async {
  final refresh = find.byKey(const Key('refresh-desktops'));
  await tester.ensureVisible(refresh);
  await tester.pumpAndSettle();
  await tester.tap(refresh);
  await tester.pump();
  await tester.pump(const Duration(milliseconds: 100));
}

Future<void> _leave(WidgetTester tester) async {
  await tester.pumpWidget(const SizedBox.shrink());
  await tester.pump(const Duration(seconds: 1));
}

void main() {
  testWidgets(
    'A refresh pressed while the server restarts says so once and retries by itself',
    (tester) async {
      final broker = await _signedIn(tester);
      final before = broker.reads;
      broker.restarts = 2;
      await _refresh(tester);
      expect(broker.reads, before + 1);
      expect(find.text(serverRestartingMessage), findsOneWidget);
      expect(find.byKey(const Key('list-refresh-error')), findsNothing);
      expect(find.byKey(const Key('retry-desktops')), findsNothing);
      await tester.pump(const Duration(seconds: 1));
      await tester.pump();
      expect(broker.reads, before + 2);
      expect(find.byKey(const Key('list-refresh-error')), findsNothing);
      await tester.pump(const Duration(seconds: 2));
      await tester.pump();
      expect(broker.reads, before + 3);
      expect(find.byKey(const Key('list-refresh-error')), findsNothing);
      await _leave(tester);
    },
  );

  testWidgets('A quiet read while the server restarts is tried again within '
      'a second, without an error', (tester) async {
    final broker = await _signedIn(tester);
    broker.restarts = 1;
    final before = broker.reads;
    // The fallback read (no live channel with this fake broker).
    await tester.pump(androidFallbackPollInterval);
    await tester.pump();
    expect(broker.reads, before + 1);
    expect(find.byKey(const Key('list-refresh-error')), findsNothing);
    await tester.pump(const Duration(seconds: 1));
    await tester.pump();
    expect(broker.reads, before + 2);
    expect(broker.restarts, 0);
    expect(find.byKey(const Key('list-refresh-error')), findsNothing);
    await _leave(tester);
  });

  testWidgets('A pressed refresh whose restart outlasts two minutes shows '
      'the error', (tester) async {
    var now = DateTime.now();
    final broker = await _signedIn(tester, clock: () => now);
    broker.restarts = 1000;
    await _refresh(tester);
    expect(find.byKey(const Key('list-refresh-error')), findsNothing);
    now = now.add(const Duration(minutes: 2, seconds: 1));
    await tester.pump(const Duration(seconds: 1));
    await tester.pump();
    expect(find.byKey(const Key('list-refresh-error')), findsOneWidget);
    broker.restarts = 0;
    await _leave(tester);
  });

  testWidgets(
    'A connect while the server restarts says so and connects without a second press',
    (tester) async {
      tester.view.physicalSize = const Size(1080, 2400);
      tester.view.devicePixelRatio = 3;
      addTearDown(tester.view.reset);
      AndroidNativeFake().install();
      addTearDown(AndroidNativeFake.uninstall);
      final messenger =
          TestDefaultBinaryMessengerBinding.instance.defaultBinaryMessenger;
      for (final channel in [
        AndroidSessionKeepalive.channel,
        const MethodChannel('multitap/pen-input'),
      ]) {
        messenger.setMockMethodCallHandler(
          channel,
          (call) async => call.method == 'start' ? 'started' : true,
        );
        addTearDown(() => messenger.setMockMethodCallHandler(channel, null));
      }
      var loads = 0;
      final sockets = <SessionSocket>[];
      await tester.pumpWidget(
        MaterialApp(
          theme: buildAppTheme(brightness: Brightness.dark),
          home: AndroidRemoteView(
            name: 'TESTER',
            token: 'not-persisted',
            options: const StreamOptions(),
            density: () async => null,
            load: (options, authorized, scale) async {
              if (++loads <= 2) throw _restart;
              return BrokerConnection(
                reservation: BrokerSessionReservation(() async {}),
                sessionId: 'android-session',
                webSocketUrl: Uri.parse(
                  'wss://fixture.invalid/api/v1/sessions/android-session',
                ),
              );
            },
            sessionFactory: (connection, media, statsSink) => RemoteSession(
              connection: connection,
              token: 'not-persisted',
              media: media,
              statsSink: statsSink,
              dialer: (_, _) async {
                final socket = SessionSocket();
                sockets.add(socket);
                return socket;
              },
            ),
            pictureBuilder: (_) => const ColoredBox(color: Color(0xFF2B4C6F)),
          ),
        ),
      );
      for (var i = 0; i < 4; i++) {
        await tester.pump(const Duration(milliseconds: 20));
      }
      expect(loads, 1);
      expect(find.text(serverRestartingMessage), findsOneWidget);
      expect(find.byKey(const Key('android-retry')), findsNothing);
      await tester.pump(const Duration(seconds: 1));
      for (var i = 0; i < 4; i++) {
        await tester.pump(const Duration(milliseconds: 20));
      }
      expect(loads, 2);
      expect(find.text(serverRestartingMessage), findsOneWidget);
      await tester.pump(const Duration(seconds: 2));
      for (var i = 0; i < 4; i++) {
        await tester.pump(const Duration(milliseconds: 20));
      }
      expect(loads, 3);
      expect(sockets, hasLength(1), reason: 'connected by itself');
      expect(find.text(serverRestartingMessage), findsNothing);
      expect(find.byKey(const Key('android-retry')), findsNothing);
      await _leave(tester);
    },
  );
}
