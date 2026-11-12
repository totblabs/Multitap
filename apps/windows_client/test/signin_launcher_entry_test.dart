import 'dart:async';
import 'dart:convert';
import 'dart:io';

import 'package:flutter/material.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:multitap_client/data/broker_api.dart';
import 'package:multitap_client/data/broker_transport.dart';
import 'package:multitap_client/data/desktop_mapping.dart';
import 'package:multitap_client/data/stream_display_entry.dart';
import 'package:multitap_client/models/desktop_machine.dart';
import 'package:multitap_client/models/session_cleanup.dart';
import 'package:multitap_client/models/stream_options.dart';
import 'package:multitap_client/ui/desktop_readiness.dart';

import 'signin_stream_contract_test.dart'
    show signinBindingJson, signinConnectionJson, signinSessionId;

Map<String, Object> offer(DateTime now) => {
  'protocol': signinStreamProtocol,
  'availabilityId': 'a' * 32,
  'expiresAtMs': now.add(const Duration(seconds: 5)).millisecondsSinceEpoch,
  'windowsState': 'no-user',
};

DesktopMachine machine({
  String id = 'pve-170',
  bool ordinary = false,
  bool online = true,
  bool pending = false,
  bool powerBusy = false,
  bool off = false,
  MachineKind kind = MachineKind.vm,
  bool approved = false,
  String power = 'running',
}) => DesktopMachine(
  id: id,
  name: 'Fixture',
  description: '',
  kind: kind,
  localPcApproved: approved,
  status: off
      ? MachineStatus.poweredOff
      : ordinary
      ? MachineStatus.ready
      : MachineStatus.awaitingHost,
  specification: 'Windows',
  accent: Colors.blue,
  hostOnline: online,
  interactiveSession: ordinary,
  powerBusy: powerBusy,
  powerState: power,
  powerStale: false,
  sessionCleanup: SessionCleanupState(pending: pending),
  signinAvailability: ordinary
      ? null
      : SigninAvailability.tryParse(offer(DateTime.now())),
);

