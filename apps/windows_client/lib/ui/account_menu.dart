import 'dart:math' as math;

import 'package:flutter/material.dart';
import 'package:flutter/services.dart';

import '../data/appearance_settings.dart';
import '../data/session_activity.dart';
import 'session_access_controls.dart';
import 'app_dialog.dart';
import 'app_menu.dart';
import 'app_theme.dart';
import 'appearance_switch.dart'
    show
        applyLanguageChoice,
        applySceneChoice,
        applyThemeChoice,
        languageChoiceLabel;
import 'korean_text.dart';
import '../l10n/l10n.dart';

/// One command row of the account menu.
@immutable
class AccountMenuEntry {
  const AccountMenuEntry({
    required this.key,
    required this.icon,
    required this.label,
    required this.onPressed,
    this.destructive = false,
    this.tooltip,
    this.disabledReason,
  });
  final String key;
  final IconData icon;
  final String label;

  /// Null greys the row out ([disabledReason] says why, as its tooltip).
  final VoidCallback? onPressed;

  /// Ends what is open (로그아웃, 완전 종료): the only coloured rows, in red.
  final bool destructive;
  final String? tooltip, disabledReason;
}

/// The account menu's one grid: every row —
/// commands and the 테마/배경 choices alike — has the same height, the same
/// side padding, a neutral icon in one column and its label in the next.
/// [touch] is the Android geometry (48 dp rows); rows grow with the text.
@immutable
class AccountMenuMetrics {
  const AccountMenuMetrics({required this.touch, this.textScale = 1});

  factory AccountMenuMetrics.of(BuildContext context, {required bool touch}) =>
      AccountMenuMetrics(
        touch: touch,
        textScale: MediaQuery.textScalerOf(context).scale(1),
      );

  final bool touch;
  final double textScale;

  double get _grow => math.max(0, textScale - 1);

  /// The panel's padding: rows, the identity card and the rules stand this
  /// far inside its edge, so hover fills and focus rings never touch it.
  double get inset => touch ? 8 : 6;

  /// Inside a row, before the icon and after the label or choice track.
  double get pad => touch ? 12 : 10;

  /// Icons and the monogram follow large text half-way, so they keep their
  /// weight beside it without crowding the row.
  double get icon => ((touch ? 20 : 18) * (1 + _grow / 2)).roundToDouble();
  double get avatar => ((touch ? 40 : 36) * (1 + _grow / 2)).roundToDouble();
  double get gap => touch ? 14 : 12;
  double get font => touch ? 14 : 13;
  double get radius => touch ? 10 : 8;

  /// The choice track's margin inside its row, and the thumb's inside it.
  double get trackInset => touch ? 6 : 4;
  double get trackPad => touch ? 3 : 2;

  /// One row height for every row: 36 px (48 dp by finger) until large text
  /// needs more, then the label's line and a fixed margin.
  double get row => math
      .max(touch ? 48.0 : 36.0, font * textScale * 1.4 + (touch ? 16 : 12))
      .roundToDouble();

  /// Where a row's label starts, from the row's own left edge.
  double get labelStart => pad + icon + gap;

  double width(Size window) => math.max(
    0,
    math.min(
      (touch ? 296.0 : 264.0) * textScale.clamp(1.0, 1.6),
      window.width - 24,
    ),
  );
}

/// The account's first letter in a tonal amber circle (the solid amber is
/// kept for the update notice, the one thing in the header that asks to be
/// noticed).
class AccountMonogram extends StatelessWidget {
  const AccountMonogram({
    super.key,
    required this.label,
    required this.size,
    this.demo = false,
    this.neutral = false,
  });
  final String label;
  final bool demo, neutral;
  final double size;

  @override
  Widget build(BuildContext context) {
    final p = AppPalette.of(context);
    final initial = label.trim().characters.firstOrNull?.toUpperCase();
    return Container(
      width: size,
      height: size,
      alignment: Alignment.center,
      decoration: BoxDecoration(
        color: neutral ? p.subtle : p.amberSoft,
        shape: BoxShape.circle,
        border: neutral
            ? null
            : Border.all(color: p.amberLine.withValues(alpha: .5)),
      ),
      child: demo || initial == null
          ? Icon(
              demo ? Icons.preview_outlined : Icons.person_outline_rounded,
              size: size * .56,
              color: neutral ? p.muted : p.amberInk,
            )
          : KeepAllText(
              initial,
              textScaler: TextScaler.noScaling,
              style: uiText(
                size: size * .45,
                weight: FontWeight.w600,
                color: neutral ? p.ink : p.amberInk,
                height: 1,
              ),
            ),
    );
  }
}

