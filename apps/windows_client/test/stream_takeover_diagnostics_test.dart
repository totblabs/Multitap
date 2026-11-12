import 'dart:convert';

import 'package:flutter_test/flutter_test.dart';
import 'package:multitap_client/data/client_diagnostics.dart';

void main() {
  test(
    'takeover diagnostics retain fixed failures and drop context/identity text',
    () async {
      final reports = <Map<String, Object?>>[];
      final diagnostics = ClientDiagnostics(
        send: (report) async {
          reports.add(report);
          return DiagnosticDelivery.accepted;
        },
        interval: const Duration(days: 1),
      );
      addTearDown(diagnostics.close);
      const codes = [
        'takeover_context_stale',
        'takeover_context_expired',
        'invalid_takeover_context',
        'invalid_context_protocol',
        'session_context_unavailable',
      ];
      for (final code in codes) {
        diagnostics.record(
          'brokerRequest',
          code: code,
          values: {
            'ok': false,
            'stage': 3,
            'context': 'opaque-sensitive-context',
            'username': 'sensitive-account-name',
            'deviceLabel': 'sensitive-device-name',
            'takeoverOffer': {'context': 'opaque-sensitive-context'},
          },
        );
      }
      await diagnostics.flush();
      final serialized = jsonEncode(reports);
      for (final code in codes) {
        expect(serialized, contains(code));
      }
      for (final secret in [
        'opaque-sensitive-context',
        'sensitive-account-name',
        'sensitive-device-name',
        'takeoverOffer',
      ]) {
        expect(serialized, isNot(contains(secret)));
      }
    },
  );
}
