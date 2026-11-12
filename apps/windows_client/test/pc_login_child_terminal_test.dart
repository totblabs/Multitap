import 'dart:async';

import 'package:flutter/material.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:multitap_client/data/broker_api.dart';
import 'package:multitap_client/data/session_windows.dart';
import 'package:multitap_client/data/window_close_guard.dart';
import 'package:multitap_client/models/session_display_mode.dart';
import 'package:multitap_client/models/stream_options.dart';
import 'package:multitap_client/ui/session_window.dart';

import 'session_window_lifecycle_test.dart' show desktop;
import 'session_renewal_test.dart' show VirtualClock;
import 'session_recovery_test.dart' show mockPresentation;

class Guard implements WindowCloseGuard {
  int closes = 0;
  @override
  void attach(Future<void> Function() onRequest) {}
  @override
  Future<bool> update({required bool busy, required bool dirty}) async => true;
  @override
  Future<bool> close() async {
    closes++;
    return true;
  }

  @override
  void dispose() {}
}

class ChildApi extends Fake implements BrokerApi, SessionRenewalBrokerApi {
  ChildApi(this.clock);
  final VirtualClock clock;
  String? inventoryError;
  int loginCalls = 0, connects = 0;
  bool replaced = false;
  Completer<List<BrokerDesktop>>? inventory;
  @override
  Future<List<BrokerDesktop>> desktops(String token) async {
    if (inventoryError != null) throw BrokerException(inventoryError!, 401);
    return inventory == null ? [desktop()] : inventory!.future;
  }

  @override
  Future<BrokerSession> login(String n, String p) async {
    loginCalls++;
    throw StateError('child never signs in');
  }

  @override
  Future<BrokerConnection> connect(
    String token,
    String id, {
    String mode = 'stream',
    StreamOptions? options,
  }) async {
    connects++;
    throw const BrokerException('network_error');
  }

  @override
  Future<DateTime> renewSession(String token) async {
    if (replaced) throw const BrokerException('pc_login_replaced', 401);
    return clock.now.add(const Duration(minutes: 10));
  }

  @override
  void recordSessionRenewal(
    String token, {
    required int stage,
    required bool ok,
    String? code,
  }) {}
  @override
  void close() {}
}

SessionWindowLaunch launch(VirtualClock clock) => SessionWindowLaunch(
  origin: Uri.parse('https://fixture.example'),
  token: 'child-owner-token',
  expiresAt: clock.now.add(const Duration(minutes: 10)),
  desktopId: 'pve-149',
  name: 'TESTER',
  mode: SessionDisplayMode.windowed,
  options: const StreamOptions(),
  pcSessionPolicy: windowsPcSessionPolicy,
);

void main() {
  setUp(mockPresentation);
  testWidgets(
    'replaced child renew retires normally without authenticating and ignores late inventory',
    (t) async {
      final clock = VirtualClock();
      final api = ChildApi(clock)..inventory = Completer<List<BrokerDesktop>>();
      final guard = Guard();
      await t.pumpWidget(
        MaterialApp(
          home: SessionWindow(
            launch: launch(clock),
            api: api,
            guard: guard,
            clock: clock.call,
          ),
        ),
      );
      await t.pump();
      api.replaced = true;
      await clock.advance(t, const Duration(minutes: 5, seconds: 1));
      await t.pump();
      expect(guard.closes, 1);
      api.inventory!.complete([desktop()]);
      // The fake guard acknowledges close without destroying a native window.
      // Its still-mounted loading spinner is therefore intentionally animated.
      await t.pump(const Duration(milliseconds: 300));
      expect(api.connects, 0);
      expect(api.loginCalls, 0);
      await t.pumpWidget(const SizedBox.shrink());
    },
  );
  for (final error in ['pc_login_replaced', 'session_expired']) {
    testWidgets(
      'child inventory definitive authentication error $error retires its owner',
      (t) async {
        final clock = VirtualClock();
        final api = ChildApi(clock)..inventoryError = error;
        final guard = Guard();
        await t.pumpWidget(
          MaterialApp(
            home: SessionWindow(
              launch: launch(clock),
              api: api,
              guard: guard,
              clock: clock.call,
            ),
          ),
        );
        await t.pump(const Duration(milliseconds: 300));
        expect(guard.closes, 1);
        expect(api.loginCalls, 0);
        await t.pumpWidget(const SizedBox.shrink());
      },
    );
  }
}
