import 'dart:async';
import 'dart:convert';
import 'dart:io';

import 'dart:typed_data';

import 'package:flutter_test/flutter_test.dart';
import 'package:multitap_client/data/broker_api.dart';
import 'package:multitap_client/data/remote_session.dart';
import 'package:multitap_client/data/track_stream.dart';
import 'package:multitap_client/data/video_geometry.dart';
import 'package:multitap_client/models/stream_options.dart';

import 'remote_session_test.dart' show SessionSocket, SessionMedia;
import 'power_api_test.dart' show ApiClient;

const options = StreamOptions(
  maxWidth: 1920,
  maxHeight: 1080,
  audio: false,
  monitor: 'all',
);
TrackPlan plan() => TrackPlan(const [
  TrackViewport('0', 640, 360, -640, 0),
  TrackViewport('1', 1280, 720, 0, 0),
]);
Map<String, dynamic> info({bool audio = false}) => {
  'type': 'stream.tracks.info',
  'sessionId': 'track-session',
  'codec': 'h264',
  'topologyEpoch': 1,
  'fpsTarget': 60,
  'bitrateTarget': 12000000,
  'tracks': [
    for (final t in plan().outputs)
      {
        'id': t.id,
        'epoch': 1,
        'width': t.width,
        'height': t.height,
        'sourceWidth': t.width,
        'sourceHeight': t.height,
        'left': t.x + 80,
        'top': t.y + 20,
        'primary': t.id == '1',
        'bitrateTarget': 6000000,
      },
  ],
  'audio': {
    'sampleRate': 48000,
    'channels': 2,
    'format': 's16le',
    'available': audio,
  },
};
Map<String, dynamic> trackStats({
  int first = 1,
  int second = 1,
  bool ready = true,
}) => {
  'mediaProtocol': 'tracks-v1',
  'topologyEpoch': 1,
  'tracksReady': ready,
  'error': 0,
  'decodedFrames': first + second,
  'tracks': [
    for (final t in plan().outputs)
      {
        'id': t.id,
        'epoch': 1,
        'width': t.width,
        'height': t.height,
        'textureId': t.id == '0' ? 90 : 91,
        'decodedFrames': t.id == '0' ? first : second,
      },
  ],
};
Uint8List packet(
  int id, {
  int pts = 1,
  int topology = 1,
  int epoch = 1,
  bool key = true,
  bool audio = false,
}) {
  final payload = audio
      ? [0, 0, 0, 0]
      : key
      ? [0, 0, 0, 1, 0x67, 1, 0, 0, 1, 0x68, 1, 0, 0, 1, 0x65, 1]
      : [0, 0, 1, 0x41, 1];
  final bytes = Uint8List(28 + payload.length)
    ..setRange(0, 4, [84, 86, 84, 49]);
  bytes[4] = audio ? 2 : 1;
  bytes[5] = key && !audio ? 1 : 0;
  bytes[6] = audio ? 255 : id;
  final data = ByteData.sublistView(bytes);
  data.setUint32(8, topology, Endian.little);
  data.setUint32(12, audio ? 0 : epoch, Endian.little);
  data.setUint64(16, pts, Endian.little);
  data.setUint32(24, payload.length, Endian.little);
  return bytes..setRange(28, bytes.length, payload);
}

TrackStream configured({StreamOptions settings = options, bool audio = false}) {
  final stream = TrackStream('track-session', plan(), settings)
    ..configure(info(audio: audio));
  stream.bindTextures({'0': 90, '1': 91}, 0);
  return stream;
}

Map<String, dynamic> response() => {
  'sessionId': 'track-session',
  'webSocketUrl': 'wss://cloud.example.com/api/v1/sessions/track-session',
  'mode': 'stream',
  'transport': 'media-tracks-v1',
  'mediaProtocol': 'tracks-v1',
  'trackViewportProvisioning': true,
  'tracks': plan().toJson(),
  'options': options.toJson(),
};
BrokerConnection connection() => BrokerConnection.fromJson(
  response(),
  Uri.parse('https://cloud.example.com'),
  requestedOptions: options,
  requestedTracks: plan(),
);

