import 'dart:async';

import 'package:flutter/material.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:multitap_client/data/host_settings_api.dart';
import 'package:multitap_client/ui/app_theme.dart';
import 'package:multitap_client/ui/host_settings_screen.dart';

import 'host_settings_test.dart' show FakeHostSettings, FakeCloseGuard;

class OwnedApi extends FakeHostSettings {
  OwnedApi({super.registered = true});
  bool restartSupported = true, cancelSupported = true;
  Completer<void>? restarting;
  @override
  Future<Map<String, dynamic>> request(
    String action, [
    Map<String, dynamic> fields = const {},
  ]) async {
    if (action == 'restartServiceV1') {
      requests.add({'action': action, ...fields});
      await restarting?.future;
      running = true;
      return {
        'ok': true,
        'restart': {'completed': true},
      };
    }
    if (action == 'cancelConnectionV1') {
      requests.add({'action': action, ...fields});
      if (fields['revision'] != connectionRequest['cancelRevision']) {
        throw const HostSettingsException('connection_request_changed');
      }
      connectionRequest = {
        ...connectionRequest,
        'status': 'cancelled',
        'cancelAcknowledged': true,
      };
      return {'ok': true};
    }
    final result = await super.request(action, fields);
    if (action != 'status') return result;
    final status = Map<String, dynamic>.from(result['status'] as Map);
    status['service'] = {
      ...status['service'] as Map,
      if (restartSupported) 'restartProtocol': 'service-restart-v1',
    };
    if (!restartSupported) (status['service'] as Map).remove('restartProtocol');
    if (cancelSupported && (status['connectionRequest'] as Map).isNotEmpty) {
      status['connectionRequest'] = {
        ...status['connectionRequest'] as Map,
        'cancelProtocol': 'cancel-request-v1',
      };
    }
    if (!cancelSupported) {
      (status['connectionRequest'] as Map).remove('cancelProtocol');
    }
    return {...result, 'status': status};
  }
}

Map<String, dynamic> pendingRequest(
  String revision, {
  String? id = 'request-A',
}) => {
  'status': 'pending',
  'origin': 'https://synthetic.example.com',
  'requestId': ?id,
  'expiresAt': DateTime.now()
      .add(const Duration(minutes: 10))
      .toIso8601String(),
  'cancelRevision': revision,
};
Future<void> openOwned(
  WidgetTester t,
  OwnedApi api, {
  FakeCloseGuard? guard,
  bool settle = true,
  // The page that holds the control under test: 1 (서버 연결) for the
  // connection request, 4 (진단 보고서) for the Host service.
  int page = 0,
}) async {
  t.view.devicePixelRatio = 1;
  t.view.physicalSize = const Size(1440, 1000);
  addTearDown(t.view.resetDevicePixelRatio);
  addTearDown(t.view.resetPhysicalSize);
  await t.pumpWidget(
    MaterialApp(
      theme: buildAppTheme(),
      home: HostSettingsScreen(api: api, closeGuard: guard ?? FakeCloseGuard()),
    ),
  );
  if (settle) {
    await t.pumpAndSettle();
  } else {
    await t.pump();
    await t.pump(const Duration(milliseconds: 400));
  }
  if (page != 0) {
    await t.tap(find.byKey(Key('host-nav-$page')));
    await t.pumpAndSettle();
  }
}

Future<void> clickOwned(
  WidgetTester t,
  String key, {
  bool settle = true,
}) async {
  final f = find.byKey(Key(key));
  await t.ensureVisible(f);
  await t.tap(f);
  if (settle) {
    await t.pumpAndSettle();
  } else {
    await t.pump();
    await t.pump(const Duration(milliseconds: 400));
  }
}

void resumeOwned(WidgetTester t) {
  if (t.binding.lifecycleState != AppLifecycleState.resumed) {
    t.binding.handleAppLifecycleStateChanged(AppLifecycleState.resumed);
  }
}

