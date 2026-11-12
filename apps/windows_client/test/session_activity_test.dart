import 'dart:async';

import 'package:fake_async/fake_async.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:multitap_client/data/broker_api.dart';
import 'package:multitap_client/data/session_activity.dart';

SessionAccess policyAt(DateTime now) => SessionAccess(
  serverTime: now,
  idleDeadline: now.add(const Duration(minutes: 30)),
  idleTimeout: const Duration(minutes: 30),
  warning: const Duration(minutes: 1),
);

void main() {
  test('server-time policy tolerates client clock skew and subtracts request elapsed', () {
    fakeAsync((async) {
      final server = DateTime.utc(2026, 11, 5);
      final client = DateTime.utc(2020, 1, 1);
      final state = SessionActivity(clock: () => client.add(async.elapsed));
      final policy = SessionAccess.parse({
        'protocol': sessionAccessProtocol,
        'idleEnabled': true,
        'idleTimeoutMs': 30000,
        'warningMs': 10000,
        'serverTime': server.toIso8601String(),
        'idleDeadline': server
            .add(const Duration(seconds: 30))
            .toIso8601String(),
        'warningAt': server.add(const Duration(seconds: 20)).toIso8601String(),
      }, responseElapsed: const Duration(seconds: 2));
      state.start(policy, record: () async => null, onTerminal: (_) {});
      expect(state.reason, isNull);
      expect(state.remaining!.inSeconds, inInclusiveRange(27, 28));
      async.elapse(const Duration(seconds: 28));
      expect(state.reason, 'session_idle_timeout');
      state.dispose();
    });
  });

  for (final freshEnabled in [false, true]) {
    test(
      'delayed pre-toggle activity cannot overwrite final idleEnabled=$freshEnabled',
      () {
        fakeAsync((async) {
          final base = DateTime.utc(2026, 11, 5);
          SessionAccess next(bool enabled) => SessionAccess(
            serverTime: base.add(async.elapsed),
            idleEnabled: enabled,
            idleDeadline: enabled
                ? base.add(async.elapsed).add(const Duration(minutes: 30))
                : null,
            idleTimeout: const Duration(minutes: 30),
            warning: const Duration(minutes: 1),
          );
          final late = Completer<SessionAccess>();
          final changes = <bool>[];
          final state = SessionActivity(clock: () => base.add(async.elapsed));
          state.start(
            next(true),
            record: () => late.future,
            onTerminal: (_) {},
            setIdle: (enabled) async => next(enabled),
            onPolicy: (value) async => changes.add(value.idleEnabled),
          );
          state.human();
          unawaited(state.setIdleEnabled(false));
          async.flushMicrotasks();
          expect(state.idleEnabled, isFalse);
          if (freshEnabled) {
            unawaited(state.setIdleEnabled(true));
            async.flushMicrotasks();
          }
          final before = List<bool>.from(changes);
          late.complete(next(!freshEnabled));
          async.flushMicrotasks();
          expect(state.idleEnabled, freshEnabled);
          expect(
            changes,
            before,
            reason: 'stale response must not reach native policy/broadcast',
          );
          expect(state.remaining == null, !freshEnabled);
          state.dispose();
        });
      },
    );
  }

  test('disable pending retains old deadline; late success cannot revive expired owner', () {
    fakeAsync((async) {
      final base = DateTime.utc(2026, 11, 5);
      final pending = Completer<SessionAccess>();
      final native = <bool>[];
      final state = SessionActivity(clock: () => base.add(async.elapsed));
      state.start(
        policyAt(base),
        record: () async => null,
        onTerminal: (_) {},
        setIdle: (_) => pending.future,
        onPolicy: (value) async => native.add(value.idleEnabled),
      );
      unawaited(state.setIdleEnabled(false));
      async.flushMicrotasks();
      expect(state.idleEnabled, isTrue);
      expect(state.idleChangePending, isTrue);
      expect(native, isEmpty);
      async.elapse(const Duration(minutes: 30));
      pending.complete(
        SessionAccess(
          serverTime: base.add(async.elapsed),
          idleEnabled: false,
          idleDeadline: null,
          idleTimeout: const Duration(minutes: 30),
          warning: const Duration(minutes: 1),
        ),
      );
      async.flushMicrotasks();
      expect(state.reason, 'session_idle_timeout');
      expect(native, isEmpty);
      state.dispose();
    });
  });

  test('disabled idle has no timer but replacement remains terminal', () {
    fakeAsync((async) {
      final base = DateTime.utc(2026, 11, 5);
      final state = SessionActivity(clock: () => base.add(async.elapsed));
      state.start(
        SessionAccess(
          serverTime: base,
          idleEnabled: false,
          idleDeadline: null,
          idleTimeout: const Duration(minutes: 30),
          warning: const Duration(minutes: 1),
        ),
        record: () async => throw StateError('off must not submit activity'),
        onTerminal: (_) {},
      );
      state.human();
      async.elapse(const Duration(hours: 1));
      expect(state.reason, isNull);
      expect(state.remaining, isNull);
      expect(state.warning, isFalse);
      state.terminate('pc_login_replaced');
      expect(state.reason, 'pc_login_replaced');
      state.dispose();
    });
  });

  test(
    'child mirror cannot independently toggle or retire newer parent policy',
    () {
      fakeAsync((async) {
        final base = DateTime.utc(2026, 11, 5);
        final ended = <String>[];
        final state = SessionActivity(clock: () => base.add(async.elapsed));
        state.start(
          policyAt(base),
          record: () async => null,
          onTerminal: ended.add,
          expiryAuthority: false,
        );
        async.elapse(const Duration(minutes: 31));
        expect(ended, isEmpty);
        expect(state.canChangeIdle, isFalse);
        state.acceptPolicy(
          SessionAccess(
            serverTime: base.add(async.elapsed),
            idleEnabled: false,
            idleDeadline: null,
            idleTimeout: const Duration(minutes: 30),
            warning: const Duration(minutes: 1),
          ),
        );
        expect(state.idleEnabled, isFalse);
        expect(state.reason, isNull);
        state.dispose();
      });
    },
  );
  test(
    'no human input warns and expires even with media and renewal continuing',
    () {
      fakeAsync((async) {
        final base = DateTime.utc(2026, 11, 5);
        var sent = 0, media = 0;
        final ended = <String>[];
        final state = SessionActivity(clock: () => base.add(async.elapsed));
        state.start(
          SessionAccess(
            serverTime: base,
            idleTimeout: const Duration(minutes: 30),
            warning: const Duration(minutes: 1),
            idleDeadline: base.add(const Duration(minutes: 30)),
          ),
          record: () async {
            sent++;
            return policyAt(base.add(async.elapsed));
          },
          onTerminal: ended.add,
        );
        final incoming = Timer.periodic(
          const Duration(seconds: 1),
          (_) => media++,
        );
        async.elapse(const Duration(minutes: 29));
        expect(state.warning, isTrue);
        expect(sent, 0);
        async.elapse(const Duration(minutes: 1));
        expect(media, 1800);
        expect(ended, ['session_idle_timeout']);
        state.human();
        expect(sent, 0, reason: 'post-expiry input cannot revive the owner');
        incoming.cancel();
        state.dispose();
      });
    },
  );

  test('human notices throttle and are never generated by a timer without new input', () {
    fakeAsync((async) {
      final base = DateTime.utc(2026, 11, 5);
      var sent = 0;
      final state = SessionActivity(clock: () => base.add(async.elapsed));
      state.start(
        SessionAccess(
          serverTime: base,
          idleTimeout: const Duration(minutes: 30),
          warning: const Duration(minutes: 1),
          idleDeadline: base.add(const Duration(minutes: 30)),
        ),
        record: () async {
          sent++;
          return policyAt(base.add(async.elapsed));
        },
        onTerminal: (_) {},
      );
      state.human();
      async.flushMicrotasks();
      for (var i = 0; i < 50; i++) {
        state.human();
      }
      async.elapse(const Duration(seconds: 4));
      expect(sent, 1);
      async.elapse(const Duration(seconds: 1));
      expect(sent, 2);
      async.elapse(const Duration(minutes: 5));
      expect(sent, 2);
      state.dispose();
    });
  });

  test(
    'resume checks server deadline before counting its first human event',
    () {
      fakeAsync((async) {
        var now = DateTime.utc(2026, 11, 5);
        var sent = 0;
        final state = SessionActivity(clock: () => now);
        state.start(
          SessionAccess(
            serverTime: now,
            idleTimeout: const Duration(minutes: 30),
            warning: const Duration(minutes: 1),
            idleDeadline: now.add(const Duration(minutes: 30)),
          ),
          record: () async {
            sent++;
            return policyAt(now);
          },
          onTerminal: (_) {},
        );
        now = now.add(
          const Duration(minutes: 31),
        ); // Suspended timers have not run.
        state.human();
        async.flushMicrotasks();
        expect(state.reason, 'session_idle_timeout');
        expect(sent, 0);
        state.dispose();
      });
    },
  );

  test('late activity reply cannot extend or retire a future login owner', () {
    fakeAsync((async) {
      final base = DateTime.utc(2026, 11, 5);
      final late = Completer<SessionAccess>();
      final ended = <String>[];
      final state = SessionActivity(clock: () => base.add(async.elapsed));
      SessionAccess policy() => SessionAccess(
        serverTime: base.add(async.elapsed),
        idleTimeout: const Duration(minutes: 30),
        warning: const Duration(minutes: 1),
        idleDeadline: base.add(async.elapsed).add(const Duration(minutes: 30)),
      );
      state.start(policy(), record: () => late.future, onTerminal: ended.add);
      state.human();
      async.elapse(const Duration(minutes: 1));
      state.start(
        policy(),
        record: () async => policy(),
        onTerminal: ended.add,
      );
      final fresh = state.policy!.idleDeadline;
      late.completeError(const BrokerException('pc_login_replaced', 401));
      async.flushMicrotasks();
      expect(state.reason, isNull);
      expect(state.policy!.idleDeadline, fresh);
      expect(ended, isEmpty);
      state.dispose();
    });
  });

  test('terminal reply keeps exact idle/replaced reason and cleanup acknowledgment gate', () {
    fakeAsync((async) {
      final base = DateTime.utc(2026, 11, 5);
      final ended = <String>[];
      final state = SessionActivity(clock: () => base);
      state.start(
        SessionAccess(
          serverTime: base,
          idleTimeout: const Duration(minutes: 30),
          warning: const Duration(minutes: 1),
          idleDeadline: base.add(const Duration(minutes: 30)),
        ),
        record: () async =>
            throw const BrokerException('pc_login_replaced', 401),
        onTerminal: ended.add,
      );
      state.human();
      async.flushMicrotasks();
      expect(ended, ['pc_login_replaced']);
      state.acknowledge();
      expect(state.reason, 'pc_login_replaced');
      state.finishRetirement(verified: false);
      state.acknowledge();
      expect(state.reason, 'pc_login_replaced');
      state.finishRetirement(verified: true);
      state.acknowledge();
      expect(state.reason, isNull);
      state.dispose();
    });
  });

  test('cursor-echo suppression is released only by its exact native pointer owner', () {
    final old = Object(), fresh = Object();
    SessionHumanSource.relative(old, true);
    SessionHumanSource.relative(fresh, true);
    SessionHumanSource.relative(old, false);
    expect(SessionHumanSource.nativeRelativePointer, isTrue);
    SessionHumanSource.relative(fresh, false);
    expect(SessionHumanSource.nativeRelativePointer, isFalse);
  });
}
