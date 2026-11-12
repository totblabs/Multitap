import 'dart:io';

import 'package:flutter_test/flutter_test.dart';
import 'package:multitap_client/data/client_diagnostic_codes.dart';
import 'package:multitap_client/data/session_failure.dart';
import 'package:multitap_client/data/stream_stall.dart';
import 'package:multitap_client/data/stream_stats_report.dart';

void main() {
  test('A relay end keeps its own reason; an unknown one keeps the grouped code', () {
    final grouped = SessionFailure.ended('tracks_source_stalled').code;
    expect(grouped, 'session_ended', reason: 'The message stays grouped.');
    expect(relayEndCode('tracks_source_stalled', grouped), 'tracks_source_stalled');
    expect(relayEndCode('client_stopped', grouped), 'client_stopped');
    expect(relayEndCode('PRIVATE_SENTINEL', grouped), 'session_ended');
    expect(relayEndCode(null, grouped), 'session_ended');
    expect(relayEndCode(42, grouped), 'session_ended');
  });

  test('A Host end records the relay detail; released relays send only the reason', () {
    // Without the detail every host_error looks the same (as with 4K video).
    final failure = SessionFailure.ended('host_error');
    expect(failure.code, 'host_disconnected');
    expect(relayEndCode('host_error', failure.code, 'host_frame_invalid'), 'host_frame_invalid');
    expect(relayEndCode('host_error', failure.code), 'host_error');
    expect(relayEndCode('host_error', failure.code, 'PRIVATE_SENTINEL'), 'host_error');
    expect(relayEndCode('host_disconnected', failure.code, 'host_service_stopping'), 'host_service_stopping');
  });

  test('Broker-initiated ends explain themselves instead of a generic "connection ended"', () {
    final generic = SessionFailure.ended('unspecified_reason');
    final failure = SessionFailure.ended('host_update');
    expect(failure.code, 'host_update');
    expect(failure.message, isNot(generic.message));
    expect(relayEndCode('host_update', failure.code), 'host_update');
    expect(failure.message, contains('업데이트'));
  });

  test('Every streaming statistics field is one the broker keeps', () {
    final server = File(
      '../../server/src/client-diagnostics.mjs',
    ).readAsStringSync();
    final quoted = RegExp(r"'([A-Za-z0-9_]+)'").allMatches(server).map((m) => m[1]).toSet();
    final observability = File(
      '../../server/src/stream-observability.mjs',
    ).readAsStringSync();
    expect(
      server,
      contains("import { observabilityDiagnosticBounds } from './stream-observability.mjs'"),
    );
    expect(
      server,
      contains('const streamStatsNumeric = [...Object.keys(observabilityDiagnosticBounds),'),
    );
    final bounds = RegExp(
      r'export const (?:hostObservabilityBounds|observabilityDiagnosticBounds) = Object\.freeze\(\{([\s\S]*?)\}\);',
    ).allMatches(observability).map((match) => match[1]!).toList();
    expect(bounds, hasLength(2));
    expect(bounds.last, contains('...hostObservabilityBounds'));
    final imported = RegExp(
      r'\b([A-Za-z][A-Za-z0-9_]*)\s*:\s*(?:0x[0-9a-fA-F]+|[0-9]+)\b',
    ).allMatches(bounds.join('\n')).map((match) => match[1]).toSet();
    // The broker's udp-media-v1 bounds are spread into the list by key.
    final udpBlock = RegExp(
      r'const udpProbeBounds = Object\.freeze\(\{([\s\S]*?)\}\);',
    ).firstMatch(server);
    expect(server, contains('...Object.keys(udpProbeBounds)'));
    final udp = RegExp(
      r'\b([A-Za-z][A-Za-z0-9_]*)\s*:\s*(?:0x[0-9a-fA-F]+|[0-9]+|maxUdpRelays)\b',
    ).allMatches(udpBlock![1]!).map((match) => match[1]).toSet();
    final accepted = {...quoted, ...imported, ...udp};
    final missing = streamStatsFields.where((field) => !accepted.contains(field)).toList();
    expect(missing, isEmpty);
    // streamStall rows too (stall-v2 added stallPtsGapMs and hostChangeLateMs).
    expect(streamStallFields.where((field) => !accepted.contains(field)), isEmpty);
  });

  test('Every client diagnostic code is one the broker keeps (not unknown_failure)', () {
    final server = File(
      '../../server/src/client-diagnostics.mjs',
    ).readAsStringSync();
    final quoted = RegExp(r"'([a-z0-9_]+)'").allMatches(server).map((m) => m[1]).toSet();
    final missing = diagnosticCodes.where((code) => !quoted.contains(code)).toList();
    expect(missing, isEmpty);
  });
}
