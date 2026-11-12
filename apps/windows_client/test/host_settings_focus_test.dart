import 'package:flutter/material.dart';
import 'package:flutter/services.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:multitap_client/ui/app_theme.dart';
import 'package:multitap_client/ui/host_settings_screen.dart';

import 'host_settings_test.dart'
    show FakeCloseGuard, FakeHostSettings, isReadOnlyHostRequest;

void main() {
  Future<void> open(
    WidgetTester tester,
    FakeHostSettings api,
    Size size,
    int page,
  ) async {
    tester.view.devicePixelRatio = 1;
    tester.view.physicalSize = size;
    addTearDown(tester.view.resetDevicePixelRatio);
    addTearDown(tester.view.resetPhysicalSize);
    await tester.pumpWidget(
      MaterialApp(
        theme: buildAppTheme(brightness: Brightness.dark)
            .copyWith(platform: TargetPlatform.windows),
        home: HostSettingsScreen(api: api, closeGuard: FakeCloseGuard()),
      ),
    );
    await tester.pumpAndSettle();
    final navigation = find.byKey(Key('host-nav-$page'));
    await tester.ensureVisible(navigation);
    await tester.tap(navigation);
    await tester.pumpAndSettle();
  }

  bool primaryFocusWithin(WidgetTester tester, String key) {
    final focused = FocusManager.instance.primaryFocus?.context;
    final target = tester.element(find.byKey(Key(key)));
    if (identical(focused, target)) return true;
    var found = false;
    focused?.visitAncestorElements((element) {
      found = identical(element, target);
      return !found;
    });
    return found;
  }

  Future<void> tab(
    WidgetTester tester,
    String expected, {
    bool reverse = false,
  }) async {
    if (reverse) await tester.sendKeyDownEvent(LogicalKeyboardKey.shiftLeft);
    await tester.sendKeyEvent(LogicalKeyboardKey.tab);
    if (reverse) await tester.sendKeyUpEvent(LogicalKeyboardKey.shiftLeft);
    await tester.pumpAndSettle();
    expect(
      primaryFocusWithin(tester, expected),
      isTrue,
      reason:
          '${reverse ? 'Shift+Tab' : 'Tab'} should reach $expected, '
          'not ${FocusManager.instance.primaryFocus?.debugLabel}',
    );
    expect(find.byKey(Key(expected)).hitTestable(), findsOneWidget);
  }

  for (final size in [const Size(1440, 1000), const Size(640, 480)]) {
    testWidgets(
      'Windows account Tab and Shift+Tab follow inputs after scrolling at $size',
      (tester) async {
        final api = FakeHostSettings();
        await open(tester, api, size, 5);
        final username = find.byKey(const Key('host-account-username'));
        await tester.ensureVisible(username);
        await tester.enterText(username, 'test-user');
        await tester.pumpAndSettle();

        await tab(tester, 'host-account-password');
        await tab(tester, 'host-account-password-confirmation');
        await tab(tester, 'host-save-account');
        await tab(tester, 'host-account-password-confirmation', reverse: true);
        await tab(tester, 'host-account-password', reverse: true);
        await tab(tester, 'host-account-username', reverse: true);

        expect(api.requests.every(isReadOnlyHostRequest), isTrue);
        expect(tester.takeException(), isNull);
        await tester.pumpWidget(const SizedBox.shrink());
      },
    );
  }

  testWidgets('Server address Tab reaches test then enrollment and reverses', (
    tester,
  ) async {
    final api = FakeHostSettings();
    await open(tester, api, const Size(640, 480), 1);
    final origin = find.byKey(const Key('host-server-origin'));
    await tester.ensureVisible(origin);
    await tester.enterText(origin, 'https://cloud.example.com');
    await tester.pumpAndSettle();
    await tab(tester, 'host-test-server');
    await tab(tester, 'host-server-origin', reverse: true);

    final test = find.byKey(const Key('host-test-server'));
    await tester.ensureVisible(test);
    await tester.tap(test);
    await tester.pumpAndSettle();
    await tester.ensureVisible(origin);
    await tester.tap(origin);
    await tester.pumpAndSettle();
    await tab(tester, 'host-test-server');
    await tab(tester, 'host-request-connection');
    await tab(tester, 'host-test-server', reverse: true);
    await tab(tester, 'host-server-origin', reverse: true);

    expect(
      api.requests.every(
        (request) =>
            isReadOnlyHostRequest(request) || request['action'] == 'testBroker',
      ),
      isTrue,
      reason: 'Tab navigation must never submit enrollment or change settings.',
    );
    expect(api.connectionRequests, 0);
    expect(tester.takeException(), isNull);
    await tester.pumpWidget(const SizedBox.shrink());
  });
}
