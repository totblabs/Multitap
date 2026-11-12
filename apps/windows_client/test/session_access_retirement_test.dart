import 'dart:async';

import 'package:flutter/foundation.dart';
import 'package:flutter/material.dart';
import 'package:flutter/services.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:fake_async/fake_async.dart';
import 'package:multitap_client/data/session_activity.dart';
import 'package:multitap_client/data/session_windows.dart';
import 'package:multitap_client/data/broker_api.dart';
import 'package:multitap_client/data/console_browser.dart';
import 'package:multitap_client/ui/themed_app.dart';

import 'auth_flow_test.dart' show TestLoginMemory, fixtureOrigin;
import 'pc_login_replacement_lifetime_test.dart'
    show NativePolicyBroker, AuthWatch, startApp, PolicyBroker;
import 'session_renewal_test.dart' show VirtualClock;

class _ConsolePolicyBroker extends NativePolicyBroker {
  _ConsolePolicyBroker(super.clock);
  @override
  Future<BrokerConsoleWebViewGrant> createConsoleWebView(
    String token,
    String desktopId,
  ) async => BrokerConsoleWebViewGrant(
    path: '/console/$desktopId/attach',
    code: 'c' * 43,
    expiresAt: DateTime.now().add(const Duration(seconds: 30)),
  );
  @override
  Future<void> closeConsoleWebViews(
    String token,
    String desktopId, {
    String? ownerId,
  }) async {}
}

void main() {
  for (final failure in ['throw', 'stall']) {
    for (final which in ['native', 'console']) {
      testWidgets(
        '$which hide $failure cannot suppress either exact close or verify retirement',
        (t) async {
          debugDefaultTargetPlatformOverride = TargetPlatform.windows;
          addTearDown(() => debugDefaultTargetPlatformOverride = null);
          final held = Completer<void>(), calls = <String>[];
          final messenger =
              TestDefaultBinaryMessengerBinding.instance.defaultBinaryMessenger;
          const consoleChannel = MethodChannel('multitap/window');
          Future<Object?> receive(MethodCall call) async {
            calls.add(call.method);
            if (call.method == 'openConsoleWebView') return true;
            if (call.method ==
                (which == 'native'
                    ? 'hideAuthentication'
                    : 'hideConsoleWebViews')) {
              if (failure == 'throw') {
                throw PlatformException(code: 'synthetic_hide_failed');
              }
              await held.future;
            }
            return null;
          }

          messenger.setMockMethodCallHandler(SessionWindows.channel, receive);
          messenger.setMockMethodCallHandler(consoleChannel, receive);
          addTearDown(
            () => messenger.setMockMethodCallHandler(
              SessionWindows.channel,
              null,
            ),
          );
          addTearDown(
            () => messenger.setMockMethodCallHandler(consoleChannel, null),
          );
          final clock = VirtualClock(),
              api = _ConsolePolicyBroker(clock),
              watch = AuthWatch();
          addTearDown(() async {
            if (!held.isCompleted) held.complete();
            await const ConsoleBrowser.windows().closeAll(api: api);
          });
          await startApp(
            t,
            api,
            clock,
            TestLoginMemory(),
            watchFactory: (_, _) => watch,
          );
          await const ConsoleBrowser.windows().open(
            fixtureOrigin,
            'pve-170',
            api: api,
            token: 'owner-1',
            current: () => true,
          );
          calls.clear();
          watch.terminal.add('pc_login_replaced');
          await t.pump();
          await t.pump(const Duration(milliseconds: 100));
          expect(calls.where((v) => v == 'retireAuthentication'), hasLength(1));
          expect(calls.where((v) => v == 'closeConsoleWebViews'), hasLength(1));
          if (failure == 'stall') await t.pump(const Duration(seconds: 16));
          await t.pump();
          final access = t
              .widget<ThemedApp>(find.byType(ThemedApp))
              .sessionAccess!;
          expect(access.retirementFailed, isTrue);
          expect(access.retirementFinished, isFalse);
          if (!held.isCompleted) held.complete();
          await t.pumpAndSettle();
          await t.pumpWidget(const SizedBox.shrink());
          debugDefaultTargetPlatformOverride = null;
        },
      );
    }
  }
  for (final fail in ['throw', 'stall']) {
    test(
      'route $fail cannot suppress native/console cleanup or claim verified retirement',
      () {
        fakeAsync((async) {
          var native = 0, consoles = 0;
          bool? verified;
          final held = Completer<void>();
          retireSessionResources(
            routes: () => fail == 'throw'
                ? Future.error(StateError('synthetic'))
                : held.future,
            native: () async {
              native++;
            },
            consoles: () async {
              consoles++;
              return true;
            },
          ).then((value) => verified = value);
          async.flushMicrotasks();
          expect(native, 1);
          expect(consoles, 1);
          if (fail == 'stall') {
            expect(verified, isNull);
            async.elapse(const Duration(seconds: 3));
          }
          expect(verified, isFalse);
          if (!held.isCompleted) held.complete();
          async.flushMicrotasks();
          expect(
            verified,
            isFalse,
            reason: 'late route success cannot overwrite timeout',
          );
        });
      },
    );
  }
  testWidgets(
    'authoritative inventory 401 never attempts saved-password login before reason ACK',
    (t) async {
      final clock = VirtualClock(),
          api = PolicyBroker(clock),
          memory = TestLoginMemory();
      await startApp(t, api, clock, memory);
      api.expired = true;
      api.renewRejected = true;
      await t.tap(find.byKey(const Key('refresh-desktops')));
      await t.pumpAndSettle();
      expect(api.recoveries, 0);
      expect(find.byKey(const ValueKey('session-end-ack')), findsOneWidget);
      expect(memory.entries.values.single.password, 'secret-pass');
      await t.pumpWidget(const SizedBox.shrink());
    },
  );
  testWidgets(
    'stalled route cleanup cannot prevent exact native retirement from starting',
    (t) async {
      debugDefaultTargetPlatformOverride = TargetPlatform.windows;
      addTearDown(() => debugDefaultTargetPlatformOverride = null);
      final calls = <String>[];
      final messenger =
          TestDefaultBinaryMessengerBinding.instance.defaultBinaryMessenger;
      messenger.setMockMethodCallHandler(SessionWindows.channel, (call) async {
        calls.add(call.method);
        return null;
      });
      addTearDown(
        () => messenger.setMockMethodCallHandler(SessionWindows.channel, null),
      );
      final clock = VirtualClock(),
          api = NativePolicyBroker(clock),
          watch = AuthWatch();
      final held = Completer<void>();
      await startApp(
        t,
        api,
        clock,
        TestLoginMemory(),
        watchFactory: (_, _) => watch,
      );
      final access = t.widget<ThemedApp>(find.byType(ThemedApp)).sessionAccess!;
      access.bindRoutes(held, () => held.future);
      watch.terminal.add('session_reauthentication_required');
      await t.pump();
      await t.pump(const Duration(milliseconds: 100));
      expect(
        calls.where((value) => value == 'hideAuthentication'),
        hasLength(1),
      );
      expect(
        calls.where((value) => value == 'retireAuthentication'),
        hasLength(1),
      );
      expect(
        t
            .widget<FilledButton>(find.byKey(const ValueKey('session-end-ack')))
            .onPressed,
        isNull,
      );
      held.complete();
      await t.pumpAndSettle();
      await t.pumpWidget(const SizedBox.shrink());
      debugDefaultTargetPlatformOverride = null;
    },
  );
}
