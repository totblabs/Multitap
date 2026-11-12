import 'dart:async';

import 'package:flutter/material.dart';

import '../data/broker_api.dart';
import '../data/connect_timing.dart';
import '../data/desktop_mapping.dart';
import '../data/drive_files.dart';
import '../data/drive_memory.dart';
import '../data/drive_session.dart';
import '../data/launcher_memory.dart';
import '../data/native_usb.dart';
import '../data/usb_memory.dart';
import '../data/usb_redirection.dart';
import '../data/remote_session.dart';
import '../data/session_windows.dart';
import '../data/session_renewal.dart';
import '../data/session_activity.dart';
import 'app_palette.dart';
import 'session_access_boundary.dart';
import '../data/window_close_guard.dart';
import '../models/desktop_machine.dart';
import '../models/session_display_mode.dart';
import '../models/stream_options.dart';
import 'remote_view.dart';
import 'session_status_panel.dart';
import '../data/track_stream.dart';
import '../data/initial_viewport.dart';
import '../data/console_resume.dart';
import '../data/notices.dart';
import 'notice_widgets.dart';
import '../data/connection_feedback.dart';
import 'connection_wait_status.dart';
import '../l10n/l10n.dart';

class SessionWindow extends StatefulWidget {
  const SessionWindow({
    super.key,
    required this.launch,
    this.api,
    this.guard,
    this.preferences,
    this.driveFactory,
    this.usbFactory,
    this.clock = DateTime.now,
  });
  final SessionWindowLaunch launch;
  final BrokerApi? api;
  final WindowCloseGuard? guard;

  /// Replaces the native drive-v1 session (tests).
  final DriveSessionFactory? driveFactory;

  /// This PC's local memory; never sent to the broker.
  final LauncherMemory? preferences;

  /// Replaces the native usb-v1 session (tests).
  final UsbSessionFactory? usbFactory;

  /// The time source of the session's lifetime (tests substitute one).
  final DateTime Function() clock;
  @override
  State<SessionWindow> createState() => _SessionWindowState();
}

class _SessionWindowState extends State<SessionWindow> {
  late final _sessionAccess = SessionActivity(clock: widget.clock);
  late final _api =
      widget.api ??
      HttpBrokerApi(
        origin: widget.launch.origin,
        diagnosticsDesktopId: widget.launch.desktopId,
        diagnosticsMode: 'stream',
        enableDiagnostics: true,
        // Nested remote keyboards can commit Unicode instead of physical keys.
        requestTextInput: true,
        // Negotiate live DPI control without changing Windows' current scale.
        requestDisplayScaleControl: true,
        // print-v1: documents printed in the VM come here to print locally.
        requestPrint: true,
        // Folders on this PC can be shared with the VM (drive-v1) where the
        // administrator allows drives for this assignment.
        requestDrives: true,
        // This PC's USB devices can be lent to the VM (usb-v1) where the
        // administrator allows USB devices for this assignment.
        requestUsb: true,
        // udp-media-v1: where the desktop shows it, this client probes UDP.
        requestUdpMedia: true,
        // audio-opus-v1: where the desktop shows it, the runner decodes Opus.
        requestAudioOpus: true,
        // relative-mouse-v1: this window captures the mouse when a remote
        // application hides the cursor, where the desktop shows it.
        requestRelativeMouse: true,
        // session-resume-v1: a lost socket is replaced and the stream goes on.
        requestSessionResume: true,
        requestStreamTakeoverContext: true,
      );
  late final _guard = widget.guard ?? WindowCloseGuard.windows();
  late final _actions = SessionWindowActions(closeWindow: _closeWindow);
  DesktopMachine? _machine;
  String? _error;
  bool _closing = false, _retired = false;

  /// The first read waits through a server restart instead of failing.
  late final _restartWait = ServerRestartWait(clock: () => widget.clock());
  Timer? _restartRetry;
  bool _configuring = false;
  bool _displayScaleAvailable = false, _displayScaleOutputsAvailable = false;
  SessionRenewer? _renewer;

  /// 공지 (data/notices.dart): this window reads its sign-in's notices itself
  /// (it is another engine than the launcher's). A notice that arrives rises
  /// over the stream without taking the keyboard; the stream's control socket
  /// says when to read, and a timer is the fallback.
  NoticeCenter? _notices;
  late DateTime _expiresAt = widget.launch.expiresAt;
  bool get _authorized =>
      mounted && !_closing && !_retired && _expiresAt.isAfter(widget.clock());