/// Builds the button that opens the menu: [open] while the menu shows,
/// [toggle] for its press, [focusNode] for the button (it keeps focus while
/// the menu is open, so the arrows enter the menu and Escape comes back).
typedef AccountMenuButtonBuilder = Widget Function(
  BuildContext context,
  bool open,
  VoidCallback toggle,
  FocusNode focusNode,
);

/// The signed-in account's menu, one design on Windows and Android: an
/// identity card (who and where), then 앱 (정보, 로그 보기), 모양 (테마, 배경),
/// 로그아웃 and 완전 종료 on its own (언어, 자동 로그아웃, 2단계 인증 and
/// 비밀번호 변경 are in 설정, [showAppSettings]), each group set
/// apart by a hairline. Icons are neutral; only the rows that end what is
/// open are red. Nothing is highlighted on open: a menu opened from the
/// keyboard focuses its first row and shows the focus ring, one opened by
/// pointer or finger leaves focus on the button until ↓ or ↑.
class AccountMenu extends StatefulWidget {
  const AccountMenu({
    super.key,
    required this.touch,
    required this.account,
    required this.serverLine,
    required this.apps,
    required this.accountEntries,
    required this.builder,
    this.exit,
    this.demo = false,
    this.focusStyle,
  });
  final bool touch, demo;

  /// The signed-in name, and "server · version" beneath it.
  final String account, serverLine;

  /// 앱 and 계정 rows, and 완전 종료 (null where the platform has none).
  final List<AccountMenuEntry> apps, accountEntries;
  final AccountMenuEntry? exit;
  final AccountMenuButtonBuilder builder;

  /// A platform's own focus paint (Android TV), applied to every control.
  final ButtonStyle Function(BuildContext context, ButtonStyle base)?
  focusStyle;

  @override
  State<AccountMenu> createState() => _AccountMenuState();
}

class _AccountMenuState extends State<AccountMenu> {
  final _menu = MenuController();
  final _button = FocusNode(debugLabel: 'account-menu-button');

  /// The first and last rows, for ↓/↑ from the button.
  final _first = FocusNode(debugLabel: 'account-menu-first');
  final _last = FocusNode(debugLabel: 'account-menu-last');
  bool _open = false;

  static final _activationKeys = {
    LogicalKeyboardKey.enter,
    LogicalKeyboardKey.numpadEnter,
    LogicalKeyboardKey.space,
    LogicalKeyboardKey.select,
    LogicalKeyboardKey.gameButtonA,
  };

  @override
  void dispose() {
    AppMenu.closed(_menu);
    _button.dispose();
    _first.dispose();
    _last.dispose();
    super.dispose();
  }

  void _toggle(BuildContext anchor) {
    if (_menu.isOpen) return _menu.close();
    final box = anchor.findRenderObject() as RenderBox?;
    final size = box?.size ?? Size.zero;
    final width = AccountMenuMetrics.of(
      context,
      touch: widget.touch,
    ).width(MediaQuery.sizeOf(context));
    _menu.open(position: Offset(size.width - width, size.height + 6));
    // Opened from the keyboard (Enter, Space, a remote's OK): the first row
    // takes focus, as in a native menu. A mouse click also leaves Flutter in
    // its keyboard highlight mode, so the press itself is what tells them
    // apart; a click or tap leaves focus on the button until ↓ or ↑
    // (otherwise the first row looks selected on every open).
    final keyboard = HardwareKeyboard.instance.logicalKeysPressed.any(
      _activationKeys.contains,
    );
    if (keyboard) {
      WidgetsBinding.instance.addPostFrameCallback((_) {
        if (_menu.isOpen) _first.requestFocus();
      });
    }
  }

  ButtonStyle _focus(BuildContext context, ButtonStyle style) =>
      widget.focusStyle?.call(context, style) ?? style;

