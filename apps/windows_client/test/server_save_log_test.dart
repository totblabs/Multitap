// A tested server address that could not be saved leaves its reason in the
// product log as fixed numbers: the Windows form, the Android dialog and the
// log viewer's row.
import 'dart:convert';
import 'dart:io';

import 'package:flutter/material.dart';
import 'package:flutter/services.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:multitap_client/android/android_app.dart';
import 'package:multitap_client/data/broker_probe.dart';
import 'package:multitap_client/data/broker_transport.dart';
import 'package:multitap_client/data/product_log.dart';
import 'package:multitap_client/data/server_save_log.dart';
import 'package:multitap_client/ui/app_theme.dart';
import 'package:multitap_client/ui/product_log_dialog.dart';
import 'package:multitap_client/ui/server_settings_dialog.dart';

const _secret = 'PRIVATE_SENTINEL';

Future<void> _form(
  WidgetTester tester, {
  required Future<BrokerProbeResult> Function(Uri) probe,
  required Future<void> Function(Uri) save,
}) async {
  tester.view.physicalSize = const Size(960, 800);
  tester.view.devicePixelRatio = 1;
  addTearDown(tester.view.resetPhysicalSize);
  addTearDown(tester.view.resetDevicePixelRatio);
  await tester.pumpWidget(
    MaterialApp(
      theme: buildAppTheme(),
      home: Scaffold(
        body: Center(
          child: SizedBox(
            width: 400,
            child: ServerSettingsForm(
              origin: null,
              onSave: save,
              onTest: probe,
              onClose: (_) {},
            ),
          ),
        ),
      ),
    ),
  );
  await tester.pumpAndSettle();
}

