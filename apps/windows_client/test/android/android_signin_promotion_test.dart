import 'dart:async';
import 'dart:convert';

import 'package:flutter/material.dart';
import 'package:flutter/services.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:multitap_client/android/android_remote.dart';
import 'package:multitap_client/android/android_session_keepalive.dart';
import 'package:multitap_client/data/broker_api.dart';
import 'package:multitap_client/data/remote_session.dart';
import 'package:multitap_client/models/desktop_machine.dart';
import 'package:multitap_client/models/stream_options.dart';

import '../remote_session_test.dart' show SessionSocket;
import '../signin_stream_contract_test.dart'
    show signinSessionId, signinBindingJson, preparePhase;
import '../signin_stream_session_test.dart'
    show SigninMedia, phaseInfo, signinFrame;
import 'android_session_contract_test.dart' show AndroidNativeFake;

const _pen = MethodChannel('multitap/pen-input');

class _PromotionMedia extends SigninMedia {
  final sizes = <(int, int)>[];
  @override
  Future<int> start(int width, int height) async {
    sizes.add((width, height));
    starts++;
    decoded = 0;
    calls.add('start');
    return 42;
  }
}

DesktopMachine _ordinary() => DesktopMachine(
  id: 'pve-170',
  name: 'Synthetic',
  description: '',
  kind: MachineKind.vm,
  status: MachineStatus.ready,
  specification: 'Windows',
  accent: Colors.blue,
  hostOnline: true,
  interactiveSession: true,
);

Future<void> _pump(WidgetTester t, [int count = 5]) async {
  for (var i = 0; i < count; i++) {
    await t.pump(const Duration(milliseconds: 40));
  }
}

void main() {
  TestWidgetsFlutterBinding.ensureInitialized();
  setUp(() {
    AndroidNativeFake().install();
    final messenger =
        TestDefaultBinaryMessengerBinding.instance.defaultBinaryMessenger;
    messenger.setMockMethodCallHandler(_pen, (_) async => null);
    messenger.setMockMethodCallHandler(
      AndroidSessionKeepalive.channel,
      (call) async => call.method == 'start' ? 'started' : true,
    );
  });
  tearDown(() {
    AndroidNativeFake.uninstall();
    final messenger =
        TestDefaultBinaryMessengerBinding.instance.defaultBinaryMessenger;
    messenger.setMockMethodCallHandler(_pen, null);
    messenger.setMockMethodCallHandler(AndroidSessionKeepalive.channel, null);
  });

  for (final scenario in ['ready', 'retired-auth', 'disposed']) {
    testWidgets(
      'Android decoded sign-in promotion preserves intent: $scenario',
      (t) async {
        t.view.physicalSize = const Size(1280, 720);
        t.view.devicePixelRatio = 1;
        addTearDown(t.view.reset);
        final starts = <StreamOptions>[];
        final sessions = <RemoteSession>[];
        final sockets = <SessionSocket>[];
        final media = <_PromotionMedia>[];
        final fresh = Completer<DesktopMachine>();
        var reads = 0;
        var authorized = true;
        const preference = StreamOptions(
          audio: true,
          bitrateMode: StreamBitrateMode.auto,
        );
        try {
          await t.pumpWidget(
            MaterialApp(
              home: AndroidRemoteView(
                name: 'Synthetic',
                desktopId: 'pve-170',
                server: 'https://fixture.invalid',
                token: 'synthetic',
                options: preference,
                signinPromotionAuthorized: () => authorized,
                refreshSigninDesktop: () {
                  reads++;
                  return fresh.future;
                },
                load: (options, current, scale) async {
                  starts.add(options);
                  final signin = starts.length == 1;
                  return BrokerConnection(
                    sessionId: signin ? signinSessionId : 'ordinary-synthetic',
                    webSocketUrl: Uri.parse('wss://fixture.invalid/synthetic'),
                    epochMedia: true,
                    viewportProvisioning: true,
                    options: signin
                        ? const StreamOptions(
                            audio: false,
                            bitrateMode: StreamBitrateMode.fixed,
                          )
                        : options,
                    signin: signin
                        ? SigninBinding.parse(signinBindingJson())
                        : null,
                  );
                },
                sessionFactory: (connection, _, stats) {
                  final socket = SessionSocket(), engine = _PromotionMedia();
                  sockets.add(socket);
                  media.add(engine);
                  final session = RemoteSession(
                    connection: connection,
                    token: 'synthetic',
                    media: engine,
                    statsSink: stats,
                    dialer: (_, _) async => socket,
                  );
                  sessions.add(session);
                  return session;
                },
                pictureBuilder: (_) => const ColoredBox(color: Colors.black),
              ),
            ),
          );
          await _pump(t);
          expect(starts, hasLength(1));
          final socket = sockets.single,
              engine = media.single,
              source = sessions.single;
          socket.incoming.add(jsonEncode(phaseInfo()));
          await _pump(t, 2);
          socket.incoming.add(signinFrame());
          engine.decoded = 1;
          await _pump(t);
          expect(source.frameReady, isTrue);
          expect(source.signinDesktopReady, isFalse);
          expect(reads, 0);

          socket.incoming.add(jsonEncode(preparePhase()));
          await _pump(t, 2);
          socket.incoming.add(
            jsonEncode(phaseInfo(generation: 2, epoch: 2, phase: 'desktop')),
          );
          await _pump(t, 2);
          expect(reads, 0, reason: 'metadata alone is not a decoded desktop');
          socket.incoming.add(signinFrame(2));
          engine.decoded = 1;
          await _pump(t);
          expect(source.signinDesktopReady, isTrue);
          expect(engine.sizes, [(1280, 720), (1280, 720)]);
          expect(reads, 1);
          expect(
            starts,
            hasLength(1),
            reason: 'fresh normal readiness is still pending',
          );

          if (scenario == 'retired-auth') authorized = false;
          if (scenario == 'disposed') {
            await t.pumpWidget(const SizedBox.shrink());
            await _pump(t);
          }
          fresh.complete(_ordinary());
          await _pump(t, 8);
          if (scenario == 'ready') {
            expect(starts, hasLength(2));
            expect(source.closed, isTrue);
            expect(starts.last.audio, isTrue);
            expect(starts.last.bitrateMode, StreamBitrateMode.auto);
            expect(sessions.last.connection.signin, isNull);
            await _pump(t);
            expect(
              starts,
              hasLength(2),
              reason: 'duplicate old notifications cannot reconnect again',
            );
          } else {
            expect(starts, hasLength(1));
          }
          expect(preference.audio, isTrue);
          expect(preference.bitrateMode, StreamBitrateMode.auto);
        } finally {
          await t.pumpWidget(const SizedBox.shrink());
          final done = <RemoteSession>[];
          for (final session in sessions) {
            unawaited(
              session.close().then<void>((_) {
                done.add(session);
              }),
            );
          }
          await t.pump(const Duration(seconds: 3));
          expect(
            done.length,
            sessions.length,
            reason: 'exact old-owner media retirement must finish',
          );
          for (final engine in media) {
            unawaited(engine.events.close());
          }
          await t.pump();
        }
      },
    );
  }
}
