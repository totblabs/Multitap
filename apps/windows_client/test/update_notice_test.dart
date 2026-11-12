import 'package:flutter/material.dart';
import 'package:flutter/services.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:multitap_client/data/update_handoff.dart';
import 'package:multitap_client/ui/app_theme.dart';
import 'package:multitap_client/ui/update_messages.dart';
import 'package:multitap_client/ui/update_notice.dart';

Map<String, Object> handoff({
  String result = 'updated',
  String product = 'client',
  String from = '0.4.61',
  String to = '0.4.62',
  String code = '',
  int stage = 6,
  bool attention = false,
  bool restart = false,
}) => {
  'result': result,
  'product': product,
  'fromVersion': from,
  'toVersion': to,
  'code': code,
  'stage': stage,
  'attention': attention,
  'restartRequired': restart,
};

void main() {
  group('handoff record (update-handoff-v1)', () {
    test('success and failure parse with fixed fields only', () {
      final updated = UpdateHandoff.parse(handoff())!;
      expect(updated.updated, isTrue);
      expect(updated.toVersion, '0.4.62');
      expect(updated.code, isEmpty);
      final failed = UpdateHandoff.parse(
        handoff(result: 'failed', code: 'rollback_failed', stage: 4),
      )!;
      expect(failed.updated, isFalse);
      expect(failed.code, 'rollback_failed');
      expect(failed.stage, 4);
      // A failure without a code is the generic one.
      expect(
        UpdateHandoff.parse(handoff(result: 'failed'))!.code,
        'install_failed',
      );
      // Host notices only accompany a completed update.
      expect(
        UpdateHandoff.parse(
          handoff(result: 'failed', attention: true, restart: true),
        )!.restartRequired,
        isFalse,
      );
    });

    test('anything outside the fixed format is ignored', () {
      for (final bad in <Map<String, Object>>[
        handoff(result: 'done'),
        handoff(product: 'server'),
        handoff(from: '0.4'),
        handoff(to: '0.4.62 '),
        handoff(code: 'C:\\Users\\secret'),
        handoff(code: 'Exception: boom'),
        handoff(stage: 7),
        handoff(stage: -1),
        {...handoff(), 'attention': 'yes'},
      ]) {
        expect(UpdateHandoff.parse(bad), isNull, reason: '$bad');
      }
      expect(UpdateHandoff.parse(null), isNull);
      expect(UpdateHandoff.parse('updated'), isNull);
    });

    test(
      'take reads the native answer once and survives a missing runner',
      () async {
        const channel = MethodChannel('multitap/window');
        final messenger =
            TestDefaultBinaryMessengerBinding.instance.defaultBinaryMessenger;
        final calls = <String>[];
        messenger.setMockMethodCallHandler(channel, (call) async {
          calls.add(call.method);
          return calls.length == 1 ? handoff() : null;
        });
        addTearDown(() => messenger.setMockMethodCallHandler(channel, null));
        expect((await UpdateHandoff.take())!.toVersion, '0.4.62');
        expect(await UpdateHandoff.take(), isNull);
        expect(calls, ['takeUpdateHandoff', 'takeUpdateHandoff']);
        messenger.setMockMethodCallHandler(channel, null);
        expect(await UpdateHandoff.take(), isNull);
      },
    );

    test('actions only use an HTTPS origin and a valid running version', () {
      final notices = UpdateNotices();
      notices.configure(server: 'http://plain.example', currentVersion: 'x');
      expect(notices.server, isNull);
      expect(notices.currentVersion, isNull);
      notices.configure(
        server: 'https://vdi.example:18443',
        currentVersion: '0.4.62',
      );
      expect(notices.server, 'https://vdi.example:18443');
      expect(notices.currentVersion, '0.4.62');
    });
  });

  test('Korean particle follows how the version is read', () {
    expect(withRo('0.4.62'), '0.4.62로');
    expect(withRo('0.4.61'), '0.4.61로');
    expect(withRo('0.2.60'), '0.2.60으로');
    expect(withRo('0.2.53'), '0.2.53으로');
    expect(withRo('0.2.56'), '0.2.56으로');
    expect(withRo('1.0.0'), '1.0.0으로');
    expect(withRo('1.0.7'), '1.0.7로');
  });

  group('relaunch notice', () {
    Future<UpdateNotices> pump(
      WidgetTester tester, {
      bool dark = true,
      Size size = const Size(1100, 760),
    }) async {
      tester.view.devicePixelRatio = 1;
      tester.view.physicalSize = size;
      addTearDown(tester.view.resetPhysicalSize);
      addTearDown(tester.view.resetDevicePixelRatio);
      final notices = UpdateNotices();
      await tester.pumpWidget(
        MaterialApp(
          theme: buildAppTheme(
            brightness: dark ? Brightness.dark : Brightness.light,
          ),
          builder: (context, child) =>
              UpdateNoticeHost(notices: notices, child: child!),
          home: Scaffold(
            body: Center(
              child: TextButton(onPressed: () {}, child: const Text('app')),
            ),
          ),
        ),
      );
      return notices;
    }

    testWidgets('a plain success says the version, never blocks, and fades', (
      tester,
    ) async {
      final notices = await pump(tester);
      final opened = <String>[];
      notices
        ..configure(server: 'https://vdi.example', currentVersion: '0.4.62')
        ..openHistory = ({
          required server,
          required product,
          required currentVersion,
        }) async => opened.add('$server|$product|$currentVersion');
      notices.show(UpdateHandoff.parse(handoff())!);
      await tester.pump();
      await tester.pump(const Duration(milliseconds: 300));
      expect(find.text('Multitap 0.4.62로 업데이트했어요'), findsOneWidget);
      // No dialog route: the window underneath stays usable.
      expect(find.byType(Dialog), findsNothing);
      await tester.tap(find.text('app'));
      await tester.pump();
      expect(find.byKey(const Key('update-notice')), findsOneWidget);
      await tester.pump(const Duration(seconds: 8));
      await tester.pumpAndSettle();
      expect(find.byKey(const Key('update-notice')), findsNothing);
      expect(notices.current, isNull);
      expect(opened, isEmpty);
    });

    testWidgets('the changelog link opens this server\'s history', (
      tester,
    ) async {
      final notices = await pump(tester, dark: false);
      final opened = <String>[];
      notices
        ..configure(server: 'https://vdi.example', currentVersion: '0.4.62')
        ..openHistory = ({
          required server,
          required product,
          required currentVersion,
        }) async => opened.add('$server|$product|$currentVersion');
      notices.show(UpdateHandoff.parse(handoff())!);
      await tester.pump();
      await tester.pump(const Duration(milliseconds: 300));
      await tester.tap(find.byKey(const Key('update-notice-history')));
      await tester.pump(const Duration(milliseconds: 400));
      expect(opened, ['https://vdi.example|client|0.4.62']);
      expect(notices.current, isNull);
    });

    testWidgets('a failure stays with its fixed code, stage and next actions', (
      tester,
    ) async {
      final notices = await pump(tester);
      final opened = <String>[];
      notices
        ..configure(server: 'https://vdi.example', currentVersion: '0.4.61')
        ..openPanel = ({
          required server,
          required product,
          required currentVersion,
          required manual,
        }) async => opened.add('$product|$currentVersion|$manual');
      notices.show(
        UpdateHandoff.parse(
          handoff(result: 'failed', code: 'install_failed', stage: 3),
        )!,
      );
      await tester.pump();
      await tester.pump(const Duration(milliseconds: 300));
      expect(find.text('업데이트하지 못했어요'), findsOneWidget);
      expect(find.textContaining('이전 버전을 다시 열었어요'), findsOneWidget);
      expect(find.text('코드 install_failed · 3/6단계 완료 후 중단'), findsOneWidget);
      await tester.pump(const Duration(seconds: 30));
      expect(find.byKey(const Key('update-notice')), findsOneWidget);
      await tester.tap(find.byKey(const Key('update-notice-retry')));
      await tester.pump(const Duration(milliseconds: 400));
      expect(opened, ['client|0.4.61|false']);
      notices.show(
        UpdateHandoff.parse(handoff(result: 'failed', code: 'install_failed'))!,
      );
      await tester.pump();
      await tester.pump(const Duration(milliseconds: 300));
      await tester.tap(find.byKey(const Key('update-notice-installer')));
      await tester.pump(const Duration(milliseconds: 400));
      expect(opened.last, 'client|0.4.61|true');
    });

    testWidgets('Host notices that need a look stay until closed', (
      tester,
    ) async {
      final notices = await pump(tester, size: const Size(640, 600));
      notices.show(
        UpdateHandoff.parse(
          handoff(
            product: 'host',
            from: '0.2.49',
            to: '0.2.50',
            attention: true,
            restart: true,
          ),
        )!,
      );
      await tester.pump();
      await tester.pump(const Duration(milliseconds: 300));
      expect(find.text('Multitap Host 0.2.50으로 업데이트했어요'), findsOneWidget);
      expect(find.textContaining('Windows를 다시 시작해야 해요'), findsOneWidget);
      expect(find.text('Host 설정에서 진단을 확인해 주세요.'), findsOneWidget);
      await tester.pump(const Duration(seconds: 30));
      expect(find.byKey(const Key('update-notice')), findsOneWidget);
      await tester.tap(find.byKey(const Key('update-notice-close')));
      await tester.pumpAndSettle();
      expect(find.byKey(const Key('update-notice')), findsNothing);
      expect(tester.takeException(), isNull);
    });

    testWidgets('a Host block points to the installer, not a blind retry', (
      tester,
    ) async {
      final notices = await pump(tester);
      notices
        ..configure(server: 'https://vdi.example', currentVersion: '0.2.49')
        ..openPanel = ({
          required server,
          required product,
          required currentVersion,
          required manual,
        }) async {};
      notices.show(
        UpdateHandoff.parse(
          handoff(
            result: 'failed',
            product: 'host',
            from: '0.2.49',
            to: '0.2.50',
            code: 'certificate_trust_required',
            stage: 0,
          ),
        )!,
      );
      await tester.pump();
      await tester.pump(const Duration(milliseconds: 300));
      expect(find.text('직접 확인이 필요해요'), findsOneWidget);
      expect(find.text('코드 certificate_trust_required'), findsOneWidget);
      expect(find.byKey(const Key('update-notice-retry')), findsNothing);
      expect(find.byKey(const Key('update-notice-installer')), findsOneWidget);
    });

    testWidgets('without a configured server no action is offered', (
      tester,
    ) async {
      final notices = await pump(tester);
      notices.show(
        UpdateHandoff.parse(handoff(result: 'failed', code: 'install_failed'))!,
      );
      await tester.pump();
      await tester.pump(const Duration(milliseconds: 300));
      expect(find.byKey(const Key('update-notice-retry')), findsNothing);
      expect(find.byKey(const Key('update-notice-installer')), findsNothing);
      expect(find.byKey(const Key('update-notice-close')), findsOneWidget);
    });
  });

  test('every fixed failure code has its own text', () {
    final seen = <String>{};
    for (final code in [
      'catalog_unavailable',
      'download_failed',
      'hash_mismatch',
      'package_version_mismatch',
      'signature_invalid',
      'installer_exited',
      'session_windows_open',
      'version_unknown',
      'installer_missing',
      'progress_channel_failed',
      'existing_install_required',
      'certificate_trust_required',
      'driver_install_required',
      'rollback_failed',
      'install_failed',
    ]) {
      final text = updateFailureMessage(
        code,
        product: 'host',
        version: '0.2.50',
      );
      expect(text, isNot(contains(code)));
      expect(seen.add(text), isTrue, reason: code);
    }
    expect(
      updateFailureMessage(
        'install_failed',
        product: 'client',
        afterRelaunch: true,
      ),
      contains('이전 버전을 다시 열었어요'),
    );
    // Review 2026-11-12: the app's own update told its user to check the
    // Host service. Only a Host update has one.
    expect(
      updateFailureMessage('rollback_failed', product: 'host'),
      contains('Host의 서비스 상태를 확인'),
    );
    expect(
      updateFailureMessage('rollback_failed', product: 'client'),
      '설치가 중단됐고 복구 여부도 확인하지 못했어요.\n설치 프로그램으로 다시 설치하세요.',
    );
    expect(updateFailureTitle('driver_install_required'), '직접 확인이 필요해요');
    expect(updateFailureTitle('install_failed'), '업데이트하지 못했어요');
  });
}
