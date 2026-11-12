import 'broker_transport.dart';

import 'dart:async';

import 'broker_probe.dart';
import 'mfa_auth.dart';
export 'mfa_auth.dart';
import 'mfa_account.dart';
export 'mfa_account.dart';
import 'client_session_policy.dart';
export 'client_session_policy.dart';
import 'session_access.dart';
export 'session_access.dart';
import 'stream_takeover.dart';
export 'stream_takeover.dart';
import 'signin_stream.dart';
export 'signin_stream.dart';

import 'clipboard_policy.dart';
import 'track_stream.dart';
import 'initial_viewport.dart';
import 'stream_size.dart';
import 'stream_capacity.dart';
import 'input_latency.dart';
import 'remote_cursor.dart';
import 'text_input.dart';
import 'display_scale.dart';
import 'touch_input.dart';
import 'caret_follow.dart';
import 'relative_mouse.dart'
    show relativeMouseAdvertised, relativeMouseProtocol;
import 'udp_probe.dart'
    show
        udpMediaAdvertised,
        udpMediaProtocol,
        udpDirectAdvertised,
        udpDirectProtocol,
        udpTracksProtocol,
        udpTracksAdvertised;
import 'audio_codec.dart'
    show audioOpusAdvertised, audioOpusProtocol, audioOpusTracksAdvertised;
import 'session_resume.dart'
    show sessionResumeAdvertised, sessionResumeProtocol, validResumeGraceMs;
import 'usb_redirection.dart' show usbProtocol, usbTracksProtocol;
import 'print_session.dart';
import 'drive_rules.dart';
import 'client_diagnostics.dart';
import 'client_diagnostic_codes.dart';
import 'stream_stall.dart';
import 'stream_stats_report.dart';
import 'stream_observability.dart';
import 'desktop_previews.dart';
import 'notices.dart';

import 'dart:convert';
import 'dart:io';

import '../models/stream_options.dart';
import '../models/stream_preparation.dart';
import '../models/thumbnail_privacy.dart';
import '../models/virtual_display.dart';
import '../models/console_preparation.dart';
import '../models/session_cleanup.dart';
import '../models/vm_resources.dart';
import '../models/host_update.dart';
import '../models/vm_audio.dart';
import '../models/desktop_use.dart';
import '../models/desktop_organize.dart';
import '../models/pc_power.dart';
import '../l10n/l10n.dart';
export '../models/virtual_display.dart';
export '../models/pc_power.dart';
export '../models/host_update.dart';
export '../models/desktop_use.dart';

/// The VM power actions an assigned account may ask for, in Proxmox terms:
/// 켜기 (start), 종료 (graceful shutdown), 다시 시작 (reboot) and 정지 (stop,
/// immediate power-off). Never reset, suspend or
/// any other Proxmox action. The broker authorizes every request itself.
const clientVmPowerActions = {'start', 'shutdown', 'reboot', 'stop'};

/// Diagnostics `action` numbers of the client power actions.
const powerActionCodes = {'start': 1, 'reboot': 2, 'shutdown': 3, 'stop': 4};

class BrokerException implements Exception {
  const BrokerException(
    this.code, [
    this.status = 0,
    this.reason,
    this.takeover,
    this.takeoverOffer,
  ]);
  final String code;
  final int status;
  final String? reason;

  /// desktop_busy only: whether this account may end the existing connection
  /// (same account or administrator). Null from brokers without takeover.
  final bool? takeover;
  final StreamTakeoverOffer? takeoverOffer;

  /// The broker is restarting (an update): its proxy answered 502-504
  /// without a broker error, or its port refused or reset the connection.
  bool get serverRestarting =>
      code == 'server_restarting' ||
      (code == 'network_error' &&
          (reason == 'connection_refused' || reason == 'connection_reset'));

  /// This PC has no working network path right now (no DNS answer, a timeout,
  /// another socket failure): Wi-Fi switching, a dropped line, just woken from
  /// sleep. A connection being recovered waits it out (2026-11-09 audit: the
  /// Windows client gave up at once where Android waited).
  bool get networkUnavailable =>
      code == 'network_error' &&
      (reason == 'dns' || reason == 'timeout' || reason == 'network');
  @override
  String toString() => message;
  String get message =>
      consolePreparationMessage(reason) ??
      consolePreparationMessage(code) ??
      (code == 'desktop_busy' && takeover == true
          ? l10n.desktopConnectedFromAnotherWindow
          : code == 'desktop_busy' && takeover == false
          ? _reasonMessage('other_user')
          : null) ??
      _reasonMessage(reason) ??
      _reasonMessage(code) ??
      switch (code) {
        'operation_cancelled' => l10n.connectionSetupCanceledYouCan,
        'http_consent' => l10n.youNeedAgreeUnencryptedHttp,
        'http_unsupported' =>
          l10n.serverDoesntAllowHttpConnections,
        'http_disabled' ||
        'http_not_allowed' => l10n.httpConnectionsTurnedOffServer,
        'origin_not_allowed' ||
        'unregistered_origin' => l10n.addressIsntRegisteredServerCheck,
        'setup_only' => l10n.addressOnlyServersFirstSetup,
        'server_access_pending' => l10n.applyingServerAccessSettingsConnect,
        'http_profile_mismatch' =>
          l10n.addressDoesntMatchServersSettings,
        'credentials_change_required' => l10n.changeInitialUsernamePasswordAdmin,
        'invalid_credentials' => l10n.checkUsernamePassword,
        'too_many_attempts' => l10n.tooManyAttemptsTryAgain,
        'login_busy' => l10n.serverBusyTryAgainMoment,
        'mfa_invalid_code' => l10n.checkVerificationCodeEnterAgain,
        'mfa_reauth_required' => l10n.verificationTimeUpVerifyAgain,
        'mfa_challenge_expired' => l10n.verificationTimeUpStartAgain,
        'mfa_client_update_required' => l10n.updateMultitapLatestVersionUse,
        'mfa_requires_https' => l10n.twoStepVerificationWorksHttps,
        'mfa_unavailable' => l10n.serverCouldntCheckTwoStep,
        'invalid_mfa_fields' => l10n.enterEitherVerificationCodeRecovery,
        'console_browser_unavailable' =>
          l10n.couldntOpenBrowserCheckDefault,
        'console_webview_unavailable' =>
          l10n.couldntOpenConsoleCheckNetwork2,
        'console_webview_runtime_required' => l10n.consoleNeedsMicrosoftEdgeWebview2,
        'console_webview_update_required' =>
          l10n.consoleCantOpenedServerTell,
        'console_webview_busy' => l10n.anotherDesktopsConsoleOpenClose,
        'console_browser_https_required' =>
          l10n.consoleOpensOnlyHttpsServer,
        'console_browser_update_required' =>
          l10n.consoleCantOpenedServerTell,
        'active_session_limit' =>
          l10n.tooManySignInsUse,
        'session_expired' ||
        'authentication_required' => l10n.signHasExpiredSignAgain,
        'pc_login_replaced' ||
        'session_idle_timeout' ||
        'session_reauthentication_required' ||
        'pc_login_requires_interaction' => pcLoginTerminalMessage(code),
        'session_access_update_required' => l10n.serverNeedsUpdateBeforeYou,
        'login_confirmation_changed' || 'login_confirmation_expired' =>
          l10n.timeRanOutWhileChecking,
        'takeover_context_stale' => l10n.connectionUseHasChangedConnect,
        'takeover_context_expired' =>
          l10n.timeConfirmConnectionSwitchUp,
        'invalid_takeover_context' => l10n.couldntCheckConnectionSwitchDetails,
        'invalid_context_protocol' =>
          l10n.couldntDisconnectOtherConnectionConnect,
        'session_context_unavailable' =>
          l10n.couldntCheckConnectionUseConnect,
        'network_error' => l10n.couldntConnectServerCheckNetwork,
        'server_restarting' => l10n.serverIsntRespondingTryAgain,
        'current_password_incorrect' => l10n.checkCurrentPassword,
        'password_confirmation_mismatch' => l10n.newPasswordConfirmationDontMatch,
        'invalid_password' => l10n.passwordsMust10256Characters,
        'host_disconnected' ||
        'host_unavailable' => l10n.lostConnectionHostTryAgain,
        'desktop_busy' || 'session_busy' => l10n.desktopHasAnotherConnection,
        _ => l10n.couldntProcessRequestTryAgain2,
      };
  static String? _reasonMessage(String? reason) => switch (reason) {
    'tls' => l10n.couldntConnectSecurelyOverHttps,
    'dns' => l10n.couldntFindServerAddressCheck,
    'timeout' => l10n.serverDidntRespondTimeCheck,
    'connection_refused' => l10n.cantReachServerAddressPort,
    'host_offline' => l10n.desktopIsntRespondingConnectAgain,
    'capabilities_pending' => l10n.hostStillGettingReadyConnect,
    'initial_viewport_unavailable' =>
      l10n.cantPrepareScreenFitsWindow,
    'viewport_encoder_limits_unavailable' =>
      l10n.hostsStateChangedConnectAgain,
    'invalid_viewport_encoder_limits' =>
      l10n.pictureQualitySettingsDontMatch,
    'invalid_initial_viewport' =>
      l10n.cantOpenVirtualDisplayScreen,
    'host_maintenance' => l10n.hostPreparingVirtualDisplayConnect,
    'admin_required' => l10n.onlyAdministratorAccountsCanUpdate,
    'host_update_unsupported' =>
      l10n.hostDoesntSupportRemoteUpdates,
    'host_update_in_progress' => l10n.hostUpdateProgressDesktopTry,
    'host_update_current' => l10n.hostAlreadyUpDate,
    'host_update_not_newer' => l10n.isntNewerThanCurrentHost,
    'host_release_unavailable' => l10n.serversLatestHostInstallerHas,
    'host_version_unknown' => l10n.couldntCheckHostsCurrentVersion,
    'too_many_host_update_attempts' => l10n.tooManyHostUpdateRequests,
    'session_cleanup_pending' => l10n.cleaningUpPreviousConnectionConnect,
    'virtual_display_required' =>
      l10n.virtualDisplayIsntReadyIf,
    'no_interactive_session' =>
      // The way in is the Proxmox console; the launcher's
      // card offers it as 'Console로 열기'.
      l10n.windowsSignedOutLockedOpen,
    'streaming_unavailable' =>
      l10n.hostIsntReadyStreamYet,
    'controller_in_use' ||
    'desktop_busy' ||
    'session_busy' => l10n.desktopHasAnotherConnectionDisconnect,
    'other_user' => l10n.anotherUserUsingDesktopConnect,
    'too_many_takeover_attempts' => l10n.tooManyConnectionSwitchRequests,
    'power_in_progress' ||
    'power_busy' => l10n.powerActionProgressTryAgain,
    'invalid_power_state' => l10n.cantDoneCurrentPowerState,
    'power_status_unavailable' =>
      l10n.couldntCheckVmsPowerState,
    'power_task_failed' => l10n.couldntFinishPowerActionRefresh,
    'power_task_unknown' || 'power_task_timeout' =>
      l10n.powerActionsResultIsntConfirmed,
    'proxmox_settings_changed' =>
      l10n.proxmoxConnectionSettingsChangedRefresh,
    'desktop_not_found' => l10n.youDontHavePermissionDesktop,
    'power_task_not_found' => l10n.cantCheckPowerActionsResult,
    'invalid_power_action' => l10n.powerActionIsntSupported,
    'power_superseded' => l10n.anotherPowerActionEndedStatus,
    'display_control_unavailable' ||
    'display_not_supported' ||
    'virtual_monitor_control_unavailable' =>
      l10n.hostDoesntSupportChangingNumber,
    'display_in_progress' ||
    'display_busy' => l10n.changingNumberMonitorsTryAgain,
    'display_operation_failed' => l10n.couldntChangeNumberMonitorsCheck,
    'display_operation_unknown' || 'display_operation_timeout' =>
      l10n.itsNotConfirmedYetWhether,
    'display_operation_not_found' =>
      l10n.cantCheckMonitorChangeResult,
    'invalid_display_count' => l10n.youCanSet12,
    'track_streaming_unavailable' =>
      l10n.allMonitorsCantUsedDesktop,
    'local_display_layout_unavailable' =>
      l10n.couldntCheckLayoutPcs2,
    'local_display_layout_unsupported' =>
      l10n.pcsCurrentMonitorSetupCant,
    'too_many_display_attempts' => l10n.tooManyMonitorChangeRequests,
    // PC power (pc-power-v1): the broker's and the PC Host's fixed codes.
    'pc_power_unsupported' => l10n.localPcsHostDoesntSupport,
    'pc_wake_unavailable' ||
    'wake_timeout' ||
    'wake_send_failed' => l10n.couldntStartLocalPc,
    'machine_locked' => l10n.localPcLockedSoWindows,
    'other_users_signed_in' => l10n.anotherUserSignedLocalPc,
    'shutdown_in_progress' => l10n.localPcAlreadyShuttingDown,
    'privilege_unavailable' => l10n.windowsDidntGivePermissionShut,
    'shutdown_failed' => l10n.windowsCouldntProcessRequestCheck,
    'host_no_response' => l10n.localPcDidntRespondCheck,
    _ => null,
  };
}

/// 'no_interactive_session' for a 로컬 PC. The Proxmox console (Console)
/// exists only for a VM, so [BrokerException.message] (which does not know
/// the machine) would send the person to something a 로컬 PC does not have.
/// A view that knows the machine shows this instead.
String get signedOutLocalPcMessage =>
    l10n.windowsSignedOutLockedSign;

/// The one line shown while a broker update restarts the server.
String get serverRestartingMessage => l10n.serverIsntRespondingMomentRetrying;

/// A broker update restarts the server: for a few seconds its proxy answers
/// 502-504 (field diagnostics 2026-11-02) or its port refuses. List reads and
/// connects wait through it with a short backoff, for up to [window].
class ServerRestartWait {
  ServerRestartWait({DateTime Function()? clock})
    : _clock = clock ?? DateTime.now;
  static const window = Duration(minutes: 2);
  static const _delays = [1, 2, 4, 8];
  final DateTime Function() _clock;
  DateTime? _since;
  int _tries = 0;

  /// A restart was met and nothing has ended the wait yet.
  bool get waiting => _since != null;

  /// The wait before trying again after [error]; null when [error] is not a
  /// restart (the wait ends) or the restart has outlasted [window].
  Duration? next(Object error) {
    if (error is! BrokerException || !error.serverRestarting) {
      reset();
      return null;
    }
    final now = _clock();
    if (now.difference(_since ??= now) >= window) return null;
    return Duration(
      seconds: _tries < _delays.length ? _delays[_tries++] : _delays.last,
    );
  }

  /// The broker said it is restarting (a stream ended as server_stopping):
  /// the wait starts before any request has failed.
  void begin() => _since ??= _clock();

  /// A success or a new request from the user starts over.
  void reset() {
    _since = null;
    _tries = 0;
  }
}

class BrokerPowerSnapshot {
  const BrokerPowerSnapshot({
    this.state = 'unknown',
    this.checkedAt,
    this.stale = true,
    this.error,
  });
  final String state;
  final DateTime? checkedAt;
  final bool stale;
  final String? error;
  factory BrokerPowerSnapshot.fromJson(Map<String, dynamic> data) {
    final state = data['state'] as String? ?? 'unknown';
    if (!['running', 'stopped', 'unknown'].contains(state)) {
      throw const BrokerException('invalid_response');
    }
    return BrokerPowerSnapshot(
      state: state,
      checkedAt: _powerDate(data['checkedAt']),
      stale: data['stale'] as bool? ?? true,
      error: data['error'] as String?,
    );
  }
}

DateTime? _powerDate(dynamic value) {
  if (value == null) return null;
  if (value is! String) throw const BrokerException('invalid_response');
  final date = DateTime.tryParse(value);
  if (date == null) throw const BrokerException('invalid_response');
  return date;
}

class BrokerPowerTask {
  const BrokerPowerTask({
    required this.id,
    required this.desktopId,
    required this.action,
    required this.status,
    this.exitStatus,
    this.submittedAt,
    this.checkedAt,
  });
  final String id, desktopId, action, status;
  final String? exitStatus;
  final DateTime? submittedAt, checkedAt;
  bool get isActive => status == 'submitted' || status == 'running';
  factory BrokerPowerTask.fromJson(
    Map<String, dynamic> data, {
    String? desktopId,
    String? taskId,
  }) {
    final id = data['id'] as String,
        desktop = data['desktopId'] as String,
        action = data['action'] as String,
        status = data['status'] as String;
    if (!RegExp(
          r'^[0-9a-fA-F]{8}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{12}$',
        ).hasMatch(id) ||
        (taskId != null && id != taskId) ||
        (desktopId != null && desktop != desktopId) ||
        !['start', 'shutdown', 'reboot', 'stop'].contains(action) ||
        ![
          'submitted',
          'running',
          'stopped',
          'unknown',
          'superseded',
        ].contains(status)) {
      throw const BrokerException('invalid_response');
    }
    return BrokerPowerTask(
      id: id,
      desktopId: desktop,
      action: action,
      status: status,
      exitStatus: data['exitStatus'] as String?,
      submittedAt: _powerDate(data['submittedAt']),
      checkedAt: _powerDate(data['checkedAt']),
    );
  }
}

class BrokerPowerResult {
  const BrokerPowerResult({
    required this.power,
    this.task,
    this.canForceStop = false,
  });
  final BrokerPowerSnapshot power;
  final BrokerPowerTask? task;
  final bool canForceStop;
  factory BrokerPowerResult.fromJson(
    Map<String, dynamic> data, {
    required String desktopId,
    String? taskId,
    bool requireTask = false,
  }) {
    final task = data['powerTask'];
    if (data['power'] is! Map<String, dynamic> ||
        (requireTask && task is! Map<String, dynamic>) ||
        (data['canForceStop'] != null && data['canForceStop'] is! bool)) {
      throw const BrokerException('invalid_response');
    }
    return BrokerPowerResult(
      canForceStop: data['canForceStop'] == true,
      power: BrokerPowerSnapshot.fromJson(
        data['power'] as Map<String, dynamic>,
      ),
      task: task is Map<String, dynamic>
          ? BrokerPowerTask.fromJson(task, desktopId: desktopId, taskId: taskId)
          : null,
    );
  }
}

/// Local retirement marker only; wire failure remains invalid_response409.
class MfaChallengeRetiredException extends BrokerException {
  const MfaChallengeRetiredException() : super('invalid_response', 409);
}

class LoginConfirmationRequired extends BrokerException {
  LoginConfirmationRequired(this.challenge)
    : super('login_confirmation_required', 409);
  final LoginConfirmation challenge;
}

class BrokerSession {
  const BrokerSession({
    required this.token,
    required this.expiresAt,
    required this.username,
    this.accountId,
    this.role,
    this.pcSessionPolicy,
    this.sessionAccess,
  });
  final String token;
  final DateTime expiresAt;
  final String username;

  /// The broker's immutable account id (kept across login-ID renames); null
  /// from a broker that does not report it.
  final String? accountId;

  /// 'admin' or 'user' as reported at sign-in. Only shows or hides
  /// administrator actions; the broker authorizes every request itself.
  final String? role;

  /// Accepted response metadata only; never grants access on its own.
  final String? pcSessionPolicy;
  final SessionAccess? sessionAccess;
  bool get isAdministrator => role == 'admin';

  /// The same session with a later expiry (renewal keeps the token).
  BrokerSession withExpiry(DateTime value) => BrokerSession(
    token: token,
    expiresAt: value,
    username: username,
    accountId: accountId,
    role: role,
    pcSessionPolicy: pcSessionPolicy,
    sessionAccess: sessionAccess,
  );

  BrokerSession withPcSessionPolicy(String? value) => BrokerSession(
    token: token,
    expiresAt: expiresAt,
    username: username,
    accountId: accountId,
    role: role,
    pcSessionPolicy: value == windowsPcSessionPolicy ? value : null,
    sessionAccess: sessionAccess,
  );

  // A renewal only moves the expiry: work started under the earlier object
  // (`_session == session` checks) stays valid across it.
  @override
  bool operator ==(Object other) =>
      other is BrokerSession &&
      other.token == token &&
      other.username == username &&
      other.accountId == accountId &&
      other.role == role;
  @override
  int get hashCode => Object.hash(token, username, accountId, role);
  factory BrokerSession.fromJson(
    Map<String, dynamic> data, {
    Duration responseElapsed = Duration.zero,
  }) {
    final user = data['user'] as Map<String, dynamic>;
    final id = user['id'];
    final role = user['role'];
    return BrokerSession(
      token: data['sessionToken'] as String,
      expiresAt: DateTime.parse(data['expiresAt'] as String),
      username: user['username'] as String,
      accountId: id is String && id.isNotEmpty && id.length <= 256
          ? id
          : id is int
          ? '$id'
          : null,
      role: role == 'admin' || role == 'user' ? role as String : null,
      sessionAccess: data.containsKey('sessionAccess')
          ? SessionAccess.parse(
              data['sessionAccess'],
              responseElapsed: responseElapsed,
            )
          : null,
    );
  }
}

