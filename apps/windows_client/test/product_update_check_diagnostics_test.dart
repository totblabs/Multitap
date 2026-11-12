import 'dart:convert';

import 'package:flutter/material.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:multitap_client/data/product_log.dart';
import 'package:multitap_client/data/product_updates.dart';
import 'package:multitap_client/ui/product_log_dialog.dart';
import 'package:multitap_client/ui/product_update_dialog.dart';

import 'product_updates_test.dart' show Transport;

void main() {
  for (final reason in [1, 2, 3]) {
    testWidgets(
      'update check preserves fixed failure reason$reason without exception data',
      (tester) async {
        const secret = 'PRIVATE_ERROR_TEXT_PASSWORD_TOKEN_URL';
        final before = ProductLog.instance.entries.toSet();
        List<ProductLogEntry> fresh() => ProductLog.instance.entries
            .where((entry) => !before.contains(entry) && entry.event == LogEvent.updateResult)
            .toList();
        final api = ProductUpdates(
          'https://updates.example',
          transport: Transport((_) {
            if (reason == 2) {
              final body = utf8.encode(jsonEncode({'releases': secret}));
              return UpdateResponse(200, body.length, Stream.value(body));
            }
            throw StateError(secret);
          }),
        );
        try {
          await tester.pumpWidget(
            MaterialApp(
              home: Builder(
                builder: (context) => TextButton(
                  onPressed: () => showDialog<void>(
                    context: context,
                    barrierDismissible: false,
                    builder: (_) => ProductUpdateDialog(
                      server: 'https://updates.example',
                      product: 'client',
                      currentVersion: reason == 1 ? '' : '1.0.7',
                      api: api,
                    ),
                  ),
                  child: const Text('open'),
                ),
              ),
            ),
          );
          await tester.tap(find.text('open'));
          // Observe the real route's result; no indefinite animation settle.
          for (var n = 0; n < 20 && fresh().isEmpty; n++) {
            await tester.pump(const Duration(milliseconds: 100));
            await tester.runAsync(() => Future<void>.delayed(Duration.zero));
          }
          await tester.pump(const Duration(milliseconds: 300));
          final actual = fresh().single;
          expect(actual.values['ok'], isFalse);
          expect(actual.values['stage'], 1);
          expect(actual.values['error'], reason);
          expect(
            actual.values.values.every(
              (value) => value is int || value is bool,
            ),
            isTrue,
          );
          expect(jsonEncode(actual.toJson()), isNot(contains(secret)));
          expect(
            jsonEncode(actual.toJson()),
            isNot(contains('updates.example')),
          );
          expect(tester.takeException(), isNull);
        } finally {
          await tester.pumpWidget(const SizedBox.shrink());
          await tester.pump(const Duration(seconds: 3));
          api.close();
        }
      },
    );
  }

  testWidgets(
    'fixed failure reasons read separately while legacy records keep their title',
    (tester) async {
      // Keep these fixtures ahead of earlier local records without clearing
      // the real bounded log shared by this isolate's preceding tests.
      final now = DateTime.now().toUtc().add(const Duration(days: 1));
      await tester.pumpWidget(
        MaterialApp(
          home: Builder(
            builder: (context) => TextButton(
              onPressed: () => showProductLog(
                context,
                loadHost: () async => [
                  for (final reason in [1, 2, 3])
                    ProductLogEntry(
                      now.add(Duration(seconds: reason)),
                      LogEvent.updateResult,
                      {'ok': false, 'stage': 1, 'error': reason},
                    ),
                  ProductLogEntry(now, LogEvent.updateResult, const {
                    'ok': false,
                  }),
                ],
              ),
              child: const Text('open'),
            ),
          ),
        ),
      );
      await tester.tap(find.text('open'));
      await tester.pumpAndSettle();
      expect(find.text('업데이트 확인 실패 · 실행 버전 확인 필요'), findsWidgets);
      expect(find.text('업데이트 확인 실패 · 업데이트 목록 형식 오류'), findsWidgets);
      expect(find.text('업데이트 확인 실패 · 서버 목록을 불러오지 못함'), findsWidgets);
      expect(find.text('업데이트 확인 실패'), findsOneWidget);
      expect(tester.takeException(), isNull);
      await tester.pumpWidget(const SizedBox.shrink());
      await tester.pump(const Duration(seconds: 3));
    },
  );
}
