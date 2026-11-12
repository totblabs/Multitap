// Read-only readiness polling with injected Host snapshots. No live VM or
// Windows display is changed; this is not a Host stop acknowledgement test.
import 'dart:async';

import 'package:flutter/material.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:multitap_client/data/broker_api.dart';
import 'package:multitap_client/data/stream_display_entry.dart';
import 'package:multitap_client/models/desktop_machine.dart';
import 'package:multitap_client/models/stream_preparation.dart';

import 'stream_display_entry_test.dart' show entryMachine;

DesktopMachine pending({
  String id = 'pve-149',
  bool online = true,
  bool interactive = true,
  bool busy = false,
  StreamPreparation? reason,
  String power = 'unknown',
  bool powerBusy = false,
}) => DesktopMachine(
  id: id,
  name: 'TESTER',
  description: '',
  kind: MachineKind.vm,
  status: MachineStatus.awaitingHost,
  specification: 'Windows',
  accent: Colors.blue,
  hostOnline: online,
  interactiveSession: interactive,
  streamPreparation: reason,
  display: VirtualDisplayState(busy: busy),
  powerState: power,
  powerStale: power == 'unknown',
  powerBusy: powerBusy,
);

/// The VM runs but its Host service is away (restarting after an update or a
/// worker/capture failure).
DesktopMachine restarting() => pending(online: false, power: 'running');

