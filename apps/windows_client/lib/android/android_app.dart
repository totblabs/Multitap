import 'dart:async';
import 'dart:io' show HttpClient, Platform;

import 'package:flutter/material.dart';
import 'package:flutter/services.dart';

import '../data/appearance_settings.dart';
import '../data/broker_api.dart';
import '../data/notices.dart';
import 'android_notices.dart';
import '../data/broker_probe.dart';
import '../data/broker_transport.dart';
import '../ui/broker_transport_controls.dart';
import '../data/client_settings.dart';
import '../data/product_log.dart';
import '../data/console_resume.dart';
import '../data/desktop_mapping.dart';
import '../data/desktop_previews.dart';
import '../data/desktop_watch.dart';
import '../data/emergency_console_session.dart';
import '../models/vm_resources.dart';
import '../data/product_updates.dart' show UpdateFailure, ProductRelease;
import '../data/session_renewal.dart';
import '../data/launcher_memory.dart' show OrganizePreference;
import '../models/desktop_machine.dart';
import '../models/desktop_organize.dart';
import '../ui/desktop_organizer.dart';
import '../models/stream_options.dart';
import '../ui/app_dialog.dart';
import '../ui/app_form.dart';
import '../ui/app_theme.dart';
import '../ui/console_resume_dialog.dart';
import '../ui/components.dart';
import '../ui/desktop_menu.dart'
    show
        DesktopActions,
        confirmPowerAction,
        powerActionLabel,
        stopUnavailableReason;
import '../ui/pc_power_help.dart';
import '../ui/desktop_readiness.dart';
import '../ui/desktop_tile.dart'
    show DesktopTile, StartProgress, desktopTileGap, desktopTileGrid;
import '../ui/korean_text.dart';
import '../ui/mfa_login_dialog.dart';
import '../ui/mfa_account_dialog.dart';
import '../ui/mfa_completion_overlay.dart';
import '../data/mfa_account_host.dart';
import '../data/console_browser.dart';
import '../ui/readiness_dialog.dart';
import '../ui/product_log_dialog.dart';
import 'android_launcher.dart';
import 'android_application.dart';
import 'android_media.dart' show AndroidRemoteMedia;
import 'android_diagnostics.dart';
import 'android_host_update_dialog.dart';
import 'android_network.dart';
import 'android_reconnect.dart' show androidAwaitBrokerRestart;
import 'android_remote.dart';
import '../data/session_activity.dart';
import '../data/session_windows.dart';
import '../ui/session_access_boundary.dart';
import 'android_remote_notice.dart' show androidNoticeDuration;
import 'android_session_keepalive.dart';
import 'android_store.dart';
import 'android_stream_preparation.dart';
import 'android_update_panel.dart';
import 'android_updates.dart';
import 'android_tv.dart';
import '../l10n/l10n.dart';

/// The Android app root: the Windows client's dark/amber theme, and its
/// night/day scene remembered on this device apart from the server settings.
class AndroidVdiApp extends StatefulWidget {
  const AndroidVdiApp({
    super.key,
    this.store,
    this.apiFactory,
    this.updatesFactory,
    this.updatePlatform,
    this.consoleTransportFactory = openConsoleTransport,
    this.television,
    this.hardwareKeyboard,
    this.desktopWatchFactory,
    this.sessionRetryDelay = const Duration(seconds: 15),
    this.sessionRenewalLead = const Duration(minutes: 5),
    this.clock = DateTime.now,
  });
  final AndroidStore? store;
  final ConsoleTransportFactory consoleTransportFactory;

  /// Tests can supply device capability without a native Activity.
  final bool? television;
  final bool? hardwareKeyboard;

  /// Tests substitute the live desktop channel; the app uses [BrokerDesktopWatch].
  final DesktopWatchFactory? desktopWatchFactory;

  /// Session lifetime handling (see AndroidHome); tests shorten and fake it.
  final Duration sessionRetryDelay, sessionRenewalLead;
  final DateTime Function() clock;

  /// Tests substitute the broker; the app uses [HttpBrokerApi].
  final BrokerApi Function(Uri origin)? apiFactory;

  /// Tests substitute the catalog/download and the native installer side.
  final AndroidUpdates Function(Uri origin)? updatesFactory;
  final AndroidUpdatePlatform? updatePlatform;
  @override
  State<AndroidVdiApp> createState() => _AndroidVdiAppState();
}

class _AndroidVdiAppState extends State<AndroidVdiApp> with WidgetsBindingObserver {
  late final _mfa = MfaAccountHost(clock: widget.clock);
  late final _sessionAccess = SessionActivity(clock: widget.clock);
  late final _sessionRoutes = SessionAccessRoutes(_sessionAccess);
  late final AndroidStore _store = widget.store ?? AndroidStore();
  AppAppearance? _appearance;
  bool _television = false;
  bool _hardwareKeyboard = false;
  int _keyboardRevision = 0;
  bool _navigationSourceSeen = false;

  // "언어 / Language" applies at once: text read through l10n is rebuilt
  // everywhere below without losing any state.
  void _languageChanged() {
    if (!mounted) return;
    setState(() {});
    rebuildAllText(context);
  }

  @override
  void didChangeLocales(List<Locale>? locales) =>
      AppLanguageController.instance.systemLocaleChanged();

  @override
  void initState() {
    super.initState();
    WidgetsBinding.instance.addObserver(this);
    AppLanguageController.instance.addListener(_languageChanged);
    AndroidApplication.channel.setMethodCallHandler((call) async {
      if (!mounted) return;
      if (call.method == 'authenticationEvent') {
        final value = call.arguments;
        if (value is Map && value['kind'] == 'terminal' && validAuthenticationOwner(value['owner']) &&
            const {'pc_login_replaced', 'session_idle_timeout'}.contains(value['reason'])) {
          // The normal auth controller may already have cleared its owner after
          // generic401. The completed presentation still fences this exact actor.
          _mfa.authenticationRetired(value['owner'] as String, value['reason'] as String);
        }
        if (value is Map &&
            value['owner'] == _sessionAccess.owner &&
            validAuthenticationOwner(value['owner'])) {
          if (value['kind'] == 'human') _sessionAccess.human();
          if (value['kind'] == 'terminal' &&
              const {
                'pc_login_replaced',
                'session_idle_timeout',
              }.contains(value['reason'])) {
            _sessionAccess.terminate(value['reason'] as String);
          }
        }
      } else if (call.method == 'consoleWebViewClosed') {
        await ConsoleBrowser.handleClosed(
          call.arguments,
          channelName: AndroidApplication.channel.name,
        );
      } else if (call.method == 'imeVisibility' && call.arguments is bool) {
        AndroidApplication.imeVisible.value = call.arguments == true;
      } else if (call.method == 'hardwareKeyboardChanged' &&
          call.arguments is bool) {
        _keyboardRevision++;
        setState(() => _hardwareKeyboard = call.arguments == true);
        if (!_hardwareKeyboard) AndroidApplication.hardwareNavigation = false;
      } else if (call.method == 'hardwareNavigation' &&
          call.arguments is bool) {
        _navigationSourceSeen = true;
        AndroidApplication.hardwareNavigation = call.arguments == true;
      }
    });
    unawaited(_load());
  }

  Future<void> _load() async {
    final keyboardRevision = _keyboardRevision;
    var television = widget.television ?? false;
    var hardwareKeyboard = widget.hardwareKeyboard ?? false;
    if (widget.television == null && Platform.isAndroid) {
      try {
        television =
            await AndroidApplication.channel
                .invokeMethod<bool>('isTelevision')
                .timeout(const Duration(seconds: 2)) ??
            false;
      } catch (_) {
        // Older/native-less shells retain the existing touch interface.
      }
    }
    if (television && widget.hardwareKeyboard == null && Platform.isAndroid) {
      try {
        hardwareKeyboard =
            await AndroidApplication.channel
                .invokeMethod<bool>('hasHardwareKeyboard')
                .timeout(const Duration(seconds: 2)) ??
            false;
      } catch (_) {
        /* Older engines keep the remote-only input contract. */
      }
    }
    var memory = const AndroidAppearanceMemory();
    try {
      memory = await _store.appearance();
    } catch (_) {
      // The device's own appearance applies; nothing else depends on it.
    }
    final system =
        WidgetsBinding.instance.platformDispatcher.platformBrightness ==
            Brightness.dark
        ? ThemeMode.dark
        : ThemeMode.light;
    if (!mounted) return;
    AppLanguageController.instance.configure(
      memory.language,
      save: _store.saveLanguage,
    );
    setState(() {
      _television = television;
      if (keyboardRevision == _keyboardRevision) {
        _hardwareKeyboard = hardwareKeyboard;
      }
      if (!_navigationSourceSeen) {
        AndroidApplication.hardwareNavigation = _hardwareKeyboard;
      }
      _appearance = AppAppearance(
        mode: memory.mode ?? system,
        scene: memory.scene,
        onSave: _store.saveTheme,
        onSaveScene: _store.saveScene,
      );
    });
  }

  @override
  void dispose() {
    WidgetsBinding.instance.removeObserver(this);
    AppLanguageController.instance.removeListener(_languageChanged);
    _mfa.dispose();
    _sessionRoutes.dispose();
    _sessionAccess.dispose();
    _noticeHost.dispose();
    AndroidApplication.channel.setMethodCallHandler(null);
    AndroidApplication.hardwareNavigation = false;
    _appearance?.dispose();
    super.dispose();
  }

  @override
  void didChangeAppLifecycleState(AppLifecycleState state) { _mfa.checkPrivacyIdle(); }

  /// 공지 (android_notices.dart): the signed-in home tells it this sign-in's
  /// notices; the layer above the pages shows them in the launcher and over a
  /// stream.
  final _noticeHost = AndroidNoticeHost();

  // Built once: a new ThemeData never equals the last, and a rebuild would run
  // a theme animation over the whole screen.
  static final _light = androidTheme(buildAppTheme());
  static final _dark = androidTheme(buildAppTheme(brightness: Brightness.dark));

  @override
  Widget build(BuildContext context) {
    final appearance = _appearance;
    MaterialApp app(ThemeMode mode, Widget home) => MaterialApp(
      title: 'Multitap',
      navigatorObservers: [_sessionRoutes],
      debugShowCheckedModeBanner: false,
      locale: AppLanguageController.instance.locale,
      supportedLocales: AppLocalizations.supportedLocales,
      localizationsDelegates: AppLocalizations.localizationsDelegates,
      theme: _light,
      darkTheme: _dark,
      themeMode: mode,
      // No tooltip bubbles on a touch screen: a long press (sticky keys,
      // the toolbar) keeps popping them up. The labels
      // stay for screen readers.
      builder: (context, child) => MfaCompletionOverlay(host: _mfa, child: SessionAccessBoundary(
        controller: _sessionAccess,
        child: AndroidTvNavigation(
          television: _television,
          hardwareKeyboard: _hardwareKeyboard,
          child: TooltipVisibility(
            visible: false,
            child: AndroidNoticeLayer(
              host: _noticeHost,
              child: appearance == null
                  ? child!
                  : AppearanceScope(controller: appearance, child: child!),
            ),
          ),
        ),
      )),
      home: home,
    );
    if (appearance == null) {
      return app(
        ThemeMode.dark,
        const Scaffold(body: Center(child: CircularProgressIndicator())),
      );
    }
    return ListenableBuilder(
      listenable: appearance,
      builder: (_, _) => app(
        appearance.mode,
        AndroidHome(
          sessionAccess: _sessionAccess,
          mfaHost: _mfa,
          noticeHost: _noticeHost,
          store: _store,
          apiFactory: widget.apiFactory,
          updatesFactory: widget.updatesFactory,
          updatePlatform: widget.updatePlatform,
          consoleTransportFactory: widget.consoleTransportFactory,
          desktopWatchFactory: widget.desktopWatchFactory,
          sessionRetryDelay: widget.sessionRetryDelay,
          sessionRenewalLead: widget.sessionRenewalLead,
          clock: widget.clock,
        ),
      ),
    );
  }
}

/// How often the list is read while the live channel is down.
const androidFallbackPollInterval = Duration(seconds: 15);

/// The app's broker client: automatic client diagnostics (fixed fields, no
/// content) as on Windows, and what this app asks of each connection.
HttpBrokerApi androidBrokerApi(
  Uri origin, {
  required String? Function() diagnosticsVersion,
  @visibleForTesting HttpClient? client,
}) => HttpBrokerApi(
  origin: origin,
  client: client,
  enableDiagnostics: true,
  diagnosticsVersion: diagnosticsVersion,
  // Committed text input where the Host offers it.
  requestTextInput: true,
  requestPcLoginReplacement: true,
  requestStreamTakeoverContext: true,
  requestTouchInput: true,
  requestCaret: true,
  // print-v1: documents printed in the VM go to Android's print dialog.
  requestPrint: true,
  // udp-media-v1: video and audio over UDP, WebSocket as the fallback.
  requestUdpMedia: true,
  // audio-opus-v1: ~0.14 instead of 1.5 Mbps of sound where the desktop shows
  // it (AndroidOpusDecoder.kt); PCM where not offered or not granted.
  requestAudioOpus: true,
  // ...except once this device's engine found no Opus decoder that plays:
  // PCM then, so the sound is there (audio-path-v1, 2026-11-11).
  audioOpusDecodable: () => AndroidRemoteMedia.opusDecodable,
  // 마우스 잠금 with a connected mouse: relative-mouse-v1 (with the cursor in
  // the video, or echoed, where granted) and usb-v1 for the virtual USB mouse
  // only, never this device's own USB devices.
  requestRelativeMouse: true,
  requestUsb: true,
  // drive-v1: folders picked on this device (Android's folder picker) are
  // drives in the desktop, where the administrator allows shared folders.
  requestDrives: true,
);

class _StartFlow {
  int seconds = 0;
  bool powered = false, connecting = false;

  /// Waits only for the previous connection's cleanup (no power request).
  bool settle = false;
}

class AndroidHome extends StatefulWidget {
  const AndroidHome({
    super.key,
    required this.store,
    this.apiFactory,
    this.updatesFactory,
    this.updatePlatform,
    this.consoleTransportFactory = openConsoleTransport,
    this.desktopWatchFactory,
    this.sessionRetryDelay = const Duration(seconds: 15),
    this.sessionRenewalLead = const Duration(minutes: 5),
    this.clock = DateTime.now,
    this.sessionAccess,
    this.mfaHost,
    this.noticeHost,
  });

  /// 공지 (android_notices.dart): where this home publishes its sign-in's
  /// notices. Null: nothing shows them (tests of other things).
  final AndroidNoticeHost? noticeHost;
  final DesktopWatchFactory? desktopWatchFactory;
  final AndroidStore store;

  /// Wait between silent sign-in attempts while the broker is unreachable,
  /// and how long before the expiry the session is renewed.
  final Duration sessionRetryDelay, sessionRenewalLead;

  /// The time source of session lifetimes (tests substitute a virtual one).
  final DateTime Function() clock;
  final SessionActivity? sessionAccess;
  final MfaAccountHost? mfaHost;
  final BrokerApi Function(Uri origin)? apiFactory;
  final AndroidUpdates Function(Uri origin)? updatesFactory;
  final AndroidUpdatePlatform? updatePlatform;
  final ConsoleTransportFactory consoleTransportFactory;
  @override
  State<AndroidHome> createState() => _AndroidHomeState();
}

class _AndroidHomeState extends State<AndroidHome> with WidgetsBindingObserver {
  late final _mfa = widget.mfaHost ?? MfaAccountHost(clock: widget.clock);
  late final _sessionAccess =
      widget.sessionAccess ?? SessionActivity(clock: widget.clock);
  String? _authenticationOwner;
  Future<void> _authenticationRetirement = Future<void>.value();
  Route<void>? _remoteRoute;
  static const _startTimeout = Duration(minutes: 5);
  static const _columnWidth = 1240.0;
  AndroidStore get _store => widget.store;
  final _id = TextEditingController();
  final _password = TextEditingController();
  final _revision = ValueNotifier<int>(0);
  Uri? _origin;
  BrokerApi? _api;
  BrokerSession? _session;
  List<BrokerDesktop> _desktops = [];
  DateTime? _checkedAt;
  bool _loading = true, _busy = false, _remember = false;

  /// [_busy] because a sign-out is being sent: the sign-in button waits
  /// without saying "로그인 중…".
  bool _signingOut = false;
  bool _refreshing = false, _streaming = false, _foreground = true;
  bool _refreshFailed = false;
  String? _error, _listError, _openingDesktop, _version;
  int _connectionIntent = 0;
  bool _streamConnectPromptOpen = false;
  int _hostUpdateIntent = 0;
  // Keep the direct response visible if its follow-up inventory read fails.
  final _hostUpdateReplies =
      <String, (BrokerSession, BrokerApi, HostUpdateState, DateTime, int)>{};
  int _serverRevision = 0, _versionReadRevision = 0, _versionReadScope = -1;
  Future<String?>? _versionRead;

  /// This app's running power request per desktop (its operation number).
  final _powerPending = <String, int>{};

  /// The broker's stop permission from the latest answer to that request
  /// (a listing read meanwhile predates it).
  final _powerCanStop = <String, bool>{};
  int _powerSequence = 0;

  final _flows = <String, _StartFlow>{};
  AndroidStreamQuality _quality = AndroidStreamQuality.recommended;
  AndroidSessionSettings _sessionSettings = const AndroidSessionSettings();

  /// The display's measured density for 자동 화면 배율, read at start and
  /// whenever the screen changes (folding, rotation); null: the bucket.
  AndroidScreenDensity? _density;
  int _memoryRevision = 0;
  int _loginInputRevision = 0,
      _loginRequestRevision = 0,
      _loginRestoreRevision = 0;

  void _loginInputChanged() => _loginInputRevision++;
  Timer? _poll, _ticker, _updateTimer;
  DesktopUpdates? _watch;
  StreamSubscription<void>? _watchChanges, _catalogFeed;

  /// 공지 (data/notices.dart): one center per sign-in token, kept in step with
  /// the session by [_syncNotices]; the live channel's pushes.
  NoticeCenter? _notices;
  String? _noticeToken;
  StreamSubscription<void>? _noticeFeed;

  /// Called from build: whatever path signed in, out or again, the notices
  /// follow the session's token. The layer above the pages hears it after
  /// this frame.
  void _syncNotices() {
    final api = _api, token = _session?.token;
    if (token != _noticeToken) {
      _noticeToken = token;
      _notices?.dispose();
      _notices = token != null && api is NoticeBrokerApi
          ? (NoticeCenter(
              api: api as NoticeBrokerApi,
              token: token,
              clock: widget.clock,
              pushConnected: () => _watch?.connected == true,
            )..start())
          : null;
    }
    final host = widget.noticeHost;
    if (host == null) return;
    final center = _notices, streaming = _streaming;
    if (identical(host.center, center) && host.streaming == streaming) return;
    WidgetsBinding.instance.addPostFrameCallback((_) {
      if (!mounted) return;
      host.bind(_notices);
      host.streaming = _streaming;
    });
  }
  StreamSubscription<String>? _terminalAuthFeed;
  StreamSubscription<List<LiveResourceSample>>? _resourceFeed;
  Timer? _sampleClock;
  bool _watchWasConnected = false;

