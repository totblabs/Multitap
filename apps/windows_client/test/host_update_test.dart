import 'dart:async';
import 'dart:convert';
import 'dart:io';

import 'package:flutter/material.dart';
import 'package:flutter/services.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:multitap_client/data/appearance_settings.dart';
import 'package:multitap_client/data/broker_api.dart';
import 'package:multitap_client/data/product_updates.dart';
import 'package:multitap_client/models/desktop_machine.dart';
import 'package:multitap_client/models/stream_options.dart';
import 'package:multitap_client/ui/app_theme.dart';
import 'package:multitap_client/ui/workspace_shell.dart';

const _origin = 'https://private-broker.example.com';
const _offered = HostUpdateState(
  present: true,
  connected: true,
  supported: true,
  version: '0.2.58',
  latestVersion: '0.2.59',
  newer: true,
  available: true,
);

DesktopMachine _machine([HostUpdateState update = _offered]) => DesktopMachine(
  id: 'pve-149',
  name: 'TESTER',
  description: '',
  kind: MachineKind.vm,
  status: MachineStatus.ready,
  specification: 'Windows 11',
  accent: Colors.blue,
  powerState: 'running',
  powerStale: false,
  hostOnline: true,
  hostUpdate: update,
);

class _Call {
  _Call(this.desktopId, this.version, this.disconnect);
  final String desktopId, version;
  final bool disconnect;
}

Future<List<_Call>> _show(
  WidgetTester t, {
  required DesktopMachine machine,
  bool administrator = true,
}) async {
  t.view.devicePixelRatio = 1;
  t.view.physicalSize = const Size(1440, 900);
  addTearDown(t.view.resetPhysicalSize);
  addTearDown(t.view.resetDevicePixelRatio);
  final appearance = AppAppearance();
  addTearDown(appearance.dispose);
  final calls = <_Call>[];
  await t.pumpWidget(
    AppearanceScope(
      controller: appearance,
      child: MaterialApp(
        debugShowCheckedModeBanner: false,
        theme: buildAppTheme(),
        home: WorkspaceShell(
          onSignOut: () {},
          onRefresh: () {},
          displayName: 'owner',
          serverOrigin: _origin,
          machines: [machine],
          onConnect: (_, {options = const StreamOptions()}) async =>
              throw StateError('UI fixture does not connect'),
          onPower: (_, _) async {},
          administrator: administrator,
          onHostUpdate: (
            desktopId,
            version, {
            required disconnectSessions,
          }) async => calls.add(_Call(desktopId, version, disconnectSessions)),
          loadHostRelease: (current) async => ProductRelease(
            product: 'host',
            version: '0.2.59',
            filename: 'Multitap-Host-0.2.59-windows-x64.exe',
            size: 17000000,
            sha256: 'a' * 64,
            url: Uri.parse(
              '$_origin/downloads/Multitap-Host-0.2.59-windows-x64.exe',
            ),
            changelog: '원격 업데이트 지원\n안정성 개선',
          ),
        ),
      ),
    ),
  );
  await t.pumpAndSettle();
  return calls;
}

Future<void> _openManage(WidgetTester t) async {
  await t.tap(find.byKey(const Key('desktop-more-pve-149')));
  await t.pumpAndSettle();
}

