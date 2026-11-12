import 'dart:async';

import 'package:flutter/material.dart';
import 'package:flutter/services.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:multitap_client/app.dart';
import 'package:multitap_client/data/appearance_settings.dart';
import 'package:multitap_client/data/client_tray.dart';
import 'package:multitap_client/data/launcher_memory.dart';
import 'package:multitap_client/data/window_close_guard.dart';
import 'package:multitap_client/ui/themed_app.dart';
import 'package:multitap_client/ui/workspace_header.dart';

import 'auth_flow_test.dart' show TestLoginMemory, fixtureOrigin;
import 'session_renewal_test.dart' show RenewingBroker, VirtualClock;

class _Guard implements WindowCloseGuard {
  Future<void> Function()? onRequest;
  int closes = 0;

  @override
  void attach(Future<void> Function() callback) => onRequest = callback;

  @override
  Future<bool> update({required bool busy, required bool dirty}) async => true;

  @override
  Future<bool> close() async {
    closes++;
    return true;
  }

  @override
  void dispose() => onRequest = null;
}

class _Tray implements ClientTray {
  Future<void> Function()? onHidden;
  final balloons = <String>[];
  bool sessions = false;
  bool disposed = false;

  @override
  void attach(Future<void> Function() callback) => onHidden = callback;

  @override
  Future<bool> balloon(String text) async {
    balloons.add(text);
    return true;
  }

  @override
  Future<bool> hasRemoteSessions() async => sessions;

  @override
  void dispose() => disposed = true;
}

/// A memory whose write fails: the notice must then never show.
class _ForgetfulMemory extends SessionLauncherMemory {
  @override
  Future<void> saveTrayNoticeShown() async => throw const FileSystemException();
}

class FileSystemException implements Exception {
  const FileSystemException();
}

Future<void> _pumpHeader(
  WidgetTester t, {
  required _Guard guard,
  required _Tray tray,
  LauncherMemory? memory,
  bool confirmExit = true,
  VoidCallback? onSignOut,
}) async {
  t.view.physicalSize = const Size(1000, 700);
  t.view.devicePixelRatio = 1;
  addTearDown(t.view.resetPhysicalSize);
  addTearDown(t.view.resetDevicePixelRatio);
  final appearance = AppAppearance();
  addTearDown(appearance.dispose);
  await t.pumpWidget(
    ThemedApp(
      appearance: appearance,
      confirmExit: confirmExit,
      exitGuard: guard,
      tray: tray,
      trayMemory: memory ?? SessionLauncherMemory(),
      home: Scaffold(
        body: Align(
          alignment: Alignment.topCenter,
          child: WorkspaceHeader(
            side: 24,
            columnWidth: 900,
            isDemo: false,
            accountLabel: 'alice',
            server: 'vdi.test',
            version: 'v1.0.0',
            update: null,
            onUpdates: null,
            onClientInfo: () {},
            onLog: () {},
            onChangePassword: null,
            onSignOut: onSignOut ?? () {},
          ),
        ),
      ),
    ),
  );
  await t.pumpAndSettle();
}

Future<void> _openMenu(WidgetTester t) async {
  await t.tap(find.byKey(const Key('workspace-account')));
  await t.pumpAndSettle();
}

