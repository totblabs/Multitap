// Injected HTTP/socket/media boundaries only; no native Windows display call.
import 'dart:async';
import 'dart:convert';

import 'package:flutter_test/flutter_test.dart';
import 'package:multitap_client/data/broker_api.dart';
import 'package:multitap_client/data/initial_viewport.dart';
import 'package:multitap_client/data/remote_session.dart';
import 'package:multitap_client/models/stream_options.dart';

import 'power_api_test.dart' show ApiClient;
import 'remote_session_test.dart' show SessionSocket, SessionMedia, flush;

const _origin = 'https://fixture.invalid';
const _sessionId = 'b44907ce-827b-4519-ac28-2066015e4870';
Map<String, dynamic> _inventory({bool capable = true}) => {
  'desktops': [
    {
      'id': 'pve-149',
      'name': 'TESTER',
      'kind': 'vm',
      'description': '',
      'os': 'Windows',
      'status': 'ready',
      'canConnect': true,
      'host': {
        'connected': true,
        'capabilities': {
          'service': true,
          'streaming': true,
          if (capable) 'displayScaleProtocol': 'display-scale-v1',
        },
      },
    },
  ],
};
Map<String, dynamic> _connected({
  bool granted = true,
  InitialViewport? viewport,
}) => {
  'sessionId': _sessionId,
  'webSocketUrl': 'wss://fixture.invalid/api/v1/sessions/$_sessionId',
  'mode': 'stream',
  'options': const StreamOptions().toJson(),
  if (granted) 'displayScaleProtocol': 'display-scale-v1',
  if (viewport != null) ...{
    'transport': 'media-epoch-v1',
    'resizeProtocol': 'epoch-v1',
    'viewportProvisioning': true,
    'initialViewportProtocol': 'epoch-v1',
    'initialViewport': {
      'protocol': 'epoch-v1',
      'sessionId': _sessionId,
      'requestId': '0b662cd7-b7fc-4ac9-9efb-e5b5d77fbc23',
      'resizeId': '9d91d74e-3847-40c2-8d2c-d635ffb72a3b',
      'epoch': 1,
      ...viewport.toJson(),
    },
  },
};

