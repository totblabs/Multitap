
/// Remote Host program update (broker host-update-v1) for any account the VM
/// is assigned to; released brokers send it to administrators only, and a
/// broker without it leaves [HostUpdateState.absent]. The view is untrusted
/// display data: it never grants connection readiness, and every request is
/// authorized again by the broker (latest release only from the client).
library;

import '../l10n/l10n.dart';

const hostUpdateProtocol = 'host-update-v1';

final _version = RegExp(r'^[0-9]{1,6}\.[0-9]{1,6}\.[0-9]{1,6}$');
bool _validVersion(String value) =>
    value.length <= 20 && _version.stringMatch(value) == value;
const _activeStates = {
  'waiting',
  'dispatched',
  'accepted',
  'downloading',
  'verifying',
  'installing',
};
const _knownStates = {
  ..._activeStates,
  'completed',
  'failed',
  'rejected',
  'unknown',
  'expired',
};

class HostUpdateOperation {
  const HostUpdateOperation({
    required this.status,
    required this.targetVersion,
    this.code = '',
    this.progress,
    this.rebootRequired = false,
    this.checkedAt,
  });
  final String status, targetVersion, code;
  final int? progress;
  final bool rebootRequired;
  final DateTime? checkedAt;
  bool get active => _activeStates.contains(status);

  static HostUpdateOperation? fromJson(dynamic data) {
    if (data is! Map) return null;
    final status = data['status'], target = data['targetVersion'];
    final code = data['code'], progress = data['progress'];
    if (status is! String ||
        !_knownStates.contains(status) ||
        target is! String ||
        !_validVersion(target) ||
        (code != null && (code is! String || code.length > 48)) ||
        (progress != null &&
            (progress is! int || progress < 0 || progress > 100))) {
      return null;
    }
    final checked = data['checkedAt'];
    return HostUpdateOperation(
      status: status,
      targetVersion: target,
      code: code as String? ?? '',
      progress: progress as int?,
      rebootRequired: data['rebootRequired'] == true,
      checkedAt: checked is String ? DateTime.tryParse(checked) : null,
    );
  }
}

class HostUpdateState {
  const HostUpdateState({
    this.present = false,
    this.connected = false,
    this.supported = false,
    this.version,
    this.latestVersion,
    this.newer = false,
    this.available = false,
    this.sessionActive = false,
    this.operation,
  });
  static const absent = HostUpdateState();

  /// True only when the broker sent an administrator view for this desktop.
  final bool present;
  final bool connected, supported, newer, available, sessionActive;
  final String? version, latestVersion;
  final HostUpdateOperation? operation;

  factory HostUpdateState.fromJson(dynamic data) {
    if (data is! Map || data['protocol'] != hostUpdateProtocol) return absent;
    String? version(dynamic value) =>
        value is String && _validVersion(value) ? value : null;
    return HostUpdateState(
      present: true,
      connected: data['connected'] == true,
      supported: data['supported'] == true,
      version: version(data['version']),
      latestVersion: version(data['latestVersion']),
      newer: data['newer'] == true,
      available: data['available'] == true,
      sessionActive: data['sessionActive'] == true,
      operation: HostUpdateOperation.fromJson(data['operation']),
    );
  }

  bool get busy => operation?.active == true;

  /// The ⋯ menu offers an update only for an administrator view of a
  /// connected Host that supports remote updates and has a newer release.
  bool get offered =>
      present &&
      connected &&
      supported &&
      newer &&
      available &&
      !busy &&
      version != null &&
      latestVersion != null;

  /// The Windows ⋯ menu row, listed only while [offered] (2026-10-26: a
  /// greyed "최신 버전" row read as a broken button).
  String get menuLabel => l10n.hostUpdate2('$version', '$latestVersion');

  /// The Android launcher's menu line for any Host view: the update when
  /// [offered], otherwise why there is nothing to do (shown disabled). Null
  /// without a Host update view (released broker).
  String? get menuText {
    if (!present) return null;
    if (offered) return menuLabel;
    if (busy) return l10n.hostUpdating;
    if (!connected) return l10n.hostUpdateHostOffline;
    if (version == null) return l10n.hostUpdateCheckingVersion;
    if (!supported) return l10n.hostRemoteUpdateNotSupported('$version');
    if (!newer || !available) return l10n.hostLatestVersion('$version');
    return l10n.hostCheckingUpdates('$version');
  }

  /// The quiet Host version line of the readiness check ("Host 1.2.3 ·
  /// 최신"): information only, never an action. Null without a Host update
  /// view (released broker).
  String? get versionText {
    if (!present) return null;
    if (version == null) {
      return connected ? l10n.checkingHostVersion : l10n.hostOfflineVersionUnknown;
    }
    final host = 'Host $version';
    if (busy) return l10n.updating(host);
    if (!connected) return l10n.offline2(host);
    if (latestVersion == null) return host;
    if (!newer) return l10n.upDate2(host);
    if (!supported) return l10n.remoteUpdateNotSupportedInstall2(host);
    return l10n.newVersionAvailable(host, '$latestVersion');
  }

  /// How long an update's last report may be old before a missing Host is
  /// a missing Host again (2026-11-02).
  static const restartBound = Duration(minutes: 5);

