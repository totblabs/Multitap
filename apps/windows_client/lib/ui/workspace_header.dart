import 'dart:async';

import 'package:flutter/material.dart';

import 'account_menu.dart';
import '../data/session_activity.dart';
import 'session_access_controls.dart';
import 'app_exit.dart';
import 'app_theme.dart';
import 'components.dart';
import 'korean_text.dart';
import '../l10n/l10n.dart';

/// The header controls' one geometry: 36 px high, 10 px corners, 20 px
/// rounded icons, 8 px apart.
abstract final class HeaderControl {
  static const height = 36.0, radius = 10.0, icon = 20.0, gap = 8.0;

  /// Below this width (in text-scaled logical pixels) the controls are icons
  /// only: the update button loses its label and the account its name.
  static const compactWidth = 600.0;

  static OutlinedBorder get shape =>
      RoundedRectangleBorder(borderRadius: BorderRadius.circular(radius));

  /// A keyboard-focus ring; the pointer gets the hover fill instead (a
  /// button focused by a click, as the account button is while its menu is
  /// open, shows no ring).
  static WidgetStateProperty<BorderSide?> focusRing(Color color) =>
      WidgetStateProperty.resolveWith(
        (states) =>
            states.contains(WidgetState.focused) &&
                FocusManager.instance.highlightMode ==
                    FocusHighlightMode.traditional
            ? BorderSide(color: color, width: 1.5)
            : BorderSide.none,
      );

  static WidgetStateProperty<MouseCursor?> get cursor =>
      WidgetStateProperty.resolveWith(
        (states) => states.contains(WidgetState.disabled)
            ? SystemMouseCursors.basic
            : SystemMouseCursors.click,
      );

  /// The quiet header button: muted ink on the header glass, a soft fill on
  /// hover/press, [filled] marking an open menu.
  static ButtonStyle quiet(AppPalette p, {bool filled = false}) => ButtonStyle(
    minimumSize: const WidgetStatePropertyAll(Size.square(height)),
    maximumSize: const WidgetStatePropertyAll(Size(double.infinity, height)),
    padding: const WidgetStatePropertyAll(EdgeInsets.zero),
    tapTargetSize: MaterialTapTargetSize.shrinkWrap,
    visualDensity: VisualDensity.standard,
    iconSize: const WidgetStatePropertyAll(icon),
    foregroundColor: WidgetStateProperty.resolveWith(
      (states) => states.contains(WidgetState.disabled)
          ? p.faint
          : filled ||
                states.contains(WidgetState.hovered) ||
                states.contains(WidgetState.focused)
          ? p.ink
          : p.muted,
    ),
    iconColor: WidgetStateProperty.resolveWith(
      (states) => states.contains(WidgetState.disabled)
          ? p.faint
          : filled ||
                states.contains(WidgetState.hovered) ||
                states.contains(WidgetState.focused)
          ? p.ink
          : p.muted,
    ),
    backgroundColor: WidgetStatePropertyAll(
      filled ? p.ink.withValues(alpha: .09) : Colors.transparent,
    ),
    overlayColor: WidgetStateProperty.resolveWith(
      (states) => states.contains(WidgetState.pressed)
          ? p.ink.withValues(alpha: .10)
          : states.contains(WidgetState.hovered) ||
                states.contains(WidgetState.focused)
          ? p.hover
          : Colors.transparent,
    ),
    side: focusRing(p.amber),
    shape: WidgetStatePropertyAll(shape),
    mouseCursor: cursor,
  );

  /// The amber "a newer client exists" button.
  static ButtonStyle notice(AppPalette p) => ButtonStyle(
    minimumSize: const WidgetStatePropertyAll(Size.square(height)),
    maximumSize: const WidgetStatePropertyAll(Size(double.infinity, height)),
    padding: const WidgetStatePropertyAll(EdgeInsets.fromLTRB(10, 0, 13, 0)),
    tapTargetSize: MaterialTapTargetSize.shrinkWrap,
    visualDensity: VisualDensity.standard,
    elevation: const WidgetStatePropertyAll(0),
    textStyle: WidgetStatePropertyAll(
      uiText(size: 13, weight: FontWeight.w600),
    ),
    iconSize: const WidgetStatePropertyAll(18),
    foregroundColor: WidgetStatePropertyAll(p.onAmber),
    iconColor: WidgetStatePropertyAll(p.onAmber),
    backgroundColor: WidgetStatePropertyAll(p.amber),
    overlayColor: WidgetStateProperty.resolveWith(
      (states) => states.contains(WidgetState.pressed)
          ? p.onAmber.withValues(alpha: .14)
          : states.contains(WidgetState.hovered) ||
                states.contains(WidgetState.focused)
          ? p.onAmber.withValues(alpha: .08)
          : Colors.transparent,
    ),
    side: focusRing(p.ink),
    shape: WidgetStatePropertyAll(shape),
    mouseCursor: cursor,
  );

