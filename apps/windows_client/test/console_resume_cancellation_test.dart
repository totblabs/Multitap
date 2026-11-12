// Actual ConsoleResume and ConsoleResumeDialog with private memory futures.
// Cancellation ends local observation; it does not prove a sent Host operation
// stopped or rolled back. No HTTP, native API, file or VM operation is used.
import 'dart:async';

import 'package:flutter/material.dart';
import 'package:flutter/services.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:multitap_client/data/broker_api.dart';
import 'package:multitap_client/data/console_resume.dart';
import 'package:multitap_client/models/console_preparation.dart';
import 'package:multitap_client/models/desktop_machine.dart';
import 'package:multitap_client/ui/app_theme.dart';
import 'package:multitap_client/ui/console_resume_dialog.dart';

import 'console_resume_test.dart' show RecoveryApi, desktop, machine, state;

Future<void> _microtasks([int count = 40]) async {
  for (var n = 0; n < count; n++) {
    await Future<void>.value();
  }
}

class _Navigation extends NavigatorObserver {
  final popped = <Route<dynamic>>[];
  @override
  void didPop(Route<dynamic> route, Route<dynamic>? previousRoute) {
    popped.add(route);
  }
}

class _DialogHarness {
  _DialogHarness(this.action);
  final Future<DesktopMachine> Function() action;
  final navigator = GlobalKey<NavigatorState>();
  final observer = _Navigation();
  final results = <DesktopMachine?>[];
  int calls = 0, cancelled = 0;

  Future<void> mount(
    WidgetTester tester, {
    Brightness brightness = Brightness.dark,
    Size size = const Size(960, 640),
    double textScale = 1,
  }) async {
    tester.view.devicePixelRatio = 1;
    tester.view.physicalSize = size;
    addTearDown(tester.view.resetDevicePixelRatio);
    addTearDown(tester.view.resetPhysicalSize);
    await tester.pumpWidget(
      MaterialApp(
        navigatorKey: navigator,
        navigatorObservers: [observer],
        theme: buildAppTheme(brightness: brightness),
        builder: (context, child) => MediaQuery(
          data: MediaQuery.of(context)
              .copyWith(textScaler: TextScaler.linear(textScale)),
          child: child!,
        ),
        home: Scaffold(
          body: Builder(
            builder: (context) => TextButton(
              key: const Key('open-cancellable-console'),
              onPressed: () async {
                final result = await showDialog<DesktopMachine>(
                  context: context,
                  barrierDismissible: false,
                  builder: (_) => ConsoleResumeDialog(
                    machine: machine,
                    resume: () {
                      calls++;
                      return action();
                    },
                    onCancelled: () {
                      cancelled++;
                    },
                  ),
                );
                results.add(result);
              },
              child: const Text('open'),
            ),
          ),
        ),
      ),
    );
    await tester.tap(find.byKey(const Key('open-cancellable-console')));
    await tester.pump();
    await tester.pump();
    await tester.pump(const Duration(milliseconds: 200));
    expect(find.byType(ConsoleResumeDialog), findsOneWidget);
    expect(calls, 1);
  }

  Future<void> drainClose(WidgetTester tester) async {
    // Prime the reverse ticker before advancing fake time. No settle loop can
    // accidentally wait for a held operation or an indeterminate spinner.
    await tester.pump();
    await tester.pump(const Duration(milliseconds: 300));
    await tester.pump();
  }
}