void main() {
  test(
    'short-lived inventory offers reject malformed or expanded authority',
    () {
      final now = DateTime.now();
      expect(SigninAvailability.tryParse(offer(now))!.fresh(now), isTrue);
      for (final patch in [
        {'protocol': 'future'},
        {'availabilityId': 'A' * 32},
        {'expiresAtMs': 1.5},
        {'expiresAtMs': 8640000000000001},
        {'windowsState': 'desktop'},
        {'authorized': true},
      ]) {
        expect(SigninAvailability.tryParse({...offer(now), ...patch}), isNull);
      }
      expect(
        SigninAvailability.tryParse(offer(now))!
            .fresh(now.add(const Duration(seconds: 5))),
        isFalse,
      );
      expect(
        SigninAvailability.tryParse(offer(now))!
            .fresh(now.subtract(const Duration(seconds: 6))),
        isFalse,
      );
    },
  );

  test('login launcher opens only a fresh eligible Host, without fabricating ordinary readiness', () {
    final target = machine();
    expect(target.status, MachineStatus.awaitingHost);
    expect(target.signinReady, isTrue);
    expect(target.isReady, isTrue);
    expect(DesktopReadiness.of(target).canStream, isTrue);
    for (final blocked in [
      machine(online: false),
      machine(pending: true),
      machine(powerBusy: true),
      machine(off: true),
      machine(power: 'paused'),
      machine(kind: MachineKind.pc),
    ]) {
      expect(blocked.signinReady, isFalse);
      expect(blocked.isReady, isFalse);
    }
    expect(machine(kind: MachineKind.pc, approved: true).signinReady, isTrue);
  });

  test(
    'single-output login entry never calls ordinary display mutation',
    () async {
      final target = machine();
      final settled = await waitForStreamDisplayControl(
        target,
        refresh: () async => throw StateError('unexpected refresh'),
        current: () => true,
      );
      expect(settled, same(target));
      expect(
        await prepareStreamDisplayCount(
          target,
          1,
          (_) async => throw StateError('unexpected display mutation'),
        ),
        same(target),
      );
      await expectLater(
        prepareStreamDisplayCount(
          target,
          2,
          (_) async => throw StateError('unexpected display mutation'),
        ),
        throwsA(isA<BrokerException>()),
      );
    },
  );

  test('desktop promotion waits for ordinary readiness and preserves owner cancellation', () async {
    var reads = 0, waits = 0;
    final result = await waitForOrdinarySigninDesktop(
      refresh: () async => ++reads == 1 ? machine() : machine(ordinary: true),
      current: () => true,
      desktopId: 'pve-170',
      wait: (_) async {
        waits++;
      },
    );
    expect(result!.interactiveSession, isTrue);
    expect(reads, 2);
    expect(waits, 1);
    expect(
      await waitForOrdinarySigninDesktop(
        refresh: () async => throw StateError('retired'),
        current: () => false,
        desktopId: 'pve-170',
      ),
      isNull,
    );
    final pending = Completer<DesktopMachine>();
    var current = true;
    final late = waitForOrdinarySigninDesktop(
      refresh: () => pending.future,
      current: () => current,
      desktopId: 'pve-170',
    );
    current = false;
    pending.complete(machine(ordinary: true));
    expect(await late, isNull);
    await expectLater(
      waitForOrdinarySigninDesktop(
        refresh: () async => machine(id: 'foreign', ordinary: true),
        current: () => true,
        desktopId: 'pve-170',
      ),
      throwsA(isA<BrokerException>()),
    );
  });

  for (final scenario in [
    'eligible',
    'other-token',
    'expired',
    'offline',
    'ordinary',
  ]) {
    test('actual HTTP inventory selects scoped login exactly once: $scenario', () async {
      BrokerTransport.clear();
      final server = await HttpServer.bind(InternetAddress.loopbackIPv4, 0);
      final origin = Uri.parse('http://127.0.0.1:${server.port}');
      final approval = BrokerTransport.approve(origin, origin.origin);
      final bodies = <Map<String, dynamic>>[];
      String? discoveredProtocol;
      final api = HttpBrokerApi(origin: origin);
      server.listen((request) async {
        request.response.headers.contentType = ContentType.json;
        if (request.uri.path == '/healthz') {
          request.response.write(
            jsonEncode({
              'service': 'multitap-broker',
              'status': 'ok',
              'authentication': 'id-password',
              'media': 'relay-v1',
              'version': '9.9.9',
              'transport': {
                'protocol': 'broker-transport-v1',
                'publicOrigin': origin.origin,
                'scheme': 'http',
                'allowInsecureHttp': true,
                'directTls': false,
              },
            }),
          );
        } else if (request.uri.path == '/api/v1/desktops') {
          discoveredProtocol = request.headers.value(
            'x-multitap-signin-protocol',
          );
          request.response.write(
            jsonEncode({
              'desktops': [
                {
                  'id': 'pve-170',
                  'name': 'Fixture',
                  'description': '',
                  'kind': 'vm',
                  'os': 'Windows',
                  'status': 'awaiting_host',
                  'canConnect': false,
                  'connectionModes': {'stream': scenario == 'ordinary'},
                  'host': {'connected': scenario != 'offline'},
                  'signinAvailability': offer(
                    DateTime.now().subtract(
                      Duration(seconds: scenario == 'expired' ? 6 : 0),
                    ),
                  ),
                },
              ],
            }),
          );
        } else {
          final body = jsonDecode(
            await utf8.decoder.bind(request).join(),
          ) as Map<String, dynamic>;
          bodies.add(body);
          request.response.write(
            jsonEncode({
              ...signinConnectionJson(),
              'webSocketUrl':
                  'ws://127.0.0.1:${server.port}/api/v1/sessions/$signinSessionId',
              'options': body['options'],
              if (body['signinProtocol'] != null) 'signin': signinBindingJson(),
            }),
          );
        }
        await request.response.close();
      });
      try {
        final desktops = await api.desktops('synthetic-a');
        expect(discoveredProtocol, signinStreamProtocol);
        final mapped = machineFromBroker(desktops.single);
        expect(mapped.signinAvailability != null, scenario != 'ordinary');
        const preference = StreamOptions(audio: true);
        final grant = await api.connect(
          scenario == 'other-token' ? 'synthetic-b' : 'synthetic-a',
          'pve-170',
          options: preference,
          displayScale: 150,
        );
        expect(bodies, hasLength(1));
        final scoped = scenario == 'eligible';
        expect(grant.signin != null, scoped);
        expect(bodies.single.containsKey('signinProtocol'), scoped);
        expect(preference.audio, isTrue);
        if (scoped) {
          expect((bodies.single['options'] as Map)['audio'], isFalse);
          for (final denied in [
            'displayScaleProtocol',
            'displayScale',
            'cursorProtocol',
            'printProtocol',
            'driveProtocol',
            'usbProtocol',
            'udpMediaProtocol',
            'audioCodecProtocol',
            'relativeMouseProtocol',
            'sessionResumeProtocol',
          ]) {
            expect(bodies.single.containsKey(denied), isFalse, reason: denied);
          }
        }
      } finally {
        api.close();
        approval.release();
        await server.close(force: true);
        BrokerTransport.clear();
      }
    });
  }
}
