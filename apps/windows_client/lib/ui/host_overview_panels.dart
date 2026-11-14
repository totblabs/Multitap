import 'package:flutter/material.dart';

import '../data/host_overview.dart';
import 'app_palette.dart';
import 'app_theme.dart' show uiText;
import 'host_settings_navigation.dart';
import 'korean_text.dart';

/// The pieces of the Host settings overview (개요): one status pill beside the
/// desktop's name, the route of three points (이 데스크톱 → 서버 → 클라이언트;
/// removed on 2026-11-11, back on 2026-11-14: the owner likes it), one banner
/// for one problem and plain rows that open a page (a fine row says nothing
/// more). They only draw what they are given; host_overview.dart decides it.

Color _routeColor(AppPalette p, HostRouteState state) => switch (state) {
  HostRouteState.ok => p.green,
  HostRouteState.warn => p.amber,
  HostRouteState.idle => p.faint,
};

/// The status beside the desktop's name on the overview, and a page's problem beside its title (a page that is fine shows nothing).
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

/// One point of the route: its ring, its name and one line of detail.
class HostRoutePoint {
  const HostRoutePoint({
    required this.name,
    required this.detail,
    required this.state,
  });
  final String name, detail;
  final HostRouteState state;
}

/// 이 데스크톱 → 서버 → 클라이언트: three rings joined by two legs. A leg is
/// solid green when it carries a stream, dashed amber where it is broken and
/// a quiet hairline when there is nothing to say about it.
class HostRouteDiagram extends StatelessWidget {
  const HostRouteDiagram({
    super.key,
    required this.points,
    required this.toServer,
    required this.toClient,
    required this.label,
  }) : assert(points.length == 3);
  final List<HostRoutePoint> points;
  final HostRouteState toServer, toClient;

  /// What the picture says, for a screen reader.
  final String label;

  static const _ring = 33.0;

  @override
  Widget build(BuildContext context) {
    final p = AppPalette.of(context);
    return Semantics(
      container: true,
      image: true,
      label: label,
      child: ExcludeSemantics(
        child: CustomPaint(
          painter: _LegPainter(
            toServer: toServer,
            toClient: toClient,
            ok: Color.alphaBlend(
              p.green.withValues(alpha: p.dark ? .55 : .70),
              p.canvas,
            ),
            warn: Color.alphaBlend(
              p.amber.withValues(alpha: p.dark ? .80 : .95),
              p.canvas,
            ),
            idle: p.line,
          ),
          child: Row(
            crossAxisAlignment: CrossAxisAlignment.start,
            children: [
              for (final (index, point) in points.indexed)
                Expanded(
                  child: Padding(
                    padding: const EdgeInsets.symmetric(horizontal: 4),
                    child: Column(
                      children: [
                        Container(
                          key: Key('host-route-ring-$index'),
                          width: _ring,
                          height: _ring,
                          alignment: Alignment.center,
                          decoration: BoxDecoration(
                            color: p.canvas,
                            shape: BoxShape.circle,
                            border: Border.all(
                              color: _routeColor(p, point.state),
                              width: 3,
                            ),
                          ),
                          child: Container(
                            width: 9,
                            height: 9,
                            decoration: BoxDecoration(
                              color: _routeColor(p, point.state),
                              shape: BoxShape.circle,
                            ),
                          ),
                        ),
                        const SizedBox(height: 8),
                        KeepAllText(
                          point.name,
                          key: Key('host-route-name-$index'),
                          textAlign: TextAlign.center,
                          maxLines: 2,
                          overflow: TextOverflow.ellipsis,
                          style: uiText(
                            size: 14,
                            weight: FontWeight.w600,
                            color: p.ink,
                            height: 1.3,
                          ),
                        ),
                        if (point.detail.isNotEmpty) ...[
                          const SizedBox(height: 6),
                          KeepAllText(
                            point.detail,
                            key: Key('host-route-detail-$index'),
                            textAlign: TextAlign.center,
                            maxLines: 3,
                            overflow: TextOverflow.ellipsis,
                            style: uiText(
                              size: 12,
                              color: point.state == HostRouteState.warn
                                  ? p.amberInk
                                  : p.muted,
                              height: 1.4,
                            ),
                          ),
                        ],
                      ],
                    ),
                  ),
                ),
            ],
          ),
        ),
      ),
    );
  }
}

class _LegPainter extends CustomPainter {
  const _LegPainter({
    required this.toServer,
    required this.toClient,
    required this.ok,
    required this.warn,
    required this.idle,
  });
  final HostRouteState toServer, toClient;
  final Color ok, warn, idle;

  @override
  void paint(Canvas canvas, Size size) {
    const y = HostRouteDiagram._ring / 2;
    final third = size.width / 3;
    void leg(double from, double to, HostRouteState state) {
      final paint = Paint()
        ..strokeWidth = 3
        ..color = switch (state) {
          HostRouteState.ok => ok,
          HostRouteState.warn => warn,
          HostRouteState.idle => idle,
        };
      if (state != HostRouteState.warn) {
        canvas.drawLine(Offset(from, y), Offset(to, y), paint);
        return;
      }
      // The broken leg: dashes of 8 with gaps of 6, clear of both rings.
      for (var x = from + 5; x < to - 5; x += 14) {
        canvas.drawLine(
          Offset(x, y),
          Offset((x + 8).clamp(from, to - 5), y),
          paint,
        );
      }
    }

    // From ring to ring: each end tucks under the ring's own edge.
    const inset = HostRouteDiagram._ring / 2 - 1;
    leg(third / 2 + inset, third * 1.5 - inset, toServer);
    leg(third * 1.5 + inset, third * 2.5 - inset, toClient);
  }

  @override
  bool shouldRepaint(_LegPainter old) =>
      old.toServer != toServer ||
      old.toClient != toClient ||
      old.ok != ok ||
      old.warn != warn ||
      old.idle != idle;
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
