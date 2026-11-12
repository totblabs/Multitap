// The server dialog's HTTPS/HTTP choice must not wrap unevenly at any width
// or text size. Widget layout only.
import 'package:flutter/material.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:multitap_client/ui/app_theme.dart';
import 'package:multitap_client/ui/broker_transport_controls.dart';

/// The whole tappable option of [value], whatever widget draws it.
Rect _option(WidgetTester t, String value) {
  final label = find.byKey(Key('broker-scheme-$value'));
  final button = find.ancestor(
    of: label,
    matching: find.byWidgetPredicate((w) => w is ButtonStyleButton),
  );
  return t.getRect(button.evaluate().isEmpty ? label : button.first);
}

Future<List<String>> _pump(
  WidgetTester t, {
  required double width,
  double scale = 1,
  bool enabled = true,
  Brightness brightness = Brightness.dark,
}) async {
  final picked = <String>[];
  t.platformDispatcher.textScaleFactorTestValue = scale;
  addTearDown(t.platformDispatcher.clearTextScaleFactorTestValue);
  await t.pumpWidget(
    MaterialApp(
      theme: buildAppTheme(brightness: brightness),
      home: Scaffold(
        body: Center(
          child: SizedBox(
            width: width,
            child: BrokerTransportChoice(
              scheme: 'https',
              onChanged: enabled ? picked.add : null,
            ),
          ),
        ),
      ),
    ),
  );
  await t.pumpAndSettle();
  return picked;
}

void main() {
  testWidgets(
    'HTTPS and HTTP are two equal halves of one row at every width and text size',
    (t) async {
      for (final width in const [240.0, 300.0, 360.0, 432.0]) {
        for (final scale in const [1.0, 1.3, 1.6, 2.0]) {
          await _pump(t, width: width, scale: scale);
          final https = _option(t, 'https'), http = _option(t, 'http');
          final at = '$width dp, text ×$scale';
          expect(https.top, closeTo(http.top, .5), reason: at);
          expect(https.height, closeTo(http.height, .5), reason: at);
          expect(https.width, closeTo(http.width, 1), reason: at);
          expect(http.left, greaterThanOrEqualTo(https.right - 1), reason: at);
          expect(http.right - https.left, closeTo(width, 1), reason: at);
          expect(t.takeException(), isNull, reason: at);
        }
      }
    },
  );

  testWidgets('A choice reports its scheme; without a handler nothing does', (
    t,
  ) async {
    final picked = await _pump(t, width: 360);
    await t.tap(find.byKey(const Key('broker-scheme-http')));
    await t.pumpAndSettle();
    expect(picked, ['http']);
    final none = await _pump(t, width: 360, enabled: false);
    await t.tap(find.byKey(const Key('broker-scheme-http')));
    await t.pumpAndSettle();
    expect(none, isEmpty);
  });
}