  /// The window keeps its own session alive (the same token, moved forward),
  /// so a working remote session is never cut because the launcher's sign-in
  /// ran out. When renewal is impossible nothing is closed here: the broker's
  /// relay ends the session itself if access is really gone.
  void _keepAlive() {
    final api = _api;
    if (api is! SessionRenewalBrokerApi) return;
    final renewing = api as SessionRenewalBrokerApi;
    _renewer = SessionRenewer(
      clock: widget.clock,
      renew: () => renewing.renewSession(widget.launch.token),
      onRenewed: (next) {
        _expiresAt = next;
        renewing.recordSessionRenewal(widget.launch.token, stage: 1, ok: true);
      },
      onLapse: (reason) {
        if (reason == RenewalLapse.replaced ||
            reason == RenewalLapse.idle ||
            reason == RenewalLapse.rejected) {
          _retireAuthentication(
            _renewer?.terminalCode ??
                (reason == RenewalLapse.rejected
                    ? 'session_expired'
                    : reason == RenewalLapse.idle
                    ? 'session_idle_timeout'
                    : 'pc_login_replaced'),
          );
          return;
        }
        renewing.recordSessionRenewal(
          widget.launch.token,
          stage: 1,
          ok: false,
          code: switch (reason) {
            RenewalLapse.unsupported => 'request_failed',
            RenewalLapse.unreachable => 'network_error',
            _ => 'session_expired',
          },
        );
      },
    )..start(_expiresAt);
  }

  /// connect-timing-v1: the launcher's request -> this window's first state
  /// and its desktop read, given to the window's first session only.
  Map<String, int>? _windowStages = {};
  final _loadWatch = Stopwatch();

  @override
  void initState() {
    super.initState();
    final owner = widget.launch.authenticationOwner;
    if (owner != null) {
      SessionWindows.observeAuthentication(
        owner,
        terminal: _retireAuthentication,
        deadline: _sessionAccess.acceptPolicy,
      );
      _sessionAccess.start(
        widget.launch.sessionAccess,
        owner: owner,
        expiryAuthority: false,
        record: () async {
          await SessionWindows.forwardHuman(owner);
          return null; // Only the parent's actual server response extends idle.
        },
        onTerminal: _retireAuthentication,
      );
    }
    final noticeApi = _api;
    if (noticeApi is NoticeBrokerApi) {
      _notices = NoticeCenter(
        api: noticeApi as NoticeBrokerApi,
        token: widget.launch.token,
        // The launcher raised what was waiting at sign-in; this window raises
        // what is fresh.
        greeting: NoticeGreeting.recent,
        clock: widget.clock,
      )..start();
    }
    final requested = widget.launch.requestedAt;
    final waited = requested == null
        ? null
        : connectStageMs(widget.clock().difference(requested).inMicroseconds);
    if (waited != null) _windowStages!['windowMs'] = waited;
    _loadWatch.start();
    // Before a view is bound nothing is connected yet (loading or failed):
    // closing just closes.
    _actions.fallback = _closeWindow;
    _guard.attach(_actions.requestClose);
    unawaited(_guard.update(busy: false, dirty: true));
    SessionWindows.bindRetirement(_actions.retire);
    unawaited(SessionWindows.channel.invokeMethod<void>('ready'));
    // A window opened with an already expired token has nothing to keep.
    if (!_expiresAt.isAfter(widget.clock())) {
      scheduleMicrotask(() => unawaited(_actions.retire()));
    } else {
      _keepAlive();
    }
    unawaited(_load());
  }

  void _check() {
    if (!_authorized) throw const BrokerException('session_expired', 401);
  }

  void _retireAuthentication([String code = 'pc_login_replaced']) {
    if (!mounted || _closing || _retired) return;
    _retired = true;
    _sessionAccess.retire(code);
    final owner = widget.launch.authenticationOwner;
    if (owner != null) {
      final reason =
          const {'pc_login_replaced', 'session_idle_timeout'}.contains(code)
          ? code
          : 'session_reauthentication_required';
      unawaited(
        SessionWindows.forwardTerminal(owner, reason).catchError((Object _) {}),
      );
    }
    _restartRetry?.cancel();
    _renewer?.stop();
    unawaited(_actions.retire());
  }