  /// Pushed CPU/RAM/uptime samples by VM id; they win over a list entry whose
  /// own sample is older (the list is read only when something else changed).
  final _pushed = <String, VmResources>{};
  Set<String> _staleShown = {};
  bool _feedSeen = false;
  bool _refreshAgain = false;
  late final AndroidUpdatePlatform _updatePlatform =
      widget.updatePlatform ?? NativeAndroidUpdatePlatform();
  StreamSubscription<String>? _installStatus;
  AndroidRelease? _update;
  bool _updateChecking = false;
  String? _abi;

  // --- search, 분류 and sort (as on Windows) ---------------------------------

  final _search = TextEditingController();
  final _searchFocus = FocusNode(debugLabel: 'desktop-search');
  String _query = '';
  bool _searchOpen = false;
  DesktopChip _chip = DesktopChip.all;
  DesktopStatus _status = DesktopStatus.all;
  DesktopKind _kind = DesktopKind.all;
  DesktopSort _sort = DesktopSort.number;
  bool _viewSaving = false;
  bool _showPreviews = true, _exitBusy = false;

  Future<void> _selectPreviews() async {
    if (_viewSaving) return;
    final next = !_showPreviews;
    setState(() => _viewSaving = true);
    try {
      await _store.saveLauncherPreviews(next);
      if (mounted) {
        setState(() => _showPreviews = next);
        _bindPreviews();
      }
    } catch (_) {
      if (mounted) _notice(l10n.couldntSavePreviewSetting);
    } finally {
      if (mounted) setState(() => _viewSaving = false);
    }
  }

  Future<void> _exitApplication() async {
    if (_exitBusy) return;
    _exitBusy = true;
    var closing = false;
    try {
      final confirmed = await showAppConfirm(
        context,
        icon: Icons.power_settings_new_rounded,
        title: l10n.quitApp,
        message: l10n.appClosesRemovedFromRecent,
        confirmLabel: l10n.quit,
        confirmKey: const Key('confirm-android-exit'),
        cancelKey: const Key('cancel-android-exit'),
      );
      if (!confirmed || !mounted) return;
      if (_streaming) {
        _notice(l10n.disconnectRemoteScreenThenQuit);
        return;
      }
      ++_connectionIntent;
      for (final id in _flows.keys.toList()) {
        _cancelFlow(id);
      }
      // 완전 종료 ends the sign-in at the server too. Left alive there, the
      // next sign-in on this same phone asked "다른 곳에서 로그아웃하고 여기서
      // 로그인할까요?" (inspection, 2026-11-12). A server that does not answer
      // in time does not keep the app open.
      final session = _session;
      if (session != null) {
        await _endSignIn(session, limit: const Duration(seconds: 3));
        if (!mounted) return;
      }
      await AndroidApplication.close();
      closing = true;
    } catch (_) {
      if (mounted) _notice(l10n.couldntQuitAppTryAgain);
    } finally {
      if (!closing) _exitBusy = false;
    }
  }

  /// Whose chip, status, kind and sort are loaded ("origin|account").
  String? _organizeFor;

  /// A favourite change being sent, and the answers not yet confirmed by a
  /// list read that began after them.
  final _favoritePending = <String, bool>{};
  final _favoriteDone = <String, (bool, DateTime)>{};

  void _searchTextChanged() {
    if (_search.text == _query) return;
    setState(() {
      _query = _search.text;
      if (_query.isNotEmpty) _searchOpen = true;
    });
  }

  void _searchFocusChanged() {
    if (!mounted) return;
    if (!_searchFocus.hasFocus && _query.isEmpty && _searchOpen) {
      setState(() => _searchOpen = false);
    } else if (_searchFocus.hasFocus && !_searchOpen) {
      setState(() => _searchOpen = true);
    }
  }

  void _openSearch({String? text}) {
    setState(() => _searchOpen = true);
    if (text != null) {
      _search.value = TextEditingValue(
        text: text,
        selection: TextSelection.collapsed(offset: text.length),
      );
    } else {
      _search.selection = TextSelection(
        baseOffset: 0,
        extentOffset: _search.text.length,
      );
    }
    _searchFocus.requestFocus();
  }

  void _closeSearch() {
    _search.clear();
    _searchFocus.unfocus();
    setState(() => _searchOpen = false);
  }

  bool _searchKey(KeyEvent event) {
    if (!mounted || _session == null) return false;
    return handleLauncherSearchKey(
      context,
      event,
      open: _openSearch,
      close: _closeSearch,
      searching: _query.isNotEmpty || _searchOpen,
      hasDesktops: _desktops.isNotEmpty,
    );
  }

  Future<void> _loadOrganize(String key, Uri origin, String account) async {
    OrganizePreference saved;
    try {
      saved = await _store.organize(origin, account);
    } catch (_) {
      saved = const OrganizePreference();
    }
    if (!mounted || _organizeFor != key) return;
    setState(() {
      final organize = saved.migrated();
      _chip = DesktopChip.parse(organize.chip);
      _status = DesktopStatus.byId(organize.status);
      _kind = DesktopKind.byId(organize.kind);
      _sort = DesktopSort.byName(organize.sort);
    });
  }

  void _saveOrganize() {
    final origin = _origin, session = _session;
    if (origin == null || session == null) return;
    unawaited(
      _store
          .saveOrganize(
            origin,
            session.username,
            OrganizePreference(
              chip: _chip.id,
              status: _status.id,
              sort: _sort.name,
              kind: _kind.id,
            ),
          )
          .catchError((Object _) {}),
    );
  }

  DesktopChip _effectiveChip(List<DesktopMachine> machines) =>
      effectiveChip(_chip, machines);

  DesktopStatus _effectiveStatus(List<DesktopMachine> machines) =>
      effectiveStatus(_status, machines);

  /// The 구분 filter, shown only while both VMs and local PCs are assigned.
  DesktopKind _effectiveKind(List<DesktopMachine> machines) =>
      effectiveKind(_kind, machines);

  void _selectChip(DesktopChip chip) {
    setState(() => _chip = chip);
    _saveOrganize();
  }

  void _selectStatus(DesktopStatus status) {
    setState(() => _status = status);
    _saveOrganize();
  }

  void _selectKind(DesktopKind kind) {
    setState(() => _kind = kind);
    _saveOrganize();
  }

  /// 필터 해제: the 구분 and 상태 filters and the 분류 chip (the search has its
  /// own clear button).
  void _clearFilters() {
    setState(() {
      _kind = DesktopKind.all;
      _status = DesktopStatus.all;
      _chip = DesktopChip.all;
    });
    _saveOrganize();
  }

  void _selectSort(DesktopSort sort) {
    setState(() => _sort = sort);
    _saveOrganize();
  }

  Future<void> _toggleFavorite(DesktopMachine machine) async {
    final api = _api, session = _session;
    if (api is! FavoriteBrokerApi || session == null) return;
    if (_favoritePending.containsKey(machine.id)) return;
    final next = !machine.organize.favorite;
    setState(() => _favoritePending[machine.id] = next);
    try {
      final saved = await (api as FavoriteBrokerApi).setFavorite(
        session.token,
        machine.id,
        next,
      );
      if (mounted) _favoriteDone[machine.id] = (saved, DateTime.now());
    } catch (error) {
      if (error is BrokerException && error.status == 401) {
        if (mounted && _session == session) _expired(error);
      } else if (mounted) {
        _notice(
          error is BrokerException && error.status == 404
              ? l10n.desktopNoLongerAssignedYou
              : l10n.couldntChangeFavoritesTryAgain,
        );
      }
    } finally {
      if (mounted) setState(() => _favoritePending.remove(machine.id));
    }
  }

  @override
  void initState() {
    super.initState();
    _id.addListener(_loginInputChanged);
    _password.addListener(_loginInputChanged);
    WidgetsBinding.instance.addObserver(this);
    _search.addListener(_searchTextChanged);
    _searchFocus.addListener(_searchFocusChanged);
    HardwareKeyboard.instance.addHandler(_searchKey);
    unawaited(_restore());
    _installStatus = _updatePlatform.statuses.listen((code) {
      if (mounted) {
        _notice(
          code == 'installed' ? l10n.updateInstalled : androidUpdateMessage(code),
        );
      }
    });
  }

  // --- live desktop state ---------------------------------------------------
  //
  // The broker computes each VM's state and pushes "something changed" over
  // the live channel; this app then reads the list once. While the channel is
  // connected nothing polls. Only while it is down the list is read every 15 s
  // (a gentle fallback), and once when it comes back or the app returns.

  void _startWatch() {
    final session = _session, origin = _origin;
    if (!_foreground ||
        session == null ||
        origin == null ||
        _watch != null ||
        (widget.desktopWatchFactory == null && _api is! HttpBrokerApi)) {
      _syncFallback();
      return;
    }
    try {
      final watch =
          widget.desktopWatchFactory?.call(origin, session.token) ??
          BrokerDesktopWatch(origin: origin, token: session.token);
      _watch = watch;
      if (watch is TerminalDesktopAuthFeed) {
        _terminalAuthFeed = (watch as TerminalDesktopAuthFeed)
            .terminalAuthentication
            .listen((code) {
              if (mounted &&
                  _watch == watch &&
                  _session == session &&
                  const {
                    'pc_login_replaced',
                    'session_idle_timeout',
                    'session_reauthentication_required',
                  }.contains(code)) {
                _retireAuthentication(session, code);
              }
            });
      }
      watch.addListener(_watchStateChanged);
      _watchChanges = watch.changes.listen((_) {
        if (!mounted || _watch != watch) return;
        // While streaming the list is read when the stream ends.
        if (!_streaming) unawaited(_refresh(quiet: true));
      }, onError: (Object _) => _stopWatch());
      if (watch is LiveDesktopFeeds) {
        final feeds = watch as LiveDesktopFeeds;
        _resourceFeed = feeds.resources.listen(
          _applyResources,
          onError: (Object _) {},
        );
        // The public catalog changed: the update button follows at once.
        _catalogFeed = feeds.catalog.listen((_) {
          if (mounted && _watch == watch && _session != null) {
            unawaited(_checkUpdates());
          }
        }, onError: (Object _) {});
      }
      if (watch is NoticeFeed) {
        _noticeFeed = (watch as NoticeFeed).notices.listen((_) {
          if (mounted && _watch == watch) _notices?.signal();
        }, onError: (Object _) {});
      }
      _sampleClock = Timer.periodic(
        const Duration(seconds: 5),
        (_) => _markExpiredSamples(),
      );
      watch.start();
    } catch (_) {
      _stopWatch();
    }
    _syncFallback();
  }

  void _stopWatch({bool sync = true}) {
    unawaited(_terminalAuthFeed?.cancel());
    _terminalAuthFeed = null;
    final watch = _watch, changes = _watchChanges;
    _watch = null;
    _watchChanges = null;
    _watchWasConnected = false;
    _feedSeen = false;
    _sampleClock?.cancel();
    _sampleClock = null;
    for (final feed in [_resourceFeed, _catalogFeed, _noticeFeed]) {
      if (feed != null) {
        unawaited(Future<void>.sync(feed.cancel).catchError((Object _) {}));
      }
    }
    _resourceFeed = _catalogFeed = _noticeFeed = null;
    watch?.removeListener(_watchStateChanged);
    if (changes != null) {
      unawaited(Future<void>.sync(changes.cancel).catchError((Object _) {}));
    }
    if (watch != null) {
      unawaited(Future<void>.sync(watch.close).catchError((Object _) {}));
    }
    if (sync) _syncFallback();
  }

  /// The channel connected or dropped: the fallback timers follow it, and a
  /// connect reads the catalog once (a release published while it was down).
  void _watchStateChanged() {
    final connected = _watch?.connected == true;
    final came = connected && !_watchWasConnected;
    _watchWasConnected = connected;
    _syncFallback();
    if (came && _session != null) unawaited(_checkUpdates());
    // The channel is (back) up: read what was sent meanwhile, and ask a
    // server that may have been updated since it said it has no notices.
    if (came) _notices?.signal();
  }

  /// How long a pushed sample stays current (the broker pushes about every 5 s).
  static const _sampleLife = Duration(seconds: 20);

  void _applyResources(List<LiveResourceSample> samples) {
    if (!mounted || _session == null) return;
    _feedSeen = true;
    setState(() {
      for (final sample in samples) {
        _pushed[sample.id] = sample.resources ?? const VmResources();
      }
    });
  }

  /// The sample a card shows: the pushed one unless the list's is newer.
  VmResources _resourcesOf(BrokerDesktop desktop) {
    final pushed = _pushed[desktop.id], listed = desktop.resources;
    final base =
        pushed != null &&
            (listed.checkedAt == null ||
                pushed.checkedAt == null ||
                !pushed.checkedAt!.isBefore(listed.checkedAt!))
        ? pushed
        : listed;
    // Only a broker that pushes samples is expected to keep them coming.
    return _feedSeen && base.expired(DateTime.now(), after: _sampleLife)
        ? base.asStale()
        : base;
  }

  /// Redraws when a sample ran past its life with no new push (the broker's
  /// read failed), so a card never keeps showing a number that is gone.
  void _markExpiredSamples() {
    if (!mounted || _session == null || !_foreground || !_feedSeen) return;
    final stale = {
      for (final desktop in _desktops)
        if (_resourcesOf(desktop).stale) desktop.id,
    };
    if (stale.length != _staleShown.length || !stale.containsAll(_staleShown)) {
      _staleShown = stale;
      setState(() {});
    }
  }

  // --- broker pictures (desktop-preview-v1, as on Windows) -------------------
  //
  // A card shows a recent picture of its desktop before the first connection.
  // Asked only while this launcher is on screen: at sign-in, on return to the
  // app, about every 60 s, and at once (at most every 15 s per desktop) for a
  // new desktop or a changed Windows session. Never in the background, during
  // a stream or with 화면 미리보기 off. Memory only; it leaves with the account.

  late final _previews = DesktopPreviewBoard(
    changed: () {
      if (mounted) setState(() {});
    },
    clock: () => widget.clock(),
  );

  bool get _launcherOnScreen =>
      mounted &&
      _session != null &&
      _foreground &&
      !_streaming &&
      _showPreviews;

  void _bindPreviews() {
    final api = _api, session = _session;
    if (api is! DesktopPreviewBrokerApi || session == null || !_showPreviews) {
      _previews.stop();
      return;
    }
    _previews.start(
      BrokerDesktopPreviews(api as DesktopPreviewBrokerApi, session.token),
      machines: () => _desktops.map(_machine),
      visible: () => _launcherOnScreen,
    );
  }

  /// Runs the fallback timer only while signed in, visible and without a
  /// connected live channel.
  // A screen that stood still (recordConnectStep stage 29). Every 5 s while
  // signed in and in front: a timer of this side that came late, or one
  // question to the platform side that took long, is recorded with how long.
  // Only on a device: the timer and the question belong to no test.
  Timer? _stallWatch;
  final _stallClock = Stopwatch();
  int _stallMark = 0;
  bool _stallProbe = false;
  static const _stallInterval = Duration(seconds: 5);
  static const _stallFloorMs = 3000;

  void _syncStallWatch() {
    if (!Platform.isAndroid) return;
    if (_session == null || !_foreground) {
      _stallWatch?.cancel();
      _stallWatch = null;
      return;
    }
    if (_stallWatch != null) return;
    _stallClock
      ..reset()
      ..start();
    _stallMark = 0;
    _stallWatch = Timer.periodic(_stallInterval, (_) => _stallTick());
  }

  bool get _stallSeen =>
      WidgetsBinding.instance.lifecycleState == AppLifecycleState.resumed;

  void _stallTick() {
    final now = _stallClock.elapsedMilliseconds;
    final late = now - _stallMark - _stallInterval.inMilliseconds;
    _stallMark = now;
    // Behind another app nothing is promised: only what the person saw.
    if (!_stallSeen) return;
    if (late > _stallFloorMs) _stalled(late, 2);
    if (_stallProbe) return;
    _stallProbe = true;
    unawaited(
      AndroidScreenDensity.read().whenComplete(() {
        _stallProbe = false;
        final took = _stallClock.elapsedMilliseconds - now;
        if (took > _stallFloorMs && _stallSeen) _stalled(took, 1);
      }),
    );
  }

  void _stalled(int ms, int side) {
    final api = _api, session = _session;
    if (api is HttpBrokerApi && session != null) {
      api.recordConnectStep(
        session.token,
        null,
        29,
        ok: false,
        elapsedMs: ms,
        reason: side,
      );
    }
  }

  void _syncFallback() {
    _syncStallWatch();
    final needed = _session != null && _foreground && _watch?.connected != true;
    if (!needed) {
      _poll?.cancel();
      _poll = null;
      return;
    }
    _poll ??= Timer.periodic(androidFallbackPollInterval, (_) {
      if (_foreground && !_streaming) unawaited(_refresh(quiet: true));
    });
  }

  @override
  void didChangeAppLifecycleState(AppLifecycleState state) {
    final returned = !_foreground;
    // Visible but not focused (split screen, a system dialog) still counts.
    // Set before the sign-in check: a sign-in that expired while the app was
    // away must not leave this false, or the list never updates by itself
    // after the next sign-in (inspection, 2026-11-12).
    _foreground =
        state == AppLifecycleState.resumed ||
        state == AppLifecycleState.inactive;
    // A sign-out that came while the app was hidden could not confirm that
    // its screens closed (that takes a frame). Confirm it now, so 확인 on the
    // sign-out screen works without pressing 다시 확인 first.
    if (_foreground && _sessionAccess.retirementRecoveryAvailable) {
      unawaited(_sessionAccess.retryRetirement());
    }
    if (!_sessionAccess.check()) return;
    if (_foreground) {
      _startWatch();
    } else {
      _stopWatch();
    }
    if (_foreground && !_streaming) unawaited(_refresh(quiet: true));
    if (_foreground && _session != null) unawaited(_checkUpdates());
    // Back from the background: fresh pictures at once.
    if (_foreground && returned) unawaited(_previews.poll());
  }

  @override
  void didChangeMetrics() => _readDensity();

  void _readDensity() => unawaited(
    AndroidScreenDensity.read().then((density) {
      if (density != null) _density = density;
    }),
  );

  Future<void> _restore() async {
    _readDensity();
    try {
      _showPreviews = await _store.launcherPreviews();
    } catch (_) {
      _showPreviews = true;
    }
    try {
      _version = await _store.appVersion();
    } catch (_) {
      _version = null;
    }
    try {
      // Audio is automatic on Android. Retire a legacy saved mute while
      // preserving the user's frame rate and bitrate choices.
      _quality = await _store.quality();
    } catch (_) {
      _quality = AndroidStreamQuality.recommended;
    }
    try {
      _sessionSettings = await _store.sessionSettings();
    } catch (_) {
      _sessionSettings = const AndroidSessionSettings();
    }
    try {
      final saved = await _store.origin();
      if (saved != null) await _selectOrigin(parseServerOrigin(saved));
    } catch (_) {
      _error = l10n.couldntLoadSavedServerSettings;
    }
    if (mounted) setState(() => _loading = false);
  }

