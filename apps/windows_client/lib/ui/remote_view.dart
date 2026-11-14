import 'dart:async';
import 'dart:math' as math;

import 'package:flutter/gestures.dart';
import 'package:flutter/material.dart';
import 'package:flutter/services.dart';

import '../data/remote_session.dart';
import '../data/session_gamepads.dart';
import '../data/broker_api.dart';
import '../data/connect_timing.dart';
import '../data/connection_feedback.dart';
import '../data/clipboard_session.dart';
import '../data/native_print.dart';
import '../data/print_session.dart';
import 'print_dialog.dart';
import 'print_progress_notice.dart';
import '../data/drive_session.dart';
import '../data/usb_redirection.dart';
import '../models/desktop_machine.dart';
import 'remote_viewport.dart';
import 'pointer_move_coalescer.dart';
import '../data/window_presentation.dart';
import '../models/stream_options.dart';
import '../models/session_display_mode.dart';
import 'app_dialog.dart'
    show AppConfirmDialog, AppDialog, AppTone, confirmDisconnect, showAppNotice;
import 'app_palette.dart';
import 'drive_panel.dart';
import 'primary_region.dart';
import 'session_toolbar.dart';
import 'session_tools.dart';
import 'stream_settings_dialog.dart';
import 'session_connection_panel.dart';
import 'session_panels.dart';
import 'virtual_display_dialog.dart';
import '../data/session_failure.dart';
import 'session_status_panel.dart';
import 'connection_wait_status.dart';
import 'screen_transition_progress.dart';
import 'session_stats_dialog.dart';
import 'stream_session_stats.dart';
import 'stream_observation_overlay.dart';
import '../data/session_windows.dart';
import '../data/stream_display_entry.dart';
import '../data/local_display_layout.dart';
import '../data/track_stream.dart';
import '../data/initial_viewport.dart';
import '../data/initial_viewport_entry.dart';
import '../data/product_log.dart';
import '../data/launcher_memory.dart';
import '../data/session_guide_memory.dart';
import '../data/relative_mouse.dart';
import '../data/usb_game_mouse.dart';
import '../data/remote_cursor.dart';
import '../data/paste_target.dart';
import '../data/transfer_notice_window.dart';
import 'clipboard_transfer_card.dart';
import 'session_guide_dialog.dart';

import 'package:flutter/scheduler.dart';

import 'console_resume_dialog.dart';
import 'korean_text.dart';
import '../l10n/l10n.dart';

/// Why a device control is shown disabled: the Host offers it, this
/// assignment does not allow it (관리자 웹 → 장치 공유).
String get adminPermissionNeeded => l10n.administratorApprovalNeeded2;

/// The picture's pixel size: the track's, the held picture's while a resize
/// is pending, or the source size of a viewport session (tiles compose at
/// source resolution even when video is scaled).
Size remoteFrameSize(RemoteSession session, StreamTrack? track) {
  if (session.heldFrame case (final w, final h) when track == null) {
    return Size(w.toDouble(), h.toDouble());
  }
  return Size(
    (track?.width ??
            (session.connection.initialViewport != null
                ? session.sourceWidth ?? session.width
                : session.width))
        .toDouble(),
    (track?.height ??
            (session.connection.initialViewport != null
                ? session.sourceHeight ?? session.height
                : session.height))
        .toDouble(),
  );
}

/// Shown while a window resize swaps the remote resolution. It stays
/// invisible for the first 250 ms so a quick swap shows no flash at all.
class _ResizeVeil extends StatefulWidget {
  const _ResizeVeil({
    String? label,
    this.veilKey = const Key('resize-veil'),
  }) : _label = label; // ignore: prefer_initializing_formals

  /// session-resume-v1 shows the same pill while the connection comes back.
  final String? _label;
  String get label => _label ?? l10n.fittingScreenSize;
  final Key veilKey;
  @override
  State<_ResizeVeil> createState() => _ResizeVeilState();
}

class _ResizeVeilState extends State<_ResizeVeil> {
  bool _visible = false;
  Timer? _delay;
  @override
  void initState() {
    super.initState();
    _delay = Timer(const Duration(milliseconds: 250), () {
      if (mounted) setState(() => _visible = true);
    });
  }

  @override
  void dispose() {
    _delay?.cancel();
    super.dispose();
  }

  @override
  Widget build(BuildContext context) => Center(
    child: AnimatedOpacity(
      key: widget.veilKey,
      opacity: _visible ? 1 : 0,
      duration: const Duration(milliseconds: 180),
      child: Container(
        padding: const EdgeInsets.symmetric(horizontal: 14, vertical: 9),
        decoration: BoxDecoration(
          color: const Color(0xE6161B24),
          borderRadius: BorderRadius.circular(999),
          border: Border.all(color: const Color(0x1FFFFFFF)),
        ),
        child: Row(
          mainAxisSize: MainAxisSize.min,
          children: [
            const SizedBox(
              width: 14,
              height: 14,
              child: CircularProgressIndicator(
                strokeWidth: 2,
                color: Color(0xFFF5A623),
              ),
            ),
            const SizedBox(width: 10),
            KeepAllText(
              widget.label,
              style: const TextStyle(color: Color(0xDDFFFFFF), fontSize: 13),
            ),
          ],
        ),
      ),
    ),
  );
}

/// Keep a local fallback until separated native cursor state is ready. The
/// session keeps legacy baked cursors and explicit remote-hidden state distinct.
MouseCursor remoteViewCursor({
  required bool captured,
  required bool localCursorFallback,
}) => !captured
    ? SystemMouseCursors.click
    : localCursorFallback
    ? SystemMouseCursors.basic
    : SystemMouseCursors.none;

class RemoteView extends StatefulWidget {
  const RemoteView({
    super.key,
    required this.machine,
    required this.load,
    this.onReconnect,
    this.onConfigureDisplays,
    this.initialOptions = StreamOptions.recommended,
    this.initialDisplayMode = SessionDisplayMode.fullscreen,
    this.pointerElapsed,
    this.windowActions,
    this.prepareDisplaysOnEntry = false,
    this.onConnectTracks,
    this.onConnectViewport,
    this.onConnectScaled,
    this.onConnectTracksScaled,
    this.onRefreshDesktop,
    this.onCheckTakeover,
    this.onWaitHostReady,
    this.onPrepareConsole,
    this.onOpenConsole,
    this.onTakeover,
    this.onConfirmTakeover,
    this.takeoverServer,
    this.pointerMemory,
    this.transferNoticeWindow,
    this.pasteTargetProbe,
    this.sessionGuideEnabled = true,
    this.guideAccountKey,
    this.relativeMouseNative,
    this.usbGameMouseNative,
    this.coachStore,
  });
  final DesktopMachine machine;

  /// Where "the bar's first-connection hint was shown" is remembered (a test
  /// supplies one; session windows use this PC's guide store, other views
  /// never show the hint).
  @visibleForTesting
  final SessionGuideStore? coachStore;

  /// The runner's side of the 마우스 잠금 virtual USB mouse (a test supplies a
  /// fake; session windows on Windows use the channel).
  @visibleForTesting
  final UsbGameMouseNative? usbGameMouseNative;

  /// The runner's mouse capture (a test supplies a fake; session windows on
  /// Windows use the channel, other views none).
  @visibleForTesting
  final RelativeMouseNative? relativeMouseNative;

  /// The card window and taskbar progress of clipboard transfers (a test supplies a fake;
  /// Windows uses the runner's, other platforms none).
  @visibleForTesting
  final TransferNoticeWindow? transferNoticeWindow;

  /// Where a paste on this PC goes (a test supplies a fake; Windows asks the runner).
  @visibleForTesting
  final PasteTargetProbe? pasteTargetProbe;
  final bool sessionGuideEnabled;
  final String? guideAccountKey;

  /// Ends the VM's existing connection elsewhere (desktop_busy recovery).
  final Future<void> Function()? onTakeover;
  final Future<void> Function(
    StreamTakeoverOffer offer,
    bool Function() authorized,
  )?
  onConfirmTakeover;
  final String? takeoverServer;

  /// This PC's local memory (session windows only): an earlier client's
  /// "마우스 가두기" choice is dropped from it, never read.
  final LauncherMemory? pointerMemory;
  final Future<RemoteSession> Function() load;
  final Future<RemoteSession> Function(StreamOptions options)? onReconnect;
  final Future<DesktopMachine> Function(int count)? onConfigureDisplays;
  final Future<DesktopMachine> Function()? onRefreshDesktop;

  /// Read-only admission context before any display or console preparation.
  final Future<void> Function()? onCheckTakeover;

  /// host-ready-wait-v1: waits on the broker for this desktop's Host after its
  /// stream ended (null: the broker does not offer it).
  final Future<bool?> Function()? onWaitHostReady;
  final Future<DesktopMachine> Function()? onPrepareConsole;

  /// Asks the launcher to open this VM's Console WebView (never an in-window
  /// console): offered on the end card where it helps.
  final Future<void> Function()? onOpenConsole;
  final StreamOptions initialOptions;
  final SessionDisplayMode initialDisplayMode;
  @visibleForTesting
  final Duration Function()? pointerElapsed;
  final SessionWindowActions? windowActions;
  final bool prepareDisplaysOnEntry;
  final Future<RemoteSession> Function(TrackPlan plan, StreamOptions options)?
  onConnectTracks;
  final Future<RemoteSession> Function(
    InitialViewport viewport,
    StreamOptions options,
  )?
  onConnectViewport;

  /// display-scale-v1 negotiation: a fresh window starts at 100%, then
  /// reconnects with the scale explicitly chosen in this window.
  final Future<RemoteSession> Function(
    InitialViewport? viewport,
    StreamOptions options,
    int scale,
  )?
  onConnectScaled;

  /// The same for independent outputs (display-scale-outputs-v1): the scale
  /// chosen in this window goes to every owned output again on reconnect.
  final Future<RemoteSession> Function(
    TrackPlan plan,
    StreamOptions options,
    int scale,
  )?
  onConnectTracksScaled;
  @override
  State<RemoteView> createState() => _RemoteViewState();
}

class _PendingRemotePress {
  _PendingRemotePress(this.session, this.revision, this.down);
  final RemoteSession session;
  final int revision;
  final PointerDownEvent down;
  PointerUpEvent? up;
}

class _RemoteViewState extends State<RemoteView> with WidgetsBindingObserver {
  final _focus = FocusNode();
  final _presentation = WindowPresentation();
  RemoteSession? _session;
  String? _error;
  int _buttons = 0;
  int _pointerRevision = 0;
  _PendingRemotePress? _pendingPress;
  RemoteSession? _initialKeyboardFor;
  ({RemoteSession session, int revision})? _initialKeyboardQueued;
  Timer? _initialKeyboardRetry;
  // A 16 ms window added ~9 ms on average to drags and map pans. 4 ms keeps
  // continuous moves at <=250/s, well inside the relay's 2000/s input limit.
  late final _pointerMoves = PointerMoveCoalescer(
    send: _sendPointerMove,
    interval: const Duration(milliseconds: 4),
    elapsed: widget.pointerElapsed,
  );
  RemoteSession? _pointerSession;
  int? _pointerTexture;
  VoidCallback? _unbindPointerOrdering;
  final RemoteViewMode _viewMode = RemoteViewMode.fitNoUpscale;
  bool _reconnecting = false;
  bool _presentationReady = false;
  bool _entryDisplaysPrepared = false;
  late SessionDisplayMode _displayMode = widget.initialDisplayMode;
  LocalDisplayLayout? _localLayout;
  bool _localLayoutVerified = false;
  // This window's physical size and pixel ratio when the layout was verified.
  (Size, double)? _verifiedMetrics;
  int _localLayoutObservation = 0;
  Timer? _groupLayoutDebounce;
  bool _groupLayoutChanging = false, _groupLayoutNeeded = false;
  bool _consoleDialogOpen = false;
  int _displayRevision = 0;
  String? _pointerTrack;
  ({String? track, double x, double y})? _lastPointerPoint;
  bool _presentationChanging = false;
  bool _allMonitorsPreflight = false;
  ScreenTransitionStage? _screenTransitionStage;
  String _screenTransitionTarget = '';
  bool _screenTransitionCancelled = false;
  bool? _allMonitorsAvailable;
  int _localDisplayProbe = 0;
  final _viewportKey = GlobalKey();
  bool _retryAllowed = true, _settingsOpen = false;
  String? _requestErrorCode;
  ConnectionWaitStep _waitStep = ConnectionWaitStep.request;
  final _waitClock = Stopwatch();
  bool _takeoverAllowed = false;
  StreamTakeoverOffer? _takeoverOffer;
  bool _takeoverRunning = false;
  int _connectionAttempt = 0;
  // One budget per explicit user intent, not per _load or RemoteSession.
  int _connectionIntent = 0;

  /// A broker update restarting the server: a connect waits through it.
  final _serverRestart = ServerRestartWait();
  Timer? _serverRestartRetry;
  bool _intentHasStreamed = false, _initialPreparationRetryUsed = false;
  RemoteSession? _initialPreparationSource;
  bool get _initialPreparationPending => _initialPreparationSource != null;
  bool _closingWindow = false, _confirmingClose = false;
  Future<void>? _terminalDrain, _terminalRetirement;
  ({
    RemoteSession? session,
    SessionWindowActions? actions,
    Route<dynamic>? route,
    NavigatorState navigator,
    String desktopId,
    Object token,
  })?
  _terminalOwner;
  PopupRoute<bool>? _guideRoute;
  bool _manualGuideOpen = false;
  int? _scalePreference = 100;
  RemoteSession? _releaseInputFor;
  Future<void>? _releaseInputPending;
  final _releaseDrainGates = <Timer, Completer<void>>{};
  ({RemoteSession session, Object token, bool preserveInitial})?
  _releaseInputContext;
  int _captureIntentRevision = 0;
  static const _autoViewport = true;
  bool _viewportChanging = false;
  ({RemoteSession session, Object token, int intent})? _viewportInputContext;
  bool _viewportOutsideBounds = false;
  Timer? _viewportDebounce;

  /// A size change is asked for once it settles: a burst (fullscreen, snap,
  /// DPI change) asks once for its final size; a border drag asks when it
  /// ends, or after a long pause should its end never be reported.
  static const viewportSettle = Duration(milliseconds: 250),
      viewportDragEndSettle = Duration(milliseconds: 100),
      viewportDragPauseSettle = Duration(milliseconds: 1500);
  (int, int)? _desiredViewport, _attemptedViewport;
  (int, int)? _viewportAttemptTarget;
  int _viewportAttempts = 0;
  InitialViewportEntry? _initialViewportEntry;
  Timer? _initialViewportDeadline;
  bool _initialViewportMetricsPending = false;

  void _clearInitialViewportEntry() {
    _initialViewportEntry = null;
    _initialViewportMetricsPending = false;
    _initialViewportDeadline?.cancel();
    _initialViewportDeadline = null;
  }

  void _failInitialViewportEntry() {
    if (!mounted || _initialViewportEntry == null || _closingWindow) return;
    final session = _session;
    _clearInitialViewportEntry();
    _viewportDebounce?.cancel();
    _release();
    setState(() {
      _screenTransitionStage = null;
      _error = session?.resizeError ?? l10n.couldNotPrepareScreenFit;
      _requestErrorCode = 'display_configuration_failed';
      _retryAllowed = true;
    });
    // Existing session teardown retains Host display restore ownership.
    if (session != null) unawaited(session.close());
  }

  final _pictureKey = GlobalKey(), _toolbarKey = GlobalKey();

  /// A press that began on the remote picture is still held (any button).
  /// Flutter keeps routing its moves and release to the picture wherever the
  /// pointer goes, so the toolbar must not take hover or hits meanwhile: a
  /// drag across it would reach the toolbar's hover, which releases remote
  /// input and drops the drag. Ends only on up or cancel.
  final _pictureHeld = ValueNotifier<bool>(false);
  int? _heldPointer;

  void _pressPicture(PointerDownEvent event) {
    _heldPointer = event.pointer;
    _pictureHeld.value = true;
  }

  // Every pointer event passes here, so the held state ends with its press
  // even if the picture was rebuilt during the drag.
  void _globalPointer(PointerEvent event) {
    if ((event is PointerUpEvent || event is PointerCancelEvent) &&
        event.pointer == _heldPointer) {
      _heldPointer = null;
      _pictureHeld.value = false;
    }
  }

  bool _pointerSyncScheduled = false;
  bool _routeCurrent = true;
  int _pointerHolds = 0;
  Rect? _toolbarBounds;
  String? _toast;
  bool _toastShare = false;
  IconData _toastIcon = Icons.mouse_outlined;
  Timer? _toastTimer;

  bool get _pointerControl => widget.windowActions != null;

  // relative-mouse-v1: with "마우스 잠금" on, a click into the picture hands the
  // mouse to the VM: this window holds the pointer in the picture and the
  // runner sends movement (data/relative_mouse.dart).
  late final RelativeMouseControl? _relativeMouse = _pointerControl
      ? RelativeMouseControl(
          native: widget.relativeMouseNative ?? ChannelRelativeMouse(),
          picture: _pictureRect,
          // cursor-echo-v1: the pointer shown is the VM's own cursor, held on
          // each position the Host echoes (an older Host echoes none: no pin).
          pinned: () => _session?.cursorEcho == true,
          // cursor-compose-v1 (preferred): no cursor here, the Host draws the
          // VM's cursor into the video, so cursor and game move together.
          composed: () => _session?.cursorCompose == true,
          onChanged: _relativeMouseChanged,
          onEngaged: _relativeMouseEngaged,
          onToggle: () => _toggleGameMouse(shortcut: true),
        )
      : null;
  bool _pictureHovered = false;
  bool get _relativeCapturing => _relativeMouse?.capturing == true;

  /// The picture in logical pixels of the window: where 마우스 잠금 holds the
  /// pointer (the bar is not part of it; it is reached after the release key).
  Rect? _pictureRect() {
    final picture = _pictureKey.currentContext?.findRenderObject();
    if (picture is! RenderBox || !picture.attached || !picture.hasSize) {
      return null;
    }
    return picture.localToGlobal(Offset.zero) & picture.size;
  }

  // The user's choice: off keeps ordinary positions even when a remote
  // application hides the cursor. Changes need no reconnect. Every new
  // connection starts off and nothing is remembered.
  bool _gameMouse = false;
  int _gameMouseRevision = 0;

  /// The session can do it here: granted, no RDP around this client, and a
  /// window that can hold the mouse. Spread over every monitor (independent
  /// outputs) only with the cursor in the video: the pointer stays parked on
  /// the monitor it was handed over on, and the VM's cursor shows in the
  /// output it is over (relative-mouse-tracks-v1).
  bool get _relativeMouseOffered {
    final session = _session;
    return session != null &&
        session.relativeMouse &&
        session.clientRemoteSession == false &&
        !session.closed &&
        (_displayMode != SessionDisplayMode.allMonitors ||
            session.cursorCompose);
  }

  /// Said when 마우스 잠금 is turned on, in a dialog with one 확인: how the
  /// mouse goes to the VM and how it comes back. Its 확인 hands the mouse
  /// over at once; Esc closes it without that.
  static String get gameMouseDialogTitle => l10n.mouseLock;
  static String get gameMouseDialogMessage =>
      l10n.pressOkLockMouseInside;
  bool _gameMouseDialogOpen = false;

  void _toggleGameMouse({bool shortcut = false}) {
    final enabled = !_gameMouse;
    if (enabled && (!_relativeMouseOffered || _gameMouseDialogOpen)) return;
    _gameMouseRevision++;
    if (!enabled) _relativeMouse?.release();
    setState(() => _gameMouse = enabled);
    _syncRelativeMouse();
    if (enabled) {
      // The intentional shortcut is the action itself. The existing entry
      // path still checks the session and USB grant; its engagement notice
      // explains how to release without another click or modal confirmation.
      unawaited(shortcut ? _handOverGameMouse() : _explainGameMouse());
    } else {
      _showToast(l10n.mouseLockOff);
    }
  }

  /// The click that closes this dialog lands on the dialog, never on the
  /// picture. Its 확인 then hands the mouse to the VM without another click
  /// into the picture.
  Future<void> _explainGameMouse() async {
    if (_gameMouseDialogOpen || !mounted) return;
    _gameMouseDialogOpen = true;
    _release();
    _holdPointer();
    final session = _session, revision = _gameMouseRevision;
    bool current() =>
        mounted &&
        !_closingWindow &&
        _gameMouse &&
        identical(session, _session) &&
        revision == _gameMouseRevision;
    var confirmed = false;
    try {
      confirmed = await showAppNotice(
        context,
        icon: Icons.mouse_outlined,
        tone: AppTone.accent,
        title: gameMouseDialogTitle,
        message: gameMouseDialogMessage,
        dialogKey: const Key('game-mouse-dialog'),
        closeKey: const Key('game-mouse-dialog-ok'),
      );
    } finally {
      _gameMouseDialogOpen = false;
      _unholdPointer();
    }
    if (!confirmed || !current()) return;
    // ModalRoute.isCurrent is refreshed by the next build after the notice
    // pops. The shortcut path does not show this route and stays immediate.
    await WidgetsBinding.instance.endOfFrame;
    if (!current() || !_captureAllowed || !_relativeMouseOffered) return;
    await _handOverGameMouse();
  }

  /// What a click into the picture does, without the click: the picture
  /// takes input again, then the mouse goes to the VM (held in the picture,
  /// or on the VM's cursor where the Host echoes it).
  Future<void> _handOverGameMouse() async {
    final session = _session;
    final revision = _gameMouseRevision;
    if (!mounted || !_gameMouse || !_relativeMouseOffered || session == null) {
      return;
    }
    // The virtual USB mouse (what League of Legends reads) starts attaching
    // when the option goes on. A hand-over before the VM confirms it would
    // use injected movement for the whole capture: wait for it, at most 4 s.
    for (
      var waited = 0;
      waited < 40 && _gameMouseUsb?.attaching == true;
      waited++
    ) {
      await Future<void>.delayed(const Duration(milliseconds: 100));
      if (!mounted ||
          !_gameMouse ||
          revision != _gameMouseRevision ||
          !identical(session, _session)) {
        return;
      }
    }
    if (!session.captured || !_focus.hasFocus) await _capture();
    if (!mounted ||
        !_gameMouse ||
        revision != _gameMouseRevision ||
        !identical(session, _session)) {
      return;
    }
    _syncRelativeMouse();
    _relativeMouse?.handOver();
  }

  // 마우스 잠금 over USB: while the option is on, the session's usb-v1 channel
  // lends the VM the virtual mouse a capture can then feed (usb_game_mouse.dart).
  UsbGameMouse? _gameMouseUsb;
  void _syncGameMouseUsb(bool available) {
    final usb = _pointerControl ? _session?.usb : null;
    if (!identical(_gameMouseUsb?.usb, usb)) {
      _gameMouseUsb
        ?..removeListener(_gameMouseUsbChanged)
        ..dispose();
      _gameMouseUsb = usb == null
          ? null
          : (UsbGameMouse(usb: usb, native: widget.usbGameMouseNative)
              ..addListener(_gameMouseUsbChanged));
    }
    _gameMouseUsb?.enabled = available;
  }

