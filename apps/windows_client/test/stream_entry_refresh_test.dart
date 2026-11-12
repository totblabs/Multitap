import 'dart:async';

import 'package:flutter/material.dart';
import 'package:flutter/services.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:multitap_client/data/broker_api.dart';
import 'package:multitap_client/data/product_log.dart';
import 'package:multitap_client/models/session_display_mode.dart';
import 'package:multitap_client/models/desktop_machine.dart';
import 'package:multitap_client/ui/remote_view.dart';

import 'display_remote_test.dart' show machine;

const unavailable = DesktopMachine(
  id: 'qa-vm',
  name: 'QA VM',
  description: '',
  kind: MachineKind.vm,
  status: MachineStatus.awaitingHost,
  specification: 'Windows',
  accent: Colors.blue,
);

void main() {
  setUp(() {
    final messenger =
        TestDefaultBinaryMessengerBinding.instance.defaultBinaryMessenger;
    for (final name in ['multitap/window', 'multitap/window/events']) {
      messenger.setMockMethodCallHandler(
        MethodChannel(name),
        (_) async => false,
      );
    }
  });
  tearDown(() {
    final messenger =
        TestDefaultBinaryMessengerBinding.instance.defaultBinaryMessenger;
    for (final name in ['multitap/window', 'multitap/window/events']) {
      messenger.setMockMethodCallHandler(MethodChannel(name), null);
    }
  });
  testWidgets(
    'Retry refreshes the host after an unavailable upgrade snapshot',
    (t) async {
      var refreshes = 0, connects = 0, changes = 0;
      await t.pumpWidget(
        MaterialApp(
          home: RemoteView(
            machine: unavailable,
            prepareDisplaysOnEntry: true,
            initialDisplayMode: SessionDisplayMode.windowed,
            onRefreshDesktop: () async =>
                ++refreshes == 1 ? unavailable : machine(1),
            onConfigureDisplays: (_) async {
              changes++;
              return machine(1);
            },
            onReconnect: (_) async {
              connects++;
              throw const BrokerException('host_maintenance');
            },
            load: () async {
              fail('A prepared entry must use the fresh connect path');
            },
          ),
        ),
      );
      await t.pumpAndSettle();
      expect(refreshes, 1);
      expect(connects, 0);
      expect(changes, 0);
      final first = ProductLog.instance.entries.last;
      expect(first.event, LogEvent.streamEnd);
      expect(first.code, 'display_not_supported');
      expect(first.values['stage'], 4);
      await t.tap(find.byKey(const Key('retry-remote-connection')));
      await t.pumpAndSettle();
      expect(refreshes, 2);
      expect(connects, 1);
      expect(changes, 0);
      expect(ProductLog.instance.entries.last.code, 'host_maintenance');
      expect(ProductLog.instance.entries.last.values['stage'], 6);
    },
  );
  testWidgets(
    'A fresh assigned desktop cannot be substituted with another VM',
    (t) async {
      var connects = 0;
      await t.pumpWidget(
        MaterialApp(
          home: RemoteView(
            machine: machine(1),
            prepareDisplaysOnEntry: true,
            onRefreshDesktop: () async => const DesktopMachine(
              id: 'other-vm',
              name: 'Other',
              description: '',
              kind: MachineKind.vm,
              status: MachineStatus.ready,
              specification: 'Windows',
              accent: Colors.blue,
            ),
            load: () async {
              connects++;
              throw const BrokerException('host_maintenance');
            },
          ),
        ),
      );
      await t.pumpAndSettle();
      expect(connects, 0);
      expect(ProductLog.instance.entries.last.code, 'invalid_response');
      expect(ProductLog.instance.entries.last.values['stage'], 2);
    },
  );
  testWidgets('A closed remote window ignores a late inventory refresh', (
    t,
  ) async {
    final refresh = Completer<DesktopMachine>();
    var connects = 0;
    await t.pumpWidget(
      MaterialApp(
        home: RemoteView(
          machine: machine(1),
          prepareDisplaysOnEntry: true,
          onRefreshDesktop: () => refresh.future,
          load: () async {
            connects++;
            throw const BrokerException('host_maintenance');
          },
        ),
      ),
    );
    await t.pump();
    await t.pumpWidget(const MaterialApp(home: SizedBox()));
    refresh.complete(machine(1));
    await t.pumpAndSettle();
    expect(connects, 0);
    expect(t.takeException(), isNull);
  });
}
