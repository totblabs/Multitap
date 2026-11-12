import 'dart:async';

import 'package:flutter/material.dart';

import '../data/update_handoff.dart';
import 'app_dialog.dart';
import 'app_theme.dart';
import 'update_messages.dart';
import 'korean_text.dart';
import '../l10n/l10n.dart';

/// The notice after an update relaunch: a small card in the corner that
/// never blocks the window. A plain success fades after a few seconds; a
/// failure or a Host notice stays until closed, with its next action.
class UpdateNoticeHost extends StatefulWidget {
  const UpdateNoticeHost({
    super.key,
    required this.child,
    this.notices,
    this.successDuration = const Duration(seconds: 8),
  });
  final Widget child;
  final UpdateNotices? notices;
  final Duration successDuration;
  @override
  State<UpdateNoticeHost> createState() => _UpdateNoticeHostState();
}

class _UpdateNoticeHostState extends State<UpdateNoticeHost> {
  UpdateNotices get _notices => widget.notices ?? UpdateNotices.instance;
  Timer? _timer;
  UpdateHandoff? _shown;
  bool _visible = false;

  @override
  void initState() {
    super.initState();
    _notices.addListener(_changed);
    WidgetsBinding.instance.addPostFrameCallback((_) => _changed());
  }

  @override
  void didUpdateWidget(UpdateNoticeHost oldWidget) {
    super.didUpdateWidget(oldWidget);
    if (!identical(oldWidget.notices, widget.notices)) {
      (oldWidget.notices ?? UpdateNotices.instance).removeListener(_changed);
      _notices.addListener(_changed);
    }
  }

  @override
  void dispose() {
    _timer?.cancel();
    _notices.removeListener(_changed);
    super.dispose();
  }

  bool _persistent(UpdateHandoff notice) =>
      !notice.updated || notice.attention || notice.restartRequired;

  void _changed() {
    if (!mounted) return;
    final current = _notices.current;
    if (current != null && !identical(current, _shown)) {
      setState(() {
        _shown = current;
        _visible = true;
      });
      _schedule(widget.successDuration);
    } else if (current == null && _visible) {
      setState(() => _visible = false);
    } else {
      setState(() {});
    }
  }

  void _schedule(Duration delay) {
    _timer?.cancel();
    final shown = _shown;
    if (shown == null || _persistent(shown)) return;
    _timer = Timer(delay, _notices.dismiss);
  }

  @override
  Widget build(BuildContext context) {
    final shown = _shown;
    // Always the same Stack (the routes keep their state); the app itself
    // keeps its exact, tight size.
    return Stack(
      fit: StackFit.expand,
      children: [
        widget.child,
        if (shown != null)
          Positioned(
            right: 0,
            bottom: 0,
            child: SafeArea(
              child: Padding(
                padding: const EdgeInsets.all(18),
                child: AnimatedSlide(
                  offset: _visible ? Offset.zero : const Offset(0, .25),
                  duration: const Duration(milliseconds: 220),
                  curve: Curves.easeOutCubic,
                  child: AnimatedOpacity(
                    opacity: _visible ? 1 : 0,
                    duration: const Duration(milliseconds: 220),
                    onEnd: () {
                      if (!_visible && mounted) setState(() => _shown = null);
                    },
                    child: IgnorePointer(
                      ignoring: !_visible,
                      child: MouseRegion(
                        // Reading pauses the fade of a plain success.
                        onEnter: (_) => _timer?.cancel(),
                        onExit: (_) => _schedule(const Duration(seconds: 4)),
                        child: UpdateNoticeCard(
                          notice: shown,
                          notices: _notices,
                        ),
                      ),
                    ),
                  ),
                ),
              ),
            ),
          ),
      ],
    );
  }
}

class UpdateNoticeCard extends StatelessWidget {
  const UpdateNoticeCard({
    super.key,
    required this.notice,
    required this.notices,
  });
  final UpdateHandoff notice;
  final UpdateNotices notices;

