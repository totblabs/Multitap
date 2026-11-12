// Isolated AOT target. Never selected by the product build. It uses real
// Flutter engines/HWNDs, production views and native close guards; broker/media
// are intentionally unavailable fixtures. No credentials, network or VM calls.
import 'dart:async';
import 'dart:convert';
import 'dart:io';

import 'package:flutter/material.dart';
import 'package:multitap_client/data/appearance_settings.dart';
import 'package:multitap_client/data/broker_api.dart';
import 'package:multitap_client/data/product_log.dart';
import 'package:multitap_client/data/session_windows.dart';
import 'package:multitap_client/data/window_close_guard.dart';
import 'package:multitap_client/models/session_display_mode.dart';
import 'package:multitap_client/models/stream_options.dart';
import 'package:multitap_client/ui/session_window.dart';
import 'package:multitap_client/ui/themed_app.dart';

class QaApi implements BrokerApi {
  @override
  Future<List<BrokerDesktop>> desktops(String token) async => [
    const BrokerDesktop(
      id: 'pve-149',
      name: 'Isolated stream',
      kind: 'vm',
      description: '',
      os: 'Windows',
      status: 'ready',
      canConnect: true,
    ),
    const BrokerDesktop(
      id: 'pve-150',
      name: 'Isolated stream 2',
      kind: 'vm',
      description: '',
      os: 'Windows',
      status: 'ready',
      canConnect: true,
    ),
  ];
  @override
  Future<BrokerConnection> connect(
    String token,
    String desktopId, {
    String mode = 'stream',
    StreamOptions? options,
  }) async {
    throw const BrokerException('network_error');
  }

  @override
  void close() {}
  @override
  dynamic noSuchMethod(Invocation invocation) =>
      throw StateError('Unexpected broker call');
}

class QaGuard implements WindowCloseGuard {
  final native = WindowCloseGuard.windows();
  final BuildContext Function() context;
  QaGuard(this.context);
  int requested = 0;
  @override
  void attach(Future<void> Function() request) {
    native.attach(() async {
      final attempt = ++requested;
      final done = request();
      await Future<void>.delayed(const Duration(milliseconds: 450));
      // Cancel the first X; accept the second. This exercises the actual view
      // confirmation route without simulated keyboard/mouse input.
      Navigator.of(context()).pop(attempt > 1);
      await done;
    });
  }

  @override
  Future<bool> update({required bool busy, required bool dirty}) =>
      native.update(busy: busy, dirty: dirty);
  @override
  Future<bool> close() => native.close();
  @override
  void dispose() => native.dispose();
}

class QaChild extends StatefulWidget {
  const QaChild({super.key, required this.launch});
  final SessionWindowLaunch launch;
  @override
  State<QaChild> createState() => _QaChildState();
}

class _QaChildState extends State<QaChild> {
  late final guard = QaGuard(() => context);
  @override
  Widget build(BuildContext context) =>
      SessionWindow(launch: widget.launch, api: QaApi(), guard: guard);
}

void check(bool value, String reason) {
  if (!value) throw StateError(reason);
}

Future<Map<Object?, Object?>> snapshot() async => (await SessionWindows.channel
    .invokeMapMethod<Object?, Object?>('qaSnapshot'))!;
Future<Map<Object?, Object?>> waitCount(int count) async {
  for (var i = 0; i < 160; i++) {
    final value = await snapshot();
    if (value['childWindows'] == count &&
        value['engineCount'] == count + 1 &&
        value['opening'] == false) {
      return value;
    }
    await Future<void>.delayed(const Duration(milliseconds: 100));
  }
  throw StateError('window_count_timeout_$count');
}

Future<Map<Object?, Object?>> open(int id) async {
  final launch = SessionWindowLaunch(
    origin: Uri.parse('https://fixture.invalid'),
    token: 'isolated-memory-only',
    expiresAt: DateTime.now().add(const Duration(minutes: 3)),
    desktopId: 'pve-$id',
    name: 'Isolated stream $id',
    mode: SessionDisplayMode.windowed,
    options: const StreamOptions(),
  );
  return (await SessionWindows.channel.invokeMapMethod<Object?, Object?>(
    'open',
    {
      'key': 'qa-$id',
      'title': 'Multitap isolated session QA',
      'payload': launch.toMap(),
    },
  ))!;
}

