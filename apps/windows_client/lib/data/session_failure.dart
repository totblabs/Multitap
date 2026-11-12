import 'dart:async';
import 'dart:io';

import 'broker_api.dart';
import '../l10n/l10n.dart';

/// Fixed Host resolution failures only. This does not infer an unchanged
/// display; that suffix is used only after the existing correlated ACK check.
String? resolutionFailureMessage(Object? reason, {bool unchanged = false}) {
  final message = switch (reason) {
    'frame_layout_unsupported' => l10n.hostCantHandleScreenSize,
    'frame_memory_unavailable' => l10n.couldntCheckHostsAvailableMemory,
    'frame_memory_insufficient' => l10n.hostDoesntHaveEnoughFree,
    'encoder_format_unsupported' => l10n.hostDoesntSupportScreenSize,
    'unsupported_display_mode' => l10n.virtualDisplayDoesntSupportResolution,
    _ => null,
  };
  if (message == null) return null;
  if (unchanged) return l10n.keepingPreviousSize(message);
  final next = switch (reason) {
    'frame_memory_unavailable' => l10n.reconnectMoment,
    'frame_memory_insufficient' => l10n.makeScreenSmallerReconnect,
    _ => l10n.reconnectDifferentScreenSize,
  };
  return '$message$next';
}

/// A stream another connection took over: who, when the server says
/// (`same_account`/`other_account`, `console_*` from the web Console; `by` the
/// other account's name). Older servers send neither: the general text.
String takenOverMessage(Object? detail, Object? by) {
  final console =
      detail == 'console_same_account' || detail == 'console_other_account';
  final other = detail == 'other_account' || detail == 'console_other_account';
  final name = by is String ? by.trim() : '';
  final shown =
      other &&
          name.isNotEmpty &&
          name.length <= 64 &&
          !name.runes.any((unit) => unit < 32 || unit == 127)
      ? name
      : null;
  final where = console ? l10n.console2 : l10n.fromAnotherWindowDevice;
  final who = switch ((detail, shown)) {
    ('same_account' || 'console_same_account', _) => l10n.sameAccount(where),
    (_, final String account) when other => l10n.account(account, where),
    _ when other => l10n.anotherAccount(where),
    _ => null,
  };
  if (who == null) {
    return SessionFailure.ended('taken_over').message;
  }
  return l10n.connectedDesktopSoConnectionEnded(who);
}

class SessionFailure {
  const SessionFailure(
    this.code,
    this.message, {
    this.canRetry = true,
    this.takeover = false,
  });
  final String code, message;
  final bool canRetry;

  /// The broker allows ending the VM's existing connection from here.
  final bool takeover;

  /// Automatic recovery is narrower than a user's explicit retry. Unknown
  /// ends, authorization/policy decisions and malformed protocol stay stopped.
  static bool transientTransportEnd(String? code) => const {
    'network_error',
    'relay_closed',
    'media_timeout',
    'host_timeout',
    'video_timeout',
    'host_disconnected',
    'client_timeout',
    'server_stopping',
    'connection_overloaded',
    'client_backpressure',
    'host_backpressure',
    'media_backpressure',
  }.contains(code);

  /// RFC 6455 protocol, unsupported data, invalid payload, policy and missing
  /// extension closes must not be treated as an ordinary transport handover.
  static bool transientCloseCode(int? code) =>
      !const {1002, 1003, 1007, 1008, 1010}.contains(code);

  static bool blocksAutomaticReconnect(String? code) => const {
    'session_reauthentication_required',
    'session_idle_timeout',
    'pc_login_replaced',
    'authorization_revoked',
    'session_expired',
    'host_revoked',
    'clipboard_policy_changed',
    'taken_over',
    'client_stopped',
    'invalid_control',
    'invalid_media',
    'invalid_stream_info',
    'invalid_stream_message',
    'host_protocol_rejected',
    'frame_layout_unsupported',
    'frame_memory_unavailable',
    'frame_memory_insufficient',
    'encoder_format_unsupported',
    'unsupported_display_mode',
  }.contains(code);

  /// A WebSocket close can arrive without the preceding JSON session.ended.
  /// Accept only the relay's fixed end reasons; never surface its free text.
  static String? transportCloseReason(Object? value) => switch (value) {
    'session_reauthentication_required' ||
    'session_idle_timeout' ||
    'pc_login_replaced' ||
    'host_disconnected' ||
    'host_timeout' ||
    'host_error' ||
    'host_revoked' ||
    'host_backpressure' ||
    'host_rate_limit' ||
    'invalid_control' ||
    'invalid_media' ||
    'invalid_stream_info' ||
    'invalid_console_result' ||
    'server_stopping' ||
    'authorization_revoked' ||
    'session_expired' ||
    'taken_over' ||
    'client_stopped' ||
    'client_timeout' ||
    'client_backpressure' ||
    'clipboard_policy_changed' ||
    'display_reconfigure' ||
    'host_update' ||
    'frame_layout_unsupported' ||
    'frame_memory_unavailable' ||
    'frame_memory_insufficient' ||
    'encoder_format_unsupported' ||
    'unsupported_display_mode' ||
    'resize_failed' => value as String,
    _ => null,
  };

