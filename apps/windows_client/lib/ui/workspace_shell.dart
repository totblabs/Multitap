import 'dart:async';
import 'dart:math' as math;

import 'package:flutter/foundation.dart' show ValueListenable;
import 'package:flutter/material.dart';
import 'package:flutter/services.dart';

import '../data/desktop_thumbnails.dart';
import '../data/desktop_previews.dart';
import '../models/desktop_organize.dart';
import 'desktop_organizer.dart';
import '../data/launcher_memory.dart';
import 'launcher_view_menu.dart';
import 'launcher_scenes.dart';
import '../models/desktop_machine.dart';
import 'app_theme.dart';
import 'components.dart';
import 'session_preview.dart';
import 'app_info_dialog.dart';
import 'change_password_dialog.dart';
import 'remote_view.dart';
import '../data/remote_session.dart';
import '../data/session_activity.dart';
import '../data/broker_api.dart' show BrokerException, clientVmPowerActions;
import '../data/stream_takeover.dart';
import '../models/pc_power.dart' show pcWakeFailure;
import 'pc_power_help.dart';
import '../models/stream_options.dart';
import '../models/session_display_mode.dart';
import 'stream_settings_dialog.dart';
import '../data/session_windows.dart';
import 'product_update_dialog.dart';
import 'product_log_dialog.dart';
import '../data/client_build_version.dart';
import 'console_resume_dialog.dart';
import '../data/track_stream.dart';
import '../data/initial_viewport.dart';
import 'desktop_menu.dart';
import 'desktop_readiness.dart';
import 'desktop_tile.dart';
import 'readiness_dialog.dart';
import 'session_cleanup_dialog.dart';
import 'app_dialog.dart';
import 'app_menu.dart';
import 'release_notes.dart' show showReleaseHistory;
import 'host_update_dialog.dart';
import 'korean_text.dart';
import 'workspace_header.dart';
import 'notice_widgets.dart';
import '../data/notices.dart' show NoticeCenter;
import '../data/product_updates.dart' show ProductRelease;
import '../l10n/l10n.dart';

/// An automatic "power on, then connect" for one desktop.
class _StartFlow {
  _StartFlow(this.mode);
  final SessionDisplayMode mode;

  /// Whole seconds since the request, counted by the launcher's ticker.
  int seconds = 0;
  bool powered = false, connecting = false;

  /// Waits only for the previous connection's cleanup (no power request).
  bool settle = false;
}

/// Assigned desktops in the user's saved launcher view. Connections
/// start only from their explicit controls or the focused desktop's Enter.
class WorkspaceShell extends StatefulWidget {
  const WorkspaceShell({
    super.key,
    required this.onSignOut,
    required this.onRefresh,
    required this.machines,
    this.isDemo = false,
    required this.displayName,
    this.guideAccountKey,
    this.sessionGuideEnabled = true,
    this.busy = false,
    this.error,
    this.signOutError,
    this.notice,
    required this.serverOrigin,
    this.onChangePassword,
    this.onManageMfa,
    this.onConnect,
    this.onConfirmTakeover,
    this.onReadDesktop,
    this.onCheckTakeover,
    this.localMonitors = 0,
    this.onPower,
    this.onConfigureDisplays,
    this.onPrepareConsole,
    this.onOpenWindow,
    this.onOpenConsole,
    this.onEndConsoles,
    this.onConnectTracks,
    this.onConnectViewport,
    this.onConnectScaled,
    this.onConnectTracksScaled,
    this.launcherMemory,
    this.thumbnails,
    this.previews,
    this.catalogChanges,
    this.liveChannel,
    this.listGeneration = 0,
    this.administrator = false,
    this.onHostUpdate,
    this.onHostBatchUpdate,
    this.loadHostRelease,
    this.checkClientUpdate,
    this.onFavorite,
    this.sessionAccess,
    this.notices,
    this.noticesActive = true,
  });

  /// 공지 (notice_widgets.dart): this sign-in's notices. The header gets the
  /// bell and a notice that arrives rises as a banner over the list's
  /// bottom-right corner;
  /// the list itself is not changed. Null: none (the demo, an older server).
  final NoticeCenter? notices;

  /// False while this window cannot be seen (it rests in the tray): the
  /// banner waits for it.
  final bool noticesActive;

  /// Marks or unmarks a desktop as this account's favourite (the broker call
  /// and the list update). Null: no favourites (design demo).
  final Future<void> Function(String desktopId, bool favorite)? onFavorite;
  final SessionActivity? sessionAccess;

  /// The newer client version on this server, or null (header badge). Polled
  /// while signed in; it never downloads. A read that failed throws: the
  /// badge keeps what it showed.
  final Future<String?> Function()? checkClientUpdate;

  /// The offered Host release with its changelog; defaults to this server's
  /// public download catalog.
  final Future<ProductRelease?> Function(String currentVersion)?
  loadHostRelease;
  final VoidCallback onSignOut;
  final Future<void> Function(BuildContext)? onHostBatchUpdate;

  /// The signed-in account is an administrator. Only shows administrator
  /// actions; the broker enforces the role on every request.
  final bool administrator;

  /// Asks the broker to update a desktop's Host (administrator accounts).
  final Future<void> Function(
    String desktopId,
    String version, {
    required bool disconnectSessions,
  })?
  onHostUpdate;
  final VoidCallback onRefresh;
  final List<DesktopMachine> machines;
  final bool isDemo, busy;
  final String displayName;
  final String? error;

  /// A sign-out that failed: its own line above the list whose 다시 시도
  /// signs out again (the list itself is fine).
  final String? signOutError;

  /// One neutral line above the list (the server restarting), or null.
  final String? notice;
  final String serverOrigin;
  final Future<void> Function(String, String, String)? onChangePassword;
  final Future<void> Function(BuildContext)? onManageMfa;
  final Future<RemoteSession> Function(String, {StreamOptions options})?
  onConnect;
  final Future<void> Function(
    String desktopId,
    StreamTakeoverOffer offer,
    bool Function() authorized,
  )?
  onConfirmTakeover;
  final Future<DesktopMachine> Function(String desktopId)? onReadDesktop;
  final Future<void> Function(String desktopId)? onCheckTakeover;
  final Future<RemoteSession> Function(
    String desktopId,
    TrackPlan plan,
    StreamOptions options,
  )?
  onConnectTracks;
  final Future<RemoteSession> Function(
    String desktopId,
    InitialViewport viewport,
    StreamOptions options,
  )?
  onConnectViewport;
  final Future<RemoteSession> Function(
    String desktopId,
    InitialViewport? viewport,
    StreamOptions options,
    int scale,
  )?
  onConnectScaled;
  final Future<RemoteSession> Function(
    String desktopId,
    TrackPlan plan,
    StreamOptions options,
    int scale,
  )?
  onConnectTracksScaled;

  /// This PC's monitor count (모든 모니터 needs exactly two).
  final int localMonitors;
  final Future<void> Function(String desktopId, String action)? onPower;
  final Future<DesktopMachine> Function(String desktopId, int count)?
  onConfigureDisplays;
  final Future<DesktopMachine> Function(String desktopId)? onPrepareConsole;
  final Future<SessionWindowOpenResult?> Function(
    DesktopMachine machine,
    SessionDisplayMode mode,
    StreamOptions options,
  )?
  onOpenWindow;
  final Future<SessionWindowOpenResult?> Function(DesktopMachine machine)?
  onOpenConsole;

  /// 'Console 끊고 연결': ends the VM's console connections.
  final Future<void> Function(String desktopId)? onEndConsoles;

  /// Remembers the last desktop and each desktop's streaming mode.
  final LauncherMemory? launcherMemory;
  final String? guideAccountKey;
  final bool sessionGuideEnabled;

  /// The signed-in account's last-frame pictures for the tiles.
  final ThumbnailSource? thumbnails;

  /// The broker's recent pictures of the assigned VMs (desktop-preview-v1),
  /// asked only while this launcher is visible. Null: none (older broker,
  /// design demo).
  final DesktopPreviewSource? previews;

  /// The broker's live channel says the public download catalog changed:
  /// the update badge is recomputed at once instead of on its timer.
  final Stream<void>? catalogChanges;

  /// Whether that live channel is connected. The update timer is only the
  /// fallback while it is not; connecting checks once more.
  final ValueListenable<bool>? liveChannel;

  /// Grows with each real list read (not with pushed CPU/RAM samples), so
  /// the tile pictures are reloaded only for those. 0: every new list.
  final int listGeneration;

  /// The page column; the header's brand and controls follow its edges.
  static const columnWidth = 1240.0;

  @override
  State<WorkspaceShell> createState() => _WorkspaceShellState();
}

class _WorkspaceShellState extends State<WorkspaceShell> {
  late final LauncherMemory _memory =
      widget.launcherMemory ?? SessionLauncherMemory();
  LauncherPreferences _prefs = const LauncherPreferences();
  bool _prefsLoaded = false, _preselected = false;
  bool _viewEdited = false;
  Future<void> _preferenceWrites = Future.value();
  final _viewMenu = MenuController();
  final _desktopScroll = ScrollController();
  final _focus = <String, FocusNode>{};
  final _flows = <String, _StartFlow>{};
  Timer? _ticker;
  final _thumbnails = <String, DesktopThumbnail>{};
  StreamSubscription<String>? _thumbnailUpdates;
  Timer? _thumbnailClock;
  int _thumbnailGeneration = 0;
  final _thumbnailReads = <String, int>{};
  late final _previews = DesktopPreviewBoard(
    changed: () {
      if (mounted) setState(() {});
    },
  );
  AppLifecycleListener? _previewLifecycle;
  String? _clientUpdate;
  bool _updatePanelOpen = false;
  bool _clientInfoOpen = false;
  Timer? _updateClock;
  StreamSubscription<void>? _catalogPush;

  /// Runs for [_updateOnReturn] after each catalog read.
  Timer? _updateRecent;
  AppLifecycleListener? _lifecycle;

  /// When the broker's desktop list was last read successfully, and a tick for open
  /// dialogs that follow it (the readiness check).
  DateTime _listReceived = DateTime.now();
  final _listChanges = ValueNotifier<int>(0);

  static const _startTimeout = Duration(minutes: 5);
  // A release published while the client is open shows within minutes, and
  // at once on coming back to the window or 새로고침 (30 minutes is too
  // slow). Each check reads only the server's small catalog.
  static const _updateInterval = Duration(minutes: 10);
  static const _updateOnReturn = Duration(minutes: 1);

  // --- search, 분류 and sort ---------------------------------------------------

  final _search = TextEditingController();
  final _searchFocus = FocusNode(debugLabel: 'desktop-search');
  String _query = '';
  bool _searchOpen = false;
  DesktopChip _chip = DesktopChip.all;
  DesktopStatus _status = DesktopStatus.all;
  DesktopKind _kind = DesktopKind.all;
  DesktopSort _sort = DesktopSort.number;

  /// A favourite change in flight (shown at once) and the ones being sent.
  final _favoriteOverride = <String, bool>{};
  final _favoriteSending = <String>{};

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
    if (_desktopScroll.hasClients) _desktopScroll.jumpTo(0);
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

