// background-video-v1: the exact connect gating (asked only where the Host
// advertises it, dropped for a released broker), the grant check, what a
// session sends, and that a session that hid says where it stands again after
// its socket was replaced. Injected HTTP and socket: no network or Host.
import 'dart:async';
import 'dart:convert';

import 'package:flutter_test/flutter_test.dart';
import 'package:multitap_client/data/background_video.dart';
import 'package:multitap_client/data/broker_api.dart';
import 'package:multitap_client/data/remote_session.dart';
import 'package:multitap_client/models/stream_options.dart';

import 'power_api_test.dart' show ApiClient;
import 'remote_session_test.dart'
    show SessionMedia, SessionSocket, flush, packet, info;

const _origin = 'https://fixture.invalid';

Map<String, dynamic> _desktop(Map<String, Object> capabilities) => {
  'id': 'pve-149',
  'name': 'TESTER',
  'kind': 'vm',
  'description': '',
  'os': 'Windows',
  'status': 'ready',
  'canConnect': true,
  'host': {'connected': true, 'capabilities': capabilities},
};
Map<String, dynamic> _connected({bool granted = false}) => {
  'sessionId': 'background-session',
  'webSocketUrl': 'wss://fixture.invalid/api/v1/sessions/background-session',
  'mode': 'stream',
  'options': const StreamOptions().toJson(),
  if (granted) 'backgroundVideoProtocol': 'background-video-v1',
};

Future<(List<Map>, BrokerConnection)> _connect(
  Map<String, Object> capabilities,
  List<(int, Map<String, dynamic>)> replies,
) async {
  final client = ApiClient()
    ..responses.add((
      200,
      {
        'desktops': [_desktop(capabilities)],
      },
    ))
    ..responses.addAll(replies);
  final api = HttpBrokerApi(origin: Uri.parse(_origin), client: client);
  await api.desktops('fixture');
  final connection = await api.connect('fixture', 'pve-149');
  return (
    [
      for (final item in client.requests.skip(1))
        jsonDecode(item.body.toString()) as Map,
    ],
    connection,
  );
}

List<bool> _sent(SessionSocket socket) => [
  for (final event in socket.sent)
    if (event['type'] == 'stream.background') event['on'] as bool,
];

void main() {
  const released = <String, Object>{
    'service': true,
    'interactiveSession': true,
    'streaming': true,
    'virtualDisplayReady': true,
  };
  const capable = <String, Object>{
    ...released,
    'backgroundVideoProtocol': 'background-video-v1',
  };

  test('Advertised only with the exact marker', () {
    expect(backgroundVideoAdvertised(capable), isTrue);
    for (final value in <Object?>[
      released,
      {...released, 'backgroundVideoProtocol': 'background-video-v2'},
      {...released, 'backgroundVideoProtocol': true},
      null,
      'background-video-v1',
    ]) {
      expect(backgroundVideoAdvertised(value), isFalse, reason: '$value');
    }
    expect(backgroundVideoMessage(true), {
      'type': 'stream.background',
      'on': true,
    });
  });

  test('Asked where advertised; older peers never see it', () async {
    final granted = await _connect(capable, [
      (200, _connected(granted: true)),
    ]);
    expect(granted.$1.single['backgroundVideoProtocol'], 'background-video-v1');
    expect(granted.$2.backgroundVideo, isTrue);
    final older = await _connect(released, [(200, _connected())]);
    expect(older.$1.single.containsKey('backgroundVideoProtocol'), isFalse);
    expect(older.$2.backgroundVideo, isFalse);
    final ungranted = await _connect(capable, [(200, _connected())]);
    expect(ungranted.$2.backgroundVideo, isFalse);
    // A released broker that rejects the field: the retry carries none of it.
    final retried = await _connect(capable, [
      (
        400,
        {
          'error': {'code': 'unexpected_field'},
        },
      ),
      (200, _connected()),
    ]);
    expect(retried.$1.first['backgroundVideoProtocol'], 'background-video-v1');
    expect(retried.$1.last.containsKey('backgroundVideoProtocol'), isFalse);
    expect(retried.$2.backgroundVideo, isFalse);
  });

  test('A grant is accepted only when requested and exact', () {
    final origin = Uri.parse(_origin);
    expect(
      () => BrokerConnection.fromJson(_connected(granted: true), origin),
      throwsA(isA<BrokerException>()),
      reason: 'never requested',
    );
    expect(
      () => BrokerConnection.fromJson(
        {..._connected(), 'backgroundVideoProtocol': 'background-video-v2'},
        origin,
        requestedBackgroundVideo: true,
      ),
      throwsA(isA<BrokerException>()),
    );
    expect(
      BrokerConnection.fromJson(
        _connected(granted: true),
        origin,
        requestedBackgroundVideo: true,
      ).backgroundVideo,
      isTrue,
    );
  });

  test('The session hides and shows once each, only with the grant', () async {
    for (final granted in [true, false]) {
      final socket = SessionSocket(), media = SessionMedia();
      final session = RemoteSession(
        connection: BrokerConnection(
          sessionId: 'background-session',
          webSocketUrl: Uri.parse(
            'wss://fixture.invalid/api/v1/sessions/background-session',
          ),
          backgroundVideo: granted,
        ),
        token: 'fixture',
        media: media,
        dialer: (_, _) async => socket,
      );
      unawaited(session.start());
      await flush();
      expect(session.backgroundVideo, granted);
      expect(
        session.setBackground(false),
        granted,
        reason: 'shown at the start: nothing to send',
      );
      expect(session.setBackground(true), granted);
      expect(session.setBackground(true), granted);
      expect(session.background, granted);
      expect(session.setBackground(false), granted);
      await flush();
      expect(_sent(socket), granted ? [true, false] : isEmpty);
      await session.close();
      expect(session.setBackground(true), isFalse, reason: 'closed');
      await media.events.close();
    }
  });

  test(
    'After its socket was replaced, a session that hid says where it stands again',
    () async {
      SessionSocket? dialed;
      final first = SessionSocket(), media = SessionMedia();
      final session = RemoteSession(
        connection: BrokerConnection(
          sessionId: 'background-session',
          webSocketUrl: Uri.parse(
            'wss://fixture.invalid/api/v1/sessions/background-session',
          ),
          sessionResume: true,
          resumeGraceMs: 15000,
          backgroundVideo: true,
        ),
        token: 'fixture-only-token',
        media: media,
        dialer: (_, _) async => first,
        resumeDialer: (_, _, _) async => dialed = SessionSocket(),
      );
      unawaited(session.start());
      first.incoming.add(jsonEncode(info));
      await flush();
      first.incoming.add(packet());
      await flush();
      first.incoming.add(
        jsonEncode({
          'type': 'session.resume.token',
          'sessionId': 'background-session',
          'token': 'A' * 43,
          'graceMs': 15000,
        }),
      );
      await flush();
      expect(session.setBackground(true), isTrue);
      await flush();
      expect(_sent(first), [true]);
      unawaited(first.incoming.close()); // lost without a close frame
      await flush();
      await flush();
      expect(dialed, isNotNull, reason: '${session.diagnosticCode}');
      expect(
        _sent(dialed!),
        [true],
        reason: 'the Host may never have had it',
      );
      await session.close();
      await media.events.close();
    },
  );
}
