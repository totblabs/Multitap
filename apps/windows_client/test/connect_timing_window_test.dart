import 'dart:async';
import 'dart:io';

import 'package:flutter/material.dart';
import 'package:flutter/services.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:multitap_client/data/broker_api.dart';
import 'package:multitap_client/data/remote_session.dart';
import 'package:multitap_client/data/session_windows.dart';
import 'package:multitap_client/data/window_close_guard.dart';
import 'package:multitap_client/models/session_display_mode.dart';
import 'package:multitap_client/models/stream_options.dart';
import 'package:multitap_client/ui/remote_view.dart';
import 'package:multitap_client/ui/session_window.dart';

import 'display_remote_test.dart' show machine;
import 'remote_session_test.dart' show SessionMedia;
import 'session_recovery_test.dart' show mockPresentation;
import 'session_window_lifecycle_test.dart' show desktop, launch;

class _Guard implements WindowCloseGuard {
  @override
  void attach(Future<void> Function() value) {}
  @override
  Future<bool> update({required bool busy, required bool dirty}) async => true;
  @override
  Future<bool> close() async => true;
  @override
  void dispose() {}
}

class _Api extends Fake implements BrokerApi {
  bool grant = false;
  @override
  Future<List<BrokerDesktop>> desktops(String token) async => [desktop()];
  @override
  Future<BrokerConnection> connect(
    String token,
    String id, {
    String mode = 'stream',
    StreamOptions? options,
  }) async {
    // The view's own first attempt is refused; the test asks itself.
    if (!grant) throw const BrokerException('host_unavailable');
    return BrokerConnection(
      sessionId: 'timing-session',
      webSocketUrl: Uri.parse('wss://fixture.example/api/v1/sessions/x'),
      options: options ?? const StreamOptions(),
    );
  }

  @override
  void close() {}
}

void main() {
  TestWidgetsFlutterBinding.ensureInitialized();
  final messenger =
      TestDefaultBinaryMessengerBinding.instance.defaultBinaryMessenger;
  setUp(() {
    mockPresentation();
    messenger.setMockMethodCallHandler(
      SessionWindows.channel,
      (_) async => null,
    );
  });
  tearDown(
    () => messenger.setMockMethodCallHandler(SessionWindows.channel, null),
  );

  testWidgets('The window gives its launch and desktop-read times to its '
      'first session only', (t) async {
    final api = _Api();
    final requested = DateTime.now().subtract(const Duration(milliseconds: 250));
    await t.pumpWidget(
      MaterialApp(
        home: SessionWindow(
          launch: SessionWindowLaunch.fromMap({
            ...launch().toMap(),
            'requestedAtMs': requested.millisecondsSinceEpoch,
          }),
          api: api,
          guard: _Guard(),
        ),
      ),
    );
    await t.pump();
    await t.pump();
    final view = t.widget<RemoteView>(find.byType(RemoteView));
    api.grant = true;
    late RemoteSession first, second;
    await t.runAsync(() async {
      first = await view.onReconnect!(const StreamOptions());
      second = await view.onReconnect!(const StreamOptions());
    });
    expect(first.connectTiming.before['windowMs'], greaterThanOrEqualTo(250));
    expect(first.connectTiming.before['windowMs'], lessThan(60000));
    expect(first.connectTiming.before.containsKey('loadMs'), isTrue);
    expect(second.connectTiming.before, isEmpty, reason: 'a reconnect did not open the window');
    await t.pumpWidget(const SizedBox());
    await t.pump();
  });

  testWidgets('The view gives each preparation step and the connect request '
      'to its session', (t) async {
    for (final name in ['multitap/window', 'multitap/window/events']) {
      messenger.setMockMethodCallHandler(MethodChannel(name), (_) async => false);
    }
    addTearDown(() {
      for (final name in ['multitap/window', 'multitap/window/events']) {
        messenger.setMockMethodCallHandler(MethodChannel(name), null);
      }
    });
    RemoteSession? made;
    final media = SessionMedia();
    await t.pumpWidget(
      MaterialApp(
        home: RemoteView(
          machine: machine(1),
          prepareDisplaysOnEntry: true,
          initialDisplayMode: SessionDisplayMode.windowed,
          onRefreshDesktop: () async => machine(1),
          onConfigureDisplays: (_) async => machine(1),
          onReconnect: (options) async => made = RemoteSession(
            connection: BrokerConnection(
              sessionId: 'view-timing',
              webSocketUrl: Uri.parse('wss://fixture.example/api/v1/sessions/x'),
              options: options,
            ),
            token: 'fixture-token',
            media: media,
            // Never opens: nothing leaves this test.
            dialer: (_, _) => Completer<WebSocket>().future,
          ),
          load: () async => fail('A prepared entry uses the connect path'),
        ),
      ),
    );
    await t.pump();
    await t.pump(const Duration(milliseconds: 50));
    expect(made, isNotNull);
    expect(made!.connectTiming.before.keys.toSet(), {
      'retireMs', 'refreshMs', 'presentMs', 'outputsMs', 'viewportMs', 'connectMs',
    });
    await t.pumpWidget(const SizedBox());
    await t.pump();
    await media.events.close();
  });
}
