import 'dart:async';
import 'dart:convert';
import 'dart:io';
import 'dart:typed_data';

import 'package:flutter_test/flutter_test.dart';
import 'package:multitap_client/data/broker_api.dart';
import 'package:multitap_client/data/remote_session.dart';
import 'package:multitap_client/data/track_stream.dart';

import 'power_api_test.dart' show ApiClient;
import 'profile_track_resize_test.dart' as r;
import 'remote_session_test.dart' show SessionSocket;
import 'track_stream_test.dart' as f;

BrokerConnection connection() => BrokerConnection.fromJson(
  {...f.response(), 'streamProfileTracksProtocol': profileTracksProtocol},
  Uri.parse('https://cloud.example.com'),
  requestedOptions: f.options,
  requestedTracks: f.plan(),
  requestedProfileTracks: true,
);

class Clock extends Stopwatch {
  int now = 0;
  @override
  int get elapsedMicroseconds => now;
}

class LiveSocket extends SessionSocket {
  @override
  void add(dynamic data) {
    super.add(data);
    final value = sent.last;
    if (value['type'] == 'ping') {
      incoming.add(jsonEncode({'type': 'pong', 'id': value['id']}));
    }
  }
}

class Media extends f.TrackMedia {
  bool decoded = true;
  @override
  Future<Map<String, dynamic>> stats() async => {
    'mediaProtocol': 'tracks-v1',
    'topologyEpoch': configuration!['topologyEpoch'],
    'tracksReady': decoded,
    'decodedFrames': decoded ? 2 : 0,
    'error': 0,
    'tracks': [
      for (final t in configuration!['tracks'] as List)
        {
          ...(t as Map),
          'textureId': t['id'] == '0' ? 90 : 91,
          'decodedFrames': decoded ? 1 : 0,
        },
    ],
  };
}

class Fixture {
  final socket = LiveSocket(), media = Media(), clock = Clock();
  late final session = RemoteSession(
    connection: connection(),
    token: 'qa-only-token',
    media: media,
    clock: clock,
    dialer: (_, _) async => socket,
  );
  late Future<void> running;
  Future<void> start() async {
    running = session.start();
    await f.flush();
    send(r.metadata(f.plan()));
    await f.flush();
    socket.incoming.add(f.packet(0));
    socket.incoming.add(f.packet(1));
    await Future<void>.delayed(const Duration(milliseconds: 1100));
    expect(session.frameReady, true);
  }

  void send(Map<String, dynamic> value) =>
      socket.incoming.add(jsonEncode(value));
  Map<String, dynamic> get command =>
      socket.sent.lastWhere((e) => e['type'] == 'stream.tracks.resize');
  Map<String, dynamic> resizeAck() => {
    ...r.resizing(r.ready()),
    'requestId': command['requestId'],
  };
  Map<String, dynamic> info({bool restored = false}) => {
    ...r.metadata(
      restored ? f.plan() : r.nextPlan(),
      topology: 2,
      epochs: restored ? null : {'0': 2, '1': 1},
      correlated: true,
    ),
    'requestId': command['requestId'],
  };
  Future<void> close() async {
    await session.close();
    await running;
    await media.events.close();
  }
}

