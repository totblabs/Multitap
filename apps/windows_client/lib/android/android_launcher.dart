import 'dart:async';
import 'dart:math' as math;

import 'package:flutter/material.dart';
import 'package:flutter/services.dart';

import '../data/app_licenses.dart' show androidLicenseManifestAsset;
import '../data/appearance_settings.dart';
import '../data/client_build_version.dart';
import '../data/login_memory.dart' show passwordMemoryAllowed;
import '../ui/app_dialog.dart';
import '../ui/app_form.dart';
import '../ui/app_info_dialog.dart';
import '../data/session_activity.dart';
import '../ui/session_access_controls.dart';
import '../ui/account_menu.dart';
import '../ui/app_theme.dart';
import '../ui/appearance_switch.dart' show applyLanguageChoice, languageChoiceLabel;
import '../ui/app_toast.dart';
import '../ui/components.dart';
import '../ui/korean_text.dart' show KeepAllText;
import '../ui/workspace_header.dart' show HeaderControl;
import 'android_tv.dart';
import '../l10n/l10n.dart';

/// Glass tones over the night (dark) or day (light) scene, as on Windows.
class AndroidGlass {
  const AndroidGlass(this.dark);
  final bool dark;
  Color get fill => dark ? const Color(0x8F0E1216) : const Color(0xA3FFFFFF);
  Color get edge => dark ? const Color(0x1FFFFFFF) : const Color(0xC7FFFFFF);
  Color get field => dark ? const Color(0x12FFFFFF) : const Color(0xB8FFFFFF);
  Color get ink => dark ? const Color(0xFFF2F5F7) : const Color(0xFF0F1C26);
  Color get muted => dark ? const Color(0xFFAEB9C1) : const Color(0xFF3A4C59);
}

/// The shared theme by finger: a switch and a segmented choice (HTTPS ·
/// HTTP) take whole 48 dp targets (the desktop theme makes them compact).
/// The segmented choice keeps the shared look (a grey groove, the chosen
/// segment raised, no amber frame): only its size changes here.
///
/// A plain SnackBar spans the screen less its 16 dp gutters: the desktop
/// theme's fixed toast width is wider than a phone. showAppToast fits both.
ThemeData androidTheme(ThemeData base) => base.copyWith(
  switchTheme: base.switchTheme.copyWith(
    materialTapTargetSize: MaterialTapTargetSize.padded,
  ),
  segmentedButtonTheme: SegmentedButtonThemeData(
    style: base.segmentedButtonTheme.style?.copyWith(
      minimumSize: const WidgetStatePropertyAll(Size(0, 48)),
      visualDensity: VisualDensity.standard,
    ),
  ),
  snackBarTheme: SnackBarThemeData(
    behavior: base.snackBarTheme.behavior,
    backgroundColor: base.snackBarTheme.backgroundColor,
    contentTextStyle: base.snackBarTheme.contentTextStyle,
    actionTextColor: base.snackBarTheme.actionTextColor,
    elevation: base.snackBarTheme.elevation,
    insetPadding: base.snackBarTheme.insetPadding,
    shape: base.snackBarTheme.shape,
  ),
);

/// Status bar and navigation bar icons readable over the scene: light icons
/// on the night scene, dark ones on the day scene; the bars stay clear.
SystemUiOverlayStyle androidSystemBars(bool dark) =>
    (dark ? SystemUiOverlayStyle.light : SystemUiOverlayStyle.dark).copyWith(
      statusBarColor: Colors.transparent,
      systemNavigationBarColor: Colors.transparent,
      systemNavigationBarDividerColor: Colors.transparent,
      systemNavigationBarContrastEnforced: false,
    );

/// One touch-sized (48 dp) menu row: a meaningful coloured icon and a short
/// label, with an optional reason or mark.
Widget androidMenuRow(
  AppPalette p, {
  required IconData icon,
  required Color color,
  required String label,
  bool enabled = true,
  String? reason,
  Key? reasonKey,
  Widget? trailing,
  bool danger = false,
}) => Row(
  children: [
    Icon(icon, size: 20, color: enabled ? color : p.disabled),
    const SizedBox(width: 14),
    Expanded(
      child: Column(
        crossAxisAlignment: CrossAxisAlignment.start,
        mainAxisSize: MainAxisSize.min,
        children: [
          KeepAllText(
            label,
            style: uiText(
              size: 14,
              weight: FontWeight.w500,
              color: !enabled
                  ? p.disabled
                  : danger
                  ? p.red
                  : p.ink,
            ),
          ),
          if (!enabled && reason != null)
            Padding(
              padding: const EdgeInsets.only(top: 2),
              child: KeepAllText(
                reason,
                key: reasonKey,
                style: uiText(size: 12, color: p.muted, height: 1.35),
              ),
            ),
        ],
      ),
    ),
    if (trailing != null) ...[const SizedBox(width: 12), trailing],
  ],
);