  /// While this VM's Host update is under way (sent, downloading, verifying,
  /// installing) and reported within [restartBound], the Host going away is
  /// expected: the card's one line for it. Null otherwise.
  String? restartLine([DateTime? now]) {
    final op = operation;
    if (!present ||
        op == null ||
        !op.active ||
        op.status == 'waiting' ||
        op.checkedAt == null ||
        (now ?? DateTime.now()).difference(op.checkedAt!) > restartBound) {
      return null;
    }
    final after = l10n.youCanReconnectWhenIts;
    return switch (op.status) {
      'downloading' =>
        l10n.downloading(op.progress == null ? '' : ' (${op.progress}%)', after),
      'verifying' => l10n.checkingFile(after),
      'installing' => l10n.installing(after),
      _ => l10n.updateStarted(after),
    };
  }

  /// A short tile line while an update runs or shortly after it finished.
  String? tileStatus([DateTime? now]) {
    final op = operation;
    if (!present || op == null) return null;
    if (!op.active &&
        op.checkedAt != null &&
        (now ?? DateTime.now()).difference(op.checkedAt!) >
            const Duration(minutes: 10)) {
      return null;
    }
    return switch (op.status) {
      'waiting' => l10n.hostUpdateWaitingStartsWhen,
      'downloading' =>
        l10n.updatingHostDownloading(op.progress == null ? '' : ' ${op.progress}%'),
      'verifying' => l10n.updatingHostCheckingFile,
      'installing' => l10n.updatingHostInstalling,
      'dispatched' || 'accepted' => l10n.hostUpdating,
      'completed' =>
        op.rebootRequired
            ? l10n.hostUpdatedWindowsRestartNeeded(op.targetVersion)
            : op.code == 'display_attention'
            // This legacy code carries any installer attention, including WinFsp.
            ? l10n.hostUpdatedCheckHostSettings(op.targetVersion)
            : l10n.hostUpdated(op.targetVersion),
      'unknown' => l10n.checkHostUpdateResult(hostUpdateReason(op.code)),
      'expired' => l10n.hostUpdateWaitExpired,
      _ => l10n.hostUpdateFailed(hostUpdateReason(op.code)),
    };
  }

  bool get failed =>
      operation != null &&
      ['failed', 'rejected', 'unknown', 'expired'].contains(operation!.status);
  bool get completed => operation?.status == 'completed';
}

/// The broker's hostMissingAfterUpdate: this desktop's Host went away with
/// its last update attempt and has stayed away while the desktop runs
/// (field case 2026-11-03). It names the attempt only, never why the Host is
/// gone. Null from a broker before it and whenever it is not the case.
class HostMissingAfterUpdate {
  const HostMissingAfterUpdate({required this.result, this.targetVersion});

  /// How the attempt ended: completed, failed or unknown (no answer).
  final String result;
  final String? targetVersion;

  static HostMissingAfterUpdate? fromJson(dynamic data) {
    if (data is! Map) return null;
    final result = data['result'], target = data['targetVersion'];
    if (result is! String ||
        !const {'completed', 'failed', 'unknown'}.contains(result) ||
        (target != null && (target is! String || !_validVersion(target)))) {
      return null;
    }
    return HostMissingAfterUpdate(
      result: result,
      targetVersion: target as String?,
    );
  }

  /// The card's two lines.
  static String get headline => l10n.hostDidntReconnectAfterUpdate;
  static String get guidance => l10n.checkConnectionStatusHostDiagnostics;
  static String guidanceFor({required bool localPc}) => localPc
      ? l10n.checkServiceStatusLocalPcs
      : l10n.openDesktopConsoleCheckHost;

  /// The checklist's Host row: which attempt, then one step per sentence.
  String get steps {
    return '$_attempt $guidance';
  }

  String get _attempt {
    final host = targetVersion == null ? 'Host' : 'Host $targetVersion';
    return switch (result) {
      'unknown' => l10n.couldntConfirmUpdateResultConnection(host),
      'failed' => l10n.couldntGetResponseFromAfter(host),
      _ => l10n.couldntGetResponseFromAfter2(host),
    };
  }

  String stepsFor({required bool localPc}) =>
      '$_attempt ${guidanceFor(localPc: localPc)}';
}

/// Fixed broker/Host codes only (server/src/host-update.mjs hostUpdateCodes).
String hostUpdateReason(String code) => switch (code) {
  'session_active' => l10n.connectionUse,
  'busy' => l10n.anotherUpdateProgress,
  'not_newer' || 'same_version' => l10n.notNewerVersion,
  'download_failed' => l10n.couldntDownloadInstaller,
  'download_size_mismatch' => l10n.fileSizeDoesntMatchRelease,
  'hash_mismatch' => l10n.filesSha256DoesntMatch,
  'signature_invalid' => l10n.couldntVerifyReleaseSignature,
  'not_executable' => l10n.downloadedFileIsntInstaller,
  'stage_failed' || 'launch_failed' => l10n.couldntStartInstaller,
  'certificate_trust_required' ||
  'driver_install_required' => l10n.mustInstalledDesktopDirectly,
  'existing_install_required' => l10n.noExistingHost,
  'install_result_missing' => l10n.didntReceiveInstallResult,
  'install_failed' => l10n.couldntFinishInstalling,
  'host_no_response' ||
  'host_no_progress' ||
  'host_not_returned' => l10n.noResponseFromHost,
  'host_disconnected' => l10n.hostConnectionLost,
  _ => l10n.noMoreDetails,
};