  void _gameMouseUsbChanged() {
    if (mounted) setState(() {});
  }

  void _syncRelativeMouse() {
    // The picture may have moved in this frame (마우스 잠금 follows it).
    if (_relativeMouse?.engaged == true) {
      WidgetsBinding.instance.addPostFrameCallback((_) {
        if (mounted) _relativeMouse?.pictureMoved();
      });
    }
    final session = _session;
    final available = _gameMouse && _relativeMouseOffered;
    _syncGameMouseUsb(available);
    _relativeMouse?.update(
      available: available,
      hidden: available && session!.remoteCursorHidden,
      owned:
          available &&
          session!.captured &&
          !session.resizing &&
          _focus.hasFocus &&
          _pointerFree,
      over: _pictureHovered,
      paused: available && session!.resizing,
      shortcutAvailable:
          _relativeMouseOffered &&
          session!.captured &&
          !session.resizing &&
          _focus.hasFocus &&
          _pointerFree,
    );
  }

  void _relativeMouseChanged() {
    if (_relativeCapturing) _pointerMoves.discard();
    // cursor-echo-v1: the Host echoes the VM's cursor exactly while a pinned
    // capture holds the mouse (nothing is sent where it was not granted).
    _session?.echoCursor(_relativeMouse?.pinnedEngaged == true);
    // cursor-compose-v1: likewise, the Host composes exactly while a
    // concealed capture holds the mouse.
    _session?.composeCursor(_relativeMouse?.composedEngaged == true);
    // No lossless tiles while the mouse is the VM's: they
    // add nothing to a game and cost bandwidth. They come back on release.
    _session?.pauseLosslessTiles(_relativeMouse?.engaged == true);
    _syncPointer();
    if (mounted) setState(() {});
  }

  /// Said at every hand-over, briefly: the mouse is the VM's until the key.
  static String get gameMouseNotice => l10n.mouseLockedInsideDesktopCtrl;

  /// Said once per session after a hand-over without the virtual USB mouse:
  /// injected movement, which some games (League of Legends) ignore.
  static String get gameMouseInjectedNotice => l10n.someGamesSuchAsLeague;
  RemoteSession? _injectedNoticed;
  Timer? _injectedNoticeTimer;

  void _relativeMouseEngaged() {
    const shown = Duration(milliseconds: 2500);
    _showToast(gameMouseNotice, duration: shown);
    final session = _session;
    if (_relativeMouse?.usbPath == true ||
        session == null ||
        identical(session, _injectedNoticed)) {
      return;
    }
    _injectedNoticed = session;
    _injectedNoticeTimer?.cancel();
    _injectedNoticeTimer = Timer(shown, () {
      if (mounted && identical(_session, session)) {
        _showToast(
          gameMouseInjectedNotice,
          duration: const Duration(seconds: 4),
        );
      }
    });
  }

  void _hoverPicture(bool over) {
    _pictureHovered = over;
    _syncRelativeMouse();
  }

  /// The bar's open menus and its settings panel, closed together when what
  /// they show no longer applies (see [_toolbarContent], [_toolbarLayout]).
  final _popups = SessionPopups();

  /// Times this window lost focus or was minimized: open menus close, like
  /// Windows menus on deactivation, and an unpinned bar folds.
  int _focusLosses = 0;

  /// The connection is up: its first picture arrived and it has not ended.
  /// A remote resolution change (texture swap) still counts as connected.
  bool get _connected => _live && _session!.frameReady;

  bool get _ended {
    final session = _session;
    return _error != null ||
        _displayDisconnected ||
        session?.closed == true ||
        session?.error != null;
  }

  /// What the bar's menus read when they open: this connection, whether it
  /// is up, and its remote resolution(s). A change closes them.
  Object _toolbarContent() {
    final session = _session;
    if (session == null) return (null, _ended);
    final frame = session.independentOutputs
        ? [
            for (final track in session.tracks)
              '${track.id}:${track.width}x${track.height}',
          ].join(',')
        : '${session.width}x${session.height}/'
              '${session.sourceWidth}x${session.sourceHeight}';
    return (session, _connected, _ended, session.resizing, frame);
  }

  /// Where and when the bar is used: the screen mode and local monitors, the
  /// connection attempt and its end, the window's focus. A change closes
  /// open menus and folds an unpinned bar.
  Object _toolbarLayout() => (
    _shownMode,
    _presentation.displayRevision,
    _connectionAttempt,
    _ended,
    _focusLosses,
  );

  /// There is no "마우스 가두기": 마우스 잠금 is the one lock. An earlier
  /// client's saved choice is dropped, never read.
  Future<void> _forgetPointerConfinement() async {
    try {
      await widget.pointerMemory?.forgetPointerConfinement();
    } catch (_) {
      // Never interrupt the session over a leftover local file.
    }
  }

  /// Menus and dialogs take the mouse back before they open; 마우스 잠금 may
  /// hold it again after the last one closes.
  void _holdPointer() {
    _pointerHolds++;
    _syncPointer();
  }

  void _unholdPointer() {
    _pointerHolds = math.max(0, _pointerHolds - 1);
    _syncPointer();
  }

  void _pointerMenuChanged(bool open) {
    if (open) {
      // The bar is in use: its first-connection hint has done its part.
      _dismissCoach();
      _holdPointer();
    } else {
      _unholdPointer();
    }
  }

  /// Nothing holds the pointer back: no menu, dialog, transition or failure.
  bool get _pointerFree {
    final session = _session;
    return _pointerControl &&
        _pointerHolds == 0 &&
        !_transferBlocked &&
        _routeCurrent &&
        !_closingWindow &&
        !_confirmingClose &&
        !_reconnecting &&
        _initialViewportEntry == null &&
        _screenTransitionStage == null &&
        !_settingsOpen &&
        !_consoleDialogOpen &&
        !_manualGuideOpen &&
        !_displayDisconnected &&
        _error == null &&
        session != null &&
        !session.closed &&
        session.error == null &&
        session.frameReady &&
        session.textureId != null;
  }

  /// 마우스 잠금 follows the state after layout, where the picture is known.
  void _syncPointer() {
    if (!_pointerControl || _pointerSyncScheduled || !mounted) return;
    _pointerSyncScheduled = true;
    WidgetsBinding.instance.addPostFrameCallback((_) {
      _pointerSyncScheduled = false;
      if (!mounted) return;
      _syncRelativeMouse();
    });
    WidgetsBinding.instance.ensureVisualUpdate();
  }

  /// cursor-v1: after layout, tell the native cursor presenter where the
  /// picture is (logical pixels of the Flutter view) and the remote source
  /// size, so it can scale the remote shape and map a remote warp. The
  /// The captured picture asks for `none` only when cursor state is ready;
  /// otherwise its local fallback stays visible until the session notifies us.
  bool _cursorViewScheduled = false;
  void _syncCursorView() {
    final session = _session;
    if (_cursorViewScheduled ||
        !mounted ||
        session == null ||
        !session.cursorSeparated ||
        session.closed) {
      return;
    }
    _cursorViewScheduled = true;
    WidgetsBinding.instance.addPostFrameCallback((_) {
      _cursorViewScheduled = false;
      if (!mounted || !identical(session, _session) || session.closed) return;
      final picture = _pictureKey.currentContext?.findRenderObject();
      final sourceWidth = session.sourceWidth ?? session.width;
      final sourceHeight = session.sourceHeight ?? session.height;
      if (picture is! RenderBox ||
          !picture.attached ||
          !picture.hasSize ||
          sourceWidth <= 0 ||
          sourceHeight <= 0) {
        return;
      }
      var area = Offset.zero & picture.size;
      // cursor-tracks-v1: the picture holds every output; report the primary
      // output's surface (its source is the session's source size). Each
      // output is shown 1:1 on its own monitor (matchesSources), so the shape
      // scale taken from it holds on both monitors.
      if (session.independentOutputs) {
        final layout = _localLayout, tracks = session.tracks;
        final primary =
            tracks.where((t) => t.primary).firstOrNull ?? tracks.firstOrNull;
        final surface = layout?.surfaces
            .where((s) => s.id == primary?.id)
            .firstOrNull;
        if (!_localLayoutVerified || layout == null || surface == null) return;
        final ratio = MediaQuery.devicePixelRatioOf(context);
        area = Rect.fromLTWH(
          (surface.bounds.left - layout.bounds.left) / ratio,
          (surface.bounds.top - layout.bounds.top) / ratio,
          surface.bounds.width / ratio,
          surface.bounds.height / ratio,
        );
      }
      final origin = picture.localToGlobal(area.topLeft);
      unawaited(
        session.cursorView(
          CursorView(
            left: origin.dx,
            top: origin.dy,
            width: area.width,
            height: area.height,
            sourceWidth: sourceWidth,
            sourceHeight: sourceHeight,
          ),
        ),
      );
    });
  }

  /// [share]: one of the notices about this connection's shared folders and
  /// USB devices (ended by itself, ended by a reconnect, connected again).
  /// Those that come at the same moment are shown together, one line each;
  /// any other toast replaces what is shown.
  void _showToast(
    String message, {
    Duration duration = const Duration(milliseconds: 1800),
    IconData icon = Icons.mouse_outlined,
    bool share = false,
  }) {
    if (!mounted) return;
    _toastTimer?.cancel();
    final shown = _toast;
    final joined = share && _toastShare && shown != null;
    setState(() {
      _toast = !joined
          ? message
          : shown.split('\n').contains(message)
          ? shown
          : '$shown\n$message';
      _toastIcon = joined && _toastIcon != icon
          ? Icons.info_outline_rounded
          : icon;
      _toastShare = share;
    });
    _toastTimer = Timer(duration, () {
      if (mounted) {
        setState(() {
          _toast = null;
          _toastShare = false;
        });
      }
    });
  }

  /// Whether this PC has the two monitors "모든 모니터 사용" needs. Measured
  /// without the cover check (the window covers them only in that mode); an
  /// unanswered probe never hides an item that was offered before.
  Future<void> _probeLocalDisplays() async {
    if (widget.onConnectTracks == null ||
        !_machine.independentDisplayStreaming) {
      return;
    }
    final probe = ++_localDisplayProbe;
    final available = await LocalDisplayLayout.twoMonitors();
    if (mounted &&
        !_closingWindow &&
        probe == _localDisplayProbe &&
        available != null &&
        available != _allMonitorsAvailable) {
      setState(() => _allMonitorsAvailable = available);
    }
  }

  /// "모든 모니터 사용" applies: the host can stream two separate outputs and
  /// this PC has two monitors (or the session is already using them).
  bool get _allMonitorsOffered =>
      widget.onConnectTracks != null &&
      _machine.independentDisplayStreaming &&
      (_displayMode == SessionDisplayMode.allMonitors ||
          _allMonitorsAvailable == true);

  /// A separate virtual-monitor count only where the session does not derive
  /// it from the screen mode. Session windows prepare one output for a window
  /// or full screen and two for 모든 모니터 on every (re)connection, so a
  /// manual count there would be undone by the next reconnect.
  bool get _virtualDisplayCountOffered =>
      !widget.prepareDisplaysOnEntry &&
      widget.onConfigureDisplays != null &&
      _machine.virtualMonitorControl &&
      _machine.display.supported;

  Future<void> _useAllMonitors() =>
      _transitionDisplayMode(SessionDisplayMode.allMonitors);

  void _transitionStage(ScreenTransitionStage stage) {
    if (mounted && _screenTransitionStage != null) {
      setState(() => _screenTransitionStage = stage);
    }
  }

  void _cancelScreenTransition() {
    // Only the local preflight can return to the still-live old session.
    // A native call already in flight must finish before its mode is restored.
    if (!_allMonitorsPreflight || _screenTransitionCancelled) return;
    setState(() => _screenTransitionCancelled = true);
    _release();
  }

  Future<void> _transitionDisplayMode(SessionDisplayMode target) async {
    if (_transferBlocked ||
        _presentationChanging ||
        _screenTransitionStage != null ||
        _closingWindow ||
        _reconnecting) {
      return;
    }
    if (_displayMode == target) {
      _scheduleGroupLayout();
      return;
    }
    final dual = target == SessionDisplayMode.allMonitors;
    final attempt = _connectionAttempt, probe = ++_localDisplayProbe;
    final previousFullscreen = _presentation.fullscreen;
    final previousDual = _displayMode == SessionDisplayMode.allMonitors;
    var presentationAttempted = false, committed = false;
    bool owned() => mounted && !_closingWindow && attempt == _connectionAttempt;
    bool current() => owned() && !_screenTransitionCancelled;
    setState(() {
      _presentationChanging = true;
      _allMonitorsPreflight = true;
      _screenTransitionCancelled = false;
      _screenTransitionTarget = dual
          ? l10n.allMonitors
          : target == SessionDisplayMode.fullscreen
          ? l10n.fullScreen
          : l10n.windowed;
      _screenTransitionStage = dual
          ? ScreenTransitionStage.localCheck
          : ScreenTransitionStage.windowMode;
    });
    _viewportDebounce?.cancel();
    _groupLayoutDebounce?.cancel();
    _release();
    try {
      if (dual && widget.onConnectTracks == null) {
        throw const BrokerException('track_streaming_unavailable');
      }
      if (dual) {
        await LocalDisplayLayout.read(covered: false);
        if (!current() || _transferBlocked) return;
        if (probe != _localDisplayProbe) {
          throw const BrokerException('local_display_layout_unavailable');
        }
      }
      // Prove the actual local window/monitor layout before retiring a working
      // stream in BOTH directions. Failure leaves that exact stream alive.
      _transitionStage(ScreenTransitionStage.windowMode);
      presentationAttempted = true;
      final presentation = _presentationReady
          ? _presentation.set(
              target != SessionDisplayMode.windowed,
              allMonitors: dual,
            )
          : _presentation.enter(
              fullscreen: target != SessionDisplayMode.windowed,
              allMonitors: dual,
            );
      await presentation.timeout(const Duration(seconds: 5));
      if (!current() || _transferBlocked) return;
      if (_presentation.fullscreen != (target != SessionDisplayMode.windowed)) {
        throw StateError('Presentation was not applied');
      }
      await WidgetsBinding.instance.endOfFrame;
      if (!current() || _transferBlocked) return;
      if (dual) {
        await LocalDisplayLayout.read();
        if (!current() || _transferBlocked) return;
        _allMonitorsAvailable = true;
      }
      _displayMode = target;
      _presentationReady = true;
      _entryDisplaysPrepared = false;
      if (!dual) {
        _localLayout = null;
        _localLayoutVerified = false;
        _lastOptions = _lastOptions.forSingleDisplay().withCodecCeiling();
      }
      committed = true;
      _allMonitorsPreflight = false;
      await _load(reconnect: true, newIntent: true);
    } catch (error) {
      if (mounted && current()) {
        if (error is BrokerException &&
            error.code == 'local_display_layout_unsupported') {
          _allMonitorsAvailable = false;
        }
        ScaffoldMessenger.of(context).showSnackBar(
          SnackBar(
            content: KeepAllText(
              error is BrokerException
                  ? error.message
                  : l10n.couldNotApplyScreenMode,
            ),
          ),
        );
      }
    } finally {
      if (!committed && presentationAttempted && owned()) {
        _transitionStage(ScreenTransitionStage.restoring);
        try {
          await _presentation
              .set(previousFullscreen, allMonitors: previousDual)
              .timeout(const Duration(seconds: 5));
        } catch (_) {
          if (mounted && owned()) {
            ScaffoldMessenger.of(context).showSnackBar(
              SnackBar(
                content: KeepAllText(
                  l10n.couldNotConfirmOriginalScreen,
                ),
              ),
            );
          }
        }
      }
      _allMonitorsPreflight = false;
      if (mounted) {
        setState(() {
          _presentationChanging = false;
          if (!committed) _screenTransitionStage = null;
        });
        if (!committed && previousDual && owned()) _scheduleGroupLayout();
      }
    }
  }

  Future<void> _setDisplayMode(bool fullscreen) async {
    if (_transferBlocked) return;
    if (!mounted ||
        _presentationChanging ||
        _screenTransitionStage != null ||
        _closingWindow ||
        _reconnecting) {
      return;
    }
    if (_displayMode == SessionDisplayMode.allMonitors) {
      await _transitionDisplayMode(
        fullscreen
            ? SessionDisplayMode.fullscreen
            : SessionDisplayMode.windowed,
      );
      return;
    }
    setState(() => _presentationChanging = true);
    _release();
    try {
      await _presentation.set(fullscreen).timeout(const Duration(seconds: 5));
      if (!mounted || _closingWindow) return;
      if (_presentation.fullscreen != fullscreen) {
        throw StateError('Presentation was not applied');
      }
      _displayMode = fullscreen
          ? SessionDisplayMode.fullscreen
          : SessionDisplayMode.windowed;
      // Explicit mode selection retries a previously refused size. Wait for
      // native WM_SIZE/DPI metrics; do not resize to the old window's bounds.
      await WidgetsBinding.instance.endOfFrame;
      if (!mounted || _closingWindow) return;
      final viewport = _viewportKey.currentContext;
      final box = viewport?.findRenderObject();
      if (viewport != null &&
          viewport.mounted &&
          box is RenderBox &&
          box.hasSize) {
        _viewportAttempts = 0;
        _attemptedViewport = null;
        _observeViewport(box.size, MediaQuery.devicePixelRatioOf(viewport));
      }
    } catch (_) {
      if (mounted && !_closingWindow) {
        ScaffoldMessenger.of(context).showSnackBar(
          SnackBar(content: KeepAllText(l10n.couldNotChangeScreenMode)),
        );
      }
    } finally {
      // The bar's settings wait for this; rebuild so they come back at once.
      if (mounted) {
        setState(() => _presentationChanging = false);
      } else {
        _presentationChanging = false;
      }
    }
  }

  void _observeViewport(Size logical, double pixelRatio) {
    final entry = _initialViewportEntry;
    if (entry != null) {
      final pixels = viewportResolution(
        logical,
        pixelRatio,
        portrait: _machine.streamSize && _machine.viewportEncoderLimits,
      );
      if (pixels != null) {
        entry.observe((pixels.width.toInt(), pixels.height.toInt()));
        if (entry.changed &&
            _session?.frameReady == true &&
            _initialViewportDeadline == null) {
          // Start only after the old source actually decodes: the initial
          // Host hardware preparation keeps its own longer watchdog.
          // Repeated target changes cannot replenish this corrective budget.
          _initialViewportDeadline = Timer(
            const Duration(seconds: 45),
            _failInitialViewportEntry,
          );
        }
      }
    }
    final session = _session;
    if (_allMonitorsPreflight ||
        !_autoViewport ||
        session == null ||
        session.closed ||
        !session.canResizeViewport) {
      return;
    }
    final pixels = viewportResolution(
      logical,
      pixelRatio,
      portrait: session.streamSize,
    );
    final outside = pixels == null;
    if (outside != _viewportOutsideBounds) {
      _viewportOutsideBounds = outside;
      WidgetsBinding.instance.addPostFrameCallback((_) {
        if (mounted) setState(() {});
      });
    }
    if (pixels == null) {
      _desiredViewport = null;
      _viewportDebounce?.cancel();
      return;
    }
    final target = (pixels.width.toInt(), pixels.height.toInt());
    final sameDesired = target == _desiredViewport;
    _desiredViewport = target;
    if (target == _attemptedViewport) {
      _viewportDebounce?.cancel();
      return;
    }
    if (target == _viewportAttemptTarget && _viewportAttempts >= 2) return;
    if (sameDesired && _viewportDebounce?.isActive == true) return;
    _scheduleViewport(
      _presentation.sizeMoving ? viewportDragPauseSettle : viewportSettle,
    );
  }

  void _scheduleViewport(Duration settle) {
    _viewportDebounce?.cancel();
    _viewportDebounce = Timer(settle, () {
      // The picture's scaling follows the settle (_followingSize).
      if (mounted) setState(() {});
      unawaited(_applyViewport());
    });
  }

  /// Windows' size/move loop: nothing is asked for while the border is
  /// dragged; its end asks for the final size at once.
  void _sizeMoveChanged(bool active) {
    if (!mounted || _desiredViewport == null) return;
    _scheduleViewport(active ? viewportDragPauseSettle : viewportDragEndSettle);
  }

  Future<void> _applyViewport() async {
    final session = _session, target = _desiredViewport;
    if (!mounted ||
        _error != null ||
        _transferBlocked ||
        _manualGuideOpen ||
        _allMonitorsPreflight ||
        !_autoViewport ||
        session == null ||
        session.closed ||
        session.textureId == null ||
        !session.canResizeViewport ||
        session.resizing ||
        _viewportChanging ||
        target == null ||
        target == _attemptedViewport) {
      return;
    }
    _viewportChanging = true;
    if (_viewportAttemptTarget != target) {
      _viewportAttemptTarget = target;
      _viewportAttempts = 0;
    }
    _viewportAttempts++;
    _attemptedViewport = null;
    final wasCaptured = session.captured && _focus.hasFocus;
    final restoreInputIntent = _captureIntentRevision;
    final viewportContext = (
      session: session,
      token: Object(),
      intent: restoreInputIntent,
    );
    _viewportInputContext = viewportContext;
    _release(preserveInitialKeyboard: true);
    var applied = false;
    try {
      applied = await session.resizeViewport(target.$1, target.$2);
      if (applied && identical(session, _session)) _attemptedViewport = target;
    } catch (_) {
      // Transport/rollback failure belongs to the session. Do not cache an
      // unacknowledged viewport or leak an asynchronous exception.
    } finally {
      if (identical(_viewportInputContext?.token, viewportContext.token)) {
        _viewportInputContext = null;
      }
      _viewportChanging = false;
      if (mounted && identical(session, _session)) {
        _queueInitialKeyboard(session);
      }
      if (!applied &&
          _initialViewportEntry != null &&
          identical(session, _session) &&
          !session.closed &&
          _viewportAttempts >= 2) {
        _failInitialViewportEntry();
      }
      // Input resumes by itself once the new picture is up; it used to wait
      // for a click into the picture after every window resize.
      if (applied &&
          wasCaptured &&
          restoreInputIntent == _captureIntentRevision &&
          mounted &&
          identical(session, _session) &&
          !session.closed &&
          !session.resizing &&
          WidgetsBinding.instance.lifecycleState == AppLifecycleState.resumed) {
        unawaited(_capture());
      }
      if (mounted &&
          identical(session, _session) &&
          _autoViewport &&
          _error == null &&
          !session.closed &&
          (_desiredViewport != target || (!applied && _viewportAttempts < 2))) {
        _scheduleViewport(
          _presentation.sizeMoving ? viewportDragPauseSettle : viewportSettle,
        );
      }
    }
  }

  /// A connection is up, so closing would end it.
  bool get _live {
    final session = _session;
    return session != null &&
        !session.closed &&
        session.error == null &&
        _error == null &&
        !_displayDisconnected;
  }

