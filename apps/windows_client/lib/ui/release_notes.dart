import 'package:flutter/material.dart';

import 'app_dialog.dart';
import 'app_theme.dart';
import 'korean_text.dart';

// The history panel (sidebar, 전체 보기) lives in its own file; callers keep
// importing showReleaseHistory from here.
export 'release_history_panel.dart'
    show showReleaseHistory, ReleaseHistoryPanel, ReleaseHistoryView;

/// Release notes as readable rows: `- `, `• `, `* ` or `1. ` lines become
/// bullet rows, `#` lines and lines ending in a colon become small headings,
/// other lines stay paragraphs, and a blank line opens a new group. The text
/// is the catalog's validated plain text; nothing is interpreted as markup
/// beyond these line prefixes.
class ChangelogText extends StatelessWidget {
  const ChangelogText(this.text, {super.key, this.keepWords = false});
  final String text;

  /// Android opts into word-preserving Korean wrapping; existing callers keep
  /// their original text rendering and note formatting.
  final bool keepWords;

  static final _bullet = RegExp(r'^(\s*)(?:[-•*·]|\d{1,2}[.)])\s+(.*)$');
  static final _heading = RegExp(r'^#{1,6}\s+(.*)$');

  @override
  Widget build(BuildContext context) {
    final p = AppPalette.of(context);
    final body = uiText(size: 13.5, height: 1.55, color: p.ink);
    Widget noteLine(String value, TextStyle style) => keepWords
        ? KeepAllText(value, style: style)
        : Text(value, style: style);
    final rows = <Widget>[];
    var gap = false;
    for (final raw in text.replaceAll('\r\n', '\n').split('\n')) {
      final line = raw.trimRight();
      if (line.trim().isEmpty) {
        gap = rows.isNotEmpty;
        continue;
      }
      final spacing = rows.isEmpty
          ? 0.0
          : gap
          ? 12.0
          : 5.0;
      gap = false;
      if (spacing > 0) rows.add(SizedBox(height: spacing));
      final bullet = _bullet.firstMatch(line);
      final heading = _heading.firstMatch(line.trim());
      if (bullet != null) {
        final nested = bullet[1]!.length >= 2;
        rows.add(
          Padding(
            padding: EdgeInsets.only(left: nested ? 18 : 0),
            child: Row(
              crossAxisAlignment: CrossAxisAlignment.start,
              children: [
                Padding(
                  // Centres the dot on the first line of the item.
                  padding: EdgeInsets.only(
                    top:
                        MediaQuery.textScalerOf(context).scale(13.5) * .78 -
                        2.5,
                  ),
                  child: Container(
                    width: 5,
                    height: 5,
                    decoration: BoxDecoration(
                      color: nested ? p.faint : p.amber,
                      shape: BoxShape.circle,
                    ),
                  ),
                ),
                const SizedBox(width: 10),
                Expanded(child: noteLine(bullet[2]!, body)),
              ],
            ),
          ),
        );
      } else if (heading != null || line.trim().endsWith(':')) {
        rows.add(
          noteLine(
            heading?[1] ?? line.trim(),
            uiText(size: 13, weight: FontWeight.w600, color: p.ink),
          ),
        );
      } else {
        rows.add(
          noteLine(
            line.trim(),
            body.copyWith(color: p.ink.withValues(alpha: .9)),
          ),
        );
      }
    }
    return Column(
      crossAxisAlignment: CrossAxisAlignment.stretch,
      mainAxisSize: MainAxisSize.min,
      children: rows,
    );
  }
}

/// A small rounded label such as "현재" or "새 버전 1.2.3".
class VersionBadge extends StatelessWidget {
  const VersionBadge(this.label, {super.key, this.tone});

  /// Null draws the neutral grey badge.
  final AppTone? tone;
  final String label;

  @override
  Widget build(BuildContext context) {
    final p = AppPalette.of(context);
    final color = tone == null ? null : p.tone(tone!);
    return Container(
      padding: const EdgeInsets.symmetric(horizontal: 8, vertical: 3),
      decoration: BoxDecoration(
        color: color?.withValues(alpha: .14) ?? p.quiet,
        borderRadius: BorderRadius.circular(7),
        border: Border.all(color: color?.withValues(alpha: .38) ?? p.tileLine),
      ),
      child: Text(
        label,
        style: uiText(
          size: 12,
          weight: FontWeight.w600,
          height: 1.3,
          color: tone == AppTone.accent
              ? p.amberInk
              : tone == AppTone.success
              ? p.green
              : p.muted,
        ),
      ),
    );
  }
}

String releaseDate(DateTime value) {
  final local = value.toLocal();
  String two(int n) => n.toString().padLeft(2, '0');
  return '${local.year}-${two(local.month)}-${two(local.day)}';
}

String productName(String product) =>
    product == 'host' ? 'Multitap Host' : 'Multitap Client';
