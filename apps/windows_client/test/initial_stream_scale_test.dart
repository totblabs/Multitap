import 'dart:async';
import 'dart:convert';
import 'dart:io';

import 'package:flutter/material.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:multitap_client/data/broker_api.dart';
import 'package:multitap_client/app.dart';
import 'package:multitap_client/data/remote_session.dart';
import 'package:multitap_client/models/stream_options.dart';
import 'package:multitap_client/ui/remote_view.dart';
import 'package:multitap_client/ui/sign_in_screen.dart';
import 'package:multitap_client/ui/workspace_shell.dart';

import 'display_remote_test.dart' show machine;
import 'remote_session_test.dart'
    show SessionSocket, SessionMedia, info, packet;
import 'session_recovery_test.dart' show mockPresentation;
import 'power_api_test.dart'
    show ApiClient, ApiHeaders, ApiRequest, ApiResponse;
import 'pc_login_replacement_lifetime_test.dart' show QuietWatch;
import 'track_stream_test.dart' as track;

class _Headers extends ApiHeaders {
  @override
  String? value(String name) =>
      name.toLowerCase() == pcSessionPolicyHeader.toLowerCase()
      ? windowsPcSessionPolicy
      : null;
}

class _Response extends Stream<List<int>> implements HttpClientResponse {
  _Response(this.value, this.trace, this.pendingBodies);
  final Map<String, dynamic> value;
  final List<String> trace;
  final List<void Function()> pendingBodies;
  @override
  final int statusCode = 200;
  @override
  final HttpHeaders headers = _Headers();
  @override
  StreamSubscription<List<int>> listen(
    void Function(List<int>)? onData, {
    Function? onError,
    void Function()? onDone,
    bool? cancelOnError,
  }) {
    trace.add('response.listen');
    final body = StreamController<List<int>>(sync: true);
    final subscription = body.stream.listen(
      onData,
      onError: onError,
      onDone: onDone,
      cancelOnError: cancelOnError,
    );
    pendingBodies.add(() {
      trace.add('controller.add');
      body.add(utf8.encode(jsonEncode(value)));
      trace.add('controller.close');
      unawaited(body.close());
    });
    return subscription;
  }

  @override
  dynamic noSuchMethod(Invocation invocation) => super.noSuchMethod(invocation);
}

class _AppRequest extends ApiRequest {
  _AppRequest(
    String method,
    Uri uri,
    Map<String, dynamic> response,
    this.trace,
    List<void Function()> pendingBodies,
  ) : wireResponse = _Response(response, trace, pendingBodies),
      super(method, uri, ApiResponse(200, response));
  final _Response wireResponse;
  final List<String> trace;
  @override
  Future<HttpClientResponse> close() async {
    trace.add('request.close');
    return wireResponse;
  }
}

// The app constructs its own real HttpBrokerApi. Only HttpClient's boundary
// is substituted so this catches missing opt-in flags in either constructor.
class _AppHttp extends ApiClient {
  _AppHttp(this.capable);
  final bool capable;
  final trace = <String>[];
  final pendingBodies = <void Function()>[];
  void deliverResponses() {
    final ready = List<void Function()>.of(pendingBodies);
    pendingBodies.clear();
    for (final deliver in ready) {
      deliver();
    }
  }

