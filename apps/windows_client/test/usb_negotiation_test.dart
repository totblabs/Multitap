// usb-v1 on the client: the broker grant (only after a request, stream
// sessions only, independent outputs only with usb-tracks-v1, only the
// session's own URL), the connect request only
// toward an advertising inventory, and the stream session starting and ending
// the USB channel. No device, Host or OS involved.
import 'dart:async';
import 'dart:convert';
import 'dart:io';

import 'package:flutter_test/flutter_test.dart';
import 'package:multitap_client/data/broker_api.dart';
import 'package:multitap_client/data/remote_session.dart';
import 'package:multitap_client/data/usb_redirection.dart';
import 'package:multitap_client/models/stream_options.dart';

import 'power_api_test.dart' show ApiClient;
import 'remote_session_test.dart'
    show SessionSocket, SessionMedia, flush, info, packet;
import 'track_stream_test.dart' as track;

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
  'sessionId': 'usb-session',
  'webSocketUrl': 'wss://fixture.invalid/api/v1/sessions/usb-session',
  'mode': 'stream',
  'options': const StreamOptions().toJson(),
  if (granted) ...{
    'usbProtocol': 'usb-v1',
    'usbWebSocketUrl': 'wss://fixture.invalid/api/v1/sessions/usb-session/usb',
  },
};

class _NoBackend extends UsbBackend {
  @override
  Future<UsbEnvironment> environment() async =>
      const UsbEnvironment(UsbDriverState.ready);
  @override
  Future<List<UsbLocalDevice>> list() async => const [];
  @override
  Future<UsbCapture> capture(UsbLocalDevice device) =>
      throw const UsbBackendException('usb_capture_failed');
  @override
  Future<void> installDriver() async {}
}

