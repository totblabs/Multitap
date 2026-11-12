import 'dart:async';

import 'package:flutter/material.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:multitap_client/data/host_settings_api.dart';
import 'package:multitap_client/ui/app_theme.dart';
import 'package:multitap_client/ui/host_settings_navigation.dart';
import 'package:multitap_client/ui/host_settings_screen.dart';

import 'host_settings_test.dart' as fixture;

const fixtureSid = 'S-1-5-21-1000-2000-3000-1001';

Map<String, dynamic> loginState({
  String state = 'off',
  bool admin = true,
  bool supported = true,
  String? operationId,
  String? operationState,
}) => {
  'schema': 1,
  'supported': supported,
  'administrator': admin,
  'state': state,
  'reason': state == 'off' || state == 'configured'
      ? 'none'
      : 'state_unavailable',
  'revision': List.filled(64, 'a').join(),
  'accountLabel': state == 'off' ? '' : 'fixture-user',
  'loggedIn': false,
  'observedAt': null,
  'operation': operationId == null
      ? null
      : {'id': operationId, 'state': operationState ?? 'configured'},
};

class _Host implements HostSettingsApi {
  final base = fixture.FakeHostSettings(registered: true);
  final calls = <String>[];
  Map<String, dynamic>? login = loginState();
  bool rejectFreshAdmin = false, loseResponse = false;
  String? followupState, lastId;
  String receiptState = 'configured';
  String? rejectedReceipt;
  Completer<Map<String, dynamic>>? held;
  int applies = 0;
  // windowsAccountPasswordState / setWindowsAccountPassword (fixed codes only).
  Map<String, dynamic> passwordState = {
    'error': '',
    'state': 'set',
    'sid': fixtureSid,
    'minimumLength': 0,
  };
  String passwordResult = '';
  bool losePasswordResponse = false;
  final passwordSets = <Map<String, dynamic>>[];
  @override
  Future<Map<String, dynamic>> request(
    String action, [
    Map<String, dynamic> fields = const {},
  ]) async {
    calls.add(action);
    if (action == 'windowsAccountPasswordState') {
      expect(fields.keys.toSet(), {'username'});
      return {'ok': true, 'account': passwordState};
    }
    if (action == 'setWindowsAccountPassword') {
      passwordSets.add(Map.of(fields));
      if (losePasswordResponse) {
        throw const HostSettingsException('synthetic observer deadline');
      }
      return {
        'ok': true,
        'password': {'error': passwordResult},
      };
    }
    if (action == 'windowsAutologonStatus') {
      return {
        'ok': true,
        'autologon': rejectFreshAdmin
            ? loginState(admin: false)
            : {
                ...?login,
                if (fields['operationId'] != null)
                  'operation': {
                    'id': fields['operationId'],
                    'state': receiptState,
                  },
              },
      };
    }
    if (action == 'windowsAutologonApply') {
      applies++;
      lastId = fields['operationId'] as String;
      expect(fields.keys.toSet(), {
        'username',
        'password',
        'passwordConfirmation',
        'operationId',
        'expectedRevision',
        'confirmed',
      });
      expect(fields['confirmed'], isTrue);
      expect(fields['password'], 'synthetic-password');
      // Saving the account comes first, with the same account (2026-10-29).
      expect(
        base.requests
            .where((v) => v['action'] == 'saveRdpCredentials')
            .last['username'],
        fields['username'],
      );
      login = loginState(
        state: followupState ?? 'configured',
        operationId: lastId,
        operationState: receiptState,
      );
      if (rejectedReceipt != null) {
        final responseStatus = loginState(state: 'unknown');
        if (rejectedReceipt != 'null') {
          responseStatus['operation'] = {
            'id': rejectedReceipt == 'foreign'
                ? 'ffffffff-ffff-4fff-8fff-ffffffffffff'
                : lastId,
            'state': 'unknown',
          };
        }
        return {
          'ok': true,
          'rejected': true,
          'code': 'configuration_changed',
          'autologon': responseStatus,
        };
      }
      if (held != null) return held!.future;
      if (loseResponse) {
        throw const HostSettingsException('synthetic observer deadline');
      }
      return {
        'ok': true,
        'autologon': loginState(
          state: 'configured',
          operationId: lastId,
          operationState: receiptState,
        ),
      };
    }
    final result = await base.request(action, fields);
    if (action == 'status') {
      final status = Map<String, dynamic>.from(result['status'] as Map);
      if (login != null) status['windowsAutologon'] = login;
      return {...result, 'status': status};
    }
    return result;
  }
}

