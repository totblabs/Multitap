// audio-opus-v1 on the client, Dart's side: the capability gate of the connect
// request (Windows and Android), the grant, stream.info's format, and the
// native engines' start arguments. Decoding itself is tested in
// test/native/opus_audio_test.cpp (Windows) and
// android/test/AndroidOpusAudioCheck.kt (Android). No network, native engine
// or audio device.
import 'dart:async';
import 'dart:convert';
import 'dart:io';

import 'package:flutter/services.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:multitap_client/android/android_app.dart' show androidBrokerApi;
import 'package:multitap_client/android/android_media.dart';
import 'package:multitap_client/data/audio_codec.dart';
import 'package:multitap_client/data/broker_api.dart';
import 'package:multitap_client/data/remote_session.dart';
import 'package:multitap_client/models/stream_options.dart';

import 'power_api_test.dart'
    show ApiClient, ApiHeaders, ApiRequest, ApiResponse;
import 'remote_session_test.dart' show SessionSocket, SessionMedia, flush, info;

// Android's broker client reads each answer's request ID (diagnostics on).
class _Headers extends ApiHeaders {
  @override
  String? value(String name) => null;
}

class _Response extends ApiResponse {
  _Response(super.statusCode, super.value);
  @override
  final HttpHeaders headers = _Headers();
}

class _Client extends ApiClient {
  @override
  Future<HttpClientRequest> openUrl(String method, Uri uri) async {
    final (status, value) = responses.removeAt(0);
    final request = ApiRequest(method, uri, _Response(status, value));
    requests.add(request);
    return request;
  }
}