class TrackMedia extends SessionMedia implements RemoteTrackMedia {
  Map<String, dynamic>? configuration;
  Map<String, dynamic> currentStats = trackStats(
    first: 0,
    second: 0,
    ready: false,
  );
  Completer<Map<String, int>>? pending;
  @override
  Future<Map<String, int>> startTracks(Map<String, dynamic> value) async {
    configuration = value;
    starts++;
    return pending == null ? {'0': 90, '1': 91} : pending!.future;
  }

  @override
  Future<Map<String, dynamic>> stats() async => currentStats;
}

Future<void> flush() => Future<void>.delayed(const Duration(milliseconds: 10));

void main() {
  test('Explicit HTTP track negotiation sends only the assigned desktop request and never retries as composite', () async {
    final client = ApiClient()..responses.add((200, response()));
    final api = HttpBrokerApi(
      origin: Uri.parse('https://cloud.example.com'),
      client: client,
    );
    expect(
      (await api.connectTracks(
        'qa-only-token',
        'pve-149',
        plan(),
        options,
      )).trackPlan!.outputs.length,
      2,
    );
    final request = client.requests.single;
    expect(request.uri.path, '/api/v1/desktops/pve-149/connect');
    expect(request.followRedirects, false);
    expect(
      request.headers.values[HttpHeaders.authorizationHeader],
      'Bearer qa-only-token',
    );
    final body = jsonDecode(request.body.toString()) as Map;
    expect(body['mediaProtocol'], 'tracks-v1');
    expect(body['resizeProtocol'], isNull);
    expect(body['tracks'], plan().toJson());
    final refused = ApiClient()
      ..responses.add((400, {'error': 'unexpected_field'}));
    final old = HttpBrokerApi(
      origin: Uri.parse('https://cloud.example.com'),
      client: refused,
    );
    await expectLater(
      old.connectTracks('qa-only-token', 'pve-149', plan(), options),
      throwsA(isA<BrokerException>()),
    );
    expect(refused.requests, hasLength(1));
    api.close();
    old.close();
  });
  test('Negotiation rejects silent protocol upgrade, downgrade, foreign origin and altered output request', () {
    expect(connection().trackPlan!.outputs.length, 2);
    final origin = Uri.parse('https://cloud.example.com');
    expect(
      () => BrokerConnection.fromJson(response(), origin),
      throwsA(isA<BrokerException>()),
    );
    for (final change in [
      {'transport': 'media-v1'},
      {'mediaProtocol': 'unknown'},
      {'trackViewportProvisioning': false},
      {'options': const StreamOptions(monitor: 'all', fps: 30).toJson()},
      {
        'tracks': [plan().outputs.first.toJson()],
      },
      {
        'webSocketUrl':
            'wss://foreign.example.com/api/v1/sessions/track-session',
      },
    ]) {
      expect(
        () => BrokerConnection.fromJson(
          {...response(), ...change},
          origin,
          requestedOptions: options,
          requestedTracks: plan(),
        ),
        throwsA(isA<BrokerException>()),
      );
    }
  });
  test('Plans reject overlap and coordinate overflow while preserving explicit legacy aggregate bounds', () {
    expect(plan().outputs.first.x, -640);
    expect(
      () => TrackPlan(const [
        TrackViewport('0', 1920, 1080, 0, 0),
        TrackViewport('1', 1920, 1080, 1000, 0),
      ]),
      throwsFormatException,
    );
    final beyondLegacyAggregate = TrackPlan(const [
      TrackViewport('0', 3840, 2160, -3840, 0),
      TrackViewport('1', 1920, 1080, 0, 0),
    ]);
    expect(beyondLegacyAggregate.outputs.length, 2);
    expect(beyondLegacyAggregate.legacyCompatible, isFalse);
    final beyondLegacyPosition = TrackPlan(const [
      TrackViewport('1', 1920, 1080, 32700, 0),
    ]);
    expect(beyondLegacyPosition.outputs.single.x, 32700);
    expect(
      beyondLegacyPosition.legacyCompatible,
      isFalse,
      reason: 'old v1 placement remains bounded independently',
    );
    for (final x in [0x7ffffffe, -0x80000001]) {
      expect(
        () => TrackPlan([TrackViewport('1', 1920, 1080, x, 0)]),
        throwsFormatException,
        reason: 'actual signed LONG placement overflow',
      );
    }
  });
  test('v2 track geometry accepts large independent outputs without relaxing IDs, parity or integer representation', () {
    for (final (width, height) in [
      (3840, 2160),
      (5120, 2880),
      (2880, 5120),
      (5120, 1440),
    ]) {
      final large = TrackPlan([
        TrackViewport('0', width, height, -width, 0),
        TrackViewport('1', width, height, 0, 0),
      ]);
      expect(large.outputs.map((output) => (output.width, output.height)), [
        (width, height),
        (width, height),
      ]);
      expect(large.legacyCompatible, isFalse);
    }
    for (final outputs in [
      [const TrackViewport('0', 5121, 2880, 0, 0)],
      [const TrackViewport('0', 5120, 0, 0, 0)],
      [const TrackViewport('physical', 5120, 2880, 0, 0)],
      [const TrackViewport('0', maximumGeometrySide + 2, 180, 0, 0)],
      [
        const TrackViewport(
          '0',
          maximumGeometrySide,
          maximumGeometrySide,
          0,
          0,
        ),
      ],
      [
        const TrackViewport('0', 5120, 2880, 0, 0),
        const TrackViewport('0', 5120, 2880, 5120, 0),
      ],
    ]) {
      expect(() => TrackPlan(outputs), throwsFormatException);
    }
  });
  test('Metadata must prove actual viewport and relative layout, with no foreign or duplicate tracks', () {
    for (final field in ['sourceWidth', 'left', 'id', 'epoch']) {
      final bad = info();
      final tracks = bad['tracks'] as List;
      (tracks.first as Map)[field] = field == 'id'
          ? '1'
          : field == 'epoch'
          ? 2
          : 999;
      expect(
        () => TrackStream('track-session', plan(), options).configure(bad),
        throwsFormatException,
      );
    }
    final good = configured();
    expect(good.configure(info()), false);
    final modified = info();
    ((modified['tracks'] as List).first as Map)['primary'] = true;
    expect(() => good.configure(modified), throwsFormatException);
    for (final field in [
      'epoch',
      'width',
      'height',
      'sourceWidth',
      'sourceHeight',
    ]) {
      final bad = info(), t = (info()['tracks'] as List).first as Map;
      ((bad['tracks'] as List).first as Map)[field] = (t[field] as int)
          .toDouble();
      expect(
        () => TrackStream('track-session', plan(), options).configure(bad),
        throwsFormatException,
      );
    }
  });
  test('Received key flags and tracksReady alone cannot acknowledge undecoded outputs', () {
    final stream = configured();
    expect(stream.acknowledge(trackStats(first: 0, second: 0), 1), isNull);
    stream.accept(packet(0), 1);
    stream.accept(packet(1), 1);
    expect(stream.acknowledge(trackStats(second: 0), 2), isNull);
    expect(stream.acknowledge(trackStats(ready: false), 3), isNull);
    expect(stream.acknowledge(trackStats(), 4), {
      'type': 'stream.tracks.ready',
      'topologyEpoch': 1,
      'tracks': [
        {'id': '0', 'epoch': 1},
        {'id': '1', 'epoch': 1},
      ],
    });
    expect(stream.acknowledge(trackStats(), 5), isNull);
  });
  test('Invalid IDR, timestamp overflow and unannounced topology never reach the decoder', () {
    for (final bytes in [
      packet(0)..[32] = 0x41,
      packet(0)..[23] = 128,
      packet(0, topology: 2),
      packet(0, epoch: 2),
      packet(0)..[24] = 0,
    ]) {
      expect(() => configured().accept(bytes, 0), throwsFormatException);
    }
    final stream = configured();
    expect(stream.accept(packet(0, key: false), 0), false);
    expect(stream.accept(packet(0, pts: 7), 0), true);
    expect(() => stream.accept(packet(0, pts: 6), 1), throwsFormatException);
  });
  test('Per-output media and decoder watchdogs cannot be kept alive by the other output', () {
    final stream = configured();
    stream.accept(packet(0), 0);
    stream.accept(packet(1), 0);
    stream.acknowledge(trackStats(second: 0), 0);
    stream.accept(packet(0, pts: 2), 16000000);
    stream.acknowledge(trackStats(first: 100, second: 0), 16000000);
    expect(stream.videoStalled(16000000), true);
    expect(stream.decoderStalled(16000000), true);
  });
  test('Old texture replies and regressed decoder counts fail rather than unlocking input', () {
    final stream = configured();
    final old = trackStats();
    ((old['tracks'] as List).last as Map)['textureId'] = 5;
    expect(() => stream.acknowledge(old, 0), throwsFormatException);
    stream.accept(packet(0), 0);
    stream.accept(packet(1), 0);
    stream.acknowledge(trackStats(first: 3), 1);
    expect(
      () => stream.acknowledge(trackStats(first: 2), 2),
      throwsFormatException,
    );
  });
  test('Shared PCM waits for all actual decoder outputs; pointer and key epochs remain session scoped', () {
    const soundOptions = StreamOptions(audio: true, monitor: 'all');
    final stream = configured(settings: soundOptions, audio: true);
    expect(stream.accept(packet(255, audio: true), 0), false);
    expect(stream.input({'type': 'input', 'event': 'key'}), isNull);
    stream.accept(packet(0), 0);
    stream.accept(packet(1), 0);
    stream.acknowledge(trackStats(), 1);
    expect(stream.accept(packet(255, audio: true), 2), true);
    expect(
      stream.input({
        'type': 'input',
        'event': 'move',
        'track': '0',
        'x': .5,
        'y': .5,
      })?['epoch'],
      1,
    );
    expect(
      stream.input({
        'type': 'input',
        'event': 'key',
        'vk': 65,
      })?['topologyEpoch'],
      1,
    );
    expect(
      stream.input({'type': 'input', 'event': 'move', 'track': 'physical'}),
      isNull,
    );
    expect(
      stream.input({
        'type': 'input',
        'event': 'move',
        'track': '0',
        'epoch': 8,
      }),
      isNull,
    );
    expect(
      stream.input({
        'type': 'input',
        'event': 'key',
        'topologyEpoch': 7,
        'vk': 65,
      }),
      isNull,
    );
  });
  test('Controller starts one multi-output owner, forwards full packets and acknowledges only after both decoders', () async {
    final socket = SessionSocket(), media = TrackMedia();
    final session = RemoteSession(
      connection: connection(),
      token: 'not-a-real-token',
      media: media,
      dialer: (_, _) async => socket,
    );
    final running = session.start();
    await flush();
    socket.incoming.add(jsonEncode(info()));
    await flush();
    expect(media.configuration?['topologyEpoch'], 1);
    expect(media.starts, 1);
    expect(session.trackTextures, {'0': 90, '1': 91});
    expect(session.width, 1280);
    expect(session.textureId, 91);
    await session.capture(true);
    expect(session.captured, false);
    socket.incoming.add(packet(0));
    socket.incoming.add(packet(1));
    await flush();
    expect(
      media.packets.map((p) => p.sublist(0, 4)),
      everyElement([84, 86, 84, 49]),
    );
    media.currentStats = trackStats(second: 0);
    await Future<void>.delayed(const Duration(milliseconds: 1050));
    expect(
      socket.sent.where((e) => e['type'] == 'stream.tracks.ready'),
      isEmpty,
    );
    media.currentStats = trackStats();
    await Future<void>.delayed(const Duration(milliseconds: 1050));
    expect(
      socket.sent.where((e) => e['type'] == 'stream.tracks.ready'),
      hasLength(1),
    );
    await session.capture(true);
    expect(session.captured, true);
    session.input({
      'type': 'input',
      'event': 'move',
      'track': '0',
      'x': .25,
      'y': .75,
    });
    final sent = socket.sent.last;
    expect(sent['track'], '0');
    expect(sent['epoch'], 1);
    expect(sent['topologyEpoch'], 1);
    await session.close();
    await running;
    await media.events.close();
    expect(media.stopped, true);
    expect(socket.wasClosed, true);
  });
  test('Close while native tracks start is pending cannot publish a late texture or ready acknowledgement', () async {
    final socket = SessionSocket(),
        media = TrackMedia()..pending = Completer<Map<String, int>>();
    final session = RemoteSession(
      connection: connection(),
      token: 'test',
      media: media,
      dialer: (_, _) async => socket,
    );
    final running = session.start();
    await flush();
    socket.incoming.add(jsonEncode(info()));
    await flush();
    await session.close();
    media.pending!.complete({'0': 90, '1': 91});
    await running;
    expect(session.textureId, isNull);
    expect(session.captured, false);
    expect(
      socket.sent.where((e) => e['type'] == 'stream.tracks.ready'),
      isEmpty,
    );
    expect(media.stopped, true);
    await media.events.close();
  });
  test('A negotiated independent-output session refuses legacy frames and cleans up its owner', () async {
    final socket = SessionSocket(), media = TrackMedia();
    final session = RemoteSession(
      connection: connection(),
      token: 'test',
      media: media,
      dialer: (_, _) async => socket,
    );
    final running = session.start();
    await flush();
    socket.incoming.add(
      jsonEncode({
        'type': 'stream.info',
        'codec': 'h264',
        'width': 1280,
        'height': 720,
      }),
    );
    await running;
    expect(session.closed, true);
    expect(session.diagnosticCode, 'invalid_stream_message');
    expect(media.starts, 0);
    expect(media.stopped, true);
    await media.events.close();
  });
  final vectors = Platform.environment['MULTITAP_TRACK_VECTORS'];
  test(
    'Actual Microsoft CPU-encoded fixture packets cross the Dart controller gate unchanged',
    () async {
      final directory = Directory(vectors!);
      final manifest = jsonDecode(
        await File('${directory.path}/manifest.json').readAsString(),
      ) as Map;
      expect(manifest['actualMicrosoftCpuEncoder'], true);
      final nativePlan = TrackPlan(const [
        TrackViewport('0', 640, 360, 0, 0),
        TrackViewport('1', 800, 600, 640, 0),
      ]);
      final metadata = info();
      metadata['tracks'] = [
        for (final t in nativePlan.outputs)
          {
            'id': t.id,
            'epoch': 1,
            'width': t.width,
            'height': t.height,
            'sourceWidth': t.width,
            'sourceHeight': t.height,
            'left': t.x,
            'top': t.y,
            'primary': t.id == '0',
            'bitrateTarget': 6000000,
          },
      ];
      final stream = TrackStream('track-session', nativePlan, options)
        ..configure(metadata);
      stream.bindTextures({'0': 90, '1': 91}, 0);
      var received = 0;
      for (final frame in manifest['frames'] as List) {
        if ((frame as Map)['topologyEpoch'] != 1) continue;
        final bytes = await File('${directory.path}/${frame['file']}')
            .readAsBytes();
        final before = Uint8List.fromList(bytes);
        expect(stream.accept(bytes, received++), true);
        expect(bytes, before);
      }
      expect(received, 16);
      expect(stream.ready, false);
    },
    skip: vectors == null
        ? 'Set MULTITAP_TRACK_VECTORS to the retained native CPU codec fixture.'
        : false,
  );
}