/// Dark/light and the scene, in 48 dp rows (the shared menu uses the
/// desktop's 34 px rows). Remembered on this device only.
class AndroidAppearanceMenu extends StatelessWidget {
  const AndroidAppearanceMenu({super.key, this.iconColor});
  final Color? iconColor;

  @override
  Widget build(BuildContext context) {
    final appearance = AppearanceScope.maybeOf(context);
    if (appearance == null) return const SizedBox.shrink();
    final p = AppPalette.of(context);
    final check = Icon(Icons.check_rounded, size: 18, color: p.green);
    return AndroidTvFocus(
      builder: (context, focused) => PopupMenuButton<Object>(
        key: const Key('appearance-menu'),
        style: AndroidTvFocus.style(context, const ButtonStyle()),
        tooltip: l10n.themeBackground,
        enabled: !appearance.saving,
        color: p.surface,
        icon: Icon(
          appearance.mode == ThemeMode.dark
              ? Icons.dark_mode_outlined
              : Icons.light_mode_outlined,
          color: focused ? AndroidTvFocus.ink(context) : iconColor ?? p.amber,
          size: 22,
        ),
        onSelected: (value) =>
            unawaited(androidApplyAppearance(context, appearance, value)),
        itemBuilder: (_) => androidAppearanceItems(p, appearance, check),
      ),
    );
  }
}

/// Applies a theme or scene choice; a save failure is said, never silent.
Future<void> androidApplyAppearance(
  BuildContext context,
  AppAppearance appearance,
  Object value,
) async {
  if (value is AppLanguage) return applyLanguageChoice(context, value);
  final what = value is ThemeMode ? l10n.theme : l10n.background;
  try {
    if (value is ThemeMode) {
      await appearance.select(value);
    } else {
      await appearance.selectScene(value as BackgroundScene);
    }
  } catch (_) {
    if (context.mounted) {
      showAppToast(
        context,
        l10n.couldntSaveChooseAgain(what),
        kind: AppToastKind.failed,
      );
    }
  }
}

/// The theme and scene rows (48 dp) of the sign-in screen's appearance
/// button (signed in, the account menu holds them as two choice rows).
List<PopupMenuEntry<Object>> androidAppearanceItems(
  AppPalette p,
  AppAppearance appearance,
  Widget check,
) => [
  for (final mode in [ThemeMode.dark, ThemeMode.light])
    PopupMenuItem<Object>(
      key: Key('theme-${mode.name}'),
      value: mode,
      child: androidMenuRow(
        p,
        icon: mode == ThemeMode.dark
            ? Icons.dark_mode_rounded
            : Icons.light_mode_rounded,
        color: p.amber,
        // The scene is its own choice below ("밤 풍경/낮 풍경" beside the
        // theme makes no sense).
        label: mode == ThemeMode.dark ? l10n.dark : l10n.light,
        trailing: appearance.mode == mode ? check : null,
      ),
    ),
  const PopupMenuDivider(height: 9),
  for (final scene in BackgroundScene.values)
    PopupMenuItem<Object>(
      key: Key('scene-${scene.name}'),
      value: scene,
      child: androidMenuRow(
        p,
        icon: switch (scene) {
          BackgroundScene.peaks => Icons.terrain_rounded,
          BackgroundScene.meadow => Icons.landscape_rounded,
        },
        color: p.blue,
        label: l10n.background2(scene.label),
        trailing: appearance.scene == scene ? check : null,
      ),
    ),
  const PopupMenuDivider(height: 9),
  for (final language in AppLanguage.values)
    PopupMenuItem<Object>(
      key: Key('language-${language.name}'),
      value: language,
      child: androidMenuRow(
        p,
        icon: Icons.translate_rounded,
        color: p.muted,
        label: language == AppLanguage.auto
            ? '${l10n.language} · ${languageChoiceLabel(language)}'
            : languageChoiceLabel(language),
        trailing: AppLanguageController.instance.choice == language
            ? check
            : null,
      ),
    ),
];

/// The launcher's top bar, as the Windows client's WorkspaceHeader: the brand
/// on the left, the update control (quiet until there is something to
/// install, then an amber "새 버전" for this app or "Host 업데이트 N대" for
/// assigned Hosts, with a red badge) and the account button whose menu is
/// the Windows one ([AccountMenu]) by finger. The row follows the page
/// column; touch targets are 48 dp.
class AndroidWorkspaceHeader extends StatelessWidget {
  const AndroidWorkspaceHeader({
    super.key,
    required this.side,
    required this.columnWidth,
    required this.account,
    required this.server,
    required this.version,
    required this.update,
    required this.onUpdates,
    required this.onChangePassword,
    this.onManageMfa,
    required this.onSignOut,
    this.onLogs,
    this.onExit,
    this.middle,
    this.hostUpdates = 0,
    this.sessionAccess,
    this.noticeBell,
    this.noticeBellWidth = 0,
  });

