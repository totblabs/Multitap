/// A Proxmox observation, separate from guest/streaming readiness. A physical
/// PC's comes from its Host alone (broker "source": "host").
class VmResources {
  const VmResources({
    this.cpuUsagePercent,
    this.vcpus,
    this.memoryUsedBytes,
    this.memoryTotalBytes,
    this.uptimeSeconds,
    this.checkedAt,
    this.receivedAt,
    this.stale = true,
    this.measuredByHost = false,
  });

  final double? cpuUsagePercent;
  final int? vcpus, memoryUsedBytes, memoryTotalBytes;

  /// Seconds since Proxmox started the VM, only while it runs (broker
  /// 2026-10-26; a released broker never sends it). A PC's: since Windows
  /// last started or woke (host-resources-v2).
  final int? uptimeSeconds;
  final DateTime? checkedAt, receivedAt;
  final bool stale;

  /// CPU and memory were measured inside the VM by the Multitap Host (broker
  /// "measuredBy": "host"), not read from Proxmox. Absent from a released
  /// broker, hence false.
  final bool measuredByHost;

  static const _maxUptimeSeconds = 100 * 366 * 86400;

  factory VmResources.fromJson(dynamic value, {DateTime? receivedAt}) {
    if (value is! Map ||
        (value['source'] != 'proxmox' && value['source'] != 'host')) {
      return const VmResources();
    }
    int? whole(dynamic n, {bool zero = false, int max = 9007199254740991}) =>
        n is int && n >= (zero ? 0 : 1) && n <= max ? n : null;
    final cpu = value['cpuUsagePercent'];
    final checked = value['checkedAt'] is String
        ? DateTime.tryParse(value['checkedAt'] as String)?.toUtc()
        : null;
    return VmResources(
      cpuUsagePercent: cpu is num && cpu.isFinite && cpu >= 0 && cpu <= 100
          ? cpu.toDouble()
          : null,
      vcpus: whole(value['vcpus'], max: 4096),
      memoryUsedBytes: whole(value['memoryUsedBytes'], zero: true),
      memoryTotalBytes: whole(value['memoryTotalBytes']),
      uptimeSeconds: whole(
        value['uptimeSeconds'],
        zero: true,
        max: _maxUptimeSeconds,
      ),
      checkedAt: checked,
      receivedAt: receivedAt ?? DateTime.now().toUtc(),
      stale: value['stale'] != false || checked == null,
      measuredByHost: value['measuredBy'] == 'host',
    );
  }

  /// A current CPU or memory measurement (not only the configured size).
  bool get measured =>
      !stale && (cpuUsagePercent != null || memoryUsedBytes != null);

  /// Used memory as a share of the configured size, at most 1. Proxmox can
  /// report more than the size (a guest without the balloon service).
  double? get memoryFraction {
    final used = memoryUsedBytes, total = memoryTotalBytes;
    if (used == null || total == null || total <= 0) return null;
    return (used / total).clamp(0.0, 1.0);
  }

  /// How long the VM has run by [now]: the reported uptime plus the time
  /// since this sample arrived. Null while stale or without an uptime.
  Duration? uptimeAt(DateTime now) {
    final seconds = uptimeSeconds;
    if (stale || seconds == null) return null;
    final since = receivedAt == null
        ? Duration.zero
        : now.toUtc().difference(receivedAt!);
    return Duration(seconds: seconds) +
        (since.isNegative ? Duration.zero : since);
  }

  /// No sample for [after]: the measurement no longer describes the VM. While
  /// the broker's live channel is up it pushes a sample about every 5 s, so
  /// the callers allow 20 s; without it the gentle fallback list read (10 s)
  /// is the only source.
  bool expired(DateTime now, {Duration after = const Duration(seconds: 10)}) =>
      !stale &&
      receivedAt != null &&
      now.toUtc().difference(receivedAt!) >= after;

  VmResources asStale() => stale
      ? this
      : VmResources(
          cpuUsagePercent: cpuUsagePercent,
          vcpus: vcpus,
          memoryUsedBytes: memoryUsedBytes,
          memoryTotalBytes: memoryTotalBytes,
          uptimeSeconds: uptimeSeconds,
          checkedAt: checkedAt,
          receivedAt: receivedAt,
          measuredByHost: measuredByHost,
        );
}
