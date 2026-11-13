import 'dart:async';
import 'dart:convert';
import 'dart:io';
import 'dart:math';

import 'product_log.dart';
import '../l10n/l10n.dart';

Duration hostSettingsRequestTimeout(String action) => switch (action) {
  // The helper ends a WinFsp installation that runs longer than 10 minutes
  // itself (setup_winfsp.hpp), three attempts at most while another
  // installation is running; this observer waits a little longer than one.
  'installWinfsp' => const Duration(minutes: 15),
  // Driver installation (DiInstallDevice) and the verified service restart.
  'installNativeDisplay' => const Duration(minutes: 8),
  // Device removal and driver installation (DIF_REMOVE, DiInstallDevice).
  'replaceUsbController' ||
  'cleanDisplayAdapters' => const Duration(minutes: 5),
  'display' => const Duration(seconds: 210),
  'repairDisplay' => const Duration(seconds: 180),
  'requestConnection' => const Duration(seconds: 90),
  // Native display recovery may finish a 75-second driver rollback.
  'recoverDisplay' ||
  'elevate' ||
  'restartServiceV1' => const Duration(seconds: 120),
  // Service checks + console verification + record backup are each bounded.
  'resetNativeRecovery' => const Duration(seconds: 90),
  'enroll' || 'service' => const Duration(seconds: 60),
  _ => const Duration(seconds: 35),
};

/// settings-progress-v1 (apps/windows_host/src/settings_progress.hpp): what a
/// long request is doing while it runs. [step] is a fixed code the window
/// has a sentence for. [done] of [total] are there only when the work has a
/// real measure (Windows Installer's own count while WinFsp installs); a step
/// without them has no known length and is shown as such.
class HostSettingsProgress {
  const HostSettingsProgress(this.action, this.step, {this.done, this.total});
  final String action, step;
  final int? done, total;

  /// 0..1 when measured, else null.
  double? get fraction {
    final done = this.done, total = this.total;
    if (done == null || total == null || total <= 0) return null;
    return (done / total).clamp(0.0, 1.0).toDouble();
  }

  static final _code = RegExp(r'^[a-z0-9_]{1,48}$');

  /// One stdout line of the helper, or null when it is not a progress line
  /// (then it belongs to the reply).
  static HostSettingsProgress? parseLine(String action, String line) {
    if (!line.startsWith('{"progress":')) return null;
    Object? decoded;
    try {
      decoded = jsonDecode(line);
    } on FormatException {
      return null;
    }
    if (decoded is! Map || decoded.length != 1) return null;
    final value = decoded['progress'];
    if (value is! Map) return null;
    final step = value['step'];
    if (step is! String || !_code.hasMatch(step)) return null;
    final done = value['done'], total = value['total'];
    final measured =
        done is int && total is int && total > 0 && done >= 0 && done <= total;
    return HostSettingsProgress(
      action,
      step,
      done: measured ? done : null,
      total: measured ? total : null,
    );
  }
}

/// A settings backend that tells what its running request is doing. The
/// window shows the step when there is one and its own sentence otherwise;
/// a backend without it (a test double, an older helper) changes nothing.
abstract interface class HostSettingsProgressSource {
  Stream<HostSettingsProgress> get progress;
}

abstract class HostSettingsApi {
  factory HostSettingsApi.windows() = NativeHostSettingsApi;
  Future<Map<String, dynamic>> request(
    String action, [
    Map<String, dynamic> fields = const {},
  ]);
}

