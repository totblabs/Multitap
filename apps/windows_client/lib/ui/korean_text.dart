import 'package:flutter/widgets.dart';

/// Korean line breaking by words, like CSS `word-break: keep-all`.
///
/// Flutter's line breaker treats every Hangul syllable as a break
/// opportunity, so "연결" could split as "연│결" (2026-10-26: "3류" line
/// breaks). A word joiner (U+2060, zero width) between the characters of a
/// word that contains Hangul leaves breaks only at spaces; a word longer than
/// the whole line still breaks.
String keepAll(String text) {
  final out = StringBuffer();
  int? previous;
  for (final rune in text.runes) {
    if (previous != null &&
        !_space(previous) &&
        !_space(rune) &&
        (_hangul(previous) || _hangul(rune))) {
      out.writeCharCode(0x2060);
    }
    out.writeCharCode(rune);
    previous = rune;
  }
  return out.toString();
}

/// One sentence per line. Messages are written as "what happened. what to
/// do." in a single string; wrapped as one paragraph the second sentence
/// breaks mid-phrase ("... 가상 / 화면 상태를 ..."). A sentence end (Korean
/// ending + . ! ?) followed by a space becomes a line break. Text that already
/// has its own line breaks, a number like "1.2.34", and strings without a
/// second sentence are unchanged.
String sentenceLines(String text) => text.replaceAllMapped(
  RegExp(r'([요다죠까네][.!?])[ \t]+(?=[^\s])'),
  (m) => '${m[1]}\n',
);

bool _hangul(int rune) =>
    (rune >= 0xAC00 && rune <= 0xD7A3) ||
    (rune >= 0x1100 && rune <= 0x11FF) ||
    (rune >= 0x3130 && rune <= 0x318F);

bool _space(int rune) =>
    rune == 0x20 || rune == 0x0A || rune == 0x0D || rune == 0x09 || rune == 0x3000;

/// A [Text] that wraps Korean by words ([keepAll]). Only the rendered string
/// changes: [data] stays the original text for semantics, selection and
/// finders.
class KeepAllText extends Text {
  const KeepAllText(
    super.data, {
    super.key,
    super.style,
    super.strutStyle,
    super.textAlign,
    super.textDirection,
    super.locale,
    super.softWrap,
    super.overflow,
    super.textScaler,
    super.maxLines,
    super.semanticsLabel,
    super.textWidthBasis,
    super.textHeightBehavior,
    super.selectionColor,
  });

  /// Text.build with the joined string: a RichText (so this widget remains
  /// the only Text carrying the text) whose semantics read the original.
  @override
  Widget build(BuildContext context) {
    // One line holds one sentence, except where a single line is asked for.
    final text = maxLines == 1 ? data! : sentenceLines(data!);
    final joined = keepAll(text);
    // Nothing to join or break (no Hangul): the ordinary Text.
    if (joined == data) return super.build(context);
    final defaults = DefaultTextStyle.of(context);
    var effective = style;
    if (style == null || style!.inherit) {
      effective = defaults.style.merge(style);
    }
    if (MediaQuery.boldTextOf(context)) {
      effective = effective!.merge(
        const TextStyle(fontWeight: FontWeight.w600),
      );
    }
    final registrar = SelectionContainer.maybeOf(context);
    Widget result = RichText(
      textAlign: textAlign ?? defaults.textAlign ?? TextAlign.start,
      textDirection: textDirection,
      locale: locale,
      softWrap: softWrap ?? defaults.softWrap,
      overflow: overflow ?? effective?.overflow ?? defaults.overflow,
      textScaler: textScaler ?? MediaQuery.textScalerOf(context),
      maxLines: maxLines ?? defaults.maxLines,
      strutStyle: strutStyle,
      textWidthBasis: textWidthBasis ?? defaults.textWidthBasis,
      textHeightBehavior:
          textHeightBehavior ??
          defaults.textHeightBehavior ??
          DefaultTextHeightBehavior.maybeOf(context),
      selectionRegistrar: registrar,
      selectionColor:
          selectionColor ??
          DefaultSelectionStyle.of(context).selectionColor ??
          DefaultSelectionStyle.defaultColor,
      text: TextSpan(style: effective, text: joined),
    );
    if (registrar != null) {
      result = MouseRegion(
        cursor:
            DefaultSelectionStyle.of(context).mouseCursor ??
            SystemMouseCursors.text,
        child: result,
      );
    }
    return Semantics(
      textDirection: textDirection,
      label: semanticsLabel ?? data,
      child: ExcludeSemantics(child: result),
    );
  }
}
