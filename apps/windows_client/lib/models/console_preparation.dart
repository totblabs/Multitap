import 'virtual_display.dart';
import '../l10n/l10n.dart';

const consolePreparationReasons = {
  'console_ready',
  'native_provider_required',
  'reboot_required',
  'virtual_device_not_ready',
  'windows_session_unknown',
  'windows_session_locked',
  'windows_identity_required',
  'windows_identity_mismatch',
  'windows_login_required',
  'windows_session_ambiguous',
  'rdp_session_active',
  'rdp_session_disconnected',
  'console_unavailable',
  'console_in_use',
  'operation_cancelled',
  'console_busy',
  'console_timeout',
  'console_transfer_failed',
  'console_transfer_pending',
  'console_binding_changed',
  'worker_failed',
  'administrator_required',
};

class ConsolePreparationOperation {
  const ConsolePreparationOperation({
    required this.id,
    required this.status,
    this.attempted,
    this.error,
  });
  final String id, status;
  final bool? attempted;
  final String? error;
  factory ConsolePreparationOperation.fromJson(Map<String, dynamic> data) {
    final id = data['id'],
        status = data['status'],
        attempted = data['attempted'],
        error = data['error'];
    if (id is! String ||
        !virtualDisplayOperationId.hasMatch(id) ||
        !{'pending', 'succeeded', 'failed', 'unknown'}.contains(status) ||
        (attempted != null && attempted is! bool) ||
        (status == 'succeeded' && (attempted == null || error != null))) {
      throw const FormatException('Invalid console operation');
    }
    return ConsolePreparationOperation(
      id: id,
      status: status as String,
      attempted: attempted as bool?,
      error: error == null
          ? null
          : error is String && RegExp(r'^[a-z_]{1,64}$').hasMatch(error)
          ? error
          : 'worker_failed',
    );
  }
}

class ConsolePreparationState {
  const ConsolePreparationState({
    this.supported = false,
    this.available = false,
    this.busy = false,
    this.reason = 'windows_session_unknown',
    this.operation,
  });
  final bool supported, available, busy;
  final String reason;
  final ConsolePreparationOperation? operation;
  factory ConsolePreparationState.fromJson(
    Object? value, {
    String? operationId,
    bool requireOperation = false,
  }) {
    if (value == null && !requireOperation) {
      return const ConsolePreparationState();
    }
    if (value is! Map<String, dynamic> ||
        value['supported'] is! bool ||
        value['available'] is! bool ||
        value['busy'] is! bool ||
        !consolePreparationReasons.contains(value['reason'])) {
      throw const FormatException('Invalid console preparation');
    }
    if (value['available'] == true &&
        (value['supported'] != true ||
            value['busy'] == true ||
            !{
              'rdp_session_active',
              'rdp_session_disconnected',
            }.contains(value['reason']))) {
      throw const FormatException('Contradictory console preparation');
    }
    final raw = value['operation'];
    final operation = raw == null
        ? null
        : raw is Map<String, dynamic>
        ? ConsolePreparationOperation.fromJson(raw)
        : throw const FormatException('Invalid console operation');
    if ((requireOperation && operation == null) ||
        (operationId != null && operation?.id != operationId)) {
      throw const FormatException('Uncorrelated console operation');
    }
    return ConsolePreparationState(
      supported: value['supported'] as bool,
      available: value['available'] as bool,
      busy: value['busy'] as bool,
      reason: value['reason'] as String,
      operation: operation,
    );
  }
}

String? consolePreparationMessage(String? code) => switch (code) {
  'windows_session_locked' =>
    l10n.windowsLockedUnlockSameWindows,
  'windows_login_required' => l10n.theresNoWindowsSignSession,
  'rdp_session_active' ||
  'rdp_session_disconnected' => l10n.youCanBringWindowsScreen,
  'windows_identity_required' => l10n.checkHostSettingsWindowsSign,
  'windows_identity_mismatch' ||
  'windows_session_ambiguous' ||
  'console_in_use' => l10n.anotherWindowsUserSeveralSign,
  'console_binding_changed' => l10n.windowsSignStateChangedDuring,
  'console_timeout' ||
  'console_transfer_pending' ||
  'console_operation_unknown' =>
    l10n.sessionSwitchResultIsntConfirmed,
  'console_transfer_failed' ||
  'console_recovery_unavailable' ||
  'windows_session_unknown' ||
  'console_unavailable' =>
    l10n.couldntReturnWindowsSessionStreaming,
  'console_busy' => l10n.windowsSessionVirtualDisplayBeing,
  'console_display_not_ready' =>
    l10n.windowsSessionBackButVirtual,
  'console_confirmation_required' => l10n.returnWindowsSessionConfirmSwitch,
  _ => null,
};