  Future<void> _confirmDisconnect() async {
    if (_confirmingClose || _closingWindow) return;
    if (_transferBlocked) {
      _showToast(l10n.filesBeingCopiedCancelCopy);
      return;
    }
    // Nothing live to end (failed, ended or still connecting): close at once
    // ("원격 창 닫기" on an ended connection does not ask again).
    if (!_live) return _closeView();
    _confirmingClose = true;
    _release();
    _holdPointer();
    bool? confirmed;
    try {
      final usb = _session?.usb, drive = _session?.drive;
      final printer = _session?.printer;
      confirmed = await confirmDisconnect(
        context,
        machine: _machine.name,
        usbShared: usb == null || usb.closed ? 0 : usb.sharedCount,
        foldersShared:
            drive?.shares
                .where((share) => share.state == DriveShareState.mapped)
                .length ??
            0,
        printsWaiting: printer?.pending.length ?? 0,
      );
    } finally {
      _confirmingClose = false;
      _unholdPointer();
    }
    if (mounted && confirmed == true && !_transferBlocked) await _closeView();
  }

  Future<void> _closeView() async {
    if (_closingWindow || _transferBlocked) return;
    _closingWindow = true;
    ++_connectionAttempt;
    _dismissSessionGuide();
    if (widget.windowActions == null) {
      if (mounted) Navigator.pop(context);
      return;
    }
    _viewportDebounce?.cancel();
    _release();
    _session?.removeListener(_changed);
    await _session?.close();
    if (mounted) {
      await widget.windowActions!.closeWindow();
      _closingWindow = false;
    }
  }

  /// Terminal access retirement is independent of ordinary transfer-protected
  /// disconnect. Join one exact old clipboard/native drain; never replay it.
  Future<void> _retireView() {
    final pending = _terminalRetirement;
    if (pending != null) return pending;
    if (!mounted) return Future<void>.value();
    final completed = Completer<void>();
    _terminalRetirement = completed.future;
    final owner = _terminalOwner ??= (
      session: _session,
      actions: widget.windowActions,
      route: ModalRoute.of(context),
      navigator: Navigator.of(context),
      desktopId: widget.machine.id,
      token: Object(),
    );
    if (_terminalDrain == null) {
      _closingWindow = true;
      ++_connectionAttempt;
      ++_connectionIntent;
      _dismissSessionGuide();
      _viewportDebounce?.cancel();
      _groupLayoutDebounce?.cancel();
      _serverRestartRetry?.cancel();
      _hostUpdateTimer?.cancel();
      _initialViewportDeadline?.cancel();
      _release();
      owner.session?.removeListener(_changed);
      final clipboard = owner.session?.clipboard;
      // Retain the full clipboard Future separately: RemoteSession.close's
      // best-effort2second wrapper does not prove native/file cleanup.
      final clipboardDrain = Future<void>.sync(() async {
        await clipboard?.close(reason: 'authorization_revoked');
      });
      final sessionDrain = Future<void>.sync(() async {
        await owner.session?.close();
      });
      _terminalDrain = Future.wait([clipboardDrain, sessionDrain])
          .then<void>((_) {});
    }
    unawaited(() async {
      try {
        await _terminalDrain!;
        if (!mounted ||
            !identical(owner.token, _terminalOwner?.token) ||
            !identical(owner.session, _session) ||
            !identical(owner.actions, widget.windowActions) ||
            widget.machine.id != owner.desktopId) {
          return;
        }
        final actions = owner.actions;
        if (actions != null) {
          await actions.closeWindow();
        } else {
          final route = owner.route;
          if (route != null &&
              route.isActive &&
              route.navigator == owner.navigator) {
            owner.navigator.removeRoute(route);
          }
        }
      } catch (_) {
        // Keep access retired and the child hidden. A failed/uncertain drain
        // stays failed; a later native recheck may not replay file mutations.
      } finally {
        if (identical(_terminalRetirement, completed.future)) {
          _terminalRetirement = null;
        }
        completed.complete();
      }
    }());
    return completed.future;
  }

  void _back() => unawaited(
    widget.windowActions == null ? _closeView() : _confirmDisconnect(),
  );

  late DesktopMachine _machine = widget.machine;
  bool _displayDisconnected = false, _displayConfigured = false;
  late StreamOptions _lastOptions = widget.initialOptions;

  /// Clipboard sharing has no command on the bar: copy and
  /// paste are ordinary Ctrl+C/Ctrl+V within the administrator's grant, and
  /// every supported Host (clipboard-sync-v1) shares
  /// automatically. What matters surfaces once: a transfer that takes a
  /// while (the transfer card: progress and 취소, in the window's corner and,
  /// while another window is in front, in a small card window of its own),
  /// a failed one, or sharing that stopped.
  ClipboardSession? _watchedClipboard;
  ClipboardTransferController? _transfer;
  bool _transferBlocked = false;
  bool _transferRebuildScheduled = false;
  bool _transferPreservesPaste = false;
  bool _transferNoticeVisible = false, _transferOverlayRaise = false;
  OverlayEntry? _transferOverlay;
  TransferNoticeWindow? _noticeWindow;
  bool _clipboardStopNoticed = false;

  /// The card window and the taskbar progress exist on Windows only (the
  /// Android card is the in-session one); a test supplies its own.
  TransferNoticeWindow? get _transferWindow => _noticeWindow ??=
      widget.transferNoticeWindow ??
      (NativeTransferNoticeWindow.supported
          ? NativeTransferNoticeWindow()
          : null);

  /// The connection whose clipboard was watched is gone (replaced, ended,
  /// or this view closes): its transfer card goes with it instead of
  /// standing on a later connection or on the launcher behind a closed
  /// in-window view.
  void _forgetClipboard({bool disposing = false}) {
    _watchedClipboard = null;
    _clipboardStopNoticed = false;
    final transfer = _transfer;
    _transfer = null;
    _transferBlocked = false;
    _transferPreservesPaste = false;
    _transferNoticeVisible = false;
    _session?.setClipboardTransferBlocked(false);
    _scheduleTransferRebuild();
    if (disposing) {
      _transferOverlay?.remove();
      _transferOverlay?.dispose();
      _transferOverlay = null;
    }
    if (transfer == null) return;
    transfer.removeListener(_transferChanged);
    if (disposing) {
      transfer.dispose();
      return;
    }
    // Not while this tree is building: the card listens to the controller.
    WidgetsBinding.instance.addPostFrameCallback((_) => transfer.dispose());
  }

  void _watchClipboard() {
    final clipboard = _session?.clipboard;
    if (!identical(clipboard, _watchedClipboard)) {
      _forgetClipboard();
      _watchedClipboard = clipboard;
      if (clipboard != null) {
        _transfer = ClipboardTransferController(
          clipboard: clipboard,
          window: _transferWindow,
          remoteName: _machine.name,
          pasteTargets:
              widget.pasteTargetProbe ??
              (NativeTransferNoticeWindow.supported
                  ? const NativePasteTargetProbe()
                  : null),
          dark: SessionChrome.of(context).dark,
          // A transfer that failed before it ever had a card: one toast says why.
          onFailureWithoutCard: (message) =>
              WidgetsBinding.instance.addPostFrameCallback(
                (_) => _showToast(
                  message,
                  icon: Icons.content_paste_off_rounded,
                  duration: const Duration(seconds: 5),
                ),
              ),
        )..addListener(_transferChanged);
        _transferChanged();
      }
    }
    _transferChanged();
    if (clipboard == null) return;
    // A paste the user attempted that could not be applied (the quiet
    // automatic copy of a folder, a locked file...): say why, once.
    final pasteNotice = clipboard.takePasteNotice();
    if (pasteNotice != null) {
      WidgetsBinding.instance.addPostFrameCallback(
        (_) => _showToast(
          pasteNotice,
          icon: Icons.content_paste_off_rounded,
          duration: const Duration(seconds: 6),
        ),
      );
    }
    // Not while the stream itself is away (session-resume-v1): the channel
    // falls with it, and a stream that ends is reconnected with a new channel.
    // The notice is for a channel that stays stopped in a live connection.
    if (_clipboardStopped(clipboard) &&
        !_clipboardStopNoticed &&
        _session?.clipboardRecovering != true &&
        _session?.resuming != true) {
      _clipboardStopNoticed = true;
      final notice = clipboardStopNotice(clipboard.error, clipboard.status);
      WidgetsBinding.instance.addPostFrameCallback(
        (_) => _showToast(
          notice,
          icon: Icons.content_paste_off_rounded,
          duration: const Duration(seconds: 6),
        ),
      );
    }
  }

  void _transferChanged() {
    final session = _session;
    final requested = _transfer?.blocksInteraction ?? false;
    session?.setClipboardTransferBlocked(requested);
    final blocked = requested || (session?.clipboardTransferBlocked ?? false);
    final preservesPaste =
        blocked && (session?.preparingClipboardPaste ?? false);
    final visible =
        _transfer != null && _transfer!.phase != TransferPhase.hidden;
    if (visible != _transferNoticeVisible ||
        blocked != _transferBlocked ||
        preservesPaste != _transferPreservesPaste) {
      _scheduleTransferRebuild();
    }
    _transferNoticeVisible = visible;
    if (blocked == _transferBlocked &&
        preservesPaste == _transferPreservesPaste) {
      return;
    }
    if (blocked && !_transferBlocked) _transferOverlayRaise = true;
    final wasPreserving = _transferPreservesPaste;
    _transferBlocked = blocked;
    _transferPreservesPaste = preservesPaste;
    if (wasPreserving && !preservesPaste) _release();
    if (blocked) {
      _dismissSessionGuide();
      _discardPointerMoves();
      if (!preservesPaste && !wasPreserving) _release();
      _heldPointer = null;
      _pictureHeld.value = false;
      _popups.closeAll();
      _viewportDebounce?.cancel();
      _groupLayoutDebounce?.cancel();
    } else {
      if (_desiredViewport != _attemptedViewport) {
        _viewportDebounce?.cancel();
        _viewportDebounce = Timer(
          const Duration(milliseconds: 650),
          () => unawaited(_applyViewport()),
        );
      }
      if (_groupLayoutNeeded) _scheduleGroupLayout();
      if (mounted) _printChanged();
    }
    _syncPointer();
  }

  void _scheduleTransferRebuild() {
    if (!mounted || _transferRebuildScheduled) return;
    _transferRebuildScheduled = true;
    WidgetsBinding.instance.addPostFrameCallback((_) {
      _transferRebuildScheduled = false;
      if (!mounted) return;
      final transfer = _transfer;
      final visible = transfer != null && _transferBlocked;
      if (!visible) {
        _transferOverlay?.remove();
        _transferOverlay?.dispose();
        _transferOverlay = null;
      } else {
        final overlay = Overlay.of(context, rootOverlay: true);
        if (_transferOverlay == null) {
          _transferOverlay = OverlayEntry(
            builder: (_) {
              final current = _transfer;
              if (current == null) return const SizedBox.shrink();
              return Positioned.fill(
                child: ClipboardTransferOverlay(
                  controller: current,
                  dark: SessionChrome.of(context).dark,
                  primaryRegion: _primaryRegion(
                    MediaQuery.devicePixelRatioOf(context),
                  ),
                  requestFocus: !_transferPreservesPaste,
                  onInteract: _release,
                ),
              );
            },
          );
          overlay.insert(_transferOverlay!);
        } else {
          _transferOverlay!.markNeedsBuild();
          if (_transferOverlayRaise) {
            _transferOverlay!.remove();
            overlay.insert(_transferOverlay!);
          }
        }
      }
      _transferOverlayRaise = false;
      setState(() {});
    });
    WidgetsBinding.instance.ensureVisualUpdate();
  }

  /// print-v1: what the VM printed to its Multitap printer comes here as a
  /// document; one dialog at a time asks where it goes (nothing prints on
  /// its own). Fixed notices: the printer is ready, could not be made, a
  /// document was skipped, or the administrator turned printing off.
  PrintSession? _watchedPrinter;
  bool _printDialogOpen = false,
      _printQueueNoticed = false,
      _printStopNoticed = false;

  /// The "문서를 받는 중" bar of a document that is still arriving; like the
  /// clipboard notice it appears only after ~1.2 s, so small documents never
  /// flash one.
  bool _printReceiving = false;
  Timer? _printSlow;
  ScaffoldMessengerState? _printMessenger;
  ScaffoldFeatureController<SnackBar, SnackBarClosedReason>? _printBar;

  void _closePrintBar({bool disposing = false}) {
    _printSlow?.cancel();
    _printReceiving = false;
    final bar = _printBar, messenger = _printMessenger;
    _printBar = null;
    if (bar == null) return;
    if (!disposing) {
      bar.close();
      return;
    }
    // Not while this tree is being torn down: the messenger rebuilds.
    WidgetsBinding.instance.addPostFrameCallback((_) {
      if (messenger?.mounted == true) bar.close();
    });
  }

  void _watchPrintReceive(PrintSession printer) {
    if (printer.receiving && !_printReceiving) {
      _printReceiving = true;
      _printSlow?.cancel();
      _printSlow = Timer(const Duration(milliseconds: 1200), () {
        if (!mounted ||
            !printer.receiving ||
            _printBar != null ||
            !identical(printer, _watchedPrinter)) {
          return;
        }
        final messenger = _printMessenger = ScaffoldMessenger.of(context);
        final bar = messenger.showSnackBar(
          SnackBar(
            key: const Key('print-receive'),
            duration: const Duration(minutes: 15),
            content: PrintReceiveNotice(printer: printer),
          ),
        );
        _printBar = bar;
        unawaited(
          bar.closed.then((_) {
            if (identical(_printBar, bar)) _printBar = null;
          }),
        );
      });
    } else if (!printer.receiving && _printReceiving) {
      // Arrived (the print dialog follows), cancelled or failed (a notice).
      _closePrintBar();
    }
  }

  /// gamepad-v1: this PC's gamepads for the current session (session_gamepads.dart).
  SessionGamepads? _gamepads;
  void _startGamepads(RemoteSession session) {
    _forgetGamepads();
    final gamepads = _gamepads = SessionGamepads(
      session,
      channel: windowsGamepadChannel,
    );
    gamepads.setActive(
      WidgetsBinding.instance.lifecycleState == null ||
          WidgetsBinding.instance.lifecycleState == AppLifecycleState.resumed,
    );
    unawaited(gamepads.start());
  }

  void _forgetGamepads() {
    final gamepads = _gamepads;
    _gamepads = null;
    if (gamepads != null) unawaited(gamepads.stop());
  }

  void _forgetPrinter({bool disposing = false}) {
    _closePrintBar(disposing: disposing);
    _watchedPrinter?.removeListener(_printChanged);
    _watchedPrinter = null;
    _printQueueNoticed = _printStopNoticed = _printLostNoticed = false;
  }

  /// Said once per session window when the account may print but the VM
  /// cannot offer its PC printer (the Host reported why before connecting).
  bool _printUnavailableNoticed = false;

  void _watchPrinter() {
    final printer = _session?.printer;
    final reason = _machine.printUnavailable;
    if (!_printUnavailableNoticed &&
        reason != null &&
        printer == null &&
        _connected) {
      _printUnavailableNoticed = true;
      _printNotice(
        l10n.desktopCannotPrintPc(printCodeMessage(reason)),
        seconds: 8,
      );
    }
    if (identical(printer, _watchedPrinter)) {
      // The session changed (it resumed, or it ended): what was held back
      // while the printer was coming back by itself is said now.
      if (printer != null && printer.closed) _printChanged();
      return;
    }
    _forgetPrinter();
    _watchedPrinter = printer?..addListener(_printChanged);
    if (printer != null) _printChanged();
  }

  /// The documents lost with a printer connection that is coming back by
  /// itself were said (the "connect again" notice then does not repeat them).
  bool _printLostNoticed = false;

  void _printNotice(
    String message, {
    int seconds = 6,
    IconData icon = Icons.print_rounded,
  }) => WidgetsBinding.instance.addPostFrameCallback(
    (_) => _showToast(
      message,
      icon: icon,
      duration: Duration(seconds: seconds),
    ),
  );

  void _printChanged() {
    final printer = _watchedPrinter;
    if (!mounted || printer == null) return;
    _watchPrintReceive(printer);
    if (!_printQueueNoticed && printer.queueReason != null) {
      _printQueueNoticed = true;
      _printNotice(printCodeMessage(printer.queueReason!));
    }
    final skipped = printer.skipped;
    if (skipped != null) {
      printer.clearSkipped();
      _printNotice(printCodeMessage(skipped));
    }
    if (printer.closed && printer.error != null) {
      // Not while the stream itself is away or the printer is coming back by
      // itself (channel-resume-v1): then only what is really lost is said,
      // without "connect again". A stream that ended says why itself.
      if (_session?.resuming == true || printer.recovering) {
        if (printer.lostDocuments > 0 && !_printLostNoticed) {
          _printLostNoticed = true;
          _printNotice(printLostNotice(printer.lostDocuments));
        }
      } else if (!_printStopNoticed && _session?.closed == false) {
        _printStopNoticed = true;
        _printNotice(
          printer.error == 'print_policy_changed'
              ? printCodeMessage('print_policy_changed')
              : printer.lostDocuments > 0 && !_printLostNoticed
              ? l10n.printerConnectionWasLostReconnect(printer.lostDocuments)
              : l10n.printerConnectionWasLostReconnect2,
        );
      }
    }
    if (!_transferBlocked &&
        !_manualGuideOpen &&
        !_printDialogOpen &&
        printer.pending.isNotEmpty &&
        !printer.closed) {
      _printDialogOpen = true;
      WidgetsBinding.instance.addPostFrameCallback(
        (_) => unawaited(_offerPrint(printer)),
      );
    }
  }

  Future<void> _offerPrint(PrintSession printer) async {
    try {
      if (!mounted ||
          _transferBlocked ||
          !identical(printer, _watchedPrinter) ||
          printer.pending.isEmpty) {
        return;
      }
      final document = printer.pending.first;
      // Kept on disk even if the channel ends while the person is choosing.
      printer.hold(document);
      // Behind another window or minimized: the taskbar button flashes, so a
      // document does not wait unseen.
      unawaited(_presentation.flash());
      _release();
      _holdPointer();
      PrintMemory? memory;
      try {
        memory = PrintMemory.windows();
      } catch (_) {}
      final outcome = await showPrintDialog(
        context,
        document: document,
        machine: _machine.name,
        memory: memory,
        waiting: printer.pending.length > 1 ? printer.pending.length - 1 : 0,
      ).whenComplete(_unholdPointer);
      await printer.finish(document, outcome);
      if (outcome == PrintOutcome.printed) {
        _printNotice(l10n.printed, seconds: 3, icon: Icons.check_circle_rounded);
      } else if (outcome == PrintOutcome.saved) {
        // (the printed case above is the brief "✓ 인쇄했어요".)
        _printNotice(l10n.savedAsPdf, seconds: 3);
      } else if (outcome == PrintOutcome.cancelled) {
        // The document is gone (the VM's print already finished): say so, so
        // an accidental Esc is not a silent loss.
        _printNotice(l10n.printingCanceledPrintAgainFrom, seconds: 4);
      }
    } finally {
      _printDialogOpen = false;
      // The next document, if one is waiting.
      if (mounted && identical(printer, _watchedPrinter)) _printChanged();
    }
  }

  /// The sharing channel ended with an error while this session continues.
  static bool _clipboardStopped(ClipboardSession clipboard) =>
      clipboard.closed &&
      clipboard.error != null &&
      clipboard.error != 'clipboard_session_closed';

  SessionDisplayMode get _shownMode =>
      _displayMode == SessionDisplayMode.allMonitors
      ? SessionDisplayMode.allMonitors
      : _presentation.fullscreen
      ? SessionDisplayMode.fullscreen
      : SessionDisplayMode.windowed;

