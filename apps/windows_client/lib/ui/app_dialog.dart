import 'package:flutter/material.dart';
import 'package:flutter/rendering.dart';

import 'app_theme.dart';
import 'korean_text.dart';
import '../l10n/l10n.dart';

/// The meaning of a dialog's icon and main button. Amber is the product's
/// ordinary "go ahead"; red is kept for actions that cut off access;
/// [neutral] is information that asks for nothing (a grey icon).
enum AppTone { accent, danger, info, success, warn, neutral }

extension AppToneColors on AppPalette {
  Color tone(AppTone tone) => switch (tone) {
    AppTone.accent => amber,
    AppTone.danger => red,
    AppTone.info => blue,
    AppTone.success => green,
    AppTone.warn => warn,
    AppTone.neutral => muted,
  };

  /// Text on a filled [tone] button, readable in both appearances.
  Color onTone(AppTone tone) => switch (tone) {
    AppTone.accent || AppTone.warn => onAmber,
    _ when dark => const Color(0xFF14181B),
    _ => Colors.white,
  };
}

/// The three button looks every dialog, panel and form shares: a quiet grey
/// cancel, a filled main action in the dialog's tone, and an outlined
/// secondary action. 42 px high, 12 px corners, as in the connect question.
abstract final class AppButtons {
  static const height = 42.0, radius = 12.0;

  // The vertical part lets a label at 150-200 % text breathe; at 100 % the
  // 42 px minimum (34 with Windows' compact density) still decides.
  static const _padding = EdgeInsets.symmetric(horizontal: 16, vertical: 12);

  static final _shape = RoundedRectangleBorder(
    borderRadius: BorderRadius.circular(radius),
  );

  /// The width of the keyboard-focus ring.
  static const focusWidth = 2.0;

  /// A ring just inside the button's edge while the keyboard has put focus
  /// on it ([inset] px further in: a ring on a filled button keeps a rim of
  /// the fill outside it, or it would merge with the surface behind). A
  /// button focused by a click or a touch shows [rest] (its usual edge, or
  /// none), as on the launcher's header.
  static WidgetStateProperty<BorderSide?> focusRing(
    Color color, {
    BorderSide rest = BorderSide.none,
    double inset = 0,
  }) => WidgetStateProperty.resolveWith(
    (states) =>
        states.contains(WidgetState.focused) &&
            !states.contains(WidgetState.disabled) &&
            FocusManager.instance.highlightMode ==
                FocusHighlightMode.traditional
        ? BorderSide(
            color: color,
            width: focusWidth,
            strokeAlign:
                BorderSide.strokeAlignInside - 2 * inset / focusWidth,
          )
        : rest,
  );

  static ButtonStyle quiet(AppPalette p) => TextButton.styleFrom(
    backgroundColor: p.quiet,
    foregroundColor: p.ink,
    disabledBackgroundColor: p.quiet,
    disabledForegroundColor: p.disabled,
    minimumSize: const Size(0, height),
    padding: _padding,
    textStyle: uiText(size: 14, weight: FontWeight.w600),
    shape: _shape,
  ).copyWith(side: focusRing(p.focus));

  // On its own fill the ring takes the label's colour (an amber ring on an
  // amber button would not show) and sits 2 px inside the edge.
  static ButtonStyle filled(AppPalette p, {AppTone tone = AppTone.accent}) =>
      FilledButton.styleFrom(
        backgroundColor: p.tone(tone),
        foregroundColor: p.onTone(tone),
        disabledBackgroundColor: p.quiet,
        disabledForegroundColor: p.disabled,
        minimumSize: const Size(0, height),
        padding: _padding,
        textStyle: uiText(size: 14, weight: FontWeight.w600),
        shape: _shape,
      ).copyWith(side: focusRing(p.onTone(tone), inset: 2));

