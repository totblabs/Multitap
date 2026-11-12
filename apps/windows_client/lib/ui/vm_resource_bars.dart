import 'dart:async';
import 'dart:math' as math;

import 'package:flutter/material.dart';

import '../models/vm_resources.dart';
import 'app_theme.dart';
import 'korean_text.dart';
import '../l10n/l10n.dart';

// --- shared wording and tones ---------------------------------------------

const _gib = 1024 * 1024 * 1024;
const _mib = 1024 * 1024;

/// A meter's colour for its load: its own calm colour, amber from 80 % and
/// red from 95 % (2026-10-26: never alarming by default).
Color loadColor(AppPalette p, double? fraction, Color calm) => fraction == null
    ? calm
    : fraction >= .95
    ? p.red
    : fraction >= .8
    ? p.amber
    : calm;

/// The value text beside a meter: ink, or the load colour once it is high.
Color loadInk(AppPalette p, double? fraction) => fraction == null
    ? p.ink
    : fraction >= .95
    ? p.red
    : fraction >= .8
    ? p.amberInk
    : p.ink;

/// "23%", or "<1%" for a share that is small but not zero.
String percentText(double percent) =>
    percent > 0 && percent < 1 ? '<1%' : '${percent.round()}%';

String _number(double value, {required bool whole}) {
  if (whole && (value - value.round()).abs() < .05) return '${value.round()}';
  return value.toStringAsFixed(1);
}

/// A configured size as Windows shows it: "8 GB", "1.5 GB", "512 MB".
String memorySizeText(int bytes) => bytes >= _gib
    ? '${_number(bytes / _gib, whole: true)} GB'
    : '${(bytes / _mib).round()} MB';

/// A measured amount in the unit of [total] when known: "5.2 GB", "310 MB".
String memoryUsedText(int bytes, {int? total}) => (total ?? bytes) >= _gib
    ? '${_number(bytes / _gib, whole: false)} GB'
    : '${(bytes / _mib).round()} MB';

/// Who measured [sample]: the Host on a 로컬 PC, the Host inside a VM
/// (sample.measuredByHost), or Proxmox from outside. One wording for the
/// meters in the readiness dialog and the launcher card's strip; the dialog
/// used to call a Host-measured sample "Proxmox" and add Proxmox's memory note.
String resourceSourceText(VmResources sample, {required bool pc}) => pc
    ? l10n.measuredLocalPc
    : sample.measuredByHost
    ? l10n.measuredInsideDesktop
    : l10n.fromProxmox;

/// Proxmox counts memory differently from Windows; a Host's own measurement
/// needs no such note.
bool resourceNeedsMemoryNote(VmResources sample, {required bool pc}) =>
    !pc && !sample.measuredByHost;

/// "CPU 4코어" and "메모리 8 GB": the meters' labels and the spec line.
String? cpuSpecText(VmResources r) =>
    r.vcpus == null ? null : l10n.cpu(r.vcpus!);
String? memorySpecText(VmResources r) => r.memoryTotalBytes == null
    ? null
    : l10n.memory(memorySizeText(r.memoryTotalBytes!));

/// The short uptime beside 실행 중: "방금 켜짐", "12분", "3시간", "4일".
String uptimeText(Duration d) {
  if (d.inMinutes < 1) return l10n.justStarted;
  if (d.inHours < 1) return l10n.min2(d.inMinutes);
  if (d.inDays < 1) return l10n.hr(d.inHours);
  return l10n.text37(d.inDays);
}

/// The precise uptime for a tooltip: "3시간 12분", "4일 6시간".
String uptimeLongText(Duration d) {
  if (d.inMinutes < 1) return l10n.under1Min;
  if (d.inHours < 1) return l10n.min2(d.inMinutes);
  final minutes = d.inMinutes % 60, hours = d.inHours % 24;
  if (d.inDays < 1) {
    return minutes == 0 ? l10n.hr(d.inHours) : l10n.hrMin(d.inHours, minutes);
  }
  return hours == 0 ? l10n.text37(d.inDays) : l10n.hr2(d.inDays, hours);
}