  @override
  Widget build(BuildContext context) {
    final p = AppPalette.of(context);
    final m = AccountMenuMetrics.of(context, touch: widget.touch);
    final width = m.width(MediaQuery.sizeOf(context));
    final appearance = AppearanceScope.maybeOf(context) != null;
    final rows = [...widget.apps, ...widget.accountEntries, ?widget.exit];
    final first = rows.firstOrNull, last = rows.lastOrNull;

    Widget row(AccountMenuEntry entry) => _CommandRow(
      entry: entry,
      metrics: m,
      focusNode: identical(entry, first)
          ? _first
          : identical(entry, last)
          ? _last
          : null,
      focusStyle: _focus,
    );
    Widget rule() => _Rule(metrics: m);

    final children = <Widget>[
      _IdentityCard(
        metrics: m,
        account: widget.account,
        serverLine: widget.serverLine,
        demo: widget.demo,
      ),
      for (final entry in widget.apps) row(entry),
      if (appearance) ...[
        rule(),
        _AppearanceRows(metrics: m, focusStyle: _focus),
      ],
      if (widget.accountEntries.isNotEmpty) ...[
        rule(),
        for (final entry in widget.accountEntries) row(entry),
      ],
      if (widget.exit case final exit?) ...[rule(), row(exit)],
    ];

    Widget menu = MenuAnchor(
      controller: _menu,
      consumeOutsideTap: true,
      // A fixed width (not the content's), so the menu's right edge lines up
      // with the button's.
      crossAxisUnconstrained: false,
      childFocusNode: _button,
      onOpen: () {
        AppMenu.opened(_menu);
        if (mounted) setState(() => _open = true);
      },
      onClose: () {
        AppMenu.closed(_menu);
        if (mounted) setState(() => _open = false);
      },
      style: AppMenu.menuStyle(p).copyWith(
        padding: WidgetStatePropertyAll(EdgeInsets.all(m.inset)),
        fixedSize: WidgetStatePropertyAll(Size(width, double.infinity)),
        shape: WidgetStatePropertyAll(
          RoundedRectangleBorder(
            borderRadius: BorderRadius.circular(m.radius + m.inset),
          ),
        ),
      ),
      menuChildren: children,
      child: Builder(
        builder: (anchor) => Actions(
          // While the menu is open, ↓ and ↑ on the button enter it (a plain
          // MenuAnchor would move focus to the page behind it instead).
          actions: {
            if (_open)
              DirectionalFocusIntent: CallbackAction<DirectionalFocusIntent>(
                onInvoke: (intent) {
                  switch (intent.direction) {
                    case TraversalDirection.down:
                      _first.requestFocus();
                    case TraversalDirection.up:
                      (_last.canRequestFocus ? _last : _first).requestFocus();
                    case TraversalDirection.left || TraversalDirection.right:
                      break;
                  }
                  return null;
                },
              ),
          },
          child: widget.builder(anchor, _open, () => _toggle(anchor), _button),
        ),
      ),
    );
    // By finger the system Back closes the menu first, as it closed the
    // popup this menu replaced (a MenuAnchor is not a route).
    if (widget.touch) {
      menu = PopScope(
        canPop: !_open,
        onPopInvokedWithResult: (didPop, _) {
          if (!didPop && _menu.isOpen) _menu.close();
        },
        child: menu,
      );
    }
    return menu;
  }
}

/// A keyboard focus: focused while Flutter shows keyboard highlights.
bool _keyboardFocus(Set<WidgetState> states) =>
    states.contains(WidgetState.focused) &&
    FocusManager.instance.highlightMode == FocusHighlightMode.traditional;

/// Who is signed in where: the monogram, the name and the muted server and
/// version line, which wraps instead of being cut. Not a control.
class _IdentityCard extends StatelessWidget {
  const _IdentityCard({
    required this.metrics,
    required this.account,
    required this.serverLine,
    required this.demo,
  });
  final AccountMenuMetrics metrics;
  final String account, serverLine;
  final bool demo;

  @override
  Widget build(BuildContext context) {
    final p = AppPalette.of(context);
    final m = metrics;
    return ExcludeFocus(
      child: Padding(
        key: const Key('account-identity'),
        padding: EdgeInsets.only(bottom: m.inset),
        child: Container(
          width: double.infinity,
          padding: EdgeInsets.symmetric(
            horizontal: m.pad,
            vertical: m.touch ? 12 : 10,
          ),
          decoration: BoxDecoration(
            color: m.touch ? p.subtle : Colors.transparent,
            borderRadius: BorderRadius.circular(m.radius),
            border: m.touch
                ? Border.all(color: p.line.withValues(alpha: .6))
                : null,
          ),
          child: Row(
            children: [
              AccountMonogram(
                label: account,
                demo: demo,
                size: m.avatar,
                neutral: !m.touch,
              ),
              SizedBox(width: m.gap),
              Expanded(
                child: Column(
                  crossAxisAlignment: CrossAxisAlignment.start,
                  mainAxisSize: MainAxisSize.min,
                  children: [
                    KeepAllText(
                      account,
                      style: uiText(
                        size: m.font + 1,
                        weight: FontWeight.w600,
                        color: p.ink,
                        height: 1.3,
                      ),
                    ),
                    const SizedBox(height: 2),
                    KeepAllText(
                      serverLine,
                      key: const Key('account-server'),
                      style: uiText(
                        size: m.font - 1.5,
                        color: p.muted,
                        height: 1.35,
                      ),
                    ),
                  ],
                ),
              ),
            ],
          ),
        ),
      ),
    );
  }
}

