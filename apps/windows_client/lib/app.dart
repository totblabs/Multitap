import 'dart:async';

import 'package:flutter/material.dart';
import 'package:flutter/services.dart';

import 'ui/themed_app.dart';
import 'data/appearance_settings.dart';
import 'ui/sign_in_screen.dart';
import 'ui/mfa_login_dialog.dart';
import 'ui/mfa_account_dialog.dart';
import 'data/mfa_account_host.dart';
import 'data/console_browser.dart';
import 'ui/workspace_shell.dart';
import 'data/broker_api.dart';
import 'data/preview_desktops.dart';
import 'models/desktop_machine.dart';
import 'data/login_memory.dart';
import 'data/remote_session.dart';
import 'data/track_stream.dart';
import 'data/initial_viewport.dart';
import 'models/stream_options.dart';
import 'data/desktop_watch.dart';
import 'data/notices.dart';
import 'models/vm_resources.dart';
import 'data/console_resume.dart';
import 'data/session_windows.dart';
import 'data/session_renewal.dart';
import 'data/session_activity.dart';
import 'ui/session_access_boundary.dart';
import 'data/update_handoff.dart';
import 'data/client_tray.dart';
import 'data/desktop_mapping.dart';
import 'data/launcher_memory.dart';
import 'data/session_guide_memory.dart';
import 'data/desktop_thumbnails.dart';
import 'data/desktop_previews.dart';
import 'data/product_updates.dart';
import 'ui/bulk_host_update_dialog.dart';
import 'models/session_display_mode.dart';

import 'package:flutter/foundation.dart';
import 'l10n/l10n.dart';

/// How often the list is read while the live channel is not connected.
const windowsFallbackListRead = Duration(seconds: 10);

class MultitapApp extends StatefulWidget {
  const MultitapApp({
    super.key,
    this.api,
    this.enableDemo = false,
    this.serverOrigin,
    this.onServerChanged,
    this.loginMemory = const NoopLoginMemory(),
    this.desktopWatchFactory,
    this.appearance,
    this.launcherMemory,
    this.thumbnails,
    this.updateCheck,
    this.hostUpdateReleaseLoader,
    this.confirmExit = false,
    this.tray,
    this.sessionRetryDelay = const Duration(seconds: 15),
    this.sessionRenewalLead = const Duration(minutes: 5),
    this.clock = DateTime.now,
  });
  final BrokerApi? api;

  /// Wait between silent sign-in attempts while the broker is unreachable,
  /// and how long before the expiry the session is renewed.
  final Duration sessionRetryDelay, sessionRenewalLead;

  /// The time source of session lifetimes (tests substitute a virtual one).
  final DateTime Function() clock;

  /// Ask before the main window closes (the Windows client).
  final bool confirmExit;

  /// The notification-area hand-off of the Windows client's main window;
  /// the native one when null and [confirmExit] is set.
  final ClientTray? tray;

  /// The newer client version on a server's public catalog, or null; the
  /// header's update badge. None when null. It throws when the catalog
  /// could not be read (the badge then keeps its last state).
  final Future<String?> Function(String server)? updateCheck;
  @visibleForTesting
  final Future<ProductRelease?> Function(String server)?
  hostUpdateReleaseLoader;

  /// Last-frame pictures of streamed desktops; none when null.
  final DesktopThumbnails? thumbnails;

  /// Last desktop and per-desktop streaming mode; per process when null.
  final LauncherMemory? launcherMemory;
  final bool enableDemo;
  final Uri? serverOrigin;
  final Future<void> Function(Uri)? onServerChanged;
  final LoginMemory loginMemory;
  final DesktopWatchFactory? desktopWatchFactory;
  final AppAppearance? appearance;

  @override
  State<MultitapApp> createState() => _MultitapAppState();
}

class _MultitapAppState extends State<MultitapApp> with WidgetsBindingObserver {
  late final AppAppearance _appearance = widget.appearance ?? AppAppearance();
  late final LauncherMemory _launcherMemory =
      widget.launcherMemory ?? SessionLauncherMemory();
  // Shared with the app shell: it listens for "the window went to the tray",
  // and the sign-in lapse raises a balloon while the window rests there.
  late final ClientTray? _tray = widget.confirmExit
      ? (widget.tray ?? ClientTray.windows())
      : null;
  bool _previewOpen = false;
  late Uri? _origin = widget.serverOrigin;

  /// The API of [_origin], made when it is first needed (a sign-in).
  /// [_madeApi] is the one that exists, and closing looks only there: an API
  /// cannot be made for an HTTP address whose approval is gone, and saving
  /// another address removes the previous one's approval.
  BrokerApi? _madeApi;
  bool _apiMade = false;
  BrokerApi? get _api {
    if (!_apiMade) {
      final origin = _origin;
      _madeApi = widget.api ?? (origin == null ? null : _newApi(origin));
      _apiMade = true;
    }
    return _madeApi;
  }

  set _api(BrokerApi? value) {
    _madeApi = value;
    _apiMade = true;
  }

  static HttpBrokerApi _newApi(Uri origin) => HttpBrokerApi(
    origin: origin,
    enableDiagnostics: true,
    requestTextInput: true,
    requestDisplayScaleControl: true,
    requestPrint: true,
    requestUdpMedia: true,
    requestAudioOpus: true,
    requestPcLoginReplacement: true,
    requestStreamTakeoverContext: true,
  );
  BrokerSession? _session;
  late final _mfa = MfaAccountHost(clock: widget.clock);
  late final _sessionAccess = SessionActivity(clock: widget.clock);
  String? _authenticationOwner;
  Future<void> _authenticationBound = Future<void>.value();
  int _authenticationRevision = 0;
  Future<void> _authenticationRetirement = Future<void>.value();
  List<DesktopMachine> _desktops = [];
  _HostBatchScope? _hostBatch;
  int _assignedReadSequence = 0;
  bool _busy = false;
  String? _error;
  String? _listFailure;
  Timer? _expiry;
  Timer? _powerRefresh;
  int _fallbackTicks = 0;
  final _powerRequests = <String, int>{};

  /// The broker's stop permission from the latest answer to this client's
  /// running power request (a listing read meanwhile predates it).
  final _powerCanStop = <String, bool>{};

  /// When this client asked for each power task still running (its own
  /// clock): the launcher counts the wait from it. Replaced, never mutated,
  /// so the launcher sees the change.
  Map<String, DateTime> _powerStarted = const {};
  int _powerSequence = 0;
  final _displayRequests = <String>{};
  int _listSequence = 0;
  int? _quietRefreshOwner, _manualRefreshOwner;
  int _quietFailures = 0;
  DateTime? _quietRetryAt;
  bool get _quietRefreshing => _quietRefreshOwner != null;

  /// A broker update restarting the server: list reads wait through it.
  late final _restartWait = ServerRestartWait(clock: () => widget.clock());
  Timer? _restartRetry;

  /// A refresh the user pressed is waiting through that restart (one line).
  bool _restartShown = false;
  bool _foreground = true;
  DesktopUpdates? _desktopUpdates;
  StreamSubscription<void>? _desktopChanges, _catalogFeed;
  StreamSubscription<String>? _terminalAuthFeed;

  /// 공지 (data/notices.dart): this sign-in's notices, the live channel's
  /// `notices` pushes, and the notice last said in a tray balloon.
  NoticeCenter? _notices;
  StreamSubscription<void>? _noticeFeed;
  String? _noticeBalloon;

  void _bindNotices(BrokerSession session) {
    _releaseNotices();
    final api = _api;
    if (api is! NoticeBrokerApi) return;
    final center = NoticeCenter(
      api: api as NoticeBrokerApi,
      token: session.token,
      clock: widget.clock,
      // While the live channel is up it says when to read; the timer is the
      // fallback (the window in the tray keeps no channel).
      pushConnected: () => _liveChannel.value,
    );
    _notices = center;
    center.addListener(_noticesChanged);
    center.start();
  }

  void _releaseNotices() {
    final center = _notices;
    _notices = null;
    _noticeBalloon = null;
    center?.removeListener(_noticesChanged);
    center?.dispose();
  }

  /// The window rests in the tray: a notice that arrives is said in a tray
  /// balloon (its title only); its banner waits for the window.
  void _noticesChanged() {
    final banner = _notices?.banner;
    if (banner == null || _foreground || banner.id == _noticeBalloon) return;
    _noticeBalloon = banner.id;
    final shown = _tray?.balloon(l10n.notice(banner.title));
    if (shown != null) unawaited(shown.catchError((Object _) => false));
  }
  StreamSubscription<List<LiveResourceSample>>? _resourceFeed;

  /// The broker said the public download catalog changed (update badges).
  final _catalogPushes = StreamController<void>.broadcast();
  final _liveChannel = ValueNotifier<bool>(false);
  VoidCallback? _liveListener;
  int _listGeneration = 1;
  int _desktopRevision = 0, _appliedDesktopRevision = 0;
  int _localMonitors = 0;

  /// The signed-in account's picture folder and the launcher's view of it.
  String? _thumbnailKey;
  ThumbnailSource? _thumbnailSource;

  /// The broker's launcher pictures for this sign-in (desktop-preview-v1).
  DesktopPreviewSource? _previewSource;
  StreamSubscription<ThumbnailUpdate>? _thumbnailNotices;
  int _thumbnailObservationRevision = 0;
  bool get _detachedWindows =>
      !kIsWeb &&
      defaultTargetPlatform == TargetPlatform.windows &&
      _api is HttpBrokerApi;

  final _browserConsolePending = <String>{};
  Future<void> _endConsoles(String desktopId) async {
    final api = _api, login = _session;
    if (api is! ConsoleTakeoverBrokerApi || login == null) {
      throw const BrokerException('session_expired', 401);
    }
    await (api as ConsoleTakeoverBrokerApi).endConsoles(login.token, desktopId);
  }

