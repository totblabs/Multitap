import 'dart:async';
import 'dart:convert';

import 'package:flutter_test/flutter_test.dart';
import 'package:multitap_client/data/broker_api.dart';
import 'package:multitap_client/data/remote_session.dart';
import 'package:multitap_client/data/clipboard_policy.dart';

import 'remote_session_test.dart'
    show SessionSocket, SessionMedia, info, packet;

void main() {
  TestWidgetsFlutterBinding.ensureInitialized();
  test(
    'Clipboard URL is scoped to the authenticated broker and exact session',
    () {
      const media =
          'wss://cloud.example.com:18443/api/v1/sessions/test-session';
      final origin = Uri.parse('https://cloud.example.com:18443');
      BrokerConnection parse(Object? clipboard) => BrokerConnection.fromJson({
        'sessionId': 'test-session',
        'webSocketUrl': media,
        'clipboardWebSocketUrl': ?clipboard,
      }, origin);
      expect(parse(null).clipboardWebSocketUrl, isNull);
      expect(
        parse('$media/clipboard').clipboardWebSocketUrl.toString(),
        '$media/clipboard',
      );
      for (final unsafe in [
        'ws://cloud.example.com:18443/api/v1/sessions/test-session/clipboard',
        'wss://other.example.com:18443/api/v1/sessions/test-session/clipboard',
        'wss://cloud.example.com/api/v1/sessions/test-session/clipboard',
        'wss://cloud.example.com:18443/api/v1/sessions/other/clipboard',
        '$media/clipboard?token=forbidden',
        '$media/clipboard#forbidden',
        42,
      ]) {
        expect(() => parse(unsafe), throwsA(isA<BrokerException>()));
      }
    },
  );
  test('Clipboard setup failure leaves the video and input session running', () async {
    final media = SessionMedia(), socket = SessionSocket();
    final session = RemoteSession(
      connection: BrokerConnection(
        sessionId: 'test-session',
        webSocketUrl: Uri.parse(
          'wss://cloud.example.com/api/v1/sessions/test-session',
        ),
        // An invalid clipboard endpoint fails before any network/native access.
        clipboardWebSocketUrl: Uri.parse('ws://invalid.example.com'),
        clipboardPolicy: const ClipboardPolicy.all(),
      ),
      token: 'fixture-only',
      media: media,
      dialer: (_, _) async => socket,
    );
    unawaited(session.start());
    await Future<void>.delayed(const Duration(milliseconds: 2));
    socket.incoming.add(jsonEncode(info));
    await Future<void>.delayed(const Duration(milliseconds: 5));
    expect(session.textureId, 42);
    expect(
      session.clipboard,
      isNull,
      reason: 'Clipboard waits for actual initial video readiness.',
    );
    socket.incoming.add(packet());
    await Future<void>.delayed(const Duration(milliseconds: 5));
    expect(session.clipboard?.ready, isFalse);
    expect(session.clipboard?.error, 'clipboard_invalid_url');
    expect(session.closed, isFalse);
    expect(media.packets, hasLength(1));
    await session.capture(true);
    expect(session.captured, isTrue);
    await session.close();
    await media.events.close();
  });
}