  @override
  Widget build(BuildContext context) {
    final p = AppPalette.of(context);
    final host = notice.product == 'host';
    final product = updateProductLabel(notice.product);
    final needsInstaller =
        !notice.updated &&
        (notice.code == 'existing_install_required' ||
            notice.code == 'certificate_trust_required' ||
            notice.code == 'driver_install_required');
    final AppTone tone;
    final IconData icon;
    final String title;
    final lines = <String>[];
    if (notice.updated) {
      title = l10n.updated(product, withRo(notice.toVersion));
      if (notice.restartRequired || notice.attention) {
        tone = AppTone.warn;
        icon = Icons.info_outline_rounded;
        if (notice.restartRequired) {
          lines.add(l10n.windowsNeedsRestartSaveWork);
        }
        if (notice.attention) {
          // Installer attention also covers shared drives and shortcuts.
          lines.add(
            host ? l10n.checkDiagnosticsHostSettings : l10n.installationFinishedButSomeItems,
          );
        }
      } else {
        tone = AppTone.success;
        icon = Icons.check_rounded;
        lines.add(
          host ? l10n.hostServiceRunningNewVersion : l10n.reopenedNewVersionSettingsUnchanged,
        );
      }
    } else {
      tone = needsInstaller ? AppTone.warn : AppTone.danger;
      icon = needsInstaller
          ? Icons.front_hand_rounded
          : Icons.error_outline_rounded;
      title = updateFailureTitle(notice.code);
      lines.add(
        updateFailureMessage(
          notice.code,
          product: notice.product,
          version: notice.toVersion,
          afterRelaunch: true,
        ),
      );
    }
    final server = notices.server, version = notices.currentVersion;
    final canOpen = server != null && version != null;
    final actions = <Widget>[
      if (notice.updated && server != null && notices.openHistory != null)
        _action(
          p,
          key: const Key('update-notice-history'),
          label: l10n.viewChanges,
          onPressed: () {
            notices.dismiss();
            unawaited(
              notices.openHistory!(
                server: server,
                product: notice.product,
                currentVersion: version ?? notice.toVersion,
              ),
            );
          },
        ),
      if (!notice.updated && canOpen && notices.openPanel != null) ...[
        if (!needsInstaller)
          _action(
            p,
            key: const Key('update-notice-retry'),
            label: l10n.tryAgain,
            onPressed: () {
              notices.dismiss();
              unawaited(
                notices.openPanel!(
                  server: server,
                  product: notice.product,
                  currentVersion: version,
                  manual: false,
                ),
              );
            },
          ),
        _action(
          p,
          key: const Key('update-notice-installer'),
          label: l10n.installInstaller,
          onPressed: () {
            notices.dismiss();
            unawaited(
              notices.openPanel!(
                server: server,
                product: notice.product,
                currentVersion: version,
                manual: true,
              ),
            );
          },
        ),
      ],
    ];
    final width = MediaQuery.sizeOf(context).width;
    return Semantics(
      liveRegion: true,
      container: true,
      child: DecoratedBox(
        decoration: BoxDecoration(
          borderRadius: BorderRadius.circular(16),
          boxShadow: [
            BoxShadow(
              color: p.shadow.withValues(alpha: p.dark ? .5 : .18),
              blurRadius: 24,
              offset: const Offset(0, 8),
            ),
          ],
        ),
        child: Material(
          key: const Key('update-notice'),
          color: p.tile,
          elevation: 0,
          shape: RoundedRectangleBorder(
            borderRadius: BorderRadius.circular(16),
            side: BorderSide(color: p.tileLine),
          ),
          clipBehavior: Clip.antiAlias,
          child: ConstrainedBox(
            constraints: BoxConstraints(
              maxWidth: (width - 36).clamp(260.0, 420.0),
            ),
            child: Padding(
              padding: const EdgeInsets.fromLTRB(16, 14, 8, 12),
              child: Row(
                crossAxisAlignment: CrossAxisAlignment.start,
                children: [
                  AppToneIcon(icon: icon, tone: tone, size: 34),
                  const SizedBox(width: 12),
                  Expanded(
                    child: Column(
                      mainAxisSize: MainAxisSize.min,
                      crossAxisAlignment: CrossAxisAlignment.start,
                      children: [
                        Padding(
                          padding: const EdgeInsets.only(top: 1, right: 4),
                          child: KeepAllText(
                            title,
                            key: const Key('update-notice-title'),
                            style: uiText(
                              size: 14.5,
                              weight: FontWeight.w600,
                              color: p.ink,
                              height: 1.35,
                            ),
                          ),
                        ),
                        for (final line in lines) ...[
                          const SizedBox(height: 4),
                          KeepAllText(
                            line,
                            style: uiText(
                              size: 13,
                              height: 1.5,
                              color: p.muted,
                            ),
                          ),
                        ],
                        if (!notice.updated) ...[
                          const SizedBox(height: 6),
                          KeepAllText(
                            l10n.code4(notice.code, notice.stage > 0 ? l10n.stoppedAfterStep6(notice.stage) : ''),
                            key: const Key('update-notice-code'),
                            style: uiText(
                              size: 12,
                              weight: FontWeight.w600,
                              color: p.tone(tone),
                            ),
                          ),
                        ],
                        if (actions.isNotEmpty) ...[
                          const SizedBox(height: 6),
                          Wrap(spacing: 4, runSpacing: 0, children: actions),
                        ],
                      ],
                    ),
                  ),
                  IconButton(
                    key: const Key('update-notice-close'),
                    onPressed: notices.dismiss,
                    style: IconButton.styleFrom(
                      foregroundColor: p.muted,
                      minimumSize: const Size.square(32),
                      maximumSize: const Size.square(32),
                      padding: EdgeInsets.zero,
                      shape: RoundedRectangleBorder(
                        borderRadius: BorderRadius.circular(9),
                      ),
                    ),
                    icon: const Icon(Icons.close_rounded, size: 18),
                  ),
                ],
              ),
            ),
          ),
        ),
      ),
    );
  }

  Widget _action(
    AppPalette p, {
    required Key key,
    required String label,
    required VoidCallback onPressed,
  }) => TextButton(
    key: key,
    onPressed: onPressed,
    style: TextButton.styleFrom(
      foregroundColor: p.amberInk,
      padding: const EdgeInsets.symmetric(horizontal: 8),
      minimumSize: const Size(0, 32),
      tapTargetSize: MaterialTapTargetSize.shrinkWrap,
      textStyle: uiText(size: 13, weight: FontWeight.w600),
    ),
    child: KeepAllText(label),
  );
}