  /// Clears the text and leaves the box (the icon collapses again).
  void _closeSearch() {
    _search.clear();
    _searchFocus.unfocus();
    setState(() => _searchOpen = false);
  }

  /// The launcher shortcuts that work without the search box (see
  /// [handleLauncherSearchKey]).
  bool _launcherKey(KeyEvent event) {
    if (!mounted) return false;
    return handleLauncherSearchKey(
      context,
      event,
      open: _openSearch,
      close: _closeSearch,
      searching: _query.isNotEmpty || _searchOpen,
      hasDesktops: widget.machines.isNotEmpty,
    );
  }

  /// The chip that applies now: a remembered or selected one that no longer
  /// has members counts as 전체.
  DesktopChip _effectiveChip(List<DesktopMachine> base) =>
      effectiveChip(_chip, base);

  /// Same for the 상태 filter (its segment is disabled without members).
  DesktopStatus _effectiveStatus(List<DesktopMachine> base) =>
      effectiveStatus(_status, base);

  /// And the 구분 filter (shown only while both kinds are assigned).
  DesktopKind _effectiveKind(List<DesktopMachine> base) =>
      effectiveKind(_kind, base);

  /// Persists the fall back to 전체 once the real list is known.
  void _normalizeChip() {
    if (!_prefsLoaded || widget.machines.isEmpty) return;
    final base = _baseMachines();
    var changed = false;
    if (_chip != _effectiveChip(base)) {
      _chip = DesktopChip.all;
      changed = true;
    }
    if (_status != _effectiveStatus(base)) {
      _status = DesktopStatus.all;
      changed = true;
    }
    if (_kind != _effectiveKind(base)) {
      _kind = DesktopKind.all;
      changed = true;
    }
    if (changed) _saveOrganize();
  }

  List<DesktopMachine> _baseMachines() => _favoriteOverride.isEmpty
      ? widget.machines
      : [
          for (final m in widget.machines)
            _favoriteOverride.containsKey(m.id)
                ? m.withOrganize(
                    m.organize.withFavorite(_favoriteOverride[m.id]!),
                  )
                : m,
        ];

  void _saveOrganize() {
    if (widget.isDemo) return;
    final next = _prefs.withOrganize(
      widget.displayName,
      OrganizePreference(
        chip: _chip.id,
        status: _status.id,
        sort: _sort.name,
        kind: _kind.id,
      ),
    );
    _prefs = next;
    _savePreferences(next);
  }

  void _savePreferences(LauncherPreferences value) {
    if (widget.isDemo) return;
    final server = widget.serverOrigin;
    _preferenceWrites = _preferenceWrites
        .then((_) => _memory.save(server, value))
        .catchError((Object _) {});
  }

