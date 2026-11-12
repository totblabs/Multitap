import 'dart:async';
import 'dart:convert';
import 'dart:typed_data';

import 'package:flutter_test/flutter_test.dart';
import 'package:multitap_client/data/broker_api.dart';
import 'package:multitap_client/data/remote_session.dart';

import 'remote_session_test.dart' show SessionMedia, SessionSocket, flush;

const _sessionId = 'a015f587-2af5-4e8b-a5c9-90fd48b4bf52';
Map<String, dynamic> _connectionJson() => {
  'sessionId': _sessionId,
  'webSocketUrl': 'wss://cloud.example.com/api/v1/sessions/$_sessionId',
  'mode': 'stream',
  'transport': 'media-epoch-v1',
  'resizeProtocol': 'epoch-v1',
};

/// One video packet of [epoch] holding SPS, PPS and an IDR slice.
Uint8List _keyFrame(int epoch) => Uint8List.fromList([
  1, 1, epoch & 255, epoch >> 8, 0, 0, 0, 0, 0, 0, 0, 0, //
  0, 0, 0, 1, 0x67, 1,
  0, 0, 0, 1, 0x68, 1,
  0, 0, 0, 1, 0x65, 1,
]);

class _Media extends SessionMedia {
  int decoded = 0;
  @override
  Future<Map<String, dynamic>> stats() async => {
    'decodedFrames': decoded,
    'error': 0,
    'audioError': -1,
    'audioPlayedFrames': 0,
  };
}

void main() {
  test('Ordinary epoch stream remains a valid old-peer connection', () {
    final value = BrokerConnection.fromJson(
      _connectionJson(),
      Uri.parse('https://cloud.example.com'),
    );
    expect(value.sessionId, _sessionId);
    expect(value.epochMedia, isTrue);
  });

  test('A connection answer that carries a sign-in phase binding is refused', () {
    expect(
      () => BrokerConnection.fromJson({
        ..._connectionJson(),
        'signin': {
          'protocol': 'signin-stream-v1',
          'phase': 'signin',
          'workerGeneration': 1,
          'transitionId': '1' * 32,
          'mediaScope': {'kind': 'composite', 'epoch': 1},
        },
      }, Uri.parse('https://cloud.example.com')),
      throwsA(
        isA<BrokerException>().having((e) => e.code, 'code', 'invalid_response'),
      ),
    );
  });

  test('Ordinary old-peer stream retains its first frame behavior', () async {
    final socket = SessionSocket(), media = _Media();
    final connection = BrokerConnection.fromJson(
      _connectionJson(),
      Uri.parse('https://cloud.example.com'),
    );
    final session = RemoteSession(
      connection: connection,
      token: 'synthetic-only',
      media: media,
      dialer: (_, _) async => socket,
    );
    try {
      unawaited(session.start());
      await flush();
      socket.incoming.add(
        jsonEncode({
          'type': 'stream.info',
          'sessionId': _sessionId,
          'epoch': 0,
          'codec': 'h264',
          'width': 1280,
          'height': 720,
          'sourceWidth': 1280,
          'sourceHeight': 720,
          'fpsTarget': 60,
        }),
      );
      await flush();
      socket.incoming.add(_keyFrame(0));
      media.decoded = 1;
      await Future<void>.delayed(const Duration(milliseconds: 65));
      expect(session.closed, isFalse);
      expect(session.frameReady, isTrue);
      expect(
        socket.sent.where((e) => e['type'] == 'stream.phase.decoded'),
        isEmpty,
      );
    } finally {
      await session.close();
    }
  });
}
