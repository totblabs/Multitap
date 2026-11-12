// touch-input-v1 on the client: frame validation and the exact wire shape,
// the broker grant (only after a request, ordinary stream sessions only), the
// connect request only toward an advertising inventory, and the session's
// sendTouch gate. No device, Host or OS input.
import 'dart:async';
import 'dart:convert';

import 'package:flutter_test/flutter_test.dart';
import 'package:multitap_client/data/broker_api.dart';
import 'package:multitap_client/data/remote_session.dart';
import 'package:multitap_client/data/stream_stats_report.dart';
import 'package:multitap_client/data/touch_input.dart';
import 'package:multitap_client/models/stream_options.dart';

import 'power_api_test.dart' show ApiClient;
import 'remote_session_test.dart'
    show SessionSocket, SessionMedia, flush, info, packet;

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
  'sessionId': 'touch-session',
  'webSocketUrl': 'wss://fixture.invalid/api/v1/sessions/touch-session',
  'mode': 'stream',
  'options': const StreamOptions().toJson(),
  if (granted) 'touchInputProtocol': 'touch-input-v1',
};
RemoteTouchContact _c(
  int id,
  RemoteTouchPhase phase, [
  double x = 0.5,
  double y = 0.5,
]) => RemoteTouchContact(id: id, phase: phase, x: x, y: y);

