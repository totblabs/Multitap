// Isolated real-engine/real-HWND lifecycle regression. The reviewed operator
// copies this entrypoint to the existing validated session_windows_qa.dart path.
// QA keeps every window hidden: no broker/media, OS input, clipboard or vault.
import 'dart:convert';
import 'dart:io';

import 'package:flutter/material.dart';
import 'package:multitap_client/data/session_windows.dart';
import 'package:multitap_client/data/window_close_guard.dart';
import 'package:multitap_client/models/session_display_mode.dart';
import 'package:multitap_client/models/stream_options.dart';

const _owner = '0123456789abcdef0123456789abcdef';

Future<Map<Object?, Object?>> _snapshot() async =>
    (await SessionWindows.channel.invokeMapMethod<Object?, Object?>(
      'qaSnapshot',
    ))!;

Future<Map<Object?, Object?>> _wait(int children) async {
  for (var attempt = 0; attempt < 200; attempt++) {
    final state = await _snapshot();
    if (state['childWindows'] == children &&
        state['engineCount'] == children + 1 &&
        state['opening'] == false &&
        state['childFirstFrames'] == children) {
      return state;
    }
    await Future<void>.delayed(const Duration(milliseconds: 50));
  }
  throw StateError('native_lifecycle_wait_timeout');
}

Future<SessionWindowOpenResult?> _open() => SessionWindows.open(
  SessionWindowLaunch(
    origin: Uri.parse('https://fixture.invalid'),
    token: 'isolated-memory-only',
    expiresAt: DateTime.now().add(const Duration(minutes: 3)),
    authenticationOwner: _owner,
    desktopId: 'pve-149',
    name: 'Isolated first-open regression',
    mode: SessionDisplayMode.windowed,
    options: const StreamOptions(),
  ),
);

void _check(bool value, String code) {
  if (!value) throw StateError(code);
}

Future<void> main(List<String> arguments) async {
  WidgetsFlutterBinding.ensureInitialized();
  SessionWindows.listen();
  final launch = await SessionWindows.bootstrap();
  final guard = WindowCloseGuard.windows();
  Future<void> close() async {
    await guard.close();
  }
  guard.attach(close);
  await guard.update(busy: false, dirty: false);
  if (launch != null) {
    SessionWindows.bindRetirement(close);
    runApp(
      const MaterialApp(
        home: Scaffold(body: Text('Isolated native child first frame')),
      ),
    );
    await WidgetsBinding.instance.endOfFrame;
    await SessionWindows.channel.invokeMethod<void>('ready');
    return;
  }
  final output = arguments
      .singleWhere((argument) => argument.startsWith('--qa-output='))
      .substring('--qa-output='.length);
  runApp(
    const MaterialApp(
      home: Scaffold(body: Text('Isolated native first-open lifecycle')),
    ),
  );
  await WidgetsBinding.instance.endOfFrame;
  final report = <String, Object?>{
    'protocol': 'native-first-open-lifecycle-qa-v1',
    'actualWindowsEngines': true,
    'networkUsed': false,
    'vmChanged': false,
    'osInputSent': false,
    'visibleWindowsExpected': false,
    'physicalKeyboardVerified': false,
    'foregroundVerified': false,
    'processId': pid,
  };
  final cycles = <Map<String, Object?>>[];
  try {
    await SessionWindows.bindAuthentication(_owner, null);
    for (var cycle = 0; cycle < 2; cycle++) {
      final opened = await _open();
      _check(opened?.opened == true, 'first_open_must_create_child');
      final first = await _wait(1);
      final checks = {
        'freshAuthenticationAlive': first['childAuthenticationRetired'] == 0,
        'freshConsoleAcquireEligible': first['childConsoleAcquireEligible'] == 1,
        'firstFrameCompleted': first['childFirstFrames'] == 1,
        'actualShowGateEligible': first['childShowEligible'] == 1,
      };
      cycles.add({'cycle': cycle, 'firstOpen': first, 'checks': checks});
      _check(checks.values.every((value) => value), 'virgin_create_poisoned_owner');
      // Only after checking the first open, exercise the existing-window path.
      _check((await _open())?.opened == false, 'same_child_must_deduplicate');
      final reused = await _wait(1);
      _check(
        reused['childAuthenticationRetired'] == 0 &&
            reused['childConsoleAcquireEligible'] == 1 &&
            reused['childShowEligible'] == 1,
        'dedup_must_keep_same_live_owner',
      );
      await SessionWindows.closeAll('application_close');
      final closed = await _wait(0);
      _check(closed['dashboardAlive'] == true, 'close_must_keep_dashboard');
      cycles.last['closed'] = closed;
    }
    final beforePlacement = await _snapshot();
    final placement = <String, Object?>{'before': beforePlacement};
    report['updatePlacement'] = placement;
    _check(
      beforePlacement['parentFirstFrameCompleted'] == true,
      'parent_first_frame_must_already_be_complete',
    );
    final revealed = await SessionWindows.channel.invokeMethod<bool>(
      'qaRestoreUpdatePlacement',
    );
    final afterPlacement = await _snapshot();
    placement.addAll({'requestedShow': revealed, 'after': afterPlacement});
    _check(
      revealed == true &&
          afterPlacement['updateRevealRequests'] ==
              (beforePlacement['updateRevealRequests'] as int) + 1,
      'placement_after_first_frame_must_request_reveal',
    );
    await SessionWindows.hideAuthentication(_owner);
    final retiredReveal = await SessionWindows.channel.invokeMethod<bool>(
      'qaRestoreUpdatePlacement',
    );
    final afterRetirement = await _snapshot();
    placement.addAll({
      'retiredRequestedShow': retiredReveal,
      'afterRetirement': afterRetirement,
    });
    _check(
      retiredReveal == false &&
          afterRetirement['updateRevealRequests'] ==
              afterPlacement['updateRevealRequests'],
      'retired_authentication_must_not_request_reveal',
    );
    report['passed'] = true;
  } catch (error) {
    report['passed'] = false;
    report['failureType'] = error.runtimeType.toString();
    report['failure'] = error.toString();
  }
  report['cycles'] = cycles;
  // Keep this report even if subsequent native cleanup fails.
  await File(output).writeAsString(jsonEncode(report), flush: true);
  try {
    await SessionWindows.closeAll('application_close');
  } catch (_) {}
  await close();
}
