import 'dart:async';

import 'package:flutter/material.dart';
import 'package:flutter/services.dart';

import '../data/host_settings_api.dart';
import '../data/broker_transport.dart';
import 'broker_transport_controls.dart';
import '../data/host_devices.dart';
import '../data/host_overview.dart';
import '../data/host_prerequisites.dart';
import '../data/host_diagnostics.dart';
import '../data/client_settings.dart';
import '../data/update_handoff.dart';
import '../data/window_close_guard.dart';
import '../data/client_build_version.dart';
import 'app_dialog.dart';
import 'app_info_dialog.dart';
import 'app_theme.dart';
import 'app_toast.dart';
import 'appearance_switch.dart';
import 'components.dart' show ProductMark;
import 'host_settings_navigation.dart';
import 'host_encoder_panel.dart';
import 'host_overview_panels.dart';
import 'password_input_scope.dart';
import 'product_update_dialog.dart';
import 'product_log_dialog.dart';
import '../data/product_log.dart';
import 'korean_text.dart';
import 'update_messages.dart' show withRo;
import 'workspace_header.dart' show HeaderControl;
import '../l10n/l10n.dart';

class HostSettingsScreen extends StatefulWidget {
  const HostSettingsScreen({
    super.key,
    this.api,
    this.closeGuard,
    this.updateCheck,
  });
  final HostSettingsApi? api;
  final WindowCloseGuard? closeGuard;

  /// The newer Host version in the configured server's public catalog, or
  /// null (see newerProductVersion). Only the catalog is read; it drives the
  /// header's update button, and installing stays the user's choice in the
  /// update panel. Without it the header offers the manual check only.
  final Future<String?> Function(String server, String currentVersion)?
  updateCheck;
  @override
  State<HostSettingsScreen> createState() => _HostSettingsScreenState();
}