void main() {
  test('frames: exact wire shape, bounds and validation', () {
    expect(
      touchInputMessage([
        const RemoteTouchContact(
          id: 3,
          phase: RemoteTouchPhase.down,
          x: 0.25,
          y: 0.5,
          pressure: 0.6,
          width: 0.01,
          height: 0.012,
        ),
        _c(4, RemoteTouchPhase.move, 0.75, 0.5),
      ]),
      {
        'type': 'input',
        'event': 'touch',
        'contacts': [
          {
            'id': 3,
            'phase': 'down',
            'x': 0.25,
            'y': 0.5,
            'pressure': 0.6,
            'w': 0.01,
            'h': 0.012,
          },
          {'id': 4, 'phase': 'move', 'x': 0.75, 'y': 0.5},
        ],
      },
    );
    for (final phase in RemoteTouchPhase.values) {
      expect(touchInputMessage([_c(0, phase, 0, 1)]), isNotNull);
    }
    final ten = [
      for (var n = 0; n < touchInputMaxContacts; n++)
        _c(n, RemoteTouchPhase.move, n / 10, 1 - n / 10),
    ];
    expect(touchInputMessage(ten)?['contacts'], hasLength(10));
    expect(
      touchInputMessage([_c(touchInputMaxId, RemoteTouchPhase.down)]),
      isNotNull,
    );
    for (final bad in <List<RemoteTouchContact>>[
      [],
      [...ten, _c(10, RemoteTouchPhase.move)],
      [_c(1, RemoteTouchPhase.down), _c(1, RemoteTouchPhase.move)],
      [_c(-1, RemoteTouchPhase.down)],
      [_c(touchInputMaxId + 1, RemoteTouchPhase.down)],
      [_c(1, RemoteTouchPhase.down, -0.01)],
      [_c(1, RemoteTouchPhase.down, 0.5, 1.01)],
      [_c(1, RemoteTouchPhase.down, double.nan)],
      [_c(1, RemoteTouchPhase.down, double.infinity)],
      [
        const RemoteTouchContact(
          id: 1,
          phase: RemoteTouchPhase.down,
          x: 0.5,
          y: 0.5,
          pressure: 1.5,
        ),
      ],
      [
        const RemoteTouchContact(
          id: 1,
          phase: RemoteTouchPhase.down,
          x: 0.5,
          y: 0.5,
          width: 0.1,
        ),
      ],
      [
        const RemoteTouchContact(
          id: 1,
          phase: RemoteTouchPhase.down,
          x: 0.5,
          y: 0.5,
          width: 0,
          height: 0.1,
        ),
      ],
    ]) {
      expect(touchInputMessage(bad), isNull, reason: '$bad');
    }
    // Diagnostics carry counts only.
    expect(
      streamStatsFields.containsAll([
        'touchInput',
        'touchFrames',
        'touchContacts',
      ]),
      isTrue,
    );
  });

  test('the broker grant is exact and only follows a request', () {
    final origin = Uri.parse(_origin);
    expect(
      BrokerConnection.fromJson(
        _connected(granted: true),
        origin,
        requestedTouchInput: true,
      ).touchInput,
      isTrue,
    );
    expect(BrokerConnection.fromJson(_connected(), origin).touchInput, isFalse);
    expect(
      BrokerConnection.fromJson(
        _connected(),
        origin,
        requestedTouchInput: true,
      ).touchInput,
      isFalse,
      reason: 'requested from a released Host: an ordinary session',
    );
    for (final (data, requested) in [
      (_connected(granted: true), false),
      ({..._connected(), 'touchInputProtocol': 'touch-input-v2'}, true),
      ({..._connected(), 'touchInputProtocol': true}, true),
    ]) {
      expect(
        () => BrokerConnection.fromJson(
          data,
          origin,
          requestedTouchInput: requested,
        ),
        throwsA(isA<BrokerException>()),
      );
    }
    expect(
      BrokerDesktop.fromJson(_desktop({'touchInputProtocol': 'touch-input-v1'}))
          .touchInput,
      isTrue,
    );
    expect(BrokerDesktop.fromJson(_desktop({})).touchInput, isFalse);
    expect(
      BrokerDesktop.fromJson(_desktop({'touchInputProtocol': 'touch-input-v2'}))
          .touchInput,
      isFalse,
    );
  });

  test(
    'HTTP: requested only by a touch client from an advertising inventory',
    () async {
      Future<(Map, BrokerConnection)> run(
        Map<String, Object> caps,
        Map<String, dynamic> reply, {
        bool requestTouch = true,
      }) async {
        final client = ApiClient()
          ..responses.add((
            200,
            {
              'desktops': [_desktop(caps)],
            },
          ))
          ..responses.add((200, reply));
        final api = HttpBrokerApi(
          origin: Uri.parse(_origin),
          client: client,
          requestTouchInput: requestTouch,
        );
        await api.desktops('fixture');
        final connection = await api.connect('fixture', 'pve-149');
        return (
          jsonDecode(client.requests.last.body.toString()) as Map,
          connection,
        );
      }

      const released = {'service': true, 'streaming': true};
      final legacy = await run(released, _connected());
      expect(legacy.$1.containsKey('touchInputProtocol'), isFalse);
      expect(legacy.$2.touchInput, isFalse);
      const capable = {...released, 'touchInputProtocol': 'touch-input-v1'};
      final touch = await run(capable, _connected(granted: true));
      expect(touch.$1['touchInputProtocol'], 'touch-input-v1');
      expect(touch.$2.touchInput, isTrue);
      // A mouse-only client (Windows) never asks, even toward a capable Host.
      final mouse = await run(capable, _connected(), requestTouch: false);
      expect(mouse.$1.containsKey('touchInputProtocol'), isFalse);
      expect(mouse.$2.touchInput, isFalse);
      // A released broker's preflight refusal: retried without the field.
      final client = ApiClient()
        ..responses.add((
          200,
          {
            'desktops': [_desktop(capable)],
          },
        ))
        ..responses.add((
          400,
          {
            'error': {'code': 'unexpected_field'},
          },
        ))
        ..responses.add((200, _connected()));
      final api = HttpBrokerApi(
        origin: Uri.parse(_origin),
        client: client,
        requestTouchInput: true,
      );
      await api.desktops('fixture');
      final retried = await api.connect('fixture', 'pve-149');
      final body = jsonDecode(client.requests.last.body.toString()) as Map;
      expect(body.containsKey('touchInputProtocol'), isFalse);
      expect(retried.touchInput, isFalse);
    },
  );

  group('session', () {
    Future<(RemoteSession, SessionSocket, SessionMedia)> open({
      bool granted = true,
    }) async {
      final socket = SessionSocket(), media = SessionMedia();
      final session = RemoteSession(
        connection: BrokerConnection.fromJson(
          _connected(granted: granted),
          Uri.parse(_origin),
          requestedTouchInput: granted,
        ),
        token: 'not-persisted',
        media: media,
        dialer: (_, _) async => socket,
      );
      unawaited(session.start());
      await flush();
      return (session, socket, media);
    }

    List<Map<String, dynamic>> touches(SessionSocket socket) =>
        socket.sent.where((m) => m['event'] == 'touch').toList();

    test('frames are sent only once input is accepted, in order', () async {
      final (session, socket, media) = await open();
      expect(session.touchInput, isTrue);
      final press = [_c(7, RemoteTouchPhase.down, 0.25, 0.5)];
      expect(session.sendTouch(press), isFalse, reason: 'no picture yet');
      socket.incoming.add(jsonEncode(info));
      await flush();
      socket.incoming.add(packet());
      await flush();
      expect(session.sendTouch(press), isFalse, reason: 'not captured');
      await session.capture(true);
      expect(session.sendTouch(press), isTrue);
      expect(
        session.sendTouch([
          _c(7, RemoteTouchPhase.move, 0.3, 0.5),
          _c(8, RemoteTouchPhase.down, 0.7, 0.5),
        ]),
        isTrue,
      );
      expect(
        session.sendTouch([_c(7, RemoteTouchPhase.up, 2, 0.5)]),
        isFalse,
        reason: 'an invalid frame is never sent',
      );
      session.input({'type': 'input', 'event': 'move', 'x': 0.1, 'y': 0.1});
      expect(touches(socket), [
        touchInputMessage(press),
        touchInputMessage([
          _c(7, RemoteTouchPhase.move, 0.3, 0.5),
          _c(8, RemoteTouchPhase.down, 0.7, 0.5),
        ]),
      ]);
      expect(socket.sent.last['event'], 'move', reason: 'kept in input order');
      expect(
        touches(socket).every((m) => !m.containsKey('sample')),
        isTrue,
        reason: 'never sampled for latency',
      );
      // Capture loss resets remote input; nothing more is sent until retaken.
      await session.capture(false);
      expect(socket.sent.last['type'], 'input.reset');
      expect(session.sendTouch(press), isFalse);
      await session.close();
      expect(session.sendTouch(press), isFalse);
      expect(touches(socket), hasLength(2));
      await media.events.close();
    });

    test('an ungranted session never sends touch', () async {
      final (session, socket, media) = await open(granted: false);
      socket.incoming.add(jsonEncode(info));
      await flush();
      socket.incoming.add(packet());
      await flush();
      await session.capture(true);
      expect(session.touchInput, isFalse);
      expect(session.sendTouch([_c(1, RemoteTouchPhase.down)]), isFalse);
      expect(touches(socket), isEmpty);
      await session.close();
      await media.events.close();
    });
  });
}