  Future<void> _selectOrigin(Uri origin) async {
    _mfa.retire();
    ++_serverRevision;
    _versionRead = null;
    final request = ++_loginRequestRevision;
    ++_loginRestoreRevision;
    ++_memoryRevision;
    final nextApi =
        widget.apiFactory?.call(origin) ??
        androidBrokerApi(
          origin,
          diagnosticsVersion: () => androidDiagnosticVersion(_version),
        );
    if (!mounted || request != _loginRequestRevision) {
      nextApi.close();
      return;
    }
    _api?.close();
    _api = nextApi;
    _origin = origin;
    // The tested/saved server is authoritative even if optional credential
    // restoration fails. Never leave the previous server's API or fields.
    _id.clear();
    _password.clear();
    _remember = false;
    _endSessionAccess();
    _session = null;
    _desktops = [];
    _error = null;
    // An update offer belongs to the server it was read from.
    _update = null;
    _stopUpdateChecks();
    final input = _loginInputRevision, server = _serverRevision;
    bool current() =>
        mounted &&
        request == _loginRequestRevision &&
        _api == nextApi &&
        _origin == origin &&
        server == _serverRevision &&
        _session == null &&
        input == _loginInputRevision;
    try {
      final memory = await _store.login(origin);
      if (!current()) return;
      _id.text = memory.username;
      _password.text = memory.password ?? '';
      _remember = memory.password != null;
    } catch (_) {
      if (current()) {
        _error = l10n.couldntLoadSavedSignDetails;
      }
    }
  }

  Future<void> _server() async {
    final result = await showDialog<Uri>(
      context: context,
      barrierDismissible: false,
      builder: (_) =>
          AndroidServerDialog(initial: _origin, save: _store.saveOrigin),
    );
    if (result == null || !mounted) return;
    setState(() => _busy = true);
    try {
      await _selectOrigin(result);
    } catch (_) {
      _error = l10n.couldntLoadServerSettings;
    }
    if (mounted) setState(() => _busy = false);
  }

  void _idChanged(String _) {
    final revision = ++_memoryRevision;
    _password.clear();
    final origin = _origin;
    if (origin == null) return;
    unawaited(
      _store.forgetPassword(origin).catchError((Object _) {
        if (mounted && revision == _memoryRevision) {
          setState(() => _error = l10n.couldntTurnOffRememberPassword);
        }
      }),
    );
  }

  void _rememberChanged(bool value) {
    ++_loginInputRevision;
    final revision = ++_memoryRevision;
    setState(() => _remember = value);
    final origin = _origin;
    if (value || origin == null) return;
    unawaited(
      _store.forgetPassword(origin).catchError((Object _) {
        if (mounted && revision == _memoryRevision && origin == _origin) {
          setState(() => _error = l10n.couldntTurnOffRememberPassword);
        }
      }),
    );
  }

  Future<void> _login() async {
    if (_busy ||
        _api == null ||
        _origin == null ||
        _sessionAccess.reason != null) {
      return;
    }
    if (_id.text.trim().isEmpty || _password.text.isEmpty) {
      setState(() => _error = l10n.enterIdPassword);
      return;
    }
    _mfa.retire();
    FocusManager.instance.primaryFocus?.unfocus();
    final origin = _origin!;
    final username = _id.text.trim(), password = _password.text;
    final remember = _remember;
    final inputRevision = _loginInputRevision;
    final request = ++_loginRequestRevision;
    setState(() {
      _busy = true;
      _error = null;
    });
    final api = _api!;
    bool current() =>
        mounted &&
        _api == api &&
        _origin == origin &&
        request == _loginRequestRevision;
    BrokerSession? authenticated;
    try {
      try {
        await _authenticationRetirement;
        if (!current() || !mounted) return;
        authenticated = await confirmManualLogin(
          context,
          api: api,
          current: current,
          attempt: () => api is InteractiveLoginBrokerApi
              ? (api as InteractiveLoginBrokerApi).loginInteractively(
                  username,
                  password,
                )
              : api.login(username, password),
        );
      } on MfaRequiredException catch (required) {
        if (!mounted || !current()) {
          required.challenge.forget();
          return;
        }
        authenticated = await showMfaLogin(
          context,
          api: api,
          challenge: required.challenge,
          current: current,
        );
      }
      if (authenticated == null) return;
      if (!current()) {
        await api.logout(authenticated.token);
        return;
      }
      final desktops = await api.desktops(authenticated.token);
      if (!current()) {
        await api.logout(authenticated.token);
        return;
      }
      try {
        // A later edit/opt-out must not resurrect a deleted remembered secret.
        if (inputRevision == _loginInputRevision) {
          ++_memoryRevision;
          await _store.saveLogin(origin, username, password, remember);
        }
      } catch (_) {
        if (current()) _notice(l10n.signedButCouldntSaveRemember);
      }
      if (!current()) {
        await api.logout(authenticated.token);
        return;
      }
      setState(() {
        _session = authenticated;
        _hostUpdateReplies.clear();
        _desktops = desktops;
        _checkedAt = DateTime.now();
        _listError = null;
        _password.clear();
      });
      ProductLog.instance.record(LogEvent.signIn, {'ok': true});
      ProductLog.instance.record(LogEvent.inventory, {
        'ok': true,
        'available': desktops.length,
      });
      _keepAlive(authenticated);
      _startUpdateChecks();
      _startWatch();
      _bindPreviews();
    } catch (error) {
      if (authenticated != null) {
        try {
          await api.logout(authenticated.token);
        } catch (_) {}
      }
      if (current()) setState(() => _error = _message(error));
    } finally {
      if (current()) setState(() => _busy = false);
    }
  }

  /// A broker update restarting the server: list reads wait through it.
  late final _restartWait = ServerRestartWait(clock: () => widget.clock());
  Timer? _restartRetry;

  /// The wait began with a refresh the user pressed: its end is shown.
  bool _restartFromUser = false;

  Future<void> _refresh({bool quiet = false}) async {
    final api = _api, session = _session;
    if (api == null || session == null) return;
    if (_refreshing) {
      // A change pushed while a read is running is read right after it.
      if (quiet) _refreshAgain = true;
      return;
    }
    // A refresh the user pressed waits through a restart afresh.
    if (!quiet) _restartWait.reset();
    _restartRetry?.cancel();
    if (!quiet) unawaited(_checkUpdates());
    final started = DateTime.now();
    setState(() {
      _refreshing = true;
      if (!quiet) _listError = null;
    });
    try {
      final desktops = await api.desktops(session.token);
      if (!mounted || _session != session) return;
      setState(() {
        _desktops = desktops;
        // A read that began after a favourite answer carries it.
        _hostUpdateReplies.removeWhere((id, reply) {
          final listed = desktops.where((d) => d.id == id).firstOrNull;
          final incoming = listed?.hostUpdate.operation?.checkedAt;
          final previous = reply.$3.operation?.checkedAt;
          if (listed == null || !reply.$4.isAfter(started)) return true;
          final operation = listed.hostUpdate.operation;
          // An earlier read with no operation cannot erase a direct response.
          if (operation == null) return false;
          // Retain only when this incoming result is demonstrably older.
          return !(incoming != null &&
              previous != null &&
              incoming.isBefore(previous));
        });
        _favoriteDone.removeWhere((_, done) => done.$2.isBefore(started));
        _checkedAt = DateTime.now();
        _listError = null;
        _refreshFailed = false;
      });
      _previews.listed(_desktops.map(_machine));
      _restartWait.reset();
      _restartFromUser = false;
      _revision.value++;
      _advanceFlows();
    } catch (error) {
      if (!mounted || _session != session) return;
      if (error is BrokerException && error.status == 401) {
        _expired(error);
        return;
      }
      final wait = _restartWait.next(error);
      if (wait != null) {
        // No error while the server restarts: the list stays, a refresh the
        // user pressed says why, and the read is tried again by itself.
        if (!quiet && !_restartFromUser) _notice(serverRestartingMessage);
        if (!quiet) _restartFromUser = true;
        setState(() => _refreshFailed = true);
        _revision.value++;
        _restartRetry = Timer(wait, () {
          if (mounted && _session == session) unawaited(_refresh(quiet: true));
        });
        return;
      }
      final shown = !quiet || _restartFromUser || _desktops.isEmpty;
      _restartFromUser = false;
      setState(() {
        _refreshFailed = true;
        if (shown) _listError = _message(error);
      });
      _revision.value++;
    } finally {
      if (mounted) setState(() => _refreshing = false);
      if (mounted && _refreshAgain && !_streaming) {
        _refreshAgain = false;
        unawaited(_refresh(quiet: true));
      }
    }
  }

  // --- session lifetime ---------------------------------------------------

  SessionRenewer? _renewer;
  Timer? _expiry;
  bool _recovering = false;

  void _stopKeeping() {
    _expiry?.cancel();
    _expiry = null;
    _renewer?.stop();
  }

  /// Renews the same token before it expires; only when that cannot work the
  /// remembered password signs in again silently (see [_onLapse]).
  void _keepAlive(BrokerSession session) {
    _stopKeeping();
    final api = _api;
    final owner = SessionWindows.newAuthenticationOwner();
    _authenticationOwner = owner;
    _sessionAccess.start(
      session.sessionAccess,
      owner: owner,
      record: () async {
        if (_session != session ||
            _api != api ||
            api is! SessionActivityBrokerApi) {
          throw const BrokerException('session_expired', 401);
        }
        return (api as SessionActivityBrokerApi).recordHumanActivity(
          session.token,
        );
      },
      setIdle: api is SessionActivityBrokerApi
          ? (enabled) {
              if (_session != session || _api != api) {
                throw const BrokerException('session_expired', 401);
              }
              return (api as SessionActivityBrokerApi).setIdleEnabled(
                session.token,
                enabled,
              );
            }
          : null,
      onPolicy: (observed) async {
        if (_session != session || _authenticationOwner != owner) {
          throw const BrokerException('session_expired', 401);
        }
        if (Platform.isAndroid) {
          await AndroidApplication.channel.invokeMethod<void>(
            'authenticationDeadline',
            {
              'owner': owner,
              'idleRemainingMs': observed.remaining?.inMilliseconds,
            },
          );
        }
      },
      onTerminal: (code) => _retireAuthentication(session, code),
    );
    unawaited(
      AndroidApplication.channel
          .invokeMethod<void>('bindAuthentication', {
            'owner': owner,
            if (session.sessionAccess != null)
              'idleRemainingMs':
                  session.sessionAccess!.remaining?.inMilliseconds,
          })
          .catchError((Object _) {
            if (Platform.isAndroid &&
                mounted &&
                _session == session &&
                _authenticationOwner == owner) {
              _retireAuthentication(
                session,
                'session_reauthentication_required',
              );
            }
          }),
    );
    _renewer = SessionRenewer(
      lead: widget.sessionRenewalLead,
      retry: widget.sessionRetryDelay,
      clock: widget.clock,
      renew: () {
        final api = _api, current = _session;
        if (current == null) {
          throw const BrokerException('session_expired', 401);
        }
        if (api is! SessionRenewalBrokerApi) {
          throw const BrokerException('renew_unsupported', 404);
        }
        return (api as SessionRenewalBrokerApi).renewSession(current.token);
      },
      onRenewed: (next) {
        final current = _session;
        if (current == null || current != session) return;
        _session = current.withExpiry(next);
        _recordRenewal(1, true);
      },
      onLapse: (reason) {
        final current = _session;
        if (current == null) return;
        if (reason == RenewalLapse.replaced ||
            reason == RenewalLapse.idle ||
            reason == RenewalLapse.rejected) {
          _retireAuthentication(
            current,
            _renewer?.terminalCode ??
                (reason == RenewalLapse.rejected
                    ? 'session_expired'
                    : reason == RenewalLapse.idle
                    ? 'session_idle_timeout'
                    : 'pc_login_replaced'),
          );
          return;
        }
        _recordRenewal(
          1,
          false,
          code: switch (reason) {
            RenewalLapse.unsupported => 'request_failed',
            RenewalLapse.unreachable => 'network_error',
            _ => 'session_expired',
          },
        );
        unawaited(_sessionLost(current, renewFirst: false, early: true));
      },
    )..start(session.expiresAt);
  }

  void _recordRenewal(int stage, bool ok, {String? code, String? token}) {
    final api = _api, bound = token ?? _session?.token;
    if (api is SessionRenewalBrokerApi && bound != null) {
      (api as SessionRenewalBrokerApi).recordSessionRenewal(
        bound,
        stage: stage,
        ok: ok,
        code: code,
      );
    }
  }

  /// A 401 (or a renewal that cannot work): renew once, then sign in again
  /// with the remembered password; the sign-in screen only when neither works.
  void _expired([BrokerException? error]) {
    final session = _session;
    if (session == null) return;
    if (error != null &&
        (pcLoginTerminalCode(error.code, error.status) != null ||
            (error.status == 401 &&
                const {
                  'session_expired',
                  'authentication_required',
                }.contains(error.code)))) {
      _retireAuthentication(session, error.code);
      return;
    }
    unawaited(_sessionLost(session, renewFirst: true, early: false));
  }

  Future<void> _sessionLost(
    BrokerSession session, {
    required bool renewFirst,
    required bool early,
  }) async {
    if (_recovering || !mounted || _session != session) return;
    _recovering = true;
    try {
      if (renewFirst && _renewer != null) {
        if (await _renewer!.renewNow()) {
          _recovering = false;
          if (mounted && _session == session) {
            unawaited(_refresh(quiet: true));
          }
          return;
        }
        if (!mounted || _session != session) return;
      }
      if (session.sessionAccess != null) {
        final left = session.expiresAt.difference(widget.clock());
        if (left > Duration.zero) {
          _expiry?.cancel();
          _expiry = Timer(left, () {
            if (mounted && _session == session) {
              _retireAuthentication(session, 'session_expired');
            }
          });
        } else {
          _retireAuthentication(session, 'session_expired');
        }
        return;
      }
      for (var attempt = 0; ; attempt++) {
        final result = await _silentLogin(session);
        if (!mounted || _session != session) return;
        final stillValid = session.expiresAt.isAfter(widget.clock());
        if (result == _Silent.unreachable &&
            attempt < 7 &&
            (stillValid || attempt < 3)) {
          await Future<void>.delayed(widget.sessionRetryDelay);
          if (!mounted || _session != session) return;
          continue;
        }
        if (early && stillValid && result == _Silent.noPassword) {
          // Nothing to sign in with: the working session stays until expiry.
          _expiry?.cancel();
          _expiry = Timer(
            session.expiresAt.difference(widget.clock()),
            () => unawaited(_showExpired()),
          );
          return;
        }
        break;
      }
      await _showExpired();
    } finally {
      _recovering = false;
    }
  }

  Future<_Silent> _silentLogin(BrokerSession session) async {
    if (_mfa.blocked) { return _Silent.noPassword; }
    final api = _api, origin = _origin;
    if (api == null || origin == null) return _Silent.noPassword;
    final memory = _memoryRevision, request = _loginRequestRevision;
    final server = _serverRevision;
    bool current() =>
        mounted &&
        _session == session &&
        _api == api &&
        _origin == origin &&
        server == _serverRevision &&
        memory == _memoryRevision &&
        request == _loginRequestRevision;
    final AndroidLoginMemory saved;
    try {
      saved = await _store.login(origin);
    } catch (_) {
      return _Silent.noPassword;
    }
    final password = saved.password;
    if (password == null ||
        password.isEmpty ||
        saved.username.toLowerCase() != session.username.toLowerCase()) {
      return _Silent.noPassword;
    }
    if (!current()) return _Silent.noPassword;
    final BrokerSession next;
    try {
      if (_mfa.blocked) { return _Silent.noPassword; }
      _mfa.retire();
      next = await api.login(saved.username, password);
    } on MfaRequiredException catch (required) {
      required.challenge.forget();
      if (!current()) return _Silent.rejected;
      _recordRenewal(
        2,
        false,
        code: 'authentication_required',
        token: session.token,
      );
      return _Silent.noPassword;
    } on BrokerException catch (error) {
      if (!current()) return _Silent.rejected;
      if (pcLoginTerminalCode(error.code, error.status) != null) {
        _retireAuthentication(session, error.code);
        return _Silent.rejected;
      }
      final rejected =
          error.code == 'invalid_credentials' ||
          error.code == 'credentials_change_required';
      _recordRenewal(
        2,
        false,
        code: rejected ? 'authentication_required' : 'network_error',
        token: session.token,
      );
      if (rejected) {
        try {
          // Re-read ownership too: never delete a replacement account/secret.
          // This is a guard before dispatch, not an atomic native CAS.
          final latest = await _store.login(origin);
          if (current() &&
              latest.username == saved.username &&
              latest.password == password) {
            ++_memoryRevision;
            await _store.forgetPassword(origin);
          }
        } catch (_) {}
        return _Silent.rejected;
      }
      return _Silent.unreachable;
    } catch (_) {
      if (!current()) return _Silent.rejected;
      _recordRenewal(2, false, code: 'network_error', token: session.token);
      return _Silent.unreachable;
    }
    if (!current() || next.accountId != session.accountId) {
      unawaited(api.logout(next.token).catchError((Object _) {}));
      return _Silent.rejected;
    }
    setState(() => _session = next);
    // The live channel and the pictures were bound to the old token.
    _stopWatch();
    _startWatch();
    _bindPreviews();
    _keepAlive(next);
    await _refresh(quiet: true);
    _recordRenewal(2, true, token: next.token);
    return _Silent.signedIn;
  }

  void _retireAuthentication(BrokerSession session, String code) {
    _mfa.sessionRetired(session.token, code);
    if (!mounted || _session != session) return;
    final api = _api, owner = _authenticationOwner;
    _sessionAccess.retire(code);
    // The navigator-wide boundary removes every Flutter video from paint now.
    // Native console shielding is submitted before any asynchronous cleanup.
    final hiddenNative = Platform.isAndroid
        ? AndroidApplication.channel.invokeMethod<void>('hideAuthentication', {
            'owner': owner,
          })
        : Future<void>.value();
    unawaited(hiddenNative.catchError((Object _) {}));
    final route = _remoteRoute;
    _remoteRoute = null;
    ++_loginRequestRevision;
    ++_loginRestoreRevision;
    ++_connectionIntent;
    _stopKeeping();
    _stopWatch();
    _stopUpdateChecks();
    setState(() {
      _openingDesktop = null;
      _streaming = false;
      _session = null;
      _desktops = [];
      _flows.clear();
      _password.clear();
      _busy = false;
      _error = pcLoginTerminalMessage(code);
    });
    if (api != null) {
      unawaited(
        api
            .logout(session.token)
            .timeout(const Duration(seconds: 5))
            .catchError((Object _) {}),
      );
    }
    WidgetsBinding.instance.addPostFrameCallback((_) {
      if (mounted && _session == null && _authenticationOwner == owner) {
        unawaited(
          AndroidApplication.channel
              .invokeMethod<void>('authenticationShieldReady', {'owner': owner})
              .catchError((Object _) {}),
        );
      }
    });
    // The check can be made again, as on Windows (WorkspaceShell). Closing
    // the routes completes in a frame and no frame is drawn while the app is
    // hidden, so a sign-out during a background stream timed out as
    // unconfirmed and its 확인 never enabled (inspection, 2026-11-12). The
    // retry is the 다시 확인 button, and it runs by itself when the app comes
    // back (didChangeAppLifecycleState).
    final recoveryOwner = Object();
    var attempt = 0;
    bool current() =>
        mounted && _session == null && _authenticationOwner == owner;
    Future<void> observe({required bool initial}) async {
      if (!current()) return;
      final observed = ++attempt;
      final outcome = await observeSessionRetirement(
        routes: () async {
          await _sessionAccess.closeRoutes();
          if (route != null && route.isActive) {
            route.navigator?.removeRoute(route);
          }
        },
        // Retiring the same owner again is answered as the first time.
        native: () => initial || !Platform.isAndroid
            ? hiddenNative
            : AndroidApplication.channel.invokeMethod<void>(
                'hideAuthentication',
                {'owner': owner},
              ),
        consoles: () => const ConsoleBrowser.android().closeAllVerified(
          api: api,
          token: session.token,
        ),
      );
      if (!current() || observed != attempt) return;
      if (initial) await _restoreSavedLogin();
      if (!current() || observed != attempt) return;
      _sessionAccess.finishRetirement(
        verified: outcome.verified,
        outcome: outcome,
        recoveryOwner: recoveryOwner,
      );
    }

    _sessionAccess.bindRetirementRecovery(recoveryOwner, () {
      if (!current()) return Future<void>.value();
      final work = observe(initial: false);
      _authenticationRetirement = work;
      return work;
    });
    _authenticationRetirement = observe(initial: true);
  }