  /// The bar's three groups: 화면 (the screen mode and 배율 in
  /// one panel; its icon is the current mode), 폴더·USB (a tab each; only
  /// the sides this connection has) and 설정 (화질·소리, 마우스 잠금, 바
  /// 열어 두기, 사용안내). Each drops a panel from its button; the narrow
  /// bar's 메뉴 lists their entries under the same captions. Statistics
  /// live in the quality chip.
  List<Widget> _menus() {
    final session = _session;
    // These are Host-confirmed states forwarded by RemoteSession's existing
    // drive/USB listeners, not permissions, local enumeration or attach attempts.
    // A closed channel/session cannot keep an old connected indicator alive.
    final drive = session?.drive;
    final usb = session?.usb;
    final mappedFolders = _connected
        ? SessionSharePanel.mappedFolders(drive)
        : 0;
    final sharedUsb = _connected ? SessionSharePanel.sharedUsb(usb) : 0;
    // Session menus are always dark chrome, so icons use the dark palette.
    const palette = AppPalette(true);
    final shown = _shownMode;
    // drive-v1 / usb-v1: only in a session the broker granted them. A side
    // the administrator or the Host offers but this connection cannot use
    // stays, with the reason, instead of silently not being there.
    final driveReason = _driveUnavailableReason;
    final usbReason = _usbUnavailableReason;
    final hasDrives = drive != null || driveReason != null;
    final hasUsb = usb != null || usbReason != null;
    final shared = mappedFolders + sharedUsb;
    final shareUsable = drive != null || usb != null;
    final quality = _qualityReason;
    final guide = _guideReason;
    return [
      SessionToolbarMenu.command(
        key: const Key('stream-menu-화면'),
        label: SessionTool.screen.label,
        tooltip: l10n.screenMode(shown.label),
        icon: sessionDisplayModeIcon(shown),
        keepOpenInOverflow: true,
        onInteract: _release,
        // The button stays lit while its panel is up; the panel's barrier
        // takes a second press, and the opener ignores a re-entry.
        onPressed: () => unawaited(_screenPanel(anchor: _toolbarAnchor())),
        onPressedAt: (anchor) => _screenPanel(anchor: anchor),
      ),
      if (hasDrives || hasUsb)
        SessionToolbarMenu.command(
          key: const Key('remote-share'),
          label: hasDrives && hasUsb
              ? SessionTool.share.label
              : hasDrives
              ? SessionTool.drives.label
              : SessionTool.usb.label,
          // Greyed, it says only its name and the reason.
          tooltip: !shareUsable
              ? (hasDrives ? SessionTool.drives.label : SessionTool.usb.label)
              : shared > 0
              ? [
                  if (mappedFolders > 0) l10n.text29(mappedFolders),
                  if (sharedUsb > 0) l10n.text30(sharedUsb),
                ].join(' · ')
              : hasDrives && hasUsb
              ? l10n.foldersUsbUsePcsFolders
              : hasDrives
              ? l10n.sharedFoldersOpenPcFolders
              : l10n.usbDevicesUsePcsUsb,
          icon: shareUsable
              ? (hasDrives ? SessionTool.drives.icon : SessionTool.usb.icon)
              : hasDrives
              ? Icons.folder_off_rounded
              : Icons.usb_off_rounded,
          active: shared > 0,
          badge: shared > 0 ? palette.green : null,
          keepOpenInOverflow: true,
          onInteract: _release,
          // Nothing usable on either side: greyed, with the reason.
          onPressed: !shareUsable
              ? null
              : () => unawaited(_sharePanel(anchor: _toolbarAnchor())),
          onPressedAt: !shareUsable
              ? null
              : (anchor) => _sharePanel(anchor: anchor),
          disabledReason: driveReason ?? usbReason,
          overflow: [
            if (hasDrives)
              SessionMenuItem(
                SessionTool.drives.label,
                key: const Key('remote-drives'),
                icon: drive == null
                    ? Icons.folder_off_rounded
                    : SessionTool.drives.icon,
                value: mappedFolders > 0 ? l10n.connected4(mappedFolders) : null,
                color: mappedFolders > 0 ? palette.green : null,
                keepToolbarOpen: true,
                disabledReason: driveReason,
                onSelected: drive == null || _shareOpen
                    ? null
                    : () => unawaited(
                        _sharePanel(
                          anchor: _toolbarAnchor(),
                          tab: SessionShareTab.folders,
                        ),
                      ),
              ),
            if (hasUsb)
              SessionMenuItem(
                SessionTool.usb.label,
                key: const Key('remote-usb'),
                icon: usb == null
                    ? Icons.usb_off_rounded
                    : SessionTool.usb.icon,
                value: sharedUsb > 0 ? l10n.connected5(sharedUsb) : null,
                color: sharedUsb > 0 ? palette.green : null,
                keepToolbarOpen: true,
                disabledReason: usbReason,
                onSelected: usb == null || _shareOpen
                    ? null
                    : () => unawaited(
                        _sharePanel(
                          anchor: _toolbarAnchor(),
                          tab: SessionShareTab.usb,
                        ),
                      ),
              ),
          ],
        ),
      // Printing has no toolbar item: the VM prints to
      // 「Multitap 프린터 (PC)」 by itself; 사용안내 explains it and a print
      // problem still shows its own notice.
      SessionToolbarMenu.command(
        key: const Key('remote-settings'),
        label: SessionTool.more.label,
        tooltip: l10n.settingsPictureQualityGuide,
        icon: SessionTool.more.icon,
        keepOpenInOverflow: true,
        onInteract: _release,
        onPressed: () => unawaited(_settingsMenu(anchor: _toolbarAnchor())),
        onPressedAt: (anchor) => _settingsMenu(anchor: anchor),
        overflow: [
          if (widget.onReconnect != null)
            SessionMenuItem(
              SessionTool.settings.label,
              key: const Key('remote-stream-settings'),
              icon: SessionTool.settings.icon,
              keepToolbarOpen: true,
              disabledReason: quality,
              onSelected: quality != null
                  ? null
                  : () => unawaited(_settings(anchor: _toolbarAnchor())),
            ),
          if (_mouseLockOffered)
            SessionMenuItem(
              SessionTool.gameMouse.label,
              key: const Key('session-game-mouse'),
              icon: SessionTool.gameMouse.icon,
              value: 'Ctrl+Shift+Alt+Z',
              checked: _gameMouse,
              onSelected: _toggleGameMouse,
            ),
          if (widget.sessionGuideEnabled)
            SessionMenuItem(
              SessionTool.guide.label,
              key: const Key('remote-session-guide'),
              icon: SessionTool.guide.icon,
              keepToolbarOpen: true,
              disabledReason: guide,
              onSelected: guide != null
                  ? null
                  : () => unawaited(
                      _requestSessionGuide(anchor: _toolbarAnchor()),
                    ),
            ),
        ],
      ),
    ];
  }

  /// 마우스 잠금 can be offered in this window now.
  bool get _mouseLockOffered => _pointerControl && _relativeMouseOffered;

  /// Why this connection has no shared folders although the administrator
  /// allows them (the VM's WebClient service is disabled or missing); null
  /// when it has them or they do not apply.
  String? get _driveUnavailableReason =>
      _session?.drive == null && _machine.driveUnavailable != null
      ? driveMessage(_machine.driveUnavailable)
      : null;

  /// The Host offers USB but this assignment does not allow it (shown with
  /// the reason, not hidden).
  String? get _usbUnavailableReason =>
      _session?.usb == null && _connected && _session!.connection.usbDenied
      ? adminPermissionNeeded
      : null;

  /// Why 화질·소리 cannot be opened now; null when it can.
  String? get _qualityReason {
    final session = _session;
    final pictured =
        _connected && session!.textureId != null && !session.resizing;
    return pictured && !_reconnecting && !_presentationChanging
        ? null
        : _connected
        ? l10n.youCanChangeWhenScreen
        : l10n.youCanChangeOnceConnected;
  }

  /// Why 사용안내 cannot be opened now; null when it can.
  String? get _guideReason {
    final session = _session;
    final pictured =
        _connected && session!.textureId != null && !session.resizing;
    return _manualGuideOpen
        ? l10n.guideOpen
        : pictured
        ? null
        : l10n.availableOnceConnected2;
  }

  /// The current streaming choices in a few words, for the 설정 panel.
  String get _qualitySummary {
    final session = _session;
    final options = session == null
        ? _lastOptions
        : session.connection.options;
    final preset = StreamPreset.of(options, _machine.maxBitrate);
    return [
      preset?.label ?? l10n.custom,
      '${options.fps} fps',
    ].join(' · ');
  }

  bool _screenPanelOpen = false;

  /// 화면: the screen mode and 배율 in one panel. A mode or a scale step
  /// closes it and applies at once; resolves true when something applied.
  Future<bool> _screenPanel({Rect? anchor}) async {
    if (!mounted || _screenPanelOpen || _closingWindow) return false;
    final session = _session;
    setState(() => _screenPanelOpen = true);
    _release();
    _holdPointer();
    try {
      final pictured =
          _connected && session!.textureId != null && !session.resizing;
      final modes = [
        SessionDisplayMode.windowed,
        SessionDisplayMode.fullscreen,
        if (_allMonitorsOffered) SessionDisplayMode.allMonitors,
      ];
      // Independent outputs only where display-scale-outputs-v1 was granted.
      final available = pictured && session.displayScaleSupported;
      final viewport = _viewportKey.currentContext;
      final box = viewport?.findRenderObject();
      final pixels = viewport != null && box is RenderBox && box.hasSize
          ? viewportResolution(
              box.size,
              MediaQuery.devicePixelRatioOf(viewport),
            )
          : null;
      final choice = await showSessionPanel<SessionScreenChoice>(
        context,
        anchor: anchor,
        width: SessionScreenPanel.widthFor(modes.length),
        minHeight: 250,
        label: SessionTool.screen.label,
        popups: _popups,
        builder: (context) => SessionScreenPanel(
          modes: modes,
          shown: _shownMode,
          changing: _reconnecting || _presentationChanging,
          session: pictured ? session : null,
          scaleAvailable: available,
          viewportPixels: pixels,
          automaticResolution: _autoViewport,
          resolutionAvailable:
              pictured &&
              session.canResizeViewport &&
              !session.independentOutputs,
          displaysOffered: _virtualDisplayCountOffered,
          displaysEnabled: pictured && _machine.canConfigureDisplays,
          onSelected: (choice) => Navigator.pop(context, choice),
        ),
      );
      if (choice == null || !mounted || _transferBlocked) return false;
      final mode = choice.mode;
      if (mode != null) {
        if (_reconnecting || _presentationChanging) return false;
        switch (mode) {
          case SessionDisplayMode.windowed:
            _release();
            unawaited(_setDisplayMode(false));
          case SessionDisplayMode.fullscreen:
            _release();
            unawaited(_setDisplayMode(true));
          case SessionDisplayMode.allMonitors:
            unawaited(_useAllMonitors());
        }
        return true;
      }
      if (choice.displays) {
        unawaited(_configureDisplays());
        return true;
      }
      final scale = choice.scale;
      if (scale == null ||
          session == null ||
          !identical(session, _session) ||
          session.closed ||
          !available) {
        return false;
      }
      _scalePreference = scale;
      session.requestDisplayScale(scale);
      return true;
    } finally {
      _unholdPointer();
      if (mounted) setState(() => _screenPanelOpen = false);
    }
  }

  bool _shareOpen = false;

  /// The tab 폴더·USB last showed in this window.
  SessionShareTab _shareTab = SessionShareTab.folders;

  /// 폴더·USB: the shared-folder panel (drive-v1) and the USB panel (usb-v1)
  /// as two tabs; every change in them applies at once.
  Future<bool> _sharePanel({Rect? anchor, SessionShareTab? tab}) async {
    final session = _session;
    if (session == null ||
        (session.drive == null && session.usb == null) ||
        _shareOpen ||
        !mounted) {
      return false;
    }
    _release();
    setState(() => _shareOpen = true);
    _holdPointer();
    try {
      // A panel is removed when the native folder picker takes the focus;
      // its confirmations must not depend on the panel's own context.
      final dialogContext = Navigator.of(context).overlay!.context;
      await showSessionPanel<void>(
        context,
        anchor: anchor,
        width: SessionSharePanel.width,
        minHeight: SessionSharePanel.minHeight,
        label: SessionTool.share.label,
        popups: _popups,
        keepOpenForDialog: true,
        // A channel that came back is a new object in the session: the open
        // panel follows the session's current ones.
        builder: (_) => SessionShareFollower(
          session: session,
          drive: () => session.drive,
          usb: () => session.usb,
          builder: (drive, usb) => SessionSharePanel(
            drive: drive,
            usb: usb,
            driveUnavailable: _driveUnavailableReason,
            usbUnavailable: _usbUnavailableReason,
            initial: tab ?? _shareTab,
            dialogContext: dialogContext,
            onTab: (shown) => _shareTab = shown,
          ),
        ),
      ).whenComplete(_unholdPointer);
    } finally {
      if (mounted) setState(() => _shareOpen = false);
    }
    return false;
  }

  /// The options this connection asked for.
  StreamOptions get _currentOptions {
    final session = _session;
    return session == null ? _lastOptions : session.connection.options;
  }

  bool _connectionOpen = false;

  /// 연결: the quality chip's panel. Resolves true when something in it was
  /// used (the bar then folds).
  Future<bool> _connectionPanel(Rect anchor) async {
    if (!mounted || _connectionOpen || _closingWindow) return false;
    _connectionOpen = true;
    String? choice;
    try {
      // 절약 helps only where it is not what the connection already asks for
      // and a reconnect is possible now.
      final saver =
          widget.onReconnect != null &&
          _qualityReason == null &&
          StreamPreset.of(_currentOptions, _machine.maxBitrate) !=
              StreamPreset.saver;
      choice = await showSessionPanel<String>(
        context,
        anchor: anchor,
        width: SessionConnectionPanel.width,
        minHeight: SessionConnectionPanel.minHeight,
        label: SessionTool.quality.label,
        popups: _popups,
        builder: (context) => SessionConnectionPanel(
          title: _machine.name,
          mode: l10n.streaming,
          listenable: _session,
          latency: () => _session?.rttMs,
          connected: () => _connected,
          values: () => sessionQualityValues(_session, device: 'PC'),
          advice: () => _toolbar?.poorSustained == true,
          onSaver: saver ? () => Navigator.pop(context, 'saver') : null,
          onStats: () => Navigator.pop(context, 'stats'),
        ),
      );
    } finally {
      _connectionOpen = false;
    }
    if (!mounted || choice == null) return false;
    if (choice == 'stats') {
      _stats();
    } else {
      unawaited(_reconnectSaver());
    }
    return true;
  }

  /// 절약으로 다시 연결, from the 연결 panel's advice: the same reconnect the
  /// 화질·소리 panel makes, with 절약's frame rate and bitrate and the
  /// current sound choice. Never during a file copy.
  Future<void> _reconnectSaver() async {
    if (_transferBlocked) {
      _showToast(l10n.filesBeingCopiedTryAgain);
      return;
    }
    final previous = _session;
    if (widget.onReconnect == null ||
        _reconnecting ||
        _settingsOpen ||
        _closingWindow ||
        !_retryAllowed ||
        !(previous?.canRetry ?? true)) {
      return;
    }
    final current = _currentOptions;
    final saver = StreamPreset.saver.options;
    final options = StreamSettingsBody.initialRequest(
      _machine,
      StreamOptions(
        fps: saver.fps,
        bitrate: saver.bitrate,
        bitrateMode: saver.bitrateMode,
        monitor: current.monitor,
      ),
    );
    // Encoding preferences preserve the user's automatic or fixed workspace.
    await _load(reconnect: true, options: options, newIntent: true);
  }

  // The bar's first-connection hint: once per account on this PC, never
  // together with 사용안내 (opening the guide dismisses it for good).
  static const _coachId = 'multitap.session-bar-coach.v1';
  bool _coachChecked = false, _coachShown = false;

  SessionGuideStore? get _coachStore {
    final injected = widget.coachStore;
    if (injected != null) return injected;
    final memory = widget.pointerMemory;
    final store = memory is SessionGuideStoreProvider
        ? (memory as SessionGuideStoreProvider).sessionGuideStore
        : null;
    // Without a file it could not be "once": never shown then.
    return store != null && store.persistent ? store : null;
  }

  SessionGuideIdentity? get _coachIdentity {
    final account = widget.guideAccountKey;
    if (account == null) return null;
    try {
      return SessionGuideIdentity(accountKey: account, desktopId: _coachId);
    } on FormatException {
      return null;
    }
  }

  Future<void> _maybeCoach() async {
    final store = _coachStore, identity = _coachIdentity;
    if (store == null || identity == null || !_pointerControl) return;
    bool seen;
    try {
      seen = await store.hidden(identity);
    } catch (_) {
      // An unreadable preference never interrupts the session.
      return;
    }
    if (seen ||
        !mounted ||
        !_connected ||
        _closingWindow ||
        _manualGuideOpen ||
        _gameMouse ||
        _transferBlocked ||
        !_popups.isEmpty ||
        !(ModalRoute.isCurrentOf(context) ?? true)) {
      return;
    }
    final toolbar = _toolbar;
    if (toolbar == null) return;
    setState(() => _coachShown = true);
    toolbar.open();
  }

  /// The hint goes for good: its 확인, any use of the bar, a click back into
  /// the picture, or 사용안내.
  void _dismissCoach({bool fold = false}) {
    if (!_coachShown) return;
    if (mounted) setState(() => _coachShown = false);
    final store = _coachStore, identity = _coachIdentity;
    if (store != null && identity != null) {
      unawaited(
        store.setHidden(identity, true).catchError((Object _) {
          // Shown again next time rather than interrupting the session.
        }),
      );
    }
    if (fold) _toolbar?.fold();
  }

  bool _settingsMenuOpen = false;

  /// The session bar itself (its 바 열어 두기 lives in the 설정 panel).
  SessionToolbarState? get _toolbar {
    final state = _toolbarKey.currentState;
    return state is SessionToolbarState ? state : null;
  }

  /// 설정: 화질·소리 opens the streaming choices in its place (their back
  /// button returns here), 마우스 잠금 and 사용안내 close it and act, 바 열어
  /// 두기 applies in place. Resolves true when the bar should fold.
  Future<bool> _settingsMenu({Rect? anchor}) async {
    if (!mounted || _settingsMenuOpen || _closingWindow) return false;
    setState(() => _settingsMenuOpen = true);
    try {
      while (true) {
        if (!mounted || _closingWindow) return false;
        _release();
        _holdPointer();
        SessionSettingsChoice? choice;
        try {
          choice = await showSessionPanel<SessionSettingsChoice>(
            context,
            anchor: anchor,
            width: SessionSettingsPanel.width,
            minHeight: SessionSettingsPanel.minHeight,
            label: SessionTool.more.label,
            popups: _popups,
            builder: (context) => SessionSettingsPanel(
              quality: widget.onReconnect != null,
              qualitySummary: _qualitySummary,
              qualityReason: _qualityReason,
              mouseLock: _mouseLockOffered ? _gameMouse : null,
              mouseLockUsb: _gameMouseUsb?.active == true,
              pinned: _toolbar?.pinned ?? false,
              onPinned: _toolbar == null
                  ? null
                  : (pinned) => _toolbar?.setPinned(pinned),
              guide: widget.sessionGuideEnabled,
              guideReason: _guideReason,
              onSelected: (choice) => Navigator.pop(context, choice),
            ),
          );
        } finally {
          _unholdPointer();
        }
        if (!mounted || choice == null) return false;
        switch (choice) {
          case SessionSettingsChoice.quality:
            var back = false;
            final applied = await _settings(
              anchor: anchor,
              onBack: () => back = true,
            );
            if (applied) return true;
            if (!back) return false;
          case SessionSettingsChoice.mouseLock:
            _toggleGameMouse();
            return true;
          case SessionSettingsChoice.guide:
            await _requestSessionGuide(anchor: anchor);
            return false;
        }
      }
    } finally {
      if (mounted) setState(() => _settingsMenuOpen = false);
    }
  }

  /// A desktop restart under this window: until [_plannedRestartUntil] the
  /// ended stream waits for the desktop and reconnects instead of failing.
  /// (The window never asks for one itself. The Host going away with the
  /// desktop running starts it, see [_hostAwayRestart].)
  DateTime? _plannedRestartUntil;
  RemoteSession? _plannedRestartFrom;
  static const plannedRestartWait = Duration(minutes: 3);
  bool get _plannedRestart =>
      _plannedRestartUntil?.isAfter(DateTime.now()) == true;

  /// The wait above began because the Host went away with the VM running
  /// (Windows restarting from inside), not from this window's own restart.
  bool _hostAwayRestart = false;

  /// A Host update ended the stream (host_update): while this
  /// timer runs the view waits for the update and the Host, then reconnects
  /// by itself, as after a restart. Past it, the manual text.
  Timer? _hostUpdateTimer;
  static const hostUpdateWait = Duration(minutes: 10);
  bool get _hostUpdating => _hostUpdateTimer?.isActive == true;

  void _endHostUpdateWait() {
    _hostUpdateTimer?.cancel();
    _hostUpdateTimer = null;
    _hostLossRecoveryUntil = null;
  }

  /// The wait ran out, or 기다리기 취소: no reconnect; 다시 연결 stays.
  void _stopHostUpdateWait({required bool cancelled}) {
    if (!mounted || _closingWindow || _hostUpdateTimer == null) return;
    _endHostUpdateWait();
    // A new stream that already started keeps going.
    if (_session?.closed == false) return;
    ++_connectionAttempt;
    _serverRestartRetry?.cancel();
    if (!cancelled) {
      ProductLog.instance.recordPreparationFailure(
        LogEvent.streamEnd,
        'host_update',
        {'ok': false, 'stage': 2},
      );
    }
    setState(() {
      _reconnecting = false;
      _screenTransitionStage = null;
      _error = cancelled
          ? l10n.stoppedWaitingHostUpdateTap
          : SessionFailure.ended('host_update').message;
      _requestErrorCode = cancelled ? 'operation_cancelled' : 'host_update';
      _retryAllowed = true;
      _takeoverAllowed = false;
    });
  }

  /// 기다리기 취소: the waiting reconnect is dropped; 다시 연결 stays.
  void _cancelPlannedRestart() {
    if (!mounted || _plannedRestartUntil == null) return;
    ++_connectionAttempt;
    setState(() {
      _plannedRestartUntil = _plannedRestartFrom = null;
      _hostAwayRestart = false;
      _hostLossRecoveryUntil = null;
      _reconnecting = false;
      _screenTransitionStage = null;
      _error = l10n.stoppedWaitingTapReconnectWhen;
      _requestErrorCode = 'operation_cancelled';
      _retryAllowed = true;
      _takeoverAllowed = false;
    });
  }

  /// USB that ended while the stream goes on is said once, and so is each
  /// device another remote window took (이 창으로 옮기기 there).
  UsbSession? _watchedUsb;
  bool _usbEndNoticed = false;
  int _usbMovesNoticed = 0, _usbResumesNoticed = 0;

  /// Devices shared in a session this window replaced (reconnect, new screen
  /// mode): offered again in the next session, and said in one line either
  /// way, never dropped silently.
  List<String> _usbResume = const [];
  void _watchUsb() {
    final usb = _session?.usb;
    if (!identical(usb, _watchedUsb)) {
      _watchedUsb = usb;
      _usbEndNoticed = false;
      _usbMovesNoticed = usb?.moves ?? 0;
      _usbResumesNoticed = usb?.resumeNotices ?? 0;
      if (usb != null && _usbResume.isNotEmpty) {
        usb.resume(_usbResume);
        _usbResume = const [];
      }
    }
    // The new session streams without USB (not granted there): say so.
    if (_usbResume.isNotEmpty && usb == null && _session?.frameReady == true) {
      _usbResume = const [];
      WidgetsBinding.instance.addPostFrameCallback(
        (_) => _showToast(
          l10n.couldNotReconnectUsbDevice,
          icon: Icons.usb_off_rounded,
          duration: const Duration(seconds: 6),
          share: true,
        ),
      );
    }
    if (usb != null && usb.resumeNotices > _usbResumesNoticed) {
      _usbResumesNoticed = usb.resumeNotices;
      final notice = usb.resumeNotice!, failed = usb.resumeFailed;
      WidgetsBinding.instance.addPostFrameCallback(
        (_) => _showToast(
          notice,
          icon: failed ? Icons.usb_off_rounded : Icons.usb_rounded,
          duration: const Duration(seconds: 6),
          share: true,
        ),
      );
    }
    if (usb != null && usb.moves > _usbMovesNoticed) {
      _usbMovesNoticed = usb.moves;
      final to = usb.lastMovedTo;
      WidgetsBinding.instance.addPostFrameCallback(
        (_) => _showToast(
          to.isEmpty ? l10n.movedUsbDeviceAnotherWindow : l10n.movedUsbDeviceWindow(to),
          icon: Icons.usb_rounded,
          duration: const Duration(seconds: 6),
        ),
      );
    }
    if (usb == null || _usbEndNoticed || _session?.closed != false) return;
    final message = usbEndedMessage(usb);
    if (message == null) return;
    _usbEndNoticed = true;
    WidgetsBinding.instance.addPostFrameCallback(
      (_) => _showToast(
        message,
        icon: Icons.usb_off_rounded,
        duration: const Duration(seconds: 6),
        share: true,
      ),
    );
  }