/// The hairline between groups, as wide as the rows.
class _Rule extends StatelessWidget {
  const _Rule({required this.metrics});
  final AccountMenuMetrics metrics;

  @override
  Widget build(BuildContext context) => Padding(
    padding: EdgeInsets.symmetric(vertical: metrics.inset - 1),
    child: Divider(height: 1, thickness: 1, color: AppPalette.of(context).line),
  );
}

/// A command row: a neutral icon, the label, a soft fill on hover and the
/// amber ring on keyboard focus; a destructive row is red throughout.
class _CommandRow extends StatelessWidget {
  const _CommandRow({
    required this.entry,
    required this.metrics,
    required this.focusNode,
    required this.focusStyle,
  });
  final AccountMenuEntry entry;
  final AccountMenuMetrics metrics;
  final FocusNode? focusNode;
  final ButtonStyle Function(BuildContext, ButtonStyle) focusStyle;

  static ButtonStyle style(
    AppPalette p,
    AccountMenuMetrics m, {
    required bool destructive,
  }) {
    Color ink(Set<WidgetState> states, {required bool icon}) =>
        states.contains(WidgetState.disabled)
        ? p.faint
        : destructive
        ? p.red
        : !icon ||
              states.contains(WidgetState.hovered) ||
              _keyboardFocus(states)
        ? p.ink
        : p.muted;
    return ButtonStyle(
      minimumSize: WidgetStatePropertyAll(Size(0, m.row)),
      maximumSize: WidgetStatePropertyAll(Size(double.infinity, m.row)),
      padding: WidgetStatePropertyAll(EdgeInsets.symmetric(horizontal: m.pad)),
      visualDensity: VisualDensity.standard,
      tapTargetSize: MaterialTapTargetSize.shrinkWrap,
      elevation: const WidgetStatePropertyAll(0),
      backgroundColor: const WidgetStatePropertyAll(Colors.transparent),
      surfaceTintColor: const WidgetStatePropertyAll(Colors.transparent),
      textStyle: WidgetStatePropertyAll(
        uiText(size: m.font, weight: FontWeight.w500),
      ),
      foregroundColor: WidgetStateProperty.resolveWith(
        (states) => ink(states, icon: false),
      ),
      iconColor: WidgetStateProperty.resolveWith(
        (states) => ink(states, icon: true),
      ),
      iconSize: WidgetStatePropertyAll(m.icon),
      overlayColor: WidgetStateProperty.resolveWith((states) {
        final tone = destructive ? p.red : p.ink;
        if (states.contains(WidgetState.pressed)) {
          return tone.withValues(alpha: .12);
        }
        if (states.contains(WidgetState.hovered) || _keyboardFocus(states)) {
          return destructive ? p.red.withValues(alpha: .09) : p.hover;
        }
        return Colors.transparent;
      }),
      side: WidgetStateProperty.resolveWith(
        (states) => _keyboardFocus(states)
            ? BorderSide(color: p.amber, width: m.touch ? 2 : 1.5)
            : BorderSide.none,
      ),
      shape: WidgetStatePropertyAll(
        RoundedRectangleBorder(borderRadius: BorderRadius.circular(m.radius)),
      ),
      mouseCursor: WidgetStateProperty.resolveWith(
        (states) => states.contains(WidgetState.disabled)
            ? SystemMouseCursors.basic
            : SystemMouseCursors.click,
      ),
    );
  }