Future<void> main(List<String> args) async {
  WidgetsFlutterBinding.ensureInitialized();
  SessionWindows.listen();
  final launch = await SessionWindows.bootstrap();
  final appearance = AppAppearance();
  if (launch != null) {
    runApp(
      ThemedApp(
        appearance: appearance,
        home: QaChild(launch: launch),
      ),
    );
    await SessionWindows.channel.invokeMethod<void>(
      'log',
      ProductLogEntry(DateTime.now(), LogEvent.streamStart, {
        'ok': true,
      }).toJson(),
    );
    return;
  }
  final output = args
      .singleWhere((arg) => arg.startsWith('--qa-output='))
      .substring('--qa-output='.length);
  runApp(
    ThemedApp(
      appearance: appearance,
      home: const Scaffold(body: Text('Isolated window lifecycle QA')),
    ),
  );
  await WidgetsBinding.instance.endOfFrame;
  final report = <String, Object?>{
    'networkUsed': false,
    'vmChanged': false,
    'actualRemoteDesktop': false,
    'actualWindowsEngines': true,
    'processId': pid,
  };
  try {
    check((await open(149))['opened'] == true, 'stream_open');
    check((await open(150))['opened'] == true, 'second_open');
    final initial = await waitCount(2);
    check(
      initial['processId'] == pid && initial['dashboardAlive'] == true,
      'single_process',
    );
    check((await open(149))['opened'] == false, 'dedup');
    await Future<void>.delayed(const Duration(seconds: 2));
    final forwardedLogs = ProductLog.instance.entries.where(
      (entry) => entry.event == LogEvent.streamStart && entry.values['ok'] == true,
    ).length;
    check(forwardedLogs >= 2, 'child_logs_reach_dashboard');
    await SessionWindows.channel.invokeMethod<void>('qaRequestClose', {
      'key': 'qa-149',
    });
    await Future<void>.delayed(const Duration(seconds: 2));
    check((await snapshot())['childWindows'] == 2, 'cancel_preserves_child');
    await SessionWindows.channel.invokeMethod<void>('qaRequestClose', {
      'key': 'qa-149',
    });
    check(
      (await waitCount(1))['dashboardAlive'] == true,
      'accept_keeps_dashboard',
    );
    await SessionWindows.closeAll('logout');
    check(
      (await waitCount(0))['dashboardAlive'] == true,
      'logout_keeps_dashboard',
    );
    for (var i = 0; i < 3; i++) {
      await open(149);
      await open(150);
      await SessionWindows.closeAll(
        'update',
      ); // includes children bootstrapping.
      await waitCount(0);
    }
    final finalSnapshot = await snapshot();
    check(
      (finalSnapshot['teardownChecks'] as int) >= 8 &&
          (finalSnapshot['teardownDrainChecks'] as int) >= 7 &&
          finalSnapshot['teardownFailures'] == 0,
      'nested_teardown_drain_and_reopen_guard',
    );
    report.addAll({
      'passed': true,
      'deduplicated': true,
      'forwardedChildLogs': forwardedLogs,
      'cancelPreservedChild': true,
      'acceptedClosePreservedDashboard': true,
      'globalCloseCycles': 4,
      'finalSnapshot': finalSnapshot,
    });
  } catch (error) {
    report.addAll({
      'passed': false,
      'failureType': error.runtimeType.toString(),
      'failure': error.toString(),
    });
  }
  await File(output).writeAsString(jsonEncode(report), flush: true);
  try {
    await SessionWindows.closeAll('application_close');
  } catch (_) {}
  await const MethodChannelShim().close();
}

class MethodChannelShim {
  const MethodChannelShim();
  Future<void> close() async {
    final guard = WindowCloseGuard.windows()..attach(() async {});
    await guard.close();
  }
}
