import 'dart:async';
import 'dart:convert';
import 'dart:io';

import 'package:flutter/material.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:multitap_client/data/product_updates.dart';
import 'package:multitap_client/ui/product_update_dialog.dart';

import 'product_updates_test.dart' show Transport, entry, executable;

class _InstallPlatform extends UpdatePlatform {
  final sessions = Completer<bool>();
  final closedSessions = Completer<void>();
  int checks = 0, closes = 0, installs = 0;
  UpdateInstallMode? installedMode;

  @override
  Future<void> verifyInstaller(DownloadedUpdate update) async {}

  @override
  Future<bool> hasRemoteSessions() {
    checks++;
    return sessions.future;
  }

  @override
  Future<void> closeRemoteSessions() {
    closes++;
    return closedSessions.future;
  }

  @override
  Future<void> install(
    DownloadedUpdate update,
    UpdateInstallMode mode, {
    required String currentVersion,
  }) async {
    installs++;
    installedMode = mode;
  }
}

Future<void> _until(WidgetTester tester, bool Function() done) async {
  for (var attempt = 0; attempt < 300; attempt++) {
    await tester.pump();
    if (done()) {
      await tester.pump(const Duration(milliseconds: 300));
      return;
    }
    await tester.runAsync(
      () => Future<void>.delayed(const Duration(milliseconds: 10)),
    );
  }
  fail('The update fixture did not reach the expected state.');
}

