import 'dart:async';

import 'package:flutter/material.dart';
import 'package:flutter/services.dart';

import 'app_dialog.dart';
import 'app_theme.dart';
import 'korean_text.dart';
import '../l10n/l10n.dart';

class SessionMetric {
  const SessionMetric(this.label, this.value);
  final String label, value;
}

class SessionMetricSection {
  const SessionMetricSection(this.title, this.metrics, {this.note});
  final String title;
  final List<SessionMetric> metrics;
  final String? note;
}

class SessionStatsDialog extends StatefulWidget {
  const SessionStatsDialog({
    super.key,
    required this.title,
    required this.machine,
    required this.sections,
    this.note,
    this.observationEnabled,
    this.onObservationChanged,
    this.reportJson,
  });
  final String title, machine;
  final List<SessionMetricSection> Function() sections;
  final String? note;
  final bool Function()? observationEnabled;
  final ValueChanged<bool>? onObservationChanged;
  final String Function()? reportJson;
  @override
  State<SessionStatsDialog> createState() => _SessionStatsDialogState();
}

class _SessionStatsDialogState extends State<SessionStatsDialog> {
  AppPalette get _palette => AppPalette.of(context);
  final _scroll = ScrollController();
  List<SessionMetricSection> _displayedSections = const [];
  Timer? _refresh;
  bool _copying = false;
  String _copyLabel = l10n.copyDiagnostics2;
  @override
  void initState() {
    super.initState();
    _refresh = Timer.periodic(const Duration(seconds: 1), (_) {
      if (mounted) setState(() {});
    });
  }

  @override
  void dispose() {
    _refresh?.cancel();
    _scroll.dispose();
    super.dispose();
  }

  Future<void> _copy() async {
    if (_copying) return;
    setState(() => _copying = true);
    // Only displayed, allowlisted metrics; no API response, URL or credential.
    final text = StringBuffer('${widget.title}\n${widget.machine}\n');
    for (final section in _displayedSections) {
      text.writeln('\n${section.title}');
      for (final metric in section.metrics) {
        text.writeln('${metric.label}: ${metric.value}');
      }
    }
    try {
      await Clipboard.setData(ClipboardData(text: text.toString()));
      if (mounted) setState(() => _copyLabel = l10n.copied);
    } catch (_) {
      if (mounted) setState(() => _copyLabel = l10n.copyAgain);
    } finally {
      if (mounted) setState(() => _copying = false);
    }
  }

  Future<void> _copyReport() async {
    if (_copying || widget.reportJson == null) return;
    setState(() => _copying = true);
    try {
      await Clipboard.setData(ClipboardData(text: widget.reportJson!()));
      if (mounted) setState(() => _copyLabel = l10n.copied);
    } catch (_) {
      if (mounted) setState(() => _copyLabel = l10n.copyAgain);
    } finally {
      if (mounted) setState(() => _copying = false);
    }
  }