  /// Sharing that ended while the stream goes on is said once.
  DriveSession? _watchedDrive;
  bool _driveEndNoticed = false;
  int _driveGoneNoticed = 0;
  /// Folders were shared in a session this window replaced (reconnect, new
  /// quality, new screen mode). A new session never shares a folder by
  /// itself, so their sharing ended there: said once when the picture is
  /// back, like the USB devices' line.
  bool _driveEndedByReconnect = false;
  void _watchDrive() {
    final drive = _session?.drive;
    if (!identical(drive, _watchedDrive)) {
      _watchedDrive = drive;
      _driveEndNoticed = false;
      _driveGoneNoticed = drive?.goneCount ?? 0;
    }
    if (_driveEndedByReconnect && _session?.frameReady == true) {
      _driveEndedByReconnect = false;
      WidgetsBinding.instance.addPostFrameCallback(
        (_) => _showToast(
          l10n.sharedFoldersEndedByReconnect,
          icon: Icons.folder_off_rounded,
          duration: const Duration(seconds: 6),
          share: true,
        ),
      );
    }
    // Only while the stream goes on; an ended session says why itself.
    if (drive == null || _session?.closed != false) return;
    if (drive.goneCount > _driveGoneNoticed && drive.lastGone != null) {
      _driveGoneNoticed = drive.goneCount;
      final gone = driveGoneMessage(drive.lastGone!);
      WidgetsBinding.instance.addPostFrameCallback(
        (_) => _showToast(
          gone,
          icon: Icons.folder_off_rounded,
          duration: const Duration(seconds: 6),
          share: true,
        ),
      );
    }
    if (_driveEndNoticed) return;
    final message = driveEndedMessage(drive);
    if (message == null) return;
    _driveEndNoticed = true;
    WidgetsBinding.instance.addPostFrameCallback(
      (_) => _showToast(
        message,
        icon: Icons.folder_off_rounded,
        duration: const Duration(seconds: 6),
        share: true,
      ),
    );
  }

  Future<void> _configureDisplays() async {
    if (_transferBlocked ||
        widget.onConfigureDisplays == null ||
        !_machine.canConfigureDisplays ||
        _session == null ||
        _session!.closed ||
        _reconnecting) {
      return;
    }
    _release();
    _holdPointer();
    final result = await showDialog<DesktopMachine>(
      context: context,
      barrierDismissible: false,
      builder: (_) => VirtualDisplayDialog(
        machine: _machine,
        onApply: (count) async {
          if (_transferBlocked) {
            throw const BrokerException('operation_cancelled');
          }
          final attempt = _connectionAttempt;
          bool current() =>
              mounted && !_closingWindow && attempt == _connectionAttempt;
          final previous = _session;
          if (previous != null) {
            _lastOptions = previous.connection.options;
          }
          previous?.removeListener(_changed);
          _forgetClipboard();
          _forgetPrinter();
          _forgetGamepads();
          _release();
          await previous?.close();
          if (!current()) throw const BrokerException('operation_cancelled');
          setState(() {
            _session = null;
            _displayDisconnected = true;
            _displayConfigured = false;
            _error = null;
            _requestErrorCode = null;
          });
          final refresh = widget.onRefreshDesktop;
          if (refresh != null) {
            var fresh = await refresh().timeout(const Duration(seconds: 8));
            if (!current()) throw const BrokerException('operation_cancelled');
            if (fresh.id != _machine.id) {
              throw const BrokerException('invalid_response');
            }
            fresh = await waitForStreamDisplayControl(
              fresh,
              refresh: refresh,
              current: current,
            );
            if (!current()) throw const BrokerException('operation_cancelled');
            if (fresh.sessionCleanup.pending) {
              throw const BrokerException('session_cleanup_pending', 409);
            }
            _machine = fresh;
          }
          return widget.onConfigureDisplays!(count);
        },
      ),
    ).whenComplete(_unholdPointer);
    if (result != null && mounted) {
      setState(() {
        _machine = result;
        _displayConfigured = true;
      });
    }
  }

  Future<void> _drainInputReleaseTurn() {
    final gate = Completer<void>();
    late final Timer timer;
    timer = Timer(Duration.zero, () {
      _releaseDrainGates.remove(timer);
      if (!gate.isCompleted) gate.complete();
    });
    _releaseDrainGates[timer] = gate;
    return gate.future;
  }

  Future<void> _releaseInput(
    RemoteSession session, {
    bool preserveInitialKeyboard = false,
  }) {
    final pending = _releaseInputPending;
    if (identical(_releaseInputFor, session) &&
        pending != null &&
        !session.captured) {
      return pending;
    }
    // capture(false) discards Dart input synchronously, then native reset and
    // released events arrive before its method reply. Install provenance first.
    final releaseContext = (
      session: session,
      token: Object(),
      preserveInitial:
          preserveInitialKeyboard && !identical(_initialKeyboardFor, session),
    );
    _releaseInputContext = releaseContext;
    // EventChannel broadcast callbacks can drain after the MethodChannel reply
    // even though native emitted reset/released first. Keep this exact release
    // token for that one event-loop turn before any successor native claim.
    final released = session.capture(false).then<void>((_) async {
      // A retired owner cannot claim input again. In particular, teardown
      // must not allocate a new event-loop timer after the view is disposed.
      if (!mounted ||
          _closingWindow ||
          !identical(session, _session) ||
          session.closed) {
        return;
      }
      await _drainInputReleaseTurn();
    });
    _releaseInputFor = session;
    _releaseInputPending = released;
    void settled() {
      if (!identical(_releaseInputPending, released)) return;
      _releaseInputFor = null;
      _releaseInputPending = null;
      final context = _releaseInputContext;
      if (identical(context?.token, releaseContext.token)) {
        _releaseInputContext = null;
        if (context?.preserveInitial == true && mounted) {
          _queueInitialKeyboard(session);
        }
      }
    }

    unawaited(
      released.then<void>(
        (_) => settled(),
        onError: (Object _, StackTrace _) => settled(),
      ),
    );
    return released;
  }

  void _release({bool preserveInitialKeyboard = false}) {
    // A pending USB hand-over must not reacquire focus after a menu, focus
    // loss, minimize or input-owner change returned control to this PC.
    _gameMouseRevision++;
    if (!preserveInitialKeyboard) {
      _captureIntentRevision++;
      _initialKeyboardFor = _session;
      _viewportInputContext = null;
      final context = _releaseInputContext;
      if (context != null) {
        _releaseInputContext = (
          session: context.session,
          token: context.token,
          preserveInitial: false,
        );
      }
    }
    _discardPointerMoves(preserveInitialKeyboard: preserveInitialKeyboard);
    if (!preserveInitialKeyboard) _focus.unfocus();
    final session = _session;
    if (session != null) {
      unawaited(
        _releaseInput(
          session,
          preserveInitialKeyboard: preserveInitialKeyboard,
        ),
      );
    }
  }

  void _discardPointerMoves({bool preserveInitialKeyboard = false}) {
    final context = _releaseInputContext;
    final expectedRelease =
        identical(context?.session, _session) && context != null;
    // Host resizing and stream metadata can reset input after the short native
    // release has settled. Only this exact active viewport transaction owns
    // those resets; an explicit release clears it and changes its intent fence.
    final viewport = _viewportInputContext;
    final expectedViewport =
        _viewportChanging &&
        viewport != null &&
        identical(viewport.session, _session) &&
        viewport.intent == _captureIntentRevision;
    if (!preserveInitialKeyboard &&
        !(expectedRelease && context.preserveInitial) &&
        !expectedViewport &&
        (_initialKeyboardQueued != null || _session?.captured == true)) {
      _initialKeyboardFor = _session;
    }
    if (!expectedRelease && !expectedViewport && !preserveInitialKeyboard) {
      _captureIntentRevision++;
    }
    _initialKeyboardQueued = null;
    _pendingPress = null;
    _initialKeyboardRetry?.cancel();
    _initialKeyboardRetry = null;
    _pointerTrack = null;
    _lastPointerPoint = null;
    _pointerRevision++;
    _pointerMoves.discard();
    _pointerSession = null;
    _pointerTexture = null;
    _buttons = 0;
  }

  void _syncPointerScope() {
    final session = _session;
    if (_transferBlocked ||
        session == null ||
        session.closed ||
        session.resizing ||
        !session.captured ||
        session.textureId == null ||
        !_focus.hasFocus) {
      if (_pointerSession != null) _discardPointerMoves();
      return;
    }
    if (!identical(session, _pointerSession) ||
        session.textureId != _pointerTexture) {
      if (_pointerSession != null) _discardPointerMoves();
      _pointerSession = session;
      _pointerTexture = session.textureId;
    }
  }

  void _sendPointerMove(double x, double y) {
    final session = _pointerSession;
    if (!mounted ||
        _transferBlocked ||
        session == null ||
        !identical(session, _session) ||
        session.closed ||
        session.resizing ||
        !session.captured ||
        session.textureId != _pointerTexture ||
        !_focus.hasFocus) {
      _discardPointerMoves();
      return;
    }
    if (session.independentOutputs && !_trackPointerLayoutReady) {
      _release();
      return;
    }
    session.input({
      'type': 'input',
      'event': 'move',
      'x': x,
      'y': y,
      if (_pointerTrack != null) 'track': _pointerTrack,
    });
  }

  /// The session bar's rectangle in window coordinates, if it is laid out.
  Rect? _toolbarAnchor() {
    final box = _toolbarKey.currentContext?.findRenderObject();
    final bounds = _toolbarBounds;
    if (box is! RenderBox || !box.attached || bounds == null) return null;
    return bounds.shift(box.localToGlobal(Offset.zero));
  }

  /// Streaming settings, then a reconnect with the applied options. From
  /// the bar ([anchor]) they drop down as a panel under the button; without
  /// one (the after-monitor-change card) they open as a dialog. Resolves
  /// true when new options were applied.
  Future<bool> _settings({Rect? anchor, VoidCallback? onBack}) async {
    if (_transferBlocked) return false;
    final previous = _session;
    if (widget.onReconnect == null ||
        _reconnecting ||
        _settingsOpen ||
        _closingWindow ||
        !_retryAllowed ||
        !(previous?.canRetry ?? true)) {
      return false;
    }
    final attempt = _connectionAttempt, intent = _connectionIntent;
    final desktopId = _machine.id;
    final displayReconnect =
        previous == null && _displayDisconnected && _displayConfigured;
    _release();
    _settingsOpen = true;
    _holdPointer();
    try {
      final initial = previous?.connection.options ?? _lastOptions;
      // The no-sound-device line only when the Host said so.
      final missingAudio = previous?.remoteAudioMissing == true;
      final track = _popups.tracker();
      final options =
          await (anchor == null
                  ? showDialog<StreamOptions>(
                      context: context,
                      builder: (_) => track(
                        StreamSettingsDialog(
                          machine: _machine,
                          initialOptions: initial,
                          reconnect: true,
                          allowUnchangedReconnect: displayReconnect,
                          audioDeviceMissing: missingAudio,
                        ),
                      ),
                    )
                  : showStreamSettingsPanel(
                      context,
                      anchor: anchor,
                      machine: _machine,
                      initialOptions: initial,
                      audioDeviceMissing: missingAudio,
                      popups: _popups,
                      onBack: onBack,
                    ))
              .whenComplete(_unholdPointer);
      if (options == null ||
          !mounted ||
          _transferBlocked ||
          _closingWindow ||
          !identical(previous, _session) ||
          attempt != _connectionAttempt ||
          intent != _connectionIntent ||
          widget.machine.id != desktopId ||
          !_retryAllowed ||
          !(previous?.canRetry ?? true)) {
        return false;
      }
      // Encoding preferences preserve the user's automatic or fixed workspace.
      await _load(reconnect: true, options: options, newIntent: true);
      return true;
    } finally {
      _settingsOpen = false;
    }
  }

  void _stats() {
    _release();
    _holdPointer();
    showDialog<void>(
      context: context,
      builder: (_) => SessionStatsDialog(
        title: l10n.connectionStats,
        machine: _machine.name,
        observationEnabled: () => _session?.observabilityEnabled == true,
        onObservationChanged: (enabled) =>
            _session?.setObservabilityEnabled(enabled),
        reportJson: () => _session?.observabilityJson() ?? '{}',
        note: l10n.detailedDiagnosticsOffByDefault,
        // Read on every refresh: the current connection, never the one
        // that was up when the dialog opened.
        sections: () => streamMetricSections(
          _session,
          options: _lastOptions,
          autoViewport: _autoViewport,
          desired: _desiredViewport,
        ),
      ),
    ).whenComplete(_unholdPointer);
  }

  @override
  void initState() {
    super.initState();
    widget.windowActions?.bind(
      this,
      confirm: _confirmDisconnect,
      retire: _retireView,
    );
    WidgetsBinding.instance.addObserver(this);
    GestureBinding.instance.pointerRouter.addGlobalRoute(_globalPointer);
    _presentation.addListener(_presentationChanged);
    _presentation.onMaximizeRequested = _maximizeRequested;
    _presentation.onSizeMove = _sizeMoveChanged;
    if (_pointerControl) unawaited(_forgetPointerConfinement());
    if (_displayMode != SessionDisplayMode.allMonitors) {
      unawaited(_probeLocalDisplays());
    }
    unawaited(_load(newIntent: true));
  }

  Future<bool> _requestSessionGuide({Rect? anchor}) async {
    final session = _session;
    if (!mounted ||
        _closingWindow ||
        _manualGuideOpen ||
        !widget.sessionGuideEnabled ||
        session == null ||
        session.closed ||
        _transferBlocked) {
      return false;
    }
    // The first-connection hint and the guide never show together.
    _dismissCoach();
    // This is an ordinary toolbar panel, like USB and shared folders. It must
    // not inherit the automatic notice's lifecycle, preference/read revision,
    // input-idle or first-frame gates. Native release is asynchronous; it does
    // not gate painting this read-only panel or closing it.
    setState(() => _manualGuideOpen = true);
    _dismissSessionGuide();
    _popups.closeAll();
    _release();
    _holdPointer();
    final route = createSessionGuidePanelRoute(
      context,
      anchor: anchor,
      machine: _machine.name,
      session: session,
      showPreference: false,
      popups: _popups,
    );
    _guideRoute = route;
    try {
      await Navigator.of(context, rootNavigator: true).push(route);
    } finally {
      if (identical(_guideRoute, route)) _guideRoute = null;
      _unholdPointer();
      if (mounted) setState(() => _manualGuideOpen = false);
    }
    return false;
  }

  void _dismissSessionGuide() {
    final route = _guideRoute;
    if (route == null) return;
    void remove() {
      if (route.isActive) route.navigator?.removeRoute(route);
    }

    final phase = SchedulerBinding.instance.schedulerPhase;
    if (phase == SchedulerPhase.idle ||
        phase == SchedulerPhase.postFrameCallbacks) {
      remove();
    } else {
      WidgetsBinding.instance.addPostFrameCallback((_) => remove());
    }
  }

  @override
  void didUpdateWidget(covariant RemoteView oldWidget) {
    super.didUpdateWidget(oldWidget);
    if (oldWidget.guideAccountKey != widget.guideAccountKey ||
        oldWidget.machine.id != widget.machine.id ||
        !identical(oldWidget.pointerMemory, widget.pointerMemory) ||
        oldWidget.sessionGuideEnabled != widget.sessionGuideEnabled) {
      _dismissSessionGuide();
    }
    if (oldWidget.guideAccountKey != widget.guideAccountKey ||
        oldWidget.takeoverServer != widget.takeoverServer) {
      // The card listed this offer for the login and server it came to. With
      // no dialog between the card and the request, nobody else may accept
      // it: the card goes back to a plain 다시 연결.
      _takeoverOffer = null;
    }
  }

  bool get _canTakeover =>
      _takeoverAllowed &&
      _error != null &&
      _takeoverOffer?.desktopId == _machine.id &&
      widget.onConfirmTakeover != null;

  ConnectionRecovery get _recovery {
    final session = _session;
    if (_closingWindow ||
        _reconnecting ||
        _transferBlocked ||
        _presentationChanging ||
        _canTakeover ||
        (_error == null && session?.error == null && session?.closed != true)) {
      return ConnectionRecovery.none;
    }
    return connectionRecovery(
      _requestErrorCode ?? session?.diagnosticCode,
      canRetry:
          _retryAllowed &&
          (session?.canRetry ?? true) &&
          widget.onReconnect != null,
      takeover: _canTakeover,
    );
  }

  ConnectionWaitStep get _observedWaitStep {
    // Waiting for the Host's update only while there is no connection: once
    // the new connection is made the list goes down its stages (it must not
    // stand on this row until the picture comes).
    if (_hostUpdating && (_session == null || _session!.closed)) {
      return ConnectionWaitStep.hostUpdate;
    }
    if (_plannedRestart && _session == null) {
      return ConnectionWaitStep.vmRestart;
    }
    // What this connection is observed doing, in the order of the rows. The
    // window-size entry does not answer 가상 화면 준비 by itself: it lasts
    // from before the request until the picture is fitted, so the list would
    // stand on that row for the whole connection and reach no other stage.
    final session = _session;
    if (session != null && !session.closed) {
      // Connected, and the Host is setting the virtual display this
      // connection asked for; ends when the stream's size arrives.
      if (session.preparingInitialDisplay) return ConnectionWaitStep.display;
      // A decoded picture that is still being fitted to the window is not
      // shown yet: it stays on the last row, never back up the list.
      if (!session.frameReady || _initialViewportEntry != null) {
        return session.width > 0 ||
                session.height > 0 ||
                session.textureId != null ||
                session.receivedBytes > 0
            ? ConnectionWaitStep.firstFrame
            : ConnectionWaitStep.relay;
      }
    }
    return _waitStep;
  }

  Widget _waitStatus() => ConnectionWaitStatus(
    step: _observedWaitStep,
    attempt: _connectionAttempt,
    elapsed: () => _waitClock.elapsed,
  );

  void _waitingFor(ConnectionWaitStep step, int attempt) {
    if (mounted && !_closingWindow && attempt == _connectionAttempt) {
      setState(() => _waitStep = step);
    }
  }

  Future<void> _recoverySettings() async {
    final recovery = _recovery;
    if (recovery == ConnectionRecovery.none || _settingsOpen) return;
    if (recovery == ConnectionRecovery.quality) {
      await _settings();
      return;
    }
    final previous = _session;
    final attempt = _connectionAttempt, intent = _connectionIntent;
    final desktopId = _machine.id;
    bool current() =>
        mounted &&
        !_closingWindow &&
        !_transferBlocked &&
        !_reconnecting &&
        identical(previous, _session) &&
        attempt == _connectionAttempt &&
        intent == _connectionIntent &&
        widget.machine.id == desktopId &&
        _recovery == recovery;
    if (!current()) return;
    _release();
    _holdPointer();
    _settingsOpen = true;
    var draft = _displayMode == SessionDisplayMode.allMonitors
        ? SessionDisplayMode.windowed
        : _displayMode;
    final track = _popups.tracker();
    try {
      final mode = await showDialog<SessionDisplayMode>(
        context: context,
        builder: (_) => track(
          StatefulBuilder(
            builder: (context, change) => AppDialog(
              key: const Key('recovery-display-settings'),
              title: l10n.screenSettings,
              subtitle: _machine.name,
              icon: Icons.monitor_rounded,
              maxWidth: 420,
              body: [
                KeepAllText(
                  l10n.remoteResolutionPreparedAgainFit,
                ),
                const SizedBox(height: 16),
                SegmentedButton<SessionDisplayMode>(
                  key: const Key('recovery-display-mode'),
                  showSelectedIcon: false,
                  segments: [
                    ButtonSegment(
                      value: SessionDisplayMode.windowed,
                      label: KeepAllText(l10n.windowed),
                    ),
                    ButtonSegment(
                      value: SessionDisplayMode.fullscreen,
                      label: KeepAllText(l10n.fullScreen),
                    ),
                  ],
                  selected: {draft},
                  onSelectionChanged: (values) =>
                      change(() => draft = values.single),
                ),
              ],
              actions: [
                TextButton(
                  key: const Key('cancel-recovery-display'),
                  onPressed: () => Navigator.pop(context),
                  child: KeepAllText(l10n.cancel),
                ),
                FilledButton(
                  key: const Key('apply-recovery-display'),
                  onPressed: () => Navigator.pop(context, draft),
                  child: KeepAllText(l10n.reconnectMode),
                ),
              ],
            ),
          ),
        ),
      );
      if (mode == null || !current()) return;
      if (mode == _displayMode) {
        await _load(reconnect: true, newIntent: true);
      } else {
        await _transitionDisplayMode(mode);
      }
    } finally {
      _unholdPointer();
      _settingsOpen = false;
    }
  }

  /// The card's "끊고 여기서 연결": bound, when the card is built, to the
  /// offer whose connections the card lists.
  VoidCallback _acceptShownTakeover() {
    final shown = _takeoverOffer;
    return () => unawaited(_takeover(shown));
  }

  /// The card is the one question, never a second one after it: it names the
  /// accounts and start times of the exact broker offer, and its button
  /// accepts that offer, [shown]. Accepting never permits an unbound legacy
  /// release; the broker still checks expiry and occupancy CAS.
  Future<void> _takeover(StreamTakeoverOffer? shown) async {
    final takeover = widget.onConfirmTakeover, offer = _takeoverOffer;
    if (!_canTakeover ||
        takeover == null ||
        offer == null ||
        !identical(offer, shown) ||
        _reconnecting ||
        _closingWindow ||
        _takeoverRunning) {
      return;
    }
    final attempt = _connectionAttempt, intent = _connectionIntent;
    final desktopId = _machine.id;
    final account = widget.guideAccountKey, server = widget.takeoverServer;
    bool current() =>
        mounted &&
        !_closingWindow &&
        _takeoverRunning &&
        _connectionAttempt == attempt &&
        _connectionIntent == intent &&
        _machine.id == desktopId &&
        widget.machine.id == desktopId &&
        widget.guideAccountKey == account &&
        widget.takeoverServer == server &&
        identical(_takeoverOffer, offer);
    // Set before anything is awaited: a second press finds it and returns.
    _takeoverRunning = true;
    _release();
    _holdPointer();
    try {
      if (!offer.currentAt(DateTime.now())) {
        throw const BrokerException('takeover_context_expired', 410);
      }
      setState(() {
        _reconnecting = true;
        _error = null;
        _requestErrorCode = null;
      });
      await takeover(offer, current).timeout(const Duration(seconds: 15));
      if (!current()) return;
      _takeoverOffer = null;
      _reconnecting = false;
      // Ordinary admission retains the Host cleanup barrier and fresh rights.
      await _load(reconnect: true, newIntent: true);
    } catch (error) {
      if (!current()) return;
      final failure = SessionFailure.request(error);
      setState(() {
        _reconnecting = false;
        _error = failure.message;
        _requestErrorCode = error is BrokerException
            ? error.code
            : failure.code;
        _retryAllowed = failure.canRetry;
        _takeoverAllowed = failure.takeover;
        _takeoverOffer = error is BrokerException ? error.takeoverOffer : null;
      });
    } finally {
      _takeoverRunning = false;
      _unholdPointer();
    }
  }

