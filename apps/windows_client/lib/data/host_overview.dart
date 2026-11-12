
/// The Host settings overview (개요): one status, one problem.
///
/// Everything here is derived from what the settings window already reads
/// (the Host tool's `status` and `prerequisiteStatus`). Nothing is guessed:
/// what the status does not say (for example whether a client is streaming
/// right now) has no state here.
library;

import '../l10n/l10n.dart';

/// What needs the user, most important first. The order of the values IS the
/// priority: the overview shows only the first one that applies.
///
/// 1. What Windows itself must have before anything else can work: the Host
///    service, the finished install, a pending Windows restart, an unfinished
///    display recovery.
/// 2. The route in menu order: the server, the Windows sign-in, the virtual
///    screen.
/// 3. What does not stop a stream now but is not set up: automatic login, the
///    optional device features.
enum HostProblem {
  /// The status could not be read at all.
  statusUnread,
  serviceMissing,
  serviceStopped,

  /// Windows did not answer the service query.
  serviceUnknown,

  /// The virtual display driver is missing and this Host can install it.
  installUnfinished,

  /// The driver is missing and cannot be installed from here.
  installBlocked,
  restartRequired,
  displayRecovery,
  serverUnregistered,
  serverApproval,
  serverOffline,

  /// No unlocked interactive Windows session (signed out or locked: the
  /// status has one flag for both).
  windowsSignedOut,

  /// An RDP login that can be moved to the streaming screen.
  displayTransfer,

  /// The service is still bringing the virtual screen up.
  displayPreparing,
  displayNotReady,

  // From here on a stream works now.
  loginSetup,
  loginAttention,
  winfspMissing,
  devicesAttention;

  /// A client cannot stream this desktop until this is solved.
  bool get blocksStreaming => index <= displayNotReady.index;

  /// The settings page that solves it (the stable page numbers of
  /// HostSettingsNavigation: 1 server, 2 virtual screen, 4 diagnostics,
  /// 5 Windows login, 6 devices).
  int get page => switch (this) {
    statusUnread || serviceMissing || serviceStopped || serviceUnknown => 4,
    installUnfinished ||
    installBlocked ||
    restartRequired ||
    displayRecovery ||
    displayTransfer ||
    displayPreparing ||
    displayNotReady => 2,
    serverUnregistered || serverApproval || serverOffline => 1,
    windowsSignedOut || loginSetup || loginAttention => 5,
    winfspMissing || devicesAttention => 6,
  };

  /// The one status beside the desktop's name, in status words.
  String get label => switch (this) {
    statusUnread => l10n.couldntReadStatus,
    serviceMissing => l10n.hostServiceMissing,
    serviceStopped => l10n.hostServiceOff,
    serviceUnknown => l10n.checkHostService,
    installUnfinished || winfspMissing => l10n.installationNeedsFinishing,
    installBlocked => l10n.checkVirtualDisplayDriver,
    restartRequired => l10n.restartWindows,
    displayRecovery => l10n.virtualDisplayNeedsRecovery,
    serverUnregistered => l10n.serverConnectionNeeded,
    serverApproval => l10n.administratorApprovalNeeded,
    serverOffline => l10n.notConnectedServer,
    windowsSignedOut => l10n.windowsSignNeeded,
    displayTransfer => l10n.streamingDisplaySwitchNeeded,
    displayPreparing => l10n.preparingVirtualDisplay2,
    displayNotReady => l10n.checkVirtualDisplay,
    loginSetup => l10n.autoSignSetupNeeded,
    loginAttention => l10n.checkWindowsSign,
    devicesAttention => l10n.checkDevices,
  };
}

/// What the settings window knows right now, as plain facts.
class HostFacts {
  const HostFacts({
    this.loaded = false,
    this.readFailed = false,
    this.serviceInstalled,
    this.serviceRunning,
    this.displayMissing = false,
    this.displayBlocked = false,
    this.restartRequired = false,
    this.recoveryRequired = false,
    this.registered = false,
    this.awaitingApproval = false,
    this.cancellingConnection = false,
    this.brokerOnline = false,
    this.interactive = false,
    this.displayReady = false,
    this.transferEligible = false,
    this.displayPreparing = false,
    this.loginSetup = false,
    this.loginAttention = false,
    this.winfspMissing = false,
    this.devicesAttention = false,
  });

  /// A status has been read. Until then nothing is known and nothing is
  /// reported as a problem, except [readFailed].
  final bool loaded;

  /// No status and the read failed.
  final bool readFailed;

  /// Null: Windows did not answer the service query.
  final bool? serviceInstalled, serviceRunning;

  /// No virtual display driver; [displayMissing] when this Host can install
  /// it, [displayBlocked] when it cannot be installed from here.
  final bool displayMissing, displayBlocked;
  final bool restartRequired, recoveryRequired;
  final bool registered, awaitingApproval, cancellingConnection;

  /// A registered Host whose service holds a fresh server acknowledgement.
  final bool brokerOnline;

