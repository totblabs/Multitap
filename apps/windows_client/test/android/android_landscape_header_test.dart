// On a phone on its side the launcher's header, title and filters on rows of
// their own take about half the height, so the first card's 연결 is below
// the fold. In-process fixtures only: no broker, VM or device.
import 'package:flutter/material.dart';
import 'package:flutter/services.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:multitap_client/android/android_app.dart';
import 'package:multitap_client/data/broker_api.dart';

import 'android_launcher_test.dart'
    show signedIn, vm, mainButton, FakeBroker, MemoryStore;

class _IdleBroker extends FakeBroker {
  _IdleBroker(this.enabled) : super([vm(), vm(id: 'pve-150')]);
  final bool enabled;
  @override
  Future<BrokerSession> login(String name, String password) async {
    final now = DateTime.now().toUtc();
    return BrokerSession(
      token: 'synthetic-policy',
      expiresAt: now.add(const Duration(hours: 1)),
      username: name,
      sessionAccess: SessionAccess(
        idleTimeout: const Duration(minutes: 30),
        warning: const Duration(minutes: 1),
        serverTime: now,
        idleEnabled: enabled,
        idleDeadline: enabled ? now.add(const Duration(minutes: 30)) : null,
      ),
    );
  }
}

void main() {
  for (final enabled in [true, false]) {
    testWidgets(
      'actual idle policy $enabled fits small landscape at 200 percent with visible connect',
      (t) async {
        await (FontLoader('Pretendard')
              ..addFont(rootBundle.load('assets/fonts/Pretendard-Regular.otf'))
              ..addFont(
                rootBundle.load('assets/fonts/Pretendard-SemiBold.otf'),
              ))
            .load();
        final store = MemoryStore(
          origin: 'https://vdi.example',
          username: 'tester',
        );
        store.prefs['launcher.view'] = 'cards';
        store.install();
        t.view.physicalSize = const Size(1080, 2400);
        t.view.devicePixelRatio = 3;
        addTearDown(t.view.resetPhysicalSize);
        addTearDown(t.view.resetDevicePixelRatio);
        addTearDown(t.platformDispatcher.clearTextScaleFactorTestValue);
        await t.pumpWidget(
          AndroidVdiApp(apiFactory: (_) => _IdleBroker(enabled)),
        );
        await t.pumpAndSettle();
        await t.enterText(
          find.byKey(const Key('sign-in-password')),
          'synthetic',
        );
        await t.ensureVisible(find.byKey(const Key('login-submit')));
        await t.tap(find.byKey(const Key('login-submit')));
        await t.pumpAndSettle();
        t.view.physicalSize = const Size(800, 360);
        t.view.devicePixelRatio = 1;
        t.platformDispatcher.textScaleFactorTestValue = 2;
        await t.pumpAndSettle();
        expect(find.byKey(const Key('session-idle-badge')), findsOneWidget);
        expect(
          t.getRect(find.byKey(const Key('workspace-header'))).height,
          lessThanOrEqualTo(56),
        );
        // At 200% card text the existing card itself may need scrolling;
        // keep the connection action reachable without reducing its type size.
        await t.ensureVisible(mainButton('pve-149'));
        await t.pumpAndSettle();
        expect(
          t
              .getRect(mainButton('pve-149'))
              .overlaps(const Rect.fromLTWH(0, 0, 800, 360)),
          isTrue,
        );
        if (!enabled) {
          expect(
            find.descendant(
              of: find.byKey(const Key('session-idle-badge')),
              matching: find.text('꺼짐'),
            ),
            findsOneWidget,
          );
        }
        expect(t.takeException(), isNull);
        await t.pumpWidget(const SizedBox.shrink());
      },
    );
  }
  Future<void> launcher(WidgetTester t, Size size) async {
    await signedIn(t, [
      vm(),
      vm(id: 'pve-150', name: '개발용 VM', power: 'stopped', stream: false),
    ]);
    t.view.physicalSize = size;
    t.view.devicePixelRatio = 1;
    await t.pumpAndSettle();
  }

  final header = find.byKey(const Key('workspace-header'));
  Finder inHeader(Key key) =>
      find.descendant(of: header, matching: find.byKey(key));

  for (final size in const [Size(915, 412), Size(800, 360)]) {
    testWidgets(
      'Landscape ${size.width.toInt()}x${size.height.toInt()}: filters retain their row and the countdown fits above visible cards',
      (t) async {
        await launcher(t, size);
        final bar = t.getRect(header);
        expect(bar.height, lessThanOrEqualTo(56));
        for (final key in const [
          Key('desktop-status-row'),
          Key('desktop-previews'),
          Key('refresh-desktops'),
          Key('account-menu'),
        ]) {
          expect(inHeader(key), findsOneWidget, reason: '$key');
        }
        expect(find.byKey(const Key('workspace-title')), findsNothing);
        final card = t.getRect(find.byKey(const Key('card-pve-149')));
        expect(card.top, lessThanOrEqualTo(bar.bottom + 16));
        // The first card's 연결 is on the first screen: whole on a 412 dp
        // phone, its top at least on a 360 dp one.
        final connect = t.getRect(mainButton('pve-149'));
        if (size.height >= 400) {
          expect(connect.bottom, lessThanOrEqualTo(size.height));
        } else {
          expect(connect.top, lessThan(size.height - 24));
        }
        // Both cards keep one height.
        expect(
          t.getSize(find.byKey(const Key('card-pve-150'))).height,
          closeTo(card.height, .1),
        );
        // The commands still work from there.
        await t.tap(inHeader(const Key('refresh-desktops')));
        await t.pumpAndSettle();
        expect(t.takeException(), isNull);
      },
    );
  }

  testWidgets('Portrait keeps the title and filters under the header', (
    t,
  ) async {
    await launcher(t, const Size(412, 915));
    expect(t.getRect(header).height, 56);
    for (final key in [
      const Key('account-menu'),
      const Key('android-header-update-action'),
    ]) {
      final action = t.getSize(inHeader(key));
      expect(action.width, greaterThanOrEqualTo(48));
      expect(action.height, greaterThanOrEqualTo(48));
    }
    expect(inHeader(const Key('desktop-status-row')), findsNothing);
    expect(find.byKey(const Key('workspace-title')), findsOneWidget);
    expect(find.byKey(const Key('refresh-desktops')), findsOneWidget);
    expect(t.takeException(), isNull);
  });
}