  /// A filled button that is not the main action yet (amber tint).
  static ButtonStyle tonal(AppPalette p) => FilledButton.styleFrom(
    backgroundColor: p.amberSoft,
    foregroundColor: p.amberInk,
    disabledBackgroundColor: p.quiet,
    disabledForegroundColor: p.disabled,
    minimumSize: const Size(0, height),
    padding: _padding,
    textStyle: uiText(size: 14, weight: FontWeight.w600),
    shape: _shape,
  ).copyWith(side: focusRing(p.focus));

  static ButtonStyle outlined(AppPalette p) => OutlinedButton.styleFrom(
    foregroundColor: p.ink,
    disabledForegroundColor: p.disabled,
    minimumSize: const Size(0, height),
    padding: _padding,
    textStyle: uiText(size: 14, weight: FontWeight.w600),
    shape: _shape,
  ).copyWith(side: focusRing(p.focus, rest: BorderSide(color: p.tileLine)));

  /// A text button that is a link or opens more detail ("자세히"), never the
  /// surface's main action: secondary ink, not amber.
  static ButtonStyle link(AppPalette p) => TextButton.styleFrom(
    foregroundColor: p.ink2,
    disabledForegroundColor: p.disabled,
    textStyle: uiText(size: 13.5, weight: FontWeight.w600),
    shape: RoundedRectangleBorder(borderRadius: BorderRadius.circular(10)),
  ).copyWith(side: focusRing(p.focus));
}

/// A round tinted icon, as on the connect question. [busy] swaps the icon
/// for a spinner in the same circle.
class AppToneIcon extends StatelessWidget {
  const AppToneIcon({
    super.key,
    required this.icon,
    this.tone = AppTone.accent,
    this.busy = false,
    this.size = 44,
  });
  final IconData icon;
  final AppTone tone;
  final bool busy;
  final double size;

  @override
  Widget build(BuildContext context) {
    final p = AppPalette.of(context);
    final neutral = tone == AppTone.neutral;
    final color = p.tone(tone);
    return Container(
      width: size,
      height: size,
      alignment: Alignment.center,
      decoration: BoxDecoration(
        // Information: a grey circle. A tone: its own tint.
        color: neutral ? p.quiet : color.withValues(alpha: .16),
        shape: BoxShape.circle,
      ),
      child: busy
          ? SizedBox.square(
              dimension: size * .48,
              child: CircularProgressIndicator(
                strokeWidth: 2.4,
                // Progress is the brand's amber, also on a grey circle.
                color: neutral ? p.amber : color,
              ),
            )
          : Icon(icon, color: color, size: size * .55),
    );
  }
}

/// A status line inside a dialog or panel: one small icon in the [tone]'s
/// colour, the sentence in the ordinary text colour. A whole sentence in red
/// or yellow reads as log output; the colour belongs to the icon.
///
/// [dot] is the state of one row in a list (a device, a folder): a small
/// dot in the state's colour instead of the icon, and the sentence in the
/// muted colour unless it asks for attention (a failure, a warning).
class AppStatusLine extends StatelessWidget {
  const AppStatusLine(
    this.text, {
    super.key,
    this.textKey,
    this.tone = AppTone.danger,
    this.size = 13,
    this.dot = false,
    this.maxLines,
  });
  final String text;

  /// The key of the sentence itself (what tests and semantics find).
  final Key? textKey;
  final AppTone tone;
  final double size;
  final bool dot;

  /// More than this many lines end in an ellipsis.
  final int? maxLines;

