import 'dart:async';
import 'dart:convert';
import 'dart:io';

import 'package:flutter/material.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:multitap_client/app.dart';
import 'package:multitap_client/data/broker_probe.dart';
import 'package:multitap_client/data/client_settings.dart';
import 'package:multitap_client/ui/app_theme.dart';
import 'package:multitap_client/ui/server_settings_dialog.dart';

const addressField = Key('server-origin');
const testButton = Key('test-server-connection');
const saveButton = Key('save-server');
final fixtureOrigin = Uri.parse('https://cloud.example.com');

bool canSave(WidgetTester tester) =>
    tester.widget<FilledButton>(find.byKey(saveButton)).onPressed != null;

Future<void> openSettings(
  WidgetTester tester, {
  required Future<void> Function(Uri) onSave,
  required Future<BrokerProbeResult> Function(Uri) onTest,
}) async {
  tester.view.physicalSize = const Size(1000, 800);
  tester.view.devicePixelRatio = 1;
  addTearDown(tester.view.resetPhysicalSize);
  addTearDown(tester.view.resetDevicePixelRatio);
  await tester.pumpWidget(
    MaterialApp(
      theme: buildAppTheme(),
      home: Builder(
        builder: (context) => TextButton(
          onPressed: () => showDialog<bool>(
            context: context,
            builder: (_) => ServerSettingsDialog(
              origin: fixtureOrigin,
              onSave: onSave,
              onTest: onTest,
            ),
          ),
          child: const Text('설정 열기'),
        ),
      ),
    ),
  );
  await tester.tap(find.text('설정 열기'));
  await tester.pumpAndSettle();
}

