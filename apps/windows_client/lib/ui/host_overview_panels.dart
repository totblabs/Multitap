import 'package:flutter/material.dart';

import '../data/host_overview.dart';
import 'app_palette.dart';
import 'app_theme.dart' show uiText;
import 'host_settings_navigation.dart';
import 'korean_text.dart';

/// The pieces of the Host settings overview (개요): the readiness said once
/// in plain words beside the desktop's name, one banner for one problem and
/// plain rows that open a page (2026-11-11: the three-point route picture
/// and the "정상" on every row said the same thing three times). They only
/// draw what they are given; host_overview.dart decides it.

Color _routeColor(AppPalette p, HostRouteState state) => switch (state) {
  HostRouteState.ok => p.green,
  HostRouteState.warn => p.amber,
  HostRouteState.idle => p.faint,
};

/// The readiness beside the desktop's name, as plain words: "스트리밍 가능",
/// the one problem, or that the status is being read.
class HostReadiness extends StatelessWidget {
  const HostReadiness({super.key, required this.state, required this.label});
  final HostRouteState state;
  final String label;

  @override
  Widget build(BuildContext context) {
    final p = AppPalette.of(context);
    return KeepAllText(
      label,
      style: uiText(
        size: 14,
        weight: FontWeight.w500,
        color: switch (state) {
          HostRouteState.ok => p.green,
          HostRouteState.warn => p.amberInk,
          HostRouteState.idle => p.muted,
        },
        height: 1.3,
      ),
    );
  }
}

/// A page's problem beside its title (a page that is fine shows nothing).
class HostStatusPill extends StatelessWidget {
  const HostStatusPill({super.key, required this.state, required this.label});
  final HostRouteState state;
  final String label;

  @override
  Widget build(BuildContext context) {
    final p = AppPalette.of(context);
    final (fill, ink) = switch (state) {
      HostRouteState.ok => (
        p.green.withValues(alpha: p.dark ? .16 : .12),
        p.green,
      ),
      HostRouteState.warn => (
        p.amber.withValues(alpha: p.dark ? .18 : .20),
        p.amberInk,
      ),
      HostRouteState.idle => (p.quiet, p.muted),
    };
    return Container(
      constraints: const BoxConstraints(minHeight: 26),
      padding: const EdgeInsets.symmetric(horizontal: 10, vertical: 3),
      decoration: BoxDecoration(
        color: fill,
        borderRadius: BorderRadius.circular(999),
      ),
      child: Row(
        mainAxisSize: MainAxisSize.min,
        children: [
          Container(
            width: 7,
            height: 7,
            decoration: BoxDecoration(
              color: _routeColor(p, state),
              shape: BoxShape.circle,
            ),
          ),
          const SizedBox(width: 6),
          Flexible(
            child: KeepAllText(
              label,
              style: uiText(
                size: 13,
                weight: FontWeight.w500,
                color: ink,
                height: 1.3,
              ),
            ),
          ),
        ],
      ),
    );
  }
}

/// One problem: one or two short sentences (what happened, what to do) and
/// at most one amber action.
class HostProblemBanner extends StatelessWidget {
  const HostProblemBanner({
    super.key,
    required this.message,
    this.messageKey,
    this.code,
    this.actionLabel,
    this.actionKey,
    this.onAction,
  });
  final String message;
  final Key? messageKey, actionKey;
  final String? actionLabel;

  /// A short code the sentence speaks of (the connection request's
  /// verification code): shown on its own line, never broken or abridged.
  final String? code;

  /// Null with a label: the action exists but cannot run now (busy).
  final VoidCallback? onAction;

