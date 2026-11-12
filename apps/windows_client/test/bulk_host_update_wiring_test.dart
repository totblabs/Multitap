import 'dart:async';
import 'dart:io';

import 'package:flutter/material.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:multitap_client/app.dart';
import 'package:multitap_client/data/broker_api.dart';
import 'package:multitap_client/data/product_updates.dart';
import 'package:multitap_client/ui/bulk_host_update_dialog.dart';
import 'package:multitap_client/ui/product_update_dialog.dart';
import 'package:multitap_client/ui/workspace_shell.dart';

import 'auth_flow_test.dart' show TestBroker, fixtureOrigin;

const _offered = HostUpdateState(
  present: true,
  connected: true,
  supported: true,
  version: '0.2.80',
  latestVersion: '0.2.81',
  newer: true,
  available: true,
);
const _accepted = HostUpdateState(
  present: true,
  connected: true,
  supported: true,
  version: '0.2.80',
  latestVersion: '0.2.81',
  newer: true,
  available: false,
  operation: HostUpdateOperation(status: 'accepted', targetVersion: '0.2.81'),
);
BrokerDesktop _vm(String id) => BrokerDesktop(
  id: id,
  name: id,
  kind: 'vm',
  description: '',
  os: 'Windows 11',
  status: 'ready',
  canConnect: true,
  hostUpdate: _offered,
);
ProductRelease _release(String product, String version) => ProductRelease(
  product: product,
  version: version,
  filename:
      'Multitap${product == 'host' ? '-Host' : ''}-$version-windows-x64.exe',
  size: 100,
  sha256: 'a' * 64,
  url: fixtureOrigin.resolve('/downloads/$product-$version.exe'),
  changelog: '테스트 변경 내역',
);

class _Broker extends TestBroker implements HostUpdateBrokerApi {
  final posts = <(String, String, String, bool)>[];
  Completer<List<BrokerDesktop>>? nextRead;
  Completer<HostUpdateState>? nextPost;
  int? postError;
  @override
  Future<List<BrokerDesktop>> desktops(String token) {
    final held = nextRead;
    nextRead = null;
    return held?.future ?? super.desktops(token);
  }

  @override
  Future<HostUpdateState> requestHostUpdate(
    String token,
    String desktopId,
    String version, {
    bool disconnectSessions = false,
  }) async {
    posts.add((token, desktopId, version, disconnectSessions));
    if (postError != null) throw BrokerException('session_expired', postError!);
    return nextPost?.future ?? _accepted;
  }
}

Future<WorkspaceShell> _login(
  WidgetTester t,
  _Broker broker, {
  DateTime Function()? clock,
}) async {
  t.view.physicalSize = const Size(1440, 1000);
  t.view.devicePixelRatio = 1;
  addTearDown(t.view.resetPhysicalSize);
  addTearDown(t.view.resetDevicePixelRatio);
  await t.pumpWidget(
    MultitapApp(
      api: broker,
      serverOrigin: fixtureOrigin,
      clock: clock ?? DateTime.now,
      hostUpdateReleaseLoader: (server) async {
        expect(server, fixtureOrigin.toString());
        return _release('host', '0.2.81');
      },
    ),
  );
  await t.pumpAndSettle();
  await t.enterText(find.byKey(const Key('sign-in-id')), 'ordinary-user');
  await t.enterText(find.byKey(const Key('sign-in-password')), 'fixture-only');
  await t.tap(find.byKey(const Key('login-submit')));
  await t.pumpAndSettle();
  return t.widget<WorkspaceShell>(find.byType(WorkspaceShell));
}

Future<BulkHostUpdateDialog> _openFromHeader(WidgetTester t) async {
  // Host updates have their own header button, never the app panel.
  await t.tap(find.byKey(const Key('open-host-updates')));
  await t.pumpAndSettle();
  expect(find.byType(ProductUpdateDialog), findsNothing);
  return t.widget<BulkHostUpdateDialog>(find.byType(BulkHostUpdateDialog));
}