String _clock(DateTime time) {
  final t = time.toLocal();
  String two(int n) => n.toString().padLeft(2, '0');
  return '${two(t.hour)}:${two(t.minute)}:${two(t.second)}';
}

String get _memoryNote => l10n.memoryValueProxmoxReportsSo;

// --- the readiness check's bars ----------------------------------------------

class VmResourceBars extends StatelessWidget {
  const VmResourceBars({super.key, required this.sample, this.pc = false});
  final VmResources sample;

  /// A physical PC: its Host measured the numbers.
  final bool pc;

  @override
  Widget build(BuildContext context) {
    final p = AppPalette.of(context);
    final cpu = sample.cpuUsagePercent;
    final used = sample.memoryUsedBytes, total = sample.memoryTotalBytes;
    final cpuText =
        '${cpu == null ? '—' : '${cpu.toStringAsFixed(1)}%'}'
        '${sample.vcpus == null ? '' : l10n.text38(sample.vcpus!)}';
    final memoryText = used == null && total == null
        ? '—'
        : '${used == null ? '—' : memoryUsedText(used, total: total)}'
              ' / ${total == null ? '—' : memorySizeText(total)}';
    final ratio = sample.memoryFraction;
    final hasValues = cpu != null || used != null;
    Widget metric(
      String label,
      String value,
      double? fraction,
      Color color, {
      bool previous = false,
    }) => Column(
      crossAxisAlignment: CrossAxisAlignment.stretch,
      children: [
        Row(
          children: [
            KeepAllText(
              label,
              style: TextStyle(fontSize: 12, height: 1.25, color: p.muted),
            ),
            if (previous) ...[
              const SizedBox(width: 6),
              KeepAllText(
                l10n.previousValue,
                style: TextStyle(fontSize: 12, height: 1.25, color: p.muted),
              ),
            ],
            const SizedBox(width: 8),
            Expanded(
              child: KeepAllText(
                value,
                maxLines: 1,
                overflow: TextOverflow.ellipsis,
                textAlign: TextAlign.end,
                style: TextStyle(
                  fontSize: 12,
                  height: 1.25,
                  fontWeight: FontWeight.w500,
                  color: sample.stale ? p.muted : p.ink,
                ),
              ),
            ),
          ],
        ),
        const SizedBox(height: 3),
        ClipRRect(
          borderRadius: BorderRadius.circular(2),
          child: ExcludeSemantics(
            child: LinearProgressIndicator(
              value: (fraction ?? 0).clamp(0.0, 1.0),
              minHeight: 3,
              color: sample.stale
                  ? p.muted.withValues(alpha: .45)
                  : loadColor(p, fraction, color),
              backgroundColor: p.line.withValues(alpha: .7),
            ),
          ),
        ),
      ],
    );
    final time = sample.checkedAt?.toLocal().toString().split('.').first;
    final uptime = sample.uptimeAt(DateTime.now());
    return Tooltip(
      message:
          '${resourceSourceText(sample, pc: pc)}${time == null ? '' : ' · $time'}'
          '${sample.stale ? l10n.couldntGetLatestStatus : ''}'
          '${resourceNeedsMemoryNote(sample, pc: pc) ? '\n$_memoryNote' : ''}',
      child: Column(
        crossAxisAlignment: CrossAxisAlignment.stretch,
        children: [
          metric(
            'CPU',
            cpuText,
            cpu == null ? null : cpu / 100,
            p.blue,
            previous: sample.stale && hasValues,
          ),
          const SizedBox(height: 8),
          metric(l10n.memory2, memoryText, ratio, p.purple),
          if (uptime != null) ...[
            const SizedBox(height: 8),
            KeepAllText(
              l10n.onFor(uptimeLongText(uptime)),
              key: const Key('readiness-uptime'),
              style: TextStyle(fontSize: 12, height: 1.25, color: p.muted),
            ),
          ],
        ],
      ),
    );
  }
}