Map<String, dynamic> _desktop(Map<String, Object> capabilities) => {
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
const _advertised = {'service': true, 'audioCodecProtocol': 'audio-opus-v1'};
const _absent = {'service': true};
Map<String, dynamic> _reply({bool granted = false}) => {
  'sessionId': 'opus-session',
  'webSocketUrl': 'wss://fixture.invalid/api/v1/sessions/opus-session',
  'mode': 'stream',
  'options': const StreamOptions().toJson(),
  if (granted) 'audioCodecProtocol': 'audio-opus-v1',
};

class _OpusMedia extends SessionMedia implements AudioFormatMedia {
  final formats = <bool>[];
  @override
  set audioOpus(bool value) => formats.add(value);
}

void main() {
  TestWidgetsFlutterBinding.ensureInitialized();

  test('Only the exact capability counts', () {
    expect(audioOpusAdvertised(_advertised), isTrue);
    for (final value in <Object?>[
      null,
      'caps',
      _absent,
      const {'audioCodecProtocol': 'audio-opus-v2'},
      const {'audioCodecProtocol': true},
    ]) {
      expect(audioOpusAdvertised(value), isFalse, reason: '$value');
    }
    expect(BrokerDesktop.fromJson(_desktop(_advertised)).audioOpus, isTrue);
    expect(BrokerDesktop.fromJson(_desktop(_absent)).audioOpus, isFalse);
  });

  test('stream.info: PCM as released, Opus only where granted', () {
    Map<String, Object> audio(String format) => {
      'sampleRate': 48000,
      'channels': 2,
      'format': format,
      'available': true,
    };
    expect(streamAudioOpus(audio('s16le'), granted: false), isFalse);
    expect(streamAudioOpus(audio('s16le'), granted: true), isFalse);
    expect(streamAudioOpus(audio('opus'), granted: true), isTrue);
    expect(streamAudioOpus(null, granted: true), isFalse);
    expect(
      () => streamAudioOpus(audio('opus'), granted: false),
      throwsFormatException,
    );
  });

  group('connect request', () {
    Future<(Map<dynamic, dynamic>, BrokerConnection)> connect(
      Map<String, Object> capabilities, {
      bool requestAudioOpus = true,
      bool granted = false,
      bool android = false,
    }) async {
      final client = _Client()
        ..responses.add((
          200,
          {
            'desktops': [_desktop(capabilities)],
          },
        ))
        ..responses.add((200, _reply(granted: granted)));
      final api = android
          ? androidBrokerApi(
              Uri.parse('https://fixture.invalid'),
              diagnosticsVersion: () => null,
              client: client,
            )
          : HttpBrokerApi(
              origin: Uri.parse('https://fixture.invalid'),
              client: client,
              requestAudioOpus: requestAudioOpus,
            );
      await api.desktops('fixture');
      final connection = await api.connect('fixture', 'pve-149');
      api.close();
      return (
        jsonDecode(
          client.requests
              .singleWhere((r) => r.uri.path.endsWith('/connect'))
              .body
              .toString(),
        ) as Map,
        connection,
      );
    }

    test(
      'the capability absent (released broker or Host): the key is absent',
      () async {
        final (body, connection) = await connect(_absent);
        expect(body.containsKey('audioCodecProtocol'), isFalse);
        expect(connection.audioOpus, isFalse);
      },
    );

    test(
      'the capability present on Windows: asked, and the grant is taken',
      () async {
        final (body, connection) = await connect(_advertised, granted: true);
        expect(body['audioCodecProtocol'], 'audio-opus-v1');
        expect(connection.audioOpus, isTrue);
      },
    );

    test('asked but not granted: PCM', () async {
      final (body, connection) = await connect(_advertised);
      expect(body['audioCodecProtocol'], 'audio-opus-v1');
      expect(connection.audioOpus, isFalse);
    });

    test('a client that keeps PCM never asks', () async {
      final (body, connection) = await connect(
        _advertised,
        requestAudioOpus: false,
      );
      expect(body.containsKey('audioCodecProtocol'), isFalse);
      expect(connection.audioOpus, isFalse);
    });

    // Android decodes Opus too (AndroidOpusDecoder.kt), 2026-11-03.
    test('Android asks where offered and takes the grant', () async {
      final (body, connection) = await connect(
        _advertised,
        granted: true,
        android: true,
      );
      expect(body['audioCodecProtocol'], 'audio-opus-v1');
      expect(connection.audioOpus, isTrue);
    });

    test('Android: not offered (old Host or broker), not asked', () async {
      final (body, connection) = await connect(_absent, android: true);
      expect(body.containsKey('audioCodecProtocol'), isFalse);
      expect(connection.audioOpus, isFalse);
    });

    test('Android: asked but not granted keeps PCM', () async {
      final (body, connection) = await connect(_advertised, android: true);
      expect(body['audioCodecProtocol'], 'audio-opus-v1');
      expect(connection.audioOpus, isFalse);
    });

    // audio-path-v1 (2026-11-11): a phone whose Opus decoders all failed stayed
    // silent on every connection. Once the engine says so, PCM is asked for.
    test('Android asks for PCM once no Opus decoder of the device plays', () async {
      addTearDown(() => AndroidRemoteMedia.opusDecodable = true);
      AndroidRemoteMedia.opusDecodable = false;
      final (body, connection) = await connect(_advertised, android: true);
      expect(body.containsKey('audioCodecProtocol'), isFalse);
      expect(connection.audioOpus, isFalse);
      AndroidRemoteMedia.opusDecodable = true;
      final (again, _) = await connect(_advertised, android: true);
      expect(again['audioCodecProtocol'], 'audio-opus-v1');
    });

    test('the engine statistics turn Opus off for the app run', () {
      addTearDown(() => AndroidRemoteMedia.opusDecodable = true);
      AndroidRemoteMedia.observeAudio({'audioEnabled': true, 'audioOpusUnusable': false});
      expect(AndroidRemoteMedia.opusDecodable, isTrue);
      AndroidRemoteMedia.observeAudio({'audioEnabled': true, 'audioOpusUnusable': true});
      expect(AndroidRemoteMedia.opusDecodable, isFalse);
      // A later reading does not turn it back on within the run.
      AndroidRemoteMedia.observeAudio({'audioEnabled': true, 'audioOpusUnusable': false});
      expect(AndroidRemoteMedia.opusDecodable, isFalse);
    });

    test('a grant nobody asked for is an invalid response', () async {
      await expectLater(
        connect(_advertised, requestAudioOpus: false, granted: true),
        throwsA(
          isA<BrokerException>().having(
            (e) => e.code,
            'code',
            'invalid_response',
          ),
        ),
      );
    });
  });

  group('session', () {
    RemoteSession make(
      SessionSocket socket,
      SessionMedia media,
      bool granted,
    ) => RemoteSession(
      connection: BrokerConnection(
        sessionId: 'test-session',
        webSocketUrl: Uri.parse(
          'wss://cloud.example.com/api/v1/sessions/test-session',
        ),
        options: const StreamOptions(),
        audioOpus: granted,
      ),
      token: 'test-session-token',
      media: media,
      dialer: (uri, token) async => socket,
    );
    Map<String, Object> streamInfo(String format) => {
      ...info,
      'audio': {
        'sampleRate': 48000,
        'channels': 2,
        'format': format,
        'available': true,
      },
    };

    test(
      'A granted Opus stream starts the runner for Opus; PCM stays PCM',
      () async {
        for (final (granted, format, opus) in [
          (true, 'opus', true),
          (true, 's16le', false),
          (false, 's16le', false),
        ]) {
          final socket = SessionSocket(), media = _OpusMedia();
          final session = make(socket, media, granted);
          unawaited(session.start());
          await flush();
          socket.incoming.add(jsonEncode(streamInfo(format)));
          await flush();
          expect(media.starts, 1, reason: '$granted $format');
          expect(media.formats, [opus], reason: '$granted $format');
          await session.close();
          await media.events.close();
        }
      },
    );

    test(
      'Opus without the grant ends the session before the runner starts',
      () async {
        final socket = SessionSocket(), media = _OpusMedia();
        final session = make(socket, media, false);
        unawaited(session.start());
        await flush();
        socket.incoming.add(jsonEncode(streamInfo('opus')));
        await flush();
        expect(session.closed, isTrue);
        expect(media.starts, 0);
        expect(media.formats, isEmpty);
        await media.events.close();
      },
    );
  });

  test('The runner gets audioFormat "opus" only for an Opus stream', () async {
    const channel = MethodChannel('multitap/remote');
    final messenger =
        TestDefaultBinaryMessengerBinding.instance.defaultBinaryMessenger;
    final calls = <MethodCall>[];
    messenger.setMockMethodCallHandler(channel, (call) async {
      calls.add(call);
      return call.method == 'start' ? 70 : null;
    });
    addTearDown(() => messenger.setMockMethodCallHandler(channel, null));
    // The Windows runner, and Android's engine (AndroidMedia.kt reads it).
    for (final media in <NativeRemoteMedia>[
      NativeRemoteMedia(),
      AndroidRemoteMedia(),
    ]) {
      final kind = media.runtimeType;
      await media.start(640, 360);
      expect(
        (calls.last.arguments as Map).containsKey('audioFormat'),
        isFalse,
        reason: '$kind: the released PCM arguments',
      );
      media.audioOpus = true;
      await media.start(640, 360);
      expect((calls.last.arguments as Map)['audioFormat'], 'opus');
      await media.restart(800, 450);
      expect((calls.last.arguments as Map)['audioFormat'], 'opus');
      await media.startHolding(800, 450);
      expect((calls.last.arguments as Map)['audioFormat'], 'opus');
      media.audioOpus = false;
      await media.start(640, 360);
      expect(
        (calls.last.arguments as Map).containsKey('audioFormat'),
        isFalse,
        reason: '$kind',
      );
      await media.stop();
      if (media is AndroidRemoteMedia) media.dispose();
    }
  });
}
