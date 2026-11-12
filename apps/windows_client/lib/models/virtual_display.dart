import '../l10n/l10n.dart';

final virtualDisplayOperationId = RegExp(
  r'^[0-9a-fA-F]{8}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{12}$',
);

DateTime? _date(dynamic value) {
  if (value == null) return null;
  if (value is! String || DateTime.tryParse(value) == null) {
    throw const FormatException('Invalid display timestamp');
  }
  return DateTime.parse(value);
}

class VirtualDisplayOperation {
  const VirtualDisplayOperation({
    required this.id,
    required this.status,
    required this.requestedCount,
    this.submittedAt,
    this.checkedAt,
    this.error,
  });
  final String id, status;
  final int requestedCount;
  final DateTime? submittedAt, checkedAt;
  final String? error;
  factory VirtualDisplayOperation.fromJson(Map<String, dynamic> data) {
    final id = data['id'],
        status = data['status'],
        count = data['requestedCount'];
    if (id is! String ||
        !virtualDisplayOperationId.hasMatch(id) ||
        !['pending', 'succeeded', 'failed', 'unknown'].contains(status) ||
        count is! int ||
        ![1, 2].contains(count)) {
      throw const FormatException('Invalid display operation');
    }
    final error = data['error'];
    return VirtualDisplayOperation(
      id: id,
      status: status as String,
      requestedCount: count,
      submittedAt: _date(data['submittedAt']),
      checkedAt: _date(data['checkedAt']),
      error: error is String && RegExp(r'^[a-z_]{1,64}$').hasMatch(error)
          ? error
          : null,
    );
  }
}

class VirtualDisplayState {
  const VirtualDisplayState({
    this.supported = false,
    this.activeCount,
    this.checkedAt,
    this.stale = true,
    this.busy = false,
    this.operation,
    this.error,
  });
  final bool supported, stale, busy;
  final int? activeCount;
  final DateTime? checkedAt;
  final VirtualDisplayOperation? operation;
  final String? error;
  factory VirtualDisplayState.fromJson(
    Map<String, dynamic> data, {
    String? operationId,
    bool requireOperation = false,
  }) {
    final count = data['activeCount'], rawOperation = data['operation'];
    if ((count != null && (count is! int || count < 0 || count > 2)) ||
        (requireOperation && rawOperation is! Map<String, dynamic>)) {
      throw const FormatException('Invalid display state');
    }
    final operation = rawOperation is Map<String, dynamic>
        ? VirtualDisplayOperation.fromJson(rawOperation)
        : null;
    if (operationId != null && operation?.id != operationId) {
      throw const FormatException('Mismatched display operation');
    }
    return VirtualDisplayState(
      supported: data['supported'] == true,
      activeCount: count as int?,
      checkedAt: _date(data['checkedAt']),
      stale: data['stale'] != false,
      busy: data['busy'] == true,
      operation: operation,
      error: data['error'] == 'host_offline' ? 'host_offline' : null,
    );
  }
  bool applied(int count, String operationId) =>
      supported &&
      !stale &&
      !busy &&
      checkedAt != null &&
      activeCount == count &&
      operation?.id == operationId &&
      operation?.status == 'succeeded' &&
      operation?.error == null &&
      operation?.requestedCount == count;

  VirtualDisplayState pending() => VirtualDisplayState(
    supported: supported,
    activeCount: activeCount,
    checkedAt: checkedAt,
    stale: true,
    busy: true,
    operation: operation,
    error: error,
  );
  String get label => busy
      ? l10n.checkingMonitors
      : activeCount == null
      ? l10n.checkMonitorCount
      : l10n.text13(activeCount!, stale ? l10n.checkAgain2 : '');
}
