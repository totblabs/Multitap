// Exact request/response negotiation with an in-memory HTTP fixture.
// No network, native decoder, Host or real streaming readiness is exercised.
import 'dart:convert';

import 'package:flutter_test/flutter_test.dart';
import 'package:multitap_client/data/broker_api.dart';
import 'package:multitap_client/data/stream_observability.dart';
import 'package:multitap_client/models/stream_options.dart';

import 'power_api_test.dart' show ApiClient;
import 'track_stream_test.dart' as tracks;

final _origin = Uri.parse('https://cloud.example.com');
const _desktopId = 'pve-149';

Map<String, dynamic> _desktop({Object? marker = streamObservabilityProtocol}) =>
    {
      'id': _desktopId,
      'name': 'Synthetic fixture',
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
          'observabilityProtocol': ?marker,
        },
      },
    };

Map<String, dynamic> _inventory({
  Object? marker = streamObservabilityProtocol,
}) => {
  'desktops': [_desktop(marker: marker)],
};

Map<String, dynamic> _connected({Object? marker, Object? path}) => {
  'sessionId': 'observability-session',
  'webSocketUrl':
      'wss://cloud.example.com/api/v1/sessions/observability-session',
  'mode': 'stream',
  'options': const StreamOptions().toJson(),
  'observabilityProtocol': ?marker,
  'observabilityPath': ?path,
};

List<Map<String, dynamic>> _connectBodies(ApiClient client) => [
  for (final request in client.requests)
    if (request.uri.path.endsWith('/connect'))
      jsonDecode(request.body.toString()) as Map<String, dynamic>,
];

HttpBrokerApi _api(ApiClient client, {bool Function()? requested}) {
  final api = HttpBrokerApi(
    origin: _origin,
    client: client,
    requestObservability: requested,
  );
  addTearDown(api.close);
  return api;
}