  @override
  Future<HttpClientRequest> openUrl(String method, Uri uri) async {
    trace.add('$method ${uri.path}');
    final Map<String, dynamic> response;
    if (uri.path == '/api/v1/auth/login') {
      response = {
        'sessionToken': 'fixture-only',
        'expiresAt': DateTime.now()
            .add(const Duration(hours: 8))
            .toUtc()
            .toIso8601String(),
        'user': {
          'id': 'fixture-account',
          'username': 'fixture',
          'role': 'user',
        },
      };
    } else if (uri.path == '/api/v1/desktops') {
      response = {
        'desktops': [
          {
            'id': 'pve-149',
            'name': 'fixture',
            'kind': 'vm',
            'description': '',
            'os': 'Windows',
            'status': 'ready',
            'canConnect': true,
            'host': {
              'connected': true,
              'capabilities': {
                'service': true,
                'streaming': true,
                if (capable) ...{
                  'displayScaleProtocol': 'display-scale-v1',
                  'displayScaleOutputsProtocol': 'display-scale-outputs-v1',
                },
              },
            },
          },
        ],
      };
    } else if (uri.path.endsWith('/connect')) {
      response = {
        ...track.response(),
        'webSocketUrl': 'wss://${uri.authority}/api/v1/sessions/track-session',
        if (capable) ...{
          'displayScaleProtocol': 'display-scale-v1',
          'displayScaleOutputsProtocol': 'display-scale-outputs-v1',
          'displayScale': 100,
        },
      };
    } else if (uri.path.startsWith('/api/v1/sessions/')) {
      response = {'released': true, 'sessionId': uri.path.split('/').last};
    } else {
      response = <String, dynamic>{}; // Optional diagnostics only.
    }
    final request = _AppRequest(method, uri, response, trace, pendingBodies);
    requests.add(request);
    return request;
  }
}

Future<void> _pumpHttp(WidgetTester tester, _AppHttp client) async {
  client.deliverResponses();
  await tester.pump(const Duration(milliseconds: 100));
  // The real HttpBrokerApi consumes a Dart IO response stream. Let its
  // cancellation/completion queue progress without awaiting the App future
  // inside runAsync, since that future also needs widget microtasks.
  await tester.runAsync(() => Future<void>.delayed(Duration.zero));
  await tester.pump();
}