void main() {
  late Directory directory;

  setUp(() {
    directory = Directory.systemTemp.createTempSync('multitap-install-owner-');
  });

  tearDown(() {
    // Only remove the empty fixture root; the dialog owns its download cleanup.
    expect(directory.listSync(), isEmpty);
    directory.deleteSync();
  });

  Future<_InstallPlatform> ready(
    WidgetTester tester, {
    String product = 'client',
    bool manual = false,
  }) async {
    final platform = _InstallPlatform();
    final transport = Transport((uri) {
      final bytes = uri.path == '/api/v1/downloads'
          ? utf8.encode(
              jsonEncode({
                'releases': [entry(product: product)],
              }),
            )
          : executable;
      return UpdateResponse(200, bytes.length, Stream.value(bytes));
    });
    final updates = ProductUpdates(
      'https://fixture.invalid',
      transport: transport,
      temporaryRoot: directory,
    );
    await tester.pumpWidget(
      MaterialApp(
        home: Builder(
          builder: (context) => TextButton(
            onPressed: () => showDialog<void>(
              context: context,
              barrierDismissible: false,
              builder: (_) => ProductUpdateDialog(
                server: 'https://fixture.invalid',
                product: product,
                currentVersion: '0.4.15',
                api: updates,
                platform: platform,
                manual: manual,
              ),
            ),
            child: const Text('open'),
          ),
        ),
      ),
    );
    await tester.tap(find.text('open'));
    await _until(
      tester,
      () =>
          find.byKey(const Key('install-product-update')).evaluate().isNotEmpty,
    );
    await tester.tap(find.byKey(const Key('install-product-update')));
    await _until(
      tester,
      () => find.byKey(const Key('apply-product-update')).evaluate().isNotEmpty,
    );
    return platform;
  }

  Future<void> cleanup(WidgetTester tester) async {
    await tester.pumpWidget(const SizedBox());
    await _until(tester, () => directory.listSync().isEmpty);
    expect(tester.takeException(), isNull);
  }

  for (final sessions in [false, true]) {
    testWidgets('closed update ignores a late session result: $sessions', (
      tester,
    ) async {
      final platform = await ready(tester);
      await tester.tap(find.byKey(const Key('apply-product-update')));
      await tester.pump();
      expect(platform.checks, 1);
      await tester.tap(find.byKey(const Key('close-product-update')));
      await tester.pumpAndSettle();
      expect(find.byType(ProductUpdateDialog), findsNothing);
      platform.sessions.complete(sessions);
      await tester.pump();
      expect(platform.installs, 0);
      expect(platform.closes, 0);
      expect(find.byKey(const Key('update-sessions-confirm')), findsNothing);
      await cleanup(tester);
    });
  }

  for (final sessions in [false, true]) {
    testWidgets('programmatic route pop retires install: $sessions', (
      tester,
    ) async {
      final platform = await ready(tester);
      await tester.tap(find.byKey(const Key('apply-product-update')));
      await tester.pump();
      Navigator.of(tester.element(find.byType(ProductUpdateDialog))).pop();
      platform.sessions.complete(sessions);
      await tester.pump();
      expect(platform.installs, 0);
      expect(platform.closes, 0);
      expect(find.byKey(const Key('update-sessions-confirm')), findsNothing);
      await tester.pumpAndSettle();
      await cleanup(tester);
    });
  }

  testWidgets('closing animation invalidates install before route disposal', (
    tester,
  ) async {
    final platform = await ready(tester);
    await tester.tap(find.byKey(const Key('apply-product-update')));
    await tester.pump();
    await tester.tap(find.byKey(const Key('close-product-update')));
    // No animation pump yet: mounted alone cannot establish ownership.
    platform.sessions.complete(false);
    await tester.pump();
    expect(platform.installs, 0);
    await tester.pumpAndSettle();
    await cleanup(tester);
  });

  testWidgets('disposed update ignores a pending session query', (
    tester,
  ) async {
    final platform = await ready(tester);
    await tester.tap(find.byKey(const Key('apply-product-update')));
    await tester.pump();
    await tester.pumpWidget(const SizedBox());
    platform.sessions.complete(false);
    await tester.pump();
    expect(platform.installs, 0);
    await cleanup(tester);
  });

  testWidgets('closed update ignores a late session-query error', (
    tester,
  ) async {
    final platform = await ready(tester);
    await tester.tap(find.byKey(const Key('apply-product-update')));
    await tester.pump();
    await tester.tap(find.byKey(const Key('close-product-update')));
    await tester.pumpAndSettle();
    platform.sessions.completeError(StateError('retired query'));
    await tester.pump();
    expect(platform.installs, 0);
    expect(platform.closes, 0);
    await cleanup(tester);
  });

  testWidgets('closing while remote windows close prevents installer handoff', (
    tester,
  ) async {
    final platform = await ready(tester);
    platform.sessions.complete(true);
    await tester.tap(find.byKey(const Key('apply-product-update')));
    await tester.pumpAndSettle();
    await tester.tap(find.byKey(const Key('confirm-update-sessions')));
    await tester.pumpAndSettle();
    expect(platform.closes, 1);
    await tester.tap(find.byKey(const Key('close-product-update')));
    await tester.pumpAndSettle();
    platform.closedSessions.complete();
    await tester.pump();
    expect(platform.installs, 0);
    await cleanup(tester);
  });

  testWidgets('cancelled remote-session confirmation never installs', (
    tester,
  ) async {
    final platform = await ready(tester);
    platform.sessions.complete(true);
    await tester.tap(find.byKey(const Key('apply-product-update')));
    await tester.pumpAndSettle();
    Navigator.of(
      tester.element(find.byKey(const Key('update-sessions-confirm'))),
    ).pop(false);
    await tester.pumpAndSettle();
    expect(platform.closes, 0);
    expect(platform.installs, 0);
    await cleanup(tester);
  });

  testWidgets('active no-session update installs exactly once', (tester) async {
    final platform = await ready(tester);
    platform.sessions.complete(false);
    await tester.tap(find.byKey(const Key('apply-product-update')));
    await tester.pump();
    expect(platform.installs, 1);
    expect(platform.installedMode, UpdateInstallMode.apply);
    await cleanup(tester);
  });

  testWidgets('active manual update preserves its installer mode', (
    tester,
  ) async {
    final platform = await ready(tester, manual: true);
    platform.sessions.complete(false);
    await tester.tap(find.byKey(const Key('apply-product-update')));
    await tester.pump();
    expect(platform.installs, 1);
    expect(platform.installedMode, UpdateInstallMode.manual);
    await cleanup(tester);
  });

  testWidgets(
    'active confirmed update waits for window closure then installs',
    (tester) async {
      final platform = await ready(tester);
      platform.sessions.complete(true);
      await tester.tap(find.byKey(const Key('apply-product-update')));
      await tester.pumpAndSettle();
      await tester.tap(find.byKey(const Key('confirm-update-sessions')));
      await tester.pumpAndSettle();
      expect(platform.closes, 1);
      expect(platform.installs, 0);
      platform.closedSessions.complete();
      await tester.pump();
      expect(platform.installs, 1);
      await cleanup(tester);
    },
  );

  testWidgets('Host installation preserves its separate session boundary', (
    tester,
  ) async {
    final platform = await ready(tester, product: 'host');
    await tester.tap(find.byKey(const Key('apply-product-update')));
    await tester.pump();
    expect(platform.checks, 0);
    expect(platform.installs, 1);
    await cleanup(tester);
  });
}