const bitrateAdaptiveProtocol = 'bitrate-adaptive-v1';

/// The adaptive-bitrate ceiling a capability advertises, or null.
int? adaptiveBitrateAdvertised(Object? capabilities) {
  if (capabilities is! Map ||
      capabilities['bitrateProtocol'] != bitrateAdaptiveProtocol) {
    return null;
  }
  final maximum = capabilities['maxBitrate'];
  return maximum is int &&
          maximum >= StreamOptions.legacyMaximumBitrate &&
          maximum <= StreamOptions.adaptiveMaximumBitrate
      ? maximum
      : null;
}

class BrokerDesktop {
  const BrokerDesktop({
    required this.id,
    required this.name,
    required this.kind,
    required this.description,
    required this.os,
    required this.status,
    required this.canConnect,
    this.signinAvailability,
    this.consoleInUse = false,
    this.qemuAgentOff = false,
    bool? canStream,
    this.consoleRelayProtocol,
    this.consoleBrowserProtocol,
    this.consoleWebViewProtocol,
    this.localPcApproved = false,
    this.monitors = const [],
    this.hostConnected,
    this.thumbnailPrivacy,
    this.interactiveSession,
    this.virtualDisplayReady,
    this.streamPreparation,
    this.power = const BrokerPowerSnapshot(),
    this.powerTask,
    this.vmState,
    this.vmStateReason,
    this.vmStateRevision = 0,
    this.powerBusy = false,
    this.powerCanForceStop = false,
    this.display = const VirtualDisplayState(),
    this.virtualMonitorControl = false,
    this.independentDisplayStreaming = false,
    this.initialViewport = false,
    this.viewportEncoderLimits = false,
    this.profileTracks = false,
    this.resources = const VmResources(),
    this.consolePreparation = const ConsolePreparationState(),
    this.vmAudio = false,
    this.audioState,
    this.sessionCleanup = const SessionCleanupState(),
    this.maxBitrate,
    this.hostUpdate = HostUpdateState.absent,
    this.hostMissingAfterUpdate,
    this.inputLatency = false,
    this.observability = false,
    this.cursor = false,
    this.cursorTracks = false,
    this.textInput = false,
    this.displayScale = false,
    this.displayScaleOutputs = false,
    this.touchInput = false,
    this.caret = false,
    this.drive = false,
    this.driveFs = false,
    this.driveUnavailable,
    this.printUnavailable,
    this.streamSize = false,
    this.streamSizeV2 = false,
    this.print = false,
    this.usb = false,
    this.udpMedia = false,
    this.udpTracks = false,
    this.udpDirect = false,
    this.audioOpus = false,
    this.audioOpusTracks = false,
    this.relativeMouse = false,
    this.cursorEcho = false,
    this.cursorCompose = false,
    this.sessionResume = false,
    this.use = DesktopUse.idle,
    this.useAccount,
    this.organize = DesktopOrganize.none,
    this.pcPower = PcPowerState.absent,
  }) : canStream = canStream ?? canConnect;

  /// Tags, pool, favourite and last use (absent from a broker before desktop
  /// organising).
  final DesktopOrganize organize;
  final SigninAvailability? signinAvailability;

  /// A console connection holds this VM.
  final bool consoleInUse;
  final bool qemuAgentOff;

  /// An approved PC's power (pc-power-v1); [PcPowerState.absent] for a VM
  /// and from a broker before it.
  final PcPowerState pcPower;

  /// Broker-owned capability, independent of Host/virtual display readiness.
  final String? consoleRelayProtocol;
  final String? consoleBrowserProtocol;
  final String? consoleWebViewProtocol;
  bool get consoleWebViewSupported =>
      kind == 'vm' &&
      BrokerConsoleWebViewGrant.desktopIdValid(id) &&
      consoleWebViewProtocol == 'console-webview-v1';
  bool get browserConsoleSupported =>
      kind == 'vm' &&
      RegExp(r'^[a-zA-Z0-9][a-zA-Z0-9_-]{0,79}$').hasMatch(id) &&
      consoleBrowserProtocol == 'console-browser-v1';
  final bool localPcApproved;

  /// drive-v1 (streaming drive redirection) is available to this account on
  /// this VM: the Host advertises it and the broker shows it only where the
  /// assignment allows drives.
  final bool drive;

  /// The Host also advertises driveFs (the "fs" revision of drive-v1); only
  /// meaningful together with [drive].
  final bool driveFs;

  /// Why the Host cannot offer drives although this account may use them
  /// (drive_webclient_unavailable), shown by the session instead of a
  /// missing 드라이브 button. Null otherwise.
  final String? driveUnavailable;

  /// Why the VM cannot offer its PC printer although this account may print
  /// (print_spooler_unavailable / print_driver_unavailable). Null otherwise.
  final String? printUnavailable;

  /// The host advertises stream-size-v1 (full-size encoding within this
  /// client's decoder limits, portrait outputs up to 2160x3840) through a
  /// broker that understands it; a released broker drops the field.
  final bool streamSize;
  final bool streamSizeV2;

  /// A connection holds the VM now: broker `activeSession` (own account or
  /// not), or only "a connection" from a broker's Host update view.
  final DesktopUse use;

  /// Who the other connection belongs to, when the broker names it.
  final String? useAccount;

  /// The host advertises input-latency-v1 through a broker that understands
  /// it (a released broker drops the unknown capability).
  final bool inputLatency;
  final bool observability;

  /// The host advertises cursor-v1 (separated local cursor) through a broker
  /// that understands it; a released broker drops the unknown capability.
  final bool cursor;

  /// The host also advertises cursor-tracks-v1 (the separated cursor for
  /// independent outputs) through a broker that grants it; a released broker
  /// drops the field.
  final bool cursorTracks;

  /// The host advertises text-input-v1 (committed Unicode text) through a
  /// broker that understands it; a released broker drops it.
  final bool textInput;

  /// The host advertises display-scale-v1 (Windows display scale of the
  /// streamed output) through a broker that understands it.
  final bool displayScale;

  /// The host also advertises display-scale-outputs-v1 (the same scale on
  /// each owned output of independent outputs) through a broker that grants
  /// it; a released broker drops the field.
  final bool displayScaleOutputs;

  /// The host advertises touch-input-v1 (Windows touch contacts) through a
  /// broker that understands it; a released broker drops it.
  final bool touchInput;

  /// The host advertises caret-v1 (text caret position reports) through a
  /// broker that understands it; a released broker drops it.
  final bool caret;

  /// The host advertises print-v1 (a printer in the VM that prints on this
  /// PC) through a broker that understands it; the grant itself is the
  /// administrator's per-VM printers permission, decided at connect.
  final bool print;

  /// The host advertises usb-v1 (USB devices of this PC inside the VM) through a
  /// broker that understands it; a released broker drops it.
  final bool usb;

  /// The desktop shows udp-media-v1 while its broker's relay runs; a released
  /// broker strips the capability (and rejects the connect field).
  final bool udpMedia;
  final bool udpTracks;
  final bool udpDirect;

  /// The host advertises audio-opus-v1 through a broker that understands it;
  /// a released broker strips it (and rejects the connect field).
  final bool audioOpus;

  /// The host's independent outputs send Opus too (audio-opus-tracks-v1).
  final bool audioOpusTracks;

  /// The host advertises relative-mouse-v1 through a broker that understands
  /// it; a released broker strips it (and rejects the connect field).
  final bool relativeMouse;

  /// The host advertises cursor-echo-v1 (the VM's real cursor position while
  /// 게임 마우스 holds the mouse) with cursor-v1, through a broker that
  /// understands it; a released broker strips it (and rejects the field).
  final bool cursorEcho;

  /// The host advertises cursor-compose-v1 (the VM's real cursor inside the
  /// video while 게임 마우스 holds the mouse) with cursor-v1; the same rule.
  final bool cursorCompose;

  /// The broker shows session-resume-v1 where it can continue a stream whose
  /// socket was lost (session_resume.dart); a released broker never shows it.
  final bool sessionResume;
  final HostUpdateState hostUpdate;

  /// The Host left with the last update attempt and stayed away (broker
  /// hostMissingAfterUpdate); null otherwise and from a broker before it.
  final HostMissingAfterUpdate? hostMissingAfterUpdate;
  final String id, name, kind, description, os, status;
  final bool canConnect;
  final bool canStream;
  final bool vmAudio;

  /// The VM's sound card as the broker last read it (`audio.state` of the
  /// list item: absent, present or pending); null when unknown or not sent.
  final String? audioState;
  bool get emergencyConsoleSupported =>
      kind == 'vm' &&
      RegExp(r'^pve-[1-9][0-9]{2,11}$').hasMatch(id) &&
      consoleRelayProtocol == 'rfb-none-v1';
  final BrokerPowerSnapshot power;
  final BrokerPowerTask? powerTask;

  /// The broker's VM state, its reason code and monotonic revision.
  final String? vmState, vmStateReason;
  final int vmStateRevision;
  final bool powerBusy, powerCanForceStop;
  final VirtualDisplayState display;
  final bool virtualMonitorControl;
  final bool independentDisplayStreaming;
  final bool initialViewport;
  final bool viewportEncoderLimits;
  final bool profileTracks;

  /// bitrate-adaptive-v1 ceiling of this desktop's host, passed through by a
  /// broker that understands it; null = released fixed 4-20 Mbps only.
  final int? maxBitrate;
  final VmResources resources;
  final ConsolePreparationState consolePreparation;
  final SessionCleanupState sessionCleanup;
  final List<RemoteMonitor> monitors;
  final StreamPreparation? streamPreparation;
  final ThumbnailPrivacy? thumbnailPrivacy;
  final bool? hostConnected, interactiveSession, virtualDisplayReady;
  factory BrokerDesktop.fromJson(Map<String, dynamic> data) => BrokerDesktop(
    id: data['id'] as String,
    name: data['name'] as String,
    kind: data['kind'] as String,
    description: data['description'] as String,
    os: data['os'] as String,
    status: data['status'] as String,
    canConnect: data['canConnect'] as bool,
    signinAvailability: SigninAvailability.tryParse(data['signinAvailability']),
    consoleInUse: data['consoleInUse'] == true,
    qemuAgentOff: data['qemuAgentOff'] == true,
    consoleRelayProtocol: data['consoleRelayProtocol'] == 'rfb-none-v1'
        ? 'rfb-none-v1'
        : null,
    consoleBrowserProtocol:
        data['consoleBrowserProtocol'] == 'console-browser-v1'
        ? 'console-browser-v1'
        : null,
    consoleWebViewProtocol:
        data['consoleWebViewProtocol'] == 'console-webview-v1'
        ? 'console-webview-v1'
        : null,
    vmAudio: data['kind'] == 'vm' && data['vmAudioProtocol'] == vmAudioProtocol,
    // Lenient: anything but a known state is "unknown" (no badge).
    audioState: switch (data['audio']) {
      {'state': final String state}
          when data['kind'] == 'vm' &&
              const {'absent', 'present', 'pending'}.contains(state) =>
        state,
      _ => null,
    },
    resources: VmResources.fromJson(data['resources']),
    hostUpdate: HostUpdateState.fromJson(data['hostUpdate']),
    hostMissingAfterUpdate: HostMissingAfterUpdate.fromJson(
      data['hostMissingAfterUpdate'],
    ),
    use: desktopUseFromJson(data['activeSession'], data['hostUpdate']),
    useAccount: desktopUseAccountFromJson(data['activeSession']),
    organize: DesktopOrganize.fromJson(data),
    sessionCleanup: SessionCleanupState.fromJson(data['sessionCleanup']),
    consolePreparation: ConsolePreparationState.fromJson(
      data['consolePreparation'],
    ),
    canStream:
        (data['connectionModes'] as Map?)?['stream'] as bool? ??
        data['canConnect'] as bool,
    localPcApproved:
        data['kind'] == 'pc' &&
        data['localPcApproved'] == true &&
        data['id'] is String &&
        RegExp(
          r'^pc-[a-f0-9]{8}-[a-f0-9]{4}-[a-f0-9]{4}-[a-f0-9]{4}-[a-f0-9]{12}$',
        ).hasMatch(data['id'] as String),
    power: BrokerPowerSnapshot.fromJson({
      'state': data['powerState'],
      'checkedAt': data['powerCheckedAt'],
      'stale': data['powerStale'],
      'error': data['powerError'],
    }),
    powerTask: data['powerTask'] is Map<String, dynamic>
        ? BrokerPowerTask.fromJson(
            data['powerTask'] as Map<String, dynamic>,
            desktopId: data['id'] as String,
          )
        // An approved PC's own task travels inside pcPower (pc-power-v1).
        : data['kind'] == 'pc' &&
              data['pcPower'] is Map &&
              (data['pcPower'] as Map)['task'] is Map<String, dynamic>
        ? BrokerPowerTask.fromJson(
            (data['pcPower'] as Map)['task'] as Map<String, dynamic>,
            desktopId: data['id'] as String,
          )
        : null,
    pcPower: data['kind'] == 'pc'
        ? PcPowerState.fromJson(data['pcPower'])
        : PcPowerState.absent,
    vmState: data['vmState'] is String ? data['vmState'] as String : null,
    vmStateReason: data['vmStateReason'] is String
        ? data['vmStateReason'] as String
        : null,
    vmStateRevision: data['vmStateRevision'] is int
        ? data['vmStateRevision'] as int
        : 0,
    powerBusy:
        data['powerBusy'] as bool? ??
        (data['kind'] == 'pc' &&
            data['pcPower'] is Map &&
            (data['pcPower'] as Map)['busy'] == true),
    powerCanForceStop: data['powerCanForceStop'] == true,
    display: data['display'] is Map<String, dynamic>
        ? VirtualDisplayState.fromJson(data['display'] as Map<String, dynamic>)
        : const VirtualDisplayState(),
    virtualMonitorControl:
        ((data['host'] as Map?)?['capabilities']
            as Map?)?['virtualMonitorControl'] ==
        true,
    viewportEncoderLimits: viewportEncoderLimitsAdvertised(
      data['host'] is Map ? (data['host'] as Map)['capabilities'] : null,
    ),
    initialViewport: initialViewportAdvertised(
      (data['host'] as Map?)?['capabilities'],
    ),
    independentDisplayStreaming: data['kind'] == 'pc'
        ? ordinaryPcTracksAdvertised((data['host'] as Map?)?['capabilities'])
        : independentTracksAdvertised((data['host'] as Map?)?['capabilities']),
    profileTracks: profileTracksAdvertised(
      (data['host'] as Map?)?['capabilities'],
    ),
    maxBitrate: adaptiveBitrateAdvertised(
      (data['host'] as Map?)?['capabilities'],
    ),
    inputLatency:
        ((data['host'] as Map?)?['capabilities']
            as Map?)?['inputLatencyProtocol'] ==
        inputLatencyProtocol,
    observability:
        ((data['host'] as Map?)?['capabilities']
            as Map?)?['observabilityProtocol'] ==
        streamObservabilityProtocol,
    cursor:
        ((data['host'] as Map?)?['capabilities'] as Map?)?['cursorProtocol'] ==
        cursorProtocol,
    cursorTracks:
        ((data['host'] as Map?)?['capabilities'] as Map?)?['cursorProtocol'] ==
            cursorProtocol &&
        ((data['host'] as Map?)?['capabilities']
                as Map?)?['cursorTracksProtocol'] ==
            cursorTracksProtocol,
    textInput:
        ((data['host'] as Map?)?['capabilities']
            as Map?)?['textInputProtocol'] ==
        textInputProtocol,
    displayScale:
        ((data['host'] as Map?)?['capabilities']
            as Map?)?['displayScaleProtocol'] ==
        displayScaleProtocol,
    displayScaleOutputs:
        ((data['host'] as Map?)?['capabilities']
                as Map?)?['displayScaleProtocol'] ==
            displayScaleProtocol &&
        ((data['host'] as Map?)?['capabilities']
                as Map?)?['displayScaleOutputsProtocol'] ==
            displayScaleOutputsProtocol,
    touchInput:
        ((data['host'] as Map?)?['capabilities']
            as Map?)?['touchInputProtocol'] ==
        touchInputProtocol,
    caret:
        ((data['host'] as Map?)?['capabilities'] as Map?)?['caretProtocol'] ==
        caretProtocol,
    drive:
        ((data['host'] as Map?)?['capabilities'] as Map?)?['driveProtocol'] ==
        driveProtocol,
    driveFs:
        ((data['host'] as Map?)?['capabilities'] as Map?)?['driveFs'] == true,
    driveUnavailable:
        ((data['host'] as Map?)?['capabilities']
                as Map?)?['driveUnavailableReason'] ==
            'drive_webclient_unavailable'
        ? 'drive_webclient_unavailable'
        : null,
    printUnavailable: switch (((data['host'] as Map?)?['capabilities']
        as Map?)?['printUnavailableReason']) {
      'print_spooler_unavailable' => 'print_spooler_unavailable',
      'print_driver_unavailable' => 'print_driver_unavailable',
      _ => null,
    },
    streamSize: streamSizeAdvertised((data['host'] as Map?)?['capabilities']),
    streamSizeV2: streamSizeV2Advertised(
      (data['host'] as Map?)?['capabilities'],
    ),
    print:
        ((data['host'] as Map?)?['capabilities'] as Map?)?['printProtocol'] ==
        printProtocol,
    usb:
        ((data['host'] as Map?)?['capabilities'] as Map?)?['usbProtocol'] ==
        usbProtocol,
    udpMedia: udpMediaAdvertised((data['host'] as Map?)?['capabilities']),
    udpTracks: udpTracksAdvertised((data['host'] as Map?)?['capabilities']),
    udpDirect: udpDirectAdvertised((data['host'] as Map?)?['capabilities']),
    audioOpus: audioOpusAdvertised((data['host'] as Map?)?['capabilities']),
    audioOpusTracks: audioOpusTracksAdvertised(
      (data['host'] as Map?)?['capabilities'],
    ),
    relativeMouse: relativeMouseAdvertised(
      (data['host'] as Map?)?['capabilities'],
    ),
    cursorEcho: cursorEchoAdvertised((data['host'] as Map?)?['capabilities']),
    cursorCompose: cursorComposeAdvertised(
      (data['host'] as Map?)?['capabilities'],
    ),
    sessionResume: sessionResumeAdvertised(
      (data['host'] as Map?)?['capabilities'],
    ),
    hostConnected: (data['host'] as Map?)?['connected'] as bool?,
    thumbnailPrivacy: ThumbnailPrivacy.fromHost(data['host']),
    interactiveSession:
        ((data['host'] as Map?)?['capabilities'] as Map?)?['interactiveSession']
            as bool?,
    virtualDisplayReady:
        ((data['host'] as Map?)?['capabilities']
                as Map?)?['virtualDisplayReady']
            as bool?,
    streamPreparation: StreamPreparation.fromCode(
      (data['consolePreparation'] is Map &&
              ((data['host'] as Map?)?['capabilities']
                      as Map?)?['interactiveSession'] !=
                  true &&
              (data['consolePreparation'] as Map)['supported'] == true)
          ? (data['consolePreparation'] as Map)['reason']
          : ((data['host'] as Map?)?['capabilities']
                as Map?)?['viewportFailureReason'],
    ),
    monitors:
        (((data['host'] as Map?)?['capabilities'] as Map?)?['monitors']
                    as List? ??
                const [])
            .take(16)
            .whereType<Map>()
            .where(
              (item) =>
                  item['id'] is String &&
                  item['width'] is int &&
                  item['height'] is int,
            )
            .map(
              (item) => RemoteMonitor(
                id: item['id'] as String,
                width: item['width'] as int,
                height: item['height'] as int,
                primary: item['primary'] == true,
              ),
            )
            .toList(),
  );
}

/// Owns one negotiated broker reservation; closing twice must not retry DELETE.
/// Its acknowledgement is not evidence of Host/Windows display restoration.
class BrokerSessionReservation {
  BrokerSessionReservation(this._cancel);
  final Future<void> Function() _cancel;
  Future<void>? _release;
  Future<void> release() =>
      _release ??= Future<void>.sync(_cancel)
          .timeout(const Duration(seconds: 3));

  /// An explicit new attempt after a failed (or never made) release, e.g.
  /// before reconnecting once the network is back; a release that succeeded
  /// is not repeated. [release] itself never retries.
  Future<void> retry() async {
    final previous = _release;
    if (previous != null) {
      try {
        await previous;
        return;
      } catch (_) {
        _release = null;
      }
    }
    return release();
  }
}

