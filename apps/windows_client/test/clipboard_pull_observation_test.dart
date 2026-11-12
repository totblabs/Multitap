import 'package:flutter_test/flutter_test.dart';
import 'package:multitap_client/data/clipboard_pull_observation.dart';
import 'package:multitap_client/data/client_diagnostic_codes.dart';

void main() {
  for (final (reason, code, detail) in [
    ('clipboard_offer_expired', 'clipboard_file_unavailable', 1),
    ('clipboard_selected_file_changed', 'clipboard_changed', 2),
  ]) {
    testWidgets('$reason retains its detail using a released diagnostic code', (
      tester,
    ) async {
      final events = <ClipboardPullObservation>[];
      final observer = ClipboardPullObserver(sending: false, emit: events.add);
      observer.failed(observer.request(), reason);
      expect(events.last.code, reason);
      expect(events.last.diagnosticCode, code);
      expect(events.last.diagnosticDetail, detail);
      expect(diagnosticCodes, contains(code));
      observer.close('clipboard_session_closed');
    });
  }

  test('the two unknown pull-wire reasons map for action 8 too; others pass', () {
    expect(
      clipboardPullDiagnosticCode('clipboard_offer_expired'),
      'clipboard_file_unavailable',
    );
    expect(clipboardPullDiagnosticDetail('clipboard_offer_expired'), 1);
    expect(
      clipboardPullDiagnosticCode('clipboard_selected_file_changed'),
      'clipboard_changed',
    );
    expect(clipboardPullDiagnosticDetail('clipboard_selected_file_changed'), 2);
    expect(
      clipboardPullDiagnosticCode('clipboard_file_locked'),
      'clipboard_file_locked',
    );
    expect(clipboardPullDiagnosticDetail('clipboard_file_locked'), 0);
  });

  testWidgets(
    'ranges failing together in one episode claim one failure report; a new episode claims again',
    (tester) async {
      final events = <ClipboardPullObservation>[];
      final observer = ClipboardPullObserver(sending: true, emit: events.add);
      final ranges = [for (var i = 0; i < 4; i++) observer.request()];
      final claims = <bool>[];
      for (final range in ranges) {
        claims.add(observer.claimFailureReport(range));
        observer.failed(range, 'clipboard_offer_expired');
      }
      expect(claims, [true, false, false, false]);
      expect(events.map((e) => e.stage), [1, 6]);
      await tester.pump(const Duration(seconds: 2));
      final next = observer.request();
      expect(observer.claimFailureReport(next), isTrue);
      observer.close('clipboard_session_closed');
    },
  );

  testWidgets('explicit waiting retry has a fresh observational epoch', (
    tester,
  ) async {
    final events = <ClipboardPullObservation>[];
    final observer = ClipboardPullObserver(sending: true, emit: events.add);
    observer.waiting(true);
    final old = observer.request();
    observer.failed(old, 'clipboard_file_locked');
    observer.waiting(false);
    observer.waiting(true);
    final retry = observer.request();
    final count = events.length;
    observer.data(old);
    observer.rangeEnded(old);
    expect(events, hasLength(count));
    observer.data(retry);
    observer.rangeEnded(retry);
    observer.waiting(false);
    await tester.pump(const Duration(seconds: 2));
    expect(events.map((e) => e.stage), [1, 6, 1, 2, 4]);
    observer.close('clipboard_session_closed');
  });

  testWidgets(
    '20,000 completed ranges have one bounded episode, not 20,000 logs',
    (tester) async {
      final events = <ClipboardPullObservation>[];
      final observer = ClipboardPullObserver(sending: false, emit: events.add);
      for (var i = 0; i < 20000; i++) {
        final token = observer.request();
        observer.data(token);
        observer.rangeEnded(token);
      }
      expect(events.map((e) => e.stage), [1, 2]);
      await tester.pump(const Duration(seconds: 2));
      expect(events.map((e) => e.stage), [1, 2, 4]);
      expect(events.last.requests, 20000);
      expect(events.last.failedRequests, 0);
      observer.close('clipboard_session_closed');
    },
  );

  testWidgets('long ranges emit at most twelve periodic summaries', (
    tester,
  ) async {
    final events = <ClipboardPullObservation>[];
    final observer = ClipboardPullObserver(sending: true, emit: events.add);
    final token = observer.request();
    observer.data(token);
    await tester.pump(const Duration(minutes: 5));
    expect(events.where((e) => e.stage == 3), hasLength(12));
    expect(events.where((e) => e.stage >= 4), isEmpty);
    observer.rangeEnded(token);
    await tester.pump(const Duration(seconds: 2));
    expect(events.last.stage, 4);
    expect(events, hasLength(15));
    observer.close('clipboard_session_closed');
  });

  testWidgets(
    'late old range events cannot contaminate a newer episode or closed owner',
    (tester) async {
      final events = <ClipboardPullObservation>[];
      final observer = ClipboardPullObserver(sending: false, emit: events.add);
      final old = observer.request();
      observer.rangeEnded(old);
      await tester.pump(const Duration(seconds: 2));
      final fresh = observer.request();
      final count = events.length;
      observer.data(old);
      observer.failed(old, 'clipboard_file_locked');
      observer.rangeEnded(old);
      expect(events, hasLength(count));
      observer.data(fresh);
      expect(events.last.stage, 2);
      observer.close('clipboard_transport_closed');
      final closedCount = events.length;
      observer.data(fresh);
      observer.rangeEnded(fresh);
      observer.failed(fresh, 'clipboard_read_failed');
      observer.request();
      await tester.pump(const Duration(minutes: 1));
      expect(events, hasLength(closedCount));
    },
  );

  testWidgets(
    'native failure after cancel wins over a misleading cancellation terminal',
    (tester) async {
      final events = <ClipboardPullObservation>[];
      final observer = ClipboardPullObserver(sending: false, emit: events.add);
      final token = observer.request();
      observer.cancelRange(token);
      observer.failed(token, 'clipboard_file_locked');
      await tester.pump(const Duration(seconds: 2));
      expect(events.map((e) => e.stage), [1, 6]);
      expect(events.last.code, 'clipboard_file_locked');
      expect(events.last.failedRequests, 1);
      observer.close('clipboard_session_closed');
    },
  );

  testWidgets(
    'waiting holds adjacent served ranges until the waiting episode ends',
    (tester) async {
      final events = <ClipboardPullObservation>[];
      final observer = ClipboardPullObserver(sending: true, emit: events.add);
      observer.waiting(true);
      observer.rangeEnded(observer.request());
      await tester.pump(const Duration(seconds: 6));
      expect(events.where((e) => e.stage == 4), isEmpty);
      observer.rangeEnded(observer.request());
      observer.waiting(false);
      await tester.pump(const Duration(seconds: 2));
      expect(events.last.stage, 4);
      expect(events.last.requests, 2);
      observer.close('clipboard_session_closed');
    },
  );

  testWidgets('many separated pastes still respect the owner event budget', (
    tester,
  ) async {
    final events = <ClipboardPullObservation>[];
    final observer = ClipboardPullObserver(sending: false, emit: events.add);
    for (var i = 0; i < 100; i++) {
      final token = observer.request();
      observer.data(token);
      observer.rangeEnded(token);
      await tester.pump(const Duration(seconds: 2));
    }
    expect(events, hasLength(64));
    expect(events.last.stage, 4);
    observer.close('clipboard_session_closed');
  });

  testWidgets('diagnostic callback failure cannot interrupt a range', (
    tester,
  ) async {
    final observer = ClipboardPullObserver(
      sending: false,
      emit: (_) => throw StateError('test only'),
    );
    final token = observer.request();
    observer.data(token);
    observer.rangeEnded(token);
    await tester.pump(const Duration(seconds: 2));
    observer.close('clipboard_session_closed');
  });
}