void main() {
  testWidgets(
    'Only the currently tested address can be saved; editing and Enter cannot bypass verification',
    (tester) async {
      final pending = Completer<BrokerProbeResult>();
      final tested = <Uri>[];
      final saved = <Uri>[];
      await openSettings(
        tester,
        onSave: (origin) async => saved.add(origin),
        onTest: (origin) {
          tested.add(origin);
          return tested.length == 1
              ? pending.future
              : Future.value(
                  const BrokerProbeResult(version: '0.3.3', elapsedMs: 32),
                );
        },
      );
      expect(canSave(tester), isFalse);
      await tester.enterText(
        find.byKey(addressField),
        'https://different.example.com',
      );
      await tester.testTextInput.receiveAction(TextInputAction.done);
      await tester.pump();
      expect(saved, isEmpty);
      expect(tested, isEmpty);
      await tester.tap(find.byKey(testButton));
      await tester.pump();
      expect(tested.single.toString(), 'https://different.example.com');
      expect(canSave(tester), isFalse);
      expect(
        tester.widget<FilledButton>(find.byKey(testButton)).onPressed,
        isNull,
      );
      pending.complete(
        const BrokerProbeResult(version: '0.3.3', elapsedMs: 45),
      );
      await tester.pumpAndSettle();
      expect(find.byKey(const Key('server-test-success')), findsOneWidget);
      expect(find.textContaining('45 ms'), findsOneWidget);
      expect(canSave(tester), isTrue);
      expect(saved, isEmpty);

      await tester.enterText(
        find.byKey(addressField),
        'https://edited.example.com',
      );
      await tester.pump();
      expect(find.byKey(const Key('server-test-success')), findsNothing);
      expect(canSave(tester), isFalse);
      await tester.testTextInput.receiveAction(TextInputAction.done);
      await tester.pump();
      expect(saved, isEmpty);
      // Returning to a previous address must still require a fresh test.
      await tester.enterText(
        find.byKey(addressField),
        'https://different.example.com',
      );
      await tester.pump();
      expect(canSave(tester), isFalse);
      await tester.enterText(
        find.byKey(addressField),
        'https://edited.example.com',
      );
      await tester.tap(find.byKey(testButton));
      await tester.pumpAndSettle();
      expect(canSave(tester), isTrue);
      await tester.testTextInput.receiveAction(TextInputAction.done);
      await tester.pumpAndSettle();
      expect(saved.single.toString(), 'https://edited.example.com');
      expect(find.byType(ServerSettingsDialog), findsNothing);
    },
  );

  testWidgets('Invalid addresses and failed tests never enable saving', (
    tester,
  ) async {
    var calls = 0;
    var saves = 0;
    await openSettings(
      tester,
      onSave: (_) async {
        saves++;
      },
      onTest: (_) async {
        calls++;
        throw const BrokerProbeException('dns');
      },
    );
    await tester.enterText(find.byKey(addressField), 'ftp://example.com');
    await tester.tap(find.byKey(testButton));
    await tester.pumpAndSettle();
    expect(calls, 0);
    expect(canSave(tester), isFalse);
    expect(find.byKey(const Key('server-setting-error')), findsOneWidget);
    await tester.enterText(find.byKey(addressField), 'https://vdi.example.com1');
    await tester.tap(find.byKey(testButton));
    await tester.pumpAndSettle();
    expect(calls, 1);
    expect(find.textContaining('철자'), findsOneWidget);
    expect(canSave(tester), isFalse);
    await tester.testTextInput.receiveAction(TextInputAction.done);
    await tester.pump();
    expect(saves, 0);
  });

  testWidgets('A failed repeat test revokes a previous successful result', (
    tester,
  ) async {
    var calls = 0;
    await openSettings(
      tester,
      onSave: (_) async => fail('Failed connection must not save'),
      onTest: (_) async {
        if (++calls == 1) {
          return const BrokerProbeResult(version: '0.3.3', elapsedMs: 23);
        }
        throw const BrokerProbeException('timeout');
      },
    );
    await tester.tap(find.byKey(testButton));
    await tester.pumpAndSettle();
    expect(canSave(tester), isTrue);
    await tester.tap(find.byKey(testButton));
    await tester.pumpAndSettle();
    expect(canSave(tester), isFalse);
    expect(find.byKey(const Key('server-test-success')), findsNothing);
    expect(find.byKey(const Key('server-setting-error')), findsOneWidget);
  });

  testWidgets('Closing during a test ignores the late result without saving', (
    tester,
  ) async {
    final pending = Completer<BrokerProbeResult>();
    var saves = 0;
    await openSettings(
      tester,
      onSave: (_) async {
        saves++;
      },
      onTest: (_) => pending.future,
    );
    await tester.tap(find.byKey(testButton));
    await tester.pump();
    await tester.tap(find.byKey(const Key('close-server-settings')));
    await tester.pumpAndSettle();
    pending.complete(const BrokerProbeResult(version: '0.3.3', elapsedMs: 55));
    await tester.pumpAndSettle();
    expect(find.byType(ServerSettingsDialog), findsNothing);
    expect(tester.takeException(), isNull);
    expect(saves, 0);
  });

  test('HTTPS legacy reload and versioned saves preserve exact origin; HTTP needs separate consent', () async {
    final temp = await Directory.systemTemp.createTemp('multitap-settings-');
    try {
      final file = File('${temp.path}/client/settings.json');
      final settings = ClientSettings(file);
      expect(await settings.load(), isNull);
      final origin = parseServerOrigin('vdi.example.com:18443/');
      expect(origin.toString(), 'https://vdi.example.com:18443');
      await settings.save(origin);
      expect(await ClientSettings(file).load(), origin);
      expect((jsonDecode(await file.readAsString()) as Map).keys, [
        'version',
        'serverOrigin',
      ]);
      expect(parseServerOrigin('http://example.com').scheme, 'http');
      await expectLater(
        settings.save(parseServerOrigin('http://example.com')),
        throwsFormatException,
      );
      for (final invalid in [
        'ftp://example.com',
        ' vdi.example.com ',
        'https://user:password@example.com',
        'https://example.com/api',
        'https://example.com?token=x',
        '',
      ]) {
        expect(() => parseServerOrigin(invalid), throwsFormatException);
      }
      await file.writeAsString('invalid json');
      expect(await settings.load(), isNull);
    } finally {
      await temp.delete(recursive: true);
    }
  });

  testWidgets('Login settings preserve the saved address when cancelled', (
    tester,
  ) async {
    tester.view.physicalSize = const Size(1440, 900);
    tester.view.devicePixelRatio = 1;
    addTearDown(tester.view.resetPhysicalSize);
    addTearDown(tester.view.resetDevicePixelRatio);
    Uri? saved;
    await tester.pumpWidget(
      MultitapApp(
        serverOrigin: fixtureOrigin,
        onServerChanged: (origin) async {
          saved = origin;
        },
      ),
    );
    await tester.pumpAndSettle();
    await tester.tap(find.byKey(const Key('open-server-settings')));
    await tester.pumpAndSettle();
    expect(canSave(tester), isFalse);
    await tester.enterText(find.byKey(addressField), 'http://example.com');
    await tester.tap(find.byKey(testButton));
    await tester.pumpAndSettle();
    expect(find.byKey(const Key('broker-http-consent')), findsOneWidget);
    await tester.tap(find.byKey(const Key('cancel-http-consent')));
    await tester.pumpAndSettle();
    expect(canSave(tester), isFalse);
    expect(saved, isNull);
    await tester.tap(find.text('취소'));
    await tester.pumpAndSettle();
    expect(find.byType(ServerSettingsDialog), findsNothing);
    expect(saved, isNull);
  });
}