  /// Renewal and silent sign-in are both impossible: the sign-in screen with
  /// the remembered ID and password filled in exactly as at app start (the
  /// login clears the field after sign-in, so it is read from the store again).
  Future<void> _showExpired() async {
    final current = _session;
    if (current != null) {
      _retireAuthentication(current, 'session_expired');
      return;
    }
    _sessionAccess.stop();
    await const ConsoleBrowser.android().closeAll(
      api: _api,
      token: _session?.token,
    );
    ++_loginRequestRevision;
    _stopKeeping();
    if (!mounted) return;
    setState(() {
      _session = null;
      _desktops = [];
      _flows.clear();
      _error = l10n.signHasExpiredSignAgain;
    });
    _syncTicker();
    _stopUpdateChecks();
    await _restoreSavedLogin();
  }

  Future<void> _restoreSavedLogin() async {
    final origin = _origin;
    if (origin == null || _busy) return;
    final restore = ++_loginRestoreRevision;
    final input = _loginInputRevision, request = _loginRequestRevision;
    try {
      final saved = await _store.login(origin);
      if (!mounted ||
          _session != null ||
          _origin != origin ||
          _busy ||
          restore != _loginRestoreRevision ||
          input != _loginInputRevision ||
          request != _loginRequestRevision) {
        return;
      }
      setState(() {
        if (saved.username.isNotEmpty) _id.text = saved.username;
        _password.text = saved.password ?? '';
        _remember = saved.password != null;
      });
    } catch (_) {}
  }

  // --- in-app update -------------------------------------------------------

  /// After sign-in, every 10 minutes, on returning to the app and on
  /// 새로고침 the configured server's public catalog is read (nothing is
  /// downloaded) to badge the header's update button. Installing is the
  /// user's choice in the update panel. While the broker's live channel is
  /// connected it pushes a catalog notice (and the 10-minute timer rests);
  /// the timer is the fallback while the channel is down.
  void _startUpdateChecks() {
    _updateTimer?.cancel();
    _updateTimer = Timer.periodic(const Duration(minutes: 10), (_) {
      // Fallback only: with the live channel connected the broker says when
      // the catalog changed.
      if (_foreground && _session != null && _watch?.connected != true) {
        unawaited(_checkUpdates());
      }
    });
    unawaited(_checkUpdates());
  }

  /// The account's feeds stop; its pushed samples and pictures leave memory.
  void _stopUpdateChecks() {
    _pushed.clear();
    _previews.stop();
    _updateTimer?.cancel();
    _updateTimer = null;
    _stopWatch();
  }

  AndroidUpdates _updates(Uri origin) =>
      widget.updatesFactory?.call(origin) ?? AndroidUpdates(origin);

  Future<void> _checkUpdates() async {
    final origin = _origin;
    final scope = _serverRevision;
    if (_updateChecking || origin == null) return;
    setState(() => _updateChecking = true);
    final updates = _updates(origin);
    try {
      final version = _version ?? await _readRunningVersion(origin);
      if (!mounted ||
          _origin != origin ||
          scope != _serverRevision ||
          version == null) {
        return;
      }
      final abi = _abi ??= await _updatePlatform.abi();
      if (abi == null) throw const UpdateFailure('abi_unavailable');
      final release = await updates.check(version, abi);
      if (!mounted || _origin != origin || scope != _serverRevision) return;
      setState(() => _update = release);
    } catch (_) {
      // A failed background check keeps the last answer; the panel's own
      // check says why when the user opens it.
    } finally {
      updates.close();
      if (mounted) setState(() => _updateChecking = false);
    }
  }

  Future<String?> _readRunningVersion(Uri origin) {
    if (!mounted || _origin != origin) return Future<String?>.value(null);
    final scope = _serverRevision;
    final pending = _versionRead;
    if (pending != null && _versionReadScope == scope) return pending;
    final request = ++_versionReadRevision;
    _versionReadScope = scope;
    late final Future<String?> reading;
    reading = _fetchRunningVersion(origin, scope, request).whenComplete(() {
      if (identical(_versionRead, reading)) _versionRead = null;
    });
    _versionRead = reading;
    return reading;
  }

  Future<String?> _fetchRunningVersion(
    Uri origin,
    int scope,
    int request,
  ) async {
    String? version;
    try {
      version = await _store.appVersion();
      if (version != null) compareAndroidVersions(version, version);
    } catch (_) {
      version = null;
    }
    if (!mounted ||
        _origin != origin ||
        scope != _serverRevision ||
        request != _versionReadRevision) {
      return null;
    }
    setState(() {
      _version = version;
      if (version == null) _update = null;
    });
    return version;
  }

  /// The update panel: running and newest version, the notes and the whole
  /// version timeline; installing starts only from its 업데이트 button.
  Future<void> _openUpdate() async {
    final origin = _origin;
    final scope = _serverRevision;
    if (origin == null) return;
    await showDialog<void>(
      context: context,
      builder: (_) => AndroidUpdatePanel(
        server: origin.authority,
        current: _version,
        readCurrent: () => _readRunningVersion(origin),
        isCurrent: () =>
            mounted && _origin == origin && scope == _serverRevision,
        updates: () => _updates(origin),
        platform: _updatePlatform,
        onChecked: (newer) {
          if (mounted && _origin == origin && scope == _serverRevision) {
            setState(() => _update = newer);
          }
        },
        hostUpdates: _hostUpdatable,
        hostUpdatesChanged: _revision,
        onHostUpdate: (id) {
          final desktop = _find(id);
          if (desktop != null) unawaited(_hostUpdate(desktop));
        },
      ),
    );
  }

  /// Assigned desktops whose Host can be updated now (the ⋯ entry's rule).
  /// The header's update button badges them too and the update panel lists
  /// them (2026-11-03); the list push keeps both current.
  List<DesktopMachine> _hostUpdatable([Iterable<DesktopMachine>? machines]) =>
      _api is HostUpdateBrokerApi
      ? [
          for (final m in machines ?? _desktops.map(_machine))
            if (m.hostUpdate.offered) m,
        ]
      : const [];

  Future<void> _signOut() async {
    final session = _session;
    if (session == null) return;
    final confirmed = await showAppConfirm(
      context,
      icon: Icons.logout_rounded,
      title: l10n.signOut,
      subtitle: '${session.username} · ${_origin?.authority ?? l10n.server}',
      message: l10n.desktopsRunningWorkStayAs,
      confirmLabel: l10n.signOut2,
      confirmKey: const Key('confirm-sign-out'),
    );
    if (!confirmed || !mounted || _session != session) return;
    await _endSignIn(session);
  }

  /// Ends [session] here and at the server: 로그아웃, and 완전 종료 before
  /// the app closes. [limit]: how long the server's answer is waited for
  /// (the sign-in is over here either way).
  Future<void> _endSignIn(BrokerSession session, {Duration? limit}) async {
    _mfa.retire();
    await const ConsoleBrowser.android().closeAll(
      api: _api,
      token: session.token,
    );
    if (!mounted || _session != session) return;
    final request = ++_loginRequestRevision;
    final api = _api, origin = _origin;
    ++_loginRestoreRevision;
    _endSessionAccess();
    setState(() {
      _session = null;
      _desktops = [];
      _flows.clear();
      _busy = true;
      _signingOut = true;
    });
    _syncTicker();
    _stopUpdateChecks();
    _stopKeeping();
    try {
      final sent = api?.logout(session.token);
      await (limit == null ? sent : sent?.timeout(limit));
    } catch (_) {}
    _signingOut = false;
    if (!mounted ||
        request != _loginRequestRevision ||
        _api != api ||
        _origin != origin ||
        _session != null) {
      return;
    }
    if (mounted) setState(() => _busy = false);
    await _restoreSavedLogin();
  }

  void _endSessionAccess() {
    final owner = _authenticationOwner;
    _sessionAccess.stop();
    if (owner == null || !Platform.isAndroid) return;
    unawaited(
      AndroidApplication.channel
          .invokeMethod<void>('hideAuthentication', {'owner': owner})
          .then((_) {
            WidgetsBinding.instance.addPostFrameCallback((_) {
              if (mounted &&
                  _session == null &&
                  _authenticationOwner == owner) {
                unawaited(
                  AndroidApplication.channel
                      .invokeMethod<void>('authenticationShieldReady', {
                        'owner': owner,
                      })
                      .catchError((Object _) {}),
                );
              }
            });
            WidgetsBinding.instance.ensureVisualUpdate();
          })
          .catchError((Object _) {}),
    );
  }

  /// The newest result replaces the one on screen instead of queueing behind
  /// it, and stays long enough to read.
  void _notice(String message, {SnackBarAction? action}) {
    ScaffoldMessenger.maybeOf(context)
      ?..hideCurrentSnackBar()
      ..showSnackBar(
        SnackBar(
          content: Text(message),
          action: action,
          duration: androidNoticeDuration(message, const Duration(seconds: 4)),
        ),
      );
  }

  bool _notificationNoticeShown = false;

  Future<void> _prepareNotification(bool Function() current) async {
    if (!Platform.isAndroid) return;
    final allowed =
        await AndroidSessionKeepalive.requestNotificationPermission();
    if (!mounted || !current() || allowed) return;
    // Once per app run: shown at every connection, it lay over each new
    // stream.
    if (_notificationNoticeShown) return;
    _notificationNoticeShown = true;
    _notice(
      l10n.notificationsOffSoConnectionStatus,
      action: SnackBarAction(
        label: l10n.notificationSettings,
        onPressed: () {
          unawaited(AndroidSessionKeepalive.openNotificationSettings());
        },
      ),
    );
  }

  String _message(Object error) => error is BrokerException
      ? error.message
      : l10n.couldntProcessRequestCheckNetwork;

  DesktopMachine _machine(BrokerDesktop desktop) {
    var machine = machineFromBroker(
      desktop,
      powerPending: _powerPending.containsKey(desktop.id),
      canForceStop: _powerPending.containsKey(desktop.id)
          ? _powerCanStop[desktop.id] ?? false
          : null,
    ).withResources(_resourcesOf(desktop));
    final reply = _hostUpdateReplies[desktop.id];
    if (reply != null &&
        reply.$1 == _session &&
        reply.$2 == _api &&
        reply.$5 == _serverRevision) {
      machine = machine.withHostUpdate(reply.$3);
    }
    final mark = _favoritePending[desktop.id] ?? _favoriteDone[desktop.id]?.$1;
    final organized = mark == null
        ? machine
        : machine.withOrganize(machine.organize.withFavorite(mark));
    // The last refresh failed: the cards show the configured size, not
    // usage that is no longer current.
    return _refreshFailed
        ? organized.withResources(organized.resources.asStale())
        : organized;
  }

  /// After an RDP session the VM's Windows session lives in RDP: the broker
  /// then reports the console hand-over (as for the Windows client), which a
  /// press on 스트리밍 연결 performs first. Without it the tile would fall
  /// through to "다시 시작" (2026-10-27: an RDP session meant a reboot).
  DesktopReadiness _readiness(DesktopMachine machine) => DesktopReadiness.of(
    machine,
    consoleRecovery: _api is ConsoleRecoveryApi,
    server: _origin?.authority ?? l10n.server,
  );

  BrokerDesktop? _find(String id) {
    final matches = _desktops.where((desktop) => desktop.id == id);
    return matches.length == 1 ? matches.single : null;
  }

  // --- connections --------------------------------------------------------

  String? _consoleUnavailable(DesktopMachine machine) {
    if (_openingDesktop != null) return l10n.anotherConnectionOpening;
    if (machine.powerBusy || machine.display.busy) return l10n.desktopTaskProgress;
    if (machine.powerState == 'stopped') return l10n.startDesktopThenConnect;
    if (machine.use.active) return l10n.endCurrentConnectionThenConnect;
    return null;
  }

  /// 'Console 끊고 연결' (2026-11-09): after one confirmation that names both
  /// acts, end the VM's console connections, then connect as soon as the
  /// tile is ready (the console's display reservation clears with it), as
  /// the Windows launcher does (WorkspaceShell._endConsolesAndConnect). The
  /// wait is the list's ordinary start flow: it shows on the tile, can be
  /// cancelled there and gives up after [_startTimeout].
  Future<void> _endConsolesAndConnect(BrokerDesktop desktop) async {
    final api = _api, login = _session, origin = _origin;
    if (api is! ConsoleTakeoverBrokerApi ||
        login == null ||
        _streamConnectPromptOpen ||
        _openingDesktop != null ||
        _flows.containsKey(desktop.id)) {
      return;
    }
    final intent = ++_connectionIntent, server = _serverRevision;
    bool current() =>
        mounted &&
        intent == _connectionIntent &&
        _session == login &&
        _api == api &&
        _origin == origin &&
        _serverRevision == server &&
        _openingDesktop == null &&
        _sessionAccess.check() &&
        login.expiresAt.isAfter(widget.clock());
    _streamConnectPromptOpen = true;
    var accepted = false;
    try {
      accepted = await showAppConfirm(
        context,
        title: l10n.disconnectConsoleConnect,
        subtitle: desktop.name,
        icon: Icons.swap_horiz_rounded,
        message: l10n.openConsoleConnectionWillEnd,
        confirmLabel: l10n.disconnectConnect,
        confirmKey: Key('confirm-end-console-${desktop.id}'),
      );
    } finally {
      _streamConnectPromptOpen = false;
    }
    if (!accepted || !current()) return;
    try {
      await (api as ConsoleTakeoverBrokerApi).endConsoles(
        login.token,
        desktop.id,
      );
    } catch (error) {
      if (mounted) {
        ScaffoldMessenger.of(context).showSnackBar(
          SnackBar(
            content: KeepAllText(
              error is BrokerException ? error.message : l10n.couldntEndConsoleConnection,
            ),
          ),
        );
        unawaited(_refresh(quiet: true));
      }
      return;
    }
    if (!current() || _flows.containsKey(desktop.id)) return;
    setState(() => _flows[desktop.id] = _StartFlow()..powered = true);
    _syncTicker();
    unawaited(_refresh(quiet: true));
  }

  Future<void> _openBrowserConsole(BrokerDesktop desktop) async {
    final api = _api, login = _session, origin = _origin;
    if (_openingDesktop != null ||
        api == null ||
        login == null ||
        origin == null) {
      return;
    }
    final intent = ++_connectionIntent, server = _serverRevision;
    setState(() => _openingDesktop = desktop.id);
    bool owner() =>
        mounted && intent == _connectionIntent && _openingDesktop == desktop.id;
    bool current() =>
        owner() &&
        _api == api &&
        _session == login &&
        _origin == origin &&
        _serverRevision == server;
    try {
      if (origin.scheme != 'https') {
        throw const BrokerException('console_browser_https_required');
      }
      final confirmed = await showAppConfirm(
        context,
        dialogKey: const Key('console-connect-confirm-dialog'),
        icon: Icons.settings_remote_outlined,
        title: l10n.openConsole,
        message:
            l10n.opensDesktopsProxmoxConsoleApp(desktop.name),
        confirmLabel: l10n.openConsole2,
        confirmKey: Key('confirm-console-${desktop.id}'),
        cancelKey: const Key('cancel-console-connect'),
      );
      if (!confirmed || !current()) return;
      final assigned = await api.desktops(login.token);
      if (!current()) return;
      final matches = assigned.where((item) => item.id == desktop.id).toList();
      if (matches.length != 1 || matches.single.kind != 'vm') {
        throw const BrokerException('desktop_not_found', 404);
      }
      ConsoleBrowser.requireSupport(
        origin,
        supported: matches.single.consoleWebViewSupported,
      );
      final machine = _machine(matches.single);
      if (machine.powerBusy || machine.display.busy || machine.use.active) {
        throw const BrokerException('console_busy', 409);
      }
      if (machine.powerState == 'stopped') {
        throw const BrokerException('console_vm_unavailable', 409);
      }
      if (api is! ConsoleWebViewBrokerApi) {
        throw const BrokerException('console_webview_update_required');
      }
      await const ConsoleBrowser.android().open(
        origin,
        desktop.id,
        api: api as ConsoleWebViewBrokerApi,
        token: login.token,
        current: current,
      );
    } on BrokerException catch (error) {
      if (current()) {
        _notice(error.message);
        if (error.status == 401) _expired(error);
      }
    } catch (_) {
      if (current()) _notice(l10n.couldntOpenConsoleCheckNetwork);
    } finally {
      if (owner()) {
        setState(() => _openingDesktop = null);
        if (_session == login && _api == api) await _refresh(quiet: true);
      }
    }
  }

  Future<void> _connect(BrokerDesktop desktop) async {
    final session = _session, api = _api;
    if (_openingDesktop != null || session == null || api == null) return;
    final assigned = _find(desktop.id);
    if (assigned == null || assigned.kind != desktop.kind) return;
    await _requestStreamConnect(assigned);
  }

  /// Connects as soon as the previous connection's cleanup clears.
  Future<void> _settleAndConnect(BrokerDesktop desktop) async {
    await _requestStreamConnect(desktop, settle: true);
  }