// --- the launcher card's strip ---------------------------------------------

/// CPU and memory on a launcher card, always the same height: two live
/// meters while Proxmox (or a PC's own Host) measures the running desktop;
/// otherwise (off, stale or not measured) the configured size alone in the
/// same columns, over a hairline instead of a gauge, so every card keeps its
/// size. A running VM (or a connected PC) says 측정 중 there until its first
/// sample, for at most [measuringFor]: the broker pushes one about every
/// 5-10 s, so a longer wait is no measurement.
class VmResourceStrip extends StatefulWidget {
  const VmResourceStrip({
    super.key,
    required this.sample,
    required this.running,
    this.touch = false,
    this.example = false,
    this.pc = false,
  });

  final VmResources sample;

  /// Proxmox reports the VM running now (a fresh power state); for a PC, its
  /// Host is connected.
  final bool running;

  /// A physical PC: its Host measures everything, there is no Proxmox.
  final bool pc;

  /// The Android card: larger type and a thicker gauge for a phone.
  final bool touch;

  /// The design demo's sample values, never a measurement.
  final bool example;

  static double _font(bool touch) => touch ? 12.5 : 11.5;
  static double _bar(bool touch) => touch ? 5 : 4;
  static double _gap(bool touch) => touch ? 6 : 5;

  /// How long a running VM without a sample says 측정 중.
  static const measuringFor = Duration(seconds: 20);

  static bool _stacked(TextScaler scaler) => scaler.scale(12) > 12 * 1.3;

  /// Expanded text gets one full-width row per resource. Every card reserves
  /// the same space, including cards whose latest sample is unavailable.
  static double heightFor(TextScaler scaler, {bool touch = false}) {
    final meter = math.max(
      touch ? 28.0 : 24.0,
      scaler.scale(_font(touch)) * 1.3 + _gap(touch) + _bar(touch),
    );
    return _stacked(scaler) ? meter * 2 + 8 : meter;
  }

  @override
  State<VmResourceStrip> createState() => _VmResourceStripState();
}

class _VmResourceStripState extends State<VmResourceStrip> {
  VmResources get sample => widget.sample;
  bool get running => widget.running;
  bool get touch => widget.touch;
  bool get example => widget.example;
  bool get pc => widget.pc;

  Timer? _patience;
  bool _waitedOut = false;

  bool get _awaiting => running && !sample.measured && !example;
  bool get _measuring => _awaiting && !_waitedOut;

  void _watch() {
    if (!_awaiting) {
      _patience?.cancel();
      _patience = null;
      _waitedOut = false;
      return;
    }
    if (_patience != null || _waitedOut) return;
    // A sample that lapsed long ago (Android's expiry) waited already.
    final received = sample.receivedAt;
    final left =
        VmResourceStrip.measuringFor -
        (received == null
            ? Duration.zero
            : DateTime.now().toUtc().difference(received));
    if (left <= Duration.zero) {
      _waitedOut = true;
      return;
    }
    _patience = Timer(left, () {
      if (mounted) setState(() => _waitedOut = true);
    });
  }

  @override
  void initState() {
    super.initState();
    _watch();
  }

  @override
  void didUpdateWidget(covariant VmResourceStrip oldWidget) {
    super.didUpdateWidget(oldWidget);
    _watch();
  }

  @override
  void dispose() {
    _patience?.cancel();
    super.dispose();
  }

  bool get _live => running && sample.measured;

