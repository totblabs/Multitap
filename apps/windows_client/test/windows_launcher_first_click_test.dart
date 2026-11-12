// Real launcher/controller routes with in-memory broker and native-channel
// boundaries. No app window, credentials, service or live API is touched.
import 'dart:async';

import 'package:flutter/foundation.dart';
import 'package:flutter/material.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:multitap_client/app.dart';
import 'package:multitap_client/data/broker_api.dart';
import 'package:multitap_client/data/desktop_mapping.dart'
    show machineFromBroker;
import 'package:multitap_client/data/launcher_memory.dart';
import 'package:multitap_client/data/initial_viewport.dart';
import 'package:multitap_client/data/session_windows.dart';
import 'package:multitap_client/models/stream_options.dart';
import 'package:multitap_client/ui/workspace_shell.dart';

import 'auth_flow_test.dart' show TestLoginMemory, fixtureOrigin;
import 'console_resume_flow_test.dart' show desktop;
import 'pc_login_replacement_lifetime_test.dart'
    show NativePolicyBroker, QuietWatch;
import 'session_renewal_test.dart' show VirtualClock;
import 'helpers/stream_connect_consent.dart';


class _LauncherBroker extends NativePolicyBroker {
  _LauncherBroker(super.clock);
  List<BrokerDesktop> assigned = [desktop(ready: true)];
  int mediaRequests = 0;
  @override
  Future<List<BrokerDesktop>> desktops(String token) async => assigned;
  @override
  Future<BrokerConnection> connect(
    String token,
    String desktopId, {
    String mode = 'stream',
    StreamOptions? options,
    InitialViewport? initialViewport,
    int? displayScale,
  }) async {
    mediaRequests++;
    throw StateError('The launcher must open the native child, not own media');
  }
}

class _HeldPreferences extends SessionLauncherMemory {
  final ready = Completer<LauncherPreferences>();
  @override
  Future<LauncherPreferences> load(String server) => ready.future;
}

Future<void> _pressConnect(
  WidgetTester t,
  String id,
  int Function() opens,
) async {
  final before = opens();
  await t.ensureVisible(find.byKey(Key('open-$id')));
  await t.tap(find.byKey(Key('open-$id')));
  await t.pump();
  expect(opens(), before, reason: 'No native child opens before user consent.');
  await acceptStreamConnect(t, id);
}

Future<void> _mount(
  WidgetTester t,
  _LauncherBroker api,
  VirtualClock clock,
) async {
  t.view.physicalSize = const Size(1440, 900);
  t.view.devicePixelRatio = 1;
  addTearDown(t.view.reset);
  final memory = SessionLauncherMemory();
  await memory.save(
    fixtureOrigin.toString(),
    const LauncherPreferences(showPreviews: false),
  );
  await t.pumpWidget(
    MultitapApp(
      api: api,
      serverOrigin: fixtureOrigin,
      clock: clock.call,
      loginMemory: TestLoginMemory(),
      launcherMemory: memory,
      desktopWatchFactory: (_, _) => QuietWatch(),
    ),
  );
  await t.pumpAndSettle();
  await t.enterText(find.byKey(const Key('sign-in-id')), 'fixture-user');
  await t.enterText(
    find.byKey(const Key('sign-in-password')),
    'fixture-password',
  );
  await t.tap(find.byKey(const Key('login-submit')));
  await t.pumpAndSettle();
}

Future<void> _finish(WidgetTester t) async {
  await t.pumpWidget(const SizedBox.shrink());
  await t.pumpAndSettle();
  debugDefaultTargetPlatformOverride = null;
}

void main() {
  TestWidgetsFlutterBinding.ensureInitialized();
  final messenger =
      TestDefaultBinaryMessengerBinding.instance.defaultBinaryMessenger;
  setUp(() => debugDefaultTargetPlatformOverride = TargetPlatform.windows);
  tearDown(() {
    messenger.setMockMethodCallHandler(SessionWindows.channel, null);
    debugDefaultTargetPlatformOverride = null;
  });

  testWidgets(
    'first launcher click survives pending native authentication bind without a second activation',
    (t) async {
      final bind = Completer<void>(), opens = <Map>[];
      String? owner;
      messenger.setMockMethodCallHandler(SessionWindows.channel, (call) async {
        if (call.method == 'bindAuthentication') {
          owner = (call.arguments as Map)['owner'] as String;
          await bind.future;
        }
        if (call.method == 'open') {
          opens.add(call.arguments as Map);
          return {'opened': true};
        }
        return null;
      });
      final clock = VirtualClock();
      final api = _LauncherBroker(clock);
      await _mount(t, api, clock);
      expect(owner, isNotNull);
      await _pressConnect(t, 'qa-vm', () => opens.length);
      expect(
        opens,
        isEmpty,
        reason: 'A child cannot precede its exact owner binding ACK',
      );
      bind.complete();
      for (var n = 0; n < 10 && opens.isEmpty; n++) {
        await t.pump(const Duration(milliseconds: 20));
      }
      expect(opens, hasLength(1));
      final payload = opens.single['payload'] as Map;
      expect(payload['authenticationOwner'], owner);
      expect(payload['desktopId'], 'qa-vm');
      expect(api.mediaRequests, 0);
      expect(t.takeException(), isNull);
      await _finish(t);
    },
  );

  testWidgets(
    'assignment revoked while first click awaits bind never opens a stale child',
    (t) async {
      final bind = Completer<void>();
      var opens = 0;
      messenger.setMockMethodCallHandler(SessionWindows.channel, (call) async {
        if (call.method == 'bindAuthentication') await bind.future;
        if (call.method == 'open') {
          opens++;
          return {'opened': true};
        }
        return null;
      });
      final clock = VirtualClock();
      final api = _LauncherBroker(clock);
      await _mount(t, api, clock);
      await _pressConnect(t, 'qa-vm', () => opens);
      api.assigned = [];
      await t.tap(find.byKey(const Key('refresh-desktops')));
      await t.pumpAndSettle();
      bind.complete();
      await t.pumpAndSettle();
      expect(opens, 0);
      expect(api.mediaRequests, 0);
      expect(t.takeException(), isNull);
      await _finish(t);
    },
  );

  testWidgets(
    'first launcher connect tap works before saved launcher preferences resolve',
    (t) async {
      final memory = _HeldPreferences();
      var opens = 0;
      final target = desktop(ready: true);
      // The full-app cases above exercise HttpBrokerApi→native open. This one
      // isolates delayed preference/focus work from the same real launch control.
      await t.pumpWidget(
        MaterialApp(
          home: WorkspaceShell(
            onSignOut: () {},
            onRefresh: () {},
            displayName: 'fixture',
            serverOrigin: fixtureOrigin.toString(),
            launcherMemory: memory,
            machines: [machineFromBroker(target)],
            onOpenWindow: (_, mode, options) async {
              opens++;
              return const SessionWindowOpenResult(opened: true);
            },
          ),
        ),
      );
      await t.pump();
      await _pressConnect(t, 'qa-vm', () => opens);
      expect(opens, 1);
      memory.ready.complete(const LauncherPreferences());
      await t.pumpAndSettle();
      expect(
        opens,
        1,
        reason: 'Late preference/focus work must neither lose nor duplicate the user click',
      );
      await _finish(t);
    },
  );
}
