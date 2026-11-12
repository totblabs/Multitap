import 'package:flutter/material.dart';

import 'app_dialog.dart';
import 'app_theme.dart';
import 'korean_text.dart';
import '../l10n/l10n.dart';

/// Connecting / failed card on the session window's dark stage. The stage
/// stays dark in both appearances (like the session bar), so the card uses
/// the dark palette too: a white card on a black stage read as a hole.
class SessionStatusPanel extends StatelessWidget {
  const SessionStatusPanel({
    super.key,
    required this.machine,
    required this.message,
    required this.onBack,
    this.busy = false,
    this.onRetry,
    this.onAlternative,
    this.status,
    this.details = const [],
    this.onSettings,
    String? settingsLabel,
    String? alternativeLabel,
    String? backLabel,
    this.title,
    this.icon = Icons.link_off_rounded,
    String? retryLabel,
    this.retryKey = const Key('retry-remote-connection'),
    this.retryIcon = Icons.refresh_rounded,
  }) : _settingsLabel = settingsLabel, // ignore: prefer_initializing_formals
       _alternativeLabel = alternativeLabel, // ignore: prefer_initializing_formals
       _backLabel = backLabel, // ignore: prefer_initializing_formals
       _retryLabel = retryLabel; // ignore: prefer_initializing_formals
  final IconData retryIcon;
  final String machine, message;
  final bool busy;
  final VoidCallback onBack;
  final VoidCallback? onRetry;
  final VoidCallback? onAlternative;

  /// Optional measured connection progress (the list of stages), below the
  /// explanation.
  final Widget? status;

  /// Quiet facts under the explanation, one per line (the connections a
  /// takeover would end: whose, and since when).
  final List<String> details;

  /// Opens settings only; it never starts another connection itself.
  final VoidCallback? onSettings;
  final String? _settingsLabel, _alternativeLabel, _backLabel, _retryLabel;
  String get settingsLabel => _settingsLabel ?? l10n.checkSettings;
  String get alternativeLabel => _alternativeLabel ?? l10n.console;
  String get backLabel => _backLabel ?? l10n.backList;
  final IconData icon;
  String get retryLabel => _retryLabel ?? l10n.reconnect;
  final Key retryKey;

  /// Replaces the connecting / failed headline.
  final String? title;

  static final _stage = buildAppTheme(brightness: Brightness.dark);

  @override
  Widget build(BuildContext context) => Theme(
    data: _stage,
    child: LayoutBuilder(
      builder: (context, constraints) {
        final p = AppPalette.of(context);
        final compact =
            constraints.maxWidth / MediaQuery.textScalerOf(context).scale(1) <
            400;
        final headline =
            title ??
            (compact
                ? (busy ? l10n.connecting3 : l10n.notConnected3)
                : (busy ? l10n.connectingDesktop : l10n.notConnected4));
        final actions = <Widget>[
          TextButton(
            key: const Key('leave-remote-connection'),
            onPressed: onBack,
            style: AppButtons.quiet(p),
            child: KeepAllText(backLabel),
          ),
          if (onAlternative != null && !busy)
            OutlinedButton.icon(
              key: const Key('emergency-console-alternative'),
              onPressed: onAlternative,
              style: AppButtons.outlined(p),
              icon: const Icon(Icons.settings_remote_outlined, size: 18),
              label: KeepAllText(alternativeLabel),
            ),
          if (onSettings != null && !busy)
            OutlinedButton.icon(
              key: const Key('recover-remote-settings'),
              onPressed: onSettings,
              style: AppButtons.outlined(p),
              icon: const Icon(Icons.tune_rounded, size: 18),
              label: KeepAllText(settingsLabel),
            ),
          if (onRetry != null && !busy)
            FilledButton.icon(
              key: retryKey,
              onPressed: onRetry,
              style: AppButtons.filled(p),
              icon: Icon(retryIcon, size: 18),
              label: KeepAllText(retryLabel),
            ),
        ];
        return Center(
          child: SingleChildScrollView(
            padding: EdgeInsets.symmetric(
              horizontal: compact ? 16 : 24,
              vertical: constraints.maxHeight < 600 ? 60 : 84,
            ),
            child: ConstrainedBox(
              constraints: const BoxConstraints(maxWidth: 420),
              child: Container(
                padding: EdgeInsets.fromLTRB(
                  compact ? 18 : 22,
                  compact ? 18 : 22,
                  compact ? 18 : 22,
                  18,
                ),
                decoration: BoxDecoration(
                  color: p.tile,
                  border: Border.all(color: p.tileLine),
                  borderRadius: BorderRadius.circular(18),
                  boxShadow: const [
                    BoxShadow(
                      color: Color(0x66000000),
                      blurRadius: 40,
                      spreadRadius: -12,
                      offset: Offset(0, 18),
                    ),
                  ],
                ),
                child: Column(
                  mainAxisSize: MainAxisSize.min,
                  crossAxisAlignment: CrossAxisAlignment.stretch,
                  children: [
                    Row(
                      children: [
                        AppToneIcon(
                          icon: icon,
                          busy: busy,
                          size: compact ? 34 : 44,
                        ),
                        SizedBox(width: compact ? 10 : 14),
                        Expanded(
                          child: Column(
                            crossAxisAlignment: CrossAxisAlignment.start,
                            children: [
                              KeepAllText(
                                headline,
                                style: uiText(
                                  size: 17,
                                  weight: FontWeight.w600,
                                  color: p.ink,
                                  height: 1.35,
                                ),
                              ),
                              const SizedBox(height: 3),
                              KeepAllText(
                                machine,
                                maxLines: 2,
                                overflow: TextOverflow.ellipsis,
                                style: uiText(size: 13, color: p.muted),
                              ),
                            ],
                          ),
                        ),
                      ],
                    ),
                    const SizedBox(height: 16),
                    KeepAllText(
                      message,
                      style: uiText(
                        size: 13.5,
                        height: 1.6,
                        color: p.ink.withValues(alpha: .86),
                      ),
                    ),
                    if (details.isNotEmpty) ...[
                      const SizedBox(height: 10),
                      for (final line in details)
                        KeepAllText(
                          line,
                          style: uiText(
                            size: 12.5,
                            height: 1.6,
                            color: p.muted,
                          ),
                        ),
                    ],
                    if (status != null) ...[
                      const SizedBox(height: 12),
                      // Its mark turns only while the card itself is busy.
                      TickerMode(enabled: busy, child: status!),
                    ],
                    const SizedBox(height: 20),
                    // As in a dialog: a lone button (연결 취소, 목록으로
                    // 돌아가기) keeps its own width at the right; a bar as
                    // wide as the card made it the strongest thing on it.
                    if (actions.length == 1)
                      Align(
                        alignment: Alignment.centerRight,
                        child: ConstrainedBox(
                          constraints: const BoxConstraints(
                            minWidth: AppDialog.loneActionWidth,
                          ),
                          child: actions.single,
                        ),
                      )
                    else
                      AppDialogActions(children: actions),
                  ],
                ),
              ),
            ),
          ),
        );
      },
    ),
  );
}
