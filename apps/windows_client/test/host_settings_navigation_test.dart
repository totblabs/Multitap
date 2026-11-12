import 'package:flutter/material.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:multitap_client/data/appearance_settings.dart';
import 'package:multitap_client/data/host_overview.dart';
import 'package:multitap_client/ui/host_overview_panels.dart';
import 'package:multitap_client/ui/host_settings_navigation.dart';
import 'package:multitap_client/ui/host_settings_screen.dart';
import 'package:multitap_client/ui/themed_app.dart';

import 'host_settings_test.dart'
    show FakeCloseGuard, FakeHostSettings, isReadOnlyHostRequest;

const _online = {
  'state': 'online',
  'online': true,
  'heartbeatAgeSeconds': 1,
  'lastReadyAt': 1792700400000,
  'lastHeartbeatAt': 1792700400000,
  'failureReason': '',
};

void main() {
  Future<void> show(
    WidgetTester tester,
    FakeHostSettings api, {
    Size size = const Size(886, 673),
    double textScale = 1,
    ThemeMode mode = ThemeMode.dark,
    GlobalKey? boundary,
    Future<String?> Function(String server, String version)? updateCheck,
  }) async {
    tester.view.physicalSize = size;
    tester.view.devicePixelRatio = 1;
    addTearDown(tester.view.resetPhysicalSize);
    addTearDown(tester.view.resetDevicePixelRatio);
    final appearance = AppAppearance(mode: mode);
    addTearDown(appearance.dispose);
    await tester.pumpWidget(
      RepaintBoundary(
        key: boundary,
        child: ThemedApp(
          key: UniqueKey(),
          appearance: appearance,
          home: MediaQuery(
            data: MediaQueryData(
              size: size,
              textScaler: TextScaler.linear(textScale),
            ),
            child: HostSettingsScreen(
              api: api,
              closeGuard: FakeCloseGuard(),
              updateCheck: updateCheck,
            ),
          ),
        ),
      ),
    );
    await tester.pumpAndSettle();
  }

  // The navigation shows no state words: an area that needs the user carries
  // one amber dot. The state itself is what the screen hands the navigation.
  HostSetupState status(WidgetTester tester, int index) {
    final state = tester
        .widget<HostSettingsNavigation>(find.byType(HostSettingsNavigation))
        .states[index];
    expect(
      find.byKey(Key('host-nav-dot-$index')),
      index != 0 && state.needsUser ? findsOneWidget : findsNothing,
      reason: 'dot of $index for $state',
    );
    return state;
  }

  Future<void> select(WidgetTester tester, int index) async {
    final item = find.byKey(Key('host-nav-$index'));
    await tester.ensureVisible(item);
    await tester.tap(item);
    await tester.pumpAndSettle();
  }

  testWidgets(
    'Sidebar selects one settings panel without changing host configuration',
    (tester) async {
      final api = FakeHostSettings(registered: true)..displays = 2;
      await show(tester, api);
      expect(find.byKey(const Key('host-settings-sidebar')), findsOneWidget);
      expect(
        tester.getRect(find.byKey(const Key('host-settings-sidebar'))).left,
        0,
      );
      await select(tester, 2);
      expect(find.byKey(const Key('host-display-setup')), findsOneWidget);
      expect(find.byKey(const Key('host-devices')), findsNothing);
      expect(
        tester
            .widget<Text>(find.byKey(const Key('host-selected-section')))
            .data,
        '가상 화면',
      );
      await select(tester, 6);
      expect(find.byKey(const Key('host-devices')), findsOneWidget);
      expect(find.byKey(const Key('host-display-setup')), findsNothing);
      await select(tester, 4);
      expect(find.byKey(const Key('host-copy-diagnostics')), findsOneWidget);
      expect(api.requests.every(isReadOnlyHostRequest), isTrue);
      expect(tester.takeException(), isNull);
      await tester.pumpWidget(const SizedBox.shrink());
    },
  );

  testWidgets(
    'Server completion requires registration, running service and fresh broker acknowledgement',
    (tester) async {
      for (final fixture in [
        (false, true, _online, HostSetupState.setup),
        (true, true, <String, dynamic>{}, HostSetupState.attention),
        (true, false, _online, HostSetupState.attention),
        (
          true,
          true,
          {..._online, 'heartbeatAgeSeconds': 31},
          HostSetupState.attention,
        ),
        (true, true, _online, HostSetupState.complete),
      ]) {
        final api = FakeHostSettings(registered: fixture.$1)
          ..running = fixture.$2
          ..statusOverrides = {'broker': fixture.$3};
        await show(tester, api);
        expect(status(tester, 1), fixture.$4);
        await tester.pumpWidget(const SizedBox.shrink());
      }
    },
  );

  testWidgets(
    'Pending and cancelling requests have distinct non-complete badges',
    (tester) async {
      final api = FakeHostSettings()
        ..connectionRequest = {
          'status': 'pending',
          'origin': 'https://cloud.example.com',
          'requestId': 'fixture-only',
          'expiresAt': DateTime.now()
              .add(const Duration(minutes: 10))
              .toIso8601String(),
        };
      await show(tester, api);
      expect(status(tester, 1), HostSetupState.approval);
      await tester.pumpWidget(const SizedBox.shrink());
      api.connectionRequest = {
        ...api.connectionRequest,
        'status': 'cancelled',
        'error': 'cancel_pending',
      };
      await show(tester, api);
      expect(status(tester, 1), HostSetupState.checking);
      await tester.pumpWidget(const SizedBox.shrink());
    },
  );

  testWidgets(
    'Display completion rejects stale ready flags during restart, recovery or console deferral',
    (tester) async {
      for (final flags in [
        {'rebootRequired': true},
        {'recoveryRequired': true},
        {'configurationPending': true},
        {'owned': false},
      ]) {
        final api = FakeHostSettings(registered: true)
          ..statusOverrides = {
            'display': {
              'installed': true,
              'deviceReady': true,
              'owned': true,
              'ready': true,
              'monitors': [
                {'id': '0', 'width': 1920, 'height': 1080},
              ],
              ...flags,
            },
          };
        await show(tester, api);
        expect(status(tester, 2), HostSetupState.attention);
        expect(
          find.descendant(
            of: find.byKey(const Key('host-open-display')),
            matching: find.textContaining('준비됨'),
          ),
          findsNothing,
        );
        expect(find.text('스트리밍 가능'), findsNothing);
        await tester.pumpWidget(const SizedBox.shrink());
      }
    },
  );

  testWidgets(
    'The unused page slot is neither listed nor part of the host state',
    (tester) async {
      final api = FakeHostSettings(registered: true)
        ..displays = 2
        ..statusOverrides = {'broker': _online};
      await show(tester, api);
      expect(status(tester, 0), HostSetupState.complete);
      expect(status(tester, 2), HostSetupState.complete);
      expect(find.byKey(const Key('host-nav-3')), findsNothing);
      await tester.pumpWidget(const SizedBox.shrink());
    },
  );

  testWidgets(
    'Narrow large-text dark and light windows retain navigation and editable draft',
    (tester) async {
      for (final mode in [ThemeMode.dark, ThemeMode.light]) {
        final api = FakeHostSettings();
        await show(
          tester,
          api,
          size: const Size(640, 600),
          textScale: 1.25,
          mode: mode,
        );
        expect(
          tester
              .widget<HostSettingsNavigation>(
                find.byType(HostSettingsNavigation),
              )
              .rail,
          isTrue,
        );
        await select(tester, 1);
        final address = find.byKey(const Key('host-server-origin'));
        await tester.ensureVisible(address);
        await tester.enterText(address, 'https://draft.example.com');
        await select(tester, 2);
        await select(tester, 1);
        expect(
          tester.widget<TextField>(address).controller!.text,
          'https://draft.example.com',
        );
        expect(api.requests.every(isReadOnlyHostRequest), isTrue);
        expect(tester.takeException(), isNull);
        await tester.pumpWidget(const SizedBox.shrink());
      }
    },
  );

  testWidgets(
    'Sidebar follows the setup flow and keeps diagnostics last, apart',
    (tester) async {
      for (final (size, scale) in [
        (const Size(886, 673), 1.0),
        (const Size(640, 600), 1.25),
      ]) {
        final api = FakeHostSettings(registered: true)..displays = 2;
        await show(tester, api, size: size, textScale: scale);
        expect(HostSettingsNavigation.order, [0, 1, 5, 2, 6]);
        expect(HostSettingsNavigation.support, [4]);
        final tops = [
          for (final page in [0, 1, 5, 2, 6, 4])
            tester.getRect(find.byKey(Key('host-nav-$page'))).top,
        ];
        for (var i = 1; i < tops.length; i++) {
          expect(tops[i], greaterThan(tops[i - 1]), reason: '$size #$i');
        }
        final support = find.byKey(const Key('host-nav-support'));
        expect(
          find.descendant(
            of: support,
            matching: find.byKey(const Key('host-nav-4')),
          ),
          findsOneWidget,
        );
        for (final page in HostSettingsNavigation.order) {
          expect(
            find.descendant(
              of: support,
              matching: find.byKey(Key('host-nav-$page')),
            ),
            findsNothing,
          );
        }
        // Diagnostics that are simply available carry no dot, and no row
        // carries state words.
        expect(find.byKey(const Key('host-nav-dot-4')), findsNothing);
        expect(
          find.descendant(
            of: find.byType(HostSettingsNavigation),
            matching: find.byType(HostSetupBadge),
          ),
          findsNothing,
        );
        expect(api.requests.every(isReadOnlyHostRequest), isTrue);
        expect(tester.takeException(), isNull);
        await tester.pumpWidget(const SizedBox.shrink());
      }
    },
  );

  testWidgets(
    'Overview gives every area one plain line, a state only when it needs one, and a way to its page',
    (tester) async {
      final api = FakeHostSettings(registered: true)
        ..displays = 2
        ..statusOverrides = {'broker': _online};
      await show(tester, api);
      final areas = find.byKey(const Key('host-overview-areas'));
      // The order of the approved design: sign-in, screen, devices, server.
      const rows = [
        (5, 'host-open-windows'),
        (2, 'host-open-display'),
        (6, 'host-open-devices'),
        (1, 'host-open-server'),
      ];
      var previous = double.negativeInfinity;
      var flagged = 0;
      for (final (page, key) in rows) {
        final open = find.descendant(of: areas, matching: find.byKey(Key(key)));
        final row = tester.widget<HostAreaRow>(open);
        expect(
          find.descendant(
            of: open,
            matching: find.text(HostSettingsNavigation.titles[page]),
          ),
          findsOneWidget,
        );
        // A dot and its word at the right only where the area needs
        // something; a fine area says nothing more (2026-11-11).
        final fine = row.state == HostSetupState.complete;
        if (!fine) flagged++;
        expect(
          find.descendant(of: open, matching: find.byType(HostSetupBadge)),
          fine ? findsNothing : findsOneWidget,
          reason: key,
        );
        final top = tester.getRect(open).top;
        expect(top, greaterThan(previous), reason: key);
        previous = top;
      }
      expect(
        find.descendant(of: areas, matching: find.byType(HostStateDot)),
        findsNWidgets(flagged),
      );
      expect(
        find.descendant(of: areas, matching: find.text('정상')),
        findsNothing,
      );
      // No cards: the areas are lines, not bordered panels.
      expect(
        find.descendant(of: areas, matching: find.byType(Card)),
        findsNothing,
      );
      expect(
        tester
            .widget<HostReadiness>(
              find.byKey(const Key('host-overview-state')),
            )
            .label,
        isNot(isEmpty),
      );
      expect(find.byType(HostStatusPill), findsNothing);
      // One banner for one problem; none when all is well.
      expect(find.byType(HostProblemBanner), findsNothing);
      // Long connection times are diagnostics, not overview material.
      expect(find.text('서버 응답 경과'), findsNothing);
      await tester.tap(find.byKey(const Key('host-open-windows')));
      await tester.pumpAndSettle();
      expect(
        tester
            .widget<Text>(find.byKey(const Key('host-selected-section')))
            .data,
        'Windows 로그인',
      );
      expect(api.requests.every(isReadOnlyHostRequest), isTrue);
      expect(tester.takeException(), isNull);
      await tester.pumpWidget(const SizedBox.shrink());
    },
  );

  testWidgets(
    'Each page opens with one state line and folds supporting detail',
    (tester) async {
      final api = FakeHostSettings(registered: true)
        ..displays = 2
        ..statusOverrides = {
          'broker': _online,
          'windowsAutologon': {
            'schema': 1,
            'supported': true,
            'administrator': true,
            'state': 'off',
            'reason': 'none',
            'revision': List.filled(64, 'a').join(),
          },
        };
      await show(tester, api);
      // A page that is fine shows only its title; one that needs the user
      // carries ONE amber status (2026-11-11: no "● 정상" pills).
      for (final (page, words) in [
        (1, null),
        // Auto-login off is a setup step.
        (5, '설정 필요'),
        (2, null),
        (6, null),
      ]) {
        await select(tester, page);
        final line = find.byKey(const Key('host-selected-status'));
        if (words == null) {
          expect(line, findsNothing, reason: '$page');
          expect(find.byType(HostStatusPill), findsNothing, reason: '$page');
          expect(find.text('정상'), findsNothing, reason: '$page');
          continue;
        }
        final pill = tester.widget<HostStatusPill>(line);
        expect(pill.label, words, reason: '$page');
        expect(pill.state, HostRouteState.warn, reason: '$page');
        // ONE status on the page's heading.
        expect(find.byType(HostStatusPill), findsOneWidget);
      }
      // The virtual screen's explanation waits behind 자세히.
      await select(tester, 2);
      expect(find.textContaining('모니터 수(1~2대)'), findsNothing);
      await tester.ensureVisible(find.byKey(const Key('host-display-help')));
      await tester.tap(find.byKey(const Key('host-display-help')));
      await tester.pumpAndSettle();
      expect(find.textContaining('모니터 수(1~2대)'), findsOneWidget);
      // 진단: a summary, one primary action, technical rows folded.
      await select(tester, 4);
      expect(find.byKey(const Key('host-selected-status')), findsNothing);
      expect(
        tester.widget(find.byKey(const Key('host-copy-diagnostics'))),
        isA<FilledButton>(),
      );
      expect(find.byType(FilledButton), findsOneWidget);
      expect(find.text('Host 버전'), findsOneWidget);
      const technical = [
        '서버 응답 경과',
        '마지막 연결 시각',
        '마지막 응답 시각',
        '연결 안내',
        '가상 화면 드라이버',
      ];
      for (final row in technical) {
        expect(find.text(row), findsNothing, reason: row);
      }
      await tester.ensureVisible(
        find.byKey(const Key('host-diagnostics-details')),
      );
      await tester.tap(find.byKey(const Key('host-diagnostics-details')));
      await tester.pumpAndSettle();
      for (final row in technical) {
        expect(find.text(row), findsOneWidget, reason: row);
      }
      expect(api.requests.every(isReadOnlyHostRequest), isTrue);
      expect(tester.takeException(), isNull);
      await tester.pumpWidget(const SizedBox.shrink());
    },
  );

  testWidgets(
    'Header keeps the manual update check as a quiet icon and every icon has a tooltip',
    (tester) async {
      for (final (size, scale) in [
        (const Size(886, 673), 1.0),
        (const Size(640, 600), 1.25),
      ]) {
        await show(
          tester,
          FakeHostSettings(registered: true),
          size: size,
          textScale: scale,
        );
        // No update is known: no amber button in the header, the check is
        // one of its quiet icons.
        for (final key in [
          'host-header-updates',
          'host-refresh',
          'host-header-info',
        ]) {
          expect(
            tester.widget<IconButton>(find.byKey(Key(key))).tooltip,
            isNotEmpty,
            reason: key,
          );
        }
        expect(find.byTooltip('업데이트 확인'), findsOneWidget);
        expect(find.byTooltip('Host 정보'), findsOneWidget);
        expect(find.byTooltip('테마'), findsOneWidget);
        expect(find.text('Multitap Host'), findsOneWidget);
        expect(
          tester.widget<Text>(find.byKey(const Key('host-version'))).data,
          '0.2.6',
        );
        expect(tester.takeException(), isNull);
        await tester.pumpWidget(const SizedBox.shrink());
      }
    },
  );

  testWidgets(
    'An update the catalog names gets the one amber button, for this server and version only',
    (tester) async {
      final asked = <(String, String)>[];
      String? newer = '1.0.52';
      Future<String?> check(String server, String version) async {
        asked.add((server, version));
        return newer;
      }

      final api = FakeHostSettings(registered: true)
        ..statusOverrides = {'version': '1.0.51', 'broker': _online};
      await show(tester, api, updateCheck: check);
      expect(asked, [('https://cloud.example.com', '1.0.51')]);
      final update = find.byKey(const Key('host-header-updates'));
      expect(tester.widget(update), isA<FilledButton>());
      expect(
        find.descendant(of: update, matching: find.text('1.0.52로 업데이트')),
        findsOneWidget,
      );
      // The status is read every three seconds; the catalog is not.
      await tester.pump(const Duration(seconds: 10));
      await tester.pumpAndSettle();
      expect(asked, hasLength(1));
      // A manual refresh asks again, and a withdrawn release removes the button.
      newer = null;
      await tester.tap(find.byKey(const Key('host-refresh')));
      await tester.pumpAndSettle();
      expect(asked, hasLength(2));
      expect(tester.widget(update), isA<IconButton>());
      expect(find.textContaining('업데이트'), findsNothing);
      // Only the status and catalog were read: nothing was installed.
      expect(api.requests.every(isReadOnlyHostRequest), isTrue);
      expect(tester.takeException(), isNull);
      await tester.pumpWidget(const SizedBox.shrink());

      // A narrow, large-text window keeps the amber button as an icon.
      newer = '1.0.60';
      await show(
        tester,
        api,
        size: const Size(624, 641),
        textScale: 2,
        updateCheck: check,
      );
      expect(
        tester.widget<IconButton>(update).tooltip,
        '1.0.60으로 업데이트',
      );
      expect(tester.takeException(), isNull);
      await tester.pumpWidget(const SizedBox.shrink());

      // An unregistered Host has no server to ask.
      asked.clear();
      await show(tester, FakeHostSettings(), updateCheck: check);
      expect(asked, isEmpty);
      expect(tester.widget<IconButton>(update).onPressed, isNull);
      await tester.pumpWidget(const SizedBox.shrink());
    },
  );
}