  @override
  Widget build(BuildContext context) {
    final p = AppPalette.of(context);
    final m = metrics;
    final button = MenuItemButton(
      key: Key(entry.key),
      focusNode: focusNode,
      // Hover fills the row; it never moves focus (a hovered row would
      // otherwise take the keyboard ring).
      requestFocusOnHover: false,
      onPressed: entry.onPressed,
      style: focusStyle(context, style(p, m, destructive: entry.destructive)),
      child: Row(
        children: [
          Icon(entry.icon),
          SizedBox(width: m.gap),
          Expanded(
            child: KeepAllText(
              entry.label,
              maxLines: 1,
              overflow: TextOverflow.ellipsis,
            ),
          ),
        ],
      ),
    );
    final hint = entry.onPressed == null ? entry.disabledReason : entry.tooltip;
    return hint != null ? Tooltip(message: hint, child: button) : button;
  }
}

typedef _Option<T> = (T value, String key, String label);

/// 테마 and 배경: the row grid of the commands (icon, label) with a two-way
/// choice track at the row's end. Both tracks share one width so they line
/// up; where they do not fit beside the label (narrow window, large text)
/// each goes under its label, still in the label column. A choice applies at
/// once and the menu stays open, so the change can be seen and undone in
/// place; the appearance is remembered apart from the server and sign-in
/// settings.
class _AppearanceRows extends StatelessWidget {
  const _AppearanceRows({required this.metrics, required this.focusStyle});
  final AccountMenuMetrics metrics;
  final ButtonStyle Function(BuildContext, ButtonStyle) focusStyle;

  static TextStyle labelStyle(AccountMenuMetrics m, Color color) =>
      uiText(size: m.font, weight: FontWeight.w500, color: color);
  static TextStyle segmentStyle(AccountMenuMetrics m, {required bool bold}) =>
      uiText(
        size: m.font - .5,
        weight: bold ? FontWeight.w600 : FontWeight.w500,
      );

  @override
  Widget build(BuildContext context) {
    final appearance = AppearanceScope.maybeOf(context);
    if (appearance == null) return const SizedBox.shrink();
    final p = AppPalette.of(context);
    final m = metrics;
    final scaler = MediaQuery.textScalerOf(context);
    final enabled = !appearance.saving;
    final themes = <_Option<ThemeMode>>[
      (ThemeMode.dark, 'theme-dark', l10n.dark),
      (ThemeMode.light, 'theme-light', l10n.light),
    ];
    final scenes = <_Option<BackgroundScene>>[
      for (final scene in BackgroundScene.values)
        (scene, 'scene-${scene.name}', scene.label),
    ];
    double measure(String text, TextStyle style) {
      final painter = TextPainter(
        text: TextSpan(text: keepAll(text), style: style),
        textScaler: scaler,
        textDirection: TextDirection.ltr,
        maxLines: 1,
      )..layout();
      final width = painter.width;
      painter.dispose();
      return width;
    }

    // One segment width for every option (the bold, selected weight), so
    // both tracks are equal and a choice never shifts its neighbour.
    final segmentPad = m.touch ? 12.0 : 10.0;
    final segment =
        [
          for (final (_, _, label) in [...themes, ...scenes])
            measure(label, segmentStyle(m, bold: true)),
        ].reduce(math.max).ceilToDouble() +
        2 * segmentPad;
    final count = math.max(themes.length, scenes.length);
    final track = count * segment + 2 * m.trackPad;
    final label = [
      for (final text in [l10n.theme2, l10n.background3])
        measure(text, labelStyle(m, p.ink)),
    ].reduce(math.max).ceilToDouble();

    return LayoutBuilder(
      builder: (context, box) {
        final stacked =
            m.labelStart + label + 12 + track + m.pad > box.maxWidth;
        Widget choice<T>(
          String key,
          IconData icon,
          String title,
          T value,
          List<_Option<T>> options,
          ValueChanged<T> onSelected,
        ) => _ChoiceRow<T>(
          key: Key(key),
          metrics: m,
          icon: icon,
          label: title,
          value: value,
          options: options,
          enabled: enabled,
          stacked: stacked,
          trackWidth: track,
          onSelected: onSelected,
          focusStyle: focusStyle,
        );
        return SizedBox(
          width: box.maxWidth,
          child: Column(
            mainAxisSize: MainAxisSize.min,
            children: [
              choice<ThemeMode>(
                'account-theme',
                Icons.contrast_rounded,
                l10n.theme2,
                appearance.mode,
                themes,
                (value) => applyThemeChoice(context, appearance, value),
              ),
              choice<BackgroundScene>(
                'account-scene',
                Icons.landscape_outlined,
                l10n.background3,
                appearance.scene,
                scenes,
                (value) => applySceneChoice(context, appearance, value),
              ),
            ],
          ),
        );
      },
    );
  }
}

