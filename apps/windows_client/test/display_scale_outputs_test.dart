// display-scale-outputs-v1 on the Windows client: independent outputs (two
// monitors) negotiate the scale, results carry each monitor's actual value and
// the 배율 menu works in an all-monitors session. Injected HTTP/socket/media
// only; no Windows DPI, native window or VM is involved.
import 'dart:async';
import 'dart:convert';

import 'package:flutter/material.dart';
import 'package:flutter/services.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:multitap_client/data/broker_api.dart';
import 'package:multitap_client/data/display_scale.dart';
import 'package:multitap_client/data/remote_session.dart';
import 'package:multitap_client/models/session_display_mode.dart';
import 'package:multitap_client/ui/remote_view.dart';

import 'power_api_test.dart' show ApiClient;
import 'remote_session_test.dart' show SessionSocket, flush;
import 'stream_display_entry_test.dart' show entryMachine;
import 'track_stream_test.dart' as track;

const _origin = 'https://cloud.example.com';
const _markers = {
  'displayScaleProtocol': 'display-scale-v1',
  'displayScaleOutputsProtocol': 'display-scale-outputs-v1',
};

Map<String, dynamic> _inventory({required bool outputs}) => {
  'desktops': [
    {
      'id': 'pve-149',
      'name': 'TESTER',
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
          'displayScaleProtocol': 'display-scale-v1',
          if (outputs)
            'displayScaleOutputsProtocol': 'display-scale-outputs-v1',
        },
      },
    },
  ],
};

Map<String, dynamic> _granted({int? scale, bool profile = false}) => {
  ...track.response(),
  ..._markers,
  'displayScale': ?scale,
  if (profile) 'streamProfileTracksProtocol': 'group-v1',
};

BrokerConnection _connection({bool granted = true}) =>
    BrokerConnection.fromJson(
      granted ? _granted() : track.response(),
      Uri.parse(_origin),
      requestedOptions: track.options,
      requestedTracks: track.plan(),
      requestedDisplayScaleControl: granted,
      requestedDisplayScaleOutputs: granted,
    );

Map<String, Object?> _output(
  String id,
  int scale,
  int maximum, [
  int code = 0,
]) => {
  'id': id,
  'ok': code == 0,
  'scale': scale,
  'maximum': maximum,
  'code': code,
};

Map<String, dynamic> _applied(Object? outputs) => {
  'type': 'display.scale.applied',
  'sessionId': 'track-session',
  'protocol': 'display-scale-v1',
  'ok': true,
  'scale': 175,
  'maximum': 200,
  'code': 0,
  'outputs': outputs,
};