  /// One fresh user intent, before remote routing or a queued cleanup flow.
  /// Automatic resumes/retries stay inside the already approved remote view.
  Future<void> _requestStreamConnect(
    BrokerDesktop desktop, {
    bool settle = false,
  }) async {
    final session = _session, api = _api, origin = _origin;
    if (session == null || api == null) return;
    // Each step that makes no request leaves a record (recordConnectStep).
    void step(int stage, {bool ok = true, int reason = 0}) {
      if (api is HttpBrokerApi) {
        api.recordConnectStep(
          session.token,
          desktop.id,
          stage,
          ok: ok,
          reason: reason,
        );
      }
    }

    if (_streamConnectPromptOpen ||
        _openingDesktop != null ||
        _flows.containsKey(desktop.id)) {
      step(
        27,
        ok: false,
        reason: _streamConnectPromptOpen
            ? 1
            : _openingDesktop != null
            ? 2
            : 3,
      );
      return;
    }
    final assigned = _find(desktop.id);
    if (assigned == null || assigned.kind != desktop.kind) return;
    bool eligible(BrokerDesktop value) {
      final machine = _machine(value);
      final readiness = _readiness(machine);
      return !machine.powerBusy &&
          !machine.display.busy &&
          (readiness.canStream || readiness.settling);
    }

    if (!eligible(assigned)) {
      step(27, ok: false, reason: 4);
      return;
    }
    step(20);
    final intent = ++_connectionIntent, server = _serverRevision;
    bool current() =>
        mounted &&
        intent == _connectionIntent &&
        _session == session &&
        _api == api &&
        _origin == origin &&
        _serverRevision == server &&
        _openingDesktop == null &&
        _sessionAccess.check() &&
        session.expiresAt.isAfter(widget.clock());
    final asksHandOver = !settle && _handOverNeeded(_machine(assigned));
    _streamConnectPromptOpen = true;
    var accepted = false;
    try {
      // One question (2026-11-11: it asked twice): when the Windows screen has
      // to be brought over from RDP, this question is that one.
      accepted = asksHandOver
          ? await _askHandOver(assigned)
          : await showAppConfirm(
              context,
              dialogKey: const Key('connect-confirm-dialog'),
              title: l10n.connect(assigned.name),
              icon: Icons.desktop_windows_rounded,
              confirmLabel: l10n.connect2,
              confirmKey: Key('confirm-connect-${assigned.id}'),
              cancelKey: Key('cancel-connect-${assigned.id}'),
            );
    } finally {
      _streamConnectPromptOpen = false;
    }
    step(21, ok: accepted);
    if (!accepted || !current()) return;
    final latest = _find(desktop.id);
    if (latest == null || latest.kind != assigned.kind || !eligible(latest)) {
      step(27, ok: false, reason: 4);
      return;
    }
    if (!settle) {
      await _open(latest, handOverAgreed: asksHandOver);
      return;
    }
    setState(
      () => _flows[desktop.id] = _StartFlow()
        ..powered = true
        ..settle = true,
    );
    _syncTicker();
    _advanceFlows();
  }

  Future<void> _qualityConnect(BrokerDesktop desktop) async {
    final session = _session, api = _api;
    if (_openingDesktop != null || session == null || api == null) return;
    final intent = ++_connectionIntent;
    bool current() =>
        mounted &&
        _session == session &&
        _api == api &&
        intent == _connectionIntent &&
        _openingDesktop == null;
    final selected = await showDialog<StreamOptions>(
      context: context,
      builder: (_) => AndroidStreamDialog(desktop: desktop, initial: _quality),
    );
    if (selected == null || !current()) return;
    final quality = AndroidStreamQuality(
      fps: selected.fps,
      bitrate: selected.automaticBitrate
          ? AndroidStreamQuality.automatic
          : selected.bitrate ~/ 1000000,
    );
    _quality = quality;
    try {
      await _store.saveQuality(quality);
    } catch (_) {
      if (current()) _notice(l10n.couldntSaveStreamingSettingsThey);
    }
    if (current()) await _requestStreamConnect(desktop);
  }

  /// The VM's Windows session is still in RDP and the broker offers the
  /// console hand-over (the Windows client's ConsoleResumeDialog flow).
  bool _handOverNeeded(DesktopMachine machine) =>
      _api is ConsoleRecoveryApi &&
      !machine.isReady &&
      machine.canRecoverConsole;

  /// One explicit hand-over request, then read-only observation until the
  /// console and the virtual display are really ready (ConsoleResume, as on
  /// Windows). Returns the refreshed machine.
  Future<DesktopMachine> _prepareConsole(String desktopId) async {
    final session = _session, api = _api;
    if (session == null || api == null) {
      throw const BrokerException('session_expired', 401);
    }
    if (api is! ConsoleRecoveryApi) {
      throw const BrokerException('console_recovery_unavailable');
    }
    final desktop = _find(desktopId);
    if (desktop == null) throw const BrokerException('desktop_not_found', 404);
    if (_powerPending.containsKey(desktopId) ||
        !_machine(desktop).canRecoverConsole) {
      throw const BrokerException('console_busy');
    }
    try {
      final result = await ConsoleResume(
        api: api as ConsoleRecoveryApi,
        loadDesktops: () => api.desktops(session.token),
        authorized: () => mounted && _session == session && _api == api,
      ).run(session.token, desktopId);
      if (!mounted || _session != session) {
        throw const BrokerException('session_expired', 401);
      }
      setState(
        () => _desktops = [
          for (final item in _desktops)
            if (item.id == desktopId) result else item,
        ],
      );
      _revision.value++;
      return _machine(result);
    } on BrokerException catch (error) {
      if (error.status == 401 && mounted && _session == session) {
        _expired(error);
      }
      rethrow;
    }
  }

  /// The one question before a Windows screen held by RDP is brought over.
  Future<bool> _askHandOver(BrokerDesktop desktop) => showAppConfirm(
    context,
    icon: Icons.swap_horiz_rounded,
    title: l10n.bringOverWindowsScreen,
    message: l10n.continuesWindowsScreenYouWere(desktop.name),
    confirmLabel: l10n.bringOverConnect,
    confirmKey: Key('confirm-console-handover-${desktop.id}'),
    cancelKey: const Key('cancel-console-handover'),
  );

  /// [handOverAgreed]: the launcher's question was already this one.
  Future<void> _open(
    BrokerDesktop desktop, {
    bool handOverAgreed = false,
  }) async {
    final api = _api, session = _session;
    if (_openingDesktop != null || api == null || session == null) return;
    final assigned = _find(desktop.id);
    if (assigned == null || assigned.kind != desktop.kind) return;
    desktop = assigned;
    final intent = ++_connectionIntent;
    setState(() {
      _openingDesktop = desktop.id;
      _streaming = true;
    });
    bool owner() =>
        mounted && intent == _connectionIntent && _openingDesktop == desktop.id;
    bool current() =>
        owner() &&
        _session == session &&
        _api == api &&
        _find(desktop.id)?.kind == desktop.kind;
    bool authorized() =>
        current() &&
        _sessionAccess.check() &&
        _session?.expiresAt.isAfter(widget.clock()) == true;
    final asked = Stopwatch()..start();
    void step(int stage, {bool ok = true, int reason = 0}) {
      if (api is HttpBrokerApi) {
        api.recordConnectStep(
          session.token,
          desktop.id,
          stage,
          ok: ok,
          elapsedMs: asked.elapsedMilliseconds,
          reason: reason,
        );
      }
    }

    step(22);
    try {
      await _prepareNotification(current);
      step(23, ok: mounted && current());
      if (!mounted || !current()) return;
      desktop = _find(desktop.id)!;
      // A controller may block output or Windows-console preparation before
      // connect can return its reviewed takeover offer. Observe first; a busy
      // controller belongs in the remote view's explicit confirmation flow.
      var occupied = false;
      if (_handOverNeeded(_machine(desktop)) &&
          api is StreamTakeoverPreflightBrokerApi) {
        try {
          await AndroidStreamPreparation(
            api: api,
            authorized: authorized,
          ).checkTakeover(session.token, desktop.id);
        } on BrokerException catch (error) {
          if (error.status != 409 || error.code != 'desktop_busy') rethrow;
          occupied = true;
        }
        if (!mounted || !current()) return;
      }
      // Every entry path shares this consent: quick connect, quality dialog,
      // wake and queued cleanup. Ordinary ready connections ask no question.
      if (!occupied && _handOverNeeded(_machine(desktop))) {
        final agreed = handOverAgreed || await _askHandOver(desktop);
        if (!agreed || !current()) return;
        // The modal may have stayed open while access or the Windows state
        // changed. Re-read before the one console-transfer POST.
        final List<BrokerDesktop> listed;
        try {
          listed = await api
              .desktops(session.token)
              .timeout(const Duration(seconds: 15));
        } catch (error) {
          if (current()) {
            _notice(_message(error));
            if (error is BrokerException && error.status == 401) {
              _expired(error);
            }
          }
          return;
        }
        if (!mounted || !current()) return;
        final matches = listed.where((item) => item.id == desktop.id).toList();
        if (matches.length != 1 || matches.single.kind != desktop.kind) {
          _notice(const BrokerException('desktop_not_found', 404).message);
          return;
        }
        desktop = matches.single;
        setState(
          () => _desktops = [
            for (final item in _desktops)
              if (item.id == desktop.id) desktop else item,
          ],
        );
        if (_handOverNeeded(_machine(desktop))) {
          final resumed = await showDialog<DesktopMachine>(
            context: context,
            barrierDismissible: false,
            builder: (_) => ConsoleResumeDialog(
              machine: _machine(desktop),
              resume: () {
                if (!current()) {
                  throw const BrokerException('session_expired', 401);
                }
                return _prepareConsole(desktop.id);
              },
            ),
          );
          if (resumed == null || !current()) return;
          desktop = _find(desktop.id)!;
        }
      }
      if (!mounted || !current()) return;
      final remoteRoute = MaterialPageRoute<void>(
        builder: (_) => AndroidRemoteView(
          onAuthenticationEnded: (code) => _retireAuthentication(session, code),
          onBrokerNotices: () => _notices?.signal(),
          name: desktop.name,
          desktopId: desktop.id,
          server: _origin?.origin,
          // Every entry path (quick, queued or quality dialog) requests
          // audio; Host capability and runtime diagnostics still decide
          // whether sound is actually available.
          options: _quality.options,
          signinPromotionAuthorized: authorized,
          refreshSigninDesktop: () async {
            if (!authorized()) {
              throw const BrokerException('session_expired', 401);
            }
            final listed = await api.desktops(session.token);
            if (!authorized()) {
              throw const BrokerException('session_expired', 401);
            }
            final matches = listed.where((value) => value.id == desktop.id);
            if (matches.length != 1 || matches.single.kind != desktop.kind) {
              throw const BrokerException('desktop_not_found', 404);
            }
            return _machine(matches.single);
          },
          maxBitrate: desktop.maxBitrate,
          driveUnavailable: desktop.driveUnavailable,
          onConnectStep: (stage, ok, reason) =>
              step(stage, ok: ok, reason: reason),
          // 설정 in the session: remembered for the next connection too.
          onOptions: (options) {
            _quality = AndroidStreamQuality.of(options);
            unawaited(_store.saveQuality(_quality).catchError((Object _) {}));
          },
          token: session.token,
          settings: _sessionSettings,
          onSettings: (settings) {
            _sessionSettings = settings;
            unawaited(
              _store.saveSessionSettings(settings).catchError((Object _) {}),
            );
          },
          onStats: api is HttpBrokerApi
              ? (values, sessionId) => api.recordStreamStats(
                  values,
                  sessionId: sessionId,
                  desktopId: desktop.id,
                )
              : null,
          displayScale: desktop.displayScale,
          streamSize:
              desktop.streamSize &&
              desktop.initialViewport &&
              desktop.viewportEncoderLimits,
          streamSizeV2:
              desktop.streamSizeV2 &&
              desktop.initialViewport &&
              desktop.viewportEncoderLimits,
          initialDensity: _density,
          network: AndroidNetworkWatch(),
          load: (requested, open, scale) {
            step(25);
            return AndroidStreamPreparation(
              api: api,
              authorized: () => open() && authorized(),
            ).connect(
              session.token,
              desktop.id,
              requested,
              displayScale: scale,
            );
          },
          takeover: (offer, open) => AndroidStreamPreparation(
            api: api,
            authorized: () => open() && authorized(),
          ).takeover(session.token, desktop.id, offer: offer),
          awaitBrokerRestart: (open, limit) => androidAwaitBrokerRestart(
            api,
            session.token,
            _machine(desktop),
            current: () => open() && current(),
            limit: limit,
          ),
          awaitHostUpdate: (open, limit) => androidAwaitBrokerRestart(
            api,
            session.token,
            _machine(desktop),
            current: () => open() && current(),
            limit: limit,
            hostUpdating: true,
          ),
          awaitHostReturn: (open, limit, away) => androidAwaitBrokerRestart(
            api,
            session.token,
            _machine(desktop),
            current: () => open() && current(),
            limit: limit,
            hostAwayWait: AndroidRemoteView.hostAwayWait,
            onHostAway: away,
          ),
        ),
      );
      _remoteRoute = remoteRoute;
      step(24);
      await Navigator.of(context).push(remoteRoute);
      if (_remoteRoute == remoteRoute) _remoteRoute = null;
    } finally {
      // The session's last statistics window and end record go out now.
      if (api is HttpBrokerApi) unawaited(api.flushDiagnostics());
      if (owner()) {
        setState(() {
          _openingDesktop = null;
          _streaming = false;
          // The wait that led to this stream (켜고 연결, 정리 후 연결) ends with
          // it. Kept until the list was read again, the card showed its step
          // line with the whole session's time and 취소 for a moment.
          if (_flows[desktop.id]?.connecting == true) {
            _flows.remove(desktop.id);
          }
        });
        _syncTicker();
        if (_session == session) await _refresh(quiet: true);
      }
      // What the tile says once the list was read again: a desktop that
      // cannot be connected after its stream ended shows here.
      final after = mounted ? _find(desktop.id) : null;
      step(
        26,
        ok: after != null && _readiness(_machine(after)).canStream,
      );
    }
  }

  // --- power --------------------------------------------------------------

  /// One broker-authorized power action, followed until Proxmox reports its
  /// task result (the same contract as the Windows client). A 정지 the
  /// broker allows over this account's own unfinished 종료 supersedes that
  /// request's polling.
  Future<void> _runPower(BrokerDesktop desktop, String action) async {
    final api = _api, session = _session;
    if (!mounted || api == null || session == null) {
      throw const BrokerException('session_expired', 401);
    }
    final origin = _origin, owner = _authenticationOwner;
    final request = _loginRequestRevision, server = _serverRevision;
    // Auth identity may outlive a renewed expiry, but not a new owner.
    bool actor() =>
        mounted &&
        _api == api &&
        _session == session &&
        _origin == origin &&
        _authenticationOwner == owner &&
        _loginRequestRevision == request &&
        _serverRevision == server;
    if (!clientVmPowerActions.contains(action)) {
      throw const BrokerException('invalid_power_action');
    }
    final id = desktop.id;
    final machine = _machine(desktop);
    final pc = machine.kind == MachineKind.pc;
    final replacing = action == 'stop' && machine.canStop;
    final refused = machine.display.busy
        ? 'display_busy'
        // An approved PC (pc-power-v1): the Windows client's rule.
        : pc
        ? machine.pcPowerRefusal(action, pending: _powerPending.containsKey(id))
        : machine.powerStale || machine.powerState == 'unknown'
        ? 'power_status_unavailable'
        : !replacing && (_powerPending.containsKey(id) || machine.powerBusy)
        ? 'power_in_progress'
        : (action == 'start'
              ? machine.powerState != 'stopped'
              : machine.powerState != 'running')
        ? 'invalid_power_state'
        // A broker before client stop never reports the permission.
        : action == 'stop' && !replacing
        ? 'invalid_power_action'
        : null;
    if (refused != null) throw BrokerException(refused);
    final operation = ++_powerSequence;
    bool current() => _powerPending[id] == operation;
    setState(() {
      _powerPending[id] = operation;
      _powerCanStop[id] = false;
    });
    // A PC woken by Wake-on-LAN boots Windows first (the broker waits 4 min).
    final deadline = DateTime.now().add(
      Duration(minutes: pc && action == 'start' ? 5 : 2),
    );
    try {
      var result = await api.requestPower(session.token, id, action);
      while (true) {
        if (!actor()) {
          throw const BrokerException('session_expired', 401);
        }
        if (!current()) throw const BrokerException('power_superseded');
        if (_powerCanStop[id] != result.canForceStop) {
          setState(() => _powerCanStop[id] = result.canForceStop);
        }
        final task = result.task;
        if (task == null) throw const BrokerException('invalid_response');
        if (task.status == 'stopped') {
          if (task.exitStatus != 'OK') {
            throw BrokerException(
              pc && pcPowerTaskCodes.contains(task.exitStatus)
                  ? task.exitStatus!
                  : 'power_task_failed',
            );
          }
          return;
        }
        if (task.status == 'unknown') {
          throw BrokerException(pc ? 'host_no_response' : 'power_task_unknown');
        }
        if (task.status == 'superseded') {
          throw const BrokerException('power_superseded');
        }
        if (DateTime.now().isAfter(deadline)) {
          throw const BrokerException('power_task_timeout');
        }
        await Future<void>.delayed(const Duration(seconds: 1));
        if (!current()) throw const BrokerException('power_superseded');
        if (!actor()) throw const BrokerException('session_expired', 401);
        try {
          result = await api.power(session.token, id, taskId: task.id);
        } on BrokerException catch (error) {
          if (error.status >= 500 && DateTime.now().isBefore(deadline)) {
            continue;
          }
          rethrow;
        }
      }
    } finally {
      // A superseded request leaves the state to the one that replaced it.
      if (mounted && current()) {
        setState(() {
          _powerPending.remove(id);
          _powerCanStop.remove(id);
        });
        if (actor()) await _refresh(quiet: true);
      }
    }
  }

  Future<void> _power(BrokerDesktop desktop, String action) async {
    final api = _api, session = _session;
    final origin = _origin, owner = _authenticationOwner;
    final request = _loginRequestRevision, server = _serverRevision;
    if (!mounted || api == null || session == null || origin == null) return;
    // A retired confirmation or reply cannot act for a replacement login.
    bool current() =>
        mounted &&
        _api == api &&
        _session == session &&
        _origin == origin &&
        _authenticationOwner == owner &&
        _loginRequestRevision == request &&
        _serverRevision == server;
    final label = powerActionLabel(action);
    final pc = desktop.kind == 'pc';
    // An offline PC the server cannot wake: the reason, not a question.
    if (pc && action == 'start' && !desktop.pcPower.wake) {
      await showPcWakeHelp(
        context,
        reason: pcWakeFailure('pc_wake_unavailable')!,
      );
      return;
    }
    if (!await confirmPowerAction(
          context,
          action: action,
          desktopId: desktop.id,
          name: desktop.name,
          pc: pc,
        ) ||
        !current()) {
      return;
    }
    try {
      await _runPower(desktop, action);
    } catch (error) {
      if (!current()) return;
      if (error is BrokerException && error.code == 'power_superseded') return;
      final wake = error is BrokerException ? pcWakeFailure(error.code) : null;
      if (pc && wake != null) {
        if (mounted) await showPcWakeHelp(context, reason: wake);
        return;
      }
      if (error is BrokerException && error.status == 401) {
        if (mounted && _session != null) _expired(error);
        return;
      }
      final message = error is BrokerException
          ? switch (error.code) {
              'session_cleanup_pending' =>
                l10n.waitingPreviousConnectionCleanUp(label),
              'display_in_progress' || 'display_busy' =>
                l10n.numberMonitorsChangingSoRequest(label),
              'host_update_in_progress' =>
                l10n.hostUpdateInstallingSoRequest(label),
              _ => error.message,
            }
          : l10n.couldntProcessRequestTryAgain(label);
      if (mounted) _notice(message);
    }
  }

  // --- power on, then connect --------------------------------------------