  Future<void> _load({
    bool reconnect = false,
    StreamOptions? options,
    bool newIntent = false,
    bool initialPreparationRetry = false,
  }) async {
    if (_transferBlocked) return;
    if (_reconnecting || _closingWindow) return;
    if (initialPreparationRetry &&
        (!identical(_session, _initialPreparationSource) ||
            _intentHasStreamed)) {
      return;
    }
    final preparationSource = initialPreparationRetry
        ? _initialPreparationSource
        : null;
    _serverRestartRetry?.cancel();
    if (newIntent) {
      _serverRestart.reset();
      ++_connectionIntent;
      _intentHasStreamed = _initialPreparationRetryUsed = false;
      _initialPreparationSource = null;
    }
    _dismissSessionGuide();
    final attempt = ++_connectionAttempt;
    if (newIntent || !_waitClock.isRunning || _session?.hasStreamed == true) {
      _waitClock
        ..reset()
        ..start();
    }
    final intent = _connectionIntent, desktopId = _machine.id;
    bool current() =>
        mounted &&
        !_closingWindow &&
        attempt == _connectionAttempt &&
        intent == _connectionIntent &&
        _machine.id == desktopId &&
        widget.machine.id == desktopId;
    final previous = _session;
    _clearInitialViewportEntry();
    final waitForPreviousDisplay =
        previous != null &&
        _machine.virtualMonitorControl &&
        _machine.display.supported &&
        (widget.prepareDisplaysOnEntry ||
            _displayMode == SessionDisplayMode.allMonitors ||
            previous.independentOutputs);
    _release();
    _unbindPointerOrdering?.call();
    _unbindPointerOrdering = null;
    previous?.removeListener(_changed);
    _forgetClipboard();
    _forgetPrinter();
    _forgetGamepads();
    setState(() {
      _reconnecting = true;
      _error = null;
      _requestErrorCode = null;
      // An attempt starts at 연결 요청, the first row: reading the desktop,
      // the window and its size are what the request is made from.
      _waitStep = reconnect
          ? ConnectionWaitStep.reconnect
          : ConnectionWaitStep.request;
      _session = null;
      _retryAllowed = true;
      _takeoverAllowed = false;
      _takeoverOffer = null;
      _displayDisconnected = _displayConfigured = false;
      _viewportDebounce?.cancel();
      _desiredViewport = _attemptedViewport = _viewportAttemptTarget = null;
      _groupLayoutDebounce?.cancel();
      _groupLayoutNeeded = false;
      _localLayoutVerified = false;
      _localLayoutObservation++;
      _viewportAttempts = 0;
      if (options != null) _lastOptions = options;
    });
    var abandoned = false;
    var preparationStage = 1;
    // connect-timing-v1: each preparation step's time, given to the session.
    final stageWatch = Stopwatch()..start();
    var stageMark = 0;
    final connectStages = <String, int>{};
    void stage(String name) {
      final now = stageWatch.elapsedMicroseconds;
      final ms = connectStageMs(now - stageMark);
      if (ms != null) connectStages[name] = ms;
      stageMark = now;
    }

    try {
      _transitionStage(ScreenTransitionStage.retiring);
      await previous?.close();
      // Its USB devices went back to this PC with it; the next session takes
      // them again (_watchUsb).
      final usbBefore = previous?.usb?.sharedAtEnd ?? const <String>[];
      if (usbBefore.isNotEmpty) _usbResume = usbBefore;
      // Its shared folders ended with it and are not shared again by
      // themselves (_watchDrive says so).
      if (previous?.drive?.sharesAtEnd.isNotEmpty ?? false) {
        _driveEndedByReconnect = true;
      }
      if (!current()) return;
      if (reconnect && previous != null) {
        // EOF can leave its exact old reservation alive until the relay
        // notices the lost network. Never terminate a different session.
        try {
          await previous.connection.releaseAgain();
        } on BrokerException catch (error) {
          if (error.status == 401 || error.status == 403) rethrow;
        } catch (_) {}
        if (!current()) return;
      }
      stage('retireMs');
      final checkTakeover = widget.onCheckTakeover;
      if (checkTakeover != null) {
        await checkTakeover().timeout(const Duration(seconds: 20));
        if (!current()) return;
      }
      final refresh = widget.onRefreshDesktop;
      if (refresh != null) {
        _transitionStage(ScreenTransitionStage.hostReady);
        preparationStage = 2;
        final transportRecovery = _hostLossRecovery;
        final recovering = transportRecovery || initialPreparationRetry;
        final recoveryWait = transportRecovery
            ? _hostLossRecoveryUntil!.difference(DateTime.now())
            : null;
        var fresh = transportRecovery
            ? _machine
            : await refresh().timeout(const Duration(seconds: 8));
        if (!current()) return;
        if (fresh.id != _machine.id) {
          throw const BrokerException('invalid_response');
        }
        // After the relay lost the Host, also a view that prepares no
        // monitor count waits quietly until the Host streams again.
        if (waitForPreviousDisplay ||
            fresh.sessionCleanup.pending ||
            recovering) {
          fresh = await waitForStreamDisplayControl(
            fresh,
            refresh: refresh,
            current: current,
            recovering: recovering,
            countControl: !recovering || waitForPreviousDisplay,
            initialPreparation: initialPreparationRetry,
            independentTracks: _displayMode == SessionDisplayMode.allMonitors,
            refreshFirst: transportRecovery,
            retryGatewayErrors: transportRecovery,
            restarting: transportRecovery && _plannedRestart,
            hostUpdating: transportRecovery && _hostUpdating,
            maxWait:
                recoveryWait ??
                (initialPreparationRetry ? const Duration(seconds: 30) : null),
            waitHost: transportRecovery ? widget.onWaitHostReady : null,
            // Windows restarting from inside the VM: the Host is gone while
            // the VM runs. Wait like a restart this window asked for.
            // A server restart takes every Host away for a moment; it keeps
            // its own wait and text.
            hostAwayWait:
                transportRecovery &&
                    !_plannedRestart &&
                    !_hostUpdating &&
                    !_serverRestart.waiting &&
                    previous?.diagnosticCode != 'server_stopping'
                ? plannedRestartWait
                : null,
            onHostAway: () {
              if (!mounted || !current()) return;
              setState(() {
                _plannedRestartUntil = DateTime.now().add(plannedRestartWait);
                _plannedRestartFrom = previous;
                _hostLossRecoveryUntil = _plannedRestartUntil;
                _hostAwayRestart = true;
              });
            },
          );
          if (!current()) return;
        }
        _machine = fresh;
        _entryDisplaysPrepared = false;
      }
      // Entry from the main list already offers this step. A surviving remote
      // window must perform the same fresh check on retry after an RDP session.
      final resume = widget.onPrepareConsole;
      if (!_machine.isReady && _machine.canRecoverConsole && resume != null) {
        if (!mounted || !current()) return;
        setState(() => _consoleDialogOpen = true);
        DesktopMachine? fresh;
        var cancelled = false;
        try {
          _holdPointer();
          // Retrying replaces the failed toolbar content and closes its old
          // popups. Let that frame finish before registering a new consent
          // route, so the old content cannot immediately retire this dialog.
          await WidgetsBinding.instance.endOfFrame;
          if (!mounted || !current() || _transferBlocked) return;
          // A retry may observe a new RDP session after this window opened.
          // Consent is required here too, before the progress dialog starts
          // its Windows handover request.
          final track = _popups.tracker();
          final consent =
              await showDialog<bool>(
                context: context,
                builder: (_) => track(
                  AppConfirmDialog(
                    key: const Key('rdp-handover-confirm'),
                    confirmKey: const Key('rdp-handover-accept'),
                    cancelKey: const Key('rdp-handover-cancel'),
                    title: l10n.takeOverWindowsScreen,
                    subtitle: _machine.name,
                    icon: Icons.swap_horiz_rounded,
                    tone: AppTone.warn,
                    message:
                        l10n.currentWindowsScreenMovesStreaming,
                    confirmLabel: l10n.takeOverScreen,
                  ),
                ),
              ) ==
              true;
          if (!current() || _transferBlocked) return;
          if (!consent) {
            setState(() {
              _error = l10n.connectionCanceled;
              _requestErrorCode = 'operation_cancelled';
              _retryAllowed = true;
              _takeoverAllowed = false;
            });
            return;
          }
          if (!mounted || !_machine.canRecoverConsole) {
            throw const BrokerException('desktop_changed');
          }
          fresh = await showDialog<DesktopMachine>(
            context: context,
            barrierDismissible: false,
            builder: (_) => track(
              ConsoleResumeDialog(
                machine: _machine,
                resume: () {
                  final prepare = widget.onPrepareConsole;
                  if (!current() ||
                      _transferBlocked ||
                      !_machine.canRecoverConsole ||
                      prepare == null) {
                    throw const BrokerException('desktop_changed');
                  }
                  return prepare();
                },
                onCancelled: () => cancelled = true,
              ),
            ),
          );
        } finally {
          _unholdPointer();
          if (mounted) setState(() => _consoleDialogOpen = false);
        }
        if (!current()) return;
        if (fresh == null) {
          if (cancelled) {
            setState(() {
              _error = l10n.connectionCanceled;
              _requestErrorCode = 'operation_cancelled';
              _retryAllowed = true;
              _takeoverAllowed = false;
            });
            return;
          }
          throw const BrokerException('console_transfer_failed');
        }
        if (fresh.id != _machine.id || !fresh.isReady) {
          throw const BrokerException('invalid_response');
        }
        _machine = fresh;
        _entryDisplaysPrepared = false;
      }
      // Initial activation and an explicitly confirmed Windows return can
      // both precede the Host's fresh viewport/count capability observation.
      // Use the same bounded, read-only wait as reconnect, after any return.
      if (refresh != null &&
          (widget.prepareDisplaysOnEntry ||
              _displayMode == SessionDisplayMode.allMonitors)) {
        _machine = await waitForStreamDisplayControl(
          _machine,
          refresh: refresh,
          current: current,
          recovering: _hostLossRecovery,
          independentTracks: _displayMode == SessionDisplayMode.allMonitors,
        );
        if (!current()) return;
      }
      stage('refreshMs');
      preparationStage = 3;
      if (_displayMode == SessionDisplayMode.allMonitors &&
          (widget.onConnectTracks == null ||
              !_machine.independentDisplayStreaming)) {
        throw const BrokerException('track_streaming_unavailable');
      }
      if (!_presentationReady) {
        _transitionStage(ScreenTransitionStage.windowMode);
        if (_displayMode == SessionDisplayMode.allMonitors) {
          // Measure only: the window is spread over both monitors next, and
          // the layout is read again with the cover check after that.
          _localLayout = await LocalDisplayLayout.read(covered: false);
          if (!current()) return;
          _allMonitorsAvailable = true;
        }
        await _presentation
            .enter(
              fullscreen: _displayMode != SessionDisplayMode.windowed,
              allMonitors: _displayMode == SessionDisplayMode.allMonitors,
            )
            .timeout(const Duration(seconds: 5));
        if (!current()) return;
        // Native sizing completes before connection and one Flutter frame
        // receives the resulting viewport. Actual output resize still needs
        // the host's negotiated viewport capability and fresh media evidence.
        await WidgetsBinding.instance.endOfFrame;
        if (!current()) return;
        _presentationReady = true;
        // A separate remote window treats maximize as fullscreen.
        if (widget.windowActions != null) {
          unawaited(_presentation.maximizeToFullscreen(true));
        }
      }
      stage('presentMs');
      preparationStage = 4;
      _transitionStage(ScreenTransitionStage.outputs);
      if ((widget.prepareDisplaysOnEntry ||
              _displayMode == SessionDisplayMode.allMonitors) &&
          !_entryDisplaysPrepared) {
        final fresh = await prepareStreamDisplayCount(
          _machine,
          _displayMode == SessionDisplayMode.allMonitors ? 2 : 1,
          widget.onConfigureDisplays,
        );
        if (!current()) return;
        _machine = fresh;
        _entryDisplaysPrepared = true;
      }
      if (_displayMode == SessionDisplayMode.allMonitors) {
        // Count preparation can take several seconds. Re-read the actual
        // local screens instead of using a layout from before a hot-plug.
        final observation = _localLayoutObservation;
        _localLayout = await LocalDisplayLayout.read();
        if (!current()) return;
        _localLayoutVerified = observation == _localLayoutObservation;
        _verifiedMetrics = _windowMetrics;
        if (!_localLayoutVerified) _groupLayoutNeeded = true;
        _lastOptions = _localLayout!.options(_lastOptions);
      } else if (widget.prepareDisplaysOnEntry) {
        _lastOptions = _lastOptions.forSingleDisplay();
      }
      stage('outputsMs');
      InitialViewport? initialViewport;
      preparationStage = 5;
      if (_displayMode != SessionDisplayMode.allMonitors &&
          _machine.initialViewport) {
        if (widget.onConnectViewport == null) {
          throw const BrokerException('initial_viewport_unavailable');
        }
        await WidgetsBinding.instance.endOfFrame;
        if (!current()) return;
        final viewport = _viewportKey.currentContext;
        final box = viewport?.findRenderObject();
        final automaticPixels =
            viewport != null &&
                viewport.mounted &&
                box is RenderBox &&
                box.hasSize
            ? viewportResolution(
                box.size,
                MediaQuery.devicePixelRatioOf(viewport),
                portrait: _machine.streamSize && _machine.viewportEncoderLimits,
              )
            : null;
        final pixels = automaticPixels;
        if (pixels == null) {
          throw const BrokerException('unsupported_display_mode');
        }
        initialViewport = InitialViewport(
          preserveEncoderLimits: _machine.viewportEncoderLimits,
          width: pixels.width.toInt(),
          height: pixels.height.toInt(),
          fps: _lastOptions.fps,
        );
        _initialViewportEntry = InitialViewportEntry((
          initialViewport.width,
          initialViewport.height,
        ));
      }
      stage('viewportMs');
      preparationStage = 6;
      _transitionStage(ScreenTransitionStage.connecting);
      _waitingFor(ConnectionWaitStep.request, attempt);
      final request = _displayMode == SessionDisplayMode.allMonitors
          ? _scalePreference != null && widget.onConnectTracksScaled != null
                ? widget.onConnectTracksScaled!(
                    _localLayout!.plan,
                    _lastOptions,
                    _scalePreference!,
                  )
                : widget.onConnectTracks!(_localLayout!.plan, _lastOptions)
          : _scalePreference != null && widget.onConnectScaled != null
          ? widget.onConnectScaled!(
              initialViewport,
              _lastOptions,
              _scalePreference!,
            )
          : initialViewport != null
          ? widget.onConnectViewport!(initialViewport, _lastOptions)
          : (reconnect || widget.prepareDisplaysOnEntry) &&
                widget.onReconnect != null
          ? widget.onReconnect!(_lastOptions)
          : widget.load();
      final session = await request
          .then((session) {
            if (abandoned || !current()) {
              unawaited(session.close());
            }
            return session;
          })
          .timeout(const Duration(seconds: 20));
      if (!current()) return;
      _serverRestart.reset();
      stage('connectMs');
      session.connectTiming.before.addAll(connectStages);
      setState(() {
        _session = session;
        _waitStep = ConnectionWaitStep.relay;
        // Every new connection starts with 마우스 잠금 off.
        _gameMouse = false;
        _lastOptions = session.connection.options;
      });
      _unbindPointerOrdering = session.bindPointerOrdering(
        beforeKey: _pointerMoves.flush,
        discard: _discardPointerMoves,
      );
      session.addListener(_changed);
      _transitionStage(ScreenTransitionStage.firstPicture);
      unawaited(session.start());
      _startGamepads(session);
    } catch (error) {
      if (current()) {
        _clearInitialViewportEntry();
        // A Host update's wait: the broker refusing while that update still
        // runs is expected, not a failure; the next try follows shortly.
        if (_hostUpdating &&
            error is BrokerException &&
            error.code == 'host_update_in_progress') {
          _serverRestartRetry = Timer(hostUpdatePollInterval, () {
            if (mounted &&
                !_closingWindow &&
                attempt == _connectionAttempt &&
                intent == _connectionIntent) {
              unawaited(_load(reconnect: reconnect));
            }
          });
          return;
        }
        var failure = !_presentationReady && error is! BrokerException
            ? SessionFailure(
                'presentation_failed',
                _displayMode == SessionDisplayMode.allMonitors
                    ? l10n.couldNotConnectAllMonitors
                    : l10n.couldNotApplyScreenMode2,
                canRetry: _displayMode != SessionDisplayMode.allMonitors,
              )
            : SessionFailure.request(error);
        ProductLog.instance.recordPreparationFailure(
          LogEvent.streamEnd,
          error is BrokerException
              ? error.code
              : failure.code == 'connection_timeout'
              ? 'network_error'
              : failure.code,
          {'ok': false, 'stage': preparationStage},
        );
        final wait = _serverRestart.next(error);
        if (wait != null) {
          // The server restarts (an update): one line, then this connect
          // tries again by itself, without a second press.
          setState(() => _screenTransitionStage = null);
          _serverRestartRetry = Timer(wait, () {
            if (mounted &&
                !_closingWindow &&
                attempt == _connectionAttempt &&
                intent == _connectionIntent) {
              unawaited(_load(reconnect: reconnect));
            }
          });
          return;
        }
        // A Host update's wait ended without its Host: the manual text.
        if (_hostUpdateTimer != null) {
          _endHostUpdateWait();
          if (error is BrokerException &&
              const {
                'host_update',
                'host_update_in_progress',
                'host_offline',
                'host_not_registered',
                'host_maintenance',
              }.contains(error.code)) {
            failure = SessionFailure.ended('host_update');
          }
        }
        setState(() {
          _screenTransitionStage = null;
          _error = failure.message;
          _requestErrorCode = error is BrokerException
              ? error.code
              : failure.code;
          _retryAllowed = failure.canRetry;
          _takeoverAllowed = failure.takeover;
          _takeoverOffer = error is BrokerException
              ? error.takeoverOffer
              : null;
        });
      }
    } finally {
      abandoned = true;
      if (current()) {
        setState(() {
          _reconnecting = false;
          if (identical(_initialPreparationSource, preparationSource)) {
            _initialPreparationSource = null;
          }
        });
        if (_session?.closed == true) _changed();
      }
    }
  }

  bool _audioMissingShown = false;
  String? _resizeErrorShown;
  RemoteSession? _displayChangeHandled;
  DateTime? _displayChangeReconnectAt;
  RemoteSession? _hostLossHandled;
  DateTime? _hostLossReconnectAt, _hostLossRecoveryUntil;

  /// Within 30 s of losing the Host's connection a (re)connection waits
  /// quietly while the Host returns and restores its display.
  bool get _hostLossRecovery =>
      _hostLossRecoveryUntil?.isAfter(DateTime.now()) == true;

  /// An explicit initial Host-preparation end gets one read-only wait and one
  /// normal connection request. Internal reloads cannot replenish this budget.
  /// Existing/other controllers are never stopped here, and a stream that was
  /// actually ready in this user intent keeps the ordinary end policy.
  void _watchInitialPreparation() {
    final session = _session;
    if (session?.hasStreamed == true) _intentHasStreamed = true;
    if (session == null ||
        !session.awaitingInitialHostPreparation ||
        _intentHasStreamed ||
        _initialPreparationRetryUsed ||
        !_retryAllowed ||
        _transferBlocked ||
        _closingWindow ||
        _reconnecting ||
        widget.onReconnect == null ||
        widget.onRefreshDesktop == null) {
      return;
    }
    _initialPreparationRetryUsed = true;
    _initialPreparationSource = session;
    final attempt = _connectionAttempt, intent = _connectionIntent;
    final desktopId = _machine.id;
    WidgetsBinding.instance.addPostFrameCallback((_) {
      if (!mounted ||
          _closingWindow ||
          _reconnecting ||
          _transferBlocked ||
          attempt != _connectionAttempt ||
          intent != _connectionIntent ||
          widget.machine.id != desktopId ||
          !identical(session, _session)) {
        if (mounted && identical(_initialPreparationSource, session)) {
          setState(() => _initialPreparationSource = null);
        }
        return;
      }
      unawaited(_load(reconnect: true, initialPreparationRetry: true));
    });
  }

  /// A dropped transport or an explicitly lost Host reconnects once and uses
  /// the same fresh readiness/cleanup gates. Only an explicit Host loss says
  /// that the Host disconnected. This avoids a failure card and a retry that
  /// fails as display_not_supported while the Host is still recovering.
  /// A second loss within a minute leaves the ordinary card; its 재연결
  /// still waits quietly.
  void _watchHostLoss() {
    final session = _session;
    // A Host update ended it: wait for the update and the Host within
    // [hostUpdateWait], then reconnect. Any other end keeps its own rule.
    if (session != null &&
        session.closed &&
        session.diagnosticCode == 'host_update' &&
        session.canRetry &&
        widget.onReconnect != null &&
        !identical(_hostLossHandled, session) &&
        !_closingWindow &&
        !_reconnecting) {
      _hostLossHandled = session;
      _hostUpdateTimer?.cancel();
      _hostUpdateTimer = Timer(
        hostUpdateWait,
        () => _stopHostUpdateWait(cancelled: false),
      );
      _hostLossRecoveryUntil = DateTime.now().add(hostUpdateWait);
      WidgetsBinding.instance.addPostFrameCallback((_) {
        if (!mounted || _closingWindow || !identical(session, _session)) return;
        unawaited(_load(reconnect: true));
      });
      return;
    }
    // A restart this window asked for: however the stream ended (the Host
    // went away with Windows), wait for the VM within the planned bound and
    // reconnect; only an authorization end stays an ordinary end.
    if (_plannedRestart &&
        session != null &&
        session.closed &&
        widget.onReconnect != null &&
        !identical(_hostLossHandled, session) &&
        !_closingWindow &&
        !_reconnecting &&
        !SessionFailure.blocksAutomaticReconnect(session.diagnosticCode)) {
      _hostLossHandled = session;
      _hostLossRecoveryUntil = _plannedRestartUntil;
      WidgetsBinding.instance.addPostFrameCallback((_) {
        if (!mounted || _closingWindow || !identical(session, _session)) return;
        unawaited(_load(reconnect: true));
      });
      return;
    }
    if (session == null ||
        !session.closed ||
        !session.canAutomaticallyReconnect ||
        widget.onReconnect == null ||
        identical(_hostLossHandled, session) ||
        _closingWindow ||
        _reconnecting) {
      return;
    }
    _hostLossHandled = session;
    final now = DateTime.now(), last = _hostLossReconnectAt;
    _hostLossRecoveryUntil = now.add(const Duration(seconds: 30));
    if (!_retryAllowed ||
        !session.canRetry ||
        (last != null && now.difference(last) < const Duration(minutes: 1))) {
      return;
    }
    _hostLossReconnectAt = now;
    // A broker update: the wait below says the server is restarting until a
    // new stream starts (restart recovery).
    if (session.diagnosticCode == 'server_stopping') _serverRestart.begin();
    WidgetsBinding.instance.addPostFrameCallback((_) {
      if (!mounted || _closingWindow || !identical(session, _session)) return;
      _showToast(
        session.endedByHostLoss
            ? l10n.hostConnectionLostReconnecting
            : l10n.connectionLostReconnecting,
        icon: Icons.sync_rounded,
        duration: const Duration(seconds: 4),
      );
      unawaited(_load(reconnect: true));
    });
  }