void main() {
  test(
    'independent outputs ask for the scale only where opted in and advertised',
    () async {
      for (final profile in [false, true]) {
        for (final enabled in [false, true]) {
          for (final outputs in [false, true]) {
            final asked = enabled && outputs;
            final reply = asked
                ? _granted(scale: 150, profile: profile)
                : {
                    ...track.response(),
                    if (profile) 'streamProfileTracksProtocol': 'group-v1',
                  };
            final client = ApiClient()
              ..responses.add((200, _inventory(outputs: outputs)))
              ..responses.add((200, reply));
            final api = HttpBrokerApi(
              origin: Uri.parse(_origin),
              client: client,
              requestDisplayScaleControl: enabled,
            );
            await api.desktops('fixture');
            final connection = profile
                ? await api.connectProfileTracks(
                    'fixture',
                    'pve-149',
                    track.plan(),
                    track.options,
                    displayScale: 150,
                  )
                : await api.connectTracks(
                    'fixture',
                    'pve-149',
                    track.plan(),
                    track.options,
                    displayScale: 150,
                  );
            final body =
                jsonDecode(client.requests.last.body.toString()) as Map;
            final reason = 'profile=$profile enabled=$enabled outputs=$outputs';
            expect(
              body['displayScaleProtocol'],
              asked ? 'display-scale-v1' : isNull,
              reason: reason,
            );
            expect(
              body['displayScaleOutputsProtocol'],
              asked ? 'display-scale-outputs-v1' : isNull,
              reason: reason,
            );
            expect(body['displayScale'], asked ? 150 : isNull, reason: reason);
            expect(connection.displayScaleOutputs, asked, reason: reason);
            expect(connection.displayScaleControl, asked, reason: reason);
            expect(connection.displayScale, asked ? 150 : isNull);
          }
        }
      }
    },
  );

  test('a grant must match the request and the kind of session', () {
    final origin = Uri.parse(_origin);
    expect(_connection().displayScaleOutputs, isTrue);
    for (final (data, requested) in [
      // Independent outputs with display-scale-v1 but not the outputs marker.
      ({...track.response(), 'displayScaleProtocol': 'display-scale-v1'}, true),
      // Not asked for.
      (_granted(), false),
      // The outputs marker without display-scale-v1, or another version.
      (
        {
          ...track.response(),
          'displayScaleOutputsProtocol': 'display-scale-outputs-v1',
        },
        true,
      ),
      (
        {
          ..._granted(),
          'displayScaleOutputsProtocol': 'display-scale-outputs-v2',
        },
        true,
      ),
    ]) {
      expect(
        () => BrokerConnection.fromJson(
          data,
          origin,
          requestedOptions: track.options,
          requestedTracks: track.plan(),
          requestedDisplayScaleControl: true,
          requestedDisplayScaleOutputs: requested,
        ),
        throwsA(isA<BrokerException>()),
        reason: '$data',
      );
    }
    // A single-output connection never carries the outputs marker.
    expect(
      () => BrokerConnection.fromJson(
        {
          'sessionId': 'single-session',
          'webSocketUrl':
              'wss://cloud.example.com/api/v1/sessions/single-session',
          'mode': 'stream',
          ..._markers,
        },
        origin,
        requestedDisplayScaleControl: true,
        requestedDisplayScaleOutputs: true,
      ),
      throwsA(isA<BrokerException>()),
    );
  });

  test('each monitor result is exact', () {
    final both = DisplayScaleApplied.parse(
      _applied([_output('0', 150, 200), _output('1', 150, 200)]),
      outputs: true,
    )!;
    expect(both.commonScale, 150);
    final split = DisplayScaleApplied.parse(
      _applied([_output('0', 175, 175), _output('1', 200, 200)]),
      outputs: true,
    )!;
    expect(split.commonScale, isNull);
    expect(split.outputs.map((o) => (o.id, o.scale, o.maximum)).toList(), [
      ('0', 175, 175),
      ('1', 200, 200),
    ]);
    for (final bad in <Object?>[
      null,
      'outputs',
      <Object?>[],
      [_output('0', 150, 200), _output('0', 150, 200)],
      [_output('0', 150, 200), _output('1', 150, 200), _output('1', 150, 200)],
      [_output('2', 150, 200)],
      [
        {..._output('0', 150, 200), 'extra': 1},
      ],
      [_output('0', 160, 200)],
      [
        {'id': '0', 'ok': true, 'scale': 0, 'maximum': 0, 'code': 0},
      ],
    ]) {
      expect(
        DisplayScaleApplied.parse(_applied(bad), outputs: true),
        isNull,
        reason: '$bad',
      );
    }
    // A single-output session's parse is unchanged.
    final single = DisplayScaleApplied.parse(
      _applied(null)..remove('outputs'),
    )!;
    expect(single.outputs, isEmpty);
    expect(single.commonScale, 175);
  });

  test('a granted dual session keeps each monitor result and asks again on the same socket', () async {
    final socket = SessionSocket();
    final media = track.TrackMedia();
    final session = RemoteSession(
      connection: _connection(),
      token: 'fixture-only',
      media: media,
      dialer: (_, _) async => socket,
    );
    unawaited(session.start());
    await flush();
    expect(session.independentOutputs, isTrue);
    expect(session.displayScaleSupported, isTrue);
    expect(session.displayScaleWanted, isNull);
    socket.incoming.add(
      jsonEncode(_applied([_output('0', 175, 175), _output('1', 200, 200)])),
    );
    await flush();
    expect(session.closed, isFalse);
    expect(
      session.displayScaleApplied?.outputs.map((o) => (o.id, o.scale)).toList(),
      [('0', 175), ('1', 200)],
    );
    expect(session.requestDisplayScale(150), isTrue);
    expect(session.requestDisplayScale(150), isFalse);
    expect(socket.sent.where((m) => m['type'] == 'display.scale').toList(), [
      {'type': 'display.scale', 'scale': 150},
    ]);
    final ungranted = RemoteSession(
      connection: _connection(granted: false),
      token: 'fixture-only',
      media: track.TrackMedia(),
      dialer: (_, _) async => SessionSocket(),
    );
    expect(ungranted.displayScaleSupported, isFalse);
    await session.close();
    await media.events.close();
  });

  for (final granted in [true, false]) {
    testWidgets('all-monitors 배율 menu follows the outputs grant: $granted', (
      t,
    ) async {
      t.view.devicePixelRatio = 1;
      t.view.physicalSize = const Size(1920, 720);
      addTearDown(t.view.resetDevicePixelRatio);
      addTearDown(t.view.resetPhysicalSize);
      final messenger =
          TestDefaultBinaryMessengerBinding.instance.defaultBinaryMessenger;
      const window = MethodChannel('multitap/window');
      const events = MethodChannel('multitap/window/events');
      messenger.setMockMethodCallHandler(events, (_) async => null);
      messenger.setMockMethodCallHandler(window, (call) async {
        if (call.method == 'monitorLayout') {
          return {
            'x': 0,
            'y': 0,
            'width': 1920,
            'height': 720,
            'monitors': [
              {
                'id': '0',
                'x': 0,
                'y': 0,
                'width': 640,
                'height': 360,
                'primary': true,
                'dpi': 96,
              },
              {
                'id': '1',
                'x': 640,
                'y': 0,
                'width': 1280,
                'height': 720,
                'primary': false,
                'dpi': 96,
              },
            ],
          };
        }
        return call.method == 'allMonitors' || call.method == 'fullscreen';
      });
      addTearDown(() {
        messenger.setMockMethodCallHandler(window, null);
        messenger.setMockMethodCallHandler(events, null);
      });
      final socket = SessionSocket();
      final media = track.TrackMedia();
      final session = RemoteSession(
        connection: _connection(granted: granted),
        token: 'fixture-only-token',
        media: media,
        dialer: (_, _) async => socket,
      );
      // The reconnect after a chosen scale (a fresh session, never started
      // here beyond its dial).
      final scaled = <int>[];
      final again = RemoteSession(
        connection: _connection(granted: granted),
        token: 'fixture-only-token',
        media: track.TrackMedia(),
        dialer: (_, _) async => SessionSocket(),
      );
      try {
        await t.pumpWidget(
          MaterialApp(
            home: RemoteView(
              machine: entryMachine(2),
              initialDisplayMode: SessionDisplayMode.allMonitors,
              load: () async => session,
              onConnectTracks: (_, _) async => session,
              onConnectTracksScaled: (_, _, scale) async {
                scaled.add(scale);
                return scaled.length == 1 ? session : again;
              },
            ),
          ),
        );
        for (var i = 0; i < 6; i++) {
          await t.pump();
        }
        socket.incoming.add(jsonEncode(track.info()));
        await t.pump();
        await t.pump();
        socket.incoming.add(track.packet(0));
        socket.incoming.add(track.packet(1));
        media.currentStats = track.trackStats();
        await t.pump(const Duration(seconds: 1));
        await t.pump();
        await t.pump();
        expect(session.frameReady, isTrue, reason: session.error);
        if (find.byKey(const Key('session-command-bar')).evaluate().isEmpty) {
          await t.tap(find.byKey(const Key('expand-session-toolbar')));
          await t.pump();
        }
        // 배율 is in the 화면 panel, under the screen modes (2026-11-10).
        await t.tap(find.byKey(const Key('stream-menu-화면')));
        await t.pump();
        await t.pump(const Duration(milliseconds: 400));
        expect(find.byKey(const Key('session-screen-panel')), findsOneWidget);
        expect(find.byKey(const Key('display-scale-panel')), findsOneWidget);
        final choice = t.widget<TextButton>(
          find.byKey(const Key('display-scale-150')),
        );
        if (!granted) {
          expect(choice.onPressed, isNull);
          expect(find.text('이 연결에서는 배율을 바꿀 수 없어요.'), findsOneWidget);
        } else {
          expect(choice.onPressed, isNotNull);
          expect(find.text('자동 · 현재 배율 확인 전'), findsOneWidget);
          await t.tap(find.byKey(const Key('display-scale-150')));
          await t.pump();
          await t.pump(const Duration(milliseconds: 400));
          expect(
            socket.sent.where((m) => m['type'] == 'display.scale').toList(),
            [
              {'type': 'display.scale', 'scale': 150},
            ],
          );
          expect(session.displayScaleWanted, 150);
          // The session ends; reconnecting asks for the chosen scale again
          // (the Host put the previous value back when the session ended).
          socket.incoming.add(
            jsonEncode({
              'type': 'session.ended',
              'reason': 'host_disconnected',
            }),
          );
          await t.pump();
          await t.pump(const Duration(milliseconds: 400));
          final retry = find.byKey(const Key('retry-remote-connection'));
          expect(retry, findsOneWidget);
          await t.tap(retry);
          for (var i = 0; i < 6; i++) {
            await t.pump(const Duration(milliseconds: 100));
          }
          expect(scaled, [100, 150]);
        }
      } finally {
        await t.pumpWidget(const SizedBox.shrink());
        final closing = session.close();
        final closingAgain = again.close();
        await t.pump(const Duration(seconds: 3));
        await t.pump();
        await closing;
        await closingAgain;
        await media.events.close();
      }
    });
  }
}
