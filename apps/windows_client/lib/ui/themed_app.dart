import 'dart:async';

import 'package:flutter/material.dart';
import 'package:flutter/services.dart';

import '../data/appearance_settings.dart';
import '../data/client_tray.dart';
import '../data/launcher_memory.dart';
import '../data/update_handoff.dart';
import '../data/window_close_guard.dart';
import 'app_dialog.dart';
import 'app_exit.dart';
import 'app_menu.dart';
import 'app_theme.dart';
import 'primary_region.dart';
import 'product_update_dialog.dart';
import 'release_notes.dart';
import 'update_notice.dart';
import 'session_guide_routes.dart';
import '../data/session_activity.dart';
import '../data/mfa_account_host.dart';
import 'mfa_completion_overlay.dart';
import 'session_access_boundary.dart';
import '../l10n/l10n.dart';

class ThemedApp extends StatefulWidget {
  const ThemedApp({
    super.key,
    required this.appearance,
    required this.home,
    this.title = 'Multitap',
    this.confirmExit = false,
    this.exitGuard,
    this.tray,
    this.trayMemory,
    this.sessionAccess,
    this.mfaHost,
  });
  final AppAppearance appearance;
  final Widget home;
  final SessionActivity? sessionAccess;
  final MfaAccountHost? mfaHost;
  final String title;

  /// Ask before the window closes (the client's main window, 2026-10-26).
  final bool confirmExit;

  /// For tests; the native window guard otherwise.
  final WindowCloseGuard? exitGuard;

  /// The notification-area hand-off of the main window (2026-10-27: closing
  /// the window rests the client in the tray); the native one when null and
  /// [confirmExit] is set.
  final ClientTray? tray;

  /// Where "the tray notice was shown" is remembered (once, ever).
  final LauncherMemory? trayMemory;

  @override
  State<ThemedApp> createState() => _ThemedAppState();
}

class _ThemedAppState extends State<ThemedApp> {
  static const _window = MethodChannel('multitap/window');

  // Built once. A new ThemeData never equals the last, so every rebuild of the
  // app, which each VM sample causes, would run a theme animation over the
  // whole window (about 24 frames per sample).
  static final _light = buildAppTheme();
  static final _dark = buildAppTheme(brightness: Brightness.dark);

  final _navigator = GlobalKey<NavigatorState>();
  final _menus = MenuDismissObserver();
  final _guideRoutes = SessionGuideRoutes();
  late final _sessionRoutes = SessionAccessRoutes(widget.sessionAccess);

  // The update notice sits above the routes; its actions open their dialogs
  // on this app's navigator.
  BuildContext? get _routes => _navigator.currentState?.overlay?.context;

  Future<void> _openPanel({
    required String server,
    required String product,
    required String currentVersion,
    required bool manual,
  }) async {
    final context = _routes;
    if (context == null) return;
    await showProductUpdates(
      context,
      server: server,
      product: product,
      currentVersion: currentVersion,
      manual: manual,
    );
  }

  Future<void> _openHistory({
    required String server,
    required String product,
    required String currentVersion,
  }) async {
    final context = _routes;
    if (context == null) return;
    await showReleaseHistory(
      context,
      server: server,
      product: product,
      currentVersion: currentVersion,
    );
  }

  WindowCloseGuard? _exit;
  ClientTray? _tray;
  bool _exitAsking = false, _noticeBusy = false;

  /// The window just went to the tray: the first time ever, one balloon says
  /// the client keeps running and how to end it.
  Future<void> _hiddenToTray() async {
    final memory = widget.trayMemory, tray = _tray;
    if (memory == null || tray == null || _noticeBusy) return;
    _noticeBusy = true;
    try {
      if (await memory.loadTrayNoticeShown()) return;
      // Remembered first: a notice that cannot be remembered is not shown, so
      // it can never repeat.
      await memory.saveTrayNoticeShown();
      await tray.balloon(TrayNotices.runsInTray);
    } catch (_) {
      // The notice is a convenience; the tray itself works without it.
    } finally {
      _noticeBusy = false;
    }
  }

  /// 완전 종료 from the account menu. Nothing open: ends at once. Remote
  /// connections that would end with it: the question first.
  Future<void> _quit() async {
    final tray = _tray;
    if (tray == null || _exit == null) return;
    await _confirmExit(sessions: await tray.hasRemoteSessions(), asked: false);
  }