  /// 공지 (android_notices.dart): the bell, left of the update button, and the
  /// width it needs. Null: no notices here (an older server).
  final Widget? noticeBell;
  final double noticeBellWidth;
  final double side, columnWidth;
  final SessionActivity? sessionAccess;
  final String account, server;

  /// How many assigned desktops' Hosts can be updated now (2026-11-03: the
  /// header badges them too, not only each desktop's ⋯).
  final int hostUpdates;

  /// A short landscape phone (backlog 2026-11-02: the header and filters took
  /// half the height): the launcher's filters and tools in this one row,
  /// which is then 52 dp tall with icon-only update and account buttons.
  final Widget? middle;

  /// The running build (null when it could not be read).
  final String? version;

  /// A newer build on this server, or null.
  final String? update;
  final VoidCallback onUpdates, onChangePassword, onSignOut;
  final VoidCallback? onManageMfa;
  final VoidCallback? onLogs, onExit;

  static ButtonStyle _touch(ButtonStyle style) => style.copyWith(
    minimumSize: const WidgetStatePropertyAll(Size.square(48)),
    maximumSize: const WidgetStatePropertyAll(
      Size(double.infinity, double.infinity),
    ),
  );

  @override
  Widget build(BuildContext context) {
    final p = AppPalette.of(context);
    final middle = this.middle;
    final supportedIdle = sessionAccess?.idleSupported == true;
    final update = this.update;
    final hosts = hostUpdates > 0 ? l10n.hostUpdates(hostUpdates) : null;
    final notice = update != null || hosts != null;
    final tip = [if (update != null) l10n.newVersion(update), ?hosts].join(' · ');
    final updateButton = AndroidTvFocus(
      builder: (context, focused) => IconButton(
        key: const Key('android-header-update-action'),
        tooltip: notice ? tip : l10n.checkUpdates,
        autofocus: AndroidTvScope.of(context),
        onPressed: onUpdates,
        style: AndroidTvFocus.style(
          context,
          _touch(HeaderControl.quiet(p)).copyWith(
            foregroundColor: WidgetStatePropertyAll(
              focused
                  ? AndroidTvFocus.ink(context)
                  : notice
                  ? p.amberInk
                  : p.muted,
            ),
          ),
        ),
        icon: const Icon(Icons.arrow_circle_up_outlined, size: 22),
      ),
    );
    // 설정: its own ⚙ button left of the account button,
    // as on Windows; not a row of the account menu.
    final settingsButton = AndroidTvFocus(
      builder: (context, focused) => IconButton(
        key: const Key('open-app-settings'),
        tooltip: l10n.settings,
        onPressed: () => unawaited(
          showAppSettings(
            context,
            touch: true,
            sessionAccess: sessionAccess,
            onManageMfa: onManageMfa,
            onChangePassword: onChangePassword,
            focusStyle: AndroidTvFocus.style,
          ),
        ),
        style: AndroidTvFocus.style(
          context,
          _touch(HeaderControl.quiet(p)).copyWith(
            foregroundColor: WidgetStatePropertyAll(
              focused ? AndroidTvFocus.ink(context) : p.muted,
            ),
          ),
        ),
        icon: const Icon(Icons.settings_outlined, size: 22),
      ),
    );
    final accountMenu = AccountMenu(
      touch: true,
      account: account,
      serverLine: version == null ? server : '$server · $version',
      focusStyle: AndroidTvFocus.style,
      // The Windows menu's entries: 정보·라이선스 opens the same
      // information window (app, server, open-source licenses) with the
      // Android notices; its 업데이트 확인 is the header's update dialog.
      apps: [
        AccountMenuEntry(
          key: 'android-update-menu',
          icon: Icons.info_outline_rounded,
          label: l10n.aboutLicenses,
          onPressed: () => unawaited(
            showAppInfoDialog(
              context,
              platform: l10n.androidClient,
              serverOrigin: server,
              account: account,
              manifestAsset: androidLicenseManifestAsset,
              loadVersion: () async => ClientBuildVersion.parse(version),
              onCheckUpdates: onUpdates,
            ),
          ),
        ),
        if (onLogs case final onLogs?)
          AccountMenuEntry(
            key: 'android-log-menu',
            icon: Icons.receipt_long_outlined,
            label: l10n.viewLogs,
            onPressed: onLogs,
          ),
      ],
      accountEntries: [
        AccountMenuEntry(
          key: 'sign-out',
          icon: Icons.logout_rounded,
          label: l10n.signOut2,
          onPressed: onSignOut,
          destructive: true,
        ),
      ],
      exit: switch (onExit) {
        final onExit? => AccountMenuEntry(
          key: 'android-exit-menu',
          icon: Icons.power_settings_new_rounded,
          label: l10n.quit,
          onPressed: onExit,
          destructive: true,
        ),
        null => null,
      },
      builder: (context, open, toggle, focusNode) => AndroidTvFocus(
        builder: (context, focused) => Tooltip(
          message: '$account · $server',
          child: TextButton(
            key: const Key('account-menu'),
            focusNode: focusNode,
            onPressed: toggle,
            style: AndroidTvFocus.style(
              context,
              _touch(HeaderControl.quiet(p, filled: open)).copyWith(
                padding: const WidgetStatePropertyAll(EdgeInsets.zero),
              ),
            ),
            child: Semantics(
              label: l10n.accountMenu(account),
              child: ExcludeSemantics(
                child: Container(
                  width: 28,
                  height: 28,
                  decoration: BoxDecoration(
                    color: open ? p.amberSoft : p.quiet,
                    shape: BoxShape.circle,
                  ),
                  child: Icon(Icons.person_outline_rounded, size: 20,
                    color: open ? p.amberInk : p.muted),
                ),
              ),
            ),
          ),
        ),
      ),
    );
    // The wordmark: one weight, one colour (Brand in ui/components.dart).
    final wordStyle = uiText(
      size: 20,
      weight: FontWeight.w700,
      color: p.ink,
    ).copyWith(letterSpacing: -.8);
    final timerStyle = uiText(
      size: 12,
      weight: FontWeight.w600,
    ).copyWith(fontFeatures: const [FontFeature.tabularFigures()]);
    double textWidth(String value, TextStyle style) {
      final painter = TextPainter(
        text: TextSpan(
          text: value,
          style: DefaultTextStyle.of(context).style.merge(style),
        ),
        textDirection: Directionality.of(context),
        textScaler: MediaQuery.textScalerOf(context),
      )..layout();
      try {
        return painter.width.ceilToDouble();
      } finally {
        painter.dispose();
      }
    }

    // Reserve the policy's full countdown width: ticking seconds and an idle
    // switch acknowledgement never move the surrounding header actions.
    final timerMinutes = math.max(
      30,
      sessionAccess?.policy?.idleTimeout.inMinutes ?? 30,
    );
    final timerWidth =
        math.max(
          textWidth('$timerMinutes:00', timerStyle),
          textWidth(l10n.off, timerStyle),
        ) +
        32;
    final actionsWidth =
        152.0 +
        (supportedIdle ? timerWidth + 4 : 0) +
        (noticeBell == null ? 0 : noticeBellWidth);
    final wordmarkWidth = 36 + textWidth('Multitap', wordStyle);

    return Container(
      key: const Key('workspace-header'),
      color: p.headerGlass,
      constraints: const BoxConstraints(minHeight: 56),
      padding: EdgeInsets.symmetric(
        horizontal: side,
        vertical: middle == null ? 4 : 2,
      ),
      child: Center(
        child: ConstrainedBox(
          constraints: BoxConstraints(maxWidth: columnWidth),
          child: LayoutBuilder(
            builder: (context, constraints) {
              final showWordmark =
                  middle == null &&
                  constraints.maxWidth >= wordmarkWidth + actionsWidth + 12;
              return Row(
                children: [
                  if (middle == null) Semantics(
                    label: 'Multitap',
                    child: ExcludeSemantics(
                      child: Row(
                        mainAxisSize: MainAxisSize.min,
                        children: [
                          const ProductMark(size: 28),
                          if (showWordmark) ...[
                            const SizedBox(width: 8),
                            Text(
                              'Multitap',
                              key: const Key('android-header-wordmark'),
                              style: wordStyle,
                              maxLines: 1,
                              softWrap: false,
                            ),
                          ],
                        ],
                      ),
                    ),
                  ),
                  if (middle != null) ...[
                    const SizedBox(width: 4),
                    Expanded(child: middle),
                    const SizedBox(width: 4),
                  ] else
                    const Spacer(),
                  ?noticeBell,
                  Stack(
                    clipBehavior: Clip.none,
                    children: [
                      KeyedSubtree(
                        key: const Key('android-update-check'),
                        child: updateButton,
                      ),
                      if (notice)
                        Positioned(
                          top: 8,
                          right: 8,
                          child: IgnorePointer(
                            child: Container(
                              key: Key(
                                update != null
                                    ? 'client-update-badge'
                                    : 'host-update-badge',
                              ),
                              width: 8,
                              height: 8,
                              decoration: BoxDecoration(
                                color: p.amberInk,
                                shape: BoxShape.circle,
                                border: Border.all(
                                  color: p.headerGlass.withValues(alpha: 1),
                                  width: 1.5,
                                ),
                              ),
                            ),
                          ),
                        ),
                    ],
                  ),
                  if (supportedIdle) ...[
                    const SizedBox(width: 4),
                    Semantics(
                      label: l10n.idleAutoSignOut,
                      child: SizedBox(
                        key: const Key('android-header-countdown-slot'),
                        width: timerWidth,
                        height: 48,
                        child: Center(
                        child: SessionIdleBadge(
                            activity: sessionAccess!,
                          compact: true,
                          flat: true,
                          ),
                        ),
                      ),
                    ),
                  ],
                  const SizedBox(width: 4),
                  settingsButton,
                  const SizedBox(width: 4),
                  accountMenu,
                ],
              );
            },
          ),
        ),
      ),
    );
  }
}

