import 'dart:async';

import 'package:flutter/material.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:multitap_client/data/host_settings_api.dart';
import 'package:multitap_client/ui/app_theme.dart';
import 'package:multitap_client/ui/host_settings_screen.dart';

import 'host_settings_test.dart' show FakeHostSettings, FakeCloseGuard;

class LoginRecordApi extends FakeHostSettings {
  LoginRecordApi() : super(registered: true) {
    credentialsConfigured = true;
    credentialUsername = 'saved-account-A';
  }
  String revision = 'a' * 64;
  bool clearSupported = true;
  VoidCallback? beforeClear;
  Completer<void>? clearPending;
  Map<String, dynamic>? replyOverride;
  void replace({String username = 'saved-account-B'}) {
    credentialsConfigured = true;
    credentialUsername = username;
    revision = 'b' * 64;
  }

  @override
  Future<Map<String, dynamic>> request(
    String action, [
    Map<String, dynamic> fields = const {},
  ]) async {
    if (action == 'clearWindowsLoginV1') {
      requests.add({'action': action, ...fields});
      beforeClear?.call();
      await clearPending?.future;
      if (fields['protocol'] != 'windows-login-clear-v1' ||
          fields['revision'] != revision) {
        throw const HostSettingsException(
          '저장 정보가 바뀌었어요.',
          code: 'windows_login_record_changed',
        );
      }
      if (replyOverride != null) return replyOverride!;
      credentialsConfigured = false;
      credentialUsername = '';
      return {
        'ok': true,
        'clear': {'protocol': 'windows-login-clear-v1', 'outcome': 'removed'},
      };
    }
    final response = await super.request(action, fields);
    if (action != 'status') return response;
    return {
      ...response,
      'status': {
        ...response['status'] as Map,
        'windowsAccount': {
          'configured': credentialsConfigured,
          'username': credentialUsername,
          'error': '',
          if (clearSupported) 'clearProtocol': 'windows-login-clear-v1',
          if (clearSupported && credentialsConfigured)
            'clearRevision': revision,
        },
      },
    };
  }

  Iterable<Map<String, dynamic>> get mutations => requests.where(
    (value) => const {
      'clearRdpCredentials',
      'clearWindowsLoginV1',
    }.contains(value['action']),
  );
}

Future<void> openAccount(
  WidgetTester t,
  LoginRecordApi api, {
  FakeCloseGuard? guard,
}) async {
  t.view.devicePixelRatio = 1;
  t.view.physicalSize = const Size(1440, 1100);
  addTearDown(t.view.resetDevicePixelRatio);
  addTearDown(t.view.resetPhysicalSize);
  await t.pumpWidget(
    MaterialApp(
      theme: buildAppTheme(),
      home: HostSettingsScreen(api: api, closeGuard: guard ?? FakeCloseGuard()),
    ),
  );
  await t.pumpAndSettle();
  await click(t, 'host-nav-5');
}

Future<void> click(WidgetTester t, String key) async {
  final target = find.byKey(Key(key));
  await t.ensureVisible(target);
  await t.tap(target);
  await t.pumpAndSettle();
}

