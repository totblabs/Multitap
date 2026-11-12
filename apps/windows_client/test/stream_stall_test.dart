import 'package:flutter_test/flutter_test.dart';
import 'package:multitap_client/data/client_diagnostics.dart';
import 'package:multitap_client/data/stream_stall.dart';

// [kind, serial, gapMs, agoMs, key, queue, queueMax, offers, full, decodes,
//  decodeMaxUs, pending, skipped, waitMs], as RemoteEngine::Stats reports it.
List<int> record(int serial, {int gapMs = 1000, int agoMs = 0, int kind = 1}) => [
  kind, serial, gapMs, agoMs, 1, 8, 8, 3, 5, 2, 18000, 1, 0, 0,
];

void main() {
  test('A stall becomes one event once the Host window covering it arrived', () {
    final watch = StallWatch();
    // Arrivals every 16 ms, then nothing for 900 ms.
    for (var us = 0; us <= 1000000; us += 16000) {
      watch.arrival(us);
    }
    watch.arrival(1900000);
    watch.host({
      'stall': {'encodeGapMs': 17, 'keyFrames': 0},
      'udpBacklogKeys': 3, 'udpClientKeys': 1, 'udpRefused': 0,
    }, 900000);
    watch.engine(null, {'udpGaps': 10, 'udpKeyRequests': 1, 'udpQueueSkips': 0}, 900000);
    // The engine published again at 2.0 s after a 1 s gap.
    watch.engine([record(1, agoMs: 0)], {'udpGaps': 14, 'udpKeyRequests': 3, 'udpQueueSkips': 1}, 2000000);
    expect(watch.take(2000000), isEmpty, reason: 'waits for the Host window');
    watch.host({
      'stall': {'encodeGapMs': 40, 'captureGapMs': 35, 'encodeMs': 9, 'keyFrames': 1, 'keyRequests': 1, 'bitrateSteps': 1, 'targetMinKbps': 18000},
      'udpBacklogKeys': 3, 'udpClientKeys': 2, 'udpRefused': 0,
    }, 1900000);
    watch.host({
      'stall': {'encodeGapMs': 20, 'keyFrames': 1, 'keyRequests': 1, 'targetMinKbps': 20000},
      'udpBacklogKeys': 3, 'udpClientKeys': 3, 'udpRefused': 2,
    }, 2900000);
    final events = watch.take(3300000, rttMs: 9, decoderBusy: 274, udpActivePath: 1);
    expect(events, hasLength(1));
    final event = events.single;
    expect(event['stallKind'], 1);
    expect(event['stallMs'], 1000);
    expect(event['stallFull'], 5);
    expect(event['stallDecodeMaxUs'], 18000);
    expect(event['stallArrivalMs'], 908);
    expect(event['stallUdpLost'], 4);
    expect(event['stallUdpKeys'], 2);
    expect(event['stallUdpSkips'], 1);
    // Windows that ended at 1.9 s and 2.9 s cover a stall from 1.0 to 2.0 s.
    expect(event['hostWindows'], 2);
    expect(event['hostEncodeGapMs'], 40);
    expect(event['hostCaptureGapMs'], 35);
    expect(event['hostKeyFrames'], 2);
    expect(event['hostKeyRequests'], 2);
    expect(event['hostTargetMinKbps'], 18000);
    expect(event['hostUdpClientKeys'], 2);
    expect(event['hostUdpRefused'], 2);
    expect(event['rttMs'], 9);
    expect(event['decoderBusy'], 274);
    expect(event['udpActivePath'], 1);
    expect(event['stallDropped'], 0);
    expect(event.keys.every(streamStallFields.contains), isTrue);
    expect(watch.takeWindow(), (1, 1000));
    expect(watch.takeWindow(), (0, 0));
  });

  test('A record is reported once; a restarted engine counts again', () {
    final watch = StallWatch(hostCoverUs: 0, spacingUs: 0, minuteRows: 100);
    watch.engine([record(1)], null, 1000000);
    watch.engine([record(1, agoMs: 1000), record(2)], null, 2000000);
    expect(watch.take(2000000), hasLength(2));
    watch.engine([record(1, agoMs: 2000), record(2, agoMs: 1000)], null, 3000000);
    expect(watch.take(3000000), isEmpty);
    watch.restart();
    watch.engine([record(1)], null, 4000000);
    expect(watch.take(4000000), hasLength(1));
    watch.engine([record(1), record(2), record(3)], null, 4500000);
    expect(watch.take(4500000), hasLength(2));
    // Without restart(): the serials went down, so the engine started over.
    watch.engine([record(1), record(1, kind: 2)], null, 5000000);
    expect(watch.take(5000000), hasLength(2));
  });

  test('At most one event per spacing; the rest are counted in the next', () {
    final watch = StallWatch(hostCoverUs: 0);
    watch.engine([record(1), record(2), record(3)], null, 1000000);
    expect(watch.take(1000000), hasLength(1));
    watch.engine([record(4)], null, 4000000);
    final next = watch.take(4000000);
    expect(next, hasLength(1));
    expect(next.single['stallDropped'], 2);
  });

  test('Malformed records are ignored', () {
    final watch = StallWatch(hostCoverUs: 0);
    watch.engine([
      [1, 1],
      'x',
      [3, 1, 1000, 0, 1, 8, 8, 3, 5, 2, 18000, 1, 0, 0],
      [1, 1, -5, 0, 1, 8, 8, 3, 5, 2, 18000, 1, 0, 0],
    ], null, 1000000);
    expect(watch.take(1000000), isEmpty);
  });

  group('Idle screen (stall-v2)', () {
    // The engine's record with the Host's spacing of the two frames (15 fields).
    List<int> shown(int serial, {required int gapMs, required int ptsGapMs}) => [
      1, serial, gapMs, 0, 0, 0, 1, 1, 0, 1, 4000, 1, 0, 0, ptsGapMs,
    ];
    Map<String, dynamic> hostWindow({int changeLateMs = 0, int keyRequests = 0}) => {
      'stall': {'encodeGapMs': 1004, 'captureGapMs': 0, 'changeLateMs': changeLateMs, 'keyRequests': keyRequests},
      'udpBacklogKeys': 0, 'udpClientKeys': 0, 'udpRefused': 0,
    };

    /// Two minutes of a still desktop: a refresh every second (1000-1040 ms
    /// shown apart, stamped about 1008 ms apart), a Host window every second.
    /// [extra] may replace one second's record and Host window.
    (List<Map<String, int>>, int) run(
      StallWatch watch, {
      int? at,
      List<int> Function(int serial)? extra,
      Map<String, dynamic>? extraHost,
      Map<String, int> Function(int second)? udp,
    }) {
      final rows = <Map<String, int>>[];
      var stalls = 0;
      for (var s = 1; s <= 120; s++) {
        final nowUs = s * 1000000;
        watch.host(s == at && extraHost != null ? extraHost : hostWindow(), nowUs - 300000);
        final record = s == at && extra != null ? extra(s) : shown(s, gapMs: 1000 + (s % 3) * 20, ptsGapMs: 1008);
        watch.engine([record], udp?.call(s) ?? {'udpGaps': 0, 'udpKeyRequests': 0}, nowUs);
        rows.addAll(watch.take(nowUs));
        stalls += watch.takeWindow().$1;
      }
      return (rows, stalls);
    }

    test('An idle desktop with 1 s refreshes produces no stall row and counts no stall', () {
      final (rows, stalls) = run(StallWatch());
      expect(rows, isEmpty);
      expect(stalls, 0);
      // Without the Host's stall object (a Host before stall-v2) as well.
      final old = StallWatch();
      for (var s = 1; s <= 30; s++) {
        old.host({}, s * 1000000 - 300000);
        old.engine([shown(s, gapMs: 1010, ptsGapMs: 1004)], null, s * 1000000);
        expect(old.take(s * 1000000), isEmpty);
      }
      expect(old.summary(), isNull);
    });

    test('A real 600 ms gap while the picture changed is exactly one row', () {
      // The Host stamped the two frames 16 ms apart: the way here added 584 ms.
      final (rows, stalls) = run(
        StallWatch(),
        at: 30,
        extra: (serial) => shown(serial, gapMs: 600, ptsGapMs: 16),
      );
      expect(rows, hasLength(1));
      expect(rows.single['stallMs'], 600);
      expect(rows.single['stallPtsGapMs'], 16);
      expect(rows.single.keys.every(streamStallFields.contains), isTrue);
      expect(stalls, 1);
    });

    test('A gap the Host owes while the picture changed, and one with lost video, still count', () {
      // Capture held a changed picture 610 ms before its encode.
      final (late, _) = run(
        StallWatch(),
        at: 40,
        extra: (serial) => shown(serial, gapMs: 640, ptsGapMs: 620),
        extraHost: hostWindow(changeLateMs: 610),
      );
      // The idle second before it overlaps the same Host window: its 1 s
      // spacing is a refresh, not that wait, and sends nothing.
      expect(late, hasLength(1));
      expect(late.single['stallMs'], 640);
      expect(late.single['hostChangeLateMs'], 610);
      // The same spacing with nothing changed on the Host (a pause in typing) is idle.
      final (pause, _) = run(StallWatch(), at: 40, extra: (serial) => shown(serial, gapMs: 640, ptsGapMs: 620));
      expect(pause, isEmpty);
      // A refresh whose neighbours were lost on UDP (the leg gave datagrams up).
      final (lost, _) = run(
        StallWatch(),
        at: 50,
        udp: (s) => {'udpGaps': s < 50 ? 0 : 4, 'udpKeyRequests': 0},
      );
      expect(lost, hasLength(1));
      expect(lost.single['stallUdpLost'], 4);
      // The Host missed its own refresh (stamped 1.6 s apart).
      final (missed, _) = run(StallWatch(), at: 60, extra: (serial) => shown(serial, gapMs: 1610, ptsGapMs: 1600));
      expect(missed, hasLength(1));
    });
  });

  test('Rows are capped per minute; the rest are counted in the next row', () {
    final watch = StallWatch(hostCoverUs: 0);
    final rows = <Map<String, int>>[];
    // A real stall every 3 s for a minute (the 2 s spacing alone allows all 20).
    for (var i = 0; i < 20; i++) {
      final nowUs = (1 + 3 * i) * 1000000;
      watch.engine([record(i + 1)], null, nowUs);
      rows.addAll(watch.take(nowUs));
    }
    expect(rows, hasLength(6));
    expect(watch.takeWindow().$1, 20, reason: 'every stall still counts in the window');
    watch.engine([record(21)], null, 70000000);
    final next = watch.take(70000000);
    expect(next, hasLength(1));
    expect(next.single['stallDropped'], 14);
  });

  test('Rows are capped per session; the rest are one closing row', () {
    final watch = StallWatch(hostCoverUs: 0);
    final rows = <Map<String, int>>[];
    // 40 real stalls, one every 15 s (no per-minute cap reached).
    for (var i = 0; i < 40; i++) {
      final nowUs = (1 + 15 * i) * 1000000;
      watch.engine([record(i + 1)], null, nowUs);
      rows.addAll(watch.take(nowUs));
    }
    expect(rows, hasLength(30));
    expect(rows.every((row) => row['stallDropped'] == 0), isTrue);
    expect(watch.summary(), {'stallDropped': 10});
    expect(watch.summary(), isNull, reason: 'counted once');
  });

  test('Diagnostics keep only stall fields in a streamStall row', () async {
    final sent = <Map<String, Object?>>[];
    final diagnostics = ClientDiagnostics(
      desktopId: 'pve-149',
      send: (report) async {
        sent.add(report);
        return DiagnosticDelivery.accepted;
      },
    );
    diagnostics.record('streamStall', values: {'stallMs': 300, 'stallText': 1, 'samples': 2});
    await diagnostics.flush();
    final rows = sent.expand((r) => r['events']! as List).cast<Map<String, Object?>>();
    final stall = rows.firstWhere((row) => row['event'] == 'streamStall');
    expect(stall['values'], {'stallMs': 300});
    diagnostics.close();
  });
}
