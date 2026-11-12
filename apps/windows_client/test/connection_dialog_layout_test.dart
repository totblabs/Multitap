import 'package:flutter/material.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:multitap_client/data/broker_api.dart';
import 'package:multitap_client/ui/app_theme.dart';
import 'package:multitap_client/ui/change_password_dialog.dart';
import 'package:multitap_client/ui/console_resume_dialog.dart';
import 'package:multitap_client/ui/stream_settings_dialog.dart';
import 'package:multitap_client/ui/virtual_display_dialog.dart';

import 'display_remote_test.dart' show machine;

void main() {
  for (final brightness in Brightness.values) {
    testWidgets(
      'Connection dialogs fit narrow and short $brightness windows with large text and keep cancel reachable',
      (tester) async {
        tester.view.devicePixelRatio = 1;
        addTearDown(tester.view.resetDevicePixelRatio);
        addTearDown(tester.view.resetPhysicalSize);
        var actions = 0, transfers = 0;
        final dialogs = <(Widget Function(), String)>[
          (() => StreamSettingsDialog(machine: machine(2)), '취소'),
          (
            () => VirtualDisplayDialog(
              machine: machine(2),
              onApply: (_) async {
                actions++;
                return machine(1);
              },
            ),
            '닫기',
          ),
          // Starts its transfer on open (the tile already explained it); the
          // longest state with a button is the failure and its 확인.
          (
            () => ConsoleResumeDialog(
              machine: machine(2),
              resume: () async {
                transfers++;
                throw const BrokerException('console_display_not_ready');
              },
            ),
            '확인',
          ),
          (
            () => ChangePasswordDialog(
              onChange: (_, _, _) async {
                actions++;
              },
            ),
            '취소',
          ),
        ];
        for (final spec in [
          (const Size(320, 480), 2.0),
          (const Size(640, 360), 1.25),
        ]) {
          tester.view.physicalSize = spec.$1;
          for (final dialog in dialogs) {
            await tester.pumpWidget(
              MaterialApp(
                theme: buildAppTheme(brightness: brightness),
                builder: (context, child) => MediaQuery(
                  data: MediaQuery.of(context)
                      .copyWith(textScaler: TextScaler.linear(spec.$2)),
                  child: child!,
                ),
                home: Scaffold(
                  body: Builder(
                    builder: (context) => TextButton(
                      onPressed: () => showDialog<void>(
                        context: context,
                        builder: (_) => dialog.$1(),
                      ),
                      child: const Text('open'),
                    ),
                  ),
                ),
              ),
            );
            await tester.tap(find.text('open'));
            await tester.pumpAndSettle();
            expect(
              tester.takeException(),
              isNull,
              reason: '${dialog.$1().runtimeType} ${spec.$1} ${spec.$2}',
            );
            // Any button kind: a lone 확인 is the filled main button.
            final cancel = find
                .ancestor(
                  of: find.text(dialog.$2),
                  matching: find.byWidgetPredicate(
                    (w) => w is ButtonStyleButton,
                  ),
                )
                .first;
            await tester.ensureVisible(cancel);
            await tester.pumpAndSettle();
            await tester.tap(cancel);
            await tester.pumpAndSettle();
            expect(find.byType(AlertDialog), findsNothing);
            expect(tester.takeException(), isNull);
            expect(
              actions,
              0,
              reason: 'Closing a dialog must not configure a display or change credentials',
            );
            await tester.pumpWidget(const SizedBox.shrink());
          }
        }
        expect(
          transfers,
          2,
          reason: 'The Windows takeover runs once per opening and closing its failure never repeats it',
        );
      },
    );
  }
}
