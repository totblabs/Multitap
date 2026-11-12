import 'dart:async';
import 'dart:convert';
import 'dart:io';

import 'package:flutter/material.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:multitap_client/data/broker_api.dart';
import 'package:multitap_client/data/console_resume.dart';
import 'package:multitap_client/models/console_preparation.dart';
import 'package:multitap_client/models/desktop_machine.dart';
import 'package:multitap_client/models/stream_preparation.dart';
import 'package:multitap_client/ui/app_theme.dart';
import 'package:multitap_client/ui/console_resume_dialog.dart';

import 'power_api_test.dart' show ApiClient, taskId;

Map<String, dynamic> response({
  String id = taskId,
  String status = 'pending',
  String? error,
  bool? attempted,
}) => {
  'consolePreparation': {
    'supported': true,
    'available': status == 'failed',
    'busy': status == 'pending',
    'reason': status == 'succeeded'
        ? 'console_ready'
        : 'rdp_session_disconnected',
    'operation': {
      'id': id,
      'status': status,
      'attempted': status == 'succeeded' ? true : attempted,
      'error': error,
    },
  },
};
ConsolePreparationState state({
  String status = 'pending',
  String id = taskId,
  String? error,
}) => ConsolePreparationState.fromJson(
  response(status: status, id: id, error: error)['consolePreparation'],
);

class RecoveryApi implements ConsoleRecoveryApi {
  int posts = 0, polls = 0;
  Future<ConsolePreparationState> Function()? start;
  Future<ConsolePreparationState> Function()? poll;
  @override
  Future<ConsolePreparationState> prepareConsole(
    String token,
    String desktopId,
  ) async {
    posts++;
    return start == null ? state() : await start!();
  }

  @override
  Future<ConsolePreparationState> consolePreparation(
    String token,
    String desktopId, {
    String? operationId,
  }) async {
    polls++;
    expect(operationId, taskId);
    return poll == null ? state(status: 'succeeded') : await poll!();
  }
}

BrokerDesktop desktop({
  bool ready = false,
  String id = taskId,
  bool locked = false,
}) => BrokerDesktop(
  id: 'pve-149',
  name: 'TESTER',
  kind: 'vm',
  description: '',
  os: 'Windows',
  status: 'ready',
  canConnect: true,
  canStream: ready,
  hostConnected: true,
  interactiveSession: ready,
  virtualDisplayReady: ready,
  consolePreparation: locked
      ? ConsolePreparationState(
          supported: true,
          reason: 'windows_session_locked',
          operation: ConsolePreparationOperation(
            id: id,
            status: 'succeeded',
            attempted: true,
          ),
        )
      : state(status: 'succeeded', id: id),
);
const machine = DesktopMachine(
  id: 'pve-149',
  name: 'TESTER',
  description: '',
  kind: MachineKind.vm,
  status: MachineStatus.awaitingHost,
  specification: 'Windows',
  accent: Colors.blue,
  hostOnline: true,
);
Matcher code(String value) =>
    isA<BrokerException>().having((e) => e.code, 'code', value);

