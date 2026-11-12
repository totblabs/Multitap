import '../helpers/stream_connect_consent.dart' show acceptStreamConnect;
import 'dart:async';

import 'package:flutter/material.dart';
import 'package:flutter/services.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:multitap_client/android/android_app.dart';
import 'package:multitap_client/android/android_application.dart';
import 'package:multitap_client/android/android_store.dart';
import 'package:multitap_client/android/android_session_keepalive.dart';
import 'package:multitap_client/android/android_tv.dart';
import 'package:multitap_client/data/broker_api.dart';
import 'package:multitap_client/data/product_log.dart';
import 'package:multitap_client/ui/app_theme.dart';
import 'package:multitap_client/ui/desktop_tile.dart';

import 'android_launcher_test.dart'
    show MemoryStore, FakeBroker, vm, mainButton;

Future<FakeBroker> boot(
  WidgetTester tester, {
  MemoryStore? memory,
  Size size = const Size(390, 844),
  int count = 4,
  bool television = false,
  FakeBroker? api,
}) async {
  tester.view.physicalSize = size;
  tester.view.devicePixelRatio = 1;
  addTearDown(tester.view.reset);
  (memory ?? MemoryStore(origin: 'https://vdi.example', username: 'tester'))
      .install();
  final broker =
      api ??
      FakeBroker([
        for (var i = 1; i <= count; i++)
          vm(id: 'pve-${100 + i}', name: '데스크톱 $i'),
      ]);
  await tester.pumpWidget(
    AndroidVdiApp(
      apiFactory: (_) => broker,
      television: television,
      hardwareKeyboard: television ? true : null,
    ),
  );
  await tester.pumpAndSettle();
  await tester.enterText(
    find.byKey(const Key('sign-in-password')),
    'fixture-password',
  );
  await tap(tester, 'login-submit');
  return broker;
}

/// Holds list reads at [gate] while it is set.
class _SlowBroker extends FakeBroker {
  _SlowBroker(super.inventory);
  Completer<void>? gate;
  int reads = 0;
  @override
  Future<List<BrokerDesktop>> desktops(String token) async {
    reads++;
    await gate?.future;
    return super.desktops(token);
  }
}

/// The server never answers a sign-out.
class _SilentLogoutBroker extends FakeBroker {
  _SilentLogoutBroker(super.inventory);
  @override
  Future<void> logout(String token) {
    calls.add('logout');
    return Completer<void>().future;
  }
}

/// A sign-in that is answered when [gate] opens.
class _GatedLoginBroker extends FakeBroker {
  _GatedLoginBroker(super.inventory, this.gate);
  final Completer<void> gate;
  @override
  Future<BrokerSession> login(String username, String password) async {
    await gate.future;
    return super.login(username, password);
  }
}

Future<void> tap(WidgetTester tester, String key) async {
  final target = find.byKey(Key(key));
  await tester.ensureVisible(target);
  await tester.pumpAndSettle();
  await tester.tap(target);
  await tester.pumpAndSettle();
}

Future<void> disposeApp(WidgetTester tester) async {
  await tester.pumpWidget(const SizedBox.shrink());
  await tester.pumpAndSettle();
}

