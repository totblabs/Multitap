import 'dart:async';
import 'dart:convert';

import 'package:flutter_test/flutter_test.dart';
import 'package:multitap_client/data/broker_api.dart';
import 'package:multitap_client/data/client_diagnostics.dart';
import 'package:multitap_client/data/remote_session.dart';
import 'package:multitap_client/data/stream_stats_report.dart';
import 'package:multitap_client/data/stream_telemetry.dart';

import 'remote_session_test.dart' show SessionSocket, info, packet;
import 'session_recovery_test.dart'
    show ManualClock, RecoveryMedia, selectedOptions;

const pipeline = HostPipelineMetrics(
  samples: 60,
  topologyMs: .2,
  captureMs: 3.5,
  convertMs: 1.1,
  encodeMs: 4.4,
  workMs: 9.2,
  p95Ms: 11.5,
  maxMs: 16.1,
  captureMaxMs: 9.5,
  encodeMaxMs: 12.25,
);

StreamStatsSample sample(
  double fps, {
  double? rtt,
  HostStreamTelemetry? host,
}) => StreamStatsSample(
  width: 1264,
  height: 760,
  sourceWidth: 1264,
  sourceHeight: 760,
  decodedFps: fps,
  textureFps: fps,
  receivedMbps: 18.4,
  rttMs: rtt,
  clientDecodeMs: 2.1,
  clientDecodeMaxMs: 7.5,
  clientQueueMs: 3,
  skippedDecodedFrames: 2,
  appliedBitrate: 42000000,
  host: host,
);

