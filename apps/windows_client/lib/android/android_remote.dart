import 'dart:async';
import 'dart:io' show Platform;
import 'dart:math' as math;
import 'dart:ui' show DisplayFeatureType;

import 'package:flutter/gestures.dart';
import 'package:flutter/material.dart';
import 'package:flutter/services.dart';

import '../data/broker_api.dart';
import '../data/clipboard_session.dart' show clipboardStopNotice;
import '../data/connection_feedback.dart';
import '../data/display_scale.dart'
    show DisplayScaleApplied, nearestDisplayScale;
import '../data/drive_session.dart';
import '../data/relative_mouse.dart';
import '../data/remote_session.dart';
import '../data/session_failure.dart';
import '../data/stream_display_entry.dart' show restartPollInterval;
import '../data/stream_size.dart'
    show DecoderLimits, EncodeLimit, nativeDecoderLimits;
import '../data/stream_stats_report.dart' show StreamStatsSink;
import '../data/text_input.dart';
import '../data/track_stream.dart' show StreamTrack;
import '../data/touch_input.dart';
import '../data/usb_game_mouse.dart';
import '../models/stream_options.dart';
import '../ui/app_dialog.dart';
import '../ui/connection_wait_status.dart';
import '../ui/drive_panel.dart' show driveEndedMessage;
import '../ui/session_access_boundary.dart';
import '../ui/app_theme.dart';
import '../ui/korean_text.dart';
import '../ui/pointer_move_coalescer.dart';
import '../models/session_guide.dart';
import '../ui/session_connection_panel.dart';
import '../ui/session_menu.dart'
    show
        SessionChrome,
        SessionMenuItem,
        SessionPopups,
        SessionToolbarMenu,
        SessionTouchScope;
import '../ui/session_panels.dart';
import '../ui/session_stats_dialog.dart';
import '../ui/session_tools.dart';
import '../ui/stream_observation_overlay.dart';
import '../ui/stream_session_stats.dart' show appCpuLabel;
import 'android_cursor.dart';
import 'android_drive.dart';
import 'android_folder_chooser.dart';
import 'android_game_mouse.dart';
import 'android_input.dart';
import 'android_keyboard.dart';
import 'android_media.dart';
import 'android_network.dart';
import 'android_picture.dart';
import 'android_pen.dart';
import 'android_print.dart';
import 'android_reconnect.dart';
import 'android_remote_notice.dart';
import 'android_session_ui.dart';
import 'android_session_keepalive.dart';
import 'android_sharp_picture.dart';
import 'android_store.dart';
import 'android_toolbar.dart';
import 'android_stream_preparation.dart';
import 'android_touch.dart';
import 'android_touch_feedback.dart';
import 'android_tv.dart';
import 'android_tv_remote.dart';
import '../l10n/l10n.dart';

/// 터치 mode's real Windows touch contacts on a session that granted
/// touch-input-v1 (each contact goes out as its own one-contact frame; the
/// Host keeps the others where they were).
class _SessionTouchContacts implements AndroidTouchContacts {
  _SessionTouchContacts(this.session);
  final RemoteSession session;

  /// Granted, and input is captured now (touch frames are only accepted
  /// then): otherwise this gesture is mouse emulation, which captures.
  @override
  bool get available => session.touchInput && session.captured;

  @override
  void contact(int id, Offset point, AndroidContactPhase phase) {
    session.sendTouch([
      RemoteTouchContact(
        id: id & touchInputMaxId,
        phase: RemoteTouchPhase.values.byName(phase.name),
        x: point.dx.clamp(0.0, 1.0),
        y: point.dy.clamp(0.0, 1.0),
      ),
    ]);
  }
}

/// Android presentation; authorization, epochs and relay framing stay in
/// RemoteSession. The picture fills the screen (outside a display cutout);
/// fingers drive the remote mouse through [AndroidTouchInput], the phone
/// keyboard types through [AndroidTypingBridge] and the key row, and a
/// small handle at the top opens the session commands.
class AndroidRemoteView extends StatefulWidget {
  const AndroidRemoteView({
    super.key,
    required this.name,
    this.desktopId,
    this.server,
    required this.load,
    required this.token,
    required this.options,
    this.takeover,
    this.settings = const AndroidSessionSettings(helpSeen: true),
    this.onSettings,
    this.onStats,
    this.displayScale = false,
    this.streamSize = false,
    this.streamSizeV2 = false,
    this.dualOutputs = false,
    this.decoderLimits,
    this.touchContacts,
    this.network,
    this.initialDensity,
    this.density = AndroidScreenDensity.read,
    this.sessionFactory,
    this.pictureBuilder,
    this.onOptions,
    this.maxBitrate,
    this.relativeMouseNative,
    this.usbGameMouseNative,
    this.driveUnavailable,
    this.onConnectStep,
    this.driveFiles,
    this.sharpPicture = const NativeAndroidSharpPicture(),
    this.reconnectJitter,
    this.awaitBrokerRestart,
    this.awaitHostUpdate,
    this.awaitHostReturn,
    this.onAuthenticationEnded,
    this.onBrokerNotices,
  });

  /// notice-v1 (android_notices.dart): the broker said on this stream's
  /// control socket that the account has notices to read.
  final VoidCallback? onBrokerNotices;
  final String name, token;
  final String? desktopId, server;
  final ValueChanged<String>? onAuthenticationEnded;
  final bool dualOutputs;

  /// The first connection's request; 설정 in the session menu replaces it
  /// for the reconnect it asks for, and tells [onOptions] (this device
  /// remembers it for the next connection, as on Windows).
  final StreamOptions options;
  final ValueChanged<StreamOptions>? onOptions;

  /// The desktop's bitrate-adaptive-v1 ceiling (null: released), for which
  /// 화질 모드 the choices read as.
  final int? maxBitrate;

  /// Connects with the measured [options] and, where the Host supports
  /// display-scale-v1, the Windows display scale to ask for (else null).
  final Future<BrokerConnection> Function(
    StreamOptions options,
    bool Function() authorized,
    int? displayScale,
  )
  load;

  /// The desktop's inventory advertises display-scale-v1: the remote output
  /// uses this screen's own pixels and Windows scales its text (the grant
  /// of each connection decides; older Hosts get an exact pixel fraction).
  final bool displayScale;

  /// The desktop's inventory advertises stream-size-v1 (with separate-v1
  /// viewport ceilings): a portrait screen keeps its own pixels up to
  /// 2160x3840 within this device's decoder limits (the grant of each
  /// connection decides; without it a portrait screen taller than 2160 rows
  /// gets an exact pixel fraction).
  final bool streamSize;

  /// The exact v2 inventory/grant. v1 retains its released workspace rules.
  final bool streamSizeV2;

  /// This device's decoder limits (tests); null asks the native engine.
  final Future<DecoderLimits?> Function()? decoderLimits;

  /// Real Windows touch contacts for 터치 mode where the session has them
  /// (touch-input-v1); null or unavailable: 터치 is mouse emulation.
  final AndroidTouchContacts? Function(RemoteSession session)? touchContacts;

  /// Network changes for a quick reconnect (Wi-Fi lost, mobile available).
  final AndroidNetworkEvents? network;

  /// The display's measured pixel density (자동 화면 배율 on tablets and
  /// foldables): [initialDensity] as the app read it, [density] to read it
  /// again (after folding or rotating); none: the density bucket.
  final AndroidScreenDensity? initialDensity;
  final Future<AndroidScreenDensity?> Function() density;

  /// Ends this VM's connection elsewhere when the broker allows it
  /// (desktop_busy with takeover); the view then connects normally.
  final Future<void> Function(
    StreamTakeoverOffer offer,
    bool Function() current,
  )?
  takeover;

  /// Restart recovery (androidAwaitBrokerRestart): waits within [limit]
  /// until the broker answers again and this desktop's Host streams;
  /// [current] false cancels it. Null: a broker restart is waited through by
  /// the reconnect back-off alone.
  final Future<void> Function(bool Function() current, Duration limit)?
  awaitBrokerRestart;

  /// After a Host update ended the stream (host_update): waits within
  /// [limit] until that update is over and this desktop's Host streams
  /// again (androidAwaitBrokerRestart with hostUpdating). Null: the tries
  /// alone wait, each refused while the update runs.
  final Future<void> Function(bool Function() current, Duration limit)?
  awaitHostUpdate;

  /// After the Host went away (host_disconnected): waits within [limit] for
  /// it to stream again, and past it up to [hostAwayWait] once the Host is
  /// seen away with the VM running (Windows restarting inside the VM); then
  /// [onHostAway] is called once.
  /// Null: the reconnect back-off alone waits (90 s).
  final Future<void> Function(
    bool Function() current,
    Duration limit,
    void Function() onHostAway,
  )?
  awaitHostReturn;
  static const hostAwayWait = Duration(minutes: 3);

  /// Touch mode, display scale and whether the gesture overview was shown;
  /// changes are reported to [onSettings] (this device remembers them).
  final AndroidSessionSettings settings;
  final ValueChanged<AndroidSessionSettings>? onSettings;

  /// One streaming statistics window for the broker's client diagnostics,
  /// with this device's input counts (never content) of the same window.
  final void Function(Map<String, Object> values, String sessionId)? onStats;

  /// Tests substitute the session (relay dialer) and the picture.
  @visibleForTesting
  final RemoteSession Function(
    BrokerConnection connection,
    AndroidRemoteMedia media,
    StreamStatsSink? statsSink,
  )?
  sessionFactory;
  @visibleForTesting
  final WidgetBuilder? pictureBuilder;

  /// Tests substitute MainActivity's pointer capture and the virtual USB
  /// mouse's native side (마우스 잠금).
  @visibleForTesting
  final MousePresenceNative? relativeMouseNative;
  @visibleForTesting
  final UsbGameMouseNative? usbGameMouseNative;

  /// Why the desktop cannot take shared folders although this account may
  /// share them (the inventory's fixed code); null where it does not apply.
  final String? driveUnavailable;

  /// Records one step of this view that makes no request (the launcher's
  /// connection record, HttpBrokerApi.recordConnectStep): 30 closing (reason
  /// tens: who asked, ones: 1 while preparing, 2 with a picture or an error;
  /// 90 the notification), 31 the keep-alive step done, 32 the first frame
  /// laid out, 33 the size planned, 35 the preparation failed, 36 it was
  /// abandoned because the app left the front. The reason's hundreds are the
  /// app's lifecycle state.
  final void Function(int stage, bool ok, int reason)? onConnectStep;

  /// Tests substitute the folder engine (AndroidDrive.kt).
  @visibleForTesting
  final AndroidDriveFileSystem? driveFiles;

  /// 선명하게's engine side (SharpPicture.kt); tests substitute it.
  final AndroidSharpPicture sharpPicture;

  /// Tests fix the reconnect jitter (0..1, see androidReconnectDelay).
  @visibleForTesting
  final double Function()? reconnectJitter;

  @override
  State<AndroidRemoteView> createState() => _AndroidRemoteViewState();
}

