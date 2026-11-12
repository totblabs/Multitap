import 'dart:async';
import 'dart:convert';
import 'dart:io';

import 'package:flutter_test/flutter_test.dart';
import 'package:multitap_client/data/audio_stats.dart';
import 'package:multitap_client/data/broker_api.dart';
import 'package:multitap_client/data/product_log.dart';
import 'package:multitap_client/data/remote_session.dart';
import 'package:multitap_client/data/stream_stats_report.dart';
import 'package:multitap_client/models/stream_options.dart';

import 'audio_stats_test.dart' show AudioMedia, audio, native;
import 'remote_session_test.dart' show SessionSocket, info, packet;
import 'session_recovery_test.dart' show ManualClock;

// 2026-11-03: Opus made Android connects slower, and no row could say which
// sessions had Opus, how much it concealed, whether the player had failed or
// whether a Windows decoder was prestarted. Fixed numbers only.
void main() {
  test('Opus concealment is a per-reading change, also across Android sessions; '
      'an engine that does not count it reports nothing', () {
    final shared = <String, num>{};
    var tracker = AudioStatsTracker(baselines: shared);
    expect(tracker.read({...native(), 'audioConcealed': 12})!.concealed, 12);
    expect(tracker.read({...native(), 'audioConcealed': 15})!.concealed, 3);
    // The next session of the same app engine (AndroidMedia counts for the app).
    tracker = AudioStatsTracker(baselines: shared);
    expect(tracker.read({...native(), 'audioConcealed': 15})!.concealed, 0);
    // A PCM stream (Android answers null; Windows leaves the key out).
    expect(tracker.read({...native(), 'audioConcealed': null})!.concealed, isNull);
    expect(tracker.read(native())!.concealed, isNull);
    expect(tracker.read({...native(), 'audioConcealed': 18})!.concealed, 3);

    StreamStatsSample tick(AudioStatsReading audio) =>
        StreamStatsSample(width: 1920, height: 1080, audio: audio);
    final window = StreamStatsWindow();
    window.add(tick(const AudioStatsReading(concealed: 4)), 0);
    window.add(tick(const AudioStatsReading()), 1000000);
    window.add(tick(const AudioStatsReading(concealed: 2)), 2000000);
    expect(window.take(3000000)!['audioConcealed'], 6);
    window.add(tick(const AudioStatsReading()), 4000000);
    expect(
      window.take(5000000)!.containsKey('audioConcealed'),
      isFalse,
      reason: 'PCM: no concealment field at all, not a zero',
    );
    expect(audioStatsFields, contains('audioConcealed'));
  });

  test('Every new field is kept by the product log, the upload and the broker', () {
    final kept = ProductLogEntry.safeValues({
      'audioFormat': 2,
      'decoderPrestarted': true,
    });
    expect(kept, {'audioFormat': 2, 'decoderPrestarted': true});
    for (final field in ['audioFormat', 'audioError', 'audioConcealed']) {
      expect(streamStatsFields, contains(field));
    }
    final server = File(
      '../../server/src/client-diagnostics.mjs',
    ).readAsStringSync();
    expect(server, contains("'audioConcealed', 'audioFormat', 'audioError',"));
    final boolean = RegExp(r'const boolean = new Set\(\[([^\]]*)\]\)')
        .firstMatch(server)![1]!;
    expect(boolean, contains("'decoderPrestarted'"));
  });

  test('audio-path-v1: each value within its bound, nothing for an engine '
      'without it, every field kept by the broker', () {
    expect(audioPathValues(const {'audioEnabled': true, 'audioBufferMs': 120.0}), isEmpty,
        reason: 'Windows reports no path: not even its buffer');
    final values = audioPathValues({
      ..._androidPath,
      'audioFocus': 9,
      'audioOutRate': -1,
      'audioStartMs': double.nan,
      'audioMuted': 'yes',
    });
    expect(values.containsKey('audioFocus'), isFalse);
    expect(values.containsKey('audioOutRate'), isFalse);
    expect(values.containsKey('audioStartMs'), isFalse);
    expect(values.containsKey('audioMuted'), isFalse);
    expect(values['audioDecoder'], 1);
    expect(values['audioBufferFollows'], false);
    expect(values['audioTrackUnderruns'], 3);
    expect(audioPathFields.every(streamStatsFields.contains), isTrue);
    expect(values.keys.every(audioPathFields.contains), isTrue);
    final server = File(
      '../../server/src/client-diagnostics.mjs',
    ).readAsStringSync();
    final boolean = RegExp(r'const boolean = new Set\(\[([^\]]*)\]\)')
        .firstMatch(server)![1]!;
    for (final flag in [
      'audioOutFloat',
      'audioBufferFollows',
      'audioFastTrack',
      'audioMuted',
      'audioOpusUnusable',
    ]) {
      expect(boolean, contains("'$flag'"));
    }
  });

  for (final opus in [false, true]) {
    testWidgets(
      '${opus ? 'An Opus' : 'A PCM'} session reports its audio format, the player '
      'error and ${opus ? 'its concealment' : 'no concealment'}; its first '
      'streamReady carries the format and the decoder prestart',
      (t) async {
        final socket = SessionSocket(), media = _Media(opus: opus);
        final clock = ManualClock();
        final reports = <Map<String, Object>>[];
        final before = ProductLog.instance.entries.length;
        final session = RemoteSession(
          connection: BrokerConnection(
            sessionId: 'qa-session',
            webSocketUrl: Uri.parse(
              'wss://cloud.example.com/api/v1/sessions/qa-session',
            ),
            options: const StreamOptions(
              maxWidth: 1280,
              maxHeight: 720,
              fps: 30,
              bitrate: 8000000,
              audio: true,
            ),
            audioOpus: opus,
          ),
          token: 'fixture-only-token',
          media: media,
          clock: clock,
          dialer: (_, _) async => socket,
          statsSink: reports.add,
        );
        unawaited(session.start());
        await t.pump();
        socket.incoming.add(
          jsonEncode({
            ...info,
            'audio': {
              'sampleRate': 48000,
              'channels': 2,
              'format': opus ? 'opus' : 's16le',
              'available': true,
            },
          }),
        );
        await t.pump();
        var answered = 0, pts = 0;
        for (var second = 1; second <= 12; second++) {
          clock.now = second * 1000000;
          for (; answered < socket.sent.length; answered++) {
            final sent = socket.sent[answered];
            if (sent['type'] == 'ping') {
              socket.incoming.add(
                jsonEncode({'type': 'pong', 'id': sent['id']}),
              );
            }
          }
          socket.incoming.add(packet());
          socket.incoming.add(audio(pts += 10000));
          media.concealed = second * 2;
          media.audioError = second >= 6 ? -1 : 0;
          await t.pump(const Duration(seconds: 1));
        }
        expect(session.frameReady, isTrue, reason: session.error);
        expect(reports, isNotEmpty);
        final report = reports.first;
        expect(report['audioFormat'], opus ? 2 : 1);
        expect(
          report['audioError'],
          0xffffffff,
          reason: "Android's -1, as an unsigned 32-bit status",
        );
        if (opus) {
          expect(report['audioConcealed'] as int, greaterThan(0));
          // audio-path-v1: the latest reading, numbers rounded, flags kept.
          expect(report['audioDecoder'], 1);
          expect(report['audioOutRate'], 48000);
          expect(report['audioStartMs'], 30);
          expect(report['audioBufferMs'], 200);
          expect(report['audioFastTrack'], true);
          expect(report['audioMuted'], false);
          expect(report['audioFocus'], 1);
          expect(report['androidApi'], 34);
        } else {
          expect(report.containsKey('audioConcealed'), isFalse);
          expect(report.containsKey('audioDecoder'), isFalse);
        }
        expect(report.keys.every(streamStatsFields.contains), isTrue);
        final ready = ProductLog.instance.entries
            .skip(before)
            .where((entry) => entry.event == LogEvent.streamReady)
            .toList();
        expect(ready, hasLength(1));
        expect(ready.single.values['audioFormat'], opus ? 2 : 1);
        expect(ready.single.values['decoderPrestarted'], true);
        await session.close();
        await media.events.close();
      },
    );
  }

  testWidgets('Sound off: no audio format or player error', (t) async {
    final socket = SessionSocket(), media = _Media(opus: false);
    final clock = ManualClock();
    final reports = <Map<String, Object>>[];
    final before = ProductLog.instance.entries.length;
    final session = RemoteSession(
      connection: BrokerConnection(
        sessionId: 'qa-session',
        webSocketUrl: Uri.parse(
          'wss://cloud.example.com/api/v1/sessions/qa-session',
        ),
        options: const StreamOptions(audio: false),
      ),
      token: 'fixture-only-token',
      media: media,
      clock: clock,
      dialer: (_, _) async => socket,
      statsSink: reports.add,
    );
    unawaited(session.start());
    await t.pump();
    socket.incoming.add(jsonEncode(info));
    await t.pump();
    var answered = 0;
    for (var second = 1; second <= 12; second++) {
      clock.now = second * 1000000;
      for (; answered < socket.sent.length; answered++) {
        final sent = socket.sent[answered];
        if (sent['type'] == 'ping') {
          socket.incoming.add(jsonEncode({'type': 'pong', 'id': sent['id']}));
        }
      }
      socket.incoming.add(packet());
      await t.pump(const Duration(seconds: 1));
    }
    expect(reports, isNotEmpty);
    expect(reports.first.containsKey('audioFormat'), isFalse);
    expect(reports.first.containsKey('audioError'), isFalse);
    final ready = ProductLog.instance.entries
        .skip(before)
        .where((entry) => entry.event == LogEvent.streamReady)
        .single;
    expect(ready.values.containsKey('audioFormat'), isFalse);
    await session.close();
    await media.events.close();
  });
}

/// An engine that reports a prestarted decoder, the player's error and, for
/// an Opus stream, its concealment (as Android: null for PCM).
class _Media extends AudioMedia {
  _Media({required this.opus});
  final bool opus;
  int concealed = 0, audioError = 0;
  @override
  Future<Map<String, dynamic>> stats() async => {
    ...await super.stats(),
    'audioError': audioError,
    'decoderPrestarted': true,
    'audioConcealed': opus ? concealed : null,
    // audio-path-v1 as the Android engine reports it.
    if (opus) ..._androidPath,
  };
}

const _androidPath = <String, Object?>{
  'audioDecoder': 1,
  'audioOutRate': 48000,
  'audioOutChannels': 2,
  'audioOutFloat': false,
  'audioStartMs': 30.0,
  'audioBufferMs': 200.0,
  'audioBufferFollows': false,
  'audioFastTrack': true,
  'audioTrackUnderruns': 3,
  'audioTrackRestarts': 0,
  'audioTrackStuck': 0,
  'audioFocus': 1,
  'audioMuted': false,
  'audioOpusUnusable': false,
  'androidApi': 34,
};
