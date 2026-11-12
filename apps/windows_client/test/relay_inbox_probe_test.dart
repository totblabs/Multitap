import 'dart:async';
import 'dart:typed_data';

import 'package:flutter_test/flutter_test.dart';
import 'package:multitap_client/data/relay_inbox.dart';

void main() {
  test('turning observation off clears backlog and stops sampling without changing delivery', () async {
    var clockCalls = 0;
    final probe = RelayInboxProbe(() => ++clockCalls);
    final source = StreamController<dynamic>();
    final gate = Completer<void>();
    final done = Completer<void>();
    final seen = <int>[];
    unawaited(() async {
      await for (final packet in relayInbox(
        source.stream,
        answer: (_) => false,
        probe: probe,
      )) {
        seen.add((packet as List<int>).length);
        if (seen.length == 1) await gate.future;
      }
      done.complete();
    }());
    source.add(Uint8List(10));
    await Future<void>.delayed(Duration.zero);
    source.add(Uint8List(20));
    await Future<void>.delayed(Duration.zero);
    expect(probe.packets, 1);
    probe.disable();
    final disabledCalls = clockCalls;
    source.add(Uint8List(30));
    await Future<void>.delayed(Duration.zero);
    expect(probe.oldestUs, null);
    expect(probe.packets, 0);
    expect(probe.bytes, 0);
    gate.complete();
    await source.close();
    await done.future;
    expect(clockCalls, disabledCalls);
    expect(seen, [10, 20, 30]);
  });
  test('queue snapshot measures waiting messages, excludes in-flight and answered text', () async {
    var now = 1000;
    final probe = RelayInboxProbe(() => now);
    final source = StreamController<dynamic>();
    final gate = Completer<void>(), done = Completer<void>();
    final seen = <int>[];
    unawaited(() async {
      await for (final packet in relayInbox(
        source.stream,
        answer: (s) => s == 'ACK',
        probe: probe,
      )) {
        seen.add((packet as Uint8List).length);
        if (seen.length == 1) await gate.future;
      }
      done.complete();
    }());
    source.add(Uint8List(10));
    await Future<void>.delayed(Duration.zero);
    source.add('ACK');
    source.add(Uint8List(20));
    source.add(Uint8List(30));
    await Future<void>.delayed(Duration.zero);
    now = 9000;
    expect(probe.packets, 2);
    expect(probe.bytes, 50);
    expect(probe.oldestUs, 8000);
    gate.complete();
    await source.close();
    await done.future;
    expect(seen, [10, 20, 30]);
    expect(probe.packets, 0);
    expect(probe.bytes, 0);
    expect(probe.oldestUs, 0);
  });
  test(
    'clock failures do not affect delivery or masquerade as a zero duration',
    () async {
      final probe = RelayInboxProbe(
        () => throw StateError('clock unavailable'),
      );
      final source = StreamController<dynamic>();
      final values = relayInbox(
        source.stream,
        answer: (_) => false,
        probe: probe,
      ).toList();
      source.add(Uint8List(4));
      await source.close();
      expect(await values, hasLength(1));
      expect(probe.bytes, 0);
    },
  );
}