class BrokerConnection {
  const BrokerConnection({
    required this.sessionId,
    required this.webSocketUrl,
    this.mode = 'stream',
    this.options = const StreamOptions(),
    this.clipboardWebSocketUrl,
    this.clipboardPolicy = const ClipboardPolicy(),
    this.clipboardSync = false,
    this.epochMedia = false,
    this.viewportProvisioning = false,
    this.trackPlan,
    this.initialViewport,
    this.viewportEncoderLimits = false,
    this.profileTracks = false,
    this.reservation,
    this.bitrateAdaptive = false,
    this.inputLatency = false,
    this.observability = false,
    this.observabilityPath = const {},
    this.observabilityVersions = const {},
    this.cursor = false,
    this.cursorTracks = false,
    this.textInput = false,
    this.displayScaleControl = false,
    this.displayScale,
    this.displayScaleOutputs = false,
    this.touchInput = false,
    this.caret = false,
    this.driveWebSocketUrl,
    this.driveFs = false,
    this.streamSize,
    this.streamSizeV2 = false,
    this.hardwareDecode,
    this.printWebSocketUrl,
    this.usbWebSocketUrl,
    this.printRequested = false,
    this.usbRequested = false,
    this.udpMedia = false,
    this.udpTracks = false,
    this.audioOpus = false,
    this.relativeMouse = false,
    this.cursorEcho = false,
    this.cursorCompose = false,
    this.sessionResume = false,
    this.resumeGraceMs = 15000,
    this.signin,
  });
  factory BrokerConnection.fromJson(
    Map<String, dynamic> data,
    Uri origin, {
    StreamOptions? requestedOptions,
    TrackPlan? requestedTracks,
    InitialViewport? requestedInitialViewport,
    bool requestedProfileTracks = false,
    bool requestedInputLatency = false,
    bool requestedObservability = false,
    bool requestedCursor = false,
    bool requestedCursorTracks = false,
    bool requestedTextInput = false,
    bool requestedDisplayScaleControl = false,
    int? requestedDisplayScale,
    bool requestedDisplayScaleOutputs = false,
    bool requestedTouchInput = false,
    bool requestedCaret = false,
    bool requestedDrive = false,
    bool requestedDriveFs = false,
    DecoderLimits? requestedStreamSize,
    bool requestedStreamSizeV2 = false,
    bool? requestedHardwareDecode,
    bool requestedPrint = false,
    bool requestedUsb = false,
    bool requestedUdpMedia = false,
    bool requestedUdpTracks = false,
    bool requestedAudioOpus = false,
    bool requestedRelativeMouse = false,
    bool requestedCursorEcho = false,
    bool requestedCursorCompose = false,
    bool requestedSessionResume = false,
    bool requestedSignin = false,
    Future<void> Function(String sessionId)? cancel,
  }) {
    final sessionId = data['sessionId'] as String;
    final uri = Uri.parse(data['webSocketUrl'] as String);

    final mode = data['mode'] as String? ?? 'stream';
    final transport = data['transport'] as String?;
    final epochMedia = transport == 'media-epoch-v1';
    final trackMedia = transport == 'media-tracks-v1';
    if (data['udpTracksProtocol'] != null &&
        (!requestedUdpTracks ||
            !trackMedia ||
            data['udpTracksProtocol'] != udpTracksProtocol)) {
      throw const BrokerException('invalid_response');
    }
    if ((requestedStreamSizeV2 &&
            data['streamSizeProtocol'] != streamSizeProtocolV2) ||
        mode != 'stream' ||
        (data['sessionCancellationProtocol'] != null &&
            data['sessionCancellationProtocol'] != 'session-cancel-v1') ||
        (data['sessionCancellationProtocol'] != null &&
            !virtualDisplayOperationId.hasMatch(sessionId)) ||
        (data['viewportEncoderLimitsProtocol'] !=
            (requestedInitialViewport?.preserveEncoderLimits == true
                ? viewportEncoderLimitsProtocol
                : null)) ||
        (trackMedia != (requestedTracks != null)) ||
        (data['streamProfileTracksProtocol'] !=
            (requestedProfileTracks ? profileTracksProtocol : null)) ||
        (requestedProfileTracks && !trackMedia) ||
        (trackMedia &&
            (mode != 'stream' ||
                data['mediaProtocol'] != 'tracks-v1' ||
                data['trackViewportProvisioning'] != true ||
                !requestedTracks!.matches(data['tracks']))) ||
        (!trackMedia &&
            (data['mediaProtocol'] != null ||
                data['tracks'] != null ||
                data['trackViewportProvisioning'] != null)) ||
        (transport != null &&
            transport != 'media-v1' &&
            !(mode == 'stream' &&
                epochMedia &&
                data['resizeProtocol'] == 'epoch-v1') &&
            !trackMedia) ||
        (data['viewportProvisioning'] != null &&
            data['viewportProvisioning'] is! bool) ||
        (data['viewportProvisioning'] == true && !epochMedia) ||
        (data['clipboardProtocol'] != null &&
            data['clipboardProtocol'] != 'clipboard-sync-v1') ||
        (data['clipboardSync'] != null && data['clipboardSync'] is! bool) ||
        (data['clipboardSync'] == true &&
            (mode != 'stream' ||
                data['clipboardProtocol'] != 'clipboard-sync-v1')) ||
        // The adaptive rate is granted only to an ordinary stream session.
        (data['bitrateProtocol'] != null &&
            (data['bitrateProtocol'] != bitrateAdaptiveProtocol ||
                mode != 'stream' ||
                trackMedia)) ||
        // Input latency is measured only when this request asked for it, for
        // an ordinary stream session; the grant itself is optional.
        (data['inputLatencyProtocol'] != null &&
            (data['inputLatencyProtocol'] != inputLatencyProtocol ||
                !requestedInputLatency ||
                mode != 'stream' ||
                trackMedia)) ||
        // cursor-v1 likewise: only on request, for a stream session; for
        // independent outputs only with cursor-tracks-v1.
        (data['cursorProtocol'] != null &&
            (data['cursorProtocol'] != cursorProtocol ||
                !requestedCursor ||
                mode != 'stream' ||
                trackMedia != (data['cursorTracksProtocol'] != null))) ||
        // cursor-tracks-v1: only on request, only with the cursor-v1 grant,
        // only for independent outputs.
        (data['cursorTracksProtocol'] != null &&
            (data['cursorTracksProtocol'] != cursorTracksProtocol ||
                !requestedCursorTracks ||
                data['cursorProtocol'] != cursorProtocol ||
                !trackMedia)) ||
        // text-input-v1 likewise.
        (data['textInputProtocol'] != null &&
            (data['textInputProtocol'] != textInputProtocol ||
                !requestedTextInput ||
                mode != 'stream' ||
                trackMedia)) ||
        // A runtime grant may omit the initial scale. Any provided initial
        // value must still exactly match the requested value.
        ((data['displayScaleProtocol'] != null ||
                data.containsKey('displayScale')) &&
            (data['displayScaleProtocol'] != displayScaleProtocol ||
                (!requestedDisplayScaleControl &&
                    requestedDisplayScale == null) ||
                (data.containsKey('displayScale')
                    ? requestedDisplayScale == null ||
                          data['displayScale'] != requestedDisplayScale
                    : requestedDisplayScale != null) ||
                mode != 'stream' ||
                // Independent outputs only with display-scale-outputs-v1.
                trackMedia != (data['displayScaleOutputsProtocol'] != null))) ||
        // display-scale-outputs-v1: only on request, only with the
        // display-scale-v1 grant, only for independent outputs.
        (data['displayScaleOutputsProtocol'] != null &&
            (data['displayScaleOutputsProtocol'] !=
                    displayScaleOutputsProtocol ||
                !requestedDisplayScaleOutputs ||
                data['displayScaleProtocol'] != displayScaleProtocol ||
                !trackMedia)) ||
        // touch-input-v1: only on request, for an ordinary stream session.
        (data['touchInputProtocol'] != null &&
            (data['touchInputProtocol'] != touchInputProtocol ||
                !requestedTouchInput ||
                mode != 'stream' ||
                trackMedia)) ||
        // relative-mouse-v1: only on request, for a stream session; for
        // independent outputs (relative-mouse-tracks-v1) only with the
        // tracks cursor and its composition, the only cursor 게임 마우스 has there.
        (data['relativeMouseProtocol'] != null &&
            (data['relativeMouseProtocol'] != relativeMouseProtocol ||
                !requestedRelativeMouse ||
                mode != 'stream' ||
                (trackMedia &&
                    (data['cursorTracksProtocol'] != cursorTracksProtocol ||
                        data['cursorComposeProtocol'] !=
                            cursorComposeProtocol)))) ||
        // caret-v1: only on request, for an ordinary stream session.
        (data['caretProtocol'] != null &&
            (data['caretProtocol'] != caretProtocol ||
                !requestedCaret ||
                mode != 'stream' ||
                trackMedia)) ||
        // cursor-echo-v1: only on request, only with the cursor-v1 grant, for
        // an ordinary stream session.
        (data['cursorEchoProtocol'] != null &&
            (data['cursorEchoProtocol'] != cursorEchoProtocol ||
                !requestedCursorEcho ||
                data['cursorProtocol'] != cursorProtocol ||
                mode != 'stream' ||
                trackMedia)) ||
        // cursor-compose-v1: the same rule; independent outputs only with
        // cursor-tracks-v1 (the output the cursor is over draws it).
        (data['cursorComposeProtocol'] != null &&
            (data['cursorComposeProtocol'] != cursorComposeProtocol ||
                !requestedCursorCompose ||
                data['cursorProtocol'] != cursorProtocol ||
                mode != 'stream' ||
                (trackMedia &&
                    data['cursorTracksProtocol'] != cursorTracksProtocol))) ||
        // stream-size-v1: only on request, for a stream with an initial
        // viewport and separate-v1 encoder ceilings.
        (data['streamSizeProtocol'] != null &&
            (data['streamSizeProtocol'] !=
                    (requestedStreamSizeV2
                        ? streamSizeProtocolV2
                        : streamSizeProtocol) ||
                requestedStreamSize == null ||
                (!trackMedia &&
                    requestedInitialViewport?.preserveEncoderLimits != true) ||
                mode != 'stream' ||
                (trackMedia && !requestedStreamSizeV2))) ||
        // print-v1: only on request, for a stream session, always with its URL.
        ((data['printProtocol'] != null || data['printWebSocketUrl'] != null) &&
            (data['printProtocol'] != printProtocol ||
                !requestedPrint ||
                mode != 'stream' ||
                data['printWebSocketUrl'] is! String)) ||
        // usb-v1: only on request, for a stream session (independent outputs
        // too; see usbTracksProtocol below).
        ((data['usbProtocol'] != null || data['usbWebSocketUrl'] != null) &&
            (data['usbProtocol'] != usbProtocol ||
                !requestedUsb ||
                mode != 'stream' ||
                data['usbWebSocketUrl'] is! String)) ||
        // usb-tracks-v1: only with the usb-v1 grant of an independent-outputs
        // session.
        (data['usbTracksProtocol'] != null &&
            (data['usbTracksProtocol'] != usbTracksProtocol ||
                data['usbProtocol'] != usbProtocol ||
                !trackMedia)) ||
        // audio-opus-v1: only on request, for a stream session (independent
        // outputs ask only with audio-opus-tracks-v1).
        (data['audioCodecProtocol'] != null &&
            (data['audioCodecProtocol'] != audioOpusProtocol ||
                !requestedAudioOpus ||
                mode != 'stream')) ||
        // session-resume-v1: only on request, for a stream session (independent
        // outputs: tracks-resume-v1 on the Host), always with its grace window.
        ((data['sessionResumeProtocol'] != null ||
                data['resumeGraceMs'] != null) &&
            (data['sessionResumeProtocol'] != sessionResumeProtocol ||
                !requestedSessionResume ||
                mode != 'stream' ||
                validResumeGraceMs(data['resumeGraceMs']) == null)) ||
        !RegExp(r'^[A-Za-z0-9-]{1,80}$').hasMatch(sessionId) ||
        !brokerRelayMatches(uri, origin) ||
        uri.userInfo.isNotEmpty ||
        uri.hasQuery ||
        uri.hasFragment ||
        uri.path != '/api/v1/sessions/$sessionId') {
      throw const BrokerException('invalid_response');
    }
    Uri? clipboard;
    if (data['clipboardWebSocketUrl'] != null) {
      if (data['clipboardWebSocketUrl'] is! String || mode != 'stream') {
        throw const BrokerException('invalid_response');
      }
      clipboard = Uri.parse(data['clipboardWebSocketUrl'] as String);
      if (!brokerRelayMatches(clipboard, origin) ||
          clipboard.userInfo.isNotEmpty ||
          clipboard.hasQuery ||
          clipboard.hasFragment ||
          clipboard.path != '/api/v1/sessions/$sessionId/clipboard') {
        throw const BrokerException('invalid_response');
      }
    }
    Uri? printUrl;
    if (data['printWebSocketUrl'] != null) {
      printUrl = Uri.parse(data['printWebSocketUrl'] as String);
      if (!brokerRelayMatches(printUrl, origin) ||
          printUrl.userInfo.isNotEmpty ||
          printUrl.hasQuery ||
          printUrl.hasFragment ||
          printUrl.path != '/api/v1/sessions/$sessionId/print') {
        throw const BrokerException('invalid_response');
      }
    }
    // drive-v1: only a grant this request asked for, for a stream session,
    // with exactly the session's drive socket on this broker's origin.
    Uri? drive;
    // driveFs: only with a drive grant and only when this request asked.
    final driveFs = data['driveFs'];
    if (driveFs != null &&
        (driveFs != true ||
            !requestedDriveFs ||
            !requestedDrive ||
            data['driveProtocol'] != driveProtocol ||
            data['driveWebSocketUrl'] == null)) {
      throw const BrokerException('invalid_response');
    }
    if (data['driveProtocol'] != null || data['driveWebSocketUrl'] != null) {
      if (!requestedDrive ||
          mode != 'stream' ||
          data['driveProtocol'] != driveProtocol ||
          data['driveWebSocketUrl'] is! String) {
        throw const BrokerException('invalid_response');
      }
      drive = Uri.tryParse(data['driveWebSocketUrl'] as String);
      if (drive == null ||
          !brokerRelayMatches(drive, origin) ||
          drive.userInfo.isNotEmpty ||
          drive.hasQuery ||
          drive.hasFragment ||
          drive.path != '/api/v1/sessions/$sessionId/drive') {
        throw const BrokerException('invalid_response');
      }
    }
    Uri? usbUrl;
    // Older brokers grant usb-v1 to independent outputs without
    // telling the Host: without usb-tracks-v1 that grant is left unused.
    if (data['usbWebSocketUrl'] != null &&
        (!trackMedia || data['usbTracksProtocol'] == usbTracksProtocol)) {
      usbUrl = Uri.parse(data['usbWebSocketUrl'] as String);
      if (!brokerRelayMatches(usbUrl, origin) ||
          usbUrl.userInfo.isNotEmpty ||
          usbUrl.hasQuery ||
          usbUrl.hasFragment ||
          usbUrl.path != '/api/v1/sessions/$sessionId/usb') {
        throw const BrokerException('invalid_response');
      }
    }
    final options = data['options'] is Map<String, dynamic>
        ? StreamOptions.fromJson(data['options'] as Map<String, dynamic>)
        : requestedOptions ?? const StreamOptions();
    InitialViewportPreparation? initial;
    if (requestedInitialViewport != null) {
      try {
        if (!epochMedia ||
            trackMedia ||
            mode != 'stream' ||
            data['initialViewportProtocol'] != initialViewportProtocol ||
            data['viewportProvisioning'] != true ||
            options.monitor != '0' ||
            options.fps != requestedInitialViewport.fps ||
            requestedOptions == null ||
            requestedOptions.toJson().entries.any(
              (e) => options.toJson()[e.key] != e.value,
            )) {
          throw const FormatException();
        }
        initial = InitialViewportPreparation.fromJson(
          data['initialViewport'],
          sessionId: sessionId,
          requested: requestedInitialViewport,
        );
      } on FormatException {
        throw const BrokerException('invalid_response');
      }
    } else if (data['initialViewportProtocol'] != null ||
        data['initialViewport'] != null) {
      throw const BrokerException('invalid_response');
    }
    if (trackMedia) {
      if (requestedOptions == null ||
          requestedOptions.toJson().entries.any(
            (entry) => options.toJson()[entry.key] != entry.value,
          )) {
        throw const BrokerException('invalid_response');
      }
      requestedTracks!.checkOptions(options);
    }
    final ClipboardPolicy clipboardPolicy;
    try {
      clipboardPolicy = ClipboardPolicy.fromJson(data['clipboardPolicy']);
    } on FormatException {
      throw const BrokerException('invalid_response');
    }
    final SigninBinding? signin;
    try {
      if (data['signin'] != null) {
        if (!requestedSignin ||
            !epochMedia ||
            trackMedia ||
            options.monitor != '0' ||
            data['udpDirectProtocol'] != null) {
          throw const FormatException();
        }
        signin = SigninBinding.parse(data['signin']);
      } else {
        if (requestedSignin) throw const FormatException();
        signin = null;
      }
    } on FormatException {
      throw const BrokerException('invalid_response');
    }
    return BrokerConnection(
      sessionId: sessionId,
      signin: signin,
      webSocketUrl: uri,
      mode: mode,
      clipboardWebSocketUrl: clipboard,
      clipboardPolicy: clipboardPolicy,
      clipboardSync: data['clipboardSync'] == true,
      epochMedia: epochMedia,
      viewportProvisioning: data['viewportProvisioning'] == true,
      trackPlan: requestedTracks,
      profileTracks: requestedProfileTracks,
      initialViewport: initial,
      viewportEncoderLimits:
          requestedInitialViewport?.preserveEncoderLimits == true,
      options: options,
      bitrateAdaptive:
          data['bitrateProtocol'] == bitrateAdaptiveProtocol &&
          options.automaticBitrate,
      inputLatency: data['inputLatencyProtocol'] == inputLatencyProtocol,
      observability:
          requestedObservability &&
          mode == 'stream' &&
          data['observabilityProtocol'] == streamObservabilityProtocol,
      observabilityPath:
          requestedObservability &&
              mode == 'stream' &&
              data['observabilityProtocol'] == streamObservabilityProtocol
          ? parseObservabilityPath(data['observabilityPath'])
          : const {},
      observabilityVersions:
          requestedObservability &&
              mode == 'stream' &&
              data['observabilityProtocol'] == streamObservabilityProtocol
          ? parseObservabilityVersions(data['observabilityVersions'])
          : const {},
      cursor: data['cursorProtocol'] == cursorProtocol,
      cursorTracks: data['cursorTracksProtocol'] == cursorTracksProtocol,
      textInput: data['textInputProtocol'] == textInputProtocol,
      displayScaleControl: data['displayScaleProtocol'] == displayScaleProtocol,
      displayScale: data['displayScaleProtocol'] == displayScaleProtocol
          ? data['displayScale'] as int?
          : null,
      displayScaleOutputs:
          data['displayScaleOutputsProtocol'] == displayScaleOutputsProtocol,
      touchInput: data['touchInputProtocol'] == touchInputProtocol,
      caret: data['caretProtocol'] == caretProtocol,
      driveWebSocketUrl: drive,
      driveFs: driveFs == true,
      streamSize:
          [
            streamSizeProtocol,
            streamSizeProtocolV2,
          ].contains(data['streamSizeProtocol'])
          ? requestedStreamSize
          : null,
      streamSizeV2: data['streamSizeProtocol'] == streamSizeProtocolV2,
      hardwareDecode: requestedHardwareDecode,
      printWebSocketUrl: printUrl,
      usbWebSocketUrl: usbUrl,
      printRequested: requestedPrint && mode == 'stream',
      usbRequested: requestedUsb && mode == 'stream',
      udpMedia: requestedUdpMedia,
      udpTracks:
          requestedUdpTracks && data['udpTracksProtocol'] == udpTracksProtocol,
      audioOpus: data['audioCodecProtocol'] == audioOpusProtocol,
      relativeMouse: data['relativeMouseProtocol'] == relativeMouseProtocol,
      cursorEcho: data['cursorEchoProtocol'] == cursorEchoProtocol,
      cursorCompose: data['cursorComposeProtocol'] == cursorComposeProtocol,
      sessionResume: data['sessionResumeProtocol'] == sessionResumeProtocol,
      resumeGraceMs: validResumeGraceMs(data['resumeGraceMs']) ?? 15000,
      reservation:
          data['sessionCancellationProtocol'] == 'session-cancel-v1' &&
              cancel != null
          ? BrokerSessionReservation(() => cancel(sessionId))
          : null,
    );
  }
  final String sessionId;
  final SigninBinding? signin;
  final Uri webSocketUrl;
  final String mode;
  final StreamOptions options;
  final Uri? clipboardWebSocketUrl;
  final ClipboardPolicy clipboardPolicy;
  final bool clipboardSync;
  final bool epochMedia, viewportProvisioning;
  final bool viewportEncoderLimits;
  final bool profileTracks;
  final TrackPlan? trackPlan;
  final InitialViewportPreparation? initialViewport;
  final BrokerSessionReservation? reservation;

