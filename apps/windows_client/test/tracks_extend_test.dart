// Independent outputs (tracks-v1, "모든 모니터") with Opus audio
// (audio-opus-tracks-v1) and transparent resume (session-resume-v1 with the
// Host's tracks-resume-v1): the connect request, the grant, the shared audio
// format, the native start arguments, and a resume of the session socket.
// Sockets, media and the broker are fakes; nothing touches a real display,
// audio device or network.
import 'dart:async';
import 'dart:convert';
import 'dart:typed_data';

import 'package:flutter_test/flutter_test.dart';
import 'package:multitap_client/data/audio_codec.dart';
import 'package:multitap_client/data/broker_api.dart';
import 'package:multitap_client/data/remote_session.dart';
import 'package:multitap_client/data/track_stream.dart';
import 'package:multitap_client/models/stream_options.dart';

import 'power_api_test.dart' show ApiClient;
import 'remote_session_test.dart' show SessionSocket;
import 'track_stream_test.dart' as f;

final origin = Uri.parse('https://cloud.example.com');
final tokenA = 'A' * 43, tokenB = 'B' * 43;
const opusGrant = {'audioCodecProtocol': 'audio-opus-v1'};
const resumeGrant = {
  'sessionResumeProtocol': 'session-resume-v1',
  'resumeGraceMs': 15000,
};
const sound = StreamOptions(
  maxWidth: 1920,
  maxHeight: 1080,
  audio: true,
  monitor: 'all',
);

Map<String, dynamic> desktop(Map<String, Object> capabilities) => {
  'id': 'pve-149',
  'name': 'TESTER',
  'kind': 'vm',
  'description': '',
  'os': 'Windows',
  'status': 'ready',
  'canConnect': true,
  'connectionModes': {'stream': true},
  'host': {'connected': true, 'capabilities': capabilities},
};
const tracksOpus = {
  'service': true,
  'audioCodecProtocol': 'audio-opus-v1',
  'audioCodecTracksProtocol': 'audio-opus-tracks-v1',
  'sessionResumeProtocol': 'session-resume-v1',
};

Map<String, dynamic> opusInfo() =>
    f.info(audio: true)
      ..['audio'] = {
        'sampleRate': 48000,
        'channels': 2,
        'format': 'opus',
        'available': true,
      };
// kind 2, connector 255: a u16 sequence + an Opus packet (not 4-byte frames).
Uint8List opusPacket(int pts) {
  final bytes = f.packet(255, audio: true, pts: pts);
  final payload = List<int>.filled(7, 0xfc)..[0] = 1;
  final value = Uint8List(28 + payload.length)..setRange(0, 28, bytes);
  ByteData.sublistView(value).setUint32(24, payload.length, Endian.little);
  return value..setRange(28, value.length, payload);
}

TrackStream ready(TrackStream stream, Map<String, dynamic> metadata) {
  stream.configure(metadata);
  stream.bindTextures({'0': 90, '1': 91}, 0);
  stream.accept(f.packet(0), 0);
  stream.accept(f.packet(1), 0);
  expect(stream.acknowledge(f.trackStats(), 1), isNotNull);
  return stream;
}