void main() {
  test('runtime grant is separate from optional initial scale and exact', () {
    final data = _connected();
    final granted = BrokerConnection.fromJson(
      data,
      Uri.parse(_origin),
      requestedDisplayScaleControl: true,
    );
    expect(granted.displayScaleControl, isTrue);
    expect(granted.displayScale, isNull);
    for (final malformed in [
      data,
      {...data, 'displayScale': null},
      {...data, 'displayScale': 100},
      {...data, 'displayScaleProtocol': 'display-scale-v2'},
    ]) {
      expect(
        () => BrokerConnection.fromJson(
          malformed,
          Uri.parse(_origin),
          requestedDisplayScaleControl: !identical(malformed, data),
        ),
        throwsA(isA<BrokerException>()),
      );
    }
  });

  test('only opted-in Windows windows request neutral control from capable inventory', () async {
    for (final enabled in [false, true]) {
      for (final capable in [false, true]) {
        final client = ApiClient()
          ..responses.add((200, _inventory(capable: capable)))
          ..responses.add((200, _connected(granted: enabled && capable)));
        final api = HttpBrokerApi(
          origin: Uri.parse(_origin),
          client: client,
          requestDisplayScaleControl: enabled,
        );
        await api.desktops('fixture');
        final connection = await api.connect('fixture', 'pve-149');
        final body = jsonDecode(client.requests.last.body.toString()) as Map;
        expect(body.containsKey('displayScaleProtocol'), enabled && capable);
        expect(body.containsKey('displayScale'), isFalse);
        expect(connection.displayScaleControl, enabled && capable);
        expect(connection.displayScale, isNull);
      }
    }
  });

  test(
    'old broker exact preflight refusal retries once and preserves viewport',
    () async {
      final viewport = InitialViewport(width: 1200, height: 800, fps: 60);
      final client = ApiClient()
        ..responses.add((200, _inventory()))
        ..responses.add((
          400,
          {
            'error': {'code': 'invalid_display_scale'},
          },
        ))
        ..responses.add((200, _connected(granted: false, viewport: viewport)));
      final api = HttpBrokerApi(
        origin: Uri.parse(_origin),
        client: client,
        requestDisplayScaleControl: true,
      );
      await api.desktops('fixture');
      final result = await api.connect(
        'fixture',
        'pve-149',
        initialViewport: viewport,
      );
      final posts = client.requests.where((r) => r.method == 'POST').toList();
      expect(posts, hasLength(2));
      final first = jsonDecode(posts.first.body.toString()) as Map;
      final last = jsonDecode(posts.last.body.toString()) as Map;
      expect(first.remove('displayScaleProtocol'), 'display-scale-v1');
      expect(
        first,
        last,
        reason: 'Only the marker is removed; viewport/options stay exact.',
      );
      expect(last.containsKey('displayScale'), isFalse);
      expect(result.displayScaleControl, isFalse);
      expect(result.initialViewport?.viewport.width, 1200);
    },
  );

  test('neutral control never retries uncertain/auth failures or repeats a refusal', () async {
    for (final failure in [
      (401, 'invalid_display_scale'),
      (503, 'invalid_display_scale'),
      (400, 'invalid_input'),
    ]) {
      final client = ApiClient()
        ..responses.add((200, _inventory()))
        ..responses.add((
          failure.$1,
          {
            'error': {'code': failure.$2},
          },
        ));
      final api = HttpBrokerApi(
        origin: Uri.parse(_origin),
        client: client,
        requestDisplayScaleControl: true,
      );
      await api.desktops('fixture');
      await expectLater(
        api.connect('fixture', 'pve-149'),
        throwsA(isA<BrokerException>()),
      );
      expect(client.requests.where((r) => r.method == 'POST'), hasLength(1));
    }
    final client = ApiClient()
      ..responses.add((200, _inventory()))
      ..responses.add((
        400,
        {
          'error': {'code': 'invalid_display_scale'},
        },
      ))
      ..responses.add((
        400,
        {
          'error': {'code': 'invalid_display_scale'},
        },
      ));
    final api = HttpBrokerApi(
      origin: Uri.parse(_origin),
      client: client,
      requestDisplayScaleControl: true,
    );
    await api.desktops('fixture');
    await expectLater(
      api.connect('fixture', 'pve-149'),
      throwsA(isA<BrokerException>()),
    );
    expect(client.requests.where((r) => r.method == 'POST'), hasLength(2));
  });

  test('neutral session sends no initial DPI request then changes scale on same socket', () async {
    final socket = SessionSocket();
    final session = RemoteSession(
      connection: BrokerConnection.fromJson(
        _connected(),
        Uri.parse(_origin),
        requestedDisplayScaleControl: true,
      ),
      token: 'fixture-only',
      media: SessionMedia(),
      dialer: (_, _) async => socket,
    );
    unawaited(session.start());
    await flush();
    expect(session.displayScaleSupported, isTrue);
    expect(session.displayScaleWanted, isNull);
    expect(socket.sent.where((m) => m['type'] == 'display.scale'), isEmpty);
    final observed = {
      'type': 'display.scale.applied',
      'sessionId': _sessionId,
      'protocol': 'display-scale-v1',
      'ok': true,
      'scale': 125,
      'maximum': 200,
      'code': 0,
    };
    socket.incoming.add(
      jsonEncode({...observed, 'sessionId': 'retired-session'}),
    );
    await flush();
    expect(session.displayScaleApplied, isNull);
    socket.incoming.add(jsonEncode(observed));
    await flush();
    expect(session.displayScaleApplied?.scale, 125);
    expect(
      session.displayScaleWanted,
      isNull,
      reason:
          'An observation must not turn automatic state into a fixed request.',
    );
    expect(socket.sent.where((m) => m['type'] == 'display.scale'), isEmpty);
    for (final scale in [125, 150, 200, 100]) {
      expect(session.requestDisplayScale(scale), isTrue);
      expect(session.requestDisplayScale(scale), isFalse);
      expect(session.connection.sessionId, _sessionId);
      expect(session.closed, isFalse);
    }
    expect(socket.sent.where((m) => m['type'] == 'display.scale').toList(), [
      for (final scale in [125, 150, 200, 100])
        {'type': 'display.scale', 'scale': scale},
    ]);
    await session.close();
  });
}