  /// A stream window's "Console로 열기": the user pressed it there, so no
  /// second question here. A refusal is said in a tray notice, since this
  /// window may be behind the stream window.
  Future<void> _consoleFromSessionWindow(String desktopId) async {
    final machine = _desktops.where((m) => m.id == desktopId).firstOrNull;
    if (!mounted || _previewOpen || machine == null) return;
    try {
      if (machine.kind != MachineKind.vm) {
        throw const BrokerException('desktop_not_found', 404);
      }
      await _openNegotiatedConsole(machine);
    } catch (error) {
      final text = error is BrokerException
          ? error.message
          : l10n.couldntOpenConsoleWindow;
      await _tray?.balloon(text).catchError((Object _) => false);
    }
  }

  Future<SessionWindowOpenResult?> _openNegotiatedConsole(
    DesktopMachine machine,
  ) async {
    final api = _api, login = _session, origin = _origin;
    await _authenticationBound;
    if (!_sessionAccess.check() || _session != login) {
      throw const BrokerException('session_expired', 401);
    }
    if (api == null ||
        login == null ||
        origin == null ||
        !login.expiresAt.isAfter(widget.clock())) {
      throw const BrokerException('session_expired', 401);
    }
    if (origin.scheme != 'https') {
      throw const BrokerException('console_browser_https_required');
    }
    if (!_browserConsolePending.add(machine.id)) return null;
    try {
      final assigned = await api.desktops(login.token);
      if (!mounted ||
          _api != api ||
          _session != login ||
          _origin != origin ||
          !login.expiresAt.isAfter(widget.clock())) {
        throw const BrokerException('session_expired', 401);
      }
      final matches = assigned.where((item) => item.id == machine.id).toList();
      if (matches.length != 1 || matches.single.kind != 'vm') {
        throw const BrokerException('desktop_not_found', 404);
      }
      ConsoleBrowser.requireSupport(
        origin,
        supported: matches.single.consoleWebViewSupported,
      );
      if (api is! ConsoleWebViewBrokerApi) {
        throw const BrokerException('console_webview_update_required');
      }
      await const ConsoleBrowser.windows().open(
        origin,
        machine.id,
        api: api as ConsoleWebViewBrokerApi,
        token: login.token,
        current: () =>
            mounted &&
            _api == api &&
            _session == login &&
            _origin == origin &&
            login.expiresAt.isAfter(widget.clock()),
      );
      return null;
    } finally {
      _browserConsolePending.remove(machine.id);
    }
  }

  Future<SessionWindowOpenResult?> _openWindow(
    DesktopMachine machine,
    SessionDisplayMode mode,
    StreamOptions options,
  ) async {
    final session = _session, origin = _origin;
    await _authenticationBound;
    if (!_sessionAccess.check() || _session != session) {
      throw const BrokerException('session_expired', 401);
    }
    if (session == null ||
        origin == null ||
        !session.expiresAt.isAfter(widget.clock())) {
      throw const BrokerException('session_expired', 401);
    }
    if (!_desktops.any((desktop) => desktop.id == machine.id)) {
      throw const BrokerException('desktop_not_found', 404);
    }
    if (machine.kind == MachineKind.pc && !machine.localPcApproved) {
      throw const BrokerException('desktop_not_found', 404);
    }
    return SessionWindows.open(
      SessionWindowLaunch(
        origin: origin,
        token: session.token,
        expiresAt: session.expiresAt,
        pcSessionPolicy: session.pcSessionPolicy,
        authenticationOwner: _authenticationOwner,
        sessionAccess: _sessionAccess.policy,
        desktopId: machine.id,
        name: machine.name,
        localPcApproved: machine.localPcApproved,
        mode: mode,
        options: options,
        thumbnailKey: _thumbnailKey,
        thumbnailEpoch: _thumbnailKey == null
            ? null
            : widget.thumbnails?.allowedEpoch(_thumbnailKey!, machine.id),
        guideAccountKey: sessionGuideAccountKey(origin, session.accountId),
        requestedAt: DateTime.now(),
      ),
    );
  }

  void _bindThumbnails(BrokerSession session) {
    _bindNotices(session); // every sign-in passes here
    unawaited(_thumbnailNotices?.cancel());
    _thumbnailNotices = null;
    final api = _api;
    _previewSource = api is DesktopPreviewBrokerApi
        ? BrokerDesktopPreviews(api as DesktopPreviewBrokerApi, session.token)
        : null;
    final store = widget.thumbnails, origin = _origin;
    final account = session.accountId;
    if (store == null || origin == null || account == null) {
      _thumbnailKey = null;
      _thumbnailSource = null;
      return;
    }
    final key = thumbnailAccountKey(origin, account);
    _thumbnailKey = key;
    store.suspendAccount(key);
    _thumbnailNotices = SessionWindows.thumbnailUpdates.listen((notice) {
      if (_thumbnailKey == key && notice.key == key) store.acceptNotice(notice);
    });
    _thumbnailSource = AccountThumbnails(store, key);
    // Another account or server signed in: earlier pictures are removed.
    unawaited(store.retainOnly(key).catchError((Object _) {}));
  }

  /// Sign-out removes the account's pictures; expiry or a password change
  /// keeps them for the same account's next sign-in.
  void _releaseThumbnails({required bool delete}) {
    _releaseNotices(); // every sign-out passes here
    unawaited(_thumbnailNotices?.cancel());
    _thumbnailNotices = null;
    final store = widget.thumbnails, key = _thumbnailKey;
    if (store != null && key != null) store.suspendAccount(key);
    _thumbnailObservationRevision++;
    _thumbnailKey = null;
    _thumbnailSource = null;
    _previewSource = null;
    if (delete && store != null && key != null) {
      unawaited(store.forget(key).catchError((Object _) {}));
    }
  }

  void _withdrawThumbnailPolicies() {
    _thumbnailObservationRevision++;
    final key = _thumbnailKey;
    if (key != null) widget.thumbnails?.suspendAccount(key);
  }

  /// Only the winning authenticated inventory response establishes Host policy.
  /// Native notices, resource pushes and cached DesktopMachine copies do not.
  void _applyThumbnailPolicies(List<DesktopMachine> machines) {
    final store = widget.thumbnails, key = _thumbnailKey;
    if (store == null || key == null) return;
    store.retainDesktops(key, {for (final machine in machines) machine.id});
    for (final machine in machines) {
      final retired = store.observePrivacy(
        key,
        machine.id,
        machine.thumbnailPrivacy,
      );
      for (final epoch in retired) {
        unawaited(SessionWindows.invalidateThumbnail(key, machine.id, epoch));
      }
    }
  }

  void _retireWindows() {
    if (_detachedWindows) {
      unawaited(
        SessionWindows.closeAll('session_expired').catchError((Object _) {}),
      );
    }
  }

  void _invalidateListReads({bool resetSession = false}) {
    _listSequence++;
    _quietRefreshOwner = null;
    if (resetSession) {
      _manualRefreshOwner = null;
      _desktopRevision = _appliedDesktopRevision = 0;
      _quietRetryAt = null;
      _quietFailures = 0;
      _restartRetry?.cancel();
      _restartWait.reset();
      _restartShown = false;
    }
  }

  @override
  void initState() {
    super.initState();
    WidgetsBinding.instance.addObserver(this);
    final lifecycle = WidgetsBinding.instance.lifecycleState;
    _foreground = lifecycle == null || _visible(lifecycle);
    _powerRefresh = Timer.periodic(const Duration(seconds: 2), (_) {
      if (_session != null &&
          _foreground &&
          _desktops.any(
            (vm) => vm.resources.expired(DateTime.now(), after: _sampleLife),
          )) {
        setState(
          () => _desktops = [
            for (final vm in _desktops)
              vm.resources.expired(DateTime.now(), after: _sampleLife)
                  ? vm.withResources(vm.resources.asStale())
                  : vm,
          ],
        );
      }
      if (_session != null &&
          _foreground &&
          !_busy &&
          !_quietRefreshing &&
          _powerRequests.isEmpty &&
          _displayRequests.isEmpty) {
        // Push first: while the live channel is connected the broker says
        // when anything changed (its state machine, server/src/vm-state.mjs),
        // and this timer only reads a change it could not read before. While
        // the channel is down the list is read gently, never every 2 s.
        if (_desktopUpdates?.connected == true) {
          _fallbackTicks = 0;
          if (_desktopRevision != _appliedDesktopRevision) {
            unawaited(_refresh(quiet: true));
          }
        } else if (++_fallbackTicks >= windowsFallbackListRead.inSeconds ~/ 2) {
          _fallbackTicks = 0;
          unawaited(_refresh(quiet: true));
        }
      }
    });
    _readLocalDisplays();
  }

  Timer? _displayRead;

  /// How long a CPU/RAM sample stays current: the live channel pushes a new
  /// one about every 5 s, so silence for 20 s means the broker's read failed;
  /// without the channel only the gentle fallback list read refreshes it.
  Duration get _sampleLife => _desktopUpdates?.connected == true
      ? const Duration(seconds: 20)
      : windowsFallbackListRead + const Duration(seconds: 5);

  /// Pushed values replace the tile's CPU/RAM/uptime without a list read.
  void _applyResources(List<LiveResourceSample> samples) {
    if (!mounted || _session == null || _desktops.isEmpty) return;
    final byId = {for (final sample in samples) sample.id: sample};
    if (!_desktops.any((vm) => byId.containsKey(vm.id))) return;
    setState(
      () => _desktops = [
        for (final vm in _desktops)
          byId.containsKey(vm.id)
              ? vm.withResources(byId[vm.id]!.resources ?? const VmResources())
              : vm,
      ],
    );
  }

  /// A list read that predates a pushed sample must not bring older values back.
  List<DesktopMachine> _keepNewerResources(List<DesktopMachine> next) {
    final previous = {for (final vm in _desktops) vm.id: vm.resources};
    return [
      for (final vm in next)
        switch ((previous[vm.id]?.checkedAt, vm.resources.checkedAt)) {
          (final DateTime before, final DateTime after)
              when before.isAfter(after) && !previous[vm.id]!.stale =>
            vm.withResources(previous[vm.id]!),
          _ => vm,
        },
    ];
  }

  /// This PC's monitor count ("모든 모니터" for independent streaming
  /// outputs). Read again after a display or window change and on returning
  /// to the foreground, so a monitor plugged in later is offered without
  /// restarting the app. No answer counts as none.
  void _readLocalDisplays() {
    unawaited(
      const MethodChannel('multitap/window')
          .invokeMethod<int>('localMonitorCount')
          .then((count) => count ?? 0, onError: (Object _) => 0)
          .then((count) {
            if (mounted && count != _localMonitors) {
              setState(() => _localMonitors = count);
            }
          }),
    );
  }