  Future<T> _authenticationBound<T>(Future<T> pending) async {
    try {
      return await pending;
    } on BrokerException catch (error) {
      if (pcLoginTerminalCode(error.code, error.status) != null ||
          (error.status == 401 &&
              const {
                'session_expired',
                'authentication_required',
              }.contains(error.code))) {
        _retireAuthentication(error.code);
      }
      rethrow;
    }
  }

  void _observeSessionAuthentication() {
    final code = _liveSession?.diagnosticCode;
    if (const {
      'pc_login_replaced',
      'session_idle_timeout',
      'session_reauthentication_required',
    }.contains(code)) {
      _retireAuthentication(code!);
    }
  }

  void _checkConnection(BrokerConnection connection) {
    try {
      _check();
    } catch (_) {
      // The POST may finish after local window retirement. Release only its
      // negotiated, exact reservation; never dial it merely to send stop.
      unawaited(connection.release().catchError((Object _) {}));
      rethrow;
    }
  }

  Future<DesktopMachine> _readDesktop() async {
    _check();
    final desktops = await _authenticationBound(
      _api.desktops(widget.launch.token),
    );
    _check();
    final found = desktops.where(
      (desktop) => desktop.id == widget.launch.desktopId,
    );
    if (found.length != 1) {
      throw const BrokerException('desktop_not_found', 404);
    }
    final fresh = machineFromBroker(found.single);
    _displayScaleAvailable = found.single.displayScale;
    _displayScaleOutputsAvailable = found.single.displayScaleOutputs;
    if (fresh.kind == MachineKind.pc && !fresh.localPcApproved) {
      throw const BrokerException('desktop_not_found', 404);
    }
    // Retain the same fresh snapshot used by RemoteView for count prechecks.
    _machine = fresh;
    return fresh;
  }

  Future<void> _checkTakeover() async {
    _check();
    final api = _api;
    if (api is! StreamTakeoverPreflightBrokerApi) return;
    await _authenticationBound(
      (api as StreamTakeoverPreflightBrokerApi).checkStreamTakeover(
        widget.launch.token,
        widget.launch.desktopId,
      ),
    );
    _check();
  }

  /// host-ready-wait-v1: the broker answers once this desktop's Host is back
  /// (null where it does not offer the wait; the view then reads the list).
  Future<bool?> _waitHostReady() async {
    _check();
    final api = _api;
    if (api is! HostReadyBrokerApi) return null;
    final ready = await _authenticationBound(
      (api as HostReadyBrokerApi).waitHostReady(
        widget.launch.token,
        widget.launch.desktopId,
      ),
    );
    _check();
    return ready;
  }

  Future<void> _load() async {
    _restartRetry?.cancel();
    try {
      final machine = await _readDesktop();
      _restartWait.reset();
      if (_loadWatch.isRunning) {
        _loadWatch.stop();
        final loaded = connectStageMs(_loadWatch.elapsedMicroseconds);
        if (loaded != null) _windowStages?['loadMs'] = loaded;
      }
      if (_authorized) {
        setState(() {
          _machine = machine;
          _error = null;
        });
      }
    } catch (error) {
      if (mounted && !_closing && !_retired) {
        final wait = _restartWait.next(error);
        if (wait != null) {
          // The server restarts (an update): say so and try again by itself.
          setState(() => _error = null);
          _restartRetry = Timer(wait, () => unawaited(_load()));
          return;
        }
        setState(
          () => _error = error is BrokerException
              ? error.message
              : l10n.couldntGetDesktopConnectionInfo,
        );
      }
    }
  }

  /// The retry button: a new request waits through a restart afresh.
  void _retryLoad() {
    _restartWait.reset();
    unawaited(_load());
  }

  Future<RemoteSession> _stream(StreamOptions options) async {
    _check();
    final connection = await _authenticationBound(
      _api.connect(
        widget.launch.token,
        widget.launch.desktopId,
        options: options,
      ),
    );
    _checkConnection(connection);
    return _session(connection);
  }

  /// The latest streaming session, so closing the window can send its last
  /// statistics window before the diagnostics channel closes.
  RemoteSession? _liveSession;

