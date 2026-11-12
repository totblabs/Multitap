// cursor-tracks-v1 on the Windows client: independent outputs (모든 모니터
// 사용) negotiate the separated cursor, hand its records (TVT1 kind 3) to the
// native presenter instead of the decoders and report the primary output's
// picture. Injected HTTP/socket/media only; no native presenter, VM or real
// cursor is involved.
import 'dart:async';
import 'dart:convert';
import 'dart:typed_data';

import 'package:flutter/material.dart';
import 'package:flutter/services.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:multitap_client/data/broker_api.dart';
import 'package:multitap_client/data/remote_cursor.dart';
import 'package:multitap_client/data/remote_session.dart';
import 'package:multitap_client/models/session_display_mode.dart';
import 'package:multitap_client/ui/remote_view.dart';

import 'power_api_test.dart' show ApiClient;
import 'remote_cursor_test.dart' show shapeRecord, stateRecord;
import 'remote_session_test.dart' show SessionSocket, flush;
import 'stream_display_entry_test.dart' show entryMachine;
import 'track_stream_test.dart' as track;

const _origin = 'https://cloud.example.com';
const _markers = {
  'cursorProtocol': 'cursor-v1',
  'cursorTracksProtocol': 'cursor-tracks-v1',
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

BrokerConnection _connection({bool granted = true}) =>
    BrokerConnection.fromJson(
      {...track.response(), if (granted) ..._markers},
      Uri.parse(_origin),
      requestedOptions: track.options,
      requestedTracks: track.plan(),
      requestedCursor: granted,
      requestedCursorTracks: granted,
    );

/// One record in its TVT1 envelope (kind 3, connector 255, track epoch 0).
Uint8List _cursor(
  Uint8List record, {
  int topology = 1,
  int connector = 255,
  int epoch = 0,
  int flags = 0,
}) {
  final bytes = Uint8List(28 + record.length)..setRange(0, 4, [84, 86, 84, 49]);
  bytes[4] = cursorMediaKind;
  bytes[5] = flags;
  bytes[6] = connector;
  final data = ByteData.sublistView(bytes);
  data.setUint32(8, topology, Endian.little);
  data.setUint32(12, epoch, Endian.little);
  data.setUint64(16, 5, Endian.little);
  data.setUint32(24, record.length, Endian.little);
  return bytes..setRange(28, bytes.length, record);
}

class _CursorTrackMedia extends track.TrackMedia implements RemoteCursorMedia {
  final records = <(int, Uint8List)>[];
  final views = <CursorView>[];
  @override
  Future<void> cursor(int epoch, Uint8List record) async =>
      records.add((epoch, Uint8List.fromList(record)));
  @override
  Future<void> cursorView(CursorView view) async => views.add(view);
}

/// A started dual session whose outputs have their first key frames.
Future<(RemoteSession, SessionSocket, _CursorTrackMedia)> _started({
  bool granted = true,
}) async {
  final socket = SessionSocket(), media = _CursorTrackMedia();
  final session = RemoteSession(
    connection: _connection(granted: granted),
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
  return (session, socket, media);
}

void main() {
  test('independent outputs ask for the separated cursor only where both markers are advertised', () async {
    for (final profile in [false, true]) {
      for (final (capabilities, asked) in <(Map<String, Object>, bool)>[
        (const {}, false),
        (const {'cursorProtocol': 'cursor-v1'}, false),
        (const {'cursorTracksProtocol': 'cursor-tracks-v1'}, false),
        (_markers, true),
      ]) {
        final client = ApiClient()
          ..responses.add((200, _inventory(capabilities)))
          ..responses.add((
            200,
            {
              ...track.response(),
              if (asked) ..._markers,
              if (profile) 'streamProfileTracksProtocol': 'group-v1',
            },
          ));
        final api = HttpBrokerApi(origin: Uri.parse(_origin), client: client);
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
          body['cursorProtocol'],
          asked ? 'cursor-v1' : isNull,
          reason: reason,
        );
        expect(
          body['cursorTracksProtocol'],
          asked ? 'cursor-tracks-v1' : isNull,
          reason: reason,
        );
        expect(connection.cursor, asked, reason: reason);
        expect(connection.cursorTracks, asked, reason: reason);
        api.close();
      }
    }
  });

  test('a grant must match the request and the kind of session', () {
    final origin = Uri.parse(_origin);
    expect(_connection().cursorTracks, isTrue);
    expect(_connection().cursor, isTrue);
    for (final (data, requested) in [
      // cursor-v1 on independent outputs without cursor-tracks-v1.
      ({...track.response(), 'cursorProtocol': 'cursor-v1'}, true),
      // The independent-outputs marker without cursor-v1, or another value.
      ({...track.response(), 'cursorTracksProtocol': 'cursor-tracks-v1'}, true),
      ({...track.response(), ..._markers, 'cursorTracksProtocol': 'v2'}, true),
      // Not asked for.
      ({...track.response(), ..._markers}, false),
    ]) {
      expect(
        () => BrokerConnection.fromJson(
          data,
          origin,
          requestedOptions: track.options,
          requestedTracks: track.plan(),
          requestedCursor: true,
          requestedCursorTracks: requested,
        ),
        throwsA(isA<BrokerException>()),
        reason: '$data',
      );
    }
    // A single-output connection never carries the independent-outputs marker.
    expect(
      () => BrokerConnection.fromJson(
        {
          'sessionId': 'single-session',
          'webSocketUrl': 'wss://cloud.example.com/api/v1/sessions/single',
          'mode': 'stream',
          ..._markers,
        },
        origin,
        requestedCursor: true,
        requestedCursorTracks: true,
      ),
      throwsA(isA<BrokerException>()),
    );
  });

  test('a granted dual session hands the records to the presenter, never to the decoders', () async {
    final (session, socket, media) = await _started();
    expect(session.independentOutputs, isTrue);
    expect(session.cursorSeparated, isTrue);
    expect(session.localCursorFallback, isTrue, reason: 'no state yet');
    final decoded = media.packets.length;
    socket.incoming.add(_cursor(shapeRecord(1)));
    socket.incoming.add(_cursor(stateRecord(true, 1)));
    socket.incoming.add(track.packet(1, pts: 2, key: false));
    await flush();
    expect(session.closed, isFalse, reason: session.error);
    expect(media.records.map((r) => r.$1), [0, 0]);
    expect(media.records.first.$2, shapeRecord(1));
    expect(media.records.last.$2, stateRecord(true, 1));
    expect(media.packets, hasLength(decoded + 1), reason: 'video only');
    expect(media.packets.every((p) => p[4] == 1), isTrue);
    expect(session.localCursorFallback, isFalse);
    // Hidden by the remote: the local pointer stays hidden over the picture.
    socket.incoming.add(_cursor(stateRecord(false, 0)));
    await flush();
    expect(session.remoteCursorHidden, isTrue);
    await session.close();
    await media.events.close();
  });

  test(
    'malformed, positioned or unannounced records end a granted dual session',
    () async {
      for (final packet in [
        _cursor(stateRecord(true, 1, position: true, x: 5, y: 5)),
        _cursor(stateRecord(true, 1), connector: 0),
        _cursor(stateRecord(true, 1), epoch: 1),
        _cursor(stateRecord(true, 1), flags: 1),
        _cursor(stateRecord(true, 1), topology: 2),
        _cursor(Uint8List.fromList(shapeRecord(1)..[1] = 9)),
      ]) {
        final (session, socket, media) = await _started();
        socket.incoming.add(packet);
        await flush();
        expect(session.closed, isTrue, reason: '$packet');
        expect(media.records, isEmpty);
        await media.events.close();
      }
    },
  );

  test(
    'without the grant a cursor record stays an invalid output packet',
    () async {
      final (session, socket, media) = await _started(granted: false);
      expect(session.cursorSeparated, isFalse);
      expect(session.localCursorFallback, isFalse);
      socket.incoming.add(_cursor(stateRecord(true, 1)));
      await flush();
      expect(session.closed, isTrue);
      expect(media.records, isEmpty);
      await media.events.close();
    },
  );

  for (final granted in [true, false]) {
    testWidgets(
      'all monitors: the cursor view is the primary output (granted: $granted)',
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
        final socket = SessionSocket();
        final media = _CursorTrackMedia();
        final session = RemoteSession(
          connection: _connection(granted: granted),
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
          if (!granted) {
            expect(media.views, isEmpty);
            return;
          }
          // Output "1" is the primary track (track.info); its surface is the
          // 1280x720 local monitor at 640,0: shown 1:1, never the whole span.
          final surface = t.getRect(find.byKey(const Key('local-output-1')));
          expect(
            media.views.last,
            CursorView(
              left: surface.left,
              top: surface.top,
              width: surface.width,
              height: surface.height,
              sourceWidth: 1280,
              sourceHeight: 720,
            ),
          );
          expect(surface.size, const Size(1280, 720));
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