  /// Host updates (the VMs' and PCs' Host program), never this app's: blue
  /// with a server glyph, so it is not taken for the amber app update
  /// (alike, the user cannot tell which one an update is for).
  static ButtonStyle hostNotice(AppPalette p) => notice(p).copyWith(
    foregroundColor: WidgetStatePropertyAll(p.blue),
    iconColor: WidgetStatePropertyAll(p.blue),
    backgroundColor: WidgetStatePropertyAll(p.blue.withValues(alpha: .14)),
    overlayColor: WidgetStateProperty.resolveWith(
      (states) => states.contains(WidgetState.pressed)
          ? p.blue.withValues(alpha: .18)
          : states.contains(WidgetState.hovered) ||
                states.contains(WidgetState.focused)
          ? p.blue.withValues(alpha: .10)
          : Colors.transparent,
    ),
  );
}

/// The launcher's top bar: the brand on the left; on the
/// right only what people need often or must notice — the client update
/// control and the account menu. The log viewer and the theme/scene choice
/// live in the account menu. The row follows the page column, so the brand
/// starts where "내 데스크톱" starts and the account ends where 새로고침 ends.
class WorkspaceHeader extends StatelessWidget {
  const WorkspaceHeader({
    super.key,
    required this.side,
    required this.columnWidth,
    required this.isDemo,
    required this.accountLabel,
    required this.server,
    required this.version,
    required this.update,
    required this.onUpdates,
    required this.onClientInfo,
    required this.onLog,
    required this.onChangePassword,
    this.onManageMfa,
    required this.onSignOut,
    this.signOutReason,
    this.hostUpdates = 0,
    this.onHostUpdates,
    this.sessionAccess,
    this.noticeBell,
  });

  /// 공지 (notice_widgets.dart): the bell with its unread count, left of the
  /// update control. Null: no notices here (the demo, an older server).
  final Widget? noticeBell;

  /// The page's side padding and column width.
  final double side, columnWidth;
  final bool isDemo;
  final String accountLabel, server, version;
  final SessionActivity? sessionAccess;

  /// A newer client version on this server, or null.
  final String? update;

  /// How many assigned desktops' Hosts can be updated now (the header badges
  /// them too, not only each desktop's ⋯).
  final int hostUpdates;

  /// Opens the app (client) update panel (null in the design demo).
  final VoidCallback? onUpdates;

  /// Opens the Host updates (batch update); its own button, shown only
  /// while a Host has one.
  final VoidCallback? onHostUpdates;
  final VoidCallback onClientInfo, onLog;

  /// Null hides 비밀번호 변경 (the demo, or no password service).
  final VoidCallback? onChangePassword;
  final VoidCallback? onManageMfa;

  /// Null greys 로그아웃 out, with [signOutReason] as its tooltip.
  final VoidCallback? onSignOut;
  final String? signOutReason;

  @override
  Widget build(BuildContext context) {
    final p = AppPalette.of(context);
    final width =
        MediaQuery.sizeOf(context).width /
        MediaQuery.textScalerOf(context).scale(1);
    final compact = width < HeaderControl.compactWidth;
    return Container(
      key: const Key('workspace-header'),
      // Translucent over the scene; no backdrop blur.
      color: p.headerGlass,
      constraints: const BoxConstraints(minHeight: 64),
      padding: EdgeInsets.symmetric(horizontal: side, vertical: 10),
      child: Center(
        child: ConstrainedBox(
          constraints: BoxConstraints(maxWidth: columnWidth),
          child: Row(
            children: [
              Expanded(
                child: Align(
                  alignment: Alignment.centerLeft,
                  child: FittedBox(
                    fit: BoxFit.scaleDown,
                    // The version lives in the account menu's first row and
                    // its "클라이언트 정보", not here as well.
                    child: Brand(onDark: p.dark),
                  ),
                ),
              ),
              const SizedBox(width: 16),
              if (hostUpdates > 0) ...[
                _HostUpdateButton(
                  hostUpdates: hostUpdates,
                  compact: compact,
                  onPressed: onHostUpdates,
                ),
                const SizedBox(width: HeaderControl.gap),
              ],
              if (noticeBell != null) ...[
                noticeBell!,
                const SizedBox(width: HeaderControl.gap),
              ],
              _UpdateButton(
                update: update,
                compact: compact,
                onPressed: onUpdates,
              ),
              const SizedBox(width: HeaderControl.gap),
              if (sessionAccess != null && !isDemo) ...[
                SessionIdleBadge(
                  activity: sessionAccess!,
                  compact: true,
                  flat: true,
                ),
                const SizedBox(width: HeaderControl.gap),
              ],
              // 설정: right of the timer, left of the
              // account button.
              IconButton(
                key: const Key('open-app-settings'),
                tooltip: l10n.settings,
                onPressed: () => unawaited(
                  showAppSettings(
                    context,
                    touch: false,
                    sessionAccess: isDemo ? null : sessionAccess,
                    onManageMfa: onManageMfa,
                    onChangePassword: onChangePassword,
                  ),
                ),
                style: HeaderControl.quiet(p),
                icon: const Icon(Icons.settings_outlined),
              ),
              const SizedBox(width: HeaderControl.gap),
              _AccountMenu(
                compact: compact,
                isDemo: isDemo,
                label: accountLabel,
                server: server,
                version: version,
                onClientInfo: onClientInfo,
                onLog: onLog,
                onSignOut: onSignOut,
                signOutReason: signOutReason,
              ),
            ],
          ),
        ),
      ),
    );
  }
}