void main() {
  test('the broker grant is exact and only follows a request', () {
    final origin = Uri.parse(_origin);
    final granted = BrokerConnection.fromJson(
      _connected(granted: true),
      origin,
      requestedUsb: true,
    );
    expect(
      granted.usbWebSocketUrl.toString(),
      'wss://fixture.invalid/api/v1/sessions/usb-session/usb',
    );
    expect(BrokerConnection.fromJson(_connected(), origin).usbWebSocketUrl, isNull);
    expect(
      BrokerConnection.fromJson(_connected(), origin, requestedUsb: true)
          .usbWebSocketUrl,
      isNull,
      reason: 'requested from a released Host: an ordinary session',
    );
    final url = 'wss://fixture.invalid/api/v1/sessions/usb-session/usb';
    for (final (data, requested) in <(Map<String, dynamic>, bool)>[
      (_connected(granted: true), false),
      ({..._connected(), 'usbProtocol': 'usb-v2', 'usbWebSocketUrl': url}, true),
      ({..._connected(), 'usbProtocol': 'usb-v1'}, true),
      ({..._connected(), 'usbWebSocketUrl': url}, true),
      ({..._connected(), 'usbProtocol': 'usb-v1', 'usbWebSocketUrl': 5}, true),
      // Only this session's URL on the configured server.
      ({..._connected(granted: true), 'usbWebSocketUrl': 'wss://evil.invalid/api/v1/sessions/usb-session/usb'}, true),
      ({..._connected(granted: true), 'usbWebSocketUrl': 'wss://fixture.invalid/api/v1/sessions/other/usb'}, true),
      ({..._connected(granted: true), 'usbWebSocketUrl': 'wss://fixture.invalid/api/v1/sessions/usb-session/clipboard'}, true),
      ({..._connected(granted: true), 'usbWebSocketUrl': 'wss://fixture.invalid:8443/api/v1/sessions/usb-session/usb'}, true),
      ({..._connected(granted: true), 'usbWebSocketUrl': 'ws://fixture.invalid/api/v1/sessions/usb-session/usb'}, true),
      ({..._connected(granted: true), 'usbWebSocketUrl': 'wss://u:p@fixture.invalid/api/v1/sessions/usb-session/usb'}, true),
      ({..._connected(granted: true), 'usbWebSocketUrl': '$url?x=1'}, true),
    ]) {
      expect(
        () => BrokerConnection.fromJson(data, origin, requestedUsb: requested),
        throwsA(isA<BrokerException>()),
        reason: '$data',
      );
    }
    expect(
      BrokerDesktop.fromJson(_desktop({'usbProtocol': 'usb-v1'})).usb,
      isTrue,
    );
    expect(BrokerDesktop.fromJson(_desktop({})).usb, isFalse);
    expect(
      BrokerDesktop.fromJson(_desktop({'usbProtocol': 'usb-v2'})).usb,
      isFalse,
    );
  });

  test('HTTP: requested only by a client that lends USB, toward an advertising inventory', () async {
    Future<(Map, BrokerConnection)> run(
      Map<String, Object> caps,
      Map<String, dynamic> reply, {
      bool requestUsb = true,
    }) async {
      final client = ApiClient()
        ..responses.add((200, {'desktops': [_desktop(caps)]}))
        ..responses.add((200, reply));
      final api = HttpBrokerApi(
        origin: Uri.parse(_origin),
        client: client,
        requestUsb: requestUsb,
      );
      await api.desktops('fixture');
      final connection = await api.connect('fixture', 'pve-149');
      return (jsonDecode(client.requests.last.body.toString()) as Map, connection);
    }

    const released = {'service': true, 'streaming': true};
    final legacy = await run(released, _connected());
    expect(legacy.$1.containsKey('usbProtocol'), isFalse);
    expect(legacy.$2.usbWebSocketUrl, isNull);
    const capable = {...released, 'usbProtocol': 'usb-v1'};
    final usb = await run(capable, _connected(granted: true));
    expect(usb.$1['usbProtocol'], 'usb-v1');
    expect(usb.$2.usbWebSocketUrl, isNotNull);
    // Not granted by the administrator: an ordinary stream.
    final denied = await run(capable, _connected());
    expect(denied.$1['usbProtocol'], 'usb-v1');
    expect(denied.$2.usbWebSocketUrl, isNull);
    // A client that does not lend devices never asks.
    final plain = await run(capable, _connected(), requestUsb: false);
    expect(plain.$1.containsKey('usbProtocol'), isFalse);
    // A released broker's preflight refusal: retried without the field.
    final client = ApiClient()
      ..responses.add((200, {'desktops': [_desktop(capable)]}))
      ..responses.add((400, {'error': {'code': 'unexpected_field'}}))
      ..responses.add((200, _connected()));
    final api = HttpBrokerApi(origin: Uri.parse(_origin), client: client, requestUsb: true);
    await api.desktops('fixture');
    final retried = await api.connect('fixture', 'pve-149');
    final body = jsonDecode(client.requests.last.body.toString()) as Map;
    expect(body.containsKey('usbProtocol'), isFalse);
    expect(retried.usbWebSocketUrl, isNull);
  });

  group('independent outputs (모든 모니터)', () {
    const trackUrl =
        'wss://cloud.example.com/api/v1/sessions/track-session/usb';
    final trackOrigin = Uri.parse('https://cloud.example.com');
    Map<String, dynamic> reply({bool granted = true, bool forwarded = true}) => {
      ...track.response(),
      if (granted) ...{'usbProtocol': 'usb-v1', 'usbWebSocketUrl': trackUrl},
      if (forwarded) 'usbTracksProtocol': 'usb-tracks-v1',
    };
    BrokerConnection parse(Map<String, dynamic> data, {bool requested = true}) =>
        BrokerConnection.fromJson(
          data,
          trackOrigin,
          requestedOptions: track.options,
          requestedTracks: track.plan(),
          requestedUsb: requested,
        );

    test('the grant is used only where the broker passes it on to the Host', () {
      expect(parse(reply()).usbWebSocketUrl.toString(), trackUrl);
      // Older brokers grant usb-v1 here but never tell the Host: the
      // session goes on without USB instead of a channel nobody answers.
      expect(parse(reply(forwarded: false)).usbWebSocketUrl, isNull);
      expect(parse(reply(granted: false, forwarded: false)).usbWebSocketUrl, isNull);
      for (final (data, requested) in <(Map<String, dynamic>, bool)>[
        (reply(), false),
        (reply(granted: false), true),
        ({...reply(), 'usbTracksProtocol': 'usb-tracks-v2'}, true),
        ({...reply(), 'usbWebSocketUrl': 'wss://cloud.example.com/api/v1/sessions/other/usb'}, true),
      ]) {
        expect(
          () => parse(data, requested: requested),
          throwsA(isA<BrokerException>()),
          reason: '$data',
        );
      }
      // An ordinary session never carries the independent-outputs marker.
      expect(
        () => BrokerConnection.fromJson(
          {..._connected(granted: true), 'usbTracksProtocol': 'usb-tracks-v1'},
          Uri.parse(_origin),
          requestedUsb: true,
        ),
        throwsA(isA<BrokerException>()),
      );
    });

    test('HTTP: both track connects ask for usb-v1 toward an advertising inventory', () async {
      for (final profile in [false, true]) {
        for (final (caps, forwarded) in <(Map<String, Object>, bool)>[
          (const {}, false),
          (const {'usbProtocol': 'usb-v1'}, true),
          (const {'usbProtocol': 'usb-v1'}, false),
        ]) {
          final asked = caps.isNotEmpty;
          final client = ApiClient()
            ..responses.add((
              200,
              {
                'desktops': [
                  _desktop({'service': true, 'streaming': true, ...caps}),
                ],
              },
            ))
            ..responses.add((
              200,
              {
                ...reply(granted: asked, forwarded: asked && forwarded),
                if (profile) 'streamProfileTracksProtocol': 'group-v1',
              },
            ));
          final api = HttpBrokerApi(
            origin: trackOrigin,
            client: client,
            requestUsb: true,
          );
          await api.desktops('fixture');
          final connection = profile
              ? await api.connectProfileTracks(
                  'fixture', 'pve-149', track.plan(), track.options)
              : await api.connectTracks(
                  'fixture', 'pve-149', track.plan(), track.options);
          final body = jsonDecode(client.requests.last.body.toString()) as Map;
          final reason = 'profile=$profile caps=$caps forwarded=$forwarded';
          expect(body['usbProtocol'], asked ? 'usb-v1' : isNull, reason: reason);
          expect(body.containsKey('usbTracksProtocol'), isFalse, reason: reason);
          expect(
            connection.usbWebSocketUrl?.toString(),
            asked && forwarded ? trackUrl : isNull,
            reason: reason,
          );
          api.close();
        }
      }
    });

    test('a granted dual session starts USB with its first pictures', () async {
      final socket = SessionSocket(), media = track.TrackMedia();
      final created = <UsbSession>[];
      final session = RemoteSession(
        connection: parse(reply()),
        token: 'not-persisted',
        media: media,
        dialer: (_, _) async => socket,
        usbFactory: (connection, token) {
          final usb = UsbSession(
            url: connection.usbWebSocketUrl!,
            token: token,
            sessionId: connection.sessionId,
            backend: _NoBackend(),
            dialer: (url, token) async => _UsbSocket(),
          );
          created.add(usb);
          return usb;
        },
      );
      final running = session.start();
      await flush();
      socket.incoming.add(jsonEncode(track.info()));
      await flush();
      socket.incoming.add(track.packet(0));
      socket.incoming.add(track.packet(1));
      await flush();
      expect(session.usb, isNull, reason: 'not before both outputs show');
      media.currentStats = track.trackStats();
      await Future<void>.delayed(const Duration(milliseconds: 1050));
      expect(session.independentOutputs, isTrue);
      expect(session.usb, isNotNull);
      expect(created, hasLength(1));
      await session.close();
      await running;
      expect(session.usb!.closed, isTrue);
      await media.events.close();
    });
  });

  group('session', () {
    Future<(RemoteSession, SessionSocket, SessionMedia, List<UsbSession>)> open({
      bool granted = true,
    }) async {
      final socket = SessionSocket(), media = SessionMedia();
      final created = <UsbSession>[];
      final session = RemoteSession(
        connection: BrokerConnection.fromJson(
          _connected(granted: granted),
          Uri.parse(_origin),
          requestedUsb: granted,
        ),
        token: 'not-persisted',
        media: media,
        dialer: (_, _) async => socket,
        usbFactory: (connection, token) {
          final usb = UsbSession(
            url: connection.usbWebSocketUrl!,
            token: token,
            sessionId: connection.sessionId,
            backend: _NoBackend(),
            dialer: (url, token) async => _UsbSocket(),
          );
          created.add(usb);
          return usb;
        },
      );
      unawaited(session.start());
      await flush();
      return (session, socket, media, created);
    }

    test('USB starts with the first picture, never before, and ends with the session', () async {
      final (session, socket, media, created) = await open();
      expect(session.usb, isNull);
      socket.incoming.add(jsonEncode(info));
      await flush();
      expect(session.usb, isNull, reason: 'not before the picture');
      socket.incoming.add(packet());
      await flush();
      expect(session.usb, isNotNull);
      expect(created, hasLength(1));
      await session.close();
      expect(session.usb!.closed, isTrue);
      await media.events.close();
    });

    test('an ungranted session has no USB', () async {
      final (session, socket, media, created) = await open(granted: false);
      socket.incoming.add(jsonEncode(info));
      await flush();
      socket.incoming.add(packet());
      await flush();
      expect(session.usb, isNull);
      expect(created, isEmpty);
      await session.close();
      await media.events.close();
    });
  });
}

class _UsbSocket extends Fake implements WebSocket {
  final incoming = StreamController<dynamic>();
  @override
  Duration? pingInterval;
  @override
  StreamSubscription<dynamic> listen(
    void Function(dynamic)? onData, {
    Function? onError,
    void Function()? onDone,
    bool? cancelOnError,
  }) => incoming.stream.listen(onData, onError: onError, onDone: onDone, cancelOnError: cancelOnError);
  @override
  void add(dynamic data) {}
  @override
  Future<void> close([int? code, String? reason]) async {
    unawaited(incoming.close());
  }
}
