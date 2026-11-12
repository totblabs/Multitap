import 'package:flutter/material.dart';

import 'app_palette.dart';
import 'korean_text.dart';
import '../l10n/l10n.dart';

enum HostSetupState {
  complete,
  setup,
  attention,
  checking,
  approval,
  unavailable,
  available;

  String get label => switch (this) {
    complete => l10n.normal,
    setup => l10n.setupNeeded,
    attention => l10n.needsCheck,
    checking => l10n.checking,
    approval => l10n.pendingApproval,
    unavailable => l10n.notSupported,
    available => l10n.available2,
  };

  /// The area wants the user: its navigation row carries the amber dot.
  bool get needsUser => this == setup || this == attention || this == approval;

  Color color(AppPalette palette) => switch (this) {
    complete => palette.green,
    setup || approval || attention => palette.amber,
    checking || unavailable || available => palette.muted,
  };

  /// The state's words in a colour readable on both appearances.
  Color ink(AppPalette palette) => switch (this) {
    setup || approval || attention => palette.amberInk,
    _ => color(palette),
  };
}

/// The colour-coded dot of a state line.
class HostStateDot extends StatelessWidget {
  const HostStateDot({super.key, required this.state, this.size = 7});

  final HostSetupState state;
  final double size;

  @override
  Widget build(BuildContext context) => Container(
    width: size,
    height: size,
    decoration: BoxDecoration(
      color: state.color(AppPalette.of(context)),
      shape: BoxShape.circle,
    ),
  );
}

/// A state line: a colour-coded dot and the state's words, no pill.
class HostSetupBadge extends StatelessWidget {
  const HostSetupBadge({super.key, required this.state, this.label});

  final HostSetupState state;

  /// The area's own words for the state (see [HostSettingsNavigation.stateLabel]).
  final String? label;

  @override
  Widget build(BuildContext context) {
    final palette = AppPalette.of(context);
    return Row(
      mainAxisSize: MainAxisSize.min,
      children: [
        HostStateDot(state: state),
        const SizedBox(width: 6),
        Flexible(
          child: KeepAllText(
            label ?? state.label,
            maxLines: 1,
            overflow: TextOverflow.ellipsis,
            style: TextStyle(
              color: state.ink(palette),
              fontSize: 13,
              height: 1.3,
              fontWeight: FontWeight.w500,
            ),
          ),
        ),
      ],
    );
  }
}

/// The left navigation: plain rows of an icon and a label. A row says nothing
/// about its area's state; one that needs the user carries a small amber dot
/// at its right. 진단 보고서 stays apart at the bottom.
class HostSettingsNavigation extends StatelessWidget {
  const HostSettingsNavigation({
    super.key,
    required this.selected,
    required this.states,
    required this.onSelected,
    required this.enabled,
    required this.rail,
    required this.width,
  });

  final int selected;
  final List<HostSetupState> states;
  final ValueChanged<int> onSelected;
  final bool enabled, rail;
  final double width;

  // Page numbers stay stable (keys, deep links). Page 3 is unused: its slot
  // stays empty and unlisted.
  static List<String> get titles => [l10n.overview, l10n.serverConnection, l10n.virtualDisplay2, '', l10n.diagnosticReport, l10n.windowsSign2, l10n.devices];
  static List<String> get shortTitles => [l10n.overview, l10n.server, l10n.screen, '', l10n.diagnostics, l10n.sign, l10n.devices];

  /// The setup flow (2026-11-03): the overview, then the server, the Windows
  /// sign-in the virtual screen needs, the screen itself and the optional
  /// devices. Diagnostics is support, listed apart at the bottom.
  static const order = [0, 1, 5, 2, 6];
  static const support = [4];
  static const icons = [
    Icons.home_outlined,
    Icons.dns_outlined,
    Icons.desktop_windows_outlined,
    Icons.block,
    Icons.fact_check_outlined,
    Icons.person_outline_rounded,
    Icons.power_outlined,
  ];

  /// The words of an area's state on its own page, or null when the state
  /// says nothing (diagnostics that are simply available).
  static String? stateLabel(int index, HostSetupState state) =>
      switch ((index, state)) {
        (4, HostSetupState.available) => null,
        _ => state.label,
      };