  /// An unlocked interactive Windows session exists.
  final bool interactive;
  final bool displayReady, transferEligible, displayPreparing;

  /// Automatic login is not set up ([loginSetup]) or needs a look
  /// ([loginAttention]).
  final bool loginSetup, loginAttention;

  /// WinFsp is missing and this Host carries its installer.
  final bool winfspMissing;
  final bool devicesAttention;
}

/// Every problem that applies, most important first.
List<HostProblem> hostProblems(HostFacts f) {
  if (!f.loaded) {
    return f.readFailed ? const [HostProblem.statusUnread] : const [];
  }
  final connecting = f.awaitingApproval || f.cancellingConnection;
  return [
    if (f.serviceInstalled == false)
      HostProblem.serviceMissing
    else if (f.serviceRunning == false)
      HostProblem.serviceStopped
    else if (f.serviceRunning == null)
      HostProblem.serviceUnknown,
    if (f.displayMissing)
      HostProblem.installUnfinished
    else if (f.displayBlocked)
      HostProblem.installBlocked,
    if (f.restartRequired) HostProblem.restartRequired,
    if (f.recoveryRequired) HostProblem.displayRecovery,
    if (connecting)
      HostProblem.serverApproval
    else if (!f.registered)
      HostProblem.serverUnregistered
    else if (!f.brokerOnline)
      HostProblem.serverOffline,
    if (!f.interactive) HostProblem.windowsSignedOut,
    if (!f.displayReady &&
        !f.displayMissing &&
        !f.displayBlocked &&
        !f.restartRequired &&
        !f.recoveryRequired &&
        f.interactive)
      f.transferEligible
          ? HostProblem.displayTransfer
          : f.displayPreparing
          ? HostProblem.displayPreparing
          : HostProblem.displayNotReady,
    if (f.loginAttention)
      HostProblem.loginAttention
    else if (f.loginSetup)
      HostProblem.loginSetup,
    if (f.winfspMissing) HostProblem.winfspMissing,
    if (f.devicesAttention) HostProblem.devicesAttention,
  ]..sort((a, b) => a.index.compareTo(b.index));
}

/// The single most important problem, or null when there is none (or nothing
/// is known yet).
HostProblem? hostMainProblem(HostFacts f) {
  final all = hostProblems(f);
  return all.isEmpty ? null : all.first;
}

/// A client can stream this desktop now: the status was read and nothing
/// that blocks a stream applies.
bool hostStreamable(HostFacts f) =>
    f.loaded && !hostProblems(f).any((problem) => problem.blocksStreaming);

/// The tone of a status: fine, needs the user, or not known yet.
enum HostRouteState { ok, warn, idle }

/// What kind of machine this Host is, as far as the settings status tells.
/// The status has no field for it; the only trace is the desktop id the
/// server gave at approval: `pve-…` for a Proxmox VM (host-requests.mjs) and
/// `pc-<uuid>` for a 로컬 PC (local-pc.mjs). Any other id says nothing.
enum HostMachineKind { vm, pc, unknown }

HostMachineKind hostMachineKind(Object? desktopId) {
  if (desktopId is! String) return HostMachineKind.unknown;
  if (RegExp(
    r'^pc-[0-9a-f]{8}(?:-[0-9a-f]{4}){3}-[0-9a-f]{12}$',
  ).hasMatch(desktopId)) {
    return HostMachineKind.pc;
  }
  return RegExp(r'^pve-[A-Za-z0-9._-]{1,120}$').hasMatch(desktopId)
      ? HostMachineKind.vm
      : HostMachineKind.unknown;
}

/// The muted line under the desktop's name. A local PC's id is a long UUID
/// that helps nobody, so it is not shown.
String hostMachineLine(Object? desktopId, {required bool named}) {
  final id = desktopId is String ? desktopId : '';
  return switch (hostMachineKind(desktopId)) {
    HostMachineKind.vm => named ? 'Proxmox VM · $id' : 'Proxmox VM',
    HostMachineKind.pc => l10n.localPc,
    HostMachineKind.unknown =>
      named && id.isNotEmpty ? l10n.desktopRegisteredServer(id) : l10n.desktopRegisteredServer2,
  };
}

/// The server as a short name: the host, with the port only when it is not
/// the scheme's own. Anything that is not an http(s) origin shows nothing.
String hostServerName(Object? origin) {
  if (origin is! String || origin.isEmpty) return '';
  final uri = Uri.tryParse(origin);
  if (uri == null ||
      uri.host.isEmpty ||
      !const {'http', 'https'}.contains(uri.scheme)) {
    return '';
  }
  final standard = uri.scheme == 'https' ? 443 : 80;
  return uri.hasPort && uri.port != standard
      ? '${uri.host}:${uri.port}'
      : uri.host;
}

/// How long ago the server last acknowledged this Host, in words. Only
/// called while that acknowledgement is fresh (at most 30 s old).
String hostAckWords(num ageSeconds) =>
    ageSeconds < 10 ? l10n.checkedJustNow : l10n.checkedSAgo(ageSeconds.floor());
