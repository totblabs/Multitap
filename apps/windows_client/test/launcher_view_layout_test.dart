// Real Flutter layouts; no app/native session is launched.
import 'package:flutter/material.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:multitap_client/data/launcher_memory.dart';
import 'package:multitap_client/models/stream_preparation.dart';

import 'launcher_view_test.dart' show showLauncherView, viewMachine;

void main() {
  testWidgets(
    'One responsive card layout stays usable across themes, narrow windows and enlarged text',
    (t) async {
      for (final brightness in Brightness.values) {
        for (final view in const [LauncherView.cards]) {
          for (final (size, scale) in [
            (const Size(1920, 1080), 1.0),
            (const Size(1366, 768), 1.5),
            (const Size(960, 640), 2.0),
            (const Size(420, 800), 2.0),
          ]) {
            final label =
                '${view.name}-${brightness.name}-${size.width.toInt()}x${size.height.toInt()}-text${(scale * 100).toInt()}';
            final fixture = await showLauncherView(
              t,
              view: view,
              size: size,
              scale: scale,
              brightness: brightness,
              machines: [
                viewMachine(101),
                viewMachine(102, stopped: true),
                viewMachine(103, preparation: StreamPreparation.login),
                for (var i = 104; i < 131; i++) viewMachine(i),
              ],
            );
            expect(t.takeException(), isNull, reason: label);
            expect(find.byKey(const Key('desktop-view')), findsNothing);
            final selector = find.byKey(const Key('desktop-previews'));
            final selectorRect = t.getRect(selector);
            expect(selectorRect.left, greaterThanOrEqualTo(0), reason: label);
            // One height with the title row's other buttons (2026-11-03:
            // 미리보기 used to stand taller than 새로고침).
            expect(
              selectorRect.height,
              closeTo(
                t.getSize(find.byKey(const Key('refresh-desktops'))).height,
                .01,
              ),
              reason: label,
            );
            expect(
              selectorRect.height,
              greaterThanOrEqualTo(34),
              reason: label,
            );
            expect(
              selectorRect.right,
              lessThanOrEqualTo(size.width),
              reason: label,
            );
            await t.tap(selector);
            await t.pumpAndSettle();
            expect(
              (await fixture.memory.load('https://fixture.example'))
                  .showPreviews,
              isFalse,
            );
            expect(find.byKey(const Key('view-list')), findsNothing);
            expect(find.byKey(const Key('view-mini')), findsNothing);
            await t.tap(selector);
            await t.pumpAndSettle();
            expect(t.takeException(), isNull, reason: label);
            await t.pumpWidget(const SizedBox());
            await t.pump();
          }
        }
      }
    },
  );
}