void main() {
  group('audio-opus-tracks-v1', () {
    test('Only both Host markers advertise Opus for independent outputs', () {
      expect(audioOpusTracksAdvertised(tracksOpus), isTrue);
      for (final value in <Object?>[
        null,
        {'audioCodecProtocol': 'audio-opus-v1'},
        {'audioCodecTracksProtocol': 'audio-opus-tracks-v1'},
        {...tracksOpus, 'audioCodecTracksProtocol': 'audio-opus-tracks-v2'},
      ]) {
        expect(audioOpusTracksAdvertised(value), isFalse, reason: '$value');
      }
      expect(
        BrokerDesktop.fromJson(desktop(tracksOpus)).audioOpusTracks,
        isTrue,
      );
      expect(
        BrokerDesktop.fromJson(
          desktop({'service': true, 'audioCodecProtocol': 'audio-opus-v1'}),
        ).audioOpusTracks,
        isFalse,
      );
    });

    test('A granted session takes Opus metadata, odd-length audio and tells the runner', () {
      final stream = ready(
        TrackStream('track-session', f.plan(), sound, audioOpus: true),
        opusInfo(),
      );
      expect(stream.opusAudio, isTrue);
      expect(stream.nativeConfiguration['audioFormat'], 'opus');
      expect(stream.accept(opusPacket(2), 2), isTrue);
      // A granted session whose Host could not start its encoder says s16le.
      final pcm = ready(
        TrackStream('track-session', f.plan(), sound, audioOpus: true),
        f.info(audio: true),
      );
      expect(pcm.opusAudio, isFalse);
      expect(pcm.nativeConfiguration.containsKey('audioFormat'), isFalse);
      expect(() => pcm.accept(opusPacket(2), 2), throwsFormatException);
    });

    test('Without the grant Opus metadata is refused and PCM stays whole frames', () {
      expect(
        () => TrackStream('track-session', f.plan(), sound).configure(opusInfo()),
        throwsFormatException,
      );
      final pcm = ready(
        TrackStream('track-session', f.plan(), sound),
        f.info(audio: true),
      );
      expect(pcm.nativeConfiguration.containsKey('audioFormat'), isFalse);
      expect(() => pcm.accept(opusPacket(2), 2), throwsFormatException);
      // A duplicate metadata that changes the format is an unannounced change.
      final opus = ready(
        TrackStream('track-session', f.plan(), sound, audioOpus: true),
        opusInfo(),
      );
      expect(() => opus.configure(f.info(audio: true)), throwsFormatException);
    });
  });

  group('connect request for independent outputs', () {
    Future<(Map<dynamic, dynamic>, BrokerConnection)> connect(
      Map<String, Object> capabilities,
      Map<String, Object> grant, {
      bool requestAudioOpus = true,
      bool requestSessionResume = true,
      bool profile = false,
    }) async {
      final client = ApiClient()
        ..responses.add((
          200,
          {
            'desktops': [desktop(capabilities)],
          },
        ))
        ..responses.add((
          200,
          {
            ...f.response(),
            if (profile) 'streamProfileTracksProtocol': profileTracksProtocol,
            ...grant,
          },
        ));
      final api = HttpBrokerApi(
        origin: origin,
        client: client,
        requestAudioOpus: requestAudioOpus,
        requestSessionResume: requestSessionResume,
      );
      try {
        await api.desktops('qa-only-token');
        final connection = profile
            ? await api.connectProfileTracks(
                'qa-only-token',
                'pve-149',
                f.plan(),
                f.options,
              )
            : await api.connectTracks(
                'qa-only-token',
                'pve-149',
                f.plan(),
                f.options,
              );
        final body = jsonDecode(
          client.requests
              .singleWhere((r) => r.uri.path.endsWith('/connect'))
              .body
              .toString(),
        ) as Map;
        return (body, connection);
      } finally {
        api.close();
      }
    }

    test('Asks for both where the Host shows them and takes the grants', () async {
      for (final profile in [false, true]) {
        final (body, connection) = await connect(tracksOpus, {
          ...opusGrant,
          ...resumeGrant,
        }, profile: profile);
        expect(body['audioCodecProtocol'], 'audio-opus-v1');
        expect(body['sessionResumeProtocol'], 'session-resume-v1');
        expect(connection.audioOpus, isTrue);
        expect(connection.sessionResume, isTrue);
        expect(connection.resumeGraceMs, 15000);
      }
    });

    test('Asked but not granted: PCM and no resume', () async {
      final (body, connection) = await connect(tracksOpus, const {});
      expect(body['audioCodecProtocol'], 'audio-opus-v1');
      expect(connection.audioOpus, isFalse);
      expect(connection.sessionResume, isFalse);
    });

    test('Not asked: one-output Opus alone, no resume marker, or a client that keeps PCM', () async {
      final (opusOnly, _) = await connect({
        'service': true,
        'audioCodecProtocol': 'audio-opus-v1',
      }, const {});
      expect(opusOnly.containsKey('audioCodecProtocol'), isFalse);
      expect(opusOnly.containsKey('sessionResumeProtocol'), isFalse);
      final (pcm, _) = await connect(
        tracksOpus,
        const {},
        requestAudioOpus: false,
        requestSessionResume: false,
      );
      expect(pcm.containsKey('audioCodecProtocol'), isFalse);
      expect(pcm.containsKey('sessionResumeProtocol'), isFalse);
    });

    test('A grant nobody asked for is an invalid response', () async {
      await expectLater(
        connect(tracksOpus, opusGrant, requestAudioOpus: false),
        throwsA(isA<BrokerException>()),
      );
      await expectLater(
        connect(tracksOpus, resumeGrant, requestSessionResume: false),
        throwsA(isA<BrokerException>()),
      );
    });
  });

  group('session-resume-v1 for independent outputs', () {
    test('Resuming restarts every output at its own IDR and is no stall', () {
      final stream = ready(
        TrackStream('track-session', f.plan(), f.options),
        f.info(),
      );
      stream.resumed(14000000);
      expect(stream.videoStalled(16000000), isFalse);
      expect(stream.decoderStalled(16000000), isFalse);
      expect(
        stream.accept(f.packet(0, pts: 5, key: false), 14000001),
        isFalse,
        reason: 'A delta before the restart IDR reached the decoder',
      );
      expect(stream.accept(f.packet(0, pts: 6), 14000002), isTrue);
      expect(stream.accept(f.packet(0, pts: 7, key: false), 14000003), isTrue);
      expect(stream.accept(f.packet(1, pts: 5, key: false), 14000004), isFalse);
      expect(stream.ready, isTrue, reason: 'Resuming is not a new readiness');
    });

    Future<void> streaming(
      SessionSocket socket,
      f.TrackMedia media, {
      String? token,
    }) async {
      await f.flush();
      socket.incoming.add(jsonEncode(f.info()));
      await f.flush();
      socket.incoming.add(f.packet(0));
      socket.incoming.add(f.packet(1));
      media.currentStats = f.trackStats();
      if (token != null) {
        socket.incoming.add(
          jsonEncode({
            'type': 'session.resume.token',
            'sessionId': 'track-session',
            'token': token,
            'graceMs': 15000,
          }),
        );
      }
      await Future<void>.delayed(const Duration(milliseconds: 1100));
    }

    RemoteSession session(
      SessionSocket socket,
      f.TrackMedia media,
      List<String> dials,
      List<SessionSocket> sockets, {
      bool granted = true,
    }) => RemoteSession(
      connection: BrokerConnection.fromJson(
        {...f.response(), if (granted) ...resumeGrant},
        origin,
        requestedOptions: f.options,
        requestedTracks: f.plan(),
        requestedSessionResume: true,
      ),
      token: 'qa-only-token',
      media: media,
      dialer: (_, _) async => socket,
      resumeDialer: (url, token, resumeToken) async {
        dials.add(
          url.path == '/api/v1/sessions/track-session' &&
                  token == 'qa-only-token'
              ? resumeToken
              : 'wrong target',
        );
        final next = SessionSocket();
        sockets.add(next);
        return next;
      },
    );

    test('A lost socket is replaced with the token; every output restarts at its IDR and input goes on', () async {
      final first = SessionSocket(), media = f.TrackMedia();
      final dials = <String>[], sockets = <SessionSocket>[];
      final remote = session(first, media, dials, sockets);
      final running = remote.start();
      await streaming(first, media, token: tokenA);
      expect(
        first.sent.where((e) => e['type'] == 'stream.tracks.ready'),
        hasLength(1),
      );
      unawaited(first.incoming.close()); // dropped without a close frame
      await f.flush();
      expect(dials, [tokenA], reason: '${remote.diagnosticCode} ${remote.error}');
      expect(remote.closed, isFalse, reason: remote.error);
      expect(first.wasClosed, isTrue);
      final second = sockets.single;
      second.incoming.add(
        jsonEncode({
          'type': 'session.resumed',
          'sessionId': 'track-session',
          'token': tokenB,
          'graceMs': 15000,
        }),
      );
      final before = media.packets.length;
      second.incoming.add(f.packet(0, pts: 5, key: false));
      second.incoming.add(f.packet(0, pts: 6));
      second.incoming.add(f.packet(1, pts: 6));
      await f.flush();
      expect(
        media.packets.length,
        before + 2,
        reason: 'Only the restart IDRs reach the decoder',
      );
      await remote.capture(true);
      remote.input({
        'type': 'input',
        'event': 'move',
        'track': '0',
        'x': .25,
        'y': .75,
      });
      expect(second.sent.last['track'], '0');
      expect(remote.resuming, isFalse);
      // The next loss uses the rotated token.
      unawaited(second.incoming.close());
      await f.flush();
      expect(dials, [tokenA, tokenB]);
      await remote.close();
      await running;
      await media.events.close();
    });

    test('Before every output was shown, or without the grant, a lost socket ends the session', () async {
      {
        final socket = SessionSocket(), media = f.TrackMedia();
        final dials = <String>[], sockets = <SessionSocket>[];
        final remote = session(socket, media, dials, sockets);
        final running = remote.start();
        await f.flush();
        socket.incoming.add(jsonEncode(f.info()));
        socket.incoming.add(
          jsonEncode({
            'type': 'session.resume.token',
            'sessionId': 'track-session',
            'token': tokenA,
            'graceMs': 15000,
          }),
        );
        await f.flush();
        unawaited(socket.incoming.close());
        await running;
        expect(dials, isEmpty);
        expect(remote.closed, isTrue);
        await media.events.close();
      }
      {
        final socket = SessionSocket(), media = f.TrackMedia();
        final dials = <String>[], sockets = <SessionSocket>[];
        final remote = session(socket, media, dials, sockets, granted: false);
        final running = remote.start();
        await streaming(socket, media, token: tokenA);
        unawaited(socket.incoming.close());
        await running;
        expect(dials, isEmpty);
        expect(remote.closed, isTrue);
        await media.events.close();
      }
    });

    test('While the Host or its track tunnel is away nothing times out; the outputs restart after', () async {
      final socket = SessionSocket(), media = f.TrackMedia();
      final dials = <String>[], sockets = <SessionSocket>[];
      final remote = session(socket, media, dials, sockets);
      final running = remote.start();
      await streaming(socket, media, token: tokenA);
      socket.incoming.add(
        jsonEncode({'type': 'session.resuming', 'sessionId': 'track-session'}),
      );
      await f.flush();
      expect(remote.resuming, isTrue);
      socket.incoming.add(
        jsonEncode({'type': 'session.resumed', 'sessionId': 'track-session'}),
      );
      await f.flush();
      expect(remote.resuming, isFalse);
      final before = media.packets.length;
      socket.incoming.add(f.packet(1, pts: 9, key: false));
      socket.incoming.add(f.packet(1, pts: 10));
      await f.flush();
      expect(media.packets.length, before + 1);
      expect(dials, isEmpty, reason: 'An open socket was replaced');
      await remote.close();
      await running;
      await media.events.close();
    });
  });
}
