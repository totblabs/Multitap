import 'dart:async';
import 'dart:convert';

import 'package:flutter_test/flutter_test.dart';
import 'package:multitap_client/data/broker_api.dart';
import 'package:multitap_client/data/remote_session.dart';
import 'package:multitap_client/data/stream_observability.dart';

import 'remote_session_test.dart' show SessionSocket, info, packet;
import 'session_recovery_test.dart'
    show ManualClock, RecoveryMedia, selectedOptions;

const id = '22222222-2222-4222-8222-222222222222';

class ObservedMedia extends RecoveryMedia {
  @override
  Future<Map<String, dynamic>> stats() async => {
    ...await super.stats(),
    'queuedPackets': 2,
    'queuedBytes': 1000,
    'queueOldestMs': 7.0,
    'presentQueuePackets': 1,
    'presentQueueOldestMs': 3.0,
    'hardwareDecoder': true,
  };
}

void main() {
  tearDown(() => StreamObservability.enabled = false);
  for (final negotiated in [false, true]) {
    testWidgets(
      'session binds observation to owner/epoch and keeps ${negotiated ? 'new' : 'legacy'} transport alive',
      (t) async {
        final socket = SessionSocket(),
            media = ObservedMedia(),
            clock = ManualClock();
        final session = RemoteSession(
          connection: BrokerConnection(
            sessionId: id,
            webSocketUrl: Uri.parse('wss://example.test/api/v1/sessions/$id'),
            options: selectedOptions,
            observability: negotiated,
            observabilityPath: const {
              'clientTls': 'proxy',
              'hostTls': 'direct',
            },
          ),
          token: 'SECRET',
          media: media,
          clock: clock,
          observe: true,
          dialer: (_, _) async => socket,
        );
        unawaited(session.start());
        await t.pump();
        socket.incoming.add(
          jsonEncode({...info, 'captureBackend': 'wgc-d3d11'}),
        );
        await t.pump();
        Map<String, Object?> stats(String owner, int epoch, int size) => {
          'type': 'stream.stats',
          'sessionId': owner,
          'epoch': epoch,
          'capturedFps': 60,
          'encodedFps': 60,
          'bitrateKbps': 12000,
          'encoder': 'x264',
          'observability': {
            'protocol': streamObservabilityProtocol,
            'metrics': {
              'hostQueueBytes': size,
              'hostQueueOldestUs': 1200,
              'password': 'SECRET',
            },
          },
        };
        socket.incoming.add(jsonEncode(stats(id, 0, 123)));
        socket.incoming.add(packet());
        await t.pump(const Duration(seconds: 1));
        clock.now = 1000000;
        socket.incoming.add(packet());
        await t.pump(const Duration(seconds: 1));
        final first = jsonDecode(session.observabilityJson()) as Map;
        final summary = first['summary'] as Map;
        expect(summary.containsKey('hostQueueBytes'), negotiated);
        if (negotiated) expect(summary['hostQueueBytes']['max'], 123);
        expect(summary['decodeQueueBytes']['max'], 1000);
        expect(summary['presentQueueOldestUs']['max'], 3000);
        expect(first['facts']['captureBackend'], 'wgc-d3d11');
        expect(first['facts']['decoderMode'], 'hardware');
        expect(session.observabilityJson().contains('SECRET'), false);
        socket.incoming.add(
          jsonEncode(stats('33333333-3333-4333-8333-333333333333', 0, 999)),
        );
        socket.incoming.add(jsonEncode(stats(id, 99, 999)));
        clock.now = 2000000;
        socket.incoming.add(packet());
        await t.pump(const Duration(seconds: 1));
        expect(session.closed, false, reason: session.error);
        final ownerSummary =
            jsonDecode(session.observabilityJson())['summary'] as Map;
        if (negotiated) expect(ownerSummary['hostQueueBytes']['max'], 123);
        clock.now = 6000000;
        socket.incoming.add(packet());
        await t.pump(const Duration(seconds: 1));
        final latest =
            (jsonDecode(session.observabilityJson())['snapshots'] as List).last;
        expect(
          (latest['metrics'] as Map).containsKey('hostQueueBytes'),
          false,
          reason: 'Host snapshots expire instead of masquerading as current',
        );
        await session.close();
        await t.pump();
        await media.events.close();
        final closed =
            jsonDecode(session.observabilityJson())['snapshots'] as List;
        expect(closed.last['phase'], 'closed');
        expect(jsonDecode(session.observabilityJson())['facts']['encoderBackend'], 'x264', reason: 'Historical encoder survives disconnect');
      },
    );
  }
}