  @override
  Widget build(BuildContext context) {
    final p = AppPalette.of(context);
    final sentence = KeepAllText(
      message,
      key: messageKey,
      style: uiText(size: 13, color: p.ink, height: 1.55),
    );
    final text = code == null
        ? sentence
        : Column(
            crossAxisAlignment: CrossAxisAlignment.start,
            children: [
              sentence,
              const SizedBox(height: 6),
              FittedBox(
                fit: BoxFit.scaleDown,
                alignment: Alignment.centerLeft,
                child: SelectableText(
                  code!,
                  key: const Key('host-overview-code'),
                  maxLines: 1,
                  style: TextStyle(
                    fontFamily: 'Consolas',
                    fontSize: 18,
                    fontWeight: FontWeight.w600,
                    letterSpacing: 1.5,
                    color: p.ink,
                  ),
                ),
              ),
            ],
          );
    final action = actionLabel == null
        ? null
        : FilledButton(
            key: actionKey,
            onPressed: onAction,
            style: FilledButton.styleFrom(
              minimumSize: const Size(0, 40),
              padding: const EdgeInsets.symmetric(horizontal: 14),
              textStyle: uiText(size: 13, weight: FontWeight.w600),
              shape: RoundedRectangleBorder(
                borderRadius: BorderRadius.circular(8),
              ),
            ),
            child: KeepAllText(actionLabel!, maxLines: 1),
          );
    return Container(
      padding: const EdgeInsets.symmetric(horizontal: 16, vertical: 14),
      decoration: BoxDecoration(
        color: p.amber.withValues(alpha: p.dark ? .10 : .14),
        borderRadius: BorderRadius.circular(10),
      ),
      child: LayoutBuilder(
        builder: (context, box) {
          final scale = MediaQuery.textScalerOf(context).scale(1);
          if (action == null) return text;
          // Beside the sentence while both fit; under it in a narrow or
          // large-text window.
          return box.maxWidth / scale < 300
              ? Column(
                  crossAxisAlignment: CrossAxisAlignment.start,
                  children: [text, const SizedBox(height: 12), action],
                )
              : Row(
                  children: [
                    Expanded(child: text),
                    const SizedBox(width: 14),
                    action,
                  ],
                );
        },
      ),
    );
  }
}

/// One line of 구성 요소: the area, one line about it, its state at the right
/// only when it needs something (a fine area says nothing more) and a
/// chevron. The whole line opens the area's page.
class HostAreaRow extends StatelessWidget {
  const HostAreaRow({
    super.key,
    required this.title,
    required this.detail,
    required this.state,
    required this.onTap,
    this.detailKey,
  });
  final String title, detail;
  final HostSetupState state;
  final VoidCallback? onTap;
  final Key? detailKey;

  @override
  Widget build(BuildContext context) {
    final p = AppPalette.of(context);
    return DecoratedBox(
      decoration: BoxDecoration(
        border: Border(top: BorderSide(color: p.line)),
      ),
      child: Semantics(
        button: true,
        child: InkWell(
          onTap: onTap,
          child: ConstrainedBox(
            constraints: const BoxConstraints(minHeight: 56),
            child: Padding(
              padding: const EdgeInsets.symmetric(vertical: 8),
              child: Row(
                children: [
                  Expanded(
                    child: Column(
                      crossAxisAlignment: CrossAxisAlignment.start,
                      mainAxisAlignment: MainAxisAlignment.center,
                      children: [
                        KeepAllText(
                          title,
                          style: uiText(
                            size: 14,
                            weight: FontWeight.w500,
                            color: p.ink,
                            height: 1.3,
                          ),
                        ),
                        const SizedBox(height: 2),
                        KeepAllText(
                          detail,
                          key: detailKey,
                          style: uiText(
                            size: 13,
                            color: p.muted,
                            height: 1.4,
                          ),
                        ),
                      ],
                    ),
                  ),
                  if (state != HostSetupState.complete) ...[
                    const SizedBox(width: 14),
                    HostSetupBadge(state: state),
                  ],
                  const SizedBox(width: 8),
                  Icon(Icons.chevron_right_rounded, size: 18, color: p.faint),
                ],
              ),
            ),
          ),
        ),
      ),
    );
  }
}