  @override
  void didChangeMetrics() {
    _displayRead?.cancel();
    _displayRead = Timer(const Duration(milliseconds: 500), _readLocalDisplays);
  }

  /// Resumed or merely unfocused: the window can still be seen.
  static bool _visible(AppLifecycleState state) =>
      state == AppLifecycleState.resumed || state == AppLifecycleState.inactive;

  void _startDesktopUpdates() {
    final session = _session, origin = _origin;
    if (!_foreground ||
        session == null ||
        origin == null ||
        _desktopUpdates != null ||
        (widget.desktopWatchFactory == null && _api is! HttpBrokerApi)) {
      return;
    }
    try {
      final updates =
          widget.desktopWatchFactory?.call(origin, session.token) ??
          BrokerDesktopWatch(origin: origin, token: session.token);
      _desktopUpdates = updates;
      var wasConnected = false;
      void sync() {
        if (_session != session || _desktopUpdates != updates) return;
        if (wasConnected && !updates.connected) _withdrawThumbnailPolicies();
        // The channel is (back) up: read what was sent meanwhile.
        if (!wasConnected && updates.connected) _notices?.signal();
        wasConnected = updates.connected;
        _liveChannel.value = updates.connected;
      }

      _liveListener = sync;
      updates.addListener(sync);
      _desktopChanges = updates.changes.listen(
        (_) {
          if (!mounted || _session != session || _desktopUpdates != updates) {
            return;
          }
          _desktopRevision++;
          // This notice may be a lock/logoff or assignment change; cached
          // permission cannot stay visible while its new snapshot is pending.
          _withdrawThumbnailPolicies();
          if (_foreground &&
              !_busy &&
              !_quietRefreshing &&
              _powerRequests.isEmpty &&
              _displayRequests.isEmpty) {
            unawaited(_refresh(quiet: true));
          }
        },
        onError: (Object _, StackTrace _) {
          if (_session == session && _desktopUpdates == updates) {
            _stopDesktopUpdates();
          }
        },
        onDone: () {
          if (_session == session && _desktopUpdates == updates) {
            _stopDesktopUpdates();
          }
        },
      );
      if (updates is LiveDesktopFeeds) {
        final feeds = updates as LiveDesktopFeeds;
        _resourceFeed = feeds.resources.listen(
          _applyResources,
          onError: (Object _, StackTrace _) {},
        );
        _catalogFeed = feeds.catalog.listen((_) {
          if (mounted && _session == session && _desktopUpdates == updates) {
            _catalogPushes.add(null);
          }
        }, onError: (Object _, StackTrace _) {});
      }
      if (updates is NoticeFeed) {
        _noticeFeed = (updates as NoticeFeed).notices.listen((_) {
          if (mounted && _session == session && _desktopUpdates == updates) {
            _notices?.signal();
          }
        }, onError: (Object _, StackTrace _) {});
      }
      if (updates is TerminalDesktopAuthFeed) {
        _terminalAuthFeed = (updates as TerminalDesktopAuthFeed)
            .terminalAuthentication
            .listen((code) {
              if (mounted &&
                  _session == session &&
                  _desktopUpdates == updates &&
                  const {
                    'pc_login_replaced',
                    'session_idle_timeout',
                    'session_reauthentication_required',
                  }.contains(code)) {
                _retireAuthentication(session, code);
              }
            });
      }
      updates.start();
    } catch (_) {
      // Events are optional: authenticated HTTP polling must remain available.
      _stopDesktopUpdates();
    }
  }

  void _stopDesktopUpdates() {
    unawaited(_terminalAuthFeed?.cancel());
    _terminalAuthFeed = null;
    _withdrawThumbnailPolicies();
    final updates = _desktopUpdates, changes = _desktopChanges;
    _desktopUpdates = null;
    _desktopChanges = null;
    final listener = _liveListener;
    _liveListener = null;
    if (updates != null && listener != null) updates.removeListener(listener);
    _liveChannel.value = false;
    final resourceFeed = _resourceFeed, catalogFeed = _catalogFeed;
    final noticeFeed = _noticeFeed;
    _resourceFeed = _catalogFeed = _noticeFeed = null;
    for (final feed in [resourceFeed, catalogFeed, noticeFeed]) {
      if (feed != null) {
        unawaited(Future<void>.sync(feed.cancel).catchError((Object _) {}));
      }
    }
    if (changes != null) {
      unawaited(Future<void>.sync(changes.cancel).catchError((Object _) {}));
    }
    if (updates != null) {
      unawaited(Future<void>.sync(updates.close).catchError((Object _) {}));
    }
  }

  @override
  void didChangeAppLifecycleState(AppLifecycleState state) {
    _mfa.checkPrivacyIdle();
    if (!_sessionAccess.check()) return;
    // An unfocused but visible window (state inactive on Windows) keeps the
    // live channel: its list must follow power and Host changes by itself.
    _foreground = _visible(state);
    if (_foreground) {
      _startDesktopUpdates();
      _readLocalDisplays();
    } else {
      _stopDesktopUpdates();
    }
    if (_foreground &&
        _session != null &&
        !_busy &&
        !_quietRefreshing &&
        _powerRequests.isEmpty &&
        _displayRequests.isEmpty) {
      unawaited(_refresh(quiet: true));
    }
  }

  Future<void> _changeServer(Uri origin) async {
    if (_busy || _session != null) {
      throw StateError('Sign out before changing server.');
    }
    _mfa.retire();
    // An address this app cannot use is refused here, before it is written.
    // Once the settings hold the new address, the app takes it: what follows
    // the write only assigns and closes.
    final next = _newApi(origin);
    try {
      await widget.onServerChanged?.call(origin);
    } catch (_) {
      next.close();
      rethrow;
    }
    if (!mounted) {
      next.close();
      return;
    }
    UpdateNotices.instance.configure(
      server: origin.toString(),
      currentVersion: clientProductVersion,
    );
    // Only an API that exists is closed: the previous address may have lost
    // its HTTP approval to this save, and none can be made for it now.
    final previous = _madeApi;
    setState(() {
      _origin = origin;
      _api = next;
      _error = null;
    });
    previous?.close();
  }

  @override
  void dispose() {
    _mfa.dispose();
    _sessionAccess.dispose();
    // The API that exists; leaving the app makes none.
    final closingApi = _madeApi;
    unawaited(
      const ConsoleBrowser.windows()
          .closeAll(api: closingApi)
          .whenComplete(() => closingApi?.close()),
    );
    unawaited(_thumbnailNotices?.cancel());
    _thumbnailNotices = null;
    _releaseNotices();
    _hostBatch?.dispose();
    _hostBatch = null;
    if (widget.appearance == null) _appearance.dispose();
    _stopKeeping();
    _powerRefresh?.cancel();
    _displayRead?.cancel();
    _restartRetry?.cancel();
    WidgetsBinding.instance.removeObserver(this);
    _stopDesktopUpdates();
    unawaited(_catalogPushes.close());
    _liveChannel.dispose();
    super.dispose();
  }

  Future<SignInResult> _login(
    BuildContext loginContext,
    String name,
    String password,
  ) async {
    final api = _api;
    if (_busy ||
        api == null ||
        _origin == null ||
        _sessionAccess.reason != null) {
      return SignInResult.failed;
    }
    _mfa.retire();
    final revision = ++_authenticationRevision;
    setState(() {
      _busy = true;
      _error = null;
    });
    try {
      final origin = _origin;
      bool current() =>
          mounted &&
          loginContext.mounted &&
          _api == api &&
          _origin == origin &&
          revision == _authenticationRevision;
      // All old close requests finish before this owner can open children.
      await _authenticationRetirement;
      if (!loginContext.mounted || !current()) return SignInResult.failed;
      BrokerSession? session;
      try {
        session = await confirmManualLogin(
          loginContext,
          api: api,
          current: current,
          attempt: () => api is InteractiveLoginBrokerApi
              ? (api as InteractiveLoginBrokerApi).loginInteractively(
                  name.trim(),
                  password,
                )
              : api.login(name.trim(), password),
        );
      } on MfaRequiredException catch (required) {
        if (!mounted || !loginContext.mounted || !current()) {
          required.challenge.forget();
          return SignInResult.failed;
        }
        session = await showMfaLogin(
          loginContext,
          api: api,
          challenge: required.challenge,
          current: current,
        );
      }
      if (session == null) return SignInResult.failed;
      if (!current()) {
        await api.logout(session.token);
        return SignInResult.failed;
      }
      _invalidateListReads(resetSession: true);
      _session = session;
      _bindThumbnails(session);
      _keepAlive(session);
      await _refresh();
      if (_session == session) _startDesktopUpdates();
      return _session == session ? SignInResult.success : SignInResult.failed;
    } on BrokerException catch (error) {
      if (mounted && revision == _authenticationRevision) {
        setState(
          () => _error = pcLoginTerminalCode(error.code, error.status) == null
              ? error.message
              : pcLoginTerminalMessage(error.code),
        );
      }
      return error.code == 'invalid_credentials'
          ? SignInResult.rejected
          : error.code == 'network_error' || error.serverRestarting
          ? SignInResult.unreachable
          : SignInResult.failed;
    } catch (_) {
      if (mounted && revision == _authenticationRevision) {
        setState(() => _error = l10n.couldntGetResponseFromServer);
      }
      return SignInResult.unreachable;
    } finally {
      if (mounted && revision == _authenticationRevision) {
        setState(() => _busy = false);
      }
    }
  }

  SessionRenewer? _renewer;
  bool _recovering = false;

  void _stopKeeping() {
    _expiry?.cancel();
    _expiry = null;
    _renewer?.stop();
  }