void main() {
  setUp(() => StreamObservability.enabled = false);
  tearDown(() => StreamObservability.enabled = false);

  test(
    'default OFF preserves connect body despite an advertising Host',
    () async {
      final client = ApiClient()
        ..responses.addAll([(200, _inventory()), (200, _connected())]);
      final api = _api(client);
      expect((await api.desktops('account-a')).single.observability, isTrue);
      final connection = await api.connect('account-a', _desktopId);
      expect(_connectBodies(client).single, {
        'mode': 'stream',
        'resizeProtocol': 'epoch-v1',
        'clipboardProtocol': 'clipboard-sync-v1',
        'options': const StreamOptions().toJson(),
      });
      expect(connection.observability, isFalse);
      expect(connection.observabilityPath, isEmpty);
    },
  );

  test(
    'explicit ON requests supported marker and accepts bounded path labels',
    () async {
      final client = ApiClient()
        ..responses.addAll([
          (200, _inventory()),
          (
            200,
            _connected(
              marker: streamObservabilityProtocol,
              path: {
                'clientTls': 'proxy',
                'hostTls': 'direct',
                'rawUrl': 'https://must-not-be-exported.invalid',
              },
            ),
          ),
        ]);
      final api = _api(client, requested: () => true);
      await api.desktops('account-a');
      final connection = await api.connect('account-a', _desktopId);
      expect(
        _connectBodies(client).single['observabilityProtocol'],
        streamObservabilityProtocol,
      );
      expect(connection.observability, isTrue);
      expect(connection.observabilityPath, {
        'clientTls': 'proxy',
        'hostTls': 'direct',
      });
    },
  );

  for (final marker in <Object?>[null, 'stream-observability-v2', true, 1]) {
    test(
      'ON keeps old wire for unsupported inventory marker $marker',
      () async {
        final client = ApiClient()
          ..responses.addAll([
            (200, _inventory(marker: marker)),
            (200, _connected()),
          ]);
        final api = _api(client, requested: () => true);
        expect((await api.desktops('account-a')).single.observability, isFalse);
        final connection = await api.connect('account-a', _desktopId);
        expect(
          _connectBodies(client).single.containsKey('observabilityProtocol'),
          isFalse,
        );
        expect(connection.observability, isFalse);
      },
    );
  }

  test('ON cannot infer Host support without this token inventory', () async {
    final client = ApiClient()..responses.add((200, _connected()));
    await _api(client, requested: () => true).connect('account-a', _desktopId);
    expect(
      _connectBodies(client).single.containsKey('observabilityProtocol'),
      isFalse,
    );
  });

  test(
    'unrequested response marker cannot grant observations or path authority',
    () {
      final connection = BrokerConnection.fromJson(
        _connected(
          marker: streamObservabilityProtocol,
          path: {'clientTls': 'proxy'},
        ),
        _origin,
      );
      expect(connection.observability, isFalse);
      expect(connection.observabilityPath, isEmpty);
      expect(connection.trackPlan, isNull);
      expect(connection.viewportProvisioning, isFalse);
    },
  );

  test(
    'requested marker may remain ungranted without failing the connection',
    () async {
      final client = ApiClient()
        ..responses.addAll([(200, _inventory()), (200, _connected())]);
      final api = _api(client, requested: () => true);
      await api.desktops('account-a');
      final connection = await api.connect('account-a', _desktopId);
      expect(
        _connectBodies(client).single['observabilityProtocol'],
        streamObservabilityProtocol,
      );
      expect(connection.observability, isFalse);
      expect(connection.observabilityPath, isEmpty);
    },
  );

  test(
    'another account token cannot reuse the preceding inventory capability',
    () async {
      final client = ApiClient()
        ..responses.addAll([
          (200, _inventory()),
          (200, _connected(marker: streamObservabilityProtocol)),
        ]);
      final api = _api(client, requested: () => true);
      await api.desktops('account-a');
      final connection = await api.connect('account-b', _desktopId);
      expect(
        _connectBodies(client).single.containsKey('observabilityProtocol'),
        isFalse,
      );
      expect(connection.observability, isFalse);
    },
  );

  test(
    'replacement inventory clears prior capability for both account tokens',
    () async {
      final client = ApiClient()
        ..responses.addAll([
          (200, _inventory()),
          (200, _inventory(marker: null)),
          (200, _connected()),
          (200, _connected()),
        ]);
      final api = _api(client, requested: () => true);
      await api.desktops('account-a');
      await api.desktops('account-b');
      await api.connect('account-b', _desktopId);
      await api.connect('account-a', _desktopId);
      expect(_connectBodies(client), hasLength(2));
      expect(
        _connectBodies(client)
            .every((body) => !body.containsKey('observabilityProtocol')),
        isTrue,
      );
    },
  );

  test(
    'logout invalidates capability even when an old token is presented again',
    () async {
      final client = ApiClient()
        ..responses.addAll([
          (200, _inventory()),
          (200, {'ok': true}),
          (200, _connected()),
        ]);
      final api = _api(client, requested: () => true);
      await api.desktops('account-a');
      await api.logout('account-a');
      await api.connect('account-a', _desktopId);
      expect(
        _connectBodies(client).single.containsKey('observabilityProtocol'),
        isFalse,
      );
    },
  );

  test('a failed opt-in callback leaves a usable legacy connection', () async {
    final client = ApiClient()
      ..responses.addAll([(200, _inventory()), (200, _connected())]);
    final api = _api(
      client,
      requested: () => throw StateError('fixture settings failure'),
    );
    await api.desktops('account-a');
    final connection = await api.connect('account-a', _desktopId);
    expect(
      _connectBodies(client).single.containsKey('observabilityProtocol'),
      isFalse,
    );
    expect(connection.observability, isFalse);
  });

  test(
    'exact old-broker preflight rejection retries without observation marker',
    () async {
      final client = ApiClient()
        ..responses.addAll([
          (200, _inventory()),
          (
            400,
            {
              'error': {'code': 'unexpected_field'},
            },
          ),
          (200, _connected(marker: streamObservabilityProtocol)),
        ]);
      final api = _api(client, requested: () => true);
      await api.desktops('account-a');
      final connection = await api.connect('account-a', _desktopId);
      final bodies = _connectBodies(client);
      expect(bodies, hasLength(2));
      expect(
        bodies.first['observabilityProtocol'],
        streamObservabilityProtocol,
      );
      expect(bodies.last.containsKey('observabilityProtocol'), isFalse);
      expect(bodies.last['options'], bodies.first['options']);
      expect(connection.observability, isFalse);
    },
  );

  for (final profile in [false, true]) {
    test(
      'independent tracks preserve explicit ${profile ? 'profile' : 'ordinary'} negotiation',
      () async {
        final response = tracks.response()
          ..['observabilityProtocol'] = streamObservabilityProtocol;
        if (profile) response['streamProfileTracksProtocol'] = 'group-v1';
        final client = ApiClient()
          ..responses.addAll([(200, _inventory()), (200, response)]);
        final api = _api(client, requested: () => true);
        await api.desktops('account-a');
        final connection = profile
            ? await api.connectProfileTracks(
                'account-a',
                _desktopId,
                tracks.plan(),
                tracks.options,
              )
            : await api.connectTracks(
                'account-a',
                _desktopId,
                tracks.plan(),
                tracks.options,
              );
        final body = _connectBodies(client).single;
        expect(body['observabilityProtocol'], streamObservabilityProtocol);
        expect(body['mediaProtocol'], 'tracks-v1');
        expect(body['tracks'], tracks.plan().toJson());
        expect(
          body['streamProfileTracksProtocol'],
          profile ? 'group-v1' : isNull,
        );
        expect(connection.observability, isTrue);
        expect(connection.profileTracks, profile);
        expect(connection.trackPlan!.outputs, hasLength(2));
      },
    );
  }

  test(
    'observation capability alone cannot mark an unavailable desktop ready',
    () {
      final raw = _desktop()
        ..['canConnect'] = false
        ..['status'] = 'unavailable';
      final desktop = BrokerDesktop.fromJson(raw);
      expect(desktop.observability, isTrue);
      expect(desktop.canConnect, isFalse);
    },
  );
}
