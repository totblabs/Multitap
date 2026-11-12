import 'dart:async';
import 'dart:convert';
import 'dart:typed_data';

import 'package:flutter_test/flutter_test.dart';
import 'package:multitap_client/data/clipboard_limits.dart';
import 'package:multitap_client/data/clipboard_policy.dart';
import 'package:multitap_client/data/clipboard_pull.dart';
import 'package:multitap_client/data/clipboard_pull_observation.dart';
import 'package:multitap_client/data/clipboard_session.dart';
import 'package:multitap_client/data/client_diagnostics.dart';
import 'package:multitap_client/data/native_clipboard.dart';

import 'clipboard_pull_test.dart' as fixture;
import 'clipboard_onpaste_test.dart' as onpaste;
import 'clipboard_session_test.dart' as base;

class _GatedClipboard extends fixture.PullClipboard {
  Completer<void>? dataGate;
  @override
  Future<void> pullData(int rangeId, int offset, Uint8List bytes) async {
    await dataGate?.future;
    await super.pullData(rangeId, offset, bytes);
  }
}

void main() {
  late _GatedClipboard bridge;
  late ClipboardPulls pulls;
  late List<ClipboardPullObservation> events;
  late List<Map<String, Object?>> sent;

  setUp(() {
    bridge = _GatedClipboard();
    events = [];
    sent = [];
    pulls = ClipboardPulls(
      owner: 'test-owner',
      bridge: bridge,
      send: sent.add,
      limits: () => ClipboardLimits.onPasteRevision,
      idPrefix: 'pullc',
      changed: () {},
      started: () {},
      failed: (_) {},
      mayServe: (_) => true,
      observed: events.add,
      linger: Duration.zero,
    )..start();
  });
  tearDown(() async => pulls.close('clipboard_session_closed'));

  String activeId() =>
      sent.lastWhere((m) => m['type'] == 'clipboard.pull')['requestId']!
          as String;
  Map<String, dynamic> chunk(String id) => {
    'type': 'clipboard.chunk',
    'requestId': id,
    'index': 0,
    'offset': 0,
    'data': base64Encode([1, 2, 3]),
  };

  testWidgets(
    'copy emits nothing; real request, delivered first bytes and ended range are distinct',
    (tester) async {
      pulls.offerPublished('unlogged-offer', [
        fixture.tree('unlogged-name', 3),
      ]);
      expect(events, isEmpty);
      bridge.platform(const ClipboardPullRequest(1, 'unlogged-offer', 0, 0));
      await tester.pump();
      expect(events.map((e) => e.stage), [1]);
      final id = activeId();
      await pulls.handle(chunk(id));
      expect(events.map((e) => e.stage), [1, 2]);
      await tester.pump(const Duration(seconds: 3));
      expect(events.where((e) => e.stage == 4), isEmpty);
      await pulls.handle({
        'type': 'clipboard.item.end',
        'requestId': id,
        'index': 0,
        'sha256': 'a' * 64,
      });
      await tester.pump(const Duration(seconds: 2));
      expect(events.map((e) => e.stage), [1, 2, 4]);
      expect(bridge.data.single.$3, [1, 2, 3]);
      expect(bridge.ended.single.$1, 1);
      expect(events.every((e) => !e.sending && e.requests == 1), isTrue);
    },
  );

  testWidgets(
    'serving observes the actual stream without changing the wire messages',
    (tester) async {
      pulls.offerSent('unlogged-source', [fixture.tree('unlogged-path', 3)]);
      bridge.files['unlogged-source'] = [
        Uint8List.fromList([1, 2, 3]),
      ];
      expect(events, isEmpty);
      await pulls.handle({
        'type': 'clipboard.pull',
        'requestId': 'pullh-1',
        'offerId': 'unlogged-source',
        'index': 0,
        'offset': 0,
      });
      await tester.pump(const Duration(seconds: 2));
      expect(events.map((e) => e.stage), [1, 2, 4]);
      expect(events.every((e) => e.sending), isTrue);
      expect(sent.map((e) => e['type']), [
        'clipboard.chunk',
        'clipboard.item.end',
      ]);
      expect(bridge.closed, isTrue);
    },
  );

  testWidgets('native cancel followed by failure yields one failure terminal', (
    tester,
  ) async {
    pulls.offerPublished('unlogged-offer', [fixture.tree('unlogged-name', 3)]);
    bridge.platform(const ClipboardPullRequest(1, 'unlogged-offer', 0, 0));
    await tester.pump();
    bridge.platform(const ClipboardPullCancel(1));
    bridge.platform(const ClipboardPullFailed(1, 0, 'clipboard_file_locked'));
    await tester.pump(const Duration(seconds: 2));
    expect(events.map((e) => e.stage), [1, 6]);
    expect(events.last.code, 'clipboard_file_locked');
    expect(sent.last['type'], 'clipboard.cancel');
  });

  testWidgets(
    'closing while native delivery is pending suppresses every late observation',
    (tester) async {
      pulls.offerPublished('unlogged-offer', [
        fixture.tree('unlogged-name', 3),
      ]);
      bridge.platform(const ClipboardPullRequest(1, 'unlogged-offer', 0, 0));
      await tester.pump();
      bridge.dataGate = Completer<void>();
      final pending = pulls.handle(chunk(activeId()));
      await tester.pump();
      // The stand-in's StreamController was made by setUp in the real zone.
      // Run subscription cancellation there while data delivery remains gated.
      await tester.runAsync(() => pulls.close('clipboard_transport_closed'));
      final count = events.length;
      bridge.dataGate!.complete();
      await tester.pump();
      await pending;
      bridge.platform(const ClipboardPullRequest(2, 'unlogged-offer', 0, 0));
      await tester.pump(const Duration(seconds: 20));
      expect(events, hasLength(count));
      expect(events.where((e) => e.stage == 2), isEmpty);
    },
    timeout: const Timeout(Duration(seconds: 15)),
  );

  test('session pull evidence survives automatic diagnostic projection without private identifiers', () async {
    const testSessionId = '11111111-1111-4111-8111-111111111111';
    final reports = <Map<String, Object?>>[];
    final diagnostics = ClientDiagnostics(
      desktopId: 'test-vm',
      mode: 'stream',
      runId: '1' * 32,
      version: () => '0.4.97+111',
      send: (report) async {
        reports.add(report);
        return DiagnosticDelivery.accepted;
      },
    )..bindSession(testSessionId);
    final socket = base.ClipboardSocket();
    final native = onpaste.OnPasteClipboard();
    final session = ClipboardSession(
      policy: const ClipboardPolicy.all(),
      url: Uri.parse(
        'wss://cloud.example.com/api/v1/sessions/$testSessionId/clipboard',
      ),
      token: 'test-token',
      sessionId: testSessionId,
      bridge: native,
      dialer: (_, _) async => socket,
      pullLinger: Duration.zero,
    );
    try {
      final starting = session.start();
      await base.settle();
      socket.emit({...onpaste.onPasteReady(), 'sessionId': testSessionId});
      await base.settle();
      await starting;
      final receiving = session.receiveRemote();
      await base.settle();
      final id =
          socket.sent.lastWhere(
                (e) => e['type'] == 'clipboard.read',
              )['requestId']
              as String;
      socket.emit({
        'type': 'clipboard.offer',
        'requestId': id,
        'format': 'tree',
        'entries': 1,
        'files': 1,
        'bytes': 3,
        'tops': 1,
        'skipped': 0,
      });
      socket.emit({
        'type': 'clipboard.entries',
        'requestId': id,
        'start': 0,
        'entries': [
          {
            'path': 'never-report-this-name',
            'kind': 'file',
            'size': 3,
            'attrs': 32,
            'mtime': 1761500000000,
          },
        ],
      });
      await base.settle();
      socket.emit({'type': 'clipboard.complete', 'requestId': id});
      await base.settle();
      await receiving;
      native.platform(ClipboardPullRequest(9, id, 0, 0));
      await base.settle();
      final range =
          socket.sent.lastWhere(
                (e) => e['type'] == 'clipboard.pull',
              )['requestId']
              as String;
      socket.emit(chunk(range));
      socket.emit({
        'type': 'clipboard.item.end',
        'requestId': range,
        'index': 0,
        'sha256': 'a' * 64,
      });
      await base.settle();
      await Future<void>.delayed(const Duration(milliseconds: 2100));
      await diagnostics.flush();
      final rows = reports
          .expand((r) => r['events'] as List)
          .whereType<Map>()
          .where(
            (r) =>
                r['event'] == 'clipboard' &&
                (r['values'] as Map)['action'] == 12,
          )
          .toList();
      expect(rows.map((r) => (r['values'] as Map)['stage']), [1, 2, 4]);
      for (final row in rows) {
        expect(row['sessionId'], testSessionId);
        expect((row['values'] as Map).keys.toSet(), {
          'action',
          'stage',
          'ok',
          'remote',
          'requests',
          'failedRequests',
          'elapsedMs',
        });
        expect((row['values'] as Map)['requests'], 1);
      }
      expect(jsonEncode(reports), isNot(contains('never-report-this-name')));
      expect(
        rows.any((r) => r.containsKey('requestId') || r.containsKey('offerId')),
        isFalse,
      );
      var rangeId = 10;
      for (final (reason, code, detail) in [
        ('clipboard_offer_expired', 'clipboard_file_unavailable', 1),
        ('clipboard_selected_file_changed', 'clipboard_changed', 2),
      ]) {
        native.platform(ClipboardPullRequest(rangeId, id, 0, 0));
        await base.settle();
        native.platform(ClipboardPullCancel(rangeId));
        native.platform(ClipboardPullFailed(rangeId++, 0, reason));
        await base.settle();
        await Future<void>.delayed(const Duration(milliseconds: 2100));
        await diagnostics.flush();
        final failure = reports
            .expand((r) => r['events'] as List)
            .whereType<Map>()
            .lastWhere(
              (r) =>
                  r['event'] == 'clipboard' &&
                  (r['values'] as Map)['action'] == 12 &&
                  (r['values'] as Map)['stage'] == 6,
            );
        expect(failure['code'], code);
        expect((failure['values'] as Map)['error'], detail);
        expect((failure['values'] as Map)['ok'], isFalse);
      }
      // Tuning #7: a paste cancelled on this PC (stage 5) is no failure.
      native.platform(ClipboardPullRequest(rangeId, id, 0, 0));
      await base.settle();
      native.platform(ClipboardPullCancel(rangeId++));
      await base.settle();
      await Future<void>.delayed(const Duration(milliseconds: 2100));
      await diagnostics.flush();
      final cancelled = reports
          .expand((r) => r['events'] as List)
          .whereType<Map>()
          .lastWhere(
            (r) =>
                r['event'] == 'clipboard' &&
                (r['values'] as Map)['action'] == 12 &&
                (r['values'] as Map)['stage'] == 5,
          );
      expect(cancelled['code'], 'clipboard_cancelled');
      expect((cancelled['values'] as Map)['ok'], isTrue);
      native.platform(ClipboardPullRequest(99, id, 0, 0));
      await base.settle();
      await diagnostics.flush();
      List<Map> pullRows() => reports
          .expand((r) => r['events'] as List)
          .whereType<Map>()
          .where(
            (r) =>
                r['event'] == 'clipboard' &&
                [12, 13].contains((r['values'] as Map)['action']),
          )
          .toList();
      final beforeClose = pullRows().length;
      await session.close(reason: 'clipboard_transport_closed');
      native.platform(
        const ClipboardPullFailed(99, 0, 'clipboard_file_locked'),
      );
      await base.settle();
      await diagnostics.flush();
      expect(
        pullRows(),
        hasLength(beforeClose),
        reason: 'closed sessions emit no new pull observations, even their close terminal',
      );
      expect(
        reports
            .expand((r) => r['events'] as List)
            .whereType<Map>()
            .any(
              (r) =>
                  r['event'] == 'clipboard' &&
                  (r['values'] as Map)['action'] == 3 &&
                  r['code'] == 'clipboard_transport_closed',
            ),
        isTrue,
      );
    } finally {
      await session.close();
      diagnostics.close();
    }
  }, timeout: const Timeout(Duration(seconds: 15)));
}