  String get _tooltip {
    final spec = [?cpuSpecText(sample), ?memorySpecText(sample)].join(' · ');
    if (example) return l10n.sampleValuesDesignNotMeasured;
    if (_measuring) return l10n.gettingCpuMemoryUsage;
    if (_live) {
      final time = sample.checkedAt;
      final cpu = sample.cpuUsagePercent, used = sample.memoryUsedBytes;
      final total = sample.memoryTotalBytes;
      final amounts = [
        if (cpu != null)
          'CPU ${cpu.toStringAsFixed(1)}%'
              '${sample.vcpus == null ? '' : l10n.text39(sample.vcpus!)}',
        if (used != null)
          l10n.memory3(memoryUsedText(used, total: total), total == null ? '' : ' / ${memorySizeText(total)}'),
      ].join(' · ');
      final source = resourceSourceText(sample, pc: pc);
      // As before: no Proxmox memory note for a Host's own measurement.
      final note = !sample.measuredByHost;
      return '$source${time == null ? '' : ' · ${_clock(time)}'}\n'
          '$amounts${note ? '\n$_memoryNote' : ''}';
    }
    if (pc && (spec.isEmpty || !running)) {
      return running ? l10n.couldntGetCpuMemoryInfo : l10n.usageShowsWhileIts;
    }
    if (spec.isEmpty) return l10n.couldntGetCpuMemoryInfo2;
    if (!running) return l10n.configuredSpecsUsageShowsWhile(spec);
    return l10n.couldntGetLatestUsageShows(pc ? l10n.specs : l10n.configuredSpecs, spec);
  }

  @override
  Widget build(BuildContext context) {
    final p = AppPalette.of(context);
    final scaler = MediaQuery.textScalerOf(context);
    final font = VmResourceStrip._font(touch);
    final live = _live, measuring = _measuring;
    final cpu = live ? sample.cpuUsagePercent : null;
    final used = live ? sample.memoryUsedBytes : null;
    // Both meters read "size · share in use"; a memory sample without a
    // configured size shows the amount instead.
    final memoryShare = used == null ? null : sample.memoryFraction;
    final cpuLabel = cpuSpecText(sample) ?? 'CPU';
    final memoryLabel = memorySpecText(sample) ?? l10n.memory2;
    final semantics = live
        ? [
            '$cpuLabel${cpu == null ? '' : l10n.used(percentText(cpu))}',
            '$memoryLabel${used == null ? '' : l10n.used2(memoryUsedText(used, total: sample.memoryTotalBytes))}',
          ].join(', ')
        : measuring
        ? l10n.measuring(cpuLabel, memoryLabel)
        : '$cpuLabel, $memoryLabel';
    final stacked = VmResourceStrip._stacked(scaler);
    final strip = Flex(
      direction: stacked ? Axis.vertical : Axis.horizontal,
      crossAxisAlignment: CrossAxisAlignment.stretch,
      children: [
        Expanded(
          child: _Meter(
            key: const Key('resource-cpu'),
            label: cpuLabel,
            value: cpu,
            fraction: cpu == null ? null : cpu / 100,
            text: percentText,
            calm: p.blue,
            live: live,
            measuring: measuring,
            touch: touch,
          ),
        ),
        SizedBox(
          width: stacked ? 0 : (touch ? 20 : 16),
          height: stacked ? 8 : 0,
        ),
        Expanded(
          child: _Meter(
            key: const Key('resource-memory'),
            label: memoryLabel,
            value: memoryShare != null ? memoryShare * 100 : used?.toDouble(),
            fraction: memoryShare,
            text: memoryShare != null
                ? percentText
                : (bytes) => memoryUsedText(bytes.round()),
            calm: p.purple,
            live: live,
            measuring: measuring,
            touch: touch,
          ),
        ),
      ],
    );
    return Semantics(
      container: true,
      label: semantics,
      child: ExcludeSemantics(
        child: Tooltip(
          message: _tooltip,
          waitDuration: const Duration(milliseconds: 500),
          child: SizedBox(
            height: VmResourceStrip.heightFor(scaler, touch: touch),
            child: DefaultTextStyle.merge(
              style: uiText(size: font, height: 1.3),
              child: strip,
            ),
          ),
        ),
      ),
    );
  }
}

class _Meter extends StatelessWidget {
  const _Meter({
    super.key,
    required this.label,
    required this.value,
    required this.fraction,
    required this.text,
    required this.calm,
    required this.live,
    required this.measuring,
    required this.touch,
  });
  final String label;