  @override
  Widget build(BuildContext context) {
    final p = AppPalette.of(context);
    final scale = MediaQuery.textScalerOf(context).scale(1);
    return Row(
      crossAxisAlignment: CrossAxisAlignment.start,
      children: [
        if (dot)
          Padding(
            // Centred on the first line of the sentence.
            padding: EdgeInsets.only(top: (size * 1.6 * scale - 6) / 2),
            child: Container(
              width: 6,
              height: 6,
              decoration: BoxDecoration(
                color: p.tone(tone),
                shape: BoxShape.circle,
              ),
            ),
          )
        else
          Padding(
            // Centred on the first line of the sentence.
            padding: EdgeInsets.only(top: (size * 1.6 - 16) * scale / 2),
            child: Icon(
              switch (tone) {
                AppTone.danger => Icons.error_outline_rounded,
                AppTone.warn => Icons.warning_amber_rounded,
                AppTone.success => Icons.check_circle_outline_rounded,
                _ => Icons.info_outline_rounded,
              },
              size: 16 * scale,
              color: p.tone(tone),
            ),
          ),
        const SizedBox(width: 8),
        Expanded(
          child: KeepAllText(
            text,
            key: textKey,
            maxLines: maxLines,
            overflow: maxLines == null ? null : TextOverflow.ellipsis,
            style: uiText(
              size: size,
              height: 1.6,
              color:
                  dot && tone != AppTone.danger && tone != AppTone.warn
                  ? p.muted
                  : p.ink,
            ),
          ),
        ),
      ],
    );
  }
}

/// The dialog button bar: equal-width buttons in one row, main action last
/// (right). When the widest label would not fit on one line at that width,
/// the buttons stack full width with the main action on top. The choice is
/// measured from the buttons themselves (measured from the window, a 400 px
/// dialog in a wide window puts "추가하고 다시 시작" on two lines), so it holds for any
/// dialog width, window width and text size, and it reports its intrinsic
/// height (the sign-in card measures it).
class AppDialogActions extends MultiChildRenderObjectWidget {
  const AppDialogActions({super.key, required super.children});

  static const gap = 10.0, stackGap = 8.0;

  @override
  RenderObject createRenderObject(BuildContext context) => _RenderButtonBar();
}

class _ButtonBarData extends ContainerBoxParentData<RenderBox> {}

