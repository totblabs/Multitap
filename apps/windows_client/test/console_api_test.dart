import 'dart:convert';
import 'dart:io';

import 'package:flutter/material.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:multitap_client/data/broker_api.dart';
import 'package:multitap_client/data/desktop_mapping.dart';
import 'package:multitap_client/models/desktop_machine.dart';
import 'package:multitap_client/models/vm_resources.dart';

import 'power_api_test.dart' show ApiClient;

const id = '01234567-89ab-cdef-8123-0123456789ab';
final origin = Uri.parse('https://fixture.invalid:8443');
Map<String, dynamic> grantJson() => {
  'id': id,
  'ticket': 'fixture-one-use-ticket',
  'relayPath': '/relay/console/$id',
  'expiresAt': DateTime.now()
      .toUtc()
      .add(const Duration(minutes: 1))
      .toIso8601String(),
};
Map<String, dynamic> desktopJson() => {
  'id': 'pve-149',
  'kind': 'vm',
  'name': 'Fixture',
  'description': '',
  'os': 'Windows',
  'status': 'online',
  'canConnect': false,
  'consoleRelayProtocol': 'rfb-none-v1',
};

void main() {
  test('only explicitly advertised real VM console capability is mapped and every clone preserves it', () {
    final dto = BrokerDesktop.fromJson(desktopJson());
    expect(dto.emergencyConsoleSupported, isTrue);
    var machine = machineFromBroker(dto);
    expect(machine.emergencyConsoleSupported, isTrue);
    final clones = [
      machine.withPower(
        state: 'stopped',
        checkedAt: null,
        stale: false,
        busy: false,
      ),
      machine.withDisplay(const VirtualDisplayState()),
      machine.withResources(const VmResources()),
      machine.withOrganize(DesktopOrganize.none),
      machine.withHostUpdate(HostUpdateState.absent),
    ];
    expect(clones.every((m) => m.emergencyConsoleSupported), isTrue);
    for (final patch in [
      {'consoleRelayProtocol': null},
      {'consoleRelayProtocol': 'future'},
      {'kind': 'pc'},
      {'kind': 'future'},
      {'id': 'pve-0'},
      {'id': 'pve-10'},
      {'id': 'pve-0149'},
      {'id': 'pc-149'},
      {'id': 'pve-149/other'},
    ]) {
      final value = BrokerDesktop.fromJson({...desktopJson(), ...patch});
      expect(value.emergencyConsoleSupported, isFalse, reason: '$patch');
      expect(
        machineFromBroker(value).emergencyConsoleSupported,
        isFalse,
        reason: '$patch',
      );
    }
    expect(
      const DesktopMachine(
        id: 'pc-149',
        name: '',
        description: '',
        kind: MachineKind.vm,
        status: MachineStatus.awaitingHost,
        specification: '',
        accent: Colors.amber,
        consoleRelayProtocol: 'rfb-none-v1',
      ).emergencyConsoleSupported,
      isFalse,
    );
  });

  test('grant JSON rejects foreign routes, invalid ticket/identity, expiry and non-origin configuration', () {
    expect(
      BrokerConsoleGrant.fromJson(grantJson(), origin).relay.toString(),
      'wss://fixture.invalid:8443/relay/console/$id',
    );
    for (final patch in [
      {'id': '../other'},
      {'ticket': ''},
      {'ticket': 'x' * 513},
      {'ticket': 'ticket\r\nsecret'},
      {'relayPath': '//evil.invalid/relay/console/$id'},
      {'relayPath': '/relay/console/$id?ticket=x'},
      {'relayPath': 'wss://evil.invalid/relay/console/$id'},
      {'expiresAt': '2020-01-01T00:00:00Z'},
      {'expiresAt': '2035-01-01T00:00:00'},
    ]) {
      expect(
        () => BrokerConsoleGrant.fromJson({...grantJson(), ...patch}, origin),
        throwsA(isA<BrokerException>()),
      );
    }
    for (final value in [
      'http://fixture.invalid',
      'https://user@fixture.invalid',
      'https://fixture.invalid/path',
      'https://fixture.invalid?token=x',
      'https://fixture.invalid#fragment',
    ]) {
      expect(
        () => BrokerConsoleGrant.fromJson(grantJson(), Uri.parse(value)),
        throwsA(isA<BrokerException>()),
      );
    }
  });

  test('optional console API sends exact authenticated empty POST and exact DELETE without redirects', () async {
    final client = ApiClient()
      ..responses.addAll([
        (201, {'consoleSession': grantJson()}),
        (200, {'released': true, 'id': id}),
      ]);
    final api = HttpBrokerApi(origin: origin, client: client);
    addTearDown(api.close);
    expect(api, isA<EmergencyConsoleBrokerApi>());
    final grant = await api.createConsole('fixture-token', 'pve-149');
    await api.releaseConsole('fixture-token', 'pve-149', grant.id);
    expect(client.requests.map((r) => r.method), ['POST', 'DELETE']);
    expect(client.requests.map((r) => r.uri.path), [
      '/api/v1/desktops/pve-149/console-sessions',
      '/api/v1/desktops/pve-149/console-sessions/$id',
    ]);
    expect(
      jsonDecode(client.requests.first.body.toString()),
      <String, Object?>{},
    );
    for (final request in client.requests) {
      expect(request.followRedirects, isFalse);
      expect(
        request.headers.values[HttpHeaders.authorizationHeader],
        'Bearer fixture-token',
      );
      expect(request.uri.hasQuery, isFalse);
    }
    for (final desktop in ['pc-149', 'pve-149/other', '../pve-149']) {
      await expectLater(
        api.createConsole('fixture-token', desktop),
        throwsA(isA<BrokerException>()),
      );
    }
    await expectLater(
      api.releaseConsole('fixture-token', 'pve-149', '../other'),
      throwsA(isA<BrokerException>()),
    );
    expect(client.requests, hasLength(2));
  });

  test(
    'wrong release acknowledgement and redirected grant never become success',
    () async {
      for (final response in [
        (200, <String, dynamic>{'released': true, 'id': 'other'}),
        (302, <String, dynamic>{}),
      ]) {
        final client = ApiClient()..responses.add(response);
        final api = HttpBrokerApi(origin: origin, client: client);
        addTearDown(api.close);
        await expectLater(
          api.releaseConsole('fixture-token', 'pve-149', id),
          throwsA(isA<BrokerException>()),
        );
        expect(client.requests.single.followRedirects, isFalse);
      }
    },
  );
}
