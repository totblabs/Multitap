// Real shared session and Windows widgets; all sockets/media/reservations fake.
import 'dart:async';
import 'dart:convert';
import 'dart:io';

import 'package:flutter/material.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:multitap_client/data/broker_api.dart';
import 'package:multitap_client/data/remote_session.dart';
import 'package:multitap_client/data/session_failure.dart';
import 'package:multitap_client/models/stream_options.dart';
import 'package:multitap_client/ui/remote_view.dart';

import 'display_change_robustness_test.dart' show ViewFixture, mockWindow;
import 'display_remote_test.dart' show machine;
import 'remote_session_test.dart' show SessionSocket;
import 'session_recovery_test.dart' show RecoveryMedia, session, connected;
import 'viewport_session_test.dart' show ResizeMedia;

void main() {
  for (final status in [503, 200, 401]) {
    testWidgets(
      'Windows server_stopping ${status == 503 ? 'outwaits HTTP503' : 'stops on HTTP$status'} in its first refresh',
      (t) async {
        t.view.physicalSize = const Size(800, 600);
        t.view.devicePixelRatio = 1;
        addTearDown(t.view.reset);
        await mockWindow(t);
        final fixtures = <ViewFixture>[], released = <String>[];
        ViewFixture create() {
          final socket = SessionSocket(), media = ResizeMedia();
          final id = 'gateway-${fixtures.length}';
          final current = RemoteSession(
            connection: BrokerConnection(
              sessionId: id,
              webSocketUrl: Uri.parse('wss://fixture.invalid/sessions/$id'),
              reservation: BrokerSessionReservation(
                () async => released.add(id),
              ),
              epochMedia: true,
              viewportProvisioning: true,
              options: const StreamOptions(),
            ),
            token: 'fixture-only',
            media: media,
            dialer: (_, _) async => socket,
          );
          final fixture = ViewFixture(socket, media, current);
          fixtures.add(fixture);
          return fixture;
        }

        final first = create();
        var reads = 0, reconnects = 0;
        await t.pumpWidget(
          MaterialApp(
            home: RemoteView(
              machine: machine(1),
              load: () async => first.session,
              onRefreshDesktop: () async {
                if (++reads == 2) {
                  throw BrokerException(
                    status == 401 ? 'session_expired' : 'invalid_response',
                    status,
                  );
                }
                return machine(1);
              },
              onReconnect: (_) async {
                expect(released, ['gateway-0']);
                reconnects++;
                return create().session;
              },
            ),
          ),
        );
        await t.pump();
        await t.pump();
        try {
          await first.initialFrame(t);
          first.socket.incoming.add(
            jsonEncode({'type': 'session.ended', 'reason': 'server_stopping'}),
          );
          for (var pass = 0; pass < 40 && reconnects == 0; pass++) {
            await t.pump(const Duration(milliseconds: 100));
          }
          expect(first.session.diagnosticCode, 'server_stopping');
          expect(reconnects, status == 503 ? 1 : 0);
          expect(reads, status == 503 ? 3 : 2);
          expect(released, [
            'gateway-0',
          ], reason: 'Gateway polling cannot replay DELETE.');
          if (status == 503) {
            await t.pump();
            await fixtures.last.initialFrame(t);
          } else {
            await t.pump(const Duration(seconds: 31));
            expect(reconnects, 0);
            expect(reads, 2);
          }
        } finally {
          await t.pumpWidget(const SizedBox());
          for (final fixture in fixtures) {
            unawaited(fixture.session.close());
          }
          await t.pump(const Duration(seconds: 3));
          for (final fixture in fixtures) {
            await fixture.media.events.close();
          }
        }
      },
    );
  }

  test('Only known transient ends qualify; policy/protocol and ambiguous handshake errors do not', () {
    for (final code in [
      'relay_closed',
      'network_error',
      'host_timeout',
      'server_stopping',
    ]) {
      expect(SessionFailure.transientTransportEnd(code), isTrue, reason: code);
    }
    for (final code in [
      'authorization_revoked',
      'session_expired',
      'host_revoked',
      'clipboard_policy_changed',
      'taken_over',
      'invalid_control',
      'invalid_media',
      'invalid_stream_message',
      'host_protocol_rejected',
      'websocket_handshake_or_transport',
      'session_ended',
      'client_stopped',
      'PRIVATE_TEXT',
    ]) {
      expect(SessionFailure.transientTransportEnd(code), isFalse, reason: code);
    }
  });

  for (final code in [1002, 1003, 1007, 1008, 1010]) {
    testWidgets(
      'A protocol/policy close $code cannot automatically reconnect even without JSON',
      (t) async {
        final socket = SessionSocket()
          ..closeCode = code
          ..closeReason = 'PRIVATE_TEXT';
        final media = RecoveryMedia();
        final current = session(socket, media);
        await connected(t, current, socket);
        unawaited(socket.incoming.close());
        await t.pump();
        await t.pump();
        expect(current.diagnosticCode, 'relay_closed');
        expect(current.endedByHostLoss, isFalse);
        expect(current.canAutomaticallyReconnect, isFalse);
        expect(current.status, isNot(contains('PRIVATE_TEXT')));
        await current.close();
        await media.events.close();
      },
    );
  }

  for (final reason in [
    'authorization_revoked',
    'clipboard_policy_changed',
    'taken_over',
    'invalid_control',
  ]) {
    testWidgets('An explicit decision $reason stays stopped', (t) async {
      final socket = SessionSocket(), media = RecoveryMedia();
      final current = session(socket, media);
      await connected(t, current, socket);
      socket.incoming.add(
        jsonEncode({'type': 'session.ended', 'reason': reason}),
      );
      await t.pump();
      await t.pump();
      expect(current.closed, isTrue);
      expect(current.canAutomaticallyReconnect, isFalse);
      final firstCause = current.diagnosticCode;
      await current.networkLost();
      expect(
        current.diagnosticCode,
        firstCause,
        reason: 'OS notification cannot erase the authoritative end.',
      );
      await media.events.close();
    });
  }

  testWidgets(
    'Observed network loss is retryable without a Host-loss claim; an explicit stop is not',
    (t) async {
      final socket = SessionSocket(), media = RecoveryMedia();
      final current = session(socket, media);
      await connected(t, current, socket);
      await current.networkLost();
      expect(current.diagnosticCode, 'network_error');
      expect(current.endedByHostLoss, isFalse);
      expect(current.canAutomaticallyReconnect, isTrue);
      await current.close();
      expect(current.diagnosticCode, 'network_error');
      await media.events.close();
      final stoppedSocket = SessionSocket(), stoppedMedia = RecoveryMedia();
      final stopped = session(stoppedSocket, stoppedMedia);
      await connected(t, stopped, stoppedSocket);
      await stopped.close();
      expect(stopped.diagnosticCode, 'client_stopped');
      expect(stopped.canAutomaticallyReconnect, isFalse);
      await stoppedMedia.events.close();
    },
  );

  for (final networkError in [false, true]) {
    testWidgets(
      'Windows ${networkError ? 'socket error' : 'bare EOF'} reconnects once with an exact old release and no Host claim',
      (t) async {
        t.view.physicalSize = const Size(800, 600);
        t.view.devicePixelRatio = 1;
        addTearDown(t.view.reset);
        await mockWindow(t);
        final fixtures = <ViewFixture>[], released = <String>[];
        ViewFixture create() {
          final socket = SessionSocket(), media = ResizeMedia();
          final id = 'transport-${fixtures.length}';
          final current = RemoteSession(
            connection: BrokerConnection(
              sessionId: id,
              webSocketUrl: Uri.parse('wss://fixture.invalid/sessions/$id'),
              reservation: BrokerSessionReservation(
                () async => released.add(id),
              ),
              epochMedia: true,
              viewportProvisioning: true,
              options: const StreamOptions(),
            ),
            token: 'fixture-only',
            media: media,
            dialer: (_, _) async => socket,
          );
          final fixture = ViewFixture(socket, media, current);
          fixtures.add(fixture);
          return fixture;
        }

        final first = create();
        var reconnects = 0;
        await t.pumpWidget(
          MaterialApp(
            home: RemoteView(
              machine: machine(1),
              load: () async => first.session,
              onRefreshDesktop: () async => machine(1),
              onReconnect: (_) async {
                expect(released, ['transport-0']);
                reconnects++;
                return create().session;
              },
            ),
          ),
        );
        await t.pump();
        await t.pump();
        try {
          await first.initialFrame(t);
          if (networkError) {
            first.socket.incoming.addError(
              const SocketException('PRIVATE_TEXT'),
            );
          } else {
            unawaited(first.socket.incoming.close());
          }
          for (var pass = 0; pass < 12 && reconnects == 0; pass++) {
            await t.pump(const Duration(milliseconds: 100));
          }
          expect(first.session.endedByHostLoss, isFalse);
          expect(
            first.session.diagnosticCode,
            networkError ? 'network_error' : 'relay_closed',
          );
          expect(reconnects, 1);
          await t.pump();
          expect(find.text('연결이 끊겨 다시 연결하고 있어요'), findsOneWidget);
          expect(find.text('Host 연결이 끊겨 다시 연결하고 있어요'), findsNothing);
          final second = fixtures.last;
          await t.pump();
          await second.initialFrame(t);
          unawaited(second.socket.incoming.close());
          await t.pump(const Duration(seconds: 2));
          await t.pump();
          expect(
            reconnects,
            1,
            reason: 'The existing once-per-minute bound remains.',
          );
          expect(
            find.byKey(const Key('retry-remote-connection')),
            findsOneWidget,
          );
        } finally {
          await t.pumpWidget(const SizedBox());
          for (final fixture in fixtures) {
            unawaited(fixture.session.close());
          }
          await t.pump(const Duration(seconds: 3));
          for (final fixture in fixtures) {
            await fixture.media.events.close();
          }
        }
      },
    );
  }
}
