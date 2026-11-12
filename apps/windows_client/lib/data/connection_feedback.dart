import '../l10n/l10n.dart';

/// Presentation states selected from observations in the owning connection.
/// A state label is not a readiness or permission decision.
enum ConnectionWaitStep {
  request,
  display,
  relay,
  firstFrame,
  reconnect,
  hostUpdate,
  vmRestart;

  const ConnectionWaitStep();
  String get label => switch (this) {
    ConnectionWaitStep.request => l10n.connectionRequest,
    ConnectionWaitStep.display => l10n.preparingVirtualDisplay,
    ConnectionWaitStep.relay => l10n.serverConnection,
    ConnectionWaitStep.firstFrame => l10n.waitingFirstFrame,
    // A state ("다시 연결 중"), never the words of the 다시 연결 button.
    ConnectionWaitStep.reconnect => l10n.reconnecting2,
    ConnectionWaitStep.hostUpdate => l10n.waitingHostUpdate,
    ConnectionWaitStep.vmRestart => l10n.waitingDesktopRestart,
  };

}

enum ConnectionRecovery { none, quality, display }

/// Only explicit, retryable media/display failures offer a settings shortcut.
/// The caller still owns cleanup, authorization and any later reconnect.
ConnectionRecovery connectionRecovery(
  String? code, {
  required bool canRetry,
  bool takeover = false,
}) {
  if (!canRetry || takeover) return ConnectionRecovery.none;
  return switch (code) {
    'connection_overloaded' ||
    'client_backpressure' ||
    'host_backpressure' ||
    'media_backpressure' => ConnectionRecovery.quality,
    'frame_layout_unsupported' ||
    'frame_memory_insufficient' ||
    'encoder_format_unsupported' ||
    'unsupported_display_mode' ||
    'initial_viewport_mismatch' => ConnectionRecovery.display,
    _ => ConnectionRecovery.none,
  };
}

String connectionElapsed(Duration elapsed) {
  final seconds = elapsed.inSeconds.clamp(0, 359999);
  final minutes = seconds ~/ 60;
  return '${minutes.toString().padLeft(2, '0')}:${(seconds % 60).toString().padLeft(2, '0')}';
}