/// The sign-in screen: the same scene, wordmark, server chip and amber
/// button as the Windows client, sized for a phone. A short landscape screen
/// puts the wordmark beside the form so the button stays reachable.
class AndroidSignInView extends StatelessWidget {
  const AndroidSignInView({
    super.key,
    required this.origin,
    required this.id,
    required this.password,
    required this.remember,
    required this.busy,
    required this.version,
    required this.onServer,
    required this.onIdChanged,
    required this.onRemember,
    required this.onLogin,
    this.onUpdates,
    this.updateVersion,
    this.error,
    this.errorIsDone = false,
    this.signingOut = false,
  });
  final Uri? origin;
  final TextEditingController id, password;
  final bool remember, busy;

  /// [busy] because a sign-out is being sent: the button waits as it is,
  /// without the spinner and "로그인 중…" of a sign-in.
  final bool signingOut;
  final String? version, error;

  /// [error] reports a finished step (password changed), not a failure.
  final bool errorIsDone;
  final VoidCallback onServer, onLogin;
  final VoidCallback? onUpdates;
  final String? updateVersion;
  final ValueChanged<String> onIdChanged;
  final ValueChanged<bool> onRemember;

  Widget _card(BuildContext context, AppPalette p, AndroidGlass glass) {
    final configured = origin != null;
    // HTTP server: 비밀번호 기억 is off and disabled, with one plain line.
    final passwordMemory = !configured || passwordMemoryAllowed(origin);
    final television = AndroidTvScope.of(context);
    // The same card as the Windows sign-in (2026-11-10 audit): the server as
    // one line at its head, two fields with their name inside, the switch,
    // the button. Only the heights are a finger's (48 dp).
    InputDecoration field(String hint, IconData icon) =>
        AppField.decoration(
          context,
          hint: hint,
          prefixIcon: Icon(icon, size: 20),
        ).copyWith(
          // A television is read from across the room: a stronger well.
          fillColor: television
              ? (p.dark ? const Color(0xFF222830) : const Color(0xFFF2F4F6))
              : null,
        );
    final host = origin == null
        ? l10n.serverSetupNeeded
        : '${origin!.host}${origin!.hasPort ? ':${origin!.port}' : ''}';
    return Container(
      key: const Key('login-card'),
      padding: television
          ? const EdgeInsets.fromLTRB(24, 16, 24, 24)
          : const EdgeInsets.fromLTRB(16, 8, 16, 16),
      decoration: BoxDecoration(
        color: p.tile,
        borderRadius: BorderRadius.circular(18),
        border: Border.all(color: p.tileLine),
      ),
      child: Column(
        mainAxisSize: MainAxisSize.min,
        crossAxisAlignment: CrossAxisAlignment.stretch,
        children: [
          AndroidTvFocus(
            builder: (context, focused) => Material(
              type: MaterialType.transparency,
              child: InkWell(
                key: const Key('open-server-settings'),
                autofocus: television,
                onTap: busy ? null : onServer,
                focusColor: television ? Colors.transparent : null,
                borderRadius: BorderRadius.circular(12),
                child: Ink(
                  decoration: BoxDecoration(
                    color: focused
                        ? AndroidTvFocus.surface(context)
                        : Colors.transparent,
                    borderRadius: BorderRadius.circular(12),
                  ),
                  child: ConstrainedBox(
                    constraints: const BoxConstraints(minHeight: 48),
                    child: Padding(
                      padding: const EdgeInsets.symmetric(horizontal: 4),
                      child: Row(
                        children: [
                          Container(
                            width: 8,
                            height: 8,
                            decoration: BoxDecoration(
                              shape: BoxShape.circle,
                              color: configured ? p.green : p.warn,
                            ),
                          ),
                          const SizedBox(width: 8),
                          Expanded(
                            child: Text(
                              host,
                              key: const Key('login-server-label'),
                              maxLines: 1,
                              overflow: TextOverflow.ellipsis,
                              style: uiText(
                                size: 14,
                                weight: FontWeight.w600,
                                color: focused
                                    ? AndroidTvFocus.ink(context)
                                    : p.ink,
                              ),
                            ),
                          ),
                          const SizedBox(width: 8),
                          // Without a server the line and the button
                          // already say what to do.
                          if (configured)
                            KeepAllText(
                              l10n.changeServer,
                              key: const Key('login-server-change'),
                              style: uiText(
                                size: 12,
                                weight: FontWeight.w500,
                                color: focused
                                    ? AndroidTvFocus.ink(context)
                                    : p.ink2,
                              ),
                            ),
                          const SizedBox(width: 2),
                          Icon(
                            Icons.chevron_right_rounded,
                            size: 18,
                            color: focused
                                ? AndroidTvFocus.ink(context)
                                : p.muted,
                          ),
                        ],
                      ),
                    ),
                  ),
                ),
              ),
            ),
          ),
          const SizedBox(height: 8),
          AndroidTvTextEntry(
            controller: id,
            label: l10n.id,
            enabled: !busy && configured,
            maxLength: 64,
            action: TextInputAction.next,
            onChanged: onIdChanged,
            builder: (focusNode, readOnly, edit) => TextField(
              key: const Key('sign-in-id'),
              focusNode: focusNode,
              readOnly: readOnly,
              onTap: edit,
              controller: id,
              enabled: !busy && configured,
              maxLength: 64,
              autocorrect: false,
              enableSuggestions: false,
              textInputAction: TextInputAction.next,
              onChanged: onIdChanged,
              style: uiText(color: p.ink, size: 15),
              decoration: field(l10n.id, Icons.person_outline_rounded),
            ),
          ),
          const SizedBox(height: 10),
          AndroidTvTextEntry(
            controller: password,
            label: l10n.password,
            enabled: !busy && configured,
            maxLength: 256,
            obscureText: true,
            onSubmitted: (_) => onLogin(),
            builder: (focusNode, readOnly, edit) => TextField(
              key: const Key('sign-in-password'),
              focusNode: focusNode,
              readOnly: readOnly,
              onTap: edit,
              controller: password,
              enabled: !busy && configured,
              maxLength: 256,
              obscureText: true,
              autocorrect: false,
              enableSuggestions: false,
              keyboardType: TextInputType.visiblePassword,
              textInputAction: TextInputAction.done,
              onSubmitted: (_) => onLogin(),
              style: uiText(color: p.ink, size: 15),
              decoration: field(l10n.password, Icons.key_outlined),
            ),
          ),
          const SizedBox(height: 4),
          AndroidTvFocus(
            builder: (context, focused) => Material(
              type: MaterialType.transparency,
              child: InkWell(
                borderRadius: BorderRadius.circular(12),
                onTap: busy || !configured || !passwordMemory
                    ? null
                    : () => onRemember(!remember),
                focusColor: television ? Colors.transparent : null,
                child: Ink(
                  decoration: BoxDecoration(
                    color: focused
                        ? AndroidTvFocus.surface(context)
                        : Colors.transparent,
                    borderRadius: BorderRadius.circular(12),
                  ),
                  child: ConstrainedBox(
                    constraints: const BoxConstraints(minHeight: 52),
                    child: Padding(
                      padding: const EdgeInsets.fromLTRB(4, 6, 0, 6),
                      child: Column(
                        crossAxisAlignment: CrossAxisAlignment.stretch,
                        children: [
                          Row(
                            children: [
                              Expanded(
                                child: KeepAllText(
                                  l10n.rememberPassword,
                                  style: uiText(
                                    size: 14,
                                    weight: FontWeight.w500,
                                    color: focused
                                        ? AndroidTvFocus.ink(context)
                                        : glass.ink,
                                  ),
                                ),
                              ),
                              const SizedBox(width: 8),
                              ExcludeFocus(
                                excluding: television,
                                child: Switch(
                                  key: const Key('remember-password'),
                                  value: remember && passwordMemory,
                                  onChanged:
                                      busy || !configured || !passwordMemory
                                      ? null
                                      : onRemember,
                                ),
                              ),
                            ],
                          ),
                          if (!passwordMemory)
                            KeepAllText(
                              l10n.passwordsRememberedOnlyHttps,
                              key: const Key('remember-password-https-only'),
                              style: uiText(
                                size: 12,
                                color: glass.muted,
                                height: 1.4,
                              ),
                            )
                          else if (!television)
                            KeepAllText(
                              l10n.passwordEncryptedKeptDevice,
                              style: uiText(
                                size: 12,
                                color: glass.muted,
                                height: 1.4,
                              ),
                            ),
                        ],
                      ),
                    ),
                  ),
                ),
              ),
            ),
          ),
          // What went wrong (or what is done), right above the button: an
          // icon in its colour and an ordinary sentence, as on Windows.
          if (error != null)
            Padding(
              padding: const EdgeInsets.fromLTRB(4, 6, 4, 0),
              child: AppStatusLine(
                error!,
                textKey: const Key('login-error'),
                tone: errorIsDone ? AppTone.success : AppTone.danger,
              ),
            ),
          const SizedBox(height: 12),
          AndroidTvFocus(
            builder: (context, _) => FilledButton(
              key: const Key('login-submit'),
              onPressed: busy ? null : (configured ? onLogin : onServer),
              style: AndroidTvFocus.style(
                context,
                AppButtons.filled(p).copyWith(
                  minimumSize: const WidgetStatePropertyAll(Size(0, 48)),
                  textStyle: WidgetStatePropertyAll(
                    uiText(size: 15, weight: FontWeight.w600),
                  ),
                ),
              ),
              child: busy && !signingOut
                  ? Row(
                      mainAxisSize: MainAxisSize.min,
                      children: [
                        SizedBox.square(
                          dimension: 18,
                          child: CircularProgressIndicator(
                            strokeWidth: 2.2,
                            color: p.disabled,
                          ),
                        ),
                        const SizedBox(width: 10),
                        Text(l10n.signing),
                      ],
                    )
                  : KeepAllText(configured ? l10n.sign : l10n.serverSettings),
            ),
          ),
          const SizedBox(height: 12),
          KeepAllText(
            l10n.askAdministratorAccount,
            textAlign: TextAlign.center,
            style: uiText(size: 12, color: p.muted),
          ),
        ],
      ),
    );
  }