void main() {
  for (final status in [502, 503, 504]) {
    testWidgets('Transport recovery includes its first HTTP$status read', (
      t,
    ) async {
      var reads = 0;
      DesktopMachine? ready;
      unawaited(
        waitForStreamDisplayControl(
          entryMachine(1),
          refresh: () async {
            if (++reads == 1) throw BrokerException('invalid_response', status);
            return entryMachine(1);
          },
          current: () => true,
          recovering: true,
          refreshFirst: true,
          retryGatewayErrors: true,
        ).then((value) => ready = value),
      );
      await t.pump();
      expect(reads, 1);
      expect(
        ready,
        isNull,
        reason: 'An old ready snapshot is not a fresh read.',
      );
      await t.pump(const Duration(milliseconds: 500));
      expect(reads, 2);
      expect(ready?.id, entryMachine(1).id);
    });
  }

  for (final item in [
    (false, false, false, const BrokerException('invalid_response', 502)),
    (true, false, false, const BrokerException('invalid_response', 502)),
    (true, true, true, const BrokerException('invalid_response', 502)),
    (true, true, false, const BrokerException('invalid_response', 200)),
    (true, true, false, const BrokerException('invalid_response')),
    (true, true, false, const BrokerException('invalid_response', 500)),
    (true, true, false, const BrokerException('session_expired', 401)),
    (true, true, false, const BrokerException('session_expired', 503)),
    (true, true, false, const BrokerException('desktop_not_found', 403)),
    (true, true, false, const BrokerException('invalid_stream_info', 503)),
    (true, true, false, const BrokerException('authorization_revoked', 502)),
  ]) {
    testWidgets(
      'Only explicit transport recovery retries ${item.$4.code}/${item.$4.status} flags${item.$1}/${item.$2}/${item.$3}',
      (t) async {
        var reads = 0;
        Object? failure;
        unawaited(
          waitForStreamDisplayControl(
            entryMachine(1),
            refresh: () async {
              reads++;
              throw item.$4;
            },
            current: () => true,
            recovering: item.$1,
            retryGatewayErrors: item.$2,
            initialPreparation: item.$3,
            refreshFirst: true,
          ).catchError((Object error) {
            failure = error;
            return entryMachine(1);
          }),
        );
        await t.pump();
        expect(failure, same(item.$4));
        await t.pump(const Duration(seconds: 31));
        expect(reads, 1);
      },
    );
  }

  testWidgets(
    'Gateway retry uses the remaining deadline and retires a late read',
    (t) async {
      final late = Completer<DesktopMachine>();
      var reads = 0;
      Object? failure;
      DesktopMachine? ready;
      unawaited(
        waitForStreamDisplayControl(
              entryMachine(1),
              refresh: () async {
          if (++reads == 1) {
                  throw const BrokerException('invalid_response', 503);
          }
                return late.future;
              },
              current: () => true,
              recovering: true,
              refreshFirst: true,
              retryGatewayErrors: true,
              maxWait: const Duration(milliseconds: 900),
            )
            .then((value) {
              ready = value;
              return value;
            })
            .catchError((Object error) {
              failure = error;
              return entryMachine(1);
            }),
      );
      await t.pump();
      await t.pump(const Duration(milliseconds: 500));
      expect(reads, 2);
      await t.pump(const Duration(milliseconds: 401));
      expect(
        failure,
        isA<BrokerException>()
            .having((e) => e.code, 'code', 'invalid_response')
            .having((e) => e.status, 'status', 503),
      );
      late.complete(entryMachine(1));
      await t.pump();
      await t.pump(const Duration(seconds: 31));
      expect(ready, isNull);
      expect(reads, 2);
    },
  );

  testWidgets('Cancelled gateway backoff performs no more reads', (t) async {
    var current = true, reads = 0;
    Object? failure;
    unawaited(
      waitForStreamDisplayControl(
        entryMachine(1),
        refresh: () async {
          reads++;
          throw const BrokerException('invalid_response', 504);
        },
        current: () => current,
        recovering: true,
        refreshFirst: true,
        retryGatewayErrors: true,
      ).catchError((Object error) {
        failure = error;
        return entryMachine(1);
      }),
    );
    await t.pump();
    current = false;
    await t.pump(const Duration(milliseconds: 500));
    expect(
      failure,
      isA<BrokerException>().having(
        (e) => e.code,
        'code',
        'operation_cancelled',
      ),
    );
    await t.pump(const Duration(seconds: 31));
    expect(reads, 1);
  });

  test('verified count control requires no additional read', () async {
    final machine = entryMachine(1);
    expect(
      await waitForStreamDisplayControl(
        machine,
        refresh: () async => throw StateError('Unnecessary read'),
        current: () => true,
      ),
      same(machine),
    );
  });

  test('offline, locked, recovery and concurrent operations are returned immediately', () async {
    for (final machine in [
      pending(online: false),
      pending(interactive: false),
      pending(reason: StreamPreparation.locked),
      pending(reason: StreamPreparation.resume),
      pending(reason: StreamPreparation.recovery),
      pending(busy: true),
    ]) {
      expect(
        await waitForStreamDisplayControl(
          machine,
          refresh: () async => throw StateError('Terminal state was retried'),
          current: () => true,
        ),
        same(machine),
      );
    }
  });

  testWidgets(
    'A running VM whose Host is restarting is awaited instead of failing as display_not_supported',
    (t) async {
      // Production: reconnecting 2-12 s after a Host restart failed at stage 4
      // as display_not_supported; the same retry seconds later succeeded.
      var reads = 0;
      DesktopMachine? ready;
      unawaited(
        waitForStreamDisplayControl(
          restarting(),
          refresh: () async => ++reads < 3 ? restarting() : entryMachine(1),
          current: () => true,
        ).then((value) => ready = value),
      );
      await t.pump(const Duration(milliseconds: 500));
      await t.pump(const Duration(milliseconds: 500));
      expect(ready, isNull);
      await t.pump(const Duration(milliseconds: 500));
      await t.pump();
      expect(reads, 3);
      expect(ready?.canConfigureDisplays, isTrue);
    },
  );

  testWidgets(
    'A Host that stays away is reported as offline after the bounded wait',
    (t) async {
      Object? failure;
      unawaited(
        waitForStreamDisplayControl(
          restarting(),
          refresh: () async => restarting(),
          current: () => true,
        ).catchError((Object error) {
          failure = error;
          return pending();
        }),
      );
      await t.pump(const Duration(seconds: 14));
      expect(failure, isNull);
      await t.pump(const Duration(seconds: 2));
      expect(
        failure,
        isA<BrokerException>().having((e) => e.code, 'code', 'host_offline'),
      );
    },
  );

  test('An offline Host is never reported as missing display support', () async {
    for (final machine in [restarting(), pending(online: false)]) {
      await expectLater(
        prepareStreamDisplayCount(
          machine,
          1,
          (_) async => fail('An offline Host cannot be configured'),
        ),
        throwsA(
          isA<BrokerException>().having((e) => e.code, 'code', 'host_offline'),
        ),
      );
    }
    // A reboot in progress is not awaited: it reports at once.
    expect(
      await waitForStreamDisplayControl(
        pending(online: false, power: 'running', powerBusy: true),
        refresh: () async => throw StateError('Power change was polled'),
        current: () => true,
      ),
      isA<DesktopMachine>(),
    );
  });

  testWidgets('changed VM identity cannot reach count preparation', (t) async {
    Object? failure;
    unawaited(
      waitForStreamDisplayControl(
        pending(),
        refresh: () async => entryMachine(1, id: 'pve-170'),
        current: () => true,
      ).catchError((Object error) {
        failure = error;
        return pending();
      }),
    );
    await t.pump(const Duration(milliseconds: 500));
    expect(
      failure,
      isA<BrokerException>().having((e) => e.code, 'code', 'invalid_response'),
    );
  });

  testWidgets('stale read after view retirement cannot continue polling', (
    t,
  ) async {
    final read = Completer<DesktopMachine>();
    var current = true, reads = 0;
    Object? failure;
    unawaited(
      waitForStreamDisplayControl(
        pending(),
        refresh: () {
          reads++;
          return read.future;
        },
        current: () => current,
      ).catchError((Object error) {
        failure = error;
        return pending();
      }),
    );
    await t.pump(const Duration(milliseconds: 500));
    expect(reads, 1);
    current = false;
    read.complete(entryMachine(1));
    await t.pump();
    expect(
      failure,
      isA<BrokerException>().having(
        (e) => e.code,
        'code',
        'operation_cancelled',
      ),
    );
    await t.pump(const Duration(seconds: 16));
    expect(reads, 1);
  });

  testWidgets(
    'whole wait is bounded and a late HTTP result never restarts observation',
    (t) async {
      final firstRead = Completer<DesktopMachine>();
      final read = Completer<DesktopMachine>();
      var reads = 0;
      Object? failure;
      unawaited(
        waitForStreamDisplayControl(
          pending(),
          refresh: () {
            reads++;
            return reads == 1 ? firstRead.future : read.future;
          },
          current: () => true,
        ).catchError((Object error) {
          failure = error;
          return pending();
        }),
      );
      // Consume most of the first read's budget before the second starts.
      // The 15-second whole limit must win over its own 8-second timeout.
      await t.pump(const Duration(milliseconds: 500));
      expect(reads, 1);
      await t.pump(const Duration(milliseconds: 6500));
      firstRead.complete(pending());
      await t.pump();
      await t.pump(const Duration(milliseconds: 500));
      expect(reads, 2);
      await t.pump(const Duration(milliseconds: 7600));
      expect(
        failure,
        isA<BrokerException>().having(
          (e) => e.code,
          'code',
          'host_maintenance',
        ),
      );
      read.complete(entryMachine(1));
      await t.pump();
      await t.pump(const Duration(seconds: 16));
      expect(reads, 2);
    },
  );

  testWidgets(
    'After a Host loss, its display recovery is awaited quietly until count control returns',
    (t) async {
      // 3 s after host_error a reconnect can fail at stage 4 as
      // display_not_supported while the Host restores its display; the same
      // retry 0.4 s later succeeds.
      final states = [
        pending(reason: StreamPreparation.recovery),
        pending(),
        pending(busy: true),
        entryMachine(1),
      ];
      var reads = 0;
      DesktopMachine? ready;
      unawaited(
        waitForStreamDisplayControl(
          pending(reason: StreamPreparation.display),
          refresh: () async => states[reads++],
          current: () => true,
          recovering: true,
        ).then((value) => ready = value),
      );
      for (var n = 0; n < 4; n++) {
        await t.pump(const Duration(milliseconds: 500));
      }
      await t.pump();
      expect(reads, 4);
      expect(ready?.canConfigureDisplays, isTrue);
    },
  );

  test('After a Host loss, states that need the user are still returned at once', () async {
    for (final machine in [
      pending(interactive: false),
      pending(reason: StreamPreparation.locked),
      pending(reason: StreamPreparation.resume),
      pending(reason: StreamPreparation.login),
      pending(reason: StreamPreparation.console),
      pending(reason: StreamPreparation.configuration),
      pending(online: false, power: 'running', powerBusy: true),
    ]) {
      expect(
        await waitForStreamDisplayControl(
          machine,
          refresh: () async => throw StateError('A user-action state was polled'),
          current: () => true,
          recovering: true,
        ),
        same(machine),
      );
    }
  });

  testWidgets('The wait after a Host loss is bounded at 30 seconds', (t) async {
    Object? failure;
    unawaited(
      waitForStreamDisplayControl(
        pending(reason: StreamPreparation.display),
        refresh: () async => pending(reason: StreamPreparation.display),
        current: () => true,
        recovering: true,
      ).catchError((Object error) {
        failure = error;
        return pending();
      }),
    );
    await t.pump(const Duration(seconds: 29));
    expect(failure, isNull);
    await t.pump(const Duration(seconds: 2));
    expect(
      failure,
      isA<BrokerException>().having((e) => e.code, 'code', 'host_maintenance'),
    );
  });

  testWidgets(
    'A view without monitor-count preparation waits for the Host to stream again, not for count control',
    (t) async {
      final streaming = DesktopMachine(
        id: 'pve-149',
        name: 'TESTER',
        description: '',
        kind: MachineKind.vm,
        status: MachineStatus.ready,
        specification: 'Windows',
        accent: Colors.blue,
        hostOnline: true,
        interactiveSession: true,
      );
      expect(streaming.canConfigureDisplays, isFalse);
      var reads = 0;
      DesktopMachine? ready;
      unawaited(
        waitForStreamDisplayControl(
          restarting(),
          refresh: () async => ++reads < 2 ? pending() : streaming,
          current: () => true,
          recovering: true,
          countControl: false,
        ).then((value) => ready = value),
      );
      await t.pump(const Duration(milliseconds: 500));
      await t.pump(const Duration(milliseconds: 500));
      await t.pump();
      expect(reads, 2);
      expect(ready, same(streaming));
    },
  );

  testWidgets(
    'authorization and request errors propagate without another query',
    (t) async {
      var reads = 0;
      Object? failure;
      unawaited(
        waitForStreamDisplayControl(
          pending(),
          refresh: () async {
            reads++;
            throw const BrokerException('session_expired', 401);
          },
          current: () => true,
        ).catchError((Object error) {
          failure = error;
          return pending();
        }),
      );
      await t.pump(const Duration(milliseconds: 500));
      expect(
        failure,
        isA<BrokerException>().having((e) => e.status, 'status', 401),
      );
      await t.pump(const Duration(seconds: 16));
      expect(reads, 1);
    },
  );
}