  void _syncTicker() {
    if (_flows.isEmpty) {
      _ticker?.cancel();
      _ticker = null;
      return;
    }
    var ticks = 0;
    _ticker ??= Timer.periodic(const Duration(seconds: 1), (_) {
      if (!mounted) return;
      final expired = <String>[];
      for (final entry in _flows.entries) {
        entry.value.seconds++;
        if (!entry.value.connecting &&
            entry.value.seconds > _startTimeout.inSeconds) {
          expired.add(entry.key);
        }
      }
      setState(() {
        for (final id in expired) {
          _flows.remove(id);
        }
      });
      if (expired.isNotEmpty) {
        _notice(l10n.autoConnectStoppedBecauseDesktop);
      }
      // While Windows starts, the list is read every 3 s (read-only); a
      // cleanup normally clears in a second or two, so every 2 s for that.
      final waiting = _flows.values.any((f) => f.powered && !f.connecting);
      final settling = _flows.values.any((f) => f.settle && !f.connecting);
      if (waiting && ++ticks % (settling ? 2 : 3) == 0 && !_streaming) {
        unawaited(_refresh(quiet: true));
      }
      _syncTicker();
    });
  }

  void _cancelFlow(String id) {
    if (_flows.remove(id) == null) return;
    setState(() {});
    _syncTicker();
    _notice(l10n.autoConnectCanceled);
  }

  void _advanceFlows() {
    if (_streamConnectPromptOpen) return;
    for (final entry in _flows.entries.toList()) {
      final flow = entry.value;
      if (flow.connecting || !flow.powered || _openingDesktop != null) continue;
      final desktop = _find(entry.key);
      if (desktop == null) continue;
      final readiness = _readiness(_machine(desktop));
      // The cleanup stopped or failed: the tile now explains it instead.
      if (flow.settle && !readiness.settling && !readiness.canStream) {
        _flows.remove(entry.key);
        _syncTicker();
        WidgetsBinding.instance.addPostFrameCallback((_) {
          if (mounted) setState(() {});
        });
        continue;
      }
      if (!readiness.canStream) continue;
      flow.connecting = true;
      WidgetsBinding.instance.addPostFrameCallback((_) {
        if (!mounted || _flows[entry.key] != flow) return;
        unawaited(
          _open(desktop).whenComplete(() {
            if (!mounted || _flows[entry.key] != flow) return;
            setState(() => _flows.remove(entry.key));
            _syncTicker();
          }),
        );
      });
    }
  }

  /// The Windows tile's step line: VM 켜기 → Windows 시작 → 연결 with the
  /// time elapsed. A flow that only waits for the previous cleanup has no
  /// such step (it shows its own second line instead).
  /// The tile's VM 켜기 → Windows 시작 → 연결 line, as on Windows: waiting
  /// for the previous connection's cleanup counts as 연결.
  StartProgress _progress(_StartFlow flow, DesktopMachine machine) {
    final stage = flow.connecting || flow.settle
        ? 2
        : flow.powered ||
              (machine.powerState == 'running' &&
                  !machine.powerStale &&
                  !machine.powerBusy)
        ? 1
        : 0;
    return StartProgress(
      stage: stage,
      elapsed: Duration(seconds: flow.seconds),
    );
  }
  // --- details and Host update -------------------------------------------

  Future<void> _details(String id) async {
    unawaited(_refresh(quiet: true));
    await showDialog<void>(
      context: context,
      builder: (_) => ValueListenableBuilder<int>(
        valueListenable: _revision,
        builder: (context, _, _) {
          final desktop = _find(id);
          if (desktop == null) {
            return AppDialog(
              icon: Icons.info_outline_rounded,
              title: l10n.couldntFindDesktop,
              body: [
                AppDialog.message(
                  context,
                  const BrokerException('desktop_not_found').message,
                ),
              ],
              onClose: () => Navigator.pop(context),
            );
          }
          final machine = _machine(desktop);
          final readiness = _readiness(machine);
          return ReadinessDialog(
            machine: machine,
            readiness: readiness,
            checkedAt: _checkedAt,
            checking: _refreshing,
            checkFailed: _refreshFailed,
            onCheck: () => unawaited(_refresh(quiet: true)),
            actionFor: (action) => switch (action) {
              ReadinessAction.start =>
                machine.canStart && !_powerPending.containsKey(id)
                    ? () => unawaited(_power(desktop, 'start'))
                    : null,
              ReadinessAction.reboot =>
                machine.canReboot
                    ? () => unawaited(_power(desktop, 'reboot'))
                    : null,
              ReadinessAction.refresh => () => unawaited(_refresh()),
              ReadinessAction.console =>
                  machine.kind == MachineKind.vm &&
                      _consoleUnavailable(machine) == null
                  ? () => unawaited(_openBrowserConsole(desktop))
                  : null,
              ReadinessAction.releaseConsole => () =>
                  unawaited(_endConsolesAndConnect(desktop)),
            },
          );
        },
      ),
    );
  }

  Future<void> _hostUpdate(BrokerDesktop desktop) async {
    final api = _api, session = _session;
    final origin = _origin, server = _serverRevision;
    final machine = _machine(desktop);
    final update = machine.hostUpdate;
    if (api == null ||
        api is! HostUpdateBrokerApi ||
        session == null ||
        origin == null ||
        !update.offered) {
      return;
    }
    final intent = ++_hostUpdateIntent;
    bool owner() =>
        mounted &&
        _api == api &&
        _session == session &&
        _origin == origin &&
        server == _serverRevision &&
        intent == _hostUpdateIntent;
    bool current() {
      final listed = _find(desktop.id);
      final latest = listed == null ? null : _machine(listed).hostUpdate;
      return owner() &&
          latest?.offered == true &&
          latest?.version == update.version &&
          latest?.latestVersion == update.latestVersion;
    }

    final release = await showDialog<ProductRelease>(
      context: context,
      builder: (_) => AndroidHostUpdateDialog(
        origin: origin,
        machine: desktop.name,
        pc: machine.kind == MachineKind.pc,
        currentVersion: update.version ?? l10n.unknown,
        targetVersion: update.latestVersion!,
        sessionActive: update.sessionActive,
        isCurrent: current,
        confirmKey: Key('confirm-host-update-${desktop.id}'),
      ),
    );
    if (release == null || !current()) return;
    try {
      final result = await (api as HostUpdateBrokerApi).requestHostUpdate(
        session.token,
        desktop.id,
        release.version,
      );
      final listed = _find(desktop.id);
      if (!owner() || listed == null) return;
      final choice = androidHostUpdateReply(
        _machine(listed).hostUpdate,
        result,
      );
      if (choice.apply) {
        setState(
          () => _hostUpdateReplies[desktop.id] = (
            session,
            api,
            result,
            DateTime.now(),
            server,
          ),
        );
        _revision.value++;
      }
      _notice('${desktop.name}: ${choice.message}');
    } catch (error) {
      if (owner()) _notice(_message(error));
    }
    if (owner()) await _refresh(quiet: true);
  }

  /// A finished step on the sign-in card, shown as done (not as a failure).
  static String get _passwordChanged => l10n.passwordChangedSignNewPassword;

  bool get _canManageMfa => _api is MfaAccountBrokerApi && _origin?.scheme == 'https' &&
      (_api as MfaAccountBrokerApi).origin == _origin &&
      _session?.accountId != null && RegExp(r'^[a-f0-9]{8}(?:-[a-f0-9]{4}){3}-[a-f0-9]{12}$').hasMatch(_session!.accountId!) &&
      validAuthenticationOwner(_authenticationOwner);
  Future<void> _manageMfa() async {
    final api = _api, session = _session, origin = _origin, actor = _authenticationOwner;
    if (!_canManageMfa || api is! MfaAccountBrokerApi || session == null || origin == null || actor == null) { return; }
    final managementApi = api as MfaAccountBrokerApi;
    bool held() => mounted && _api == api && _origin == origin && _session == session && _authenticationOwner == actor;
    final controller = _mfa.begin(api: managementApi, token: session.token, accountId: session.accountId!, username: session.username, actor: actor,
      currentPresentation: () => mounted && _api == api && _origin == origin,
      currentAuthentication: () => held() && _sessionAccess.check(),
      onCommitted: (_) { if (held()) { _retireAuthentication(session, 'session_reauthentication_required'); } },
      onAuthenticationRejected: (reason) { if (held()) { _retireAuthentication(session, reason); } });
    if (controller == null) { return; }
    try {
      await showDialog<void>(context: context, barrierDismissible: false,
        builder: (routeContext) => MfaAccountDialog(controller: controller, onClose: () => Navigator.of(routeContext).pop()));
    } finally { _mfa.finished(controller); }
  }

  Future<void> _changePassword() async {
    final api = _api, session = _session;
    final origin = _origin, server = _serverRevision;
    final request = _loginRequestRevision;
    if (api == null || session == null || origin == null) return;
    // An expiry renewal keeps the same session identity. A new login/server
    // owns its own password memory and must survive this dialog's completion.
    bool current() =>
        mounted &&
        _api == api &&
        _session == session &&
        _origin == origin &&
        _serverRevision == server &&
        _loginRequestRevision == request;
    final done = await showDialog<bool>(
      context: context,
      barrierDismissible: false,
      builder: (_) => AndroidPasswordDialog(api: api, token: session.token),
    );
    if (done == true && current()) {
      try {
        await _store.forgetPassword(origin);
      } catch (_) {}
      if (!current()) return;
      _endSessionAccess();
      setState(() {
        _session = null;
        _desktops = [];
        _flows.clear();
        _password.clear();
        _remember = false;
        _error = _passwordChanged;
      });
      _syncTicker();
      _stopUpdateChecks();
      _stopKeeping();
    }
  }

  /// The tile's actions, as the Windows launcher gives its [DesktopTile]
  /// (2026-11-03: one tile on both): the main button connects (or powers
  /// on, then connects), [DesktopActions.attention] is the fix of a tile
  /// that needs one, ▾ holds 화질 옵션 and 긴급 콘솔, ⋯ manages, ⏻ powers.
  DesktopActions _actionsFor(BrokerDesktop desktop) {
    final m = _machine(desktop);
    final r = _readiness(m);
    final flow = _flows[desktop.id];
    final opening = _openingDesktop != null;
    final available = !m.powerBusy && !m.display.busy && !opening;
    final pc = m.kind == MachineKind.pc;
    final powerReason = m.powerBusy
        ? l10n.powerTaskProgress
        : pc && !m.pcPower.supported
        ? l10n.serverLocalPcsHostDoesnt
        : !pc && (m.powerStale || m.powerState == 'unknown')
        ? l10n.checkingDesktopsPowerStateRefresh
        : m.display.busy
        ? l10n.numberMonitorsChangingPowerAvailable
        : null;
    final reboot = m.canReboot
        ? () => unawaited(_power(desktop, 'reboot'))
        : null;
    final canStart =
        flow == null &&
        available &&
        m.canStart &&
        !_powerPending.containsKey(desktop.id) &&
        r.phase == TilePhase.off;
    final VoidCallback? primary = switch (r.phase) {
      TilePhase.ready when r.settling =>
        available ? () => unawaited(_settleAndConnect(desktop)) : null,
      TilePhase.ready => available ? () => unawaited(_connect(desktop)) : null,
      TilePhase.off =>
        canStart ? () => unawaited(_power(desktop, 'start')) : null,
      _ => null,
    };
    return DesktopActions(
      primary: flow == null ? primary : null,
      // A reboot only for a restart-required state, otherwise a refresh.
      attention: flow != null
          ? null
          : r.primary == ReadinessAction.reboot
          ? reboot
          : r.primary == ReadinessAction.console &&
                m.kind == MachineKind.vm &&
                _consoleUnavailable(m) == null
          ? () => unawaited(_openBrowserConsole(desktop))
          : r.primary == ReadinessAction.releaseConsole
          ? () => unawaited(_endConsolesAndConnect(desktop))
          : _refreshing
          ? null
          : () => unawaited(_refresh()),
      cancel: flow == null ? null : () => _cancelFlow(desktop.id),
      streamUnavailable: r.canStream ? null : r.headline,
      showPower:
          !pc ||
          (m.pcPower.supported &&
              (m.hostOnline != true || m.pcPower.actions.isNotEmpty)),
      showStart: pc ? m.hostOnline != true : m.powerState != 'running',
      pcPower: pc,
      powerReason: powerReason,
      // An offline PC the server cannot wake still answers 켜기 with why.
      start:
          (m.canStart ||
                  (pc &&
                      m.pcPower.supported &&
                      m.hostOnline != true &&
                      !m.powerBusy &&
                      !m.pcPower.wake)) &&
              !_powerPending.containsKey(desktop.id) &&
              flow == null
          ? () => unawaited(_power(desktop, 'start'))
          : null,
      reboot: reboot,
      shutdown: m.canShutdown
          ? () => unawaited(_power(desktop, 'shutdown'))
          : null,
      // Also while this account's own 종료 has not finished: the broker then
      // replaces the shutdown with the stop.
      stop: m.canStop ? () => unawaited(_power(desktop, 'stop')) : null,
      stopReason: stopUnavailableReason(m),
      quality: r.canStream && available && flow == null
          ? () => unawaited(_qualityConnect(desktop))
          : null,
      details: () => unawaited(_details(desktop.id)),
      console:
          m.kind == MachineKind.vm &&
              flow == null &&
              _consoleUnavailable(m) == null
          ? () => unawaited(_openBrowserConsole(desktop))
          : null,
      consoleReason: m.kind == MachineKind.vm ? _consoleUnavailable(m) : null,
      hostUpdate: m.hostUpdate.offered && _api is HostUpdateBrokerApi
          ? () => unawaited(_hostUpdate(desktop))
          : null,
      favorite: _api is FavoriteBrokerApi && m.organize.supported
          ? m.organize.favorite
          : null,
      toggleFavorite: _favoritePending.containsKey(desktop.id)
          ? null
          : () => unawaited(_toggleFavorite(m)),
    );
  }

  @override
  void dispose() {
    if (widget.mfaHost == null) { _mfa.dispose(); } else { _mfa.retire(); }
    if (widget.sessionAccess == null) _sessionAccess.dispose();
    WidgetsBinding.instance.removeObserver(this);
    final closingApi = _api;
    unawaited(
      const ConsoleBrowser.android()
          .closeAll(api: closingApi)
          .whenComplete(() => closingApi?.close()),
    );
    HardwareKeyboard.instance.removeHandler(_searchKey);
    _search.dispose();
    _searchFocus.dispose();
    _stopWatch(sync: false);
    _notices?.dispose();
    _notices = null;
    final noticeHost = widget.noticeHost;
    if (noticeHost != null) {
      // After this frame: the layer above is not rebuilt while the tree locks.
      WidgetsBinding.instance.addPostFrameCallback((_) {
        noticeHost.bind(null);
        noticeHost.streaming = false;
      });
    }
    _previews.stop();
    _poll?.cancel();
    _poll = null;
    _stallWatch?.cancel();
    _stallWatch = null;
    _restartRetry?.cancel();
    _ticker?.cancel();
    _updateTimer?.cancel();
    _stopKeeping();
    unawaited(_installStatus?.cancel());
    _id.dispose();
    _password.dispose();
    _revision.dispose();
    super.dispose();
  }

  @override
  Widget build(BuildContext context) {
    _syncNotices();
    final child = _buildHome(context);
    return widget.mfaHost == null ? MfaCompletionOverlay(host: _mfa, child: child) : child;
  }
  Widget _buildHome(BuildContext context) {
    final p = AppPalette.of(context);
    if (_loading) {
      return Scaffold(
        body: AnnotatedRegion<SystemUiOverlayStyle>(
          value: androidSystemBars(p.dark),
          child: const SceneBackground(
            child: Center(child: CircularProgressIndicator()),
          ),
        ),
      );
    }
    if (_session == null) {
      return Scaffold(
        body: AndroidSignInView(
          origin: _origin,
          id: _id,
          password: _password,
          remember: _remember,
          busy: _busy,
          signingOut: _signingOut,
          version: _version,
          updateVersion: _update?.version,
          onUpdates: () => unawaited(_openUpdate()),
          error: _error,
          errorIsDone: _error == _passwordChanged,
          onServer: () => unawaited(_server()),
          onIdChanged: _idChanged,
          onRemember: _rememberChanged,
          onLogin: () => unawaited(_login()),
        ),
      );
    }
    // Back first closes a search left open.
    return PopScope(
      canPop: !(_searchOpen || _query.isNotEmpty),
      onPopInvokedWithResult: (didPop, _) {
        if (!didPop) _closeSearch();
      },
      child: Scaffold(
        body: AnnotatedRegion<SystemUiOverlayStyle>(
          value: androidSystemBars(p.dark),
          child: SceneBackground(child: SafeArea(child: _workspace())),
        ),
      ),
    );
  }

  /// The assigned desktops as the Windows launcher shows them (2026-11-03:
  /// "카드뷰도 이상하고"): its [DesktopTile] by finger, in its grid
  /// ([desktopTileGrid]: responsive columns, one size for every tile).
  Widget _tiles(List<DesktopMachine> shown, TextScaler scaler, Size screen) =>
      LayoutBuilder(
        builder: (context, bounds) {
          final grid = desktopTileGrid(
            bounds.maxWidth,
            scaler,
            viewport: screen,
            previews: _showPreviews,
            touch: true,
          );
          final tiles = [
            for (final machine in shown)
              if (_find(machine.id) case final desktop?)
                DesktopTile(
                  key: ValueKey('slot-${machine.id}'),
                  machine: machine,
                  readiness: _readiness(machine),
                  actions: _actionsFor(desktop),
                  // No window modes: one full screen.
                  mode: null,
                  progress: switch (_flows[machine.id]) {
                    final flow? => _progress(flow, machine),
                    null => null,
                  },
                  thumbnail: _previews.pictureFor(machine),
                  previewNote: _previews.noteFor(machine),
                  showPreview: _showPreviews,
                  pictureHeight: grid.picture,
                  touch: true,
                  focusStyle: AndroidTvFocus.style,
                ),
          ];
          return Column(
            key: const Key('cards-view'),
            children: [
              for (var row = 0; row < tiles.length; row += grid.columns) ...[
                if (row > 0) const SizedBox(height: desktopTileGap),
                SizedBox(
                  height: grid.height,
                  child: Row(
                    crossAxisAlignment: CrossAxisAlignment.stretch,
                    children: [
                      for (var i = row; i < row + grid.columns; i++) ...[
                        if (i > row) const SizedBox(width: desktopTileGap),
                        Expanded(
                          child: i < tiles.length
                              ? tiles[i]
                              : const SizedBox.shrink(),
                        ),
                      ],
                    ],
                  ),
                ),
              ],
            ],
          );
        },
      );