/// One native protected-record snapshot. This is distinct from Windows
/// automatic-login state and never contains a password.
class SavedWindowsLoginStatus {
  const SavedWindowsLoginStatus({
    this.configured = false,
    this.username = '',
    this.revision,
  });
  final bool configured;
  final String username;
  final String? revision;
  static const protocol = 'windows-login-clear-v1';
  factory SavedWindowsLoginStatus.parse(Object? value) {
    if (value is! Map) return const SavedWindowsLoginStatus();
    final username = value['username'];
    final revision = value['clearRevision'];
    return SavedWindowsLoginStatus(
      configured: value['configured'] == true,
      username:
          username is String && username.isNotEmpty && username.length <= 256
          ? username
          : '',
      revision:
          value['clearProtocol'] == protocol &&
              revision is String &&
              RegExp(r'^[a-f0-9]{64}$').hasMatch(revision)
          ? revision
          : null,
    );
  }
  bool get canClear => configured && username.isNotEmpty && revision != null;
}

bool windowsLoginRemovalObserved(Map<String, dynamic> response) {
  final receipt = response['clear'];
  return response['ok'] == true &&
      receipt is Map &&
      receipt.length == 2 &&
      receipt['protocol'] == SavedWindowsLoginStatus.protocol &&
      receipt['outcome'] == 'removed';
}

/// Host-local only. These values never travel through BrokerApi or session IPC.
class WindowsAutologonStatus {
  const WindowsAutologonStatus({
    this.supported = false,
    this.administrator = false,
    this.state = 'unknown',
    this.reason = 'backend_unavailable',
    this.revision,
    this.accountLabel = '',
    this.loggedIn = false,
    this.observedAt,
    this.operationId,
    this.operationState,
    this.supportsTakeOwnership = false,
    this.enabled,
  });
  final bool supported, administrator, loggedIn;
  final bool supportsTakeOwnership;
  final bool? enabled;
  final String state, reason, accountLabel;
  final String? revision, observedAt, operationId, operationState;
  static final revisionPattern = RegExp(r'^[a-f0-9]{64}$');
  static final uuidPattern = RegExp(
    r'^[a-f0-9]{8}-[a-f0-9]{4}-[a-f0-9]{4}-[a-f0-9]{4}-[a-f0-9]{12}$',
  );
  static const states = {
    'off',
    'configured',
    'observed',
    'needs_update',
    'external',
    'policy_blocked',
    'unknown',
    'failed',
  };
  factory WindowsAutologonStatus.parse(Object? value) {
    if (value is! Map ||
        value['schema'] != 1 ||
        value['supported'] is! bool ||
        value['administrator'] is! bool ||
        !states.contains(value['state'])) {
      return const WindowsAutologonStatus();
    }
    final revision = value['revision'];
    final label = value['accountLabel'];
    final reason = value['reason'];
    final operation = value['operation'];
    final id = operation is Map ? operation['id'] : null;
    final operationState = operation is Map ? operation['state'] : null;
    return WindowsAutologonStatus(
      supported: value['supported'] == true,
      administrator: value['administrator'] == true,
      state: value['state'] as String,
      revision: revision is String && revisionPattern.hasMatch(revision)
          ? revision
          : null,
      reason: reason is String && RegExp(r'^[a-z_]{1,48}$').hasMatch(reason)
          ? reason
          : 'state_unavailable',
      accountLabel: label is String && label.length <= 256 ? label : '',
      loggedIn: value['loggedIn'] == true,
      supportsTakeOwnership: value['supportsTakeOwnership'] == true,
      enabled: value['enabled'] is bool ? value['enabled'] as bool : null,
      observedAt:
          value['observedAt'] is String &&
              DateTime.tryParse(value['observedAt'] as String)?.isUtc == true
          ? value['observedAt'] as String
          : null,
      operationId: id is String && uuidPattern.hasMatch(id) ? id : null,
      operationState:
          const {
            'configured',
            'disabled',
            'partial',
            'unknown',
          }.contains(operationState)
          ? operationState as String
          : null,
    );
  }
  bool get canApply =>
      supported &&
      administrator &&
      revision != null &&
      const {'off', 'configured', 'observed', 'needs_update'}.contains(state);
  bool get canDisable =>
      supported &&
      administrator &&
      revision != null &&
      (const {'configured', 'observed', 'needs_update'}.contains(state) ||
          (state == 'policy_blocked' && enabled == true));
  bool get canTakeOwnership =>
      supported &&
      administrator &&
      revision != null &&
      supportsTakeOwnership &&
      state == 'external';
  bool get canTakeOwnershipApply =>
      canTakeOwnership && reason != 'policy_restricted';
  bool get canRepair =>
      supported &&
      administrator &&
      revision != null &&
      state == 'failed' &&
      reason == 'repair_required';
}

