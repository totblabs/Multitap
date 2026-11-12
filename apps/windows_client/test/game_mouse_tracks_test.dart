// 마우스 잠금 (formerly 게임 마우스) on independent outputs (모든 모니터, relative-mouse-tracks-v1):
// asked with the tracks cursor and its composition, accepted only as such,
// movement and buttons/wheel at the remote cursor carry the topology and no
// output, and the composed cursor is asked on the session socket. Injected
// HTTP/socket/media only; no native capture, VM or real pointer.
import 'dart:async';
import 'dart:convert';

import 'package:flutter/material.dart';
import 'package:flutter/services.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:multitap_client/data/broker_api.dart';
import 'package:multitap_client/data/relative_mouse.dart';
import 'package:multitap_client/data/remote_session.dart';
import 'package:multitap_client/models/session_display_mode.dart';
import 'package:multitap_client/ui/remote_view.dart';
import 'package:multitap_client/data/session_windows.dart';

import 'power_api_test.dart' show ApiClient;
import 'remote_session_test.dart' show SessionSocket, flush;
import 'stream_display_entry_test.dart' show entryMachine;
import 'track_stream_test.dart' as track;

/// Dual-output media whose runner says this client is not inside RDP.
class _GameTrackMedia extends track.TrackMedia {
  @override
  Future<Map<String, dynamic>> stats() async => {
    ...await super.stats(),
    'remoteSession': false,
  };
}

/// The runner's clip and raw input: records how each capture was asked for.
class _Native implements RelativeMouseNative {
  final concealed = <bool>[];
  @override
  Future<RelativeMousePath?> engage(
    Rect? picture, {
    bool pin = false,
    bool conceal = false,
  }) async {
    concealed.add(conceal);
    return RelativeMousePath.injected;
  }

  @override
  Future<void> release() async {}
  @override
  Future<void> armKey(bool armed) async {}
  @override
  void listen(void Function(String event) onEvent) {}
  @override
  void dispose() {}
}

const _origin = 'https://cloud.example.com';
const _cursor = {
  'cursorProtocol': 'cursor-v1',
  'cursorTracksProtocol': 'cursor-tracks-v1',
};
const _game = {
  'relativeMouseProtocol': 'relative-mouse-v1',
  'cursorComposeProtocol': 'cursor-compose-v1',
};

Map<String, dynamic> _inventory(Map<String, Object> capabilities) => {
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
        'capabilities': {'service': true, 'streaming': true, ...capabilities},
      },
    },
  ],
};

BrokerConnection _connection(Map<String, Object> grant) =>
    BrokerConnection.fromJson(
      {...track.response(), ...grant},
      Uri.parse(_origin),
      requestedOptions: track.options,
      requestedTracks: track.plan(),
      requestedCursor: true,
      requestedCursorTracks: true,
      requestedRelativeMouse: true,
      requestedCursorCompose: true,
    );