/// The Host program of assigned VMs/PCs: blue, a server glyph, its own label
/// ("Host 업데이트 N대"). Never shares the amber app-update button.
class _HostUpdateButton extends StatelessWidget {
  const _HostUpdateButton({
    required this.hostUpdates,
    required this.compact,
    required this.onPressed,
  });
  final int hostUpdates;
  final bool compact;
  final VoidCallback? onPressed;

  static const _icon = Icons.dns_outlined;

  @override
  Widget build(BuildContext context) {
    final p = AppPalette.of(context);
    final label = l10n.hostUpdates(hostUpdates);
    if (compact) {
      return IconButton(
        key: const Key('open-host-updates'),
        tooltip: label,
        onPressed: onPressed,
        style: HeaderControl.hostNotice(p),
        icon: const Icon(_icon, size: HeaderControl.icon),
      );
    }
    return FilledButton.icon(
      key: const Key('open-host-updates'),
      onPressed: onPressed,
      style: HeaderControl.hostNotice(p),
      icon: const Icon(_icon),
      label: KeepAllText(label, maxLines: 1),
    );
  }
}

/// The app (this client) only. Quiet like the account button until there is
/// a new app version; then an amber button "새 버전 vX" with a red dot on its
/// corner. It only opens the update panel: downloading and installing wait
/// for the choice there.
class _UpdateButton extends StatelessWidget {
  const _UpdateButton({
    required this.update,
    required this.compact,
    required this.onPressed,
  });
  final String? update;
  final bool compact;
  final VoidCallback? onPressed;

  // One glyph in both states: an upward arrow in a circle, which reads as
  // "update" (the list's 새로고침 keeps the circular-arrow refresh glyph).
  static const _icon = Icons.arrow_circle_up_outlined;

  @override
  Widget build(BuildContext context) {
    final p = AppPalette.of(context);
    final update = this.update;
    final label = update != null ? l10n.newV(update) : null;
    final tip = update != null ? l10n.newAppVersionV(update) : '';
    final Widget button;
    if (label == null) {
      button = IconButton(
        key: const Key('check-client-updates'),
        tooltip: onPressed == null
            ? l10n.updatesCantCheckedDemo
            : l10n.checkAppUpdates,
        onPressed: onPressed,
        style: HeaderControl.quiet(p),
        icon: const Icon(_icon),
      );
    } else if (compact) {
      button = IconButton(
        key: const Key('check-client-updates'),
        tooltip: tip,
        onPressed: onPressed,
        style: HeaderControl.notice(p),
        icon: const Icon(_icon, size: HeaderControl.icon),
      );
    } else {
      button = Tooltip(
        message: tip,
        child: FilledButton.icon(
          key: const Key('check-client-updates'),
          onPressed: onPressed,
          style: HeaderControl.notice(p),
          icon: const Icon(_icon),
          label: KeepAllText(label, maxLines: 1),
        ),
      );
    }
    if (label == null) return button;
    // The dot is ringed in the header colour so it reads as a badge cut into
    // the button's corner (a dot drawn over the icon is hard to see).
    return Stack(
      clipBehavior: Clip.none,
      children: [
        button,
        Positioned(
          top: -3,
          right: -3,
          child: IgnorePointer(
            child: Container(
              key: const Key('client-update-badge'),
              width: 12,
              height: 12,
              decoration: BoxDecoration(
                color: p.red,
                shape: BoxShape.circle,
                border: Border.all(
                  color: p.headerGlass.withValues(alpha: 1),
                  width: 2,
                ),
              ),
            ),
          ),
        ),
      ],
    );
  }
}