  /// The broker granted bitrate-adaptive-v1: the client reports video arrival
  /// times and the host's controller sets the rate.
  final bool bitrateAdaptive;

  /// The broker granted input-latency-v1: sampled input carries a sequence
  /// number and the host acknowledges injection and tags the next frame.
  final bool inputLatency;
  final bool observability;
  final Map<String, String> observabilityPath;
  final Map<String, String?> observabilityVersions;

  /// The broker granted cursor-v1: the host leaves the cursor out of the video
  /// and sends shape/visibility/position records (binary media kind 3).
  final bool cursor;

  /// The grant covers independent outputs (cursor-tracks-v1): the records
  /// arrive in TVT1 kind 3 on the track media, never with a position.
  final bool cursorTracks;

  /// The broker granted text-input-v1: committed text may be sent as
  /// `{"type":"input","event":"text"}` (see text_input.dart).
  final bool textInput;

  /// The broker granted runtime display-scale-v1 control. It may grant this
  /// without asking the Host to change the current Windows DPI on connection.
  final bool displayScaleControl;

  /// Optional initial Windows scale (percent), distinct from the runtime
  /// grant. A null value asks for no initial DPI change.
  final int? displayScale;

  /// The grant covers every owned output of independent outputs
  /// (display-scale-outputs-v1); results then carry each output's values.
  final bool displayScaleOutputs;

  /// The broker granted touch-input-v1: touch frames may be sent as
  /// `{"type":"input","event":"touch"}` (see touch_input.dart).
  final bool touchInput;

  /// The broker granted caret-v1: "caret.watch" may be sent and "caret"
  /// reports arrive (see caret_follow.dart).
  final bool caret;

  /// The broker granted drive-v1: this session's drive socket
  /// (drive_session.dart). Null when not granted.
  final Uri? driveWebSocketUrl;

  /// The drive grant includes the "fs" revision: the Host may also send
  /// `setsize` and `flush` requests. False with no drive grant.
  final bool driveFs;

  /// The broker granted stream-size-v1 with these declared decoder limits:
  /// full-size pictures within them, portrait outputs up to 2160x3840,
  /// mid-epoch picture changes and `stream.decoder` reports. Null otherwise.
  final DecoderLimits? streamSize;
  final bool streamSizeV2;
  final bool? hardwareDecode;

  /// The broker granted print-v1: this session's print socket, where the
  /// Host offers what is printed to its VM printer (see print_session.dart).
  final Uri? printWebSocketUrl;

  /// The broker granted usb-v1 (the assignment allows USB devices and the Host
  /// can attach them): the session's own USB channel. Null when not granted.
  final Uri? usbWebSocketUrl;

  /// This request asked for print-v1 / usb-v1, which the client does only
  /// where the Host advertises it through this broker. The broker grants a
  /// requested one exactly when the assignment allows it (negotiatePrint /
  /// negotiateUsb), so requested without a URL means "관리자 허용이 필요해요".
  final bool printRequested, usbRequested;

  bool get printDenied => printRequested && printWebSocketUrl == null;
  bool get usbDenied => usbRequested && usbWebSocketUrl == null;

  /// This request offered udp-media-v1. The grant, if any, comes later on the
  /// session's socket; a session that did not offer it ignores one.
  final bool udpMedia;
  final bool udpTracks;

  /// The broker granted audio-opus-v1: stream.info may announce Opus audio
  /// (audio_codec.dart), which the Windows runner and Android's engine decode.
  final bool audioOpus;

  /// The broker granted relative-mouse-v1: `moveRelative`, and buttons and
  /// wheel steps without a position, may be sent.
  final bool relativeMouse;

  /// The broker granted cursor-echo-v1: "cursor.echo" may be sent, and while
  /// it is on the Host sends the VM's real cursor position as it changes.
  final bool cursorEcho;

  /// The broker granted cursor-compose-v1: "cursor.compose" may be sent, and
  /// while it is on the Host draws the cursor into the video.
  final bool cursorCompose;

  /// The broker granted session-resume-v1: a lost socket is replaced within
  /// [resumeGraceMs] and the stream continues (session_resume.dart).
  final bool sessionResume;
  final int resumeGraceMs;
  Future<void> release() => reservation?.release() ?? Future<void>.value();

  /// A new release attempt if the earlier one failed (see
  /// [BrokerSessionReservation.retry]).
  Future<void> releaseAgain() => reservation?.retry() ?? Future<void>.value();
}

/// The connected server's version from its last desktop list (null: not
/// stated, an older server). Read by the client information window.
class BrokerServerVersion {
  String? value;
}

final brokerServerVersion = BrokerServerVersion();

abstract interface class BrokerApi {
  Future<VirtualDisplayState> configureDisplays(
    String token,
    String desktopId,
    int count,
  );
  Future<VirtualDisplayState> display(
    String token,
    String desktopId, {
    String? operationId,
  });
  Future<BrokerPowerResult> requestPower(
    String token,
    String desktopId,
    String action,
  );
  Future<BrokerPowerResult> power(
    String token,
    String desktopId, {
    String? taskId,
  });
  Future<BrokerSession> login(String username, String password);
  Future<List<BrokerDesktop>> desktops(String token);
  Future<void> logout(String token);
  Future<void> changePassword(
    String token,
    String currentPassword,
    String newPassword,
    String confirmation,
  );
  Future<BrokerConnection> connect(
    String token,
    String desktopId, {
    String mode = 'stream',
    StreamOptions? options,
  });
  void close();
}

abstract interface class ConsoleRecoveryApi {
  Future<ConsolePreparationState> prepareConsole(
    String token,
    String desktopId,
  );
  Future<ConsolePreparationState> consolePreparation(
    String token,
    String desktopId, {
    String? operationId,
  });
}

/// Administrator accounts only (the broker answers 403 otherwise): ask the
/// broker to update this desktop's Host to its own latest catalog release.
abstract interface class HostUpdateBrokerApi {
  Future<HostUpdateState> requestHostUpdate(
    String token,
    String desktopId,
    String version, {
    bool disconnectSessions = false,
  });
}

/// Desktop organising: this account's favourite mark for one assigned desktop
/// (PUT /api/v1/desktops/:id/favorite). A broker before it answers 404.
abstract interface class FavoriteBrokerApi {
  Future<bool> setFavorite(String token, String desktopId, bool favorite);
}

/// [displayScale]: a Windows scale for every owned output
/// (display-scale-outputs-v1), sent only where the inventory advertises it.
abstract interface class TrackBrokerApi {
  Future<BrokerConnection> connectTracks(
    String token,
    String desktopId,
    TrackPlan tracks,
    StreamOptions options, {
    int? displayScale,
  });
}

abstract interface class ProfileTrackBrokerApi {
  Future<BrokerConnection> connectProfileTracks(
    String token,
    String desktopId,
    TrackPlan tracks,
    StreamOptions options, {
    int? displayScale,
  });
}

/// host-ready-wait-v1 (restart recovery, 2026-11-03): after a broker restart
/// every stream ended as server_stopping and waits for its desktop's Host to
/// return. GET /api/v1/desktops/:id/host-ready answers the moment the Host has
/// reported to the new broker (true), or after the broker's hold (about 5 s)
/// with false. Null: the current list does not offer it (a released broker);
/// the caller keeps reading the list instead.
abstract interface class HostReadyBrokerApi {
  Future<bool?> waitHostReady(String token, String desktopId);
}

const hostReadyProtocol = 'host-ready-wait-v1';

/// Ends this VM's existing connection when the broker allows it (same account
/// or administrator). It only releases; the caller then connects normally.
abstract interface class TakeoverBrokerApi {
  Future<bool> takeover(String token, String desktopId);
}

/// Consumes exactly the offered controller set, rechecked by the broker.
/// A new client must never fall back to an empty legacy release request.
abstract interface class ConfirmedTakeoverBrokerApi {
  Future<bool> confirmTakeover(
    String token,
    String desktopId,
    StreamTakeoverOffer offer,
  );
}

/// Read-only occupancy check before preparing the Host's displays. A busy
/// answer carries the same context-bound offer as an ordinary admission.
abstract interface class StreamTakeoverPreflightBrokerApi {
  Future<void> checkStreamTakeover(String token, String desktopId);
}

/// display-scale-v1: a connection that also asks for a Windows display scale
/// where the inventory advertises it (otherwise the same as [BrokerApi.connect]
/// or [InitialViewportBrokerApi.connectViewport]).
abstract interface class DisplayScaleBrokerApi {
  Future<BrokerConnection> connectScaled(
    String token,
    String desktopId, {
    required StreamOptions options,
    InitialViewport? initialViewport,
    required int displayScale,
  });
}

abstract interface class InitialViewportBrokerApi {
  Future<BrokerConnection> connectViewport(
    String token,
    String desktopId,
    InitialViewport viewport,
    StreamOptions options,
  );
}

/// Optional explicit path only. Ordinary callers/fakes never opt into a secure
/// desktop phase, and no launcher entry is enabled by implementing this API.
abstract interface class SigninStreamBrokerApi {
  Future<BrokerConnection> connectSignin(
    String token,
    String desktopId, {
    required String availabilityId,
    StreamOptions? options,
    InitialViewport? initialViewport,
  });
}

/// Records a power action this client refused before sending it (VM state,
/// display change in progress), so the diagnostics show why nothing reached
/// the broker.
abstract interface class PowerDiagnosticsBrokerApi {
  void recordPowerRefused(
    String token,
    String desktopId,
    String action,
    String code,
  );
}

/// Sliding renewal of the signed-in session (POST /api/v1/auth/renew). The
/// broker keeps the SAME token and moves its expiry (up to an absolute
/// maximum), so every channel bound to the token keeps working. A broker
/// without the endpoint answers 404 and the caller signs in again silently.
abstract interface class SessionRenewalBrokerApi {
  Future<DateTime> renewSession(String token);

  /// Diagnostics (event signIn): [stage] 1 = renewal, 2 = silent sign-in with
  /// the remembered password; failures carry a fixed code, never a message.
  void recordSessionRenewal(
    String token, {
    required int stage,
    required bool ok,
    String? code,
  });
}

class BrokerConsoleGrant {
  static final _ticket = RegExp(r'^[A-Za-z0-9_-]{1,512}$');
  const BrokerConsoleGrant({
    required this.id,
    required this.ticket,
    required this.relay,
    required this.expiresAt,
  });
  final String id, ticket;
  final Uri relay;
  final DateTime expiresAt;
  static final _uuid = RegExp(
    r'^[a-f0-9]{8}-[a-f0-9]{4}-[a-f0-9]{4}-[a-f0-9]{4}-[a-f0-9]{12}$',
  );
  // Apply the transport boundary to direct grants as well as parsed replies.
  // No Bearer-bearing request may start before this exact origin/path check.
  bool validFor(Uri origin) =>
      _uuid.hasMatch(id) &&
      _ticket.hasMatch(ticket) &&
      expiresAt.isUtc &&
      expiresAt.isAfter(DateTime.now()) &&
      brokerOriginAllowed(origin) &&
      origin.host.isNotEmpty &&
      origin.userInfo.isEmpty &&
      !origin.hasQuery &&
      !origin.hasFragment &&
      ['', '/'].contains(origin.path) &&
      brokerRelayMatches(relay, origin) &&
      relay.userInfo.isEmpty &&
      !relay.hasQuery &&
      !relay.hasFragment &&
      relay.path == '/relay/console/$id';

  factory BrokerConsoleGrant.fromJson(Map<String, dynamic> value, Uri origin) {
    final id = value['id'],
        ticket = value['ticket'],
        route = value['relayPath'];
    final expiry = value['expiresAt'];
    if (id is! String ||
        !_uuid.hasMatch(id) ||
        ticket is! String ||
        !_ticket.hasMatch(ticket) ||
        route != '/relay/console/$id' ||
        expiry is! String) {
      throw const BrokerException('invalid_response');
    }
    final expiresAt = DateTime.tryParse(expiry);
    if (expiresAt == null || !expiresAt.isUtc) {
      throw const BrokerException('invalid_response');
    }
    final grant = BrokerConsoleGrant(
      id: id,
      ticket: ticket,
      relay: origin.replace(
        scheme: brokerRelayScheme(origin),
        path: route as String,
      ),
      expiresAt: expiresAt,
    );
    if (!grant.validFor(origin)) {
      throw const BrokerException('invalid_response');
    }
    return grant;
  }
}

abstract interface class EmergencyConsoleBrokerApi {
  Future<BrokerConsoleGrant> createConsole(String token, String desktopId);
  Future<void> releaseConsole(String token, String desktopId, String id);
}

class BrokerConsoleWebViewGrant {
  const BrokerConsoleWebViewGrant({
    required this.path,
    required this.code,
    required this.expiresAt,
  });
  final String path, code;
  final DateTime expiresAt;

  static bool desktopIdValid(String value) =>
      RegExp(r'^[a-zA-Z0-9][a-zA-Z0-9_-]{0,79}$').hasMatch(value);

  bool validFor(Uri origin, String desktopId, {DateTime? now}) {
    final clock = now ?? DateTime.now();
    return origin.scheme == 'https' &&
        origin.host.isNotEmpty &&
        origin.userInfo.isEmpty &&
        !origin.hasQuery &&
        !origin.hasFragment &&
        (origin.path.isEmpty || origin.path == '/') &&
        origin.port > 0 &&
        origin.port <= 65535 &&
        desktopIdValid(desktopId) &&
        path == '/console/$desktopId/attach' &&
        RegExp(r'^[A-Za-z0-9_-]{43}$').hasMatch(code) &&
        expiresAt.isAfter(clock) &&
        !expiresAt.isAfter(clock.add(const Duration(minutes: 2)));
  }

  factory BrokerConsoleWebViewGrant.fromJson(
    Map<String, dynamic> value,
    Uri origin,
    String desktopId, {
    DateTime? now,
  }) {
    final expiry = value['expiresAt'];
    if (value.length != 4 ||
        value['protocol'] != 'console-webview-v1' ||
        value['path'] is! String ||
        value['code'] is! String ||
        expiry is! String) {
      throw const BrokerException('invalid_response');
    }
    final parsed = DateTime.tryParse(expiry);
    if (parsed == null) throw const BrokerException('invalid_response');
    final grant = BrokerConsoleWebViewGrant(
      path: value['path'] as String,
      code: value['code'] as String,
      expiresAt: parsed,
    );
    if (!grant.validFor(origin, desktopId, now: now)) {
      throw const BrokerException('invalid_response');
    }
    return grant;
  }
}

/// 'Console 끊고 연결': ends the VM's console connections so a
/// stream can start.
abstract interface class ConsoleTakeoverBrokerApi {
  Future<int> endConsoles(String token, String desktopId);
}

abstract interface class ConsoleWebViewBrokerApi {
  Future<BrokerConsoleWebViewGrant> createConsoleWebView(
    String token,
    String desktopId,
  );
  Future<void> closeConsoleWebViews(
    String token,
    String desktopId, {
    String? ownerId,
  });
}

/// Explicit user submission; ordinary login remains safe automatic recovery.
abstract interface class InteractiveLoginBrokerApi {
  Future<BrokerSession> loginInteractively(String username, String password);
}

abstract interface class LoginConfirmationBrokerApi {
  Future<BrokerSession> confirmLogin(LoginConfirmation challenge);
}

abstract interface class SessionActivityBrokerApi {
  Future<SessionAccess> recordHumanActivity(String token);
  Future<SessionAccess> setIdleEnabled(String token, bool enabled);
}

abstract interface class MfaBrokerApi {
  Future<BrokerSession> verifyMfa(
    MfaChallenge challenge, {
    String? code,
    String? recoveryCode,
  });
}