class _Catalog extends ProductUpdates {
  _Catalog() : super(fixtureOrigin.toString());
  final downloadGate = Completer<DownloadedUpdate>();
  void Function()? verifying;
  @override
  Future<ProductRelease?> check(String product, String currentVersion) async =>
      _release(product, '0.4.96');
  @override
  Future<DownloadedUpdate> download(
    ProductRelease release,
    void Function(int, int) progress, {
    void Function()? verifying,
    void Function(int attempt)? retrying,
  }) {
    this.verifying = verifying;
    return downloadGate.future;
  }

  @override
  void close() {}
}

class _Download extends DownloadedUpdate {
  _Download()
    : super(
        _release('client', '0.4.96'),
        File('fixture-not-written.exe'),
        Directory('fixture-not-created'),
      );
  @override
  Future<void> discard() async {}
}

class _Platform extends UpdatePlatform {
  final installGate = Completer<void>();
  int installs = 0;
  @override
  Future<void> verifyInstaller(DownloadedUpdate update) async {}
  @override
  Future<bool> hasRemoteSessions() async => false;
  @override
  Future<void> closeRemoteSessions() async =>
      fail('No remote sessions in fixture');
  @override
  Future<void> install(
    DownloadedUpdate update,
    UpdateInstallMode mode, {
    required String currentVersion,
  }) async {
    installs++;
    await installGate.future;
  }
}