String newWindowsAutologonOperationId() {
  final random = Random.secure();
  final bytes = List<int>.generate(16, (_) => random.nextInt(256));
  bytes[6] = (bytes[6] & 15) | 64;
  bytes[8] = (bytes[8] & 63) | 128;
  final hex = bytes.map((v) => v.toRadixString(16).padLeft(2, '0')).join();
  return '${hex.substring(0, 8)}-${hex.substring(8, 12)}-${hex.substring(12, 16)}-${hex.substring(16, 20)}-${hex.substring(20)}';
}

class HostSettingsException implements Exception {
  const HostSettingsException(this.message, {this.code});
  final String message;
  final String? code;
  @override
  String toString() => message;
}

String hostTransportFailure(String value) {
  // Existing enrollment keeps its typed HTTP exception; translate only these fixed codes.
  final denied = RegExp(
    r'^Enrollment HTTP [0-9]{3}: (http_disabled|http_not_allowed|origin_not_allowed|unregistered_origin)$',
  ).firstMatch(value)?.group(1);
  final code = switch (denied) {
    'http_disabled' || 'http_not_allowed' => 'broker_http_disabled',
    'origin_not_allowed' ||
    'unregistered_origin' => 'broker_origin_not_allowed',
    _ => value,
  };
  return switch (code) {
    'windows_login_record_changed' =>
      l10n.savedInfoHasChangedGone,
    'windows_login_clear_unverified' =>
      l10n.couldntConfirmDeletionDontRun,
    'windows_login_clear_update_required' => l10n.updateHostClearSavedInfo,
    'host_service_restart_unverified' =>
      l10n.couldntConfirmServiceRestartedDont,
    'connection_request_changed' => l10n.connectionRequestChangedCheckCurrent,
    'invalid_broker_origin' => l10n.checkServerAddressPort1,
    'http_consent_required' => l10n.agreeHttpServerAddressFirst2,
    'broker_transport_profile_mismatch' =>
      l10n.currentAddressDoesntMatchServers,
    'broker_http_unsupported' =>
      l10n.cantConfirmHttpSupportServer,
    'broker_http_disabled' => l10n.httpConnectionsOffServerAsk,
    'broker_origin_not_allowed' => l10n.addressIsntServersAllowedList,
    'broker_setup_only' => l10n.addressOnlyFirstSetupSet,
    'broker_tls_failed' => l10n.couldntConnectSecurelyOverHttps2,
    'broker_connection_failed' => l10n.couldntConnectAddressPortCheck,
    'broker_timeout' => l10n.serverDidntRespondTimeCheck2,
    'broker_dns_failed' => l10n.couldntFindServerAddressCheck,
    'broker_network_failed' => l10n.couldntReachServerBecauseNetwork,
    'broker_health_failed' ||
    'broker_health_too_large' ||
    'broker_incompatible' => l10n.couldntVerifyMultitapServerResponse,
    _ => value,
  };
}

typedef HostSettingsProcessStarter = Future<Process> Function(
  String executable,
);