  // The version on a glass panel so it stays readable over the bright day
  // scene too.
  Widget _footer(AppPalette p, AndroidGlass glass, {bool television = false}) =>
      DecoratedBox(
        decoration: BoxDecoration(
          color: television ? Colors.transparent : glass.fill,
          borderRadius: television ? null : BorderRadius.circular(10),
        ),
        child: Padding(
          padding: television
              ? EdgeInsets.zero
              : const EdgeInsets.symmetric(horizontal: 12),
          child: Wrap(
            key: const Key('login-footer'),
            alignment: WrapAlignment.center,
            // The texts level with the 48 dp update button.
            crossAxisAlignment: WrapCrossAlignment.center,
            spacing: 10,
            runSpacing: 4,
            children: [
              KeepAllText(
                l10n.androidStreaming,
                style: uiText(size: 12, color: glass.muted),
              ),
              if (version != null)
                Text(
                  version!,
                  key: const Key('login-version'),
                  style: uiText(
                    size: 12,
                    weight: FontWeight.w600,
                    color: glass.ink,
                  ),
                ),
              if (onUpdates != null)
                AndroidTvFocus(
                  builder: (context, _) => TextButton.icon(
                    key: const Key('login-update-button'),
                    onPressed: origin != null && !busy ? onUpdates : null,
                    style: AndroidTvFocus.style(
                      context,
                      TextButton.styleFrom(
                        foregroundColor: updateVersion == null
                            ? glass.ink
                            : p.amberInk,
                        minimumSize: const Size(0, 48),
                        padding: const EdgeInsets.symmetric(horizontal: 10),
                      ),
                    ),
                    icon: Badge(
                      isLabelVisible: updateVersion != null,
                      child: const Icon(Icons.system_update_outlined, size: 18),
                    ),
                    label: Text(
                      updateVersion == null ? l10n.checkUpdates : l10n.newVersion2('$updateVersion'),
                    ),
                  ),
                ),
            ],
          ),
        ),
      );

