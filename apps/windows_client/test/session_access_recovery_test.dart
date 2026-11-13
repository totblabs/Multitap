// Client22 recovery contracts. The unchanged four public21 RED assertions and
// their original isolated logs remain preserved under the private artifact.
import 'dart:async';

import 'package:flutter/foundation.dart';
import 'package:flutter/material.dart';
import 'package:flutter/services.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:multitap_client/data/session_windows.dart';
import 'package:multitap_client/data/session_activity.dart';
import 'package:multitap_client/ui/themed_app.dart';

import 'auth_flow_test.dart' show TestLoginMemory;
import 'pc_login_replacement_lifetime_test.dart'
    show NativePolicyBroker, AuthWatch, startApp;
import 'session_renewal_test.dart' show VirtualClock;

void main() {
  for (final failure in ['pending-error', 'timeout-then-late-success']) {
    testWidgets(
      'real App offers owner-bound local recovery after $failure; ACK cannot bypass retirement',
      (tester) async {
        debugDefaultTargetPlatformOverride = TargetPlatform.windows;
        try {
          final messenger =
              TestDefaultBinaryMessengerBinding.instance.defaultBinaryMessenger;
          final first = Completer<void>(), retry = Completer<void>();
          final closeOwners = <String>[];
          String? boundOwner;
          messenger.setMockMethodCallHandler(SessionWindows.channel, (call) async {
            if (call.method == 'bindAuthentication') {
              boundOwner = (call.arguments as Map)['owner'] as String;
            }
            if (call.method == 'retireAuthentication') {
              closeOwners.add((call.arguments as Map)['owner'] as String);
              if (closeOwners.length == 1) {
                if (failure == 'pending-error') {
                  throw PlatformException(code: 'session_close_pending');
                }
                await first.future;
              } else {
                await retry.future;
              }
            }
            return null;
          });
          addTearDown(() {
            if (!first.isCompleted) first.complete();
            if (!retry.isCompleted) retry.complete();
            messenger.setMockMethodCallHandler(SessionWindows.channel, null);
          });
          final clock = VirtualClock(), api = NativePolicyBroker(clock);
          final watch = AuthWatch();
          await startApp(
            tester,
            api,
            clock,
            TestLoginMemory(),
            watchFactory: (_, _) => watch,
          );
          final retiredOwner = boundOwner;
          expect(retiredOwner, isNotNull);
          expect(api.manual, 1);
          watch.terminal.add('pc_login_replaced');
          await tester.pump();
          await tester.pump(const Duration(milliseconds: 100));
          if (failure == 'timeout-then-late-success') {
            await tester.pump(const Duration(seconds: 16));
          }
          await tester.pump();
          final access = tester
              .widget<ThemedApp>(find.byType(ThemedApp))
              .sessionAccess!;
          expect(access.retirementFailed, isTrue);
          expect(access.retirementFinished, isFalse);
          expect(access.retirementAttempts, hasLength(1));
          // What could not be confirmed is said in the wait's place: no
          // mark keeps turning beside it.
          expect(find.byKey(const Key('session-end-busy')), findsNothing);
          expect(find.byKey(const Key('app-busy-line-spinner')), findsNothing);
          expect(find.textContaining('아직 확인하지 못했어요'), findsOneWidget);
          final originalOutcome = access.retirementAttempts.single;
          expect(originalOutcome.verified, isFalse);
          expect(closeOwners, [retiredOwner]);
          expect(
            tester
                .widget<ButtonStyleButton>(
                  find.byKey(const ValueKey('session-end-ack')),
                )
                .onPressed,
            isNull,
            reason: 'Unverified retirement never admits a new authentication owner',
          );
          if (!first.isCompleted) first.complete();
          await tester.pump();
          expect(access.retirementFinished, isFalse,
              reason: 'Late completion cannot rewrite the first timed-out attempt');
          expect(api.manual, 1);

          // Proposed22 behavior: explicit local cleanup/recheck action, separate
          // from ACK. This finder is the expected RED on current20/21 source.
          final recover = find.byKey(const ValueKey('session-end-retry'));
          expect(recover, findsOneWidget);
          final action = tester.widget<ButtonStyleButton>(recover).onPressed;
          expect(action, isNotNull);
          action!();
          action();
          await tester.pump();
          expect(closeOwners, [retiredOwner, retiredOwner],
              reason: 'Repeated recovery activation coalesces one exact-owner attempt');
          expect(access.retirementFinished, isFalse);
          expect(api.manual, 1, reason: 'Recovery never signs in or takes over again');
          expect(
            tester.widget<ButtonStyleButton>(recover).onPressed,
            isNull,
            reason: 'One recovery attempt is already pending',
          );
          // Checking again is a wait once more: it moves, and the earlier
          // result is not left standing beside it.
          expect(
            tester
                .widget<Text>(find.byKey(const Key('session-end-busy')))
                .data,
            '열린 연결을 닫는 중이에요',
          );
          expect(find.byKey(const Key('app-busy-line-spinner')), findsOneWidget);
          expect(find.textContaining('아직 확인하지 못했어요'), findsNothing);
          retry.complete();
          await tester.pumpAndSettle();
          expect(find.byKey(const Key('session-end-busy')), findsNothing);
          expect(access.retirementFinished, isTrue);
          expect(access.retirementAttempts, hasLength(2));
          expect(identical(access.retirementAttempts.first, originalOutcome), isTrue);
          expect(originalOutcome.verified, isFalse,
              reason: 'Successful recovery preserves the original failed observation');
          expect(access.retirementAttempts.last.verified, isTrue);
          expect(api.manual, 1);
          await tester.tap(find.byKey(const ValueKey('session-end-ack')));
          await tester.pumpAndSettle();
          expect(find.byKey(const Key('login-submit')), findsOneWidget);
          expect(api.manual, 1, reason: 'Returning to login is not automatic login');
          await tester.pumpWidget(const SizedBox.shrink());
        } finally {
          debugDefaultTargetPlatformOverride = null;
        }
      },
    );
  }

  testWidgets('fresh hide failure remains unverified until that stage succeeds', (
    tester,
  ) async {
    debugDefaultTargetPlatformOverride = TargetPlatform.windows;
    try {
      final messenger =
          TestDefaultBinaryMessengerBinding.instance.defaultBinaryMessenger;
      var hides = 0, closes = 0;
      messenger.setMockMethodCallHandler(SessionWindows.channel, (call) async {
        if (call.method == 'hideAuthentication' && ++hides <= 2) {
          throw PlatformException(code: 'synthetic_hide_failed');
        }
        if (call.method == 'retireAuthentication') closes++;
        return null;
      });
      addTearDown(() => messenger.setMockMethodCallHandler(SessionWindows.channel, null));
      final clock = VirtualClock(), api = NativePolicyBroker(clock);
      final watch = AuthWatch();
      await startApp(tester, api, clock, TestLoginMemory(),
          watchFactory: (_, _) => watch);
      watch.terminal.add('pc_login_replaced');
      await tester.pumpAndSettle();
      final access = tester.widget<ThemedApp>(find.byType(ThemedApp)).sessionAccess!;
      expect(access.retirementFinished, isFalse);
      expect(access.retirementAttempts.single.stages[SessionRetirementStage.nativeHide],
          SessionRetirementResult.failed);
      await tester.tap(find.byKey(const ValueKey('session-end-retry')));
      await tester.pumpAndSettle();
      expect(access.retirementFinished, isFalse);
      expect(access.retirementAttempts, hasLength(2));
      expect(tester.widget<ButtonStyleButton>(
          find.byKey(const ValueKey('session-end-ack'))).onPressed, isNull);
      expect(closes, 2,
          reason: 'Successful native close cannot mask a failed required hide');
      await tester.tap(find.byKey(const ValueKey('session-end-retry')));
      await tester.pumpAndSettle();
      expect(access.retirementFinished, isTrue);
      expect(access.retirementAttempts, hasLength(3));
      expect(access.retirementAttempts.take(2).every((r) => !r.verified), isTrue);
      expect(api.manual, 1);
      await tester.pumpWidget(const SizedBox.shrink());
    } finally {
      debugDefaultTargetPlatformOverride = null;
    }
  });

  testWidgets('stage timeout preserves the original work and observation', (
    tester,
  ) async {
    final held = Completer<void>();
    var mutations = 0;
    Future<void> mutation() { mutations++; return held.future; }
    final originalWork = mutation();
    Future<SessionRetirementOutcome> observe() => observeSessionRetirement(
      routes: () async {}, native: () => originalWork,
      consoles: () async => true,
    );
    final first = observe();
    await tester.pump();
    await tester.pump(const Duration(seconds: 16));
    final failed = await first;
    expect(failed.verified, isFalse);
    expect(failed.stages[SessionRetirementStage.nativeChildren],
        SessionRetirementResult.timedOut);
    held.complete();
    await tester.pump();
    final recovered = await observe();
    expect(recovered.verified, isTrue);
    expect(mutations, 1, reason: 'Recovery only joins the original work');
    expect(failed.stages[SessionRetirementStage.nativeChildren],
        SessionRetirementResult.timedOut);
    expect(() => failed.stages[SessionRetirementStage.routes] =
        SessionRetirementResult.verified, throwsUnsupportedError);
  });

  test('new retirement owner rejects an old pending recovery result', () async {
    final state = SessionActivity(), oldOwner = Object(), nextOwner = Object();
    final held = Completer<void>();
    final verified = SessionRetirementOutcome({
      SessionRetirementStage.nativeChildren: SessionRetirementResult.verified,
    });
    state.retire('pc_login_replaced');
    state.bindRetirementRecovery(oldOwner, () async {
      await held.future;
      state.finishRetirement(verified: true, outcome: verified,
          recoveryOwner: oldOwner);
    });
    state.finishRetirement(verified: false, recoveryOwner: oldOwner);
    final first = state.retryRetirement();
    expect(identical(first, state.retryRetirement()), isTrue);
    state.acknowledge();
    expect(state.reason, 'pc_login_replaced');
    state.start(null, record: () async => null, onTerminal: (_) {});
    state.retire('session_idle_timeout');
    state.bindRetirementRecovery(nextOwner, () async {});
    held.complete();
    await first;
    expect(state.reason, 'session_idle_timeout');
    expect(state.retirementFinished, isFalse);
    expect(state.retirementAttempts, isEmpty);
    expect(state.retirementRecovering, isFalse);
    state.finishRetirement(verified: true, outcome: verified,
        recoveryOwner: nextOwner);
    expect(state.retirementFinished, isTrue);
    state.dispose();
  });

  test('disposed retirement cannot commit a pending recovery or notify', () async {
    final state = SessionActivity(), owner = Object(), held = Completer<void>();
    state.retire('pc_login_replaced');
    state.bindRetirementRecovery(owner, () async {
      await held.future;
      state.finishRetirement(verified: true, recoveryOwner: owner);
    });
    state.finishRetirement(verified: false, recoveryOwner: owner);
    final pending = state.retryRetirement();
    state.dispose();
    held.complete();
    await pending;
    expect(state.retirementFinished, isFalse);
  });

  test('legacy boolean retirement has no recovery action unless bound', () {
    final state = SessionActivity();
    state.retire('pc_login_replaced');
    state.finishRetirement(verified: false);
    expect(state.retirementRecoveryAvailable, isFalse);
    state.acknowledge();
    expect(state.reason, 'pc_login_replaced');
    state.finishRetirement(verified: true);
    state.acknowledge();
    expect(state.reason, isNull);
    state.dispose();
  });
}