class NativeHostSettingsApi
    implements HostSettingsApi, HostSettingsProgressSource {
  NativeHostSettingsApi({
    String? executable,
    HostSettingsProcessStarter? processStarter,
  }) : executable =
           executable ??
           '${File(Platform.resolvedExecutable).parent.path}${Platform.pathSeparator}MultitapHostTool.exe',
       _startProcess = processStarter ?? _start;
  final String executable;
  final HostSettingsProcessStarter _startProcess;
  static Future<Process> _start(String executable) => Process.start(
    executable,
    const ['--settings-rpc-stdin'],
    environment: hostToolEnvironment(),
    runInShell: false,
    mode: ProcessStartMode.normal,
  );

  /// Added to the helper's inherited environment: MULTITAP_LANG=ko|en, the
  /// app's language (자동 resolved as the app does), so the helper's own
  /// error texts match the screen; MULTITAP_SETTINGS_PROGRESS=1, so it
  /// writes a line for each step of a long request before its reply.
  static Map<String, String> hostToolEnvironment() => {
    'MULTITAP_LANG': AppLanguageController.instance.code,
    'MULTITAP_SETTINGS_PROGRESS': '1',
  };

  final _progress = StreamController<HostSettingsProgress>.broadcast();
  @override
  Stream<HostSettingsProgress> get progress => _progress.stream;

  @override
  Future<Map<String, dynamic>> request(
    String action, [
    Map<String, dynamic> fields = const {},
  ]) async {
    const actions = [
      'status',
      'testBroker',
      'requestConnection',
      'cancelConnection',
      'display',
      'recoverDisplay',
      'repairDisplay',
      '', // unused; kept so later log codes keep their numbers
      'service',
      // The shared Windows account (historic action names).
      'saveRdpCredentials',
      'clearRdpCredentials',
      'logs',
      'displayRepairPlan',
      'consoleTransfer',
      'resetNativeRecovery',
      'windowsAutologonStatus',
      'windowsAutologonApply',
      'windowsAutologonDisable',
      'windowsAutologonRepair',
      'encoderSettings',
      'applyEncoderSettings',
      // Append only: the log records an action by its position.
      'installWinfsp',
      'startPrintSpooler',
      'cleanDisplayAdapters',
      'replaceUsbController',
      'windowsAccountPasswordState',
      'setWindowsAccountPassword',
      'cancelConnectionV1',
      'restartServiceV1',
      'clearWindowsLoginV1',
      '', // four unused slots, as above
      '',
      '',
      '',
      // The finish of a first install (host_prerequisites.dart).
      'prerequisiteStatus',
      'installNativeDisplay',
    ];
    final code = actions.indexOf(action);
    try {
      final result = await _request(action, fields);
      if (action == 'status') {
        final state = result['status'];
        if (state is Map && state['display'] is Map) {
          final d = state['display'] as Map;
          final values = <String, dynamic>{
            'installed': d['installed'],
            'started': d['deviceReady'],
            'active': d['activeDisplay'],
            'owned': d['owned'],
            'pending': d['configurationPending'],
            'recovery': d['recoveryRequired'],
            'reboot': d['rebootRequired'],
            'remote': d['remoteSession'],
            'monitors': d['monitors'] is List
                ? (d['monitors'] as List).length
                : 0,
            'ok': d['ready'],
          };
          final fingerprint = jsonEncode(ProductLogEntry.safeValues(values));
          if (fingerprint != _displayFingerprint) {
            _displayFingerprint = fingerprint;
            ProductLog.instance.record(LogEvent.displayObserved, values);
          }
        }
      } else if (action != 'logs' && action != 'prerequisiteStatus') {
        ProductLog.instance.record(LogEvent.hostAction, {
          'action': code,
          'ok': true,
        });
      }
      return result;
    } catch (_) {
      if (action != 'logs' && action != 'prerequisiteStatus') {
        ProductLog.instance.record(LogEvent.hostAction, {
          'action': code,
          'ok': false,
        });
      }
      rethrow;
    }
  }

  String? _displayFingerprint;
  Future<Map<String, dynamic>> _request(
    String action,
    Map<String, dynamic> fields,
  ) async {
    Process? child;
    try {
      final process = await _startProcess(executable);
      child = process;
      // Line by line: a progress line goes to the window at once, every
      // other line is the reply (one line; kept whole whatever it holds).
      final output = process.stdout
          .transform(utf8.decoder)
          .transform(const LineSplitter())
          .fold<String>('', (value, line) {
            final step = HostSettingsProgress.parseLine(action, line);
            if (step != null) {
              if (!_progress.isClosed) _progress.add(step);
              return value;
            }
            if (value.length + line.length > 131072) {
              throw HostSettingsException(l10n.couldntReadHostSettingsResponse);
            }
            return value.isEmpty ? line : '$value\n$line';
          });
      final errors = process.stderr.drain<void>();
      // Enrollment and Windows account fields travel through stdin only.
      process.stdin.write(jsonEncode({'action': action, ...fields}));
      await process.stdin.close();
      final timeout = hostSettingsRequestTimeout(action);
      final results = await Future.wait<Object?>([
        output,
        errors,
        process.exitCode,
      ]).timeout(timeout);
      final result = results[0] as String;
      final exitCode = results[2] as int;
      final decoded = jsonDecode(result);
      if (decoded is! Map<String, dynamic>) {
        throw const FormatException('Invalid settings response');
      }
      if (decoded['ok'] != true || exitCode != 0) {
        final message = decoded['error'];
        throw HostSettingsException(
          message is String && message.length < 1500
              ? hostTransportFailure(message)
              : l10n.couldntFinishSetupRefreshStatus,
          code:
              const {
                'host_service_restart_unverified',
                'connection_request_changed',
                'windows_login_record_changed',
                'windows_login_clear_unverified',
                'windows_login_clear_update_required',
              }.contains(message)
              ? message as String
              : null,
        );
      }
      return decoded;
    } on TimeoutException {
      if (const {
        'windowsAutologonApply',
        'windowsAutologonDisable',
        'windowsAutologonRepair',
        'applyEncoderSettings',
        'installWinfsp',
        'installNativeDisplay',
        'replaceUsbController',
        'cleanDisplayAdapters',
        'setWindowsAccountPassword',
        'cancelConnectionV1',
        'restartServiceV1',
        'clearWindowsLoginV1',
      }.contains(action)) {
        // An observer deadline is not proof that an LSA/registry mutation ended.
        // Keep the owned RPC process/its exclusive native lock until actual exit.
        // Reconciliation is GET-only; the UI retains this operation UUID.
        // (msiexec keeps running whatever happens to its RPC process.)
        throw HostSettingsException(switch (action) {
          'clearWindowsLoginV1' =>
            l10n.deletionIsntConfirmedYetDont,
          'restartServiceV1' =>
            l10n.serviceRestartIsntConfirmedYet,
          'cancelConnectionV1' =>
            l10n.requestCancellationIsntConfirmedYet,
          'applyEncoderSettings' =>
            l10n.encoderSaveIsntConfirmedYet,
          'installWinfsp' =>
            l10n.winfspInstallIsntConfirmedYet,
          'installNativeDisplay' =>
            l10n.virtualDisplayDriverInstallIsnt,
          'replaceUsbController' || 'cleanDisplayAdapters' =>
            l10n.deviceChangeIsntConfirmedYet,
          'setWindowsAccountPassword' => l10n.passwordResultIsntConfirmedYet,
          _ => l10n.autoSignRequestResultIsnt,
        });
      }
      child?.kill(ProcessSignal.sigkill);
      try {
        await child?.exitCode.timeout(const Duration(seconds: 5));
      } catch (_) {
        /* The bounded RPC process was already asked to stop. */
      }
      throw HostSettingsException(
        l10n.settingsResponseTimedOutRefresh,
      );
    } on HostSettingsException {
      rethrow;
    } on ProcessException {
      throw HostSettingsException(
        l10n.couldntFindHostConfigurationFile,
      );
    } on FormatException {
      throw HostSettingsException(l10n.couldntReadHostSettingsResponse);
    }
  }
}
