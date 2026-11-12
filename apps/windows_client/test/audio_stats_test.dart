import 'dart:async';
import 'dart:convert';
import 'dart:typed_data';

import 'package:flutter/services.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:multitap_client/data/audio_stats.dart';
import 'package:multitap_client/data/broker_api.dart';
import 'package:multitap_client/data/remote_session.dart';
import 'package:multitap_client/data/stream_stats_report.dart';
import 'package:multitap_client/models/stream_options.dart';

import 'remote_session_test.dart' show SessionSocket, info, packet;
import 'session_recovery_test.dart' show ManualClock, RecoveryMedia;

Map<String, dynamic> native({
  bool playing = true,
  double depth = 42,
  int underruns = 0,
  int overruns = 0,
  int inserted = 0,
  int trimmed = 0,
  double avTotal = 0,
  int avSamples = 0,
}) => {
  'audioEnabled': true,
  'audioPlaying': playing,
  'audioDepthMs': depth,
  'audioTargetMs': 40.0,
  'audioLatencyMs': depth + 10,
  'audioUnderruns': underruns,
  'audioOverruns': overruns,
  'audioInsertedFrames': inserted,
  'audioTrimmedFrames': trimmed,
  'audioAvOffsetTotalMs': avTotal,
  'audioAvOffsetSamples': avSamples,
};

Uint8List audio(int pts) {
  final bytes = Uint8List(12 + 16)..[0] = 2;
  ByteData.sublistView(bytes).setUint64(4, pts, Endian.little);
  return bytes;
}

/// Video finds the decoder full while [videoFull]; audio is always taken.
class AudioMedia extends RecoveryMedia {
  bool videoFull = false;
  Map<String, dynamic> audioStats = native();
  @override
  Future<void> push(Uint8List packet) async {
    if (packet[0] == 1 && videoFull) {
      fullAnswers++;
      throw PlatformException(code: 'media_queue_full');
    }
    packets.add(packet);
  }

  @override
  Future<Map<String, dynamic>> stats() async => {
    ...await super.stats(),
    'audioError': 0,
    ...audioStats,
  };
}