  RemoteSession _session(BrokerConnection connection) {
    final api = _api;
    final windowStages = _windowStages;
    _windowStages = null;
    _liveSession?.removeListener(_observeSessionAuthentication);
    final session = RemoteSession(
      connection: connection,
      token: widget.launch.token,
      statsSink: api is HttpBrokerApi
          ? (values) => api.recordStreamStats(
              values,
              sessionId: connection.sessionId,
              desktopId: widget.launch.desktopId,
            )
          : null,
      stallSink: api is HttpBrokerApi
          ? (values) => api.recordStreamStall(
              values,
              sessionId: connection.sessionId,
              desktopId: widget.launch.desktopId,
            )
          : null,
      driveFactory: widget.driveFactory ?? _drive,
      usbFactory: widget.usbFactory ?? _usb,
    )..connectTiming.before.addAll(windowStages ?? const {});
    _liveSession = session;
    session.onBrokerNotices = _notices?.signal;
    session.addListener(_observeSessionAuthentication);
    return session;
  }

  /// drive-v1: the native worker shares the folders remembered for this VM.
  DriveSession _drive(BrokerConnection connection, String token) {
    DriveMemory? memory;
    try {
      memory = FileDriveMemory.windows();
    } catch (_) {
      /* Without app data nothing is remembered; sharing still works. */
    }
    return DriveSession(
      url: connection.driveWebSocketUrl!,
      token: token,
      sessionId: connection.sessionId,
      files: NativeDriveFileSystem(),
      fs: connection.driveFs,
      memory: memory,
      server: widget.launch.origin.origin,
      desktopId: widget.launch.desktopId,
    );
  }

  NativeUsbBackend? _usbBackend;

  /// usb-v1: the native runner captures this PC's devices through UsbDk.
  UsbSession _usb(BrokerConnection connection, String token) {
    UsbMemory? memory;
    try {
      memory = FileUsbMemory.windows();
    } catch (_) {
      /* Without app data the picker just forgets which devices were shared. */
    }
    return UsbSession(
      url: connection.usbWebSocketUrl!,
      token: token,
      sessionId: connection.sessionId,
      // The other remote windows show this VM's name for a device it holds.
      backend: _usbBackend ??= NativeUsbBackend(
        null,
        _machine?.name ?? widget.launch.name,
      ),
      memory: memory,
    );
  }

  Future<DesktopMachine> _resumeConsole() async {
    _check();
    final api = _api;
    if (api is! ConsoleRecoveryApi) {
      throw const BrokerException('console_recovery_unavailable');
    }
    if (_configuring || _machine?.canRecoverConsole != true) {
      throw const BrokerException('console_busy');
    }
    final result = await ConsoleResume(
      api: api as ConsoleRecoveryApi,
      loadDesktops: () => api.desktops(widget.launch.token),
      authorized: () => _authorized,
    ).run(widget.launch.token, widget.launch.desktopId);
    _check();
    final fresh = machineFromBroker(result);
    if (fresh.id != widget.launch.desktopId) {
      throw const BrokerException('invalid_response');
    }
    _machine = fresh;
    return fresh;
  }

  /// Ends this VM's connection in another window or on another PC; the view
  /// then connects (and waits for Host cleanup) through its ordinary path.
  Future<void> _confirmTakeover(
    StreamTakeoverOffer offer,
    bool Function() authorized,
  ) async {
    _check();
    if (!authorized()) throw const BrokerException('operation_cancelled');
    final api = _api;
    if (api is! ConfirmedTakeoverBrokerApi) {
      throw const BrokerException('invalid_takeover_context');
    }
    final released = await _authenticationBound(
      (api as ConfirmedTakeoverBrokerApi).confirmTakeover(
        widget.launch.token,
        widget.launch.desktopId,
        offer,
      ),
    );
    _check();
    if (!authorized()) throw const BrokerException('operation_cancelled');
    if (!released) throw const BrokerException('invalid_response');
  }

  Future<RemoteSession> _streamViewport(
    InitialViewport viewport,
    StreamOptions options,
  ) async {
    _check();
    final api = _api;
    if (api is! InitialViewportBrokerApi) {
      throw const BrokerException('initial_viewport_unavailable');
    }
    final connection = await _authenticationBound(
      (api as InitialViewportBrokerApi).connectViewport(
        widget.launch.token,
        widget.launch.desktopId,
        viewport,
        options,
      ),
    );
    _checkConnection(connection);
    return _session(connection);
  }

