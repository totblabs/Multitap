import 'dart:async';
import 'dart:convert';
import 'dart:typed_data';

import 'package:flutter_test/flutter_test.dart';
import 'package:multitap_client/data/broker_api.dart';
import 'package:multitap_client/data/connect_timing.dart';
import 'package:multitap_client/data/product_log.dart';
import 'package:multitap_client/data/remote_session.dart';
import 'package:multitap_client/data/session_windows.dart';
import 'package:multitap_client/models/session_display_mode.dart';
import 'package:multitap_client/models/stream_options.dart';

import 'remote_session_test.dart' show SessionSocket, SessionMedia;

/// A native engine whose first picture is decoded [decodeDelay] after the
/// key frame was handed over (the real engine starts its decoder on its own
/// thread after start() returned, then decodes asynchronously).
class SlowDecodeMedia extends SessionMedia {
  SlowDecodeMedia(this.decodeDelay);
  final Duration decodeDelay;
  final _sinceFirstPush = Stopwatch();
  @override
  Future<void> push(Uint8List packet) async {
    await super.push(packet);
    if (!_sinceFirstPush.isRunning) _sinceFirstPush.start();
  }

  @override
  Future<Map<String, dynamic>> stats() async => {
    ...await super.stats(),
    'decodedFrames':
        _sinceFirstPush.isRunning && _sinceFirstPush.elapsed >= decodeDelay
        ? 1
        : 0,
  };
}

Future<void> flush() => Future<void>.delayed(const Duration(milliseconds: 5));
Uint8List keyFrame() =>
    Uint8List.fromList([1, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0x65]);

RemoteSession make(SessionSocket socket, SessionMedia media) => RemoteSession(
  connection: BrokerConnection(
    sessionId: 'test-session',
    webSocketUrl: Uri.parse(
      'wss://cloud.example.com/api/v1/sessions/test-session',
    ),
    options: const StreamOptions(),
  ),
  token: 'test-session-token',
  media: media,
  dialer: (uri, token) async {
    await Future<void>.delayed(const Duration(milliseconds: 30));
    return socket;
  },
);

const info = {
  'type': 'stream.info',
  'codec': 'h264',
  'width': 1280,
  'height': 720,
};

void main() {
  test('The first streamReady carries every stage once, with the Host stages '
      'of its stream info', () async {
    final socket = SessionSocket(),
        media = SlowDecodeMedia(const Duration(milliseconds: 60));
    final session = make(socket, media);
    session.connectTiming.before.addAll({
      'windowMs': 310,
      'loadMs': 45,
      'connectMs': 80,
      'notAStage': 1,
      'refreshMs': -1,
    });
    final before = ProductLog.instance.entries.length;
    unawaited(session.start());
    await Future<void>.delayed(const Duration(milliseconds: 60));
    socket.incoming.add(
      jsonEncode({
        ...info,
        'startStages': {
          'admitMs': 3,
          'workerMs': 140,
          'displayMs': 220,
          'mediaMs': 95,
          'unknownMs': 7,
          'frameMs': 'x',
        },
      }),
    );
    await flush();
    await Future<void>.delayed(const Duration(milliseconds: 20));
    socket.incoming.add(keyFrame());
    final pushed = Stopwatch()..start();
    while (!session.frameReady && pushed.elapsed < const Duration(seconds: 2)) {
      await Future<void>.delayed(const Duration(milliseconds: 5));
    }
    final ready = ProductLog.instance.entries
        .skip(before)
        .where((entry) => entry.event == LogEvent.streamReady)
        .toList();
    expect(ready, hasLength(1));
    final values = ready.single.values;
    expect(values['windowMs'], 310);
    expect(values['loadMs'], 45);
    expect(values['connectMs'], 80);
    expect(values.containsKey('notAStage'), isFalse);
    expect(values.containsKey('refreshMs'), isFalse);
    expect(values['hostAdmitMs'], 3);
    expect(values['hostWorkerMs'], 140);
    expect(values['hostDisplayMs'], 220);
    expect(values['hostMediaMs'], 95);
    expect(values.containsKey('unknownMs'), isFalse);
    // Real timers (and a busy machine): only the dial's own wait and the
    // decode's bound a stage from below; every stage is present.
    expect(values['dialMs'] as int, greaterThanOrEqualTo(29));
    expect(values['infoMs'] as int, greaterThanOrEqualTo(0));
    expect(values['decoderMs'] as int, greaterThanOrEqualTo(0));
    expect(values['keyFrameMs'] as int, greaterThanOrEqualTo(0));
    expect(values['firstFrameMs'] as int, greaterThanOrEqualTo(59));
    final stages =
        (values['dialMs'] as int) +
        (values['infoMs'] as int) +
        (values['decoderMs'] as int) +
        (values['keyFrameMs'] as int) +
        (values['firstFrameMs'] as int);
    // Millisecond truncation of five stages.
    expect((values['elapsedMs'] as int) - stages, inInclusiveRange(0, 5));
    // A later ready (a resize) repeats none of them.
    expect(session.connectTiming.ready(0), isEmpty);
    await session.close();
    await media.events.close();
  });

  test('Every stage field is kept by the product log and its diagnostics', () {
    final kept = ProductLogEntry.safeValues({
      for (final field in connectTimingFields) field: 7,
    });
    for (final field in connectTimingFields) {
      expect(kept[field], 7, reason: field);
    }
    for (final field in hostStartStageFields.values) {
      expect(connectTimingFields, contains(field));
    }
  });

  test('The launcher request time crosses to the session window as a number '
      'and a malformed one is left out', () {
    final requested = DateTime.fromMillisecondsSinceEpoch(1793000000000);
    final launch = SessionWindowLaunch(
      origin: Uri.parse('https://vdi.example.com'),
      token: 'token',
      expiresAt: DateTime.utc(2030),
      desktopId: 'pve-170',
      name: 'VM',
      mode: SessionDisplayMode.windowed,
      options: const StreamOptions(),
      requestedAt: requested,
    );
    final map = launch.toMap();
    expect(map['requestedAtMs'], 1793000000000);
    expect(SessionWindowLaunch.fromMap(map).requestedAt, requested);
    expect(
      SessionWindowLaunch.fromMap({...map, 'requestedAtMs': 'soon'}).requestedAt,
      isNull,
    );
    expect(
      SessionWindowLaunch.fromMap({...map}..remove('requestedAtMs')).requestedAt,
      isNull,
    );
  });
}