  void _selectView(LauncherView view, {bool? showPreviews}) {
    _viewEdited = true;
    setState(() => _prefs = _prefs.withView(view, showPreviews: showPreviews));
    if (_prefsLoaded) _savePreferences(_prefs);
    if (_desktopScroll.hasClients) _desktopScroll.jumpTo(0);
    _bindThumbnails();
  }

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
    final send = widget.onFavorite;
    if (send == null || _favoriteSending.contains(machine.id)) return;
    final next = !machine.organize.favorite;
    final messenger = ScaffoldMessenger.of(context);
    setState(() {
      _favoriteOverride[machine.id] = next;
      _favoriteSending.add(machine.id);
    });
    try {
      await send(machine.id, next);
    } catch (error) {
      if (messenger.mounted) {
        messenger.showSnackBar(
          SnackBar(
            content: KeepAllText(
              error is BrokerException && error.status == 404
                  ? l10n.desktopNoLongerAssignedYou
                  : l10n.couldntChangeFavoritesTryAgain,
            ),
          ),
        );
      }
    } finally {
      // Done either way: the answer (or the rollback) is what the list shows.
      if (mounted) {
        setState(() {
          _favoriteOverride.remove(machine.id);
          _favoriteSending.remove(machine.id);
        });
      }
    }
  }

  void _searchSubmitted(String _) {
    final shown = _shownMachines();
    if (shown.length == 1) {
      _node(shown.first.id).requestFocus();
    }
  }

  List<DesktopMachine> _shownMachines() {
    final base = _baseMachines();
    return organizeDesktops(
      base,
      query: _query,
      chip: _effectiveChip(base),
      status: _effectiveStatus(base),
      kind: _effectiveKind(base),
      sort: _sort,
    );
  }

  @override
  void initState() {
    super.initState();
    _search.addListener(_searchTextChanged);
    _searchFocus.addListener(_searchFocusChanged);
    HardwareKeyboard.instance.addHandler(_launcherKey);
    unawaited(_loadPrefs());
    _bindThumbnails();
    // Back on screen (restored, shown, focused): fresh pictures at once.
    _previewLifecycle = AppLifecycleListener(
      onResume: () => unawaited(_previews.poll()),
      onShow: () => unawaited(_previews.poll()),
    );
    if (!widget.isDemo && widget.checkClientUpdate != null) {
      unawaited(_checkClientUpdate());
      _updateClock = Timer.periodic(_updateInterval, (_) {
        if (widget.liveChannel?.value != true) unawaited(_checkClientUpdate());
      });
      _bindCatalog();
      _lifecycle = AppLifecycleListener(
        onResume: () {
          if (_updateRecent?.isActive != true) {
            unawaited(_checkClientUpdate());
          }
        },
      );
    }
  }

  void _bindCatalog() {
    unawaited(_catalogPush?.cancel());
    _catalogPush = widget.catalogChanges?.listen(
      (_) => unawaited(_checkClientUpdate()),
      onError: (Object _, StackTrace _) {},
    );
    widget.liveChannel?.removeListener(_liveChanged);
    widget.liveChannel?.addListener(_liveChanged);
  }

  /// The channel came (back): anything published meanwhile is read now.
  void _liveChanged() {
    if (widget.liveChannel?.value == true) unawaited(_checkClientUpdate());
  }

  void _refreshList() {
    widget.onRefresh();
    unawaited(_checkClientUpdate());
  }

  Future<void> _checkClientUpdate() async {
    final check = widget.checkClientUpdate;
    if (check == null) return;
    _updateRecent?.cancel();
    _updateRecent = Timer(_updateOnReturn, () {});
    final String? version;
    try {
      version = await check();
    } catch (_) {
      // The catalog could not be read: nothing new is known, so the button
      // keeps its last state (a "새 버전" already found stays).
      return;
    }
    if (mounted && version != _clientUpdate) {
      setState(() => _clientUpdate = version);
    }
  }

  // --- last-frame pictures --------------------------------------------------

  /// Screen content leaves memory with its account, not only the disk.
  void _evictThumbnails() {
    for (final thumbnail in _thumbnails.values) {
      unawaited(thumbnail.image.evict().catchError((Object _) => false));
    }
    _thumbnails.clear();
  }

  void _removeThumbnail(String id) {
    final previous = _thumbnails.remove(id);
    if (previous != null) {
      unawaited(previous.image.evict().catchError((Object _) => false));
    }
  }

  bool _thumbnailAllowed(String id) {
    return switch (widget.thumbnails) {
      ThumbnailPolicySource policy => policy.isAllowed(id),
      _ => true,
    };
  }

  // --- broker pictures (desktop-preview-v1) ---------------------------------

  bool get _previewsShown =>
      widget.previews != null &&
      !widget.isDemo &&
      _prefsLoaded &&
      _prefs.view.usesPreviews &&
      _prefs.showPreviews;

  /// Minimised or hidden: nothing is asked (the Host captures nothing).
  static bool get _onScreen => switch (WidgetsBinding.instance.lifecycleState) {
    AppLifecycleState.hidden ||
    AppLifecycleState.paused ||
    AppLifecycleState.detached => false,
    _ => true,
  };

  void _bindPreviews() {
    final source = widget.previews;
    if (source == null || !_previewsShown) {
      _previews.stop();
      return;
    }
    _previews.start(
      source,
      machines: () => widget.machines,
      visible: () => _previewsShown && _onScreen,
    );
  }

  /// The newer of this PC's last streamed frame and the Host's picture.
  DesktopThumbnail? _pictureFor(DesktopMachine machine) =>
      _previews.pictureFor(machine, local: _thumbnails[machine.id]);

  String? _previewNoteFor(DesktopMachine machine) =>
      _previews.noteFor(machine, local: _thumbnails[machine.id]);

  void _bindThumbnails() {
    _bindPreviews();
    _thumbnailGeneration++;
    _thumbnailReads.clear();
    unawaited(_thumbnailUpdates?.cancel());
    _thumbnailUpdates = null;
    _thumbnailClock?.cancel();
    _thumbnailClock = null;
    _evictThumbnails();
    final source = widget.thumbnails;
    if (source == null ||
        !_prefsLoaded ||
        !_prefs.view.usesPreviews ||
        !_prefs.showPreviews) {
      return;
    }
    // A session window just wrote a new picture (it closed, or its periodic
    // refresh ran): reload that one tile from disk.
    _thumbnailUpdates = source.updates.listen((id) {
      if (widget.machines.any((machine) => machine.id == id)) {
        // A policy withdrawal must remove the retained provider synchronously,
        // before an asynchronous file read can complete or fail.
        if (!_thumbnailAllowed(id) && _thumbnails.containsKey(id)) {
          setState(() => _removeThumbnail(id));
        }
        unawaited(_loadThumbnail(id, replaced: true));
      }
    }, onError: (Object _, StackTrace _) {});
    // Keeps "N분 전" current between list refreshes.
    _thumbnailClock = Timer.periodic(const Duration(minutes: 1), (_) {
      if (mounted && _thumbnails.isNotEmpty) setState(() {});
    });
    _loadThumbnails();
  }

  void _loadThumbnails() {
    if (!_prefs.view.usesPreviews || !_prefs.showPreviews) return;
    for (final machine in widget.machines) {
      unawaited(_loadThumbnail(machine.id));
    }
  }

  Future<void> _loadThumbnail(String id, {bool replaced = false}) async {
    final source = widget.thumbnails;
    if (source == null) return;
    final generation = _thumbnailGeneration;
    final read = _thumbnailReads[id] = (_thumbnailReads[id] ?? 0) + 1;
    if (!_thumbnailAllowed(id)) {
      if (_thumbnails.containsKey(id)) setState(() => _removeThumbnail(id));
      return;
    }
    DesktopThumbnail? next;
    try {
      next = await source.read(id, refresh: replaced);
    } catch (_) {
      next = null;
    }
    if (!mounted ||
        generation != _thumbnailGeneration ||
        read != _thumbnailReads[id] ||
        !_thumbnailAllowed(id)) {
      if (next != null) {
        unawaited(next.image.evict().catchError((Object _) => false));
      }
      return;
    }
    final previous = _thumbnails[id];
    if (next == previous) return;
    // The replaced picture's decoded pixels are no longer shown anywhere.
    if (previous != null && previous.image != next?.image) {
      unawaited(previous.image.evict().catchError((Object _) => false));
    }
    final incoming = next;
    setState(() {
      if (incoming == null) {
        _thumbnails.remove(id);
      } else {
        final budget = thumbnailCacheByteBudget;
        _thumbnails.remove(id);
        var retained = _thumbnails.values.fold<int>(
          0,
          (sum, item) => sum + item.encodedBytes,
        );
        while (_thumbnails.isNotEmpty &&
            retained + incoming.encodedBytes > budget) {
          final oldest = _thumbnails.keys.first;
          retained -= _thumbnails[oldest]!.encodedBytes;
          _removeThumbnail(oldest);
        }
        if (budget > 0 && incoming.encodedBytes <= budget) {
          _thumbnails[id] = incoming;
        } else {
          unawaited(incoming.image.evict().catchError((Object _) => false));
        }
      }
    });
  }

  Future<void> _loadPrefs() async {
    if (widget.isDemo) {
      _prefsLoaded = true;
      _bindThumbnails();
      return;
    }
    try {
      final prefs = await _memory.load(widget.serverOrigin);
      if (!mounted) return;
      final organize = prefs.organizeFor(widget.displayName).migrated();
      setState(() {
        _prefs = _viewEdited
            ? prefs.withView(_prefs.view, showPreviews: _prefs.showPreviews)
            : prefs;
        _chip = DesktopChip.parse(organize.chip);
        _status = DesktopStatus.byId(organize.status);
        _kind = DesktopKind.byId(organize.kind);
        _sort = DesktopSort.byName(organize.sort);
      });
    } catch (_) {
      // Preferences are a convenience; the defaults remain usable.
    }
    _prefsLoaded = true;
    if (_viewEdited) _savePreferences(_prefs);
    if (mounted) {
      _bindThumbnails();
      _normalizeChip();
      _preselect();
    }
  }

  void _remember(DesktopMachine machine) {
    if (widget.isDemo) return;
    final next = LauncherPreferences(
      lastDesktop: machine.id,
      organize: _prefs.organize,
      view: _prefs.view,
      showPreviews: _prefs.showPreviews,
    );
    setState(() => _prefs = next);
    _savePreferences(next);
  }

  // The launcher always starts windowed. Explicit menu choices belong only to
  // that connection attempt; an active remote window owns its later changes.
  SessionDisplayMode _modeFor(DesktopMachine machine) =>
      _prefs.modeFor(machine.id);

  bool _allMonitors(DesktopMachine machine) =>
      machine.independentDisplayStreaming &&
      widget.localMonitors == 2 &&
      (widget.onOpenWindow != null || widget.onConnectTracks != null);

  FocusNode _node(String id) =>
      _focus.putIfAbsent(id, () => FocusNode(debugLabel: 'desktop-$id'));

  Future<void> _revealDesktop(String id) async {
    final previousFocus = FocusManager.instance.primaryFocus;
    for (var pass = 0; pass < 3; pass++) {
      if (!mounted) return;
      if (ModalRoute.of(context)?.isCurrent == false) return;
      final currentFocus = FocusManager.instance.primaryFocus;
      if (currentFocus != previousFocus && currentFocus is! FocusScopeNode) {
        return;
      }
      final shown = _shownMachines();
      final index = shown.indexWhere((machine) => machine.id == id);
      if (index < 0) return;
      final node = _node(id), target = node.context;
      if (target != null && target.mounted) {
        final tileBox = target.findRenderObject();
        final viewportBox = Scrollable.of(target).context.findRenderObject();
        if (tileBox is RenderBox && viewportBox is RenderBox) {
          final top = tileBox.localToGlobal(Offset.zero).dy;
          final start = viewportBox.localToGlobal(Offset.zero).dy;
          if (top < start || top >= start + viewportBox.size.height) {
            await Scrollable.ensureVisible(target, alignment: .15);
          }
        }
        if (mounted && identical(_focus[id], node) && node.context != null) {
          node.requestFocus();
        }
        return;
      }
      if (!_desktopScroll.hasClients) return;
      // Lazy children have no Focus context until built. Reveal the estimated
      // position, then use the real child bounds; no connection is triggered.
      final fraction = shown.length <= 1 ? 0.0 : index / (shown.length - 1);
      _desktopScroll.jumpTo(_desktopScroll.position.maxScrollExtent * fraction);
      await WidgetsBinding.instance.endOfFrame;
    }
  }

  // The last desktop (or the only one) takes keyboard focus, so Enter
  // connects at once.
  void _preselect() {
    if (_preselected || !_prefsLoaded || widget.machines.isEmpty) return;
    _preselected = true;
    final shown = _shownMachines();
    if (shown.isEmpty) return;
    final last = shown.where((m) => m.id == _prefs.lastDesktop);
    final target = last.isNotEmpty ? last.first : shown.first;
    WidgetsBinding.instance.addPostFrameCallback((_) {
      if (!mounted || ModalRoute.of(context)?.isCurrent == false) return;
      // Never take focus from a control the user is already on.
      final primary = FocusManager.instance.primaryFocus;
      if (primary != null && primary is! FocusScopeNode) return;
      unawaited(_revealDesktop(target.id));
    });
  }

  @override
  void didUpdateWidget(WorkspaceShell oldWidget) {
    super.didUpdateWidget(oldWidget);
    if (widget.serverOrigin != oldWidget.serverOrigin ||
        widget.guideAccountKey != oldWidget.guideAccountKey ||
        !identical(widget.sessionAccess, oldWidget.sessionAccess) ||
        widget.onConnect != oldWidget.onConnect ||
        widget.onOpenWindow != oldWidget.onOpenWindow ||
        widget.onConnectTracks != oldWidget.onConnectTracks) {
      ++_streamConnectPromptRevision;
      _flows.clear();
      _syncTicker();
    }
    final ids = {for (final m in widget.machines) m.id};
    for (final id in _focus.keys.toList()) {
      if (!ids.contains(id)) _focus.remove(id)!.dispose();
    }
    _flows.removeWhere((id, _) => !ids.contains(id));
    _favoriteOverride.removeWhere((id, _) => !ids.contains(id));
    _favoriteSending.removeWhere((id) => !ids.contains(id));
    _normalizeChip();
    final listed = !identical(widget.machines, oldWidget.machines);
    // The time of the last successful check: a real list read, never a
    // pushed CPU/RAM sample or the emptied list of a failed read.
    if (widget.listGeneration == 0
        ? listed && widget.error == null
        : widget.listGeneration != oldWidget.listGeneration) {
      _listReceived = DateTime.now();
    }
    if (listed ||
        widget.busy != oldWidget.busy ||
        widget.error != oldWidget.error) {
      // Dialogs live in another route; tell them after this build.
      WidgetsBinding.instance.addPostFrameCallback((_) {
        if (mounted) _listChanges.value++;
      });
    }
    if (!_preselected) _preselect();
    _advanceFlows();
    if (widget.catalogChanges != oldWidget.catalogChanges ||
        widget.liveChannel != oldWidget.liveChannel) {
      oldWidget.liveChannel?.removeListener(_liveChanged);
      if (!widget.isDemo && widget.checkClientUpdate != null) _bindCatalog();
    }
    if (widget.previews != oldWidget.previews) {
      _bindPreviews();
    } else if (_previewsShown) {
      // Gone from the list: forget its picture. A new desktop, or a Windows
      // session change (lock, sign-out, switch), is asked about at once.
      _previews.listed(
        widget.machines,
        ask: !identical(widget.machines, oldWidget.machines),
      );
    }
    if (widget.thumbnails != oldWidget.thumbnails) {
      _bindThumbnails();
    } else if (widget.thumbnails != null &&
        (widget.listGeneration == 0
            ? !identical(widget.machines, oldWidget.machines)
            : widget.listGeneration != oldWidget.listGeneration)) {
      // The normal list refresh also picks up pictures written meanwhile.
      for (final id
          in _thumbnails.keys
              .where((id) => !ids.contains(id) || !_thumbnailAllowed(id))
              .toList()) {
        _removeThumbnail(id);
        _thumbnailReads[id] = (_thumbnailReads[id] ?? 0) + 1;
      }
      _loadThumbnails();
    }
  }

  @override
  void dispose() {
    HardwareKeyboard.instance.removeHandler(_launcherKey);
    _search.dispose();
    _searchFocus.dispose();
    _thumbnailGeneration++;
    unawaited(_thumbnailUpdates?.cancel());
    _thumbnailClock?.cancel();
    _previews.stop();
    _previewLifecycle?.dispose();
    _updateClock?.cancel();
    unawaited(_catalogPush?.cancel());
    widget.liveChannel?.removeListener(_liveChanged);
    _updateRecent?.cancel();
    _lifecycle?.dispose();
    _listChanges.dispose();
    _desktopScroll.dispose();
    AppMenu.closed(_viewMenu);
    _evictThumbnails();
    _ticker?.cancel();
    for (final node in _focus.values) {
      node.dispose();
    }
    super.dispose();
  }

  // --- power on, then connect -------------------------------------------

  void _syncTicker() {
    if (_flows.isEmpty) {
      _ticker?.cancel();
      _ticker = null;
      return;
    }
    _ticker ??= Timer.periodic(const Duration(seconds: 1), (_) {
      if (!mounted) return;
      for (final flow in _flows.values) {
        flow.seconds++;
      }
      // A cleanup normally clears in a second or two: look again soon.
      if (_flows.values.any(
        (f) => f.settle && !f.connecting && f.seconds.isEven,
      )) {
        _check();
      }
      final expired = [
        for (final entry in _flows.entries)
          if (!entry.value.connecting &&
              entry.value.seconds > _startTimeout.inSeconds)
            entry.key,
      ];
      setState(() {
        for (final id in expired) {
          _flows.remove(id);
        }
      });
      if (expired.isNotEmpty) {
        ScaffoldMessenger.maybeOf(context)?.showSnackBar(
          SnackBar(
            content: KeepAllText(
              l10n.autoConnectStoppedBecauseConnection,
            ),
          ),
        );
      }
      _syncTicker();
    });
  }

  Future<void> _openConsole(DesktopMachine machine) async {
    if (_confirming ||
        widget.isDemo ||
        widget.onOpenConsole == null ||
        machine.kind != MachineKind.vm) {
      return;
    }
    _confirming = true;
    try {
      if (!await showAppConfirm(
        context,
        title: l10n.openConsole,
        subtitle: machine.name,
        icon: Icons.settings_remote_outlined,
        message: l10n.opensDesktopsOwnScreenApp,
        confirmLabel: l10n.openConsole2,
      )) {
        return;
      }
      if (!mounted) return;
      final current = widget.machines
          .where((m) => m.id == machine.id)
          .firstOrNull;
      if (current == null || current.kind != MachineKind.vm) return;
      final result = await widget.onOpenConsole!(current);
      if (mounted && result?.opened == false) {
        ScaffoldMessenger.of(context).showSnackBar(
          SnackBar(
            content: Text(l10n.broughtDesktopsOpenWindowFront),
          ),
        );
      }
    } catch (error) {
      if (mounted) {
        ScaffoldMessenger.of(context).showSnackBar(
          SnackBar(
            content: Text(
              error is BrokerException ? error.message : l10n.couldntOpenConsoleWindow,
            ),
          ),
        );
      }
    } finally {
      _confirming = false;
    }
  }

  bool _confirming = false;
  bool _streamConnectPromptOpen = false;
  int _streamConnectPromptRevision = 0;

  Future<void> _connectReady(
    DesktopMachine machine,
    SessionDisplayMode mode,
    Future<void> Function(DesktopMachine current, bool handoverAgreed) connect,
  ) async {
    if (_confirming || _streamConnectPromptOpen || !mounted) return;
    // Fresh launcher intents ask before a callback, queued connection or native
    // window can start. An occupied stream still needs its separate consent.
    // One question, never two: when the Windows screen has
    // to be taken over from RDP, this question is that one, and [connect]
    // hears that it was agreed to.
    final current = widget.machines
        .where((value) => value.id == machine.id)
        .firstOrNull;
    if (current == null) return;
    bool eligible(DesktopMachine value) {
      final readiness = _readiness(value);
      return !widget.busy &&
          !value.powerBusy &&
          !value.display.busy &&
          (mode != SessionDisplayMode.allMonitors || _allMonitors(value)) &&
          ((_canOpen(value) && readiness.phase == TilePhase.ready) ||
              (readiness.settling &&
                  (widget.onConnect != null || widget.onOpenWindow != null)));
    }

    if (!eligible(current)) {
      ScaffoldMessenger.maybeOf(context)?.showSnackBar(
        SnackBar(content: KeepAllText(l10n.connectionReadinessChangedCheckList)),
      );
      return;
    }
    if (widget.isDemo) {
      await connect(current, false);
      return;
    }
    bool handover(DesktopMachine value) =>
        !value.isReady &&
        value.canRecoverConsole &&
        widget.onPrepareConsole != null;
    // A connection that first waits for the previous one's cleanup starts
    // later, from its queue: that start asks its own question then.
    final asksHandover = handover(current) && !_readiness(current).settling;
    final revision = ++_streamConnectPromptRevision;
    final server = widget.serverOrigin, account = widget.guideAccountKey;
    final access = widget.sessionAccess, owner = access?.owner;
    final onConnect = widget.onConnect, onOpenWindow = widget.onOpenWindow;
    final onTracks = widget.onConnectTracks;
    bool owned() =>
        mounted &&
        revision == _streamConnectPromptRevision &&
        widget.serverOrigin == server &&
        widget.guideAccountKey == account &&
        identical(widget.sessionAccess, access) &&
        access?.owner == owner &&
        (access?.check() ?? true) &&
        widget.onConnect == onConnect &&
        widget.onOpenWindow == onOpenWindow &&
        widget.onConnectTracks == onTracks;
    final messenger = ScaffoldMessenger.maybeOf(context);
    _streamConnectPromptOpen = true;
    var accepted = false;
    try {
      accepted = asksHandover
          ? await _askHandover(current)
          : await showAppConfirm(
              context,
              dialogKey: const Key('connect-confirm-dialog'),
              title: l10n.connect5(current.name),
              icon: Icons.desktop_windows_rounded,
              confirmLabel: l10n.connect2,
              confirmKey: Key('confirm-connect-${current.id}'),
              cancelKey: Key('cancel-connect-${current.id}'),
            );
    } finally {
      _streamConnectPromptOpen = false;
    }
    if (!accepted || !owned()) return;
    final latest = widget.machines
        .where((value) => value.id == current.id && value.kind == current.kind)
        .firstOrNull;
    if (latest == null || !eligible(latest)) {
      // The desktop's state changed while the question was open: said, the
      // same way as before the question.
      messenger?.showSnackBar(
        SnackBar(content: KeepAllText(l10n.connectionReadinessChangedCheckList)),
      );
      return;
    }
    // Agreed only for the state that was asked about: a screen that needs the
    // takeover only now gets its own question later.
    await connect(latest, asksHandover);
  }

  /// The one question before a Windows screen held by RDP is taken over.
  Future<bool> _askHandover(DesktopMachine machine) => showAppConfirm(
    context,
    dialogKey: const Key('rdp-handover-confirm'),
    confirmKey: const Key('rdp-handover-accept'),
    cancelKey: const Key('rdp-handover-cancel'),
    title: l10n.takeOverWindowsScreen,
    subtitle: machine.name,
    icon: Icons.swap_horiz_rounded,
    tone: AppTone.warn,
    message:
        l10n.currentWindowsScreenMovesStreaming,
    confirmLabel: l10n.takeOverConnect,
  );

  /// 'Console 끊고 연결': after a confirmation, end the VM's
  /// console connections, then connect as soon as the tile is ready (the
  /// console's display reservation clears with it).
  Future<void> _endConsolesAndConnect(DesktopMachine machine) async {
    final end = widget.onEndConsoles;
    if (end == null || _confirming || _flows.containsKey(machine.id)) return;
    _confirming = true;
    bool accepted;
    try {
      accepted = await showAppConfirm(
        context,
        title: l10n.disconnectConsoleConnect,
        subtitle: machine.name,
        icon: Icons.swap_horiz_rounded,
        message: l10n.openConsoleConnectionWillEnd,
        confirmLabel: l10n.disconnectConnect,
        confirmKey: Key('confirm-end-console-${machine.id}'),
      );
    } finally {
      _confirming = false;
    }
    if (!accepted || !mounted) return;
    try {
      await end(machine.id);
    } catch (error) {
      if (mounted) {
        ScaffoldMessenger.of(context).showSnackBar(
          SnackBar(
            content: KeepAllText(
              error is BrokerException ? error.message : l10n.couldntEndConsoleConnection,
            ),
          ),
        );
      }
      return;
    }
    if (!mounted) return;
    setState(
      () => _flows[machine.id] = _StartFlow(_modeFor(machine))..powered = true,
    );
    _syncTicker();
    widget.onRefresh();
  }

  /// Connects as soon as the previous connection's cleanup clears.
  void _settleAndConnect(DesktopMachine machine, SessionDisplayMode mode) {
    if (_flows.containsKey(machine.id)) return;
    setState(
      () => _flows[machine.id] = _StartFlow(mode)
        ..powered = true
        ..settle = true,
    );
    _syncTicker();
    _advanceFlows();
  }

  void _cancelFlow(String id) {
    if (_flows.remove(id) == null) return;
    setState(() {});
    _syncTicker();
    ScaffoldMessenger.of(context).showSnackBar(
      SnackBar(content: KeepAllText(l10n.autoConnectCanceled)),
    );
  }

  void _advanceFlows() {
    if (_streamConnectPromptOpen) return;
    for (final entry in _flows.entries.toList()) {
      final flow = entry.value;
      if (flow.connecting) continue;
      final match = widget.machines.where((m) => m.id == entry.key);
      if (match.isEmpty) continue;
      final machine = match.first;
      final readiness = _readiness(machine);
      // The cleanup stopped or failed: the tile now explains it instead.
      // (Also called from didUpdateWidget, so the rebuild is scheduled.)
      if (flow.settle && !readiness.settling && !readiness.canStream) {
        _flows.remove(entry.key);
        _syncTicker();
        WidgetsBinding.instance.addPostFrameCallback((_) {
          if (mounted) setState(() {});
        });
        continue;
      }
      if (!readiness.canStream || !_canOpen(machine)) continue;
      flow.connecting = true;
      WidgetsBinding.instance.addPostFrameCallback((_) {
        if (!mounted || _flows[entry.key] != flow) return;
        unawaited(
          _openDesktop(machine, mode: flow.mode).whenComplete(() {
            if (!mounted || _flows[entry.key] != flow) return;
            setState(() => _flows.remove(entry.key));
            _syncTicker();
          }),
        );
      });
    }
  }

  StartProgress? _progress(DesktopMachine machine) {
    final flow = _flows[machine.id];
    if (flow == null) return null;
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

  // --- connections --------------------------------------------------------

  Future<void> _detached(
    DesktopMachine machine,
    SessionDisplayMode mode,
    StreamOptions options,
  ) async {
    try {
      await widget.onOpenWindow!(machine, mode, options);
    } catch (error) {
      if (mounted) {
        ScaffoldMessenger.of(context).showSnackBar(
          SnackBar(
            content: KeepAllText(
              error is BrokerException
                  ? error.message
                  : l10n.couldntOpenRemoteWindowTry,
            ),
          ),
        );
      }
    }
  }

  Future<void> _signOut() async {
    final confirmed = await showAppConfirm(
      context,
      icon: Icons.logout_rounded,
      title: l10n.signOut,
      subtitle: '${widget.displayName} · $_serverHost',
      message: widget.onOpenWindow == null
          ? null
          : l10n.openRemoteWindowsCloseToo,
      confirmLabel: l10n.signOut2,
      cancelKey: const Key('cancel-sign-out'),
      confirmKey: const Key('confirm-sign-out'),
    );
    if (confirmed && mounted) widget.onSignOut();
  }

  Future<void> _power(DesktopMachine machine, String action) async {
    final pc = machine.kind == MachineKind.pc;
    if (pc ? !machine.pcPower.supported : machine.kind != MachineKind.vm) {
      throw const BrokerException('invalid_power_action');
    }
    if (!clientVmPowerActions.contains(action)) {
      throw const BrokerException('invalid_power_action');
    }
    // An offline PC the server cannot wake: the reason, not a question.
    if (pc && action == 'start' && !machine.pcPower.wake) {
      await showPcWakeHelp(
        context,
        reason: pcWakeFailure('pc_wake_unavailable')!,
      );
      return;
    }
    // The same words as the tile menu and the attention button.
    final label = powerActionLabel(action);
    if (!await confirmPowerAction(
          context,
          action: action,
          desktopId: machine.id,
          name: machine.name,
          pc: pc,
        ) ||
        !mounted) {
      return;
    }
    final messenger = ScaffoldMessenger.of(context);
    try {
      await widget.onPower!(machine.id, action);
      if (messenger.mounted) {
        messenger.showSnackBar(
          SnackBar(
            content: KeepAllText(
              pc && action == 'start'
                  ? l10n.localPc4(machine.name)
                  // Accepted is not done: Windows still closes its apps.
                  : pc
                  ? l10n.windowsReceivedRequest(machine.name, label)
                  : l10n.finished(machine.name, label),
            ),
          ),
        );
      }
    } catch (error) {
      if (error is BrokerException && error.code == 'power_superseded') return;
      // 켜기 that did not bring the PC up: one line and what WOL needs.
      final wake = error is BrokerException ? pcWakeFailure(error.code) : null;
      if (pc && wake != null) {
        if (mounted) await showPcWakeHelp(context, reason: wake);
        return;
      }
      // Codes shared with streaming get power wording here.
      final message = error is BrokerException
          ? switch (error.code) {
              'session_cleanup_pending' =>
                l10n.waitingPreviousConnectionCleanUp(label),
              'display_in_progress' =>
                l10n.numberMonitorsChangingSoRequest(label),
              'host_update_in_progress' =>
                l10n.hostUpdateInstallingSoRequest(label),
              _ => error.toString(),
            }
          // Runtime text of another error is not a message for a person.
          : l10n.couldntProcessRequestTryAgain3(label);
      if (messenger.mounted) {
        messenger.showSnackBar(SnackBar(content: KeepAllText(message)));
      }
    }
  }

  /// Any account the VM is assigned to (the broker decides what is offered):
  /// version, changelog and (with a live session) the explicit "now,
  /// disconnecting" choice, then one broker request. Progress and the result
  /// arrive as the tile's short Host update status.
  Future<void> _hostUpdate(DesktopMachine machine) async {
    final update = machine.hostUpdate, onHostUpdate = widget.onHostUpdate;
    if (onHostUpdate == null || !update.offered) {
      return;
    }
    final loader = widget.loadHostRelease;
    final choice = await showHostUpdateConfirm(
      context,
      machine: machine,
      server: widget.serverOrigin,
      loadRelease: loader == null ? null : () => loader(update.version!),
    );
    if (choice == null || !mounted) return;
    final current = widget.machines
        .where((m) => m.id == machine.id)
        .firstOrNull;
    if (current == null ||
        !current.hostUpdate.offered ||
        current.hostUpdate.latestVersion != update.latestVersion) {
      return;
    }
    final messenger = ScaffoldMessenger.of(context);
    try {
      await onHostUpdate(
        machine.id,
        update.latestVersion!,
        disconnectSessions: choice == HostUpdateChoice.disconnect,
      );
      if (messenger.mounted) {
        messenger.showSnackBar(
          SnackBar(
            content: KeepAllText(
              update.sessionActive && choice == HostUpdateChoice.update
                  ? l10n.hostUpdateStartsWhenConnection(machine.name, '${update.latestVersion}')
                  : l10n.hostUpdateRequested(machine.name, '${update.latestVersion}'),
            ),
          ),
        );
      }
    } catch (error) {
      if (messenger.mounted) {
        messenger.showSnackBar(
          SnackBar(
            content: KeepAllText(
              error is BrokerException
                  ? error.toString()
                  : l10n.couldntRequestHostUpdateTry,
            ),
          ),
        );
      }
    }
  }

  Future<void> _openDesktop(
    DesktopMachine machine, {
    // New clients prefer the automatic rate; released peers get fixed 20 Mbps.
    StreamOptions options = StreamOptions.recommended,
    SessionDisplayMode mode = SessionDisplayMode.windowed,
    // The launcher's question was already the takeover question.
    bool handoverAgreed = false,
  }) async {
    final isDemo = widget.isDemo;
    // All entry points, including a queued cleanup/power flow, arrive here.
    // Re-read the assignment immediately before opening or handing over.
    final current = widget.machines
        .where((value) => value.id == machine.id)
        .firstOrNull;
    if (current == null ||
        (!isDemo &&
            (widget.busy ||
                current.powerBusy ||
                current.display.busy ||
                !_canOpen(current)))) {
      return;
    }
    var target = current;
    if (mode == SessionDisplayMode.allMonitors &&
        (widget.localMonitors != 2 ||
            !target.independentDisplayStreaming ||
            (widget.onOpenWindow == null && widget.onConnectTracks == null))) {
      ScaffoldMessenger.of(context).showSnackBar(
        SnackBar(
          content: KeepAllText(
            l10n.allMonitorsWorksOnlyWhen,
          ),
        ),
      );
      return;
    }
    if (!isDemo) _remember(target);
    if (!isDemo &&
        !target.isReady &&
        target.canRecoverConsole &&
        widget.onPrepareConsole != null) {
      final resumed = await _resumeWindowsAfterConsent(
        target,
        agreed: handoverAgreed,
      );
      if (resumed == null || !mounted) return;
      target = resumed;
    }
    if (!mounted) return;
    if (!isDemo && widget.onOpenWindow != null) {
      await _detached(target, mode, options.forMonitors(target.monitors));
      return;
    }
    final onConnect = widget.onConnect;
    final onConnectTracks = widget.onConnectTracks;
    final onConnectViewport = widget.onConnectViewport;
    final onConfigureDisplays = widget.onConfigureDisplays;
    final onPrepareConsole = widget.onPrepareConsole;
    await showDialog<void>(
      context: context,
      builder: (context) => Dialog.fullscreen(
        child: isDemo
            ? SessionPreview(
                machine: target,
                onClose: () => Navigator.pop(context),
              )
            : RemoteView(
                sessionGuideEnabled: widget.sessionGuideEnabled,
                guideAccountKey: widget.guideAccountKey,
                pointerMemory: _memory,
                onPrepareConsole: onPrepareConsole == null
                    ? null
                    : () => onPrepareConsole(target.id),
                prepareDisplaysOnEntry: onConfigureDisplays != null,
                initialDisplayMode: mode,
                initialOptions: options.forSingleDisplay(),
                machine: target,
                onRefreshDesktop: widget.onReadDesktop == null
                    ? null
                    : () => widget.onReadDesktop!(target.id),
                onCheckTakeover: widget.onCheckTakeover == null
                    ? null
                    : () => widget.onCheckTakeover!(target.id),
                load: () => onConnect!(
                  target.id,
                  options: options.forMonitors(target.monitors),
                ),
                onReconnect: (options) =>
                    onConnect!(target.id, options: options),
                onConfirmTakeover: widget.onConfirmTakeover == null
                    ? null
                    : (offer, authorized) => widget.onConfirmTakeover!(
                        target.id,
                        offer,
                        authorized,
                      ),
                takeoverServer: widget.serverOrigin,
                onConnectTracks: onConnectTracks == null
                    ? null
                    : (plan, options) =>
                          onConnectTracks(target.id, plan, options),
                onConnectViewport: onConnectViewport == null
                    ? null
                    : (viewport, options) =>
                          onConnectViewport(target.id, viewport, options),
                onConnectScaled: widget.onConnectScaled == null
                    ? null
                    : (viewport, options, scale) => widget.onConnectScaled!(
                        target.id,
                        viewport,
                        options,
                        scale,
                      ),
                onConnectTracksScaled: widget.onConnectTracksScaled == null
                    ? null
                    : (plan, options, scale) => widget.onConnectTracksScaled!(
                        target.id,
                        plan,
                        options,
                        scale,
                      ),
                onConfigureDisplays: onConfigureDisplays == null
                    ? null
                    : (count) => onConfigureDisplays(target.id, count),
              ),
      ),
    );
  }

  Future<DesktopMachine?> _resumeWindowsAfterConsent(
    DesktopMachine machine, {
    bool agreed = false,
  }) async {
    if (_confirming || !mounted) return null;
    final origin = widget.serverOrigin;
    final account = widget.guideAccountKey ?? widget.displayName;
    bool owned() =>
        mounted &&
        widget.serverOrigin == origin &&
        (widget.guideAccountKey ?? widget.displayName) == account;
    DesktopMachine? current() =>
        widget.machines.where((value) => value.id == machine.id).firstOrNull;
    bool eligible(DesktopMachine value) =>
        !widget.busy &&
        !value.powerBusy &&
        !value.display.busy &&
        value.kind == machine.kind &&
        _canOpen(value);
    _confirming = true;
    try {
      final consent = agreed || await _askHandover(machine);
      if (!consent || !mounted || !owned()) return null;
      final fresh = current();
      if (fresh == null || !eligible(fresh)) return null;
      if (fresh.isReady) return fresh;
      if (!fresh.canRecoverConsole || widget.onPrepareConsole == null) {
        return null;
      }
      final resumed = await showDialog<DesktopMachine>(
        context: context,
        barrierDismissible: false,
        builder: (_) => ConsoleResumeDialog(
          machine: fresh,
          resume: () {
            final latest = current();
            final prepare = widget.onPrepareConsole;
            if (!owned() || latest == null || !eligible(latest)) {
              throw const BrokerException('desktop_changed');
            }
            if (latest.isReady) return Future.value(latest);
            if (!latest.canRecoverConsole || prepare == null) {
              throw const BrokerException('desktop_changed');
            }
            return prepare(latest.id);
          },
        ),
      );
      if (!owned() ||
          resumed == null ||
          resumed.id != machine.id ||
          !resumed.isReady) {
        return null;
      }
      // The parent's successful handover updates the list in setState.
      // Read its new assignment after that frame, not the preceding snapshot
      // that still marks our own display operation as pending.
      await WidgetsBinding.instance.endOfFrame;
      if (!owned()) return null;
      final latest = current();
      return latest != null && eligible(latest) ? resumed : null;
    } finally {
      _confirming = false;
    }
  }

  Future<void> _streamSettings(DesktopMachine machine) async {
    final origin = widget.serverOrigin;
    final account = widget.guideAccountKey ?? widget.displayName;
    final options = await showDialog<StreamOptions>(
      context: context,
      builder: (_) => StreamSettingsDialog(machine: machine),
    );
    if (options != null &&
        mounted &&
        widget.serverOrigin == origin &&
        (widget.guideAccountKey ?? widget.displayName) == account) {
      final mode = _modeFor(machine);
      await _connectReady(
        machine,
        mode,
        (current, agreed) => _openDesktop(
          current,
          options: options,
          mode: mode,
          handoverAgreed: agreed,
        ),
      );
    }
  }

  void _chooseMode(DesktopMachine machine, SessionDisplayMode mode) {
    unawaited(
      _connectReady(machine, mode, (current, agreed) async {
        await _openDesktop(current, mode: mode, handoverAgreed: agreed);
      }),
    );
  }

  // Read-only information and licenses; the existing release history remains.
  // Updates, logs, appearance and password keep their existing entry points.
  Future<void> _openSettings() async {
    if (_clientInfoOpen) return;
    _clientInfoOpen = true;
    try {
      await showAppInfoDialog(
        context,
        isDemo: widget.isDemo,
        serverOrigin: widget.serverOrigin,
        account: widget.isDemo ? null : widget.displayName,
        onCheckUpdates: widget.isDemo
            ? null
            : () => unawaited(
                showProductUpdates(
                  context,
                  server: widget.serverOrigin,
                  product: 'client',
                  currentVersion: clientProductVersion,
                ),
              ),
        onReleaseHistory: widget.isDemo
            ? null
            : () => unawaited(
                showReleaseHistory(
                  context,
                  server: widget.serverOrigin,
                  product: 'client',
                  currentVersion: clientProductVersion,
                ),
              ),
      );
    } finally {
      _clientInfoOpen = false;
    }
  }

  void _changePassword() => showDialog<void>(
    context: context,
    barrierDismissible: false,
    builder: (_) => ChangePasswordDialog(onChange: widget.onChangePassword!),
  );

  // --- readiness ----------------------------------------------------------

  String get _serverHost =>
      Uri.tryParse(widget.serverOrigin)?.host.isNotEmpty == true
      ? Uri.parse(widget.serverOrigin).host
      : l10n.server;

  bool _canOpen(DesktopMachine m) =>
      !widget.busy &&
      (m.kind != MachineKind.pc || m.localPcApproved || widget.isDemo) &&
      (m.isReady || (widget.onPrepareConsole != null && m.canRecoverConsole)) &&
      (widget.isDemo ||
          widget.onConnect != null ||
          widget.onOpenWindow != null);

  DesktopReadiness _readiness(DesktopMachine m) => DesktopReadiness.of(
    m,
    consoleRecovery: widget.onPrepareConsole != null,
    preview: widget.isDemo,
    server: _serverHost,
  );

  void _details(DesktopMachine machine) {
    final readiness = _readiness(machine);
    if (readiness.cleanup && !widget.isDemo) {
      showDialog<void>(
        context: context,
        builder: (_) => SessionCleanupDialog(
          machine: machine,
          onRefresh: widget.busy ? null : widget.onRefresh,
        ),
      );
      return;
    }
    // A real check: opening asks the broker again for the Host and Proxmox
    // state, and the dialog follows the answer.
    _check();
    showDialog<void>(
      context: context,
      builder: (_) => ListenableBuilder(
        listenable: _listChanges,
        builder: (_, _) {
          final current =
              widget.machines.where((m) => m.id == machine.id).firstOrNull ??
              machine;
          final readiness = _readiness(current);
          final actions = _actionsFor(current, readiness);
          return ReadinessDialog(
            machine: current,
            readiness: readiness,
            hostVersion: widget.isDemo ? null : current.hostUpdate.versionText,
            checkedAt: widget.isDemo ? null : _listReceived,
            checking: widget.busy,
            checkFailed: !widget.busy && widget.error != null,
            onCheck: widget.isDemo ? null : _check,
            actionFor: (action) => switch (action) {
              ReadinessAction.start =>
                actions.primary == null || readiness.phase != TilePhase.off
                    ? null
                    : actions.primary,
              ReadinessAction.reboot => actions.reboot,
              ReadinessAction.refresh =>
                widget.busy || widget.isDemo ? null : widget.onRefresh,
              ReadinessAction.console => actions.console,
              ReadinessAction.releaseConsole => actions.attention,
            },
          );
        },
      ),
    );
  }

  void _check() {
    if (widget.isDemo || widget.busy) return;
    widget.onRefresh();
  }

  DesktopActions _actionsFor(DesktopMachine m, DesktopReadiness r) {
    final isDemo = widget.isDemo;
    final busy = widget.busy;
    final available = !busy && !m.powerBusy && !m.display.busy;
    final streamOk = available && _canOpen(m);
    final flow = _flows[m.id];
    final canStartFlow =
        !isDemo &&
        widget.onPower != null &&
        !busy &&
        m.canStart &&
        r.phase == TilePhase.off;
    final mode = _modeFor(m);
    final powerOk = !busy && widget.onPower != null && !isDemo;
    final reboot = powerOk && m.canReboot ? () => _power(m, 'reboot') : null;
    final refresh = busy || isDemo ? null : widget.onRefresh;
    // The previous connection is still being cleaned up: a press waits for it.
    final settleOk =
        !isDemo &&
        available &&
        r.settling &&
        (widget.onConnect != null || widget.onOpenWindow != null);
    return DesktopActions(
      console:
          !isDemo && m.kind == MachineKind.vm && widget.onOpenConsole != null
          ? () => unawaited(_openConsole(m))
          : null,
      // Main controls and menu choices recheck current assignment/readiness
      // before starting, without a second ordinary-connect confirmation.
      primary: flow != null
          ? null
          : r.settling
          ? (settleOk
                ? () => unawaited(
                    _connectReady(
                      m,
                      mode,
                      (current, _) async => _settleAndConnect(current, mode),
                    ),
                  )
                : null)
          : r.phase == TilePhase.ready
          ? (streamOk
                ? () => unawaited(
                    _connectReady(
                      m,
                      mode,
                      (current, agreed) => _openDesktop(
                        current,
                        mode: mode,
                        handoverAgreed: agreed,
                      ),
                    ),
                  )
                : null)
          : r.phase == TilePhase.off
          ? (canStartFlow ? () => unawaited(_power(m, 'start')) : null)
          : null,
      stream: isDemo
          ? null
          : settleOk && flow == null
          ? (mode) {
              unawaited(
                _connectReady(m, mode, (current, _) async {
                  _settleAndConnect(current, mode);
                }),
              );
            }
          : streamOk && r.phase == TilePhase.ready
          ? (mode) => _chooseMode(m, mode)
          : null,
      streamUnavailable: m.streamReadiness,
      allMonitors: _allMonitors(m),
      allMonitorsReason: widget.localMonitors > 2
          ? l10n.allMonitorsWorksOnlyWhen2
          : null,
      localMonitors: widget.localMonitors,
      quality: !isDemo && streamOk && r.phase == TilePhase.ready
          ? () => unawaited(_streamSettings(m))
          : null,
      // An approved PC: its ⏻ only where the broker offers PC power, and
      // never for an online Host that announced nothing (too old).
      showPower:
          !isDemo &&
          widget.onPower != null &&
          (m.kind == MachineKind.vm ||
              (m.pcPower.supported &&
                  (m.hostOnline != true || m.pcPower.actions.isNotEmpty))),
      pcPower: m.kind == MachineKind.pc,
      showStart: m.kind == MachineKind.pc
          ? m.hostOnline != true
          : m.powerState != 'running',
      powerReason: m.powerBusy
          ? l10n.powerTaskProgress
          : m.kind == MachineKind.vm &&
                (m.powerStale || m.powerState == 'unknown')
          ? l10n.checkingPowerStatusRefreshMoment
          : m.display.busy
          ? l10n.numberMonitorsChangingPowerAvailable
          : busy
          ? l10n.refreshingList
          : null,
      // An offline PC the server cannot wake still answers 켜기 with why.
      start:
          powerOk &&
              (m.canStart ||
                  (m.kind == MachineKind.pc &&
                      m.hostOnline != true &&
                      !m.powerBusy &&
                      !m.pcPower.wake))
          ? () => unawaited(_power(m, 'start'))
          : null,
      reboot: reboot == null ? null : () => unawaited(reboot()),
      shutdown: powerOk && m.canShutdown
          ? () => unawaited(_power(m, 'shutdown'))
          : null,
      // Also while this account's own 종료 has not finished: the broker
      // then replaces the shutdown with the stop.
      stop: powerOk && m.canStop ? () => unawaited(_power(m, 'stop')) : null,
      stopReason: busy ? null : stopUnavailableReason(m),
      details: () => _details(m),
      attention: r.primary == ReadinessAction.reboot
          ? (reboot == null ? null : () => unawaited(reboot()))
          : r.primary == ReadinessAction.releaseConsole
          ? (widget.onEndConsoles == null || busy || isDemo
                ? null
                : () => unawaited(_endConsolesAndConnect(m)))
          : refresh,
      cancel: flow == null ? null : () => _cancelFlow(m.id),
      menu: !isDemo,
      // Favourites only where the broker has them; the change shows at once.
      favorite: !isDemo && widget.onFavorite != null && m.organize.supported
          ? m.organize.favorite
          : null,
      toggleFavorite: _favoriteSending.contains(m.id)
          ? null
          : () => unawaited(_toggleFavorite(m)),
      // Only an update that can be installed now; its version and state
      // otherwise appear in the readiness check.
      hostUpdate:
          !isDemo &&
              !busy &&
              widget.onHostUpdate != null &&
              m.hostUpdate.offered
          ? () => unawaited(_hostUpdate(m))
          : null,
    );
  }

  // --- layout -------------------------------------------------------------

  /// The list with the notice banner floating over its bottom-right corner,
  /// where a Windows notification rises and clear of the title row's filters
  /// and search. The banner takes no room in the list and no keyboard focus.
  Widget _noticeLayer(double side, Widget list) {
    final notices = widget.notices;
    // Always the same shape, so the list keeps its place and its state.
    return Stack(
      fit: StackFit.expand,
      children: [
        list,
        if (notices != null && !widget.isDemo)
        Positioned(
          bottom: 16,
          left: side,
          right: side,
          child: Align(
            alignment: Alignment.bottomRight,
            child: NoticeBanner(
              center: notices,
              active: widget.noticesActive,
              fromBelow: true,
            ),
          ),
        ),
      ],
    );
  }

  /// Assigned desktops whose Host can be updated now (the ⋯ badge's rule).
  /// The header's update button badges them too; the list push keeps the
  /// count live.
  int get _hostUpdates => widget.isDemo || widget.onHostBatchUpdate == null
      ? 0
      : widget.machines.where((m) => m.hostUpdate.offered).length;

  Widget _header(BuildContext context, double side) {
    final versionLabel = runningClientBuild.version == null
        ? runningClientBuild.label
        : 'v${runningClientBuild.version}';
    final hostUpdates = _hostUpdates;
    // The panel's own check is current; read the badge again afterwards.
    Future<void> updates() async {
      if (_updatePanelOpen) return;
      _updatePanelOpen = true;
      try {
        // The app only: Host updates have their own header button.
        await showProductUpdates(
          context,
          server: widget.serverOrigin,
          product: 'client',
          currentVersion: clientProductVersion,
        );
      } finally {
        _updatePanelOpen = false;
        if (mounted) unawaited(_checkClientUpdate());
      }
    }

    return WorkspaceHeader(
      sessionAccess: widget.sessionAccess,
      noticeBell: widget.notices == null || widget.isDemo
          ? null
          : NoticeBell(center: widget.notices!),
      side: side,
      columnWidth: WorkspaceShell.columnWidth,
      isDemo: widget.isDemo,
      accountLabel: widget.isDemo ? l10n.demoNoServer : widget.displayName,
      server: _serverHost,
      version: versionLabel,
      update: _clientUpdate,
      hostUpdates: hostUpdates,
      onUpdates: widget.isDemo ? null : () => unawaited(updates()),
      // Host updates open on their own, never inside the app update panel.
      onHostUpdates: widget.isDemo
          ? null
          : () {
              final batch = widget.onHostBatchUpdate;
              if (hostUpdates > 0 && batch != null) {
                unawaited(batch(context));
              }
            },
      onClientInfo: () => unawaited(_openSettings()),
      onLog: () => unawaited(showProductLog(context)),
      onChangePassword: !widget.isDemo && widget.onChangePassword != null
          ? _changePassword
          : null,
      onManageMfa: !widget.isDemo && widget.onManageMfa != null
          ? () => unawaited(widget.onManageMfa!(context)) : null,
      onSignOut: widget.busy ? null : () => unawaited(_signOut()),
      signOutReason: widget.busy ? l10n.refreshingListTryAgainMoment : null,
    );
  }

  Widget _viewSelector({bool compact = false}) => LauncherViewMenu(
    controller: _viewMenu,
    view: _prefs.view,
    showPreviews: _prefs.showPreviews,
    onSelected: (view) => _selectView(view),
    onPreviews: (value) => _selectView(_prefs.view, showPreviews: value),
    compact: compact,
    compactChrome: true,
  );

  /// 내 데스크톱 and its count (N, or shown/N while a filter hides some).
  Widget _titleBlock(AppPalette p, int count, int shown) => Expanded(
    child: Row(
      children: [
        Flexible(
          child: FittedBox(
            key: const Key('workspace-title'),
            fit: BoxFit.scaleDown,
            alignment: Alignment.centerLeft,
            child: KeepAllText(
              l10n.myDesktops,
              maxLines: 1,
              style: TextStyle(
                color: p.ink,
                fontSize: 24,
                fontWeight: FontWeight.w600,
                letterSpacing: -.3,
                shadows: p.sceneHalo,
              ),
            ),
          ),
        ),
        const SizedBox(width: 12),
        if (count > 0)
          Flexible(
            child: Padding(
              padding: const EdgeInsets.symmetric(vertical: 2),
              child: FittedBox(
                fit: BoxFit.scaleDown,
                child: KeepAllText(
                  shown == count ? '$count' : '$shown/$count',
                  key: const Key('desktop-count'),
                  style: TextStyle(
                    color: p.muted,
                    fontFamily: 'Pretendard',
                    fontSize: 13,
                    fontWeight: FontWeight.w600,
                    fontFeatures: const [FontFeature.tabularFigures()],
                  ),
                ),
              ),
            ),
          ),
      ],
    ),
  );

  /// While the list is read the button keeps its shape and label; a spinner
  /// takes the icon's place and presses wait (the bar under the title shows
  /// the same read).
  Widget _refreshButton({bool compact = false}) => LauncherHeaderButton(
    key: const Key('refresh-desktops'),
    compactChrome: true,
    icon: Icons.refresh_rounded,
    label: compact ? null : l10n.refresh,
    tooltip: widget.busy ? l10n.refreshingList2 : l10n.refreshList,
    busy: widget.busy,
    onPressed: _refreshList,
  );

  DesktopSearchField _searchField({
    required bool open,
    double? width,
    bool closable = false,
  }) => DesktopSearchField(
    controller: _search,
    focusNode: _searchFocus,
    onSubmitted: _searchSubmitted,
    onLeave: _closeSearch,
    onOpen: _openSearch,
    onDown: () {
      final shownNow = _shownMachines();
      if (shownNow.isNotEmpty) _node(shownNow.first.id).requestFocus();
    },
    open: open,
    width: width,
    closable: closable,
    compactChrome: true,
  );

  /// The header: 내 데스크톱 with its count, then the 구분 filter (only with
  /// both VMs and local PCs), the status filter, the search, the sort button,
  /// 미리보기 and 새로고침 — every control one height and the same pill.
  ///
  /// Wide windows keep all of it on the title line, the search growing out of
  /// its magnifier (or always open when there is room). Narrow windows and
  /// enlarged text keep 미리보기 and 새로고침 beside the title (round icons
  /// with tooltips when the line is short) and put the filters, the magnifier
  /// and the sort button on their own row under it (the filters scroll
  /// sideways if they must); an open search takes a row of its own below
  /// that, so the filters stay visible while searching.
  Widget _titleRow(
    BuildContext context, {
    required int count,
    required int shown,
    required bool organize,
    required bool inline,
    required bool wide,
    required bool labels,
    required DesktopStatusCounts counts,
    required DesktopStatus status,
    required List<DesktopMachine> base,
    required DesktopKind? kind,
    required bool filtered,
  }) {
    final p = AppPalette.of(context);
    final hasDesktops = count > 0;
    final searching = _searchOpen || _query.isNotEmpty;
    final kindFilter = kind == null
        ? null
        : DesktopKindFilter(
            selected: kind,
            compactChrome: true,
            machines: base,
            onSelected: _selectKind,
          );
    final statusFilter = DesktopStatusFilter(
      selected: status,
      compactChrome: true,
      counts: counts,
      onSelected: _selectStatus,
    );
    final reset = filtered
        ? DesktopFilterReset(onPressed: _clearFilters, compactChrome: true)
        : null;
    final sort = organize
        ? DesktopSortMenu(sort: _sort, onSelected: _selectSort)
        : null;
    final toggle = hasDesktops ? _viewSelector(compact: !labels) : null;
    final refresh = widget.isDemo ? null : _refreshButton(compact: !labels);
    if (inline) {
      return Row(
        children: [
          _titleBlock(p, count, shown),
          const SizedBox(width: 12),
          if (hasDesktops) ...[
            if (reset != null) ...[reset, const SizedBox(width: 6)],
            if (kindFilter != null) ...[kindFilter, const SizedBox(width: 8)],
            statusFilter,
            const SizedBox(width: 8),
            _searchField(
              open: searching || (organize && wide),
              width: wide ? 240 : 200,
            ),
            if (sort != null) ...[const SizedBox(width: 8), sort],
            const SizedBox(width: 8),
          ],
          if (toggle != null) ...[toggle, const SizedBox(width: 8)],
          ?refresh,
        ],
      );
    }
    return Column(
      crossAxisAlignment: CrossAxisAlignment.stretch,
      children: [
        Row(
          children: [
            _titleBlock(p, count, shown),
            const SizedBox(width: 12),
            ?toggle,
            if (toggle != null && refresh != null) const SizedBox(width: 8),
            ?refresh,
          ],
        ),
        if (hasDesktops)
          Padding(
            padding: const EdgeInsets.only(top: 12),
            // The filters wrap onto another line rather than hide behind the
            // edge, the magnifier and sort stay at the right end and go to a
            // line of their own only when the filters need the width; a
            // filter wider than the whole line alone scrolls sideways.
            child: Wrap(
              alignment: WrapAlignment.spaceBetween,
              spacing: 8,
              runSpacing: 8,
              children: [
                Wrap(
                  key: const Key('desktop-status-row'),
                  spacing: 8,
                  runSpacing: 8,
                  children: [
                    if (kindFilter != null)
                      DesktopSideScroll(
                        height: organizerHeight(context),
                        child: kindFilter,
                      ),
                    DesktopSideScroll(
                      height: organizerHeight(context),
                      child: Row(
                        mainAxisSize: MainAxisSize.min,
                        children: [
                          statusFilter,
                          if (reset != null) ...[
                            const SizedBox(width: 6),
                            reset,
                          ],
                        ],
                      ),
                    ),
                  ],
                ),
                Row(
                  mainAxisSize: MainAxisSize.min,
                  children: [
                    if (!searching) _searchField(open: false, width: 200),
                    if (!searching && sort != null) const SizedBox(width: 8),
                    ?sort,
                  ],
                ),
              ],
            ),
          ),
        if (hasDesktops)
          AnimatedSize(
            duration: const Duration(milliseconds: 180),
            curve: Curves.easeOutCubic,
            alignment: Alignment.topCenter,
            child: searching
                ? Padding(
                    padding: const EdgeInsets.only(top: 10),
                    child: _searchField(open: true, closable: true),
                  )
                : const SizedBox(width: double.infinity),
          ),
      ],
    );
  }

  /// Under the title: the 분류 chips (favourites, pools, tags), when there
  /// are any.
  Widget _organizeBar(
    BuildContext context, {
    required List<(DesktopChip, int)> chips,
    required DesktopChip chip,
  }) {
    if (chips.isEmpty || widget.machines.isEmpty) {
      return const SizedBox.shrink();
    }
    return Padding(
      padding: const EdgeInsets.only(top: 12),
      child: DesktopChipRow(
        chips: chips,
        selected: chip,
        onSelected: _selectChip,
      ),
    );
  }

  /// A compact frosted card centred over the scene (the empty list and the
  /// failed first load), in the dialog's icon/title/subtitle layout.
  Widget _stateCard(
    BuildContext context, {
    required Key key,
    required IconData icon,
    required AppTone tone,
    required String title,
    required String detail,
    Key? detailKey,
    Widget? action,
  }) {
    final p = AppPalette.of(context);
    return Center(
      child: Container(
        key: key,
        constraints: const BoxConstraints(maxWidth: 440),
        margin: const EdgeInsets.only(top: 24),
        padding: const EdgeInsets.all(24),
        decoration: BoxDecoration(
          color: p.tileGlass,
          border: Border.all(color: p.glassEdge),
          borderRadius: BorderRadius.circular(16),
          boxShadow: [
            BoxShadow(
              color: p.shadow.withValues(alpha: .22),
              blurRadius: 18,
              spreadRadius: -10,
              offset: const Offset(0, 10),
            ),
          ],
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
                  child: Column(
                    crossAxisAlignment: CrossAxisAlignment.start,
                    children: [
                      KeepAllText(
                        title,
                        style: uiText(
                          size: 16,
                          weight: FontWeight.w600,
                          color: p.ink,
                        ),
                      ),
                      const SizedBox(height: 3),
                      KeepAllText(
                        detail,
                        key: detailKey,
                        style: uiText(size: 13, height: 1.5, color: p.muted),
                      ),
                    ],
                  ),
                ),
              ],
            ),
            if (action != null) ...[const SizedBox(height: 18), action],
          ],
        ),
      ),
    );
  }

  Widget _empty(BuildContext context) => _stateCard(
    context,
    key: const Key('empty-desktops'),
    icon: Icons.desktop_windows_outlined,
    tone: AppTone.info,
    title: l10n.noDesktopsAssignedYet,
    detail: l10n.askAdministratorAssignYouDesktop,
  );

  /// The search and the filters leave nothing to show.
  Widget _noMatch(
    BuildContext context,
    DesktopChip chip,
    DesktopStatus status,
    DesktopKind kind,
  ) {
    final p = AppPalette.of(context);
    final query = _query.trim();
    final filtered =
        status != DesktopStatus.all ||
        chip != DesktopChip.all ||
        kind != DesktopKind.all;
    return _stateCard(
      context,
      key: const Key('no-match-desktops'),
      icon: Icons.search_off_rounded,
      tone: AppTone.info,
      title: noMatchTitle(query),
      detail: noMatchDetail(status, chip, kind: kind),
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
          if (query.isNotEmpty)
            FilledButton.icon(
              key: const Key('no-match-clear'),
              onPressed: _closeSearch,
              style: AppButtons.filled(p),
              icon: const Icon(Icons.close_rounded, size: 18),
              label: KeepAllText(l10n.clearSearch),
            ),
        ],
      ),
    );
  }

  /// The list could not be read. With no desktops it is the whole page; over
  /// an older list it stays a slim banner above the tiles. The banner needs
  /// the whole sentence ([desktopListFailureText]); on the card that sentence
  /// only repeats the title, so the card says what to do instead.
  Widget _error(BuildContext context, String message) {
    final p = AppPalette.of(context);
    final retry = widget.busy ? null : widget.onRefresh;
    if (widget.machines.isEmpty) {
      return _stateCard(
        context,
        key: const Key('workspace-error-card'),
        icon: Icons.cloud_off_rounded,
        tone: AppTone.danger,
        title: l10n.couldntLoadList,
        detail: message == desktopListFailureText
            ? l10n.tryAgainMoment
            : message,
        detailKey: const Key('workspace-error'),
        action: widget.isDemo
            ? null
            : FilledButton.icon(
                key: const Key('workspace-error-retry'),
                onPressed: retry,
                style: AppButtons.filled(p),
                icon: const Icon(Icons.refresh_rounded, size: 18),
                label: KeepAllText(l10n.tryAgain),
              ),
      );
    }
    return _errorLine(
      context,
      message,
      textKey: const Key('workspace-error'),
      retryKey: const Key('workspace-error-retry'),
      onRetry: retry,
    );
  }

  /// One red line above the list with 다시 시도 for what failed.
  Widget _errorLine(
    BuildContext context,
    String message, {
    required Key textKey,
    required Key retryKey,
    required VoidCallback? onRetry,
  }) {
    final p = AppPalette.of(context);
    return Container(
      margin: const EdgeInsets.only(bottom: 16),
      padding: const EdgeInsets.fromLTRB(14, 8, 8, 8),
      decoration: BoxDecoration(
        color: Color.alphaBlend(p.red.withValues(alpha: .10), p.tileGlass),
        borderRadius: BorderRadius.circular(12),
        border: Border.all(color: p.red.withValues(alpha: .32)),
      ),
      child: Row(
        children: [
          Icon(Icons.error_outline_rounded, size: 18, color: p.red),
          const SizedBox(width: 10),
          Expanded(
            child: KeepAllText(
              message,
              key: textKey,
              style: uiText(size: 13, color: p.ink),
            ),
          ),
          if (!widget.isDemo)
            TextButton(
              key: retryKey,
              onPressed: onRetry,
              child: KeepAllText(l10n.tryAgain),
            ),
        ],
      ),
    );
  }

  /// A wait the user should know about: one quiet line, not an error.
  Widget _notice(BuildContext context, String message) {
    final p = AppPalette.of(context);
    return Container(
      margin: const EdgeInsets.only(bottom: 16),
      padding: const EdgeInsets.symmetric(horizontal: 14, vertical: 10),
      decoration: BoxDecoration(
        color: Color.alphaBlend(p.amber.withValues(alpha: .10), p.tileGlass),
        borderRadius: BorderRadius.circular(12),
        border: Border.all(color: p.amber.withValues(alpha: .32)),
      ),
      child: Row(
        children: [
          Icon(Icons.sync_rounded, size: 18, color: p.amber),
          const SizedBox(width: 10),
          Expanded(
            child: KeepAllText(
              message,
              key: const Key('workspace-notice'),
              style: uiText(size: 13, color: p.ink),
            ),
          ),
        ],
      ),
    );
  }

  Widget _desktopSliver(
    BuildContext context,
    List<DesktopMachine> shown,
    double width,
  ) {
    final scaler = MediaQuery.textScalerOf(context);
    final view = _prefs.view;
    if (view.isScene) {
      final base = _baseMachines();
      return SliverToBoxAdapter(
        child: LauncherScenes(
          view: view,
          desktops: [
            for (final machine in shown)
              LauncherDesktop(
                machine: machine,
                readiness: _readiness(machine),
                actions: _actionsFor(machine, _readiness(machine)),
                mode: _modeFor(machine),
                progress: _progress(machine),
                focusNode: _node(machine.id),
                thumbnail: _pictureFor(machine),
              ),
          ],
          chips: [(DesktopChip.all, base.length), ...chipsFor(base)],
          selectedChip: _effectiveChip(base),
          onChip: _selectChip,
          initialDesktop: _prefs.lastDesktop,
          preview: widget.isDemo,
          showPreviews: _prefs.showPreviews,
        ),
      );
    }
    final indexByKey = {
      for (var i = 0; i < shown.length; i++)
        '${view == LauncherView.cards ? 'slot' : 'row'}-${shown[i].id}': i,
    };
    int? indexOf(Key key) =>
        key is ValueKey<String> ? indexByKey[key.value] : null;
    // One grid rule with Android's launcher (desktopTileGrid).
    final grid = desktopTileGrid(
      width,
      scaler,
      viewport: MediaQuery.sizeOf(context),
      previews: _prefs.showPreviews,
    );
    Widget desktop(BuildContext context, int index) {
      final machine = shown[index], readiness = _readiness(machine);
      if (view != LauncherView.cards) {
        return Padding(
          key: ValueKey('row-${machine.id}'),
          padding: EdgeInsets.only(bottom: view == LauncherView.mini ? 6 : 10),
          child: DesktopRow(
            machine: machine,
            readiness: readiness,
            actions: _actionsFor(machine, readiness),
            mode: _modeFor(machine),
            progress: _progress(machine),
            focusNode: _node(machine.id),
            preview: widget.isDemo,
            dense: view == LauncherView.mini,
          ),
        );
      }
      return SizedBox(
        key: ValueKey('slot-${machine.id}'),
        child: DesktopTile(
          machine: machine,
          readiness: readiness,
          actions: _actionsFor(machine, readiness),
          mode: _modeFor(machine),
          progress: _progress(machine),
          focusNode: _node(machine.id),
          preview: widget.isDemo,
          thumbnail: _pictureFor(machine),
          showPreview: _prefs.showPreviews,
          previewNote: _previewNoteFor(machine),
          pictureHeight: grid.picture,
        ),
      );
    }

    if (view != LauncherView.cards) {
      return SliverList.builder(
        itemCount: shown.length,
        itemBuilder: desktop,
        findChildIndexCallback: indexOf,
      );
    }
    return SliverGrid.builder(
      gridDelegate: SliverGridDelegateWithFixedCrossAxisCount(
        crossAxisCount: grid.columns,
        mainAxisExtent: grid.height,
        crossAxisSpacing: desktopTileGap,
        mainAxisSpacing: desktopTileGap,
      ),
      itemCount: shown.length,
      itemBuilder: desktop,
      findChildIndexCallback: indexOf,
    );
  }

  @override
  Widget build(BuildContext context) {
    final p = AppPalette.of(context);
    final size = MediaQuery.sizeOf(context);
    final side = size.width < 600
        ? 16.0
        : size.width < 1100
        ? 32.0
        : 48.0;
    // The search and sort sit beside the title when the page column is wide
    // enough for the (scaled) text; otherwise on their own row.
    final textScale = MediaQuery.textScalerOf(context).scale(1).clamp(1.0, 1.6);
    final columnWidth = math.min(
      WorkspaceShell.columnWidth,
      size.width - 2 * side,
    );
    final base = _baseMachines();
    final kinds = kindFilterUseful(base);
    // The 구분 filter needs its own room on the title line.
    final extra = kinds ? 210.0 : 0.0;
    final inline = columnWidth >= (720 + extra) * textScale;
    final wide = columnWidth >= (940 + extra) * textScale;
    // A short title line keeps 미리보기 and 새로고침 as round icons.
    final labels =
        inline ||
        columnWidth >= 400 * MediaQuery.textScalerOf(context).scale(1);
    final organize = organizeUseful(base);
    final chip = _effectiveChip(base);
    final status = _effectiveStatus(base);
    final kind = _effectiveKind(base);
    final shown = organizeDesktops(
      base,
      query: _query,
      chip: chip,
      status: status,
      kind: kind,
      sort: _sort,
    );
    // The same full-window scenery as sign-in (dusk in dark, daylight in
    // light) behind the header, tiles and rows. Surfaces over it are
    // translucent fills with hairline edges; nothing uses a backdrop blur.
    return Scaffold(
      backgroundColor: p.launcher,
      body: SceneBackground(
        child: SafeArea(
          child: Column(
            children: [
              _header(context, side),
              Divider(height: 1, color: p.glassEdge),
              Expanded(
                child: _noticeLayer(
                  side,
                  FocusTraversalGroup(
                  child: CustomScrollView(
                    key: const Key('workspace-scroll'),
                    controller: _desktopScroll,
                    slivers: [
                      SliverPadding(
                        padding: EdgeInsets.fromLTRB(
                          (size.width - columnWidth) / 2,
                          18,
                          (size.width - columnWidth) / 2,
                          0,
                        ),
                        sliver: SliverToBoxAdapter(
                          child: Column(
                            crossAxisAlignment: CrossAxisAlignment.stretch,
                            children: [
                              _titleRow(
                                context,
                                count: widget.machines.length,
                                shown: shown.length,
                                organize: organize,
                                inline: inline,
                                wide: wide,
                                labels: labels,
                                counts: DesktopStatusCounts.of(
                                  facetMachines(base, kind: kind, chip: chip),
                                ),
                                status: status,
                                base: facetMachines(
                                  base,
                                  status: status,
                                  chip: chip,
                                ),
                                kind: kinds ? kind : null,
                                filtered:
                                    status != DesktopStatus.all ||
                                    chip != DesktopChip.all ||
                                    kind != DesktopKind.all,
                              ),
                              if (!LauncherScenes.hasSidebar(
                                context,
                                columnWidth,
                                _prefs.view,
                              ))
                                _organizeBar(
                                  context,
                                  chips: chipsFor(base),
                                  chip: chip,
                                ),
                              SizedBox(
                                height: 18,
                                child: widget.busy
                                    ? Align(
                                        alignment: const Alignment(0, -.1),
                                        child: LinearProgressIndicator(
                                          minHeight: 3,
                                          color: p.amber,
                                          backgroundColor: p.quiet,
                                        ),
                                      )
                                    : null,
                              ),
                              if (widget.notice != null)
                                _notice(context, widget.notice!),
                              if (widget.signOutError != null)
                                _errorLine(
                                  context,
                                  widget.signOutError!,
                                  textKey: const Key('workspace-sign-out-error'),
                                  retryKey: const Key(
                                    'workspace-sign-out-retry',
                                  ),
                                  onRetry: widget.busy
                                      ? null
                                      : widget.onSignOut,
                                ),
                              if (widget.error != null)
                                _error(context, widget.error!),
                              if (!widget.busy &&
                                  widget.error == null &&
                                  widget.notice == null &&
                                  widget.machines.isEmpty)
                                _empty(context),
                              if (widget.machines.isNotEmpty && shown.isEmpty)
                                _noMatch(context, chip, status, kind),
                            ],
                          ),
                        ),
                      ),
                      if (shown.isNotEmpty)
                        SliverPadding(
                          padding: EdgeInsets.symmetric(
                            horizontal: (size.width - columnWidth) / 2,
                          ),
                          sliver: _desktopSliver(context, shown, columnWidth),
                        ),
                      SliverToBoxAdapter(
                        child: SizedBox(
                          height: 20 + math.min(26, size.height * .03),
                        ),
                      ),
                    ],
                  ),
                ),
                ),
              ),
              if (widget.isDemo)
                Container(
                  key: const Key('workspace-footer'),
                  color: p.headerGlass,
                  padding: EdgeInsets.symmetric(horizontal: side, vertical: 12),
                  child: Row(
                    children: [
                      Icon(
                        Icons.info_outline_rounded,
                        size: 14,
                        color: p.muted,
                      ),
                      const SizedBox(width: 7),
                      Expanded(
                        child: KeepAllText(
                          l10n.sampleAssignmentsNotConnectedYet,
                          key: const Key('workspace-management-note'),
                          style: TextStyle(color: p.muted, fontSize: 12),
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

/// A list that could not be read when the server gave no reason of its own
/// (app.dart). The banner over an older list shows it whole; the empty page's
/// card has the same words as its title and shows what to do instead.
String get desktopListFailureText => l10n.couldntLoadDesktopList;
