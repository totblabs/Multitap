/// Read-only broker evidence for a retired streaming worker and its display
/// recovery. Local socket closure and a count-command ACK cannot clear it.
class SessionCleanupState {
  const SessionCleanupState({
    this.pending = false,
    this.workerQuiescent = false,
    this.phase,
    this.checkedAt,
  });
  final bool pending, workerQuiescent;
  final String? phase;
  final DateTime? checkedAt;
  bool get allowsEmergencyControl => !pending || workerQuiescent;

  /// The broker's stop deadline; a normal stop and display restore is sooner.
  static const settleWindow = Duration(seconds: 45);

  /// Whether the wait has outlived normal cleanup: the Host reported an
  /// unconfirmed restore, the broker deadline passed, or a normal phase is
  /// older than [settleWindow] by this PC's clock (a broker clock ahead of
  /// this PC never makes it stalled sooner). Descriptive only: it never
  /// clears the barrier, which only a fresh broker snapshot can do.
  bool stalledAt(DateTime now) {
    if (!pending) return false;
    if (phase == 'recovery_required' || phase == 'unknown') return true;
    final at = checkedAt;
    return at != null && now.toUtc().difference(at) >= settleWindow;
  }

  bool get stalled => stalledAt(DateTime.now());

  factory SessionCleanupState.fromJson(Object? value) {
    // Released brokers predate this negotiated protocol.
    if (value == null) return const SessionCleanupState();
    if (value is! Map<String, dynamic> || value['pending'] is! bool) {
      throw const FormatException('Invalid session cleanup');
    }
    if (value['pending'] == false && value.length == 1) {
      return const SessionCleanupState();
    }
    final time = value['checkedAt'];
    final checkedAt = time is String && time.length <= 40
        ? DateTime.tryParse(time)
        : null;
    if (value['pending'] != true ||
        value.length != 4 ||
        value['workerQuiescent'] is! bool ||
        !{
          'pending',
          'awaiting_capabilities',
          'recovery_required',
          'unknown',
        }.contains(value['phase']) ||
        checkedAt == null ||
        !checkedAt.isUtc) {
      throw const FormatException('Invalid session cleanup');
    }
    return SessionCleanupState(
      pending: true,
      workerQuiescent: value['workerQuiescent'] as bool,
      phase: value['phase'] as String,
      checkedAt: checkedAt,
    );
  }
}