  /// The measured amount (percent or bytes); null shows the size alone.
  final double? value;
  final double? fraction;
  final String Function(double value) text;
  final Color calm;

  /// [measuring]: the first sample is still on its way (측정 중).
  final bool live, measuring, touch;

  static const _fade = Duration(milliseconds: 250);

  @override
  Widget build(BuildContext context) {
    final p = AppPalette.of(context);
    final bar = VmResourceStrip._bar(touch);
    final measured = live && value != null;
    final color = loadColor(p, fraction, calm);
    final labelStyle = TextStyle(color: p.muted, fontWeight: FontWeight.w500);
    // A new sample is one frame. Easing each VM's sample every few seconds keeps
    // the whole window redrawing: three cores on a PC with no GPU.
    final shown = (fraction ?? 0).clamp(0.0, 1.0);
    return Column(
      crossAxisAlignment: CrossAxisAlignment.stretch,
      children: [
        Expanded(
          child: LayoutBuilder(
            builder: (context, row) => Row(
              crossAxisAlignment: CrossAxisAlignment.center,
              children: [
                Expanded(
                  child: KeepAllText(
                    label,
                    maxLines: 1,
                    overflow: TextOverflow.ellipsis,
                    style: labelStyle,
                  ),
                ),
                // Measurements arriving or lapsing fade instead of popping.
                AnimatedSwitcher(
                  duration: _fade,
                  child: !measured
                      ? measuring
                            // Never wider than the meter with large text.
                            ? ConstrainedBox(
                                key: const ValueKey('measuring'),
                                constraints: BoxConstraints(
                                  maxWidth: row.maxWidth * .6,
                                ),
                                child: Padding(
                                  padding: const EdgeInsets.only(left: 6),
                                  child: KeepAllText(
                                    l10n.measuring2,
                                    maxLines: 1,
                                    overflow: TextOverflow.ellipsis,
                                    style: TextStyle(color: p.muted),
                                  ),
                                ),
                              )
                            : const SizedBox.shrink()
                      : Padding(
                          key: const ValueKey('value'),
                          padding: const EdgeInsets.only(left: 6),
                          child: KeepAllText(
                            text(value!),
                            maxLines: 1,
                            style: TextStyle(
                              color: loadInk(p, fraction),
                              fontWeight: FontWeight.w600,
                              fontFeatures: const [
                                FontFeature.tabularFigures(),
                              ],
                            ),
                          ),
                        ),
                ),
              ],
            ),
          ),
        ),
        SizedBox(height: VmResourceStrip._gap(touch)),
        SizedBox(
          height: bar,
          child: AnimatedSwitcher(
            duration: _fade,
            layoutBuilder: (current, previous) =>
                Stack(fit: StackFit.expand, children: [...previous, ?current]),
            child: measured
                ? DecoratedBox(
                    key: const ValueKey('gauge'),
                    decoration: BoxDecoration(
                      color: p.dark
                          ? const Color(0xFF283037)
                          : const Color(0xFFE3E8EC),
                      borderRadius: BorderRadius.circular(bar / 2),
                    ),
                    child: Align(
                      alignment: Alignment.centerLeft,
                      child: FractionallySizedBox(
                        widthFactor: shown == 0 ? 0 : math.max(shown, .015),
                        heightFactor: 1,
                        child: DecoratedBox(
                          decoration: BoxDecoration(
                            color: color,
                            borderRadius: BorderRadius.circular(bar / 2),
                          ),
                        ),
                      ),
                    ),
                  )
                // No measurement: a hairline rail, never an empty gauge that
                // would read as 0 %.
                : Center(
                    key: const ValueKey('rail'),
                    child: Container(
                      height: 1,
                      color: p.dark
                          ? const Color(0xFF2A3239)
                          : const Color(0xFFDCE1E5),
                    ),
                  ),
          ),
        ),
      ],
    );
  }
}
