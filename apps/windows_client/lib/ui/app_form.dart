import 'package:flutter/material.dart';

import 'app_dialog.dart';
import 'app_theme.dart';
import 'korean_text.dart';

/// The form parts every dialog and panel shares:
/// one field, one select and one fold, so the label position, the arrow and
/// the "more" mark are the same on every screen.

bool _touch(BuildContext context) => switch (Theme.of(context).platform) {
  TargetPlatform.android || TargetPlatform.iOS => true,
  _ => false,
};

/// A labelled form row: the label above the control (12/600, muted), the
/// control, then one line of help or an error below it (12).
///
/// The control is the caller's own [TextField] with [AppField.decoration]
/// (or any other control), so its keys, focus and input rules stay where
/// they were. The label is read out as the control's name; the visible label
/// is not read a second time.
class AppField extends StatelessWidget {
  const AppField({
    super.key,
    required this.label,
    required this.child,
    this.help,
    this.error,
    this.errorKey,
  });

  final String label;
  final Widget child;
  final String? help, error;
  final Key? errorKey;

  /// The height of a field: 42 px, 48 px where a finger presses it.
  static double height(BuildContext context) => _touch(context) ? 48 : 42;

  /// The label style, for a caption that sits above something else.
  static TextStyle labelStyle(AppPalette p) =>
      uiText(size: 12, weight: FontWeight.w600, color: p.muted, height: 1.5);

  /// The field itself: the well, a hairline, 12 px corners (the theme) at
  /// the product's height, with no label inside it.
  static InputDecoration decoration(
    BuildContext context, {
    String? hint,
    Widget? prefixIcon,
    Widget? suffixIcon,
  }) {
    final touch = _touch(context);
    final p = AppPalette.of(context);
    // An icon in the field is part of the field: no taller than it.
    final icon = BoxConstraints(minWidth: 42, minHeight: touch ? 48 : 42);
    return InputDecoration(
      hintText: hint,
      prefixIcon: prefixIcon,
      prefixIconColor: p.muted,
      prefixIconConstraints: icon,
      suffixIcon: suffixIcon,
      suffixIconColor: p.muted,
      suffixIconConstraints: icon,
      counterText: '',
      isDense: true,
      contentPadding: EdgeInsets.symmetric(
        horizontal: 14,
        // With a 14 px line (22 px at the theme's 1.6) this is 42 and 48.
        vertical: touch ? 13 : 10,
      ),
      constraints: BoxConstraints(minHeight: touch ? 48 : 42),
    );
  }

  @override
  Widget build(BuildContext context) {
    final p = AppPalette.of(context);
    return Column(
      mainAxisSize: MainAxisSize.min,
      crossAxisAlignment: CrossAxisAlignment.stretch,
      children: [
        ExcludeSemantics(child: KeepAllText(label, style: labelStyle(p))),
        const SizedBox(height: 6),
        Semantics(label: label, child: child),
        if (error != null) ...[
          const SizedBox(height: 6),
          AppStatusLine(error!, textKey: errorKey, size: 12),
        ] else if (help != null) ...[
          const SizedBox(height: 6),
          KeepAllText(
            help!,
            style: uiText(size: 12, height: 1.5, color: p.muted),
          ),
        ],
      ],
    );
  }
}

/// A choice from a list in the look of [AppField]: the label above, a 42 px
/// well with a chevron (never Material's filled triangle or a label that
/// cuts the border).
class AppSelect<T> extends StatelessWidget {
  const AppSelect({
    super.key,
    this.fieldKey,
    required this.label,
    required this.value,
    required this.items,
    required this.onChanged,
    this.help,
  });

  /// The key of the control itself (what a test taps). A caller that changes
  /// [value] from outside gives a key that changes with it.
  final Key? fieldKey;
  final String label;
  final T? value;
  final List<DropdownMenuItem<T>> items;
  final ValueChanged<T?>? onChanged;
  final String? help;