  /// The overview is the summary itself and never carries the dot.
  bool _dot(int index) => index != 0 && states[index].needsUser;

  Widget _item(BuildContext context, int index) {
    final palette = AppPalette.of(context);
    final active = selected == index;
    final quiet = support.contains(index);
    final ink = active
        ? palette.amberInk
        : quiet
        ? palette.muted
        : Color.lerp(palette.muted, palette.ink, .55)!;
    final icon = Icon(icons[index], color: ink, size: rail ? 18 : 16);
    final dot = _dot(index)
        ? Container(
            key: Key('host-nav-dot-$index'),
            width: 8,
            height: 8,
            decoration: BoxDecoration(
              color: palette.amber,
              shape: BoxShape.circle,
            ),
          )
        : null;
    final label = TextStyle(
      fontSize: rail ? 12 : 14,
      height: 1.3,
      color: ink,
      fontWeight: active ? FontWeight.w600 : FontWeight.w400,
    );
    return Padding(
      padding: const EdgeInsets.only(bottom: 2),
      child: Semantics(
        selected: active,
        button: true,
        label: dot == null ? titles[index] : l10n.needsAttention(titles[index]),
        child: Tooltip(
          message: rail ? titles[index] : '',
          child: Material(
            color: active
                ? palette.amber.withValues(alpha: palette.dark ? .12 : .16)
                : Colors.transparent,
            borderRadius: BorderRadius.circular(8),
            child: InkWell(
              key: Key('host-nav-$index'),
              borderRadius: BorderRadius.circular(8),
              onTap: enabled ? () => onSelected(index) : null,
              child: ConstrainedBox(
                constraints: BoxConstraints(minHeight: rail ? 52 : 40),
                child: Padding(
                  padding: EdgeInsets.symmetric(
                    horizontal: rail ? 4 : 10,
                    vertical: rail ? 7 : 6,
                  ),
                  child: rail
                      ? Column(
                          mainAxisAlignment: MainAxisAlignment.center,
                          children: [
                            Stack(
                              clipBehavior: Clip.none,
                              children: [
                                icon,
                                if (dot != null)
                                  Positioned(right: -7, top: -3, child: dot),
                              ],
                            ),
                            const SizedBox(height: 4),
                            FittedBox(
                              fit: BoxFit.scaleDown,
                              child: KeepAllText(
                                shortTitles[index],
                                maxLines: 1,
                                style: label,
                              ),
                            ),
                          ],
                        )
                      : Row(
                          children: [
                            icon,
                            const SizedBox(width: 10),
                            Expanded(
                              child: KeepAllText(titles[index], style: label),
                            ),
                            // No gap: "Windows 로그인" keeps one line beside it.
                            ?dot,
                          ],
                        ),
                ),
              ),
            ),
          ),
        ),
      ),
    );
  }

  @override
  Widget build(BuildContext context) {
    final palette = AppPalette.of(context);
    final inset = EdgeInsets.symmetric(horizontal: rail ? 6 : 10);
    return SizedBox(
      key: const Key('host-settings-sidebar'),
      width: width,
      child: Material(
        color: palette.surface,
        // A handful of items: all stay built (finders, screen readers). The
        // setup steps scroll when a short, large-text window cannot show
        // them; 진단 보고서 stays at the bottom.
        child: Column(
          crossAxisAlignment: CrossAxisAlignment.stretch,
          children: [
            Expanded(
              child: SingleChildScrollView(
                padding: inset.copyWith(top: 12, bottom: 8),
                child: Column(
                  crossAxisAlignment: CrossAxisAlignment.stretch,
                  children: [for (final index in order) _item(context, index)],
                ),
              ),
            ),
            Padding(
              key: const Key('host-nav-support'),
              padding: inset.copyWith(top: 4, bottom: 10),
              child: Column(
                crossAxisAlignment: CrossAxisAlignment.stretch,
                children: [for (final index in support) _item(context, index)],
              ),
            ),
          ],
        ),
      ),
    );
  }
}