void main() {
  test('A window reports its longest video gap, then starts over', () {
    final window = StreamStatsWindow(firstPeriod: const Duration(seconds: 2));
    for (final (at, gap) in [(0, 12.0), (1000000, 80.5), (2000000, 30.0)]) {
      window.add(
        StreamStatsSample(width: 1280, height: 720, videoGapMaxMs: gap),
        at,
      );
    }
    expect(window.take(3000000)!['videoGapMaxMs'], 81);
    expect(streamStatsFields, contains('videoGapMaxMs'));
    window.add(const StreamStatsSample(width: 1280, height: 720), 3000000);
    expect(window.take(40000000)!['videoGapMaxMs'], isNot(81));
  });
  test('A window reports scaled averages and extremes, then starts over', () {
    final window = StreamStatsWindow(firstPeriod: const Duration(seconds: 30));
    expect(window.take(0), isNull);
    const host = HostStreamTelemetry(
      capturedFps: 60,
      encodedFps: 59,
      bitrateKbps: 41000,
      pipeline: pipeline,
      bitrateTarget: 45000000,
      automaticBitrate: true,
      networkDelayMs: 4,
    );
    window.add(sample(60, rtt: 20, host: host), 0);
    window.add(sample(50, rtt: 30), 1000000);
    window.add(
      StreamStatsSample(
        width: 1264,
        height: 760,
        decodedFps: 58,
        appliedBitrate: 42000000,
        inputHostP50Ms: 6.1,
        inputFrameP95Ms: 48.2,
      ),
      2000000,
    );
    expect(window.due(29000000), isFalse);
    expect(window.due(30000000), isTrue);
    expect(window.size, (1264, 760));
    final report = window.take(30000000)!;
    expect(report['samples'], 3);
    expect(report['windowMs'], 30000);
    expect(report['decodeFps10'], 560);
    expect(report['decodeFpsMin10'], 500);
    expect(report['rttMs'], 25);
    expect(report['rttMaxMs'], 30);
    expect(report['captureFps10'], 600);
    expect(report['encodeFps10'], 590);
    expect(report['hostWorkUs'], 9200);
    expect(report['hostP95Us'], 11500);
    expect(report['encodeUs'], 4400);
    expect(report['captureMaxUs'], 9500);
    expect(report['encodeMaxUs'], 12250);
    expect(report['convertMaxUs'], isNull, reason: 'older host field absent');
    expect(report['decodeUs'], 2100);
    expect(report['receiveKbps'], 18400);
    expect(report['inputHostP50Us'], 6100);
    expect(report['inputFrameP95Us'], 48200);
    expect(report['inputHostP95Us'], isNull);
    expect(report['bitrateKbps'], 42000);
    expect(report['targetKbps'], isNull, reason: 'last sample had no host');
    expect(report['auto'], isNull);
    expect(report.keys.every(streamStatsFields.contains), isTrue);
    expect(window.samples, 0);
    expect(window.take(31000000), isNull);
  });

  test('A window reports texture pacing and how the client renders', () {
    final window = StreamStatsWindow(firstPeriod: const Duration(seconds: 10));
    StreamStatsSample paced(
      double wait,
      double upload,
      int queued,
      int skipped, {
      bool? gpu = false,
    }) => StreamStatsSample(
      width: 1264,
      height: 760,
      decodedFps: 37,
      textureFps: 37,
      textureWaitMs: wait,
      textureUploadMs: upload,
      textureQueuedFrames: queued,
      skippedFrames: skipped,
      gpuHardware: gpu,
      remoteSession: true,
      texturePath: 0,
      displayHz: 32,
      decodeTier: 2,
      decodeVendor: 0x8086,
    );
    window.add(paced(9.5, .18, 16, 0), 0);
    window.add(paced(16.5, .22, 18, 2), 1000000);
    window.add(
      StreamStatsSample(width: 1264, height: 760, decodedFps: 37),
      2000000,
    );
    final report = window.take(10000000)!;
    expect(report['textureWaitUs'], 13000);
    expect(report['textureUploadUs'], 200);
    expect(report['queuedFrames'], 34);
    expect(report['skippedFrames'], 2);
    // Render facts come from the window's last reading only.
    expect(report['gpuHardware'], isNull);
    expect(report['remoteSession'], isNull);
    expect(report.keys.every(streamStatsFields.contains), isTrue);
    window.add(paced(4, .5, 0, 0, gpu: true), 11000000);
    final next = window.take(21000000)!;
    expect(next['gpuHardware'], isTrue);
    expect(next['remoteSession'], isTrue);
    expect(next['texturePath'], 0);
    expect(next['displayHz'], 32);
    expect(next['decodeTier'], 2);
    expect(next['decodeVendor'], 0x8086);
    expect(next['queuedFrames'], 0);
    expect(next['textureWaitUs'], 4000);
  });

  test('A window reports where input -> frame goes (latest host and client shares)', () {
    final window = StreamStatsWindow(firstPeriod: const Duration(seconds: 10));
    HostStreamTelemetry host(double capture) => HostStreamTelemetry(
      capturedFps: 60,
      encodedFps: 60,
      bitrateKbps: 4000,
      inputFrame: HostInputFrameMetrics(
        samples: 9,
        injectToCaptureP50Ms: capture,
        injectToCaptureP95Ms: capture * 2,
        captureToEncodedP50Ms: 2.25,
        captureToEncodedP95Ms: 5.5,
      ),
    );
    window.add(
      StreamStatsSample(
        width: 1264,
        height: 760,
        decodedFps: 60,
        host: host(40),
        inputFrameP50Ms: 59,
        inputPushP50Ms: .8,
        inputDecodeP50Ms: 1.6,
      ),
      0,
    );
    window.add(
      StreamStatsSample(
        width: 1264,
        height: 760,
        decodedFps: 60,
        host: host(31.5),
      ),
      1000000,
    );
    window.add(
      const StreamStatsSample(width: 1264, height: 760, decodedFps: 60),
      2000000,
    );
    final report = window.take(10000000)!;
    expect(report['hostInputCaptureP50Us'], 31500, reason: 'latest host value');
    expect(report['hostInputCaptureP95Us'], 63000);
    expect(report['hostCaptureEncodedP50Us'], 2250);
    expect(report['hostCaptureEncodedP95Us'], 5500);
    expect(report['inputPushP50Us'], 800);
    expect(report['inputDecodeP50Us'], 1600);
    expect(report['inputFrameP50Us'], 59000);
    expect(report.keys.every(streamStatsFields.contains), isTrue);
    window.add(
      const StreamStatsSample(width: 1264, height: 760, decodedFps: 60),
      11000000,
    );
    final next = window.take(21000000)!;
    expect(next.containsKey('hostInputCaptureP50Us'), isFalse);
    expect(next.containsKey('inputPushP50Us'), isFalse);
  });

  test('A window reports the tagged packet after the encoder (host send, relay, transfer, size)', () {
    final window = StreamStatsWindow(firstPeriod: const Duration(seconds: 10));
    window.add(
      const StreamStatsSample(
        width: 1580,
        height: 892,
        decodedFps: 7,
        host: HostStreamTelemetry(
          capturedFps: 18,
          encodedFps: 7,
          bitrateKbps: 900,
          inputSend: SendPathMetrics(
            samples: 6,
            firstP50Ms: 1.2,
            firstP95Ms: 4.1,
            secondP50Ms: .3,
            secondP95Ms: 2.6,
          ),
          relayForward: SendPathMetrics(
            samples: 6,
            firstP50Ms: .4,
            firstP95Ms: 1.9,
          ),
        ),
        inputTransferP50Ms: 9.8,
        inputTransferP95Ms: 31,
        inputFrameBytes: 48211,
        inputDownlinkP50Ms: 11.4,
        inputDownlinkP95Ms: 52,
      ),
      0,
    );
    final report = window.take(10000000)!;
    expect(report['hostSendQueueP50Us'], 1200);
    expect(report['hostSendQueueP95Us'], 4100);
    expect(report['hostSendWriteP50Us'], 300);
    expect(report['hostSendWriteP95Us'], 2600);
    expect(report['relayForwardP50Us'], 400);
    expect(report['relayForwardP95Us'], 1900);
    expect(report['inputTransferP50Us'], 9800);
    expect(report['inputTransferP95Us'], 31000);
    expect(report['inputFrameBytes'], 48211);
    expect(report['inputDownlinkP50Us'], 11400);
    expect(report['inputDownlinkP95Us'], 52000);
    expect(report.keys.every(streamStatsFields.contains), isTrue);
    window.add(
      const StreamStatsSample(width: 1580, height: 892, decodedFps: 7),
      11000000,
    );
    final next = window.take(21000000)!;
    for (final key in [
      'hostSendQueueP50Us',
      'relayForwardP50Us',
      'inputTransferP50Us',
      'inputFrameBytes',
    ]) {
      expect(next.containsKey(key), isFalse, reason: key);
    }
  });

  test('A window reports backpressure that no longer ends the stream', () {
    final window = StreamStatsWindow(firstPeriod: const Duration(seconds: 10));
    StreamStatsSample tick({int? waits, int? dropped}) => StreamStatsSample(
      width: 1920,
      height: 1080,
      decodedFps: 60,
      mediaWaits: waits,
      audioDroppedFrames: dropped,
    );
    window.add(tick(waits: 0, dropped: 0), 0);
    window.add(tick(waits: 3, dropped: 960), 1000000);
    window.add(tick(waits: 1, dropped: 1920), 2000000);
    window.add(tick(), 3000000);
    final report = window.take(10000000)!;
    expect(report['mediaWaits'], 4);
    // 2880 frames of 48 kHz audio dropped = 60 ms.
    expect(report['audioDropMs'], 60);
    expect(report.keys.every(streamStatsFields.contains), isTrue);
    window.add(tick(), 11000000);
    final quiet = window.take(21000000)!;
    expect(quiet.containsKey('mediaWaits'), isFalse);
    expect(quiet.containsKey('audioDropMs'), isFalse);
  });

  test(
    'Diagnostics keep only the bounded integer/boolean statistics fields',
    () async {
      final sent = <Map<String, Object?>>[];
      final diagnostics = ClientDiagnostics(
        send: (report) async {
          sent.add(report);
          return DiagnosticDelivery.accepted;
        },
        desktopId: 'pve-149',
        mode: 'stream',
        version: () => '0.4.62+76',
      );
      diagnostics.record(
        'streamStats',
        values: {
          'decodeFps10': 598,
          'auto': true,
          'width': 1264,
          'secretText': 'PRIVATE_SENTINEL',
          'rttMs': -1,
          'receiveKbps': 1 << 40,
          'ok': true,
          'gpuHardware': false,
          'texturePath': 0,
          'textureWaitUs': 13000,
          'monitors': 2,
          'adapterName': 'PRIVATE_ADAPTER',
        },
        sessionId: '614c03e4-5595-4d77-a075-f6a61cfc4e8b',
      );
      await diagnostics.flush();
      diagnostics.close();
      final events = (sent.single['events'] as List)
          .cast<Map<String, Object?>>();
      final stats = events.singleWhere((e) => e['event'] == 'streamStats');
      expect(stats['values'], {
        'decodeFps10': 598,
        'auto': true,
        'width': 1264,
        'gpuHardware': false,
        'texturePath': 0,
        'textureWaitUs': 13000,
        'monitors': 2,
      });
      expect(jsonEncode(sent).contains('PRIVATE_ADAPTER'), isFalse);
      expect(jsonEncode(sent).contains('PRIVATE_SENTINEL'), isFalse);
      expect(stats['sessionId'], '614c03e4-5595-4d77-a075-f6a61cfc4e8b');
    },
  );

  testWidgets(
    'A streaming session sends its window through the sink when due and on close',
    (t) async {
      final socket = SessionSocket(), media = RecoveryMedia();
      final clock = ManualClock();
      final reports = <Map<String, Object>>[];
      final current = RemoteSession(
        connection: BrokerConnection(
          sessionId: 'qa-session',
          webSocketUrl: Uri.parse(
            'wss://cloud.example.com/api/v1/sessions/qa-session',
          ),
          options: selectedOptions,
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
      var answered = 0, second = 0;
      Future<void> advance(int seconds) async {
        for (var i = 0; i < seconds; i++) {
          clock.now = ++second * 1000000;
          expect(current.closed, isFalse, reason: current.error);
          // A live host answers each liveness ping.
          for (; answered < socket.sent.length; answered++) {
            final sent = socket.sent[answered];
            if (sent['type'] == 'ping') {
              socket.incoming.add(
                jsonEncode({'type': 'pong', 'id': sent['id']}),
              );
            }
          }
          socket.incoming.add(packet());
          await t.pump(const Duration(seconds: 1));
        }
      }

      await advance(14);
      expect(current.frameReady, isTrue, reason: current.error);
      expect(reports, hasLength(1), reason: 'a short first window');
      expect(reports.first['width'], 1280);
      expect(reports.first['samples'] as int, inInclusiveRange(9, 11));
      expect(reports.first.keys.every(streamStatsFields.contains), isTrue);
      await advance(30);
      expect(reports, hasLength(2), reason: 'then 30 s windows');
      expect(reports[1]['samples'] as int, inInclusiveRange(29, 31));
      // Closing the window flushes the partial window before diagnostics close.
      await advance(4);
      current.flushStats();
      expect(reports, hasLength(3));
      current.flushStats();
      expect(reports, hasLength(3), reason: 'nothing new to report');
      await current.close();
      expect(reports, hasLength(3));
      await media.events.close();
    },
  );
}
