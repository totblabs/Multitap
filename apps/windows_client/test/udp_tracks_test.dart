import 'dart:convert';
import 'dart:typed_data';

import 'package:flutter_test/flutter_test.dart';
import 'package:multitap_client/data/broker_api.dart';
import 'package:multitap_client/data/remote_session.dart';
import 'package:multitap_client/data/udp_probe.dart';

import 'profile_track_session_test.dart' show LiveSocket;
import 'track_stream_test.dart' as f;

class Leg implements UdpLeg {
  int starts = 0, stops = 0, hostKeys = 0;
  final inputs = <(int, int, Map)>[];
  UdpLegReading reading = UdpLegReading(publicKey: Uint8List(32));
  @override
  Future<void> start(UdpGrant grant, String sessionId, int clockUs) async {
    starts++;
  }

  @override
  Future<void> stream(int epoch, int generation) async {}
  @override
  Future<UdpLegReading?> poll() async {
    final result = reading;
    reading = UdpLegReading(report: result.report);
    return result;
  }

  @override
  Future<void> hostKey(Uint8List key, {required bool keyed}) async {
    expect(keyed, true);
    hostKeys++;
  }

  @override
  Future<void> input(int sequence, int epoch, Uint8List bytes) async {
    inputs.add((sequence, epoch, jsonDecode(utf8.decode(bytes)) as Map));
  }

  @override
  Future<void> peer(List<(String, int)> candidates) async {}
  @override
  Future<void> stop() async {
    stops++;
  }
}

Map<String, dynamic> grant(int generation) => {
  'type': 'udp.tracks.grant',
  'protocol': udpTracksProtocol,
  'sessionId': 'track-session',
  'topologyEpoch': 1,
  'generation': generation,
  'tracks': [
    for (final id in ['0', '1'])
      {
        'track': id,
        'epoch': 1,
        'primary': id == '1',
        'bitrateTarget': 6000000,
        'grant': {
          'protocol': udpMediaProtocol,
          'id': base64.encode(List.filled(8, 1)),
          'ticket': base64.encode(List.filled(32, 2)),
          'expiresAt': 1,
          'keys': true,
          'directProtocol': udpDirectProtocol,
          'relays': [
            {'host': '192.0.2.1', 'port': 18481},
          ],
        },
      },
  ],
};

void main() {
  test('each real output owns its leg, keys/generation and traffic; input sequence survives regrant', () async {
    final socket = LiveSocket(), media = f.TrackMedia(), legs = <Leg>[];
    final connection = BrokerConnection.fromJson(
      {...f.response(), 'udpTracksProtocol': udpTracksProtocol},
      Uri.parse('https://cloud.example.com'),
      requestedOptions: f.options,
      requestedTracks: f.plan(),
      requestedUdpMedia: true,
      requestedUdpTracks: true,
    );
    final session = RemoteSession(
      connection: connection,
      token: 'qa',
      media: media,
      dialer: (_, _) async => socket,
      udpLegFactory: () {
        final leg = Leg();
        legs.add(leg);
        return leg;
      },
    );
    final running = session.start();
    Future<void> control(Map<String, dynamic> event) async {
      socket.incoming.add(jsonEncode(event));
      await f.flush();
    }

    await f.flush();
    await control(grant(1));
    expect(legs, isEmpty, reason: 'keys cannot advertise a screen as ready');
    await control(f.info());
    socket.incoming.add(f.packet(0));
    socket.incoming.add(f.packet(1));
    media.currentStats = f.trackStats();
    await Future<void>.delayed(const Duration(milliseconds: 1100));
    expect(session.frameReady, true);
    await control(grant(1));
    await Future<void>.delayed(const Duration(milliseconds: 130));
    expect(legs.length, 2);
    expect(legs.map((leg) => leg.starts), [1, 1]);
    final keys = socket.sent
        .where((event) => event['type'] == 'udp.tracks.key')
        .toList();
    expect(keys.map((event) => event['track']), ['0', '1']);
    for (final id in ['0', '1']) {
      await control({
        'type': 'udp.tracks.key',
        'protocol': udpMediaProtocol,
        'sessionId': 'track-session',
        'track': id,
        'epoch': 1,
        'topologyEpoch': 1,
        'generation': 1,
        'publicKey': base64.encode(List.filled(32, 3)),
      });
    }
    expect(legs.map((leg) => leg.hostKeys), [1, 1]);
    await session.capture(true);
    session.input({
      'type': 'input',
      'event': 'move',
      'track': '0',
      'epoch': 1,
      'x': .5,
      'y': .5,
    });
    session.input({'type': 'input', 'event': 'key', 'vk': 65, 'down': true});
    await f.flush();
    expect(legs[0].inputs.last.$3['track'], '0');
    expect(legs[1].inputs.last.$3['event'], 'key');
    final oldSequence = legs[1].inputs.last.$1;
    expect(oldSequence, greaterThan(legs[0].inputs.last.$1));
    legs[0].reading = const UdpLegReading(
      report: {'udpActivePath': 1, 'udpDirectPath': 2},
      arrivals: [(10, 10, 120)],
    );
    legs[1].reading = const UdpLegReading(report: {'udpActivePath': 0});
    await Future<void>.delayed(const Duration(milliseconds: 130));
    expect(session.videoPathLabel, '1:P2P / 2:TCP');
    final feedback = socket.sent.lastWhere(
      (event) => event['type'] == 'udp.tracks.feedback',
    );
    expect((feedback['tracks'] as List).single, {
      'track': '0',
      'epoch': 1,
      'pts': 10,
      'frames': 1,
      'bytes': 120,
    });
    await control(grant(2));
    await Future<void>.delayed(const Duration(milliseconds: 130));
    expect(legs.length, 4);
    expect(legs.take(2).map((leg) => leg.stops), [1, 1]);
    await control({
      'type': 'udp.tracks.key',
      'protocol': udpMediaProtocol,
      'sessionId': 'track-session',
      'track': '1',
      'epoch': 1,
      'topologyEpoch': 1,
      'generation': 1,
      'publicKey': base64.encode(List.filled(32, 3)),
    });
    expect(legs[3].hostKeys, 0, reason: 'old generation cannot key successor');
    session.input({'type': 'input', 'event': 'key', 'vk': 65, 'down': false});
    await f.flush();
    expect(legs[3].inputs.last.$1, greaterThan(oldSequence));
    await control(grant(1));
    expect(legs.length, 4);
    await session.close();
    await running;
    await media.events.close();
    expect(legs.skip(2).map((leg) => leg.stops), [1, 1]);
  });

  test('a track UDP grant is never accepted unsolicited or with an ordinary response', () {
    expect(
      udpTracksAdvertised({'udpTracksHostProtocol': udpTracksProtocol}),
      false,
    );
    expect(
      () => BrokerConnection.fromJson(
        {...f.response(), 'udpTracksProtocol': udpTracksProtocol},
        Uri.parse('https://cloud.example.com'),
        requestedOptions: f.options,
        requestedTracks: f.plan(),
      ),
      throwsA(isA<BrokerException>()),
    );
    final scope = const UdpTrackScope('0', 7, 9, 2, true);
    expect(scope.matches({...scope.fields, 'generation': 1}), false);
    expect(scope.matches(scope.fields), true);
  });
}
