import 'dart:convert';
import 'dart:io';

import 'package:flutter_test/flutter_test.dart';
import 'package:multitap_client/data/stream_observability.dart';
import 'package:multitap_client/data/stream_stats_report.dart';
import 'package:multitap_client/data/remote_session.dart';
import 'package:multitap_client/data/broker_api.dart';

const sessionId = '11111111-1111-4111-8111-111111111111';
void main() {
  tearDown(() => StreamObservability.enabled = false);
  test(
    'summary excludes preparation and closed phases, versions reject newlines',
    () {
      final report = StreamObservationReport();
      report.add(1000, 'connecting', {'hostWriteUs': 900});
      report.add(2000, 'streaming', {'hostWriteUs': 20});
      report.add(3000, 'resizing', {'hostWriteUs': 800});
      report.add(4000, 'streaming', {'hostWriteUs': 40});
      report.add(5000, 'closed', {'hostWriteUs': 700});
      final data = report.export(
        sessionId: sessionId,
        enabled: true,
        negotiated: true,
        outputs: 1,
        versions: {'client': '0.4.132\n', 'host': '0.2.109'},
      );
      expect(data['totalSnapshots'], 5);
      expect((data['summary'] as Map)['hostWriteUs'], {
        'samples': 2,
        'min': 20,
        'max': 40,
        'mean': 30.0,
        'snapshotP50': 20,
        'snapshotP95': 40,
      });
      expect((data['versions'] as Map)['client'], null);
      expect((data['versions'] as Map)['host'], '0.2.109');
    },
  );
  test('local-only observations cannot escape the negotiated diagnostics allowlist', () {
    final window = StreamStatsWindow();
    window.add(
      const StreamStatsSample(
        width: 1280,
        height: 720,
        observability: {
          'hostQueuePackets': 3,
          'decodeQueueBytes': 1234,
          'presentQueuePackets': 2,
          'hostCaptureUs': 700,
        },
      ),
      1000,
    );
    final report = window.take(2000)!;
    expect(report['hostQueuePackets'], 3);
    expect(report.keys.every(streamStatsFields.contains), true);
    expect(report.containsKey('decodeQueueBytes'), false);
    expect(report.containsKey('hostCaptureUs'), false);
  });
  test(
    'preference is opt-in, bounded, and ordered for future child windows',
    () async {
      final folder = await Directory.systemTemp.createTemp(
        'multitap-observation-pref-',
      );
      final file = File('${folder.path}/observation.json');
      try {
        await StreamObservability.init(file);
        expect(StreamObservability.enabled, false);
        await Future.wait([
          StreamObservability.choose(true),
          StreamObservability.choose(false),
        ]);
        expect(jsonDecode(await file.readAsString()), {'enabled': false});
        await StreamObservability.choose(true);
        await StreamObservability.init(file);
        expect(StreamObservability.enabled, true);
        await file.writeAsString('x' * 257);
        await StreamObservability.init(file);
        expect(StreamObservability.enabled, false);
      } finally {
        await StreamObservability.init(null);
        if (await file.exists()) await file.delete();
        await folder.delete();
      }
    },
  );
  test('output identity and source observations remain separate and redact unknown fields', () {
    final report = StreamObservationReport();
    report.add(
      1000,
      'streaming',
      {},
      epoch: 2,
      outputs: {
        '0': {
          'host': {'id': '0', 'epoch': 7, 'capturedFps': 60, 'token': 'SECRET'},
          'decoder': {
            'id': '0',
            'epoch': 7,
            'decodedFrames': 90,
            'hardwareDecode': true,
          },
        },
        '1': {
          'host': {'id': '0', 'epoch': 7, 'capturedFps': 999},
        },
        'SECRET': {
          'host': {'id': '0', 'epoch': 7},
        },
      },
    );
    final exported = report.export(
      sessionId: sessionId,
      enabled: true,
      negotiated: true,
      outputs: 2,
      distributions: {
        'hostApply': {'samples': 10, 'p50Ms': 1, 'p95Ms': 3, 'key': 'SECRET'},
        'roundTrip': {'samples': 0, 'p50Ms': 1, 'p95Ms': 3},
      },
      versions: {'broker': '0.5.91', 'host': 'SECRET'},
    );
    final row = (exported['snapshots'] as List).single as Map;
    expect(row['epoch'], 2);
    expect((row['outputs'] as Map).keys, ['0']);
    expect((exported['inputDistributions'] as Map).keys, ['hostApply']);
    expect((exported['versions'] as Map)['broker'], '0.5.91');
    expect(jsonEncode(exported).contains('SECRET'), false);
  });
  test('optional Host snapshot is projected and never accepts secrets or invalid numbers', () {
    expect(
      parseHostObservation({
        'protocol': streamObservabilityProtocol,
        'metrics': {
          'hostQueuePackets': 4,
          'hostQueueBytes': -1,
          'readbackUs': double.nan,
          'serviceCpuPct10': 1001,
          'password': 'SECRET',
          'hostWriteUs': 60000000,
        },
      }),
      {'hostQueuePackets': 4, 'hostWriteUs': 60000000},
    );
    expect(
      parseHostObservation({
        'protocol': 'future',
        'metrics': {'hostQueuePackets': 1},
      }),
      isEmpty,
    );
    expect(
      parseHostObservation({
        'protocol': streamObservabilityProtocol,
        'metrics': [],
      }),
      isEmpty,
    );
  });
  test('retention, quantile meaning, absent fields and terminal ownership are explicit', () {
    final report = StreamObservationReport(maximumSamples: 3);
    for (var n = 1; n <= 4; n++) {
      report.add(n * 1000, 'streaming', {
        'hostWriteUs': n * 10,
        'token': 'SECRET',
      });
    }
    report.add(3000, 'streaming', {'hostWriteUs': 500}); // late owner
    final data = report.export(
      sessionId: sessionId,
      enabled: true,
      negotiated: true,
      outputs: 2,
    );
    expect(data['totalSnapshots'], 4);
    expect(data['retainedSnapshots'], 3);
    final metric = (data['summary'] as Map)['hostWriteUs'] as Map;
    expect(metric, {
      'samples': 3,
      'min': 20,
      'max': 40,
      'mean': 30.0,
      'snapshotP50': 30,
      'snapshotP95': 40,
    });
    expect((data['summary'] as Map).containsKey('readbackUs'), false);
    expect(jsonEncode(data).contains('SECRET'), false);
    report.add(5000, 'closed', {});
    report.add(6000, 'streaming', {'hostWriteUs': 900});
    expect(
      report.export(
        sessionId: sessionId,
        enabled: true,
        negotiated: true,
        outputs: 1,
      )['totalSnapshots'],
      5,
    );
  });
  test('path and categorical facts cannot export arbitrary addresses or input content', () {
    final data = StreamObservationReport().export(
      sessionId: 'SECRET-TOKEN',
      enabled: false,
      negotiated: false,
      outputs: 1,
      path: {'clientTls': 'proxy', 'hostTls': 'https://SECRET'},
      facts: {
        'captureBackend': 'gdi',
        'encoderBackend': 'SECRET',
        'clipboard': 'SECRET',
      },
    );
    expect(data['sessionId'], isNull);
    expect(data['path'], {'clientTls': 'proxy'});
    expect(data['facts'], {'captureBackend': 'gdi'});
    expect(jsonEncode(data).contains('SECRET'), false);
    expect(
      (data['unavailable'] as Map)['tcpRtt'],
      'os_connection_measurement_required',
    );
  });
  test('observability is off by default and does not need a media engine to export', () async {
    final session = RemoteSession(
      connection: BrokerConnection(
        sessionId: sessionId,
        webSocketUrl: Uri.parse(
          'wss://example.test/api/v1/sessions/$sessionId',
        ),
      ),
      token: 'DO_NOT_EXPORT',
    );
    expect(session.observabilityEnabled, false);
    expect(jsonDecode(session.observabilityJson())['totalSnapshots'], 0);
    session.setObservabilityEnabled(true);
    expect(StreamObservability.enabled, true);
    expect(jsonDecode(session.observabilityJson())['negotiated'], false);
    expect(session.observabilityJson().contains('DO_NOT_EXPORT'), false);
    session.setObservabilityEnabled(false);
  });
}