class HttpBrokerApi
    implements
        BrokerApi,
        InteractiveLoginBrokerApi,
        LoginConfirmationBrokerApi,
        SessionActivityBrokerApi,
        MfaBrokerApi,
        MfaAccountBrokerApi,
        EmergencyConsoleBrokerApi,
        ConsoleWebViewBrokerApi,
        ConsoleTakeoverBrokerApi,
        SessionRenewalBrokerApi,
        ConsoleRecoveryApi,
        HostUpdateBrokerApi,
        FavoriteBrokerApi,
        TrackBrokerApi,
        ProfileTrackBrokerApi,
        TakeoverBrokerApi,
        ConfirmedTakeoverBrokerApi,
        StreamTakeoverPreflightBrokerApi,
        InitialViewportBrokerApi,
        SigninStreamBrokerApi,
        DisplayScaleBrokerApi,
        PowerDiagnosticsBrokerApi,
        DesktopPreviewBrokerApi,
        NoticeBrokerApi,
        NoticeRemovalBrokerApi,
        HostReadyBrokerApi {
  HttpBrokerApi({
    required this.origin,
    HttpClient? client,
    HttpClient Function()? cleanupClientFactory,
    this.diagnosticsDesktopId,
    this.diagnosticsMode,
    this.diagnosticsVersion,
    this.enableDiagnostics = false,
    this.requestObservability,
    this.requestTextInput = false,
    this.requestDisplayScaleControl = false,
    this.requestTouchInput = false,
    this.requestCaret = false,
    this.requestDrives = false,
    Future<DecoderLimits?> Function()? decoderLimits,
    Future<DecoderAdmission> Function(List<StreamSourceSize>, int)?
    streamAdmission,
    this.requestPrint = false,
    this.requestUsb = false,
    this.requestUdpMedia = false,
    this.requestAudioOpus = false,
    this.audioOpusDecodable,
    this.requestRelativeMouse = false,
    this.requestSessionResume = false,
    this.requestPcLoginReplacement = false,
    this.requestStreamTakeoverContext = false,
  }) : _http = client ?? HttpClient(),
       _cleanupClientFactory = cleanupClientFactory ?? HttpClient.new,
       decoderLimits = decoderLimits ?? nativeDecoderLimits,
       streamAdmission =
           streamAdmission ??
           ((sources, fps) => nativeStreamAdmission(sources, fps)) {
    try {
      _transportToken = BrokerTransport.token(origin);
    } catch (_) {
      try {
        _http.close(force: true);
      } catch (_) {}
      throw ArgumentError('A confirmed broker origin is required.');
    }
    if (!brokerOriginAllowed(origin) ||
        origin.host.isEmpty ||
        origin.userInfo.isNotEmpty ||
        origin.hasQuery ||
        origin.hasFragment ||
        !['', '/'].contains(origin.path)) {
      throw ArgumentError('A HTTPS server origin is required.');
    }
    _http.connectionTimeout = const Duration(seconds: 10);
  }
  @override
  final Uri origin;
  final bool requestPcLoginReplacement;
  final bool requestStreamTakeoverContext;
  late final Object? _transportToken;
  bool _transportReady = false;
  @override
  Future<BrokerConsoleGrant> createConsole(
    String token,
    String desktopId,
  ) async {
    if (!RegExp(r'^pve-[0-9]{1,12}$').hasMatch(desktopId)) {
      throw const BrokerException('desktop_not_found', 404);
    }
    final value = await _request(
      '/api/v1/desktops/$desktopId/console-sessions',
      method: 'POST',
      token: token,
      data: <String, Object?>{},
    );
    final grant = value['consoleSession'];
    if (grant is! Map<String, dynamic>) {
      throw const BrokerException('invalid_response');
    }
    return BrokerConsoleGrant.fromJson(grant, origin);
  }

  @override
  Future<BrokerConsoleWebViewGrant> createConsoleWebView(
    String token,
    String desktopId,
  ) async {
    if (origin.scheme != 'https') {
      throw const BrokerException('console_browser_https_required');
    }
    if (!BrokerConsoleWebViewGrant.desktopIdValid(desktopId)) {
      throw const BrokerException('desktop_not_found', 404);
    }
    try {
      final value = await _request(
        '/api/v1/desktops/$desktopId/console-webview',
        method: 'POST',
        token: token,
        data: <String, Object?>{},
      );
      return BrokerConsoleWebViewGrant.fromJson(value, origin, desktopId);
    } on BrokerException catch (error) {
      if ((error.status == 404 && error.code != 'desktop_not_found') ||
          error.status == 405) {
        throw const BrokerException('console_webview_update_required');
      }
      rethrow;
    }
  }

  @override
  Future<void> closeConsoleWebViews(
    String token,
    String desktopId, {
    String? ownerId,
  }) async {
    if (origin.scheme != 'https') {
      throw const BrokerException('console_browser_https_required');
    }
    if (!BrokerConsoleWebViewGrant.desktopIdValid(desktopId)) return;
    if (ownerId != null && !RegExp(r'^[a-f0-9]{64}$').hasMatch(ownerId)) {
      throw const BrokerException('invalid_console_webview_owner');
    }
    await _request(
      '/api/v1/desktops/$desktopId/console-webviews/close',
      method: 'POST',
      token: token,
      data: <String, Object?>{'ownerId': ?ownerId},
    );
  }

  @override
  Future<void> releaseConsole(String token, String desktopId, String id) async {
    if (!RegExp(r'^pve-[0-9]{1,12}$').hasMatch(desktopId) ||
        !BrokerConsoleGrant._uuid.hasMatch(id)) {
      throw const BrokerException('invalid_response');
    }
    final value = await _request(
      '/api/v1/desktops/$desktopId/console-sessions/$id',
      method: 'DELETE',
      token: token,
    );
    if (value['released'] != true || value['id'] != id) {
      throw const BrokerException('invalid_response');
    }
  }

  final HttpClient _http;
  final HttpClient Function() _cleanupClientFactory;
  final String? diagnosticsDesktopId, diagnosticsMode;

  /// The running platform's native package version. An omitted provider
  /// retains the Windows EXE version; a provider returning null stays unknown.
  final String? Function()? diagnosticsVersion;
  final bool enableDiagnostics;
  final bool Function()? requestObservability;
  final Set<String> _observabilityDesktops = {};
  bool _observabilityRequested(String token, String id) {
    try {
      return (requestObservability?.call() ?? StreamObservability.enabled) &&
          _inventoryToken == token &&
          _observabilityDesktops.contains(id);
    } catch (_) {
      return false;
    }
  }

  /// Ask for text-input-v1 where the inventory advertises it (a client whose
  /// keyboard commits composed text, e.g. Android or a nested Windows remote
  /// keyboard). Physical keys remain ordinary key messages.
  final bool requestTextInput;

  /// Windows session windows negotiate runtime DPI control without requesting
  /// an initial DPI value. Other clients retain their existing opt-in path.
  final bool requestDisplayScaleControl;

  /// Ask for touch-input-v1 where the inventory advertises it (a client with
  /// a touch screen that sends touch contacts, e.g. Android's direct touch).
  /// Mouse-only clients never ask.
  final bool requestTouchInput;

  /// Ask for caret-v1 where the inventory advertises it (a client with a soft
  /// keyboard that follows the text caret, e.g. Android).
  final bool requestCaret;

  /// Ask for drive-v1 where the inventory advertises it (a client that can
  /// share local folders: the Windows session windows). Others never ask.
  final bool requestDrives;

  /// stream-size-v1: this device's decoder limits (null = unknown, then the
  /// released safe default is declared).
  final Future<DecoderLimits?> Function() decoderLimits;
  final Future<DecoderAdmission> Function(List<StreamSourceSize>, int)
  streamAdmission;
  Future<DecoderAdmission> _capacity(
    List<StreamSourceSize> sources,
    int fps,
  ) async {
    try {
      return await streamAdmission(sources, fps);
    } on StreamCapacityException catch (error) {
      throw BrokerException("decoder_${error.reason}", 409);
    }
  }

  /// Ask for print-v1 where the inventory advertises it (a client that prints
  /// or saves what the VM sends to its printer, see print_session.dart).
  final bool requestPrint;

  /// Ask for usb-v1 where the inventory advertises it (a client that can lend
  /// this PC's USB devices to the VM). The grant also needs the administrator's
  /// per-assignment usbDevices permission.
  final bool requestUsb;

  /// Ask for udp-media-v1 where the inventory advertises it (Windows and
  /// Android, which both run the native UDP leg and fall back to the WebSocket).
  final bool requestUdpMedia;

  /// Ask for audio-opus-v1 where the inventory advertises it (a Windows session
  /// window, whose runner decodes Opus, and Android, whose engine does).
  final bool requestAudioOpus;

  /// Read at each connect: false once this device's engine found no Opus
  /// decoder it can play (Android: none listed, none made, no usable output
  /// format). The connection then asks for PCM, which every Host sends, so the
  /// sound comes back instead of staying silent. Null: always decodable.
  final bool Function()? audioOpusDecodable;

  /// Ask for relative-mouse-v1 where the inventory advertises it (a Windows
  /// session window; Android keeps its own pointer).
  final bool requestRelativeMouse;

  /// Ask for session-resume-v1 where the inventory advertises it (a Windows
  /// session window; Android reconnects on its own network events).
  final bool requestSessionResume;
  ClientDiagnostics? _diagnostics;
  String? _diagnosticToken;
  bool _closed = false;
  void _startDiagnostics(String token) {
    if (!enableDiagnostics || _closed || _diagnosticToken == token) return;
    _diagnostics?.close();
    _diagnosticToken = token;
    _streamStatsAccepted = _streamStallAccepted = _diagnosticsAcknowledged =
        false;
    _heldStats.clear();
    _heldStalls.clear();
    _diagnostics = ClientDiagnostics(
      desktopId: diagnosticsDesktopId,
      mode: diagnosticsMode,
      version: diagnosticsVersion,
      send: (report) async {
        if (_closed || _diagnosticToken != token) {
          return DiagnosticDelivery.unavailable;
        }
        try {
          final ack = await _requestRaw(
            '/api/v1/client/diagnostics',
            method: 'POST',
            token: token,
            data: report,
          );
          final accepted =
              ack['accepted'] == true &&
              ack['protocol'] == report['protocol'] &&
              ack['runId'] == report['runId'] &&
              ack['sequence'] == report['sequence'];
          // Only a broker that says so accepts streamStats rows; a released
          // one rejects the whole batch. The reporter isolates rejected rows.
          if (accepted && _diagnosticToken == token) {
            _streamStatsAccepted =
                ack['streamStatsProtocol'] == streamStatsProtocol;
            _streamStallAccepted =
                ack['streamStallProtocol'] == streamStallProtocol;
            final held = List.of(_heldStats);
            _heldStats.clear();
            if (_streamStatsAccepted) {
              for (final stats in held) {
                _recordStats(stats.values, stats.sessionId, stats.desktopId);
              }
            }
            _diagnosticsAcknowledged = true;
            final stalls = List.of(_heldStalls);
            _heldStalls.clear();
            if (_streamStallAccepted) {
              for (final stall in stalls) {
                _recordStall(stall.values, stall.sessionId, stall.desktopId);
              }
            }
          }
          return accepted
              ? DiagnosticDelivery.accepted
              : DiagnosticDelivery.retry;
        } on BrokerException catch (error) {
          if ((error.status == 400 &&
                  error.code == 'invalid_client_diagnostics') ||
              error.status == 413) {
            return DiagnosticDelivery.rejected;
          }
          return [400, 401, 403, 404, 405, 428].contains(error.status)
              ? DiagnosticDelivery.unavailable
              : DiagnosticDelivery.retry;
        }
      },
    );
  }

  bool _streamStatsAccepted = false, _streamStallAccepted = false;

  /// This diagnostics run has had an acknowledgement (accepted upload).
  bool _diagnosticsAcknowledged = false;

  /// Windows produced before this run's first acknowledgement; sent once the
  /// broker says it accepts them, dropped if it does not (bounded).
  final _heldStats =
      <({Map<String, Object> values, String sessionId, String desktopId})>[];

  /// Stall rows produced before this run's first acknowledgement (a session's
  /// first seconds); sent once the broker acknowledges stream-stall-v1,
  /// dropped if that acknowledgement does not name it (bounded).
  final _heldStalls =
      <({Map<String, Object> values, String sessionId, String desktopId})>[];

  /// One streaming statistics window for the administrator's diagnostics.
  /// Only a broker that acknowledged [streamStatsProtocol] receives it.
  void recordStreamStats(
    Map<String, Object> values, {
    required String sessionId,
    required String desktopId,
  }) {
    if (_closed || !enableDiagnostics) return;
    if (!_streamStatsAccepted) {
      if (_heldStats.length < 4) {
        _heldStats.add((
          values: values,
          sessionId: sessionId,
          desktopId: desktopId,
        ));
      }
      return;
    }
    _recordStats(values, sessionId, desktopId);
  }

  /// One stream-stall-v1 event. Only a broker that acknowledged it receives it;
  /// before this run's first acknowledgement it is held (see [_heldStalls]).
  void recordStreamStall(
    Map<String, Object> values, {
    required String sessionId,
    required String desktopId,
  }) {
    if (_closed || !enableDiagnostics) return;
    if (!_diagnosticsAcknowledged) {
      if (_heldStalls.length < 8) {
        _heldStalls.add((
          values: values,
          sessionId: sessionId,
          desktopId: desktopId,
        ));
      }
      return;
    }
    if (_streamStallAccepted) _recordStall(values, sessionId, desktopId);
  }

  void _recordStall(
    Map<String, Object> values,
    String sessionId,
    String desktopId,
  ) {
    _diagnostics?.record(
      'streamStall',
      values: values,
      sessionId: sessionId,
      desktop: desktopId,
      connectionMode: 'stream',
    );
  }

  void _recordStats(
    Map<String, Object> values,
    String sessionId,
    String desktopId,
  ) {
    _diagnostics?.record(
      'streamStats',
      values: values,
      sessionId: sessionId,
      desktop: desktopId,
      connectionMode: 'stream',
    );
  }

  Future<void> flushDiagnostics() async {
    try {
      await Future<void>(() async {
        final logger = _diagnostics;
        // At most the bounded queue; final disconnect records should not be
        // stranded behind an already in-flight upload.
        for (var i = 0; i < 10 && logger?.pending == true; i++) {
          await logger!.flush();
          if (logger.retryPending) break;
        }
      }).timeout(const Duration(seconds: 2));
    } catch (_) {
      /* Closing a window never depends on diagnostics delivery. */
    }
  }

  Future<Map<String, dynamic>> _request(
    String path, {
    String method = 'GET',
    String? token,
    Map<String, Object?>? data,
    PcLoginIntent? loginIntent,
    void Function(String?)? onPcPolicy,
  }) async {
    if (token != null && path != '/api/v1/auth/logout') {
      _startDiagnostics(token);
    }
    final watch = Stopwatch()..start();
    // Power requests (submit: stage 5, status/task poll: stage 6) are recorded
    // too, so a refused or failed power action has its fixed code.
    final target = RegExp(
      r'^/api/v1/desktops/([A-Za-z0-9_-]+)(?:/(connect|display|console-preparation|power)(?:/(?:operations|tasks)/([a-fA-F0-9-]{36}))?)?$',
    ).firstMatch(path);
    final kind = target?.group(2);
    final event = kind == 'display'
        ? 'displayConfigure'
        : path == '/api/v1/desktops'
        ? 'inventory'
        : 'brokerRequest';
    final selectedMode = data?['mode'] is String
        ? data!['mode'] as String
        : null;
    void observe(
      Map<String, dynamic>? value, {
      BrokerException? error,
      String? requestId,
    }) {
      final logger = _diagnosticToken == token ? _diagnostics : null;
      if (logger == null || (target == null && path != '/api/v1/desktops')) {
        return;
      }
      final display = value?['display'];
      // A Windows session takeover answers in consolePreparation; its failure
      // (e.g. windows_session_locked) was recorded as ok before.
      final console = value?['consolePreparation'];
      final operation = display is Map
          ? display['operation']
          : console is Map
          ? console['operation']
          : null;
      final Object? powerTask = kind == 'power' ? (value?['powerTask']) : null;
      final powerFailed =
          powerTask is Map &&
          powerTask['status'] == 'stopped' &&
          powerTask['exitStatus'] != 'OK';
      final failed =
          powerFailed ||
          (operation is Map &&
              ['failed', 'unknown'].contains(operation['status']));
      final failureCode =
          (diagnosticCodes.contains(error?.reason) ? error?.reason : null) ??
          error?.code ??
          (powerFailed
              ? 'power_task_failed'
              : failed && operation['error'] is String
              ? operation['error'] as String
              : failed
              ? (display is Map
                    ? 'display_operation_failed'
                    : 'console_transfer_failed')
              : null);
      if (kind == 'connect' && error == null) {
        logger.bindSession(value?['sessionId'], desktop: target?.group(1));
      }
      // A new admission belongs to its request, not the previously successful
      // session on this desktop. Keep that binding for its late feature/end
      // events; only this observation declines implicit session inheritance.
      final connectionSession = kind == 'connect'
          ? (value?['sessionId'])
          : null;
      logger.record(
        event,
        desktop: target?.group(1),
        connectionMode: selectedMode,
        operationId: operation is Map ? operation['id'] : target?.group(3),
        requestId: requestId,
        sessionId: connectionSession is String ? connectionSession : null,
        inheritSession: kind != 'connect',
        code: failureCode,
        values: {
          'ok': error == null && !failed,
          'elapsedMs': watch.elapsedMilliseconds.clamp(0, 0xffffffff),
          'stage': kind == 'connect'
              ? 3
              : kind == 'display'
              ? 2
              : kind == 'console-preparation'
              ? 1
              : kind == 'power'
              ? (method == 'POST' ? 5 : 6)
              : 0,
          if (data?['count'] is int) 'action': data!['count'],
          if (kind == 'power' && powerActionCodes[data?['action']] != null)
            'action': powerActionCodes[data?['action']],
          if (operation is Map && operation['requestedCount'] is int)
            'action': operation['requestedCount'],
          if (display is Map && display['activeCount'] is int)
            'monitors': display['activeCount'],
          if (operation is Map) 'pending': operation['status'] == 'pending',
          if (error != null) 'error': error.status,
        },
      );
    }

    String? requestId;
    try {
      final value = await _requestRaw(
        path,
        method: method,
        token: token,
        data: data,
        loginIntent: loginIntent,
        onPcPolicy: onPcPolicy,
        onRequestId: _diagnostics == null ? null : (value) => requestId = value,
      );
      observe(value, requestId: requestId);
      return value;
    } on BrokerException catch (error) {
      observe(null, error: error, requestId: requestId);
      rethrow;
    }
  }

  // The token whose last inventory the per-desktop capability sets below
  // were read from.
  String? _inventoryToken;
  Map<String, SigninAvailability> _signinOffers = {};
  // bitrate-adaptive-v1 ceilings from the last inventory of this token. A
  // desktop absent here is treated as a released peer (fixed 4-20 Mbps).
  Map<String, int> _bitrateCeilings = {};
  int? _bitrateCeiling(String token, String desktopId) =>
      _inventoryToken == token ? _bitrateCeilings[desktopId] : null;
  // input-latency-v1 is requested only from a broker whose inventory shows the
  // host capability: a released broker rejects the unknown connect field.
  Set<String> _inputLatencyDesktops = {};
  bool _inputLatencyAdvertised(String token, String desktopId) =>
      _inventoryToken == token && _inputLatencyDesktops.contains(desktopId);
  // cursor-v1: the same rule (a released broker rejects the unknown field).
  Set<String> _cursorDesktops = {};
  bool _cursorAdvertised(String token, String desktopId) =>
      _inventoryToken == token && _cursorDesktops.contains(desktopId);
  // cursor-tracks-v1: independent outputs ask for the separated cursor only
  // where the current inventory shows both markers.
  Set<String> _cursorTracksDesktops = {};
  bool _cursorTracksAdvertised(String token, String desktopId) =>
      _inventoryToken == token && _cursorTracksDesktops.contains(desktopId);
  // text-input-v1: the same rule (a released broker rejects the unknown field).
  Set<String> _textInputDesktops = {};
  bool _textInputAdvertised(String token, String desktopId) =>
      _inventoryToken == token && _textInputDesktops.contains(desktopId);
  // display-scale-v1: the same rule (a released broker rejects the fields).
  Set<String> _displayScaleDesktops = {};
  bool _displayScaleAdvertised(String token, String desktopId) =>
      _inventoryToken == token && _displayScaleDesktops.contains(desktopId);
  Set<String> _displayScaleOutputsDesktops = {};
  // display-scale-outputs-v1: independent outputs ask for runtime control
  // (and a remembered scale) only where this window opted in to DPI control
  // and the current inventory shows the capability.
  bool _displayScaleOutputsRequested(String token, String desktopId) =>
      requestDisplayScaleControl &&
      _inventoryToken == token &&
      _displayScaleOutputsDesktops.contains(desktopId);
  // touch-input-v1: the same rule (a released broker rejects the field).
  Set<String> _touchInputDesktops = {};
  bool _touchInputAdvertised(String token, String desktopId) =>
      _inventoryToken == token && _touchInputDesktops.contains(desktopId);
  // caret-v1: the same rule (a released broker rejects the field).
  Set<String> _caretDesktops = {};
  bool _caretAdvertised(String token, String desktopId) =>
      _inventoryToken == token && _caretDesktops.contains(desktopId);
  // print-v1: the same rule (a released broker rejects the field).
  Set<String> _printDesktops = {};
  bool _printAdvertised(String token, String desktopId) =>
      _inventoryToken == token && _printDesktops.contains(desktopId);
  // usb-v1: the same rule (a released broker rejects the field).
  Set<String> _usbDesktops = {};
  bool _usbAdvertised(String token, String desktopId) =>
      _inventoryToken == token && _usbDesktops.contains(desktopId);
  // udp-media-v1: the same rule (a released broker rejects the field).
  Set<String> _udpMediaDesktops = {};
  Set<String> _udpDirectDesktops = {};
  Set<String> _udpTracksDesktops = {};
  bool _udpMediaRequested(String token, String desktopId) =>
      requestUdpMedia &&
      _inventoryToken == token &&
      _udpMediaDesktops.contains(desktopId);
  // audio-opus-v1: the same rule (a released broker rejects the field).
  Set<String> _audioOpusDesktops = {};
  bool _audioOpusRequested(String token, String desktopId) =>
      requestAudioOpus &&
      (audioOpusDecodable?.call() ?? true) &&
      _inventoryToken == token &&
      _audioOpusDesktops.contains(desktopId);
  // audio-opus-tracks-v1: independent outputs ask only where the Host shows it.
  Set<String> _audioOpusTracksDesktops = {};
  bool _audioOpusTracksRequested(String token, String desktopId) =>
      _audioOpusRequested(token, desktopId) &&
      _audioOpusTracksDesktops.contains(desktopId);
  // relative-mouse-v1: the same rule (a released broker rejects the field).
  Set<String> _relativeMouseDesktops = {};
  bool _relativeMouseRequested(String token, String desktopId) =>
      requestRelativeMouse &&
      _inventoryToken == token &&
      _relativeMouseDesktops.contains(desktopId);
  // cursor-echo-v1: the same rule (a released broker rejects the field).
  Set<String> _cursorEchoDesktops = {};
  bool _cursorEchoAdvertised(String token, String desktopId) =>
      _inventoryToken == token && _cursorEchoDesktops.contains(desktopId);
  // cursor-compose-v1: the same rule.
  Set<String> _cursorComposeDesktops = {};
  // desktop-preview-v1: desktops whose current list entry offers a picture.
  Set<String> _previewDesktops = {};

  @override
  bool previewOffered(String token, String desktopId) =>
      _inventoryToken == token && _previewDesktops.contains(desktopId);

  /// The launcher picture of one assigned VM or PC. Not recorded in
  /// diagnostics (the path matches none of the observed routes) and never
  /// cached here.
  @override
  Future<DesktopPreview> desktopPreview(String token, String desktopId) async {
    if (!previewOffered(token, desktopId) ||
        !RegExp(
          r'^(?:pve-[1-9][0-9]{0,11}|pc-[a-f0-9]{8}-[a-f0-9]{4}-[a-f0-9]{4}-[a-f0-9]{4}-[a-f0-9]{12})$',
        ).hasMatch(desktopId)) {
      return const DesktopPreview.unavailable();
    }
    final value = await _request(
      '/api/v1/desktops/$desktopId/preview',
      token: token,
    );
    return DesktopPreview.parse(value);
  }

  // ---- notice-v1 (notices.dart): this account's notices, and the ones the
  // person saw. Neither is recorded in diagnostics (no observed route matches)
  // and a broker without them answers 404, which the caller takes as "none".
  @override
  Future<List<BrokerNotice>> notices(String token) async {
    final value = await _request('/api/v1/notices', token: token);
    final list = BrokerNotice.parseList(value);
    // 지우기 only where this answer offered it (an older broker's does not).
    if (BrokerNotice.removalOffered(value)) {
      _noticeRemovalToken = token;
    } else if (_noticeRemovalToken == token) {
      _noticeRemovalToken = null;
    }
    return list;
  }

  String? _noticeRemovalToken;
  @override
  bool noticeRemovalOffered(String token) => _noticeRemovalToken == token;

  /// Never sent to a broker whose list answer did not offer it.
  @override
  Future<void> removeNotices(String token, List<String> ids) async {
    if (ids.isEmpty) return;
    if (!noticeRemovalOffered(token)) {
      throw const BrokerException('notice_remove_unavailable');
    }
    await _request(
      '/api/v1/notices/remove',
      method: 'POST',
      token: token,
      data: {'ids': ids},
    );
  }

  @override
  Future<void> markNoticesSeen(String token, List<String> ids) async {
    if (ids.isEmpty) return;
    await _request(
      '/api/v1/notices/seen',
      method: 'POST',
      token: token,
      data: {'ids': ids},
    );
  }

  // host-ready-wait-v1: desktops whose current list entry offers the wait.
  Set<String> _hostReadyDesktops = {};

  @override
  Future<bool?> waitHostReady(String token, String desktopId) async {
    if (_inventoryToken != token || !_hostReadyDesktops.contains(desktopId)) {
      return null;
    }
    final value = await _request(
      '/api/v1/desktops/${Uri.encodeComponent(desktopId)}/host-ready',
      token: token,
    );
    if (value['protocol'] != hostReadyProtocol || value['ready'] is! bool) {
      throw const BrokerException('invalid_response');
    }
    return value['ready'] as bool;
  }

  bool _cursorComposeAdvertised(String token, String desktopId) =>
      _inventoryToken == token && _cursorComposeDesktops.contains(desktopId);
  // 게임 마우스 on independent outputs (relative-mouse-tracks-v1): movement with
  // the composed cursor, asked with the tracks cursor; the broker grants it
  // only for a Host that takes it there.
  bool _gameMouseTracksRequested(String token, String desktopId, bool cursor) =>
      cursor &&
      _relativeMouseRequested(token, desktopId) &&
      _cursorComposeAdvertised(token, desktopId);
  // session-resume-v1: the same rule (a released broker rejects the field).
  Set<String> _sessionResumeDesktops = {};
  bool _sessionResumeRequested(String token, String desktopId) =>
      requestSessionResume &&
      _inventoryToken == token &&
      _sessionResumeDesktops.contains(desktopId);
  // drive-v1: the same rule (a released broker rejects the field; the broker
  // shows it only where this account's assignment allows drives).
  Set<String> _driveDesktops = {};
  bool _driveRequested(String token, String desktopId) =>
      requestDrives &&
      _inventoryToken == token &&
      _driveDesktops.contains(desktopId);
  // The "fs" revision of drive-v1 (driveFs), only where the Host advertises it.
  Set<String> _driveFsDesktops = {};
  bool _driveFsAdvertised(String token, String desktopId) =>
      _driveRequested(token, desktopId) && _driveFsDesktops.contains(desktopId);

  // stream-size-v1: the same rule (a released broker rejects the fields).
  Set<String> _streamSizeDesktops = {};
  Set<String> _streamSizeV2Desktops = {};
  bool _streamSizeAdvertised(String token, String desktopId) =>
      _inventoryToken == token && _streamSizeDesktops.contains(desktopId);
  bool _streamSizeV2Advertised(String token, String desktopId) =>
      _inventoryToken == token && _streamSizeV2Desktops.contains(desktopId);

  /// A stale inventory (host downgraded or broker replaced) gets its explicit
  /// preflight refusal once; the released request is then sent instead.
  static bool _bitrateRefused(BrokerException error, StreamOptions sent) =>
      (sent.automaticBitrate ||
          sent.bitrate > StreamOptions.legacyMaximumBitrate) &&
      ((error.status == 400 && error.code == 'invalid_stream_options') ||
          (error.status == 409 && error.code == 'stream_bitrate_unavailable'));
  int _inventorySequence = 0;
  Future<Map<String, dynamic>> _requestRaw(
    String path, {
    String method = 'GET',
    String? token,
    Map<String, Object?>? data,
    void Function(String?)? onRequestId,
    HttpClient? client,
    PcLoginIntent? loginIntent,
    void Function(String?)? onPcPolicy,
  }) async {
    HttpClientRequest? request;
    var responseStatus = 0;
    try {
      if (_closed && client == null) {
        throw const BrokerException('operation_cancelled');
      }
      BrokerTransport.requireCurrent(origin, _transportToken);
      if (origin.scheme == 'http' && !_transportReady) {
        await ensureBrokerTransport(
          origin,
          client: client ?? _http,
          force: true,
        );
      }
      BrokerTransport.requireCurrent(origin, _transportToken);
      if (_closed && client == null) {
        throw const BrokerException('operation_cancelled');
      }
      _transportReady = true;
      request = await (client ?? _http)
          .openUrl(method, origin.resolve(path))
          .timeout(const Duration(seconds: 12));
      BrokerTransport.requireCurrent(origin, _transportToken);
      if (_closed && client == null) {
        request.abort();
        throw const BrokerException('operation_cancelled');
      }
      request.followRedirects = false;
      request.headers.set(HttpHeaders.acceptHeader, 'application/json');
      request.headers.set(brokerWireHeader, brokerWireVersion);
      // The server's few human sentences come in the app's language.
      request.headers.set(HttpHeaders.acceptLanguageHeader, acceptLanguage);
      if (path == '/api/v1/desktops' ||
          RegExp(r'^/api/v1/desktops/[A-Za-z0-9_-]{1,80}$').hasMatch(path)) {
        request.headers.set('X-Multitap-Signin-Protocol', signinStreamProtocol);
      }
      if (path == '/api/v1/auth/login') {
        request.headers.set('X-Multitap-Auth-Protocol', 'mfa-v1');
        if (requestPcLoginReplacement &&
            loginIntent == PcLoginIntent.confirmable) {
          request.headers.set('X-Multitap-Login-Context', 'v1');
        }
      }
      final takeoverTarget = RegExp(
        r'^/api/v1/desktops/([A-Za-z0-9_-]{1,80})/(connect|takeover|takeover-context)$',
      ).firstMatch(path);
      if ((requestStreamTakeoverContext &&
              const {
                'connect',
                'takeover-context',
              }.contains(takeoverTarget?.group(2))) ||
          (takeoverTarget?.group(2) == 'takeover' &&
              data?['context'] is String)) {
        request.headers.set(streamTakeoverHeader, 'v1');
      }
      if (requestPcLoginReplacement && loginIntent != null) {
        request.headers.set(pcSessionPolicyHeader, windowsPcSessionPolicy);
        request.headers.set(pcLoginIntentHeader, loginIntent.wire);
      }
      if (token != null) {
        request.headers.set(HttpHeaders.authorizationHeader, 'Bearer $token');
      }
      if (data != null) {
        request.headers.contentType = ContentType.json;
        request.write(jsonEncode(data));
      }
      final response = await request.close().timeout(
        const Duration(seconds: 15),
      );
      responseStatus = response.statusCode;
      onRequestId?.call(response.headers.value('x-request-id'));
      final bytes = <int>[];
      await for (final chunk in response.timeout(const Duration(seconds: 15))) {
        if (bytes.length + chunk.length > 1048576) {
          throw BrokerException('invalid_response', responseStatus);
        }
        bytes.addAll(chunk);
      }
      // A reverse proxy can answer an oversized diagnostic POST with HTML.
      // Its definitive 413 still isolates this batch instead of retrying it
      // forever as a malformed acknowledgement. Other endpoints keep theirs.
      if (path == '/api/v1/client/diagnostics' && response.statusCode == 413) {
        throw const BrokerException('invalid_client_diagnostics', 413);
      }
      final value = jsonDecode(utf8.decode(bytes)) as Map<String, dynamic>;
      if (path == '/api/v1/auth/login' && response.statusCode == 202) {
        if (origin.scheme != 'https') {
          throw const BrokerException('mfa_requires_https', 403);
        }
        if (value.keys.length != 1 || !value.containsKey('mfa')) {
          throw const BrokerException('invalid_response', 202);
        }
        throw MfaRequiredException(
          MfaChallenge.fromJson(
            value['mfa'],
            origin: origin,
            pcLoginIntent: requestPcLoginReplacement ? loginIntent : null,
          ),
        );
      }
      if ((path == '/api/v1/auth/login' ||
              path == '/api/v1/auth/login/confirm' ||
              path == '/api/v1/auth/mfa/verify') &&
          response.statusCode >= 200 &&
          response.statusCode < 300 &&
          response.statusCode != 200) {
        throw BrokerException('invalid_response', response.statusCode);
      }
      if (response.statusCode < 200 || response.statusCode >= 300) {
        final error = value['error'] as Map<String, dynamic>?;
        final rawDetails = error?['details'];
        if ((path == '/api/v1/auth/login' ||
                path == '/api/v1/auth/mfa/verify') &&
            requestPcLoginReplacement &&
            loginIntent == PcLoginIntent.confirmable &&
            response.statusCode == 409 &&
            error?['code'] == 'login_confirmation_required') {
          try {
            throw LoginConfirmationRequired(
              LoginConfirmation.parse(rawDetails, origin, DateTime.now()),
            );
          } on FormatException {
            if (path == '/api/v1/auth/mfa/verify') {
              throw const MfaChallengeRetiredException();
            }
            throw const BrokerException('invalid_response', 409);
          }
        }
        if (path == '/api/v1/auth/login' &&
            requestPcLoginReplacement &&
            loginIntent == PcLoginIntent.confirmable &&
            response.statusCode == 400) {
          throw const BrokerException('session_access_update_required', 400);
        }
        final details = rawDetails as Map?;
        final takeover = details?['takeover'];
        StreamTakeoverOffer? offer;
        if (requestStreamTakeoverContext &&
            const {
              'connect',
              'takeover-context',
            }.contains(takeoverTarget?.group(2)) &&
            response.statusCode == 409 &&
            error?['code'] == 'desktop_busy' &&
            takeover == true &&
            details?.containsKey('takeoverOffer') == true) {
          try {
            offer = StreamTakeoverOffer.parse(
              details!['takeoverOffer'],
              expectedDesktopId: takeoverTarget?.group(1),
            );
          } on FormatException {
            throw BrokerException('invalid_response', response.statusCode);
          }
        }
        throw BrokerException(
          error?['code'] as String? ??
              (_gateway(response.statusCode)
                  ? 'server_restarting'
                  : 'request_failed'),
          response.statusCode,
          _safeReason(details?['reason']),
          takeover is bool ? takeover : null,
          offer,
        );
      }
      if (onPcPolicy != null &&
          requestPcLoginReplacement &&
          loginIntent != null) {
        String? accepted;
        try {
          if (response.headers.value(pcSessionPolicyHeader) ==
              windowsPcSessionPolicy) {
            accepted = windowsPcSessionPolicy;
          }
        } catch (_) {
          // Absent, duplicated or malformed ACK never establishes a policy.
        }
        onPcPolicy(accepted);
      }
      return value;
    } on BrokerException {
      rethrow;
    } on BrokerProbeException catch (error) {
      throw BrokerException(
        error.code.startsWith('http_') ||
                const {'origin_not_allowed', 'setup_only'}.contains(error.code)
            ? error.code
            : 'network_error',
        error.status ?? 0,
        error.code,
      );
    } on TimeoutException {
      request?.abort();
      throw const BrokerException('network_error', 0, 'timeout');
    } on HandshakeException {
      request?.abort();
      throw const BrokerException('network_error', 0, 'tls');
    } on SocketException catch (error) {
      request?.abort();
      throw BrokerException('network_error', 0, brokerSocketFailureCode(error));
    } on IOException catch (error) {
      request?.abort();
      // The broker went away mid-request ("Connection closed before full
      // header was received"): a restart, like a reset socket.
      throw BrokerException(
        'network_error',
        0,
        error is HttpException && error.message.startsWith('Connection closed')
            ? 'connection_reset'
            : null,
      );
    } on FormatException {
      try {
        request?.abort();
      } catch (_) {
        /* Preserve the original refusal. */
      }
      if (origin.scheme == 'http' &&
          !BrokerTransport.current(origin, _transportToken)) {
        throw const BrokerException('http_consent');
      }
      throw BrokerException(_unparsed(responseStatus), responseStatus);
    } on TypeError {
      throw BrokerException(_unparsed(responseStatus), responseStatus);
    }
  }

  /// A proxy's own 502-504 page (no broker JSON) while the broker restarts.
  static bool _gateway(int status) =>
      status == 502 || status == 503 || status == 504;
  static String _unparsed(int status) =>
      _gateway(status) ? 'server_restarting' : 'invalid_response';

  static String? _safeReason(dynamic reason) =>
      reason is String && RegExp(r'^[a-z_]{1,64}$').hasMatch(reason)
      ? reason
      : null;

  Future<void> _cancelConnection(
    String token,
    String sessionId,
    String desktopId,
  ) async {
    // A late connect reply may arrive after this API/window has closed. Use
    // a separately bounded client for this exact same-origin cleanup only.
    final client = _cleanupClientFactory()
      ..connectionTimeout = const Duration(seconds: 3);
    final watch = Stopwatch()..start();
    var released = false;
    BrokerException? failure;
    try {
      final result = await _requestRaw(
        '/api/v1/sessions/$sessionId',
        method: 'DELETE',
        token: token,
        client: client,
      ).timeout(const Duration(seconds: 3));
      if (result['sessionId'] != sessionId || result['released'] != true) {
        throw const BrokerException('invalid_response');
      }
      released = true;
    } on BrokerException catch (error) {
      // The relay already removed it (the session's own socket close ends it
      // first): nothing is held, which is exactly what this release wanted.
      if (error.status == 404 && error.code == 'session_not_found') {
        released = true;
        return;
      }
      failure = error;
      rethrow;
    } on TimeoutException {
      failure = const BrokerException('network_error');
      rethrow;
    } finally {
      client.close(force: true);
      if (!_closed && _diagnosticToken == token) {
        _diagnostics?.record(
          'brokerRequest',
          sessionId: sessionId,
          desktop: desktopId,
          code: failure?.code,
          values: {
            'ok': released,
            'stage': 4,
            'elapsedMs': watch.elapsedMilliseconds.clamp(0, 0xffffffff),
            if (failure != null) 'error': failure.status,
          },
        );
      }
    }
  }

  VirtualDisplayState _displayResponse(
    Map<String, dynamic> data, {
    String? operationId,
    bool requireOperation = false,
  }) {
    try {
      if (data['display'] is! Map<String, dynamic>) {
        throw const FormatException();
      }
      return VirtualDisplayState.fromJson(
        data['display'] as Map<String, dynamic>,
        operationId: operationId,
        requireOperation: requireOperation,
      );
    } on FormatException {
      throw const BrokerException('invalid_response');
    } on TypeError {
      throw const BrokerException('invalid_response');
    }
  }

  @override
  Future<VirtualDisplayState> configureDisplays(
    String token,
    String desktopId,
    int count,
  ) async {
    if (count != 1 && count != 2) {
      throw const BrokerException('invalid_display_count');
    }
    return _displayResponse(
      await _request(
        '/api/v1/desktops/${Uri.encodeComponent(desktopId)}/display',
        method: 'POST',
        token: token,
        data: {'count': count},
      ),
      requireOperation: true,
    );
  }

  @override
  Future<ConsolePreparationState> prepareConsole(
    String token,
    String desktopId,
  ) async => _consoleResponse(
    await _request(
      '/api/v1/desktops/${Uri.encodeComponent(desktopId)}/console-preparation',
      method: 'POST',
      token: token,
      data: {'confirmation': 'resume-windows-session'},
    ),
    requireOperation: true,
  );

  @override
  Future<ConsolePreparationState> consolePreparation(
    String token,
    String desktopId, {
    String? operationId,
  }) async {
    if (operationId != null &&
        !virtualDisplayOperationId.hasMatch(operationId)) {
      throw const BrokerException('invalid_response');
    }
    return _consoleResponse(
      await _request(
        '/api/v1/desktops/${Uri.encodeComponent(desktopId)}/console-preparation${operationId == null ? '' : '/operations/$operationId'}',
        token: token,
      ),
      operationId: operationId,
      requireOperation: operationId != null,
    );
  }

  @override
  Future<HostUpdateState> requestHostUpdate(
    String token,
    String desktopId,
    String version, {
    bool disconnectSessions = false,
  }) async {
    if (!RegExp(r'^[0-9]{1,6}\.[0-9]{1,6}\.[0-9]{1,6}$').hasMatch(version)) {
      throw const BrokerException('invalid_host_update_version');
    }
    final value = await _request(
      '/api/v1/desktops/${Uri.encodeComponent(desktopId)}/host-update',
      method: 'POST',
      token: token,
      data: {'version': version, 'disconnectSessions': disconnectSessions},
    );
    final state = HostUpdateState.fromJson(value['hostUpdate']);
    if (!state.present || state.operation?.targetVersion != version) {
      throw const BrokerException('invalid_response');
    }
    return state;
  }

  @override
  Future<bool> setFavorite(
    String token,
    String desktopId,
    bool favorite,
  ) async {
    if (!RegExp(r'^[A-Za-z0-9_-]{1,128}$').hasMatch(desktopId)) {
      throw const BrokerException('desktop_not_found', 404);
    }
    final value = await _request(
      '/api/v1/desktops/${Uri.encodeComponent(desktopId)}/favorite',
      method: 'PUT',
      token: token,
      data: {'favorite': favorite},
    );
    final answer = value['favorite'];
    if (answer is! bool) throw const BrokerException('invalid_response');
    return answer;
  }

  ConsolePreparationState _consoleResponse(
    Map<String, dynamic> value, {
    String? operationId,
    bool requireOperation = false,
  }) {
    try {
      return ConsolePreparationState.fromJson(
        value['consolePreparation'],
        operationId: operationId,
        requireOperation: requireOperation,
      );
    } on FormatException {
      throw const BrokerException('invalid_response');
    }
  }

  /// Explicit candidate negotiation. Never retry a rejected track request as
  /// a composite session, or let a broker upgrade an ordinary request silently.
  @override
  Future<BrokerConnection> connectTracks(
    String token,
    String desktopId,
    TrackPlan tracks,
    StreamOptions requested, {
    int? displayScale,
  }) async {
    if (displayScale != null && !validDisplayScale(displayScale)) {
      throw ArgumentError('Invalid display scale.');
    }
    // Independent outputs keep a fixed total rate (up to the host ceiling).
    var options = requested.negotiated(
      _bitrateCeiling(token, desktopId),
      independentOutputs: true,
    );
    tracks.checkOptions(options);
    final v2 = _streamSizeV2Advertised(token, desktopId);
    final admission = v2
        ? await _capacity([
            for (final t in tracks.outputs) StreamSourceSize(t.width, t.height),
          ], options.fps)
        : null;
    final size = admission?.limits;
    if (!v2 && !tracks.legacyCompatible) {
      throw const BrokerException('stream_size_unavailable', 409);
    }
    if (v2 && !_streamSizeV2Advertised(token, desktopId)) {
      throw const BrokerException('stream_size_unavailable', 409);
    }
    final drive = _driveRequested(token, desktopId);
    final observe = _observabilityRequested(token, desktopId);
    final udp =
        _udpMediaRequested(token, desktopId) &&
        _udpTracksDesktops.contains(desktopId);
    final scaleOutputs = _displayScaleOutputsRequested(token, desktopId);
    final scale = scaleOutputs ? displayScale : null;
    final cursor = _cursorTracksAdvertised(token, desktopId);
    final game = _gameMouseTracksRequested(token, desktopId, cursor);
    // usb-v1 as in an ordinary stream (released brokers accept the field here).
    final usb = requestUsb && _usbAdvertised(token, desktopId);
    // audio-opus-tracks-v1 and session-resume-v1 (tracks-resume-v1 on the Host)
    // as in an ordinary stream: the broker grants each only where it can.
    final opus = _audioOpusTracksRequested(token, desktopId);
    final resume = _sessionResumeRequested(token, desktopId);
    Future<Map<String, dynamic>> request() => _request(
      '/api/v1/desktops/${Uri.encodeComponent(desktopId)}/connect',
      method: 'POST',
      token: token,
      data: {
        'mode': 'stream',
        if (observe) 'observabilityProtocol': streamObservabilityProtocol,
        'mediaProtocol': 'tracks-v1',
        'tracks': tracks.toJson(),
        if (usb) 'usbProtocol': usbProtocol,
        if (cursor) ...{
          'cursorProtocol': cursorProtocol,
          'cursorTracksProtocol': cursorTracksProtocol,
        },
        if (game) ...{
          'relativeMouseProtocol': relativeMouseProtocol,
          'cursorComposeProtocol': cursorComposeProtocol,
        },
        if (v2) ...{
          'streamSizeProtocol': streamSizeProtocolV2,
          'decoderLimits': size!.toJson(),
        },
        'options': options.toJson(),
        'clipboardProtocol': 'clipboard-sync-v1',
        if (drive) 'driveProtocol': driveProtocol,
        if (udp) 'udpMediaProtocol': udpMediaProtocol,
        if (udp) 'udpTracksProtocol': udpTracksProtocol,
        if (udp && _udpDirectDesktops.contains(desktopId))
          'udpDirectProtocol': udpDirectProtocol,
        if (opus) 'audioCodecProtocol': audioOpusProtocol,
        if (resume) 'sessionResumeProtocol': sessionResumeProtocol,
        if (scaleOutputs) ...{
          'displayScaleProtocol': displayScaleProtocol,
          'displayScaleOutputsProtocol': displayScaleOutputsProtocol,
          'displayScale': ?scale,
        },
      },
    );
    Map<String, dynamic> data;
    try {
      data = await request();
    } on BrokerException catch (error) {
      if (!_bitrateRefused(error, options)) rethrow;
      options = requested.negotiated(null, independentOutputs: true);
      data = await request();
    }
    return BrokerConnection.fromJson(
      data,
      origin,
      requestedOptions: options,
      requestedTracks: tracks,
      requestedObservability: observe,
      requestedStreamSize: size,
      requestedStreamSizeV2: v2,
      requestedHardwareDecode: admission?.hardwareDecode,
      requestedDrive: drive,
      requestedUsb: usb,
      requestedUdpMedia: udp,
      requestedUdpTracks: udp,
      requestedDisplayScaleControl: scaleOutputs,
      requestedDisplayScale: scale,
      requestedDisplayScaleOutputs: scaleOutputs,
      requestedCursor: cursor,
      requestedCursorTracks: cursor,
      requestedRelativeMouse: game,
      requestedCursorCompose: game,
      requestedAudioOpus: opus,
      requestedSessionResume: resume,
      cancel: (id) => _cancelConnection(token, id, desktopId),
    );
  }

  @override
  Future<BrokerConnection> connectProfileTracks(
    String token,
    String desktopId,
    TrackPlan tracks,
    StreamOptions requested, {
    int? displayScale,
  }) async {
    if (displayScale != null && !validDisplayScale(displayScale)) {
      throw ArgumentError('Invalid display scale.');
    }
    var options = requested.negotiated(
      _bitrateCeiling(token, desktopId),
      independentOutputs: true,
    );
    tracks.checkOptions(options);
    final v2 = _streamSizeV2Advertised(token, desktopId);
    final admission = v2
        ? await _capacity([
            for (final t in tracks.outputs) StreamSourceSize(t.width, t.height),
          ], options.fps)
        : null;
    final size = admission?.limits;
    if (!v2 && !tracks.legacyCompatible) {
      throw const BrokerException('stream_size_unavailable', 409);
    }
    if (v2 && !_streamSizeV2Advertised(token, desktopId)) {
      throw const BrokerException('stream_size_unavailable', 409);
    }
    final drive = _driveRequested(token, desktopId);
    final observe = _observabilityRequested(token, desktopId);
    final udp =
        _udpMediaRequested(token, desktopId) &&
        _udpTracksDesktops.contains(desktopId);
    final scaleOutputs = _displayScaleOutputsRequested(token, desktopId);
    final scale = scaleOutputs ? displayScale : null;
    final cursor = _cursorTracksAdvertised(token, desktopId);
    final game = _gameMouseTracksRequested(token, desktopId, cursor);
    final usb = requestUsb && _usbAdvertised(token, desktopId);
    // audio-opus-tracks-v1 and session-resume-v1 (tracks-resume-v1 on the Host)
    // as in an ordinary stream: the broker grants each only where it can.
    final opus = _audioOpusTracksRequested(token, desktopId);
    final resume = _sessionResumeRequested(token, desktopId);
    Future<Map<String, dynamic>> request() => _request(
      '/api/v1/desktops/${Uri.encodeComponent(desktopId)}/connect',
      method: 'POST',
      token: token,
      data: {
        'mode': 'stream',
        if (observe) 'observabilityProtocol': streamObservabilityProtocol,
        'mediaProtocol': 'tracks-v1',
        'streamProfileTracksProtocol': profileTracksProtocol,
        'tracks': tracks.toJson(),
        if (usb) 'usbProtocol': usbProtocol,
        if (cursor) ...{
          'cursorProtocol': cursorProtocol,
          'cursorTracksProtocol': cursorTracksProtocol,
        },
        if (game) ...{
          'relativeMouseProtocol': relativeMouseProtocol,
          'cursorComposeProtocol': cursorComposeProtocol,
        },
        if (v2) ...{
          'streamSizeProtocol': streamSizeProtocolV2,
          'decoderLimits': size!.toJson(),
        },
        'options': options.toJson(),
        'clipboardProtocol': 'clipboard-sync-v1',
        if (drive) 'driveProtocol': driveProtocol,
        if (udp) 'udpMediaProtocol': udpMediaProtocol,
        if (udp) 'udpTracksProtocol': udpTracksProtocol,
        if (udp && _udpDirectDesktops.contains(desktopId))
          'udpDirectProtocol': udpDirectProtocol,
        if (opus) 'audioCodecProtocol': audioOpusProtocol,
        if (resume) 'sessionResumeProtocol': sessionResumeProtocol,
        if (scaleOutputs) ...{
          'displayScaleProtocol': displayScaleProtocol,
          'displayScaleOutputsProtocol': displayScaleOutputsProtocol,
          'displayScale': ?scale,
        },
      },
    );
    Map<String, dynamic> data;
    try {
      data = await request();
    } on BrokerException catch (error) {
      if (!_bitrateRefused(error, options)) rethrow;
      options = requested.negotiated(null, independentOutputs: true);
      data = await request();
    }
    return BrokerConnection.fromJson(
      data,
      origin,
      requestedOptions: options,
      requestedTracks: tracks,
      requestedObservability: observe,
      requestedStreamSize: size,
      requestedStreamSizeV2: v2,
      requestedHardwareDecode: admission?.hardwareDecode,
      requestedProfileTracks: true,
      requestedDrive: drive,
      requestedUsb: usb,
      requestedUdpMedia: udp,
      requestedUdpTracks: udp,
      requestedDisplayScaleControl: scaleOutputs,
      requestedDisplayScale: scale,
      requestedDisplayScaleOutputs: scaleOutputs,
      requestedCursor: cursor,
      requestedCursorTracks: cursor,
      requestedRelativeMouse: game,
      requestedCursorCompose: game,
      requestedAudioOpus: opus,
      requestedSessionResume: resume,
      cancel: (id) => _cancelConnection(token, id, desktopId),
    );
  }

  @override
  Future<VirtualDisplayState> display(
    String token,
    String desktopId, {
    String? operationId,
  }) async {
    if (operationId != null &&
        !virtualDisplayOperationId.hasMatch(operationId)) {
      throw const BrokerException('invalid_response');
    }
    return _displayResponse(
      await _request(
        '/api/v1/desktops/${Uri.encodeComponent(desktopId)}/display${operationId == null ? '' : '/operations/$operationId'}',
        token: token,
      ),
      operationId: operationId,
      requireOperation: operationId != null,
    );
  }

  @override
  void recordPowerRefused(
    String token,
    String desktopId,
    String action,
    String code,
  ) {
    final logger = _diagnosticToken == token ? _diagnostics : null;
    logger?.record(
      'brokerRequest',
      desktop: desktopId,
      code: code,
      values: {
        'ok': false,
        'stage': 5,
        'elapsedMs': 0,
        'action': ?powerActionCodes[action],
      },
    );
  }

  /// A step of a connection the person asked for that makes no request of
  /// its own, so that a wait there leaves a record (without it a two-minute
  /// wait between a disconnect and the next connection on Android leaves
  /// nothing recorded). Stages: 20 연결 pressed, 21 its question answered
  /// (ok: agreed), 22 opening, 23 the notification step done, 24 the remote
  /// view opened, 25 its preparation started, 26 the remote view closed (ok:
  /// the desktop can be connected again), 27 연결 pressed and not taken
  /// (error: 1 a question is open, 2 another is opening, 3 an automatic
  /// connection waits, 4 the desktop is not ready), 29 the app's screen stood
  /// still for elapsedMs (error: 1 the platform side, 2 the Flutter side),
  /// 30-37 the remote view's own (AndroidRemoteView.onConnectStep).
  void recordConnectStep(
    String token,
    String? desktopId,
    int stage, {
    bool ok = true,
    int elapsedMs = 0,
    int reason = 0,
  }) {
    final logger = _diagnosticToken == token ? _diagnostics : null;
    logger?.record(
      'brokerRequest',
      desktop: desktopId,
      inheritSession: false,
      values: {
        'ok': ok,
        'stage': stage,
        'elapsedMs': elapsedMs.clamp(0, 0xffffffff),
        if (reason != 0) 'error': reason,
      },
    );
  }

  @override
  Future<BrokerPowerResult> requestPower(
    String token,
    String desktopId,
    String action,
  ) async {
    if (!clientVmPowerActions.contains(action)) {
      throw const BrokerException('invalid_power_action');
    }
    return BrokerPowerResult.fromJson(
      await _request(
        '/api/v1/desktops/${Uri.encodeComponent(desktopId)}/power',
        method: 'POST',
        token: token,
        data: {'action': action},
      ),
      desktopId: desktopId,
      requireTask: true,
    );
  }

  @override
  Future<BrokerPowerResult> power(
    String token,
    String desktopId, {
    String? taskId,
  }) async {
    if (taskId != null && !RegExp(r'^[0-9a-fA-F-]{36}$').hasMatch(taskId)) {
      throw const BrokerException('invalid_response');
    }
    return BrokerPowerResult.fromJson(
      await _request(
        '/api/v1/desktops/${Uri.encodeComponent(desktopId)}/power${taskId == null ? '' : '/tasks/${Uri.encodeComponent(taskId)}'}',
        token: token,
      ),
      desktopId: desktopId,
      taskId: taskId,
      requireTask: taskId != null,
    );
  }

  @override
  Future<BrokerSession> login(String username, String password) =>
      _loginWithIntent(username, password, PcLoginIntent.recovery);

  @override
  Future<BrokerSession> loginInteractively(String username, String password) =>
      requestPcLoginReplacement
      ? _loginWithIntent(username, password, PcLoginIntent.confirmable)
      : login(username, password);

  @override
  Future<BrokerSession> confirmLogin(LoginConfirmation challenge) async {
    if (!requestPcLoginReplacement) {
      challenge.forget();
      throw const BrokerException('session_access_update_required', 400);
    }
    String token;
    try {
      token = challenge.consume(origin, DateTime.now());
    } on FormatException {
      throw const BrokerException('login_confirmation_expired', 410);
    }
    String? accepted;
    final elapsed = Stopwatch()..start();
    final value = await _request(
      '/api/v1/auth/login/confirm',
      method: 'POST',
      data: {'challengeToken': token, 'confirm': true},
      loginIntent: PcLoginIntent.confirmable,
      onPcPolicy: (policy) => accepted = policy,
    );
    return BrokerSession.fromJson(
      value,
      responseElapsed: elapsed.elapsed,
    ).withPcSessionPolicy(accepted);
  }

  @override
  Future<SessionAccess> recordHumanActivity(String token) async {
    final elapsed = Stopwatch()..start();
    final value = await _request(
      '/api/v1/auth/activity',
      method: 'POST',
      token: token,
      data: const {},
    );
    try {
      return SessionAccess.parse(value, responseElapsed: elapsed.elapsed);
    } on FormatException {
      throw const BrokerException('invalid_response');
    }
  }

  @override
  Future<SessionAccess> setIdleEnabled(String token, bool enabled) async {
    final elapsed = Stopwatch()..start();
    final value = await _request(
      '/api/v1/auth/idle',
      method: 'PUT',
      token: token,
      data: {'enabled': enabled},
    );
    try {
      return SessionAccess.parse(value, responseElapsed: elapsed.elapsed);
    } on FormatException {
      throw const BrokerException('invalid_response');
    }
  }

  Future<BrokerSession> _loginWithIntent(
    String username,
    String password,
    PcLoginIntent intent,
  ) async {
    String? accepted;
    final elapsed = Stopwatch()..start();
    final value = await _request(
      '/api/v1/auth/login',
      method: 'POST',
      data: {'username': username, 'password': password},
      loginIntent: intent,
      onPcPolicy: (policy) => accepted = policy,
    );
    return BrokerSession.fromJson(
      value,
      responseElapsed: elapsed.elapsed,
    ).withPcSessionPolicy(accepted);
  }

  @override
  Future<BrokerSession> verifyMfa(
    MfaChallenge challenge, {
    String? code,
    String? recoveryCode,
  }) async {
    if (origin.scheme != 'https' || challenge.origin != origin) {
      throw const BrokerException('mfa_requires_https', 403);
    }
    if (challenge.remaining == Duration.zero) {
      challenge.forget();
      throw const BrokerException('mfa_challenge_expired', 410);
    }
    if ((code == null) == (recoveryCode == null)) {
      throw const BrokerException('invalid_mfa_fields', 400);
    }
    final normalizedRecovery = recoveryCode == null ? null : normalizeMfaRecoveryCode(recoveryCode);
    if (recoveryCode != null && normalizedRecovery == null) { throw const BrokerException('invalid_mfa_fields', 400); }
    try {
      String? accepted;
      final session = BrokerSession.fromJson(
        await _request(
          '/api/v1/auth/mfa/verify',
          method: 'POST',
          data: {
            'challengeToken': challenge.token,
            'code': ?code,
            'recoveryCode': ?normalizedRecovery,
          },
          loginIntent: challenge.pcLoginIntent,
          onPcPolicy: (policy) => accepted = policy,
        ),
      );
      challenge.forget();
      return session.withPcSessionPolicy(accepted);
    } on BrokerException catch (error) {
      if (error is MfaChallengeRetiredException ||
          error.code == 'mfa_challenge_expired' ||
          error.code == 'login_confirmation_required') {
        // A successful factor proof may continue into a distinct login
        // replacement question. Its old MFA token is already consumed.
        challenge.forget();
      }
      rethrow;
    }
  }

  Future<T> _mfaAccountRequest<T>(String token, String suffix,
      T Function(Map<String, dynamic>) decode, {Map<String, Object>? data}) async {
    if (origin.scheme != 'https') { throw const BrokerException('mfa_requires_https', 403); }
    if (token.isEmpty) { throw const BrokerException('authentication_required', 401); }
    final value = await _request('/api/v1/auth/mfa$suffix',
        method: data == null ? 'GET' : 'POST', token: token, data: data);
    try { return decode(value); }
    on FormatException { throw const BrokerException('invalid_response'); }
  }
  void _mfaCurrentPassword(String value) {
    if (value.isEmpty || value.length > 256) { throw const BrokerException('invalid_mfa_fields', 400); }
  }
  @override
  Future<MfaAccountStatus> readMfaAccount(String token) =>
      _mfaAccountRequest(token, '', MfaAccountStatus.parse);
  @override
  Future<MfaEnrollment> setupMfaAccount(String token, String currentPassword) {
    _mfaCurrentPassword(currentPassword);
    return _mfaAccountRequest(token, '/setup', MfaEnrollment.parse,
        data: {'currentPassword': currentPassword});
  }
  @override
  Future<MfaAccountChange> enableMfaAccount(String token, MfaEnrollment enrollment, String code) {
    if (enrollment.remaining == Duration.zero) { throw const BrokerException('mfa_challenge_expired', 410); }
    if (!RegExp(r'^[0-9]{6}$').hasMatch(code)) { throw const BrokerException('invalid_mfa_fields', 400); }
    return _mfaAccountRequest(token, '/enable',
        (value) => MfaAccountChange.parse(value, MfaAccountAction.enable),
        data: {'setupToken': enrollment.token, 'code': code});
  }
  @override
  Future<void> reauthenticateMfaAccount(String token, {required String currentPassword, String? code, String? recoveryCode}) async {
    _mfaCurrentPassword(currentPassword);
    if ((code == null) == (recoveryCode == null)) { throw const BrokerException('invalid_mfa_fields', 400); }
    if (code != null && !RegExp(r'^[0-9]{6}$').hasMatch(code)) { throw const BrokerException('invalid_mfa_fields', 400); }
    final normalizedRecovery = recoveryCode == null ? null : normalizeMfaRecoveryCode(recoveryCode);
    if (recoveryCode != null && normalizedRecovery == null) { throw const BrokerException('invalid_mfa_fields', 400); }
    await _mfaAccountRequest<bool>(token, '/reauth', (value) {
      if (value.length != 1 || value['ok'] != true) { throw const FormatException('Invalid MFA reauthentication.'); }
      return true;
    }, data: {'currentPassword': currentPassword, 'code': ?code, 'recoveryCode': ?normalizedRecovery});
  }
  @override
  Future<MfaAccountChange> disableMfaAccount(String token, String currentPassword) {
    _mfaCurrentPassword(currentPassword);
    return _mfaAccountRequest(token, '/disable',
        (value) => MfaAccountChange.parse(value, MfaAccountAction.disable),
        data: {'currentPassword': currentPassword});
  }
  @override
  Future<MfaAccountChange> regenerateMfaRecovery(String token, String currentPassword) {
    _mfaCurrentPassword(currentPassword);
    return _mfaAccountRequest(token, '/recovery',
        (value) => MfaAccountChange.parse(value, MfaAccountAction.regenerate),
        data: {'currentPassword': currentPassword});
  }

  @override
  Future<DateTime> renewSession(String token) async {
    final value = (await _request(
      '/api/v1/auth/renew',
      method: 'POST',
      token: token,
    ))['expiresAt'];
    final expiry = value is String ? DateTime.tryParse(value) : null;
    if (expiry == null) throw const BrokerException('invalid_response');
    return expiry;
  }

  @override
  void recordSessionRenewal(
    String token, {
    required int stage,
    required bool ok,
    String? code,
  }) {
    final logger = _diagnosticToken == token ? _diagnostics : null;
    logger?.record(
      'signIn',
      code: ok ? null : code,
      values: {'ok': ok, 'stage': stage},
    );
  }

  @override
  Future<List<BrokerDesktop>> desktops(String token) async {
    final sequence = ++_inventorySequence;
    final response = await _request('/api/v1/desktops', token: token);
    final inventory = response['desktops'] as List;
    // The server states its version beside the list (the info window's
    // 서버 section); where it says nothing the version stays unknown.
    final server = response['server'];
    final version = server is Map ? server['version'] : null;
    brokerServerVersion.value =
        version is String && RegExp(r'^\d+\.\d+\.\d+$').hasMatch(version)
        ? version
        : null;
    final desktops = inventory
        .map((value) => BrokerDesktop.fromJson(value as Map<String, dynamic>))
        .toList();
    if (sequence == _inventorySequence) {
      _inventoryToken = token;
      _signinOffers = {
        for (final desktop in desktops)
          if (!desktop.canStream &&
              desktop.hostConnected == true &&
              desktop.signinAvailability?.fresh(DateTime.now()) == true)
            desktop.id: desktop.signinAvailability!,
      };
      _bitrateCeilings = {
        for (final desktop in desktops)
          if (desktop.maxBitrate != null) desktop.id: desktop.maxBitrate!,
      };
      _observabilityDesktops
        ..clear()
        ..addAll(desktops.where((d) => d.observability).map((d) => d.id));
      _inputLatencyDesktops = {
        for (final desktop in desktops)
          if (desktop.inputLatency) desktop.id,
      };
      _cursorDesktops = {
        for (final desktop in desktops)
          if (desktop.cursor) desktop.id,
      };
      _cursorTracksDesktops = {
        for (final desktop in desktops)
          if (desktop.cursorTracks) desktop.id,
      };
      _textInputDesktops = {
        for (final desktop in desktops)
          if (desktop.textInput) desktop.id,
      };
      _displayScaleDesktops = {
        for (final desktop in desktops)
          if (desktop.displayScale) desktop.id,
      };
      _displayScaleOutputsDesktops = {
        for (final desktop in desktops)
          if (desktop.displayScaleOutputs) desktop.id,
      };
      _touchInputDesktops = {
        for (final desktop in desktops)
          if (desktop.touchInput) desktop.id,
      };
      _caretDesktops = {
        for (final desktop in desktops)
          if (desktop.caret) desktop.id,
      };
      _driveDesktops = {
        for (final desktop in desktops)
          if (desktop.drive) desktop.id,
      };
      _driveFsDesktops = {
        for (final desktop in desktops)
          if (desktop.drive && desktop.driveFs) desktop.id,
      };
      _streamSizeDesktops = {
        for (final desktop in desktops)
          if (desktop.streamSize) desktop.id,
      };
      _streamSizeV2Desktops = {
        for (final desktop in desktops)
          if (desktop.streamSizeV2) desktop.id,
      };
      _printDesktops = {
        for (final desktop in desktops)
          if (desktop.print) desktop.id,
      };
      _usbDesktops = {
        for (final desktop in desktops)
          if (desktop.usb) desktop.id,
      };
      _udpTracksDesktops = {
        for (final desktop in desktops)
          if (desktop.udpTracks) desktop.id,
      };
      _udpMediaDesktops = {
        for (final desktop in desktops)
          if (desktop.udpMedia) desktop.id,
      };
      _udpDirectDesktops = {
        for (final desktop in desktops)
          if (desktop.udpDirect) desktop.id,
      };
      _audioOpusDesktops = {
        for (final desktop in desktops)
          if (desktop.audioOpus) desktop.id,
      };
      _audioOpusTracksDesktops = {
        for (final desktop in desktops)
          if (desktop.audioOpusTracks) desktop.id,
      };
      _relativeMouseDesktops = {
        for (final desktop in desktops)
          if (desktop.relativeMouse) desktop.id,
      };
      _cursorEchoDesktops = {
        for (final desktop in desktops)
          if (desktop.cursorEcho) desktop.id,
      };
      _cursorComposeDesktops = {
        for (final desktop in desktops)
          if (desktop.cursorCompose) desktop.id,
      };
      // desktop-preview-v1: read from the raw entry (BrokerDesktop keeps no copy).
      _previewDesktops = {
        for (final value in inventory)
          if (value is Map &&
              (value['kind'] == 'vm' || value['kind'] == 'pc') &&
              value['previewProtocol'] == desktopPreviewProtocol &&
              value['id'] is String)
            value['id'] as String,
      };
      _sessionResumeDesktops = {
        for (final desktop in desktops)
          if (desktop.sessionResume) desktop.id,
      };
      // host-ready-wait-v1: read from the raw entry, like desktop-preview-v1.
      _hostReadyDesktops = {
        for (final value in inventory)
          if (value is Map &&
              value['hostReadyProtocol'] == hostReadyProtocol &&
              value['id'] is String)
            value['id'] as String,
      };
    }
    return desktops;
  }

  @override
  Future<void> logout(String token) async {
    _diagnostics?.close();
    _diagnostics = null;
    _diagnosticToken = null;
    if (_inventoryToken == token) {
      _inventorySequence++;
      _inventoryToken = null;
      _bitrateCeilings = {};
      _inputLatencyDesktops = {};
      _observabilityDesktops.clear();
      _cursorDesktops = {};
      _cursorTracksDesktops = {};
      _textInputDesktops = {};
      _displayScaleDesktops = {};
      _displayScaleOutputsDesktops = {};
      _touchInputDesktops = {};
      _caretDesktops = {};
      _driveDesktops = {};
      _driveFsDesktops = {};
      _streamSizeDesktops = {};
      _streamSizeV2Desktops = {};
      _printDesktops = {};
      _usbDesktops = {};
      _udpMediaDesktops = {};
      _udpDirectDesktops = {};
      _udpTracksDesktops = {};
      _audioOpusDesktops = {};
      _audioOpusTracksDesktops = {};
      _relativeMouseDesktops = {};
      _cursorEchoDesktops = {};
      _cursorComposeDesktops = {};
      _previewDesktops = {};
      _sessionResumeDesktops = {};
      _hostReadyDesktops = {};
    }
    await _request('/api/v1/auth/logout', method: 'POST', token: token);
  }

  @override
  Future<void> changePassword(
    String token,
    String currentPassword,
    String newPassword,
    String confirmation,
  ) async {
    await _request(
      '/api/v1/auth/password',
      method: 'POST',
      token: token,
      data: {
        'currentPassword': currentPassword,
        'newPassword': newPassword,
        'passwordConfirmation': confirmation,
      },
    );
  }

  @override
  Future<BrokerConnection> connect(
    String token,
    String desktopId, {
    String mode = 'stream',
    StreamOptions? options,
    InitialViewport? initialViewport,
    int? displayScale,
    String? signinAvailabilityId,
  }) async {
    final offered = _inventoryToken == token ? _signinOffers[desktopId] : null;
    signinAvailabilityId ??= offered?.fresh(DateTime.now()) == true
        ? offered!.id
        : null;
    final signinRequested = signinAvailabilityId != null;
    if (signinRequested) displayScale = null;
    // Streaming is the only mode.
    if (mode != 'stream' ||
        (signinRequested &&
            (!RegExp(r'^[a-f0-9]{32}$').hasMatch(signinAvailabilityId) ||
                (options ?? const StreamOptions()).monitor != '0')) ||
        (displayScale != null && !validDisplayScale(displayScale)) ||
        (initialViewport != null &&
            ((options ?? const StreamOptions()).monitor != '0' ||
                (options ?? const StreamOptions()).fps !=
                    initialViewport.fps))) {
      throw ArgumentError('Invalid connection mode.');
    }
    // stream-size-v1: only with an initial viewport and separate-v1 encoder
    // ceilings, where the inventory advertises it. The box becomes two
    // per-side ceilings of 3840 and the decoder limits carry the real one; an
    // unknown decoder declares today's 3840x2160 at 4.19 MP@60.
    final v2 =
        mode == 'stream' &&
        initialViewport != null &&
        initialViewport.preserveEncoderLimits &&
        _streamSizeV2Advertised(token, desktopId);
    final admission = v2
        ? await _capacity([
            StreamSourceSize(initialViewport.width, initialViewport.height),
          ], (options ?? const StreamOptions()).fps)
        : null;
    final DecoderLimits? size = v2
        ? admission!.limits
        : mode == 'stream' &&
              initialViewport != null &&
              initialViewport.preserveEncoderLimits &&
              _streamSizeAdvertised(token, desktopId)
        ? await decoderLimits().then(
                (value) => value?.legacy,
                onError: (Object _) => null,
              ) ??
              DecoderLimits.safeDefault
        : null;
    if (size == null && initialViewport != null && !initialViewport.released) {
      throw const BrokerException('stream_size_unavailable', 409);
    }
    if (!v2 &&
        initialViewport != null &&
        !legacyOwnedOutputSize(
          initialViewport.width,
          initialViewport.height,
          portrait: size != null,
        )) {
      throw const BrokerException("stream_size_unavailable", 409);
    }
    if (v2 && !_streamSizeV2Advertised(token, desktopId)) {
      throw const BrokerException("stream_size_unavailable", 409);
    }
    StreamOptions boxed(StreamOptions value) => v2
        ? value.withBox(size!.maxWidth, size.maxHeight)
        : size != null
        ? value.withBox(streamMaximumSide, streamMaximumSide)
        : value.maxHeight > releasedMaximumHeight
        ? value.withCodecCeiling()
        : value;
    // Send only what this desktop's broker and host accept (bitrate-adaptive-v1).
    final preference = options ?? const StreamOptions();
    final preferred = signinRequested
        ? StreamOptions(
            maxWidth: preference.maxWidth,
            maxHeight: preference.maxHeight,
            fps: preference.fps,
            bitrate: preference.bitrate,
            audio: false,
            monitor: preference.monitor,
            bitrateMode: StreamBitrateMode.fixed,
          )
        : preference;
    var requested = boxed(
      preferred.negotiated(_bitrateCeiling(token, desktopId)),
    );
    // Optional measurement: requested only where the inventory advertised it.
    var latency =
        !signinRequested &&
        mode == 'stream' &&
        _inputLatencyAdvertised(token, desktopId);
    var observe =
        !signinRequested &&
        mode == 'stream' &&
        _observabilityRequested(token, desktopId);
    // cursor-v1 likewise, for this ordinary (non-track) stream request only.
    var cursor =
        !signinRequested &&
        mode == 'stream' &&
        _cursorAdvertised(token, desktopId);
    // text-input-v1 likewise, only for a client that sends committed text.
    var text =
        mode == 'stream' &&
        requestTextInput &&
        _textInputAdvertised(token, desktopId);
    // Negotiate the runtime grant separately from an optional initial value.
    // A marker-only Windows request preserves the current scale on connect.
    var scaleControl =
        !signinRequested &&
        mode == 'stream' &&
        (requestDisplayScaleControl || displayScale != null) &&
        _displayScaleAdvertised(token, desktopId);
    var scale = scaleControl ? displayScale : null;
    // touch-input-v1 likewise, only for a client that sends touch contacts.
    var touch =
        !signinRequested &&
        mode == 'stream' &&
        requestTouchInput &&
        _touchInputAdvertised(token, desktopId);
    // caret-v1 likewise, only for a client that follows the text caret.
    var caret =
        !signinRequested &&
        mode == 'stream' &&
        requestCaret &&
        _caretAdvertised(token, desktopId);
    // print-v1 likewise, only for a client that prints what the VM sends.
    var print =
        !signinRequested &&
        mode == 'stream' &&
        requestPrint &&
        _printAdvertised(token, desktopId);
    // drive-v1 likewise, only for a client that shares local folders.
    var drive =
        !signinRequested &&
        mode == 'stream' &&
        _driveRequested(token, desktopId);
    // driveFs (the "fs" revision) rides along only where the Host shows it.
    var driveFs =
        !signinRequested &&
        mode == 'stream' &&
        _driveFsAdvertised(token, desktopId);
    // usb-v1 likewise, only for a client that can lend this PC's USB devices.
    var usb =
        !signinRequested &&
        mode == 'stream' &&
        requestUsb &&
        _usbAdvertised(token, desktopId);
    // udp-media-v1 likewise, only for a client that runs the UDP probe.
    final udp =
        !signinRequested &&
        mode == 'stream' &&
        _udpMediaRequested(token, desktopId);
    // audio-opus-v1 likewise, only for a client whose runner decodes Opus.
    var opus =
        !signinRequested &&
        mode == 'stream' &&
        _audioOpusRequested(token, desktopId);
    // relative-mouse-v1 likewise, only for a client that captures the mouse.
    var relativeMouse =
        !signinRequested &&
        mode == 'stream' &&
        _relativeMouseRequested(token, desktopId);
    // cursor-echo-v1 likewise, only with cursor-v1 and relative-mouse-v1 (the
    // echo serves the captured 게임 마우스).
    var cursorEcho =
        cursor && relativeMouse && _cursorEchoAdvertised(token, desktopId);
    // cursor-compose-v1 likewise (preferred over the echo where granted).
    var cursorCompose =
        cursor && relativeMouse && _cursorComposeAdvertised(token, desktopId);
    // session-resume-v1 likewise, only for a client that resumes its socket.
    var resume =
        !signinRequested &&
        mode == 'stream' &&
        _sessionResumeRequested(token, desktopId);
    final input = <String, Object?>{
      'mode': mode,
      if (signinRequested) ...{
        'signinProtocol': signinStreamProtocol,
        'signinAvailabilityId': signinAvailabilityId,
      },
      if (mode == 'stream') 'resizeProtocol': 'epoch-v1',
      if (mode == 'stream') 'clipboardProtocol': 'clipboard-sync-v1',
      if (latency) 'inputLatencyProtocol': inputLatencyProtocol,
      if (observe) 'observabilityProtocol': streamObservabilityProtocol,
      if (cursor) 'cursorProtocol': cursorProtocol,
      if (text) 'textInputProtocol': textInputProtocol,
      if (scaleControl) 'displayScaleProtocol': displayScaleProtocol,
      'displayScale': ?scale,
      if (touch) 'touchInputProtocol': touchInputProtocol,
      if (caret) 'caretProtocol': caretProtocol,
      if (print) 'printProtocol': printProtocol,
      if (drive) 'driveProtocol': driveProtocol,
      if (drive && driveFs) 'driveFs': true,
      if (usb) 'usbProtocol': usbProtocol,
      if (udp) 'udpMediaProtocol': udpMediaProtocol,
      if (!signinRequested && udp && _udpDirectDesktops.contains(desktopId))
        'udpDirectProtocol': udpDirectProtocol,
      if (opus) 'audioCodecProtocol': audioOpusProtocol,
      if (relativeMouse) 'relativeMouseProtocol': relativeMouseProtocol,
      if (cursorEcho) 'cursorEchoProtocol': cursorEchoProtocol,
      if (cursorCompose) 'cursorComposeProtocol': cursorComposeProtocol,
      if (resume) 'sessionResumeProtocol': sessionResumeProtocol,
      if (initialViewport != null) ...{
        'initialViewportProtocol': initialViewportProtocol,
        'initialViewport': initialViewport.toJson(),
        if (initialViewport.preserveEncoderLimits)
          'viewportEncoderLimitsProtocol': viewportEncoderLimitsProtocol,
      },
      if (size != null) ...{
        'streamSizeProtocol': v2 ? streamSizeProtocolV2 : streamSizeProtocol,
        'decoderLimits': size.toJson(),
      },
      if (mode == 'stream') 'options': requested.toJson(),
    };
    Future<Map<String, dynamic>> request() => _request(
      '/api/v1/desktops/${Uri.encodeComponent(desktopId)}/connect',
      method: 'POST',
      token: token,
      data: input,
    );
    // Reads the variables as they stand when called (fallbacks clear them).
    BrokerConnection build(Map<String, dynamic> data) =>
        BrokerConnection.fromJson(
          data,
          origin,
          cancel: (id) => _cancelConnection(token, id, desktopId),
          requestedInitialViewport: initialViewport,
          requestedOptions: mode == 'stream' ? requested : null,
          requestedInputLatency: latency,
          requestedObservability: observe,
          requestedCursor: cursor,
          requestedTextInput: text,
          requestedDisplayScaleControl: scaleControl,
          requestedDisplayScale: scale,
          requestedTouchInput: touch,
          requestedCaret: caret,
          requestedDrive: drive,
          requestedDriveFs: driveFs,
          requestedStreamSize: size,
          requestedStreamSizeV2: v2,
          requestedHardwareDecode: admission?.hardwareDecode,
          requestedPrint: print,
          requestedUsb: usb,
          requestedUdpMedia: udp,
          requestedAudioOpus: opus,
          requestedRelativeMouse: relativeMouse,
          requestedCursorEcho: cursorEcho,
          requestedCursorCompose: cursorCompose,
          requestedSessionResume: resume,
          requestedSignin: signinRequested,
        );
    Map<String, dynamic> data;
    try {
      data = await request();
    } on BrokerException catch (error) {
      // A sign-in refusal cannot fall back to an ordinary/unwrapped session or
      // replay a POST with weaker negotiation. The explicit new path is one shot.
      if (signinRequested) rethrow;
      // Released brokers validate this field before reserving a session.
      // Their exact refusal of a marker-only request can retry once without
      // that marker, preserving the viewport and every unrelated option.
      // Never retry an uncertain POST, timeout, auth or Host failure here.
      if (scaleControl &&
          scale == null &&
          error.status == 400 &&
          error.code == 'invalid_display_scale') {
        input.remove('displayScaleProtocol');
        scaleControl = false;
        return build(await request());
      }
      if (mode == 'stream' && _bitrateRefused(error, requested)) {
        requested = boxed(preferred.negotiated(null));
        input['options'] = requested.toJson();
        data = await request();
        return BrokerConnection.fromJson(
          data,
          origin,
          cancel: (id) => _cancelConnection(token, id, desktopId),
          requestedInitialViewport: initialViewport,
          requestedOptions: requested,
          requestedInputLatency: latency,
          requestedObservability: observe,
          requestedCursor: cursor,
          requestedTextInput: text,
          requestedDisplayScaleControl: scaleControl,
          requestedDisplayScale: scale,
          requestedTouchInput: touch,
          requestedCaret: caret,
          requestedDrive: drive,
          requestedDriveFs: driveFs,
          requestedStreamSize: size,
          requestedStreamSizeV2: v2,
          requestedHardwareDecode: admission?.hardwareDecode,
          requestedPrint: print,
          requestedUsb: usb,
          requestedUdpMedia: udp,
          requestedAudioOpus: opus,
          requestedRelativeMouse: relativeMouse,
          requestedCursorEcho: cursorEcho,
          requestedCursorCompose: cursorCompose,
          requestedSessionResume: resume,
        );
      }
      // Released brokers reject unknown fields before creating a session.
      // Retry only that explicit preflight rejection, never an uncertain POST.
      var failure = error;
      if (driveFs && error.status == 400 && error.code == 'unexpected_field') {
        // A broker that knows drive-v1 but not its "fs" revision: drop only
        // driveFs and keep the drive grant.
        input.remove('driveFs');
        driveFs = false;
        try {
          return build(await request());
        } on BrokerException catch (second) {
          failure = second;
        }
      }
      if (mode != 'stream' ||
          initialViewport != null ||
          failure.status != 400 ||
          failure.code != 'unexpected_field') {
        if (identical(failure, error)) rethrow;
        throw failure;
      }
      input.remove('driveFs');
      driveFs = false;
      input.remove('clipboardProtocol');
      input.remove('inputLatencyProtocol');
      input.remove('observabilityProtocol');
      observe = false;
      input.remove('cursorProtocol');
      input.remove('textInputProtocol');
      input.remove('displayScaleProtocol');
      input.remove('displayScale');
      input.remove('touchInputProtocol');
      input.remove('relativeMouseProtocol');
      input.remove('cursorEchoProtocol');
      input.remove('cursorComposeProtocol');
      input.remove('sessionResumeProtocol');
      resume = false;
      input.remove('audioCodecProtocol');
      opus = false;
      input.remove('caretProtocol');
      input.remove('printProtocol');
      input.remove('driveProtocol');
      input.remove('usbProtocol');
      usb = false;
      latency = false;
      cursor = false;
      text = false;
      scaleControl = false;
      scale = null;
      touch = false;
      relativeMouse = false;
      cursorEcho = false;
      cursorCompose = false;
      print = false;
      drive = false;
      data = await request();
    }
    return build(data);
  }

  @override
  Future<BrokerConnection> connectSignin(
    String token,
    String desktopId, {
    required String availabilityId,
    StreamOptions? options,
    InitialViewport? initialViewport,
  }) => connect(
    token,
    desktopId,
    options: options,
    initialViewport: initialViewport,
    signinAvailabilityId: availabilityId,
  );

  @override
  Future<BrokerConnection> connectScaled(
    String token,
    String desktopId, {
    required StreamOptions options,
    InitialViewport? initialViewport,
    required int displayScale,
  }) => connect(
    token,
    desktopId,
    options: options,
    initialViewport: initialViewport,
    displayScale: displayScale,
  );

  @override
  Future<BrokerConnection> connectViewport(
    String token,
    String desktopId,
    InitialViewport viewport,
    StreamOptions options,
  ) => connect(token, desktopId, options: options, initialViewport: viewport);

  @override
  Future<int> endConsoles(String token, String desktopId) async {
    final value = await _request(
      '/api/v1/desktops/${Uri.encodeComponent(desktopId)}/console/release',
      method: 'POST',
      token: token,
      data: const <String, Object?>{},
    );
    final released = value['released'];
    if (released is! int || released < 0) {
      throw const BrokerException('invalid_response');
    }
    return released;
  }

  @override
  Future<bool> takeover(String token, String desktopId) async {
    final value = await _request(
      '/api/v1/desktops/${Uri.encodeComponent(desktopId)}/takeover',
      method: 'POST',
      token: token,
      data: const <String, Object?>{},
    );
    final released = value['released'];
    if (released is! bool) throw const BrokerException('invalid_response');
    return released;
  }

  @override
  Future<void> checkStreamTakeover(String token, String desktopId) async {
    if (!requestStreamTakeoverContext) {
      return;
    }
    late final Map<String, dynamic> value;
    try {
      value = await _request(
        '/api/v1/desktops/${Uri.encodeComponent(desktopId)}/takeover-context',
        method: 'POST',
        token: token,
        data: const <String, Object?>{},
      );
    } on BrokerException catch (error) {
      // An old server lacks only this optional read. Its ordinary display and
      // connection gates remain authoritative; it can never enable a release.
      if (error.status == 404 && error.code == 'not_found') return;
      rethrow;
    }
    if (value.length != 1 || value['busy'] != false) {
      throw const BrokerException('invalid_response');
    }
  }

  @override
  Future<bool> confirmTakeover(
    String token,
    String desktopId,
    StreamTakeoverOffer offer,
  ) async {
    if (offer.desktopId != desktopId) {
      throw const BrokerException('invalid_takeover_context');
    }
    if (!offer.currentAt(DateTime.now())) {
      throw const BrokerException('takeover_context_expired', 410);
    }
    final value = await _request(
      '/api/v1/desktops/${Uri.encodeComponent(desktopId)}/takeover',
      method: 'POST',
      token: token,
      data: {'context': offer.context},
    );
    if (value['released'] != true) {
      throw const BrokerException('invalid_response');
    }
    return true;
  }

  @override
  void close() {
    _closed = true;
    _diagnostics?.close();
    _diagnostics = null;
    _diagnosticToken = null;
    _inventorySequence++;
    _inventoryToken = null;
    _bitrateCeilings = {};
    _inputLatencyDesktops = {};
    _observabilityDesktops.clear();
    _cursorDesktops = {};
    _cursorTracksDesktops = {};
    _textInputDesktops = {};
    _displayScaleDesktops = {};
    _displayScaleOutputsDesktops = {};
    _touchInputDesktops = {};
    _caretDesktops = {};
    _driveDesktops = {};
    _driveFsDesktops = {};
    _udpMediaDesktops = {};
    _udpDirectDesktops = {};
    _udpTracksDesktops = {};
    _audioOpusDesktops = {};
    _audioOpusTracksDesktops = {};
    _relativeMouseDesktops = {};
    _cursorEchoDesktops = {};
    _cursorComposeDesktops = {};
    _previewDesktops = {};
    _sessionResumeDesktops = {};
    _hostReadyDesktops = {};
    _streamSizeDesktops = {};
    _streamSizeV2Desktops = {};
    _printDesktops = {};
    _http.close(force: true);
  }
}