void main() {
  test(
    'Native counters become per-reading changes; a restart counts from zero',
    () {
      final tracker = AudioStatsTracker();
      expect(tracker.read({'audioEnabled': false}), isNull);
      expect(
        tracker.read({'audioEnabled': true}),
        isNull,
        reason: 'an engine without audio-stats-v1',
      );
      var reading = tracker.read(
        native(underruns: 2, inserted: 30, avTotal: 90, avSamples: 3),
      )!;
      expect(reading.underruns, 2);
      expect(reading.insertedFrames, 30);
      expect(reading.avOffsetSumMs, 90);
      expect(reading.avOffsetSamples, 3);
      expect(reading.depthMs, 42);
      reading = tracker.read(
        native(
          underruns: 3,
          inserted: 30,
          trimmed: 7,
          avTotal: 60,
          avSamples: 6,
        ),
      )!;
      expect(reading.underruns, 1);
      expect(reading.insertedFrames, 0);
      expect(reading.trimmedFrames, 7);
      expect(
        reading.avOffsetSumMs,
        -30,
        reason: 'the newer offsets were negative',
      );
      expect(reading.avOffsetSamples, 3);
      reading = tracker.read(
        native(playing: false, underruns: 1, avTotal: 5, avSamples: 1),
      )!;
      expect(reading.underruns, 1, reason: 'a restarted engine');
      expect(reading.avOffsetSumMs, 5);
      expect(reading.depthMs, isNull, reason: 'no depth while no sound plays');
      expect(reading.latencyMs, isNull);
      expect(reading.targetMs, 40);
    },
  );

  test(
    'A new stream does not report the counts of earlier streams again',
    () {
      // Android's AndroidAudioTrim counts for the whole app: 17 overruns in one
      // session stay 17 in every later one, and a tracker reset at each
      // session start or resize would send them again (249 for 31 real).
      final tracker = AudioStatsTracker();
      var reading = tracker.read(native(overruns: 17, trimmed: 480))!;
      expect(reading.overruns, 17);
      expect(reading.trimmedFrames, 480);
      tracker.reset();
      reading = tracker.read(native(overruns: 17, trimmed: 480))!;
      expect(reading.overruns, 0, reason: 'nothing new since the last reading');
      expect(reading.trimmedFrames, 0);
      reading = tracker.read(native(overruns: 20, trimmed: 480))!;
      expect(reading.overruns, 3);
      tracker.reset();
      // A Windows engine that really restarted counts from zero again.
      reading = tracker.read(native(overruns: 2))!;
      expect(reading.overruns, 2, reason: 'a restarted engine');
    },
  );

  test(
    'The Host members are taken once each, and malformed ones are ignored',
    () {
      final tracker = AudioStatsTracker();
      tracker.host({
        'audioCapture': {'packets': 100, 'avgUs': 12000, 'maxUs': 31000},
        'audioSend': {
          'sent': 98,
          'dropped': 2,
          'queuePackets': 40,
          'queueAvgUs': 800,
          'queueMaxUs': 9000,
        },
      });
      tracker.host({
        'audioCapture': {'packets': 100, 'avgUs': 14000, 'maxUs': 20000},
        'audioSend': {'sent': 100, 'dropped': 0},
      });
      tracker.host({
        'audioCapture': {
          'packets': 5,
          'avgUs': 50000,
          'maxUs': 100,
        }, // average above maximum
        'audioSend': {'sent': -1, 'dropped': 0},
      });
      var reading = tracker.read(native())!;
      expect(reading.hostCapturePackets, 200);
      expect(reading.hostCaptureSumUs, 2600000);
      expect(reading.hostCaptureMaxUs, 31000);
      expect(reading.hostSent, 198);
      expect(reading.hostDropped, 2);
      expect(reading.hostQueuePackets, 40);
      expect(reading.hostQueueMaxUs, 9000);
      reading = tracker.read(native())!;
      expect(
        reading.hostSent,
        0,
        reason: 'a stream.stats counts once, however long it is shown',
      );
      expect(reading.hostCaptureMaxUs, isNull);
    },
  );

  test('A window reports the audio fields as fixed non-negative numbers', () {
    final window = StreamStatsWindow(firstPeriod: const Duration(seconds: 10));
    StreamStatsSample tick(AudioStatsReading? audio) => StreamStatsSample(
      width: 1920,
      height: 1080,
      decodedFps: 60,
      audio: audio,
    );
    window.add(
      tick(
        const AudioStatsReading(
          depthMs: 40,
          latencyMs: 50,
          targetMs: 40,
          underruns: 1,
          avOffsetSumMs: 120,
          avOffsetSamples: 4,
          hostCapturePackets: 100,
          hostCaptureSumUs: 1000000,
          hostCaptureMaxUs: 25000,
          hostSent: 100,
          hostQueuePackets: 100,
          hostQueueSumUs: 50000,
          hostQueueMaxUs: 4000,
        ),
      ),
      0,
    );
    window.add(
      tick(
        const AudioStatsReading(
          depthMs: 60,
          latencyMs: 70,
          targetMs: 60,
          underruns: 1,
          overruns: 1,
          insertedFrames: 12,
          trimmedFrames: 30,
          avOffsetSumMs: 40,
          avOffsetSamples: 4,
          hostSent: 99,
          hostDropped: 1,
        ),
      ),
      1000000,
    );
    window.add(
      tick(const AudioStatsReading(targetMs: 60)),
      2000000,
    ); // the VM went silent
    window.add(tick(null), 3000000);
    final report = window.take(10000000)!;
    expect(report['audioDepthMs'], 50);
    expect(report['audioLatencyMs'], 60);
    expect(report['audioTargetMs'], 60);
    expect(report['audioUnderruns'], 2);
    expect(report['audioOverruns'], 1);
    expect(report['audioInserted'], 12);
    expect(report['audioTrimmed'], 30);
    expect(
      report['audioAvLagMs'],
      20,
      reason: '160 ms over 8 frames: the sound is behind',
    );
    expect(report.containsKey('audioAvLeadMs'), isFalse);
    expect(report['hostAudioCaptureUs'], 10000);
    expect(report['hostAudioCaptureMaxUs'], 25000);
    expect(report['hostAudioSent'], 199);
    expect(report['hostAudioDropped'], 1);
    expect(report['hostAudioQueueUs'], 500);
    expect(report['hostAudioQueueMaxUs'], 4000);
    expect(report.keys.every(streamStatsFields.contains), isTrue);
    expect(audioStatsFields.every(streamStatsFields.contains), isTrue);
    window.add(
      tick(const AudioStatsReading(avOffsetSumMs: -90, avOffsetSamples: 3)),
      11000000,
    );
    final next = window.take(21000000)!;
    expect(next['audioAvLeadMs'], 30, reason: 'the sound ahead of the picture');
    expect(next.containsKey('audioAvLagMs'), isFalse);
    expect(next.containsKey('audioDepthMs'), isFalse);
    expect(
      next.containsKey('hostAudioSent'),
      isFalse,
      reason: 'no Host audio in this window',
    );
    final quiet = StreamStatsWindow()..add(tick(null), 0);
    expect(quiet.take(1000000)!.keys.where(audioStatsFields.contains), isEmpty);
  });

  testWidgets(
    'A session reports both sides and plays audio while video waits for the decoder (#98)',
    (t) async {
      final socket = SessionSocket(), media = AudioMedia();
      final clock = ManualClock();
      final reports = <Map<String, Object>>[];
      final current = RemoteSession(
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
        ),
        token: 'fixture-only-token',
        media: media,
        clock: clock,
        dialer: (_, _) async => socket,
        statsSink: reports.add,
      );
      unawaited(current.start());
      await t.pump();
      socket.incoming.add(jsonEncode(info));
      await t.pump();
      var answered = 0, second = 0, pts = 0;
      Future<void> advance(int seconds) async {
        for (var i = 0; i < seconds; i++) {
          clock.now = ++second * 1000000;
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
          socket.incoming.add(
            jsonEncode({
              'type': 'stream.stats',
              'capturedFps': 30,
              'encodedFps': 30,
              'bitrateKbps': 8000,
              'audioCapture': {'packets': 100, 'avgUs': 11000, 'maxUs': 23000},
              'audioSend': {
                'sent': 100,
                'dropped': 0,
                'queuePackets': 100,
                'queueAvgUs': 700,
                'queueMaxUs': 5000,
              },
            }),
          );
          media.audioStats = native(
            underruns: second ~/ 5,
            trimmed: second * 10,
            avTotal: second * 25.0,
            avSamples: second,
          );
          await t.pump(const Duration(seconds: 1));
        }
      }

      await advance(14);
      expect(current.frameReady, isTrue, reason: current.error);
      expect(reports, hasLength(1));
      final report = reports.first;
      expect(report['hostAudioSent'] as int, greaterThanOrEqualTo(900));
      expect(report['hostAudioDropped'], 0);
      expect(report['hostAudioCaptureUs'], 11000);
      expect(report['hostAudioQueueMaxUs'], 5000);
      expect(report['audioDepthMs'], 42);
      expect(report['audioAvLagMs'], 25);
      expect(report['audioUnderruns'] as int, greaterThanOrEqualTo(1));
      expect(report.keys.every(streamStatsFields.contains), isTrue);

      // The decoder is full: the reader waits on this video packet. Audio that
      // arrives behind it is still played at once.
      final before = media.packets.where((p) => p[0] == 2).length;
      media.videoFull = true;
      socket.incoming.add(packet());
      await t.pump(const Duration(milliseconds: 20));
      for (var n = 0; n < 5; n++) {
        socket.incoming.add(audio(pts += 10000));
      }
      await t.pump(const Duration(milliseconds: 20));
      expect(
        media.fullAnswers,
        greaterThan(0),
        reason: 'the video push is waiting',
      );
      expect(
        media.packets.where((p) => p[0] == 2).length,
        before + 5,
        reason: 'audio did not wait behind the video',
      );
      media.videoFull = false;
      await t.pump(const Duration(milliseconds: 50));
      // Video idle (a still screen sends no video): every audio packet still
      // reaches the engine as it arrives.
      final idle = media.packets.where((p) => p[0] == 2).length;
      for (var n = 0; n < 20; n++) {
        socket.incoming.add(audio(pts += 10000));
        await t.pump(const Duration(milliseconds: 10));
        expect(media.packets.where((p) => p[0] == 2).length, idle + n + 1);
      }
      expect(current.closed, isFalse, reason: current.error);
      await current.close();
      await media.events.close();
    },
  );
}
