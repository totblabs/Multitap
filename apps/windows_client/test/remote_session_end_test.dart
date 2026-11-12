// Fixed end-cause evidence only: no network, native clipboard, USB or media.
import 'dart:async';
import 'dart:convert';
import 'dart:io';

import 'package:flutter_test/flutter_test.dart';
import 'package:multitap_client/data/product_log.dart';

import 'remote_session_test.dart' show SessionSocket;
import 'session_recovery_test.dart' show RecoveryMedia, session, connected;

class _ClosingSocket extends SessionSocket {
  String? reason;
  @override
  String? get closeReason => reason;
}

void main() {
  final ends = <ProductLogEntry>[];
  StreamSubscription<ProductLogEntry>? subscription;
  setUp(() {
    ends.clear();
    subscription = ProductLog.instance.localRecords.listen((entry) {
      if (entry.event == LogEvent.streamEnd) ends.add(entry);
    });
  });
  tearDown(() => subscription?.cancel());

  testWidgets(
    'A local stop records one successful client_stopped before cleanup',
    (t) async {
      final socket = _ClosingSocket(), media = RecoveryMedia();
      final current = session(socket, media);
      await connected(t, current, socket);
      await current.close();
      await current.close();
      await t.pump();
      expect(ends, hasLength(1));
      expect(ends.single.code, 'client_stopped');
      expect(ends.single.values['ok'], isTrue);
      expect(current.endedByHostLoss, isFalse);
      await media.events.close();
    },
  );

  for (final reason in <String?>[
    null,
    '',
    'PRIVATE_TOKEN_SENTINEL',
    'clipboard_write_failed',
  ]) {
    testWidgets(
      'Bare relay EOF uses a fixed failure, never arbitrary or feature text: $reason',
      (t) async {
        final socket = _ClosingSocket()..reason = reason;
        final media = RecoveryMedia();
        final current = session(socket, media);
        await connected(t, current, socket);
        unawaited(socket.incoming.close());
        await t.pump();
        await t.pump();
        expect(current.closed, isTrue);
        expect(current.diagnosticCode, 'relay_closed');
        expect(current.canRetry, isTrue);
        expect(current.endedByHostLoss, isFalse);
        expect(current.status, '연결이 끊겼어요. 다시 연결하세요.');
        expect(ends, hasLength(1));
        expect(ends.single.code, 'relay_closed');
        expect(ends.single.values['ok'], isFalse);
        expect(
          jsonEncode(ends.single.toJson()),
          isNot(contains('PRIVATE_TOKEN')),
        );
        await current.close();
        await media.events.close();
      },
    );
  }

  for (final reason in [
    'host_error',
    'authorization_revoked',
    'host_revoked',
  ]) {
    testWidgets(
      'A fixed close reason survives missing JSON, with policy retry gates: $reason',
      (t) async {
        final socket = _ClosingSocket()..reason = reason;
        final media = RecoveryMedia();
        final current = session(socket, media);
        await connected(t, current, socket);
        unawaited(socket.incoming.close());
        await t.pump();
        await t.pump();
        expect(current.closed, isTrue);
        expect(current.diagnosticCode, reason);
        expect(current.canRetry, reason == 'host_error');
        expect(current.endedByHostLoss, reason == 'host_error');
        expect(ends, hasLength(1));
        expect(ends.single.code, reason);
        await current.close();
        await media.events.close();
      },
    );
  }

  testWidgets(
    'Explicit relay detail wins over socket cleanup and repeated close',
    (t) async {
      final socket = _ClosingSocket()..reason = 'client_stopped';
      final media = RecoveryMedia();
      final current = session(socket, media);
      await connected(t, current, socket);
      socket.incoming.add(
        jsonEncode({
          'type': 'session.ended',
          'reason': 'host_error',
          'detail': 'host_frame_invalid',
        }),
      );
      await t.pump();
      await t.pump();
      await current.close();
      expect(current.diagnosticCode, 'host_frame_invalid');
      expect(current.endedByHostLoss, isTrue);
      expect(ends, hasLength(1));
      expect(ends.single.code, 'host_frame_invalid');
      expect(ends.single.values['ok'], isFalse);
      await media.events.close();
    },
  );

  // 2026-11-09 edge-case audit: a takeover says who; the code stays
  // taken_over (no automatic reconnect, an ordinary end).
  testWidgets('A takeover end names the other account', (t) async {
    final socket = _ClosingSocket(), media = RecoveryMedia();
    final current = session(socket, media);
    await connected(t, current, socket);
    socket.incoming.add(
      jsonEncode({
        'type': 'session.ended',
        'reason': 'taken_over',
        'detail': 'other_account',
        'by': 'bob',
      }),
    );
    await t.pump();
    await t.pump();
    await current.close();
    expect(current.diagnosticCode, 'taken_over');
    expect(current.status, startsWith('bob 계정이 다른 창이나 기기에서'));
    expect(current.canAutomaticallyReconnect, isFalse);
    await media.events.close();
  });

  // Tuning #7: an end someone meant is ok, its code kept as information.
  for (final (reason, ok) in [
    ('taken_over', true),
    ('host_update', true),
    ('display_reconfigure', true),
    ('clipboard_policy_changed', true),
    ('authorization_revoked', true),
    ('server_stopping', true),
    ('host_timeout', false),
    ('client_backpressure', false),
  ]) {
    testWidgets('A relay end is ok only when meant: $reason', (t) async {
      final socket = _ClosingSocket(), media = RecoveryMedia();
      final current = session(socket, media);
      await connected(t, current, socket);
      socket.incoming.add(
        jsonEncode({'type': 'session.ended', 'reason': reason}),
      );
      await t.pump();
      await t.pump();
      await current.close();
      expect(ends, hasLength(1));
      expect(ends.single.code, reason);
      expect(ends.single.values['ok'], ok);
      await media.events.close();
    });
  }

  testWidgets(
    'A socket exception records network_error without exception text',
    (t) async {
      final socket = _ClosingSocket(), media = RecoveryMedia();
      final current = session(socket, media);
      await connected(t, current, socket);
      socket.incoming.addError(const SocketException('PRIVATE_TOKEN_SENTINEL'));
      await t.pump();
      await t.pump();
      expect(current.closed, isTrue);
      expect(current.diagnosticCode, 'network_error');
      expect(ends, hasLength(1));
      expect(ends.single.code, 'network_error');
      expect(
        jsonEncode(ends.single.toJson()),
        isNot(contains('PRIVATE_TOKEN')),
      );
      await current.close();
      await media.events.close();
    },
  );
}
