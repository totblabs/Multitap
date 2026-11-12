// In-memory logger only; no native engine, network, clipboard or filesystem.
import 'dart:convert';

import 'package:flutter_test/flutter_test.dart';
import 'package:multitap_client/data/client_diagnostics.dart';
import 'package:multitap_client/data/product_log.dart';

const _old = 'aaaaaaaa-aaaa-aaaa-aaaa-aaaaaaaaaaaa';
const _explicit = 'cccccccc-cccc-cccc-cccc-cccccccccccc';
const _request = 'bbbbbbbb-bbbb-bbbb-bbbb-bbbbbbbbbbbb';

void main() {
  test('Declining request inheritance preserves bound feature events and explicit session identity', () async {
    final log = ProductLog(), reports = <Map<String, Object?>>[];
    final reporter = ClientDiagnostics(
      log: log,
      desktopId: 'fixture-vm',
      mode: 'stream',
      interval: const Duration(hours: 1),
      send: (report) async {
        reports.add(report);
        return DiagnosticDelivery.accepted;
      },
    );
    addTearDown(() {
      reporter.close();
      log.dispose();
    });
    reporter.bindSession(_old);
    reporter.record(
      'brokerRequest',
      requestId: _request,
      inheritSession: false,
      values: {'ok': false, 'stage': 3},
    );
    reporter.record('clipboard', values: {'ok': true});
    reporter.record(
      'brokerRequest',
      sessionId: _explicit,
      inheritSession: false,
    );
    reporter.record('usb', values: {'ok': false});
    await reporter.flush();
    final rows = (reports.single['events'] as List).skip(1).toList();
    expect(rows[0].containsKey('sessionId'), isFalse);
    expect(rows[0]['requestId'], _request);
    expect(rows[1]['sessionId'], _old);
    expect(rows[2]['sessionId'], _explicit);
    expect(
      rows[3]['sessionId'],
      _old,
      reason: 'Explicit per-row identity must not replace the bound session',
    );
    expect(jsonEncode(reports), isNot(contains('inheritSession')));
  });

  test('Scoped invalid identifiers and unsafe values remain excluded without clearing the prior binding', () async {
    final log = ProductLog(), reports = <Map<String, Object?>>[];
    final reporter = ClientDiagnostics(
      log: log,
      desktopId: 'fixture-vm',
      mode: 'stream',
      interval: const Duration(hours: 1),
      send: (report) async {
        reports.add(report);
        return DiagnosticDelivery.accepted;
      },
    );
    addTearDown(() {
      reporter.close();
      log.dispose();
    });
    reporter.bindSession(_old);
    reporter.bindSession('PRIVATE-INVALID', desktop: 'fixture-vm');
    reporter.record(
      'brokerRequest',
      sessionId: 'PRIVATE-INVALID',
      requestId: 'PRIVATE-REQUEST',
      operationId: 'PRIVATE-OPERATION',
      inheritSession: false,
      code: 'PRIVATE-FAILURE',
      values: {
        'ok': false,
        'stage': 3,
        'token': 'PRIVATE',
        'filename': 'PRIVATE',
      },
    );
    reporter.record('clipboard');
    await reporter.flush();
    final rows = reports.single['events'] as List;
    expect(rows[1].containsKey('sessionId'), isFalse);
    expect(rows[1].containsKey('requestId'), isFalse);
    expect(rows[1].containsKey('operationId'), isFalse);
    expect(rows[1]['values'], {'ok': false, 'stage': 3});
    expect(rows[1]['code'], 'unknown_failure');
    expect(rows[2]['sessionId'], _old);
    expect(jsonEncode(reports), isNot(contains('PRIVATE')));
  });
}