class _ChoiceRow<T> extends StatelessWidget {
  const _ChoiceRow({
    super.key,
    required this.metrics,
    this.icon,
    required this.label,
    required this.value,
    required this.options,
    required this.enabled,
    required this.stacked,
    required this.trackWidth,
    required this.onSelected,
    required this.focusStyle,
    this.fit = false,
  });
  final AccountMenuMetrics metrics;

  /// Option names shrink to their segment instead of being cut (the three
  /// language names on a narrow phone at large text).
  final bool fit;

  /// Null in the settings window: the label starts the row.
  final IconData? icon;
  final String label;
  final T value;
  final List<_Option<T>> options;
  final bool enabled, stacked;
  final double trackWidth;
  final ValueChanged<T> onSelected;
  final ButtonStyle Function(BuildContext, ButtonStyle) focusStyle;

  @override
  Widget build(BuildContext context) {
    final p = AppPalette.of(context);
    final m = metrics;
    final head = ExcludeSemantics(
      child: Row(
        children: [
          if (icon case final icon?) ...[
            Icon(icon, size: m.icon, color: enabled ? p.muted : p.faint),
            SizedBox(width: m.gap),
          ],
          Flexible(
            child: KeepAllText(
              label,
              maxLines: 1,
              overflow: TextOverflow.ellipsis,
              style: _AppearanceRows.labelStyle(m, enabled ? p.ink : p.faint),
            ),
          ),
        ],
      ),
    );
    final track = Semantics(
      container: true,
      explicitChildNodes: true,
      label: label,
      child: SizedBox(
        height: m.row,
        child: Stack(
          children: [
            Positioned.fill(
              top: m.trackInset,
              bottom: m.trackInset,
              child: DecoratedBox(
                decoration: BoxDecoration(
                  color: p.subtle,
                  borderRadius: BorderRadius.circular(m.radius),
                  border: Border.all(color: p.line),
                ),
              ),
            ),
            Padding(
              padding: EdgeInsets.symmetric(horizontal: m.trackPad),
              child: Row(
                crossAxisAlignment: CrossAxisAlignment.stretch,
                children: [
                  for (final (option, key, text) in options)
                    Expanded(
                      child: _Segment(
                        key: Key(key),
                        metrics: m,
                        text: text,
                        selected: option == value,
                        fit: fit,
                        onPressed: !enabled
                            ? null
                            : () {
                                if (option != value) onSelected(option);
                              },
                        focusStyle: focusStyle,
                      ),
                    ),
                ],
              ),
            ),
          ],
        ),
      ),
    );
    if (stacked) {
      return Column(
        mainAxisSize: MainAxisSize.min,
        children: [
          SizedBox(
            height: m.row,
            child: Padding(
              padding: EdgeInsets.symmetric(horizontal: m.pad),
              child: head,
            ),
          ),
          Padding(
            padding: EdgeInsets.only(
              left: icon == null ? m.pad : m.labelStart,
              right: m.pad,
            ),
            child: track,
          ),
        ],
      );
    }
    return SizedBox(
      height: m.row,
      child: Padding(
        padding: EdgeInsets.symmetric(horizontal: m.pad),
        child: Row(
          children: [
            Expanded(child: head),
            const SizedBox(width: 12),
            SizedBox(width: trackWidth, child: track),
          ],
        ),
      ),
    );
  }
}

/// One choice: the whole row height takes the press (48 dp by finger), the
/// thumb is drawn inside the track — amber when chosen, a soft fill on
/// hover and the amber ring on keyboard focus.
class _Segment extends StatelessWidget {
  const _Segment({
    super.key,
    required this.metrics,
    required this.text,
    required this.selected,
    required this.onPressed,
    required this.focusStyle,
    this.fit = false,
  });
  final AccountMenuMetrics metrics;
  final String text;
  final bool selected, fit;
  final VoidCallback? onPressed;
  final ButtonStyle Function(BuildContext, ButtonStyle) focusStyle;