  /// Keeps [session] alive: renews the same token before it expires, and only
  /// when that cannot work signs in again silently (see [_onLapse]).
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
      onPolicy: (policy) async {
        if (_session != session || _authenticationOwner != owner) {
          throw const BrokerException('session_expired', 401);
        }
        if (_detachedWindows) {
          await SessionWindows.broadcastDeadline(owner, policy);
        }
      },
      onTerminal: (code) => _retireAuthentication(session, code),
    );
    if (_detachedWindows) {
      SessionWindows.observeAuthentication(
        owner,
        human: _sessionAccess.human,
        terminal: (code) => _retireAuthentication(session, code),
      );
      // A stream window of this sign-in asks for its VM's Console (Windows
      // signed out or locked).
      SessionWindows.bindConsoleRequests(_consoleFromSessionWindow);
      _authenticationBound = SessionWindows.bindAuthentication(
        owner,
        session.sessionAccess,
      );
      // The open boundary awaits this same future; no unhandled late error.
      unawaited(_authenticationBound.catchError((Object _) {}));
    }
    _renewer = SessionRenewer(
      lead: widget.sessionRenewalLead,
      retry: widget.sessionRetryDelay,
      clock: widget.clock,
      renew: _renewCurrent,
      onRenewed: (next) {
        final current = _session;
        if (current == null || current != session) return;
        // Same token, later expiry: `_session == session` checks stay valid.
        _session = current.withExpiry(next);
        _recordRenewal(1, true);
      },
      onLapse: _onLapse,
    )..start(session.expiresAt);
  }

  Future<DateTime> _renewCurrent() {
    final api = _api, session = _session;
    if (session == null) {
      throw const BrokerException('session_expired', 401);
    }
    if (api is! SessionRenewalBrokerApi) {
      throw const BrokerException('renew_unsupported', 404);
    }
    return (api as SessionRenewalBrokerApi).renewSession(session.token);
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

  void _onLapse(RenewalLapse reason) {
    final session = _session;
    if (session == null) return;
    if (reason == RenewalLapse.replaced ||
        reason == RenewalLapse.idle ||
        reason == RenewalLapse.rejected) {
      _retireAuthentication(
        session,
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
    unawaited(_sessionLost(session, renewFirst: false, early: true));
  }

  /// A 401 (or a renewal that cannot work). The token itself is gone or about
  /// to be, so: try renewal once (a skewed clock can say "expired" early),
  /// then sign in again with the remembered password. Only when neither is
  /// possible the sign-in screen appears.
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
    _withdrawThumbnailPolicies();
    _recovering = true;
    try {
      if (renewFirst && _renewer != null) {
        if (await _renewer!.renewNow()) {
          _recovering = false;
          if (mounted && _session == session) {
            unawaited(_refresh(quiet: true, force: true));
          }
          return;
        }
        if (!mounted || _session != session) return;
      }
      if (session.sessionAccess != null) {
        // Negotiated owners renew the same bearer only. Absolute expiry and
        // terminal retirement must never turn into remembered-password login.
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
        if (result == _Silent.signedIn) return;
        final stillValid = session.expiresAt.isAfter(widget.clock());
        if (result == _Silent.unreachable &&
            attempt < 7 &&
            (stillValid || attempt < 3)) {
          await Future<void>.delayed(widget.sessionRetryDelay);
          if (!mounted || _session != session) return;
          continue;
        }
        if (early && stillValid && result == _Silent.noPassword) {
          // Nothing to sign in with: the working session is left alone until
          // its real expiry (open streams are not cut earlier).
          _expiry?.cancel();
          _expiry = Timer(
            session.expiresAt.difference(widget.clock()),
            () => _showExpired(),
          );
          return;
        }
        break;
      }
      _showExpired();
    } finally {
      _recovering = false;
    }
  }

  Future<_Silent> _silentLogin(BrokerSession session) async {
    if (_mfa.blocked) { return _Silent.noPassword; }
    final api = _api, origin = _origin;
    if (api == null || origin == null) return _Silent.noPassword;
    final memory = widget.loginMemory, revision = _authenticationRevision;
    bool owns(BrokerSession owner) =>
        mounted &&
        _api == api &&
        _origin == origin &&
        _authenticationRevision == revision &&
        _session == owner &&
        identical(widget.loginMemory, memory);
    if (!owns(session)) return _Silent.noPassword;
    final RememberedLogin remembered;
    try {
      remembered = await memory.load(origin);
    } catch (_) {
      return _Silent.noPassword;
    }
    final password = remembered.password;
    if (password == null ||
        password.isEmpty ||
        remembered.username.toLowerCase() != session.username.toLowerCase()) {
      return _Silent.noPassword;
    }
    if (!owns(session)) return _Silent.noPassword;
    final BrokerSession next;
    try {
      if (_mfa.blocked) { return _Silent.noPassword; }
      _mfa.retire();
      next = await api.login(remembered.username, password);
    } on MfaRequiredException catch (required) {
      required.challenge.forget();
      if (!owns(session)) return _Silent.noPassword;
      _recordRenewal(
        2,
        false,
        code: 'authentication_required',
        token: session.token,
      );
      return _Silent.noPassword;
    } on BrokerException catch (error) {
      if (!owns(session)) return _Silent.noPassword;
      if (pcLoginTerminalCode(error.code, error.status) != null) {
        _retireAuthentication(session, error.code);
        return _Silent.noPassword;
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
        // Erase only the exact credential used by this still-current owner.
        // A newer login or queued save can replace it while the POST waits.
        try {
          if (memory is ConditionalLoginMemory) {
            await (memory as ConditionalLoginMemory).forgetPasswordIfMatches(
              origin,
              remembered,
              current: () => owns(session),
            );
          } else {
            final latest = await memory.load(origin);
            if (owns(session) &&
                latest.username == remembered.username &&
                latest.password == password) {
              await memory.forgetPassword(origin);
            }
          }
        } catch (_) {}
        return _Silent.rejected;
      }
      return _Silent.unreachable;
    } catch (_) {
      if (!owns(session)) return _Silent.noPassword;
      _recordRenewal(2, false, code: 'network_error', token: session.token);
      return _Silent.unreachable;
    }
    if (!owns(session)) {
      unawaited(api.logout(next.token).catchError((Object _) {}));
      return _Silent.noPassword;
    }
    if (next.accountId != session.accountId) {
      unawaited(api.logout(next.token).catchError((Object _) {}));
      return _Silent.rejected;
    }
    _stopDesktopUpdates();
    _session = next;
    _bindThumbnails(next);
    _keepAlive(next);
    _startDesktopUpdates();
    await _refresh(quiet: true, force: true);
    if (!owns(next)) return _Silent.noPassword;
    _recordRenewal(2, true, token: next.token);
    return _Silent.signedIn;
  }

  void _retireAuthentication(BrokerSession owner, String code) {
    _mfa.sessionRetired(owner.token, code);
    if (!mounted || _session != owner) return;
    final api = _api;
    final detached = _detachedWindows;
    final windowOwner = _authenticationOwner;
    final origin = _origin;
    final tray = _tray;
    if (tray != null) {
      unawaited(
        tray
            .balloon(l10n.youWereSignedOutMultitap)
            .catchError((Object _) => false),
      );
    }
    _sessionAccess.retire(code);
    final hiddenNative = detached && windowOwner != null
        ? SessionWindows.hideAuthentication(windowOwner)
        : Future<void>.value();
    final hiddenConsoles = const ConsoleBrowser.windows().hideAll(
      api: api,
      token: owner.token,
    );
    unawaited(hiddenNative.catchError((Object _) {}));
    unawaited(hiddenConsoles.catchError((Object _) {}));
    final retirementRevision = ++_authenticationRevision;
    _hostBatch?.invalidate();
    _stopDesktopUpdates();
    _stopKeeping();
    _invalidateListReads(resetSession: true);
    setState(() {
      _session = null;
      _releaseThumbnails(delete: false);
      _desktops = [];
      _powerRequests.clear();
      _powerStarted = const {};
      _displayRequests.clear();
      _busy = false;
      _error = pcLoginTerminalMessage(code);
    });
    // Exact retired bearer only. Offline failure never restores it locally.
    if (api != null) {
      unawaited(
        api
            .logout(owner.token)
            .timeout(const Duration(seconds: 5))
            .catchError((Object _) {}),
      );
    }
    // A retry only re-observes this retired owner. It never signs in, repeats
    // logout or replays an old clipboard/file operation.
    final recoveryOwner = Object();
    var attempt = 0;
    bool current() =>
        mounted &&
        _session == null &&
        _api == api &&
        _origin == origin &&
        _authenticationOwner == windowOwner &&
        _authenticationRevision == retirementRevision;
    Future<void> observe({required bool initial}) async {
      if (!current()) return;
      final observedAttempt = ++attempt;
      final outcome = await observeSessionRetirement(
        routes: _sessionAccess.closeRoutes,
        nativeHide: () async {
          if (initial) {
            await hiddenNative;
          } else if (detached) {
            if (windowOwner == null) {
              throw StateError('missing_authentication_owner');
            }
            await SessionWindows.hideAuthentication(windowOwner);
          }
        },
        consoleHide: () => initial
            ? hiddenConsoles
            : const ConsoleBrowser.windows().hideAll(
                api: api,
                token: owner.token,
              ),
        native: () async {
          if (detached) {
            if (windowOwner == null) {
              throw StateError('missing_authentication_owner');
            }
            await SessionWindows.retireAuthentication(windowOwner);
          }
        },
        consoles: () async {
          return const ConsoleBrowser.windows().closeAllVerified(
            api: api,
            token: owner.token,
          );
        },
      );
      if (current() && observedAttempt == attempt) {
        _sessionAccess.finishRetirement(
          verified: outcome.verified,
          outcome: outcome,
          recoveryOwner: recoveryOwner,
        );
      }
    }

    _sessionAccess.bindRetirementRecovery(recoveryOwner, () {
      if (!current()) return Future<void>.value();
      final work = observe(initial: false);
      _authenticationRetirement = work;
      return work;
    });
    // Invoke exact-owner close immediately. Manual login also waits for each
    // later recovery observation before it can open a successor child.
    _authenticationRetirement = observe(initial: true);
  }

  /// An exhausted local lifetime uses the same reason/cleanup boundary.
  /// Explicit logout has already obtained its own confirmation.
  void _showExpired({bool acknowledge = true}) {
    final current = _session;
    if (acknowledge && current != null) {
      _retireAuthentication(current, 'session_expired');
      return;
    }
    _sessionAccess.stop();
    unawaited(
      const ConsoleBrowser.windows().closeAll(
        api: _api,
        token: _session?.token,
      ),
    );
    // Resting in the tray: a balloon is the only way to learn that signing in
    // again is needed (the native side shows it only while the window is
    // hidden; a click on it brings the window back).
    final tray = _tray;
    if (tray != null) {
      unawaited(
        tray.balloon(TrayNotices.signInLapsed).catchError((Object _) => false),
      );
    }
    _stopDesktopUpdates();
    _invalidateListReads(resetSession: true);
    _stopKeeping();
    if (mounted) {
      setState(() {
        _session = null;
        _releaseThumbnails(delete: false);
        _busy = false;
        _powerRequests.clear();
        _powerStarted = const {};
        _displayRequests.clear();
        _desktops = [];
        _error = l10n.signHasExpiredSignAgain;
      });
    }
  }

  DesktopMachine _toMachine(BrokerDesktop desktop) => machineFromBroker(
    desktop,
    powerPending: _powerRequests.containsKey(desktop.id),
    displayPending: _displayRequests.contains(desktop.id),
    canForceStop: _powerRequests.containsKey(desktop.id)
        ? _powerCanStop[desktop.id] ?? false
        : null,
  );

  /// [again]: the retry of a read that met a server restart.
  Future<void> _refresh({
    bool quiet = false,
    bool force = false,
    bool again = false,
  }) async {
    final session = _session;
    if (session == null) return;
    if (force) _invalidateListReads();
    if (quiet && _quietRefreshing) return;
    if (quiet &&
        !force &&
        !again &&
        _quietRetryAt?.isAfter(DateTime.now()) == true) {
      return;
    }
    // A refresh the user pressed waits through a restart afresh.
    if (!quiet) _restartWait.reset();
    _restartRetry?.cancel();
    Duration? restartWait;
    final request = ++_listSequence;
    final assignedRead = ++_assignedReadSequence;
    if (quiet) {
      _quietRefreshOwner = request;
    } else {
      _quietRefreshOwner = null;
      _manualRefreshOwner = request;
    }
    final revision = _desktopRevision;
    final thumbnailObservation = _thumbnailObservationRevision;
    var succeeded = false;
    if (mounted && !quiet) {
      setState(() {
        _busy = true;
        _error = null;
        _signOutError = null;
      });
    }
    try {
      final result = await _api!.desktops(session.token);
      if (mounted && _session == session && _listSequence == request) {
        setState(() {
          _desktops = _keepNewerResources(result.map(_toMachine).toList());
          if (thumbnailObservation == _thumbnailObservationRevision) {
            _applyThumbnailPolicies(_desktops);
          }
          _listGeneration++;
          if (!quiet || _error == _listFailure) _error = null;
          _listFailure = null;
          _appliedDesktopRevision = revision;
          _quietRetryAt = null;
          _quietFailures = 0;
          _restartWait.reset();
          _restartShown = false;
          succeeded = true;
        });
        _hostBatch?.publish(_desktops, assignedRead);
      }
    } on BrokerException catch (error) {
      if (mounted && _session == session) {
        if (error.status == 401 ||
            pcLoginTerminalCode(error.code, error.status) != null) {
          _withdrawThumbnailPolicies();
          _expired(error);
        } else if (_listSequence == request) {
          _withdrawThumbnailPolicies();
          restartWait = _restartWait.next(error);
          setState(() {
            if (restartWait != null) {
              // No error while the server restarts: the list stays, and a
              // refresh the user pressed says why it waits.
              if (!quiet) _restartShown = true;
            } else {
              _restartShown = false;
              if (!quiet || _error == null || _error == _listFailure) {
                _error = error.message;
              }
              _listFailure = error.message;
            }
            _desktops = quiet || restartWait != null
                ? [
                    for (final vm in _desktops)
                      vm.withResources(vm.resources.asStale()),
                  ]
                : [];
          });
        }
      }
    } catch (_) {
      if (mounted && _session == session && _listSequence == request) {
        _withdrawThumbnailPolicies();
        setState(() {
          _restartShown = false;
          if (!quiet || _error == null || _error == _listFailure) {
            _error = desktopListFailureText;
          }
          _listFailure = desktopListFailureText;
          _desktops = quiet
              ? [
                  for (final vm in _desktops)
                    vm.withResources(vm.resources.asStale()),
                ]
              : [];
        });
      }
    } finally {
      if (!succeeded &&
          mounted &&
          _session == session &&
          _listSequence == request) {
        final wait = restartWait;
        if (wait != null) {
          _quietRetryAt = DateTime.now().add(wait);
          _restartRetry = Timer(wait, () {
            if (mounted && _session == session) {
              unawaited(_refresh(quiet: true, again: true));
            }
          });
        } else {
          const delays = [2, 4, 8, 15];
          final index = _quietFailures < delays.length
              ? _quietFailures
              : delays.length - 1;
          _quietFailures++;
          _quietRetryAt = DateTime.now().add(Duration(seconds: delays[index]));
        }
      }
      if (_quietRefreshOwner == request) _quietRefreshOwner = null;
      if (!quiet && _manualRefreshOwner == request) {
        _manualRefreshOwner = null;
        if (mounted && _session == session) setState(() => _busy = false);
      }
      if (succeeded &&
          mounted &&
          _session == session &&
          _foreground &&
          !_busy &&
          !_quietRefreshing &&
          _powerRequests.isEmpty &&
          _displayRequests.isEmpty &&
          _desktopRevision != _appliedDesktopRevision) {
        scheduleMicrotask(() {
          if (mounted &&
              _session == session &&
              _foreground &&
              !_busy &&
              !_quietRefreshing &&
              _powerRequests.isEmpty &&
              _displayRequests.isEmpty) {
            unawaited(_refresh(quiet: true));
          }
        });
      }
    }
  }

  /// A sign-out that did not happen. Its own line, never the list's error:
  /// 다시 시도 there signs out again instead of reloading the list.
  String? _signOutError;

  /// [_busy] is the sign-out (closing the remote windows, then the server's
  /// answer), not a list read: the launcher says which.
  bool _signingOut = false;

  Future<void> _logout() async {
    _mfa.retire();
    if (_previewOpen) {
      setState(() {
        _previewOpen = false;
        _error = null;
      });
      return;
    }
    final session = _session;
    if (session == null || _busy) return;
    setState(() {
      _busy = true;
      _signingOut = true;
      _error = null;
      _signOutError = null;
    });
    var cleared = false;
    try {
      // Resolves only after the windows closed and their last pictures were
      // written, so the deletion below cannot race a final write.
      if (_detachedWindows) await SessionWindows.closeAll('logout');
      await const ConsoleBrowser.windows().closeAll(
        api: _api,
        token: session.token,
      );
      await _api!.logout(session.token);
      if (!mounted || _session != session) return;
      _sessionAccess.stop();
      final owner = _authenticationOwner;
      if (_detachedWindows && owner != null) {
        unawaited(
          SessionWindows.hideAuthentication(owner).catchError((Object _) {}),
        );
      }
      _stopDesktopUpdates();
      _invalidateListReads(resetSession: true);
      _stopKeeping();
      if (mounted) {
        setState(() {
          _session = null;
          _releaseThumbnails(delete: true);
          _powerRequests.clear();
          _powerStarted = const {};
          _displayRequests.clear();
          _desktops = [];
        });
        cleared = true;
      }
    } on BrokerException catch (error) {
      if (!mounted || _session != session) return;
      if (error.status == 401) {
        // Signed out already on the server; the user still asked to leave.
        final store = widget.thumbnails, key = _thumbnailKey;
        _showExpired(acknowledge: false);
        if (store != null && key != null) {
          unawaited(store.forget(key).catchError((Object _) {}));
        }
      } else if (mounted) {
        setState(() => _signOutError = l10n.couldntSignOutCheckNetwork);
      }
    } catch (_) {
      if (mounted && _session == session) {
        setState(() => _signOutError = l10n.couldntCloseRemoteWindowClose);
      }
    } finally {
      _signingOut = false;
      if (mounted && (_session == session || (cleared && _session == null))) {
        setState(() => _busy = false);
      } else if (mounted) {
        setState(() {});
      }
    }
  }

  bool get _canManageMfa => _api is MfaAccountBrokerApi && _origin?.scheme == 'https' &&
      (_api as MfaAccountBrokerApi).origin == _origin &&
      _session?.accountId != null && RegExp(r'^[a-f0-9]{8}(?:-[a-f0-9]{4}){3}-[a-f0-9]{12}$').hasMatch(_session!.accountId!) &&
      validAuthenticationOwner(_authenticationOwner);

  Future<void> _manageMfa(BuildContext dialogContext) async {
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
      await showDialog<void>(context: dialogContext, barrierDismissible: false,
        builder: (routeContext) => MfaAccountDialog(controller: controller, onClose: () => Navigator.of(routeContext).pop()));
    } finally { _mfa.finished(controller); }
  }

  Future<void> _changePassword(
    String current,
    String password,
    String confirmation,
  ) async {
    final session = _session;
    final origin = _origin;
    if (session == null || origin == null) {
      throw const BrokerException('session_expired', 401);
    }
    await _api!.changePassword(session.token, current, password, confirmation);
    if (!mounted || _session != session) return;
    // The sign-in ends here: its idle sign-out must not fire on the sign-in
    // screen afterwards.
    _sessionAccess.stop();
    _retireWindows();
    _stopDesktopUpdates();
    try {
      await widget.loginMemory.forgetPassword(origin);
    } catch (_) {
      /* The server revoked all existing sessions regardless of local storage. */
    }
    if (!mounted || _session != session) return;
    _invalidateListReads(resetSession: true);
    _stopKeeping();
    if (mounted) {
      setState(() {
        _session = null;
        _releaseThumbnails(delete: false);
        _powerRequests.clear();
        _powerStarted = const {};
        _displayRequests.clear();
        _desktops = [];
        _error = l10n.passwordWasChangedSignAgain;
      });
    }
  }

  void _checkConnection(BrokerConnection connection, BrokerSession session) {
    if (!mounted || _session != session) {
      unawaited(connection.release().catchError((Object _) {}));
      throw const BrokerException('session_expired', 401);
    }
  }

  /// Streams opened in this window report statistics like a remote window.
  RemoteSession _remoteSession(
    BrokerConnection connection,
    String token,
    String desktopId,
  ) {
    final api = _api;
    return RemoteSession(
      connection: connection,
      token: token,
      statsSink: api is HttpBrokerApi
          ? (values) => api.recordStreamStats(
              values,
              sessionId: connection.sessionId,
              desktopId: desktopId,
            )
          : null,
      stallSink: api is HttpBrokerApi
          ? (values) => api.recordStreamStall(
              values,
              sessionId: connection.sessionId,
              desktopId: desktopId,
            )
          : null,
    );
  }

  Future<RemoteSession> _connect(
    String desktopId, {
    StreamOptions options = const StreamOptions(),
  }) async {
    final session = _session;
    if (session == null) throw const BrokerException('session_expired', 401);
    if (_displayRequests.contains(desktopId) ||
        _desktops.any((m) => m.id == desktopId && m.display.busy)) {
      throw const BrokerException('display_busy');
    }
    final connection = await _api!.connect(
      session.token,
      desktopId,
      options: options,
    );
    _checkConnection(connection, session);
    return _remoteSession(connection, session.token, desktopId);
  }

  Future<void> _confirmStreamTakeover(
    String desktopId,
    StreamTakeoverOffer offer,
    bool Function() authorized,
  ) async {
    final session = _session, api = _api, origin = _origin;
    final revision = _authenticationRevision;
    bool current() =>
        mounted &&
        session != null &&
        _session == session &&
        _api == api &&
        _origin == origin &&
        _authenticationRevision == revision &&
        authorized() &&
        _desktops.any((machine) => machine.id == desktopId);
    if (!current()) throw const BrokerException('operation_cancelled');
    if (api is! ConfirmedTakeoverBrokerApi || offer.desktopId != desktopId) {
      throw const BrokerException('invalid_takeover_context');
    }
    final released = await (api as ConfirmedTakeoverBrokerApi).confirmTakeover(
      session!.token,
      desktopId,
      offer,
    );
    if (!current()) throw const BrokerException('operation_cancelled');
    if (!released) throw const BrokerException('invalid_response');
  }

  Future<DesktopMachine> _readStreamDesktop(String desktopId) async {
    final session = _session, api = _api, origin = _origin;
    final revision = _authenticationRevision;
    bool current() =>
        mounted &&
        session != null &&
        _session == session &&
        _api == api &&
        _origin == origin &&
        _authenticationRevision == revision;
    if (!current()) throw const BrokerException('operation_cancelled');
    final values = await api!.desktops(session!.token);
    if (!current()) throw const BrokerException('operation_cancelled');
    final matches = values.where((value) => value.id == desktopId);
    if (matches.length != 1) {
      throw const BrokerException('desktop_not_found', 404);
    }
    final fresh = machineFromBroker(matches.single);
    if (fresh.kind == MachineKind.pc && !fresh.localPcApproved) {
      throw const BrokerException('desktop_not_found', 404);
    }
    return fresh;
  }

  Future<void> _checkStreamTakeover(String desktopId) async {
    final session = _session, api = _api, origin = _origin;
    final revision = _authenticationRevision;
    bool current() =>
        mounted &&
        session != null &&
        _session == session &&
        _api == api &&
        _origin == origin &&
        _authenticationRevision == revision &&
        _desktops.any((machine) => machine.id == desktopId);
    if (!current()) throw const BrokerException('operation_cancelled');
    if (api is! StreamTakeoverPreflightBrokerApi) return;
    await (api as StreamTakeoverPreflightBrokerApi).checkStreamTakeover(
      session!.token,
      desktopId,
    );
    if (!current()) throw const BrokerException('operation_cancelled');
  }

  Future<RemoteSession> _connectTracks(
    String desktopId,
    TrackPlan plan,
    StreamOptions options, {
    int? displayScale,
  }) async {
    final session = _session, api = _api;
    if (session == null) throw const BrokerException('session_expired', 401);
    if (api is! TrackBrokerApi) {
      throw const BrokerException('track_streaming_unavailable');
    }
    if (_displayRequests.contains(desktopId) ||
        _desktops.any((m) => m.id == desktopId && m.display.busy)) {
      throw const BrokerException('display_busy');
    }
    final BrokerConnection connection;
    if (_desktops.any((m) => m.id == desktopId && m.requiresProfileTracks)) {
      if (api is! ProfileTrackBrokerApi) {
        throw const BrokerException('track_streaming_unavailable');
      }
      connection = await (api as ProfileTrackBrokerApi).connectProfileTracks(
        session.token,
        desktopId,
        plan,
        options,
        displayScale: displayScale,
      );
    } else {
      connection = await (api as TrackBrokerApi).connectTracks(
        session.token,
        desktopId,
        plan,
        options,
        displayScale: displayScale,
      );
    }
    _checkConnection(connection, session);
    return _remoteSession(connection, session.token, desktopId);
  }

  Future<RemoteSession> _connectScaled(
    String desktopId,
    InitialViewport? viewport,
    StreamOptions options,
    int scale,
  ) async {
    final session = _session, api = _api;
    if (session == null) throw const BrokerException('session_expired', 401);
    if (api is! DisplayScaleBrokerApi) {
      throw const BrokerException('display_not_supported');
    }
    if (_displayRequests.contains(desktopId) ||
        _desktops.any((m) => m.id == desktopId && m.display.busy)) {
      throw const BrokerException('display_busy');
    }
    final connection = await (api as DisplayScaleBrokerApi).connectScaled(
      session.token,
      desktopId,
      options: options,
      initialViewport: viewport,
      displayScale: scale,
    );
    _checkConnection(connection, session);
    return _remoteSession(connection, session.token, desktopId);
  }

  Future<RemoteSession> _connectViewport(
    String desktopId,
    InitialViewport viewport,
    StreamOptions options,
  ) async {
    final session = _session, api = _api;
    if (session == null) throw const BrokerException('session_expired', 401);
    if (api is! InitialViewportBrokerApi) {
      throw const BrokerException('initial_viewport_unavailable');
    }
    if (_displayRequests.contains(desktopId) ||
        _desktops.any((m) => m.id == desktopId && m.display.busy)) {
      throw const BrokerException('display_busy');
    }
    final connection = await (api as InitialViewportBrokerApi).connectViewport(
      session.token,
      desktopId,
      viewport,
      options,
    );
    _checkConnection(connection, session);
    return _remoteSession(connection, session.token, desktopId);
  }

  void _applyPower(String desktopId, BrokerPowerResult result, int operation) {
    if (!mounted || _powerRequests[desktopId] != operation) return;
    final power = result.power, task = result.task;
    _powerCanStop[desktopId] = result.canForceStop;
    setState(
      () => _desktops = [
        for (final machine in _desktops)
          if (machine.id != desktopId)
            machine
          else
            machine.withPower(
              state: power.state,
              checkedAt: power.checkedAt,
              stale: power.stale,
              canForceStop: result.canForceStop,
              busy:
                  _powerRequests.containsKey(desktopId) ||
                  task?.isActive == true,
              error: power.error,
              action: task?.action,
              taskStatus: task?.status,
              exitStatus: task?.exitStatus,
            ),
      ],
    );
  }

  Future<void> _power(String desktopId, String action) async {
    final session = _session, api = _api;
    if (session == null || api == null) {
      throw const BrokerException('session_expired', 401);
    }
    if (!clientVmPowerActions.contains(action)) {
      throw const BrokerException('invalid_power_action');
    }
    final matches = _desktops.where((machine) => machine.id == desktopId);
    if (matches.isEmpty) throw const BrokerException('desktop_not_found', 404);
    final target = matches.first;
    final pc = target.kind == MachineKind.pc;
    // 정지 may replace this account's own unfinished 종료 when the broker
    // says so (canStop); its request supersedes the shutdown's polling here.
    final replacing = action == 'stop' && target.canStop;
    // A refusal before sending is recorded too: nothing reaches the broker.
    final refused = _displayRequests.contains(desktopId) || target.display.busy
        ? 'display_busy'
        // An approved PC (pc-power-v1): only what the broker offers now.
        : pc
        ? target.pcPowerRefusal(
            action,
            pending: _powerRequests.containsKey(desktopId),
          )
        : target.powerStale || target.powerState == 'unknown'
        ? 'power_status_unavailable'
        : !replacing &&
              (_powerRequests.containsKey(desktopId) || target.powerBusy)
        ? 'power_in_progress'
        : (action == 'start'
              ? target.powerState != 'stopped'
              : target.powerState != 'running')
        ? 'invalid_power_state'
        // A broker before client stop never reports the permission.
        : action == 'stop' && !replacing
        ? 'invalid_power_action'
        : null;
    if (refused != null) {
      if (api is PowerDiagnosticsBrokerApi) {
        (api as PowerDiagnosticsBrokerApi).recordPowerRefused(
          session.token,
          desktopId,
          action,
          refused,
        );
      }
      throw BrokerException(refused);
    }
    final operation = ++_powerSequence;
    _invalidateListReads();
    _powerRequests[desktopId] = operation;
    _powerCanStop[desktopId] = false;
    // This client's own clock at the request: the tile counts the wait.
    _powerStarted = {..._powerStarted, desktopId: DateTime.now()};
    setState(
      () => _desktops = [
        for (final machine in _desktops)
          if (machine.id != desktopId)
            machine
          else
            machine.withPower(
              state: machine.powerState,
              checkedAt: machine.powerCheckedAt,
              stale: machine.powerStale,
              busy: true,
              error: machine.powerError,
              action: action,
              taskStatus: 'requesting',
            ),
      ],
    );
    // A PC woken by Wake-on-LAN boots Windows first (the broker waits 4 min).
    final deadline = DateTime.now().add(
      Duration(minutes: pc && action == 'start' ? 5 : 2),
    );
    try {
      var result = await api.requestPower(session.token, desktopId, action);
      while (true) {
        if (!mounted || _session != session) {
          throw const BrokerException('session_expired', 401);
        }
        if (_powerRequests[desktopId] != operation) {
          throw const BrokerException('power_superseded');
        }
        _applyPower(desktopId, result, operation);
        final task = result.task;
        if (task == null) throw const BrokerException('invalid_response');
        if (task.status == 'stopped') {
          if (task.exitStatus != 'OK') {
            // A PC's fixed refusal or wake result keeps its own words.
            throw BrokerException(
              pc && pcPowerTaskCodes.contains(task.exitStatus)
                  ? task.exitStatus!
                  : 'power_task_failed',
            );
          }
          break;
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
        if (_powerRequests[desktopId] != operation) {
          throw const BrokerException('power_superseded');
        }
        if (!mounted || _session != session) {
          throw const BrokerException('session_expired', 401);
        }
        try {
          result = await api.power(session.token, desktopId, taskId: task.id);
        } on BrokerException catch (error) {
          if (error.status >= 500 && DateTime.now().isBefore(deadline)) {
            continue;
          }
          rethrow;
        }
      }
    } on BrokerException catch (error) {
      if ((error.status == 401 ||
              pcLoginTerminalCode(error.code, error.status) != null) &&
          mounted &&
          _session == session) {
        _expired(error);
      }
      rethrow;
    } finally {
      if (mounted &&
          _session == session &&
          _powerRequests[desktopId] == operation) {
        _powerRequests.remove(desktopId);
        _powerCanStop.remove(desktopId);
        _powerStarted = {..._powerStarted}..remove(desktopId);
        await _refresh(quiet: true, force: true);
      }
    }
  }

  Future<DesktopMachine> _prepareConsole(String desktopId) async {
    final session = _session, api = _api;
    if (session == null || api == null) {
      throw const BrokerException('session_expired', 401);
    }
    if (api is! ConsoleRecoveryApi) {
      throw const BrokerException('console_recovery_unavailable');
    }
    final matches = _desktops.where((machine) => machine.id == desktopId);
    if (matches.length != 1) {
      throw const BrokerException('desktop_not_found', 404);
    }
    if (_displayRequests.contains(desktopId) ||
        _powerRequests.containsKey(desktopId) ||
        !matches.single.canRecoverConsole) {
      throw const BrokerException('console_busy');
    }
    _invalidateListReads();
    _displayRequests.add(desktopId);
    setState(
      () => _desktops = [
        for (final machine in _desktops)
          if (machine.id == desktopId)
            machine.withDisplay(machine.display.pending())
          else
            machine,
      ],
    );
    try {
      final result = await ConsoleResume(
        api: api as ConsoleRecoveryApi,
        loadDesktops: () => api.desktops(session.token),
        authorized: () => mounted && _session == session && _api == api,
      ).run(session.token, desktopId);
      if (!mounted || _session != session) {
        throw const BrokerException('session_expired', 401);
      }
      _displayRequests.remove(desktopId);
      final updated = _toMachine(result);
      setState(
        () => _desktops = [
          for (final machine in _desktops)
            if (machine.id == desktopId) updated else machine,
        ],
      );
      return updated;
    } on BrokerException catch (error) {
      if ((error.status == 401 ||
              pcLoginTerminalCode(error.code, error.status) != null) &&
          mounted &&
          _session == session) {
        _expired(error);
      }
      rethrow;
    } finally {
      if (mounted && _session == session) {
        _displayRequests.remove(desktopId);
        await _refresh(quiet: true, force: true);
      }
    }
  }

  /// The account's assigned Hosts; every request is still broker-authorized.
  Future<void> Function(BuildContext)? _hostBatchCallback() {
    final session = _session, api = _api, origin = _origin;
    if (_previewOpen ||
        session == null ||
        api == null ||
        api is! HostUpdateBrokerApi ||
        origin == null) {
      return null;
    }
    return (dialogContext) async {
      if (_hostBatch != null) return;
      bool authorized() =>
          mounted &&
          _session == session &&
          identical(_api, api) &&
          _origin == origin &&
          !_previewOpen &&
          _session!.expiresAt.isAfter(widget.clock());
      if (!authorized() || !dialogContext.mounted) {
        throw const BrokerException('session_expired', 401);
      }
      final scope = _HostBatchScope(
        context: dialogContext,
        authorized: authorized,
        machines: _desktops,
      );
      _hostBatch = scope;
      void expired([BrokerException? error]) {
        scope.invalidate();
        if (authorized()) _expired(error);
      }

      final boundApi = _BoundHostUpdateApi(
        api: api as HostUpdateBrokerApi,
        token: session.token,
        scope: scope,
        expired: expired,
      );
      Future<List<DesktopMachine>> observe() async {
        scope.requireCurrent();
        final read = ++_assignedReadSequence;
        try {
          // _refresh intentionally absorbs errors for the launcher. This
          // observer instead returns only a successful authenticated response.
          final fresh = await api.desktops(session.token);
          scope.requireCurrent();
          scope.publish(fresh.map(_toMachine).toList(), read);
          return scope.machines.value;
        } on BrokerException catch (error) {
          if (error.status == 401 ||
              pcLoginTerminalCode(error.code, error.status) != null) {
            expired(error);
          }
          rethrow;
        }
      }

      Future<ProductRelease?> latest() async {
        scope.requireCurrent();
        ProductUpdates? catalog;
        try {
          final release = widget.hostUpdateReleaseLoader != null
              ? await widget.hostUpdateReleaseLoader!(origin.toString())
              : await (catalog = ProductUpdates(
                  origin.toString(),
                )).check('host', '0.0.0');
          scope.requireCurrent();
          if (release != null &&
              (release.product != 'host' ||
                  release.url.origin != origin.origin)) {
            throw const UpdateFailure('invalid_catalog');
          }
          return release;
        } finally {
          catalog?.close();
        }
      }

      try {
        await showBulkHostUpdateDialog(
          dialogContext,
          api: boundApi,
          token: session.token,
          server: origin.toString(),
          assignedMachines: scope.machines.value,
          liveMachines: scope.machines,
          refreshAssigned: observe,
          loadRelease: latest,
          administrator: session.isAdministrator,
        );
      } finally {
        if (identical(_hostBatch, scope)) _hostBatch = null;
        scope.dispose();
        if (authorized()) unawaited(_refresh(quiet: true, force: true));
      }
    };
  }

  // The broker repeats assignment and current-release checks.
  Future<void> _hostUpdate(
    String desktopId,
    String version, {
    required bool disconnectSessions,
  }) async {
    final session = _session, api = _api;
    if (session == null || api == null) {
      throw const BrokerException('session_expired', 401);
    }
    if (api is! HostUpdateBrokerApi) {
      throw const BrokerException('host_update_unavailable');
    }
    try {
      final state = await (api as HostUpdateBrokerApi).requestHostUpdate(
        session.token,
        desktopId,
        version,
        disconnectSessions: disconnectSessions,
      );
      if (!mounted || _session != session) return;
      setState(
        () => _desktops = [
          for (final machine in _desktops)
            if (machine.id == desktopId)
              machine.withHostUpdate(state)
            else
              machine,
        ],
      );
    } on BrokerException catch (error) {
      if ((error.status == 401 ||
              pcLoginTerminalCode(error.code, error.status) != null) &&
          mounted &&
          _session == session) {
        _expired(error);
      }
      rethrow;
    } finally {
      if (mounted && _session == session) {
        unawaited(_refresh(quiet: true, force: true));
      }
    }
  }

  /// This account's favourite mark (desktop organising). The list shows the
  /// broker's answer; a read that started before it is dropped so it cannot
  /// bring the old mark back.
  Future<void> _setFavorite(String desktopId, bool favorite) async {
    final session = _session, api = _api;
    if (session == null || api == null) {
      throw const BrokerException('session_expired', 401);
    }
    if (api is! FavoriteBrokerApi) {
      throw const BrokerException('favorite_unavailable');
    }
    _invalidateListReads();
    try {
      final saved = await (api as FavoriteBrokerApi).setFavorite(
        session.token,
        desktopId,
        favorite,
      );
      if (!mounted || _session != session) return;
      setState(
        () => _desktops = [
          for (final machine in _desktops)
            if (machine.id == desktopId)
              machine.withOrganize(machine.organize.withFavorite(saved))
            else
              machine,
        ],
      );
    } on BrokerException catch (error) {
      if ((error.status == 401 ||
              pcLoginTerminalCode(error.code, error.status) != null) &&
          mounted &&
          _session == session) {
        _expired(error);
      }
      rethrow;
    }
  }

  Future<DesktopMachine> _configureDisplays(String desktopId, int count) async {
    final session = _session, api = _api;
    if (session == null || api == null) {
      throw const BrokerException('session_expired', 401);
    }
    if (count != 1 && count != 2) {
      throw const BrokerException('invalid_display_count');
    }
    final matches = _desktops.where((m) => m.id == desktopId);
    if (matches.isEmpty) throw const BrokerException('desktop_not_found', 404);
    final target = matches.first;
    if (_displayRequests.contains(desktopId) || target.display.busy) {
      throw const BrokerException('display_busy');
    }
    if (_powerRequests.containsKey(desktopId) || target.powerBusy) {
      throw const BrokerException('power_in_progress');
    }
    if (!target.canConfigureDisplays) {
      throw const BrokerException('display_not_supported');
    }
    _invalidateListReads();
    _displayRequests.add(desktopId);
    setState(
      () => _desktops = [
        for (final m in _desktops)
          if (m.id == desktopId) m.withDisplay(m.display.pending()) else m,
      ],
    );
    final deadline = DateTime.now().add(const Duration(seconds: 30));
    try {
      var state = await api
          .configureDisplays(session.token, desktopId, count)
          .timeout(
            const Duration(seconds: 30),
            onTimeout: () =>
                throw const BrokerException('display_operation_timeout'),
          );
      final id = state.operation?.id;
      if (id == null || state.operation?.requestedCount != count) {
        throw const BrokerException('invalid_response');
      }
      while (true) {
        if (!mounted || _session != session) {
          throw const BrokerException('session_expired', 401);
        }
        setState(
          () => _desktops = [
            for (final m in _desktops)
              if (m.id == desktopId) m.withDisplay(state) else m,
          ],
        );
        final op = state.operation;
        if (op?.id != id || op?.requestedCount != count) {
          throw const BrokerException('invalid_response');
        }
        if (op?.status == 'failed') {
          throw BrokerException('display_operation_failed', 0, op?.error);
        }
        if (op?.status == 'unknown') {
          throw const BrokerException('display_operation_unknown');
        }
        if (state.applied(count, id)) {
          // Obtain the newly advertised monitor IDs instead of deriving topology from a command ACK.
          final fresh = await api
              .desktops(session.token)
              .timeout(
                deadline.difference(DateTime.now()).isNegative
                    ? Duration.zero
                    : deadline.difference(DateTime.now()),
                onTimeout: () =>
                    throw const BrokerException('display_operation_timeout'),
              );
          if (!mounted || _session != session) {
            throw const BrokerException('session_expired', 401);
          }
          final match = fresh.where((m) => m.id == desktopId);
          if (match.isEmpty) {
            throw const BrokerException('desktop_not_found', 404);
          }
          final desktop = match.first;
          final ids = desktop.monitors.map((m) => m.id).toSet();
          if (desktop.virtualMonitorControl &&
              desktop.hostConnected == true &&
              desktop.display.applied(count, id) &&
              desktop.monitors.length == count &&
              ids.length == count &&
              ids.contains('0') &&
              (count == 1 || ids.contains('1'))) {
            _displayRequests.remove(desktopId);
            setState(() => _desktops = fresh.map(_toMachine).toList());
            return _desktops.firstWhere((m) => m.id == desktopId);
          }
        }
        if (DateTime.now().isAfter(deadline)) {
          throw const BrokerException('display_operation_timeout');
        }
        await Future<void>.delayed(const Duration(seconds: 1));
        if (!mounted || _session != session) {
          throw const BrokerException('session_expired', 401);
        }
        try {
          state = await api
              .display(session.token, desktopId, operationId: id)
              .timeout(
                deadline.difference(DateTime.now()).isNegative
                    ? Duration.zero
                    : deadline.difference(DateTime.now()),
                onTimeout: () =>
                    throw const BrokerException('display_operation_timeout'),
              );
        } on BrokerException catch (error) {
          if (error.status >= 500 && DateTime.now().isBefore(deadline)) {
            continue;
          }
          rethrow;
        }
      }
    } on BrokerException catch (error) {
      if ((error.status == 401 ||
              pcLoginTerminalCode(error.code, error.status) != null) &&
          mounted &&
          _session == session) {
        _expired(error);
      }
      rethrow;
    } finally {
      if (mounted && _session == session) {
        _displayRequests.remove(desktopId);
        unawaited(_refresh(quiet: true, force: true));
      }
    }
  }

  @override
  Widget build(BuildContext context) {
    final batch = _hostBatch;
    if (batch != null && !batch.current) batch.invalidate();
    return _buildApp();
  }

  Widget _buildApp() => ThemedApp(
    appearance: _appearance,
    confirmExit: widget.confirmExit,
    tray: _tray,
    trayMemory: _launcherMemory,
    sessionAccess: _sessionAccess,
    mfaHost: _mfa,
    home: _previewOpen || _session != null
        ? WorkspaceShell(
            sessionAccess: _previewOpen ? null : _sessionAccess,
            onSignOut: _logout,
            signOutError: _previewOpen ? null : _signOutError,
            onRefresh: _refresh,
            onChangePassword: _previewOpen ? null : _changePassword,
            onManageMfa: !_previewOpen && _canManageMfa ? _manageMfa : null,
            onConnect: _previewOpen ? null : _connect,
            onReadDesktop: _previewOpen ? null : _readStreamDesktop,
            onCheckTakeover:
                _previewOpen || _api is! StreamTakeoverPreflightBrokerApi
                ? null
                : _checkStreamTakeover,
            onConfirmTakeover:
                _previewOpen || _api is! ConfirmedTakeoverBrokerApi
                ? null
                : _confirmStreamTakeover,
            onOpenWindow: _previewOpen || !_detachedWindows
                ? null
                : _openWindow,
            onOpenConsole: _previewOpen ? null : _openNegotiatedConsole,
            onEndConsoles: _previewOpen || _api is! ConsoleTakeoverBrokerApi
                ? null
                : _endConsoles,
            onConnectTracks: _previewOpen ? null : _connectTracks,
            onConnectViewport: _previewOpen ? null : _connectViewport,
            onConnectScaled: _previewOpen || _api is! DisplayScaleBrokerApi
                ? null
                : _connectScaled,
            onConnectTracksScaled: _previewOpen || _api is! TrackBrokerApi
                ? null
                : (desktopId, plan, options, scale) => _connectTracks(
                    desktopId,
                    plan,
                    options,
                    displayScale: scale,
                  ),
            onPrepareConsole: _previewOpen || _api is! ConsoleRecoveryApi
                ? null
                : _prepareConsole,
            onPower: _previewOpen ? null : _power,
            onConfigureDisplays: _previewOpen ? null : _configureDisplays,
            administrator: !_previewOpen && _session?.isAdministrator == true,
            onHostUpdate: _previewOpen || _api is! HostUpdateBrokerApi
                ? null
                : _hostUpdate,
            onHostBatchUpdate: _hostBatchCallback(),
            onFavorite: _previewOpen || _api is! FavoriteBrokerApi
                ? null
                : _setFavorite,
            localMonitors: _localMonitors,
            machines: _previewOpen ? previewDesktops : _desktops,
            isDemo: _previewOpen,
            displayName: _session?.username ?? l10n.demo,
            busy: _busy,
            signingOut: !_previewOpen && _signingOut,
            powerStarted: _previewOpen ? const {} : _powerStarted,
            error: _error,
            notice: _restartShown && !_previewOpen
                ? serverRestartingMessage
                : null,
            serverOrigin: _origin?.toString() ?? l10n.notSet,
            launcherMemory: _launcherMemory,
            sessionGuideEnabled: !_previewOpen && _session != null,
            guideAccountKey: _origin == null
                ? null
                : sessionGuideAccountKey(_origin!, _session?.accountId),
            thumbnails: _previewOpen ? null : _thumbnailSource,
            previews: _previewOpen ? null : _previewSource,
            catalogChanges: _previewOpen ? null : _catalogPushes.stream,
            notices: _previewOpen ? null : _notices,
            noticesActive: _foreground,
            liveChannel: _previewOpen ? null : _liveChannel,
            listGeneration: _listGeneration,
            checkClientUpdate:
                _previewOpen || _origin == null || widget.updateCheck == null
                ? null
                : () => widget.updateCheck!(_origin.toString()),
          )
        : Builder(
            builder: (loginContext) => SignInScreen(
              onLogin: (name, password) => _login(loginContext, name, password),
              serverOrigin: _origin,
              onServerChanged: _changeServer,
              loginMemory: widget.loginMemory,
              busy: _busy,
              error: _error,
              // The line was about the attempt before this edit.
              onEdited: () {
                if (_error != null && !_busy) setState(() => _error = null);
              },
              onPreview: widget.enableDemo
                  ? () => setState(() {
                      _previewOpen = true;
                      _error = null;
                    })
                  : null,
            ),
          ),
  );
}

/// An update panel is bound to the account/API/origin that opened it. Closing
/// a revoked panel never cancels a POST or an operation already accepted.
class _HostBatchScope {
  _HostBatchScope({
    required BuildContext context,
    required this.authorized,
    required List<DesktopMachine> machines,
  }) : machines = ValueNotifier(List.unmodifiable(machines)),
       navigator = Navigator.of(context),
       anchor = ModalRoute.of(context);
  final bool Function() authorized;
  final ValueNotifier<List<DesktopMachine>> machines;
  final NavigatorState navigator;
  final ModalRoute<dynamic>? anchor;
  bool invalidated = false, disposed = false;
  int _lastRead = 0;
  bool get current => !invalidated && !disposed && authorized();
  void requireCurrent() {
    // Closing a panel retires its callbacks, not the working broker login.
    if (disposed) throw const BrokerException('host_update_panel_closed', 409);
    if (current) return;
    invalidate();
    throw const BrokerException('session_expired', 401);
  }

  void publish(List<DesktopMachine> fresh, int read) {
    if (!current || read < _lastRead) return;
    _lastRead = read;
    machines.value = List.unmodifiable(fresh);
  }

  void invalidate() {
    if (invalidated || disposed) return;
    invalidated = true;
    WidgetsBinding.instance.addPostFrameCallback((_) {
      if (disposed) return;
      machines.value = const [];
      if (navigator.mounted && anchor?.isActive == true) {
        navigator.popUntil(
          (route) => identical(route, anchor) || route.isFirst,
        );
      }
    });
  }

  void dispose() {
    if (disposed) return;
    disposed = true;
    machines.dispose();
  }
}

class _BoundHostUpdateApi implements HostUpdateBrokerApi {
  _BoundHostUpdateApi({
    required this.api,
    required this.token,
    required this.scope,
    required this.expired,
  });
  final HostUpdateBrokerApi api;
  final String token;
  final _HostBatchScope scope;
  final void Function([BrokerException? error]) expired;
  @override
  Future<HostUpdateState> requestHostUpdate(
    String requestToken,
    String desktopId,
    String version, {
    bool disconnectSessions = false,
  }) async {
    scope.requireCurrent();
    if (requestToken != token) {
      throw const BrokerException('permission_denied', 403);
    }
    final assigned = scope.machines.value.where(
      (machine) => machine.id == desktopId,
    );
    if (assigned.isEmpty) throw const BrokerException('desktop_not_found', 404);
    if (assigned.first.hostUpdate.latestVersion != version) {
      throw const BrokerException('host_release_unavailable', 409);
    }
    try {
      // The broker ends only what this account may end (its own
      // connections, or all of them for an administrator).
      final result = await api.requestHostUpdate(
        token,
        desktopId,
        version,
        disconnectSessions: disconnectSessions,
      );
      scope.requireCurrent();
      return result;
    } on BrokerException catch (error) {
      if (error.status == 401 ||
          pcLoginTerminalCode(error.code, error.status) != null) {
        expired(error);
      }
      rethrow;
    }
  }
}

enum _Silent { signedIn, noPassword, rejected, unreachable }