void main() {
  test('Ordinary accounts get no Host update view; parsing is strict', () {
    final plain = BrokerDesktop.fromJson({
      'id': 'pve-149',
      'name': 'TESTER',
      'kind': 'vm',
      'description': '',
      'os': 'Windows 11',
      'status': 'ready',
      'canConnect': true,
    });
    expect(plain.hostUpdate.present, isFalse);
    expect(plain.hostUpdate.offered, isFalse);
    for (final data in [
      null,
      'yes',
      {'protocol': 'host-update-v2', 'available': true},
      {'available': true, 'newer': true, 'supported': true},
    ]) {
      expect(HostUpdateState.fromJson(data).present, isFalse);
    }
    final view = HostUpdateState.fromJson({
      'protocol': 'host-update-v1',
      'connected': true,
      'supported': true,
      'version': '0.2.58',
      'latestVersion': '0.2.59',
      'newer': true,
      'available': true,
      'sessionActive': true,
      'operation': null,
    });
    expect(view.offered, isTrue);
    expect(view.menuLabel, 'Host 업데이트 · 0.2.58 → 0.2.59');
    final legacy = HostUpdateState.fromJson({
      'protocol': 'host-update-v1',
      'connected': true,
      'supported': false,
      'version': '0.2.57',
      'latestVersion': '0.2.59',
      'newer': true,
      'available': false,
    });
    expect(legacy.present, isTrue);
    expect(legacy.offered, isFalse);
    final session = BrokerSession.fromJson({
      'sessionToken': 'x' * 43,
      'expiresAt': '2026-10-25T00:00:00.000Z',
      'user': {'id': 'a', 'username': 'owner', 'role': 'admin'},
    });
    expect(session.isAdministrator, isTrue);
    for (final role in ['user', 'Admin', null, 7]) {
      expect(
        BrokerSession.fromJson({
          'sessionToken': 'x' * 43,
          'expiresAt': '2026-10-25T00:00:00.000Z',
          'user': {'id': 'a', 'username': 'someone', 'role': role},
        }).isAdministrator,
        isFalse,
      );
    }
  });

  test(
    'Tile status follows the broker operation and fades after completion',
    () {
      HostUpdateState state(Map<String, Object?> operation) =>
          HostUpdateState.fromJson({
            'protocol': 'host-update-v1',
            'connected': true,
            'supported': true,
            'version': '0.2.58',
            'latestVersion': '0.2.59',
            'newer': true,
            'available': false,
            'operation': {
              'targetVersion': '0.2.59',
              'checkedAt': '2026-10-25T00:00:00.000Z',
              ...operation,
            },
          });
      final now = DateTime.parse('2026-10-25T00:01:00.000Z');
      expect(
        state({'status': 'downloading', 'progress': 42}).tileStatus(now),
        'Host 업데이트 중 · 다운로드 42%',
      );
      expect(
        state({'status': 'installing'}).tileStatus(now),
        'Host 업데이트 중 · 설치',
      );
      expect(
        state({'status': 'waiting', 'code': 'session_active'}).tileStatus(now),
        'Host 업데이트 대기 · 연결이 끝나면 시작해요',
      );
      expect(
        state({'status': 'completed'}).tileStatus(now),
        'Host 0.2.59 업데이트 완료',
      );
      expect(
        state({'status': 'completed', 'rebootRequired': true}).tileStatus(now),
        'Host 0.2.59 업데이트 완료 · Windows 다시 시작 필요',
      );
      expect(
        state({'status': 'failed', 'code': 'driver_install_required'})
            .tileStatus(now),
        'Host 업데이트 실패 · 데스크톱에서 직접 설치해야 해요',
      );
      expect(
        state({'status': 'unknown', 'code': 'host_not_returned'})
            .tileStatus(now),
        'Host 업데이트 결과 확인 필요 · Host 응답이 없어요',
      );
      expect(state({'status': 'downloading'}).busy, isTrue);
      expect(state({'status': 'downloading'}).offered, isFalse);
      expect(
        state({'status': 'completed'})
            .tileStatus(now.add(const Duration(minutes: 20))),
        isNull,
      );
      expect(
        state({'status': 'C:\\private', 'targetVersion': '0.2.59'}).operation,
        isNull,
      );
    },
  );

  test(
    'The client request posts only the version and the disconnect choice',
    () async {
      final client = _ApiClient()
        ..responses.add((
          202,
          {
            'hostUpdate': {
              'protocol': 'host-update-v1',
              'connected': true,
              'supported': true,
              'version': '0.2.58',
              'latestVersion': '0.2.59',
              'newer': true,
              'available': false,
              'sessionActive': false,
              'operation': {
                'id': '614c03e4-5595-4d77-a075-f6a61cfc4e8b',
                'status': 'dispatched',
                'code': '',
                'targetVersion': '0.2.59',
              },
            },
          },
        ))
        ..responses.add((
          403,
          {
            'error': {'code': 'admin_required', 'message': 'admin_required'},
          },
        ));
      final api = HttpBrokerApi(
        origin: Uri.parse('https://cloud.example.com'),
        client: client,
      );
      final state = await api.requestHostUpdate(
        'qa-token',
        'pve-149',
        '0.2.59',
        disconnectSessions: true,
      );
      expect(state.operation!.status, 'dispatched');
      final request = client.requests.single;
      expect(request.method, 'POST');
      expect(request.uri.path, '/api/v1/desktops/pve-149/host-update');
      expect(jsonDecode(request.body.toString()), {
        'version': '0.2.59',
        'disconnectSessions': true,
      });
      expect(
        request.headers.values[HttpHeaders.authorizationHeader],
        'Bearer qa-token',
      );
      await expectLater(
        api.requestHostUpdate('qa-token', 'pve-149', '0.2.59'),
        throwsA(
          isA<BrokerException>()
              .having((e) => e.status, 'status', 403)
              .having((e) => e.message, 'message', contains('관리자')),
        ),
      );
      await expectLater(
        api.requestHostUpdate('qa-token', 'pve-149', 'latest'),
        throwsA(isA<BrokerException>()),
      );
    },
  );

  test(
    'The favourite call is a PUT of the flag and returns the saved value',
    () async {
      final client = _ApiClient()
        ..responses.add((200, {'favorite': true}))
        ..responses.add((
          404,
          {
            'error': {'code': 'desktop_not_found', 'message': 'x'},
          },
        ))
        ..responses.add((200, {'favorite': 'yes'}));
      final api = HttpBrokerApi(
        origin: Uri.parse('https://cloud.example.com'),
        client: client,
      );
      expect(await api.setFavorite('qa-token', 'pve-149', true), isTrue);
      final request = client.requests.single;
      expect(request.method, 'PUT');
      expect(request.uri.path, '/api/v1/desktops/pve-149/favorite');
      expect(jsonDecode(request.body.toString()), {'favorite': true});
      expect(
        request.headers.values[HttpHeaders.authorizationHeader],
        'Bearer qa-token',
      );
      await expectLater(
        api.setFavorite('qa-token', 'pve-149', false),
        throwsA(isA<BrokerException>().having((e) => e.status, 'status', 404)),
      );
      // A malformed answer is not taken as success.
      await expectLater(
        api.setFavorite('qa-token', 'pve-149', true),
        throwsA(isA<BrokerException>()),
      );
      await expectLater(
        api.setFavorite('qa-token', '../x', true),
        throwsA(isA<BrokerException>()),
      );
    },
  );
  testWidgets(
    'The ⋯ Host update row is listed only when an update can be installed; the readiness check shows the version',
    (t) async {
      Future<void> closeMenu() async {
        await t.sendKeyEvent(LogicalKeyboardKey.escape);
        await t.pumpAndSettle();
      }

      bool badge() => t
          .widget<Badge>(find.byKey(const Key('host-update-badge-pve-149')))
          .isLabelVisible;

      // ⋯ → 연결 준비 점검: the quiet Host version line, then close.
      Future<String?> readinessVersion() async {
        await _openManage(t);
        await t.tap(find.byKey(const Key('details-pve-149')));
        await t.pumpAndSettle();
        expect(find.byKey(const Key('readiness-dialog')), findsOneWidget);
        final line = find.byKey(const Key('readiness-host-version'));
        final text = line.evaluate().isEmpty ? null : t.widget<Text>(line).data;
        await t.tap(find.byKey(const Key('close-readiness')));
        await t.pumpAndSettle();
        return text;
      }

      await _show(t, machine: _machine());
      // An installable Host update marks ⋯ with a badge and lists its row.
      expect(badge(), isTrue);
      await _openManage(t);
      expect(find.byKey(const Key('host-update-pve-149')), findsOneWidget);
      expect(find.text('Host 업데이트 · 0.2.58 → 0.2.59'), findsOneWidget);
      await closeMenu();
      expect(await readinessVersion(), 'Host 0.2.58 · 새 버전 0.2.59 설치 가능');

      // Any account the VM is assigned to may update its Host.
      await _show(t, machine: _machine(), administrator: false);
      await _openManage(t);
      expect(find.text('Host 업데이트 · 0.2.58 → 0.2.59'), findsOneWidget);
      await closeMenu();

      // A released broker sends no Host view: nothing to show anywhere.
      await _show(t, machine: _machine(HostUpdateState.absent));
      await _openManage(t);
      expect(find.byKey(const Key('details-pve-149')), findsOneWidget);
      expect(find.byKey(const Key('host-update-pve-149')), findsNothing);
      await closeMenu();
      expect(await readinessVersion(), isNull);

      // Nothing to install: no greyed row in ⋯ (it read as a broken button);
      // the version and why there is no update are information in the check.
      for (final (update, text) in [
        (
          const HostUpdateState(
            present: true,
            connected: true,
            supported: false,
            version: '0.2.57',
            latestVersion: '0.2.59',
            newer: true,
          ),
          'Host 0.2.57 · 원격 업데이트 미지원 · 직접 설치 필요',
        ),
        (
          const HostUpdateState(
            present: true,
            connected: true,
            supported: true,
            version: '0.2.59',
            latestVersion: '0.2.59',
          ),
          'Host 0.2.59 · 최신',
        ),
        (
          const HostUpdateState(present: true, version: '0.2.59'),
          'Host 0.2.59 · 오프라인',
        ),
        (const HostUpdateState(present: true, connected: true), 'Host 버전 확인 중'),
      ]) {
        await _show(t, machine: _machine(update));
        expect(badge(), isFalse, reason: text);
        await _openManage(t);
        expect(find.byKey(const Key('details-pve-149')), findsOneWidget);
        expect(
          find.byKey(const Key('host-update-pve-149')),
          findsNothing,
          reason: text,
        );
        await closeMenu();
        expect(await readinessVersion(), text);
        expect(find.byKey(const Key('host-update-dialog')), findsNothing);
      }
    },
  );

  testWidgets(
    'Confirming shows the version step and changelog, then sends one request',
    (t) async {
      final calls = await _show(t, machine: _machine());
      await _openManage(t);
      await t.tap(find.byKey(const Key('host-update-pve-149')));
      await t.pumpAndSettle();
      expect(find.byKey(const Key('host-update-dialog')), findsOneWidget);
      expect(find.text('현재 0.2.58'), findsOneWidget);
      expect(find.text('새 버전 0.2.59'), findsOneWidget);
      expect(find.textContaining('원격 업데이트 지원'), findsOneWidget);
      expect(find.byKey(const Key('host-update-now')), findsNothing);
      expect(find.byKey(const Key('host-update-session-note')), findsNothing);
      expect(calls, isEmpty);
      await t.tap(find.byKey(const Key('host-update-confirm')));
      await t.pumpAndSettle();
      expect(calls.single.desktopId, 'pve-149');
      expect(calls.single.version, '0.2.59');
      expect(calls.single.disconnect, isFalse);
    },
  );

  testWidgets(
    'With a live session there is a second explicit choice that disconnects',
    (t) async {
      const busy = HostUpdateState(
        present: true,
        connected: true,
        supported: true,
        version: '0.2.58',
        latestVersion: '0.2.59',
        newer: true,
        available: true,
        sessionActive: true,
      );
      final calls = await _show(t, machine: _machine(busy));
      Future<void> open() async {
        await _openManage(t);
        await t.tap(find.byKey(const Key('host-update-pve-149')));
        await t.pumpAndSettle();
      }

      await open();
      expect(find.byKey(const Key('host-update-session-note')), findsOneWidget);
      expect(find.text('지금 업데이트'), findsOneWidget);
      expect(find.text('끝나면 업데이트'), findsOneWidget);
      expect(find.byKey(const Key('host-update-confirm')), findsNothing);
      await t.tap(find.byKey(const Key('host-update-cancel')));
      await t.pumpAndSettle();
      expect(calls, isEmpty);

      await open();
      await t.tap(find.byKey(const Key('host-update-now')));
      await t.pumpAndSettle();
      expect(calls.single.disconnect, isTrue);

      await open();
      await t.tap(find.byKey(const Key('host-update-when-idle')));
      await t.pumpAndSettle();
      expect(calls.last.disconnect, isFalse);
      expect(calls, hasLength(2));
    },
  );

  testWidgets(
    'The tile shows the short update status instead of the detail line',
    (t) async {
      final running = HostUpdateState.fromJson({
        'protocol': 'host-update-v1',
        'connected': true,
        'supported': true,
        'version': '0.2.58',
        'latestVersion': '0.2.59',
        'newer': true,
        'available': false,
        'operation': {
          'status': 'downloading',
          'progress': 42,
          'targetVersion': '0.2.59',
          'checkedAt': DateTime.now().toUtc().toIso8601String(),
        },
      });
      await _show(t, machine: _machine(running));
      expect(
        find.byKey(const Key('host-update-status-pve-149')),
        findsOneWidget,
      );
      expect(find.text('Host 업데이트 중 · 다운로드 42%'), findsOneWidget);
      // A running update offers nothing more in ⋯; the check says it runs.
      await _openManage(t);
      expect(find.byKey(const Key('host-update-pve-149')), findsNothing);
      await t.tap(find.byKey(const Key('details-pve-149')));
      await t.pumpAndSettle();
      expect(find.text('Host 0.2.58 · 업데이트 중'), findsOneWidget);
      expect(find.byKey(const Key('host-update-dialog')), findsNothing);
    },
  );

  test('The readiness version line names the state without offering it', () {
    String? text(HostUpdateState state) => state.versionText;
    expect(text(HostUpdateState.absent), isNull);
    expect(text(_offered), 'Host 0.2.58 · 새 버전 0.2.59 설치 가능');
    expect(text(const HostUpdateState(present: true)), 'Host 오프라인 · 버전 확인 안 됨');
    // A catalog without a Host release: the version alone, no "최신" claim.
    expect(
      text(
        const HostUpdateState(
          present: true,
          connected: true,
          supported: true,
          version: '0.2.59',
        ),
      ),
      'Host 0.2.59',
    );
  });
}