  /// A display change that could not be applied says so once (the stream
  /// keeps its size). One that ended the stream (the relay ends the session
  /// on any Host-side resize failure) reconnects once at the current window
  /// size instead of waiting for 재연결; a second one within a minute leaves
  /// the ordinary card. Logged as viewportResult recovery with the cause.
  void _watchDisplayChange() {
    final session = _session;
    if (session == null) return;
    final error = session.closed ? null : session.resizeError;
    if (error != _resizeErrorShown) {
      _resizeErrorShown = error;
      if (error != null) {
        WidgetsBinding.instance.addPostFrameCallback(
          (_) => _showToast(
            error,
            icon: Icons.aspect_ratio_rounded,
            duration: const Duration(seconds: 4),
          ),
        );
      }
    }
    if (!session.closed ||
        !session.endedByDisplayChange ||
        identical(_displayChangeHandled, session) ||
        _closingWindow ||
        _reconnecting) {
      return;
    }
    _displayChangeHandled = session;
    final now = DateTime.now(), last = _displayChangeReconnectAt;
    if (last != null && now.difference(last) < const Duration(minutes: 1)) {
      return;
    }
    _displayChangeReconnectAt = now;
    ProductLog.instance.recordFailure(
      LogEvent.viewportResult,
      session.diagnosticCode,
      {'ok': false, 'recovery': true},
    );
    WidgetsBinding.instance.addPostFrameCallback((_) {
      if (!mounted || _closingWindow || !identical(session, _session)) return;
      _showToast(
        l10n.couldNotChangeScreenSize,
        icon: Icons.aspect_ratio_rounded,
        duration: const Duration(seconds: 4),
      );
      unawaited(_load(reconnect: true));
    });
  }

  void _changed() {
    _syncPointerScope();
    _syncRelativeMouse();
    _watchClipboard();
    _watchPrinter();
    _watchDrive();
    _watchUsb();
    _watchInitialPreparation();
    _watchDisplayChange();
    _watchHostLoss();
    // Say once why there is no sound instead of a silent "waiting" status.
    final session = _session;
    final entry = _initialViewportEntry;
    if (entry != null && session != null) {
      if (session.closed || session.error != null) {
        _clearInitialViewportEntry();
      } else if (!_initialViewportMetricsPending &&
          entry.satisfied(
            sourceWidth: session.sourceWidth,
            sourceHeight: session.sourceHeight,
            decoded: session.frameReady,
            changing: session.resizing,
          )) {
        _clearInitialViewportEntry();
      }
    }
    // A planned restart ends once a new stream shows the VM again.
    if (_plannedRestartUntil != null &&
        session != null &&
        !identical(session, _plannedRestartFrom) &&
        session.frameReady &&
        !session.closed) {
      _plannedRestartUntil = _plannedRestartFrom = null;
      _hostAwayRestart = false;
    }
    // A Host update's wait ends once a new stream shows the VM again.
    if (_hostUpdateTimer != null &&
        session != null &&
        session.frameReady &&
        !session.closed) {
      _endHostUpdateWait();
    }
    if (_screenTransitionStage != null &&
        !_allMonitorsPreflight &&
        session != null &&
        (session.closed ||
            session.error != null ||
            (_screenTransitionStage == ScreenTransitionStage.firstPicture &&
                _initialViewportEntry == null &&
                session.frameReady &&
                !session.resizing))) {
      _screenTransitionStage = null;
    }
    if (!_audioMissingShown &&
        session != null &&
        session.remoteAudioMissing &&
        session.frameReady) {
      _audioMissingShown = true;
      // One plain line, no action: the administrator adds the device.
      WidgetsBinding.instance.addPostFrameCallback(
        (_) => _showToast(
          audioDeviceMissingText,
          icon: Icons.volume_off_rounded,
          duration: const Duration(seconds: 10),
        ),
      );
    }
    // Only a layout no timer waits for: session changes arrive about three
    // times a second, and restarting the 650 ms wait on each one postponed
    // the re-read for good (the outputs stayed behind "모니터 구성이 변경됐어요").
    if (_groupLayoutNeeded &&
        _groupLayoutDebounce?.isActive != true &&
        !_groupLayoutChanging &&
        _session?.frameReady == true &&
        _session?.resizing != true) {
      _scheduleGroupLayout();
    }
    if (mounted) {
      setState(() {});
      if (session != null) _queueInitialKeyboard(session);
    }
  }

  bool _initialKeyboardAllowed(RemoteSession session) {
    final focused = FocusManager.instance.primaryFocus;
    final lifecycle = WidgetsBinding.instance.lifecycleState;
    return identical(session, _session) &&
        _pointerFree &&
        _captureAllowed &&
        !session.resizing &&
        !_viewportChanging &&
        _viewportDebounce?.isActive != true &&
        _releaseInputPending == null &&
        _releaseInputContext == null &&
        (lifecycle == null || lifecycle == AppLifecycleState.resumed) &&
        (_focus.hasFocus || focused == null || focused is FocusScopeNode);
  }

  /// A newly opened native session window is already the user's input target.
  /// Claim the keyboard only after its real picture and final viewport exist.
  /// Windows may show/activate it just after that frame, so a refused native
  /// claim gets two bounded retries. A release, local control, menu, background
  /// transition or replacement retires this intent; it never steals focus back.
  void _queueInitialKeyboard(RemoteSession session) {
    if (!_pointerControl ||
        identical(_initialKeyboardFor, session) ||
        identical(_initialKeyboardQueued?.session, session) ||
        !_initialKeyboardAllowed(session)) {
      return;
    }
    final revision = _pointerRevision, attempt = _connectionAttempt;
    final queued = (session: session, revision: revision);
    _initialKeyboardQueued = queued;
    var claims = 0;
    bool current() =>
        mounted &&
        revision == _pointerRevision &&
        attempt == _connectionAttempt &&
        _initialKeyboardAllowed(session);
    late Future<void> Function() claim;
    claim = () async {
      _initialKeyboardRetry = null;
      if (!current() || session.captured) return;
      claims++;
      await _capture();
      if (!current() || session.captured || claims >= 3) return;
      _initialKeyboardRetry = Timer(
        const Duration(milliseconds: 150),
        () => unawaited(claim()),
      );
    };
    WidgetsBinding.instance.addPostFrameCallback((_) {
      if (_initialKeyboardQueued != queued) return;
      _initialKeyboardQueued = null;
      // The first ready notification may precede the build that discovers a
      // viewport settle. That natural deferral is not an attempted claim.
      if (!current()) return;
      _initialKeyboardFor = session;
      unawaited(claim());
    });
  }

  /// Maximize (button, caption double-click, Win+Up) in a remote window means
  /// "use the whole monitor": the same path as choosing fullscreen.
  void _maximizeRequested() {
    if (!mounted ||
        _transferBlocked ||
        _closingWindow ||
        _presentation.fullscreen ||
        _displayMode == SessionDisplayMode.allMonitors) {
      return;
    }
    unawaited(_setDisplayMode(true));
  }

  void _presentationChanged() {
    if (_displayRevision != _presentation.displayRevision) {
      _displayRevision = _presentation.displayRevision;
      unawaited(_probeLocalDisplays());
      _localLayoutVerified = false;
      _localLayoutObservation++;
      _scheduleGroupLayout();
    }
    _changed();
  }

  /// Each session window runs its own engine, so its view is the implicit one.
  (Size, double)? get _windowMetrics {
    final view = WidgetsBinding.instance.platformDispatcher.implicitView;
    return view == null ? null : (view.physicalSize, view.devicePixelRatio);
  }

  @override
  void didChangeMetrics() {
    final entry = _initialViewportEntry;
    if (entry != null) {
      _initialViewportMetricsPending = true;
      WidgetsBinding.instance.addPostFrameCallback((_) {
        if (!mounted || !identical(entry, _initialViewportEntry)) return;
        final viewport = _viewportKey.currentContext;
        final box = viewport?.findRenderObject();
        if (viewport != null && box is RenderBox && box.hasSize) {
          _observeViewport(box.size, MediaQuery.devicePixelRatioOf(viewport));
        }
        _initialViewportMetricsPending = false;
        _changed();
      });
    }
    if (_displayMode == SessionDisplayMode.allMonitors) {
      final metrics = _windowMetrics;
      final minimized = metrics?.$1.isEmpty ?? false;
      // Minimized, the window reports an empty view; restored, it comes back
      // with the size and scale its layout was verified at. Neither changes a
      // monitor: re-reading then would put "모니터 구성이 변경됐어요 … 다시
      // 선택하세요" over a live stream.
      final restored =
          metrics != null &&
          _localLayoutVerified &&
          metrics == _verifiedMetrics;
      if (minimized || restored) {
        if (minimized && _session?.captured == true) _release();
        _syncPointer();
        _syncCursorView();
        return;
      }
      _localLayoutVerified = false;
      _localLayoutObservation++;
      if (_session?.captured == true) _release();
    }
    _scheduleGroupLayout();
    _syncPointer();
    _syncCursorView();
  }

  void _scheduleGroupLayout() {
    if (!mounted ||
        _closingWindow ||
        _displayMode != SessionDisplayMode.allMonitors ||
        _session?.independentOutputs != true) {
      return;
    }
    if (_transferBlocked || _allMonitorsPreflight) {
      _groupLayoutNeeded = true;
      return;
    }
    if (_session?.captured == true) _release();
    _groupLayoutNeeded = true;
    _groupLayoutDebounce?.cancel();
    _groupLayoutDebounce = Timer(
      const Duration(milliseconds: 650),
      () => unawaited(_applyGroupLayout()),
    );
  }

  Future<void> _applyGroupLayout() async {
    final session = _session;
    if (!mounted ||
        _transferBlocked ||
        _allMonitorsPreflight ||
        _closingWindow ||
        _reconnecting ||
        _displayMode != SessionDisplayMode.allMonitors ||
        session == null ||
        session.closed ||
        !session.independentOutputs ||
        !session.frameReady ||
        session.resizing ||
        _groupLayoutChanging) {
      return;
    }
    _groupLayoutNeeded = false;
    _groupLayoutChanging = true;
    final observation = _localLayoutObservation;
    try {
      final layout = await LocalDisplayLayout.read();
      if (!mounted || _closingWindow || !identical(session, _session)) return;
      if (observation != _localLayoutObservation) {
        _groupLayoutNeeded = true;
        return;
      }
      if (_transferBlocked || _allMonitorsPreflight) {
        _groupLayoutNeeded = true;
        return;
      }
      // Ordinary PC tracks cannot resize in-place, but must still reobserve
      // monitor/DPI changes. A changed source plan stays blocked until a new
      // connection; never manufacture a Host resize or fresh capability.
      final applied = session.canResizeTracks
          ? await session.resizeTracks(layout.plan)
          : layout.matchesSources(session.tracks);
      if (!mounted || _closingWindow || !identical(session, _session)) return;
      if (observation != _localLayoutObservation) {
        _groupLayoutNeeded = true;
        return;
      }
      setState(() {
        _localLayout = layout;
        _localLayoutVerified = applied && layout.matchesSources(session.tracks);
        _verifiedMetrics = _windowMetrics;
      });
    } catch (_) {
      // An unplugged or unsupported layout cannot redirect input into another
      // output. Keep the layout warning and the window-mode action available.
    } finally {
      _groupLayoutChanging = false;
      if (mounted && identical(session, _session) && _groupLayoutNeeded) {
        _scheduleGroupLayout();
      }
    }
  }

  @override
  void didChangeAppLifecycleState(AppLifecycleState state) {
    // background-video-v1: minimised, the Host sends one picture a second;
    // shown again (focused or not), every frame.
    _session?.setBackground(
      state == AppLifecycleState.hidden || state == AppLifecycleState.paused,
    );
    // gamepad-v1: only the focused window's pads drive the desktop.
    _gamepads?.setActive(state == AppLifecycleState.resumed);
    if (state != AppLifecycleState.resumed) {
      _release();
      // A press whose release never comes back must not leave the toolbar
      // ignoring the pointer.
      _heldPointer = null;
      _pictureHeld.value = false;
      // Like Windows menus on deactivation: the bar's menus and panel close
      // and an unpinned bar folds (a pinned one stays). Closed here, not in
      // the next build: a minimized window builds again only once restored.
      // Except while the shared-folder panel's native folder picker or
      // confirmation is up: the picker itself takes the focus, and removing
      // the panel then lost the folder the person had just chosen.
      // (_focusLosses closes them again through the toolbar's layout key.)
      if (_session?.drive?.adding == true) return;
      _popups.closeAll();
      if (mounted && state != AppLifecycleState.detached) {
        setState(() => _focusLosses++);
      }
    }
  }

  bool get _captureAllowed =>
      _initialViewportEntry == null &&
      !_transferBlocked &&
      !_manualGuideOpen &&
      !_allMonitorsPreflight &&
      _screenTransitionStage == null &&
      _routeCurrent &&
      _pointerHolds == 0 &&
      !_settingsOpen &&
      !_consoleDialogOpen &&
      !_viewportChanging &&
      mounted &&
      !_closingWindow;

  Future<void> _capture() async {
    if (!_captureAllowed) return;
    final session = _session;
    if (session == null) return;
    final revision = _captureIntentRevision, attempt = _connectionAttempt;
    final release = identical(_releaseInputFor, session)
        ? _releaseInputPending
        : null;
    if (release != null) await release;
    final lifecycle = WidgetsBinding.instance.lifecycleState;
    if (!_captureAllowed ||
        !identical(session, _session) ||
        revision != _captureIntentRevision ||
        attempt != _connectionAttempt ||
        (lifecycle != null && lifecycle != AppLifecycleState.resumed)) {
      return;
    }
    _focus.requestFocus();
    await session.capture(true);
  }

  bool get _trackPointerLayoutReady {
    final session = _session, layout = _localLayout;
    final viewport = _viewportKey.currentContext;
    final box = viewport?.findRenderObject();
    return _localLayoutVerified &&
        session != null &&
        layout != null &&
        viewport != null &&
        box is RenderBox &&
        box.hasSize &&
        layout.matchesViewport(
          box.size,
          MediaQuery.devicePixelRatioOf(viewport),
        ) &&
        layout.matchesSources(session.tracks);
  }

  ({String? track, double x, double y})? _pointerPoint(
    PointerEvent event,
    double width,
    double height,
    String? track,
  ) {
    if (track != null) {
      if (!_trackPointerLayoutReady) {
        _release();
        return null;
      }
      final viewport = _viewportKey.currentContext;
      final box = viewport?.findRenderObject();
      final layout = _localLayout;
      if (viewport == null ||
          box is! RenderBox ||
          !box.hasSize ||
          layout == null) {
        return null;
      }
      return layout.pointAt(
        box.globalToLocal(event.position),
        MediaQuery.devicePixelRatioOf(viewport),
      );
    }
    return (
      track: null,
      x: (event.localPosition.dx / width).clamp(0.0, 1.0),
      y: (event.localPosition.dy / height).clamp(0.0, 1.0),
    );
  }

  void _movePointer(
    ({String? track, double x, double y}) point, {
    required bool immediate,
  }) {
    if (_pointerTrack != point.track) {
      _pointerMoves.flush();
      _pointerTrack = point.track;
    }
    _lastPointerPoint = point;
    _pointerMoves.move(point.x, point.y, immediate: immediate);
  }

  void _point(
    PointerEvent event,
    double width,
    double height, {
    bool force = false,
    String? track,
  }) {
    final session = _session;
    if (_pendingPress != null ||
        _transferBlocked ||
        session == null ||
        !session.captured) {
      return;
    }
    if (_manualGuideOpen) return;
    _syncPointerScope();
    if (!identical(session, _pointerSession)) return;
    if (_relativeCapturing) {
      // A parked pointer has no position: buttons act at the remote cursor.
      _pointButtons(session, event, null);
      return;
    }
    // A pressed pointer keeps its original Flutter hit-test route. Resolve
    // against the entire display layout so it can cross to another output.
    var point = _pointerPoint(event, width, height, track);
    if (!session.captured) return;
    if (point == null) {
      // A new press in an unmapped gap cannot become a click at the previous
      // source point. Relinquish ownership rather than replay it on re-entry.
      if (event.buttons & ~_buttons != 0) {
        _release();
        return;
      }
      // Gaps do not invent cursor coordinates. A release there must still
      // reach the last valid remote position instead of leaving a button held.
      if (_buttons == event.buttons) return;
      point = _lastPointerPoint;
      if (point == null) return;
    }
    _movePointer(point, immediate: force || _buttons != event.buttons);
    _pointButtons(session, event, point);
  }

  void _pointButtons(
    RemoteSession session,
    PointerEvent event,
    ({String? track, double x, double y})? point,
  ) {
    // The virtual USB mouse presses buttons itself; a button pressed here
    // before it began is still let go here.
    final usb = _relativeMouse?.usbPath == true;
    for (final button in {
      1: 'left',
      2: 'right',
      4: 'middle',
      kBackMouseButton: 'x1',
      kForwardMouseButton: 'x2',
    }.entries) {
      if ((_buttons & button.key) != (event.buttons & button.key)) {
        if (usb && event.buttons & button.key != 0) continue;
        session.input({
          'type': 'input',
          'event': 'button',
          'button': button.value,
          'down': event.buttons & button.key != 0,
          if (point != null) ...{
            'x': point.x,
            'y': point.y,
            'track': ?point.track,
          },
        });
      }
    }
    _buttons = usb ? _buttons & event.buttons : event.buttons;
  }

  @override
  void dispose() {
    _dismissSessionGuide();
    _waitClock.stop();
    _clearInitialViewportEntry();
    widget.windowActions?.unbind(this);
    _withdrawRegion();
    _connectionAttempt++;
    _captureIntentRevision++;
    _viewportInputContext = null;
    // Retire waiters before completing canceled gates: disposal does not
    // acknowledge native capture, and no successor may reacquire input.
    final releaseDrains = _releaseDrainGates.entries.toList();
    _releaseDrainGates.clear();
    for (final drain in releaseDrains) {
      drain.key.cancel();
      if (!drain.value.isCompleted) drain.value.complete();
    }
    _discardPointerMoves();
    _pointerMoves.dispose();
    _initialKeyboardRetry?.cancel();
    _unbindPointerOrdering?.call();
    _unbindPointerOrdering = null;
    _viewportDebounce?.cancel();
    _groupLayoutDebounce?.cancel();
    _serverRestartRetry?.cancel();
    _hostUpdateTimer?.cancel();
    _toastTimer?.cancel();
    _injectedNoticeTimer?.cancel();
    _forgetClipboard(disposing: true);
    final notice = _noticeWindow;
    if (notice is NativeTransferNoticeWindow) notice.dispose();
    _forgetPrinter(disposing: true);
    _forgetGamepads();
    _relativeMouse?.dispose();
    _gameMouseUsb
      ?..removeListener(_gameMouseUsbChanged)
      ..dispose();
    WidgetsBinding.instance.removeObserver(this);
    GestureBinding.instance.pointerRouter.removeGlobalRoute(_globalPointer);
    _pictureHeld.dispose();
    _session?.removeListener(_changed);
    _presentation.removeListener(_presentationChanged);
    _presentation.dispose();
    unawaited(_session?.close());
    _focus.dispose();
    super.dispose();
  }

  /// While the window's size is being followed (settling, or a resize in
  /// flight with the previous picture held), the picture fills the window,
  /// scaled up too, instead of keeping its pixels until the new size arrives.
  bool _followingSize(RemoteSession session) =>
      session.heldTextureId != null ||
      session.resizing ||
      _viewportChanging ||
      _viewportDebounce?.isActive == true;

  Widget _desktop(RemoteSession session, {StreamTrack? track}) =>
      RemoteViewport(
        frameSize: remoteFrameSize(session, track),
        mode: track == null && _followingSize(session)
            ? RemoteViewMode.fit
            : _viewMode,
        captured: session.captured,
        builder: (display) {
          final width = display.width, height = display.height;
          final surface = Listener(
            behavior: HitTestBehavior.opaque,
            onPointerDown: (event) async {
              if (!_captureAllowed || !identical(session, _session)) return;
              _pressPicture(event);
              final revision = ++_pointerRevision;
              _PendingRemotePress? pending;
              // A captured, focused picture already owns this input. Awaiting
              // another native claim can let Up cancel a quick click's Down.
              // First claim and focus return still wait for native acceptance.
              if (!session.captured || !_focus.hasFocus) {
                pending = _PendingRemotePress(session, revision, event);
                _pendingPress = pending;
                await _capture();
                if (!identical(_pendingPress, pending)) return;
                _pendingPress = null;
              }
              if (!_captureAllowed ||
                  !identical(session, _session) ||
                  !session.captured ||
                  !_focus.hasFocus ||
                  revision != _pointerRevision) {
                return;
              }
              // 마우스 잠금: the click lands where it was made, then the mouse
              // is handed to the VM. With the virtual USB mouse the whole
              // capture is its own, the click that starts it included: nothing
              // is injected beside it (games such as League of Legends ignore
              // injected input). A pen or a finger never hands it over.
              final mouse = event.kind == PointerDeviceKind.mouse;
              final usbOnly =
                  mouse &&
                  !_relativeCapturing &&
                  _gameMouse &&
                  _relativeMouseOffered &&
                  _gameMouseUsb?.active == true;
              if (revision == _pointerRevision && !usbOnly) {
                _point(event, width, height, force: true, track: track?.id);
                final up = pending?.up;
                if (up != null) {
                  _point(up, width, height, force: true, track: track?.id);
                }
              }
              if (mouse) {
                _syncRelativeMouse();
                _relativeMouse?.pressed();
              }
            },
            onPointerUp: (event) {
              final pending = _pendingPress;
              if (pending != null &&
                  identical(pending.session, session) &&
                  pending.down.pointer == event.pointer &&
                  pending.revision == _pointerRevision) {
                pending.up = event;
                return;
              }
              _pointerRevision++;
              _point(event, width, height, force: true, track: track?.id);
            },
            onPointerHover: (event) =>
                _point(event, width, height, track: track?.id),
            onPointerMove: (event) =>
                _point(event, width, height, track: track?.id),
            // A cancelled press (e.g. a right drag leaving the window) lets
            // go: input.reset lifts whatever the remote still holds.
            onPointerCancel: (_) => _release(),
            onPointerSignal: (event) {
              if (event is PointerScrollEvent && session.captured) {
                _syncPointerScope();
                if (!identical(session, _pointerSession)) return;
                final delta = (-event.scrollDelta.dy.sign * 120).round();
                if (_relativeCapturing) {
                  // The virtual USB mouse carries the wheel itself.
                  if (_relativeMouse?.usbPath == true) return;
                  session.input({
                    'type': 'input',
                    'event': 'wheel',
                    'delta': delta,
                  });
                  return;
                }
                final point = _pointerPoint(event, width, height, track?.id);
                if (point == null) return;
                _movePointer(point, immediate: true);
                session.input({
                  'type': 'input',
                  'event': 'wheel',
                  'delta': delta,
                  'x': point.x,
                  'y': point.y,
                  'track': ?point.track,
                });
              }
            },
            child: MouseRegion(
              cursor: remoteViewCursor(
                captured: session.captured,
                // Right after a composed capture the video still shows the
                // VM's cursor: no second one here until it is gone.
                localCursorFallback:
                    !session.cursorSettling &&
                    (session.localCursorFallback ||
                        _relativeMouse?.localPointerNeeded == true),
              ),
              onEnter: (_) => _hoverPicture(true),
              onExit: (_) => _hoverPicture(false),
              child: Texture(
                textureId: track == null
                    ? session.heldTextureId ?? session.textureId!
                    : session.trackTextures[track.id]!,
                // 1:1 with the remote pixels (the usual case: the viewport
                // requests the window's physical size): no filtering, so
                // text and icons keep their exact pixels.
                filterQuality: remoteTextureFilter(
                  // A track's texture has its source size; the single
                  // stream's has the video size unless tiles compose it at
                  // the (larger) source size, which is then filtered at
                  // 1:1 - exact on whole pixels.
                  texture: track == null
                      ? Size(
                          session.width.toDouble(),
                          session.height.toDouble(),
                        )
                      : remoteFrameSize(session, track),
                  shown: display,
                  pixelRatio: MediaQuery.devicePixelRatioOf(context),
                ),
              ),
            ),
          );
          if (track != null) return surface;
          return Focus(
            key: _pictureKey,
            focusNode: _focus,
            onFocusChange: (focused) {
              if (!focused) _release();
            },
            child: surface,
          );
        },
      );

