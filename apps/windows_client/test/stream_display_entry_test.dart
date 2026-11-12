// Actual entry policy with injected broker display operations. No Windows
// display, service or authenticated VM is changed by these fixtures.
import 'dart:async';

import 'package:flutter/material.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:multitap_client/data/broker_api.dart';
import 'package:multitap_client/data/stream_display_entry.dart';
import 'package:multitap_client/models/desktop_machine.dart';
import 'package:multitap_client/models/stream_options.dart';

DesktopMachine entryMachine(
  int count, {
  String id = 'pve-149',
  bool stale = false,
  List<RemoteMonitor>? monitors,
  bool initialViewport = false,
  bool viewportEncoderLimits = false,
}) => DesktopMachine(
  id: id,
  name: 'TESTER',
  description: '',
  kind: MachineKind.vm,
  status: MachineStatus.ready,
  specification: 'Windows',
  accent: Colors.blue,
  hostOnline: true,
  virtualMonitorControl: true,
  independentDisplayStreaming: true,
  initialViewport: initialViewport,
  viewportEncoderLimits: viewportEncoderLimits,
  display: VirtualDisplayState(
    supported: true,
    activeCount: count,
    stale: stale,
    checkedAt: DateTime.utc(2026, 10, 24),
  ),
  monitors:
      monitors ??
      [
        for (var n = 0; n < count; n++)
          RemoteMonitor(id: '$n', width: 1920, height: 1080, primary: n == 0),
      ],
);

void main() {
  test('single-window entry reduces two owned outputs to one before admitting capture', () async {
    final pending = Completer<DesktopMachine>(), calls = <int>[];
    final result = prepareSingleStreamDisplay(entryMachine(2), (count) {
      calls.add(count);
      return pending.future;
    });
    var completed = false;
    unawaited(result.then((_) => completed = true));
    await Future<void>.value();
    expect(calls, [1]);
    expect(completed, isFalse);
    pending.complete(entryMachine(1));
    expect((await result).monitors.single.id, '0');
  });

  test('dual entry requests exactly two outputs; known fresh count does not flap devices', () async {
    final calls = <int>[];
    final fresh = await prepareStreamDisplayCount(entryMachine(1), 2, (
      count,
    ) async {
      calls.add(count);
      return entryMachine(2);
    });
    expect(fresh.monitors.length, 2);
    expect(calls, [2]);
    expect(
      await prepareStreamDisplayCount(fresh, 2, (_) async {
        fail('A verified two-output layout must not be recreated.');
      }),
      same(fresh),
    );
  });

  test('command completion never substitutes wrong VM, stale count or missing owned output', () async {
    for (final fresh in [
      entryMachine(1, id: 'pve-170'),
      entryMachine(1, stale: true),
      entryMachine(2),
      entryMachine(
        1,
        monitors: const [RemoteMonitor(id: '1', width: 1920, height: 1080)],
      ),
    ]) {
      await expectLater(
        prepareSingleStreamDisplay(entryMachine(2), (_) async => fresh),
        throwsA(
          isA<BrokerException>().having(
            (e) => e.code,
            'code',
            'display_operation_unknown',
          ),
        ),
      );
    }
  });

  test('single display discards old secondary/all selection while retaining negotiated quality', () {
    for (final monitor in ['1', 'all']) {
      final options = StreamOptions(
        monitor: monitor,
        maxWidth: 2560,
        maxHeight: 1440,
        fps: 30,
        bitrate: 8000000,
        audio: false,
      ).forSingleDisplay();
      expect(options.toJson(), {
        'maxWidth': 2560,
        'maxHeight': 1440,
        'fps': 30,
        'bitrate': 8000000,
        'audio': false,
        'monitor': '0',
      });
    }
  });
}