class _HostSettingsScreenState extends State<HostSettingsScreen>
    with WidgetsBindingObserver {
  late final HostSettingsApi _api = widget.api ?? HostSettingsApi.windows();
  late final WindowCloseGuard _closeGuard =
      widget.closeGuard ?? WindowCloseGuard.windows();
  final _origin = TextEditingController();
  final _scroll = ScrollController();
  final _repairPlanAnchor = GlobalKey();
  final _accountUsername = TextEditingController(),
      _accountPassword = TextEditingController(),
      _accountPasswordConfirmation = TextEditingController();
  // One Windows account: saving it also turns automatic login on, which stays
  // on. There is no separate auto-login form or switch.
  bool _windowsConfirming = false;
  String? _windowsPendingId, _windowsPendingAction;
  // Set when the typed local account has no Windows password: the form then
  // makes one for exactly this account (its SID), then saves it.
  ({String username, String sid, int minimumLength})? _passwordSetup;
  WindowsAutologonStatus get _windowsLogin =>
      WindowsAutologonStatus.parse(_state['windowsAutologon']);
  Map<String, dynamic> _state = {};
  // host-prerequisites-v1: the Host installer places files and the service
  // only. What a first install still needs is read once the status says no
  // virtual display is installed, offered once per window, and stays as the
  // overview's banner until it is done (host_prerequisites.dart).
  HostPrerequisites? _prerequisites;
  bool _finishOffered = false;
  Map<String, dynamic>? _displayRepairPlan, _displayRepairResult;
  bool _repairTimedOut = false;
  int? _repairExitCode;
  final _originFocus = FocusNode();
  String? _tested, _notice, _originError;
  String _brokerScheme = 'https';
  String? _httpConsentOrigin, _boundHostHttpOrigin;
  int _transportRevision = 0;
  bool _transportConfirming = false;
  String? _accountUsernameError,
      _accountPasswordError,
      _accountConfirmationError;
  bool _mutating = false, _closeDialog = false;
  bool _encoderDirty = false,
      _encoderMutating = false,
      _encoderPending = false,
      _encoderNavigation = false;
  bool _infoOpen = false;
  // 진단's technical rows stay folded unless asked for (자세히 or 연결 진단).
  bool _diagnosticsDetails = false;
  Timer? _approvalTimer;
  final _statusAge = Stopwatch();
  Future<Map<String, dynamic>>? _statusRequest;
  int _statusGeneration = 0;
  int _operationGeneration = 0;
  int _clearAccountConsentGeneration = 0;
  bool _statusReloadNeeded = false;
  bool _approvalPolling = false,
      _approvalReadError = false,
      _approvalDeadlineReached = false,
      _foreground = true;
  String? _approvalIdentity;
  DateTime? _approvalDeadline;
  String _operation = l10n.checkingHostStatus, _phase = l10n.waitingWindowsRespond;
  bool _busy = true,
      _error = false,
      _editRegistration = false,
      _editAccount = false;
  bool _disposing = false;
  int _page = 0;
  AppPalette get _palette => AppPalette.of(context);
  // The header's update button: the newer version the catalog named for this
  // server and version, read again at most every ten minutes.
  String? _updateVersion, _updateKey;
  DateTime? _updateCheckedAt;
  Map<String, dynamic> _section(String key) =>
      Map<String, dynamic>.from(_state[key] as Map? ?? {});
  bool get _admin => _state['administrator'] == true;
  // No virtual display of either kind is installed and this Host's own driver
  // package can be installed. A Host whose display works is never asked.
  bool get _displayMissing =>
      _section('display')['installed'] == false &&
      (_prerequisites?.displayInstallable ?? false);
  // The driver is missing but must not be installed from here (why, in words).
  String? get _displayFinishAttention =>
      _section('display')['installed'] == false
      ? _prerequisites?.attention
      : null;
  // WinFsp is missing and this Host carries its installer (status `devices`).
  bool get _winfspMissing {
    final devices = _state['devices'];
    final drives = devices is Map ? devices['drives'] : null;
    return drives is Map &&
        drives['winfsp'] == false &&
        drives['package'] == true;
  }

  bool get _registered => _state['registered'] == true;
  Map<String, dynamic> get _connectionRequest => _section('connectionRequest');
  String get _requestStatus => '${_connectionRequest['status'] ?? ''}';
  String get _requestOrigin => '${_connectionRequest['origin'] ?? ''}';
  bool get _awaitingApproval =>
      _requestStatus == 'pending' ||
      (_requestStatus == 'approved' &&
          (!_registered || _state['serverOrigin'] != _requestOrigin));
  bool get _cancellingConnection =>
      _requestStatus == 'cancelled' &&
      _connectionRequest['error'] == 'cancel_pending';
  // host-request-code-v1: the code the native Host derives from this request's
  // key; the administrator compares it with the console before approving.
  // Anything that is not exactly the Crockford XXXX-XXXX form is not shown.
  static final _verificationCodePattern = RegExp(
    r'^[0-9A-HJKMNP-TV-Z]{4}-[0-9A-HJKMNP-TV-Z]{4}$',
  );
  String? get _verificationCode {
    final value = _connectionRequest['verificationCode'];
    return value is String && _verificationCodePattern.hasMatch(value)
        ? value
        : null;
  }

  bool get _connectionWorkflow => _awaitingApproval || _cancellingConnection;
  bool get _needsStatusPoll =>
      _statusReloadNeeded ||
      _state.isEmpty ||
      (_connectionWorkflow
          ? !_approvalDeadlineReached
          : _registered ||
                (_requestStatus.isEmpty && _state['version'] is String));
  bool get _brokerOnline {
    final broker = _section('broker');
    final age = broker['heartbeatAgeSeconds'];
    return _registered &&
        _section('service')['running'] == true &&
        !_approvalReadError &&
        broker['state'] == 'online' &&
        broker['online'] == true &&
        age is num &&
        age.isFinite &&
        age >= 0 &&
        age + _statusAge.elapsedMilliseconds / 1000 <= 30;
  }

  bool get _accountDirty =>
      _accountPassword.text.isNotEmpty ||
      _accountPasswordConfirmation.text.isNotEmpty ||
      _accountUsername.text !=
          '${_section('windowsAccount')['username'] ?? ''}';
  bool get _hasDrafts =>
      _encoderDirty ||
      _accountDirty ||
      ((!_registered || _editRegistration) &&
          !_connectionWorkflow &&
          _origin.text.trim() !=
              (_registered
                  ? '${_state['serverOrigin'] ?? ''}'
                  : _requestOrigin));
  Future<bool> _syncClose() =>
      _closeGuard.update(busy: _mutating || _encoderPending, dirty: _hasDrafts);
  void _draftsChanged() {
    unawaited(_syncClose());
  }

  void _resetScroll() {
    if (_scroll.hasClients) _scroll.jumpTo(0);
  }

  void _navigate(int page) {
    unawaited(_navigateEncoderSafe(page));
  }

  Future<void> _navigateEncoderSafe(int page) async {
    if (_encoderMutating || _encoderNavigation || page == _page) return;
    if (_encoderPending) {
      showAppToast(context, l10n.checkEncoderSaveStatusFirst);
      return;
    }
    if (_page == 2 && _encoderDirty) {
      _encoderNavigation = true;
      try {
        if (!await _confirm(
          l10n.discardEncoderChoice,
          l10n.unsavedEncoderChoiceNotApplied,
          accept: l10n.discardChanges2,
        )) {
          return;
        }
        if (!mounted || _encoderMutating || _encoderPending) return;
        _encoderDirty = false;
      } finally {
        _encoderNavigation = false;
      }
    }
    if (!mounted) return;
    setState(() => _page = page);
    _resetScroll();
    unawaited(_syncClose());
  }

  Future<bool> _encoderProtectMutation(bool begin) async {
    if (begin) {
      if (_busy || _mutating || _encoderPending || !_admin) return false;
      ++_statusGeneration;
      _approvalTimer?.cancel();
      setState(() {
        _encoderMutating = true;
        _mutating = true;
        _busy = true;
        _operation = l10n.savingEncoderChoice;
        _phase = l10n.savingChoiceNextStreamingConnection;
      });
      try {
        if (await _syncClose()) return true;
      } catch (_) {
        /* Keep the write behind a functioning close guard. */
      }
    } else if (!_encoderMutating) {
      return false;
    }
    _encoderMutating = false;
    _mutating = false;
    if (mounted) {
      setState(() => _busy = false);
      unawaited(_syncClose());
      _scheduleApprovalPoll();
    }
    return false;
  }

  void _encoderDraftChanged(bool dirty) {
    if (!mounted || dirty == _encoderDirty) return;
    setState(() => _encoderDirty = dirty);
    unawaited(_syncClose());
  }

  void _encoderPendingChanged(bool pending) {
    if (!mounted || pending == _encoderPending) return;
    setState(() => _encoderPending = pending);
    unawaited(_syncClose());
  }

  Future<void> _requestClose() async {
    if (!mounted || _closeDialog) return;
    if (_mutating || _encoderPending) {
      showAppToast(
        context,
        _encoderPending
            ? l10n.checkEncoderSaveStatusBefore
            : l10n.closeWindowAfterSettingsTask,
      );
      return;
    }
    _closeDialog = true;
    try {
      if (_hasDrafts &&
          !await _confirm(
            l10n.discardUnsavedSettings,
            l10n.serverAddressAccountDetailsEncoder,
            accept: l10n.closeWithoutSaving,
            whileReading: true,
          )) {
        return;
      }
      if (mounted) _clearAccountConsentGeneration++;
      if (mounted && !await _closeGuard.close()) {
        setState(() {
          _notice = l10n.taskStatusChangedSoWindow;
          _error = true;
        });
      }
    } finally {
      _closeDialog = false;
    }
  }

  @override
  void initState() {
    super.initState();
    WidgetsBinding.instance.addObserver(this);
    _foreground =
        WidgetsBinding.instance.lifecycleState == null ||
        WidgetsBinding.instance.lifecycleState == AppLifecycleState.resumed;
    _closeGuard.attach(_requestClose);
    _origin.addListener(_draftsChanged);
    _accountUsername.addListener(_draftsChanged);
    _accountPassword.addListener(_draftsChanged);
    _accountPasswordConfirmation.addListener(_draftsChanged);
    unawaited(_refresh(initial: true));
  }

  @override
  void didChangeAppLifecycleState(AppLifecycleState state) {
    _foreground = state == AppLifecycleState.resumed;
    if (!_foreground) _statusGeneration++;
    _scheduleApprovalPoll();
  }

  Future<Map<String, dynamic>> _readStatus(int generation) async {
    // Keep the original future until it settles, even after our UI deadline.
    // A late helper response must not start a second overlapping status process.
    final previous = _statusRequest;
    if (previous != null) {
      try {
        await previous.timeout(const Duration(seconds: 10));
      } on TimeoutException {
        throw HostSettingsException(l10n.statusResponseDelayedCheckAgain);
      } catch (_) {
        // A fresh read may recover a completed failed helper process.
      }
    }
    if (!mounted || generation != _statusGeneration) {
      throw HostSettingsException(l10n.previousStatusCheckWasCanceled);
    }
    final request = _api.request('status');
    _statusRequest = request;
    unawaited(
      request.then<void>(
        (_) {
          if (identical(_statusRequest, request)) _statusRequest = null;
        },
        onError: (Object _, StackTrace _) {
          if (identical(_statusRequest, request)) _statusRequest = null;
        },
      ),
    );
    try {
      return await request.timeout(const Duration(seconds: 10));
    } on TimeoutException {
      throw HostSettingsException(l10n.statusResponseDelayedCheckAgain);
    }
  }

  void _applyStatus(Map<String, dynamic> response, {bool initial = false}) {
    final wasAwaiting = _awaitingApproval;
    final previousPage = _page;
    final state = Map<String, dynamic>.from(response['status'] as Map);
    try {
      final saved = normalizeBrokerOrigin(
        state['serverOrigin'] as String? ?? '',
      );
      if (saved.scheme == 'http' &&
          state['administrator'] == true &&
          state['httpConsentOrigin'] == saved.origin) {
        BrokerTransport.restore(saved, state['httpConsentOrigin']);
        _boundHostHttpOrigin = saved.origin;
      } else if (_boundHostHttpOrigin != null) {
        BrokerTransport.clear();
        _boundHostHttpOrigin = null;
      }
    } on FormatException {
      if (_boundHostHttpOrigin != null) {
        BrokerTransport.clear();
        _boundHostHttpOrigin = null;
      }
    }
    // The update notice acts only on this Host's configured server/version.
    UpdateNotices.instance.configure(
      server: state['serverOrigin'] as String?,
      currentVersion: state['version'] as String?,
    );
    setState(() {
      _state = state;
      _statusReloadNeeded = false;
      if ((state['display'] as Map?)?['provider'] == 'native') {
        _displayRepairPlan = null;
        _displayRepairResult = null;
      }
      _statusAge
        ..reset()
        ..start();
      if (initial && _origin.text.isEmpty) {
        _origin.text = _connectionWorkflow
            ? _requestOrigin
            : state['serverOrigin'] as String? ?? '';
      }
      final request = _connectionRequest;
      final identity =
          '${request['requestId'] ?? ''}|$_requestOrigin|${request['expiresAt'] ?? ''}';
      if (_approvalIdentity != identity) {
        _approvalIdentity = identity;
        _approvalDeadlineReached = false;
        _approvalDeadline =
            DateTime.tryParse('${request['expiresAt'] ?? ''}') ??
            DateTime.now().add(const Duration(minutes: 15));
      }
      if (wasAwaiting &&
          !_awaitingApproval &&
          _registered &&
          (_requestStatus.isEmpty ||
              (_requestStatus == 'approved' &&
                  state['serverOrigin'] == _requestOrigin))) {
        _editRegistration = false;
        _tested = null;
        _origin.text = state['serverOrigin'] as String? ?? '';
        if (_page == 1) _page = 0;
        _notice = l10n.administratorApprovedServerRegistrationComplete;
        _error = false;
      }
      if (initial || !_editAccount) {
        _accountUsername.text =
            '${_section('windowsAccount')['username'] ?? ''}';
      }
      if (!_admin) {
        _accountPassword.clear();
        _accountPasswordConfirmation.clear();
      }
      final prefix = _origin.text.split('://');
      if (prefix.length > 1 &&
          const {'http', 'https'}.contains(prefix.first.toLowerCase())) {
        _brokerScheme = prefix.first.toLowerCase();
      }
      _approvalReadError = false;
    });
    if (_page != previousPage) _resetScroll();
    _scheduleApprovalPoll();
    unawaited(_syncClose());
    _checkForUpdate();
  }

  void _scheduleApprovalPoll() {
    _approvalTimer?.cancel();
    _approvalTimer = null;
    if (!mounted || !_foreground || !_needsStatusPoll || _busy) {
      return;
    }
    if (!_connectionWorkflow) {
      _approvalTimer = Timer(const Duration(seconds: 3), () {
        if (mounted) unawaited(_pollApproval());
      });
      return;
    }
    final remaining =
        _approvalDeadline?.difference(DateTime.now()) ??
        const Duration(minutes: 15);
    if (remaining <= Duration.zero) {
      setState(() => _approvalDeadlineReached = true);
      return;
    }
    final atDeadline = remaining <= const Duration(seconds: 3);
    _approvalTimer = Timer(
      atDeadline ? remaining : const Duration(seconds: 3),
      () {
        if (!mounted) return;
        if (atDeadline) {
          setState(() => _approvalDeadlineReached = true);
        } else {
          unawaited(_pollApproval());
        }
      },
    );
  }

  Future<void> _pollApproval() async {
    if (!mounted ||
        !_foreground ||
        !_needsStatusPoll ||
        _busy ||
        _approvalPolling ||
        _statusRequest != null) {
      if (mounted && _foreground && !_busy && _registered) {
        setState(
          () {},
        ); // Age the last ACK even while a helper read is delayed.
      }
      _scheduleApprovalPoll();
      return;
    }
    _approvalPolling = true;
    _scheduleApprovalPoll();
    final generation = _statusGeneration;
    try {
      final response = await _readStatus(generation);
      if (mounted && _foreground && generation == _statusGeneration && !_busy) {
        _applyStatus(response, initial: _state.isEmpty);
      }
    } catch (_) {
      if (mounted && _foreground && generation == _statusGeneration && !_busy) {
        setState(() => _approvalReadError = true);
      }
    } finally {
      _approvalPolling = false;
      _scheduleApprovalPoll();
    }
  }

  Future<bool> _load({bool initial = false}) async {
    final generation = _statusGeneration;
    final response = await _readStatus(generation);
    if (!mounted || !_foreground || generation != _statusGeneration) {
      if (mounted) _statusReloadNeeded = true;
      return false;
    }
    _applyStatus(response, initial: initial);
    return true;
  }

  void _checkForUpdate({bool force = false}) {
    final check = widget.updateCheck;
    final server = _state['serverOrigin'], version = _state['version'];
    if (check == null ||
        server is! String ||
        server.isEmpty ||
        version is! String ||
        !RegExp(r'^[0-9]{1,6}\.[0-9]{1,6}\.[0-9]{1,6}$').hasMatch(version)) {
      _updateKey = null;
      if (_updateVersion != null) setState(() => _updateVersion = null);
      return;
    }
    final key = '$server|$version', now = DateTime.now();
    if (key != _updateKey && _updateVersion != null) {
      // Another server or an updated Host: the old answer no longer applies.
      setState(() => _updateVersion = null);
    } else if (!force &&
        key == _updateKey &&
        _updateCheckedAt != null &&
        now.difference(_updateCheckedAt!) < const Duration(minutes: 10)) {
      return;
    }
    _updateKey = key;
    _updateCheckedAt = now;
    unawaited(
      check(server, version).then<void>(
        (newer) {
          if (!mounted || _disposing || _updateKey != key) return;
          if (newer != _updateVersion) setState(() => _updateVersion = newer);
        },
        onError: (Object _) {
          // Not known: the manual check stays.
        },
      ),
    );
  }

  bool get _canCheckUpdates =>
      !_busy &&
      _state['serverOrigin'] is String &&
      (_state['serverOrigin'] as String).isNotEmpty &&
      _state['version'] is String &&
      RegExp(r'^[0-9]{1,6}\.[0-9]{1,6}\.[0-9]{1,6}$')
          .hasMatch(_state['version'] as String);

  Future<void> _checkUpdates() => showProductUpdates(
    context,
    server: _state['serverOrigin'] as String,
    product: 'host',
    currentVersion: _state['version'] as String,
  );

  Future<void> _refresh({bool initial = false}) async {
    if (_busy && !initial) return;
    _statusGeneration++;
    final generation = _statusGeneration, owner = ++_operationGeneration;
    _approvalTimer?.cancel();
    setState(() {
      _busy = true;
      _operation = l10n.checkingHostStatus;
      _phase = l10n.checkingWindowsServiceStatus;
    });
    try {
      final applied = await _load(initial: initial);
      if (applied) await _readPrerequisites();
      if (applied && !initial && mounted) _checkForUpdate(force: true);
      if (mounted &&
          owner == _operationGeneration &&
          generation == _statusGeneration &&
          applied) {
        setState(() {
          _notice = null;
          _error = false;
        });
      }
    } catch (error) {
      if (mounted &&
          _foreground &&
          owner == _operationGeneration &&
          generation == _statusGeneration) {
        setState(() {
          _notice = '$error';
          _error = true;
          _approvalReadError = true;
        });
      }
    } finally {
      if (mounted && owner == _operationGeneration) {
        setState(() => _busy = false);
        unawaited(_syncClose());
        _scheduleApprovalPoll();
      }
    }
    // A first install is finished here: asked once, as soon as the window
    // knows the virtual display driver is missing. Declining leaves the banner.
    if (initial &&
        mounted &&
        owner == _operationGeneration &&
        !_busy &&
        !_finishOffered &&
        _admin &&
        _displayMissing) {
      _finishOffered = true;
      await _finishInstall();
    }
  }

  // Read only, and only while the status says no virtual display is
  // installed: a Host whose display works costs no extra helper process.
  Future<void> _readPrerequisites() async {
    HostPrerequisites? next;
    if (_admin && _section('display')['installed'] == false) {
      try {
        final reply = await _api
            .request('prerequisiteStatus')
            .timeout(const Duration(seconds: 15));
        next = HostPrerequisites.parse(reply['prerequisites']);
      } catch (_) {
        next = null; // Not known: nothing is offered.
      }
    }
    if (mounted && !_disposing && !identical(_prerequisites, next)) {
      setState(() => _prerequisites = next);
    }
  }

  // What the status says once the service has finished taking a new device:
  // the service runs, a registered Host is connected to the server, and the
  // virtual display is ready where this Windows login can show one.
  bool get _statusSettled {
    final display = _section('display');
    final shows =
        display['installed'] == true &&
        display['remoteSession'] != true &&
        display['rebootRequired'] != true &&
        _state['interactiveSession'] == true;
    return _section('service')['running'] == true &&
        (!_registered || _brokerOnline) &&
        (!shows || _virtualDisplayReady);
  }

  // After the finish the service opens the new device (or was restarted) and
  // connects to the server again. The one read a step takes right after its
  // reply can show the moment in between, and the areas would keep "확인 필요"
  // from it. So the status is read again until it has settled, at most ten
  // times two seconds apart, under the progress panel.
  Future<void> _settleStatus() async {
    if (!mounted || _busy || _statusSettled) return;
    final owner = ++_operationGeneration;
    _statusGeneration++;
    _approvalTimer?.cancel();
    setState(() {
      _busy = true;
      _operation = l10n.checkingHostStatus;
      _phase = l10n.hostServicePreparingVirtualDisplay;
    });
    try {
      for (var attempt = 0; attempt < 10; attempt++) {
        await Future<void>.delayed(const Duration(seconds: 2));
        if (!mounted || owner != _operationGeneration) return;
        try {
          await _load();
        } catch (_) {
          // The next read may succeed; the last applied status stays.
        }
        if (!mounted || owner != _operationGeneration) return;
        if (_statusSettled) break;
      }
    } finally {
      if (mounted && owner == _operationGeneration) {
        setState(() => _busy = false);
        unawaited(_syncClose());
        _scheduleApprovalPoll();
      }
    }
  }

  // The finish of a first install: one confirmation (it names the certificate
  // when one must be trusted), then the virtual display driver and WinFsp in
  // turn under the progress panel, then one result. Nothing restarts Windows.
  Future<void> _finishInstall() async {
    if (_busy || !_admin) return;
    final winfsp = _winfspMissing;
    if (!_displayMissing) {
      if (winfsp) await _deviceAction(HostDeviceAction.installWinfsp);
      return;
    }
    final trust = _prerequisites?.certificateTrusted != true;
    final remote = _prerequisites?.remoteSession == true;
    final ask = hostFinishConfirmation(trust: trust, winfsp: winfsp);
    if (!await _confirm(ask.title, ask.message, accept: ask.accept)) return;
    ({bool ok, String message})? driver, drives;
    await _action(
      'installNativeDisplay',
      {'confirmed': true, if (trust) 'trustConfirmed': true},
      l10n.installingVirtualDisplayDriver,
      '',
      outcome: (reply) => driver = hostNativeDisplayInstallResult(
        reply,
        remoteSession: remote,
      ),
      quiet: true,
      phase: l10n.checkingSignatureInstallingDriverScreen,
    );
    if (!mounted) return;
    driver ??= (
      ok: false,
      message: _notice ?? l10n.couldntConfirmVirtualDisplayDriver,
    );
    if (_winfspMissing && !_busy) {
      await _action(
        'installWinfsp',
        {'confirmed': true},
        l10n.installingWinfsp,
        '',
        outcome: (reply) => drives = hostWinfspInstallResult(reply),
        quiet: true,
        phase: l10n.installingWinfspMayTake1,
      );
      if (!mounted) return;
      drives ??= (
        ok: false,
        message: _notice ?? l10n.couldNotCheckWinfspInstall,
      );
    }
    await _readPrerequisites();
    await _settleStatus();
    if (!mounted) return;
    final present = _state['devices'];
    final summary = hostFinishSummary(
      display: driver,
      winfsp: drives,
      winfspPresent:
          present is Map &&
          present['drives'] is Map &&
          (present['drives'] as Map)['winfsp'] == true,
    );
    setState(() {
      _notice = summary.message;
      _error = !summary.ok;
    });
    if (!_foreground) return;
    await showAppNotice(
      context,
      icon: summary.ok ? Icons.check_rounded : Icons.warning_amber_rounded,
      tone: summary.ok ? AppTone.success : AppTone.warn,
      title: summary.ok ? l10n.installationFinished : l10n.couldNotFinishInstallation,
      message: _forMachine(summary.message),
      dialogKey: const Key('host-result-dialog'),
      closeKey: const Key('host-result-close'),
    );
  }

  Future<bool> _confirm(
    String title,
    String detail, {
    String? accept,
    bool danger = false,
    bool whileReading = false,
  }) async {
    if (_mutating || (_busy && !whileReading)) return false;
    return showAppConfirm(
      context,
      icon: danger ? Icons.warning_amber_rounded : Icons.help_outline_rounded,
      tone: danger ? AppTone.danger : AppTone.accent,
      title: title,
      message: _forMachine(detail),
      confirmLabel: accept ?? l10n.continueLabel,
      dialogKey: const Key('host-confirm-dialog'),
      confirmKey: const Key('host-confirm'),
    );
  }

  Future<bool> _action(
    String action,
    Map<String, dynamic> fields,
    String title,
    String success, {
    ({bool ok, String message}) Function(Map<String, dynamic>)? outcome,
    bool notify = true,
    // The caller reports the result itself (the steps of saving the account).
    bool quiet = false,
    bool Function()? current,
    String? phase,
  }) async {
    final shownPhase = phase ?? l10n.requestSentWaitingWindowsRespond;
    if (_busy) return false;
    final owner = ++_operationGeneration;
    bool owns() =>
        mounted && owner == _operationGeneration && (current?.call() ?? true);
    _statusGeneration++;
    _approvalTimer?.cancel();
    setState(() {
      _busy = true;
      _notice = null;
      _error = false;
      _operation = title;
      _phase = shownPhase;
    });
    var accepted = false;
    try {
      _mutating = !{'testBroker', 'elevate'}.contains(action);
      if (_mutating && !await _syncClose()) {
        throw HostSettingsException(
          l10n.couldNotStartTaskOpen,
        );
      }
      if (!owns()) return false;
      final response = await _api.request(action, fields);
      if (!owns()) return false;
      accepted = true;
      final reported = outcome?.call(response);
      if (action != 'testBroker' && action != 'elevate') {
        if (mounted) {
          setState(() => _phase = l10n.checkingChangedStatus);
        }
        if (!await _load()) {
          throw HostSettingsException(l10n.statusCheckStoppedRefreshCurrent);
        }
      }
      if (owns()) {
        setState(() {
          _notice = reported?.message ?? success;
          _error = reported?.ok == false;
        });
      }
    } catch (error) {
      if (owns() &&
          !accepted &&
          (action == 'display' ||
              action == 'recoverDisplay' ||
              action == 'clearWindowsLoginV1')) {
        try {
          await _load();
        } catch (_) {
          // The original failure remains visible if diagnostics cannot refresh.
        }
      }
      if (owns()) {
        final message = _actionFailureMessage(action, error);
        setState(() {
          _error = true;
          if (accepted) _approvalReadError = true;
          _notice = accepted
              ? l10n.taskRespondedButChangedStatus
              : message;
        });
      }
    } finally {
      if (owner == _operationGeneration) _mutating = false;
      if (mounted && owner == _operationGeneration) {
        setState(() => _busy = false);
        unawaited(_syncClose());
        _scheduleApprovalPoll();
      }
    }
    if (!mounted || !owns()) return false;
    final ok = accepted && !_error;
    if (_foreground && action != 'testBroker' && !quiet && (notify || !ok)) {
      await showAppNotice(
        context,
        icon: _error ? Icons.warning_amber_rounded : Icons.check_rounded,
        tone: _error ? AppTone.warn : AppTone.success,
        title: _error ? l10n.checkResult : l10n.done2,
        message: _forMachine(_notice ?? l10n.refreshStatusCheck),
        dialogKey: const Key('host-result-dialog'),
        closeKey: const Key('host-result-close'),
      );
    }
    return mounted && owner == _operationGeneration && ok;
  }

  String _actionFailureMessage(String action, Object error) {
    final code = '$error';
    if (action == 'consoleTransfer') {
      return _consoleTransferHint(
        code.startsWith('console_transfer_')
            ? code.substring('console_transfer_'.length)
            : '',
      );
    }
    if (action == 'saveRdpCredentials' || action == 'clearRdpCredentials') {
      return switch (code) {
        'rdp_credentials_confirmation' => l10n.passwordsDoNotMatchEnter,
        'rdp_credentials_invalid_input' => l10n.checkFormatWindowsAccountPassword,
        'rdp_credentials_invalid' => l10n.checkWindowsAccountPassword,
        'rdp_account_restricted' =>
          l10n.windowsAccountLockedItsSign,
        'rdp_credentials_validation_failed' =>
          l10n.windowsCouldNotCheckAccount,
        'rdp_credentials_storage_failed' =>
          l10n.couldNotSaveDeleteWindows,
        _ => l10n.couldNotFinishSettingUp,
      };
    }
    if (action == 'display' || action == 'recoverDisplay') {
      return _displayFailure({'error': code}) ?? l10n.couldNotFinishSettingUp2;
    }
    if (action == 'repairDisplay') {
      return l10n.couldNotCheckVirtualDisplay;
    }
    return code;
  }

  void _hostOriginChanged() => setState(() {
    _transportRevision++;
    _tested = null;
    _originError = null;
    _httpConsentOrigin = null;
    final prefix = _origin.text.split('://');
    if (prefix.length > 1 &&
        const {'http', 'https'}.contains(prefix.first.toLowerCase())) {
      _brokerScheme = prefix.first.toLowerCase();
    }
  });

  Future<void> _test() async {
    if (_busy || _transportConfirming) return;
    final input = _origin.text;
    String address;
    try {
      address = parseServerOrigin(
        _origin.text,
        defaultScheme: _brokerScheme,
      ).toString();
    } on FormatException catch (error) {
      setState(() {
        _originError = error.message.toString();
        _tested = null;
      });
      _originFocus.requestFocus();
      return;
    }
    final parsed = Uri.parse(address), revision = _transportRevision;
    if (parsed.scheme == 'http' && _httpConsentOrigin != address) {
      if (!_admin) {
        setState(() => _originError = l10n.useHttpAddressOpenSettings);
        return;
      }
      if (_state['serverOrigin'] == address &&
          _state['httpConsentOrigin'] == address) {
        _httpConsentOrigin = address;
      } else {
        setState(() => _transportConfirming = true);
        bool accepted = false;
        try {
          accepted = await confirmInsecureBroker(context, parsed);
        } finally {
          if (mounted) setState(() => _transportConfirming = false);
        }
        if (!mounted ||
            !accepted ||
            revision != _transportRevision ||
            _origin.text != input ||
            !_admin) {
          return;
        }
        _httpConsentOrigin = address;
      }
    }
    setState(() {
      _tested = null;
      _originError = null;
      _origin.text = address;
      _brokerScheme = parsed.scheme;
    });
    final ok = await _action(
      'testBroker',
      {
        'origin': address,
        if (parsed.scheme == 'http') 'httpConsentOrigin': _httpConsentOrigin,
      },
      l10n.testingServerConnection,
      l10n.serverChecked,
    );
    if (mounted &&
        ok &&
        revision == _transportRevision &&
        _origin.text == address) {
      setState(() => _tested = address);
    }
  }

  Future<void> _requestConnection() async {
    if (!_admin ||
        _connectionWorkflow ||
        _tested == null ||
        _tested != _origin.text.trim()) {
      return;
    }
    final origin = _tested!;
    final revision = _transportRevision;
    if (Uri.parse(origin).scheme == 'http' && _httpConsentOrigin != origin) {
      return;
    }
    if (_registered &&
        !await _confirm(
          l10n.changeServerConnection,
          l10n.serverRequestOnceAdministratorApproves(origin),
          accept: l10n.requestChange,
        )) {
      return;
    }
    if (!mounted ||
        !_admin ||
        revision != _transportRevision ||
        _tested != origin ||
        _origin.text != origin) {
      return;
    }
    final ok = await _action(
      'requestConnection',
      {
        'origin': origin,
        if (Uri.parse(origin).scheme == 'http')
          'httpConsentOrigin': _httpConsentOrigin,
      },
      l10n.requestingServerConnection,
      l10n.connectionRequestSentApproveAdmin,
    );
    if (mounted && ok) {
      // The server page now shows the request with its code; the overview
      // says the same in one line.
      setState(() {
        _editRegistration = false;
        _tested = null;
        _notice = null;
      });
      _resetScroll();
      unawaited(_syncClose());
      _scheduleApprovalPoll();
    }
  }

  Future<void> _cancelConnection({bool edit = false}) async {
    if (!_admin || !_awaitingApproval || _busy) return;
    final request = _connectionRequest;
    final revision = request['cancelRevision'];
    if (request['cancelProtocol'] != 'cancel-request-v1' ||
        revision is! String ||
        !RegExp(r'^[a-f0-9]{64}$').hasMatch(revision)) {
      await _hostUpdateRequired(l10n.updateHostCancelRequestSafely);
      return;
    }
    if (!await _confirm(
      edit ? l10n.editServerConnect : l10n.cancelConnectionRequest,
      edit
          ? l10n.cancelsCurrentApprovalRequestSo
          : l10n.stopsWaitingAdministratorApprovalExisting,
      accept: edit ? l10n.cancelEdit : l10n.cancelRequest,
    )) {
      return;
    }
    if (!mounted ||
        !_admin ||
        !_awaitingApproval ||
        _busy ||
        _connectionRequest['cancelProtocol'] != 'cancel-request-v1' ||
        _connectionRequest['cancelRevision'] != revision) {
      if (mounted) {
        setState(() {
          _notice = l10n.connectionRequestChangedCheckCurrent;
          _error = true;
        });
      }
      return;
    }
    final ok = await _action(
      'cancelConnectionV1',
      {'protocol': 'cancel-request-v1', 'revision': revision},
      l10n.cancelingConnectionRequest,
      l10n.connectionRequestWasCanceledCheck,
    );
    if (mounted && ok && !_awaitingApproval && _requestStatus == 'cancelled') {
      setState(() {
        _tested = null;
        _editRegistration = edit;
        _page = edit ? 1 : 0;
        _notice = null;
      });
      _resetScroll();
      unawaited(_syncClose());
    }
  }

  Future<void> _hostUpdateRequired(String message) => showAppNotice(
    context,
    icon: Icons.info_outline_rounded,
    tone: AppTone.warn,
    title: l10n.hostUpdateNeeded,
    message: message,
    dialogKey: const Key('host-result-dialog'),
    closeKey: const Key('host-result-close'),
  );

  // A restart is one native-owned transaction. Closing this observer must
  // never abandon an accepted stop before its corresponding start.
  Future<void> _service() async {
    if (!_admin || _busy) return;
    final running = _section('service')['running'] == true;
    if (!running) {
      if (!await _confirm(
        l10n.startHostService,
        l10n.onceServiceStartsConnectsRegistered,
        accept: l10n.startService,
      )) {
        return;
      }
      if (!mounted ||
          !_admin ||
          _busy ||
          _section('service')['running'] != false) {
        return;
      }
      await _action(
        'service',
        {'command': 'start'},
        l10n.startingService,
        l10n.serviceHasStartedReconnectsServer,
      );
      return;
    }
    if (_section('service')['restartProtocol'] != 'service-restart-v1') {
      await _hostUpdateRequired(
        l10n.updateHostRestartSafelyCurrent,
      );
      return;
    }
    if (!await _confirm(
      l10n.restartHostService,
      l10n.activeConnectionsDropBrieflyYou,
      accept: l10n.restart,
    )) {
      return;
    }
    if (!mounted ||
        !_admin ||
        _busy ||
        _section('service')['running'] != true ||
        _section('service')['restartProtocol'] != 'service-restart-v1') {
      return;
    }
    await _action(
      'restartServiceV1',
      {'protocol': 'service-restart-v1', 'confirmed': true},
      l10n.restartingService,
      l10n.serviceHasRestartedReconnectsServer,
    );
  }

  Future<void> _display() async {
    final display = _section('display');
    if (display['provider'] == 'native') {
      await _refresh();
      return;
    }
    final recovery = display['recoveryRequired'] == true;
    if (display['rebootRequired'] == true ||
        (!recovery && _displayNeedsConsole(display))) {
      return;
    }
    if (!await _confirm(
      recovery ? l10n.restoreVirtualDisplay : l10n.setUpVirtualDisplay,
      recovery
          ? l10n.restoresInterruptedScreenChangePrevious
          : l10n.checksSignedDriverPreparesVirtual,
      accept: recovery ? l10n.restore : l10n.setUpLabel,
    )) {
      return;
    }
    await _action(
      recovery ? 'recoverDisplay' : 'display',
      {},
      recovery
          ? l10n.restoringVirtualDisplayShort
          : l10n.settingUpVirtualDisplayShort,
      l10n.virtualDisplayRequestDoneCheck,
    );
  }

  static Map<String, String> get _resetSteps => {
    'confirm': l10n.ok,
    'session': l10n.currentSign,
    'service': l10n.hostService,
    'sessions': l10n.activeConnections,
    'pending': l10n.screenTasksProgress,
    'sdk': l10n.earlierVirtualMonitor,
    'console': l10n.proxmoxConsoleScreen,
    'archive': l10n.logBackup,
    'reinitialize': l10n.virtualDisplayService,
  };
  static Map<String, String> get _resetCodes => {
    'confirmed': l10n.checked,
    'confirmation_required': l10n.needsChecking,
    'console': l10n.checkConsoleSign,
    'remote_session': l10n.cannotRunFromRdpScreen,
    'not_console': l10n.runFromSignProxmoxConsole,
    'locked': l10n.unlockWindows,
    'connected': l10n.connected,
    'service_unavailable': l10n.serviceNotRunning,
    'service_busy': l10n.anotherResetProgress,
    'protocol': l10n.couldNotCheckServiceResponse,
    'idle': l10n.noActiveConnections,
    'stream_active': l10n.streamingConnected,
    'rdp_session_active': l10n.windowsRdpSignActive,
    'console_operation': l10n.consoleSwitchProgress,
    'display_operation': l10n.changingNumberMonitors,
    'recovery_running': l10n.automaticRestoreProgress,
    'worker_running': l10n.waitingEarlierScreenTaskEnd,
    'none': l10n.none,
    'change_pending': l10n.resolutionMonitorChangeProgress,
    'controller_active': l10n.virtualMonitorUse,
    'reservation_held': l10n.streamingScreenTaskProgress,
    'absent': l10n.noneLeft,
    'present': l10n.stillPresentDriver,
    'transport_unavailable': l10n.couldNotConnectDriver,
    'identity_unavailable': l10n.couldNotReadVirtualDisplay,
    'not_selected': l10n.multitapVirtualDisplayDriverNot,
    'console_active': l10n.onLabel4,
    'console_reenabled': l10n.turnedAgain,
    'display_active': l10n.anotherScreenNoConsoleId,
    'no_active_display': l10n.noScreen,
    'owned_output_active': l10n.multitapVirtualMonitorStill,
    'reenable_rejected': l10n.windowsRefusedTurnConsoleScreen,
    'reenable_unverified': l10n.couldNotConfirmAfterTurning,
    'query_failed': l10n.couldNotReadScreenSetup,
    'ineligible': l10n.cannotCheckFromCurrentSign,
    'moved': l10n.movedBackupFolder,
    'nothing_to_move': l10n.noLogsCleanUp,
    'backup_failed': l10n.couldNotCreateBackupFolder,
    'move_partial': l10n.onlyPartlyMoved,
    'reinitialized': l10n.preparedAgain,
    'failed': l10n.failed,
  };

  ({bool ok, String message}) _resetOutcome(Map<String, dynamic> response) {
    final reset = response['reset'];
    if (reset is! Map) {
      return (ok: false, message: l10n.couldNotCheckResetResult);
    }
    final lines = <String>[];
    final steps = reset['steps'];
    if (steps is List) {
      for (final step in steps.take(12)) {
        if (step is! Map) continue;
        final name = _resetSteps['${step['step']}'] ?? '${step['step']}';
        final code = _resetCodes['${step['code']}'] ?? '${step['code']}';
        lines.add('${step['ok'] == true ? '✓' : '✗'} $name: $code');
      }
    }
    final ok = reset['ok'] == true;
    final moved = reset['moved'];
    final head = ok
        ? l10n.backedUpRecoveryLogsPrepared(moved is int ? l10n.text18(moved) : l10n.text19)
        : l10n.nothingWasResetLogsScreens;
    return (ok: ok, message: [head, '', ...lines].join('\n'));
  }

  Future<void> _resetNativeRecovery() async {
    final display = _section('display');
    if (_busy || !_admin || display['provider'] != 'native') return;
    if (!await _confirm(
      l10n.resetVirtualDisplayRecoveryLogs,
      l10n.movesLogsEarlierStreamingScreens,
      accept: l10n.resetLogs,
      danger: true,
    )) {
      return;
    }
    await _action(
      'resetNativeRecovery',
      {'confirmed': true},
      l10n.resettingVirtualDisplayRecoveryLogs,
      l10n.virtualDisplayRecoveryLogsWere,
      outcome: _resetOutcome,
    );
  }

  String _consoleTransferHint(String reason) => switch (reason) {
    'restart_required' => l10n.saveWorkRestartWindowsThen,
    'console_in_use' => l10n.anotherUserSignedConsoleTheir,
    'session_changed' => l10n.signStatusChangedRefreshStatus,
    'administrator_required' => l10n.openHostSettingsAsAdministrator,
    'already_console' => l10n.currentSignAlreadyConnectedStreaming,
    'virtual_device_not_ready' => l10n.virtualDisplayDeviceNotReady,
    'rejected' => l10n.windowsDidNotAllowSign,
    _ => l10n.couldNotCheckCurrentSign,
  };

  Future<void> _consoleTransfer() async {
    final plan = _section('consoleTransfer');
    if (_busy ||
        !_admin ||
        plan['eligible'] != true ||
        plan['sourceSessionId'] is! int ||
        plan['consoleSessionId'] is! int) {
      return;
    }
    final fields = {
      'sourceSessionId': plan['sourceSessionId'],
      'consoleSessionId': plan['consoleSessionId'],
    };
    if (!await _confirm(
      l10n.switchStreamingScreen,
      l10n.movesCurrentWindowsSignStreaming,
      accept: l10n.switchScreen,
    )) {
      return;
    }
    await _action(
      'consoleTransfer',
      fields,
      l10n.switchingWindowsSign,
      l10n.switchRequestedCheckWhetherVirtual,
      outcome: (response) {
        final result = response['transfer'];
        final complete =
            result is Map &&
            result['requested'] == true &&
            result['completed'] == true;
        return (
          ok: complete,
          message: complete
              ? l10n.windowsSignSwitchConfirmedOnce
              : l10n.switchRequestWasReceivedBut,
        );
      },
    );
  }

  bool _validDisplayRepairPlan(Map<String, dynamic> plan) {
    if (!RegExp(r'^[a-fA-F0-9]{64}$').hasMatch('${plan['planId'] ?? ''}')) {
      return false;
    }
    final items = plan['items'];
    if (items is! List || items.length > 128) return false;
    for (final item in items) {
      if (item is! Map ||
          item['required'] != true ||
          !{'device', 'configuration'}.contains(item['kind'])) {
        return false;
      }
      for (final field in ['id', 'name', 'impact']) {
        if (item[field] is! String ||
            (item[field] as String).isEmpty ||
            (item[field] as String).length > 2048) {
          return false;
        }
      }
      if (item['kind'] == 'device' &&
          ('${item['hardwareId'] ?? ''}'.toLowerCase() != r'root\mttvdd' ||
              item['instanceId'] is! String ||
              (item['instanceId'] as String).isEmpty)) {
        return false;
      }
      if (item['kind'] == 'configuration' &&
          (item['registryPath'] is! String ||
              (item['registryPath'] as String).isEmpty)) {
        return false;
      }
    }
    return plan['canRepair'] is bool &&
        plan['repairNeeded'] is bool &&
        plan['driverStoreRetained'] == true &&
        plan['configurationFilesRetained'] == true;
  }

  Future<void> _readDisplayRepairPlan() async {
    if (_section('display')['provider'] == 'native') {
      await _refresh();
      return;
    }
    if (_busy || !_admin) return;
    _statusGeneration++;
    _approvalTimer?.cancel();
    setState(() {
      _busy = true;
      _displayRepairPlan = null;
      _notice = null;
      _error = false;
      _operation = l10n.checkingVirtualDisplayConflicts;
      _phase = l10n.lookingUpExistingDevicesLeftover;
    });
    try {
      final response = await _api.request('displayRepairPlan');
      final value = response['plan'];
      if (value is! Map ||
          !_validDisplayRepairPlan(Map<String, dynamic>.from(value))) {
        throw const HostSettingsException('invalid_repair_plan');
      }
      if (mounted) {
        setState(() => _displayRepairPlan = Map<String, dynamic>.from(value));
        WidgetsBinding.instance.addPostFrameCallback((_) {
          final anchor = _repairPlanAnchor.currentContext;
          if (mounted && anchor != null) {
            unawaited(
              Scrollable.ensureVisible(
                anchor,
                duration: const Duration(milliseconds: 150),
              ),
            );
          }
        });
      }
    } catch (_) {
      if (mounted) {
        setState(() {
          _notice = l10n.couldNotCheckConflictsRefresh;
          _error = true;
        });
      }
    } finally {
      if (mounted) {
        setState(() => _busy = false);
        _scheduleApprovalPoll();
      }
    }
  }

  Future<void> _repairDisplay() async {
    if (_section('display')['provider'] == 'native') {
      await _refresh();
      return;
    }
    final plan = _displayRepairPlan;
    if (_busy ||
        !_admin ||
        plan == null ||
        !_validDisplayRepairPlan(plan) ||
        plan['canRepair'] != true ||
        plan['repairNeeded'] != true ||
        (plan['items'] as List).isEmpty) {
      return;
    }
    final items = (plan['items'] as List).cast<Map>();
    final description = items
        .map((item) {
          final kind = item['kind'] == 'device' ? l10n.devices : l10n.leftoverSetting;
          final location = item['kind'] == 'device'
              ? item['instanceId']
              : '${item['registryPath']} · ${item['registryValue'] ?? ''}';
          return l10n.impact(kind, item['name'], location, item['impact']);
        })
        .join('\n\n');
    if (!await _confirm(
      l10n.cleanUpCheckedConflictsInstall,
      l10n.allRequiredItemsShownCleaned(description),
      accept: l10n.cleanUpInstall,
      danger: true,
    )) {
      return;
    }
    final planId = plan['planId'] as String;
    setState(() {
      _displayRepairPlan = null;
      _displayRepairResult = null;
      _repairTimedOut = false;
      _repairExitCode = null;
    });
    await _action(
      'repairDisplay',
      {'planId': planId},
      l10n.cleaningUpConflictsInstallingMultitap,
      '',
      outcome: (response) {
        final value = response['result'];
        if (value is! Map) {
          throw const HostSettingsException('invalid_repair_result');
        }
        final result = <String, dynamic>{};
        for (final field in [
          'repairInstalled',
          'cleanupCompleted',
          'configurationRestored',
          'configurationRestoreAttempted',
          'partialRemoval',
          'rebootRequired',
          'configurationPending',
          'removalUncertain',
        ]) {
          if (value[field] is bool) result[field] = value[field];
        }
        for (final field in ['removedDeviceCount', 'removeRequestedCount']) {
          final count = value[field];
          if (count is int && count >= 0 && count <= 1024) {
            result[field] = count;
          }
        }
        final reason = value['reason'];
        result['reason'] =
            reason is String && RegExp(r'^[a-z0-9_]{1,96}$').hasMatch(reason)
            ? reason
            : 'repair_result_requires_review';
        final timedOut = response['timedOut'] == true;
        final exitCode = response['helperExitCode'] is int
            ? response['helperExitCode'] as int
            : null;
        if (mounted) {
          setState(() {
            _displayRepairResult = result;
            _repairTimedOut = timedOut;
            _repairExitCode = exitCode;
          });
        }
        final ok =
            !timedOut &&
            exitCode == 0 &&
            result['repairInstalled'] == true &&
            result['cleanupCompleted'] == true &&
            result['partialRemoval'] != true &&
            result['removalUncertain'] != true;
        return (ok: ok, message: _displayRepairOutcome(result, timedOut, ok));
      },
    );
  }

  String _displayRepairOutcome(
    Map<String, dynamic> result,
    bool timedOut,
    bool ok,
  ) {
    if (timedOut) return l10n.operationTimedOutCheckActual;
    if (result['reason'] == 'repair_plan_changed') {
      return l10n.devicesSettingsChangedSoCleanup;
    }
    if (result['removalUncertain'] == true) {
      return l10n.deviceRemovalRequestWasSent;
    }
    if (result['partialRemoval'] == true) {
      return l10n.onlySomeItemsWereCleaned;
    }
    if (!ok && result['cleanupCompleted'] == true) {
      return l10n.conflictsWereCleanedUpBut;
    }
    if (!ok) return l10n.cleanupInstallDidNotFinish;
    if (result['rebootRequired'] == true) {
      return l10n.driverInstalledSaveWorkRestart;
    }
    if (result['configurationPending'] == true) {
      return l10n.driverInstalledSignWindowsFrom;
    }
    return l10n.driverInstalledCheckNewVirtual;
  }

  Future<void> _saveAccount() async {
    if (_busy || !_admin) return;
    final setup = _passwordSetup;
    final username = setup?.username ?? _accountUsername.text.trim();
    // A blank password: does this local account have none? Then the form
    // offers making one instead of Windows Settings.
    if (setup == null &&
        username.isNotEmpty &&
        _accountPassword.text.isEmpty &&
        _accountPasswordConfirmation.text.isEmpty) {
      await _checkPasswordless(username);
      return;
    }
    final minimum = setup?.minimumLength ?? 0;
    setState(() {
      _accountUsernameError = username.isEmpty ? l10n.enterWindowsAccount : null;
      _accountPasswordError = _accountPassword.text.isEmpty
          ? setup != null
                ? l10n.enterNewPassword
                : l10n.enterWindowsPassword
          : _accountPassword.text.length > 256
          ? l10n.passwordTooLong
          : _accountPassword.text.length < minimum
          ? l10n.useLeastCharacters(minimum)
          : null;
      _accountConfirmationError =
          _accountPassword.text != _accountPasswordConfirmation.text
          ? l10n.passwordsDoNotMatch
          : null;
    });
    if (_accountUsernameError != null ||
        _accountPasswordError != null ||
        _accountConfirmationError != null) {
      return;
    }
    // Saving the account also turns automatic login on:
    // one confirmation names both consequences, then each step reports its
    // own observed result. An existing foreign setting is taken over only
    // where this Host can; anything else saves the account alone and says why.
    final login = _windowsLogin;
    final takeover = login.state == 'external' && login.canTakeOwnershipApply;
    final enable = takeover || login.canApply;
    final confirmed = await _confirm(
      setup != null
          ? l10n.createNewPassword
          : enable
          ? l10n.saveWindowsAccountTurnAuto
          : l10n.saveWindowsAccount,
      [
        l10n.windowsAccount2(username),
        '',
        if (setup != null) ...[
          l10n.accountGetsWindowsPasswordNeeded,
          enable ? l10n.thenAccountSavedAutoSign : _autologonUnavailable(login),
        ] else ...[
          l10n.streamingConnectsWindowsSignedAccount,
          enable
              ? l10n.whenWindowsStartsSignsAccount
              : _autologonUnavailable(login),
        ],
        if (takeover) l10n.hostTakesOverExistingAuto,
        l10n.passwordStoredOnlyHostsProtected,
        if (setup == null) l10n.windowsAccountPasswordsSecurityPolicies,
      ].join('\n'),
      accept: setup != null
          ? l10n.createPassword
          : enable
          ? l10n.saveTurn
          : l10n.save,
    );
    final password = _accountPassword.text,
        confirmation = _accountPasswordConfirmation.text;
    _accountPassword.clear();
    _accountPasswordConfirmation.clear();
    if (!confirmed || !mounted) return;
    // Step 1 (only for an account without a password): make it. Every step
    // below reports its own line; a step that did not finish stops the rest.
    final made = l10n.windowsPasswordCreatedNeededFrom;
    if (setup != null) {
      final code = await _makePassword(setup, password, confirmation);
      if (!mounted) return;
      if (code != '') {
        // No answer is not proof either way: the next blank save checks again.
        if (code == null ||
            const {
              'password_exists',
              'account_changed',
              'account_invalid',
              'local_account_required',
            }.contains(code)) {
          setState(() => _passwordSetup = null);
        }
        await _reportAccountSteps([
          switch (code) {
            null => l10n.windowsPasswordResultNotConfirmed,
            _notSent => l10n.windowsPasswordNotCreatedReopen,
            final String refused =>
              l10n.windowsPasswordNotCreated(_passwordReason(refused)).trim(),
          },
          l10n.windowsAccountNotSaved,
          l10n.autoSignNotChanged,
        ], ok: false);
        return;
      }
      setState(() => _passwordSetup = null);
    }
    final saved = await _action(
      'saveRdpCredentials',
      {
        'username': username,
        'password': password,
        'passwordConfirmation': confirmation,
      },
      l10n.checkingSavingWindowsAccount,
      l10n.windowsAccountCheckedSaved,
      notify: false,
      quiet: setup != null,
    );
    if (!mounted) return;
    if (!saved) {
      if (setup != null) {
        await _reportAccountSteps([
          made,
          l10n.windowsAccountNotSaved2(_notice ?? '').trim(),
          l10n.autoSignNotChanged,
        ], ok: false);
      }
      return;
    }
    setState(() {
      _editAccount = false;
      _accountUsername.text = '${_section('windowsAccount')['username'] ?? ''}';
    });
    unawaited(_syncClose());
    var autologon = _autologonUnavailable(_windowsLogin), turnedOn = false;
    if (enable && _windowsLogin.revision != login.revision) {
      autologon = l10n.settingsChangedDuringCheckCheck;
    } else if (enable) {
      setState(() => _notice = null);
      await _windowsChange(
        'apply',
        takeOwnership: takeover,
        credentials: (
          username: username,
          password: password,
          confirmation: confirmation,
        ),
      );
      if (!mounted) return;
      autologon = _notice ?? l10n.notChangedRefreshStatusCheck;
      turnedOn = _notice != null && !_error;
    }
    await _reportAccountSteps([
      if (setup != null) made,
      l10n.windowsAccountSaved,
      l10n.autoSign(autologon),
    ], ok: turnedOn);
  }

  static const _notSent = 'not_sent';

  /// setWindowsAccountPassword behind the close guard: '' made, a fixed code
  /// refused, [_notSent] when nothing was sent, null when sent but unanswered.
  Future<String?> _makePassword(
    ({String username, String sid, int minimumLength}) setup,
    String password,
    String confirmation,
  ) async {
    _statusGeneration++;
    _approvalTimer?.cancel();
    setState(() {
      _busy = true;
      _mutating = true;
      _notice = null;
      _error = false;
      _operation = l10n.creatingWindowsPassword;
      _phase = l10n.requestSentWaitingWindowsRespond;
    });
    String? code = _notSent;
    try {
      if (!await _syncClose()) return code;
      code = null;
      final result = (await _api.request('setWindowsAccountPassword', {
        'username': setup.username,
        'sid': setup.sid,
        'password': password,
        'passwordConfirmation': confirmation,
        'confirmed': true,
      }))['password'];
      code = result is Map && result['error'] is String
          ? result['error'] as String
          : null;
      try {
        await _load();
      } catch (_) {
        // The answer stands; the page shows the next status read.
      }
    } catch (_) {
      // Sent without a readable answer: the password may exist now or not.
    } finally {
      _mutating = false;
      if (mounted) {
        setState(() => _busy = false);
        unawaited(_syncClose());
        _scheduleApprovalPoll();
      }
    }
    return code;
  }

  /// One line per step of saving the account; anything unfinished warns.
  Future<void> _reportAccountSteps(
    List<String> lines, {
    required bool ok,
  }) async {
    final message = lines.join('\n');
    setState(() {
      _notice = message;
      _error = !ok;
    });
    await showAppNotice(
      context,
      icon: ok ? Icons.check_rounded : Icons.warning_amber_rounded,
      tone: ok ? AppTone.success : AppTone.warn,
      title: ok ? l10n.done2 : l10n.checkResult,
      message: message,
      dialogKey: const Key('host-result-dialog'),
      closeKey: const Key('host-result-close'),
    );
  }

  /// windowsAccountPasswordState: an empty password opens 새 비밀번호 만들기
  /// for exactly this account; anything else asks for the password as before.
  Future<void> _checkPasswordless(String username) async {
    _approvalTimer?.cancel();
    setState(() {
      _busy = true;
      _operation = l10n.checkingWindowsAccount;
      _phase = l10n.checkingWhetherAccountHasPassword;
    });
    ({String username, String sid, int minimumLength})? setup;
    String? usernameError, passwordError = l10n.enterWindowsPassword;
    try {
      final account = (await _api.request('windowsAccountPasswordState', {
        'username': username,
      }))['account'];
      if (account is Map) {
        final error = account['error'], sid = account['sid'];
        final minimum = account['minimumLength'];
        if (error == '' &&
            account['state'] == 'empty' &&
            sid is String &&
            RegExp(r'^S-1-5-21(-[0-9]{1,10}){4}$').hasMatch(sid)) {
          setup = (
            username: username,
            sid: sid,
            minimumLength: minimum is int && minimum >= 0 && minimum <= 256
                ? minimum
                : 0,
          );
          passwordError = null;
        } else if (error == '' && account['state'] == 'unknown') {
          passwordError = l10n.couldNotConfirmAccountHas;
        } else if (error is String && _passwordReason(error).isNotEmpty) {
          usernameError = _passwordReason(error);
          passwordError = null;
        }
      }
    } catch (_) {
      // An older Host or a failed check: the password is asked for as before.
    } finally {
      if (mounted) {
        setState(() {
          _busy = false;
          _passwordSetup = setup;
          _accountUsernameError = usernameError;
          _accountPasswordError = passwordError;
          _accountConfirmationError = null;
        });
        _scheduleApprovalPoll();
      }
    }
  }

  /// Fixed codes of windowsAccountPasswordState / setWindowsAccountPassword.
  String _passwordReason(String code) => switch (code) {
    'password_exists' => l10n.accountAlreadyHasPasswordEnter,
    'password_policy' => l10n.doesNotMeetWindowsPassword,
    'password_state_unknown' => l10n.couldNotConfirmAccountHas2,
    'password_set_denied' => l10n.windowsRefusedChange,
    'password_confirmation' => l10n.passwordsDoNotMatch,
    'password_invalid_input' => l10n.checkPasswordFormat,
    'local_account_required' => l10n.notLocalAccountWindowsEnter,
    'account_invalid' => l10n.accountDoesNotExistWindows,
    'account_restricted' => l10n.accountBlockedCheckAccountStatus,
    'account_changed' => l10n.accountChangedDuringCheckSave,
    'account_unavailable' => l10n.couldNotCheckAccountTry,
    _ => '',
  };

  /// Why saving the account cannot turn automatic login on right now.
  String _autologonUnavailable(WindowsAutologonStatus login) => !login.supported
      ? l10n.hostDoesNotSupportAuto
      : !login.administrator
      ? l10n.noAdministratorRightsSoAuto
      : login.reason == 'policy_restricted'
      ? l10n.windowsPolicyPreventsAutoSign
      : login.state == 'external'
      ? l10n.autoSignSettingAlreadyExists
      : login.canRepair
      ? l10n.previousChangeMustRecoveredFirst
      : l10n.autoSignCannotChangedRight;

  Future<void> _clearAccount() async {
    if (_busy || !_admin || _closeDialog) return;
    final saved = SavedWindowsLoginStatus.parse(_section('windowsAccount'));
    if (!saved.configured) return;
    if (!saved.canClear) {
      await showAppNotice(
        context,
        title: l10n.hostUpdateNeeded,
        message: l10n.updateHostClearSavedInfo2,
      );
      return;
    }
    final consent = ++_clearAccountConsentGeneration;
    final operation = _operationGeneration;
    bool current() =>
        mounted && !_closeDialog && consent == _clearAccountConsentGeneration;
    if (!await _confirm(
      l10n.clearWindowsSignInfoSaved,
      l10n.onlyHostsSavedInfoCleared(saved.username),
      accept: l10n.clearSavedInfo,
      danger: true,
    )) {
      return;
    }
    if (!current() || operation != _operationGeneration || _busy || !_admin) {
      return;
    }
    final now = SavedWindowsLoginStatus.parse(_section('windowsAccount'));
    if (!now.canClear ||
        now.revision != saved.revision ||
        now.username != saved.username) {
      try {
        await _load();
      } catch (_) {}
      if (!mounted || !current()) return;
      await showAppNotice(
        context,
        title: l10n.savedInfoChanged,
        message: l10n.checkNewStatusThenChoose,
      );
      return;
    }
    final ok = await _action(
      'clearWindowsLoginV1',
      {
        'protocol': SavedWindowsLoginStatus.protocol,
        'revision': saved.revision,
      },
      l10n.clearingSignInfoSavedHost,
      l10n.windowsSignInfoSavedHost,
      current: current,
      outcome: (response) => windowsLoginRemovalObserved(response)
          ? (ok: true, message: l10n.windowsSignInfoSavedHost)
          : (ok: false, message: l10n.couldntConfirmDeletionDontRun),
    );
    if (current() && ok) {
      setState(() {
        _editAccount = false;
        _accountUsername.text =
            '${_section('windowsAccount')['username'] ?? ''}';
        _accountPassword.clear();
        _accountPasswordConfirmation.clear();
      });
      unawaited(_syncClose());
    }
  }

  String _displayHint(Map<String, dynamic> d) {
    if (d.isEmpty) return l10n.checkingVirtualDisplayStatus;
    if (_displayMissing) return l10n.noVirtualDisplayDriverYet;
    if (d['provider'] == 'native' && d['rebootRequired'] == true) {
      return l10n.saveWorkRestartWindowsWont;
    }
    if (d['provider'] == 'native') {
      if (d['recoveryRequired'] == true) {
        return l10n.screenRecoveryFromLastStreaming;
      }
      if (d['ready'] == true) {
        return l10n.multitapVirtualDisplayAvailableCurrent;
      }
      if (d['remoteSession'] == true &&
          d['installed'] == true &&
          d['deviceReady'] == true) {
        final transfer = _section('consoleTransfer');
        return transfer['eligible'] == true
            ? l10n.youSignedViaRdpSwitch
            : _consoleTransferHint('${transfer['reason'] ?? ''}');
      }
      if (d['installed'] == true &&
          d['deviceReady'] == true &&
          {
            '',
            'native_display_not_ready',
            'native_display_change_pending',
            'native_display_activation_pending',
          }.contains(d['error'] ?? '')) {
        return l10n.servicePreparingVirtualDisplayPlease;
      }
    }
    if (d['recoveryRequired'] == true) {
      return l10n.previousScreenChangeWasInterrupted;
    }
    if (d['rebootRequired'] == true) {
      return l10n.saveWorkRestartWindowsWont;
    }
    if (d['ready'] == true) return l10n.multitapVirtualDisplayAvailableCurrent;
    if ((d['foreignDeviceCount'] as num? ?? 0) > 0) {
      return l10n.anotherVirtualDisplayDevicePresent;
    }
    if (_displayFailure(d) != null) return _displayFailure(d)!;
    if (d['remoteSession'] == true || d['configurationPending'] == true) {
      return l10n.signWindowsFromProxmoxConsole;
    }
    if (d['installed'] != true) return l10n.runVirtualDisplaySetupPrepare;
    if (d['deviceReady'] != true) {
      return l10n.virtualDisplayDeviceDidNot;
    }
    return l10n.noVirtualDisplayRunVirtual;
  }

  bool _displayNeedsConsole(Map<String, dynamic> display) =>
      display['installed'] == true &&
      display['deviceReady'] == true &&
      display['owned'] == true &&
      display['remoteSession'] == true &&
      display['configurationPending'] == true;

  String? _displayFailure(Map<String, dynamic> display) =>
      switch (display['error']) {
        null || '' => null,
        'administrator_required' => l10n.virtualDisplaySetupNeedsAdministrator,
        'signed_driver_install_failed' || 'virtual_device_driver_bind_failed' =>
          l10n.windowsCouldNotFinishPreparing,
        'existing_virtual_display_configuration' =>
          l10n.existingVirtualDisplaySetupNeeds,
        'existing_virtual_display_requires_review' ||
        'stale_virtual_display_ownership_requires_review' ||
        'owned_virtual_display_identity_mismatch' ||
        'monitor_ownership_changed' =>
          l10n.virtualDisplayDeviceOwnershipDuplicate,
        'driver_hash_mismatch' ||
        'driver_signature_untrusted' ||
        'driver_catalog_membership_invalid' =>
          l10n.couldNotVerifyDriverFiles,
        'driver_file_unavailable' || 'driver_package_lock_failed' =>
          l10n.couldNotVerifyVirtualDisplay,
        'registry_write_denied' ||
        'registry_write_failed' ||
        'owned_device_naming_failed' =>
          l10n.couldNotSaveVirtualDisplay,
        'viewport_recovery_required' =>
          l10n.previousScreenChangeNeedsRecovery,
        'native_display_legacy_operation_blocked' =>
          l10n.hostServiceManagesVirtualDisplay,
        'native_display_snapshot_invalid' =>
          l10n.couldNotReadVirtualDisplay2,
        'native_display_topology_mismatch' =>
          l10n.couldNotCheckCurrentDisplay,
        'native_display_configuration_untrusted' ||
        'native_display_provider_unavailable' =>
          l10n.couldNotVerifyVirtualDisplay,
        'interactive_console_required' =>
          l10n.youSignedViaRdpSwitch2,
        _ => l10n.couldNotCheckVirtualDisplay2,
      };

  @override
  void dispose() {
    _disposing = true;
    _clearAccountConsentGeneration++;
    WidgetsBinding.instance.removeObserver(this);
    _statusGeneration++;
    _approvalTimer?.cancel();
    _statusAge.stop();
    _closeGuard.dispose();
    _origin.dispose();
    _accountUsername.dispose();
    _accountPassword.dispose();
    _accountPasswordConfirmation.dispose();
    _originFocus.dispose();
    _scroll.dispose();
    super.dispose();
  }

  // The virtual screen itself is ready, whoever is or is not signed in: the
  // 가상 화면 area answers for this, the Windows 로그인 area for the session.
  bool get _displayOutputReady {
    final display = _section('display');
    final monitors = display['monitors'];
    return display['ready'] == true &&
        display['installed'] == true &&
        display['deviceReady'] == true &&
        display['owned'] == true &&
        display['foreignDeviceCount'] == 0 &&
        monitors is List &&
        monitors.isNotEmpty &&
        monitors.length <= 2 &&
        display['configurationPending'] != true &&
        display['rebootRequired'] != true &&
        display['recoveryRequired'] != true;
  }

  // A stream can use it now: the screen is ready in an unlocked session.
  bool get _virtualDisplayReady =>
      _displayOutputReady && _state['interactiveSession'] == true;

  List<HostSetupState> get _setupStates {
    if (_state.isEmpty) return List.filled(7, HostSetupState.checking);
    final service = _section('service'), display = _section('display');
    final server = _cancellingConnection
        ? HostSetupState.checking
        : _awaitingApproval
        ? HostSetupState.approval
        : !_registered
        ? HostSetupState.setup
        : _brokerOnline
        ? HostSetupState.complete
        : HostSetupState.attention;
    final screen = _displayOutputReady
        ? HostSetupState.complete
        : display['provider'] == 'native' &&
              _section('consoleTransfer')['eligible'] == true
        ? HostSetupState.setup
        : display['installed'] == false &&
              display['error'] != null &&
              display['error'] != ''
        ? HostSetupState.attention
        : display['installed'] == false
        ? HostSetupState.setup
        : HostSetupState.attention;
    final operational =
        server == HostSetupState.complete &&
        _virtualDisplayReady &&
        service['running'] == true;
    final states = <HostSetupState>[
      operational
          ? HostSetupState.complete
          : !_registered
          ? HostSetupState.setup
          : HostSetupState.attention,
      server,
      screen,
      HostSetupState.unavailable, // 3: unused (never shown)
      _approvalReadError || _error || service['running'] is! bool
          ? HostSetupState.attention
          : HostSetupState.available,
      // A Windows nobody is signed in to (or a locked one) cannot be
      // streamed, whatever the automatic login says.
      _state['interactiveSession'] == true
          ? _loginState
          : HostSetupState.attention,
      // Optional features: never part of the operational state above.
      _devicesState,
    ];
    if (_mutating) states[_page] = HostSetupState.checking;
    return states;
  }

  // Complete only when automatic login is on for the saved account. Off is a
  // setup step, never a resting choice.
  HostSetupState get _loginState => !_windowsLogin.supported
      ? HostSetupState.unavailable
      : const {'configured', 'observed'}.contains(_windowsLogin.state)
      ? _windowsAccountMatches
            ? HostSetupState.complete
            : _section('windowsAccount')['configured'] == true
            ? HostSetupState.attention
            : HostSetupState.setup
      : _windowsLogin.state == 'off'
      ? HostSetupState.setup
      : HostSetupState.attention;

  List<HostDeviceRow> get _deviceRows => hostDeviceRows(_state['devices']);

  HostSetupState get _devicesState {
    final rows = _deviceRows;
    bool any(HostDeviceLevel level) => rows.any((row) => row.level == level);
    return rows.isEmpty ||
            any(HostDeviceLevel.attention) ||
            any(HostDeviceLevel.unknown)
        ? HostSetupState.attention
        : any(HostDeviceLevel.setup)
        ? HostSetupState.setup
        : HostSetupState.complete;
  }

  // Every page but the overview opens with its title, and with ONE status
  // only when the area needs the user (2026-11-11: a "● 정상" on every page
  // said nothing).
  Widget _sectionHeading() {
    final state = _setupStates[_page];
    final label = state.needsUser
        ? HostSettingsNavigation.stateLabel(_page, state)
        : null;
    return Padding(
      padding: const EdgeInsets.only(bottom: 20),
      child: Wrap(
        spacing: 12,
        runSpacing: 8,
        crossAxisAlignment: WrapCrossAlignment.center,
        children: [
          Semantics(
            header: true,
            child: KeepAllText(
              HostSettingsNavigation.titles[_page],
              key: const Key('host-selected-section'),
              style: TextStyle(
                fontSize: 20,
                fontWeight: FontWeight.w600,
                letterSpacing: -.2,
                height: 1.3,
                color: _palette.ink,
              ),
            ),
          ),
          if (label != null)
            HostStatusPill(
              key: const Key('host-selected-status'),
              state: HostRouteState.warn,
              label: label,
            ),
        ],
      ),
    );
  }

  Future<void> _openInformation() async {
    if (_infoOpen) return;
    _infoOpen = true;
    try {
      await showAppInfoDialog(
        context,
        title: l10n.hostInfo,
        productName: 'Multitap Host',
        platform: 'Windows Host',
        versionLabel: l10n.hostToolsVersion,
        manifestAsset: 'assets/licenses/host_native_notices.json',
        // This is the existing recent HostTool status version, never the
        // client UI's EXE version or a manually maintained fallback string.
        loadVersion: () async => ClientBuildVersion.parse(
          _statusAge.isRunning &&
                  _statusAge.elapsed < const Duration(seconds: 30) &&
                  !_busy &&
                  !_error &&
                  !_approvalReadError
              ? _state['version']
              : null,
        ),
      );
    } finally {
      _infoOpen = false;
    }
  }

  // The header of the approved design: the mark, the product and its
  // version, then quiet icons with tooltips. A newer version the server's
  // catalog names gets the one amber button; until then the manual check is
  // one of the quiet icons.
  Widget _header({required bool compact}) {
    final quiet = HeaderControl.quiet(_palette);
    final version = _state['version'];
    final newer = _updateVersion;
    final updateHint = _canCheckUpdates
        ? l10n.checkUpdates
        : _busy
        ? l10n.availableWhenOperationFinishes
        : l10n.availableAfterRegisteringServer;
    final Widget update;
    if (newer == null) {
      update = IconButton(
        key: const Key('host-header-updates'),
        tooltip: updateHint,
        style: quiet,
        onPressed: _canCheckUpdates ? _checkUpdates : null,
        icon: const Icon(Icons.download_rounded),
      );
    } else if (compact) {
      update = IconButton.filled(
        key: const Key('host-header-updates'),
        tooltip: l10n.update2(withRo(newer)),
        style: IconButton.styleFrom(
          backgroundColor: _palette.amber,
          foregroundColor: _palette.onAmber,
          minimumSize: const Size.square(HeaderControl.height),
          maximumSize: const Size.square(HeaderControl.height),
          padding: EdgeInsets.zero,
          shape: RoundedRectangleBorder(borderRadius: BorderRadius.circular(8)),
        ),
        onPressed: _canCheckUpdates ? _checkUpdates : null,
        icon: const Icon(Icons.download_rounded, size: 18),
      );
    } else {
      update = FilledButton.icon(
        key: const Key('host-header-updates'),
        style: FilledButton.styleFrom(
          minimumSize: const Size(0, HeaderControl.height),
          padding: const EdgeInsets.fromLTRB(10, 0, 12, 0),
          textStyle: uiText(size: 13, weight: FontWeight.w600),
          shape: RoundedRectangleBorder(borderRadius: BorderRadius.circular(8)),
        ),
        onPressed: _canCheckUpdates ? _checkUpdates : null,
        icon: const Icon(Icons.download_rounded, size: 16),
        label: KeepAllText(l10n.update2(withRo(newer)), maxLines: 1),
      );
    }
    return Material(
      color: _palette.surface,
      child: ConstrainedBox(
        constraints: const BoxConstraints(minHeight: 56),
        child: Padding(
          padding: const EdgeInsets.fromLTRB(20, 8, 12, 8),
          child: Row(
            children: [
              const ProductMark(size: 28),
              const SizedBox(width: 12),
              Expanded(
                child: Row(
                  crossAxisAlignment: CrossAxisAlignment.baseline,
                  textBaseline: TextBaseline.alphabetic,
                  children: [
                    Flexible(
                      child: KeepAllText(
                        'Multitap Host',
                        maxLines: 1,
                        overflow: TextOverflow.ellipsis,
                        style: TextStyle(
                          fontSize: 16,
                          fontWeight: FontWeight.w600,
                          color: _palette.ink,
                        ),
                      ),
                    ),
                    if (version is String && version.isNotEmpty) ...[
                      const SizedBox(width: 8),
                      Text(
                        version,
                        key: const Key('host-version'),
                        maxLines: 1,
                        style: TextStyle(color: _palette.muted, fontSize: 12),
                      ),
                    ],
                  ],
                ),
              ),
              const SizedBox(width: 8),
              update,
              const SizedBox(width: 4),
              IconButton(
                key: const Key('host-refresh'),
                tooltip: l10n.refreshStatus2,
                style: quiet,
                onPressed: _busy ? null : _refresh,
                icon: const Icon(Icons.refresh_rounded),
              ),
              LanguageSwitch(iconColor: _palette.muted),
              AppearanceSwitch(
                compact: true,
                iconColor: _palette.muted,
                scenes: false,
                languages: false,
              ),
              IconButton(
                key: const Key('host-header-info'),
                tooltip: l10n.hostInfo,
                style: quiet,
                onPressed: () => unawaited(_openInformation()),
                icon: const Icon(Icons.info_outline_rounded),
              ),
            ],
          ),
        ),
      ),
    );
  }

  @override
  Widget build(BuildContext context) => PopScope(
    canPop: !_busy,
    onPopInvokedWithResult: (didPop, result) {
      if (didPop) _clearAccountConsentGeneration++;
    },
    child: Scaffold(
      backgroundColor: _palette.canvas,
      // Spatial reading order can interleave the fixed sidebar with inputs
      // after the form scrolls. Keep Tab in the declared header/sidebar/form
      // order so account and server fields stay consecutive at every size.
      body: FocusTraversalGroup(
        policy: WidgetOrderTraversalPolicy(),
        child: SafeArea(
          child: LayoutBuilder(
            builder: (context, bounds) {
              final effectiveWidth =
                  bounds.maxWidth / MediaQuery.textScalerOf(context).scale(1);
              // The labelled navigation of the design (184 px) while the page
              // beside it keeps a readable width; icons over short names in a
              // narrow or large-text window.
              final rail = effectiveWidth < 560;
              return Column(
                children: [
                  _header(compact: effectiveWidth < 520),
                  Divider(height: 1, color: _palette.line),
                  Expanded(
                    child: Row(
                      crossAxisAlignment: CrossAxisAlignment.stretch,
                      children: [
                        HostSettingsNavigation(
                          selected: _page,
                          states: _setupStates,
                          onSelected: _navigate,
                          enabled: !_busy,
                          rail: rail,
                          width: rail ? 72 : 184,
                        ),
                        VerticalDivider(width: 1, color: _palette.line),
                        Expanded(child: _settingsContent()),
                      ],
                    ),
                  ),
                ],
              );
            },
          ),
        ),
      ),
    ),
  );

  Widget _settingsContent() => LayoutBuilder(
    builder: (context, box) {
      final scale = MediaQuery.textScalerOf(context).scale(1);
      final side = box.maxWidth / scale < 400 ? 20.0 : 28.0;
      final body = Column(
        crossAxisAlignment: CrossAxisAlignment.stretch,
        children: [
          if (_page != 0) _sectionHeading(),
          if (_busy) _operationPanel(),
          if (_notice != null) _noticeBar(),
          // The overview shows only what is there; the device page asks
          // for administrator rights only when it offers a fix.
          if (!_admin &&
              !_busy &&
              _page != 0 &&
              (_page != 6 || _deviceRows.any((row) => row.action != null)))
            _adminBar(),
          switch (_page) {
            0 => _overview(),
            1 => _server(),
            2 => _displays(),
            5 => _windowsLoginPanel(),
            6 => _devices(),
            _ => _diagnostics(),
          },
        ],
      );
      return Scrollbar(
        controller: _scroll,
        thumbVisibility: true,
        child: SingleChildScrollView(
          controller: _scroll,
          padding: EdgeInsets.fromLTRB(side, 28, side, 24),
          child: Center(
            child: ConstrainedBox(
              constraints: BoxConstraints(
                maxWidth: 640,
                // The overview's closing line rests at the bottom of a window
                // with room to spare.
                minHeight: _page == 0 && box.maxHeight.isFinite
                    ? (box.maxHeight - 52).clamp(0, double.infinity)
                    : 0,
              ),
              child: _page == 0
                  ? Column(
                      crossAxisAlignment: CrossAxisAlignment.stretch,
                      mainAxisAlignment: MainAxisAlignment.spaceBetween,
                      children: [body, _overviewFootnote()],
                    )
                  : body,
            ),
          ),
        ),
      );
    },
  );

  // What a running operation is doing: its name, a progress line and the
  // current step.
  Widget _operationPanel() => Padding(
    padding: const EdgeInsets.only(bottom: 20),
    child: Column(
      crossAxisAlignment: CrossAxisAlignment.start,
      children: [
        KeepAllText(
          _operation,
          style: TextStyle(
            fontSize: 14,
            fontWeight: FontWeight.w600,
            color: _palette.ink,
          ),
        ),
        const SizedBox(height: 10),
        const LinearProgressIndicator(
          key: Key('host-operation-progress'),
          minHeight: 4,
        ),
        const SizedBox(height: 8),
        KeepAllText(
          _phase,
          style: TextStyle(color: _palette.muted, fontSize: 12, height: 1.5),
        ),
      ],
    ),
  );

  /// The VM's name from the broker, or null (an older Host or server): the
  /// desktop id stays the title then.
  String? get _desktopName {
    final value = _state['desktopName'];
    return value is String && value.trim().isNotEmpty ? value.trim() : null;
  }

  // What the window knows, as the facts the overview is decided from
  // (host_overview.dart). Every value is one the pages already show.
  HostFacts get _facts {
    if (_state.isEmpty) return HostFacts(readFailed: _error && !_busy);
    final service = _section('service'), display = _section('display');
    final native = display['provider'] == 'native';
    final login = _loginState;
    bool? known(Object? value) => value is bool ? value : null;
    return HostFacts(
      loaded: true,
      serviceInstalled: known(service['installed']),
      serviceRunning: known(service['running']),
      displayMissing: _admin && _displayMissing,
      displayBlocked:
          _admin && !_displayMissing && _displayFinishAttention != null,
      restartRequired: display['rebootRequired'] == true,
      recoveryRequired: display['recoveryRequired'] == true,
      registered: _registered,
      awaitingApproval: _awaitingApproval,
      cancellingConnection: _cancellingConnection,
      brokerOnline: _brokerOnline,
      interactive: _state['interactiveSession'] == true,
      displayReady: _displayOutputReady,
      transferEligible:
          native && _section('consoleTransfer')['eligible'] == true,
      displayPreparing:
          native &&
          display['installed'] == true &&
          display['deviceReady'] == true &&
          display['remoteSession'] != true &&
          const {
            '',
            'native_display_not_ready',
            'native_display_change_pending',
            'native_display_activation_pending',
          }.contains(display['error'] ?? ''),
      loginSetup: login == HostSetupState.setup,
      loginAttention: login == HostSetupState.attention,
      winfspMissing: _admin && _winfspMissing,
      devicesAttention: _devicesState == HostSetupState.attention,
    );
  }

  /// A 로컬 PC has no Proxmox console and no Proxmox to add a sound card in:
  /// what is shown says 본체 화면 there. For a VM, and wherever the desktop id
  /// does not tell the kind, the texts keep naming both.
  String _forMachine(String text) =>
      hostMachineKind(_state['desktopId']) != HostMachineKind.pc
      ? text
      : text
            .replaceAll(
              l10n.administratorNeedsAddAudioDevice,
              l10n.connectSoundOutputDeviceTurn,
            )
            .replaceAll(l10n.proxmoxConsoleLocalPcScreen, l10n.localPcScreen)
            .replaceAll(l10n.consoleLocalPcScreen, l10n.localPcScreen)
            .replaceAll(l10n.proxmoxConsoleScreen, l10n.localPcScreen)
            .replaceAll(l10n.proxmoxConsole, l10n.localPcScreen);

  /// Automatic login is on for the saved account.
  bool get _autoLoginOn =>
      const {'configured', 'observed'}.contains(_windowsLogin.state) &&
      _windowsLogin.accountLabel.isNotEmpty;

  /// The server's short name: the registered one, or the one being asked.
  String get _serverName => hostServerName(
    _connectionWorkflow && _requestOrigin.isNotEmpty
        ? _requestOrigin
        : _state['serverOrigin'],
  );

  /// "방금 확인" / "12초 전 확인" while the acknowledgement is fresh.
  String get _ackWords {
    final age = _section('broker')['heartbeatAgeSeconds'];
    return hostAckWords(
      age is num && age.isFinite && age >= 0
          ? age + _statusAge.elapsedMilliseconds / 1000
          : 0,
    );
  }

  String _withServer(String words) =>
      _serverName.isEmpty ? words : '$_serverName · $words';

  // 개요: the desktop's name with its readiness said once in plain words,
  // ONE sentence and ONE action when something is wrong, then a plain line
  // per area that opens its page (a fine area carries no state word).
  Widget _overview() {
    final facts = _facts;
    final problem = hostMainProblem(facts);
    final id = '${_state['desktopId'] ?? ''}';
    final local = hostMachineKind(id) == HostMachineKind.pc;
    final name = !_registered || id.isEmpty
        ? l10n.desktop4
        : _desktopName ?? (local ? l10n.desktop4 : id);
    final (readyState, readyLabel) = problem != null
        ? (HostRouteState.warn, problem.label)
        : facts.loaded
        ? (HostRouteState.ok, l10n.readyStream)
        : (HostRouteState.idle, l10n.checkingStatus2);
    final d = _section('display');
    final monitors = (d['monitors'] as List? ?? []).length;
    final states = _setupStates;
    return Column(
      crossAxisAlignment: CrossAxisAlignment.stretch,
      children: [
        Wrap(
          spacing: 12,
          runSpacing: 4,
          crossAxisAlignment: WrapCrossAlignment.center,
          children: [
            Semantics(
              header: true,
              child: KeepAllText(
                name,
                key: const Key('host-connection-heading'),
                style: TextStyle(
                  fontSize: 20,
                  fontWeight: FontWeight.w600,
                  letterSpacing: -.2,
                  height: 1.3,
                  color: _palette.ink,
                ),
              ),
            ),
            HostReadiness(
              key: const Key('host-overview-state'),
              state: readyState,
              label: readyLabel,
            ),
          ],
        ),
        const SizedBox(height: 4),
        // Nothing is said about the machine before a status was read.
        KeepAllText(
          _state.isEmpty
              ? l10n.checkingHostStatus2
              : _registered
              ? hostMachineLine(id, named: _desktopName != null)
              : l10n.notRegisteredServerYet,
          key: const Key('host-machine-line'),
          style: TextStyle(fontSize: 13, color: _palette.muted, height: 1.45),
        ),
        if (problem != null) ...[
          const SizedBox(height: 24),
          _problemBanner(problem),
        ],
        const SizedBox(height: 32),
        KeepAllText(
          l10n.components,
          style: TextStyle(
            fontSize: 14,
            fontWeight: FontWeight.w600,
            color: Color.lerp(_palette.muted, _palette.ink, .6),
          ),
        ),
        const SizedBox(height: 6),
        Column(
          key: const Key('host-overview-areas'),
          crossAxisAlignment: CrossAxisAlignment.stretch,
          children: [
            _areaRow(
              page: 5,
              state: states[5],
              detail: _state.isEmpty
                  ? l10n.checking3
                  : _state['interactiveSession'] != true
                  ? l10n.signedOutLocked
                  : states[5] == HostSetupState.complete
                  ? l10n.accountAutoSign(_windowsLogin.accountLabel)
                  : _autoLoginOn &&
                        _section('windowsAccount')['configured'] != true
                  ? l10n.noSavedWindowsAccount
                  : _autoLoginOn
                  ? l10n.autoSignAccountDiffersFrom
                  : _windowsStateLabel(_windowsLogin),
            ),
            _areaRow(
              page: 2,
              state: states[2],
              detail: _displayOutputReady
                  ? l10n.ready4(monitors)
                  : _state.isEmpty
                  ? l10n.checking3
                  : _displayMissing
                  ? l10n.noDriverYet
                  : d['rebootRequired'] == true
                  ? l10n.windowsNeedsRestart
                  : d['recoveryRequired'] == true
                  ? l10n.screenRecoveryNeeded
                  : facts.transferEligible
                  ? l10n.mustSwitchStreamingScreen
                  : facts.displayPreparing
                  ? l10n.servicePreparing
                  : l10n.virtualDisplayNeedsChecking,
            ),
            // Devices are optional; they join once the desktop is registered.
            if (_registered)
              _areaRow(
                page: 6,
                state: states[6],
                detail: hostDevicesSummary(_deviceRows),
              ),
            _areaRow(
              page: 1,
              state: states[1],
              detail: _state.isEmpty
                  ? l10n.checking3
                  : _cancellingConnection
                  ? _withServer(l10n.cancelingRequest)
                  : _awaitingApproval
                  ? _withServer(l10n.pendingApproval)
                  : !_registered
                  ? l10n.notRegistered2
                  : _brokerOnline
                  ? _withServer(_ackWords)
                  : _brokerStateText,
              detailKey: _registered && !_connectionWorkflow
                  ? const Key('host-broker-state')
                  : null,
            ),
          ],
        ),
      ],
    );
  }

  // While a stream runs only the Multitap virtual screen is on, and the
  // screens that were on come back afterwards.
  Widget _overviewFootnote() => Padding(
    padding: const EdgeInsets.only(top: 20),
    child: KeepAllText(
      l10n.whileStreamingOnlyVirtualDisplay,
      key: const Key('host-overview-footnote'),
      style: TextStyle(fontSize: 12, color: _palette.muted, height: 1.6),
    ),
  );

  static const _openKeys = {
    1: 'host-open-server',
    2: 'host-open-display',
    5: 'host-open-windows',
    6: 'host-open-devices',
  };

  /// One overview line: the area, its current words, its state, the way there.
  Widget _areaRow({
    required int page,
    required HostSetupState state,
    required String detail,
    Key? detailKey,
  }) => HostAreaRow(
    key: Key(_openKeys[page]!),
    title: HostSettingsNavigation.titles[page],
    detail: detail,
    detailKey: detailKey,
    state: state,
    onTap: _busy ? null : () => _navigate(page),
  );

  bool get _serviceControllable {
    final s = _section('service');
    return !_busy &&
        _admin &&
        s['installed'] == true &&
        s['running'] is bool &&
        !{
          'starting',
          'stopping',
          'paused',
          'pausing',
          'continuing',
        }.contains(s['state']);
  }

  // The one banner of the overview: what happened and what to do in one or
  // two short sentences, and the one action that does it (or opens its page).
  Widget _problemBanner(HostProblem problem) {
    Widget banner(
      String message, {
      String key = 'host-problem',
      String? messageKey,
      String? code,
      String? action,
      String actionKey = 'host-problem-action',
      VoidCallback? run,
    }) => HostProblemBanner(
      key: Key(key),
      message: _forMachine(message),
      messageKey: Key(messageKey ?? '$key-message'),
      code: code,
      actionLabel: action,
      actionKey: Key(actionKey),
      onAction: _busy ? null : run,
    );
    void open(int page) => _navigate(page);
    final d = _section('display');
    final local = hostMachineKind(_state['desktopId']) == HostMachineKind.pc;
    switch (problem) {
      case HostProblem.statusUnread:
      case HostProblem.serviceUnknown:
        return banner(
          problem == HostProblem.statusUnread
              ? l10n.couldNotReadHostStatus
              : l10n.couldNotReadHostService,
          action: l10n.refresh,
          run: _refresh,
        );
      case HostProblem.serviceMissing:
        return banner(l10n.hostServiceNotInstalledRun);
      case HostProblem.serviceStopped:
        final moving = const {
          'starting',
          'stopping',
          'paused',
          'pausing',
          'continuing',
        }.contains(_section('service')['state']);
        return banner(
          moving
              ? l10n.hostServiceChangingStateRefresh
              : l10n.hostServiceOffSoCannot,
          action: moving ? l10n.refresh : l10n.startService,
          actionKey: moving ? 'host-problem-action' : 'host-service-start',
          run: moving
              ? _refresh
              : _serviceControllable
              ? _service
              : null,
        );
      case HostProblem.installUnfinished:
      case HostProblem.winfspMissing:
        // A first install that is not finished yet (or was declined, or
        // failed): it stays until the status says the parts are there.
        return banner(
          hostFinishCardMessage(
            display: problem == HostProblem.installUnfinished,
            winfsp: _winfspMissing,
          ),
          key: 'host-finish-card',
          messageKey: 'host-finish-message',
          action: hostFinishAction,
          actionKey: 'host-finish-install',
          run: _finishInstall,
        );
      case HostProblem.installBlocked:
        return banner(
          _displayFinishAttention!,
          key: 'host-finish-card',
          messageKey: 'host-finish-message',
        );
      case HostProblem.restartRequired:
      case HostProblem.displayRecovery:
        final recovery = d['recoveryRequired'] == true;
        final restart = d['rebootRequired'] == true;
        final save = l10n.saveWorkRestartWindowsWont;
        return banner(
          d['provider'] == 'native' && restart
              ? save
              : d['provider'] == 'native'
              ? l10n.screenRecoveryFromLastStreaming2
              : recovery
              ? l10n.checkVirtualDisplayStatusFrom(restart ? ' $save' : '')
              : save,
          key: 'host-display-attention',
          action: l10n.viewVirtualDisplay,
          actionKey: 'host-display-attention-open',
          run: () => open(2),
        );
      case HostProblem.serverUnregistered:
        return banner(
          _terminalRequestMessage ??
              l10n.notRegisteredServerYetRequest,
          messageKey: _terminalRequestMessage == null
              ? null
              : 'host-connection-result',
          action: l10n.connect4,
          actionKey: 'host-connect',
          run: () => open(1),
        );
      case HostProblem.serverApproval:
        final code = _requestStatus == 'pending' && !_approvalDeadlineReached
            ? _verificationCode
            : null;
        return banner(
          _cancellingConnection
              ? l10n.cancelingConnectionRequestWaitingServer
              : _approvalDeadlineReached
              ? l10n.requestExpiredRefreshStatusThen
              : _requestStatus == 'approved'
              ? l10n.checkingRegistrationResultApprovedConnection
              : code != null
              ? l10n.adminConsoleUnderPendingApprovals
              : l10n.approveConnectionRequestAdminConsole,
          messageKey: 'host-approval-summary',
          code: _cancellingConnection ? null : code,
          action: l10n.viewRequest,
          actionKey: 'host-open-request',
          run: () => open(1),
        );
      case HostProblem.serverOffline:
        return banner(
          _brokerGuidance,
          messageKey: 'host-broker-guidance',
          action: l10n.connectionDiagnostics,
          actionKey: 'host-broker-diagnostics',
          run: () {
            _diagnosticsDetails = true;
            open(4);
          },
        );
      case HostProblem.windowsSignedOut:
        // The status has one flag for "signed out" and "locked". A local PC
        // has no Proxmox console; a restart signs in only with automatic
        // login on.
        final where = local ? l10n.fromLocalPcScreen : l10n.fromConsoleClient;
        return banner(
          l10n.windowsSignedOutLocked(_autoLoginOn ? l10n.signRestartDesktopSignAutomatically(where) : l10n.sign2(where)),
          action: l10n.signSettings,
          run: () => open(5),
        );
      case HostProblem.displayTransfer:
        return banner(
          l10n.currentWindowsSignNotStreaming,
          action: l10n.viewVirtualDisplay,
          run: () => open(2),
        );
      case HostProblem.displayPreparing:
        return banner(
          l10n.servicePreparingVirtualDisplayPlease,
          action: l10n.viewVirtualDisplay,
          run: () => open(2),
        );
      case HostProblem.displayNotReady:
        return banner(
          _displayHint(d),
          action: l10n.viewVirtualDisplay,
          run: () => open(2),
        );
      case HostProblem.loginSetup:
        return banner(
          l10n.autoSignOffSaveWindows,
          action: l10n.signSettings,
          run: () => open(5),
        );
      case HostProblem.loginAttention:
        return banner(
          l10n.autoSignNeedsCheckingSave,
          action: l10n.signSettings,
          run: () => open(5),
        );
      case HostProblem.devicesAttention:
        return banner(
          l10n.someDeviceFeaturesNotReady,
          action: l10n.viewDevices,
          run: () => open(6),
        );
    }
  }

  Widget _noticeBar() {
    final tone = _error ? _palette.red : _palette.green;
    return Container(
      margin: const EdgeInsets.only(bottom: 20),
      padding: const EdgeInsets.fromLTRB(14, 12, 16, 12),
      decoration: BoxDecoration(
        color: tone.withValues(alpha: .10),
        borderRadius: BorderRadius.circular(10),
      ),
      child: Row(
        crossAxisAlignment: CrossAxisAlignment.start,
        children: [
          Icon(
            _error
                ? Icons.error_outline_rounded
                : Icons.check_circle_outline_rounded,
            size: 18,
            color: tone,
          ),
          const SizedBox(width: 10),
          Expanded(
            child: KeepAllText(
              _forMachine(_notice!),
              key: const Key('host-feedback'),
              style: TextStyle(color: tone, fontSize: 12.5, height: 1.5),
            ),
          ),
        ],
      ),
    );
  }

  Widget _adminBar() {
    final button = OutlinedButton.icon(
      key: const Key('host-elevate'),
      onPressed: () async {
        if (await _confirm(
          l10n.openAdministratorSettingsWindow,
          l10n.youCanChangeSettingsNew,
          accept: l10n.openNewWindow,
        )) {
          await _action(
            'elevate',
            {},
            l10n.waitingWindowsAdministratorApproval,
            l10n.administratorSettingsWindowOpenContinue,
          );
        }
      },
      icon: const Icon(Icons.admin_panel_settings_outlined, size: 18),
      label: KeepAllText(l10n.settingsAsAdministrator),
    );
    final text = Row(
      children: [
        Icon(Icons.lock_outline_rounded, size: 18, color: _palette.amberInk),
        const SizedBox(width: 10),
        Expanded(
          child: KeepAllText(
            l10n.changingSettingsNeedsAdministratorRights,
            style: TextStyle(fontSize: 12.5, height: 1.5),
          ),
        ),
      ],
    );
    return Container(
      margin: const EdgeInsets.only(bottom: 20),
      padding: const EdgeInsets.fromLTRB(14, 12, 12, 12),
      decoration: BoxDecoration(
        color: _palette.amber.withValues(alpha: _palette.dark ? .10 : .14),
        borderRadius: BorderRadius.circular(10),
      ),
      child: LayoutBuilder(
        builder: (context, box) => box.maxWidth < 420
            ? Column(
                crossAxisAlignment: CrossAxisAlignment.start,
                children: [text, const SizedBox(height: 10), button],
              )
            : Row(
                children: [
                  Expanded(child: text),
                  const SizedBox(width: 12),
                  button,
                ],
              ),
      ),
    );
  }

  String? get _terminalRequestMessage => switch (_requestStatus) {
    'rejected' => l10n.administratorRejectedRequestCheckServer,
    'expired' => l10n.connectionRequestExpiredCheckServer,
    'cancelled' => l10n.connectionRequestCanceled,
    // host-machine-v1: the Host kept a registration copied with the disk
    // (full clone) aside; this Windows asks for its own.
    _ => !_registered && _state['copiedRegistration'] == true
        ? l10n.hostCopiedRegistrationRequestAgain
        : null,
  };

  String _safeOrigin(String value) {
    try {
      return parseServerOrigin(value).toString();
    } on FormatException {
      return l10n.checkServerAddress2;
    }
  }

  Widget _approvalPanel() {
    final expiry = DateTime.tryParse('${_connectionRequest['expiresAt'] ?? ''}')
        ?.toLocal();
    // Only while the administrator still has to compare and approve it.
    final code =
        _cancellingConnection ||
            _approvalDeadlineReached ||
            _requestStatus != 'pending'
        ? null
        : _verificationCode;
    final expiryText = expiry == null
        ? null
        : l10n.untilDate(
            '${formatMonthDay(expiry)} ${expiry.hour.toString().padLeft(2, '0')}:${expiry.minute.toString().padLeft(2, '0')}',
          );
    return _panel(
      children: [
        KeepAllText(
          _cancellingConnection
              ? l10n.cancelingConnectionRequest
              : _approvalDeadlineReached
              ? l10n.checkApprovalWaitTime
              : _requestStatus == 'approved'
              ? l10n.checkingServerRegistration
              : l10n.awaitingAdministratorApproval,
          key: const Key('host-approval-heading'),
          style: TextStyle(
            fontSize: 15,
            fontWeight: FontWeight.w600,
            color: _palette.ink,
          ),
        ),
        const SizedBox(height: 6),
        SelectableText(
          _safeOrigin(_requestOrigin),
          style: TextStyle(fontSize: 13, color: _palette.muted),
        ),
        if (code != null) ...[
          const SizedBox(height: 14),
          Container(
            key: const Key('host-verification-code'),
            width: double.infinity,
            padding: const EdgeInsets.symmetric(horizontal: 16, vertical: 12),
            decoration: BoxDecoration(
              color: _palette.amber.withValues(
                alpha: _palette.dark ? .10 : .14,
              ),
              borderRadius: BorderRadius.circular(10),
            ),
            child: Column(
              crossAxisAlignment: CrossAxisAlignment.start,
              children: [
                KeepAllText(
                  l10n.confirmationCode,
                  style: TextStyle(fontSize: 12, color: _palette.muted),
                ),
                const SizedBox(height: 2),
                SelectableText(
                  code,
                  key: const Key('host-verification-code-value'),
                  style: const TextStyle(
                    fontSize: 22,
                    fontWeight: FontWeight.w600,
                    letterSpacing: 2,
                    fontFamily: 'Consolas',
                  ),
                ),
                const SizedBox(height: 4),
                KeepAllText(
                  l10n.adminConsoleMustShowSame,
                  style: TextStyle(
                    fontSize: 12,
                    color: _palette.muted,
                    height: 1.5,
                  ),
                ),
              ],
            ),
          ),
        ],
        const SizedBox(height: 10),
        KeepAllText(
          _cancellingConnection
              ? l10n.waitingServerConfirmCancellationAfter
              : _approvalDeadlineReached
              ? l10n.requestExpiredRefreshStatusThen
              : _requestStatus == 'approved'
              ? l10n.checkingRegistrationResultApprovedConnection
              : code != null
              ? l10n.adminConsoleGoDesktopsPending
              : l10n.adminConsoleApproveConnectionRequest,
          style: const TextStyle(fontSize: 13, height: 1.6),
        ),
        const SizedBox(height: 8),
        KeepAllText(
          _section('service')['running'] == true
              ? l10n.evenIfYouCloseWindow
              : l10n.startWindowsServiceKeepChecking,
          style: TextStyle(fontSize: 12, color: _palette.muted, height: 1.5),
        ),
        if (expiryText != null && !_cancellingConnection) ...[
          const SizedBox(height: 6),
          KeepAllText(
            l10n.approvalValid(expiryText),
            style: TextStyle(fontSize: 12, color: _palette.muted),
          ),
        ],
        if (_approvalReadError) ...[
          const SizedBox(height: 10),
          KeepAllText(
            l10n.couldNotCheckApprovalStatus,
            key: Key('host-approval-status-error'),
            style: TextStyle(fontSize: 12, color: _palette.amberInk),
          ),
        ],
        if (_connectionRequest['error'] == 'already_approved') ...[
          const SizedBox(height: 10),
          KeepAllText(
            l10n.requestAlreadyApprovedCheckingServer,
            style: TextStyle(fontSize: 12, color: _palette.muted),
          ),
        ],
        const SizedBox(height: 16),
        Wrap(
          spacing: 8,
          runSpacing: 6,
          children: [
            OutlinedButton.icon(
              key: const Key('host-cancel-connection'),
              onPressed: _busy || !_admin || _cancellingConnection
                  ? null
                  : _cancelConnection,
              icon: const Icon(Icons.close_rounded, size: 17),
              label: KeepAllText(l10n.cancelRequest),
            ),
            TextButton.icon(
              key: const Key('host-edit-pending-server'),
              onPressed: _busy || !_admin || _cancellingConnection
                  ? null
                  : () => _cancelConnection(edit: true),
              icon: const Icon(Icons.edit_outlined, size: 17),
              label: KeepAllText(l10n.editServer),
            ),
          ],
        ),
      ],
    );
  }

  Widget _server() => _connectionWorkflow
      ? _approvalPanel()
      : _panel(
          title: _registered && !_editRegistration ? l10n.serverConnection3 : l10n.connectServer,
          subtitle: _registered && !_editRegistration
              ? l10n.registrationKeptAfterUpdates
              : l10n.checkServerAddressRequestConnection,
          children: [
            if (_registered && !_editRegistration) ...[
              ..._rows([
                _info(l10n.serverAddress, '${_state['serverOrigin'] ?? '—'}'),
                _info(l10n.desktopId, '${_state['desktopId'] ?? '—'}'),
                _info(l10n.connectionStatus, _brokerStateText),
              ]),
              // Not connected: one sentence on what to do and the way to
              // the details (the overview says the same).
              if (!_brokerOnline) ...[
                const SizedBox(height: 10),
                KeepAllText(
                  _brokerGuidance,
                  key: const Key('host-server-guidance'),
                  style: TextStyle(
                    fontSize: 13,
                    height: 1.5,
                    color: _palette.amberInk,
                  ),
                ),
                TextButton(
                  key: const Key('host-server-diagnostics'),
                  onPressed: _busy
                      ? null
                      : () {
                          _diagnosticsDetails = true;
                          _navigate(4);
                        },
                  style: TextButton.styleFrom(
                    padding: EdgeInsets.zero,
                    minimumSize: const Size(0, 32),
                    tapTargetSize: MaterialTapTargetSize.shrinkWrap,
                    alignment: Alignment.centerLeft,
                  ),
                  child: KeepAllText(l10n.connectionDiagnostics),
                ),
              ],
              const SizedBox(height: 18),
              OutlinedButton(
                key: const Key('host-change-server'),
                onPressed: _busy || !_admin
                    ? null
                    : () => setState(() {
                        _editRegistration = true;
                        _tested = null;
                      }),
                child: KeepAllText(l10n.changeServerReRegister),
              ),
            ] else ...[
              // Why the form is back after a request ended without a
              // registration (the overview says the same).
              if (!_registered && _terminalRequestMessage != null) ...[
                KeepAllText(
                  _terminalRequestMessage!,
                  key: const Key('host-request-result'),
                  style: TextStyle(
                    fontSize: 13,
                    height: 1.5,
                    color: _palette.amberInk,
                  ),
                ),
                const SizedBox(height: 14),
              ],
              BrokerTransportChoice(
                scheme: _brokerScheme,
                onChanged: _busy || _transportConfirming
                    ? null
                    : (value) {
                        final split = _origin.text.indexOf('://');
                        if (split >= 0) {
                          _origin.text =
                              '$value${_origin.text.substring(split)}';
                        }
                        _brokerScheme = value;
                        _hostOriginChanged();
                      },
              ),
              const SizedBox(height: 12),
              TextField(
                key: const Key('host-server-origin'),
                controller: _origin,
                focusNode: _originFocus,
                enabled: !_busy,
                maxLength: 200,
                keyboardType: TextInputType.url,
                autocorrect: false,
                enableSuggestions: false,
                onChanged: (_) => _hostOriginChanged(),
                onSubmitted: (_) {
                  if (!_busy) unawaited(_test());
                },
                decoration: InputDecoration(
                  labelText: l10n.serverAddress,
                  hintText: _brokerScheme == 'http'
                      ? 'IP:18480'
                      : 'https://cloud.example.com',
                  errorText: _originError,
                  counterText: '',
                ),
              ),
              const SizedBox(height: 10),
              SizedBox(
                width: double.infinity,
                child: OutlinedButton(
                  key: const Key('host-test-server'),
                  onPressed: _busy || _origin.text.trim().isEmpty
                      ? null
                      : _test,
                  child: KeepAllText(
                    _tested == _origin.text.trim() ? l10n.connectionChecked : l10n.testServerConnection,
                  ),
                ),
              ),
              const SizedBox(height: 10),
              KeepAllText(
                _tested != null && _tested == _origin.text.trim()
                    ? l10n.nowSendConnectionRequest
                    : l10n.firstFinishServerConnectionTest,
                key: const Key('host-enrollment-step'),
                style: TextStyle(
                  fontSize: 12,
                  height: 1.5,
                  color: _tested != null && _tested == _origin.text.trim()
                      ? _palette.green
                      : _palette.muted,
                ),
              ),
              const SizedBox(height: 20),
              SizedBox(
                width: double.infinity,
                child: FilledButton(
                  key: const Key('host-request-connection'),
                  onPressed:
                      !_busy &&
                          _admin &&
                          _tested != null &&
                          _tested == _origin.text.trim() &&
                          !_connectionWorkflow
                      ? _requestConnection
                      : null,
                  child: KeepAllText(l10n.connectionRequest),
                ),
              ),
              if (_registered) ...[
                const SizedBox(height: 6),
                Align(
                  alignment: Alignment.centerRight,
                  child: TextButton(
                    onPressed: _busy
                        ? null
                        : () => setState(() {
                            _editRegistration = false;
                            _origin.text = '${_state['serverOrigin'] ?? ''}';
                            _tested = null;
                          }),
                    child: KeepAllText(l10n.discardChanges),
                  ),
                ),
              ],
            ],
          ],
        );
  Widget _displays() {
    final d = _section('display'), monitors = d['monitors'] as List? ?? [];
    return Column(
      crossAxisAlignment: CrossAxisAlignment.stretch,
      children: [
        _panel(
          title: _displayOutputReady
              ? l10n.text20(monitors.length)
              : d['provider'] == 'native' && d['rebootRequired'] == true
              ? l10n.windowsRestartNeeded
              : d['provider'] == 'native' &&
                    _section('consoleTransfer')['eligible'] == true
              ? l10n.switchStreamingScreen2
              : l10n.checkVirtualDisplay2,
          subtitle: _forMachine(_displayHint(d)),
          children: [
            ..._rows([
              for (final (index, m) in monitors.indexed)
                _info(l10n.monitor2(index + 1), '${m['width']} × ${m['height']}'),
            ]),
            if (monitors.isEmpty &&
                !(d['provider'] == 'native' && d['remoteSession'] == true))
              KeepAllText(
                l10n.noMultitapVirtualMonitor,
                style: TextStyle(color: _palette.muted, fontSize: 12.5),
              ),
            const SizedBox(height: 16),
            // One row of actions: the next step first, checks after it.
            Wrap(
              spacing: 8,
              runSpacing: 8,
              children: [
                if (d['provider'] == 'native' && d['remoteSession'] == true)
                  FilledButton.icon(
                    key: const Key('host-console-transfer'),
                    onPressed:
                        _busy ||
                            !_admin ||
                            _section('consoleTransfer')['eligible'] != true
                        ? null
                        : _consoleTransfer,
                    icon: const Icon(Icons.desktop_windows_rounded, size: 19),
                    label: KeepAllText(
                      d['rebootRequired'] == true
                          ? l10n.switchScreenAfterWindowsRestart
                          : l10n.switchStreamingScreen2,
                    ),
                  ),
                if (d['provider'] == 'native')
                  OutlinedButton.icon(
                    key: const Key('host-native-display-refresh'),
                    onPressed: _busy ? null : _refresh,
                    icon: const Icon(Icons.refresh_rounded, size: 19),
                    label: KeepAllText(l10n.refreshVirtualDisplayStatus),
                  ),
                if (d['provider'] == 'native' && d['recoveryRequired'] == true)
                  OutlinedButton.icon(
                    key: const Key('host-native-recovery-reset'),
                    onPressed: _busy || !_admin || d['remoteSession'] == true
                        ? null
                        : _resetNativeRecovery,
                    icon: const Icon(
                      Icons.history_toggle_off_rounded,
                      size: 19,
                    ),
                    label: KeepAllText(l10n.resetVirtualDisplayRecoveryRecord),
                  ),
                // A first install: the current driver, after the one
                // confirmation. The older driver's setup below is offered only
                // where this one cannot be installed.
                if (_admin && _displayMissing)
                  FilledButton.icon(
                    key: const Key('host-display-finish'),
                    onPressed: _busy ? null : _finishInstall,
                    icon: const Icon(Icons.download_done_rounded, size: 19),
                    label: KeepAllText(hostFinishAction),
                  ),
                if (d['provider'] != 'native' && !(_admin && _displayMissing))
                  OutlinedButton.icon(
                    key: const Key('host-display-setup'),
                    onPressed:
                        _busy ||
                            !_admin ||
                            d['rebootRequired'] == true ||
                            (d['recoveryRequired'] != true &&
                                _displayNeedsConsole(d))
                        ? null
                        : _display,
                    icon: Icon(
                      d['recoveryRequired'] == true
                          ? Icons.restore_rounded
                          : Icons.monitor_outlined,
                      size: 19,
                    ),
                    label: KeepAllText(
                      d['rebootRequired'] == true
                          ? l10n.checkAfterWindowsRestart
                          : d['recoveryRequired'] == true
                          ? l10n.recoverVirtualDisplay
                          : _displayNeedsConsole(d)
                          ? _forMachine(l10n.setUpVirtualDisplayFrom)
                          : l10n.setUpVirtualDisplay2,
                    ),
                  ),
                if (d['provider'] != 'native')
                  OutlinedButton.icon(
                    key: const Key('host-display-repair-scan'),
                    onPressed: _busy || !_admin ? null : _readDisplayRepairPlan,
                    icon: const Icon(Icons.manage_search_rounded, size: 19),
                    label: KeepAllText(l10n.checkExistingDevicesLeftoverSettings),
                  ),
              ],
            ),
            _displayAdapterLine(),
            const SizedBox(height: 6),
            _details(
              key: const Key('host-display-help'),
              children: [
                _detailText(
                  d['provider'] == 'native'
                      ? l10n.onceDisplayReadyResolutionFollows
                      : l10n.setNumberMonitors12,
                ),
              ],
            ),
          ],
        ),
        Padding(
          padding: const EdgeInsets.only(bottom: 28),
          child: HostEncoderPanel(
            api: _api,
            administrator: _admin,
            blocked: _busy || _encoderNavigation,
            protectMutation: _encoderProtectMutation,
            onDirty: _encoderDraftChanged,
            onPending: _encoderPendingChanged,
          ),
        ),
        if (_displayRepairPlan != null)
          _displayRepairPlanPanel(_displayRepairPlan!),
        if (_displayRepairResult != null)
          _displayRepairResultPanel(_displayRepairResult!),
      ],
    );
  }

  Widget _displayRepairPlanPanel(Map<String, dynamic> plan) {
    final items = (plan['items'] as List).cast<Map>();
    final devices = items.where((item) => item['kind'] == 'device').length,
        configurations = items
            .where((item) => item['kind'] == 'configuration')
            .length;
    return _panel(
      key: _repairPlanAnchor,
      title: l10n.conflictsFound,
      subtitle: l10n.devicesLeftoverSettingsNothingDeleted(devices, configurations),
      children: [
        if (items.isEmpty && plan['canRepair'] == true)
          KeepAllText(l10n.noConflictsCleanUp, style: TextStyle(fontSize: 13)),
        for (var i = 0; i < items.length; i++) ...[
          if (i > 0) const Divider(height: 28),
          Row(
            crossAxisAlignment: CrossAxisAlignment.start,
            children: [
              Icon(
                items[i]['kind'] == 'device'
                    ? Icons.desktop_windows_outlined
                    : Icons.tune_rounded,
                size: 20,
                color: _palette.amber,
              ),
              const SizedBox(width: 10),
              Expanded(
                child: Column(
                  crossAxisAlignment: CrossAxisAlignment.start,
                  children: [
                    KeepAllText(
                      items[i]['kind'] == 'device'
                          ? l10n.virtualDisplayDeviceRequired
                          : l10n.leftoverSettingsRequired,
                      style: TextStyle(fontSize: 12, color: _palette.muted),
                    ),
                    const SizedBox(height: 4),
                    KeepAllText(
                      '${items[i]['name']}',
                      style: const TextStyle(
                        fontSize: 13,
                        fontWeight: FontWeight.w600,
                      ),
                    ),
                    const SizedBox(height: 5),
                    SelectableText(
                      items[i]['kind'] == 'device'
                          ? '${items[i]['instanceId']}'
                          : '${items[i]['registryPath']}\n${items[i]['registryValue'] ?? ''}',
                      style: TextStyle(
                        fontSize: 12,
                        color: _palette.muted,
                        height: 1.5,
                      ),
                    ),
                    const SizedBox(height: 7),
                    KeepAllText(
                      l10n.impact2(items[i]['impact']),
                      style: const TextStyle(fontSize: 12, height: 1.5),
                    ),
                  ],
                ),
              ),
            ],
          ),
        ],
        if (items.isNotEmpty) ...[
          const SizedBox(height: 18),
          KeepAllText(
            l10n.requiredItemsAboveCleanedUp,
            style: TextStyle(fontSize: 12, color: _palette.muted, height: 1.6),
          ),
          const SizedBox(height: 16),
          const SizedBox(height: 12),
          FilledButton.icon(
            key: const Key('host-display-repair-apply'),
            style: FilledButton.styleFrom(
              backgroundColor: _palette.red,
              foregroundColor: Theme.of(context).colorScheme.onError,
            ),
            onPressed:
                _busy ||
                    !_admin ||
                    plan['canRepair'] != true ||
                    plan['repairNeeded'] != true
                ? null
                : _repairDisplay,
            icon: const Icon(Icons.build_outlined, size: 18),
            label: KeepAllText(l10n.cleanUpCheckedConflictsInstall2),
          ),
        ],
        if (plan['canRepair'] != true)
          KeepAllText(
            _forMachine(_displayRepairPlanReason('${plan['reason'] ?? ''}')),
            key: const Key('host-display-repair-blocked'),
            style: TextStyle(
              fontSize: 12,
              color: _palette.amberInk,
              height: 1.6,
            ),
          ),
      ],
    );
  }

  String _displayRepairPlanReason(String reason) => switch (reason) {
    'driver_hash_mismatch' ||
    'driver_signature_untrusted' ||
    'driver_catalog_membership_invalid' ||
    'driver_hash_failed' => l10n.couldNotVerifyDriverFiles2,
    'driver_file_unavailable' ||
    'driver_path_reparse_point' ||
    'driver_path_type_invalid' ||
    'driver_package_lock_failed' =>
      l10n.couldNotSafelyVerifyVirtual,
    'reboot_required' ||
    'repair_restart_required' => l10n.restartWindowsThenCheckConflict,
    'remote_session' ||
    'repair_console_required' => l10n.signWindowsFromProxmoxConsole2,
    'recovery_required' ||
    'viewport_recovery_required' => l10n.firstRecoverPreviousScreenChange,
    _ => l10n.automaticCleanupCannotProceedCopy,
  };

  Widget _displayRepairResultPanel(Map<String, dynamic> result) {
    final ok =
        !_repairTimedOut &&
        _repairExitCode == 0 &&
        result['repairInstalled'] == true &&
        result['cleanupCompleted'] == true &&
        result['partialRemoval'] != true &&
        result['removalUncertain'] != true;
    return _panel(
      key: const Key('host-display-repair-result'),
      title: l10n.lastCleanupInstallResult,
      subtitle: _forMachine(_displayRepairOutcome(result, _repairTimedOut, ok)),
      children: [
        _info(
          l10n.conflictCleanup,
          result['cleanupCompleted'] == true &&
                  result['removalUncertain'] != true &&
                  result['partialRemoval'] != true
              ? l10n.done
              : l10n.needsCheck,
          key: const Key('host-repair-cleanup-state'),
        ),
        if (result['removeRequestedCount'] is int)
          _info(l10n.devicesRequestedRemoval, l10n.text21(result['removeRequestedCount'])),
        if (result['removedDeviceCount'] is int)
          _info(l10n.devicesConfirmedRemoved, l10n.text21(result['removedDeviceCount'])),
        _info(
          l10n.driverInstall,
          _repairTimedOut || result['repairInstalled'] is! bool
              ? l10n.needsCheck
              : result['repairInstalled'] == true
              ? l10n.done
              : l10n.notCompleted,
        ),
        if (result['configurationRestoreAttempted'] == true &&
            result['configurationRestored'] == true)
          KeepAllText(
            l10n.previousSettingsRestored,
            style: TextStyle(fontSize: 12, color: _palette.muted),
          ),
        if (result['partialRemoval'] == true)
          KeepAllText(
            l10n.someItemsRemainCheckConflict,
            style: TextStyle(fontSize: 12, color: _palette.red, height: 1.6),
          ),
        if (result['removalUncertain'] == true)
          KeepAllText(
            l10n.deviceRemovalNotConfirmedYet,
            style: TextStyle(
              fontSize: 12,
              color: _palette.amberInk,
              height: 1.6,
            ),
          ),
        if (result['rebootRequired'] == true)
          KeepAllText(
            l10n.windowsNeedsRestartDoesNot,
            style: TextStyle(
              fontSize: 12,
              color: _palette.amberInk,
              height: 1.6,
            ),
          ),
      ],
    );
  }

  String _windowsStateLabel(WindowsAutologonStatus status) =>
      switch (status.state) {
        'off' => l10n.offTurnsWhenYouSave,
        'configured' => l10n.setAppliesFromNextWindows,
        'observed' => l10n.windowsSignSetAccountConfirmed,
        'needs_update' => l10n.accountCheckNeededSaveAccount,
        'external' => l10n.windowsHasExistingAutoSign,
        'policy_blocked' => l10n.windowsPolicyPreventsTurning,
        'failed' =>
          status.enabled == false
              ? l10n.autoSignOffRecoveryNeeded
              : l10n.checkChangeResultRecoveryStatus,
        _ =>
          status.supported ? l10n.couldNotCheckCurrentSetting : l10n.hostDoesNotSupportAuto2,
      };

  String _windowsReason(String code) => switch (code) {
    'credentials_rejected' => l10n.windowsAccountPasswordWrongNot,
    'invalid_credentials' => l10n.enterAccountPasswordPasswordConfirmation,
    'local_account_required' => l10n.onlyLocalWindowsAccountsPassword,
    'account_restricted' ||
    'account_invalid' ||
    'account_changed' => l10n.accountChangedRestrictedCheckAccount,
    'policy_restricted' => l10n.windowsPolicyBlocksAutoSign,
    'foreign_configuration' ||
    'configuration_changed' => l10n.windowsSettingsChangedDuringCheck,
    'stale_revision' => l10n.settingsChangedDuringCheckRefresh,
    'operation_busy' => l10n.anotherSettingsTaskRunningCheck,
    'repair_required' => l10n.autoSignChangeStoppedMidway,
    'administrator_required' => l10n.openHostSettingsAsAdministrator,
    _ => l10n.couldNotConfirmResultCheck,
  };

  // Automatic login stays on: the settings window only
  // applies it (as the second step of saving the account) or repairs a
  // half-finished change. It never offers turning it off.
  bool _windowsCanChange(
    WindowsAutologonStatus status,
    String action,
    bool takeover,
  ) => action == 'apply'
      ? takeover
            ? status.canTakeOwnershipApply
            : status.canApply
      : action == 'repair' && status.canRepair;

  /// apply runs only from [_saveAccount], whose confirmation already named
  /// it; repair asks for its own confirmation.
  Future<void> _windowsChange(
    String action, {
    bool takeOwnership = false,
    ({String username, String password, String confirmation})? credentials,
  }) async {
    final before = _windowsLogin;
    if (_busy || _windowsConfirming || !_admin || _windowsPendingId != null) {
      return;
    }
    if (!_windowsCanChange(before, action, takeOwnership) ||
        (action == 'apply') != (credentials != null)) {
      return;
    }
    if (action == 'repair') {
      setState(() => _windowsConfirming = true);
      var confirmed = false;
      try {
        confirmed = await _confirm(
          l10n.recoverSettingsMultitapChanged,
          l10n.onlyAutoSignSettingsMultitap,
          accept: l10n.recoverSettings,
        );
      } finally {
        if (mounted) setState(() => _windowsConfirming = false);
      }
      if (!mounted || !confirmed) return;
      if (!_admin ||
          _busy ||
          _windowsLogin.revision != before.revision ||
          !_windowsCanChange(_windowsLogin, action, takeOwnership)) {
        return;
      }
    }
    final fields = <String, dynamic>{
      if (credentials != null) ...{
        'username': credentials.username,
        'password': credentials.password,
        'passwordConfirmation': credentials.confirmation,
      },
    };
    _statusGeneration++;
    _approvalTimer?.cancel();
    setState(() {
      _busy = true;
      _mutating = true;
      _operation = l10n.settingUpWindowsAutoSign;
      _phase = l10n.checkingCurrentSettingsAccountPermissions;
    });
    try {
      if (!await _syncClose()) {
        throw HostSettingsException(
          l10n.couldNotPrepareWindowProtection,
        );
      }
      final fresh = WindowsAutologonStatus.parse(
        (await _api.request('windowsAutologonStatus'))['autologon'],
      );
      if (!mounted ||
          !_admin ||
          !fresh.administrator ||
          fresh.revision != before.revision ||
          !_windowsCanChange(fresh, action, takeOwnership)) {
        throw HostSettingsException(
          l10n.currentSettingsPermissionsChangedRefresh,
        );
      }
      if (!await _syncClose()) {
        throw HostSettingsException(
          l10n.couldNotVerifyWindowProtection,
        );
      }
      if (!mounted || !_admin || _windowsLogin.revision != before.revision) {
        return;
      }
      final id = newWindowsAutologonOperationId();
      _windowsPendingId = id;
      _windowsPendingAction = action;
      final response = await _api.request(
        action == 'apply' ? 'windowsAutologonApply' : 'windowsAutologonRepair',
        {
          ...fields,
          'operationId': id,
          'expectedRevision': fresh.revision,
          'confirmed': true,
          if (takeOwnership) 'takeOwnership': true,
        },
      );
      if (!mounted) {
        return;
      }
      if (response['rejected'] == true) {
        final ownReceipt = WindowsAutologonStatus.parse(response['autologon']);
        final admitted =
            ownReceipt.operationId == id &&
            const {
              'partial',
              'configured',
              'disabled',
            }.contains(ownReceipt.operationState);
        // Only definite pre-admission errors prove no OS mutation. In
        // particular configuration_changed may occur AFTER an admitted write.
        if (!admitted &&
            const {
              'credentials_rejected',
              'account_restricted',
              'account_invalid',
              'account_changed',
              'account_unavailable',
              'invalid_credentials',
              'local_account_required',
              'policy_restricted',
              'stale_revision',
              'foreign_configuration',
              'not_owned',
              'operation_busy',
            }.contains(response['code'])) {
          _windowsPendingId = null;
          _windowsPendingAction = null;
          await _load();
          if (mounted) {
            setState(() {
              _notice = _windowsReason('${response['code']}');
              _error = true;
            });
          }
        } else {
          // Missing/foreign/opposite receipts cannot release the own UUID.
          await _finishWindowsReceipt(response);
        }
      } else {
        await _finishWindowsReceipt(response);
      }
    } catch (_) {
      if (mounted) {
        setState(() {
          _notice = _windowsPendingId == null
              ? l10n.couldNotChangeSettingsRefresh
              : l10n.requestResultNotConfirmedYet;
          _error = true;
        });
      }
    } finally {
      fields.remove('password');
      fields.remove('passwordConfirmation');
      _mutating = false;
      if (mounted) {
        setState(() => _busy = false);
        unawaited(_syncClose());
        _scheduleApprovalPoll();
      }
    }
  }

  Future<void> _finishWindowsReceipt(Map<String, dynamic> response) async {
    final receipt = WindowsAutologonStatus.parse(response['autologon']);
    final id = _windowsPendingId, action = _windowsPendingAction;
    final expected = action == 'apply' ? 'configured' : 'disabled';
    if (id != null &&
        receipt.operationId == id &&
        receipt.operationState == 'partial' &&
        receipt.state == 'failed' &&
        receipt.canRepair) {
      await _load();
      if (mounted &&
          _windowsLogin.canRepair &&
          _windowsLogin.operationId == id) {
        setState(() {
          _windowsPendingId = null;
          _windowsPendingAction = null;
          _notice = l10n.changeStoppedMidwayRecoverSettings;
          _error = true;
        });
      }
      return;
    }
    if (id == null ||
        receipt.operationId != id ||
        receipt.operationState != expected) {
      await _load();
      if (mounted) {
        setState(() {
          _notice = l10n.couldNotConfirmWhetherRequest;
          _error = true;
        });
      }
      return;
    }
    await _load();
    if (!mounted) {
      return;
    }
    final current = _windowsLogin;
    final currentVerified =
        _admin &&
        current.administrator &&
        current.supported &&
        (action == 'apply'
            ? const {'configured', 'observed'}.contains(current.state)
            : current.state == 'off');
    setState(() {
      _windowsPendingId = null;
      _windowsPendingAction = null;
      _notice = currentVerified
          ? action == 'apply'
                ? l10n.turnedAppliesFromNextWindows
                : l10n.settingsMultitapChangedRecoveredAuto
          : l10n.requestFinishedButCurrentSetting;
      _error = !currentVerified;
    });
  }

  Future<void> _reconcileWindowsLogin() async {
    final id = _windowsPendingId;
    if (_busy || id == null || !_admin) {
      return;
    }
    setState(() => _busy = true);
    try {
      await _finishWindowsReceipt(
        await _api.request('windowsAutologonStatus', {'operationId': id}),
      );
    } catch (_) {
      if (mounted) {
        setState(() {
          _notice = l10n.resultNotConfirmedYetSame;
          _error = true;
        });
      }
    } finally {
      if (mounted) {
        setState(() => _busy = false);
        _scheduleApprovalPoll();
      }
    }
  }

  String get _savedWindowsAccount {
    final value = _section('windowsAccount')['username'];
    return value is String &&
            value.length <= 256 &&
            !RegExp(r'[\x00-\x1f\x7f]').hasMatch(value)
        ? value
        : '';
  }

  /// The saved account and the auto-login account name the same local user
  /// (`.\name`, `PC\name` and `name` compare equal, ignoring case).
  bool get _windowsAccountMatches {
    String key(String name) => name.trim().split(r'\').last.toLowerCase();
    final label = _windowsLogin.accountLabel, saved = _savedWindowsAccount;
    return _section('windowsAccount')['configured'] == true &&
        label.isNotEmpty &&
        saved.isNotEmpty &&
        key(label) == key(saved);
  }

  Widget _windowsDetail(String label, String value) => _info(label, value);

  // The auto-login state as one plain line: its words, then one hint.
  Widget _windowsStatusBanner(WindowsAutologonStatus status) => Padding(
    key: const Key('host-windows-state'),
    padding: const EdgeInsets.only(bottom: 8),
    child: Column(
      crossAxisAlignment: CrossAxisAlignment.start,
      children: [
        KeepAllText(
          _windowsStateLabel(status),
          style: TextStyle(
            color: _palette.ink,
            fontSize: 14,
            fontWeight: FontWeight.w500,
            height: 1.4,
          ),
        ),
        const SizedBox(height: 2),
        KeepAllText(
          status.state == 'external'
              ? status.canTakeOwnershipApply
                    ? l10n.whenYouSaveAccountHost
                    : status.supportsTakeOwnership
                    ? _windowsReason(status.reason)
                    : l10n.hostVersionCannotTakeOver
              : status.state == 'off'
              ? l10n.onceWindowsSignsAccountWhen
              : status.state == 'configured' || status.state == 'observed'
              ? l10n.windowsSignsAccountAutomaticallyWhen
              : status.reason != 'none'
              ? _windowsReason(status.reason)
              : l10n.refreshCheckCurrentSetting,
          style: TextStyle(fontSize: 13, color: _palette.muted, height: 1.5),
        ),
      ],
    ),
  );

  void _openAccountForm() {
    setState(() {
      _editAccount = true;
      _passwordSetup = null;
      _accountUsernameError = _accountPasswordError =
          _accountConfirmationError = null;
    });
  }

  // One Windows account (status windowsAccount + windowsAutologon): streaming
  // resumes its Windows login and automatic login signs it in. Saving the
  // account turns automatic login on; there is no OFF switch.
  // Not the cloud sign-in account.
  Widget _windowsLoginPanel() {
    final status = _windowsLogin;
    final account = _section('windowsAccount');
    final saved = account['configured'] == true;
    final on = const {'configured', 'observed'}.contains(status.state);
    final canTurnOn =
        status.canApply ||
        (status.state == 'external' && status.canTakeOwnershipApply);
    final enabled =
        !_busy && _admin && !_windowsConfirming && _windowsPendingId == null;
    final note = TextStyle(fontSize: 12, color: _palette.muted, height: 1.6);
    return _panel(
      key: const Key('host-windows-account'),
      title: l10n.windowsAccount3,
      subtitle: l10n.streamingAutoSignUseAccount,
      children: [
        _windowsStatusBanner(status),
        ..._rows([
          _info(
            l10n.savedAccount,
            saved ? _savedWindowsAccount : l10n.none,
            key: const Key('host-windows-saved-account'),
          ),
          if (status.accountLabel.isNotEmpty)
            _windowsDetail(l10n.autoSignAccount, status.accountLabel),
          if (status.enabled != null)
            _windowsDetail(
              l10n.windowsSetting,
              status.enabled! ? l10n.confirmed : l10n.offConfirmed,
            ),
        ]),
        if (saved && on && !_windowsAccountMatches)
          Padding(
            padding: const EdgeInsets.only(top: 6),
            child: KeepAllText(
              l10n.autoSignAccountDiffersFrom2,
              key: const Key('host-windows-account-mismatch'),
              style: TextStyle(
                fontSize: 12,
                color: _palette.amberInk,
                height: 1.6,
              ),
            ),
          ),
        if (status.state == 'external' && !status.supportsTakeOwnership)
          KeepAllText(
            l10n.updateHostThenCheckAgain,
            style: note,
          ),
        const SizedBox(height: 14),
        if (!saved || _editAccount)
          ..._accountForm(
            saved: saved,
            enabled: enabled,
            // Only what the state line above does not already say.
            hint: !canTurnOn
                ? _autologonUnavailable(status)
                : on
                ? l10n.savingAlsoSwitchesAutoSign
                : saved
                ? l10n.enterPasswordOnceMoreTurn
                : null,
            turnOn: canTurnOn,
          )
        else
          Wrap(
            spacing: 10,
            runSpacing: 8,
            children: [
              if (canTurnOn && !on)
                FilledButton.icon(
                  key: const Key('host-edit-account'),
                  onPressed: enabled ? _openAccountForm : null,
                  icon: const Icon(Icons.login_rounded, size: 18),
                  label: KeepAllText(l10n.turnAutoSign),
                )
              else
                OutlinedButton.icon(
                  key: const Key('host-edit-account'),
                  onPressed: enabled ? _openAccountForm : null,
                  icon: const Icon(Icons.edit_outlined, size: 17),
                  label: KeepAllText(l10n.changeAccount),
                ),
              TextButton.icon(
                key: const Key('host-clear-account'),
                onPressed: !_busy && _admin ? _clearAccount : null,
                icon: const Icon(Icons.delete_outline_rounded, size: 17),
                label: KeepAllText(l10n.clearSavedSignInfo),
              ),
            ],
          ),
        if (_windowsPendingId != null) ...[
          const SizedBox(height: 12),
          KeepAllText(
            l10n.unclearWhetherAutoSignFinished,
            style: note,
          ),
          const SizedBox(height: 6),
          OutlinedButton.icon(
            key: const Key('host-windows-result-check'),
            onPressed: !_busy && _admin ? _reconcileWindowsLogin : null,
            icon: const Icon(Icons.refresh_rounded),
            label: KeepAllText(l10n.checkRequestResult),
          ),
        ],
        if (status.canRepair) ...[
          const SizedBox(height: 10),
          OutlinedButton.icon(
            key: const Key('host-windows-repair'),
            onPressed: enabled ? () => _windowsChange('repair') : null,
            icon: const Icon(Icons.build_outlined),
            label: KeepAllText(l10n.recoverSettingsMultitapChanged2),
          ),
        ],
        if (account['error'] is String &&
            (account['error'] as String).isNotEmpty) ...[
          const SizedBox(height: 14),
          KeepAllText(
            l10n.savedWindowsAccountNeedsChecking,
            key: const Key('host-account-error'),
            style: TextStyle(fontSize: 12, color: _palette.amberInk),
          ),
        ],
        const SizedBox(height: 16),
        Divider(height: 1, color: _palette.line),
        _windowsDetail(
          l10n.currentWindowsSign,
          _state['interactiveSession'] == true
              ? l10n.signedNotLocked
              : l10n.checkSignLockState,
        ),
        if (status.observedAt != null)
          _windowsDetail(l10n.signChecked, status.observedAt!),
        const SizedBox(height: 8),
        // No in-stream sign-in screen (2026-11-09): a signed-out or locked
        // Windows is reached through the Proxmox console or a restart.
        KeepAllText(
          _forMachine(
            l10n.ifWindowsSignsOutLocks,
          ),
          style: note,
        ),
      ],
    );
  }

  List<Widget> _accountForm({
    required bool saved,
    required bool enabled,
    required String? hint,
    required bool turnOn,
  }) => [
    if (hint != null) ...[
      KeepAllText(
        hint,
        key: const Key('host-account-form-hint'),
        style: TextStyle(fontSize: 12.5, color: _palette.ink, height: 1.6),
      ),
      const SizedBox(height: 14),
    ],
    TextField(
      key: const Key('host-account-username'),
      controller: _accountUsername,
      enabled: enabled,
      maxLength: 256,
      autocorrect: false,
      enableSuggestions: false,
      onChanged: (_) => setState(() {
        _editAccount = true;
        _accountUsernameError = null;
        // A new password is only ever made for the account that was checked.
        _passwordSetup = null;
      }),
      decoration: InputDecoration(
        labelText: l10n.windowsAccount3,
        helperText: l10n.localUserNameComputerName,
        helperMaxLines: 2,
        errorText: _accountUsernameError,
        counterText: '',
        prefixIcon: const Icon(Icons.person_outline_rounded),
      ),
    ),
    if (_passwordSetup case final setup?) ...[
      const SizedBox(height: 18),
      KeepAllText(
        l10n.createNewPassword2,
        key: const Key('host-new-password-title'),
        style: TextStyle(
          fontSize: 13.5,
          fontWeight: FontWeight.w600,
          color: _palette.ink,
        ),
      ),
      const SizedBox(height: 4),
      KeepAllText(
        l10n.accountHasNoWindowsPassword(setup.minimumLength > 0 ? l10n.leastCharacters(setup.minimumLength) : ''),
        key: const Key('host-new-password-policy'),
        style: TextStyle(fontSize: 12.5, color: _palette.muted, height: 1.6),
      ),
    ],
    const SizedBox(height: 18),
    PasswordInputScope(
      builder: (focus) => TextField(
        key: const Key('host-account-password'),
        focusNode: focus,
        controller: _accountPassword,
        enabled: enabled,
        obscureText: true,
        autocorrect: false,
        enableSuggestions: false,
        maxLength: 256,
        onChanged: (_) => setState(() {
          _editAccount = true;
          _accountPasswordError = null;
          _accountConfirmationError = null;
        }),
        decoration: InputDecoration(
          labelText: _passwordSetup != null ? l10n.newWindowsPassword : l10n.windowsPassword,
          helperText: _passwordSetup != null ? null : l10n.leaveBlankSaveIfThere,
          errorText: _accountPasswordError,
          counterText: '',
          prefixIcon: const Icon(Icons.lock_outline_rounded),
        ),
      ),
    ),
    const SizedBox(height: 14),
    PasswordInputScope(
      builder: (focus) => TextField(
        key: const Key('host-account-password-confirmation'),
        focusNode: focus,
        controller: _accountPasswordConfirmation,
        enabled: enabled,
        obscureText: true,
        autocorrect: false,
        enableSuggestions: false,
        maxLength: 256,
        onChanged: (_) => setState(() {
          _editAccount = true;
          _accountConfirmationError = null;
        }),
        decoration: InputDecoration(
          labelText: _passwordSetup != null ? l10n.confirmNewPassword : l10n.confirmPassword,
          errorText: _accountConfirmationError,
          counterText: '',
          prefixIcon: const Icon(Icons.lock_outline_rounded),
        ),
      ),
    ),
    const SizedBox(height: 20),
    SizedBox(
      width: double.infinity,
      child: FilledButton.icon(
        key: const Key('host-save-account'),
        onPressed: enabled ? _saveAccount : null,
        icon: const Icon(Icons.verified_user_outlined, size: 18),
        label: KeepAllText(
          _passwordSetup != null
              ? l10n.createPasswordSave
              : turnOn
              ? l10n.saveTurnAutoSign
              : l10n.checkSaveAccount,
        ),
      ),
    ),
    if (saved) ...[
      const SizedBox(height: 6),
      Align(
        alignment: Alignment.centerRight,
        child: TextButton(
          key: const Key('host-cancel-account'),
          onPressed: _busy
              ? null
              : () async {
                  if (_accountDirty &&
                      !await _confirm(
                        l10n.discardWindowsAccountInfoYou,
                        l10n.savedWindowsAccountKept,
                        accept: l10n.discardChanges,
                      )) {
                    return;
                  }
                  if (!mounted) return;
                  setState(() {
                    _editAccount = false;
                    _passwordSetup = null;
                    _accountUsername.text =
                        '${_section('windowsAccount')['username'] ?? ''}';
                    _accountPassword.clear();
                    _accountPasswordConfirmation.clear();
                    _accountUsernameError = _accountPasswordError =
                        _accountConfirmationError = null;
                  });
                  unawaited(_syncClose());
                },
          child: KeepAllText(l10n.discardChanges),
        ),
      ),
    ],
  ];

  String _serviceText(Map<String, dynamic> service) =>
      switch (service['state']) {
        'starting' => l10n.startingService,
        'stopping' => l10n.serviceStopping,
        'paused' => l10n.servicePaused,
        'pausing' || 'continuing' => l10n.serviceChangingState,
        _ =>
          service['running'] == true
              ? l10n.serviceRunning
              : service['installed'] == false
              ? l10n.serviceNotInstalled
              : service['installed'] == true && service['running'] == false
              ? l10n.serviceStopped
              : l10n.serviceQueryNeeded,
      };

  String get _brokerStateText {
    final service = _section('service');
    if (service['running'] != true) {
      return service['running'] == false ? l10n.serviceStartNeeded : l10n.checkServiceStatus;
    }
    if (_approvalReadError) return l10n.connectionStatusDelayed;
    if (_brokerOnline) return l10n.serverConnectionConfirmed;
    return switch (_section('broker')['state']) {
      'connecting' => l10n.connectingServer2,
      'awaiting_ack' => l10n.waitingServerResponse,
      'reconnecting' => l10n.reconnectingServer,
      'stale' || 'online' => l10n.checkServerResponse,
      'waiting_registration' => l10n.checkServerRegistration,
      _ => l10n.checkServerConnection,
    };
  }

  String get _brokerGuidance {
    final service = _section('service');
    if (service['running'] != true) {
      return service['installed'] == false
          ? l10n.runHostInstallerAgainInstall2
          : service['running'] == false
          ? l10n.hostServiceOffStartService
          : l10n.couldNotReadServiceStatus;
    }
    if (_approvalReadError) {
      return l10n.couldNotReadLatestStatus;
    }
    if (_brokerOnline) {
      return _section('display')['ready'] == true &&
              _state['interactiveSession'] == true
          ? l10n.connectedServerYouCanConnect
          : l10n.connectedServerCheckVirtualDisplay;
    }
    final broker = _section('broker');
    return switch (broker['failureReason']) {
      'authorization_failed' ||
      'credentials_unavailable' ||
      'unregistered' => l10n.serverCouldNotConfirmHosts,
      'tls_failed' => l10n.couldNotVerifyServerCertificate,
      'network_unavailable' => l10n.couldNotConnectServerCheck,
      'server_unavailable' => l10n.serverNotRespondingTellAdministrator,
      'protocol_rejected' => l10n.hostServerVersionsDoNot,
      'heartbeat_timeout' ||
      'clock_changed' => l10n.couldNotConfirmRecentServer,
      'transport_closed' || 'send_failed' => l10n.serverConnectionDroppedServiceReconnects,
      _ => switch (broker['state']) {
        'connecting' || 'reconnecting' => l10n.serviceConnectingServerPleaseWait,
        'awaiting_ack' => l10n.connectedServerWaitingResponse,
        'stale' || 'online' => l10n.couldNotConfirmRecentServer2,
        _ => l10n.couldNotCheckServerConnection,
      },
    };
  }

  String _brokerAckTime(dynamic value) {
    if (value is! num ||
        !value.isFinite ||
        value < 0 ||
        value > 8640000000000000) {
      return l10n.noConfirmedResponse;
    }
    final time = DateTime.fromMillisecondsSinceEpoch(value.toInt()).toLocal();
    String pad(int n) => n.toString().padLeft(2, '0');
    return '${time.year}-${pad(time.month)}-${pad(time.day)} '
        '${pad(time.hour)}:${pad(time.minute)}:${pad(time.second)}';
  }

  String get _brokerAckAge {
    final age = _section('broker')['heartbeatAgeSeconds'];
    if (age is! num || !age.isFinite || age < 0 || age > 86400 * 365) {
      return l10n.noConfirmedResponse;
    }
    return l10n.sAgo((age + _statusAge.elapsedMilliseconds / 1000).floor());
  }

  Future<void> _copyDiagnostics() async {
    try {
      final broker = _section('broker');
      final age = broker['heartbeatAgeSeconds'];
      await Clipboard.setData(
        ClipboardData(
          text: hostDiagnosticsText({
            ..._state,
            'broker': {
              ...broker,
              'online': _brokerOnline,
              'state': !_registered
                  ? 'waiting_registration'
                  : _section('service')['running'] == false
                  ? 'stopped'
                  : _approvalReadError
                  ? 'unknown'
                  : !_brokerOnline && broker['state'] == 'online'
                  ? 'stale'
                  : broker['state'],
              'heartbeatAgeSeconds': age is num && age.isFinite && age >= 0
                  ? age + _statusAge.elapsedMilliseconds / 1000
                  : null,
              if (_approvalReadError) 'failureReason': 'status_unavailable',
            },
          }),
        ),
      );
      if (mounted) {
        showAppToast(context, l10n.diagnosticsCopied, kind: AppToastKind.done);
      }
    } catch (_) {
      if (mounted) {
        setState(() {
          _notice = l10n.couldNotCopyDiagnosticsTry;
          _error = true;
        });
      }
    }
  }

  // 진단 is support: a short summary and one primary action (copy for the
  // administrator). The technical rows stay behind 자세히, opened at once when
  // 연결 진단 brought the user here.
  Widget _diagnostics() {
    final s = _section('service'), d = _section('display');
    final broker = _section('broker');
    final outputs = (d['monitors'] as List? ?? []).length;
    String label(dynamic v, String yes, String no) => v == true
        ? yes
        : v == false
        ? no
        : l10n.queryNeeded;
    return Column(
      crossAxisAlignment: CrossAxisAlignment.stretch,
      children: [
        _serviceRow(s),
        _diagnosticsSummary(s, d, broker, outputs, label),
      ],
    );
  }

  // The Windows service everything rests on: its state and the one control
  // (start a stopped one, restart a running one).
  Widget _serviceRow(Map<String, dynamic> s) {
    final state = s['running'] == true
        ? HostSetupState.complete
        : s['running'] == false || s['installed'] == false
        ? HostSetupState.attention
        : HostSetupState.checking;
    final button = OutlinedButton.icon(
      key: const Key('host-service'),
      onPressed: _serviceControllable ? _service : null,
      icon: Icon(
        s['running'] == true
            ? Icons.restart_alt_rounded
            : Icons.play_arrow_rounded,
        size: 17,
      ),
      label: KeepAllText(s['running'] == true ? l10n.restartService : l10n.startService),
    );
    final info = Column(
      crossAxisAlignment: CrossAxisAlignment.start,
      children: [
        KeepAllText(
          l10n.hostService,
          style: TextStyle(
            fontSize: 14,
            fontWeight: FontWeight.w500,
            color: _palette.ink,
          ),
        ),
        const SizedBox(height: 4),
        HostSetupBadge(state: state, label: _serviceText(s)),
      ],
    );
    return _panel(
      key: const Key('host-service-card'),
      children: [
        LayoutBuilder(
          builder: (context, box) =>
              box.maxWidth / MediaQuery.textScalerOf(context).scale(1) < 320
              ? Column(
                  crossAxisAlignment: CrossAxisAlignment.start,
                  children: [info, const SizedBox(height: 12), button],
                )
              : Row(
                  children: [
                    Expanded(child: info),
                    const SizedBox(width: 12),
                    button,
                  ],
                ),
        ),
      ],
    );
  }

  Widget _diagnosticsSummary(
    Map<String, dynamic> s,
    Map<String, dynamic> d,
    Map<String, dynamic> broker,
    int outputs,
    String Function(dynamic v, String yes, String no) label,
  ) {
    return _panel(
      title: l10n.summary,
      subtitle: l10n.diagnosticsSentServerAutomaticallyPasswords,
      children: [
        Wrap(
          spacing: 8,
          runSpacing: 8,
          children: [
            FilledButton.icon(
              key: const Key('host-copy-diagnostics'),
              onPressed: _busy || _state.isEmpty ? null : _copyDiagnostics,
              icon: const Icon(Icons.copy_all_outlined, size: 18),
              label: KeepAllText(l10n.copyDiagnostics),
            ),
            OutlinedButton.icon(
              key: const Key('host-view-log'),
              onPressed: () => showProductLog(
                context,
                loadHost: () async {
                  final response = await _api.request('logs');
                  final events = response['events'];
                  if (events is! List || events.length > 192) {
                    throw const FormatException('Invalid host log');
                  }
                  return events
                      .map(ProductLogEntry.parse)
                      .whereType<ProductLogEntry>()
                      .toList();
                },
              ),
              icon: const Icon(Icons.receipt_long_outlined, size: 18),
              label: KeepAllText(l10n.viewLogs),
            ),
          ],
        ),
        const SizedBox(height: 10),
        ..._rows([
          _info(l10n.hostVersion2, '${_state['version'] ?? '—'}'),
          _info(l10n.service, _serviceText(s)),
          _info(l10n.serverConnection, _registered ? _brokerStateText : l10n.serverRegistrationNeeded),
          _info(
            l10n.windowsSign2,
            label(_state['interactiveSession'], l10n.signed, l10n.checkSign),
          ),
          _info(
            l10n.virtualDisplay2,
            _displayOutputReady
                ? l10n.ready5(outputs)
                : l10n.checkNeeded(outputs),
          ),
          if (s['queryError'] is num && s['queryError'] != 0)
            _info(l10n.serviceQueryError2, l10n.windowsError(s['queryError'])),
        ]),
        const SizedBox(height: 4),
        Divider(height: 1, color: _palette.line),
        _details(
          key: const Key('host-diagnostics-details'),
          initiallyExpanded: _diagnosticsDetails,
          onExpansionChanged: (open) => _diagnosticsDetails = open,
          children: _rows([
            _info(l10n.serviceInstall2, label(s['installed'], l10n.installed, l10n.notInstalled)),
            if (_registered) ...[
              _info(l10n.sinceServerResponse, _brokerAckAge),
              _info(l10n.lastConnected, _brokerAckTime(broker['lastReadyAt'])),
              _info(l10n.lastResponse, _brokerAckTime(broker['lastHeartbeatAt'])),
              _info(l10n.connectionGuidance, _brokerGuidance),
            ],
            _info(l10n.virtualDisplayDriver2, label(d['installed'], l10n.installed, l10n.notInstalled)),
            _info(l10n.virtualDisplayDevice2, label(d['deviceReady'], l10n.started, l10n.needsCheck)),
            _info(l10n.virtualMonitors, l10n.text22(outputs)),
            _info(l10n.screenRecovery, label(d['recoveryRequired'], l10n.needed, l10n.notNeeded)),
            _info(l10n.windowsRestart2, label(d['rebootRequired'], l10n.needed, l10n.notRequested2)),
            for (final row in _deviceRows)
              _info(
                row.title,
                [row.level.label, if (row.status.isNotEmpty) row.status]
                    .join(' · '),
              ),
          ]),
        ),
      ],
    );
  }

  // devices-v1: read-only. Each row is the observed state and, when something
  // is missing, the one next action; supporting detail stays behind 자세히.
  Widget _devices() {
    final rows = _deviceRows;
    return _panel(
      key: const Key('host-devices'),
      children: [
        if (rows.isEmpty)
          Padding(
            padding: const EdgeInsets.symmetric(vertical: 12),
            child: KeepAllText(
              l10n.couldNotReadDeviceStatus,
              key: const Key('host-devices-unavailable'),
              style: TextStyle(
                color: _palette.red,
                fontSize: 13,
                height: 1.5,
              ),
            ),
          ),
        for (var index = 0; index < rows.length; index++) ...[
          if (index > 0) Divider(height: 1, color: _palette.line),
          _deviceRow(rows[index]),
        ],
        Divider(height: 1, color: _palette.line),
        _details(
          key: const Key('host-devices-help'),
          children: [
            _detailText(
              l10n.permissionsTurnedPerUserDesktop,
            ),
          ],
        ),
      ],
    );
  }

  Widget _disabledReason(String? reason, Widget child) =>
      reason == null ? child : Tooltip(message: reason, child: child);

  // WinFsp installs a driver: one confirmation that says a manual restart may
  // follow. Starting a stopped spooler is explained in its row and acts at once
  // (its start type is never changed); a dialog appears only on failure.
  Future<void> _deviceAction(HostDeviceAction action) async {
    if (action == HostDeviceAction.installWinfsp) {
      if (!await _confirm(
        l10n.installWinfsp3,
        l10n.installsSharedFolderDriverWinfsp,
        accept: l10n.install,
      )) {
        return;
      }
      await _action(
        'installWinfsp',
        {'confirmed': true},
        l10n.installingWinfsp,
        '',
        outcome: hostWinfspInstallResult,
        phase: l10n.installingWinfspMayTake1,
      );
      return;
    }
    if (action == HostDeviceAction.replaceUsbController) {
      // Another product's controller: one confirmation that names what stops.
      if (!await _confirm(
        l10n.switchUsbConnectionMultitap,
        l10n.usbConnectionFromOtherPrograms,
        accept: l10n.switchLabel,
        danger: true,
      )) {
        return;
      }
      await _action(
        'replaceUsbController',
        {'confirmed': true},
        l10n.switchingUsbConnection,
        '',
        outcome: hostUsbReplaceResult,
        phase: l10n.switchingUsbConnectionMultitapDo,
      );
      return;
    }
    await _action(
      'startPrintSpooler',
      {},
      l10n.startingPrintSpooler,
      '',
      outcome: hostSpoolerStartResult,
      notify: false,
      phase: l10n.startingWindowsPrintSpooler,
    );
  }

  // display-adapters-v1: one line on the 가상 화면 page; 정리 lists the exact
  // nodes and removes them only after one confirmation (never the adapter in use
  // or another company's). Automatic cleanup handles the provable ones itself.
  Widget _displayAdapterLine() {
    final adapters = HostDisplayAdapters.parse(_state['displayAdapters']);
    return Padding(
      padding: const EdgeInsets.only(top: 14),
      child: Row(
        crossAxisAlignment: CrossAxisAlignment.center,
        children: [
          Icon(
            Icons.developer_board_outlined,
            size: 18,
            color: adapters.leftover > 0 ? _palette.amber : _palette.muted,
          ),
          const SizedBox(width: 8),
          Expanded(
            child: KeepAllText(
              adapters.summary,
              key: const Key('host-display-adapters'),
              style: TextStyle(color: _palette.muted, fontSize: 12),
            ),
          ),
          if (adapters.extras.isNotEmpty)
            _disabledReason(
              _admin ? null : l10n.openSettingsAsAdministrator,
              TextButton.icon(
                key: const Key('host-display-adapters-clean'),
                onPressed: _busy || !_admin
                    ? null
                    : () => unawaited(_cleanDisplayAdapters(adapters)),
                icon: const Icon(Icons.cleaning_services_outlined, size: 17),
                label: KeepAllText(l10n.cleanUp),
              ),
            ),
        ],
      ),
    );
  }

  Future<void> _cleanDisplayAdapters(HostDisplayAdapters adapters) async {
    final list = adapters.extras
        .map((extra) => '• ${extra.id} · ${extra.description}')
        .join('\n');
    if (!await _confirm(
      l10n.remove(adapters.extras.length),
      l10n.adapterUseDevicesFromOther(list, adapters.unproven > 0 ? l10n.devicesWithoutMultitapRecordMay : ''),
      accept: l10n.clear,
      danger: true,
    )) {
      return;
    }
    await _action(
      'cleanDisplayAdapters',
      {
        'confirmed': true,
        'ids': [for (final extra in adapters.extras) extra.id],
      },
      l10n.cleaningUpVirtualDisplayDevices,
      '',
      outcome: hostAdapterCleanupResult,
      phase: l10n.removingOnlyCheckedDevicesOne,
    );
  }

  HostSetupState _deviceState(HostDeviceLevel level) => switch (level) {
    HostDeviceLevel.ready => HostSetupState.complete,
    HostDeviceLevel.setup => HostSetupState.setup,
    HostDeviceLevel.attention => HostSetupState.attention,
    _ => HostSetupState.available,
  };

  // One device feature on one line: its name · its state word · one fact
  // ("공유 폴더 · 준비됨 · WinFsp 사용"). Only when something is missing, the
  // next step as one sentence and the one action Host settings can take.
  Widget _deviceRow(HostDeviceRow row) {
    final state = _deviceState(row.level);
    final separator = KeepAllText(
      '·',
      style: TextStyle(color: _palette.faint, fontSize: 14, height: 1.4),
    );
    return Semantics(
      container: true,
      label: '${row.title}, ${row.level.label}',
      child: Padding(
        key: Key('host-device-${row.id}'),
        padding: const EdgeInsets.symmetric(vertical: 12),
        child: Column(
          crossAxisAlignment: CrossAxisAlignment.start,
          children: [
            Wrap(
              spacing: 6,
              runSpacing: 2,
              crossAxisAlignment: WrapCrossAlignment.center,
              children: [
                KeepAllText(
                  row.title,
                  style: TextStyle(
                    fontSize: 14,
                    height: 1.4,
                    fontWeight: FontWeight.w500,
                    color: _palette.ink,
                  ),
                ),
                separator,
                KeepAllText(
                  row.level.label,
                  key: Key('host-device-${row.id}-level'),
                  style: TextStyle(
                    fontSize: 14,
                    height: 1.4,
                    fontWeight: FontWeight.w500,
                    color: state.ink(_palette),
                  ),
                ),
                if (row.status.isNotEmpty) ...[
                  separator,
                  KeepAllText(
                    _forMachine(row.status),
                    key: Key('host-device-${row.id}-status'),
                    style: TextStyle(
                      color: _palette.muted,
                      fontSize: 14,
                      height: 1.4,
                    ),
                  ),
                ],
              ],
            ),
            if (row.next != null) ...[
              const SizedBox(height: 2),
              KeepAllText(
                _forMachine(row.next!),
                key: Key('host-device-${row.id}-next'),
                style: TextStyle(
                  color: _palette.ink,
                  fontSize: 13,
                  height: 1.5,
                ),
              ),
            ],
            if (row.action case final action?) ...[
              const SizedBox(height: 10),
              _disabledReason(
                _admin ? null : l10n.openSettingsAsAdministrator,
                FilledButton.tonalIcon(
                  key: Key('host-device-${row.id}-action'),
                  onPressed: _busy || !_admin
                      ? null
                      : () => unawaited(_deviceAction(action)),
                  icon: Icon(switch (action) {
                    HostDeviceAction.installWinfsp => Icons.download_rounded,
                    HostDeviceAction.startSpooler => Icons.play_arrow_rounded,
                    HostDeviceAction.replaceUsbController =>
                      Icons.swap_horiz_rounded,
                  }, size: 18),
                  label: KeepAllText(action.label),
                ),
              ),
            ],
          ],
        ),
      ),
    );
  }

  /// One plain section (no card): an optional small heading and a one- or
  /// two-line note, then the section's rows and actions. Sections are parted
  /// by space alone.
  Widget _panel({
    Key? key,
    String? title,
    String? subtitle,
    required List<Widget> children,
  }) => Padding(
    key: key,
    padding: const EdgeInsets.only(bottom: 28),
    child: Column(
      crossAxisAlignment: CrossAxisAlignment.start,
      children: [
        if (title != null)
          KeepAllText(
            title,
            style: TextStyle(
              color: _palette.ink,
              fontWeight: FontWeight.w600,
              fontSize: 15,
              height: 1.35,
            ),
          ),
        if (subtitle != null)
          Padding(
            padding: const EdgeInsets.only(top: 4),
            child: KeepAllText(
              subtitle,
              style: TextStyle(
                color: _palette.muted,
                fontSize: 13,
                height: 1.55,
              ),
            ),
          ),
        if (title != null || subtitle != null) const SizedBox(height: 12),
        ...children,
      ],
    ),
  );

  /// A readable row: the label, then the value.
  Widget _info(String name, String value, {Key? key}) => Padding(
    key: key,
    padding: const EdgeInsets.symmetric(vertical: 9),
    child: Row(
      crossAxisAlignment: CrossAxisAlignment.start,
      children: [
        SizedBox(
          // The label column grows with large text, up to half as wide again.
          width:
              116 * MediaQuery.textScalerOf(context).scale(1).clamp(1.0, 1.5),
          child: KeepAllText(
            name,
            style: TextStyle(
              color: _palette.muted,
              fontSize: 13,
              height: 1.45,
            ),
          ),
        ),
        const SizedBox(width: 8),
        Expanded(
          child: SelectableText(
            value,
            style: TextStyle(
              color: _palette.ink,
              fontSize: 13,
              height: 1.45,
              fontWeight: FontWeight.w500,
            ),
          ),
        ),
      ],
    ),
  );

  /// Rows separated by hairlines.
  List<Widget> _rows(List<Widget> rows) => [
    for (var i = 0; i < rows.length; i++) ...[
      if (i > 0) Divider(height: 1, color: _palette.line),
      rows[i],
    ],
  ];

  /// The labelled 자세히 control: supporting detail stays folded until asked.
  Widget _details({
    required Key key,
    required List<Widget> children,
    bool initiallyExpanded = false,
    ValueChanged<bool>? onExpansionChanged,
  }) => ExpansionTile(
    key: key,
    initiallyExpanded: initiallyExpanded,
    onExpansionChanged: onExpansionChanged,
    dense: true,
    tilePadding: EdgeInsets.zero,
    childrenPadding: const EdgeInsets.only(bottom: 8),
    expandedCrossAxisAlignment: CrossAxisAlignment.stretch,
    shape: const Border(),
    collapsedShape: const Border(),
    iconColor: _palette.amberInk,
    collapsedIconColor: _palette.amberInk,
    title: KeepAllText(
      l10n.details,
      style: TextStyle(
        fontSize: 13,
        fontWeight: FontWeight.w600,
        color: _palette.amberInk,
      ),
    ),
    children: children,
  );

  Widget _detailText(String text) => KeepAllText(
    text,
    style: TextStyle(color: _palette.muted, fontSize: 12.5, height: 1.6),
  );
}