void main() {
  test('independent outputs ask for 마우스 잠금 only with the tracks cursor, movement and composition advertised', () async {
    for (final profile in [false, true]) {
      for (final (capabilities, asked) in <(Map<String, Object>, bool)>[
        (_cursor, false),
        ({..._cursor, 'relativeMouseProtocol': 'relative-mouse-v1'}, false),
        ({..._cursor, 'cursorComposeProtocol': 'cursor-compose-v1'}, false),
        (_game, false),
        ({..._cursor, ..._game}, true),
      ]) {
        final client = ApiClient()
          ..responses.add((200, _inventory(capabilities)))
          ..responses.add((
            200,
            {
              ...track.response(),
              if (capabilities.containsKey('cursorTracksProtocol')) ..._cursor,
              if (asked) ..._game,
              if (profile) 'streamProfileTracksProtocol': 'group-v1',
            },
          ));
        final api = HttpBrokerApi(
          origin: Uri.parse(_origin),
          client: client,
          requestRelativeMouse: true,
        );
        await api.desktops('fixture');
        final connection = profile
            ? await api.connectProfileTracks(
                'fixture',
                'pve-149',
                track.plan(),
                track.options,
              )
            : await api.connectTracks(
                'fixture',
                'pve-149',
                track.plan(),
                track.options,
              );
        final body = jsonDecode(client.requests.last.body.toString()) as Map;
        final reason = 'profile=$profile $capabilities';
        expect(
          body['relativeMouseProtocol'],
          asked ? 'relative-mouse-v1' : isNull,
          reason: reason,
        );
        expect(
          body['cursorComposeProtocol'],
          asked ? 'cursor-compose-v1' : isNull,
          reason: reason,
        );
        expect(connection.relativeMouse, asked, reason: reason);
        expect(connection.cursorCompose, asked, reason: reason);
        api.close();
      }
    }
  });

  test(
    'a tracks grant of movement needs the tracks cursor and its composition',
    () {
      final granted = _connection({..._cursor, ..._game});
      expect(granted.relativeMouse, isTrue);
      expect(granted.cursorCompose, isTrue);
      // Composition alone (a Host that takes it, a client that did not get
      // movement) is a valid grant; movement without it is not.
      expect(
        _connection({..._cursor, 'cursorComposeProtocol': 'cursor-compose-v1'})
            .cursorCompose,
        isTrue,
      );
      for (final grant in <Map<String, Object>>[
        {..._cursor, 'relativeMouseProtocol': 'relative-mouse-v1'},
        {..._game},
        {'cursorProtocol': 'cursor-v1', ..._game},
      ]) {
        expect(
          () => _connection(grant),
          throwsA(isA<BrokerException>()),
          reason: '$grant',
        );
      }
    },
  );

  test('a granted dual session sends movement and buttons with the topology and no output, and asks for the composed cursor', () async {
    final socket = SessionSocket(), media = track.TrackMedia();
    final session = RemoteSession(
      connection: _connection({..._cursor, ..._game}),
      token: 'fixture-only',
      media: media,
      dialer: (_, _) async => socket,
    );
    unawaited(session.start());
    await flush();
    socket.incoming.add(jsonEncode(track.info()));
    await flush();
    socket.incoming.add(track.packet(0));
    socket.incoming.add(track.packet(1));
    await flush();
    media.currentStats = track.trackStats();
    for (
      var n = 0;
      n < 30 && !socket.sent.any((e) => e['type'] == 'stream.tracks.ready');
      n++
    ) {
      await Future<void>.delayed(const Duration(milliseconds: 100));
    }
    expect(socket.sent.any((e) => e['type'] == 'stream.tracks.ready'), isTrue);
    await session.capture(true);
    expect(session.captured, isTrue);
    expect(session.relativeMouse, isTrue);
    expect(session.cursorCompose, isTrue);
    expect(session.cursorEcho, isFalse, reason: 'never on independent outputs');
    expect(session.composeCursor(true), isTrue);
    expect(socket.sent.last, {'type': 'cursor.compose', 'on': true});
    for (final event in <Map<String, dynamic>>[
      {'type': 'input', 'event': 'moveRelative', 'dx': 12, 'dy': -3},
      {'type': 'input', 'event': 'button', 'button': 'left', 'down': true},
      {'type': 'input', 'event': 'wheel', 'delta': -120},
    ]) {
      media.events.add(event);
      await flush();
      expect(socket.sent.last, {...event, 'topologyEpoch': 1});
    }
    // Positions keep their output.
    session.input({
      'type': 'input',
      'event': 'move',
      'track': '0',
      'x': 0.5,
      'y': 0.5,
    });
    await flush();
    expect(socket.sent.last['track'], '0');
    expect(socket.sent.last['epoch'], 1);
    await session.close();
    await media.events.close();
  });

  // The bar offers 마우스 잠금 spread over every monitor only with the grant;
  // its 확인 hands the mouse over parked (the cursor comes in the video).
  for (final granted in [true, false]) {
    testWidgets(
      'all monitors: 마우스 잠금 is offered and hands over with the composed cursor (granted: $granted)',
      (t) async {
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
        final socket = SessionSocket(),
            media = _GameTrackMedia(),
            native = _Native();
        final session = RemoteSession(
          connection: _connection(granted ? {..._cursor, ..._game} : _cursor),
          token: 'fixture-only-token',
          media: media,
          dialer: (_, _) async => socket,
        );
        try {
          await t.pumpWidget(
            MaterialApp(
              home: RemoteView(
                machine: entryMachine(2),
                initialDisplayMode: SessionDisplayMode.allMonitors,
                load: () async => session,
                onConnectTracks: (_, _) async => session,
                windowActions: SessionWindowActions(closeWindow: () async {}),
                relativeMouseNative: native,
                sessionGuideEnabled: false,
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
          for (var i = 0; i < 6; i++) {
            await t.pump();
          }
          expect(session.frameReady, isTrue, reason: session.error);
          await t.tap(find.byKey(const Key('expand-session-toolbar')));
          for (var i = 0; i < 6; i++) {
            await t.pump();
          }
          // 마우스 잠금 is a switch row of the bar's 설정 panel (2026-11-10).
          // No time passes in these frames, so the panel is up at the start
          // of its entrance and goes at once when the row is chosen.
          await t.tap(find.byKey(const Key('remote-settings')));
          for (var i = 0; i < 6; i++) {
            await t.pump();
          }
          expect(
            find.byKey(const Key('session-settings-panel')),
            findsOneWidget,
          );
          final item = find.byKey(const Key('session-game-mouse'));
          if (!granted) {
            expect(item, findsNothing);
            return;
          }
          // The row hands its key to its InkWell too: the first is the row.
          expect(item, findsWidgets);
          await t.tap(item.first);
          for (var i = 0; i < 6; i++) {
            await t.pump();
          }
          expect(find.byKey(const Key('session-settings-panel')), findsNothing);
          await t.tap(find.byKey(const Key('game-mouse-dialog-ok')));
          for (var i = 0; i < 12; i++) {
            await t.pump(const Duration(milliseconds: 50));
          }
          expect(native.concealed, [
            true,
          ], reason: 'parked, nothing drawn here');
          expect(socket.sent.where((e) => e['type'] == 'cursor.compose'), [
            {'type': 'cursor.compose', 'on': true},
          ]);
        } finally {
          await t.pumpWidget(const SizedBox.shrink());
          final closing = session.close();
          await t.pump(const Duration(seconds: 3));
          await t.pump();
          await closing;
          await media.events.close();
        }
      },
    );
  }
}