  @override
  Widget build(BuildContext context) {
    final p = AppPalette.of(context);
    return AppField(
      label: label,
      help: help,
      child: DropdownButtonFormField<T>(
        key: fieldKey,
        initialValue: value,
        isExpanded: true,
        items: items,
        onChanged: onChanged,
        // The dense button is 24 px high: 42 px in all (48 for a finger).
        decoration: AppField.decoration(context).copyWith(
          contentPadding: EdgeInsets.fromLTRB(
            14,
            _touch(context) ? 12 : 9,
            10,
            _touch(context) ? 12 : 9,
          ),
        ),
        style: uiText(size: 14, weight: FontWeight.w500, color: p.ink),
        icon: const Icon(Icons.keyboard_arrow_down_rounded, size: 20),
        iconEnabledColor: p.muted,
        iconDisabledColor: p.disabled,
        dropdownColor: p.tile,
        borderRadius: BorderRadius.circular(12),
      ),
    );
  }
}

/// The one fold of the product: a 16 px chevron on the left that turns down
/// when open, and a 13/600 label in the secondary ink. Never amber: opening
/// more detail is not the surface's main action.
///
/// With [open] the caller owns the state and usually shows what is folded
/// itself; without it the fold keeps its own state and shows [children].
class AppDisclosure extends StatefulWidget {
  const AppDisclosure({
    super.key,
    required this.label,
    this.headerKey,
    this.open,
    this.onChanged,
    this.initiallyOpen = false,
    this.maintainState = false,
    this.children = const [],
  });

  final String label;

  /// The key of the row that is pressed.
  final Key? headerKey;
  final bool? open;
  final ValueChanged<bool>? onChanged;
  final bool initiallyOpen;

  /// Keep [children] alive (not shown) while the fold is closed.
  final bool maintainState;
  final List<Widget> children;

  @override
  State<AppDisclosure> createState() => _AppDisclosureState();
}

class _AppDisclosureState extends State<AppDisclosure> {
  late bool _own = widget.initiallyOpen;
  bool get _open => widget.open ?? _own;

  void _toggle() {
    final next = !_open;
    if (widget.open == null) setState(() => _own = next);
    widget.onChanged?.call(next);
  }

  @override
  Widget build(BuildContext context) {
    final p = AppPalette.of(context);
    final scale = MediaQuery.textScalerOf(context).scale(1);
    final still = MediaQuery.maybeDisableAnimationsOf(context) ?? false;
    final header = Align(
      alignment: Alignment.centerLeft,
      // One node: a button that says whether it is open.
      child: MergeSemantics(
        child: Semantics(
          expanded: _open,
          child: TextButton(
            key: widget.headerKey,
            onPressed: _toggle,
            style: AppButtons.link(p).copyWith(
              minimumSize: WidgetStatePropertyAll(
                Size(0, _touch(context) ? 48 : 36),
              ),
              padding: const WidgetStatePropertyAll(
                EdgeInsets.fromLTRB(2, 8, 10, 8),
              ),
              tapTargetSize: MaterialTapTargetSize.shrinkWrap,
              textStyle: WidgetStatePropertyAll(
                uiText(size: 13, weight: FontWeight.w600),
              ),
            ),
            child: Row(
              mainAxisSize: MainAxisSize.min,
              children: [
                AnimatedRotation(
                  turns: _open ? .25 : 0,
                  duration: still
                      ? Duration.zero
                      : const Duration(milliseconds: 180),
                  curve: Curves.easeOutCubic,
                  child: Icon(
                    Icons.chevron_right_rounded,
                    size: 16 * scale,
                    color: p.muted,
                  ),
                ),
                const SizedBox(width: 6),
                Flexible(child: KeepAllText(widget.label)),
              ],
            ),
          ),
        ),
      ),
    );
    if (widget.children.isEmpty) return header;
    final body = Column(
      mainAxisSize: MainAxisSize.min,
      crossAxisAlignment: CrossAxisAlignment.stretch,
      children: widget.children,
    );
    return Column(
      mainAxisSize: MainAxisSize.min,
      crossAxisAlignment: CrossAxisAlignment.stretch,
      children: [
        header,
        if (widget.maintainState)
          Offstage(
            offstage: !_open,
            child: TickerMode(enabled: _open, child: body),
          )
        else if (_open)
          body,
      ],
    );
  }
}