class _RenderButtonBar extends RenderBox
    with
        ContainerRenderObjectMixin<RenderBox, _ButtonBarData>,
        RenderBoxContainerDefaultsMixin<RenderBox, _ButtonBarData> {
  @override
  void setupParentData(RenderBox child) {
    if (child.parentData is! _ButtonBarData) {
      child.parentData = _ButtonBarData();
    }
  }

  List<RenderBox> get _buttons {
    final list = <RenderBox>[];
    var child = firstChild;
    while (child != null) {
      list.add(child);
      child = childAfter(child);
    }
    return list;
  }

  /// The width each button gets in one row, or null when they stack.
  double? _rowCell(double width) {
    final buttons = _buttons;
    if (buttons.isEmpty) return null;
    final gaps = AppDialogActions.gap * (buttons.length - 1);
    if (!width.isFinite) {
      return buttons
          .map((b) => b.getMaxIntrinsicWidth(double.infinity))
          .reduce((a, b) => a > b ? a : b);
    }
    final cell = (width - gaps) / buttons.length;
    if (buttons.length == 1) return cell;
    for (final button in buttons) {
      // A label that would need a second line at this width: stack.
      if (button.getMaxIntrinsicWidth(double.infinity) > cell + .5) {
        return null;
      }
    }
    return cell;
  }

  Size _layout(BoxConstraints constraints, {required bool dry}) {
    final buttons = _buttons;
    if (buttons.isEmpty) return constraints.smallest;
    final cell = _rowCell(constraints.maxWidth);
    Size place(RenderBox child, BoxConstraints c) => dry
        ? child.getDryLayout(c)
        : (child..layout(c, parentUsesSize: true)).size;
    if (cell != null) {
      var height = 0.0;
      final sizes = [
        for (final button in buttons)
          place(button, BoxConstraints.tightFor(width: cell)),
      ];
      for (final size in sizes) {
        if (size.height > height) height = size.height;
      }
      if (!dry) {
        var x = 0.0;
        for (final (index, button) in buttons.indexed) {
          (button.parentData! as _ButtonBarData).offset = Offset(
            x,
            (height - sizes[index].height) / 2,
          );
          x += cell + AppDialogActions.gap;
        }
      }
      final width =
          cell * buttons.length + AppDialogActions.gap * (buttons.length - 1);
      return constraints.constrain(Size(width, height));
    }
    // Stacked, main action (the last child) on top.
    final width = constraints.maxWidth;
    var y = 0.0;
    for (final button in buttons.reversed) {
      final size = place(button, BoxConstraints.tightFor(width: width));
      if (!dry) {
        (button.parentData! as _ButtonBarData).offset = Offset(0, y);
      }
      y += size.height + AppDialogActions.stackGap;
    }
    return constraints.constrain(Size(width, y - AppDialogActions.stackGap));
  }

  @override
  void performLayout() => size = _layout(constraints, dry: false);

  @override
  Size computeDryLayout(BoxConstraints constraints) =>
      _layout(constraints, dry: true);

  double _height(double width, double Function(RenderBox, double) measure) {
    final buttons = _buttons;
    if (buttons.isEmpty) return 0;
    final cell = _rowCell(width);
    if (cell != null) {
      return buttons
          .map((b) => measure(b, cell))
          .reduce((a, b) => a > b ? a : b);
    }
    return buttons.map((b) => measure(b, width)).reduce((a, b) => a + b) +
        AppDialogActions.stackGap * (buttons.length - 1);
  }

  @override
  double computeMinIntrinsicHeight(double width) =>
      _height(width, (b, w) => b.getMinIntrinsicHeight(w));

  @override
  double computeMaxIntrinsicHeight(double width) =>
      _height(width, (b, w) => b.getMaxIntrinsicHeight(w));

  @override
  double computeMinIntrinsicWidth(double height) => _buttons.fold(
    0.0,
    (widest, b) => b.getMinIntrinsicWidth(height) > widest
        ? b.getMinIntrinsicWidth(height)
        : widest,
  );

  @override
  double computeMaxIntrinsicWidth(double height) {
    final buttons = _buttons;
    if (buttons.isEmpty) return 0;
    final widest = buttons
        .map((b) => b.getMaxIntrinsicWidth(height))
        .reduce((a, b) => a > b ? a : b);
    return widest * buttons.length +
        AppDialogActions.gap * (buttons.length - 1);
  }

  @override
  bool hitTestChildren(BoxHitTestResult result, {required Offset position}) =>
      defaultHitTestChildren(result, position: position);

  @override
  void paint(PaintingContext context, Offset offset) =>
      defaultPaint(context, offset);
}

/// The product dialog, in the connect question's design: the tile
/// surface, 18 px corners, a hairline edge, a round tinted icon beside a short
/// title and subtitle, the body, and equal-width buttons. The body scrolls
/// between a fixed header and fixed buttons, so actions stay reachable in
/// short windows and with large text.
class AppDialog extends StatelessWidget {
  const AppDialog({
    super.key,
    required this.title,
    this.titleKey,
    this.subtitle,
    this.subtitleKey,
    this.icon,
    this.leading,
    this.tone,
    this.busy = false,
    this.onClose,
    this.closeKey,
    this.body = const [],
    this.scrollBody = true,
    this.scrollAll = false,
    this.footer,
    this.actions = const [],
    this.maxWidth = 400,
  });

  final String title;
  final Key? titleKey, subtitleKey, closeKey;
  final String? subtitle;
  final IconData? icon;

  /// Replaces the round icon (the product mark on the client info).
  final Widget? leading;

  /// The icon's meaning. Left out, the icon is amber only beside an amber
  /// main action in [actions] (one accent per surface, and the two read as a
  /// pair); a dialog that only informs, with a grey 닫기 or none, gets the
  /// grey [AppTone.neutral] circle. A dialog whose main action sits inside
  /// [body] names its tone.
  final AppTone? tone;
  final bool busy;

  /// Shows a small close button in the header.
  final VoidCallback? onClose;
  final List<Widget> body;

  /// False when the body brings its own scrolling (a list or a scrollbar).
  final bool scrollBody;