  @override
  Widget build(BuildContext context) {
    final p = AppPalette.of(context);
    final glass = AndroidGlass(p.dark);
    final television = AndroidTvScope.of(context);
    return AnnotatedRegion<SystemUiOverlayStyle>(
      value: androidSystemBars(p.dark),
      child: SceneBackground(
        child: SafeArea(
          child: Stack(
            fit: StackFit.expand,
            children: [
              LayoutBuilder(
                builder: (context, constraints) {
                  final scale = MediaQuery.textScalerOf(context).scale(1);
                  // The layout follows the screen, not the keyboard: on a
                  // Fold's inner screen (≈932×704 dp) the open keyboard made
                  // the form "short landscape", rebuilt the fields, dropped
                  // their focus and closed the keyboard again (2026-10-26).
                  // (The Scaffold removes the inset from the body's
                  // MediaQuery, so it is read from the view.)
                  final view = View.of(context);
                  final height =
                      constraints.maxHeight +
                      view.viewInsets.bottom / view.devicePixelRatio;
                  final wide =
                      constraints.maxWidth >= 640 &&
                      constraints.maxWidth > height &&
                      height < 600;
                  final brand = FittedBox(
                    fit: BoxFit.scaleDown,
                    child: Brand(
                      onDark: p.dark,
                      scale: television ? 1.9 : 1.2,
                    ),
                  );
                  if (television &&
                      constraints.maxWidth >= 640 &&
                      constraints.maxWidth > height) {
                    // TV keeps a calm two-column composition at 1080p too.
                    // Scroll the whole group on a short TV or with large text.
                    final edge = constraints.maxWidth < 800 ? 24.0 : 48.0;
                    final gap = constraints.maxWidth < 800 ? 24.0 : 80.0;
                    final width = math.min(
                      1100.0,
                      constraints.maxWidth - edge * 2,
                    );
                    final formWidth = (width * .48).clamp(
                      width < 700 ? 320.0 : 420.0,
                      480.0,
                    );
                    return SingleChildScrollView(
                      key: const Key('login-scroll'),
                      padding: EdgeInsets.all(edge),
                      child: ConstrainedBox(
                        constraints: BoxConstraints(
                          minHeight: math.max(
                            0,
                            constraints.maxHeight - edge * 2,
                          ),
                        ),
                        child: Center(
                          child: ConstrainedBox(
                            constraints: const BoxConstraints(maxWidth: 1100),
                            child: Row(
                              key: const Key('tv-login-columns'),
                              children: [
                                Expanded(
                                  child: Column(
                                    mainAxisSize: MainAxisSize.min,
                                    children: [
                                      brand,
                                      const SizedBox(height: 28),
                                      _footer(p, glass, television: true),
                                    ],
                                  ),
                                ),
                                SizedBox(width: gap),
                                SizedBox(
                                  width: formWidth,
                                  child: _card(context, p, glass),
                                ),
                              ],
                            ),
                          ),
                        ),
                      ),
                    );
                  }
                  if (wide) {
                    // Short landscape: wordmark and notices beside the form.
                    return Row(
                      children: [
                        Expanded(
                          child: SingleChildScrollView(
                            padding: const EdgeInsets.fromLTRB(24, 24, 12, 24),
                            child: ConstrainedBox(
                              constraints: BoxConstraints(
                                minHeight: constraints.maxHeight - 48,
                              ),
                              child: Center(
                                child: Column(
                                  mainAxisSize: MainAxisSize.min,
                                  children: [
                                    brand,
                                    const SizedBox(height: 20),
                                    _footer(p, glass, television: television),
                                  ],
                                ),
                              ),
                            ),
                          ),
                        ),
                        SizedBox(
                          width: (constraints.maxWidth * .48).clamp(340, 440),
                          child: SingleChildScrollView(
                            key: const Key('login-scroll'),
                            padding: const EdgeInsets.fromLTRB(8, 16, 24, 16),
                            child: ConstrainedBox(
                              constraints: BoxConstraints(
                                minHeight: constraints.maxHeight - 32,
                              ),
                              child: Center(child: _card(context, p, glass)),
                            ),
                          ),
                        ),
                      ],
                    );
                  }
                  final tall = height / scale > 700;
                  return SingleChildScrollView(
                    key: const Key('login-scroll'),
                    padding: const EdgeInsets.symmetric(horizontal: 16),
                    child: ConstrainedBox(
                      constraints: BoxConstraints(
                        minHeight: constraints.maxHeight,
                      ),
                      child: Center(
                        child: ConstrainedBox(
                          constraints: const BoxConstraints(maxWidth: 420),
                          child: Column(
                            mainAxisSize: MainAxisSize.min,
                            children: [
                              SizedBox(height: tall ? 32 : 24),
                              brand,
                              const SizedBox(height: 16),
                              _card(context, p, glass),
                              const SizedBox(height: 12),
                              _footer(p, glass, television: television),
                              const SizedBox(height: 24),
                            ],
                          ),
                        ),
                      ),
                    ),
                  );
                },
              ),
              Positioned(
                top: television ? 24 : 6,
                right: television ? 24 : 6,
                child: DecoratedBox(
                  decoration: BoxDecoration(
                    color: glass.fill,
                    shape: BoxShape.circle,
                    border: Border.all(color: glass.edge),
                  ),
                  child: AndroidAppearanceMenu(iconColor: glass.ink),
                ),
              ),
            ],
          ),
        ),
      ),
    );
  }
}