  Widget _workspace() {
    final p = AppPalette.of(context);
    final session = _session!;
    final size = MediaQuery.sizeOf(context);
    final scaler = MediaQuery.textScalerOf(context);
    final width = size.width;
    // The Windows page rhythm: 16 / 32 / 48 side margins and a centred column.
    final side = width < 600
        ? 16.0
        : width < 1100
        ? 32.0
        : 48.0;
    // A phone on its side: the filters and tools join the header row, so
    // the first card's 연결 is on screen (backlog 2026-11-02).
    final shortLandscape =
        !AndroidTvScope.of(context) &&
        size.width > size.height &&
        size.height < 500;
    final machines = [for (final desktop in _desktops) _machine(desktop)];
    Widget header({Widget? middle}) => AndroidWorkspaceHeader(
      sessionAccess: _sessionAccess,
      side: shortLandscape ? 16 : side,
      columnWidth: _columnWidth,
      account: session.username,
      server: _origin?.authority ?? '',
      version: _version,
      update: _update?.version,
      hostUpdates: _hostUpdatable(machines).length,
      noticeBell: _notices == null
          ? null
          : AndroidNoticeBell(center: _notices!),
      noticeBellWidth: AndroidNoticeBell.width,
      onUpdates: () => unawaited(_openUpdate()),
      onChangePassword: () => unawaited(_changePassword()),
      onManageMfa: _canManageMfa ? () => unawaited(_manageMfa()) : null,
      onSignOut: () => unawaited(_signOut()),
      onLogs: () => unawaited(showProductLog(context)),
      onExit: () => unawaited(_exitApplication()),
      middle: middle,
    );
    // Every control of the title and filter rows is this tall and drawn as
    // one pill or circle (2026-11-03, as on Windows; by finger ≥ 48 dp).
    final barHeight = organizerHeight(context, touch: true);
    // While the list is read 새로고침 keeps its shape and label; a spinner
    // takes the icon's place and presses wait. [label] false: one round icon
    // (the short landscape header row).
    Widget refreshButton({bool label = true}) => LauncherHeaderButton(
      key: const Key('refresh-desktops'),
      icon: Icons.refresh_rounded,
      label: label ? l10n.refresh : null,
      tooltip: _refreshing ? l10n.checkingList : l10n.refreshList,
      busy: _refreshing,
      height: barHeight,
      compactChrome: true,
      onPressed: () => unawaited(_refresh()),
    );
    final refresh = refreshButton(label: width >= 600);
    // A phone keeps the 상태 filter whole: 미리보기 is its icon alone.
    final compactViewOptions =
        shortLandscape || width < 600 * scaler.scale(1).clamp(1.0, 1.6);
    // 미리보기 is a toggle that shows its state (amber while on), as on
    // Windows; it was a 보기 옵션 menu holding only this choice.
    final viewMenu = LauncherHeaderButton(
      key: const Key('desktop-previews'),
      icon: _showPreviews ? Icons.image_outlined : Icons.hide_image_outlined,
      label: compactViewOptions ? null : l10n.previews,
      tooltip: _showPreviews ? l10n.screenPreviewsTapTurnOff : l10n.screenPreviewsOffTapTurn,
      selected: _showPreviews,
      height: barHeight,
      compactChrome: true,
      onPressed: () => unawaited(_selectPreviews()),
    );
    final controls = _desktops.isEmpty
        ? Wrap(
            alignment: WrapAlignment.end,
            spacing: 8,
            runSpacing: 8,
            children: [viewMenu, refresh],
          )
        : refresh;
    final organize = organizeUseful(machines);
    final chip = _effectiveChip(machines);
    final status = _effectiveStatus(machines);
    final kinds = kindFilterUseful(machines);
    final kind = _effectiveKind(machines);
    final shown = organizeDesktops(
      machines,
      query: _query,
      chip: chip,
      status: status,
      kind: kind,
      sort: _sort,
    );
    final organizeKey = '${_origin?.origin}|${session.username}';
    if (_organizeFor != organizeKey) {
      _organizeFor = organizeKey;
      _chip = DesktopChip.all;
      _status = DesktopStatus.all;
      _kind = DesktopKind.all;
      _sort = DesktopSort.number;
      final origin = _origin;
      // After this build: clearing the box notifies its listener.
      WidgetsBinding.instance.addPostFrameCallback((_) {
        if (!mounted || _organizeFor != organizeKey) return;
        if (_query.isNotEmpty || _searchOpen) _closeSearch();
        if (origin != null) {
          unawaited(_loadOrganize(organizeKey, origin, session.username));
        }
      });
    } else if (_desktops.isNotEmpty && (_status != status || _kind != kind)) {
      // A remembered status or kind that has no members any more counts as
      // 전체 and is forgotten, so it does not come back by surprise later.
      _status = status;
      _kind = kind;
      _saveOrganize();
    }
    final searching = _searchOpen || _query.isNotEmpty;
    DesktopSearchField searchBox({required bool open, double? width}) =>
        DesktopSearchField(
          controller: _search,
          focusNode: _searchFocus,
          onSubmitted: (_) {
            if (shown.length == 1) {
              FocusManager.instance.primaryFocus?.unfocus();
            }
          },
          onLeave: _closeSearch,
          onOpen: _openSearch,
          open: open,
          width: width,
          closable: width == null,
          touch: true,
          compactChrome: true,
          openTooltip: l10n.search,
        );
    final sortMenu = PopupMenuButton<DesktopSort>(
      key: const Key('desktop-sort'),
      tooltip: l10n.sort(_sort.label),
      color: p.surface,
      onSelected: _selectSort,
      itemBuilder: (_) => [
        for (final sort in DesktopSort.values)
          PopupMenuItem(
            key: Key('sort-${sort.name}'),
            value: sort,
            child: Row(
              children: [
                Expanded(child: KeepAllText(sort.label)),
                if (sort == _sort)
                  Icon(Icons.check_rounded, size: 18, color: p.green),
              ],
            ),
          ),
      ],
      child: Container(
        width: barHeight,
        height: barHeight,
        decoration: BoxDecoration(
          color: p.pillGlass,
          borderRadius: BorderRadius.circular(10),
        ),
        child: Icon(Icons.swap_vert_rounded, size: 20, color: p.ink),
      ),
    );
    final filtered =
        status != DesktopStatus.all ||
        chip != DesktopChip.all ||
        kind != DesktopKind.all;
    final chips = chipsFor(machines);
    // The 구분 filter (전체 · VM · 로컬 PC) beside 상태, only while both kinds
    // are assigned (2026-11-03, as on Windows).
    final kindFilter = kinds
        ? DesktopKindFilter(
            selected: kind,
            machines: facetMachines(machines, status: status, chip: chip),
            onSelected: _selectKind,
            touch: true,
            compactChrome: true,
          )
        : null;
    final statusRow = Row(
      mainAxisSize: MainAxisSize.min,
      children: [
        DesktopStatusFilter(
          selected: status,
          counts: DesktopStatusCounts.of(
            facetMachines(machines, kind: kind, chip: chip),
          ),
          onSelected: _selectStatus,
          onUnavailable: (status) => _notice(status.emptyReason),
          touch: true,
          compactChrome: true,
        ),
        if (filtered) ...[
          const SizedBox(width: 6),
          shortLandscape
              ? IconButton(
                  key: const Key('desktop-filter-reset'),
                  tooltip: l10n.clearAllFilters,
                  onPressed: _clearFilters,
                  style: IconButton.styleFrom(
                    minimumSize: Size.square(barHeight),
                    maximumSize: Size.square(barHeight),
                    foregroundColor: p.amberInk,
                    padding: EdgeInsets.zero,
                    tapTargetSize: MaterialTapTargetSize.shrinkWrap,
                  ),
                  icon: const Icon(Icons.filter_alt_off_outlined, size: 20),
                )
              : DesktopFilterReset(
                  onPressed: _clearFilters,
                  touch: true,
                  compactChrome: true,
                ),
        ],
      ],
    );
    // Under the title: the filters (each scrolls sideways if it must), the
    // magnifier and the sort menu; an open search takes a row of its own
    // below so the filters stay in view; then the 분류 chips. The short
    // landscape header keeps them in one sideways row.
    final statusStrip = DesktopSideScroll(
      key: const Key('desktop-status-row'),
      height: barHeight,
      child: Row(
        children: [
          if (kindFilter != null) ...[kindFilter, const SizedBox(width: 8)],
          statusRow,
        ],
      ),
    );
    // With both filters the 상태 filter wraps onto its own line rather than
    // hide behind the edge (as on Windows).
    final filters = kindFilter == null
        ? statusStrip
        : Wrap(
            key: const Key('desktop-status-row'),
            spacing: 8,
            runSpacing: 8,
            children: [
              DesktopSideScroll(height: barHeight, child: kindFilter),
              DesktopSideScroll(height: barHeight, child: statusRow),
            ],
          );
    final tools = Row(
      mainAxisSize: MainAxisSize.min,
      children: [
        if (!searching) ...[
          searchBox(open: false, width: 200),
          const SizedBox(width: 8),
        ],
        if (organize) ...[sortMenu, const SizedBox(width: 8)],
        viewMenu,
      ],
    );
    // Keep the ordinary phone's filter and two tools on one line when
    // their actual scaled labels fit. Enlarged text and mixed fleets keep
    // coherent groups whole and wrap below, rather than squeezing targets.
    double controlTextWidth(String text, TextStyle style) {
      final painter = TextPainter(
        text: TextSpan(
          text: text,
          style: DefaultTextStyle.of(context).style.merge(style),
        ),
        textDirection: Directionality.of(context),
        textScaler: scaler,
      )..layout();
      try {
        return painter.width.ceilToDouble();
      } finally {
        painter.dispose();
      }
    }

    final statusCounts = DesktopStatusCounts.of(
      facetMachines(machines, kind: kind, chip: chip),
    );
    // Four pixels of explicit inset, two of decoration border, plus two
    // pixels of rounding slack. Counts use the rendered tabular figures.
    final statusWidth =
        8 +
        DesktopStatus.values.fold<double>(
          0,
          (sum, value) =>
              sum +
              20 +
              6 +
              controlTextWidth(
                value.label,
                uiText(
                  size: 14,
                  weight: value == status ? FontWeight.w600 : FontWeight.w600,
                ),
              ) +
              controlTextWidth(
                '${statusCounts.of(value)}',
                uiText(
                  size: 13,
                ).copyWith(fontFeatures: const [FontFeature.tabularFigures()]),
              ),
        );
    final toolsCount = (searching ? 0 : 1) + (organize ? 1 : 0) + 1;
    final compactToolsWidth = toolsCount * barHeight + (toolsCount - 1) * 8;
    final organizeBar = _desktops.isEmpty
        ? const SizedBox.shrink()
        : Column(
            crossAxisAlignment: CrossAxisAlignment.stretch,
            children: [
              if (!shortLandscape)
                Padding(
                  padding: const EdgeInsets.only(top: 12),
                  child: LayoutBuilder(
                    builder: (context, bounds) {
                      if (kindFilter != null ||
                          filtered ||
                          bounds.maxWidth <
                              statusWidth + 8 + compactToolsWidth) {
                        return Column(
                          crossAxisAlignment: CrossAxisAlignment.stretch,
                          children: [
                            filters,
                            const SizedBox(height: 8),
                            Align(
                              alignment: Alignment.centerRight,
                              child: tools,
                            ),
                          ],
                        );
                      }
                      // Level with the last filter line.
                      return Row(
                        crossAxisAlignment: CrossAxisAlignment.end,
                        children: [
                          Expanded(child: filters),
                          const SizedBox(width: 8),
                          tools,
                        ],
                      );
                    },
                  ),
                ),
              AnimatedSize(
                duration: const Duration(milliseconds: 180),
                curve: Curves.easeOutCubic,
                alignment: Alignment.topCenter,
                child: searching
                    ? Padding(
                        padding: const EdgeInsets.only(top: 10),
                        child: searchBox(open: true),
                      )
                    : const SizedBox(width: double.infinity),
              ),
              if (chips.isNotEmpty)
                Padding(
                  padding: const EdgeInsets.only(top: 12),
                  child: DesktopChipRow(
                    chips: chips,
                    selected: chip,
                    onSelected: _selectChip,
                    touch: true,
                  ),
                ),
            ],
          );
    final titleLabel = Wrap(
      spacing: 8,
      runSpacing: 4,
      crossAxisAlignment: WrapCrossAlignment.center,
      children: [
        KeepAllText(
          l10n.myDesktops,
          key: const Key('workspace-title'),
          style: uiText(size: 20, weight: FontWeight.w600, color: p.ink),
        ),
        if (_desktops.isNotEmpty)
          KeepAllText(
            shown.length == _desktops.length
                ? '${_desktops.length}'
                : '${shown.length}/${_desktops.length}',
            key: const Key('desktop-count'),
            style: uiText(
              size: 13,
              color: p.muted,
            ).copyWith(fontFeatures: const [FontFeature.tabularFigures()]),
          ),
      ],
    );
    final title = LayoutBuilder(
      builder: (context, bounds) {
        // Keep the filtered count and refresh action readable: at phone
        // widths enlarged text gets a second row rather than squeezing the
        // count out of the title row.
        if (bounds.maxWidth < 300 * scaler.scale(1).clamp(1.0, 1.6)) {
          return Column(
            crossAxisAlignment: CrossAxisAlignment.stretch,
            children: [
              titleLabel,
              const SizedBox(height: 10),
              Align(alignment: Alignment.centerRight, child: controls),
            ],
          );
        }
        return Row(
          children: [
            Expanded(child: titleLabel),
            const SizedBox(width: 12),
            controls,
          ],
        );
      },
    );
    Widget stateCard(
      IconData icon,
      AppTone tone,
      String heading,
      String detail, {
      Widget? action,
    }) => Container(
      constraints: const BoxConstraints(maxWidth: 440),
      margin: const EdgeInsets.only(top: 12),
      padding: const EdgeInsets.all(16),
      decoration: BoxDecoration(
        color: p.tileGlass,
        border: Border.all(color: p.glassEdge),
        borderRadius: BorderRadius.circular(16),
      ),
      child: Column(
        mainAxisSize: MainAxisSize.min,
        crossAxisAlignment: CrossAxisAlignment.stretch,
        children: [
          Row(
            children: [
              AppToneIcon(icon: icon, tone: tone),
              const SizedBox(width: 14),
              Expanded(
                child: KeepAllText(
                  heading,
                  style: uiText(
                    size: 16,
                    weight: FontWeight.w600,
                    color: p.ink,
                  ),
                ),
              ),
            ],
          ),
          const SizedBox(height: 12),
          KeepAllText(
            detail,
            style: uiText(size: 13, height: 1.5, color: p.muted),
          ),
          if (action != null) ...[const SizedBox(height: 18), action],
        ],
      ),
    );
    final retry = FilledButton.icon(
      key: const Key('retry-desktops'),
      onPressed: _refreshing ? null : () => unawaited(_refresh()),
      style: AndroidTvFocus.style(context, AppButtons.filled(p)),
      icon: const Icon(Icons.refresh_rounded, size: 19),
      label: Text(l10n.tryAgain),
    );
    final Widget content;
    if (_listError != null && _desktops.isEmpty) {
      content = Center(
        child: stateCard(
          Icons.cloud_off_rounded,
          AppTone.danger,
          l10n.couldntLoadList,
          _listError!,
          action: retry,
        ),
      );
    } else if (_desktops.isEmpty) {
      content = Center(
        child: stateCard(
          Icons.desktop_windows_outlined,
          AppTone.info,
          l10n.noDesktopsAssignedYet,
          l10n.askAdministratorAssignDesktopPull,
        ),
      );
    } else {
      content = Column(
        crossAxisAlignment: CrossAxisAlignment.stretch,
        children: [
          if (_listError != null)
            Container(
              key: const Key('list-refresh-error'),
              margin: const EdgeInsets.only(bottom: 16),
              padding: const EdgeInsets.fromLTRB(14, 8, 8, 8),
              decoration: BoxDecoration(
                color: Color.alphaBlend(
                  p.red.withValues(alpha: .10),
                  p.tileGlass,
                ),
                borderRadius: BorderRadius.circular(12),
                border: Border.all(color: p.red.withValues(alpha: .32)),
              ),
              child: Row(
                children: [
                  Icon(Icons.error_outline_rounded, size: 18, color: p.red),
                  const SizedBox(width: 10),
                  Expanded(
                    child: KeepAllText(
                      l10n.listMayOutDate('$_listError'),
                      style: uiText(size: 13, color: p.ink),
                    ),
                  ),
                  TextButton(
                    onPressed: _refreshing ? null : () => unawaited(_refresh()),
                    child: Text(l10n.tryAgain),
                  ),
                ],
              ),
            ),
          if (shown.isEmpty)
            Center(
              child: stateCard(
                Icons.search_off_rounded,
                AppTone.info,
                noMatchTitle(_query),
                noMatchDetail(status, chip, kind: kind),
                action: Wrap(
                  alignment: WrapAlignment.end,
                  crossAxisAlignment: WrapCrossAlignment.center,
                  spacing: 8,
                  runSpacing: 6,
                  children: [
                    if (filtered)
                      TextButton(
                        key: const Key('no-match-all'),
                        onPressed: _clearFilters,
                        child: KeepAllText(l10n.clearFilters),
                      ),
                    if (_query.trim().isNotEmpty)
                      FilledButton.icon(
                        key: const Key('no-match-clear'),
                        onPressed: _closeSearch,
                        style: AndroidTvFocus.style(
                          context,
                          AppButtons.filled(p),
                        ),
                        icon: const Icon(Icons.close_rounded, size: 19),
                        label: KeepAllText(l10n.clearSearch),
                      ),
                  ],
                ),
              ),
            ),
          _tiles(shown, scaler, size),
        ],
      );
    }
    return Column(
      children: [
        header(
          middle: !shortLandscape
              ? null
              : _desktops.isEmpty
              ? Align(alignment: Alignment.centerRight, child: controls)
              : Row(
                  children: [
                    Expanded(child: statusStrip),
                    const SizedBox(width: 4),
                    tools,
                    const SizedBox(width: 4),
                    refreshButton(label: false),
                  ],
                ),
        ),
        Divider(height: 1, color: p.glassEdge),
        Expanded(
          child: RefreshIndicator(
            color: p.amber,
            backgroundColor: p.surface,
            onRefresh: _refresh,
            child: ListView(
              physics: const AlwaysScrollableScrollPhysics(),
              padding: EdgeInsets.fromLTRB(
                side,
                shortLandscape ? 0 : 16,
                side,
                24,
              ),
              children: [
                Center(
                  child: ConstrainedBox(
                    constraints: const BoxConstraints(maxWidth: _columnWidth),
                    child: Column(
                      crossAxisAlignment: CrossAxisAlignment.stretch,
                      children: [
                        if (!shortLandscape) ...[title],
                        organizeBar,
                        SizedBox(height: shortLandscape ? 12 : 16),
                        content,
                      ],
                    ),
                  ),
                ),
              ],
            ),
          ),
        ),
      ],
    );
  }
}

class AndroidServerDialog extends StatefulWidget {
  const AndroidServerDialog({
    super.key,
    required this.initial,
    required this.save,
    this.probe,
  });
  final Uri? initial;
  final Future<void> Function(Uri) save;
  final Future<BrokerProbeResult> Function(Uri)? probe;
  @override
  State<AndroidServerDialog> createState() => _AndroidServerDialogState();
}

