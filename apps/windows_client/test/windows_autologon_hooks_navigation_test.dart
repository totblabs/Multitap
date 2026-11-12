import 'package:flutter/material.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:multitap_client/ui/app_theme.dart';
import 'package:multitap_client/ui/host_settings_navigation.dart';
import 'package:multitap_client/ui/host_settings_screen.dart';

import 'host_settings_test.dart' as fixture;

void main() {
  test('Windows login appends sixth section while original page indices stay stable', () {
    // Page 3 is an empty, unlisted slot.
    expect(HostSettingsNavigation.titles.take(5).toList(), [
      '개요',
      '서버 연결',
      '가상 화면',
      '',
      '진단 보고서',
    ]);
    expect(HostSettingsNavigation.shortTitles.take(5).toList(), [
      '개요',
      '서버',
      '화면',
      '',
      '진단',
    ]);
    expect(HostSettingsNavigation.order, isNot(contains(3)));
    expect(HostSettingsNavigation.titles[5], 'Windows 로그인');
    expect(HostSettingsNavigation.icons[5], Icons.person_outline_rounded);
  });
  testWidgets(
    'actual sidebar opens legacy unsupported login read-only and keeps the diagnostics route',
    (tester) async {
      final api = fixture.FakeHostSettings(registered: true);
      tester.view.physicalSize = const Size(960, 640);
      tester.view.devicePixelRatio = 1;
      addTearDown(tester.view.resetPhysicalSize);
      addTearDown(tester.view.resetDevicePixelRatio);
      await tester.pumpWidget(
        MaterialApp(
          theme: buildAppTheme(brightness: Brightness.dark),
          home: HostSettingsScreen(
            api: api,
            closeGuard: fixture.FakeCloseGuard(),
          ),
        ),
      );
      await tester.pumpAndSettle();
      Future<void> select(int page) async {
        final item = find.byKey(Key('host-nav-$page'));
        await tester.ensureVisible(item);
        await tester.tap(item);
        await tester.pumpAndSettle();
      }

      await select(5);
      expect(find.text('이 Host는 자동 로그인 설정을 지원하지 않아요'), findsOneWidget);
      expect(find.byKey(const Key('host-windows-apply')), findsNothing);
      expect(find.byKey(const Key('host-nav-3')), findsNothing);
      await select(4);
      expect(find.byKey(const Key('host-copy-diagnostics')), findsOneWidget);
      // Read-only: status and the page's own status reads, nothing applied.
      expect(api.requests.every(fixture.isReadOnlyHostRequest), isTrue);
      expect(tester.takeException(), isNull);
      await tester.pumpWidget(const SizedBox());
    },
  );
}