  @override
  Widget build(BuildContext context) {
    final p = AppPalette.of(context);
    final m = metrics;
    final radius = BorderRadius.circular(m.radius - m.trackPad);
    Color ink(Set<WidgetState> states) => states.contains(WidgetState.disabled)
        ? p.faint
        : selected
        ? p.amberInk
        : states.contains(WidgetState.hovered) || _keyboardFocus(states)
        ? p.ink
        : p.muted;
    final style = ButtonStyle(
      minimumSize: const WidgetStatePropertyAll(Size.zero),
      padding: WidgetStatePropertyAll(
        EdgeInsets.symmetric(horizontal: m.touch ? 8 : 6),
      ),
      tapTargetSize: MaterialTapTargetSize.shrinkWrap,
      visualDensity: VisualDensity.standard,
      elevation: const WidgetStatePropertyAll(0),
      backgroundColor: const WidgetStatePropertyAll(Colors.transparent),
      overlayColor: const WidgetStatePropertyAll(Colors.transparent),
      splashFactory: NoSplash.splashFactory,
      textStyle: WidgetStatePropertyAll(
        _AppearanceRows.segmentStyle(m, bold: selected),
      ),
      foregroundColor: WidgetStateProperty.resolveWith(ink),
      shape: WidgetStatePropertyAll(
        RoundedRectangleBorder(borderRadius: radius),
      ),
      mouseCursor: WidgetStateProperty.resolveWith(
        (states) => states.contains(WidgetState.disabled) || selected
            ? SystemMouseCursors.basic
            : SystemMouseCursors.click,
      ),
      backgroundBuilder: (context, states, child) {
        final ring = _keyboardFocus(states);
        final fill = selected
            ? p.amberSoft
            : states.contains(WidgetState.pressed)
            ? p.ink.withValues(alpha: .10)
            : states.contains(WidgetState.hovered)
            ? p.hover
            : Colors.transparent;
        return Padding(
          padding: EdgeInsets.symmetric(vertical: m.trackInset + m.trackPad),
          child: DecoratedBox(
            decoration: BoxDecoration(
              color: fill,
              borderRadius: radius,
              border: ring
                  ? Border.all(color: p.amber, width: m.touch ? 2 : 1.5)
                  : selected
                  ? Border.all(color: p.amberLine)
                  : null,
            ),
            child: child,
          ),
        );
      },
    );
    return Semantics(
      selected: selected,
      inMutuallyExclusiveGroup: true,
      child: TextButton(
        onPressed: onPressed,
        style: focusStyle(context, style),
        child: fit
            ? FittedBox(
                fit: BoxFit.scaleDown,
                child: KeepAllText(
                  text,
                  maxLines: 1,
                  textAlign: TextAlign.center,
                ),
              )
            : KeepAllText(
                text,
                maxLines: 1,
                overflow: TextOverflow.ellipsis,
                textAlign: TextAlign.center,
              ),
      ),
    );
  }
}

/// 설정: the header's ⚙ (Windows) or the menu's 설정
/// (Android) opens this window: 언어 / Language, 자동 로그아웃, 2단계 인증 and
/// 비밀번호 변경, each one line with a short hint. The last two open what
/// they opened from the account menu before. A plain heading and plain rows:
/// no gear beside the title, no icon before a row.
Future<void> showAppSettings(
  BuildContext context, {
  required bool touch,
  SessionActivity? sessionAccess,
  VoidCallback? onManageMfa,
  VoidCallback? onChangePassword,
  ButtonStyle Function(BuildContext context, ButtonStyle base)? focusStyle,
}) => showDialog<void>(
  context: context,
  builder: (dialog) {
    VoidCallback? then(VoidCallback? action) => action == null
        ? null
        : () {
            Navigator.pop(dialog);
            action();
          };
    return AppDialog(
      key: const Key('app-settings-dialog'),
      tone: AppTone.neutral,
      title: l10n.settings,
      onClose: () => Navigator.pop(dialog),
      closeKey: const Key('close-app-settings'),
      body: [
        AppSettingsPanel(
          touch: touch,
          sessionAccess: sessionAccess,
          onManageMfa: then(onManageMfa),
          onChangePassword: then(onChangePassword),
          focusStyle: focusStyle,
        ),
      ],
    );
  },
);

/// The rows of [showAppSettings], in the account menu's grid.
class AppSettingsPanel extends StatelessWidget {
  const AppSettingsPanel({
    super.key,
    required this.touch,
    this.sessionAccess,
    this.onManageMfa,
    this.onChangePassword,
    this.focusStyle,
  });
  final bool touch;
  final SessionActivity? sessionAccess;
  final VoidCallback? onManageMfa, onChangePassword;
  final ButtonStyle Function(BuildContext context, ButtonStyle base)?
  focusStyle;