  /// Scroll the header and actions too when a keyboard leaves less room than
  /// their minimum height. Keep this choice stable while editing a form.
  final bool scrollAll;

  /// What the buttons do, said right above them: it stays in view while
  /// [body] scrolls (a notice under a long list had to be scrolled to).
  final Widget? footer;
  final List<Widget> actions;
  final double maxWidth;

  /// The least width of a dialog's only button.
  static const loneActionWidth = 96.0;

  /// Authored paragraphs in the shared secondary style. Use an explicit
  /// newline between an action's result, preserved work and the next step;
  /// keep ordinary word wrapping within each sentence. Never split arbitrary
  /// punctuation here (versions, paths and user names are also displayed).
  static Widget message(BuildContext context, String text, {Key? key}) =>
      KeepAllText(
        text,
        key: key,
        style: uiText(
          size: 13.5,
          height: 1.6,
          color: AppPalette.of(context).muted,
        ),
      );

  /// Whether [actions] holds an amber main action (see [tone]).
  static bool _hasAccentAction(List<Widget> actions, AppPalette p) {
    for (var action in actions) {
      if (action is Tooltip && action.child != null) action = action.child!;
      if (action is FilledButton) {
        final fill = action.style?.backgroundColor?.resolve(const {});
        if (fill == null || fill == p.amber) return true;
      }
    }
    return false;
  }