class _Headers extends Fake implements HttpHeaders {
  final values = <String, Object>{};
  @override
  ContentType? contentType;
  @override
  void set(String name, Object value, {bool preserveHeaderCase = false}) {
    values[name] = value;
  }
}

class _Response extends Fake implements HttpClientResponse {
  _Response(this.statusCode, this.value);
  @override
  final int statusCode;
  final Map<String, dynamic> value;
  @override
  HttpHeaders get headers => _Headers();
  @override
  Stream<List<int>> timeout(
    Duration timeLimit, {
    void Function(EventSink<List<int>>)? onTimeout,
  }) =>
      Stream<List<int>>.value(utf8.encode(jsonEncode(value)))
          .timeout(timeLimit, onTimeout: onTimeout);
  @override
  StreamSubscription<List<int>> listen(
    void Function(List<int>)? onData, {
    Function? onError,
    void Function()? onDone,
    bool? cancelOnError,
  }) => Stream<List<int>>.value(utf8.encode(jsonEncode(value))).listen(
    onData,
    onError: onError,
    onDone: onDone,
    cancelOnError: cancelOnError,
  );
}

class _Request extends Fake implements HttpClientRequest {
  _Request(this.method, this.uri, this.response);
  @override
  final String method;
  @override
  final Uri uri;
  final _Response response;
  @override
  final _Headers headers = _Headers();
  @override
  bool followRedirects = true;
  final body = StringBuffer();
  @override
  void write(Object? value) => body.write(value);
  @override
  Future<HttpClientResponse> close() async => response;
}

class _ApiClient extends Fake implements HttpClient {
  final responses = <(int, Map<String, dynamic>)>[];
  final requests = <_Request>[];
  @override
  Duration? connectionTimeout;
  @override
  Future<HttpClientRequest> openUrl(String method, Uri uri) async {
    final response = responses.removeAt(0);
    final request = _Request(method, uri, _Response(response.$1, response.$2));
    requests.add(request);
    return request;
  }

  @override
  void close({bool force = false}) {}
}