void main() {
  setUp(() {
    final messenger =
        TestDefaultBinaryMessengerBinding.instance.defaultBinaryMessenger;
    messenger.setMockMethodCallHandler(
      const MethodChannel('multitap/pen-input'),
      (_) async => null,
    );
    messenger.setMockMethodCallHandler(
      AndroidSessionKeepalive.channel,
      (call) async => call.method == 'start' ? true : null,
    );
  });
  tearDown(() {
    final messenger =
        TestDefaultBinaryMessengerBinding.instance.defaultBinaryMessenger;
    messenger.setMockMethodCallHandler(
      const MethodChannel('multitap/pen-input'),
      null,
    );
    messenger.setMockMethodCallHandler(AndroidSessionKeepalive.channel, null);
  });
  TestWidgetsFlutterBinding.ensureInitialized();
  tearDown(() {
    final messenger =
        TestDefaultBinaryMessengerBinding.instance.defaultBinaryMessenger;
    messenger.setMockMethodCallHandler(AndroidStore.channel, null);
    messenger.setMockMethodCallHandler(AndroidApplication.channel, null);
  });

  testWidgets(
    'fresh default is fixed cards with sort and refresh but no layout or size chooser',
    (tester) async {
      await boot(tester);
      expect(find.byKey(const Key('cards-view')), findsOneWidget);
      final sort = tester.getRect(find.byKey(const Key('desktop-sort')));
      final refresh = tester.getRect(find.byKey(const Key('refresh-desktops')));
      expect(refresh.bottom, lessThan(sort.top));
      expect(find.byKey(const Key('desktop-previews')), findsOneWidget);
      expect(find.textContaining('카드 크기'), findsNothing);
      await disposeApp(tester);
    },
  );

  testWidgets('every card owns its action and body taps never connect', (
    tester,
  ) async {
    final broker = await boot(tester);
    expect(mainButton('pve-101'), findsOneWidget);
    expect(mainButton('pve-102'), findsOneWidget);
    await tap(tester, 'desktop-name-pve-102');
    expect(mainButton('pve-102'), findsOneWidget);
    expect(mainButton('pve-101'), findsOneWidget);
    expect(
      broker.calls.where(
        (call) => call == 'connect' || call.startsWith('power:'),
      ),
      isEmpty,
    );
    await tester.ensureVisible(mainButton('pve-102'));
    await tester.pumpAndSettle();
    await tester.tap(mainButton('pve-102'));
    await acceptStreamConnect(tester, 'pve-102');
    await tester.pumpAndSettle();
    expect(find.byKey(const Key('connect-confirm-dialog')), findsNothing);
    expect(find.byKey(const Key('android-session-panel')), findsOneWidget);
    expect(find.text('데스크톱 2'), findsOneWidget);
    expect(broker.calls.where((c) => c.startsWith('power:')), isEmpty);
    await disposeApp(tester);
  });

  testWidgets(
    'as on Windows, ⋯ holds the status check and ▾ the quality options',
    (tester) async {
      await boot(tester, count: 1);
      await tap(tester, 'desktop-more-pve-101');
      expect(find.byKey(const Key('details-pve-101')), findsOneWidget);
      expect(find.byKey(const Key('stream-settings-pve-101')), findsNothing);
      await tap(tester, 'details-pve-101');
      expect(find.byKey(const Key('readiness-verdict')), findsOneWidget);
      await tap(tester, 'close-readiness');
      await tap(tester, 'stream-modes-pve-101');
      await tap(tester, 'stream-settings-pve-101');
      expect(find.text('스트리밍 설정'), findsOneWidget);
      await disposeApp(tester);
    },
  );

  testWidgets(
    'preview-only options preserve visibility across a restart without choosing a layout',
    (tester) async {
      final memory = MemoryStore(
        origin: 'https://vdi.example',
        username: 'tester',
        prefs: {'launcher.view': 'docking'},
      );
      await boot(tester, memory: memory, count: 1);
      for (final size in ['small', 'medium', 'large']) {
        expect(find.byKey(Key('android-card-size-$size')), findsNothing);
      }
      // One press of the toggle, as on Windows (no menu in between).
      await tap(tester, 'desktop-previews');
      expect(memory.prefs['launcher.previews'], 'off');
      expect(
        tester.widget<DesktopTile>(find.byType(DesktopTile)).showPreview,
        isFalse,
      );
      expect(find.byKey(const Key('desktop-previews')), findsOneWidget);
      await disposeApp(tester);
      await boot(tester, memory: memory, count: 1);
      expect(find.byKey(const Key('cards-view')), findsOneWidget);
      expect(
        tester.widget<DesktopTile>(find.byType(DesktopTile)).showPreview,
        isFalse,
      );
      expect(memory.prefs['launcher.previews'], 'off');
      await disposeApp(tester);
    },
  );

  testWidgets('미리보기 is a toggle that shows whether it is on, as on Windows', (
    tester,
  ) async {
    await boot(tester, size: const Size(1280, 800), count: 1);
    final button = find.byKey(const Key('desktop-previews'));
    final p = AppPalette.of(tester.element(button));
    Material material() => tester.widget<Material>(
      find.descendant(of: button, matching: find.byType(Material)).first,
    );
    bool? toggled() => tester
        .widget<Semantics>(
          find.descendant(
            of: button,
            matching: find.byWidgetPredicate(
              (w) => w is Semantics && w.properties.button == true,
            ),
          ),
        )
        .properties
        .toggled;
    String? tip() => tester
        .widget<Tooltip>(
          find.descendant(of: button, matching: find.byType(Tooltip)),
        )
        .message;
    expect(
      find.descendant(of: button, matching: find.text('미리보기')),
      findsOneWidget,
    );
    expect(material().color, p.amberSoft);
    expect(toggled(), isTrue);
    expect(tip(), '화면 미리보기 켜짐 · 누르면 꺼요');
    await tap(tester, 'desktop-previews');
    expect(material().color, p.pillGlass);
    expect(toggled(), isFalse);
    expect(tip(), '화면 미리보기 꺼짐 · 누르면 켜요');
    await disposeApp(tester);
  });

  testWidgets('새로고침 keeps its shape and label while the list is read', (
    tester,
  ) async {
    final broker = _SlowBroker([vm()]);
    await boot(tester, size: const Size(1280, 800), api: broker);
    final refresh = find.byKey(const Key('refresh-desktops'));
    final idle = tester.getSize(refresh);
    expect(find.byKey(const Key('header-button-busy')), findsNothing);
    broker.gate = Completer<void>();
    await tester.tap(refresh);
    await tester.pump();
    await tester.pump(const Duration(milliseconds: 100));
    final reads = broker.reads;
    expect(tester.getSize(refresh), idle);
    expect(
      find.descendant(of: refresh, matching: find.text('새로고침')),
      findsOneWidget,
    );
    expect(
      find.descendant(
        of: refresh,
        matching: find.byKey(const Key('header-button-busy')),
      ),
      findsOneWidget,
    );
    expect(find.text('확인 중'), findsNothing);
    // Not faded out, and a second press waits for the read in flight.
    expect(
      tester
          .widget<Material>(
            find.descendant(of: refresh, matching: find.byType(Material)).first,
          )
          .color,
      AppPalette.of(tester.element(refresh)).pillGlass,
    );
    await tester.tap(refresh);
    await tester.pump();
    expect(broker.reads, reads);
    broker.gate!.complete();
    broker.gate = null;
    await tester.pumpAndSettle();
    expect(find.byKey(const Key('header-button-busy')), findsNothing);
    await disposeApp(tester);
  });

  for (final size in [const Size(390, 844), const Size(1280, 800)]) {
    testWidgets(
      'fixed cards keep equal geometry, footer alignment and independent actions at $size',
      (tester) async {
        final broker = await boot(tester, size: size);
        expect(find.byKey(const Key('desktop-dock')), findsNothing);
        final cards = [
          for (var id = 101; id <= 104; id++)
            tester.getRect(find.byKey(Key('card-pve-$id'))),
        ];
        final actions = [
          for (var id = 101; id <= 104; id++)
            tester.getRect(mainButton('pve-$id')),
        ];
        for (var id = 101; id <= 104; id++) {
          expect(mainButton('pve-$id'), findsOneWidget);
        }
        for (var i = 1; i < cards.length; i++) {
          expect(cards[i].width, closeTo(cards.first.width, .1));
          expect(cards[i].height, closeTo(cards.first.height, .1));
          expect(
            actions[i].top - cards[i].top,
            closeTo(actions.first.top - cards.first.top, .1),
          );
        }
        expect(
          broker.calls.where((c) => c == 'connect' || c.startsWith('power:')),
          isEmpty,
        );
        expect(tester.takeException(), isNull);
        await disposeApp(tester);
      },
    );
  }

  testWidgets('full exit confirms once and never sends a VM power request', (
    tester,
  ) async {
    final calls = <String>[];
    FakeBroker? api;
    bool? signedOutBeforeClose;
    TestDefaultBinaryMessengerBinding.instance.defaultBinaryMessenger
        .setMockMethodCallHandler(AndroidApplication.channel, (call) async {
          calls.add(call.method);
          if (call.method == 'close') {
            signedOutBeforeClose = api?.calls.contains('logout');
          }
          return null;
        });
    final broker = api = await boot(tester, count: 1);
    calls.clear(); // Authentication binding is not an application-exit action.
    await tap(tester, 'account-menu');
    await tap(tester, 'android-exit-menu');
    await tap(tester, 'cancel-android-exit');
    expect(calls, isEmpty);
    expect(broker.calls, isNot(contains('logout')));
    await tap(tester, 'account-menu');
    await tap(tester, 'android-exit-menu');
    await tap(tester, 'confirm-android-exit');
    expect(calls, ['close']);
    // 완전 종료 ends the sign-in at the server first: left alive, the next
    // sign-in on the same phone would ask to sign out "elsewhere".
    expect(broker.calls.where((call) => call == 'logout'), hasLength(1));
    expect(signedOutBeforeClose, isTrue);
    expect(broker.calls.where((call) => call.startsWith('power:')), isEmpty);
    await disposeApp(tester);
  });

  testWidgets('full exit does not wait for a server that never answers the '
      'sign-out', (tester) async {
    final calls = <String>[];
    TestDefaultBinaryMessengerBinding.instance.defaultBinaryMessenger
        .setMockMethodCallHandler(AndroidApplication.channel, (call) async {
          calls.add(call.method);
          return null;
        });
    final broker = _SilentLogoutBroker([vm(id: 'pve-101', name: '데스크톱 1')]);
    await boot(tester, api: broker);
    calls.clear();
    await tap(tester, 'account-menu');
    await tap(tester, 'android-exit-menu');
    await tester.tap(find.byKey(const Key('confirm-android-exit')));
    await tester.pump();
    await tester.pump(const Duration(milliseconds: 2500));
    expect(broker.calls, contains('logout'));
    expect(calls, isEmpty, reason: 'the sign-out is given 3 s');
    // Waiting for a sign-out, the sign-in button does not say "로그인 중…".
    expect(find.byKey(const Key('login-submit')), findsOneWidget);
    expect(find.text('로그인 중…'), findsNothing);
    await tester.pump(const Duration(milliseconds: 600));
    await tester.pump();
    expect(calls, ['close']);
    await disposeApp(tester);
  });

  // The sign-in button must not show "로그인 중…" until the sign-out is
  // answered.
  testWidgets('로그아웃 while the server has not answered: the sign-in button '
      'waits without saying it is signing in', (tester) async {
    final broker = _SilentLogoutBroker([vm(id: 'pve-101', name: '데스크톱 1')]);
    await boot(tester, api: broker);
    await tap(tester, 'account-menu');
    await tap(tester, 'sign-out');
    await tester.tap(find.byKey(const Key('confirm-sign-out')));
    await tester.pump();
    await tester.pump(const Duration(milliseconds: 500));
    expect(broker.calls, contains('logout'));
    final button = tester.widget<FilledButton>(
      find.byKey(const Key('login-submit')),
    );
    expect(button.onPressed, isNull, reason: 'the sign-out is still sent');
    expect(find.text('로그인 중…'), findsNothing);
    expect(
      find.descendant(
        of: find.byKey(const Key('login-submit')),
        matching: find.byType(CircularProgressIndicator),
      ),
      findsNothing,
    );
    expect(
      find.descendant(
        of: find.byKey(const Key('login-submit')),
        matching: find.text('로그인'),
      ),
      findsOneWidget,
    );
    await disposeApp(tester);
  });

  testWidgets('a sign-in still says 로그인 중… while it is sent', (tester) async {
    tester.view.physicalSize = const Size(390, 844);
    tester.view.devicePixelRatio = 1;
    addTearDown(tester.view.reset);
    MemoryStore(origin: 'https://vdi.example', username: 'tester').install();
    final gate = Completer<void>();
    final broker = _GatedLoginBroker([], gate);
    await tester.pumpWidget(AndroidVdiApp(apiFactory: (_) => broker));
    await tester.pumpAndSettle();
    await tester.enterText(
      find.byKey(const Key('sign-in-password')),
      'fixture-password',
    );
    await tester.ensureVisible(find.byKey(const Key('login-submit')));
    await tester.pumpAndSettle();
    await tester.tap(find.byKey(const Key('login-submit')));
    await tester.pump();
    await tester.pump(const Duration(milliseconds: 100));
    expect(find.text('로그인 중…'), findsOneWidget);
    gate.complete();
    await tester.pumpAndSettle();
    expect(find.text('로그인 중…'), findsNothing);
    await disposeApp(tester);
  });

  testWidgets(
    'TV D-pad focus follows card buttons and OK opens only the focused desktop',
    (tester) async {
      final broker = await boot(
        tester,
        size: const Size(960, 540),
        count: 2,
        television: true,
      );
      final first = mainButton('pve-101');
      final second = mainButton('pve-102');
      FocusNode node(Finder target) {
        final element = target.evaluate().single;
        return FocusManager.instance.rootScope.descendants.firstWhere((focus) {
          if (focus is FocusScopeNode ||
              !focus.canRequestFocus ||
              focus.skipTraversal ||
              focus.context == null) {
            return false;
          }
          var inside = false;
          (focus.context! as Element).visitAncestorElements((ancestor) {
            if (ancestor == element) {
              inside = true;
              return false;
            }
            return true;
          });
          return inside;
        });
      }

      await tester.ensureVisible(first);
      node(first).requestFocus();
      await tester.pumpAndSettle();
      // Right goes through this tile's ▾ (the Windows footer) to the next
      // tile's main button.
      await tester.sendKeyEvent(LogicalKeyboardKey.arrowRight);
      await tester.pumpAndSettle();
      expect(
        node(find.byKey(const Key('stream-modes-pve-101'))).hasPrimaryFocus,
        isTrue,
      );
      await tester.sendKeyEvent(LogicalKeyboardKey.arrowRight);
      await tester.pumpAndSettle();
      expect(node(second).hasPrimaryFocus, isTrue);
      final button = tester.widget<FilledButton>(second);
      expect(
        button.style!.backgroundColor!.resolve({WidgetState.focused}),
        AndroidTvFocus.surface(tester.element(second)),
      );
      await tester.sendKeyEvent(LogicalKeyboardKey.enter);
      await acceptStreamConnect(tester, 'pve-102');
      await tester.pumpAndSettle();
      expect(find.byKey(const Key('connect-confirm-dialog')), findsNothing);
      expect(find.byKey(const Key('android-session-panel')), findsOneWidget);
      expect(find.text('데스크톱 2'), findsOneWidget);
      expect(broker.calls.where((c) => c.startsWith('power:')), isEmpty);
      await disposeApp(tester);
    },
  );

  testWidgets(
    'account log is usable with enlarged text and excludes arbitrary fields',
    (tester) async {
      await boot(tester, count: 1);
      ProductLog.instance.record(LogEvent.inventory, {
        'ok': true,
        'available': 1,
        'password': 'never-show-this',
      });
      tester.platformDispatcher.textScaleFactorTestValue = 1.5;
      addTearDown(tester.platformDispatcher.clearTextScaleFactorTestValue);
      await tester.pumpAndSettle();
      await tap(tester, 'account-menu');
      await tap(tester, 'android-log-menu');
      expect(find.text('로그 보기'), findsOneWidget);
      expect(find.textContaining('never-show-this'), findsNothing);
      expect(tester.takeException(), isNull);
      await disposeApp(tester);
    },
  );
}