Future<void> _open(
  WidgetTester t,
  _Host api, {
  fixture.FakeCloseGuard? guard,
  Size size = const Size(1366, 768),
  double scale = 1,
  Brightness brightness = Brightness.dark,
}) async {
  t.view.devicePixelRatio = 1;
  t.view.physicalSize = size;
  addTearDown(t.view.resetPhysicalSize);
  addTearDown(t.view.resetDevicePixelRatio);
  await t.pumpWidget(
    MaterialApp(
      theme: buildAppTheme(brightness: brightness),
      builder: (context, child) => MediaQuery(
        data: MediaQuery.of(context)
            .copyWith(textScaler: TextScaler.linear(scale)),
        child: child!,
      ),
      home: HostSettingsScreen(
        api: api,
        closeGuard: guard ?? fixture.FakeCloseGuard(),
      ),
    ),
  );
  await t.pumpAndSettle();
  await t.scrollUntilVisible(
    find.byKey(const Key('host-nav-5')),
    120,
    scrollable: find.descendant(
      of: find.byKey(const Key('host-settings-sidebar')),
      matching: find.byType(Scrollable),
    ),
  );
  await t.pumpAndSettle();
  await t.tap(find.byKey(const Key('host-nav-5')));
  await t.pumpAndSettle();
}

Future<void> tap(WidgetTester t, String key) async {
  await t.ensureVisible(find.byKey(Key(key)));
  await t.tap(find.byKey(Key(key)));
  await t.pump();
}

// The one Windows account form: saving it also turns automatic login on.
Future<void> draft(WidgetTester t) async {
  await t.enterText(
    find.byKey(const Key('host-account-username')),
    'fixture-user',
  );
  await t.enterText(
    find.byKey(const Key('host-account-password')),
    'synthetic-password',
  );
  await t.enterText(
    find.byKey(const Key('host-account-password-confirmation')),
    'synthetic-password',
  );
}

Future<void> closeResult(WidgetTester t) async {
  if (find.byKey(const Key('host-result-close')).evaluate().isNotEmpty) {
    await t.tap(find.byKey(const Key('host-result-close')));
    await t.pumpAndSettle();
  }
}

int _saves(_Host api) =>
    api.base.requests.where((v) => v['action'] == 'saveRdpCredentials').length;

// A local account without a Windows password: a blank save checks it, then
// the form makes one for exactly that account.
Future<void> _openNewPassword(
  WidgetTester t,
  _Host api, {
  fixture.FakeCloseGuard? guard,
}) async {
  api.passwordState = {
    'error': '',
    'state': 'empty',
    'sid': fixtureSid,
    'minimumLength': 8,
  };
  await _open(t, api, guard: guard);
  await t.enterText(
    find.byKey(const Key('host-account-username')),
    'fixture-user',
  );
  await tap(t, 'host-save-account');
  await t.pumpAndSettle();
}

Future<void> makePassword(WidgetTester t) async {
  await t.enterText(
    find.byKey(const Key('host-account-password')),
    'synthetic-password',
  );
  await t.enterText(
    find.byKey(const Key('host-account-password-confirmation')),
    'synthetic-password',
  );
  await confirm(t);
  await t.pumpAndSettle();
}

bool says(String text) => find
    .descendant(
      of: find.byKey(const Key('host-result-dialog')),
      matching: find.textContaining(text),
    )
    .evaluate()
    .isNotEmpty;

