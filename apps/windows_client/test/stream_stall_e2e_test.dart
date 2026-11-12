// stream-stall-v1 end to end on the Dart side (the client half, without which
// the broker's acknowledgement finds no client to send rows): an engine stall
// record becomes one row through the session's sink, and the broker API sends
// rows only after its acknowledgement.
import 'dart:async';
import 'dart:convert';

import 'package:flutter_test/flutter_test.dart';
import 'package:multitap_client/data/broker_api.dart';
import 'package:multitap_client/data/product_log.dart';
import 'package:multitap_client/data/remote_session.dart';
import 'package:multitap_client/data/stream_stall.dart';

import 'client_diagnostics_test.dart' show DiagnosticClient;
import 'power_api_test.dart' show taskId;
import 'remote_session_test.dart' show SessionSocket, info, packet;
import 'session_recovery_test.dart'
    show ManualClock, RecoveryMedia, selectedOptions;

/// The engine's Stats() with one kind 1 record of a 1 s gap that ended now:
/// [kind, serial, gapMs, agoMs, key, queue, queueMax, offers, full, decodes,
/// decodeMaxUs, pending, skipped, waitMs].
class StallMedia extends RecoveryMedia {
  bool stalled = false;

  /// stall-v2 engines add ptsGapMs: a refresh of a still screen, stamped by the
  /// Host as far apart as it was shown (nothing lost or refused).
  bool idle = false;

  /// A new 300 ms stall at every reading (more than the 2 s spacing sends).
  bool burst = false;
  int _serial = 0;
  @override
  Future<Map<String, dynamic>> stats() async => {
    ...await super.stats(),
    if (stalled)
      'stalls': [
        [1, 1, 1000, 0, 1, 8, 8, 3, 5, 2, 18000, 1, 0, 0],
      ],
    if (idle)
      'stalls': [
        [1, 1, 1010, 0, 0, 0, 1, 1, 0, 1, 4000, 1, 0, 0, 1004],
      ],
    if (burst)
      'stalls': [
        [1, ++_serial, 300, 0, 0, 2, 2, 3, 0, 2, 9000, 1, 0, 0, 16],
      ],
  };
}