class _AndroidRemoteViewState extends State<AndroidRemoteView>
    with WidgetsBindingObserver {
  RemoteSession? _session;
  late final _backgroundKeeper = AndroidSessionKeepaliveOwner(
    onDisconnect: _stopFromNotification,
  );
  bool _backgroundFailureShown = false;

  Future<void> _keepSessionAlive(int generation) async {
    if (!_inFront) return;
    final started = await _backgroundKeeper.start();
    if (!started &&
        mounted &&
        generation == _generation &&
        Platform.isAndroid &&
        !_backgroundFailureShown) {
      _backgroundFailureShown = true;
      _notice(l10n.couldntKeepConnectionRunningBackground);
    }
  }

  /// print-v1: documents the VM printed go to Android's print dialog.
  late final _printOffers = AndroidPrintOffers(
    foregroundOnly: true,
    onNotice: (message) => _notice(message),
    progressContext: () => mounted ? context : null,
  );
  AndroidRemoteMedia? _media;
  AndroidInputActions? _actions;
  late AndroidSessionSettings _settings = widget.settings;
  // A new streaming view starts Windows at 100%. Keep this choice local to
  // the view so reconnects, rotation and resize retain later manual changes.
  int _scalePreference = 100;

  /// What the next connection asks for ([AndroidRemoteView.options] until
  /// 설정 changes it).
  late StreamOptions _options = widget.options;
  bool _dualOutputs = false;
  String? _selectedTrackId;
  StreamTrack? get _selectedOutput {
    final tracks = _session?.tracks ?? const <StreamTrack>[];
    return tracks.where((track) => track.id == _selectedTrackId).firstOrNull ??
        tracks.where((track) => track.primary).firstOrNull ??
        tracks.firstOrNull;
  }

  late final AndroidPenInput _pen = AndroidPenInput(
    project: (point) => _view?.normalize(point),
    clamp: (point) => _view?.normalizeClamped(point) ?? _touch.cursor,
    send: _penEvent,
    takeOver: _penTakeOver,
  );
  late final AndroidTouchInput _touch = AndroidTouchInput(
    view: () => _view!,
    visible: () => _visible,
    onView: (view) {
      if (!mounted) return;
      // A pinch is a move by hand; the cursor pulling the picture is not.
      _camera.moved(byHand: view.scale != _view?.scale);
      setState(() => _view = view);
    },
    send: _touchEvent,
    onCursor: (point) => _media?.cursorState.pointer(point),
    onCue: _cue,
    mode: _settings.touch,
    speed: _settings.speed,
    source: () {
      final session = _session;
      final width = _selectedOutput?.width ?? session?.sourceWidth,
          height = _selectedOutput?.height ?? session?.sourceHeight;
      if (width == null || height == null || width <= 0 || height <= 0) {
        return _viewSource;
      }
      // Quality changes alter the encoded picture, not Windows' workspace.
      return Size(width.toDouble(), height.toDouble());
    },
    follow: () => !_camera.manual,
    contacts: () {
      final session = _session;
      if (session == null) return null;
      return widget.touchContacts?.call(session) ??
          (identical(_sessionContacts?.session, session)
              ? _sessionContacts
              : _sessionContacts = _SessionTouchContacts(session));
    },
  );
  _SessionTouchContacts? _sessionContacts;
  late final _moves = PointerMoveCoalescer(
    send: (x, y) => _deliver([
      {'type': 'input', 'event': 'move', 'x': x, 'y': y},
    ]),
    interval: const Duration(milliseconds: 8),
  );
  final _cues = ValueNotifier<(AndroidTouchCue, Offset?, int)?>(null);
  final _counters = AndroidInputCounters();

  /// Committed text (text-input-v1) or key presses (older Hosts), chosen per
  /// session by the broker's grant.
  var _typing = AndroidTypingBridge();
  int _typingEpoch = 0;
  final _field = TextEditingController.fromValue(AndroidTypingBridge.initial);
  late final _fieldFocus = FocusNode(
    debugLabel: 'android remote keyboard',
    onKeyEvent: _fieldKey,
  );
  final _latched = <AndroidModifier>{}, _locked = <AndroidModifier>{};

  bool _loading = false, _canRetry = true, _menuOpen = false;
  bool _canTakeover = false, _closing = false, _allowPop = false;
  StreamTakeoverOffer? _takeoverOffer;
  bool _takeoverRunning = false;
  int _takeoverRevision = 0;
  bool _keyboard = false, _imeSeen = false;
  Object _tvImeOwner = Object();

  /// The toolbar's open panel ('screen') and its move handle.
  String? _toolPanel;
  bool _moving = false;

  /// The tool dock (its 도구 button floats with the other buttons).
  final _barKey = GlobalKey<AndroidSessionBarState>();

  /// The tool dock is open: the floating buttons make way for it.
  bool _barOpen = false;
  final _popups = SessionPopups();
  final _hold = AndroidPointerHold();
  bool _resumeConnect = false, _applying = false, _wasCaptured = false;
  bool _hangulHinted = false, _unsupportedHinted = false;
  String? _error;
  ({String message, String code})? _requestFailure;
  int _generation = 0, _buttons = 0, _pointerRevision = 0, _cueCount = 0;
  int _penPointerEpoch = 0;
  StreamSubscription<dynamic>? _penSignals;
  int _queued = 0;
  Timer? _resizeTimer;
  Size? _viewport;

  /// The picture area in device pixels, its device pixel ratio and the
  /// remote output planned for it (see [AndroidViewportPlan]).
  Size? _physical;
  double _dpr = 1;
  AndroidViewportPlan? _plan;
  final _continuity = AndroidPictureContinuity();

  /// This device's decoder limits for a stream-size-v1 plan before a
  /// connection (null until known, or when the engine cannot say).
  DecoderLimits? _decoderLimits;
  int? _viewPixelScale, _viewPlanScale;
  DisplayScaleApplied? _seenScale;
  bool _scaleChanged = false;
  String? _attemptedViewport;
  int _viewportRevision = 0;
  Object? _viewportInFlight;
  Future<void> _inputQueue = Future<void>.value();
  AndroidPictureView? _view;
  Size? _viewSource;
  Rect _visible = Rect.zero;

  /// The picture above the phone keyboard (moves up, returns on close).
  final _camera = AndroidKeyboardCamera();

  /// This window in dp and the display's measured density (자동 화면 배율).
  Size? _window;
  late AndroidScreenDensity? _density = widget.initialDensity;
  int _densityRead = 0;
  AndroidOrientation? _orientation;
  bool _captureQueued = false;
  final _tvInput = GlobalKey<AndroidTvRemoteState>();
  bool _tvMenuOpen = false;
  AndroidTvPointerMode _tvMode = AndroidTvPointerMode.pointer;
  String? _tvPanel;
  Object? _tvPanelOwner;

  // 마우스 잠금, as on Windows:
  // with a mouse connected and the option on, a mouse click on the picture
  // (or 확인 in its dialog) hands the mouse to the VM through Android's
  // pointer capture. The virtual USB mouse carries it where the VM reads it,
  // else moveRelative (AndroidGameMouse.kt). Back, a finger or a pen on the
  // picture, the dock and the app leaving the front take it back. Every
  // connection starts off; a finger alone never offers it.
  late final MousePresenceNative _gameMouseNative =
      widget.relativeMouseNative ?? ChannelRelativeMouse();
  late final RelativeMouseControl _relativeMouse;
  bool _gameMouse = false, _mousePresent = false, _gameMouseDialogOpen = false;

  /// Hand-overs waiting for the VM to confirm the virtual USB mouse (up to
  /// 4 s each): said on the picture, where nothing moved meanwhile.
  int _gameMouseWaits = 0;

  /// The mouse press that handed the mouse over: its release is used up too.
  int? _gameMousePress;
  UsbGameMouse? _gameMouseUsb;
  RemoteSession? _injectedNoticed;

  @override
  void initState() {
    super.initState();
    _relativeMouse = RelativeMouseControl(
      native: _gameMouseNative,
      // No pointer to pin here: with cursor-echo-v1 the drawn cursor follows
      // the positions the Host echoes, so it is the VM's own.
      pinned: () => _session?.cursorEcho == true,
      // cursor-compose-v1 (preferred): the VM's cursor comes in the video.
      composed: () => _session?.cursorCompose == true,
      onChanged: _relativeMouseChanged,
      onEngaged: _relativeMouseEngaged,
    );
    _gameMouseNative.onMouse = _mouseChanged;
    unawaited(_gameMouseNative.mousePresent().then(_mouseChanged));
    WidgetsBinding.instance.addObserver(this);
    // Where Android's folder picker cannot open: this app's own folder list.
    _driveFiles.chooser = _chooseFolder;
    _readDensity();
    _field.addListener(_typed);
    _penSignals = AndroidPenSignals.events.listen(
      (event) {
        if (mounted) {
          _pen.signal(event);
        }
      },
      onError: (Object _) {
        if (mounted) {
          _pen.signal(const {'type': 'unavailable'});
        }
      },
      onDone: () {
        if (mounted) {
          _pen.signal(const {'type': 'unavailable'});
        }
      },
    );
    _networkWatch = widget.network?.changes.listen(
      _networkChanged,
      onError: (Object _) {},
    );
    unawaited(
      SystemChrome.setEnabledSystemUIMode(SystemUiMode.immersiveSticky),
    );
    unawaited(_connect());
  }

  @override
  void didChangeAppLifecycleState(AppLifecycleState state) {
    _step(37, ok: state == AppLifecycleState.resumed);
    if (state != AppLifecycleState.resumed) ++_takeoverRevision;
    _session?.setAndroidForeground(state == AppLifecycleState.resumed);
    switch (state) {
      case AppLifecycleState.inactive:
        // A notification shade or system dialog: keep the stream, release
        // input so nothing stays pressed.
        _release();
        _suspendViewport();
      case AppLifecycleState.hidden || AppLifecycleState.paused:
        _release();
        _suspendViewport();
        if (_reconnecting && _reconnectTimer != null) {
          // A reconnect waiting for its next try: connect again on return.
          _stopReconnecting();
          _resumeConnect = true;
        }
        if (_serverRestartRetry?.isActive == true) {
          // A connect waiting through a server restart: the same.
          _serverRestartRetry!.cancel();
          _resumeConnect = true;
        }
        final current = _session;
        if ((current == null || current.closed) && _loading) {
          // Abandon the preparation; returning to the app starts over.
          _step(36, ok: false);
          ++_generation;
          _backgroundKeeper.stop();
          _resumeConnect = true;
          setState(() {
            _loading = false;
            _error = _backgroundMessage;
          });
        }
      case AppLifecycleState.resumed:
        _captureHold = false;
        if (_session?.closed == false) {
          unawaited(_keepSessionAlive(_generation));
        }
        if (mounted) {
          _viewportSettling = true;
          setState(() {});
          WidgetsBinding.instance.addPostFrameCallback((_) {
            if (mounted && _inFront) _settleViewport();
          });
        }
      case AppLifecycleState.detached:
        break;
    }
  }

  Timer? _resumeViewportTimer;
  bool _viewportSettling = false;

  void _suspendViewport() {
    ++_viewportRevision;
    _viewportSettling = true;
    _resizeTimer?.cancel();
    _resizeTimer = null;
    _resumeViewportTimer?.cancel();
    _resumeViewportTimer = null;
  }

  // Android may briefly report system-bar/surface geometry on return. Wait
  // for a stable foreground layout before changing the remote display.
  void _settleViewport() {
    _resumeViewportTimer?.cancel();
    _resumeViewportTimer = Timer(const Duration(milliseconds: 650), () {
      _resumeViewportTimer = null;
      if (!mounted || !_inFront || _closing || _allowPop) return;
      _viewportSettling = false;
      if (_resumeConnect && _hostUpdate) {
        // Back while a Host update's wait still runs: it goes on.
        _resumeConnect = false;
        setState(() => _error = null);
        _scheduleReconnect();
      } else if (_resumeConnect) {
        _resumeConnect = false;
        unawaited(_connect());
      } else {
        _replan();
        // An unchanged measurement must also resume a cancelled resize.
        _scheduleResize();
      }
    });
  }

  static String get _backgroundMessage => l10n.stoppedPreparingConnectionWhileOutside;

  void _retireMedia(AndroidRemoteMedia? media, RemoteSession? session) {
    if (media == null) return;
    // The cursor state outlives a closing session only until its stop ran.
    unawaited(
      (session?.close() ?? Future<void>.value()).whenComplete(media.dispose),
    );
  }

  bool _takeoverAvailable(StreamTakeoverOffer? offer) =>
      offer != null &&
      widget.desktopId != null &&
      offer.desktopId == widget.desktopId &&
      offer.currentAt(DateTime.now());

  StreamTakeoverOffer? _offeredTakeover(Object error) {
    if (error is! BrokerException ||
        error.code != 'desktop_busy' ||
        error.takeover != true) {
      return null;
    }
    final offer = error.takeoverOffer;
    return _takeoverAvailable(offer) ? offer : null;
  }

  String _takeoverFailureMessage(SessionFailure failure) =>
      failure.takeover && !_takeoverAvailable(_takeoverOffer)
      ? streamTakeoverUnavailableMessage
      : failure.message;

  void _expiredTakeover() {
    const error = BrokerException('takeover_context_expired', 410);
    setState(() {
      _takeoverOffer = null;
      _canTakeover = false;
      _error = error.message;
      _requestFailure = (message: error.message, code: error.code);
      _canRetry = true;
    });
  }

  /// The card is the one question, never a second one after it: it names the
  /// accounts and start times of the exact broker offer, and its button
  /// accepts that offer, [shown]. The broker still checks expiry and
  /// occupancy; nothing here releases a connection without that offer.
  Future<void> _takeover(StreamTakeoverOffer? shown) async {
    final takeover = widget.takeover;
    final offer = _takeoverOffer;
    if (takeover == null ||
        !identical(offer, shown) ||
        _loading ||
        _takeoverRunning ||
        _closing ||
        _allowPop ||
        !mounted ||
        !_inFront) {
      return;
    }
    if (!_takeoverAvailable(offer)) {
      _expiredTakeover();
      return;
    }
    final generation = _generation, revision = ++_takeoverRevision;
    final token = widget.token, desktopId = widget.desktopId;
    bool current() =>
        mounted &&
        !_closing &&
        !_allowPop &&
        _inFront &&
        generation == _generation &&
        revision == _takeoverRevision &&
        widget.token == token &&
        widget.desktopId == desktopId &&
        identical(widget.takeover, takeover) &&
        identical(_takeoverOffer, offer);
    // Set before anything is awaited: a second press finds it and returns.
    _takeoverRunning = true;
    _release();
    setState(() {
      _loading = true;
      _error = null;
      _requestFailure = null;
      _canTakeover = false;
    });
    try {
      await takeover(offer!, current);
    } catch (error) {
      final failure = SessionFailure.request(error);
      if (current()) {
        setState(() {
          _takeoverOffer = _offeredTakeover(error);
          _loading = false;
          _error = _takeoverFailureMessage(failure);
          _requestFailure = (
            message: _error!,
            code: error is BrokerException ? error.code : failure.code,
          );
          _canRetry = failure.canRetry;
          _canTakeover = failure.takeover && _takeoverAvailable(_takeoverOffer);
        });
      }
      return;
    } finally {
      _takeoverRunning = false;
    }
    if (!current()) return;
    setState(() => _loading = false);
    await _connect();
  }

  /// A broker update restarting the server: a connect waits through it.
  final _serverRestart = ServerRestartWait();
  Timer? _serverRestartRetry;

  /// [auto]: a quiet reconnect after the connection dropped (the last
  /// picture stays up; see [_sessionEnded]). [again]: the retry of a connect
  /// that met a server restart.
  Future<void> _connect({bool auto = false, bool again = false}) async {
    if (_loading || _closing || _allowPop || !mounted) return;
    if (!auto) _stopReconnecting();
    _serverRestartRetry?.cancel();
    if (!auto && !again) _serverRestart.reset();
    // A new connection has measured nothing yet: no advice carried over.
    _poorLatency.update(connected: false, ms: null);
    final generation = ++_generation;
    if (!auto || !_waitClock.isRunning) {
      _waitClock
        ..reset()
        ..start();
    }
    ++_takeoverRevision;
    _takeoverOffer = null;
    _clearNotice(rebuild: false);
    _printOffers.watch(null);
    _backgroundKeeper.stop();
    final previous = _session, previousMedia = _media;
    previous?.removeListener(_changed);
    previous?.caretUpdates.removeListener(_caretChanged);
    _release();
    _retireTyping();
    setState(() {
      _loading = true;
      _error = null;
      _requestFailure = null;
      _session = null;
      _media = null;
      _view = null;
      _menuOpen = false;
      _barOpen = false;
      _toolPanel = null;
      _panelFrom = null;
      _canRetry = true;
      _canTakeover = false;
      _attemptedViewport = null;
      ++_viewportRevision;
      _viewportInFlight = null;
      _resumeConnect = false;
      // Every connection starts with 마우스 잠금 off, a reconnect included.
      _gameMouse = false;
    });
    _syncRelativeMouse();
    await previous?.close();
    _retireMedia(previousMedia, previous);
    if (auto && previous != null) {
      // The dropped connection may still hold this VM at the broker (its
      // release could not get through while the network was down): end
      // exactly that own session again before asking for a new one.
      try {
        await previous.connection.releaseAgain();
      } catch (_) {}
      if (!mounted || generation != _generation) return;
    }
    try {
      final restartWait = widget.awaitBrokerRestart;
      final updateWait = widget.awaitHostUpdate;
      if (auto && updateWait != null && _hostUpdate) {
        // A Host update ended the stream: wait for it and for its Host
        // (host-ready) instead of connections it refuses.
        await updateWait(
          () => mounted && generation == _generation && !_allowPop,
          _hostUpdateUntil!.difference(DateTime.now()),
        );
        if (!mounted || generation != _generation) return;
      } else if (auto &&
          widget.awaitHostReturn != null &&
          _hostLossWait &&
          !_plannedRestart &&
          !_serverRestart.waiting) {
        // The Host went away: wait for it (host-ready) instead of trying the
        // whole connection on the back-off; Windows restarting inside the VM
        // is waited through like this view's own restart.
        _hostLossWait = false;
        final previousSession = _session;
        await widget.awaitHostReturn!(
          () => mounted && generation == _generation && !_allowPop,
          restartWaitLimit,
          () {
            if (!mounted || generation != _generation) return;
            setState(() {
              _startPlannedRestart(previousSession);
              _hostAwayRestart = true;
            });
          },
        );
        if (!mounted || generation != _generation) return;
      } else if (auto && restartWait != null && _restartRecovery) {
        // The broker restarts: wait on it and then on the Host (host-ready)
        // instead of trying the whole connection on the back-off.
        final spent = DateTime.now().difference(
          _reconnectSince ?? DateTime.now(),
        );
        final left = reconnectBudget - spent;
        try {
          await restartWait(
            () => mounted && generation == _generation && !_allowPop,
            left < restartWaitLimit ? left : restartWaitLimit,
          );
        } catch (_) {
          // Outlasted or failed otherwise: the back-off alone from now on.
          if (generation == _generation) _restartWaitFailed = true;
          rethrow;
        }
        if (!mounted || generation != _generation) return;
      }
      await _keepSessionAlive(generation);
      _step(31, ok: mounted && generation == _generation);
      if (!mounted || generation != _generation) return;
      // The immersive route must be laid out before requesting a real mode.
      // Measuring the list/dialog or a logical-pixel size gives the wrong
      // initial desktop resolution on high-DPI and portrait Android devices.
      await WidgetsBinding.instance.endOfFrame;
      _step(32, ok: mounted && generation == _generation);
      if (!mounted || generation != _generation) return;
      // stream-size-v1: the decoder's limits decide a portrait plan before
      // the connect request declares them (asked once per process).
      if (widget.streamSize) {
        _decoderLimits ??=
            await (widget.decoderLimits ?? nativeDecoderLimits)();
        if (!mounted || generation != _generation) return;
      }
      // 선명하게: the engine's plan decides the size this request asks for.
      final physicalNow = _physical;
      if (physicalNow != null) {
        _prepareSharp(physicalNow);
        final planning = _sharpPlanning;
        if (planning != null) {
          await planning;
          if (!mounted || generation != _generation) return;
        }
      }
      // A new connection plans from the inventory's advertisement again.
      _camera.reset();
      _replan();
      final plan = _plan;
      _step(33, ok: plan != null);
      if (plan == null) throw const BrokerException('invalid_response');
      final viewportOptions = androidViewportOptions(
        plan.size,
        _options,
        portrait: widget.streamSize && _decoderLimits != null,
        fullWorkspace: widget.streamSizeV2,
      );
      final options = !_dualOutputs
          ? viewportOptions
          : StreamOptions(
              maxWidth: viewportOptions.maxWidth,
              maxHeight: viewportOptions.maxHeight,
              fps: viewportOptions.fps,
              bitrate: viewportOptions.bitrate,
              audio: viewportOptions.audio,
              bitrateMode: viewportOptions.bitrateMode,
              monitor: 'all',
            );
      final pending = widget.load(
        options,
        () => mounted && generation == _generation && !_allowPop,
        plan.displayScale,
      );
      var gaveUp = false;
      unawaited(
        pending.then<void>((connection) {
          // Never dial an abandoned reservation; release only its exact
          // session (closed route, newer attempt or an expired wait).
          if (gaveUp || !mounted || generation != _generation) {
            unawaited(connection.release().catchError((Object _) {}));
          }
        }, onError: (Object _) {}),
      );
      final BrokerConnection connection;
      try {
        // Cleanup observation (45 s), display settling (15 s) and the
        // preparation itself (30 s) are each bounded by the preparation.
        connection = await pending.timeout(const Duration(seconds: 100));
      } on TimeoutException {
        gaveUp = true;
        rethrow;
      }
      if (!mounted || generation != _generation) return;
      _serverRestart.reset();
      final media = AndroidRemoteMedia(audioEnabled: connection.options.audio);
      // The engine let input go itself (Ctrl+Alt+Shift+F12, or the window
      // lost focus): it is not taken back until the picture is touched or
      // the app is in front again.
      unawaited(_engineInput?.cancel());
      _engineInput = media.input.listen((event) {
        if (event['type'] == 'capture.released') {
          _captureHold = true;
          _tvInput.currentState?.cancel();
          _pen.reset();
          _penPointerEpoch++;
        }
      }, onError: (Object _) {});
      final onStats = widget.onStats;
      final StreamStatsSink? statsSink = onStats == null
          ? null
          : (values) =>
                onStats({...values, ..._statsInput()}, connection.sessionId);
      final session =
          widget.sessionFactory?.call(connection, media, statsSink) ??
          RemoteSession(
            connection: connection,
            token: widget.token,
            media: media,
            statsSink: statsSink,
            // drive-v1 where granted: folders picked on this device.
            driveFactory: (connection, token) =>
                androidDriveSession(connection, token, files: _driveFiles),
            // usb-v1 where granted: 마우스 잠금's virtual USB mouse only.
            usbFactory: androidUsbSession,
          );
      session.onBrokerNotices = widget.onBrokerNotices;
      session.setAndroidForeground(_inFront);
      final actions = AndroidInputActions(
        ready: () =>
            mounted &&
            identical(session, _session) &&
            !session.closed &&
            !session.resizing &&
            session.frameReady &&
            session.textureId != null,
        acquired: () => session.captured,
        capture: session.capture,
        send: (event) => _sendToOutput(session, event),
      );
      session.addListener(_changed);
      session.caretUpdates.addListener(_caretChanged);
      setState(() {
        _session = session;
        _media = media;
        _actions = actions;
        // Hosts with text-input-v1 receive the field's text; older ones keys.
        _typing = AndroidTypingBridge(text: session.textInput);
        // A reconnect keeps showing the last picture until the new one.
        if (!auto) _continuity.reset();
        _seenScale = null;
      });
      _resetField();
      // The grant decides: without display-scale-v1 the output becomes an
      // exact pixel fraction of this screen (one resize after the start).
      _replan();
      unawaited(session.start());
    } catch (error) {
      final failure = SessionFailure.request(error);
      if (mounted && generation == _generation) {
        _step(35, ok: false);
        _takeoverOffer = _offeredTakeover(error);
        final wait = auto ? null : _serverRestart.next(error);
        if (wait != null) {
          // The server restarts (an update): one line, then this connect
          // tries again by itself, without a second press.
          _serverRestartRetry = Timer(wait, () {
            if (mounted && generation == _generation) {
              unawaited(_connect(again: true));
            }
          });
        } else if (auto &&
            _hostUpdate &&
            error is BrokerException &&
            error.code == 'host_update') {
          // The update ran past its wait, or its Host stayed away after it.
          _giveUpReconnect(hostUpdate: true);
        } else if (error is BrokerException &&
            error.code == 'host_maintenance' &&
            failure.canRetry) {
          _waitForPreparation();
        } else if (auto &&
            _reconnecting &&
            !_preparingAgain &&
            _retryable(failure) &&
            !androidReconnectRefusalFinal(error)) {
          // Still offline (or the old connection not released yet, or the
          // Host not back): the next attempt follows, within the budget.
          _lastReconnectFailure = failure;
          _offlineWait = androidReconnectOffline(error);
          // A broker restarting (502-504, a refused or reset port).
          final restart = error is BrokerException && error.serverRestarting;
          if (restart) _serverRestart.begin();
          _scheduleReconnect(restart: restart);
        } else {
          _stopReconnecting();
          setState(() {
            _error = _takeoverFailureMessage(failure);
            _requestFailure = (
              message: _error!,
              code: error is BrokerException ? error.code : failure.code,
            );
            _canRetry = failure.canRetry;
            _canTakeover =
                failure.takeover &&
                widget.takeover != null &&
                _takeoverAvailable(_takeoverOffer);
          });
        }
      }
    } finally {
      if (mounted && generation == _generation) {
        setState(() => _loading = false);
        if (_session == null || _session!.closed) _backgroundKeeper.stop();
      }
    }
  }

  void _changed() {
    final authentication = _session?.diagnosticCode;
    if (const {
      'pc_login_replaced',
      'session_idle_timeout',
      'session_reauthentication_required',
    }.contains(authentication)) {
      widget.onAuthenticationEnded?.call(authentication!);
      return;
    }
    if (!mounted) return;
    final session = _session;
    if (session?.resizing == true && _pen.active) {
      _penPointerEpoch++;
      _pen.reset();
    }
    if (session?.closed == true) {
      _pen.reset();
      _backgroundKeeper.stop();
      _clearNotice(rebuild: false);
    }
    _printOffers.watch(session?.closed == false ? session?.printer : null);
    final captured = session?.captured == true;
    if (!captured) {
      _buttons = 0;
      // Capture ended elsewhere (focus loss, a resize): the host already
      // released everything; forget a gesture in progress.
      if (_wasCaptured) {
        _touch.reset();
        _pen.reset();
        _penPointerEpoch++;
        _moves.discard();
      }
    }
    _wasCaptured = captured;
    setState(() {});
    // Input released, a resize or the end: the mouse comes back.
    _syncRelativeMouse();
    if (session?.textureId != null &&
        !session!.closed &&
        !session.resizing &&
        !_menuOpen) {
      _scheduleResize();
    }
    _syncScale();
    _scaleResult(session);
    _watchAudioDevice(session);
    _watchClipboard(session);
    _watchDrive(session);
    _watchSharp(session);
    if (session != null) {
      if (session.closed) {
        if (!identical(session, _endedSession)) {
          _endedSession = session;
          _sessionEnded(session);
        }
      } else if (_reconnecting && session.frameReady) {
        // Back: the new picture replaces the kept one.
        _stopReconnecting();
        setState(() {});
      }
      // A planned restart ends once a new stream shows the VM again.
      if (_plannedRestartTimer != null &&
          !session.closed &&
          session.frameReady &&
          !identical(session, _plannedRestartFrom)) {
        _endPlannedRestart();
      }
      // A Host update's wait ends once a new stream shows the VM again.
      if (_hostUpdateTimer != null && !session.closed && session.frameReady) {
        _endHostUpdate();
      }
      if (!session.closed &&
          session.frameReady &&
          !identical(session, _markedSession)) {
        // This connection runs on the current default network.
        _markedSession = session;
        unawaited(widget.network?.mark());
      }
    }
  }

  StreamSubscription<String>? _networkWatch;
  RemoteSession? _markedSession;

  /// The network the connection was on is gone: end it now and reconnect
  /// quietly instead of waiting for a timeout; a new network: retry now,
  /// once per wait for the network. The engine repeats `available` while
  /// another network than the marked one stays the default (each change of
  /// its capabilities), and a Host that refused (409 while it restarts) is
  /// not helped by a network: those waits keep their back-off (otherwise
  /// three connects could follow within 2 s while a Host restarts).
  void _networkChanged(String event) {
    if (!mounted || _allowPop) return;
    final session = _session;
    if (event == 'lost') {
      if (_reconnecting) _earlyRetry = true;
      if (session != null && !session.closed && session.frameReady) {
        unawaited(session.networkLost());
      }
    } else if (event == 'available' &&
        _reconnecting &&
        _reconnectTimer != null &&
        !_loading &&
        _offlineWait &&
        _earlyRetry) {
      _earlyRetry = false;
      _reconnectTimer?.cancel();
      _reconnectTimer = null;
      _reconnectAttempts++;
      unawaited(_connect(auto: true));
    }
  }

  // --- quiet reconnect -------------------------------------------------------

  /// How long a dropped connection is retried, and the waits between tries
  /// (each up to a fifth shorter by jitter, androidReconnectDelay).
  static const reconnectBudget = Duration(seconds: 90);
  static const _reconnectDelays = [1, 2, 4, 8, 8, 8, 8, 8, 8, 8];
  static const _preparationBudget = Duration(seconds: 30);
  static const _preparationDelays = [1, 2, 4, 4, 4];
  final _random = math.Random();
  bool _preparingAgain = false;
  Timer? _preparationTimer;
  bool _reconnecting = false;
  DateTime? _reconnectSince;

  /// How long this wait has lasted: from the connection the user asked for,
  /// or from the moment a running connection dropped, through every
  /// automatic retry. The list's own clock restarts with each attempt, so
  /// with it the time would fall back to 00:00 at every retry.
  final _waitClock = Stopwatch();
  int _reconnectAttempts = 0;
  Timer? _reconnectTimer;
  RemoteSession? _endedSession;
  SessionFailure? _lastReconnectFailure;

  /// The wait is for the network (the broker was not reached), and a newly
  /// available network may still cut it short once (see [_networkChanged]).
  bool _offlineWait = false, _earlyRetry = false;

  /// Restart recovery (host-ready-wait-v1): while the broker
  /// restarts each try first waits on it and on the Host
  /// ([AndroidRemoteView.awaitBrokerRestart], at most [restartWaitLimit]),
  /// until one such wait fails; then the back-off alone decides.
  static const restartWaitLimit = Duration(seconds: 30);
  bool _restartWaitFailed = false;
  bool get _restartRecovery =>
      _serverRestart.waiting &&
      widget.awaitBrokerRestart != null &&
      !_restartWaitFailed &&
      !_preparingAgain;

  bool _retryable(SessionFailure failure) =>
      failure.canRetry &&
      const {
        'connection_timeout',
        'connection_failed',
        'connection_rejected',
        'host_disconnected',
        'host_maintenance',
        'desktop_busy',
      }.contains(failure.code);

  /// A connection that ended by itself (not closed here): a transient end is
  /// retried quietly while the last picture stays on screen; anything else
  /// shows its reason.
  void _sessionEnded(RemoteSession session) {
    if (mounted && identical(session, _session) && session.closed) {
      _retireTyping();
    }
    if (_allowPop || _closing || _resumeConnect || !mounted) return;
    final preparing =
        session.canRetryInitialPreparation ||
        (_preparingAgain && session.canRetryInitialDisplayPreparation);
    // A restart asked here: however the stream ended (the Host went away with
    // Windows), wait for the VM; only an authorization end stays an end.
    final restarting =
        _plannedRestart &&
        !SessionFailure.blocksAutomaticReconnect(session.diagnosticCode);
    // A Host update ended it: wait for the update and the Host, then
    // reconnect (as on Windows); any other end keeps its own rule.
    final updating =
        session.diagnosticCode == 'host_update' && session.canRetry;
    final transient =
        (session.canAutomaticallyReconnect ||
            preparing ||
            restarting ||
            updating) &&
        session.connection.trackPlan == null;
    if (!transient) {
      if (_reconnecting) {
        _stopReconnecting();
        setState(() {});
      }
      return;
    }
    // Also in the background: the wait goes on when the user comes back.
    if (updating) _beginHostUpdate();
    final lifecycle = WidgetsBinding.instance.lifecycleState;
    if (lifecycle == AppLifecycleState.hidden ||
        lifecycle == AppLifecycleState.paused ||
        lifecycle == AppLifecycleState.detached) {
      // Not in front: connect again when the user comes back.
      _stopReconnecting();
      _resumeConnect = true;
      return;
    }
    if (preparing) {
      _waitForPreparation();
      return;
    }
    // A broker update: the server is said to restart until a new stream
    // starts, and the reconnect waits on it at once (as on Windows).
    final serverRestart = session.diagnosticCode == 'server_stopping';
    if (serverRestart) _serverRestart.begin();
    // A transport end may be the network; a Host that went away is not.
    _offlineWait = !session.endedByHostLoss && !restarting && !updating;
    _hostLossWait =
        session.endedByHostLoss && !restarting && !updating && !serverRestart;
    if (!_reconnecting) _earlyRetry = true;
    if (_reconnectSince == null) {
      _waitClock
        ..reset()
        ..start();
    }
    _reconnectSince ??= DateTime.now();
    _scheduleReconnect(restart: serverRestart);
  }

  // A Host may finish the previous worker between a fresh inventory read
  // and this reservation. Only its explicit, pre-picture maintenance reply
  // gets this short retry path; every try re-enters the preparation/cleanup
  // checks and releases only our own previous session in _connect.
  void _waitForPreparation() {
    if (!_preparingAgain) {
      _stopReconnecting();
      _preparingAgain = true;
      _preparationTimer = Timer(_preparationBudget, () {
        if (!mounted || !_preparingAgain || _allowPop) return;
        // Revoke an in-flight load too. Its late reservation is released by
        // the generation guard and can never open a replacement socket.
        ++_generation;
        _loading = false;
        _backgroundKeeper.stop();
        unawaited(_session?.close());
        _giveUpReconnect();
      });
    }
    _lastReconnectFailure = SessionFailure.ended('host_maintenance');
    _scheduleReconnect();
  }

  /// [restart]: the broker is restarting (server_stopping, 502-504, a
  /// refused or reset port): with restart recovery the next try starts at
  /// once and paces itself; every other failure keeps the back-off.
  void _scheduleReconnect({bool restart = false}) {
    final since = _reconnectSince ?? DateTime.now();
    _reconnectSince = since;
    final delays = _preparingAgain ? _preparationDelays : _reconnectDelays;
    final budget = _preparingAgain ? _preparationBudget : reconnectBudget;
    // A planned restart or a Host update keeps trying, at the longest delay,
    // until its bound.
    final restarting = !_preparingAgain && (_plannedRestart || _hostUpdate);
    if (!restarting &&
        (_reconnectAttempts >= delays.length ||
            DateTime.now().difference(since) >= budget)) {
      _giveUpReconnect();
      return;
    }
    final delay = restart && _restartRecovery
        ? restartPollInterval
        : androidReconnectDelay(
            delays,
            _reconnectAttempts,
            (widget.reconnectJitter ?? _random.nextDouble)(),
          );
    _reconnectTimer?.cancel();
    _reconnectTimer = Timer(delay, () {
      _reconnectTimer = null;
      if (!mounted || !_reconnecting || _allowPop) return;
      _reconnectAttempts++;
      unawaited(_connect(auto: true));
    });
    if (!_reconnecting) setState(() => _reconnecting = true);
  }

  void _giveUpReconnect({bool restart = false, bool hostUpdate = false}) {
    final failure = _lastReconnectFailure;
    final preparation = _preparingAgain;
    _endPlannedRestart();
    _endHostUpdate();
    _stopReconnecting();
    setState(() {
      _error = preparation
          ? l10n.hostsVirtualDisplayIsntReady
          : hostUpdate
          ? SessionFailure.ended('host_update').message
          : restart
          ? l10n.desktopWasntReadyWithin3
          : failure?.code == 'desktop_busy'
          ? _takeoverFailureMessage(failure!)
          : l10n.connectionEndedCouldntReconnectCheck;
      _canRetry = true;
      _canTakeover =
          failure?.takeover == true &&
          widget.takeover != null &&
          _takeoverAvailable(_takeoverOffer);
    });
  }

  void _stopReconnecting() {
    _reconnectTimer?.cancel();
    _reconnectTimer = null;
    _preparationTimer?.cancel();
    _preparationTimer = null;
    _reconnecting = false;
    _preparingAgain = false;
    _reconnectSince = null;
    _reconnectAttempts = 0;
    _lastReconnectFailure = null;
    _offlineWait = _earlyRetry = _restartWaitFailed = false;
  }

  /// display-scale-v1: the Windows scale of the current plan, once the
  /// picture is up (repeats are ignored by the session).
  void _syncScale() {
    final session = _session, wanted = _plan?.displayScale;
    if (!mounted ||
        _closing ||
        _allowPop ||
        !_inFront ||
        _viewportSettling ||
        session == null ||
        session.closed ||
        wanted == null ||
        !session.displayScaleSupported ||
        !session.frameReady) {
      return;
    }
    session.requestDisplayScale(wanted);
  }

  /// Tells what Windows did with a scale the user just chose (it may allow
  /// less for this screen's size), or that it could not change it.
  void _scaleResult(RemoteSession? session) {
    final applied = session?.displayScaleApplied;
    if (session == null || applied == null || identical(applied, _seenScale)) {
      return;
    }
    _seenScale = applied;
    final wanted = session.displayScaleWanted;
    if (!applied.ok) {
      if (_scaleChanged) _notice(l10n.couldntChangeWindowsScaleScreen);
    } else if (_scaleChanged && wanted != null && applied.scale < wanted) {
      _notice(l10n.setLargestScaleWindowsAllows(applied.scale));
    }
    _scaleChanged = false;
  }

  /// Input goes back to this device: pending moves, a gesture in progress,
  /// held mouse buttons and one-shot modifiers are dropped and the session
  /// resets the remote input.
  void _release() {
    _tvInput.currentState?.cancel();
    _actions?.invalidate();
    _pointerRevision++;
    _touch.reset();
    _pen.reset();
    _penPointerEpoch++;
    _moves.discard();
    _buttons = 0;
    if (_latched.isNotEmpty || _locked.isNotEmpty) {
      _latched.clear();
      _locked.clear();
      if (mounted) setState(() {});
    }
    unawaited(_session?.capture(false));
  }

  DateTime? _captureTried;
  StreamSubscription<Map<String, dynamic>>? _engineInput;
  bool _captureHold = false;

  /// The app is in front (no state reported yet counts as in front).
  static bool get _inFront {
    final state = WidgetsBinding.instance.lifecycleState;
    return state == null || state == AppLifecycleState.resumed;
  }

  /// An external keyboard types into the remote whenever the session is on
  /// screen, as in ToTBDesk (its remote view keeps the focus), not only
  /// after a touch: input is captured again once nothing else (a menu, a
  /// dialog, the gesture overview, another app) has it. Toolbar actions
  /// still reset the remote input first; a release by the engine itself
  /// ([_captureHold]) is respected.
  void _captureWhenIdle() {
    final session = _session;
    if (AndroidTvScope.of(context) &&
        (_tvMenuOpen ||
            _keyboard ||
            _toolPanel != null ||
            _barKey.currentState?.isOpen == true)) {
      return;
    }
    if (_captureQueued ||
        _captureHold ||
        session == null ||
        session.closed ||
        session.captured ||
        session.resizing ||
        !session.frameReady ||
        _menuOpen ||
        _queued > 0 ||
        !_inFront) {
      return;
    }
    final tried = _captureTried;
    if (tried != null &&
        DateTime.now().difference(tried) < const Duration(seconds: 2)) {
      return;
    }
    _captureQueued = true;
    WidgetsBinding.instance.addPostFrameCallback((_) {
      _captureQueued = false;
      if (!mounted ||
          !identical(session, _session) ||
          session.closed ||
          session.captured ||
          _menuOpen) {
        return;
      }
      _captureTried = DateTime.now();
      _deliver(const []);
    });
  }

  bool _current(RemoteSession session, int revision) =>
      mounted &&
      identical(session, _session) &&
      !session.closed &&
      !session.resizing &&
      session.frameReady &&
      revision == _pointerRevision &&
      _inFront &&
      !_menuOpen;

  /// Sends input messages in order, taking input capture first when needed
  /// (the same capture the Windows client takes on a click).
  void _deliver(
    List<Map<String, dynamic>> events, {
    VoidCallback? onDiscarded,
  }) {
    final session = _session;
    if (events.isNotEmpty) _captureHold = false;
    if (session == null ||
        session.closed ||
        session.resizing ||
        !session.frameReady ||
        !_inFront ||
        _menuOpen ||
        events.isEmpty && session.captured) {
      if (events.isNotEmpty) onDiscarded?.call();
      return;
    }
    if (_queued == 0 && session.captured) {
      for (final event in events) {
        _sendToOutput(session, event);
      }
      return;
    }
    final revision = _pointerRevision;
    final pointerEpoch = _penPointerEpoch;
    final pointerBatch = events.any(
      (event) => const {'move', 'button', 'wheel'}.contains(event['event']),
    );
    bool current() =>
        _current(session, revision) &&
        (!pointerBatch || pointerEpoch == _penPointerEpoch);
    _queued++;
    _inputQueue = _inputQueue
        .then((_) async {
          if (!current()) {
            onDiscarded?.call();
            return;
          }
          if (!session.captured) await session.capture(true);
          if (!current() || !session.captured) {
            onDiscarded?.call();
            return;
          }
          for (final event in events) {
            _sendToOutput(session, event);
          }
        })
        .catchError((Object _) {
          onDiscarded?.call();
        })
        .whenComplete(() => _queued--);
  }

  void _sendToOutput(RemoteSession session, Map<String, dynamic> event) {
    final selected = _selectedOutput;
    session.input(
      selected != null && event.containsKey('x') && event.containsKey('y')
          ? {...event, 'track': selected.id, 'epoch': selected.epoch}
          : event,
    );
  }

  void _selectOutput(String id) {
    _release();
    setState(() {
      _selectedTrackId = id;
      _continuity.reset();
      _camera.reset();
      _view = null;
    });
    _media?.cursorState.reset();
    _foldBar();
  }

  void _changeOutputCount(bool dual) {
    if (_dualOutputs == dual) return;
    _release();
    setState(() {
      _dualOutputs = dual;
      _selectedTrackId = null;
    });
    unawaited(_connect(again: true));
  }

  void _touchEvent(Map<String, dynamic> event) {
    if (event['event'] == 'move') {
      _moves.move(event['x'] as double, event['y'] as double);
      return;
    }
    _moves.flush();
    if (event['event'] == 'button') {
      // Text still being composed lands where the caret was, before the
      // click moves it.
      if (event['down'] == true) _flushPending();
      _typing.commitRemote();
    }
    _deliver([event]);
  }

  /// This window's input counts for the diagnostics row, with how the
  /// picture is sized (Windows scale applied/asked for, device pixels per
  /// remote pixel).
  Map<String, Object> _statsInput() {
    final session = _session;
    final applied = session?.displayScaleApplied;
    final wanted = session?.displayScaleWanted;
    final view = _view, source = _viewSource;
    // A requested integer plan does not prove the shown texture is 1:1.
    // Unknown is omitted; actual fractional fitting and zoom are recorded as 0.
    final known = view != null && source != null;
    _viewPixelScale = known ? view.wholePixelScale(source, _dpr) : null;
    final pixels = known ? (_viewPixelScale ?? 0) : null;
    return {
      ..._counters.take(
        mode: _touch.mode,
        textInput: session?.textInput == true,
      ),
      if (session != null && session.displayScaleSupported)
        'displayScale': applied?.ok == true ? applied!.scale : 0,
      'displayScaleWanted': ?wanted,
      'pixelScale': ?pixels,
    };
  }

  void _cue(AndroidTouchCue cue, Offset? at) {
    _counters.cue(cue);
    switch (cue) {
      // Felt the moment the finger lifts, although the click itself waits
      // out the double-tap window.
      case AndroidTouchCue.tap:
        unawaited(HapticFeedback.lightImpact());
        // The keyboard is open: the field tapped is where typing goes.
        if (_camera.open) setState(() => _camera.tapped(_touch.cursor));
      case AndroidTouchCue.rightClick:
        // Android's own long-press feel (LONG_PRESS), unlike a tap's click.
        unawaited(HapticFeedback.vibrate());
      case AndroidTouchCue.hold:
        unawaited(HapticFeedback.mediumImpact());
      case AndroidTouchCue.dragStart:
        unawaited(HapticFeedback.lightImpact());
      case AndroidTouchCue.dragEnd:
        unawaited(HapticFeedback.selectionClick());
      case AndroidTouchCue.zoomLimit:
        unawaited(HapticFeedback.selectionClick());
      case AndroidTouchCue.keyboard:
        unawaited(HapticFeedback.selectionClick());
        _toggleKeyboard();
        return;
      case AndroidTouchCue.touchMode:
        unawaited(HapticFeedback.mediumImpact());
        _toggleTouchModeByGesture();
        return;
      default:
        break;
    }
    _cues.value = (cue, at, ++_cueCount);
  }

  /// Four-finger tap or the floating button updates the mode without a popup.
  void _toggleTouchModeByGesture() {
    final next = _touch.mode == AndroidTouchMode.direct
        ? AndroidTouchMode.trackpad
        : AndroidTouchMode.direct;
    _setTouchMode(next);
  }

  // --- keys ----------------------------------------------------------------

  List<Map<String, dynamic>> _keyEvents(AndroidKeyStroke stroke) {
    final modifiers = <int>[
      for (final m in AndroidModifier.values)
        if (_latched.contains(m) || _locked.contains(m)) m.vk,
    ];
    if (stroke.shift && !modifiers.contains(0xa0)) modifiers.add(0xa0);
    Map<String, dynamic> key(int vk, bool down) => {
      'type': 'input',
      'event': 'key',
      'vk': vk,
      'scan': 0,
      'extended': androidExtendedKeys.contains(vk),
      'down': down,
    };
    return [
      for (final vk in modifiers) key(vk, true),
      {...key(stroke.vk, true), 'extended': stroke.extended},
      {...key(stroke.vk, false), 'extended': stroke.extended},
      for (final vk in modifiers.reversed) key(vk, false),
    ];
  }

  /// Key presses and committed text, in order, as one delivery. With Ctrl,
  /// Alt or Win held from the key row, characters are shortcuts (Ctrl+C),
  /// so they go as keys rather than text.
  void _sendTyped(List<Object> actions, {VoidCallback? onDiscarded}) {
    if (actions.isEmpty) return;
    // A tap's click still waiting out the double-tap window lands first.
    _touch.flushPendingClick();
    _moves.flush();
    final shortcut = [
      AndroidModifier.ctrl,
      AndroidModifier.alt,
      AndroidModifier.win,
    ].any((m) => _latched.contains(m) || _locked.contains(m));
    final events = <Map<String, dynamic>>[];
    for (final action in actions) {
      if (action is AndroidKeyStroke) {
        events.addAll(_keyEvents(action));
        continue;
      }
      final text = action as String;
      final session = _session;
      if (!shortcut && session != null && session.textInput) {
        final messages = textInputMessages(text);
        if (messages.isEmpty) continue;
        events.addAll(messages);
        _counters.textCommits++;
        _counters.textUnits += text.length;
        continue;
      }
      for (final rune in text.runes) {
        final strokes = androidStrokesForRune(rune);
        if (strokes == null) {
          _counters.textDropped++;
          continue;
        }
        for (final stroke in strokes) {
          events.addAll(_keyEvents(stroke));
        }
      }
    }
    if (_latched.isNotEmpty) setState(_latched.clear);
    _deliver(events, onDiscarded: onDiscarded);
  }

  /// Text mode: sends what the phone keyboard is still composing and starts
  /// the field over (the keyboard's composition ends there).
  void _flushPending() {
    if (!_typing.text) return;
    final pending = _typing.pending;
    _resetField();
    if (pending.isNotEmpty) _sendTyped([pending]);
  }

  /// A key from the key row or a navigation key from the phone keyboard:
  /// pending text goes first, the remote composition ends and the field
  /// starts over.
  void _key(AndroidKeyStroke stroke) {
    _flushPending();
    _typing.commitRemote();
    _resetField();
    _counters.keyPresses++;
    _sendTyped([stroke]);
  }

  void _modifier(AndroidModifier modifier, {required bool lock}) {
    setState(() {
      if (lock) {
        _latched.remove(modifier);
        if (!_locked.remove(modifier)) _locked.add(modifier);
      } else if (_locked.remove(modifier) || _latched.remove(modifier)) {
        // Tapped again: released.
      } else {
        _latched.add(modifier);
      }
    });
  }

  void _resetField() {
    ++_typingEpoch;
    _typing.clear();
    if (_field.value == AndroidTypingBridge.initial) return;
    _applying = true;
    _field.value = AndroidTypingBridge.initial;
    _applying = false;
  }

  /// A connection boundary or discarded capture retires uncertain composition.
  /// Do not flush or replay text whose native delivery cannot be confirmed.
  void _retireTyping() {
    if (!mounted) return;
    _session?.watchCaret(false);
    setState(() {
      _keyboard = false;
      _imeSeen = false;
      _latched.clear();
      _locked.clear();
    });
    _fieldFocus.unfocus();
    _resetField();
  }

  void _typed() {
    if (_applying || !_keyboard) return;
    SessionAccessBoundary.human();
    if (!_inFront) {
      _retireTyping();
      return;
    }
    if (_session?.resizing == true) {
      // _deliver cannot send during a display transaction. Do not let the
      // bridge remember this edit as remote text: its next deletion could
      // otherwise backspace text that was already on the remote desktop.
      // Reset the controller under _applying together with the bridge.
      _resetField();
      _notice(l10n.couldntSendInputWhileScreen);
      return;
    }
    final owner = _session;
    final typingEpoch = _typingEpoch;
    final update = _typing.update(_field.value);
    if (!_typing.text) {
      _counters.textKeyChars += update.strokes
          .where((s) => s != AndroidKeyStroke.backspace)
          .length;
    }
    _sendTyped(
      update.actions,
      onDiscarded: () {
        if (!mounted ||
            !identical(owner, _session) ||
            typingEpoch != _typingEpoch) {
          return;
        }
        // The bridge advanced before capture. Retire every dependent queued edit
        // without replaying an input whose remote delivery may be uncertain.
        _release();
        _retireTyping();
        if (_inFront) _notice(l10n.couldntConfirmInputWasDelivered);
      },
    );
    if (typingEpoch != _typingEpoch || !_keyboard) return;
    final reset = update.reset;
    if (reset != null && _field.value != reset) {
      _applying = true;
      _field.value = reset;
      _applying = false;
    }
    if (update.hangul && !_hangulHinted) {
      _hangulHinted = true;
      _notice(
        l10n.hostReceivesKoreanTextAs,
      );
    }
    if (update.unsupported && !_unsupportedHinted) {
      _unsupportedHinted = true;
      _counters.textDropped++;
      _notice(
        _typing.text
            ? l10n.controlCharactersCantSentSo
            : l10n.hostCantReceiveSomeCharacters,
      );
    }
  }

  KeyEventResult _fieldKey(FocusNode _, KeyEvent event) {
    if (AndroidTvScope.of(context)) return KeyEventResult.ignored;
    if (event is KeyUpEvent) return KeyEventResult.ignored;
    // Let Android's InputConnection edit the focused multiline field. Sending
    // Enter here too resets its history before a late composing snapshot and
    // replays that text. Backspace and newline edits already become remote keys.
    if (event.logicalKey == LogicalKeyboardKey.backspace ||
        event.logicalKey == LogicalKeyboardKey.enter ||
        event.logicalKey == LogicalKeyboardKey.numpadEnter) {
      return KeyEventResult.ignored;
    }
    final stroke = androidStrokeForKey(event.logicalKey);
    if (stroke == null) return KeyEventResult.ignored;
    _key(stroke);
    return KeyEventResult.handled;
  }

  /// caret-v1 report (or none) of the Host: the camera keeps the caret in view.
  void _caretChanged() {
    final caret = _session?.caretUpdates.value?.fraction;
    _camera.caret(
      caret == null
          ? null
          : Rect.fromLTRB(caret.left, caret.top, caret.right, caret.bottom),
    );
    if (_keyboard && mounted) setState(() {});
  }

  void _toggleKeyboard() {
    final session = _session;
    if (_keyboard) {
      // What was still being composed is typed before the keyboard closes;
      // held Ctrl/Alt/Shift are let go with it.
      _flushPending();
      session?.watchCaret(false);
      setState(() {
        _keyboard = false;
        _latched.clear();
        _locked.clear();
      });
      _fieldFocus.unfocus();
      _resetField();
      return;
    }
    if (session == null || session.closed || !session.frameReady) return;
    _resetField();
    _counters.keyboardOpens++;
    setState(() {
      _keyboard = true;
      _imeSeen = false;
      _tvImeOwner = Object();
      // The move handle shows by itself while part of the picture is hidden.
      _toolPanel = null;
      _moving = false;
    });
    // caret-v1: the Host reports where the text caret is while the keyboard
    // is open, and the picture follows it (older Hosts: taps).
    session.watchCaret(true);
    if (_fieldFocus.hasFocus) {
      // Focused already (Android hid its keyboard meanwhile): show it again.
      unawaited(
        SystemChannels.textInput
            .invokeMethod<void>('TextInput.show')
            .catchError((Object _) {}),
      );
    } else {
      _fieldFocus.requestFocus();
    }
    // A hardware keyboard reaches the remote only while input is captured.
    _deliver(const []);
  }

  // --- pointer ------------------------------------------------------------

  bool get _penReady {
    final session = _session;
    return mounted &&
        !_closing &&
        !_allowPop &&
        _inFront &&
        !_menuOpen &&
        _view != null &&
        session != null &&
        !session.closed &&
        !session.resizing &&
        session.frameReady;
  }

  void _penEvent(Map<String, dynamic> event) {
    if (!_penReady) {
      return;
    }
    final point = Offset(event['x'] as double, event['y'] as double);
    _touch.cursor = point;
    _media?.cursorState.pointer(point);
    _touchEvent(event);
  }

  /// Retire only pointer producers. Independent queued keys/text and the
  /// keyboard's selected modifiers keep their existing owner and order.
  void _penTakeOver() {
    final fingerDrag = _touch.dragging,
        mouseButtons = _buttons,
        at = _touch.cursor;
    _penPointerEpoch++;
    _moves.discard();
    _touch.reset(); // Also cancels only this gesture's real touch contacts.
    _buttons = 0;
    for (final item in const [
      (kPrimaryMouseButton, 'left'),
      (kSecondaryMouseButton, 'right'),
      (kMiddleMouseButton, 'middle'),
    ]) {
      if (mouseButtons & item.$1 != 0 || item.$2 == 'left' && fingerDrag) {
        _touchEvent({
          'type': 'input',
          'event': 'button',
          'button': item.$2,
          'down': false,
          'x': at.dx,
          'y': at.dy,
        });
      }
    }
  }

  void _pointer(PointerEvent event) {
    final session = _session;
    if (session == null ||
        _view == null ||
        session.closed ||
        !session.frameReady ||
        !_inFront ||
        _closing ||
        _allowPop ||
        session.resizing ||
        _menuOpen) {
      _pen.reset();
      if (event is PointerUpEvent || event is PointerCancelEvent) {
        _touch.cancel(event.pointer);
      }
      return;
    }
    // A finger or a pen on the picture takes the mouse back from the VM.
    if (event is PointerDownEvent &&
        event.kind != PointerDeviceKind.mouse &&
        _relativeMouse.capturing) {
      _relativeMouse.release();
    }
    // Proximity is not an explicit request to retake input after F12/native
    // capture loss. A contact Down still uses the ordinary capture queue.
    if (event is PointerHoverEvent &&
        (event.kind == PointerDeviceKind.stylus ||
            event.kind == PointerDeviceKind.invertedStylus) &&
        (_captureHold || !session.captured)) {
      return;
    }
    if (_pen.handle(event)) {
      return;
    }
    if (event is PointerDownEvent && _toolPanel != null) {
      // Touching the picture closes the bar's panel (the bar itself folds
      // by the pointer hold, unless pinned).
      setState(() => _toolPanel = null);
    }
    if (event.kind == PointerDeviceKind.mouse ||
        event.kind == PointerDeviceKind.trackpad) {
      if (_gameMouseClick(event)) return;
      _mouse(event);
      return;
    }
    final time = event.timeStamp;
    switch (event) {
      case PointerDownEvent():
        if (_touch.mode == AndroidTouchMode.trackpad) {
          final at = _media?.cursorState.position;
          if (at != null && !_touch.active) _touch.cursor = at;
        }
        _touch.down(event.pointer, event.localPosition, time);
      case PointerMoveEvent():
        _touch.move(event.pointer, event.localPosition, time);
      case PointerUpEvent():
        _touch.up(event.pointer, event.localPosition, time);
      case PointerCancelEvent():
        _touch.cancel(event.pointer);
      default:
        break;
    }
  }

  void _mouse(PointerEvent event) {
    final session = _session!, view = _view!;
    // A finger tap's waiting click goes before the mouse's own input.
    _touch.flushPendingClick();
    final point = view.normalize(event.localPosition);
    if (event is PointerScrollEvent) {
      if (point == null || !session.captured) return;
      _moves.flush();
      _deliver([
        {
          'type': 'input',
          'event': 'wheel',
          'delta': (-event.scrollDelta.dy * 3).round().clamp(-1200, 1200),
          'x': point.dx,
          'y': point.dy,
        },
      ]);
      return;
    }
    if (event is PointerDownEvent ||
        event is PointerUpEvent ||
        event is PointerCancelEvent) {
      if (point == null && event is PointerDownEvent) return;
      final at = point ?? view.normalizeClamped(event.localPosition);
      final buttons = event is PointerCancelEvent ? 0 : event.buttons;
      final changes = <Map<String, dynamic>>[
        for (final item in [
          (kPrimaryMouseButton, 'left'),
          (kSecondaryMouseButton, 'right'),
          (kMiddleMouseButton, 'middle'),
        ])
          if ((_buttons & item.$1) != (buttons & item.$1))
            {
              'type': 'input',
              'event': 'button',
              'button': item.$2,
              'down': buttons & item.$1 != 0,
              'x': at.dx,
              'y': at.dy,
            },
      ];
      _buttons = buttons;
      _touch.cursor = at;
      _media?.cursorState.pointer(at);
      if (changes.any((e) => e['down'] == true)) _flushPending();
      _typing.commitRemote();
      _counters.mouseButtons += changes.length;
      _moves.flush();
      _deliver(changes);
      return;
    }
    if ((event is PointerMoveEvent || event is PointerHoverEvent) &&
        point != null &&
        session.captured) {
      _touch.cursor = point;
      _media?.cursorState.pointer(point);
      _moves.move(point.dx, point.dy);
    }
  }

  // --- 마우스 잠금 (formerly 게임 마우스) -----------------------------------------

  /// Said when 마우스 잠금 is turned on, in a dialog with one 확인 (as on
  /// Windows): 확인 hands the mouse over at once.
  static String get gameMouseDialogTitle => l10n.mouseLock;
  static String get gameMouseDialogMessage =>
      l10n.tapOkLockMouseInside;

  /// Said at every hand-over, briefly.
  static String get gameMouseNotice => l10n.mouseLockedInsideDesktopPress;

  /// Added once per connection when the hand-over is injected movement,
  /// which some games (League of Legends) ignore.
  static String get gameMouseInjectedNotice => l10n.someGamesSuchAsLeague;

  /// The session can do it: relative-mouse-v1 granted.
  bool get _gameMouseOffered {
    final session = _session;
    return session != null && session.relativeMouse && !session.closed;
  }

  void _mouseChanged(bool present) {
    if (!mounted || present == _mousePresent) return;
    setState(() {
      _mousePresent = present;
      // The last mouse left: the option goes off with it.
      if (!present) _gameMouse = false;
    });
    _syncRelativeMouse();
  }

  void _syncRelativeMouse() {
    final session = _session;
    final available = _gameMouse && _mousePresent && _gameMouseOffered;
    _syncGameMouseUsb(available);
    _relativeMouse.update(
      available: available,
      // Nothing is drawn for a hidden remote cursor here.
      hidden: false,
      owned:
          available &&
          session!.captured &&
          !session.resizing &&
          session.frameReady &&
          _inFront &&
          !_menuOpen &&
          !_closing &&
          !_allowPop,
      // Presses reach this only from the picture.
      over: true,
    );
  }

  // While the option is on, the session's usb-v1 channel lends the VM the
  // virtual mouse a hand-over can then feed (usb_game_mouse.dart).
  void _syncGameMouseUsb(bool available) {
    final usb = _session?.usb;
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

  void _toggleGameMouse() {
    final enabled = !_gameMouse;
    setState(() => _gameMouse = enabled);
    _syncRelativeMouse();
    if (enabled) {
      unawaited(_explainGameMouse());
    } else {
      _notice(l10n.mouseLockOff);
    }
  }

  Future<void> _explainGameMouse() async {
    if (_gameMouseDialogOpen || !mounted) return;
    _gameMouseDialogOpen = true;
    _release();
    _menuOpen = true;
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
      _menuOpen = false;
    }
    if (confirmed) await _handOverGameMouse();
  }

  /// A mouse press on the picture with 마우스 잠금 on hands the mouse to the
  /// VM and is used up, its release with it: no click lands beside the
  /// hand-over. A finger or a pen never hands it over.
  bool _gameMouseClick(PointerEvent event) {
    if (event.pointer == _gameMousePress) {
      if (event is PointerUpEvent || event is PointerCancelEvent) {
        _gameMousePress = null;
      }
      return true;
    }
    // Held: the mouse reaches the VM natively, nothing here adds to it.
    if (_relativeMouse.capturing) return true;
    if (event is! PointerDownEvent ||
        event.kind != PointerDeviceKind.mouse ||
        !_gameMouse ||
        !_mousePresent ||
        !_gameMouseOffered) {
      return false;
    }
    _gameMousePress = event.pointer;
    unawaited(_handOverGameMouse());
    return true;
  }

  Future<void> _handOverGameMouse() async {
    final session = _session;
    if (!mounted ||
        !_gameMouse ||
        !_mousePresent ||
        !_gameMouseOffered ||
        session == null) {
      return;
    }
    // The virtual USB mouse (what League of Legends reads) starts attaching
    // when the option goes on. A hand-over before the VM confirms it would
    // use injected movement for the whole capture: wait for it, at most 4 s.
    final waits = _gameMouseUsb?.attaching == true;
    if (waits) setState(() => _gameMouseWaits++);
    try {
      for (
        var waited = 0;
        waited < 40 && _gameMouseUsb?.attaching == true;
        waited++
      ) {
        await Future<void>.delayed(const Duration(milliseconds: 100));
        if (!mounted || !_gameMouse || !identical(session, _session)) return;
      }
    } finally {
      if (waits) {
        _gameMouseWaits--;
        if (mounted) setState(() {});
      }
    }
    _captureHold = false;
    if (!session.captured) await session.capture(true);
    if (!mounted || !_gameMouse || !identical(session, _session)) return;
    _syncRelativeMouse();
    _relativeMouse.handOver();
  }

  void _relativeMouseChanged() {
    // The Host draws (or echoes) the VM's cursor exactly while the mouse is
    // held; nothing is sent where it was not granted.
    _session?.echoCursor(_relativeMouse.pinnedEngaged);
    _session?.composeCursor(_relativeMouse.composedEngaged);
    if (mounted) setState(() {});
  }

  void _relativeMouseEngaged() {
    final session = _session;
    if (_relativeMouse.usbPath ||
        session == null ||
        identical(session, _injectedNoticed)) {
      _notice(gameMouseNotice, duration: const Duration(milliseconds: 2500));
      return;
    }
    _injectedNoticed = session;
    _notice(
      '$gameMouseNotice\n$gameMouseInjectedNotice',
      duration: const Duration(seconds: 4),
    );
  }

  // --- viewport -------------------------------------------------------------

  /// The same "already this size" rule as [RemoteSession.resizeViewport]:
  /// with an initial viewport (or encoder limits) the encoded picture may be
  /// smaller than the actual output, so only the source size decides.
  bool _hasSize(RemoteSession session, int w, int h) =>
      session.sourceWidth == w &&
      session.sourceHeight == h &&
      (session.connection.initialViewport != null ||
          (session.width == w && session.height == h));

  void _scheduleResize() {
    final session = _session, viewport = _viewport;
    if (_closing ||
        _allowPop ||
        !_inFront ||
        _viewportSettling ||
        session == null ||
        !session.canResizeViewport ||
        viewport == null ||
        _resizeTimer != null ||
        _viewportInFlight != null ||
        session.resizing) {
      return;
    }
    StreamOptions requested;
    try {
      requested = androidViewportOptions(
        viewport,
        _options,
        portrait: session.streamSize,
        fullWorkspace: session.connection.streamSizeV2,
      );
    } on BrokerException {
      // Transient zero/small geometry while Android recreates its surface is
      // not an exception to throw from LayoutBuilder.
      return;
    }
    final w = requested.maxWidth, h = requested.maxHeight;
    final key = '$w:$h';
    if (w < 320 ||
        h < 180 ||
        _hasSize(session, w, h) ||
        _attemptedViewport == key) {
      return;
    }
    _resizeTimer = Timer(const Duration(milliseconds: 650), () {
      _resizeTimer = null;
      if (mounted &&
          !_closing &&
          !_allowPop &&
          _inFront &&
          !_viewportSettling &&
          identical(session, _session) &&
          !session.closed &&
          _viewport == viewport &&
          session.canResizeViewport &&
          !session.resizing &&
          !_menuOpen) {
        final revision = _viewportRevision;
        final flight = Object();
        var dispatched = false;
        var deferredByMenu = false;
        _viewportInFlight = flight;
        _release();
        unawaited(
          session
              .resizeViewport(
                w,
                h,
                isCurrent: () {
                  if (_menuOpen) deferredByMenu = true;
                  return mounted &&
                      !_closing &&
                      !_allowPop &&
                      _inFront &&
                      !_viewportSettling &&
                      identical(session, _session) &&
                      !session.closed &&
                      revision == _viewportRevision &&
                      _viewport == viewport &&
                      !_menuOpen;
                },
                onDispatched: () {
                  dispatched = true;
                  _attemptedViewport = key;
                },
              )
              .then((applied) {
                if (!applied &&
                    dispatched &&
                    mounted &&
                    identical(session, _session) &&
                    !session.closed &&
                    session.resizeError != null) {
                  final sharp = _sharpActive;
                  if (sharp != null && sharp.width == w && sharp.height == h) {
                    // The desktop does not take the sharper size: today's.
                    _sharpFellBack('host_refused');
                    return;
                  }
                  _notice(
                    l10n.couldntFitScreenSizeTap,
                  );
                }
              })
              .whenComplete(() {
                if (identical(_viewportInFlight, flight)) {
                  _viewportInFlight = null;
                  // A cancelled pre-send request must not consume the attempt.
                  // A newer stable foreground geometry can now be scheduled.
                  if (mounted &&
                      (revision != _viewportRevision ||
                          (!dispatched && deferredByMenu && !_menuOpen)) &&
                      identical(session, _session)) {
                    _scheduleResize();
                  }
                }
              }),
        );
      }
    });
  }

  /// The screen without a display cutout (camera notch): the remote desktop
  /// never sits under it, and the resolution follows the remaining area.
  static Rect _safeArea(BuildContext context, Size size) {
    var left = 0.0, top = 0.0, right = 0.0, bottom = 0.0;
    for (final feature in MediaQuery.displayFeaturesOf(context)) {
      if (feature.type != DisplayFeatureType.cutout) continue;
      final b = feature.bounds;
      final edges = [
        b.top,
        b.left,
        size.width - b.right,
        size.height - b.bottom,
      ];
      final nearest = edges.indexOf(edges.reduce(math.min));
      switch (nearest) {
        case 0:
          top = math.max(top, b.bottom);
        case 1:
          left = math.max(left, b.right);
        case 2:
          right = math.max(right, size.width - b.left);
        default:
          bottom = math.max(bottom, size.height - b.top);
      }
    }
    final area = Rect.fromLTRB(
      left,
      top,
      size.width - right,
      size.height - bottom,
    );
    return area.width < size.width / 2 || area.height < size.height / 2
        ? Offset.zero & size
        : area;
  }

  /// The picture area in device pixels and the window in dp (called from
  /// layout). A new shape (rotation, folding) reads the display's density
  /// again: an unfolded inner screen is another panel.
  void _measure(Rect area, double devicePixelRatio, Size window) {
    if (!_inFront) return;
    final physical = Size(
      (area.width * devicePixelRatio).roundToDouble(),
      (area.height * devicePixelRatio).roundToDouble(),
    );
    if (_physical == physical &&
        _dpr == devicePixelRatio &&
        _window == window) {
      return;
    }
    final reread = _physical != null && _physical != physical;
    ++_viewportRevision;
    _physical = physical;
    _dpr = devicePixelRatio;
    _window = window;
    if (_viewportSettling) _settleViewport();
    if (reread) _readDensity();
    _replan();
  }

  /// Reads the display's density again (never waited for: the app's value
  /// from its start plans the first connection; a different answer
  /// re-plans the automatic source and Windows scale from the latest metrics).
  void _readDensity() {
    final read = ++_densityRead;
    unawaited(
      widget.density().then<void>((density) {
        if (!mounted ||
            read != _densityRead ||
            density == null ||
            density == _density) {
          return;
        }
        _density = density;
        _replan();
        setState(() {});
      }, onError: (Object _) {}),
    );
  }

  /// Remember resolution/orientation per screen kind; Windows scale belongs
  /// to this streaming view and starts at 100% regardless of older settings.
  AndroidScreenPolicy get _policy => AndroidScreenPolicy.of(
    _window ?? (_physical ?? Size.zero) / _dpr,
    _dpr,
    _density,
    (kind) => _settings.screen(kind).copyWith(scale: _scalePreference),
    television: AndroidTvScope.of(context),
  );

  /// The remote output for this screen: its own pixels with a Windows
  /// display scale where the session (or, before one, the inventory) has
  /// display-scale-v1, otherwise an exact integer fraction of them; or the
  /// 해상도 and 화면 배율 chosen for this kind of screen. A new size is
  /// requested after the usual settle delay; a new Windows scale is sent
  /// once the layout is done.
  void _replan() {
    if (!_inFront || _viewportSettling) return;
    final physical = _physical;
    if (physical == null) return;
    final session = _session;
    _prepareSharp(physical);
    final policy = _planningPolicy;
    final plan = AndroidViewportPlan.of(
      physical,
      _dpr,
      policy,
      hostScale: session != null
          ? session.displayScaleSupported
          : widget.displayScale,
      // stream-size-v1: the session's declared limits once granted; before
      // a connection, this device's limits where the inventory advertises it.
      portraitLimits: session != null
          ? (session.streamSize ? session.connection.streamSize : null)
          : (widget.streamSize ? _decoderLimits : null),
      fps: _options.fps,
      fullWorkspace: session?.connection.streamSizeV2 ?? widget.streamSizeV2,
    );
    _orient(policy.choice.orientation);
    _plan = plan;
    if (_viewport != plan.size) {
      ++_viewportRevision;
      _viewport = plan.size;
      _resizeTimer?.cancel();
      _resizeTimer = null;
      _scheduleResize();
    }
    // Never notify the session's listeners from inside a layout.
    scheduleMicrotask(_syncScale);
  }

  // --- 선명하게 ---------------------------------------------------------------

  /// What the engine planned for [_sharpKey], or is planning.
  AndroidSharpPlan? _sharpPlan;
  String? _sharpKey, _sharpPendingKey;
  Future<void>? _sharpPlanning;

  /// The desktop did not give the planned picture in this view (a refused
  /// resize, or a picture encoded smaller): the automatic size until 선명하게
  /// is chosen again.
  bool _sharpRefused = false;
  String? _sharpRefusedReason;

  /// Windows' scale as the person set it, relative to the automatic size
  /// (1.0 = 100 % there): the scale asked of Windows follows the picture's
  /// size from it, so things keep their size when 선명하게 goes on or off.
  double _apparentScale = 1;

  bool get _hostScale => _session?.displayScaleSupported ?? widget.displayScale;

  bool get _sharpChosen {
    final policy = _policy;
    return policy.deviceClass != AndroidDeviceClass.television &&
        policy.choice.resolution.kind == AndroidResolutionKind.sharp;
  }

  String _sharpKeyFor(Size physical, Size automatic, bool portrait) =>
      '${physical.width.floor()}x${physical.height.floor()}:'
      '${automatic.width.round()}x${automatic.height.round()}:'
      '${_options.fps}:$portrait';

  /// The plan in force: 선명하게 is chosen, Windows' scale can follow, the
  /// engine planned something sharper for this very picture area, and the
  /// desktop has not refused it.
  AndroidSharpPlan? get _sharpActive {
    final plan = _sharpPlan, physical = _physical;
    if (plan == null ||
        physical == null ||
        _sharpRefused ||
        !plan.gains ||
        !_sharpChosen ||
        !_hostScale) {
      return null;
    }
    final automatic = _policy.recommendedSize(physical, _dpr);
    return _sharpKey == _sharpKeyFor(physical, automatic, _sharpPortrait)
        ? plan
        : null;
  }

  bool get _sharpPortrait => _session?.streamSize ?? widget.streamSize;

  /// The policy the remote output is planned with: 선명하게 with its planned
  /// size, or as 자동 while there is none.
  AndroidScreenPolicy get _planningPolicy {
    final policy = _policy;
    if (policy.choice.resolution.kind != AndroidResolutionKind.sharp) {
      return policy;
    }
    final plan = _sharpActive;
    return AndroidScreenPolicy(
      deviceClass: policy.deviceClass,
      density: policy.density,
      window: policy.window,
      choice: plan == null
          ? policy.choice.copyWith(resolution: AndroidResolution.auto)
          : policy.choice,
      sharp: plan?.size,
    );
  }

  /// Asks the engine for the plan of this picture area (once per area, frame
  /// rate and grant) and keeps Windows' scale in step with the size in force.
  void _prepareSharp(Size physical) {
    if (!_sharpChosen) {
      if (_sharpPlan != null || _sharpKey != null || _sharpRefused) {
        _sharpPlan = null;
        _sharpKey = _sharpPendingKey = null;
        _sharpPlanning = null;
        _sharpRefused = false;
        _sharpRefusedReason = null;
        widget.sharpPicture.off();
      }
      _syncSharpScale();
      return;
    }
    if (_hostScale && !_sharpRefused) {
      final automatic = _policy.recommendedSize(physical, _dpr);
      final portrait = _sharpPortrait;
      final key = _sharpKeyFor(physical, automatic, portrait);
      if (!automatic.isEmpty && _sharpKey != key && _sharpPendingKey != key) {
        _sharpPendingKey = key;
        _sharpPlanning = widget.sharpPicture
            .plan(
              panel: physical,
              base: automatic,
              fps: _options.fps,
              portrait: portrait,
            )
            .then<void>((plan) {
              if (!mounted || _sharpPendingKey != key) return;
              _sharpPendingKey = null;
              _sharpPlanning = null;
              _sharpPlan = plan;
              _sharpKey = key;
              if (plan != null && plan.fallback) {
                _notice(l10n.deviceCantReceiveSharperPicture);
              }
              _replan();
              setState(() {});
            }, onError: (Object _) {});
      }
    }
    _syncSharpScale();
  }

  /// The Windows scale for the size in force, from [_apparentScale]. The
  /// engine's own step is used where the person changed nothing.
  void _syncSharpScale() {
    if (AndroidTvScope.of(context) || _scalePreference == 0) return;
    final plan = _sharpActive;
    final wanted = plan != null && _apparentScale == 1
        ? plan.scale
        : nearestDisplayScale(_apparentScale * (plan?.factor ?? 1));
    _scalePreference = wanted < 100 ? 100 : (wanted > 200 ? 200 : wanted);
  }

  /// The desktop did not give the planned picture: the automatic size for
  /// the rest of this view, counted in the statistics, said once.
  void _sharpFellBack(String reason) {
    if (_sharpRefused || _sharpActive == null) return;
    _sharpRefused = true;
    _sharpRefusedReason = reason;
    widget.sharpPicture.hostFallback(reason);
    _notice(l10n.couldntApplySharpSoShows);
    _attemptedViewport = null;
    _replan();
    setState(() {});
  }

  /// The desktop has the planned size but encodes the picture smaller (its
  /// own load, an administrator cap, or this device's decoder falling
  /// behind): that is not sharper, only smaller.
  void _watchSharp(RemoteSession? session) {
    final plan = _sharpActive;
    if (plan == null ||
        session == null ||
        session.closed ||
        session.resizing ||
        !session.frameReady) {
      return;
    }
    final limit = session.encodeLimit;
    if (limit != null &&
        limit != EncodeLimit.none &&
        session.sourceWidth == plan.width &&
        session.sourceHeight == plan.height &&
        session.width > 0 &&
        session.width < plan.width) {
      _sharpFellBack('host_reduced');
    }
  }

  /// 화면 방향 for this kind of screen while the session is shown.
  void _orient(AndroidOrientation orientation) {
    if (_orientation == orientation) return;
    final first = _orientation == null;
    _orientation = orientation;
    // The device's own rotation is already in force at the start.
    if (first && orientation == AndroidOrientation.auto) return;
    scheduleMicrotask(
      () => unawaited(
        SystemChrome.setPreferredOrientations(orientation.preferred)
            .catchError((Object _) {}),
      ),
    );
  }

  // --- commands -------------------------------------------------------------

  String? _noticeMessage;
  Timer? _noticeTimer;
  int _noticeRevision = 0;

  void _clearNotice({bool rebuild = true}) {
    _noticeTimer?.cancel();
    _noticeTimer = null;
    ++_noticeRevision;
    if (rebuild && mounted) {
      setState(() => _noticeMessage = null);
    } else {
      _noticeMessage = null;
    }
  }

  /// Said once per view when the Host reports no playback device: long
  /// enough to read, and no action (the administrator adds the device in
  /// Proxmox).
  bool _audioMissingShown = false;
  void _watchAudioDevice(RemoteSession? session) {
    if (_audioMissingShown ||
        session == null ||
        !session.remoteAudioMissing ||
        !session.frameReady) {
      return;
    }
    _audioMissingShown = true;
    WidgetsBinding.instance.addPostFrameCallback(
      (_) => _notice(
        audioDeviceMissingText,
        duration: const Duration(seconds: 15),
      ),
    );
  }

  /// Clipboard sharing is automatic and silent, as on Windows: a
  /// VM copy lands on this device, a copy made here goes to the VM when the
  /// session is in front. What surfaces is what the PC says too: a paste in
  /// the VM that could not be applied, and sharing that stopped (once).
  Object? _clipboardStopNoticed;
  void _watchClipboard(RemoteSession? session) {
    final clipboard = session?.clipboard;
    if (session == null || clipboard == null || session.closed) return;
    final paste = clipboard.takePasteNotice();
    if (paste != null) {
      WidgetsBinding.instance.addPostFrameCallback(
        (_) => _notice(paste, duration: const Duration(seconds: 6)),
      );
    }
    if (clipboard.closed &&
        clipboard.error != null &&
        clipboard.error != 'clipboard_session_closed' &&
        !session.clipboardRecovering &&
        // Not while the stream itself is away: see RemoteView._watchClipboard.
        !session.resuming &&
        !identical(_clipboardStopNoticed, clipboard)) {
      _clipboardStopNoticed = clipboard;
      final notice = clipboardStopNotice(clipboard.error, clipboard.status);
      WidgetsBinding.instance.addPostFrameCallback(
        (_) => _notice(notice, duration: const Duration(seconds: 6)),
      );
    }
  }

  /// A desktop restart under this view (as on Windows): while
  /// [_plannedRestartTimer] runs, the ended stream waits for the desktop and
  /// reconnects instead of failing. The view never asks for one itself; the
  /// Host going away with the desktop running starts it
  /// ([_hostAwayRestart]).
  Timer? _plannedRestartTimer;
  RemoteSession? _plannedRestartFrom;
  static const plannedRestartWait = Duration(minutes: 3);
  bool get _plannedRestart => _plannedRestartTimer?.isActive == true;

  void _endPlannedRestart() {
    _plannedRestartTimer?.cancel();
    _plannedRestartTimer = null;
    _plannedRestartFrom = null;
    _hostAwayRestart = false;
  }

  /// The wait began because the Host went away with the VM running (Windows
  /// restarting inside the VM), not from this view's own restart.
  bool _hostAwayRestart = false;

  /// The stream ended because the Host went away: the next automatic try
  /// first waits for it ([AndroidRemoteView.awaitHostReturn]), once.
  bool _hostLossWait = false;

  void _startPlannedRestart(RemoteSession? from) {
    _endPlannedRestart();
    _plannedRestartFrom = from;
    _plannedRestartTimer = Timer(plannedRestartWait, () {
      if (!mounted || !_reconnecting || _preparingAgain || _allowPop) {
        _endPlannedRestart();
        return;
      }
      // Past the bound: revoke an attempt in flight, as preparation does.
      ++_generation;
      _loading = false;
      _backgroundKeeper.stop();
      unawaited(_session?.close());
      _giveUpReconnect(restart: true);
    });
  }

  /// A Host update ended the stream (host_update, as on Windows): while
  /// [_hostUpdateTimer] runs the view waits for the update
  /// and the Host, and reconnects by itself; past it, the manual text.
  Timer? _hostUpdateTimer;
  DateTime? _hostUpdateUntil;
  static const hostUpdateWait = Duration(minutes: 10);
  bool get _hostUpdate => _hostUpdateTimer?.isActive == true;

  void _endHostUpdate() {
    _hostUpdateTimer?.cancel();
    _hostUpdateTimer = null;
    _hostUpdateUntil = null;
  }

  void _beginHostUpdate() {
    _endHostUpdate();
    _hostUpdateUntil = DateTime.now().add(hostUpdateWait);
    _hostUpdateTimer = Timer(hostUpdateWait, () {
      // Not waiting any more, or a new stream already started: it goes on.
      if (!mounted ||
          !_reconnecting ||
          _allowPop ||
          _session?.closed == false) {
        _endHostUpdate();
        return;
      }
      // Past the bound: revoke an attempt in flight, as preparation does.
      ++_generation;
      _loading = false;
      _backgroundKeeper.stop();
      unawaited(_session?.close());
      _giveUpReconnect(hostUpdate: true);
    });
  }

  /// 기다리기 취소 of a Host update's wait: the same as the restart's.
  void _cancelHostUpdate() {
    if (!mounted || !_hostUpdate) return;
    _endHostUpdate();
    ++_generation;
    _loading = false;
    _backgroundKeeper.stop();
    _stopReconnecting();
    setState(() {
      _error = l10n.stoppedWaitingHostUpdateTap;
      _canRetry = true;
      _canTakeover = false;
    });
  }

  /// 기다리기 취소: the waiting reconnect stops; 다시 연결 stays.
  void _cancelPlannedRestart() {
    if (!mounted || !_plannedRestart) return;
    _endPlannedRestart();
    ++_generation;
    _loading = false;
    _backgroundKeeper.stop();
    _stopReconnecting();
    setState(() {
      _error = l10n.stoppedWaitingTapReconnectWhen;
      _canRetry = true;
      _canTakeover = false;
    });
  }

  /// [duration] is the least it stays; a longer message stays long enough
  /// to read (androidNoticeDuration).
  void _notice(
    String message, {
    Duration duration = const Duration(seconds: 3),
  }) {
    if (!mounted || _closing || _allowPop) return;
    _noticeTimer?.cancel();
    final revision = ++_noticeRevision;
    setState(() => _noticeMessage = message);
    _noticeTimer = Timer(androidNoticeDuration(message, duration), () {
      if (mounted && revision == _noticeRevision) _clearNotice();
    });
  }

  void _saveSettings(AndroidSessionSettings settings) {
    _settings = settings;
    widget.onSettings?.call(settings);
  }

  void _saveScreenChoice(AndroidDeviceClass kind, AndroidScreenChoice choice) {
    final scaleChanged = choice.scale != _scalePreference;
    final remembered = _settings.screen(kind);
    if (scaleChanged && choice.scale > 0) {
      // Relative to the automatic size, so it carries over when 선명하게
      // goes on or off.
      _apparentScale = choice.scale / 100 / (_sharpActive?.factor ?? 1);
    }
    _scalePreference = choice.scale;
    _saveSettings(
      _settings.withScreen(
        kind,
        choice.copyWith(scale: scaleChanged ? choice.scale : remembered.scale),
      ),
    );
  }

  ConnectionRecovery _recovery(RemoteSession? session) {
    final failure = _requestFailure;
    final code = _error != null
        ? (failure?.message == _error ? failure?.code : null)
        : session?.diagnosticCode;
    return connectionRecovery(
      code,
      canRetry: _canRetry && (session?.canRetry ?? true),
      takeover: _canTakeover,
    );
  }

  /// Opening a shortcut edits a local draft only. An explicit apply uses the
  /// same cleanup-before-load and fresh broker authorization as every retry.
  Future<void> _recover(ConnectionRecovery recovery) async {
    if (recovery == ConnectionRecovery.none ||
        _loading ||
        _closing ||
        _allowPop ||
        _recovery(_session) != recovery) {
      return;
    }
    final owner = _session, generation = _generation;
    bool current() =>
        mounted &&
        !_loading &&
        !_closing &&
        !_allowPop &&
        generation == _generation &&
        identical(owner, _session) &&
        _recovery(_session) == recovery;
    if (recovery == ConnectionRecovery.quality) {
      final options = await showDialog<StreamOptions>(
        context: context,
        builder: (dialog) => Dialog(
          backgroundColor: Colors.transparent,
          insetPadding: const EdgeInsets.all(16),
          child: ConstrainedBox(
            constraints: const BoxConstraints(maxWidth: 420),
            child: AndroidStreamSettingsPanel(
              key: const Key('android-recovery-quality-dialog'),
              options: _options,
              maxBitrate: widget.maxBitrate,
              onCancel: () => Navigator.of(dialog).pop(),
              onApply: (value) => Navigator.of(dialog).pop(value),
            ),
          ),
        ),
      );
      if (options == null || !current()) return;
      _options = options;
      widget.onOptions?.call(options);
    } else {
      final policy = _policy, physical = _physical;
      if (physical == null || physical.isEmpty) return;
      var draft = policy.choice;
      final choice = await showDialog<AndroidScreenChoice>(
        context: context,
        builder: (dialog) => StatefulBuilder(
          builder: (context, update) {
            final chrome = SessionChrome.of(context);
            return AppDialog(
              title: l10n.screenSettings,
              titleKey: const Key('android-recovery-display-dialog'),
              body: [
                AndroidScreenOptions(
                  deviceClass: policy.deviceClass,
                  choice: draft,
                  autoScale: null,
                  resolutions: androidResolutionOptions(
                    physical,
                    deviceClass: policy.deviceClass,
                    policy: policy,
                    devicePixelRatio: _dpr,
                    sharp: widget.displayScale,
                  ),
                  scaleAvailable: false,
                  parts: const {AndroidScreenPart.resolution},
                  onChoice: (value) => update(() => draft = value),
                ),
              ],
              actions: [
                TextButton(
                  key: const Key('android-recovery-display-cancel'),
                  onPressed: () => Navigator.of(dialog).pop(),
                  style: AndroidSessionButtons.quiet(chrome),
                  child: Text(l10n.cancel),
                ),
                FilledButton(
                  key: const Key('android-recovery-display-apply'),
                  onPressed: draft == policy.choice
                      ? null
                      : () => Navigator.of(dialog).pop(draft),
                  style: AndroidSessionButtons.filled(chrome),
                  child: Text(l10n.applyReconnect),
                ),
              ],
            );
          },
        ),
      );
      if (choice == null || !current()) return;
      setState(() {
        _camera.reset();
        _view = null;
        _saveScreenChoice(policy.deviceClass, choice);
      });
    }
    if (current()) await _connect();
  }

  void _retryAutomaticResolution() {
    if (!mounted ||
        _loading ||
        _closing ||
        _allowPop ||
        !_canRetry ||
        !(_session?.canRetry ?? true) ||
        _canTakeover ||
        _recovery(_session) != ConnectionRecovery.display) {
      return;
    }
    final policy = _policy;
    if (policy.choice.resolution.kind != AndroidResolutionKind.desktop &&
        policy.choice.resolution.kind != AndroidResolutionKind.workspace &&
        policy.choice.resolution.kind != AndroidResolutionKind.sharp) {
      return;
    }
    setState(() {
      _camera.reset();
      _view = null;
      _saveScreenChoice(
        policy.deviceClass,
        policy.choice.copyWith(resolution: AndroidResolution.auto),
      );
    });
    unawaited(_connect());
  }

  void _setTouchMode(AndroidTouchMode mode) {
    if (_touch.mode == mode) return;
    _release();
    setState(() => _touch.mode = mode);
    // No explanation pops up (no repeated hints); the
    // mode button shows the mode.
    _saveSettings(_settings.copyWith(touch: mode));
  }

  void _setSpeed(AndroidPointerSpeed speed) {
    if (_settings.speed == speed) return;
    _touch.speed = speed;
    setState(() => _saveSettings(_settings.copyWith(speed: speed)));
  }

  /// Zoom of this picture only (the remote is unchanged), never below the
  /// whole picture.
  void _zoomBy(double factor) {
    final view = _view;
    if (view == null) return;
    final centre = _visible.center;
    _camera.moved(byHand: true);
    setState(
      () => _view = view.zoomTo(
        view.scale * factor,
        view.anchorAt(centre),
        centre,
        visible: _visible,
      ),
    );
  }

  /// The picture covers the screen (edges may be cut off).
  void _fill() {
    final view = _view;
    if (view == null || view.fitted.isEmpty) return;
    final scale = math.max(
      view.area.width / view.fitted.width,
      view.area.height / view.fitted.height,
    );
    final rest = view.reset(visible: _visible);
    _camera.moved(byHand: true);
    setState(
      () => _view = rest.zoomTo(
        scale,
        const Offset(.5, .5),
        rest.area.center,
        visible: _visible,
      ),
    );
  }

  /// The move handle: another part of the picture, nothing is sent. A tap's
  /// waiting click lands first and held key-row modifiers are let go
  /// (ToTBDesk WINDOWS-INPUT-2.3.0).
  void _pan(Offset delta) {
    final view = _view;
    if (view == null) return;
    _touch.flushPendingClick();
    _camera.moved(byHand: true);
    setState(() {
      _latched.clear();
      _locked.clear();
      _view = view.panned(delta, visible: _visible);
    });
  }

  /// A new 화면 배율, 해상도 or 화면 방향 for this kind of screen, remembered
  /// for it: with display-scale-v1 a new scale changes only the Windows
  /// scale (no resize, no reconnect); a new size goes through the resize
  /// the rotation uses; older Hosts get the new pixel fraction as a resize.
  void _setScreen(AndroidScreenChoice choice) {
    final policy = _policy;
    final previous = policy.choice;
    if (previous == choice) return;
    setState(() {
      if (choice.resolution != previous.resolution &&
          (choice.resolution.kind == AndroidResolutionKind.desktop ||
              choice.resolution.kind == AndroidResolutionKind.workspace ||
              previous.resolution.kind == AndroidResolutionKind.desktop ||
              previous.resolution.kind == AndroidResolutionKind.workspace)) {
        _camera.reset();
        _view = null;
      }
      if (choice.resolution != previous.resolution) {
        // 선명하게 chosen (again): the desktop is asked once more.
        _sharpRefused = false;
        _sharpRefusedReason = null;
      }
      _saveScreenChoice(policy.deviceClass, choice);
    });
    final session = _session;
    if (choice.scale != previous.scale) _scaleChanged = true;
    final hostScale = session?.displayScaleSupported ?? widget.displayScale;
    final resize =
        choice.resolution != previous.resolution ||
        (choice.scale != previous.scale && !hostScale);
    if (session != null &&
        !session.closed &&
        resize &&
        !session.canResizeViewport) {
      _notice(l10n.hostCantChangeScreenSize);
    }
    _attemptedViewport = null;
    _replan();
  }

  /// 전체 보기 (with the keyboard open: the whole picture placed for typing).
  void _fit() {
    final view = _view;
    if (view == null) return;
    setState(() {
      _view = _camera.fit(view, _visible, _touch.cursor);
      // The whole picture again: 화면 이동 has nothing left to move.
      _moving = false;
    });
  }

  /// 화면 배율 (Windows scale, applied live with display-scale-v1 and shown
  /// as the Host set it), 해상도 (through the resize) and 화면 방향: the
  /// [parts] asked for (배율 has its own panel, as on Windows; the TV's
  /// local 화면 panel keeps all three).
  Widget _screenOptions(
    RemoteSession session, [
    Set<AndroidScreenPart> parts = const {...AndroidScreenPart.values},
  ]) {
    final policy = _policy;
    final physical = _physical ?? Size.zero;
    final automatic = AndroidViewportPlan.of(
      physical,
      _dpr,
      AndroidScreenPolicy(
        deviceClass: policy.deviceClass,
        choice: policy.choice.copyWith(scale: 0),
        density: policy.density,
        window: policy.window,
      ),
      hostScale: true,
      portraitLimits: session.streamSize ? session.connection.streamSize : null,
      fps: _options.fps,
      fullWorkspace: session.connection.streamSizeV2,
    ).displayScale;
    // By finger the 화면 panel's first lines already give the resolution and
    // the Windows scale (android-screen-observed-state): the 배율 caption does
    // not say them a second time. The TV's panel has no such lines.
    final scaleNote = AndroidTvScope.of(context)
        ? l10n.resolutionWindowsScaleApplySeparately(
            _remoteResolutionStatus(session),
            _windowsTextScaleStatus(session),
          )
        : l10n.resolutionWindowsScaleSeparate;
    final width = session.sourceWidth ?? session.width;
    final height = session.sourceHeight ?? session.height;
    String? resolutionNote = width > 0 && height > 0
        ? l10n.current(width, height)
        : null;
    final resolution = policy.choice.resolution;
    if (resolution.kind == AndroidResolutionKind.auto &&
        policy.deviceClass != AndroidDeviceClass.television) {
      final desired = policy.recommendedSize(physical, _dpr);
      final plan = _plan;
      final requested = plan != null && plan.size != desired
          ? l10n.requested2(plan.width, plan.height)
          : '';
      final currentWidth = session.sourceWidth,
          currentHeight = session.sourceHeight;
      final current =
          currentWidth != null &&
              currentHeight != null &&
              currentWidth > 0 &&
              currentHeight > 0
          ? l10n.current2(currentWidth, currentHeight)
          : l10n.checkingCurrent;
      resolutionNote =
          l10n.recommended(desired.width.toInt(), desired.height.toInt(), requested, current);
    } else if (resolution.kind == AndroidResolutionKind.desktop ||
        resolution.kind == AndroidResolutionKind.workspace) {
      final desired = resolution.kind == AndroidResolutionKind.workspace
          ? resolution.workspaceSize(physical)
          : resolution.desktopSize(physical);
      final desiredWidth = desired.width.round(),
          desiredHeight = desired.height.round();
      final plan = _plan;
      var requestNote = '';
      if (plan != null &&
          (plan.width < desiredWidth || plan.height < desiredHeight)) {
        final support = resolution.kind == AndroidResolutionKind.workspace
            ? l10n.supportedRange
            : l10n.deviceSupport;
        requestNote = ' · $support ${plan.width}×${plan.height}';
      }
      final currentWidth = session.sourceWidth,
          currentHeight = session.sourceHeight;
      final current =
          currentWidth != null &&
              currentHeight != null &&
              currentWidth > 0 &&
              currentHeight > 0
          ? l10n.current2(currentWidth, currentHeight)
          : l10n.checkingCurrent;
      resolutionNote = l10n.selected(desiredWidth, desiredHeight, requestNote, current);
    } else if (resolution.kind == AndroidResolutionKind.sharp) {
      // What 선명하게 gives here, or why it gives the automatic size.
      final plan = _sharpActive, planned = _sharpPlan;
      final currentWidth = session.sourceWidth,
          currentHeight = session.sourceHeight;
      final current =
          currentWidth != null &&
              currentHeight != null &&
              currentWidth > 0 &&
              currentHeight > 0
          ? l10n.current2(currentWidth, currentHeight)
          : l10n.checkingCurrent;
      resolutionNote = plan != null
          ? l10n.sharp2(plan.width, plan.height, plan.reason == null ? '' : ' (${androidSharpReasonLabel(plan.reason)})', current)
          : _sharpRefused
          ? l10n.couldntApplySharpSoShows2(androidSharpReasonLabel(_sharpRefusedReason), current)
          : _sharpPendingKey != null
          ? l10n.checkingSharp(current)
          : l10n.cantReceiveSharperPictureSo(planned == null ? '' : ' (${androidSharpReasonLabel(planned.reason)})', current);
    }
    return AndroidScreenOptions(
      deviceClass: policy.deviceClass,
      choice: policy.choice,
      autoScale: automatic,
      resolutions: androidResolutionOptions(
        physical,
        deviceClass: policy.deviceClass,
        policy: _planningPolicy,
        devicePixelRatio: _dpr,
        sharp: session.displayScaleSupported,
      ),
      scaleAvailable: session.displayScaleSupported,
      resolutionAvailable: session.canResizeViewport,
      scaleNote: scaleNote,
      resolutionNote: resolutionNote,
      onChoice: _setScreen,
      parts: parts,
    );
  }

  /// The session menu's 화면 panel. Windows' 화면 holds window modes a phone
  /// does not have; here only what a touch screen needs: the
  /// whole picture again and 화면 이동. Pinch still zooms the local picture.
  /// Explicit zoom buttons remain on TV, where there is no pinch gesture. The size
  /// uses a bounded aspect-preserving size and turns with the device.
  /// 해상도 is separate from Windows 배율; 포인터 속도 is under 입력 방식.
  /// The TV's local panel (its remote has no cluster) keeps 꽉 채우기,
  /// 해상도, 화면 방향, 화면 배율 and 포인터 속도 here.
  String _remoteResolutionStatus(RemoteSession session) {
    final width =
        _selectedOutput?.width ?? session.sourceWidth ?? session.width;
    final height =
        _selectedOutput?.height ?? session.sourceHeight ?? session.height;
    return width > 0 && height > 0 ? l10n.remoteResolution(width, height) : l10n.remoteResolutionUnknown;
  }

  String _windowsTextScaleStatus(RemoteSession session) {
    final result = session.displayScaleApplied;
    final selected = _selectedOutput?.id;
    final applied =
        selected != null && result != null && result.outputs.isNotEmpty
        ? result.outputs.where((output) => output.id == selected).firstOrNull
        : result;
    return androidWindowsTextScaleStatus(
      automatic: _policy.choice.scale == 0,
      supported: session.displayScaleSupported,
      wanted: session.displayScaleWanted,
      applied: applied,
    );
  }

  Widget _screenPanel(RemoteSession session, AndroidPictureView? view) {
    final chrome = SessionChrome.of(context);
    final television = AndroidTvScope.of(context);
    final scale = view?.scale ?? 1;
    final rtt = session.rttMs;
    void act(VoidCallback action) {
      action();
      _touch.flushPendingClick();
    }

    return AndroidToolbarPanel(
      key: const Key('android-screen-panel'),
      title: SessionTool.screen.label,
      // By finger the menu's quality chip shows the latency already.
      trailing: !television
          ? null
          : Text(
              rtt == null ? l10n.connected : '${rtt.toStringAsFixed(0)} ms',
              style: TextStyle(
                fontFamily: 'Pretendard',
                fontSize: 12,
                fontWeight: FontWeight.w600,
                color: chrome.latencyColor(rtt),
                fontFeatures: const [FontFeature.tabularFigures()],
              ),
            ),
      children: [
        if (!television)
          KeepAllText(
            l10n.windowsScale(_remoteResolutionStatus(session), _windowsTextScaleStatus(session)),
            key: const Key('android-screen-observed-state'),
            style: TextStyle(fontSize: 12, height: 1.5, color: chrome.muted),
          ),
        if (widget.dualOutputs)
          AndroidPanelRow(
            label: l10n.remoteScreen,
            items: [
              AndroidPanelItem(
                key: const Key('android-single-output'),
                label: l10n.n1Screen,
                selected: !_dualOutputs,
                onPressed: () => _changeOutputCount(false),
              ),
              AndroidPanelItem(
                key: const Key('android-dual-output'),
                label: l10n.n2Screens,
                selected: _dualOutputs,
                onPressed: () => _changeOutputCount(true),
              ),
            ],
          ),
        if (session.tracks.length > 1)
          AndroidPanelRow(
            label: l10n.screenShow,
            items: [
              for (final track in session.tracks)
                AndroidPanelItem(
                  key: ValueKey('android-output-${track.id}'),
                  label:
                      l10n.screen2(int.parse(track.id) + 1, session.outputPathLabel(track.id)),
                  selected: _selectedOutput?.id == track.id,
                  onPressed: () => _selectOutput(track.id),
                ),
            ],
          ),
        AndroidPanelRow(
          label: l10n.view,
          items: [
            AndroidPanelItem(
              key: const Key('android-screen-fit'),
              label: l10n.wholeScreen,
              icon: Icons.fit_screen_rounded,
              selected: view != null && !view.zoomed,
              onPressed: () => act(_fit),
            ),
            if (television)
              AndroidPanelItem(
                key: const Key('android-screen-fill'),
                label: l10n.fill,
                icon: Icons.crop_free_rounded,
                onPressed: () => act(_fill),
              ),
          ],
        ),
        if (television)
          AndroidPanelRow(
            items: [
              AndroidPanelItem(
                key: const Key('android-screen-zoom-out'),
                icon: Icons.remove_rounded,
                tooltip: l10n.zoomOut,
                onPressed: scale > 1.001
                    ? () => act(() => _zoomBy(1 / 1.25))
                    : null,
              ),
              AndroidPanelItem(
                key: const Key('android-screen-zoom'),
                label: l10n.zoom((scale * 100).round()),
                flex: 2,
                tooltip: l10n.zoomsDeviceOnlyRemoteResolution,
                onPressed: () => act(_fit),
              ),
              AndroidPanelItem(
                key: const Key('android-screen-zoom-in'),
                icon: Icons.add_rounded,
                tooltip: l10n.zoom2,
                onPressed: scale < AndroidPictureView.maxScale - .001
                    ? () => act(() => _zoomBy(1.25))
                    : null,
              ),
            ],
          ),
        AndroidPanelRow(
          items: [
            AndroidPanelItem(
              key: const Key('android-session-move'),
              label: l10n.pan,
              icon: Icons.open_with_rounded,
              selected: _moving,
              tooltip: l10n.useWhenZoomedWhenKeyboard,
              onPressed: (view?.canPan(_visible) ?? false) || _moving
                  ? () {
                      setState(() => _moving = !_moving);
                      // Its handle floats beside the cluster: the menu
                      // makes way.
                      if (_moving) _foldBar();
                    }
                  : null,
            ),
          ],
        ),
        // As the Windows 화면 panel: how the picture sits here first, then
        // 배율 (the same five steps), then the resolution.
        if (!television && session.displayScaleSupported)
          _screenOptions(session, const {AndroidScreenPart.scale}),
        if (!television)
          _screenOptions(session, const {AndroidScreenPart.resolution}),
        if (television) ...[_screenOptions(session), _speedRow()],
      ],
    );
  }

  /// 포인터 속도: how far the cursor goes for a finger's travel.
  Widget _speedRow() => AndroidPanelRow(
    label: l10n.pointerSpeed,
    items: [
      for (final value in AndroidPointerSpeed.values)
        AndroidPanelItem(
          key: Key('android-speed-${value.name}'),
          label: value.label,
          selected: _settings.speed == value,
          onPressed: () => _setSpeed(value),
        ),
    ],
  );

  /// 입력 방식, from a long press on the cluster's 입력 방식 button (a tap
  /// switches it): the two ways a finger works, and in pointer mode its
  /// 포인터 속도 (not in 화면; shown only where it applies).
  Widget _inputPanel() => AndroidToolbarPanel(
    key: const Key('android-input-panel'),
    title: l10n.inputMode,
    children: [
      AndroidPanelRow(
        items: [
          for (final mode in AndroidTouchMode.values)
            AndroidPanelItem(
              key: Key('android-input-${mode.name}'),
              label: mode.label,
              icon: mode == AndroidTouchMode.trackpad
                  ? Icons.mouse_rounded
                  : Icons.touch_app_rounded,
              selected: _touch.mode == mode,
              onPressed: () => _setTouchMode(mode),
            ),
        ],
      ),
      if (_touch.mode == AndroidTouchMode.trackpad) _speedRow(),
    ],
  );

  /// The long press on 입력 방식: its panel opens in the session menu's
  /// place beside the cluster.
  void _openInputPanel() {
    if (_keyboard) _toggleKeyboard();
    _release();
    setState(() {
      _panelFrom = null;
      _toolPanel = 'input';
    });
    _barKey.currentState?.show();
  }

  /// 사용안내, as Windows' 사용안내 panel: what works on this screen, here
  /// the touch gestures of the current 입력 방식.
  Widget _helpPanel() => AndroidTouchHelp(
    mode: _touch.mode,
    // 터치 on a Host that granted touch-input-v1 is real Windows touch.
    realTouch: _session?.touchInput ?? false,
    onClose: _foldBar,
    tips: _session == null
        ? const []
        : SessionGuideContent.forAndroid(_session!).tips,
  );

  /// The session menu's 화질·소리 panel, as on Windows, applied by
  /// reconnecting.
  Widget _settingsPanel(RemoteSession session) => AndroidStreamSettingsPanel(
    key: const Key('android-settings-panel'),
    options: _options,
    maxBitrate: widget.maxBitrate,
    // The no-sound-device line only when the Host said so.
    audioDeviceMissing: session.remoteAudioMissing,
    onCancel: _closePanel,
    onApply: _applyOptions,
  );

  /// 설정's 재연결: this connection is replaced by one asking for [options],
  /// which this device also remembers for the next one.
  void _applyOptions(StreamOptions options) {
    _options = options;
    widget.onOptions?.call(options);
    _foldBar();
    setState(() {
      _toolPanel = null;
      _panelFrom = null;
    });
    unawaited(_connect());
  }

  // --- 폴더·USB ---------------------------------------------------------------

  AndroidDriveFileSystem get _driveFiles =>
      widget.driveFiles ?? androidDriveFiles;

  Future<String?> _chooseFolder(AndroidDriveFileSystem files) async =>
      mounted ? showAndroidFolderChooser(context, files) : null;

  /// One step of this view for the connection record
  /// ([AndroidRemoteView.onConnectStep]); the app's lifecycle state rides
  /// along (hundreds: 1 resumed, 2 inactive, 3 hidden, 4 paused, 5 detached,
  /// 0 not reported).
  void _step(int stage, {bool ok = true, int reason = 0}) {
    final state = WidgetsBinding.instance.lifecycleState;
    final seen = switch (state) {
      null => 0,
      AppLifecycleState.resumed => 1,
      AppLifecycleState.inactive => 2,
      AppLifecycleState.hidden => 3,
      AppLifecycleState.paused => 4,
      AppLifecycleState.detached => 5,
    };
    widget.onConnectStep?.call(stage, ok, seen * 100 + reason);
  }

  /// Why this connection has no shared folders although the administrator
  /// allows them (as on Windows); null when it has them or they do not apply.
  String? _driveReason(RemoteSession session) =>
      session.drive == null && widget.driveUnavailable != null
      ? androidDriveMessage(widget.driveUnavailable)
      : null;

  /// Sharing that ended while the stream goes on is said once, and so is a
  /// folder whose share ended because it is gone or its access was taken
  /// back (the session passes its drive channel's changes on).
  DriveSession? _watchedDrive;
  bool _driveEndNoticed = false;
  int _driveGoneNoticed = 0;
  void _watchDrive(RemoteSession? session) {
    final drive = session?.drive;
    if (!identical(drive, _watchedDrive)) {
      _watchedDrive = drive;
      _driveEndNoticed = false;
      _driveGoneNoticed = drive?.goneCount ?? 0;
    }
    // Only while the stream goes on; an ended session says why itself.
    if (!mounted || drive == null || session == null || session.closed) return;
    if (drive.goneCount > _driveGoneNoticed && drive.lastGone != null) {
      _driveGoneNoticed = drive.goneCount;
      _notice(
        androidDriveWording(_driveFiles).gone(drive.lastGone!),
        duration: const Duration(seconds: 6),
      );
    }
    if (_driveEndNoticed) return;
    final message = driveEndedMessage(drive);
    if (message == null) return;
    _driveEndNoticed = true;
    _notice(message, duration: const Duration(seconds: 6));
  }

  /// 폴더·USB: the Windows 폴더·USB panel itself in the menu card, with this
  /// device's words. Folders are picked with Android's own folder picker.
  Widget _sharePanel(RemoteSession session) => AndroidToolbarPanel(
    key: const Key('android-share-panel'),
    title: SessionTool.drives.label,
    children: [
      _sharedPanel(
        SessionSharePanel(
          embedded: true,
          drive: session.drive,
          driveUnavailable: _driveReason(session),
          driveWording: androidDriveWording(_driveFiles),
          dialogContext: context,
        ),
      ),
    ],
  );

  /// The panel the session menu shows in its place, and how wide it may be.
  (Widget, double)? _openPanel(
    RemoteSession session,
    AndroidPictureView? view,
  ) => switch (_toolPanel) {
    // The TV's 화면 keeps its sizes and choices: the wider card.
    'screen' => (
      _screenPanel(session, view),
      AndroidTvScope.of(context)
          ? AndroidSessionBar.defaultPanelWidth
          : AndroidSessionBar.pageWidth,
    ),
    'connection' => (
      _connectionPanel(session),
      AndroidSessionBar.defaultPanelWidth,
    ),
    'share' => (_sharePanel(session), AndroidSessionBar.defaultPanelWidth),
    'more' => (_morePanel(session), AndroidSessionBar.defaultPanelWidth),
    // On its side the settings take two columns (they fit the height).
    'settings' => (
      _settingsPanel(session),
      androidPanelBeside(MediaQuery.sizeOf(context))
          ? 2 * AndroidSessionBar.defaultPanelWidth
          : AndroidSessionBar.defaultPanelWidth,
    ),
    'input' => (_inputPanel(), AndroidSessionBar.pageWidth),
    'help' => (_helpPanel(), AndroidSessionBar.defaultPanelWidth),
    _ => null,
  };

  /// 화면, 배율, 설정 or 사용안내 in the session menu's place; the same one
  /// again closes it.
  void _togglePanel(String panel) {
    // The panel needs the room: the keyboard closes first (ToTBDesk
    // KEYBOARD-2.3.2).
    if (_keyboard) _toggleKeyboard();
    _release();
    setState(() {
      _panelFrom = null;
      _toolPanel = _toolPanel == panel ? null : panel;
    });
  }

  /// A panel opened from the 설정 panel: Back and 취소 return there, not to
  /// the menu.
  String? _panelFrom;

  /// 화질·소리, 입력 방식 or 사용안내 from the 설정 panel, in its place.
  void _openFromSettings(String panel) {
    _release();
    setState(() {
      _panelFrom = 'more';
      _toolPanel = panel;
    });
  }

  /// One level up: a panel opened from 설정 goes back to 설정, any other
  /// back to the session menu.
  void _closePanel() {
    if (!mounted || _toolPanel == null) return;
    final from = _toolPanel == _panelFrom ? null : _panelFrom;
    setState(() {
      _toolPanel = from;
      _panelFrom = null;
    });
  }

  /// Whether the round trip has stayed poor (the Windows bar's rule): the
  /// 연결 panel then says what helps.
  late final _poorLatency = SessionPoorLatency(() {
    if (mounted) setState(() {});
  });

  /// The panels of the Windows bar in the menu card: the same widgets, dark
  /// like the card, with rows a finger can hit.
  Widget _sharedPanel(Widget child) => Theme(
    data: buildAppTheme(brightness: Brightness.dark),
    child: SessionTouchScope(
      child: Padding(
        padding: const EdgeInsets.fromLTRB(4, 4, 4, 2),
        child: child,
      ),
    ),
  );

  /// 연결 품질: the Windows 연결 panel itself (the round trip as one number
  /// with its word, what was measured, 연결 통계 보기 and, while the latency
  /// stays poor, 절약으로 다시 연결), plus 화면 크기 다시 적용 after a failed
  /// resize.
  Widget _connectionPanel(RemoteSession session) {
    final saver =
        StreamPreset.of(_options, widget.maxBitrate) != StreamPreset.saver;
    return AndroidToolbarPanel(
      key: const Key('android-connection-panel'),
      title: SessionTool.quality.label,
      children: [
        _sharedPanel(
          SessionConnectionPanel(
            embedded: true,
            title: widget.name,
            mode: l10n.streaming,
            listenable: session,
            latency: () => _session?.rttMs,
            connected: () => _session?.closed == false,
            values: () => sessionQualityValues(_session, device: l10n.device),
            advice: () => _poorLatency.sustained,
            onSaver: saver ? _reconnectSaver : null,
            onStats: () => unawaited(_command('stats')),
            actions: [
              if (session.canResizeViewport && session.resizeError != null)
                SessionPanelRow(
                  key: const Key('android-session-resize'),
                  icon: Icons.aspect_ratio_rounded,
                  title: l10n.reapplyScreenSize,
                  onTap: () => unawaited(_command('resize')),
                ),
            ],
          ),
        ),
      ],
    );
  }

  /// 절약으로 다시 연결, as on Windows: 절약's frame rate and bitrate with
  /// everything else as it is.
  void _reconnectSaver() {
    final saver = StreamPreset.saver.options;
    _applyOptions(
      StreamOptions(
        maxWidth: _options.maxWidth,
        maxHeight: _options.maxHeight,
        fps: saver.fps,
        bitrate: saver.bitrate,
        bitrateMode: saver.bitrateMode,
        audio: _options.audio,
        monitor: _options.monitor,
      ),
    );
  }

  /// 설정: the Windows 설정 panel itself, with what a phone has: 화질·소리,
  /// 마우스 잠금 (where a mouse can be locked), 입력 방식 (Android's own, where
  /// a Windows user would look for how the pointer works) and 사용안내.
  /// 툴바 열어 두기 does not apply: the three buttons are always there.
  Widget _morePanel(RemoteSession session) {
    final preset = StreamPreset.of(_options, widget.maxBitrate);
    final trackpad = _touch.mode == AndroidTouchMode.trackpad;
    return AndroidToolbarPanel(
      key: const Key('android-more-panel'),
      title: SessionTool.more.label,
      children: [
        _sharedPanel(
          SessionSettingsPanel(
            embedded: true,
            showPin: false,
            quality: true,
            qualitySummary:
                '${preset?.label ?? l10n.custom} · ${_options.fps} fps',
            qualityKey: const Key('android-session-settings'),
            mouseLock: _gameMouseOffered ? _gameMouse : null,
            mouseLockKey: const Key('android-session-game-mouse'),
            mouseLockHint: _gameMouse ? l10n.pressBackRelease : l10n.locksMouseInsideDesktop,
            mouseLockReason: _mousePresent ? null : l10n.availableWhenMouseConnected,
            guide: true,
            guideKey: const Key('android-session-help'),
            guideSubtitle: l10n.touchControlsPrinterCopy,
            extra: [
              if (!AndroidTvScope.of(context))
                SessionPanelRow(
                  key: const Key('android-session-input'),
                  // Its own icon in either mode: 마우스 잠금 above it already
                  // wears the mouse.
                  icon: Icons.touch_app_rounded,
                  title: l10n.inputMode,
                  subtitle: trackpad
                      ? l10n.speed(_touch.mode.label, _settings.speed.label)
                      : _touch.mode.label,
                  opens: true,
                  onTap: () => _openFromSettings('input'),
                ),
            ],
            onSelected: (choice) {
              switch (choice) {
                case SessionSettingsChoice.quality:
                  _openFromSettings('settings');
                case SessionSettingsChoice.mouseLock:
                  _toggleGameMouse();
                  _foldBar();
                case SessionSettingsChoice.guide:
                  _openFromSettings('help');
              }
            },
          ),
        ),
      ],
    );
  }

  /// The TV's 단축키 panel (its remote has no key row): Windows keys and
  /// editing chords, sent as key presses.
  List<SessionMenuItem> _tvShortcuts(RemoteSession session) {
    SessionMenuItem entry(
      String label,
      String action,
      IconData icon, {
      bool enabled = true,
      String? hint,
    }) => SessionMenuItem(
      label,
      icon: icon,
      value: hint,
      onSelected: enabled ? () => unawaited(_command(action)) : null,
    );
    return [
      SessionMenuItem(
        session.resizing ? l10n.shortcutsAvailableOnceScreenSize : 'Windows',
        heading: true,
      ),
      entry(
        l10n.windowsKey,
        'windows',
        Icons.window_rounded,
        enabled: !session.resizing,
        hint: 'Win',
      ),
      entry(
        l10n.switchApps,
        'altTab',
        Icons.swap_horiz_rounded,
        enabled: !session.resizing,
        hint: 'Alt+Tab',
      ),
      entry(
        l10n.taskManager,
        'taskManager',
        Icons.monitor_heart_outlined,
        enabled: !session.resizing,
        hint: 'Ctrl+Shift+Esc',
      ),
      SessionMenuItem(l10n.edit, heading: true),
      entry(l10n.copy, 'copy', Icons.copy_rounded, hint: 'Ctrl+C'),
      entry(l10n.paste, 'paste', Icons.content_paste_rounded, hint: 'Ctrl+V'),
      entry(l10n.selectAll, 'selectAll', Icons.select_all_rounded, hint: 'Ctrl+A'),
      entry(l10n.undo, 'undo', Icons.undo_rounded, hint: 'Ctrl+Z'),
      const SessionMenuItem('', heading: true),
      SessionMenuItem(
        'Ctrl+Alt+Del',
        value: l10n.cantSend,
        icon: Icons.lock_outline_rounded,
        onSelected: () => _notice(
          l10n.ctrlAltDelCantSent,
        ),
      ),
    ];
  }

  /// The session menu's entries: the Windows bar's, in [SessionTool] order,
  /// less what a phone cannot do: 화면 ·
  /// 배율 (where the Host sets Windows' scale) · 설정 · 마우스 잠금 (where
  /// granted) · 사용안내. Nothing Android-only: no 클립보드 or 키 조합 (the
  /// PC has neither; Win, Ctrl, Alt, Esc and Ctrl+Alt+Del are on the key
  /// row). 키보드 and 입력 방식 are the cluster's one-tap buttons.
  List<SessionToolbarMenu> _barActions(
    RemoteSession session, {
    required bool canPan,
    required bool pictured,
  }) {
    // A panel in the menu's place, from its row.
    SessionToolbarMenu panel(
      String id,
      SessionTool tool,
      String tooltip, {
      String? disabledReason,
    }) => SessionToolbarMenu.command(
      key: Key('android-session-$id'),
      keepOpenInOverflow: true,
      label: tool.androidLabel,
      tooltip: tooltip,
      icon: tool.icon,
      onInteract: _release,
      disabledReason: disabledReason ?? l10n.youCanChangeOnceConnected,
      onPressed: pictured ? () => _togglePanel(id) : null,
      onPressedAt: pictured
          ? (_) async {
              _togglePanel(id);
              return false;
            }
          : null,
    );
    // The Windows bar's commands, in its order (연결 품질 is the menu's first
    // row and 연결 끊기 its last): 화면, 폴더·USB and 설정. What Windows puts
    // in those panels is in the same panels here (배율 in 화면; 화질·소리,
    // 마우스 잠금 and 사용안내 in 설정).
    // 폴더·USB as on Windows: only in a connection the broker granted it, or
    // greyed with the reason where the desktop offers it and this connection
    // cannot use it.
    final drive = session.drive;
    final driveReason = _driveReason(session);
    final mapped = SessionSharePanel.mappedFolders(drive);
    return [
      panel(
        'screen',
        SessionTool.screen,
        AndroidTvScope.of(context)
            ? l10n.screenWholeScreenZoomPan
            : l10n.screenViewScaleResolution,
      ),
      if (drive != null || driveReason != null)
        SessionToolbarMenu.command(
          key: const Key('android-session-share'),
          keepOpenInOverflow: true,
          label: SessionTool.drives.label,
          tooltip: mapped > 0 ? l10n.text2(mapped) : l10n.useDevicesFoldersDesktop,
          icon: drive != null
              ? SessionTool.drives.icon
              : Icons.folder_off_rounded,
          active: mapped > 0,
          onInteract: _release,
          disabledReason: driveReason ?? l10n.availableOnceConnected,
          onPressed: drive != null && pictured
              ? () => _togglePanel('share')
              : null,
          onPressedAt: drive != null && pictured
              ? (_) async {
                  _togglePanel('share');
                  return false;
                }
              : null,
        ),
      panel('more', SessionTool.more, l10n.settingsPictureQualityMouse),
    ];
  }

  Future<void> _command(String action) async {
    final session = _session;
    if (action == 'close') {
      await _close(source: 4);
      return;
    }
    if (session == null || session.closed) return;
    if (action.startsWith('touch:')) {
      _setTouchMode(AndroidTouchMode.parse(action.substring(6)));
      return;
    }
    switch (action) {
      case 'keyboard':
        _toggleKeyboard();
      case 'fit':
        _fit();
      case 'resize':
        _attemptedViewport = null;
        _resizeTimer?.cancel();
        _resizeTimer = null;
        _scheduleResize();
      case 'stats':
        await _stats();
      case 'windows' || 'altTab' || 'taskManager':
        if (_closing || _allowPop) return;
        if (session.resizing) {
          _notice(l10n.screenSizeAdjustingChooseShortcut);
          return;
        }
        final generation = _generation;
        final confirmed = await session.shortcut(action);
        if (!confirmed &&
            mounted &&
            identical(session, _session) &&
            generation == _generation &&
            !_allowPop) {
          _notice(l10n.couldntConfirmShortcutWasSent);
        }
      case 'copy' || 'paste' || 'selectAll' || 'undo':
        await _actions?.chord(action);
    }
  }

  List<SessionMetricSection> _statsSections() {
    final session = _session;
    if (session == null) return const [];
    final diagnostics = _media?.diagnostics;
    final latency = session.inputLatency;
    String ms(double? value) =>
        value == null ? l10n.notMeasured : '${value.toStringAsFixed(0)} ms';
    String fps(double? value) =>
        value == null ? l10n.notMeasured : '${value.toStringAsFixed(1)} fps';
    return [
      SessionMetricSection(l10n.streaming, [
        SessionMetric(l10n.technology, '${StreamOptions.engineName} · H.264'),
        SessionMetric(l10n.decoder, '${diagnostics?['decoder'] ?? l10n.preparing}'),
        SessionMetric(l10n.hardwareDecoding, switch (diagnostics?['hardwareDecoder']) {
          true => l10n.onLabel2,
          false => l10n.off2,
          _ => l10n.unknown,
        }),
        SessionMetric(l10n.appCpu, appCpuLabel(session.appCpuPct10, null)),
        SessionMetric(l10n.receivedSize, '${session.width} × ${session.height}'),
        SessionMetric(
          l10n.remoteOriginal,
          '${session.sourceWidth ?? l10n.unknown} × ${session.sourceHeight ?? l10n.unknown}',
        ),
        SessionMetric(
          l10n.screenSettings,
          '${_policy.deviceClass.label} · ${_policy.choice.scale == 0 ? l10n.auto : '${_policy.choice.scale}%'}'
              ' · ${switch (_policy.choice.resolution.kind) {
                AndroidResolutionKind.auto => l10n.auto,
                AndroidResolutionKind.device => l10n.devicePixels,
                AndroidResolutionKind.half => l10n.half,
                AndroidResolutionKind.sharp => l10n.sharp,
                AndroidResolutionKind.preset => l10n.fixed(_policy.choice.resolution.shortSide),
                AndroidResolutionKind.longEdge => l10n.selected2(_plan?.width ?? 0, _plan?.height ?? 0),
                AndroidResolutionKind.desktop => l10n.selected3(_policy.choice.resolution.desktopLabel),
                AndroidResolutionKind.workspace => l10n.selected4(_policy.choice.resolution.workspaceLabel),
              }}',
        ),
        SessionMetric(l10n.windowsScale2, _windowsTextScaleStatus(session)),
        SessionMetric(
          l10n.pixelScale,
          _viewPixelScale == null
              ? l10n.zoomFitInterpolated
              : l10n.devicePixels1RemotePixel('$_viewPixelScale'),
        ),
        SessionMetric(l10n.targetFrameRate, '${session.connection.options.fps} fps'),
        SessionMetric(
          l10n.mouseCursor,
          session.cursorSeparated ? l10n.shownDevice : l10n.video,
        ),
        SessionMetric(l10n.audio, session.audioStatus),
      ]),
      SessionMetricSection(
        l10n.framesNetwork,
        [
          SessionMetric(l10n.sourceCapture, fps(session.capturedFps)),
          SessionMetric(l10n.decodingDevice, fps(session.decodedFps)),
          SessionMetric(l10n.sentScreen, fps(session.textureFps)),
          SessionMetric(
            l10n.receiveRate,
            session.receivedMbps == null
                ? l10n.notMeasured
                : '${session.receivedMbps!.toStringAsFixed(1)} Mbps',
          ),
          SessionMetric('Host RTT', ms(session.rttMs)),
          if (latency.supported) ...[
            SessionMetric(
              l10n.inputAppliedHostEstP50,
              '${ms(latency.toHost?.p50Ms)} / ${ms(latency.toHost?.p95Ms)}',
            ),
            SessionMetric(
              l10n.inputNewFrameDecodedP50,
              '${ms(latency.toFrame?.p50Ms)} / ${ms(latency.toFrame?.p95Ms)}',
            ),
          ] else
            SessionMetric(l10n.inputLatency, l10n.hostDoesntSupportMeasuring),
          SessionMetric(
            l10n.audioDrops,
            // The unit belongs to a measured count only (it read "미측정 프레임").
            switch (diagnostics?['audioDroppedFrames']) {
              null => l10n.notMeasured,
              final frames => l10n.text3(frames),
            },
          ),
        ],
        note: l10n.inputNewFrameDecodedRuns,
      ),
      // What the native engine measured on this device (android_engine_stats.dart).
      if (diagnostics != null)
        SessionMetricSection(l10n.processingDevice, [
          for (final (label, value) in androidEngineMetrics(diagnostics))
            SessionMetric(label, value),
        ], note: androidEngineMetricsNote),
    ];
  }

  Future<void> _stats() async {
    final session = _session;
    if (session == null) return;
    _release();
    _menuOpen = true;
    await showDialog<void>(
      context: context,
      builder: (_) => SessionStatsDialog(
        title: l10n.connectionStats,
        machine: l10n.streaming2(widget.name),
        sections: _statsSections,
        observationEnabled: () => session.observabilityEnabled,
        onObservationChanged: session.setObservabilityEnabled,
        reportJson: session.observabilityJson,
        note: l10n.detailedDiagnosticsOffByDefault,
      ),
    );
    _menuOpen = false;
    if (mounted && identical(session, _session) && !session.closed) {
      _scheduleResize();
    }
  }

  /// [source]: who asked (the record of a view that closed, [_step] 30):
  /// 1 Back, 2 the session bar's 연결 끊기, 3 the menu's, 4 a command.
  Future<void> _close({int source = 0}) async {
    if (_closing || _allowPop || !mounted) return;
    ++_takeoverRevision;
    _closing = true;
    ++_viewportRevision;
    _resizeTimer?.cancel();
    _resizeTimer = null;
    _release();
    final session = _session;
    final active = session != null && !session.closed;
    _step(30, ok: active, reason: source * 10 + (_loading ? 1 : 2));
    var approved = !active;
    if (active) {
      _menuOpen = true;
      approved = await confirmDisconnect(
        context,
        machine: widget.name,
        printsWaiting: session.printer?.pending.length ?? 0,
      );
      _menuOpen = false;
    }
    _closing = false;
    if (!mounted) return;
    if (!approved) {
      if (_viewportSettling || _resumeConnect) {
        _settleViewport();
      } else {
        _scheduleResize();
      }
      return;
    }
    // Invalidate pending output preparation before closing; a late successful
    // count change must never attach a new controller to an abandoned route.
    ++_generation;
    _backgroundKeeper.stop();
    _stopReconnecting();
    _keyboard = false;
    _fieldFocus.unfocus();
    // The last statistics window (with its input counts) before the route
    // closes the session.
    _session?.flushStats();
    setState(() => _allowPop = true);
    await WidgetsBinding.instance.endOfFrame;
    if (mounted) Navigator.pop(context);
  }

  Future<void> _stopFromNotification() async {
    if (!mounted || _allowPop) return;
    _step(30, ok: _session?.closed == false, reason: 90);
    final route = ModalRoute.of(context);
    final navigator = Navigator.of(context);
    ++_generation;
    ++_viewportRevision;
    _closing = true;
    _resizeTimer?.cancel();
    _resumeViewportTimer?.cancel();
    _stopReconnecting();
    _release();
    _retireTyping();
    setState(() {
      _allowPop = true;
      _loading = false;
      _toolPanel = null;
    });
    try {
      await _session?.close();
    } finally {
      _backgroundKeeper.stop();
    }
    // No endOfFrame wait: a notification action must work while backgrounded.
    if (mounted && route?.isActive == true && navigator.canPop()) {
      navigator.popUntil((candidate) => identical(candidate, route));
      if (navigator.canPop()) navigator.pop();
    }
  }

  /// Closes the open session menu; false when it was closed already.
  bool _foldBar() {
    final bar = _barKey.currentState;
    if (bar == null || !bar.isOpen) return false;
    bar.fold();
    return !bar.isOpen;
  }

  void _back() {
    // 마우스 잠금 holds the mouse: Back takes it back, nothing else.
    if (_relativeMouse.capturing) {
      _relativeMouse.release();
      return;
    }
    if (_tvPanel != null) {
      _closeTvPanel();
      return;
    }
    if (AndroidTvScope.of(context) &&
        _session?.frameReady == true &&
        !_keyboard &&
        _toolPanel == null &&
        _barKey.currentState?.isOpen != true) {
      unawaited(_tvMenu());
      return;
    }
    if (_toolPanel != null) {
      // A panel goes back to where it was opened from: 설정 or the menu.
      _closePanel();
    } else if (_keyboard) {
      _toggleKeyboard();
    } else {
      // Back ends 화면 이동 and goes one level up in the session menu (a
      // page to the menu, the menu closed); with nothing open it leaves.
      final folded = _barKey.currentState?.back() ?? false;
      // Only a 화면 이동 that is on screen takes this Back. One left on after
      // the picture went back to the whole view by itself (a pinch) is
      // cleared without using Back up: that press did nothing visible.
      final moved = _moving && (_view?.canPan(_visible) ?? false);
      if (_moving) setState(() => _moving = false);
      if (!moved && !folded) unawaited(_close(source: 1));
    }
  }

  Future<void> _tvMenu() async {
    if (!mounted || _tvMenuOpen || _closing || _allowPop) return;
    final generation = _generation, session = _session;
    _release();
    setState(() {
      _tvMenuOpen = true;
      _menuOpen = true;
    });
    final action = await showAndroidTvRemoteMenu(
      context,
      keyboardOpen: _keyboard,
      mode: _tvMode,
    );
    if (!mounted) return;
    setState(() {
      _tvMenuOpen = false;
      _menuOpen = false;
    });
    if (generation != _generation || !identical(session, _session)) return;
    if (action == 'keyboard') _toggleKeyboard();
    if (action == 'disconnect') unawaited(_close(source: 3));
    if (action == 'screen' || action == 'shortcuts') {
      _release();
      setState(() {
        _tvPanel = action;
        _tvPanelOwner = session;
        _menuOpen = true;
      });
    }
    if (action?.startsWith('mode:') == true) {
      _tvInput.currentState?.cancel();
      setState(
        () =>
            _tvMode = AndroidTvPointerMode.values.byName(action!.substring(5)),
      );
    }
  }

  void _closeTvPanel() {
    if (!mounted || _tvPanel == null) return;
    _tvInput.currentState?.cancel();
    setState(() {
      _tvPanel = null;
      _tvPanelOwner = null;
      _menuOpen = false;
    });
  }

  @override
  void dispose() {
    _poorLatency.dispose();
    _tvInput.currentState?.cancel();
    ++_generation;
    _clearNotice(rebuild: false);
    _backgroundKeeper.stop();
    _actions?.invalidate();
    _resizeTimer?.cancel();
    _resumeViewportTimer?.cancel();
    _serverRestartRetry?.cancel();
    _stopReconnecting();
    _endPlannedRestart();
    _endHostUpdate();
    unawaited(_networkWatch?.cancel());
    unawaited(_engineInput?.cancel());
    unawaited(_penSignals?.cancel());
    _pen.reset();
    _pen.signal(const {'type': 'unavailable'});
    WidgetsBinding.instance.removeObserver(this);
    if (_driveFiles.chooser == _chooseFolder) _driveFiles.chooser = null;
    _session?.removeListener(_changed);
    _session?.caretUpdates.removeListener(_caretChanged);
    _gameMouseNative.onMouse = null;
    _relativeMouse.dispose();
    _gameMouseUsb
      ?..removeListener(_gameMouseUsbChanged)
      ..dispose();
    _printOffers.dispose();
    _retireMedia(_media, _session);
    _touch.dispose();
    _moves.dispose();
    _cues.dispose();
    _hold.dispose();
    _field.removeListener(_typed);
    _field.dispose();
    _fieldFocus.dispose();
    unawaited(SystemChrome.setEnabledSystemUIMode(SystemUiMode.edgeToEdge));
    // The list follows the device's rotation again.
    if (_orientation != null && _orientation != AndroidOrientation.auto) {
      unawaited(
        SystemChrome.setPreferredOrientations(const [])
            .catchError((Object _) {}),
      );
    }
    super.dispose();
  }

  // --- build ----------------------------------------------------------------

  Widget _picture(
    RemoteSession session,
    AndroidRemoteMedia? media,
    AndroidShownPicture shown,
    AndroidPictureView view,
  ) {
    final sourceWidth =
        _selectedOutput?.width ?? session.sourceWidth ?? session.width;
    final sourceHeight =
        _selectedOutput?.height ?? session.sourceHeight ?? session.height;
    final cursor = session.cursorSeparated ? media?.cursorState : null;
    cursor?.source = Size(sourceWidth.toDouble(), sourceHeight.toDouble());
    // Whole device pixels per remote pixel are drawn without smoothing
    // (sharp text); zoom and fractional fits are filtered.
    final whole = view.wholePixelScale(shown.size, _dpr);
    _viewPixelScale = whole;
    final picture = Stack(
      fit: StackFit.expand,
      children: [
        widget.pictureBuilder?.call(context) ??
            Texture(
              key: ValueKey(shown.texture),
              textureId: shown.texture,
              filterQuality: whole != null
                  ? FilterQuality.none
                  : FilterQuality.low,
            ),
        // 마우스 잠금 with the cursor in the video: none drawn here.
        if (cursor != null && !_relativeMouse.composedEngaged)
          AndroidCursorOverlay(
            cursor: cursor,
            sourceWidth: sourceWidth,
            // Trackpad: a cursor at least about 22 dp tall to steer.
            minimumScale: _touch.mode == AndroidTouchMode.trackpad ? .7 : 0,
          ),
        // Unknown/missing shapes wait for Host evidence; explicit hidden
        // state stays hidden. Never invent a trackpad arrow over the video.
      ],
    );
    // With a separated cursor the remote shape is drawn here; hide the
    // device's own mouse pointer over the picture so there are never two.
    return cursor == null
        ? picture
        : MouseRegion(
            cursor: session.captured
                ? SystemMouseCursors.none
                : MouseCursor.defer,
            child: picture,
          );
  }

  AndroidPictureView _layoutView(
    AndroidShownPicture shown,
    Rect area,
    Rect visible,
  ) {
    // The shown texture's own size: an older picture kept during a resize
    // stays at its shape (letterboxed), never stretched to the new one.
    final source = shown.size;
    // A smaller picture (the Host encoding below the source, stream-size-v1
    // or a pixel budget) is fitted to the area: AndroidPictureView.fit draws
    // whole device pixels only when they fill it.
    final pixelScale = _plan?.pixelScale ?? 1;
    var view = _view;
    var fresh = false;
    if (view == null ||
        view.area != area ||
        _viewSource != source ||
        _viewPlanScale != pixelScale) {
      view = AndroidPictureView.fit(
        area,
        source,
        devicePixelRatio: _dpr,
        pixelScale: pixelScale,
      );
      _viewSource = source;
      _viewPlanScale = pixelScale;
      fresh = true;
    }
    // The keyboard and its key row: the picture moves up with them (zoom
    // kept, the typing point in view) and returns when they close.
    if (fresh || visible != _visible || _camera.open) {
      view = _camera.place(view, visible, _touch.cursor, fresh: fresh);
    }
    _visible = visible;
    return _view = view;
  }

  Widget _panel(RemoteSession? session, String? failed) {
    final chrome = SessionChrome.of(context);
    final leave = TextButton(
      key: const Key('android-leave'),
      onPressed: _close,
      style: AndroidSessionButtons.quiet(chrome),
      child: Text(l10n.backList),
    );
    if (failed != null && !_loading) {
      final retry = _canRetry && (session?.canRetry ?? true);
      final recovery = _recovery(session);
      // The one question before a takeover: whose connections the button
      // ends, and since when. The button accepts the offer listed here.
      final offer = _canTakeover ? _takeoverOffer : null;
      return AndroidSessionPanel(
        title: widget.name,
        message: failed,
        detail: offer == null
            ? null
            : streamTakeoverSessionLines(offer).join('\n'),
        icon: _resumeConnect
            ? Icons.pause_circle_outline_rounded
            : Icons.link_off_rounded,
        actions: [
          leave,
          if (retry &&
              !_canTakeover &&
              recovery == ConnectionRecovery.display &&
              (_policy.choice.resolution.kind ==
                      AndroidResolutionKind.desktop ||
                  _policy.choice.resolution.kind ==
                      AndroidResolutionKind.workspace))
            TextButton.icon(
              key: const Key('android-resolution-auto-retry'),
              onPressed: _retryAutomaticResolution,
              style: AndroidSessionButtons.quiet(chrome),
              icon: const Icon(Icons.fit_screen_rounded, size: 19),
              label: Text(l10n.reconnectAutoResolution),
            ),
          if (recovery != ConnectionRecovery.none)
            TextButton.icon(
              key: Key('android-recovery-${recovery.name}'),
              onPressed: () => _recover(recovery),
              style: AndroidSessionButtons.quiet(chrome),
              icon: Icon(
                recovery == ConnectionRecovery.quality
                    ? Icons.tune_rounded
                    : Icons.desktop_windows_rounded,
                size: 19,
              ),
              label: Text(
                recovery == ConnectionRecovery.quality ? l10n.pictureQualitySettings : l10n.screenSettings,
              ),
            ),
          if (retry)
            FilledButton.icon(
              key: Key(_canTakeover ? 'android-takeover' : 'android-retry'),
              onPressed: _canTakeover ? () => _takeover(offer) : _connect,
              style: AndroidSessionButtons.filled(chrome),
              icon: Icon(
                _canTakeover ? Icons.swap_horiz_rounded : Icons.refresh_rounded,
                size: 19,
              ),
              label: Text(_canTakeover ? l10n.disconnectConnectHere : l10n.reconnect),
            ),
        ],
      );
    }
    final restarting = _reconnecting && _plannedRestart;
    final updating = _reconnecting && _hostUpdate;
    return AndroidSessionPanel(
      busy: true,
      title: widget.name,
      progress: _waitStatus(session),
      message: _preparingAgain
          ? l10n.hostPreparingVirtualDisplayPlease
          : updating
          ? l10n.hostUpdatingReconnectsAutomaticallyWhen
          : restarting
          ? l10n.waitingDesktopConnectsAutomaticallyWhen
          : _serverRestart.waiting && (session == null || session.closed)
          ? serverRestartingMessage
          : _reconnecting
          ? l10n.connectionDroppedReconnecting
          : _loading || session == null
          ? l10n.requestingConnection
          : session.status,
      actions: [
        leave,
        if (updating)
          TextButton(
            key: const Key('android-host-update-cancel'),
            onPressed: _cancelHostUpdate,
            style: AndroidSessionButtons.quiet(chrome),
            child: Text(l10n.stopWaiting),
          )
        else if (restarting)
          TextButton(
            key: const Key('android-planned-restart-cancel'),
            onPressed: _cancelPlannedRestart,
            style: AndroidSessionButtons.quiet(chrome),
            child: Text(l10n.stopWaiting),
          ),
      ],
    );
  }

  ConnectionWaitStep _waitStep(RemoteSession? session) {
    if (_reconnecting && _hostUpdate) return ConnectionWaitStep.hostUpdate;
    if (_reconnecting && _plannedRestart) return ConnectionWaitStep.vmRestart;
    if (_preparingAgain) return ConnectionWaitStep.display;
    if (_reconnecting) return ConnectionWaitStep.reconnect;
    if (session == null || session.closed) return ConnectionWaitStep.request;
    // Connected, and the Host is setting the virtual display this connection
    // asked for (the line under the name says so): that stage, not 서버 연결.
    if (session.preparingInitialDisplay) return ConnectionWaitStep.display;
    if ((session.width > 0 && session.height > 0) ||
        session.textureId != null ||
        session.receivedBytes > 0) {
      return ConnectionWaitStep.firstFrame;
    }
    return ConnectionWaitStep.relay;
  }

  Widget _waitStatus(RemoteSession? session, {bool explainRestart = false}) {
    final chrome = SessionChrome.of(context);
    return Column(
      mainAxisSize: MainAxisSize.min,
      children: [
        ConnectionWaitStatus(
          step: _waitStep(session),
          attempt: _generation,
          elapsed: () => _waitClock.elapsed,
          foreground: chrome.ink,
          secondary: chrome.muted,
          // The chip over a still picture keeps one line; the connecting
          // card lists the stages.
          compact: explainRestart,
        ),
        if (explainRestart && _serverRestart.waiting)
          KeepAllText(
            serverRestartingMessage,
            style: _Chip.labelStyle.copyWith(color: chrome.muted),
          ),
      ],
    );
  }

  @override
  Widget build(BuildContext context) {
    final session = _session;
    final failed =
        _error ??
        (session?.closed == true && !_reconnecting
            ? session?.error ?? session?.status
            : null);
    // While the resolution changes, or the connection is made again, the
    // previous picture stays up at its own size until the new texture has a
    // frame (AndroidPictureContinuity).
    final shown = session == null || session.closed
        ? (_reconnecting ? _continuity.shown : null)
        : _continuity.update(
            texture:
                session.trackTextures[_selectedOutput?.id] ?? session.textureId,
            size: Size(
              (_selectedOutput?.width ?? session.width).toDouble(),
              (_selectedOutput?.height ?? session.height).toDouble(),
            ),
            settled:
                !session.resizing &&
                !session.pictureChanging &&
                session.frameReady,
          );
    // Reconnecting: the last picture, still, without input.
    final frozen = _reconnecting && shown != null && failed == null;
    final ready =
        !frozen &&
        session != null &&
        shown != null &&
        session.frameReady &&
        failed == null;
    final chrome = SessionChrome.of(context);
    final inset = MediaQuery.viewInsetsOf(context).bottom;
    if (_tvPanel != null && (!ready || !identical(_tvPanelOwner, session))) {
      WidgetsBinding.instance.addPostFrameCallback((_) => _closeTvPanel());
    }
    if (_keyboard) {
      if (inset > 0) {
        _imeSeen = true;
      } else if (_imeSeen) {
        final typingEpoch = _typingEpoch;
        final generation = _generation;
        final keyboardSession = _session;
        // Android hid its keyboard (Back): the key row goes with it.
        WidgetsBinding.instance.addPostFrameCallback((_) {
          if (mounted &&
              typingEpoch == _typingEpoch &&
              generation == _generation &&
              identical(keyboardSession, _session) &&
              _keyboard &&
              _imeSeen &&
              MediaQuery.viewInsetsOf(context).bottom == 0) {
            _toggleKeyboard();
          }
        });
      }
    }
    return PopScope(
      canPop: _allowPop,
      onPopInvokedWithResult: (didPop, _) {
        if (!didPop) _back();
      },
      child: AnnotatedRegion<SystemUiOverlayStyle>(
        value: SystemUiOverlayStyle.light,
        child: Scaffold(
          backgroundColor: const Color(0xff080d14),
          resizeToAvoidBottomInset: false,
          body: AndroidTvRemote(
            key: _tvInput,
            enabled:
                AndroidTvScope.of(context) &&
                ready &&
                !_keyboard &&
                !_menuOpen &&
                !_closing &&
                !_allowPop &&
                _toolPanel == null &&
                _barKey.currentState?.isOpen != true &&
                inset == 0,
            session: session,
            mode: _tvMode,
            speed: _settings.speed,
            position: () => _touch.cursor,
            source: () => _viewSource,
            canControl: () =>
                mounted &&
                _inFront &&
                !_menuOpen &&
                !_closing &&
                !_allowPop &&
                identical(session, _session) &&
                session?.frameReady == true &&
                session?.closed == false &&
                session?.resizing == false,
            onMove: (point) {
              _touch.cursor = point;
              _media?.cursorState.pointer(point);
              _touchEvent({
                'type': 'input',
                'event': 'move',
                'x': point.dx,
                'y': point.dy,
              });
            },
            onClick: (point, right) {
              _touchEvent({
                'type': 'input',
                'event': 'move',
                'x': point.dx,
                'y': point.dy,
              });
              for (final down in [true, false]) {
                _touchEvent({
                  'type': 'input',
                  'event': 'button',
                  'button': right ? 'right' : 'left',
                  'down': down,
                  'x': point.dx,
                  'y': point.dy,
                });
              }
            },
            onMenu: () => unawaited(_tvMenu()),
            onScroll: (point, delta) => _touchEvent({
              'type': 'input',
              'event': 'wheel',
              'delta': delta,
              'x': point.dx,
              'y': point.dy,
            }),
            onDragStart: (point) {
              final owner = session!;
              final revision = _pointerRevision;
              _touchEvent({
                'type': 'input',
                'event': 'move',
                'x': point.dx,
                'y': point.dy,
              });
              _touchEvent({
                'type': 'input',
                'event': 'button',
                'button': 'left',
                'down': true,
                'x': point.dx,
                'y': point.dy,
              });
              return (point, {required cancelled}) {
                if (cancelled ||
                    !identical(owner, _session) ||
                    revision != _pointerRevision ||
                    !_inFront ||
                    owner.closed ||
                    owner.resizing) {
                  if (identical(owner, _session)) {
                    _pointerRevision++;
                    _moves.discard();
                  }
                  unawaited(owner.capture(false));
                } else {
                  _touchEvent({
                    'type': 'input',
                    'event': 'button',
                    'button': 'left',
                    'down': false,
                    'x': point.dx,
                    'y': point.dy,
                  });
                }
              };
            },
            child: LayoutBuilder(
              builder: (context, constraints) {
                final size = constraints.biggest;
                final area = _safeArea(context, size);
                _measure(
                  area,
                  MediaQuery.devicePixelRatioOf(context),
                  MediaQuery.sizeOf(context),
                );
                final keyRow = _keyboard ? AndroidKeyRow.height : 0.0;
                final visible = Rect.fromLTRB(
                  area.left,
                  area.top,
                  area.right,
                  math.max(
                    area.top + 1,
                    math.min(area.bottom, size.height - inset - keyRow),
                  ),
                );
                final view = ready ? _layoutView(shown, area, visible) : null;
                final still = frozen ? _layoutView(shown, area, visible) : null;
                // 이동 only where part of the picture is out of sight; with
                // the keyboard open its handle shows by itself.
                final canPan = view != null && view.canPan(visible);
                // Overlay bounds include system bars and the key row's SafeArea;
                // they must not change the actual remote viewport measurement.
                final padding = MediaQuery.viewPaddingOf(context);
                final controls = Rect.fromLTRB(
                  math.max(area.left, padding.left),
                  math.max(area.top, padding.top),
                  math.min(area.right, size.width - padding.right),
                  math.min(
                    visible.bottom,
                    size.height - math.max(inset, padding.bottom) - keyRow,
                  ),
                );
                const diameter = AndroidMoveHandle.diameter;
                // The floating cluster: 키보드, 입력 방식 and 도구 in a row
                // (always one tap away). It needs room for that
                // row one way or the other; the dock opens anyway.
                const clusterLength = 3 * androidBubbleSize;
                final controlsFit =
                    controls.width >= androidBubbleSize &&
                    controls.height >= androidBubbleSize &&
                    (controls.width >= clusterLength ||
                        controls.height >= clusterLength);
                final wantsMove = canPan && (_moving || _keyboard);
                final bubble = controlsFit
                    ? androidBubbleLayout(
                        controls,
                        _settings.toolbarEdge,
                        _settings.toolbar,
                        length: clusterLength,
                      ).rect
                    : null;
                // 화면 이동 sits beside the cluster, towards the picture.
                Rect? move;
                if (bubble != null && wantsMove) {
                  const gap = 8.0, half = diameter / 2;
                  final centre = bubble.center;
                  final at = switch (_settings.toolbarEdge) {
                    AndroidBubbleEdge.right => centre.translate(
                      -(bubble.width / 2 + gap + half),
                      0,
                    ),
                    AndroidBubbleEdge.left => centre.translate(
                      bubble.width / 2 + gap + half,
                      0,
                    ),
                    AndroidBubbleEdge.top => centre.translate(
                      0,
                      bubble.height / 2 + gap + half,
                    ),
                    AndroidBubbleEdge.bottom => centre.translate(
                      0,
                      -(bubble.height / 2 + gap + half),
                    ),
                  };
                  final x = at.dx
                      .clamp(
                        controls.left + half,
                        math.max(controls.left + half, controls.right - half),
                      )
                      .toDouble();
                  final y = at.dy
                      .clamp(
                        controls.top + half,
                        math.max(controls.top + half, controls.bottom - half),
                      )
                      .toDouble();
                  move = Rect.fromCenter(
                    center: Offset(x, y),
                    width: diameter,
                    height: diameter,
                  );
                }
                final zoomLabel = view == null
                    ? ''
                    : '${(view.scale * 100).round()}%';
                // The chip stays centred above the picture's bottom edge.
                // Measure its actual labels rather than assuming a phone
                // width or text scale. The screen menu always retains _fit.
                // Padding 28 + label gap 10 + icon 17 + icon gap 4.
                final zoomWidth = view != null && view.zoomed
                    ? _Chip.textWidth(context, zoomLabel, _Chip.labelStyle) +
                          _Chip.textWidth(context, l10n.wholeScreen, _Chip.actionStyle) +
                          59
                    : 0.0;
                final zoomHeight = math.max(
                  48.0,
                  MediaQuery.textScalerOf(context).scale(13) * 1.3 + 16,
                );
                final zoomRect = Rect.fromLTWH(
                  (size.width - zoomWidth) / 2,
                  visible.bottom - 14 - zoomHeight,
                  zoomWidth,
                  zoomHeight,
                );
                final edges = math.max(area.left, size.width - area.right);
                // It never lies under the bubble or the move handle.
                // The open dock covers the bottom edge: no chip under it.
                final showZoom =
                    view != null &&
                    view.zoomed &&
                    !_barOpen &&
                    zoomWidth <= size.width - edges * 2 - 16 &&
                    !(ready &&
                        [?bubble, ?move].any(
                          (control) => control.inflate(6).overlaps(zoomRect),
                        ));
                if (ready) _captureWhenIdle();
                return Stack(
                  fit: StackFit.expand,
                  children: [
                    if (still != null)
                      IgnorePointer(
                        key: const Key('android-reconnecting-picture'),
                        child: ClipRect(
                          child: Stack(
                            children: [
                              Positioned.fromRect(
                                rect: still.rect,
                                child:
                                    widget.pictureBuilder?.call(context) ??
                                    Texture(
                                      key: ValueKey(shown!.texture),
                                      textureId: shown.texture,
                                      filterQuality:
                                          still.wholePixelScale(
                                                shown.size,
                                                _dpr,
                                              ) !=
                                              null
                                          ? FilterQuality.none
                                          : FilterQuality.low,
                                    ),
                              ),
                              // Dimmed a little: not live.
                              const Positioned.fill(
                                child: ColoredBox(color: Color(0x40000000)),
                              ),
                            ],
                          ),
                        ),
                      ),
                    if (still != null)
                      Positioned(
                        left: 0,
                        right: 0,
                        top: area.top + 44,
                        child: Center(
                          child: _hostUpdate
                              ? _Chip(
                                  key: const Key('android-host-update'),
                                  busy: true,
                                  label: l10n.hostUpdating,
                                  status: _waitStatus(
                                    session,
                                    explainRestart: true,
                                  ),
                                  action: l10n.stopWaiting,
                                  actionIcon: Icons.close_rounded,
                                  chrome: chrome,
                                  onTap: _cancelHostUpdate,
                                )
                              : _plannedRestart
                              ? _Chip(
                                  key: const Key('android-planned-restart'),
                                  busy: true,
                                  label: _hostAwayRestart
                                      ? l10n.waitingDesktop
                                      : l10n.desktopRestarting,
                                  status: _waitStatus(
                                    session,
                                    explainRestart: true,
                                  ),
                                  action: l10n.stopWaiting,
                                  actionIcon: Icons.close_rounded,
                                  chrome: chrome,
                                  onTap: _cancelPlannedRestart,
                                )
                              : _Chip(
                                  key: const Key('android-reconnecting'),
                                  busy: true,
                                  label: _serverRestart.waiting
                                      ? serverRestartingMessage
                                      : l10n.reconnecting2,
                                  status: _waitStatus(
                                    session,
                                    explainRestart: true,
                                  ),
                                  chrome: chrome,
                                ),
                        ),
                      ),
                    if (view != null)
                      Listener(
                        key: const Key('android-remote-surface'),
                        behavior: HitTestBehavior.opaque,
                        onPointerDown: (event) {
                          _hold.down(event.pointer);
                          // A touch back into the picture folds an unpinned bar.
                          _foldBar();
                          _pointer(event);
                        },
                        onPointerMove: _pointer,
                        onPointerUp: (event) {
                          _hold.up(event.pointer);
                          _pointer(event);
                        },
                        onPointerCancel: (event) {
                          _hold.up(event.pointer);
                          _pointer(event);
                        },
                        onPointerHover: _pointer,
                        onPointerSignal: _pointer,
                        child: ClipRect(
                          child: Stack(
                            children: [
                              Positioned.fromRect(
                                rect: view.rect,
                                child: _picture(session!, _media, shown!, view),
                              ),
                              Positioned.fill(
                                child: AndroidTouchFeedback(
                                  key: const Key('android-touch-feedback'),
                                  cues: _cues,
                                  view: () => _view,
                                  longPress: _touch.longPress,
                                ),
                              ),
                            ],
                          ),
                        ),
                      ),
                    if (ready && session.resizing)
                      Center(
                        child: _Chip(
                          busy: true,
                          label: l10n.fittingScreenSize,
                          chrome: chrome,
                        ),
                      )
                    else if (ready && _gameMouseWaits > 0)
                      Center(
                        child: _Chip(
                          key: const Key('android-game-mouse-waiting'),
                          busy: true,
                          label: l10n.androidMouseLockConnecting,
                          chrome: chrome,
                        ),
                      ),
                    if (!ready && !frozen)
                      Positioned.fill(
                        // Clear of the system bars and the cutout: its
                        // buttons stay whole on a short landscape screen.
                        child: SafeArea(
                          child: LayoutBuilder(
                            builder: (context, safe) => SingleChildScrollView(
                              child: ConstrainedBox(
                                constraints: BoxConstraints(
                                  minHeight: safe.maxHeight,
                                ),
                                child: Center(child: _panel(session, failed)),
                              ),
                            ),
                          ),
                        ),
                      ),
                    if (showZoom)
                      Positioned(
                        left: 0,
                        right: 0,
                        bottom: size.height - visible.bottom + 14,
                        child: Center(
                          child: _Chip(
                            key: const Key('android-zoom-chip'),
                            label: zoomLabel,
                            action: l10n.wholeScreen,
                            chrome: chrome,
                            onTap: _fit,
                          ),
                        ),
                      ),
                    // The phone keyboard types into this invisible field.
                    Positioned(
                      left: 0,
                      bottom: 0,
                      width: 2,
                      height: 2,
                      child: IgnorePointer(
                        child: Opacity(
                          opacity: 0,
                          child: AndroidTvImeOwner(
                            active:
                                AndroidTvScope.of(context) &&
                                _keyboard &&
                                _fieldFocus.hasFocus,
                            ownerToken: (_generation, _tvImeOwner),
                            onDismissed: () {
                              if (_keyboard) _toggleKeyboard();
                            },
                            child: TextField(
                              key: const Key('android-remote-keyboard-field'),
                              controller: _field,
                              focusNode: _fieldFocus,
                              keyboardType: TextInputType.multiline,
                              maxLines: null,
                              textInputAction: TextInputAction.newline,
                              textCapitalization: TextCapitalization.none,
                              autocorrect: false,
                              enableSuggestions: false,
                              enableIMEPersonalizedLearning: false,
                              smartDashesType: SmartDashesType.disabled,
                              smartQuotesType: SmartQuotesType.disabled,
                              enableInteractiveSelection: false,
                              showCursor: false,
                              onTapOutside: (_) {},
                              decoration: const InputDecoration.collapsed(
                                hintText: null,
                              ),
                              style: const TextStyle(
                                fontSize: 1,
                                color: Colors.transparent,
                              ),
                            ),
                          ),
                        ),
                      ),
                    ),
                    if (_keyboard)
                      Positioned(
                        left: 0,
                        right: 0,
                        bottom: inset,
                        child: SafeArea(
                          top: false,
                          bottom: inset == 0,
                          child: AndroidKeyRow(
                            latched: _latched,
                            locked: _locked,
                            onKey: _key,
                            onModifier: _modifier,
                            onHide: _toggleKeyboard,
                            onSecure: () => _notice(
                              l10n.ctrlAltDelCantSent,
                            ),
                          ),
                        ),
                      ),
                    // Keep these controls below toolbar popups in paint order.
                    if (ready && !_barOpen && move != null)
                      Positioned.fromRect(
                        rect: move,
                        child: AndroidMoveHandle(onPan: _pan, size: size),
                      ),
                    // The session tools: the amber cluster (wherever the
                    // user put it) and the menu it opens beside itself, with
                    // the 화면, 배율 and 설정 panels in the menu's place.
                    if (ready)
                      Positioned.fill(
                        // A stable slot: the siblings before it come and go, and the
                        // bar keeps its open state.
                        key: const ValueKey('android-session-bar-host'),
                        child: Builder(
                          builder: (context) {
                            _poorLatency.update(
                              connected: !session.closed,
                              ms: session.rttMs,
                            );
                            final panel = _openPanel(session, view);
                            return AndroidSessionBar(
                              key: _barKey,
                              popups: _popups,
                              title: widget.name,
                              modeLabel: l10n.streaming,
                              latencyMs: session.rttMs,
                              pointerHeld: _hold.held,
                              layoutKey: (
                                size,
                                MediaQuery.orientationOf(context),
                              ),
                              bubbleArea: controlsFit ? controls : null,
                              // Under ⋯: 입력 방식, then 키보드 at the end.
                              quick: [
                                AndroidQuickButton(
                                  key: const Key(
                                    'android-remote-touch-mode-button',
                                  ),
                                  icon: _touch.mode == AndroidTouchMode.trackpad
                                      ? Icons.mouse_rounded
                                      : Icons.touch_app_rounded,
                                  label: _touch.mode.label,
                                  tooltip:
                                      _touch.mode == AndroidTouchMode.trackpad
                                      ? l10n.nowPointerTrackpadTapTouch
                                      : l10n.nowTouchTapPointerTrackpad,
                                  onPressed: _toggleTouchModeByGesture,
                                  onLongPress: _openInputPanel,
                                ),
                                AndroidQuickButton(
                                  key: const Key(
                                    'android-remote-keyboard-button',
                                  ),
                                  icon: _keyboard
                                      ? Icons.keyboard_hide_rounded
                                      : Icons.keyboard_rounded,
                                  tooltip: _keyboard ? l10n.closeKeyboard : l10n.openKeyboard,
                                  active: _keyboard,
                                  onPressed: () {
                                    if (!_keyboard) _release();
                                    _toggleKeyboard();
                                  },
                                ),
                              ],
                              bubbleEdge: _settings.toolbarEdge,
                              bubbleAlong: _settings.toolbar,
                              onBubbleMoved: (edge, along) => setState(
                                () => _saveSettings(
                                  _settings.copyWith(
                                    toolbarEdge: edge,
                                    toolbar: along,
                                  ),
                                ),
                              ),
                              onInteract: _release,
                              onDisconnect: () => unawaited(_close(source: 2)),
                              onQuality: () => _togglePanel('connection'),
                              onPanelClosed: _closePanel,
                              onOpenChanged: (open) {
                                if (!mounted) return;
                                if (AndroidTvScope.of(context)) {
                                  _tvInput.currentState?.cancel();
                                  if (open) _release();
                                }
                                setState(() {
                                  _barOpen = open;
                                  if (!open) _toolPanel = null;
                                });
                              },
                              actions: _barActions(
                                session,
                                canPan: canPan,
                                pictured: view != null,
                              ),
                              panel: panel?.$1,
                              panelWidth: panel?.$2,
                            );
                          },
                        ),
                      ),
                    if (_noticeMessage != null)
                      Positioned(
                        left: math.max(12, area.left + 8),
                        right: math.max(12, size.width - area.right + 8),
                        top: area.top + 48,
                        child: AndroidRemoteNotice(
                          key: const Key('android-remote-notice'),
                          message: _noticeMessage!,
                        ),
                      ),
                    if (_tvPanel != null &&
                        ready &&
                        identical(_tvPanelOwner, session))
                      AndroidTvLocalPanel(
                        onClose: _closeTvPanel,
                        child: _tvPanel == 'screen'
                            ? _screenPanel(session, view)
                            : androidTvShortcuts(
                                _tvShortcuts(session),
                                _closeTvPanel,
                              ),
                      ),
                    if (_tvPanel == null)
                      Positioned.fromRect(
                        rect: visible,
                        child: StreamObservationOverlay(session: session),
                      ),
                  ],
                );
              },
            ),
          ),
        ),
      ),
    );
  }
}