  Widget _independentDesktop(
    RemoteSession session,
    Size viewport,
    double ratio,
  ) {
    final layout = _localLayout;
    if (!_localLayoutVerified ||
        layout == null ||
        !layout.matchesViewport(viewport, ratio) ||
        !layout.matchesSources(session.tracks) ||
        session.tracks.length != 2 ||
        session.trackTextures.length != 2) {
      if (session.captured) {
        WidgetsBinding.instance.addPostFrameCallback((_) {
          if (mounted && identical(session, _session)) _release();
        });
      }
      return _placeOnPrimary(
        SessionStatusPanel(
          machine: _machine.name,
          // Still connected: never the "not connected" headline.
          title: l10n.cantShowScreenOnMonitors,
          icon: Icons.desktop_access_disabled_outlined,
          message: l10n.monitorSetupChangedDisconnectThen,
          onBack: _back,
        ),
        ratio,
      );
    }
    return Focus(
      key: _pictureKey,
      focusNode: _focus,
      onFocusChange: (focused) {
        if (!focused) _release();
      },
      child: Stack(
        children: [
          for (final surface in layout.surfaces)
            Positioned(
              key: Key('local-output-${surface.id}'),
              left: (surface.bounds.left - layout.bounds.left) / ratio,
              top: (surface.bounds.top - layout.bounds.top) / ratio,
              width: surface.bounds.width / ratio,
              height: surface.bounds.height / ratio,
              child: _desktop(
                session,
                track: session.tracks.singleWhere(
                  (track) => track.id == surface.id,
                ),
              ),
            ),
        ],
      ),
    );
  }

  /// Only states the user can act on get the resolution badge: a failed
  /// resize (retry) or a connection without resizing.
  bool _viewportBadge(RemoteSession session) =>
      !session.closed &&
      session.textureId != null &&
      !session.resizing &&
      (session.resizeError != null ||
          (!session.canResizeViewport && !session.independentOutputs));

  /// The primary local monitor's part of this view (logical pixels) while
  /// the window spans the monitors (모든 모니터 사용); null otherwise, or
  /// while the local layout is unknown.
  Rect? _primaryRegion(double ratio) {
    final layout = _localLayout;
    if (_displayMode != SessionDisplayMode.allMonitors || layout == null) {
      return null;
    }
    final primary = layout.surfaces.first.bounds;
    return Rect.fromLTWH(
      (primary.left - layout.bounds.left) / ratio,
      (primary.top - layout.bounds.top) / ratio,
      primary.width / ratio,
      primary.height / ratio,
    );
  }

  Widget _placeOnPrimary(Widget child, double ratio) {
    final region = _primaryRegion(ratio);
    return region == null
        ? child
        : Positioned.fromRect(rect: region, child: child);
  }

  // The region handed to the app's PrimaryRegionScope, so its dialogs,
  // menus and panels stay on the primary monitor too.
  ValueNotifier<Rect?>? _regionScope;
  Rect? _regionPending, _regionPublished;
  bool _regionScheduled = false;

  void _publishRegion(Rect? region) {
    _regionPending = region;
    if (_regionScheduled) return;
    _regionScheduled = true;
    WidgetsBinding.instance.addPostFrameCallback((_) {
      _regionScheduled = false;
      if (!mounted) return;
      var region = _regionPending;
      final box = _viewportKey.currentContext?.findRenderObject();
      if (region != null && box is RenderBox && box.attached) {
        region = region.shift(box.localToGlobal(Offset.zero));
      }
      final scope = _regionScope ??= PrimaryRegionScope.maybeOf(context);
      if (scope != null && region != _regionPublished) {
        _regionPublished = region;
        scope.value = region;
      }
    });
  }

  void _withdrawRegion() {
    final scope = _regionScope, published = _regionPublished;
    if (scope == null || published == null) return;
    // The scope rebuilds the routes; not while this tree is being torn down.
    WidgetsBinding.instance.addPostFrameCallback((_) {
      try {
        if (scope.value == published) scope.value = null;
      } catch (_) {
        // The whole window (and its scope) went away first.
      }
    });
  }

  @override
  Widget build(BuildContext context) {
    final session = _session;
    final rawFailure = _error ?? session?.error;
    // A 로컬 PC has no Console: the signed-out text names the machine itself.
    final failure =
        rawFailure != null &&
            _machine.kind == MachineKind.pc &&
            (_requestErrorCode ?? session?.diagnosticCode) ==
                'no_interactive_session'
        ? signedOutLocalPcMessage
        : rawFailure;
    // A dialog or menu route above this one takes the mouse back.
    _routeCurrent = ModalRoute.isCurrentOf(context) ?? true;
    _syncPointer();
    _syncCursorView();
    // The bar's first-connection hint, once the first picture is up.
    if (!_coachChecked && _connected) {
      _coachChecked = true;
      WidgetsBinding.instance.addPostFrameCallback((_) {
        if (mounted) unawaited(_maybeCoach());
      });
    }
    final region = _primaryRegion(MediaQuery.devicePixelRatioOf(context));
    _publishRegion(region);
    final theme = Theme.of(context), size = MediaQuery.sizeOf(context);
    return Focus(
      onKeyEvent: (_, event) {
        if (_screenTransitionStage != null &&
            event is KeyDownEvent &&
            event.logicalKey == LogicalKeyboardKey.escape) {
          if (_screenTransitionStage != ScreenTransitionStage.restoring) {
            _cancelScreenTransition();
          }
          return KeyEventResult.handled;
        }
        if (_transferBlocked &&
            event is KeyDownEvent &&
            event.logicalKey == LogicalKeyboardKey.escape) {
          _transfer?.cancel();
          return KeyEventResult.handled;
        }
        if (event is KeyDownEvent &&
            event.logicalKey == LogicalKeyboardKey.escape &&
            session?.captured != true &&
            _presentation.fullscreen) {
          unawaited(_setDisplayMode(false));
          return KeyEventResult.handled;
        }
        return KeyEventResult.ignored;
      },
      // Snack bars float inside the primary monitor's part of a spanning
      // window instead of across the seam.
      child: Theme(
        data: region == null
            ? theme
            : theme.copyWith(
                snackBarTheme: theme.snackBarTheme.copyWith(
                  behavior: SnackBarBehavior.floating,
                  insetPadding: EdgeInsets.fromLTRB(
                    region.left + 16,
                    10,
                    math.max(16, size.width - region.right + 16),
                    math.max(10, size.height - region.bottom + 16),
                  ),
                ),
              ),
        child: Scaffold(
          backgroundColor: const Color(0xFF080D16),
          body: LayoutBuilder(
            key: _viewportKey,
            builder: (context, constraints) {
              _observeViewport(
                constraints.biggest,
                MediaQuery.devicePixelRatioOf(context),
              );
              _syncPointer();
              final remoteContent = Stack(
                fit: StackFit.expand,
                children: [
                  if (_screenTransitionStage != null &&
                      (session == null || !session.frameReady))
                    const SizedBox.expand()
                  else if (_displayDisconnected && session == null)
                    // The same session card as connecting/failed, not plain
                    // white text on the black stage.
                    _placeOnPrimary(
                      SessionStatusPanel(
                        machine: _machine.name,
                        icon: Icons.monitor_rounded,
                        title: _displayConfigured
                            ? l10n.text31(_machine.display.activeCount ?? 0)
                            : l10n.disconnected4,
                        message:
                            _error ??
                            (_displayConfigured
                                ? l10n.reconnectPickUpWhereYou
                                : l10n.checkNumberMonitorsDesktopList),
                        backLabel: widget.windowActions == null
                            ? l10n.backList
                            : l10n.closeRemoteWindow,
                        onBack: _back,
                        onRetry:
                            _displayConfigured &&
                                widget.onReconnect != null &&
                                !_reconnecting
                            ? () => unawaited(_settings())
                            : null,
                        retryKey: const Key('reconnect-after-display-change'),
                        retryLabel: _machine.display.activeCount == 1
                            ? l10n.reconnect2
                            : l10n.reconnect2,
                      ),
                      MediaQuery.devicePixelRatioOf(context),
                    )
                  // A Host update ended the stream: say so, reconnect by
                  // itself, and let the wait be cancelled.
                  else if (_hostUpdating &&
                      _error == null &&
                      (session == null || session.closed))
                    _placeOnPrimary(
                      SessionStatusPanel(
                        key: const Key('host-update-wait'),
                        busy: true,
                        machine: _machine.name,
                        icon: Icons.system_update_alt_rounded,
                        title: l10n.hostUpdating2,
                        message: l10n.reconnectsAutomaticallyWhenDoneWaits,
                        status: _waitStatus(),
                        backLabel: l10n.stopWaiting,
                        onBack: () => _stopHostUpdateWait(cancelled: true),
                      ),
                      MediaQuery.devicePixelRatioOf(context),
                    )
                  else if (!_initialPreparationPending &&
                      (failure != null || session?.closed == true))
                    _placeOnPrimary(
                      SessionStatusPanel(
                        machine: _machine.name,
                        backLabel: widget.windowActions == null
                            ? l10n.backList
                            : l10n.closeRemoteWindow,
                        message:
                            _takeoverAllowed &&
                                _takeoverOffer == null &&
                                widget.onConfirmTakeover != null
                            ? streamTakeoverUnavailableMessage
                            : failure ?? session!.status,
                        onSettings: _recovery == ConnectionRecovery.none
                            ? null
                            : () => unawaited(_recoverySettings()),
                        settingsLabel: _recovery == ConnectionRecovery.display
                            ? l10n.screenSettings
                            : l10n.pictureQuality,
                        // Only where the Console helps: Windows signed out or
                        // locked, or the Host away while the VM runs.
                        onAlternative:
                            widget.onOpenConsole == null ||
                                !const {
                                  'no_interactive_session',
                                  'host_offline',
                                }.contains(
                                  _requestErrorCode ?? session?.diagnosticCode,
                                )
                            ? null
                            : () => unawaited(widget.onOpenConsole!()),
                        alternativeLabel: l10n.openConsole2,
                        onBack: _back,
                        // The one question before a takeover: whose
                        // connections the button ends, and since when.
                        details: _canTakeover
                            ? streamTakeoverSessionLines(_takeoverOffer!)
                            : const [],
                        onRetry: _retryAllowed && (session?.canRetry ?? true)
                            ? _canTakeover
                                  ? _acceptShownTakeover()
                                  : () => unawaited(
                                      _load(reconnect: true, newIntent: true),
                                    )
                            : null,
                        title: _canTakeover ? l10n.connectedSomewhereElse : null,
                        retryLabel: _canTakeover ? l10n.disconnectConnectHere : l10n.reconnect,
                        retryKey: _canTakeover
                            ? const Key('takeover-remote-connection')
                            : const Key('retry-remote-connection'),
                        retryIcon: _canTakeover
                            ? Icons.swap_horiz_rounded
                            : Icons.refresh_rounded,
                      ),
                      MediaQuery.devicePixelRatioOf(context),
                    )
                  // A window resize swaps the decoder for ~0.5 s: a quiet pill,
                  // not the full "connecting" card.
                  else if (session != null &&
                      session.resizing &&
                      (session.textureId == null || !session.frameReady))
                    _placeOnPrimary(
                      const _ResizeVeil(),
                      MediaQuery.devicePixelRatioOf(context),
                    )
                  // The VM restart this window asked for: say so, reconnect
                  // by itself, and let the wait be cancelled.
                  else if (_plannedRestart && _reconnecting && session == null)
                    _placeOnPrimary(
                      SessionStatusPanel(
                        key: const Key('planned-restart-wait'),
                        busy: true,
                        machine: _machine.name,
                        icon: Icons.restart_alt_rounded,
                        title: _hostAwayRestart
                            ? l10n.waitingDesktop
                            : l10n.desktopRestarting,
                        message: _hostAwayRestart
                            ? l10n.ifWindowsRestartingConnectsAs
                            : l10n.connectsAutomaticallyWhenDesktopBack,
                        status: _waitStatus(),
                        backLabel: l10n.stopWaiting,
                        onBack: _cancelPlannedRestart,
                      ),
                      MediaQuery.devicePixelRatioOf(context),
                    )
                  else if (session?.textureId == null ||
                      !session!.frameReady ||
                      _initialViewportEntry != null)
                    _placeOnPrimary(
                      SessionStatusPanel(
                        busy: !_consoleDialogOpen,
                        backLabel: widget.windowActions == null
                            ? l10n.backList
                            : l10n.cancelConnection,
                        machine: _machine.name,
                        message:
                            _initialViewportEntry != null && session != null
                            ? l10n.preparingScreenFitWindow
                            : _initialPreparationPending
                            ? l10n.hostPreparingVirtualDisplayPlease
                            : _serverRestart.waiting && session == null
                            ? serverRestartingMessage
                            : _reconnecting
                            ? l10n.checkingAccountPermissionsHostConnection
                            : session?.resizing == true
                            ? l10n.fittingScreenSize2
                            : session?.status ?? l10n.preparingHostConnection,
                        status: _waitStatus(),
                        onBack: _back,
                      ),
                      MediaQuery.devicePixelRatioOf(context),
                    )
                  else
                    session.independentOutputs
                        ? _independentDesktop(
                            session,
                            constraints.biggest,
                            MediaQuery.devicePixelRatioOf(context),
                          )
                        : _desktop(session),
                  // The previous picture, scaled to the window, stays until the
                  // new size's first frame; the quiet pill tells why it waits.
                  if (session != null && session.heldTextureId != null)
                    _placeOnPrimary(
                      const _ResizeVeil(),
                      MediaQuery.devicePixelRatioOf(context),
                    )
                  // session-resume-v1: the last picture stays and one short
                  // line says the connection is coming back; no error unless
                  // the broker's window runs out. (A resume never starts
                  // while a resize holds a picture, so one pill at most.)
                  else if (session != null &&
                      !session.closed &&
                      session.resuming &&
                      session.frameReady)
                    _placeOnPrimary(
                      IgnorePointer(
                        child: _ResizeVeil(
                          label: l10n.reconnecting2,
                          veilKey: Key('resume-veil'),
                        ),
                      ),
                      MediaQuery.devicePixelRatioOf(context),
                    ),
                  // Only states the user can act on get a badge: a failed
                  // resize (retry) or a connection without resizing. Resizing
                  // itself has the veil; a window too small to resize just
                  // keeps the current resolution.
                  if (session != null && _viewportBadge(session))
                    Positioned(
                      right: 16,
                      bottom: 16,
                      child: Tooltip(
                        message:
                            !session.canResizeViewport &&
                                !session.canResizeTracks
                            ? l10n.connectionCantMatchResolutionWindow
                            : l10n.matchesResolutionWindowSizeAgain,
                        child: FilledButton.tonalIcon(
                          key: const Key('viewport-status'),
                          onPressed:
                              session.resizeError != null &&
                                  session.canResizeViewport &&
                                  !_viewportOutsideBounds
                              ? () {
                                  _release();
                                  _viewportAttempts = 0;
                                  _attemptedViewport = null;
                                  unawaited(_applyViewport());
                                }
                              : _stats,
                          icon: const Icon(
                            Icons.aspect_ratio_rounded,
                            size: 17,
                          ),
                          label: KeepAllText(
                            session.resizeError != null
                                ? l10n.retryResolution
                                : l10n.fixedResolution,
                          ),
                        ),
                      ),
                    ),
                  _placeOnPrimary(
                    ValueListenableBuilder<bool>(
                      valueListenable: _pictureHeld,
                      builder: (context, held, _) => SessionToolbar(
                        key: _toolbarKey,
                        // 마우스 잠금 holds the pointer in the picture, over the
                        // bar's grip too: the bar is reached after the key.
                        pointerHeld: held || _relativeCapturing,
                        popups: _popups,
                        contentKey: _toolbarContent(),
                        layoutKey: _toolbarLayout(),
                        title: _machine.name,
                        modeLabel: l10n.streaming,
                        latencyMs: session?.rttMs,
                        fullscreen: _presentation.fullscreen,
                        // A window has its own title bar; full screen has
                        // none, so the bar carries the window's buttons: —
                        // 최소화 and ❐ 창 모드로 (also in the 화면 panel).
                        onMinimize: _presentation.fullscreen
                            ? () {
                                _release();
                                unawaited(_presentation.minimize());
                              }
                            : null,
                        onRestore:
                            _presentation.fullscreen &&
                                !_reconnecting &&
                                !_presentationChanging
                            ? () {
                                _release();
                                unawaited(_setDisplayMode(false));
                              }
                            : null,
                        onDisconnect: () => unawaited(_confirmDisconnect()),
                        onQualityAt: _connectionPanel,
                        // While the stream resumes the picture itself says so in
                        // one short line (session-resume-v1): the handle does not
                        // repeat it, so `resuming` is not passed here.
                        onOpenChanged: (open) {
                          if (!open) _dismissCoach();
                        },
                        inputCaptured: session?.captured == true,
                        onInteract: _release,
                        // A resolution change keeps the connection: the bar
                        // never reads 연결 대기 during the texture swap.
                        connected: _connected,
                        gameMouse: _gameMouse,
                        gameMouseUsb: _gameMouseUsb?.active == true,
                        // 마우스 잠금's switch is in the 설정 panel; the bar
                        // only shows that it is on.
                        onMenuChanged: _pointerMenuChanged,
                        onBounds: (bounds) => _toolbarBounds = bounds,
                        actions: _menus(),
                      ),
                    ),
                    MediaQuery.devicePixelRatioOf(context),
                  ),
                  // The bar's first-connection hint, under the open bar; only
                  // the hint itself takes the pointer.
                  if (_coachShown)
                    _placeOnPrimary(
                      Align(
                        alignment: Alignment.topCenter,
                        child: Padding(
                          padding: const EdgeInsets.fromLTRB(
                            12,
                            SessionChrome.barHeight + 24,
                            12,
                            0,
                          ),
                          child: SessionBarCoach(
                            onConfirm: () => _dismissCoach(fold: true),
                          ),
                        ),
                      ),
                      MediaQuery.devicePixelRatioOf(context),
                    ),
                  if (_toast != null)
                    _placeOnPrimary(
                      // A notice never takes the pointer.
                      IgnorePointer(
                        child: Align(
                          alignment: const Alignment(0, .88),
                          child: _SessionToast(
                            key: const Key('pointer-confinement-toast'),
                            message: _toast!,
                            icon: _toastIcon,
                          ),
                        ),
                      ),
                      MediaQuery.devicePixelRatioOf(context),
                    ),
                ],
              );
              return Stack(
                fit: StackFit.expand,
                children: [
                  ExcludeFocus(
                    excluding:
                        (_transferBlocked && !_transferPreservesPaste) ||
                        _screenTransitionStage != null,
                    child: AbsorbPointer(
                      absorbing:
                          _transferBlocked || _screenTransitionStage != null,
                      child: remoteContent,
                    ),
                  ),
                  if (_screenTransitionStage != null)
                    _placeOnPrimary(
                      ScreenTransitionProgress(
                        key: const Key('screen-transition-overlay'),
                        target: _screenTransitionTarget,
                        stage: _screenTransitionStage!,
                        cancelling: _screenTransitionCancelled,
                        onCancel:
                            _allMonitorsPreflight &&
                                _screenTransitionStage !=
                                    ScreenTransitionStage.restoring
                            ? _cancelScreenTransition
                            : null,
                        onClose: _back,
                      ),
                      MediaQuery.devicePixelRatioOf(context),
                    ),
                  if (!_transferBlocked && _transfer != null)
                    ClipboardTransferOverlay(
                      controller: _transfer!,
                      dark: SessionChrome.of(context).dark,
                      primaryRegion: _primaryRegion(
                        MediaQuery.devicePixelRatioOf(context),
                      ),
                      requestFocus: false,
                      onInteract: _release,
                    ),
                  _placeOnPrimary(
                    StreamObservationOverlay(session: _session),
                    MediaQuery.devicePixelRatioOf(context),
                  ),
                ],
              );
            },
          ),
        ),
      ),
    );
  }
}

/// A small session-chrome notice over the picture. It takes no input.
class _SessionToast extends StatelessWidget {
  const _SessionToast({
    super.key,
    required this.message,
    this.icon = Icons.mouse_outlined,
  });
  final String message;
  final IconData icon;

  @override
  Widget build(BuildContext context) {
    final chrome = SessionChrome.of(context);
    // Long notices (the missing sound device's line) wrap inside the
    // window, or the primary monitor's part of it, instead of running off.
    return Padding(
      padding: const EdgeInsets.symmetric(horizontal: 16),
      child: ConstrainedBox(
        constraints: const BoxConstraints(maxWidth: 560),
        child: Semantics(
          liveRegion: true,
          child: DecoratedBox(
            decoration: BoxDecoration(
              color: chrome.surface,
              borderRadius: BorderRadius.circular(12),
              border: Border.all(color: chrome.edge),
              boxShadow: [
                BoxShadow(
                  color: chrome.shadow,
                  blurRadius: 18,
                  spreadRadius: -6,
                  offset: const Offset(0, 8),
                ),
              ],
            ),
            child: Padding(
              padding: const EdgeInsets.fromLTRB(12, 8, 14, 8),
              child: Row(
                mainAxisSize: MainAxisSize.min,
                children: [
                  Icon(icon, size: 16, color: chrome.accent),
                  const SizedBox(width: 8),
                  Flexible(
                    child: KeepAllText(
                      message,
                      maxLines: 4,
                      overflow: TextOverflow.ellipsis,
                      style: TextStyle(
                        fontFamily: 'Pretendard',
                        fontFamilyFallback: const ['Malgun Gothic', 'Segoe UI'],
                        fontSize: 12.5,
                        height: 1.4,
                        fontWeight: FontWeight.w600,
                        color: chrome.ink,
                      ),
                    ),
                  ),
                ],
              ),
            ),
          ),
        ),
      ),
    );
  }
}