void main() {
  for (final idle in [false, true]) {
    testWidgets(idle
      ? 'An idle screen refresh sends no streamStall row; a burst leaves one closing count'
      : 'An engine stall of 250 ms or more becomes one streamStall row and counts in the window', (t) async {
    final socket = SessionSocket(), media = StallMedia(), clock = ManualClock();
    final stalls = <Map<String, Object>>[], windows = <Map<String, Object>>[];
    final current = RemoteSession(
      connection: BrokerConnection(
        sessionId: 'qa-session',
        webSocketUrl: Uri.parse(
          'wss://cloud.example.com/api/v1/sessions/qa-session',
        ),
        options: selectedOptions,
      ),
      token: 'fixture-only-token',
      media: media,
      clock: clock,
      dialer: (_, _) async => socket,
      statsSink: windows.add,
      stallSink: stalls.add,
    );
    unawaited(current.start());
    await t.pump();
    socket.incoming.add(jsonEncode(info));
    await t.pump();
    var answered = 0, second = 0;
    Future<void> advance(int seconds) async {
      for (var i = 0; i < seconds; i++) {
        clock.now = ++second * 1000000;
        for (; answered < socket.sent.length; answered++) {
          final sent = socket.sent[answered];
          if (sent['type'] == 'ping') {
            socket.incoming.add(jsonEncode({'type': 'pong', 'id': sent['id']}));
          }
        }
        socket.incoming.add(packet());
        await t.pump(const Duration(seconds: 1));
      }
    }

    await advance(5);
    expect(current.frameReady, isTrue, reason: current.error);
    expect(stalls, isEmpty);
    if (idle) {
      media.idle = true;
      await advance(6);
      current.flushStats();
      expect(stalls, isEmpty);
      expect(windows.last['stalls'], 0);
      // A real stall every second for 20 s: six rows (the per-minute cap), and
      // the window that closes the session sends the rest as one closing row.
      media
        ..idle = false
        ..burst = true;
      await advance(20);
      expect(stalls, hasLength(6));
      current.flushStats();
      expect(stalls, hasLength(7));
      expect(stalls.last.keys, ['stallDropped']);
      expect(stalls.last['stallDropped'], greaterThan(0));
      await current.close();
      expect(stalls, hasLength(7), reason: 'counted once');
      await media.events.close();
      return;
    }
    media.stalled = true;
    // The engine keeps reporting the same record; the Host window covering it
    // never comes (an older Host), so it leaves after the 1.2 s wait, once.
    await advance(6);
    expect(stalls, hasLength(1));
    expect(stalls.single['stallKind'], 1);
    expect(stalls.single['stallMs'], 1000);
    expect(stalls.single['stallFull'], 5);
    expect(stalls.single.keys.every(streamStallFields.contains), isTrue);
    current.flushStats();
    expect(windows.last['stalls'], 1);
    expect(windows.last['stallMaxMs'], 1000);
    await current.close();
    await media.events.close();
  });
  }

  for (final acknowledged in [true, false]) {
    test(acknowledged
      ? 'Stalls before the first acknowledgement are held, then sent once it names stream-stall-v1'
      : 'Stalls before the first acknowledgement are dropped when it does not name stream-stall-v1', () async {
    final client = DiagnosticClient()..ackStall = acknowledged;
    final api = HttpBrokerApi(
      origin: Uri.parse('https://configured.invalid'),
      client: client,
      enableDiagnostics: true,
      diagnosticsDesktopId: 'pve-149',
      diagnosticsMode: 'stream',
    );
    addTearDown(api.close);
    List<Object?> sentMs() => [
      for (final r in client.reports)
        for (final e in r['events'] as List)
          if (e['event'] == 'streamStall') e['values']['stallMs'],
    ];
    await api.desktops('token');
    // A session's first seconds: no upload acknowledged yet.
    for (var i = 0; i < 10; i++) {
      api.recordStreamStall(
        {'stallKind': 1, 'stallMs': 300 + i},
        sessionId: taskId,
        desktopId: 'pve-149',
      );
    }
    await api.flushDiagnostics();
    ProductLog.instance.record(LogEvent.streamReady, {'ok': true});
    await api.flushDiagnostics();
    // A later acknowledgement does not bring back what a refusal dropped.
    client.ackStall = true;
    ProductLog.instance.record(LogEvent.streamReady, {'ok': true});
    await api.flushDiagnostics();
    expect(
      sentMs(),
      acknowledged ? [300, 301, 302, 303, 304, 305, 306, 307] : isEmpty,
      reason: 'bounded to 8 held rows',
    );
  });
  }

  test('streamStall rows go to the broker only after it acknowledges stream-stall-v1', () async {
    final client = DiagnosticClient();
    final api = HttpBrokerApi(
      origin: Uri.parse('https://configured.invalid'),
      client: client,
      enableDiagnostics: true,
      diagnosticsDesktopId: 'pve-149',
      diagnosticsMode: 'stream',
    );
    addTearDown(api.close);
    List<Map> sentStalls() => [
      for (final r in client.reports)
        for (final e in r['events'] as List)
          if (e['event'] == 'streamStall') e as Map,
    ];
    void stall(int ms) => api.recordStreamStall(
      {'stallKind': 1, 'stallMs': ms, 'stallText': 7},
      sessionId: taskId,
      desktopId: 'pve-149',
    );
    await api.desktops('token');
    await api.flushDiagnostics();
    stall(300);
    await api.flushDiagnostics();
    expect(sentStalls(), isEmpty, reason: 'a broker that did not acknowledge it');
    // A stream-stall-v1 broker answers each upload with streamStallProtocol.
    client.ackStall = true;
    ProductLog.instance.record(LogEvent.streamReady, {'ok': true});
    await api.flushDiagnostics();
    stall(1707);
    await api.flushDiagnostics();
    final row = sentStalls().single;
    expect(row['values'], {'stallKind': 1, 'stallMs': 1707});
    expect(row['sessionId'], taskId);
    expect(row['desktopId'], 'pve-149');
    expect(row['mode'], 'stream');
  });
}