  Future<RemoteSession> _streamScaled(
    InitialViewport? viewport,
    StreamOptions options,
    int scale,
  ) async {
    _check();
    final api = _api;
    if (!_displayScaleAvailable || api is! DisplayScaleBrokerApi) {
      throw const BrokerException('display_not_supported');
    }
    final connection = await _authenticationBound(
      (api as DisplayScaleBrokerApi).connectScaled(
        widget.launch.token,
        widget.launch.desktopId,
        options: options,
        initialViewport: viewport,
        displayScale: scale,
      ),
    );
    _checkConnection(connection);
    return _session(connection);
  }

  Future<RemoteSession> _streamTracks(
    TrackPlan plan,
    StreamOptions options, {
    int? displayScale,
  }) async {
    _check();
    final api = _api;
    if (api is! TrackBrokerApi) {
      throw const BrokerException('track_streaming_unavailable');
    }
    final BrokerConnection connection;
    if (_machine?.requiresProfileTracks == true) {
      if (api is! ProfileTrackBrokerApi) {
        throw const BrokerException('track_streaming_unavailable');
      }
      connection = await _authenticationBound(
        (api as ProfileTrackBrokerApi).connectProfileTracks(
          widget.launch.token,
          widget.launch.desktopId,
          plan,
          options,
          displayScale: displayScale,
        ),
      );
    } else {
      connection = await _authenticationBound(
        (api as TrackBrokerApi).connectTracks(
          widget.launch.token,
          widget.launch.desktopId,
          plan,
          options,
          displayScale: displayScale,
        ),
      );
    }
    _checkConnection(connection);
    return _session(connection);
  }

  Future<DesktopMachine> _configure(int count) async {
    _check();
    if (count != 1 && count != 2) {
      throw const BrokerException('invalid_display_count');
    }
    if (_configuring || _machine?.display.busy == true) {
      throw const BrokerException('display_busy');
    }
    if (_machine?.powerBusy == true) {
      throw const BrokerException('power_in_progress');
    }
    if (_machine?.canConfigureDisplays != true) {
      throw const BrokerException('display_not_supported');
    }
    _configuring = true;
    final deadline = DateTime.now().add(const Duration(seconds: 30));
    Duration remaining() {
      final value = deadline.difference(DateTime.now());
      if (value <= Duration.zero) {
        throw const BrokerException('display_operation_timeout');
      }
      return value;
    }

    Future<T> bounded<T>(Future<T> Function() action) {
      final timeout = remaining();
      return _authenticationBound(action()).timeout(
        timeout,
        onTimeout: () =>
            throw const BrokerException('display_operation_timeout'),
      );
    }

    try {
      var state = await bounded(
        () => _api.configureDisplays(
          widget.launch.token,
          widget.launch.desktopId,
          count,
        ),
      );
      final id = state.operation?.id;
      if (id == null || state.operation?.requestedCount != count) {
        throw const BrokerException('invalid_response');
      }
      while (true) {
        _check();
        final operation = state.operation;
        if (operation?.id != id || operation?.requestedCount != count) {
          throw const BrokerException('invalid_response');
        }
        if (operation?.status == 'failed') {
          throw BrokerException(
            'display_operation_failed',
            0,
            operation?.error,
          );
        }
        if (operation?.status == 'unknown') {
          throw const BrokerException('display_operation_unknown');
        }
        if (state.applied(count, id)) {
          // An operation ACK alone is not proof of fresh active outputs.
          final fresh = await bounded(_readDesktop);
          final ids = fresh.monitors.map((monitor) => monitor.id).toSet();
          if (fresh.virtualMonitorControl &&
              fresh.hostOnline == true &&
              fresh.display.applied(count, id) &&
              fresh.monitors.length == count &&
              ids.length == count &&
              ids.contains('0') &&
              (count == 1 || ids.contains('1'))) {
            return fresh;
          }
        }
        remaining();
        await Future<void>.delayed(const Duration(milliseconds: 700));
        _check();
        try {
          state = await bounded(
            () => _api.display(
              widget.launch.token,
              widget.launch.desktopId,
              operationId: id,
            ),
          );
        } on BrokerException catch (error) {
          if (error.status >= 500 && DateTime.now().isBefore(deadline)) {
            continue;
          }
          rethrow;
        }
      }
    } finally {
      _configuring = false;
    }
  }