void main() {
  test('HTTP group negotiation is explicit and rejects downgrade, unsolicited upgrade and incomplete capability tuples', () async {
    final client = ApiClient()
      ..responses.add((
        200,
        {...f.response(), 'streamProfileTracksProtocol': profileTracksProtocol},
      ));
    final api = HttpBrokerApi(
      origin: Uri.parse('https://cloud.example.com'),
      client: client,
    );
    final c = await api.connectProfileTracks(
      'qa-only-token',
      'pve-149',
      f.plan(),
      f.options,
    );
    expect(c.profileTracks, true);
    final body = jsonDecode(client.requests.single.body.toString()) as Map;
    expect(body['streamProfileTracksProtocol'], profileTracksProtocol);
    expect(body['tracks'], f.plan().toJson());
    expect(client.requests.single.followRedirects, false);
    expect(
      client.requests.single.headers.values[HttpHeaders.authorizationHeader],
      'Bearer qa-only-token',
    );
    expect(
      () => BrokerConnection.fromJson(
        f.response(),
        Uri.parse('https://cloud.example.com'),
        requestedOptions: f.options,
        requestedTracks: f.plan(),
        requestedProfileTracks: true,
      ),
      throwsA(isA<BrokerException>()),
    );
    expect(
      () => BrokerConnection.fromJson(
        {...f.response(), 'streamProfileTracksProtocol': profileTracksProtocol},
        Uri.parse('https://cloud.example.com'),
        requestedOptions: f.options,
        requestedTracks: f.plan(),
      ),
      throwsA(isA<BrokerException>()),
    );
    final caps = <String, Object>{
      'streamProfileTracksProtocol': profileTracksProtocol,
      'streamProfileProtocol': 'console-profile-v1',
      'videoTrackProtocol': 'tracks-v1',
      'resizeProtocol': 'epoch-v1',
      for (final key in [
        'trackViewportProvisioning',
        'viewportProvisioning',
        'dynamicResolution',
        'service',
        'interactiveSession',
        'streaming',
        'virtualDisplayReady',
      ])
        key: true,
      'monitors': [
        {'id': '0', 'width': 1280, 'height': 720},
      ],
    };
    expect(profileTracksAdvertised(caps), true);
    for (final key in caps.keys) {
      expect(
        profileTracksAdvertised({...caps}..remove(key)),
        false,
        reason: key,
      );
    }
    expect(profileTracksAdvertised({...caps, 'service': 1}), false);
    expect(
      profileTracksAdvertised({
        ...caps,
        'monitors': [
          {'id': '1', 'width': 1280, 'height': 720},
        ],
      }),
      false,
    );
    api.close();
  });
  test('Controller releases captured input before request, preserves old source until metadata, and completes only with both fresh decoded outputs', () async {
    final f = Fixture();
    await f.start();
    await f.session.capture(true);
    expect(f.session.captured, true);
    final changed = f.session.resizeTracks(r.nextPlan());
    await Future<void>.delayed(const Duration(milliseconds: 15));
    expect(f.session.captured, false);
    expect(f.session.resizing, true);
    expect(f.media.starts, 1);
    expect(f.session.frameReady, false);
    f.send(f.resizeAck());
    f.media.decoded = false;
    f.send(f.info());
    await Future<void>.delayed(const Duration(milliseconds: 15));
    expect(f.media.starts, 2);
    expect(f.session.width, 800);
    expect(f.session.appliedFps, 60);
    f.socket.incoming.add(fixturePacket(0, epoch: 2));
    f.socket.incoming.add(fixturePacket(1));
    await Future<void>.delayed(const Duration(milliseconds: 1100));
    expect(f.session.resizing, true);
    expect(f.session.frameReady, false);
    f.media.decoded = true;
    expect(await changed.timeout(const Duration(seconds: 3)), true);
    expect(f.session.frameReady, true);
    expect(f.session.resizing, false);
    expect(
      f.socket.sent
          .where((e) => e['type'] == 'stream.tracks.ready')
          .map((e) => e['topologyEpoch']),
      [1, 2],
    );
    await f.close();
  });
  test('Verified old-size restore resumes a new topology and reports request failure without ending the session', () async {
    final f = Fixture();
    await f.start();
    final changed = f.session.resizeTracks(r.nextPlan());
    await Future<void>.delayed(const Duration(milliseconds: 15));
    f.send(f.resizeAck());
    f.send({...r.restored(), 'requestId': f.command['requestId']});
    f.send(f.info(restored: true));
    await Future<void>.delayed(const Duration(milliseconds: 15));
    f.socket.incoming.add(fixturePacket(0));
    f.socket.incoming.add(fixturePacket(1));
    expect(await changed.timeout(const Duration(seconds: 3)), false);
    expect(f.session.closed, false);
    expect(f.session.frameReady, true);
    expect(f.session.resizeError, contains('복구'));
    expect(f.session.width, 640);
    expect(f.media.configuration!['topologyEpoch'], 2);
    await f.close();
  });
  test('Undispatched rejection keeps old decoders and close resolves an outstanding request without late publication', () async {
    final f = Fixture();
    await f.start();
    final changed = f.session.resizeTracks(r.nextPlan());
    await Future<void>.delayed(const Duration(milliseconds: 15));
    f.send({
      'type': 'stream.tracks.resize.failed',
      'requestId': f.command['requestId'],
      'reason': 'track_streaming_unavailable',
      'unchanged': true,
    });
    expect(await changed.timeout(const Duration(seconds: 2)), false);
    expect(f.media.starts, 1);
    expect(f.session.frameReady, true);
    final next = f.session.resizeTracks(r.nextPlan());
    await Future<void>.delayed(const Duration(milliseconds: 15));
    await f.close();
    expect(await next, false);
  });
  test('Hardware preparation has a bounded 90-second budget, independent from old output watchdogs', () async {
    final f = Fixture();
    await f.start();
    final changed = f.session.resizeTracks(r.nextPlan());
    await Future<void>.delayed(const Duration(milliseconds: 15));
    f.send(f.resizeAck());
    f.clock.now = 20000000;
    await Future<void>.delayed(const Duration(milliseconds: 1100));
    expect(f.session.closed, false);
    expect(f.session.resizing, true);
    f.clock.now = 91000000;
    expect(await changed.timeout(const Duration(seconds: 3)), false);
    expect(f.session.closed, true);
    expect(f.session.diagnosticCode, 'track_resize_timeout');
    await f.close();
  });
  test('Late metadata cannot renew the expired hardware deadline before the next watchdog tick', () async {
    final f = Fixture();
    await f.start();
    final changed = f.session.resizeTracks(r.nextPlan());
    await Future<void>.delayed(const Duration(milliseconds: 15));
    f.send(f.resizeAck());
    await Future<void>.delayed(const Duration(milliseconds: 15));
    f.clock.now = 91000000;
    f.send(f.info());
    expect(await changed.timeout(const Duration(seconds: 2)), false);
    expect(f.session.diagnosticCode, 'track_resize_timeout');
    expect(f.media.starts, 1);
    await f.close();
  });
  test('Closing while replacement decoders start cannot publish a late texture or ready acknowledgement', () async {
    final f = Fixture();
    await f.start();
    final changed = f.session.resizeTracks(r.nextPlan());
    await Future<void>.delayed(const Duration(milliseconds: 15));
    f.media.pending = Completer<Map<String, int>>();
    f.send(f.resizeAck());
    f.send(f.info());
    await Future<void>.delayed(const Duration(milliseconds: 15));
    expect(f.media.starts, 2);
    await f.session.close();
    expect(await changed, false);
    f.media.pending!.complete({'0': 92, '1': 93});
    await f.close();
    expect(
      f.socket.sent
          .where((e) => e['type'] == 'stream.tracks.ready')
          .map((e) => e['topologyEpoch']),
      [1],
    );
    expect(f.session.trackTextures, isEmpty);
  });
}

// Synthetic Annex B markers exercise transport gates, not a real H.264 decoder.
Uint8List fixturePacket(int id, {int epoch = 1}) =>
    f.packet(id, topology: 2, epoch: epoch, pts: 0);
