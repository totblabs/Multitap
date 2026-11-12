import 'package:flutter/material.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:multitap_client/data/host_settings_api.dart';
import 'package:multitap_client/ui/app_theme.dart';
import 'package:multitap_client/ui/host_settings_screen.dart';

import 'host_settings_test.dart' as fixture;

Map<String, dynamic> _state({
  String state = 'external',
  String reason = 'foreign_configuration',
  bool? enabled = true,
  bool administrator = true,
  bool takeover = true,
  String revision = 'a',
  String? id,
  String? operation,
}) => {
  'schema': 1,
  'supported': true,
  'administrator': administrator,
  'supportsTakeOwnership': takeover,
  'enabled': enabled,
  'state': state,
  'reason': reason,
  'revision': List.filled(64, revision).join(),
  'accountLabel': state == 'off' ? '' : 'fixture-user',
  'loggedIn': false,
  if (id != null) 'operation': {'id': id, 'state': operation ?? 'unknown'},
};

// Memory-only Host settings. All passwords below are synthetic test inputs.
class _Host implements HostSettingsApi {
  _Host({Map<String, dynamic>? state}) : login = state ?? _state();
  final base = fixture.FakeHostSettings(registered: true);
  Map<String, dynamic> login;
  Map<String, dynamic>? fresh;
  final requests = <Map<String, dynamic>>[];
  bool loseResponse = false, partial = false, foreignReceipt = false;
  bool? partialEnabled = false;
  String partialCode = 'configuration_changed';
  String? lastId;
  List<Map<String, dynamic>> get mutations => requests
      .where(
        (value) => const {
          'windowsAutologonApply',
          'windowsAutologonDisable',
          'windowsAutologonRepair',
        }.contains(value['action']),
      )
      .toList();

  @override
  Future<Map<String, dynamic>> request(
    String action, [
    Map<String, dynamic> fields = const {},
  ]) async {
    requests.add({'action': action, ...fields});
    if (action == 'windowsAutologonStatus') {
      return {'ok': true, 'autologon': fresh ?? login};
    }
    if (const {
      'windowsAutologonApply',
      'windowsAutologonDisable',
      'windowsAutologonRepair',
    }.contains(action)) {
      lastId = fields['operationId'] as String;
      if (loseResponse) {
        login = _state(
          state: 'unknown',
          reason: 'state_unavailable',
          enabled: null,
          id: lastId,
        );
        throw const HostSettingsException('synthetic lost response');
      }
      if (partial) {
        login = _state(
          state: 'failed',
          reason: 'repair_required',
          enabled: partialEnabled,
          id: lastId,
          operation: 'partial',
        );
        return {
          'ok': true,
          'rejected': true,
          'code': partialCode,
          'autologon': login,
        };
      }
      final applying = action == 'windowsAutologonApply';
      login = _state(
        state: applying ? 'configured' : 'off',
        reason: 'none',
        enabled: applying,
        id: lastId,
        operation: applying ? 'configured' : 'disabled',
      );
      return {
        'ok': true,
        'autologon': foreignReceipt
            ? {
                ...login,
                'operation': {
                  'id': 'ffffffff-ffff-4fff-8fff-ffffffffffff',
                  'state': applying ? 'configured' : 'disabled',
                },
              }
            : login,
      };
    }
    final result = await base.request(action, fields);
    if (action != 'status') return result;
    return {
      ...result,
      'status': {
        ...Map<String, dynamic>.from(result['status'] as Map),
        'windowsAutologon': login,
      },
    };
  }
}

class _RefuseMutationGuard extends fixture.FakeCloseGuard {
  _RefuseMutationGuard(this.refuseAt);
  final int refuseAt;
  int mutationChecks = 0;
  @override
  Future<bool> update({required bool busy, required bool dirty}) async {
    if (busy && ++mutationChecks == refuseAt) return false;
    return super.update(busy: busy, dirty: dirty);
  }
}

