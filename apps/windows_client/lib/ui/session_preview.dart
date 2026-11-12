import 'package:flutter/material.dart';

import '../models/desktop_machine.dart';
import 'app_dialog.dart';
import 'app_theme.dart';
import 'korean_text.dart';
import 'release_notes.dart' show VersionBadge;
import '../l10n/l10n.dart';

/// The design demo's stand-in for a session window: the same dark stage and
/// card as a real connecting window, clearly marked as not connected.
class SessionPreview extends StatefulWidget {
  const SessionPreview({
    super.key,
    required this.machine,
    required this.onClose,
  });
  final DesktopMachine machine;
  final VoidCallback onClose;

  @override
  State<SessionPreview> createState() => _SessionPreviewState();
}

class _SessionPreviewState extends State<SessionPreview> {
  static final _stage = buildAppTheme(brightness: Brightness.dark);
  bool _showDetails = true;

  @override
  Widget build(BuildContext context) => Theme(
    data: _stage,
    child: Builder(
      builder: (context) {
        final p = AppPalette.of(context);
        return ColoredBox(
          color: AppPalette.stage,
          child: Column(
            children: [
              Container(
                padding: const EdgeInsets.fromLTRB(12, 8, 12, 8),
                decoration: BoxDecoration(
                  color: p.headerGlass,
                  border: Border(bottom: BorderSide(color: p.glassEdge)),
                ),
                child: Row(
                  children: [
                    IconButton(
                      key: const Key('close-session-preview'),
                      onPressed: widget.onClose,
                      tooltip: l10n.backList,
                      color: p.muted,
                      icon: const Icon(Icons.arrow_back_rounded, size: 20),
                    ),
                    const SizedBox(width: 6),
                    Expanded(
                      child: KeepAllText(
                        widget.machine.name,
                        overflow: TextOverflow.ellipsis,
                        style: uiText(
                          size: 14,
                          weight: FontWeight.w600,
                          color: p.ink,
                        ),
                      ),
                    ),
                    VersionBadge(l10n.notConnected),
                    const SizedBox(width: 6),
                    IconButton(
                      onPressed: () =>
                          setState(() => _showDetails = !_showDetails),
                      tooltip: _showDetails ? l10n.hideDetails2 : l10n.showDetails,
                      color: p.muted,
                      icon: Icon(
                        _showDetails
                            ? Icons.info_rounded
                            : Icons.info_outline_rounded,
                        size: 20,
                      ),
                    ),
                  ],
                ),
              ),
              Expanded(
                child: Center(
                  child: SingleChildScrollView(
                    padding: const EdgeInsets.all(24),
                    child: ConstrainedBox(
                      constraints: const BoxConstraints(maxWidth: 440),
                      child: Container(
                        padding: const EdgeInsets.fromLTRB(22, 22, 22, 18),
                        decoration: BoxDecoration(
                          color: p.tile,
                          borderRadius: BorderRadius.circular(18),
                          border: Border.all(color: p.tileLine),
                        ),
                        child: Column(
                          mainAxisSize: MainAxisSize.min,
                          crossAxisAlignment: CrossAxisAlignment.stretch,
                          children: [
                            Row(
                              children: [
                                const AppToneIcon(
                                  icon: Icons.desktop_windows_outlined,
                                  tone: AppTone.info,
                                ),
                                const SizedBox(width: 14),
                                Expanded(
                                  child: Column(
                                    crossAxisAlignment:
                                        CrossAxisAlignment.start,
                                    children: [
                                      KeepAllText(
                                        l10n.remoteScreenOpensHere,
                                        style: uiText(
                                          size: 17,
                                          weight: FontWeight.w600,
                                          color: p.ink,
                                        ),
                                      ),
                                      const SizedBox(height: 3),
                                      KeepAllText(
                                        l10n.designDemoNoRealConnection,
                                        style: uiText(size: 13, color: p.muted),
                                      ),
                                    ],
                                  ),
                                ),
                              ],
                            ),
                            if (_showDetails) ...[
                              const SizedBox(height: 16),
                              DecoratedBox(
                                decoration: BoxDecoration(
                                  color: p.tileWell,
                                  borderRadius: BorderRadius.circular(12),
                                  border: Border.all(color: p.tileLine),
                                ),
                                child: Padding(
                                  padding: EdgeInsets.symmetric(vertical: 12),
                                  child: Row(
                                    children: [
                                      _SessionMetric(
                                        label: l10n.connectionStatus,
                                        value: l10n.notConnected2,
                                      ),
                                      _SessionMetric(
                                        label: l10n.frameRate,
                                        value: '—',
                                      ),
                                      _SessionMetric(label: l10n.latency, value: '—'),
                                      _SessionMetric(
                                        label: l10n.input,
                                        value: l10n.inactive,
                                      ),
                                    ],
                                  ),
                                ),
                              ),
                            ],
                            const SizedBox(height: 18),
                            TextButton(
                              onPressed: widget.onClose,
                              style: AppButtons.quiet(p),
                              child: KeepAllText(l10n.backDesktopList),
                            ),
                          ],
                        ),
                      ),
                    ),
                  ),
                ),
              ),
            ],
          ),
        );
      },
    ),
  );
}

class _SessionMetric extends StatelessWidget {
  const _SessionMetric({required this.label, required this.value});
  final String label;
  final String value;

  @override
  Widget build(BuildContext context) {
    final p = AppPalette.of(context);
    return Expanded(
      child: Column(
        children: [
          KeepAllText(
            label,
            textAlign: TextAlign.center,
            style: uiText(size: 12, color: p.muted),
          ),
          const SizedBox(height: 4),
          KeepAllText(
            value,
            textAlign: TextAlign.center,
            style: uiText(size: 13.5, weight: FontWeight.w600, color: p.ink),
          ),
        ],
      ),
    );
  }
}