void main() {
  testWidgets(
    'An installable Host update badges the header update button and the panel offers it',
    (t) async {
      final broker = _Broker()
        ..assigned = [
          _vm('pve-101'),
          _vm('pve-102'),
          BrokerDesktop(
            id: 'pc-gaming',
            name: 'TOWER-PC',
            kind: 'pc',
            description: '',
            os: 'Windows 11',
            status: 'ready',
            canConnect: true,
            hostUpdate: _offered,
          ),
          BrokerDesktop(
            id: 'pve-103',
            name: 'pve-103',
            kind: 'vm',
            description: '',
            os: 'Windows 11',
            status: 'ready',
            canConnect: true,
            hostUpdate: _accepted,
          ),
        ];
      await _login(t, broker);
      // Two VMs and the local PC can be updated now; the busy one cannot.
      // The Host button is separate from the app update button.
      expect(find.byKey(const Key('client-update-badge')), findsNothing);
      expect(
        find.descendant(
          of: find.byKey(const Key('open-host-updates')),
          matching: find.text('Host 업데이트 3대'),
        ),
        findsOneWidget,
      );
      await t.tap(find.byKey(const Key('open-host-updates')));
      await t.pumpAndSettle();
      final bulk = t.widget<BulkHostUpdateDialog>(
        find.byType(BulkHostUpdateDialog),
      );
      expect(bulk.assignedMachines.map((m) => m.id), [
        'pve-101',
        'pve-102',
        'pc-gaming',
        'pve-103',
      ]);
      expect(broker.posts, isEmpty);
      await t.tap(find.byKey(const Key('bulk-host-close')));
      await t.pumpAndSettle();

      // The list push clears the badge once nothing is installable.
      broker.assigned = [
        BrokerDesktop(
          id: 'pve-101',
          name: 'pve-101',
          kind: 'vm',
          description: '',
          os: 'Windows 11',
          status: 'ready',
          canConnect: true,
          hostUpdate: _accepted,
        ),
      ];
      await t.tap(find.byKey(const Key('refresh-desktops')));
      await t.pumpAndSettle();
      expect(find.byKey(const Key('open-host-updates')), findsNothing);
      await t.pumpWidget(const SizedBox());
    },
  );

  testWidgets(
    'Ordinary assigned account opens bulk below Navigator and returns to the launcher',
    (t) async {
      final broker = _Broker()..assigned = [_vm('pve-101'), _vm('pve-102')];
      final shell = await _login(t, broker);
      expect(shell.administrator, isFalse);
      expect(shell.onHostBatchUpdate, isNotNull);
      final bulk = await _openFromHeader(t);
      expect(bulk.server, fixtureOrigin.toString());
      expect(bulk.token, 'test-token');
      expect(bulk.liveMachines, isNotNull);
      expect(bulk.refreshAssigned, isNotNull);
      expect(bulk.assignedMachines.map((m) => m.id), ['pve-101', 'pve-102']);
      expect(
        broker.posts,
        isEmpty,
        reason: 'Opening never requests an update.',
      );
      broker.networkFailure = true;
      await expectLater(
        bulk.refreshAssigned!(),
        throwsA(isA<BrokerException>()),
      );
      expect(
        bulk.liveMachines!.value,
        hasLength(2),
        reason: 'A read failure is not a successful empty assignment.',
      );
      broker.networkFailure = false;
      broker.assigned = [];
      expect(await bulk.refreshAssigned!(), isEmpty);
      await t.pumpAndSettle();
      expect(bulk.liveMachines!.value, isEmpty);
      final pendingRead = Completer<List<BrokerDesktop>>();
      broker.nextRead = pendingRead;
      final lateRead = bulk.refreshAssigned!();
      final lateRejected = expectLater(
        lateRead,
        throwsA(isA<BrokerException>()),
      );
      await t.tap(find.byKey(const Key('bulk-host-close')));
      await t.pumpAndSettle();
      pendingRead.complete([]);
      await lateRejected;
      await t.pumpAndSettle();
      expect(find.byType(BulkHostUpdateDialog), findsNothing);
      expect(find.byType(ProductUpdateDialog), findsNothing);
      expect(
        find.byType(WorkspaceShell),
        findsOneWidget,
        reason: 'A late callback from a closed panel must not expire the current login.',
      );
      expect(broker.posts, isEmpty);
    },
  );

  testWidgets(
    'Bound requests retain token/latest/assignment and pass the disconnect choice to the broker',
    (t) async {
      final broker = _Broker()..assigned = [_vm('pve-101')];
      await _login(t, broker);
      final bulk = await _openFromHeader(t);
      expect(bulk.administrator, isFalse);
      for (final (token, id, version, disconnect) in [
        ('wrong-token', 'pve-101', '0.2.81', true),
        (bulk.token, 'not-assigned', '0.2.81', true),
        (bulk.token, 'pve-101', '0.2.80', true),
      ]) {
        await expectLater(
          bulk.api.requestHostUpdate(
            token,
            id,
            version,
            disconnectSessions: disconnect,
          ),
          throwsA(isA<BrokerException>()),
        );
      }
      expect(broker.posts, isEmpty);
      expect(
        await bulk.api.requestHostUpdate(bulk.token, 'pve-101', '0.2.81'),
        same(_accepted),
      );
      // The broker decides which connections this account may end.
      expect(
        await bulk.api.requestHostUpdate(
          bulk.token,
          'pve-101',
          '0.2.81',
          disconnectSessions: true,
        ),
        same(_accepted),
      );
      expect(broker.posts, [
        ('test-token', 'pve-101', '0.2.81', false),
        ('test-token', 'pve-101', '0.2.81', true),
      ]);
      await t.pumpWidget(const SizedBox());
    },
  );

  testWidgets(
    'A late fallback response cannot restore an assignment removed by a newer list push',
    (t) async {
      final broker = _Broker()..assigned = [_vm('pve-101')];
      final shell = await _login(t, broker);
      final bulk = await _openFromHeader(t);
      final old = Completer<List<BrokerDesktop>>();
      broker.nextRead = old;
      final late = bulk.refreshAssigned!();
      broker.assigned = [];
      shell.onRefresh();
      await t.pumpAndSettle();
      expect(bulk.liveMachines!.value, isEmpty);
      old.complete([_vm('pve-101')]);
      expect(await late, isEmpty);
      await expectLater(
        bulk.api.requestHostUpdate(bulk.token, 'pve-101', '0.2.81'),
        throwsA(isA<BrokerException>()),
      );
      expect(broker.posts, isEmpty);
      await t.pumpWidget(const SizedBox());
    },
  );

  testWidgets(
    'A session lapse closes bulk and rejects a delayed POST result and all later requests',
    (t) async {
      var now = DateTime.now();
      final broker = _Broker()..assigned = [_vm('pve-101')];
      final shell = await _login(t, broker, clock: () => now);
      final bulk = await _openFromHeader(t);
      final gate = Completer<HostUpdateState>();
      broker.nextPost = gate;
      final pending = bulk.api.requestHostUpdate(
        bulk.token,
        'pve-101',
        '0.2.81',
      );
      final rejected = expectLater(pending, throwsA(isA<BrokerException>()));
      now = now.add(const Duration(hours: 2));
      shell.onRefresh();
      await t.pumpAndSettle();
      expect(find.byType(BulkHostUpdateDialog), findsNothing);
      expect(find.byType(ProductUpdateDialog), findsNothing);
      gate.complete(_accepted);
      await rejected;
      await expectLater(
        bulk.api.requestHostUpdate(bulk.token, 'pve-101', '0.2.81'),
        throwsA(isA<BrokerException>()),
      );
      expect(broker.posts, hasLength(1));
      await t.pumpWidget(const SizedBox());
    },
  );

  testWidgets(
    'An actual broker 401 runs existing expiry recovery and closes the old bulk panel',
    (t) async {
      final broker = _Broker()..assigned = [_vm('pve-101')];
      await _login(t, broker);
      final bulk = await _openFromHeader(t);
      broker.postError = 401;
      await expectLater(
        bulk.api.requestHostUpdate(bulk.token, 'pve-101', '0.2.81'),
        throwsA(isA<BrokerException>()),
      );
      await t.pumpAndSettle();
      expect(find.byType(BulkHostUpdateDialog), findsNothing);
      expect(find.byType(WorkspaceShell), findsNothing);
      expect(broker.posts, hasLength(1));
      await t.pumpWidget(const SizedBox());
    },
  );

  testWidgets(
    'Client update states remain visible and block duplicate bulk/download/install actions',
    (t) async {
      final catalog = _Catalog(), platform = _Platform();
      final bulkGate = Completer<void>();
      var opens = 0;
      await t.pumpWidget(
        MaterialApp(
          home: ProductUpdateDialog(
            server: fixtureOrigin.toString(),
            product: 'client',
            currentVersion: '0.4.95',
            api: catalog,
            platform: platform,
            onHostBatchUpdate: (context) async {
              expect(Navigator.of(context), isNotNull);
              opens++;
              await bulkGate.future;
            },
          ),
        ),
      );
      await t.pumpAndSettle();
      final trigger = find.byKey(const Key('open-bulk-host-update'));
      final first = t.widget<TextButton>(trigger).onPressed!;
      first();
      first();
      await t.pump();
      expect(opens, 1);
      expect(t.widget<TextButton>(trigger).onPressed, isNull);
      // Even a stale caller cannot start downloading while bulk is open.
      t
          .widget<FilledButton>(find.byKey(const Key('install-product-update')))
          .onPressed!();
      await t.pump();
      expect(catalog.verifying, isNull);
      bulkGate.complete();
      await t.pumpAndSettle();
      expect(find.byKey(const Key('install-product-update')), findsOneWidget);
      await t.tap(find.byKey(const Key('install-product-update')));
      await t.pump();
      expect(t.widget<TextButton>(trigger).onPressed, isNull);
      catalog.verifying!();
      await t.pump();
      expect(t.widget<TextButton>(trigger).onPressed, isNull);
      catalog.downloadGate.complete(_Download());
      await t.pumpAndSettle();
      expect(t.widget<TextButton>(trigger).onPressed, isNotNull);
      await t.tap(find.byKey(const Key('apply-product-update')));
      await t.pump();
      expect(platform.installs, 1);
      expect(t.widget<TextButton>(trigger).onPressed, isNull);
      platform.installGate.completeError(const UpdateFailure('launch_failed'));
      await t.pumpAndSettle();
      expect(t.widget<TextButton>(trigger).onPressed, isNotNull);
      expect(opens, 1);
      await t.pumpWidget(const SizedBox());
    },
  );
}