class _AndroidServerDialogState extends State<AndroidServerDialog> {
  final _testFocus = FocusNode(debugLabel: 'server-test');
  final _saveFocus = FocusNode(debugLabel: 'server-save');
  late final _address = TextEditingController(
    text: widget.initial?.origin ?? '',
  );
  bool _busy = false, _ok = false;
  bool _confirmingHttp = false;
  Uri? _tested;
  String? _message;
  late String _scheme = widget.initial?.scheme ?? 'https';
  String? _httpConsent;
  HttpTransportApproval? _httpApproval;
  int _revision = 0;
  @override
  void dispose() {
    _revision++;
    _httpApproval?.release();
    _testFocus.dispose();
    _saveFocus.dispose();
    _address.dispose();
    super.dispose();
  }

  Future<void> _test() async {
    if (_busy) return;
    final input = _address.text, revision = ++_revision;
    _httpApproval?.release();
    _httpApproval = null;
    FocusManager.instance.primaryFocus?.unfocus();
    setState(() {
      _busy = true;
      _tested = null;
      _message = null;
    });
    try {
      final origin = parseServerOrigin(input, defaultScheme: _scheme);
      if (origin.scheme == 'http' && _httpConsent != origin.origin) {
        try {
          _httpConsent = BrokerTransport.consentOrigin(origin);
        } on FormatException {
          /* New exact address. */
        }
        if (_httpConsent != origin.origin) {
          setState(() => _confirmingHttp = true);
          bool accepted;
          try {
            accepted = await confirmInsecureBroker(context, origin);
          } finally {
            if (mounted) setState(() => _confirmingHttp = false);
          }
          if (!accepted) return;
          if (!mounted || revision != _revision || input != _address.text) {
            return;
          }
          _httpConsent = origin.origin;
        }
      }
      final result =
          await (widget.probe?.call(origin) ??
              probeBroker(origin, httpConsentOrigin: _httpConsent));
      if (origin.scheme == 'http' &&
          (result.httpApproval?.origin != origin ||
              result.httpApproval?.current != true)) {
        result.httpApproval?.release();
        throw const BrokerProbeException('http_consent');
      }
      if (mounted && revision == _revision && input == _address.text) {
        setState(() {
          _tested = origin;
          _scheme = origin.scheme;
          _httpApproval = result.httpApproval;
          _ok = true;
          _message =
              l10n.connectionOkMultitapMs(result.version, result.elapsedMs);
        });
      } else {
        result.httpApproval?.release();
      }
    } catch (error) {
      if (mounted && revision == _revision) {
        setState(() {
          _ok = false;
          _message = error is BrokerProbeException
              ? error.message
              : error is FormatException
              ? error.message.toString()
              : l10n.checkServerAddressPort;
        });
      }
    } finally {
      if (mounted) {
        setState(() => _busy = false);
        _restoreTvFocus();
      }
    }
  }

  void _addressChanged() => setState(() {
    _revision++;
    _httpApproval?.release();
    _httpApproval = null;
    _httpConsent = null;
    _tested = null;
    _message = null;
    _ok = false;
    final prefix = _address.text.split('://');
    if (prefix.length > 1 &&
        const {'http', 'https'}.contains(prefix.first.toLowerCase())) {
      _scheme = prefix.first.toLowerCase();
    }
  });

  void _restoreTvFocus() {
    if (!AndroidTvScope.of(context)) return;
    WidgetsBinding.instance.addPostFrameCallback((_) {
      if (!mounted || _busy || ModalRoute.of(context)?.isCurrent == false) {
        return;
      }
      (_tested == null ? _testFocus : _saveFocus).requestFocus();
    });
  }

  Future<void> _save() async {
    final tested = _tested;
    if (_busy ||
        tested == null ||
        (tested.scheme == 'http' && _httpApproval?.current != true) ||
        parseServerOrigin(_address.text, defaultScheme: _scheme) != tested) {
      return;
    }
    setState(() => _busy = true);
    try {
      await widget.save(tested);
      BrokerTransport.stored(tested);
      if (mounted) Navigator.pop(context, tested);
    } catch (_) {
      if (mounted) {
        setState(() {
          _busy = false;
          _ok = false;
          _message = l10n.couldntSaveServerAddress;
        });
        _restoreTvFocus();
      }
    }
  }

  @override
  Widget build(BuildContext context) {
    final p = AppPalette.of(context);
    final message = _message;
    return PopScope(
      canPop: !_busy,
      child: AppDialog(
        icon: Icons.dns_outlined,
        tone: AppTone.accent,
        busy: _busy && !_confirmingHttp,
        title: l10n.signServer,
        scrollAll: true,
        maxWidth: AndroidTvScope.of(context) ? 560 : 400,
        // What the choice below means (it reads HTTPS / HTTP alone), in the
        // PC's words.
        subtitle: AndroidTvScope.of(context)
            ? null
            : (_scheme == 'https' ? l10n.httpsSecureConnection : l10n.httpNotEncrypted),
        body: [
          AppDialog.message(context, l10n.youCanSaveAddressAfter),
          const SizedBox(height: 14),
          BrokerTransportChoice(
            scheme: _scheme,
            onChanged: _busy
                ? null
                : (value) {
                    final split = _address.text.indexOf('://');
                    if (split >= 0) {
                      _address.text = '$value${_address.text.substring(split)}';
                    }
                    _scheme = value;
                    _addressChanged();
                  },
          ),
          const SizedBox(height: 12),
          AndroidTvTextEntry(
            controller: _address,
            label: l10n.serverAddress,
            enabled: !_busy,
            keyboardType: TextInputType.url,
            maxLength: 2048,
            action: TextInputAction.go,
            onSubmitted: (_) => unawaited(_test()),
            onChanged: (_) => _addressChanged(),
            builder: (focusNode, readOnly, edit) => TextField(
              controller: _address,
              focusNode: focusNode,
              readOnly: readOnly,
              onTap: edit,
              autofocus: AndroidTvScope.of(context),
              enabled: !_busy,
              keyboardType: TextInputType.url,
              autocorrect: false,
              enableSuggestions: false,
              textInputAction: TextInputAction.go,
              onSubmitted: (_) => unawaited(_test()),
              decoration: InputDecoration(
                labelText: l10n.serverAddress,
                hintText: _scheme == 'http' ? 'IP:18480' : l10n.httpsServerAddress,
                prefixIcon: Icon(Icons.link_rounded, size: 19),
              ),
              onChanged: (_) => _addressChanged(),
            ),
          ),
          const SizedBox(height: 12),
          AndroidTvFocus(
            builder: (context, _) => OutlinedButton.icon(
              focusNode: _testFocus,
              onPressed: _busy ? null : _test,
              style: AndroidTvFocus.style(
                context,
                AppButtons.outlined(p).copyWith(
                  minimumSize: const WidgetStatePropertyAll(Size(0, 46)),
                ),
              ),
              icon: _busy && !_confirmingHttp
                  ? const SizedBox.square(
                      dimension: 18,
                      child: CircularProgressIndicator(strokeWidth: 2),
                    )
                  : const Icon(Icons.network_check_rounded, size: 19),
              label: Text(l10n.testConnection),
            ),
          ),
          if (message != null)
            Container(
              key: const Key('server-test-result'),
              margin: const EdgeInsets.only(top: 12),
              padding: const EdgeInsets.fromLTRB(12, 10, 12, 10),
              decoration: BoxDecoration(
                color: _ok ? p.okSoft : p.red.withValues(alpha: .1),
                borderRadius: BorderRadius.circular(12),
                border: Border.all(
                  color: _ok ? p.okLine : p.red.withValues(alpha: .35),
                ),
              ),
              child: Row(
                crossAxisAlignment: CrossAxisAlignment.start,
                children: [
                  Icon(
                    _ok
                        ? Icons.check_circle_rounded
                        : Icons.error_outline_rounded,
                    size: 18,
                    color: _ok ? p.green : p.red,
                  ),
                  const SizedBox(width: 9),
                  Expanded(
                    child: KeepAllText(
                      message,
                      style: uiText(size: 13, height: 1.5, color: p.ink),
                    ),
                  ),
                ],
              ),
            ),
        ],
        actions: [
          AndroidTvFocus(
            builder: (context, _) => TextButton(
              onPressed: _busy ? null : () => Navigator.pop(context),
              style: AndroidTvFocus.style(context, AppButtons.quiet(p)),
              child: Text(l10n.cancel),
            ),
          ),
          AndroidTvFocus(
            builder: (context, _) => FilledButton(
              focusNode: _saveFocus,
              onPressed: _busy || _tested == null ? null : _save,
              style: AndroidTvFocus.style(context, AppButtons.filled(p)),
              child: Text(l10n.save),
            ),
          ),
        ],
      ),
    );
  }
}

class AndroidStreamDialog extends StatefulWidget {
  const AndroidStreamDialog({
    super.key,
    required this.desktop,
    this.initial = AndroidStreamQuality.recommended,
  });
  final BrokerDesktop desktop;
  final AndroidStreamQuality initial;
  @override
  State<AndroidStreamDialog> createState() => _AndroidStreamDialogState();
}

class _AndroidStreamDialogState extends State<AndroidStreamDialog> {
  late int _fps = widget.initial.fps, _bitrate = widget.initial.bitrate;

  StreamPreset? get _preset => StreamPreset.of(
    AndroidStreamQuality(fps: _fps, bitrate: _bitrate).options,
    widget.desktop.maxBitrate,
  );

  // A mode sets the frame rate and the bitrate; changing either of them
  // afterwards reads 사용자 설정.
  void _choose(StreamPreset preset) {
    _fps = preset.options.fps;
    _bitrate = preset.options.automaticBitrate
        ? AndroidStreamQuality.automatic
        : preset.options.bitrate ~/ 1000000;
  }

  Widget _label(AppPalette p, String text) => Padding(
    padding: const EdgeInsets.only(bottom: 8),
    child: KeepAllText(
      text,
      style: uiText(size: 13, weight: FontWeight.w600, color: p.muted),
    ),
  );

  Widget _choice<T>(
    AppPalette p, {
    required List<(T, String)> options,
    required T selected,
    required ValueChanged<T> onSelected,
    required String keyPrefix,
    String? Function(T value)? tooltip,
  }) => Wrap(
    spacing: 8,
    runSpacing: 8,
    children: [
      for (final (value, label) in options)
        ChoiceChip(
          key: Key('$keyPrefix-$value'),
          tooltip: tooltip?.call(value),
          label: Text(label),
          selected: value == selected,
          showCheckmark: false,
          backgroundColor: p.quiet,
          selectedColor: p.amberSoft,
          side: BorderSide.none,
          shape: RoundedRectangleBorder(
            borderRadius: BorderRadius.circular(10),
          ),
          materialTapTargetSize: MaterialTapTargetSize.padded,
          labelStyle: uiText(
            size: 13.5,
            weight: FontWeight.w600,
            color: value == selected ? p.amberInk : p.ink,
          ),
          padding: const EdgeInsets.symmetric(horizontal: 8, vertical: 6),
          onSelected: (_) => setState(() => onSelected(value)),
        ),
    ],
  );

  @override
  Widget build(BuildContext context) {
    final p = AppPalette.of(context);
    return AppDialog(
      icon: Icons.tune_rounded,
      // The Windows dialog's name, and the ▾ row's (2026-11-03).
      title: l10n.streamingSettings,
      subtitle: widget.desktop.name,
      body: [
        LayoutBuilder(
          key: const Key('android-c-recommended-settings'),
          builder: (context, bounds) {
            final current = KeepAllText(
              l10n.qualityMode(_preset?.label ?? l10n.custom),
              key: const Key('android-c-stream-preset-status'),
              style: uiText(size: 13, weight: FontWeight.w600, color: p.ink),
            );
            final reset = TextButton(
              key: const Key('android-c-use-recommended'),
              onPressed: () => setState(() => _choose(StreamPreset.work)),
              style: AndroidTvFocus.style(
                context,
                TextButton.styleFrom(
                  minimumSize: const Size(48, 48),
                  foregroundColor: p.ink,
                  textStyle: uiText(size: 13, weight: FontWeight.w600),
                  shape: RoundedRectangleBorder(
                    borderRadius: BorderRadius.circular(10),
                  ),
                ),
              ),
              child: Text(l10n.useRecommended),
            );
            if (bounds.maxWidth >=
                280 * MediaQuery.textScalerOf(context).scale(1)) {
              return Row(
                children: [
                  Expanded(child: current),
                  const SizedBox(width: 8),
                  reset,
                ],
              );
            }
            return Column(
              crossAxisAlignment: CrossAxisAlignment.stretch,
              mainAxisSize: MainAxisSize.min,
              children: [
                current,
                Align(alignment: Alignment.centerRight, child: reset),
              ],
            );
          },
        ),
        const SizedBox(height: 8),
        _choice<String>(
          p,
          keyPrefix: 'preset',
          options: [
            for (final mode in StreamPreset.values) (mode.name, mode.label),
          ],
          selected: _preset?.name ?? '',
          tooltip: (name) => StreamPreset.values.byName(name).detail,
          onSelected: (name) => _choose(StreamPreset.values.byName(name)),
        ),
        const SizedBox(height: 6),
        KeepAllText(
          l10n.workKeepsTextSharpSaver,
          style: uiText(size: 12, height: 1.5, color: p.muted),
        ),
        const SizedBox(height: 12),
        KeepAllText(
          '$_fps fps · ${AndroidStreamQuality.bitrateLabel(_bitrate)}',
          key: const Key('android-c-stream-summary'),
          style: uiText(size: 12.5, weight: FontWeight.w600, color: p.ink),
        ),
        const SizedBox(height: 10),
        AppDisclosure(
          headerKey: const Key('android-c-stream-advanced'),
          label: l10n.advancedSettings,
          maintainState: true,
          children: [
            Align(alignment: Alignment.centerLeft, child: _label(p, l10n.targetFrameRate)),
            Align(
              alignment: Alignment.centerLeft,
              child: _choice<int>(
                p,
                keyPrefix: 'fps',
                options: const [(30, '30 fps'), (60, '60 fps')],
                selected: _fps,
                onSelected: (v) => _fps = v,
              ),
            ),
            const SizedBox(height: 16),
            Align(
              alignment: Alignment.centerLeft,
              child: _label(p, l10n.videoBitrateMbps),
            ),
            Align(
              alignment: Alignment.centerLeft,
              child: _choice<int>(
                p,
                keyPrefix: 'bitrate',
                options: [
                  for (final v in AndroidStreamQuality.bitrates)
                    (v, v == AndroidStreamQuality.automatic ? l10n.auto : '$v'),
                ],
                tooltip: AndroidStreamQuality.bitrateLabel,
                selected: _bitrate,
                onSelected: (v) => _bitrate = v,
              ),
            ),
            const SizedBox(height: 12),
          ],
        ),
        const SizedBox(height: 6),
        AppDialog.message(
          context,
          l10n.fitsOneVirtualDisplayDevices,
        ),
      ],
      actions: [
        TextButton(
          onPressed: () => Navigator.pop(context),
          style: AndroidTvFocus.style(
            context,
            AppButtons.quiet(p).copyWith(
              minimumSize: const WidgetStatePropertyAll(Size(48, 48)),
              shape: WidgetStatePropertyAll(
                RoundedRectangleBorder(borderRadius: BorderRadius.circular(10)),
              ),
            ),
          ),
          child: Text(l10n.cancel),
        ),
        FilledButton(
          onPressed: () => Navigator.pop(
            context,
            AndroidStreamQuality(
              fps: _fps,
              bitrate: _bitrate,
            ).options,
          ),
          style: AndroidTvFocus.style(
            context,
            AppButtons.filled(p).copyWith(
              minimumSize: const WidgetStatePropertyAll(Size(48, 48)),
              shape: WidgetStatePropertyAll(
                RoundedRectangleBorder(borderRadius: BorderRadius.circular(10)),
              ),
            ),
          ),
          child: Text(l10n.connect2),
        ),
      ],
    );
  }
}

class AndroidPasswordDialog extends StatefulWidget {
  const AndroidPasswordDialog({
    super.key,
    required this.api,
    required this.token,
  });
  final BrokerApi api;
  final String token;
  @override
  State<AndroidPasswordDialog> createState() => _AndroidPasswordDialogState();
}

class _AndroidPasswordDialogState extends State<AndroidPasswordDialog> {
  final _old = TextEditingController(),
      _next = TextEditingController(),
      _confirm = TextEditingController();
  bool _busy = false;
  String? _error;
  @override
  void dispose() {
    _old.dispose();
    _next.dispose();
    _confirm.dispose();
    super.dispose();
  }

  Future<void> _save() async {
    if (_busy) return;
    if (_next.text != _confirm.text ||
        _next.text.length < 10 ||
        _next.text.length > 256) {
      setState(() => _error = l10n.newPasswordMust10256);
      return;
    }
    setState(() {
      _busy = true;
      _error = null;
    });
    try {
      await widget.api.changePassword(
        widget.token,
        _old.text,
        _next.text,
        _confirm.text,
      );
      if (mounted) Navigator.pop(context, true);
    } catch (error) {
      if (mounted) {
        setState(
          () => _error = error is BrokerException
              ? error.message
              : l10n.couldntChangePassword,
        );
      }
    } finally {
      if (mounted) setState(() => _busy = false);
    }
  }

  @override
  Widget build(BuildContext context) {
    final p = AppPalette.of(context);
    return PopScope(
      canPop: !_busy,
      child: AppDialog(
        icon: Icons.password_rounded,
        tone: AppTone.info,
        busy: _busy,
        title: l10n.changePassword,
        subtitle: l10n.changingSignsYouOutAll,
        body: [
          for (final (index, item) in [
            (_old, l10n.currentPassword),
            (_next, l10n.newPassword),
            (_confirm, l10n.confirmNewPassword),
          ].indexed)
            Padding(
              padding: const EdgeInsets.only(bottom: 12),
              child: AndroidTvTextEntry(
                controller: item.$1,
                label: item.$2,
                enabled: !_busy,
                obscureText: true,
                maxLength: 256,
                action: index < 2 ? TextInputAction.next : TextInputAction.done,
                onSubmitted: index < 2 ? null : (_) => unawaited(_save()),
                builder: (focusNode, readOnly, edit) => TextField(
                  controller: item.$1,
                  focusNode: focusNode,
                  readOnly: readOnly,
                  onTap: edit,
                  autofocus: index == 0 && AndroidTvScope.of(context),
                  enabled: !_busy,
                  obscureText: true,
                  keyboardType: TextInputType.visiblePassword,
                  enableSuggestions: false,
                  autocorrect: false,
                  textInputAction: index < 2
                      ? TextInputAction.next
                      : TextInputAction.done,
                  onSubmitted: index < 2 ? null : (_) => unawaited(_save()),
                  decoration: InputDecoration(
                    labelText: item.$2,
                    helperText: index == 1 ? l10n.least10Characters : null,
                  ),
                ),
              ),
            ),
          if (_error != null)
            KeepAllText(
              _error!,
              style: uiText(size: 13, height: 1.5, color: p.red),
            ),
        ],
        actions: [
          TextButton(
            onPressed: _busy ? null : () => Navigator.pop(context),
            style: AndroidTvFocus.style(context, AppButtons.quiet(p)),
            child: Text(l10n.cancel),
          ),
          FilledButton(
            onPressed: _busy ? null : _save,
            style: AndroidTvFocus.style(context, AppButtons.filled(p)),
            child: Text(l10n.change),
          ),
        ],
      ),
    );
  }
}

enum _Silent { signedIn, noPassword, rejected, unreachable }