void main() {
  group('완전 종료 in the account menu', () {
    testWidgets('is the last item, below 로그아웃, with the exit icon', (t) async {
      await _pumpHeader(t, guard: _Guard(), tray: _Tray());
      await _openMenu(t);
      final signOut = find.byKey(const Key('sign-out'));
      final quit = find.byKey(const Key('quit-app'));
      expect(quit, findsOneWidget);
      expect(find.descendant(of: quit, matching: find.text('완전 종료')), findsOneWidget);
      expect(
        find.descendant(
          of: quit,
          matching: find.byIcon(Icons.power_settings_new_rounded),
        ),
        findsOneWidget,
      );
      expect(
        t.getTopLeft(quit).dy,
        greaterThan(t.getBottomLeft(signOut).dy - 1),
      );
      // Nothing follows it: it is the last row of the menu.
      final rows = find.byType(MenuItemButton);
      expect(t.getTopLeft(rows.last).dy, t.getTopLeft(quit).dy);
    });

    testWidgets('exits at once, without a dialog, when no session is open', (
      t,
    ) async {
      final guard = _Guard(), tray = _Tray();
      var signedOut = 0;
      await _pumpHeader(t, guard: guard, tray: tray, onSignOut: () => signedOut++);
      await _openMenu(t);
      await t.tap(find.byKey(const Key('quit-app')));
      await t.pumpAndSettle();
      expect(find.byKey(const Key('exit-confirm-dialog')), findsNothing);
      expect(guard.closes, 1);
      expect(signedOut, 0);
    });

    testWidgets('asks first when remote connections would end; 취소 leaves '
        'everything as it was', (t) async {
      final guard = _Guard(), tray = _Tray()..sessions = true;
      await _pumpHeader(t, guard: guard, tray: tray);
      await _openMenu(t);
      await t.tap(find.byKey(const Key('quit-app')));
      await t.pumpAndSettle();
      expect(find.byKey(const Key('exit-confirm-dialog')), findsOneWidget);
      expect(find.text('Multitap을 종료할까요?'), findsOneWidget);
      expect(find.text('열려 있는 연결도 모두 끊겨요. 데스크톱은 계속 켜져 있어요.'), findsOneWidget);
      await t.tap(find.byKey(const Key('exit-cancel')));
      await t.pumpAndSettle();
      expect(guard.closes, 0);
      expect(find.byKey(const Key('exit-confirm-dialog')), findsNothing);
      // The window and its header are still there, and the tray was not touched.
      expect(find.byKey(const Key('workspace-account')), findsOneWidget);
      expect(tray.balloons, isEmpty);
      // Asking again and confirming exits.
      await _openMenu(t);
      await t.tap(find.byKey(const Key('quit-app')));
      await t.pumpAndSettle();
      await t.tap(find.byKey(const Key('exit-confirm')));
      await t.pumpAndSettle();
      expect(guard.closes, 1);
    });

    testWidgets('is not offered outside the main client window', (t) async {
      await _pumpHeader(
        t,
        guard: _Guard(),
        tray: _Tray(),
        confirmExit: false,
      );
      await _openMenu(t);
      expect(find.byKey(const Key('sign-out')), findsOneWidget);
      expect(find.byKey(const Key('quit-app')), findsNothing);
    });

    testWidgets('로그아웃 stays a separate, unchanged action', (t) async {
      final guard = _Guard();
      var signedOut = 0;
      await _pumpHeader(
        t,
        guard: guard,
        tray: _Tray(),
        onSignOut: () => signedOut++,
      );
      await _openMenu(t);
      await t.tap(find.byKey(const Key('sign-out')));
      await t.pumpAndSettle();
      expect(signedOut, 1);
      expect(guard.closes, 0);
    });
  });

  group('the one-time tray notice', () {
    testWidgets('shows once when the window first rests in the tray, never again', (
      t,
    ) async {
      final tray = _Tray();
      final memory = SessionLauncherMemory();
      await _pumpHeader(t, guard: _Guard(), tray: tray, memory: memory);
      expect(tray.onHidden, isNotNull);
      await tray.onHidden!();
      expect(tray.balloons, [TrayNotices.runsInTray]);
      expect(
        TrayNotices.runsInTray,
        "Multitap은 트레이에서 계속 실행 중이에요. 완전히 끝내려면 트레이 아이콘이나 계정 메뉴의 '완전 종료'를 누르세요.",
      );
      await tray.onHidden!();
      await tray.onHidden!();
      expect(tray.balloons, hasLength(1));
      // A later run of the client (same store): still never again.
      final next = _Tray();
      await t.pumpWidget(const SizedBox());
      await _pumpHeader(t, guard: _Guard(), tray: next, memory: memory);
      await next.onHidden!();
      expect(next.balloons, isEmpty);
      expect(await memory.loadTrayNoticeShown(), isTrue);
    });

    testWidgets('two hides at once give one balloon', (t) async {
      final tray = _Tray();
      await _pumpHeader(t, guard: _Guard(), tray: tray);
      await Future.wait([tray.onHidden!(), tray.onHidden!()]);
      expect(tray.balloons, hasLength(1));
    });

    testWidgets('a notice that cannot be remembered is not shown', (t) async {
      final tray = _Tray();
      await _pumpHeader(
        t,
        guard: _Guard(),
        tray: tray,
        memory: _ForgetfulMemory(),
      );
      await tray.onHidden!();
      await tray.onHidden!();
      expect(tray.balloons, isEmpty);
    });
  });

  group('through the native window channels', () {
    final calls = <String, List<MethodCall>>{};
    late bool sessionsOpen;

    setUp(() {
      calls.clear();
      sessionsOpen = false;
      final messenger =
          TestDefaultBinaryMessengerBinding.instance.defaultBinaryMessenger;
      for (final name in ['multitap/tray', 'multitap/window/close', 'multitap/window']) {
        messenger.setMockMethodCallHandler(MethodChannel(name), (call) async {
          calls.putIfAbsent(name, () => []).add(call);
          if (name == 'multitap/window' && call.method == 'hasSessionWindows') {
            return sessionsOpen;
          }
          if (call.method == 'balloon' || call.method == 'close') return true;
          if (call.method == 'update') return true;
          return null;
        });
      }
      addTearDown(() {
        for (final name in ['multitap/tray', 'multitap/window/close', 'multitap/window']) {
          messenger.setMockMethodCallHandler(MethodChannel(name), null);
        }
      });
    });

    Future<void> nativeSays(String channel, String method, [Object? args]) {
      final completer = Completer<void>();
      TestDefaultBinaryMessengerBinding.instance.defaultBinaryMessenger
          .handlePlatformMessage(
            channel,
            const StandardMethodCodec().encodeMethodCall(MethodCall(method, args)),
            (_) => completer.complete(),
          );
      return completer.future;
    }

    Future<void> pumpClient(WidgetTester t) async {
      final appearance = AppAppearance();
      addTearDown(appearance.dispose);
      await t.pumpWidget(
        ThemedApp(
          appearance: appearance,
          confirmExit: true,
          trayMemory: SessionLauncherMemory(),
          home: Scaffold(
            body: WorkspaceHeader(
              side: 24,
              columnWidth: 900,
              isDemo: false,
              accountLabel: 'alice',
              server: 'vdi.test',
              version: 'v1',
              update: null,
              onUpdates: null,
              onClientInfo: () {},
              onLog: () {},
              onChangePassword: null,
              onSignOut: () {},
            ),
          ),
        ),
      );
      await t.pumpAndSettle();
    }

    testWidgets('"hidden" from the window raises the balloon through the tray '
        'channel, once', (t) async {
      await pumpClient(t);
      await t.runAsync(() => nativeSays('multitap/tray', 'hidden'));
      await t.pump();
      final balloons =
          calls['multitap/tray']!.where((call) => call.method == 'balloon').toList();
      expect(balloons, hasLength(1));
      expect(balloons.single.arguments, {
        'title': 'Multitap',
        'text': TrayNotices.runsInTray,
      });
      await t.runAsync(() => nativeSays('multitap/tray', 'hidden'));
      await t.pump();
      expect(
        calls['multitap/tray']!.where((call) => call.method == 'balloon'),
        hasLength(1),
      );
    });

    testWidgets('the native quit request with open sessions asks, 취소 keeps '
        'the client, 종료 closes through the close channel', (t) async {
      await pumpClient(t);
      // The tray menu's 완전 종료 with sessions open: native shows the window
      // and asks the app (sessions: true).
      late Future<void> asked;
      await t.runAsync(() async {
        asked = nativeSays('multitap/window/close', 'requested', {'sessions': true});
      });
      await t.pumpAndSettle();
      expect(find.byKey(const Key('exit-confirm-dialog')), findsOneWidget);
      expect(find.text('열려 있는 연결도 모두 끊겨요. 데스크톱은 계속 켜져 있어요.'), findsOneWidget);
      await t.tap(find.byKey(const Key('exit-cancel')));
      await t.pumpAndSettle();
      await t.runAsync(() => asked);
      expect(
        calls['multitap/window/close']!.where((call) => call.method == 'close'),
        isEmpty,
      );
      late Future<void> again;
      await t.runAsync(() async {
        again = nativeSays('multitap/window/close', 'requested', {'sessions': true});
      });
      await t.pumpAndSettle();
      await t.tap(find.byKey(const Key('exit-confirm')));
      await t.pumpAndSettle();
      await t.runAsync(() => again);
      expect(
        calls['multitap/window/close']!.where((call) => call.method == 'close'),
        hasLength(1),
      );
    });

    testWidgets('the account menu asks the window whether sessions are open', (
      t,
    ) async {
      await pumpClient(t);
      sessionsOpen = false;
      await _openMenu(t);
      await t.tap(find.byKey(const Key('quit-app')));
      await t.pumpAndSettle();
      expect(find.byKey(const Key('exit-confirm-dialog')), findsNothing);
      expect(
        calls['multitap/window']!.where((c) => c.method == 'hasSessionWindows'),
        hasLength(1),
      );
      expect(
        calls['multitap/window/close']!.where((c) => c.method == 'close'),
        hasLength(1),
      );
      sessionsOpen = true;
      await _openMenu(t);
      await t.tap(find.byKey(const Key('quit-app')));
      await t.pumpAndSettle();
      expect(find.byKey(const Key('exit-confirm-dialog')), findsOneWidget);
      // Still one close: the second waits for the answer.
      expect(
        calls['multitap/window/close']!.where((c) => c.method == 'close'),
        hasLength(1),
      );
    });
  });

  group('sign-in lapse while the window rests in the tray', () {
    testWidgets('raises the lapse balloon, once, and the sign-in screen waits '
        'for the click', (t) async {
      final clock = VirtualClock();
      final tray = _Tray();
      final api = RenewingBroker(clock)..renewSupported = false;
      t.view.physicalSize = const Size(1440, 900);
      t.view.devicePixelRatio = 1;
      addTearDown(t.view.resetPhysicalSize);
      addTearDown(t.view.resetDevicePixelRatio);
      await t.pumpWidget(
        MultitapApp(
          api: api,
          serverOrigin: fixtureOrigin,
          loginMemory: TestLoginMemory(),
          sessionRetryDelay: const Duration(seconds: 1),
          clock: clock.call,
          confirmExit: true,
          tray: tray,
        ),
      );
      await t.pumpAndSettle();
      await t.enterText(find.byKey(const Key('sign-in-id')), 'alice');
      await t.enterText(find.byKey(const Key('sign-in-password')), 'secret-pass');
      await t.tap(find.byKey(const Key('login-submit')));
      await t.pumpAndSettle();
      expect(find.byKey(const Key('workspace-account')), findsOneWidget);
      // Still signed in: no balloon, and an update being available would not
      // raise one either (nothing here asks for attention).
      await clock.advance(t, const Duration(minutes: 6));
      expect(tray.balloons, isEmpty);
      // The real expiry with no remembered password: sign-in is needed again.
      await clock.advance(t, const Duration(minutes: 5));
      await t.pumpAndSettle();
      expect(find.byKey(const Key('workspace-account')), findsNothing);
        expect(tray.balloons, ['Multitap에서 로그아웃됐어요. 클릭해서 이유를 확인하세요.']);
        expect(find.byKey(const ValueKey('session-end-ack')), findsOneWidget);
    });
  });
}