Future<void> _open(
  WidgetTester t,
  _Host api, {
  Size size = const Size(1366, 768),
  double scale = 1,
  Brightness brightness = Brightness.dark,
  GlobalKey? boundary,
  fixture.FakeCloseGuard? guard,
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
      home: RepaintBoundary(
        key: boundary,
        child: HostSettingsScreen(
          api: api,
          closeGuard: guard ?? fixture.FakeCloseGuard(),
        ),
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

Future<void> _tap(WidgetTester t, String key) async {
  await t.ensureVisible(find.byKey(Key(key)));
  await t.pumpAndSettle();
  await t.tap(find.byKey(Key(key)));
  await t.pumpAndSettle();
}

// The one Windows account form (2026-10-29): saving it also applies or takes
// over automatic login. A saved account opens it with host-edit-account.
Future<void> _draft(WidgetTester t) async {
  if (find.byKey(const Key('host-account-password')).evaluate().isEmpty) {
    await _tap(t, 'host-edit-account');
  }
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

Future<void> _closeResult(WidgetTester t) async {
  if (find.byKey(const Key('host-result-close')).evaluate().isNotEmpty) {
    await _tap(t, 'host-result-close');
  }
}

int _saves(_Host api) =>
    api.requests.where((v) => v['action'] == 'saveRdpCredentials').length;

Future<void> _confirm(WidgetTester t, String key) async {
  await _tap(t, key);
  expect(find.byKey(const Key('host-confirm')), findsOneWidget);
  await _tap(t, 'host-confirm');
}

String _field(WidgetTester t, String key) =>
    t.widget<TextField>(find.byKey(Key(key))).controller!.text;

Future<void> _dispose(WidgetTester t) async {
  await t.pumpWidget(const SizedBox());
  await t.pumpAndSettle();
}

void main() {
  test('takeover is additive and actual flag observation is nullable', () {
    final legacy = WindowsAutologonStatus.parse({
      ..._state(),
      'supportsTakeOwnership': null,
      'enabled': '1',
    });
    expect(legacy.canTakeOwnership, isFalse);
    expect(legacy.enabled, isNull);
    final external = WindowsAutologonStatus.parse(_state());
    expect(external.canTakeOwnership, isTrue);
    expect(external.canApply, isFalse);
    expect(external.canDisable, isFalse);
    expect(
      WindowsAutologonStatus.parse(_state(administrator: false))
          .canTakeOwnership,
      isFalse,
    );
  });

  test('policy blocks apply but keeps separately authorized external or owned disable', () {
    final external = WindowsAutologonStatus.parse(
      _state(reason: 'policy_restricted'),
    );
    expect(external.canTakeOwnershipApply, isFalse);
    expect(external.canTakeOwnership, isTrue);
    final owned = WindowsAutologonStatus.parse(
      _state(state: 'policy_blocked', reason: 'policy_restricted'),
    );
    expect(owned.canDisable, isTrue);
    expect(owned.canTakeOwnership, isFalse);
    expect(
      WindowsAutologonStatus.parse(
        _state(state: 'policy_blocked', enabled: null),
      ).canDisable,
      isFalse,
    );
  });

  testWidgets(
    'external settings are taken over only by saving the account, never switched off',
    (t) async {
      final api = _Host();
      await _open(t, api);
      expect(_autoLoginSwitch, findsNothing);
      for (final removed in [
        'host-windows-autologon-toggle',
        'host-windows-takeover',
        'host-windows-takeover-disable',
      ]) {
        expect(find.byKey(Key(removed)), findsNothing, reason: removed);
      }
      expect(find.text('계정을 저장하면 이 Host가 기존 설정을 넘겨받아 켜요.'), findsOneWidget);
      expect(
        find.descendant(
          of: find.byKey(const Key('host-save-account')),
          matching: find.text('저장하고 자동 로그인 켜기'),
        ),
        findsOneWidget,
      );
      expect(api.mutations, isEmpty);
      await _dispose(t);
    },
  );

  testWidgets(
    'old Host without takeover capability saves the account only and explains the next action',
    (t) async {
      final api = _Host(state: _state(takeover: false));
      await _open(t, api);
      expect(_autoLoginSwitch, findsNothing);
      expect(find.textContaining('Host 업데이트 후 다시 확인하세요.'), findsOneWidget);
      await _draft(t);
      await _confirm(t, 'host-save-account');
      expect(_saves(api), 1);
      expect(api.mutations, isEmpty);
      await _dispose(t);
    },
  );

  testWidgets(
    'takeover requires local password confirmation and explicit irreversible-cleanup consent',
    (t) async {
      final api = _Host();
      await _open(t, api);
      await _draft(t);
      await t.enterText(
        find.byKey(const Key('host-account-password-confirmation')),
        'different',
      );
      await _tap(t, 'host-save-account');
      expect(find.byKey(const Key('host-confirm')), findsNothing);
      expect(api.mutations, isEmpty);
      await t.enterText(
        find.byKey(const Key('host-account-password-confirmation')),
        'synthetic-password',
      );
      await _tap(t, 'host-save-account');
      expect(find.textContaining('삭제되며 복원할 수 없어요'), findsOneWidget);
      expect(api.mutations, isEmpty);
      expect(_saves(api), 0);
      await _tap(t, 'host-confirm');
      final sent = api.mutations.single;
      expect(sent['takeOwnership'], isTrue);
      expect(sent['confirmed'], isTrue);
      expect(sent['username'], 'fixture-user');
      expect(sent['password'], 'synthetic-password');
      expect(sent['passwordConfirmation'], 'synthetic-password');
      expect(
        WindowsAutologonStatus.uuidPattern.hasMatch(
          sent['operationId'] as String,
        ),
        isTrue,
      );
      // The account step ran first, once, with the same account.
      final save = api.requests.indexWhere(
        (v) => v['action'] == 'saveRdpCredentials',
      );
      expect(_saves(api), 1);
      expect(api.requests[save]['username'], 'fixture-user');
      expect(save, lessThan(api.requests.indexOf(sent)));
      await _closeResult(t);
      expect(find.byKey(const Key('host-account-password')), findsNothing);
      await _dispose(t);
    },
  );

  testWidgets(
    'external policy restriction offers no switch-off and saving keeps the policy',
    (t) async {
      final api = _Host(state: _state(reason: 'policy_restricted'));
      await _open(t, api);
      expect(
        find.byKey(const Key('host-windows-takeover-disable')),
        findsNothing,
      );
      await _draft(t);
      await _confirm(t, 'host-save-account');
      expect(_saves(api), 1);
      expect(api.mutations, isEmpty);
      expect(
        find.descendant(
          of: find.byKey(const Key('host-result-dialog')),
          matching: find.textContaining('정책'),
        ),
        findsOneWidget,
      );
      await _dispose(t);
    },
  );

  testWidgets(
    'ordinary owned account replacement has no takeover flag and needs no saved account',
    (t) async {
      final api = _Host(
        state: _state(state: 'configured', reason: 'none'),
      );
      api.base.statusOverrides = {
        'windowsAccount': {'configured': false, 'username': '', 'error': ''},
      };
      await _open(t, api);
      await _draft(t);
      await _confirm(t, 'host-save-account');
      expect(api.mutations.single.containsKey('takeOwnership'), isFalse);
      expect(api.mutations.single['action'], 'windowsAutologonApply');
      await _dispose(t);
    },
  );

  testWidgets('owned setting under policy offers no switch-off', (t) async {
    final api = _Host(
      state: _state(state: 'policy_blocked', reason: 'policy_restricted'),
    );
    await _open(t, api);
    expect(_autoLoginSwitch, findsNothing);
    expect(find.text('Windows 정책으로 켤 수 없어요'), findsOneWidget);
    expect(api.mutations, isEmpty);
    await _dispose(t);
  });

  testWidgets(
    'A saved account opens the form with its name and empty password fields',
    (t) async {
      final api = _Host(
        state: _state(state: 'off', reason: 'none'),
      );
      api.base.credentialUsername = r'.\same-windows-user';
      api.base.credentialsConfigured = true;
      await _open(t, api);
      await _tap(t, 'host-edit-account');
      expect(_field(t, 'host-account-username'), r'.\same-windows-user');
      expect(_field(t, 'host-account-password'), isEmpty);
      expect(_field(t, 'host-account-password-confirmation'), isEmpty);
      expect(_saves(api), 0);
      expect(api.mutations, isEmpty);
      await _dispose(t);
    },
  );

  testWidgets('cancel clears the draft secrets without a mutation', (t) async {
    final api = _Host();
    api.base
      ..credentialsConfigured = true
      ..credentialUsername = 'fixture-user';
    await _open(t, api);
    await _draft(t);
    await _tap(t, 'host-cancel-account');
    await _tap(t, 'host-confirm');
    expect(find.byKey(const Key('host-account-password')), findsNothing);
    await _tap(t, 'host-edit-account');
    expect(_field(t, 'host-account-password'), isEmpty);
    expect(_field(t, 'host-account-password-confirmation'), isEmpty);
    expect(_saves(api), 0);
    expect(api.mutations, isEmpty);
    await _dispose(t);
  });

  for (final changed in ['revision', 'administrator', 'external-policy']) {
    testWidgets(
      'fresh $changed invalidates the confirmed takeover before POST',
      (t) async {
        final api = _Host();
        await _open(t, api);
        await _draft(t);
        api.fresh = switch (changed) {
          'revision' => _state(revision: 'b'),
          'administrator' => _state(administrator: false),
          _ => _state(reason: 'policy_restricted'),
        };
        await _confirm(t, 'host-save-account');
        expect(api.mutations, isEmpty);
        expect(find.textContaining('자동 로그인: 켰어요.'), findsNothing);
        await _closeResult(t);
        expect(find.byKey(const Key('host-account-password')), findsNothing);
        await _dispose(t);
      },
    );
  }

  // Busy close-guard checks in order: 1 before the account save, 2 the
  // status refresh after it (not awaited), 3 and 4 the auto-login step's two
  // checks before its POST.
  for (final refusal in [1, 3, 4]) {
    testWidgets(
      'close protection refusal $refusal prevents the takeover POST',
      (t) async {
        final api = _Host();
        await _open(t, api, guard: _RefuseMutationGuard(refusal));
        await _draft(t);
        await _confirm(t, 'host-save-account');
        expect(api.mutations, isEmpty);
        expect(_saves(api), refusal == 1 ? 0 : 1);
        await _dispose(t);
      },
    );
  }

  testWidgets('lost POST response keeps its own UUID and only GET is retried', (
    t,
  ) async {
    final api = _Host()..loseResponse = true;
    await _open(t, api);
    await _draft(t);
    await _confirm(t, 'host-save-account');
    await _closeResult(t);
    final id = api.lastId;
    await _tap(t, 'host-windows-result-check');
    await _tap(t, 'host-windows-result-check');
    expect(api.mutations, hasLength(1));
    expect(
      api.requests.where(
        (v) =>
            v['action'] == 'windowsAutologonStatus' && v['operationId'] == id,
      ),
      hasLength(2),
    );
    expect(find.byKey(const Key('host-windows-result-check')), findsOneWidget);
    await _dispose(t);
  });

  testWidgets(
    'foreign terminal receipt cannot release the own operation UUID',
    (t) async {
      final api = _Host()..foreignReceipt = true;
      await _open(t, api);
      await _draft(t);
      await _confirm(t, 'host-save-account');
      await _closeResult(t);
      expect(
        find.byKey(const Key('host-windows-result-check')),
        findsOneWidget,
      );
      expect(api.mutations, hasLength(1));
      await _tap(t, 'host-windows-result-check');
      expect(api.mutations, hasLength(1));
      await _dispose(t);
    },
  );

  for (final code in [
    'configuration_changed',
    'account_changed',
    'policy_restricted',
  ]) {
    testWidgets(
      'matching partial $code is admitted and exposes recovery after actual flag-off observation',
      (t) async {
        final api = _Host()
          ..partial = true
          ..partialCode = code;
        await _open(t, api);
        await _draft(t);
        await _confirm(t, 'host-save-account');
        // The account step and the auto-login step report separately.
        expect(
          find.descendant(
            of: find.byKey(const Key('host-result-dialog')),
            matching: find.textContaining('Windows 계정: 저장했어요.'),
          ),
          findsOneWidget,
        );
        await _closeResult(t);
        expect(find.text('자동 로그인 꺼짐 · 복구가 필요해요'), findsOneWidget);
        expect(find.byKey(const Key('host-windows-repair')), findsOneWidget);
        expect(
          find.byKey(const Key('host-windows-result-check')),
          findsNothing,
        );
        expect(api.mutations, hasLength(1));
        await _dispose(t);
      },
    );
  }

  testWidgets(
    'partial result never claims off when the actual flag is unknown',
    (t) async {
      final api = _Host()
        ..partial = true
        ..partialEnabled = null;
      await _open(t, api);
      await _draft(t);
      await _confirm(t, 'host-save-account');
      await _closeResult(t);
      expect(find.text('자동 로그인 꺼짐 · 복구가 필요해요'), findsNothing);
      expect(find.text('변경 결과와 복구 상태를 확인해야 해요'), findsOneWidget);
      await _dispose(t);
    },
  );

  for (final scene in [
    'external',
    'policy',
    'unknown',
    'partial',
    'disabled',
    'owned',
  ]) {
    testWidgets('managed login state $scene has bounded, truthful actions', (
      t,
    ) async {
      final state = switch (scene) {
        'policy' => _state(reason: 'policy_restricted'),
        'unknown' => _state(
          state: 'unknown',
          reason: 'state_unavailable',
          enabled: null,
        ),
        'partial' => _state(
          state: 'failed',
          reason: 'repair_required',
          enabled: false,
        ),
        'disabled' => _state(state: 'off', reason: 'none', enabled: false),
        'owned' => _state(state: 'configured', reason: 'none'),
        _ => _state(),
      };
      final api = _Host(state: state);
      await _open(t, api, scale: 1.5);
      expect(t.takeException(), isNull);
      // No scene offers a switch or a way to turn automatic login off.
      expect(_autoLoginSwitch, findsNothing);
      expect(
        find.byKey(const Key('host-windows-takeover-disable')),
        findsNothing,
      );
      if (scene == 'unknown') {
        expect(find.byKey(const Key('host-windows-repair')), findsNothing);
      } else if (scene == 'partial') {
        expect(find.byKey(const Key('host-windows-repair')), findsOneWidget);
      } else if (scene == 'disabled') {
        expect(find.text('꺼짐 · 계정을 저장하면 켜져요'), findsOneWidget);
        expect(
          find.descendant(
            of: find.byKey(const Key('host-save-account')),
            matching: find.text('저장하고 자동 로그인 켜기'),
          ),
          findsOneWidget,
        );
      } else if (scene == 'owned') {
        expect(find.byKey(const Key('host-save-account')), findsOneWidget);
      }
      expect(api.mutations, isEmpty);
      await _dispose(t);
    });
  }

  for (final size in [
    const Size(1920, 1080),
    const Size(1366, 768),
    const Size(960, 640),
  ]) {
    for (final brightness in Brightness.values) {
      for (final scale in [1.0, 1.5, 2.0]) {
        testWidgets(
          'managed login layout ${size.width.toInt()} ${brightness.name} $scale',
          (t) async {
            final api = _Host();
            await _open(
              t,
              api,
              size: size,
              scale: scale,
              brightness: brightness,
            );
            expect(t.takeException(), isNull);
            await _draft(t);
            await t.ensureVisible(find.byKey(const Key('host-save-account')));
            await t.pumpAndSettle();
            expect(
              find.byKey(const Key('host-save-account')).hitTestable(),
              findsOneWidget,
            );
            expect(t.takeException(), isNull);
            expect(api.mutations, isEmpty);
            await _dispose(t);
          },
        );
      }
    }
  }
}

/// A switch for automatic login itself.
final _autoLoginSwitch = find.byType(SwitchListTile);