void main() {
  for (final sameName in [false, true]) {
    testWidgets('confirmation cannot erase replacement sameName=$sameName', (
      t,
    ) async {
      final api = LoginRecordApi();
      await openAccount(t, api);
      await click(t, 'host-clear-account');
      api.replace(username: sameName ? 'saved-account-A' : 'saved-account-B');
      await click(t, 'host-confirm');
      expect(api.credentialsConfigured, isTrue);
      expect(
        api.credentialUsername,
        sameName ? 'saved-account-A' : 'saved-account-B',
      );
      expect(
        api.requests.where((r) => r['action'] == 'clearRdpCredentials'),
        isEmpty,
      );
      expect(
        api.requests.where((r) => r['action'] == 'status').length,
        greaterThan(1),
      );
      expect(t.takeException(), isNull);
    });
  }
  testWidgets('cancel never clears the saved record', (t) async {
    final api = LoginRecordApi();
    await openAccount(t, api);
    await click(t, 'host-clear-account');
    await t.tap(find.text('취소'));
    await t.pumpAndSettle();
    expect(api.credentialsConfigured, isTrue);
    expect(api.mutations, isEmpty);
  });
  testWidgets('stable confirmed record can be removed', (t) async {
    final api = LoginRecordApi();
    await openAccount(t, api);
    await click(t, 'host-clear-account');
    await click(t, 'host-confirm');
    expect(api.credentialsConfigured, isFalse);
    expect(api.mutations, hasLength(1));
    expect(api.mutations.single, {
      'action': 'clearWindowsLoginV1',
      'protocol': SavedWindowsLoginStatus.protocol,
      'revision': 'a' * 64,
    });
  });
  testWidgets(
    'observed replacement while consent is open sends no destructive RPC',
    (t) async {
      final api = LoginRecordApi();
      await openAccount(t, api);
      await click(t, 'host-clear-account');
      api.replace();
      await t.pump(const Duration(seconds: 3));
      await t.pumpAndSettle();
      await click(t, 'host-confirm');
      expect(api.mutations, isEmpty);
      expect(api.credentialUsername, 'saved-account-B');
      expect(find.text('저장 정보가 바뀌었어요'), findsOneWidget);
    },
  );
  testWidgets('native CAS rejects replacement after UI revalidation', (
    t,
  ) async {
    final api = LoginRecordApi();
    api.beforeClear = api.replace;
    await openAccount(t, api);
    await click(t, 'host-clear-account');
    await click(t, 'host-confirm');
    expect(api.credentialsConfigured, isTrue);
    expect(api.credentialUsername, 'saved-account-B');
    expect(api.mutations.single['revision'], 'a' * 64);
    expect(find.text('saved-account-B'), findsWidgets);
  });
  testWidgets(
    'legacy helper receives update guidance and no unbound deletion',
    (t) async {
      final api = LoginRecordApi()..clearSupported = false;
      await openAccount(t, api);
      await click(t, 'host-clear-account');
      expect(api.mutations, isEmpty);
      expect(find.text('Host 업데이트가 필요해요'), findsOneWidget);
      expect(find.byKey(const Key('host-confirm')), findsNothing);
    },
  );
  testWidgets(
    'unknown mutation response is not deletion success and is not replayed',
    (t) async {
      final api = LoginRecordApi()
        ..replyOverride = {
          'ok': true,
          'clear': {
            'protocol': SavedWindowsLoginStatus.protocol,
            'outcome': 'unknown',
          },
        };
      await openAccount(t, api);
      await click(t, 'host-clear-account');
      await click(t, 'host-confirm');
      expect(api.mutations, hasLength(1));
      expect(api.credentialsConfigured, isTrue);
      expect(find.textContaining('삭제 완료를 확인하지 못했어요'), findsWidgets);
      expect(find.text('Host에 저장된 Windows 로그인 정보를 지웠어요.'), findsNothing);
    },
  );
  testWidgets('native window close retires a pending clear consent', (t) async {
    final api = LoginRecordApi();
    final guard = FakeCloseGuard();
    await openAccount(t, api, guard: guard);
    await click(t, 'host-clear-account');
    await guard.onRequest!();
    await t.pumpAndSettle();
    expect(guard.closes, 1);
    await click(t, 'host-confirm');
    expect(api.mutations, isEmpty);
    expect(api.credentialsConfigured, isTrue);
  });
  testWidgets(
    'late native result after route exit does not notice or refresh a new owner',
    (t) async {
      final api = LoginRecordApi()..clearPending = Completer<void>();
      final nav = GlobalKey<NavigatorState>();
      t.view.devicePixelRatio = 1;
      t.view.physicalSize = const Size(1440, 1100);
      addTearDown(t.view.resetDevicePixelRatio);
      addTearDown(t.view.resetPhysicalSize);
      await t.pumpWidget(
        MaterialApp(
          navigatorKey: nav,
          theme: buildAppTheme(),
          home: const Scaffold(body: Text('new owner')),
        ),
      );
      unawaited(
        nav.currentState!.push(
          MaterialPageRoute<void>(
            builder: (_) =>
                HostSettingsScreen(api: api, closeGuard: FakeCloseGuard()),
          ),
        ),
      );
      await t.pumpAndSettle();
      await click(t, 'host-nav-5');
      await click(t, 'host-clear-account');
      await t.tap(find.byKey(const Key('host-confirm')));
      await t.pump();
      await t.pump(const Duration(milliseconds: 400));
      expect(api.mutations, hasLength(1));
      nav.currentState!.pop();
      await t.pumpAndSettle();
      final reads = api.requests.where((r) => r['action'] == 'status').length;
      api.clearPending!.complete();
      await t.pumpAndSettle();
      expect(find.text('new owner'), findsOneWidget);
      expect(find.byKey(const Key('host-result-dialog')), findsNothing);
      expect(api.requests.where((r) => r['action'] == 'status').length, reads);
      expect(t.takeException(), isNull);
    },
  );
  test('only exact removed receipts establish an observed removal', () {
    expect(
      windowsLoginRemovalObserved({
        'ok': true,
        'clear': {
          'protocol': SavedWindowsLoginStatus.protocol,
          'outcome': 'removed',
        },
      }),
      isTrue,
    );
    for (final receipt in [
      null,
      {'protocol': 'old', 'outcome': 'removed'},
      {'protocol': SavedWindowsLoginStatus.protocol, 'outcome': 'missing'},
      {
        'protocol': SavedWindowsLoginStatus.protocol,
        'outcome': 'removed',
        'extra': true,
      },
    ]) {
      expect(
        windowsLoginRemovalObserved({'ok': true, 'clear': receipt}),
        isFalse,
      );
    }
    expect(
      SavedWindowsLoginStatus.parse({
        'configured': true,
        'username': 'A',
        'clearRevision': 'a' * 64,
      }).canClear,
      isFalse,
    );
  });
}