class _Chip extends StatelessWidget {
  static const labelStyle = TextStyle(
    inherit: false,
    fontFamily: 'Pretendard',
    fontSize: 13,
    fontWeight: FontWeight.w600,
    fontFeatures: [FontFeature.tabularFigures()],
  );
  static const actionStyle = TextStyle(
    inherit: false,
    fontFamily: 'Pretendard',
    fontSize: 13,
    fontWeight: FontWeight.w600,
  );

  static double textWidth(BuildContext context, String text, TextStyle style) {
    final painter = TextPainter(
      text: TextSpan(
        text: keepAll(text),
        style: MediaQuery.boldTextOf(context)
            ? style.copyWith(fontWeight: FontWeight.w600)
            : style,
      ),
      textDirection: Directionality.of(context),
      textScaler: MediaQuery.textScalerOf(context),
      locale: Localizations.maybeLocaleOf(context),
    )..layout();
    final width = painter.width;
    painter.dispose();
    return width;
  }

  const _Chip({
    super.key,
    required this.label,
    required this.chrome,
    this.action,
    this.actionIcon = Icons.fit_screen_rounded,
    this.onTap,
    this.busy = false,
    this.status,
  });
  final String label;
  final String? action;
  final IconData actionIcon;
  final SessionChrome chrome;
  final VoidCallback? onTap;
  final bool busy;
  final Widget? status;