Future<void> confirm(WidgetTester t) async {
  await tap(t, 'host-save-account');
  for (
    var i = 0;
    i < 8 && find.byKey(const Key('host-confirm')).evaluate().isEmpty;
    i++
  ) {
    await t.pump(const Duration(milliseconds: 50));
  }
  expect(find.byKey(const Key('host-confirm')), findsOneWidget);
  await t.tap(find.byKey(const Key('host-confirm')));
  await t.pump();
  await t.pump();
}

void main() {
  test('missing backend is unknown unsupported, never off or writable', () {
    final missing = WindowsAutologonStatus.parse(null);
    expect(missing.supported, isFalse);
    expect(missing.state, 'unknown');
    expect(missing.canApply, isFalse);
  });
  test('malformed revisions and unknown enums cannot enable actions', () {
    expect(
      WindowsAutologonStatus.parse({...loginState(), 'revision': 'bad'})
          .canApply,
      isFalse,
    );
    expect(
      WindowsAutologonStatus.parse({...loginState(), 'state': 'invented'})
          .supported,
      isFalse,
    );
  });
  test(
    'Windows observation does not imply configuration or stream readiness',
    () {
      final value = WindowsAutologonStatus.parse({
        ...loginState(),
        'loggedIn': true,
      });
      expect(value.state, 'off');
      expect(value.loggedIn, isTrue);
    },
  );
  test('operation IDs are UUIDs and independent between explicit intents', () {
    final a = newWindowsAutologonOperationId(),
        b = newWindowsAutologonOperationId();
    expect(WindowsAutologonStatus.uuidPattern.hasMatch(a), isTrue);
    expect(a, isNot(b));
  });
  testWidgets(
    'No auto-login switch: off is a setup step and drafts never call native mutation',
    (t) async {
      final api = _Host();
      await _open(t, api);
      expect(_autoLoginSwitch, findsNothing);
      // No bare switch either.
      expect(find.byType(Switch), findsNothing);
      expect(
        find.byKey(const Key('host-windows-autologon-toggle')),
        findsNothing,
      );
      // Nothing here is described as off by default (login-screen streaming
      // is ON by default since 2026-11-09).
      expect(find.textContaining('기본 꺼짐'), findsNothing);
      expect(find.textContaining('원할 때만'), findsNothing);
      expect(find.text('꺼짐 · 계정을 저장하면 켜져요'), findsOneWidget);
      expect(
        t
            .widget<HostSettingsNavigation>(find.byType(HostSettingsNavigation))
            .states[5],
        HostSetupState.setup,
      );
      await draft(t);
      expect(api.applies, 0);
      expect(_saves(api), 0);
      expect(
        api.calls.where(
          (v) =>
              v.startsWith('windowsAutologon') && v != 'windowsAutologonStatus',
        ),
        isEmpty,
      );
      expect(
        // Signed out or locked: Console or a restart (2026-11-09); no switch.
        find.textContaining(
          'Windows에서 로그아웃되거나 잠기면 Console(로컬 PC는 본체 화면)에서 로그인하세요.',
        ),
        findsOneWidget,
      );
      expect(find.textContaining('다시 시작하면 자동으로 로그인돼요.'), findsOneWidget);
      await t.pumpWidget(const SizedBox());
    },
  );
  testWidgets(
    'Saving the account turns auto-login on after one confirmation and reports each step',
    (t) async {
      final api = _Host();
      await _open(t, api);
      await draft(t);
      await tap(t, 'host-save-account');
      await t.pumpAndSettle();
      final question = find.byKey(const Key('host-confirm-dialog'));
      expect(
        find.descendant(
          of: question,
          matching: find.textContaining('이 계정으로 자동 로그인해요'),
        ),
        findsOneWidget,
      );
      expect(
        find.descendant(
          of: question,
          matching: find.textContaining('synthetic-password'),
        ),
        findsNothing,
      );
      await t.tap(find.byKey(const Key('host-confirm')));
      await t.pumpAndSettle();
      // No second question between the account and auto-login steps.
      expect(find.byKey(const Key('host-confirm-dialog')), findsNothing);
      expect(_saves(api), 1);
      expect(api.applies, 1);
      final order = api.calls
          .where(
            (v) => v == 'saveRdpCredentials' || v == 'windowsAutologonApply',
          )
          .toList();
      expect(order, ['saveRdpCredentials', 'windowsAutologonApply']);
      final result = find.byKey(const Key('host-result-dialog'));
      expect(
        find.descendant(
          of: result,
          matching: find.textContaining('Windows 계정: 저장했어요.'),
        ),
        findsOneWidget,
      );
      expect(
        find.descendant(
          of: result,
          matching: find.textContaining('자동 로그인: 켰어요.'),
        ),
        findsOneWidget,
      );
      await closeResult(t);
      expect(find.byKey(const Key('host-account-password')), findsNothing);
      expect(
        t
            .widget<HostSettingsNavigation>(find.byType(HostSettingsNavigation))
            .states[5],
        HostSetupState.complete,
      );
      await t.pumpWidget(const SizedBox());
    },
  );
  testWidgets(
    'A saved account without auto-login is not complete and offers turning it on',
    (t) async {
      final api = _Host();
      api.base
        ..credentialsConfigured = true
        ..credentialUsername = 'fixture-user';
      await _open(t, api);
      expect(
        t
            .widget<HostSettingsNavigation>(find.byType(HostSettingsNavigation))
            .states[5],
        HostSetupState.setup,
      );
      final turnOn = find.byKey(const Key('host-edit-account'));
      expect(t.widget(turnOn), isA<FilledButton>());
      expect(
        find.descendant(of: turnOn, matching: find.text('자동 로그인 켜기')),
        findsOneWidget,
      );
      await tap(t, 'host-edit-account');
      await t.pumpAndSettle();
      expect(
        t
            .widget<TextField>(find.byKey(const Key('host-account-username')))
            .controller!
            .text,
        'fixture-user',
      );
      expect(
        t
            .widget<TextField>(find.byKey(const Key('host-account-password')))
            .controller!
            .text,
        isEmpty,
      );
      expect(api.applies, 0);
      await t.pumpWidget(const SizedBox());
    },
  );
  testWidgets(
    'Auto-login for a different account than the saved one needs a check',
    (t) async {
      final api = _Host()..login = loginState(state: 'configured');
      api.base
        ..credentialsConfigured = true
        ..credentialUsername = r'.\someone-else';
      await _open(t, api);
      expect(
        t
            .widget<HostSettingsNavigation>(find.byType(HostSettingsNavigation))
            .states[5],
        HostSetupState.attention,
      );
      expect(
        find.byKey(const Key('host-windows-account-mismatch')),
        findsOneWidget,
      );
      api.base.credentialUsername = r'.\FIXTURE-USER';
      await tap(t, 'host-refresh');
      await t.pumpAndSettle();
      expect(
        t
            .widget<HostSettingsNavigation>(find.byType(HostSettingsNavigation))
            .states[5],
        HostSetupState.complete,
      );
      expect(
        find.byKey(const Key('host-windows-account-mismatch')),
        findsNothing,
      );
      await t.pumpWidget(const SizedBox());
    },
  );
  testWidgets(
    'cancel confirmation saves nothing and clears both secret fields',
    (t) async {
      final api = _Host();
      await _open(t, api);
      await draft(t);
      await tap(t, 'host-save-account');
      await t.pump(const Duration(milliseconds: 250));
      expect(api.applies, 0);
      await t.tap(find.text('취소'));
      await t.pumpAndSettle();
      expect(api.applies, 0);
      expect(_saves(api), 0);
      expect(
        t
            .widget<TextField>(find.byKey(const Key('host-account-password')))
            .controller!
            .text,
        isEmpty,
      );
      expect(
        t
            .widget<TextField>(
              find.byKey(const Key('host-account-password-confirmation')),
            )
            .controller!
            .text,
        isEmpty,
      );
      await t.pumpWidget(const SizedBox());
    },
  );
  testWidgets('native close protection must succeed before any save', (
    t,
  ) async {
    final api = _Host(), guard = fixture.FakeCloseGuard()..available = false;
    await _open(t, api, guard: guard);
    await draft(t);
    await confirm(t);
    await t.pumpAndSettle();
    expect(_saves(api), 0);
    expect(api.applies, 0);
    expect(api.calls.contains('windowsAutologonStatus'), isFalse);
    await t.pumpWidget(const SizedBox());
  });
  testWidgets('fresh authority loss refuses the auto-login step', (t) async {
    final api = _Host();
    await _open(t, api);
    await draft(t);
    api.rejectFreshAdmin = true;
    await confirm(t);
    await t.pumpAndSettle();
    expect(_saves(api), 1);
    expect(api.applies, 0);
    final result = find.byKey(const Key('host-result-dialog'));
    expect(
      find.descendant(
        of: result,
        matching: find.textContaining('자동 로그인: 켰어요.'),
      ),
      findsNothing,
    );
    await t.pumpWidget(const SizedBox());
  });
  testWidgets('unknown result reconciles own UUID through GET without repost', (
    t,
  ) async {
    final api = _Host()..loseResponse = true;
    await _open(t, api);
    await draft(t);
    await confirm(t);
    await t.pumpAndSettle();
    await closeResult(t);
    expect(api.applies, 1);
    final id = api.lastId;
    await tap(t, 'host-windows-result-check');
    await t.pumpAndSettle();
    expect(api.applies, 1);
    expect(api.lastId, id);
    expect(find.byKey(const Key('host-windows-result-check')), findsNothing);
    await t.pumpWidget(const SizedBox());
  });
  testWidgets('opposite-action receipt cannot settle own apply intent', (
    t,
  ) async {
    final api = _Host()..receiptState = 'disabled';
    await _open(t, api);
    await draft(t);
    await confirm(t);
    await t.pumpAndSettle();
    await closeResult(t);
    expect(api.applies, 1);
    expect(find.byKey(const Key('host-windows-result-check')), findsOneWidget);
    await tap(t, 'host-windows-result-check');
    await t.pumpAndSettle();
    expect(api.applies, 1);
    expect(find.byKey(const Key('host-windows-result-check')), findsOneWidget);
    await t.pumpWidget(const SizedBox());
  });
  for (final state in ['unknown', 'external', 'failed']) {
    testWidgets(
      'matching acknowledgment with current $state requires status check',
      (t) async {
        final api = _Host()..followupState = state;
        await _open(t, api);
        await draft(t);
        await confirm(t);
        await t.pumpAndSettle();
        await closeResult(t);
        expect(api.applies, 1);
        expect(
          find.textContaining('요청은 끝났지만 현재 설정을 확인하지 못했어요.'),
          findsOneWidget,
        );
        expect(find.textContaining('자동 로그인: 켰어요.'), findsNothing);
        await t.pumpWidget(const SizedBox());
      },
    );
  }
  testWidgets(
    'foreign configuration has no overwrite path: the account alone is saved',
    (t) async {
      final api = _Host()..login = loginState(state: 'external');
      await _open(t, api);
      expect(_autoLoginSwitch, findsNothing);
      await draft(t);
      await confirm(t);
      await t.pumpAndSettle();
      expect(_saves(api), 1);
      expect(api.applies, 0);
      final result = find.byKey(const Key('host-result-dialog'));
      expect(
        find.descendant(
          of: result,
          matching: find.textContaining('넘겨받을 수 없어요'),
        ),
        findsOneWidget,
      );
      await t.pumpWidget(const SizedBox());
    },
  );
  for (final receipt in ['null', 'foreign', 'unknown']) {
    testWidgets(
      'admitted rejection with $receipt receipt retains own UUID and no repost',
      (t) async {
        final api = _Host()..rejectedReceipt = receipt;
        await _open(t, api);
        await draft(t);
        await confirm(t);
        await t.pumpAndSettle();
        await closeResult(t);
        expect(api.applies, 1);
        expect(api.lastId, isNotNull);
        expect(
          find.byKey(const Key('host-windows-result-check')),
          findsOneWidget,
        );
        expect(find.textContaining('자동 로그인: 켰어요.'), findsNothing);
        await t.pumpWidget(const SizedBox());
      },
    );
  }
  testWidgets(
    'a fresh blocked state saves the account only and never applies',
    (t) async {
      final api = _Host();
      await _open(t, api);
      await draft(t);
      api.login = loginState(state: 'external');
      await tap(t, 'host-refresh');
      await t.pumpAndSettle();
      await confirm(t);
      await t.pumpAndSettle();
      expect(_saves(api), 1);
      expect(api.applies, 0);
      await t.pumpWidget(const SizedBox());
    },
  );
  testWidgets(
    'late apply after screen dispose cannot revive UI or reissue native work',
    (t) async {
      final api = _Host()..held = Completer<Map<String, dynamic>>();
      await _open(t, api);
      await draft(t);
      await confirm(t);
      // The held apply keeps progress running: pump until it is issued.
      for (var i = 0; i < 40 && api.applies == 0; i++) {
        await t.pump(const Duration(milliseconds: 50));
      }
      expect(api.applies, 1);
      await t.pumpWidget(const SizedBox());
      api.held!.complete({
        'ok': true,
        'autologon': loginState(state: 'configured', operationId: api.lastId),
      });
      await t.pump();
      await t.pump();
      expect(api.applies, 1);
      expect(t.takeException(), isNull);
    },
  );
  testWidgets(
    'A blank password on an account without one offers 새 비밀번호 만들기 for that account',
    (t) async {
      final api = _Host();
      await _openNewPassword(t, api);
      expect(api.calls.where((v) => v == 'windowsAccountPasswordState'), [
        'windowsAccountPasswordState',
      ]);
      expect(find.byKey(const Key('host-confirm-dialog')), findsNothing);
      expect(find.byKey(const Key('host-new-password-title')), findsOneWidget);
      expect(find.textContaining('8자 이상'), findsOneWidget);
      expect(find.text('새 Windows 비밀번호'), findsOneWidget);
      expect(
        find.descendant(
          of: find.byKey(const Key('host-save-account')),
          matching: find.text('비밀번호 만들고 저장'),
        ),
        findsOneWidget,
      );
      // The Windows minimum is checked before anything is asked or sent.
      await t.enterText(
        find.byKey(const Key('host-account-password')),
        'short',
      );
      await t.enterText(
        find.byKey(const Key('host-account-password-confirmation')),
        'short',
      );
      await tap(t, 'host-save-account');
      await t.pumpAndSettle();
      expect(find.text('8자 이상으로 만들어 주세요.'), findsOneWidget);
      expect(find.byKey(const Key('host-confirm-dialog')), findsNothing);
      // Another account name leaves it: only the checked account gets one.
      await t.enterText(
        find.byKey(const Key('host-account-username')),
        'other-user',
      );
      await t.pump();
      expect(find.byKey(const Key('host-new-password-title')), findsNothing);
      expect(api.passwordSets, isEmpty);
      expect(_saves(api), 0);
      expect(api.applies, 0);
      await t.pumpWidget(const SizedBox());
    },
  );
  testWidgets(
    'Making the password: one confirmation names the consequence, then each step reports',
    (t) async {
      final api = _Host();
      await _openNewPassword(t, api);
      await t.enterText(
        find.byKey(const Key('host-account-password')),
        'synthetic-password',
      );
      await t.enterText(
        find.byKey(const Key('host-account-password-confirmation')),
        'synthetic-password',
      );
      await tap(t, 'host-save-account');
      await t.pumpAndSettle();
      final question = find.byKey(const Key('host-confirm-dialog'));
      for (final (text, count) in [
        ('이 계정에 Windows 비밀번호가 생겨요. 다음 로그인부터 이 비밀번호가 필요해요.', 1),
        ('Windows 계정: fixture-user', 1),
        ('비밀번호와 보안 정책은 바꾸지 않아요', 0),
        ('synthetic-password', 0),
      ]) {
        expect(
          find.descendant(of: question, matching: find.textContaining(text)),
          findsNWidgets(count),
        );
      }
      await t.tap(find.byKey(const Key('host-confirm')));
      await t.pumpAndSettle();
      // No second question between the steps.
      expect(find.byKey(const Key('host-confirm-dialog')), findsNothing);
      expect(
        api.calls.where(
          (v) => const {
            'setWindowsAccountPassword',
            'saveRdpCredentials',
            'windowsAutologonApply',
          }.contains(v),
        ),
        [
          'setWindowsAccountPassword',
          'saveRdpCredentials',
          'windowsAutologonApply',
        ],
      );
      expect(api.passwordSets.single, {
        'username': 'fixture-user',
        'sid': fixtureSid,
        'password': 'synthetic-password',
        'passwordConfirmation': 'synthetic-password',
        'confirmed': true,
      });
      expect(says('작업 완료'), isTrue);
      expect(says('Windows 비밀번호: 만들었어요. 다음 로그인부터 필요해요.'), isTrue);
      expect(says('Windows 계정: 저장했어요.'), isTrue);
      expect(says('자동 로그인: 켰어요.'), isTrue);
      expect(find.textContaining('synthetic-password'), findsNothing);
      await closeResult(t);
      expect(find.byKey(const Key('host-new-password-title')), findsNothing);
      await t.pumpWidget(const SizedBox());
    },
  );
  testWidgets(
    'A password Windows refuses saves nothing and says so on its own line',
    (t) async {
      final api = _Host()..passwordResult = 'password_policy';
      await _openNewPassword(t, api);
      await makePassword(t);
      expect(api.passwordSets, hasLength(1));
      expect(_saves(api), 0);
      expect(api.applies, 0);
      expect(says('결과 확인이 필요해요'), isTrue);
      expect(says('작업 완료'), isFalse);
      expect(says('Windows 비밀번호: 만들지 못했어요. Windows 비밀번호 규칙에 맞지 않아요.'), isTrue);
      expect(says('Windows 계정: 저장하지 않았어요.'), isTrue);
      expect(says('자동 로그인: 바꾸지 않았어요.'), isTrue);
      await closeResult(t);
      // Still making one for the same account; both entries were cleared.
      expect(find.byKey(const Key('host-new-password-title')), findsOneWidget);
      expect(
        t
            .widget<TextField>(find.byKey(const Key('host-account-password')))
            .controller!
            .text,
        isEmpty,
      );
      await t.pumpWidget(const SizedBox());
    },
  );
  testWidgets(
    'An account with a password, or one that is not local, is never offered one',
    (t) async {
      final api = _Host();
      await _open(t, api);
      await t.enterText(
        find.byKey(const Key('host-account-username')),
        'fixture-user',
      );
      Future<void> blankSave() async {
        await tap(t, 'host-save-account');
        await t.pumpAndSettle();
        expect(find.byKey(const Key('host-new-password-title')), findsNothing);
        expect(find.byKey(const Key('host-confirm-dialog')), findsNothing);
      }

      await blankSave();
      expect(find.text('Windows 비밀번호를 입력하세요.'), findsOneWidget);
      api.passwordState = {'error': 'local_account_required'};
      await blankSave();
      expect(find.textContaining('이 Windows의 로컬 계정이 아니에요.'), findsOneWidget);
      api.passwordState = {
        'error': '',
        'state': 'unknown',
        'sid': fixtureSid,
        'minimumLength': 0,
      };
      await blankSave();
      expect(find.textContaining('비밀번호가 없는지 확인하지 못했어요.'), findsOneWidget);
      // Not a local user SID (BUILTIN\Administrators): never opened.
      api.passwordState = {
        'error': '',
        'state': 'empty',
        'sid': 'S-1-5-32-544',
        'minimumLength': 0,
      };
      await blankSave();
      expect(
        api.calls.where((v) => v == 'windowsAccountPasswordState'),
        hasLength(4),
      );
      expect(api.passwordSets, isEmpty);
      expect(_saves(api), 0);
      await t.pumpWidget(const SizedBox());
    },
  );
  testWidgets(
    'No answer to the password step stops before saving and claims neither result',
    (t) async {
      final api = _Host()..losePasswordResponse = true;
      await _openNewPassword(t, api);
      await makePassword(t);
      expect(api.passwordSets, hasLength(1));
      expect(_saves(api), 0);
      expect(api.applies, 0);
      expect(says('Windows 비밀번호: 결과를 확인하지 못했어요.'), isTrue);
      expect(says('만들었어요'), isFalse);
      expect(says('Windows 계정: 저장하지 않았어요.'), isTrue);
      await closeResult(t);
      // The next blank save checks the account again.
      expect(find.byKey(const Key('host-new-password-title')), findsNothing);
      await t.pumpWidget(const SizedBox());
    },
  );
  testWidgets('Close protection must be ready before the password step', (
    t,
  ) async {
    final api = _Host(), guard = fixture.FakeCloseGuard()..available = false;
    await _openNewPassword(t, api, guard: guard);
    await makePassword(t);
    expect(api.passwordSets, isEmpty);
    expect(_saves(api), 0);
    expect(says('Windows 비밀번호: 만들지 않았어요.'), isTrue);
    await t.pumpWidget(const SizedBox());
  });
  testWidgets(
    'Password made but the account not saved is reported, never as success',
    (t) async {
      final api = _Host();
      api.base.rejectCredentials = true;
      await _openNewPassword(t, api);
      await makePassword(t);
      expect(api.passwordSets, hasLength(1));
      expect(_saves(api), 1);
      expect(api.applies, 0);
      expect(find.byKey(const Key('host-result-dialog')), findsOneWidget);
      expect(says('결과 확인이 필요해요'), isTrue);
      expect(says('Windows 비밀번호: 만들었어요. 다음 로그인부터 필요해요.'), isTrue);
      expect(says('Windows 계정: 저장하지 못했어요.'), isTrue);
      expect(says('자동 로그인: 바꾸지 않았어요.'), isTrue);
      await t.pumpWidget(const SizedBox());
    },
  );
  testWidgets(
    'Password made and account saved but auto-login unavailable is not success',
    (t) async {
      final api = _Host()..login = loginState(state: 'external');
      await _openNewPassword(t, api);
      await makePassword(t);
      expect(_saves(api), 1);
      expect(api.applies, 0);
      expect(says('결과 확인이 필요해요'), isTrue);
      expect(says('Windows 비밀번호: 만들었어요.'), isTrue);
      expect(says('Windows 계정: 저장했어요.'), isTrue);
      expect(says('넘겨받을 수 없어요'), isTrue);
      await t.pumpWidget(const SizedBox());
    },
  );
  for (final size in [
    const Size(1920, 1080),
    const Size(1366, 768),
    const Size(960, 640),
  ]) {
    for (final brightness in Brightness.values) {
      testWidgets(
        'Windows login layout ${size.width} ${brightness.name} 150%',
        (t) async {
          final api = _Host();
          await _open(t, api, size: size, brightness: brightness, scale: 1.5);
          await draft(t);
          await t.ensureVisible(find.byKey(const Key('host-save-account')));
          await t.pumpAndSettle();
          expect(
            find.byKey(const Key('host-save-account')).hitTestable(),
            findsOneWidget,
          );
          expect(t.takeException(), isNull);
          final fields = [
            t.widget<TextField>(find.byKey(const Key('host-account-password'))),
            t.widget<TextField>(
              find.byKey(const Key('host-account-password-confirmation')),
            ),
          ];
          expect(
            fields.every(
              (f) => f.obscureText && f.decoration!.suffixIcon == null,
            ),
            isTrue,
          );
          await t.pumpWidget(const SizedBox());
        },
      );
    }
  }
}

/// A switch for automatic login itself.
final _autoLoginSwitch = find.byType(SwitchListTile);