  @override
  Widget build(BuildContext context) {
    final m = AccountMenuMetrics.of(context, touch: touch);
    ButtonStyle focus(BuildContext context, ButtonStyle style) =>
        focusStyle?.call(context, style) ?? style;
    Widget rule() => _Rule(metrics: m);
    Widget command(
      String key,
      String label,
      String hint,
      VoidCallback action,
    ) => Column(
      key: Key('$key-row'),
      mainAxisSize: MainAxisSize.min,
      crossAxisAlignment: CrossAxisAlignment.stretch,
      children: [
        // A plain button in the menu rows' style (a MenuItemButton needs a
        // menu around it).
        TextButton(
          key: Key(key),
          onPressed: action,
          // In a dialog the label may take a second line at large text
          // rather than be cut.
          style: focus(
            context,
            _CommandRow.style(
              AppPalette.of(context),
              m,
              destructive: false,
            ).copyWith(
              maximumSize: const WidgetStatePropertyAll(Size.infinite),
            ),
          ),
          child: Row(children: [Expanded(child: KeepAllText(label))]),
        ),
        _SettingHint(metrics: m, text: hint),
      ],
    );
    return Column(
      key: const Key('app-settings'),
      mainAxisSize: MainAxisSize.min,
      crossAxisAlignment: CrossAxisAlignment.stretch,
      children: [
        _LanguageRow(metrics: m, focusStyle: focus),
        _SettingHint(metrics: m, text: l10n.languageHint),
        if (sessionAccess case final activity?) ...[
          rule(),
          SessionIdleControl(activity: activity, touch: touch),
        ],
        if (onManageMfa case final action?) ...[
          rule(),
          command(
            'account-mfa',
            l10n.twoStepVerification,
            l10n.mfaSettingHint,
            action,
          ),
        ],
        if (onChangePassword case final action?) ...[
          rule(),
          command(
            'account-change-password',
            l10n.changePassword,
            l10n.passwordSettingHint,
            action,
          ),
        ],
      ],
    );
  }
}

/// A setting's short hint under its row, under the label.
class _SettingHint extends StatelessWidget {
  const _SettingHint({required this.metrics, required this.text});
  final AccountMenuMetrics metrics;
  final String text;

  @override
  Widget build(BuildContext context) => Padding(
    padding: EdgeInsets.fromLTRB(
      metrics.pad,
      0,
      metrics.pad,
      metrics.inset,
    ),
    child: KeepAllText(
      text,
      style: uiText(
        size: 12,
        color: AppPalette.of(context).muted,
        height: 1.45,
      ),
    ),
  );
}

/// 언어 / Language: a three-way track (자동, 한국어, English) that applies at
/// once; beside its label, or under it where it does not fit.
class _LanguageRow extends StatelessWidget {
  const _LanguageRow({required this.metrics, required this.focusStyle});
  final AccountMenuMetrics metrics;
  final ButtonStyle Function(BuildContext, ButtonStyle) focusStyle;

  @override
  Widget build(BuildContext context) => ListenableBuilder(
    listenable: AppLanguageController.instance,
    builder: (context, _) {
      final p = AppPalette.of(context);
      final m = metrics;
      final scaler = MediaQuery.textScalerOf(context);
      double measure(String text, TextStyle style) {
        final painter = TextPainter(
          text: TextSpan(text: keepAll(text), style: style),
          textScaler: scaler,
          textDirection: TextDirection.ltr,
          maxLines: 1,
        )..layout();
        final width = painter.width;
        painter.dispose();
        return width;
      }

      final options = <_Option<AppLanguage>>[
        for (final language in AppLanguage.values)
          (
            language,
            'language-${language.name}',
            languageChoiceLabel(language),
          ),
      ];
      final segment =
          [
            for (final (_, _, text) in options)
              measure(text, _AppearanceRows.segmentStyle(m, bold: true)),
          ].reduce(math.max).ceilToDouble() +
          2 * (m.touch ? 12.0 : 10.0);
      final track = options.length * segment + 2 * m.trackPad;
      final label = measure(
        l10n.language,
        _AppearanceRows.labelStyle(m, p.ink),
      ).ceilToDouble();
      return LayoutBuilder(
        builder: (context, box) => _ChoiceRow<AppLanguage>(
          key: const Key('account-language'),
          metrics: m,
          label: l10n.language,
          value: AppLanguageController.instance.choice,
          options: options,
          enabled: true,
          stacked: m.pad + label + 12 + track + m.pad > box.maxWidth,
          trackWidth: track,
          onSelected: (value) => applyLanguageChoice(context, value),
          focusStyle: focusStyle,
          fit: true,
        ),
      );
    },
  );
}
