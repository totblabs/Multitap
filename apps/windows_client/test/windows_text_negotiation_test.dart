import 'dart:convert';

import 'package:flutter_test/flutter_test.dart';
import 'package:multitap_client/data/broker_api.dart';

import 'power_api_test.dart' show ApiClient;
import 'track_stream_test.dart' as tracks;

final _origin = Uri.parse('https://cloud.example.com');
Map<String, dynamic> _inventory({bool capable = true}) => {
  'desktops': [
    {
      'id': 'vm',
      'name': 'VM',
      'kind': 'vm',
      'description': '',
      'os': 'Windows',
      'status': 'online',
      'canConnect': true,
      'host': {
        'connected': true,
        'capabilities': {if (capable) 'textInputProtocol': 'text-input-v1'},
      },
    },
  ],
};
Map<String, dynamic> _connection({bool grant = false}) => {
  'sessionId': 'text-test',
  'webSocketUrl': 'wss://cloud.example.com/api/v1/sessions/text-test',
  if (grant) 'textInputProtocol': 'text-input-v1',
};

void main() {
  for (final request in [false, true]) {
    test(
      'HTTP requestTextInput=$request requires the current exact inventory grant',
      () async {
        final client = ApiClient()
          ..responses.addAll([
            (200, _inventory()),
            (200, _connection(grant: request)),
          ]);
        final api = HttpBrokerApi(
          origin: _origin,
          client: client,
          requestTextInput: request,
        );
        addTearDown(api.close);
        await api.desktops('synthetic-token');
        final connection = await api.connect('synthetic-token', 'vm');
        final body = jsonDecode(client.requests.last.body.toString()) as Map;
        expect(body['textInputProtocol'], request ? 'text-input-v1' : null);
        expect(connection.textInput, request);
      },
    );
  }
  for (final reason in [
    'old-host',
    'no-inventory',
    'different-token',
    'different-desktop',
  ]) {
    test(
      'Windows text opt-in does not invent capability for $reason',
      () async {
        final client = ApiClient()
          ..responses.addAll([
            if (reason != 'no-inventory')
              (200, _inventory(capable: reason != 'old-host')),
            (200, _connection()),
          ]);
        final api = HttpBrokerApi(
          origin: _origin,
          client: client,
          requestTextInput: true,
        );
        addTearDown(api.close);
        if (reason != 'no-inventory') await api.desktops('synthetic-token');
        final connection = await api.connect(
          reason == 'different-token' ? 'new-token' : 'synthetic-token',
          reason == 'different-desktop' ? 'other-vm' : 'vm',
        );
        expect(
          (jsonDecode(client.requests.last.body.toString()) as Map).containsKey(
            'textInputProtocol',
          ),
          isFalse,
        );
        expect(connection.textInput, isFalse);
      },
    );
  }
  test('Legacy400 fallback removes text opt-in and does not silently retain a grant', () async {
    final client = ApiClient()
      ..responses.addAll([
        (200, _inventory()),
        (
          400,
          {
            'error': {'code': 'unexpected_field'},
          },
        ),
        (200, _connection()),
      ]);
    final api = HttpBrokerApi(
      origin: _origin,
      client: client,
      requestTextInput: true,
    );
    addTearDown(api.close);
    await api.desktops('synthetic-token');
    final connection = await api.connect('synthetic-token', 'vm');
    expect(
      (jsonDecode(client.requests[1].body.toString())
          as Map)['textInputProtocol'],
      'text-input-v1',
    );
    expect(
      (jsonDecode(client.requests[2].body.toString()) as Map).containsKey(
        'textInputProtocol',
      ),
      isFalse,
    );
    expect(connection.textInput, isFalse);
  });
  test(
    'Independent outputs stay key-only even for a text-enabled Windows API',
    () async {
      final client = ApiClient()
        ..responses.addAll([(200, _inventory()), (200, tracks.response())]);
      final api = HttpBrokerApi(
        origin: _origin,
        client: client,
        requestTextInput: true,
      );
      addTearDown(api.close);
      await api.desktops('synthetic-token');
      final connection = await api.connectTracks(
        'synthetic-token',
        'vm',
        tracks.plan(),
        tracks.options,
      );
      expect(
        (jsonDecode(client.requests.last.body.toString()) as Map).containsKey(
          'textInputProtocol',
        ),
        isFalse,
      );
      expect(connection.textInput, isFalse);
      expect(
        () => BrokerConnection.fromJson(
          {...tracks.response(), 'textInputProtocol': 'text-input-v1'},
          _origin,
          requestedOptions: tracks.options,
          requestedTracks: tracks.plan(),
          requestedTextInput: true,
        ),
        throwsA(isA<BrokerException>()),
      );
    },
  );
}