/// The signed-in account as a compact button (a monogram, the name and a
/// chevron) and its menu ([AccountMenu]): who and where, 정보·라이선스 and
/// 로그 보기, the theme and scene, 로그아웃, and 완전 종료 (설정 is the ⚙ beside it).
class _AccountMenu extends StatelessWidget {
  const _AccountMenu({
    required this.compact,
    required this.isDemo,
    required this.label,
    required this.server,
    required this.version,
    required this.onClientInfo,
    required this.onLog,
    required this.onSignOut,
    required this.signOutReason,
  });
  final bool compact, isDemo;
  final String label, server, version;
  final VoidCallback onClientInfo, onLog;
  final VoidCallback? onSignOut;
  final String? signOutReason;

  @override
  Widget build(BuildContext context) {
    // Only the client's main window offers 완전 종료 (see AppExitScope).
    final exit = AppExitScope.maybeOf(context);
    return AccountMenu(
      touch: false,
      demo: isDemo,
      account: label,
      serverLine: '$server · $version',
      apps: [
        AccountMenuEntry(
          key: 'open-settings',
          icon: Icons.info_outline_rounded,
          label: l10n.aboutLicenses,
          onPressed: onClientInfo,
        ),
        AccountMenuEntry(
          key: 'open-client-log',
          icon: Icons.receipt_long_outlined,
          label: l10n.viewLogs,
          onPressed: onLog,
        ),
      ],
      accountEntries: [
        AccountMenuEntry(
          key: 'sign-out',
          icon: Icons.logout_rounded,
          label: isDemo ? l10n.closeDemo : l10n.signOut2,
          onPressed: onSignOut,
          disabledReason: signOutReason,
          destructive: true,
        ),
      ],
      // The only way out that does not leave Multitap in the tray: the
      // window's X hides it there.
      exit: exit == null
          ? null
          : AccountMenuEntry(
              key: 'quit-app',
              icon: Icons.power_settings_new_rounded,
              label: l10n.quit,
              tooltip: l10n.quitsMultitapCompletelyInsteadKeeping,
              onPressed: () => unawaited(exit.requestExit()),
              destructive: true,
            ),
      builder: (context, open, toggle, focusNode) =>
          _button(context, open, toggle, focusNode),
    );
  }

  Widget _button(
    BuildContext context,
    bool open,
    VoidCallback toggle,
    FocusNode focusNode,
  ) {
    final p = AppPalette.of(context);
    final nameStyle = uiText(size: 14, weight: FontWeight.w500);
    final button = TextButton(
      key: const Key('workspace-account'),
      focusNode: focusNode,
      onPressed: toggle,
      style: HeaderControl.quiet(p, filled: open).copyWith(
        padding: WidgetStatePropertyAll(
          compact ? EdgeInsets.zero : const EdgeInsets.fromLTRB(5, 0, 8, 0),
        ),
        textStyle: WidgetStatePropertyAll(nameStyle),
      ),
      child: Row(
        mainAxisSize: MainAxisSize.min,
        children: [
          AccountMonogram(label: label, demo: isDemo, size: 28, neutral: true),
          if (!compact) ...[
            const SizedBox(width: 8),
            ConstrainedBox(
              constraints: const BoxConstraints(maxWidth: _nameWidth),
              child: KeepAllText(
                label,
                maxLines: 1,
                overflow: TextOverflow.ellipsis,
                style: TextStyle(color: p.ink),
              ),
            ),
            const SizedBox(width: 4),
            AnimatedRotation(
              turns: open ? .5 : 0,
              duration: const Duration(milliseconds: 150),
              child: const Icon(Icons.expand_more_rounded, size: 18),
            ),
          ],
        ],
      ),
    );
    // The name as a tooltip only where it is not fully visible: reduced to
    // the monogram, or cut short.
    final name = TextPainter(
      text: TextSpan(text: keepAll(label), style: nameStyle),
      textScaler: MediaQuery.textScalerOf(context),
      textDirection: TextDirection.ltr,
      maxLines: 1,
    )..layout(maxWidth: _nameWidth);
    final hidden = compact || name.didExceedMaxLines;
    name.dispose();
    return hidden ? Tooltip(message: label, child: button) : button;
  }

  static const _nameWidth = 140.0;
}