  @override
  Widget build(BuildContext context) => Material(
    color: chrome.surface.withValues(alpha: .92),
    shape: StadiumBorder(side: BorderSide(color: chrome.edge)),
    child: InkWell(
      customBorder: const StadiumBorder(),
      onTap: onTap,
      child: ConstrainedBox(
        // One that acts on a tap is a whole 48 dp target.
        constraints: BoxConstraints(minHeight: onTap == null ? 40 : 48),
        child: Padding(
          padding: const EdgeInsets.symmetric(horizontal: 14, vertical: 8),
          child: Row(
            mainAxisSize: MainAxisSize.min,
            children: [
              if (busy) ...[
                SizedBox.square(
                  dimension: 14,
                  child: CircularProgressIndicator(
                    strokeWidth: 2,
                    color: chrome.accent,
                  ),
                ),
                const SizedBox(width: 9),
              ],
              // A long line (the server restart) wraps on a narrow phone.
              Flexible(
                child:
                    status ??
                    KeepAllText(
                      label,
                      style: labelStyle.copyWith(color: chrome.ink),
                    ),
              ),
              if (action != null) ...[
                const SizedBox(width: 10),
                Icon(actionIcon, size: 17, color: chrome.accent),
                const SizedBox(width: 4),
                KeepAllText(
                  action!,
                  style: actionStyle.copyWith(color: chrome.accent),
                ),
              ],
            ],
          ),
        ),
      ),
    ),
  );
}