void main() {
  test('HTTP fixture authenticates through real interactive login and PC policy parser', () async {
    final client = _AppHttp(true);
    final api = HttpBrokerApi(
      origin: Uri.parse('https://cloud.example.com'),
      client: client,
      requestPcLoginReplacement: true,
    );
    BrokerSession? result;
    Object? failure;
    api
        .loginInteractively('fixture', 'fixture-password')
        .then<void>(
          (value) => result = value,
          onError: (Object error) => failure = error,
        );
    for (var i = 0; i < 20 && result == null && failure == null; i++) {
      client.deliverResponses();
      await Future<void>.delayed(Duration.zero);
    }
    expect(failure, isNull, reason: '${client.trace}');
    expect(result, isNotNull, reason: '${client.trace}');
    expect(result!.username, 'fixture');
    expect(result!.pcSessionPolicy, windowsPcSessionPolicy);
    api.close();
  });
  testWidgets('fresh Windows stream requests100 before its first picture', (
    t,
  ) async {
    mockPresentation();
    final socket = SessionSocket(), media = SessionMedia();
    final stream = RemoteSession(
      connection: BrokerConnection(
        sessionId: 'initial-scale',
        webSocketUrl: Uri.parse('wss://fixture.invalid/initial-scale'),
        displayScale: 100,
      ),
      token: 'fixture-only',
      media: media,
      dialer: (_, _) async => socket,
    );
    final requests = <int>[];
    var legacy = 0;
    try {
      await t.pumpWidget(
        MaterialApp(
          home: RemoteView(
            sessionGuideEnabled: false,
            machine: machine(1),
            load: () async {
              legacy++;
              return stream;
            },
            onConnectScaled: (_, _, scale) async {
              expect(media.starts, 0);
              requests.add(scale);
              return stream;
            },
          ),
        ),
      );
      for (var i = 0; i < 12; i++) {
        await t.pump(const Duration(milliseconds: 20));
      }
      expect(requests, [100]);
      expect(legacy, 0);
      expect(media.starts, 0);
    } finally {
      await t.pumpWidget(const SizedBox.shrink());
      await t.pump(const Duration(seconds: 3));
      await stream.close();
      await media.events.close();
    }
  }, variant: TargetPlatformVariant.only(TargetPlatform.windows));

  testWidgets('manual Windows scale survives widget resize and reconnect', (
    t,
  ) async {
    mockPresentation();
    t.view.devicePixelRatio = 1;
    t.view.physicalSize = const Size(1280, 800);
    addTearDown(t.view.reset);
    final sockets = [SessionSocket(), SessionSocket()];
    final media = [SessionMedia(), SessionMedia()];
    final sessions = [
      for (var i = 0; i < 2; i++)
        RemoteSession(
          connection: BrokerConnection(
            sessionId: 'scale-$i',
            webSocketUrl: Uri.parse('wss://fixture.invalid/scale-$i'),
            displayScale: i == 0 ? 100 : 150,
          ),
          token: 'fixture-only',
          media: media[i],
          dialer: (_, _) async => sockets[i],
        ),
    ];
    final requested = <int>[];
    try {
      await t.pumpWidget(
        MaterialApp(
          home: RemoteView(
            machine: machine(1),
            sessionGuideEnabled: false,
            load: () async => throw StateError('scaled callback required'),
            onConnectScaled: (_, _, scale) async {
              requested.add(scale);
              return sessions[requested.length - 1];
            },
          ),
        ),
      );
      for (var i = 0; i < 6; i++) {
        await t.pump();
      }
      expect(requested, [100]);
      sockets.first.incoming.add(jsonEncode(info));
      await t.pump();
      sockets.first.incoming.add(packet());
      await t.pump(const Duration(seconds: 1));
      await t.pump();
      expect(sessions.first.frameReady, isTrue);
      if (find.byKey(const Key('session-command-bar')).evaluate().isEmpty) {
        await t.tap(find.byKey(const Key('expand-session-toolbar')));
        await t.pump();
      }
      // 배율 is in the 화면 panel, under the screen modes (2026-11-10).
      await t.tap(find.byKey(const Key('stream-menu-화면')));
      await t.pump();
      await t.pump(const Duration(milliseconds: 400));
      expect(find.byKey(const Key('display-scale-panel')), findsOneWidget);
      await t.tap(find.byKey(const Key('display-scale-150')));
      await t.pump();
      await t.pump(const Duration(milliseconds: 400));
      expect(sessions.first.displayScaleWanted, 150);
      t.view.physicalSize = const Size(1440, 900);
      await t.pump(const Duration(seconds: 1));
      expect(requested, [100]);
      expect(sessions.first.displayScaleWanted, 150);
      sockets.first.incoming.add(
        jsonEncode({'type': 'session.ended', 'reason': 'host_disconnected'}),
      );
      await t.pump();
      await t.pump(const Duration(milliseconds: 400));
      await t.tap(find.byKey(const Key('retry-remote-connection')));
      for (var i = 0; i < 8; i++) {
        await t.pump(const Duration(milliseconds: 100));
      }
      expect(requested, [100, 150]);
    } finally {
      await t.pumpWidget(const SizedBox.shrink());
      await t.pump(const Duration(seconds: 3));
      for (final session in sessions) {
        await session.close();
      }
      for (final engine in media) {
        await engine.events.close();
      }
    }
  }, variant: TargetPlatformVariant.only(TargetPlatform.windows));

  for (final capable in [false, true]) {
    for (final changeServer in [false, true]) {
      testWidgets(
        'app-created HTTP dual100 and legacy capability gate: $capable/serverchange$changeServer',
        (t) async {
          mockPresentation();
          final client = _AppHttp(capable);
          await HttpOverrides.runZoned(() async {
            try {
              await t.pumpWidget(
                MultitapApp(
                  serverOrigin: Uri.parse('https://cloud.example.com'),
                  desktopWatchFactory: (_, _) => QuietWatch(),
                ),
              );
              await t.pumpAndSettle();
              if (changeServer) {
                await t
                    .widget<SignInScreen>(find.byType(SignInScreen))
                    .onServerChanged(
                      Uri.parse('https://second.fixture.invalid'),
                    );
                await t.pumpAndSettle();
              }
              final login = t.widget<SignInScreen>(find.byType(SignInScreen));
              Object? loginResult;
              login
                  .onLogin('fixture', 'fixture-password')
                  .then((value) => loginResult = value);
              for (var i = 0; i < 40 && loginResult == null; i++) {
                await _pumpHttp(t, client);
              }
              final signIn = find.byType(SignInScreen);
              final loginError = signIn.evaluate().isEmpty
                  ? null
                  : t.widget<SignInScreen>(signIn).error;
              expect(
                loginResult,
                SignInResult.success,
                reason:
                    'login finished through app-created HTTP: ${client.trace}; error: $loginError',
              );
              await t.pumpAndSettle();
              final shell = t.widget<WorkspaceShell>(
                find.byType(WorkspaceShell),
              );
              expect(shell.onConnectTracksScaled, isNotNull);
              RemoteSession? session;
              Object? failure;
              shell
                  .onConnectTracksScaled!(
                    'pve-149',
                    track.plan(),
                    track.options,
                    100,
                  )
                  .then<void>(
                    (value) => session = value,
                    onError: (Object error) => failure = error,
                  );
              for (
                var i = 0;
                i < 40 && session == null && failure == null;
                i++
              ) {
                await _pumpHttp(t, client);
              }
              expect(failure, isNull);
              expect(
                session,
                isNotNull,
                reason:
                    'connect finished: ${client.requests.map((r) => r.uri.path).toList()}',
              );
              final request = client.requests.singleWhere(
                (r) => r.uri.path.endsWith('/connect'),
              );
              final body = jsonDecode(request.body.toString()) as Map;
              expect(body['tracks'], track.plan().toJson());
              expect(
                request.headers.values[HttpHeaders.authorizationHeader],
                'Bearer fixture-only',
              );
              expect(body.containsKey('displayScaleProtocol'), capable);
              expect(body.containsKey('displayScaleOutputsProtocol'), capable);
              expect(body.containsKey('displayScale'), capable);
              expect(body['displayScale'], capable ? 100 : isNull);
              expect(session!.connection.displayScale, capable ? 100 : isNull);
              expect(session!.connection.displayScaleOutputs, capable);
              var closed = false;
              final closing = session!.close().then((_) => closed = true);
              for (var i = 0; i < 40 && !closed; i++) {
                await _pumpHttp(t, client);
              }
              expect(closed, isTrue);
              await closing;
            } finally {
              await t.pumpWidget(const SizedBox.shrink());
              await t.pump(const Duration(seconds: 3));
            }
          }, createHttpClient: (_) => client);
        },
        timeout: const Timeout(Duration(seconds: 45)),
      );
    }

    test(
      'new100 request uses current inventory and preserves legacy Host: $capable',
      () async {
        const sessionId = 'b44907ce-827b-4519-ac28-2066015e4870';
        final client = ApiClient()
          ..responses.add((
            200,
            {
              'desktops': [
                {
                  'id': 'pve-149',
                  'name': 'fixture',
                  'kind': 'vm',
                  'description': '',
                  'os': 'Windows',
                  'status': 'ready',
                  'canConnect': true,
                  'host': {
                    'connected': true,
                    'capabilities': {
                      'service': true,
                      'streaming': true,
                      if (capable) 'displayScaleProtocol': 'display-scale-v1',
                    },
                  },
                },
              ],
            },
          ))
          ..responses.add((
            200,
            {
              'sessionId': sessionId,
              'webSocketUrl':
                  'wss://fixture.invalid/api/v1/sessions/$sessionId',
              'mode': 'stream',
              'options': const StreamOptions().toJson(),
              if (capable) ...{
                'displayScaleProtocol': 'display-scale-v1',
                'displayScale': 100,
              },
            },
          ));
        final api = HttpBrokerApi(
          origin: Uri.parse('https://fixture.invalid'),
          client: client,
          requestDisplayScaleControl: true,
        );
        await api.desktops('fixture');
        final connection = await api.connectScaled(
          'fixture',
          'pve-149',
          options: const StreamOptions(),
          displayScale: 100,
        );
        final body = jsonDecode(client.requests.last.body.toString()) as Map;
        expect(body.containsKey('displayScaleProtocol'), capable);
        expect(body['displayScale'], capable ? 100 : isNull);
        expect(body.containsKey('displayScale'), capable);
        expect(connection.displayScale, capable ? 100 : isNull);
        expect(connection.displayScaleControl, capable);
        api.close();
      },
    );
  }
}