  factory SessionFailure.ended(Object? reason) => switch (reason) {
    'session_reauthentication_required' => SessionFailure(
      'session_reauthentication_required',
      pcLoginTerminalMessage('session_reauthentication_required'),
      canRetry: false,
    ),
    'session_idle_timeout' => SessionFailure(
      'session_idle_timeout',
      pcLoginTerminalMessage('session_idle_timeout'),
      canRetry: false,
    ),
    'pc_login_replaced' => SessionFailure(
      'pc_login_replaced',
      pcLoginTerminalMessage('pc_login_replaced'),
      canRetry: false,
    ),
    'taken_over' => SessionFailure(
      'taken_over',
      l10n.desktopWasConnectedFromAnother,
    ),
    'clipboard_policy_changed' => SessionFailure(
      'clipboard_policy_changed',
      l10n.administratorChangedClipboardSharingPermission,
    ),
    'host_maintenance' => SessionFailure(
      'host_maintenance',
      l10n.hostPreparingVirtualDisplayConnect,
    ),
    'host_update' => SessionFailure(
      'host_update',
      l10n.disconnectedHostUpdateReconnectWhen,
    ),
    'authorization_revoked' || 'session_expired' => SessionFailure(
      'authorization_revoked',
      l10n.signExpiredDesktopsAssignmentChanged,
      canRetry: false,
    ),
    'host_revoked' => SessionFailure(
      'host_revoked',
      l10n.desktopsHostWasUnregisteredLet,
      canRetry: false,
    ),
    'no_interactive_session' || 'virtual_display_required' => SessionFailure(
      reason as String,
      BrokerException(reason).message,
    ),
    // The broker's checks of the desktop's VM before a stream, each with its
    // own code rather than the general virtual_display_required.
    'host_machine_mismatch' => SessionFailure(
      'host_machine_mismatch',
      l10n.hostDoesntMatchRegisteredVm,
      canRetry: false,
    ),
    'desktop_changed' => SessionFailure(
      'desktop_changed',
      l10n.desktopRegistrationChangedRefresh,
    ),
    'desktop_unavailable' => SessionFailure(
      'desktop_unavailable',
      l10n.proxmoxNoLongerListsVm,
      canRetry: false,
    ),
    'power_in_progress' => SessionFailure(
      'power_in_progress',
      l10n.powerActionProgressTryAgain,
    ),
    'frame_layout_unsupported' ||
    'frame_memory_unavailable' ||
    'frame_memory_insufficient' ||
    'encoder_format_unsupported' ||
    'unsupported_display_mode' => SessionFailure(
      reason as String,
      resolutionFailureMessage(reason)!,
    ),
    'worker_failed' || 'capture_failed' => SessionFailure(
      'capture_failed',
      l10n.hostCouldntPrepareScreenReconnect,
    ),
    'encoder_failed' => SessionFailure(
      'encoder_failed',
      l10n.hostStoppedSendingScreenReconnect,
    ),
    'host_disconnected' ||
    'host_timeout' ||
    'host_error' ||
    'invalid_console_result' => SessionFailure(
      'host_disconnected',
      l10n.hostConnectionDroppedReconnectMoment,
    ),
    'server_stopping' => SessionFailure(
      'server_stopping',
      l10n.serverRestartingReconnectMoment,
    ),
    'client_backpressure' || 'host_backpressure' => SessionFailure(
      'connection_overloaded',
      l10n.screenCouldntProcessedTimeSo,
    ),
    'display_reconfigure' || 'resize_failed' => SessionFailure(
      'display_changed',
      l10n.virtualDisplaySetupChangedScreen,
    ),
    _ => SessionFailure('session_ended', l10n.disconnectedReconnect),
  };

  factory SessionFailure.request(Object error) {
    if (error is BrokerException) {
      if (error.code == 'credentials_change_required') {
        return SessionFailure(error.code, error.message, canRetry: false);
      }
      if (error.status == 401 ||
          error.code == 'session_expired' ||
          error.code == 'authentication_required') {
        return SessionFailure.ended('session_expired');
      }
      if (error.status == 403 ||
          error.status == 404 ||
          error.code == 'desktop_not_found') {
        return SessionFailure(
          'desktop_not_found',
          const BrokerException('desktop_not_found').message,
          canRetry: false,
        );
      }
      if (error.code == 'desktop_busy') {
        return SessionFailure(
          'desktop_busy',
          error.message,
          takeover: error.takeover == true,
        );
      }
      if (error.code == 'track_streaming_unavailable') {
        return SessionFailure(
          'track_streaming_unavailable',
          l10n.couldntConnectAllMonitorsFrom,
          canRetry: false,
        );
      }
      return SessionFailure('connection_rejected', error.message);
    }
    if (error is TimeoutException ||
        error is SocketException ||
        error is WebSocketException) {
      return SessionFailure(
        'connection_timeout',
        l10n.couldntConnectServerCheckNetwork2,
      );
    }
    return SessionFailure(
      'connection_failed',
      l10n.couldntConnectDesktopReconnectIf,
    );
  }
}
