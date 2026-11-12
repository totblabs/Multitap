import 'dart:convert';

import 'package:flutter/material.dart';
import 'package:flutter/services.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:multitap_client/data/broker_api.dart';
import 'package:multitap_client/data/remote_session.dart';
import 'package:multitap_client/ui/app_theme.dart';
import 'package:multitap_client/ui/session_stats_dialog.dart';
import 'package:multitap_client/ui/stream_observation_overlay.dart';

void main() {
  testWidgets(
    'new diagnostics controls stay reachable at narrow width and copy explicit JSON',
    (t) async {
      t.view.physicalSize = const Size(360, 640);
      t.view.devicePixelRatio = 1;
      addTearDown(t.view.resetPhysicalSize);
      addTearDown(t.view.resetDevicePixelRatio);
      var enabled = false;
      final copies = <String>[];
      TestDefaultBinaryMessengerBinding.instance.defaultBinaryMessenger
          .setMockMethodCallHandler(SystemChannels.platform, (call) async {
            if (call.method == 'Clipboard.setData') {
              copies.add((call.arguments as Map)['text'] as String);
            }
            return null;
          });
      addTearDown(
        () => TestDefaultBinaryMessengerBinding.instance.defaultBinaryMessenger
            .setMockMethodCallHandler(SystemChannels.platform, null),
      );
      await t.pumpWidget(
        MaterialApp(
          theme: buildAppTheme(brightness: Brightness.dark),
          home: Scaffold(
            body: SessionStatsDialog(
              title: 'Stats',
              machine: 'fixture',
              sections: () => const [
                SessionMetricSection('Queue', [SessionMetric('age', '1 ms')]),
              ],
              observationEnabled: () => enabled,
              onObservationChanged: (v) => enabled = v,
              reportJson: () => jsonEncode({
                'schema': 'stream-observability-v1',
                'enabled': enabled,
              }),
            ),
          ),
        ),
      );
      await t.pump();
      expect(t.takeException(), isNull);
      await t.tap(find.byKey(const Key('toggle-stream-observability')));
      await t.pump();
      expect(enabled, true);
      await t.tap(find.byKey(const Key('copy-stream-observability')));
      await t.pump();
      expect(copies, hasLength(1));
      expect(jsonDecode(copies.single)['enabled'], true);
      expect(t.takeException(), isNull);
      await t.pumpWidget(const SizedBox());
    },
  );
  testWidgets('overlay cannot intercept remote input or focus', (t) async {
    final session = RemoteSession(
      connection: BrokerConnection(
        sessionId: '44444444-4444-4444-8444-444444444444',
        webSocketUrl: Uri.parse('wss://example.test/unused'),
      ),
      token: 'not-exported',
      observe: true,
    );
    var taps = 0;
    await t.pumpWidget(
      MaterialApp(
        home: Stack(
          children: [
            Positioned.fill(
              child: GestureDetector(
                behavior: HitTestBehavior.opaque,
                onTap: () => taps++,
                child: const ColoredBox(color: Colors.black),
              ),
            ),
            StreamObservationOverlay(session: session),
          ],
        ),
      ),
    );
    await t.tapAt(t.getCenter(find.textContaining('TCP')));
    await t.pump();
    expect(taps, 1);
    expect(
      FocusManager.instance.primaryFocus?.context?.widget,
      isNot(isA<StreamObservationOverlay>()),
    );
    await t.pumpWidget(const SizedBox());
  });
}