void main() {
  test('One confirmed POST is followed by correlated polling and fresh actual stream readiness', () async {
    final api = RecoveryApi();
    var loads = 0;
    final result = await ConsoleResume(
      api: api,
      loadDesktops: () async => [desktop(ready: ++loads == 2)],
      authorized: () => true,
      delay: (_) async {},
    ).run('fixture', 'pve-149');
    expect(result.canStream, isTrue);
    expect(loads, 2);
    expect(api.posts, 1);
    expect(api.polls, 1);
  });
  test(
    'Unknown and failed results never retry the transfer or open a stream',
    () async {
      for (final status in ['unknown', 'failed']) {
        final api = RecoveryApi()
          ..start = () async => state(
            status: status,
            error: status == 'failed' ? 'console_binding_changed' : null,
          );
        await expectLater(
          ConsoleResume(
            api: api,
            loadDesktops: () async => throw StateError('must not load'),
            authorized: () => true,
          ).run('fixture', 'pve-149'),
          throwsA(
            code(
              status == 'failed'
                  ? 'console_binding_changed'
                  : 'console_operation_unknown',
            ),
          ),
        );
        expect(api.posts, 1);
        expect(api.polls, 0);
      }
    },
  );
  test(
    'Lost POST acknowledgement remains unknown without a second mutation',
    () async {
      final api = RecoveryApi()
        ..start = () => Completer<ConsolePreparationState>().future;
      await expectLater(
        ConsoleResume(
          api: api,
          loadDesktops: () async => [],
          authorized: () => true,
          timeout: const Duration(milliseconds: 20),
        ).run('fixture', 'pve-149'),
        throwsA(code('console_operation_unknown')),
      );
      expect(api.posts, 1);
      expect(api.polls, 0);
    },
  );
  test('Authorization loss, wrong operation and locked Windows stop the preparation sequence', () async {
    var authorized = false;
    final denied = RecoveryApi();
    await expectLater(
      ConsoleResume(
        api: denied,
        loadDesktops: () async => [],
        authorized: () => authorized,
      ).run('fixture', 'pve-149'),
      throwsA(code('session_expired')),
    );
    expect(denied.posts, 0);
    authorized = true;
    final revoked = RecoveryApi();
    await expectLater(
      ConsoleResume(
        api: revoked,
        loadDesktops: () async => [],
        authorized: () => authorized,
        delay: (_) async {
          authorized = false;
        },
      ).run('fixture', 'pve-149'),
      throwsA(code('session_expired')),
    );
    expect(revoked.posts, 1);
    expect(revoked.polls, 0);
    for (final locked in [false, true]) {
      final api = RecoveryApi()..start = () async => state(status: 'succeeded');
      await expectLater(
        ConsoleResume(
          api: api,
          loadDesktops: () async => [
            desktop(
              ready: true,
              id: locked ? taskId : '00000000-1234-1234-1234-000000000002',
              locked: locked,
            ),
          ],
          authorized: () => true,
        ).run('fixture', 'pve-149'),
        throwsA(
          code(locked ? 'windows_session_locked' : 'console_binding_changed'),
        ),
      );
      expect(api.posts, 1);
      expect(api.polls, 0);
    }
  });
  test('Recovery state never claims streaming readiness and rejects contradictory availability', () {
    expect(ConsolePreparationState.fromJson(null).available, isFalse);
    for (final extra in [
      {'supported': false, 'available': true},
      {'available': true, 'busy': true},
      {'reason': 'PRIVATE_SID'},
      {'supported': 'true'},
    ]) {
      final value = Map<String, dynamic>.from(
        response()['consolePreparation'] as Map,
      )..addAll(extra);
      expect(
        () => ConsolePreparationState.fromJson(value),
        throwsFormatException,
      );
    }
    expect(
      () => ConsolePreparationState.fromJson(
        response()['consolePreparation'],
        operationId: '00000000-1234-1234-1234-000000000002',
      ),
      throwsFormatException,
    );
    expect(
      StreamPreparation.fromCode('no_interactive_session'),
      StreamPreparation.session,
    );
    expect(
      StreamPreparation.fromCode('rdp_session_disconnected'),
      StreamPreparation.resume,
    );
    expect(
      StreamPreparation.fromCode('windows_session_locked'),
      StreamPreparation.locked,
    );
    // Missing saved identity and an absent login are different observations.
    expect(
      StreamPreparation.fromCode('windows_login_required'),
      StreamPreparation.login,
    );
    expect(
      StreamPreparation.fromCode('windows_identity_required'),
      StreamPreparation.identity,
    );
    expect(
      StreamPreparation.fromCode('windows_identity_mismatch'),
      StreamPreparation.configuration,
    );
  });
  test(
    'HTTPS API sends only explicit confirmation and polls its opaque operation',
    () async {
      final client = ApiClient()
        ..responses.addAll([
          (202, response()),
          (200, response(status: 'succeeded')),
        ]);
      final api = HttpBrokerApi(
        origin: Uri.parse('https://cloud.example'),
        client: client,
      );
      final first = await api.prepareConsole('fixture-token', 'pve-149');
      final done = await api.consolePreparation(
        'fixture-token',
        'pve-149',
        operationId: first.operation!.id,
      );
      expect(done.operation!.status, 'succeeded');
      expect(
        client.requests.first.uri.path,
        '/api/v1/desktops/pve-149/console-preparation',
      );
      expect(jsonDecode(client.requests.first.body.toString()), {
        'confirmation': 'resume-windows-session',
      });
      expect(
        client.requests.first.headers.values[HttpHeaders.authorizationHeader],
        'Bearer fixture-token',
      );
      expect(client.requests.first.followRedirects, isFalse);
      expect(
        client.requests.last.uri.path,
        '/api/v1/desktops/pve-149/console-preparation/operations/$taskId',
      );
    },
  );
  testWidgets(
    'Transfer starts once, offers cancellation while busy, and cannot repeat after an uncertain failure',
    (tester) async {
      final pending = Completer<DesktopMachine>();
      var calls = 0, closed = false;
      DesktopMachine? result;
      await tester.pumpWidget(
        MaterialApp(
          theme: buildAppTheme(),
          home: Builder(
            builder: (context) => Scaffold(
              body: TextButton(
                onPressed: () async {
                  result = await showDialog<DesktopMachine>(
                    context: context,
                    barrierDismissible: false,
                    builder: (_) => ConsoleResumeDialog(
                      machine: machine,
                      resume: () {
                        calls++;
                        return pending.future;
                      },
                    ),
                  );
                  closed = true;
                },
                child: const Text('open'),
              ),
            ),
          ),
        ),
      );
      await tester.tap(find.text('open'));
      // The tile already explained the takeover: no question, no tap.
      await tester.pump();
      await tester.pump();
      expect(calls, 1);
      final dialogButtons = find.descendant(
        of: find.byType(ConsoleResumeDialog),
        matching: find.byWidgetPredicate((w) => w is ButtonStyleButton),
      );
      expect(find.text('Windows 화면을 넘겨받는 중'), findsOneWidget);
      expect(find.byType(CircularProgressIndicator), findsOneWidget);
      expect(dialogButtons, findsOneWidget);
      expect(find.byKey(const Key('console-resume-cancel')), findsOneWidget);
      expect(find.widgetWithText(TextButton, '취소'), findsOneWidget);
      expect(find.byKey(const Key('console-resume-close')), findsNothing);
      // This case observes failure without cancelling; the outside barrier
      // remains inert. Escape/back cancellation has separate coverage.
      await tester.tapAt(const Offset(4, 4));
      await tester.pump(const Duration(milliseconds: 300));
      expect(find.byType(ConsoleResumeDialog), findsOneWidget);
      expect(closed, isFalse);
      expect(calls, 1);
      pending.completeError(const BrokerException('console_operation_unknown'));
      await tester.pumpAndSettle();
      expect(find.text('Windows 화면을 넘겨받지 못했어요'), findsOneWidget);
      expect(find.textContaining('같은 작업을 반복하지 말고'), findsOneWidget);
      expect(find.byType(CircularProgressIndicator), findsNothing);
      expect(find.byKey(const Key('console-resume-cancel')), findsNothing);
      expect(
        find.descendant(
          of: find.byKey(const Key('console-resume-close')),
          matching: find.text('확인'),
        ),
        findsOneWidget,
      );
      expect(dialogButtons, findsOneWidget);
      expect(calls, 1);
      await tester.tap(find.byKey(const Key('console-resume-close')));
      await tester.pumpAndSettle();
      expect(find.byType(ConsoleResumeDialog), findsNothing);
      expect(closed, isTrue);
      expect(
        result,
        isNull,
        reason: 'A failed takeover never yields a machine to connect to.',
      );
      expect(calls, 1);
    },
  );
}