void main() {
  setUp(BrokerTransport.clear);
  tearDown(BrokerTransport.clear);

  test('Each kind of failure has its own number', () {
    final http = Uri.parse('http://example.test:18480');
    Object thrownBy(void Function() action) {
      try {
        action();
      } catch (error) {
        return error;
      }
      fail('Nothing was thrown.');
    }

    expect(
      serverSaveFailureNumber(const FileSystemException('refused', _secret)),
      1,
    );
    // The refusals the save path really meets.
    expect(
      serverSaveFailureNumber(thrownBy(() => BrokerTransport.token(http))),
      2,
    );
    BrokerTransport.restore(http, http.origin);
    expect(
      serverSaveFailureNumber(
        thrownBy(() => encodeBrokerTransportSettings(http)),
      ),
      3,
    );
    expect(
      serverSaveFailureNumber(
        thrownBy(() => normalizeBrokerOrigin('ftp://example.test')),
      ),
      4,
    );
    expect(serverSaveFailureNumber(StateError(_secret)), 5);
    expect(serverSaveFailureNumber(ArgumentError(_secret)), 6);
    expect(serverSaveFailureNumber(PlatformException(code: _secret)), 7);
    expect(serverSaveFailureNumber(MissingPluginException(_secret)), 7);
    expect(serverSaveFailureNumber(Exception(_secret)), 9);
  });

  test('The record holds numbers only, never the exception text', () {
    final log = ProductLog();
    addTearDown(log.dispose);
    recordServerSaveFailure(
      const FileSystemException(_secret, _secret, OSError(_secret, 5)),
      ServerSaveStage.save,
      log: log,
    );
    recordServerSaveFailure(
      ArgumentError(_secret),
      ServerSaveStage.keep,
      log: log,
    );
    expect(log.entries.first.event, LogEvent.serverSave);
    expect(log.entries.first.values, {
      'ok': false,
      'stage': 1,
      'error': 1,
      'osError': 5,
    });
    expect(log.entries.last.values, {'ok': false, 'stage': 2, 'error': 6});
    expect(
      jsonEncode([for (final entry in log.entries) entry.toJson()]),
      isNot(contains(_secret)),
    );
    // The record survives a restart of the app (the log file's reader).
    final reread = ProductLogEntry.parse(
      jsonDecode(jsonEncode(log.entries.first.toJson())),
    );
    expect(reread!.event, LogEvent.serverSave);
    expect(reread.values, log.entries.first.values);
  });

  testWidgets('Windows form: a save that throws is recorded as the save step', (
    tester,
  ) async {
    await _form(
      tester,
      probe: (_) async =>
          const BrokerProbeResult(version: '1.1.1', elapsedMs: 3),
      save: (_) async =>
          throw const FileSystemException(_secret, _secret, OSError('', 32)),
    );
    await tester.enterText(
      find.byKey(const Key('server-origin')),
      'https://vdi.example.test',
    );
    await tester.tap(find.byKey(const Key('test-server-connection')));
    await tester.pumpAndSettle();
    final logged = ProductLog.instance.entries.length;
    await tester.tap(find.byKey(const Key('save-server')));
    await tester.pumpAndSettle();
    expect(find.byKey(const Key('server-setting-error')), findsOneWidget);
    expect(find.text('서버 주소를 저장하지 못했어요'), findsOneWidget);
    final record = ProductLog.instance.entries.skip(logged).single;
    expect(record.event, LogEvent.serverSave);
    expect(record.values, {
      'ok': false,
      'stage': 1,
      'error': 1,
      'osError': 32,
    });
    await tester.pumpWidget(const SizedBox());
  });

  testWidgets('Windows form: an HTTP approval lost after the save is '
      'recorded as the keep step', (tester) async {
    final origin = Uri.parse('http://example.test:18480');
    late BrokerProbeResult result;
    await _form(
      tester,
      probe: (origin) async => result = BrokerProbeResult(
        version: '1.1.1',
        elapsedMs: 3,
        httpApproval: BrokerTransport.approve(origin, origin.origin),
      ),
      save: (_) async => result.httpApproval!.release(),
    );
    await tester.enterText(
      find.byKey(const Key('server-origin')),
      origin.origin,
    );
    await tester.tap(find.byKey(const Key('test-server-connection')));
    await tester.pumpAndSettle();
    await tester.tap(find.byKey(const Key('confirm-http-consent')));
    await tester.pumpAndSettle();
    final logged = ProductLog.instance.entries.length;
    await tester.tap(find.byKey(const Key('save-server')));
    await tester.pumpAndSettle();
    expect(find.byKey(const Key('server-setting-error')), findsOneWidget);
    expect(ProductLog.instance.entries.skip(logged).single.values, {
      'ok': false,
      'stage': 2,
      'error': 2,
    });
    await tester.pumpWidget(const SizedBox());
  });

  testWidgets('Android dialog: a store that fails is recorded', (tester) async {
    await tester.pumpWidget(
      MaterialApp(
        theme: buildAppTheme(),
        home: Scaffold(
          body: AndroidServerDialog(
            initial: null,
            save: (_) async => throw PlatformException(code: _secret),
            probe: (_) async =>
                const BrokerProbeResult(version: '1.1.1', elapsedMs: 3),
          ),
        ),
      ),
    );
    await tester.pumpAndSettle();
    await tester.enterText(
      find.byType(TextField),
      'https://vdi.example.test',
    );
    await tester.tap(find.text('연결 테스트'));
    await tester.pumpAndSettle();
    final logged = ProductLog.instance.entries.length;
    await tester.tap(find.text('저장'));
    await tester.pumpAndSettle();
    expect(find.text('서버 주소를 저장하지 못했어요.'), findsOneWidget);
    final record = ProductLog.instance.entries.skip(logged).single;
    expect(record.event, LogEvent.serverSave);
    expect(record.values, {'ok': false, 'stage': 1, 'error': 7});
    await tester.pumpWidget(const SizedBox());
  });

  testWidgets('The log viewer reads the record as a failed save with its '
      'numbers', (tester) async {
    await tester.pumpWidget(
      MaterialApp(
        home: Builder(
          builder: (context) => TextButton(
            onPressed: () => showProductLog(
              context,
              loadHost: () async => [
                ProductLogEntry(
                  DateTime.utc(2026, 11, 12),
                  LogEvent.serverSave,
                  const {'ok': false, 'stage': 1, 'error': 1, 'osError': 5},
                ),
              ],
            ),
            child: const Text('열기'),
          ),
        ),
      ),
    );
    await tester.tap(find.text('열기'));
    await tester.pumpAndSettle();
    // (Records of the tests above may be listed too.)
    final row = find.textContaining(
      '서버 주소 저장\n단계 1 · 오류 코드 1 · 시스템 오류 코드 5',
    );
    expect(row, findsOneWidget);
    expect(find.byIcon(Icons.error_outline_rounded), findsWidgets);
    expect(tester.takeException(), isNull);
  });
}