  @override
  Widget build(BuildContext context) {
    final p = AppPalette.of(context);
    final tone =
        this.tone ??
        (_hasAccentAction(actions, p) ? AppTone.accent : AppTone.neutral);
    final touchClose = switch (Theme.of(context).platform) {
      TargetPlatform.android || TargetPlatform.iOS => true,
      _ => false,
    };
    final closeSize = touchClose ? 48.0 : 34.0;
    final scale = MediaQuery.textScalerOf(context).scale(1);
    final narrow = MediaQuery.sizeOf(context).width / scale < 380;
    // Larger Windows text widens the dialog too (up to 1.5x), so its lines
    // and buttons keep their shape instead of wrapping into a narrow column.
    final width = maxWidth * scale.clamp(1.0, 1.5);
    final side = narrow ? 18.0 : 22.0;
    // Narrow windows keep a smaller icon (a busy spinner still shows).
    final leading = narrow
        ? (icon == null
              ? null
              : AppToneIcon(icon: icon!, tone: tone, busy: busy, size: 34))
        : this.leading ??
              (icon == null
                  ? null
                  : AppToneIcon(icon: icon!, tone: tone, busy: busy));
    final content = Column(
      mainAxisSize: MainAxisSize.min,
      crossAxisAlignment: CrossAxisAlignment.stretch,
      children: body,
    );
    Widget scroll(Widget child) =>
        scrollAll ? SingleChildScrollView(child: child) : child;
    final dialog = Dialog(
      backgroundColor: p.tile,
      surfaceTintColor: Colors.transparent,
      insetPadding: EdgeInsets.symmetric(
        horizontal: narrow ? 16 : 24,
        vertical: 24,
      ),
      clipBehavior: Clip.antiAlias,
      shape: RoundedRectangleBorder(
        borderRadius: BorderRadius.circular(18),
        side: BorderSide(color: p.tileLine),
      ),
      child: ConstrainedBox(
        constraints: BoxConstraints(maxWidth: width),
        child: scroll(
          Padding(
            padding: EdgeInsets.fromLTRB(side, side, side, 18),
            child: Column(
              mainAxisSize: MainAxisSize.min,
              crossAxisAlignment: CrossAxisAlignment.stretch,
              children: [
                Row(
                  children: [
                    if (leading != null) ...[
                      leading,
                      SizedBox(width: narrow ? 10 : 14),
                    ],
                    Expanded(
                      child: Column(
                        crossAxisAlignment: CrossAxisAlignment.start,
                        mainAxisSize: MainAxisSize.min,
                        children: [
                          KeepAllText(
                            title,
                            key: titleKey,
                            maxLines: 2,
                            overflow: TextOverflow.ellipsis,
                            style: uiText(
                              size: 17,
                              weight: FontWeight.w600,
                              color: p.ink,
                              height: 1.35,
                            ).copyWith(letterSpacing: -.2),
                          ),
                          if (subtitle != null) ...[
                            const SizedBox(height: 3),
                            KeepAllText(
                              subtitle!,
                              key: subtitleKey,
                              maxLines: 2,
                              overflow: TextOverflow.ellipsis,
                              style: uiText(size: 13, color: p.muted),
                            ),
                          ],
                        ],
                      ),
                    ),
                    if (onClose != null) ...[
                      const SizedBox(width: 8),
                      IconButton(
                        key: closeKey,
                        tooltip: l10n.close,
                        onPressed: onClose,
                        style: IconButton.styleFrom(
                          foregroundColor: p.muted,
                          minimumSize: Size.square(closeSize),
                          maximumSize: Size.square(closeSize),
                          visualDensity: touchClose
                              ? VisualDensity.standard
                              : null,
                          padding: EdgeInsets.zero,
                          shape: RoundedRectangleBorder(
                            borderRadius: BorderRadius.circular(10),
                          ),
                        ).copyWith(side: AppButtons.focusRing(p.focus)),
                        icon: const Icon(Icons.close_rounded, size: 20),
                      ),
                    ],
                  ],
                ),
                if (body.isNotEmpty) ...[
                  const SizedBox(height: 16),
                  if (scrollAll)
                    content
                  else
                    Flexible(
                      child: scrollBody
                          ? SingleChildScrollView(child: content)
                          : content,
                    ),
                ],
                if (footer != null) ...[
                  SizedBox(height: body.isEmpty ? 16 : 12),
                  footer!,
                ],
                if (actions.length == 1) ...[
                  const SizedBox(height: 20),
                  // A lone button (확인, 닫기, 취소) keeps its own width at
                  // the right: a bar as wide as the dialog made the one
                  // button that does nothing the strongest thing in it.
                  Align(
                    alignment: Alignment.centerRight,
                    child: ConstrainedBox(
                      constraints: const BoxConstraints(
                        minWidth: loneActionWidth,
                      ),
                      child: actions.single,
                    ),
                  ),
                ] else if (actions.isNotEmpty) ...[
                  const SizedBox(height: 20),
                  AppDialogActions(children: actions),
                ],
              ],
            ),
          ),
        ),
      ),
    );
    // A text button or a fold with no style of its own is a link here, never
    // a second accent beside the main action.
    final theme = Theme.of(context);
    return Theme(
      data: theme.copyWith(
        textButtonTheme: TextButtonThemeData(style: AppButtons.link(p)),
        expansionTileTheme: theme.expansionTileTheme.copyWith(
          iconColor: p.muted,
          collapsedIconColor: p.muted,
          textColor: p.ink,
          collapsedTextColor: p.ink,
        ),
      ),
      child: dialog,
    );
  }
}

/// A yes/no question in the [AppDialog] look. The main button takes focus so
/// Enter confirms, except for [AppTone.danger] where Enter stays on cancel.
class AppConfirmDialog extends StatelessWidget {
  const AppConfirmDialog({
    super.key,
    required this.title,
    required this.confirmLabel,
    this.subtitle,
    this.message,
    this.icon = Icons.help_outline_rounded,
    this.tone = AppTone.accent,
    String? cancelLabel,
    this.confirmKey,
    this.cancelKey,
    this.titleKey,
  }) : _cancelLabel = cancelLabel; // ignore: prefer_initializing_formals
  final String title, confirmLabel;
  final String? _cancelLabel;
  String get cancelLabel => _cancelLabel ?? l10n.cancel;
  final String? subtitle, message;
  final IconData icon;
  final AppTone tone;
  final Key? confirmKey, cancelKey, titleKey;