  /// [asked]: the native side asked (the tray menu, or X without a tray icon),
  /// and it is answered with the dialog even when no session is open.
  Future<void> _confirmExit({bool? sessions, bool asked = true}) async {
    final context = _routes, guard = _exit;
    if (context == null || guard == null || _exitAsking) return;
    _exitAsking = true;
    try {
      sessions ??=
          guard is NativeWindowCloseGuard && guard.requestedWithSessions;
      if (!asked && !sessions) {
        await guard.close();
        return;
      }
      final ok = await showAppConfirm(
        context,
        icon: Icons.power_settings_new_rounded,
        tone: sessions ? AppTone.danger : AppTone.accent,
        title: l10n.quitMultitap,
        message: sessions
            ? l10n.allOpenConnectionsCloseToo
            : l10n.desktopsStay,
        confirmLabel: l10n.quitLabel,
        dialogKey: const Key('exit-confirm-dialog'),
        confirmKey: const Key('exit-confirm'),
        cancelKey: const Key('exit-cancel'),
      );
      if (ok) await guard.close();
    } finally {
      _exitAsking = false;
    }
  }

  // "언어 / Language": the new language applies at once. Text read through
  // l10n (not an inherited widget) is rebuilt everywhere below; no state is
  // lost.
  void _languageChanged() {
    if (!mounted) return;
    setState(() {});
    rebuildAllText(context);
  }

  @override
  void initState() {
    super.initState();
    AppLanguageController.instance.addListener(_languageChanged);
    widget.appearance.addListener(_appearanceChanged);
    _appearanceChanged();
    UpdateNotices.instance
      ..openPanel = _openPanel
      ..openHistory = _openHistory;
    if (widget.confirmExit) {
      final guard = _exit = widget.exitGuard ?? WindowCloseGuard.windows();
      guard.attach(_confirmExit);
      unawaited(guard.update(busy: false, dirty: true));
      final tray = _tray = widget.tray ?? ClientTray.windows();
      tray.attach(_hiddenToTray);
    }
  }

  @override
  void didUpdateWidget(ThemedApp oldWidget) {
    super.didUpdateWidget(oldWidget);
    if (!identical(oldWidget.appearance, widget.appearance)) {
      oldWidget.appearance.removeListener(_appearanceChanged);
      widget.appearance.addListener(_appearanceChanged);
      _appearanceChanged();
    }
  }

  void _appearanceChanged() => unawaited(_syncCaption());

  Future<void> _syncCaption() async {
    try {
      await _window.invokeMethod<void>(
        'appearance',
        widget.appearance.mode != ThemeMode.light,
      );
    } on MissingPluginException {
      // Non-Windows widget previews have no native frame.
    } on PlatformException {
      // An unavailable frame theme must not interrupt authentication/session state.
    }
  }

  @override
  void dispose() {
    AppLanguageController.instance.removeListener(_languageChanged);
    _sessionRoutes.dispose();
    _guideRoutes.dispose();
    _exit?.dispose();
    _tray?.dispose();
    widget.appearance.removeListener(_appearanceChanged);
    final notices = UpdateNotices.instance;
    if (notices.openPanel == _openPanel) notices.openPanel = null;
    if (notices.openHistory == _openHistory) notices.openHistory = null;
    super.dispose();
  }

  @override
  Widget build(BuildContext context) {
    final app = AppearanceScope(
      controller: widget.appearance,
      child: ListenableBuilder(
        listenable: widget.appearance,
        builder: (context, _) => MaterialApp(
          navigatorKey: _navigator,
          navigatorObservers: [_menus, _guideRoutes, _sessionRoutes],
          // Popups of a remote window spanning two monitors stay on the
          // primary one (PrimaryRegionScope).
          builder: (context, child) {
            final content = UpdateNoticeHost(
              child: PrimaryRegionScope.builder(context, child),
            );
            final protected = widget.sessionAccess == null
                ? content
                : SessionAccessBoundary(
                    controller: widget.sessionAccess!,
                    child: content,
                  );
            return widget.mfaHost == null ? protected : MfaCompletionOverlay(host: widget.mfaHost!, child: protected);
          },
          title: widget.title,
          debugShowCheckedModeBanner: false,
          theme: _light,
          darkTheme: _dark,
          themeMode: widget.appearance.mode,
          locale: AppLanguageController.instance.locale,
          supportedLocales: AppLocalizations.supportedLocales,
          localizationsDelegates: AppLocalizations.localizationsDelegates,
          home: widget.home,
        ),
      ),
    );
    // Only the client's main window offers 완전 종료.
    return _exit == null ? app : AppExitScope(requestExit: _quit, child: app);
  }
}