  Widget _row(SessionMetric metric) => Padding(
    padding: const EdgeInsets.symmetric(vertical: 7),
    child: LayoutBuilder(
      builder: (context, bounds) {
        final label = KeepAllText(
          metric.label,
          style: TextStyle(color: _palette.muted, fontSize: 12),
        );
        final value = KeepAllText(
          metric.value,
          style: TextStyle(
            color: _palette.ink,
            fontSize: 13,
            fontWeight: FontWeight.w600,
            height: 1.5,
          ),
        );
        if (bounds.maxWidth / MediaQuery.textScalerOf(context).scale(1) < 300) {
          return Column(
            crossAxisAlignment: CrossAxisAlignment.start,
            children: [label, const SizedBox(height: 3), value],
          );
        }
        return Row(
          crossAxisAlignment: CrossAxisAlignment.start,
          children: [
            Expanded(flex: 4, child: label),
            const SizedBox(width: 16),
            Expanded(
              flex: 6,
              child: Align(alignment: Alignment.centerRight, child: value),
            ),
          ],
        );
      },
    ),
  );
  @override
  Widget build(BuildContext context) {
    final p = _palette;
    final size = MediaQuery.sizeOf(context);
    final compact =
        size.width / MediaQuery.textScalerOf(context).scale(1) < 380;
    _displayedSections = widget.sections();
    return AppDialog(
      icon: Icons.insights_rounded,
      title: widget.title,
      subtitle: widget.machine,
      maxWidth: 540,
      scrollBody: false,
      body: [
        Flexible(
          child: Scrollbar(
            controller: _scroll,
            thumbVisibility: true,
            child: SingleChildScrollView(
              key: const Key('session-stats-scroll'),
              controller: _scroll,
              padding: const EdgeInsets.only(right: 12),
              child: Column(
                crossAxisAlignment: CrossAxisAlignment.stretch,
                mainAxisSize: MainAxisSize.min,
                children: [
                  for (final section in _displayedSections) ...[
                    Semantics(
                      header: true,
                      child: Padding(
                        padding: const EdgeInsets.only(left: 2, bottom: 4),
                        child: KeepAllText(
                          section.title,
                          style: uiText(
                            size: 12,
                            weight: FontWeight.w600,
                            color: p.muted,
                          ),
                        ),
                      ),
                    ),
                    DecoratedBox(
                      decoration: BoxDecoration(
                        color: p.tileWell,
                        borderRadius: BorderRadius.circular(12),
                        border: Border.all(color: p.tileLine),
                      ),
                      child: Padding(
                        padding: const EdgeInsets.symmetric(
                          horizontal: 14,
                          vertical: 4,
                        ),
                        child: Column(
                          crossAxisAlignment: CrossAxisAlignment.stretch,
                          children: [
                            for (final metric in section.metrics) _row(metric),
                          ],
                        ),
                      ),
                    ),
                    if (section.note != null)
                      Padding(
                        padding: const EdgeInsets.fromLTRB(2, 6, 2, 0),
                        child: KeepAllText(
                          section.note!,
                          style: uiText(
                            size: 12,
                            height: 1.5,
                            color: p.muted,
                          ),
                        ),
                      ),
                    const SizedBox(height: 14),
                  ],
                  if (widget.note != null)
                    Padding(
                      padding: const EdgeInsets.only(bottom: 8),
                      child: KeepAllText(
                        widget.note!,
                        style: uiText(size: 12, height: 1.5, color: p.muted),
                      ),
                    ),
                ],
              ),
            ),
          ),
        ),
      ],
      actions: [
        if (widget.onObservationChanged != null)
          TextButton.icon(
            key: const Key('toggle-stream-observability'),
            onPressed: () {
              widget.onObservationChanged!(
                widget.observationEnabled?.call() != true,
              );
              setState(() {});
            },
            icon: Icon(
              widget.observationEnabled?.call() == true
                  ? Icons.check_box_rounded
                  : Icons.check_box_outline_blank_rounded,
            ),
            label: Text(l10n.detailedDiagnostics),
          ),
        if (widget.reportJson != null)
          TextButton.icon(
            key: const Key('copy-stream-observability'),
            onPressed: _copying ? null : _copyReport,
            icon: const Icon(Icons.data_object_rounded),
            label: Text(l10n.copyJson),
          ),
        TextButton.icon(
          key: const Key('copy-session-diagnostics'),
          onPressed: _copying ? null : _copy,
          style: AppButtons.quiet(p),
          icon: _copying
              ? const SizedBox.square(
                  dimension: 16,
                  child: CircularProgressIndicator(strokeWidth: 2),
                )
              : Icon(
                  _copyLabel == l10n.copied
                      ? Icons.check_rounded
                      : Icons.copy_rounded,
                  size: 16,
                ),
          label: KeepAllText(
            compact
                ? _copyLabel == l10n.copyDiagnostics2
                      ? l10n.copy
                      : _copyLabel == l10n.copyAgain
                      ? l10n.tryAgain
                      : _copyLabel
                : _copyLabel,
          ),
        ),
        // Closing carries nothing out: the grey button, not the amber one.
        TextButton(
          key: const Key('close-session-stats'),
          onPressed: () => Navigator.pop(context),
          style: AppButtons.quiet(p),
          child: KeepAllText(l10n.close),
        ),
      ],
    );
  }
}