void main() {
  test(
    'pre-cancelled explicit and zone-scoped tokens never send console POST',
    () async {
      final cancellation = ConsoleResumeCancellation()..cancel();
      var loads = 0;
      for (final scoped in [false, true]) {
        final api = RecoveryApi();
        final workflow = ConsoleResume(
          api: api,
          loadDesktops: () async {
            loads++;
            return [desktop(ready: true)];
          },
          authorized: () => true,
        );
        final result = scoped
            ? cancellation.bind(
                () => workflow.run('synthetic-token', 'pve-149'),
              )
            : workflow.run(
                'synthetic-token',
                'pve-149',
                cancellation: cancellation,
              );
        await expectLater(result, throwsA(isA<ConsoleResumeCancelled>()));
        expect(api.posts, 0);
        expect(api.polls, 0);
      }
      expect(cancellation.isCancelled, isTrue);
      expect(loads, 0);
    },
  );

  for (final stage in [
    'post',
    'poll-delay',
    'poll',
    'load',
    'readiness-delay',
  ]) {
    for (final lateError in [false, true]) {
      test(
        'cancel promptly retires $stage observation; late ${lateError ? 'error' : 'success'} cannot advance it',
        () async {
          final cancellation = ConsoleResumeCancellation();
          final operation = Completer<ConsolePreparationState>();
          final desktops = Completer<List<BrokerDesktop>>();
          final pause = Completer<void>();
          final api = RecoveryApi();
          var loads = 0, delays = 0;
          api.start = stage == 'post'
              ? () => operation.future
              : () async => state(
                  status: stage == 'poll' || stage == 'poll-delay'
                      ? 'pending'
                      : 'succeeded',
                );
          if (stage == 'poll') api.poll = () => operation.future;
          final workflow = ConsoleResume(
            api: api,
            authorized: () => true,
            loadDesktops: () {
              loads++;
              return stage == 'load'
                  ? desktops.future
                  : Future.value([desktop(ready: stage != 'readiness-delay')]);
            },
            delay: (_) {
              delays++;
              return stage == 'poll-delay' || stage == 'readiness-delay'
                  ? pause.future
                  : Future<void>.value();
            },
          );
          Object? outcome;
          var settled = false;
          unawaited(
            workflow
                .run('synthetic-token', 'pve-149', cancellation: cancellation)
                .then<void>(
                  (value) {
                    outcome = value;
                    settled = true;
                  },
                  onError: (Object error, StackTrace _) {
                    outcome = error;
                    settled = true;
                  },
                ),
          );
          await _microtasks();
          final reached = switch (stage) {
            'post' => api.posts == 1,
            'poll' => api.polls == 1,
            'load' => loads == 1,
            _ => delays == 1,
          };
          final wasPending = !settled;
          cancellation.cancel();
          cancellation.cancel(); // Idempotence must not restart anything.
          await _microtasks();
          final cancelledBeforeActualReply =
              settled && outcome is ConsoleResumeCancelled;
          final callsAtCancellation = [api.posts, api.polls, loads, delays];
          // Complete only the actually observed fake with a late error. Unused
          // completers receive successful values, so this fixture adds no stray
          // unobserved error of its own and clears all timeout-bearing futures.
          if (lateError && (stage == 'post' || stage == 'poll')) {
            operation.completeError(StateError('synthetic-late-operation'));
          } else {
            operation.complete(state(status: 'succeeded'));
          }
          if (lateError && stage == 'load') {
            desktops.completeError(StateError('synthetic-late-list'));
          } else {
            desktops.complete([desktop(ready: true)]);
          }
          if (lateError &&
              (stage == 'poll-delay' || stage == 'readiness-delay')) {
            pause.completeError(StateError('synthetic-late-delay'));
          } else {
            pause.complete();
          }
          await _microtasks();
          expect(
            reached,
            isTrue,
            reason: 'The intended production await stage must actually have started.',
          );
          expect(wasPending, isTrue);
          expect(
            cancelledBeforeActualReply,
            isTrue,
            reason: 'Local cancellation must settle without the pending operation or the 45-second observer timeout.',
          );
          expect(outcome, isA<ConsoleResumeCancelled>());
          expect(
            [api.posts, api.polls, loads, delays],
            callsAtCancellation,
            reason: 'A cancelled observer must not poll/load again or retry the confirmed POST.',
          );
          expect(api.posts, 1);
        },
      );
    }
  }

  test('zone-scoped cancellation affects only its own workflow and preserves another token', () async {
    final a = ConsoleResumeCancellation(), b = ConsoleResumeCancellation();
    final replyA = Completer<ConsolePreparationState>();
    final replyB = Completer<ConsolePreparationState>();
    final apiA = RecoveryApi()..start = () => replyA.future;
    final apiB = RecoveryApi()..start = () => replyB.future;
    ConsoleResume workflow(RecoveryApi api) => ConsoleResume(
      api: api,
      loadDesktops: () async => [desktop(ready: true)],
      authorized: () => true,
    );
    final resultA = a.bind(() => workflow(apiA).run('synthetic-A', 'pve-149'));
    final observedA = expectLater(
      resultA,
      throwsA(isA<ConsoleResumeCancelled>()),
    );
    final resultB = b.bind(() => workflow(apiB).run('synthetic-B', 'pve-149'));
    await _microtasks();
    a.cancel();
    await observedA;
    replyB.complete(state(status: 'succeeded'));
    final ready = await resultB;
    replyA.complete(state(status: 'succeeded'));
    await _microtasks();
    expect(ready.canStream, isTrue);
    expect(a.isCancelled, isTrue);
    expect(b.isCancelled, isFalse);
    expect(apiA.posts, 1);
    expect(apiA.polls, 0);
    expect(apiB.posts, 1);
  });

  for (final lateError in [false, true]) {
    testWidgets(
      'dialog cancel returns null and late ${lateError ? 'error' : 'success'} cannot pop a newer unrelated route',
      (tester) async {
        final pending = Completer<DesktopMachine>();
        final harness = _DialogHarness(() => pending.future);
        await harness.mount(tester);
        expect(
          find.byKey(const Key('console-resume-cancel')).hitTestable(),
          findsOneWidget,
        );
        expect(find.byKey(const Key('console-resume-close')), findsNothing);
        await tester.tap(find.byKey(const Key('console-resume-cancel')));
        await harness.drainClose(tester);
        final closedWhilePending = find
            .byType(ConsoleResumeDialog)
            .evaluate()
            .isEmpty;
        final resultWhilePending = List<DesktopMachine?>.from(harness.results);
        final foreign = MaterialPageRoute<void>(
          builder: (_) => const Scaffold(
            key: Key('unrelated-successor-route'),
            body: Text('unrelated'),
          ),
        );
        unawaited(harness.navigator.currentState!.push(foreign));
        await tester.pump();
        await tester.pump(const Duration(milliseconds: 300));
        if (lateError) {
          pending.completeError(StateError('synthetic-late-dialog-result'));
        } else {
          pending.complete(machine);
        }
        await tester.pump();
        await tester.pump(const Duration(milliseconds: 100));
        expect(closedWhilePending, isTrue);
        expect(resultWhilePending, [null]);
        expect(harness.results, [null]);
        expect(harness.calls, 1);
        expect(harness.cancelled, 1);
        expect(
          find.byKey(const Key('unrelated-successor-route')),
          findsOneWidget,
        );
        expect(foreign.isCurrent, isTrue);
        expect(harness.observer.popped.contains(foreign), isFalse);
        expect(find.byType(ConsoleResumeDialog), findsNothing);
        expect(tester.takeException(), isNull);
      },
      variant: TargetPlatformVariant.only(TargetPlatform.windows),
    );
  }

  for (final action in ['escape', 'back']) {
    testWidgets(
      '$action cancels a busy preparation without waiting for its callback',
      (tester) async {
        final pending = Completer<DesktopMachine>();
        final harness = _DialogHarness(() => pending.future);
        await harness.mount(tester);
        if (action == 'escape') {
          await tester.sendKeyEvent(LogicalKeyboardKey.escape);
        } else {
          await tester.binding.handlePopRoute();
        }
        await harness.drainClose(tester);
        final closedWhilePending = find
            .byType(ConsoleResumeDialog)
            .evaluate()
            .isEmpty;
        final cancelledBeforeReply = harness.cancelled;
        final resultsBeforeReply = List<DesktopMachine?>.from(harness.results);
        pending.complete(machine);
        await tester.pump();
        expect(closedWhilePending, isTrue);
        expect(cancelledBeforeReply, 1);
        expect(resultsBeforeReply, [null]);
        expect(harness.results, [null]);
        expect(harness.calls, 1);
        expect(tester.takeException(), isNull);
      },
      variant: TargetPlatformVariant.only(TargetPlatform.windows),
    );
  }

  testWidgets(
    'dialog binds the unchanged callback to actual ConsoleResume so cancel stops late polling',
    (tester) async {
      final reply = Completer<ConsolePreparationState>();
      final api = RecoveryApi()..start = () => reply.future;
      var loads = 0;
      final workflow = ConsoleResume(
        api: api,
        authorized: () => true,
        loadDesktops: () async {
          loads++;
          return [desktop(ready: true)];
        },
        delay: (_) async {},
      );
      final harness = _DialogHarness(() async {
        await workflow.run('synthetic-token', 'pve-149'); // No explicit token.
        return machine;
      });
      await harness.mount(tester);
      expect(api.posts, 1);
      await tester.tap(find.byKey(const Key('console-resume-cancel')));
      await harness.drainClose(tester);
      reply.complete(state());
      await tester.pump();
      await tester.pump(const Duration(milliseconds: 100));
      expect(harness.results, [null]);
      expect(harness.calls, 1);
      expect(api.posts, 1);
      expect(api.polls, 0);
      expect(loads, 0);
      expect(tester.takeException(), isNull);
    },
    variant: TargetPlatformVariant.only(TargetPlatform.windows),
  );

  testWidgets(
    'a successful preparation still returns the machine exactly once',
    (tester) async {
      final reply = Completer<DesktopMachine>();
      final harness = _DialogHarness(() => reply.future);
      await harness.mount(tester);
      reply.complete(machine);
      await harness.drainClose(tester);
      await tester.pump();
      expect(harness.results, [machine]);
      expect(harness.calls, 1);
      expect(harness.cancelled, 0);
      expect(find.byType(ConsoleResumeDialog), findsNothing);
      expect(tester.takeException(), isNull);
    },
    variant: TargetPlatformVariant.only(TargetPlatform.windows),
  );

  for (final brightness in Brightness.values) {
    testWidgets(
      'busy cancel remains visible in ${brightness.name} narrow 200-percent dialog',
      (tester) async {
        final reply = Completer<DesktopMachine>();
        final harness = _DialogHarness(() => reply.future);
        await harness.mount(
          tester,
          brightness: brightness,
          size: const Size(360, 640),
          textScale: 2,
        );
        final bounds = tester.getRect(find.byType(Dialog));
        final cancel = find.byKey(const Key('console-resume-cancel'));
        final cancelBounds = tester.getRect(cancel);
        expect(bounds.left, greaterThanOrEqualTo(0));
        expect(bounds.top, greaterThanOrEqualTo(0));
        expect(bounds.right, lessThanOrEqualTo(360));
        expect(bounds.bottom, lessThanOrEqualTo(640));
        expect(cancel.hitTestable(), findsOneWidget);
        expect(cancelBounds.left, greaterThanOrEqualTo(bounds.left));
        expect(cancelBounds.right, lessThanOrEqualTo(bounds.right));
        expect(cancelBounds.bottom, lessThanOrEqualTo(bounds.bottom));
        expect(tester.takeException(), isNull);
        await tester.tap(cancel);
        await harness.drainClose(tester);
        final resultBeforeReply = List<DesktopMachine?>.from(harness.results);
        reply.complete(machine);
        await tester.pump();
        expect(resultBeforeReply, [null]);
        expect(harness.results, [null]);
        expect(harness.cancelled, 1);
        expect(tester.takeException(), isNull);
      },
      variant: TargetPlatformVariant.only(TargetPlatform.windows),
    );
  }
}