void main() {
  testWidgets(
    'A successor request observed during the result notice is not opened for old edit consent',
    (t) async {
      final api = OwnedApi(registered: true)
        ..connectionRequest = pendingRequest('a' * 64);
      await openOwned(t, api, page: 1);
      await clickOwned(t, 'host-edit-pending-server');
      await clickOwned(t, 'host-confirm');
      expect(find.byKey(const Key('host-result-dialog')), findsOneWidget);
      api.connectionRequest = pendingRequest('b' * 64, id: 'request-B');
      await t.pump(const Duration(milliseconds: 3100));
      await t.pump();
      expect(
        find.byKey(const Key('host-edit-pending-server')),
        findsOneWidget,
        reason: 'The successor must actually reach the widget before closing the old notice.',
      );
      await clickOwned(t, 'host-result-close');
      expect(find.byKey(const Key('host-edit-pending-server')), findsOneWidget);
      expect(find.byKey(const Key('host-server-origin')), findsNothing);
    },
  );
  testWidgets(
    'Initial foreground recovery preserves a draft typed while its fresh read is held',
    (t) async {
      final api = OwnedApi(registered: false);
      final response = await api.request('status');
      api.requests.clear();
      final old = Completer<Map<String, dynamic>>();
      api.nextStatus = old;
      addTearDown(() => resumeOwned(t));
      await openOwned(t, api, settle: false);
      t.binding.handleAppLifecycleStateChanged(AppLifecycleState.inactive);
      old.complete(response);
      await t.pump();
      await t.pump(const Duration(milliseconds: 400));
      final fresh = Completer<Map<String, dynamic>>();
      api.nextStatus = fresh;
      resumeOwned(t);
      await t.pump(const Duration(milliseconds: 3100));
      await t.pump();
      await clickOwned(t, 'host-nav-1');
      await t.enterText(
        find.byKey(const Key('host-server-origin')),
        'https://new-draft.example.com',
      );
      await t.pump(const Duration(seconds: 3));
      expect(api.requests.where((r) => r['action'] == 'status'), hasLength(2));
      fresh.complete(response);
      await t.pumpAndSettle();
      expect(
        t
            .widget<TextField>(find.byKey(const Key('host-server-origin')))
            .controller!
            .text,
        'https://new-draft.example.com',
      );
      expect(
        api.requests.where(
          (r) => !{'status', 'encoderSettings'}.contains(r['action']),
        ),
        isEmpty,
      );
    },
  );
  testWidgets(
    'Old helpers are guided to update without untargeted cancel or restart fallback',
    (t) async {
      final api = OwnedApi(registered: false)
        ..restartSupported = false
        ..cancelSupported = false
        ..connectionRequest = pendingRequest('a' * 64);
      await openOwned(t, api, page: 1);
      await clickOwned(t, 'host-cancel-connection');
      expect(find.text('Host 업데이트가 필요해요'), findsOneWidget);
      expect(
        api.requests.where(
          (r) => !{'status', 'encoderSettings'}.contains(r['action']),
        ),
        isEmpty,
      );
      await clickOwned(t, 'host-result-close');
      await t.pumpWidget(const SizedBox());
      await t.pump();
      final service = OwnedApi()..restartSupported = false;
      await openOwned(t, service, page: 4);
      await clickOwned(t, 'host-service');
      expect(find.text('Host 업데이트가 필요해요'), findsOneWidget);
      expect(
        service.requests.where(
          (r) => !{'status', 'encoderSettings'}.contains(r['action']),
        ),
        isEmpty,
      );
    },
  );
  testWidgets(
    'Closing the uncertain-result notice cannot abandon a native restart',
    (t) async {
      final api = OwnedApi(), guard = FakeCloseGuard();
      await openOwned(t, api, guard: guard, page: 4);
      await clickOwned(t, 'host-service');
      api.failStatus = true;
      await clickOwned(t, 'host-confirm');
      expect(find.text('결과 확인이 필요해요'), findsOneWidget);
      await clickOwned(t, 'host-result-close');
      await guard.onRequest!();
      await t.pump();
      expect(guard.closes, 1);
      expect(api.running, isTrue);
      expect(
        api.requests.where((r) => r['action'] == 'restartServiceV1'),
        hasLength(1),
      );
      expect(api.requests.where((r) => r['action'] == 'service'), isEmpty);
    },
  );
  testWidgets(
    'A discarded post-write read reports uncertainty and never replays the write',
    (t) async {
      final api = OwnedApi()..restarting = Completer<void>();
      addTearDown(() => resumeOwned(t));
      await openOwned(t, api, page: 4);
      final response = await api.request('status');
      await clickOwned(t, 'host-service');
      await clickOwned(t, 'host-confirm', settle: false);
      final held = Completer<Map<String, dynamic>>();
      api.nextStatus = held;
      api.restarting!.complete();
      await t.pump();
      await t.pump();
      t.binding.handleAppLifecycleStateChanged(AppLifecycleState.inactive);
      held.complete(response);
      await t.pump();
      await t.pump(const Duration(milliseconds: 400));
      expect(find.textContaining('작업 응답은 받았지만'), findsOneWidget);
      resumeOwned(t);
      await t.pump(const Duration(milliseconds: 3100));
      await t.pumpAndSettle();
      expect(
        api.requests.where((r) => r['action'] == 'restartServiceV1'),
        hasLength(1),
      );
      expect(api.requests.where((r) => r['action'] == 'service'), isEmpty);
    },
  );
  testWidgets(
    'Initial foreground recovery reacquires one discarded status without manual refresh',
    (t) async {
      final api = OwnedApi(registered: false),
          held = Completer<Map<String, dynamic>>();
      final initial = await api.request('status');
      api.requests.clear();
      api.nextStatus = held;
      addTearDown(() => resumeOwned(t));
      await openOwned(t, api, settle: false);
      t.binding.handleAppLifecycleStateChanged(AppLifecycleState.inactive);
      held.complete(initial);
      await t.pump();
      await t.pump(const Duration(milliseconds: 400));
      resumeOwned(t);
      await t.pump(const Duration(milliseconds: 3100));
      await t.pumpAndSettle();
      expect(api.requests.where((r) => r['action'] == 'status'), hasLength(2));
      expect(find.byKey(const Key('host-refresh')), findsOneWidget);
    },
  );
  testWidgets(
    'Discarded background refresh retains its prior actionable warning',
    (t) async {
      final api = OwnedApi();
      addTearDown(() => resumeOwned(t));
      await openOwned(t, api);
      api.failStatus = true;
      await clickOwned(t, 'host-refresh');
      expect(find.textContaining('상태 조회 실패'), findsOneWidget);
      api.failStatus = false;
      final response = await api.request('status');
      final held = Completer<Map<String, dynamic>>();
      api.nextStatus = held;
      await clickOwned(t, 'host-refresh', settle: false);
      t.binding.handleAppLifecycleStateChanged(AppLifecycleState.inactive);
      held.complete(response);
      await t.pump();
      await t.pump(const Duration(milliseconds: 400));
      expect(find.textContaining('상태 조회 실패'), findsOneWidget);
      resumeOwned(t);
    },
  );
  testWidgets(
    'Consent for request A never cancels a replacement pending request B',
    (t) async {
      final api = OwnedApi(registered: false)
        ..connectionRequest = pendingRequest('a' * 64);
      await openOwned(t, api, page: 1);
      await clickOwned(t, 'host-cancel-connection');
      api.connectionRequest = pendingRequest('b' * 64, id: 'request-B');
      await t.pump(const Duration(milliseconds: 3100));
      await t.pump();
      await clickOwned(t, 'host-confirm');
      expect(
        api.requests.where(
          (r) =>
              r['action'] == 'cancelConnection' ||
              r['action'] == 'cancelConnectionV1',
        ),
        isEmpty,
      );
    },
  );
  testWidgets(
    'Exact revision cancels even before the server request ID is known',
    (t) async {
      final api = OwnedApi(registered: false)
        ..connectionRequest = pendingRequest('a' * 64, id: null);
      await openOwned(t, api, page: 1);
      await clickOwned(t, 'host-cancel-connection');
      await clickOwned(t, 'host-confirm');
      expect(
        api.requests.where((r) => r['action'] == 'cancelConnectionV1').single,
        {
          'action': 'cancelConnectionV1',
          'protocol': 'cancel-request-v1',
          'revision': 'a' * 64,
        },
      );
    },
  );
  testWidgets(
    'Restart is one native transaction despite persistent post-operation status failure',
    (t) async {
      final api = OwnedApi(), guard = FakeCloseGuard();
      await openOwned(t, api, guard: guard, page: 4);
      await clickOwned(t, 'host-service');
      api.failStatus = true;
      await clickOwned(t, 'host-confirm', settle: false);
      expect(api.requests.where((r) => r['action'] == 'service'), isEmpty);
      expect(
        api.requests.where((r) => r['action'] == 'restartServiceV1').single,
        {
          'action': 'restartServiceV1',
          'protocol': 'service-restart-v1',
          'confirmed': true,
        },
      );
      await t.pumpWidget(const SizedBox());
      await t.pump(const Duration(seconds: 31));
    },
  );
  testWidgets(
    'Restart holds the close guard until the native transaction finishes',
    (t) async {
      final api = OwnedApi()..restarting = Completer<void>(),
          guard = FakeCloseGuard();
      await openOwned(t, api, guard: guard, page: 4);
      await clickOwned(t, 'host-service');
      await clickOwned(t, 'host-confirm', settle: false);
      expect(guard.busy, isTrue);
      await guard.onRequest!();
      await t.pump();
      expect(guard.closes, 0);
      api.restarting!.complete();
      await t.pumpAndSettle();
      expect(api.requests.where((r) => r['action'] == 'service'), isEmpty);
    },
  );
  test('Atomic restart has a 120-second observer deadline', () {
    expect(
      hostSettingsRequestTimeout('restartServiceV1'),
      const Duration(seconds: 120),
    );
  });
}