  Future<void> _closeWindow() async {
    if (_closing) return;
    _closing = true;
    _retired = true;
    _renewer?.stop();
    _notices?.close();
    final api = _api;
    _liveSession?.flushStats();
    if (api is HttpBrokerApi) await api.flushDiagnostics();
    _api.close();
    bool accepted = false;
    try {
      accepted = await _guard.close();
    } catch (_) {
      /* Retry only local closure. */
    }
    if (!accepted && mounted) {
      _closing = false;
      setState(() {
        _machine = null;
        _error = l10n.disconnectedClickCloseRemoteWindow;
      });
    }
  }

  @override
  void dispose() {
    _sessionAccess.dispose();
    SessionWindows.observeAuthentication(null);
    _closing = true;
    _restartRetry?.cancel();
    _renewer?.stop();
    _liveSession?.removeListener(_observeSessionAuthentication);
    _notices?.dispose();
    _api.close();
    unawaited(_usbBackend?.dispose());
    _guard.dispose();
    SessionWindows.bindRetirement(null);
    super.dispose();
  }

  @override
  Widget build(BuildContext context) {
    final notices = _notices;
    final window = _buildWindow(context);
    if (notices == null) return window;
    // The banner floats over the top-right corner of the primary monitor's
    // part of the window (the whole window with one monitor and in full
    // screen). Everything else in this layer lets the pointer through.
    return Stack(
      fit: StackFit.expand,
      children: [
        window,
        NoticeCorner(
          top: 44,
          child: NoticeBanner(
            center: notices,
            onStream: true,
            clock: widget.clock,
          ),
        ),
      ],
    );
  }

  Widget _buildWindow(BuildContext context) {
    final machine = _machine;
    return SessionAccessBoundary(
      controller: _sessionAccess,
      child: PopScope(
        canPop: false,
        onPopInvokedWithResult: (popped, _) {
          if (!popped) unawaited(_actions.requestClose());
        },
        child: machine == null
            ? Scaffold(
                backgroundColor: AppPalette.stage,
                body: SessionStatusPanel(
                  backLabel: _error == null ? l10n.cancelConnection : l10n.closeRemoteWindow,
                  machine: widget.launch.name,
                  message:
                      _error ??
                      (_restartWait.waiting
                          ? serverRestartingMessage
                          : l10n.checkingDesktopConnectionInfo),
                  busy: _error == null,
                  // The list the connection card shows next, at its first
                  // row: the card keeps its size when that one replaces it.
                  status: _error == null
                      ? const ConnectionWaitStatus(
                          step: ConnectionWaitStep.request,
                        )
                      : null,
                  onBack: () => unawaited(_actions.requestClose()),
                  onRetry: _error == null || _retired ? null : _retryLoad,
                ),
              )
            : RemoteView(
                sessionGuideEnabled: true,
                guideAccountKey: widget.launch.guideAccountKey,
                onRefreshDesktop: _readDesktop,
                onCheckTakeover: _api is StreamTakeoverPreflightBrokerApi
                    ? _checkTakeover
                    : null,
                onWaitHostReady: _waitHostReady,
                onPrepareConsole: _resumeConsole,
                // Windows signed out or locked, or the Host away: the
                // launcher opens this VM's Console.
                onOpenConsole: machine.kind == MachineKind.vm
                    ? () async {
                        await SessionWindows.openConsoleInLauncher(machine.id);
                      }
                    : null,
                onConfirmTakeover: _confirmTakeover,
                takeoverServer: widget.launch.origin.toString(),
                prepareDisplaysOnEntry: true,
                machine: machine,
                load: () => _stream(
                  widget.launch.options.forMonitors(machine.monitors),
                ),
                onReconnect: _stream,
                onConnectTracks: _streamTracks,
                onConnectTracksScaled: _displayScaleOutputsAvailable
                    ? (plan, options, scale) =>
                          _streamTracks(plan, options, displayScale: scale)
                    : null,
                onConnectViewport: _streamViewport,
                onConnectScaled:
                    _displayScaleAvailable && _api is DisplayScaleBrokerApi
                    ? _streamScaled
                    : null,
                onConfigureDisplays: _configure,
                initialOptions: widget.launch.options.forMonitors(
                  machine.monitors,
                ),
                // 모든 모니터 streams on every monitor only where the Host can
                // stream separate outputs.
                initialDisplayMode:
                    widget.launch.mode == SessionDisplayMode.allMonitors &&
                        !machine.independentDisplayStreaming
                    ? SessionDisplayMode.fullscreen
                    : widget.launch.mode,
                windowActions: _actions,
                pointerMemory: widget.preferences,
              ),
      ),
    );
  }
}