  @override
  Widget build(BuildContext context) {
    final p = AppPalette.of(context);
    final danger = tone == AppTone.danger;
    return AppDialog(
      icon: icon,
      tone: tone,
      title: title,
      titleKey: titleKey,
      subtitle: subtitle,
      body: [if (message != null) AppDialog.message(context, message!)],
      actions: [
        TextButton(
          key: cancelKey,
          autofocus: danger,
          onPressed: () => Navigator.pop(context, false),
          style: AppButtons.quiet(p),
          child: KeepAllText(cancelLabel),
        ),
        FilledButton(
          key: confirmKey,
          autofocus: !danger,
          onPressed: () => Navigator.pop(context, true),
          style: AppButtons.filled(p, tone: tone),
          child: KeepAllText(confirmLabel),
        ),
      ],
    );
  }
}

/// Asks [title] and resolves true only for the main button.
Future<bool> showAppConfirm(
  BuildContext context, {
  required String title,
  required String confirmLabel,
  String? subtitle,
  String? message,
  IconData icon = Icons.help_outline_rounded,
  AppTone tone = AppTone.accent,
  String? cancelLabel,
  Key? dialogKey,
  Key? confirmKey,
  Key? cancelKey,
  bool barrierDismissible = true,
  // A popup panel's own question (AnchoredPanelRoute.ownedDialogSettings)
  // keeps that panel open behind it.
  RouteSettings? routeSettings,
}) async {
  final confirmed = await showDialog<bool>(
    context: context,
    barrierDismissible: barrierDismissible,
    routeSettings: routeSettings,
    builder: (_) => AppConfirmDialog(
      key: dialogKey,
      title: title,
      subtitle: subtitle,
      message: message,
      icon: icon,
      tone: tone,
      confirmLabel: confirmLabel,
      cancelLabel: cancelLabel,
      confirmKey: confirmKey,
      cancelKey: cancelKey,
    ),
  );
  return confirmed == true;
}

/// A result the user acknowledges with one button: true when that button
/// closed it (Esc or a click beside it: false).
Future<bool> showAppNotice(
  BuildContext context, {
  required String title,
  String? message,
  IconData icon = Icons.check_rounded,
  AppTone tone = AppTone.success,
  String? closeLabel,
  Key? dialogKey,
  Key? closeKey,
}) => showDialog<bool>(
  context: context,
  builder: (context) {
    final p = AppPalette.of(context);
    return AppDialog(
      key: dialogKey,
      icon: icon,
      tone: tone,
      title: title,
      body: [if (message != null) AppDialog.message(context, message)],
      // Acknowledging does nothing: the grey button. Amber is for a button
      // that carries something out.
      actions: [
        TextButton(
          key: closeKey,
          autofocus: true,
          onPressed: () => Navigator.pop(context, true),
          style: AppButtons.quiet(p),
          child: KeepAllText(closeLabel ?? l10n.ok),
        ),
      ],
    );
  },
).then((closed) => closed == true);

/// The one question before a remote window or session closes (from the
/// toolbar, the window X or a loading window).
///
/// [usbShared]: devices of this PC the VM has now; closing hands them back.
/// [foldersShared]: folders of this PC mapped as VM drives (a VM program
/// saving there loses its work when the drive goes).
/// [printsWaiting]: documents the VM printed that were not printed or saved
/// here yet; closing discards them.
Future<bool> confirmDisconnect(
  BuildContext context, {
  required String machine,
  int usbShared = 0,
  int foldersShared = 0,
  int printsWaiting = 0,
}) => showAppConfirm(
  context,
  icon: Icons.link_off_rounded,
  title: l10n.disconnect2,
  subtitle: l10n.streaming3(machine),
  message: [
    l10n.desktopsRunningWorkStayAs,
    if (usbShared > 0)
      l10n.ifOneUseUnsavedWork(usbShared),
    if (foldersShared > 0)
      l10n.programsDesktopSavingThemMay(foldersShared),
    if (printsWaiting > 0)
      l10n.text14(printsWaiting),
  ].join('\n'),
  confirmLabel: l10n.disconnect,
);
