import 'package:flutter/material.dart';

import 'desktop_use.dart';
import 'stream_options.dart';
import 'stream_preparation.dart';
import 'virtual_display.dart';
import 'vm_resources.dart';
import 'console_preparation.dart';
import 'session_cleanup.dart';
import 'host_update.dart';
import 'desktop_organize.dart';
import 'thumbnail_privacy.dart';
import 'pc_power.dart';
import '../data/signin_stream.dart' show SigninAvailability;
import '../l10n/l10n.dart';

export 'desktop_use.dart';
export 'pc_power.dart' show PcPowerState;
export 'desktop_organize.dart' show DesktopOrganize;

enum MachineKind { pc, vm }

enum MachineStatus { ready, poweredOff, awaitingHost }

class DesktopMachine {
  const DesktopMachine({
    required this.id,
    required this.name,
    required this.description,
    required this.kind,
    required this.status,
    required this.specification,
    required this.accent,
    this.signinAvailability,
    this.consoleInUse = false,
    this.qemuAgentOff = false,
    this.consoleRelayProtocol,
    this.consoleBrowserProtocol,
    this.vmAudio = false,
    this.audioState,
    this.localPcApproved = false,
    this.monitors = const [],
    this.hostOnline,
    this.thumbnailPrivacy,
    this.interactiveSession,
    this.virtualDisplayReady,
    this.streamPreparation,
    this.powerState = 'unknown',
    this.powerCheckedAt,
    this.powerStale = true,
    this.powerBusy = false,
    this.powerCanForceStop = false,
    this.powerError,
    this.powerAction,
    this.powerTaskStatus,
    this.powerExitStatus,
    this.vmState,
    this.vmStateReason,
    this.vmStateRevision = 0,
    this.display = const VirtualDisplayState(),
    this.virtualMonitorControl = false,
    this.independentDisplayStreaming = false,
    this.initialViewport = false,
    this.viewportEncoderLimits = false,
    this.streamSize = false,
    this.streamSizeV2 = false,
    this.driveUnavailable,
    this.printUnavailable,
    this.profileTracks = false,
    this.resources = const VmResources(),
    this.consolePreparation = const ConsolePreparationState(),
    this.sessionCleanup = const SessionCleanupState(),
    this.maxBitrate,
    this.hostUpdate = HostUpdateState.absent,
    this.hostMissingAfterUpdate,
    this.use = DesktopUse.idle,
    this.useAccount,
    this.organize = DesktopOrganize.none,
    this.pcPower = PcPowerState.absent,
  });

  final String id;
  final String name;
  final String description;
  final MachineKind kind;
  final MachineStatus status;
  final String specification;
  final Color accent;
  final String? consoleRelayProtocol;
  final String? consoleBrowserProtocol;
  bool get browserConsoleSupported =>
      kind == MachineKind.vm &&
      RegExp(r'^[a-zA-Z0-9][a-zA-Z0-9_-]{0,79}$').hasMatch(id) &&
      consoleBrowserProtocol == 'console-browser-v1';
  bool get hasEmergencyConsole =>
      emergencyConsoleSupported || browserConsoleSupported;
  final bool vmAudio;

  /// The VM's sound card as the broker last read it: absent, present or
  /// pending; null when unknown (no card badge then).
  final String? audioState;
  final bool localPcApproved;
  bool get emergencyConsoleSupported =>
      kind == MachineKind.vm &&
      RegExp(r'^pve-[1-9][0-9]{2,11}$').hasMatch(id) &&
      consoleRelayProtocol == 'rfb-none-v1';
  final List<RemoteMonitor> monitors;
  final StreamPreparation? streamPreparation;
  final ThumbnailPrivacy? thumbnailPrivacy;
  final bool? hostOnline, interactiveSession, virtualDisplayReady;
  final String powerState;
  final DateTime? powerCheckedAt;
  final bool powerStale, powerBusy, powerCanForceStop;
  final String? powerError, powerAction, powerTaskStatus, powerExitStatus;

  /// The broker's VM state machine (server/src/vm-state.mjs): the
  /// state this tile renders, a fixed reason code and a revision that only
  /// grows. Null from a broker before it (the tile then derives from the
  /// power and Host fields as before).
  final String? vmState, vmStateReason;
  final int vmStateRevision;
  final VirtualDisplayState display;
  final bool virtualMonitorControl;
  final bool independentDisplayStreaming;
  final bool initialViewport;
  final bool viewportEncoderLimits;

  /// The Host and broker support stream-size-v1: full-size pictures within
  /// this client's decoder limits and portrait outputs up to 2160x3840.
  final bool streamSize;
  final bool streamSizeV2;

  /// A fixed reason the Host cannot share drives although the assignment allows
  /// them (drive_webclient_unavailable: the VM's WebClient service is
  /// disabled or missing); null when drives work or are not allowed.
  final String? driveUnavailable;

  /// A fixed reason the VM cannot offer its PC printer although the
  /// assignment allows printers (print_spooler_unavailable,
  /// print_driver_unavailable); null otherwise.
  final String? printUnavailable;
  final bool profileTracks;

  /// Console-profile group negotiation belongs to Proxmox VMs. Approved PCs
  /// use ordinary owned-output tracks even when their Host advertises it.
  bool get requiresProfileTracks => kind == MachineKind.vm && profileTracks;
  final VmResources resources;

  /// CPU/RAM/uptime describe the desktop now: a VM Proxmox reports running,
  /// or a PC whose Host is connected (it measures them itself).
  bool get resourcesRunning => kind == MachineKind.pc
      ? hostOnline == true
      : powerState == 'running' && !powerStale;
  final ConsolePreparationState consolePreparation;
  final SessionCleanupState sessionCleanup;

  /// bitrate-adaptive-v1 ceiling (automatic and manual up to it), or null for
  /// a released broker/host (manual 4-20 Mbps).
  final int? maxBitrate;
  int get bitrateCeiling => maxBitrate ?? StreamOptions.legacyMaximumBitrate;

  /// Administrator accounts only; [HostUpdateState.absent] for everyone else.
  final HostUpdateState hostUpdate;

  /// The Host went away with the last update attempt and stayed away while
  /// the desktop runs (broker hostMissingAfterUpdate); null otherwise.
  final HostMissingAfterUpdate? hostMissingAfterUpdate;

  /// A connection holds the VM now (display only).
  final DesktopUse use;

  /// Who the other connection belongs to, when the broker names it.
  final String? useAccount;

  /// Proxmox tags and pool, this account's favourite and last use (desktop
  /// organising; empty from a broker before it).
  final DesktopOrganize organize;

  /// An approved PC's power (pc-power-v1): what its Host or the server's
  /// Wake-on-LAN can do now. [PcPowerState.absent] for VMs.
  final PcPowerState pcPower;
  bool get canRecoverConsole =>
      kind == MachineKind.vm &&
      sessionCleanup.allowsEmergencyControl &&
      hostOnline == true &&
      consolePreparation.supported &&
      consolePreparation.available &&
      !consolePreparation.busy &&
      !display.busy &&
      !powerBusy &&
      status != MachineStatus.poweredOff;

  /// Fresh read-only owned topology, independent of permission to change it.
  /// The broker's stale flag is authoritative; this PC's clock is not.
  int? get verifiedOwnedDisplayCount {
    final count = display.activeCount;
    if (hostOnline != true ||
        virtualDisplayReady != true ||
        status == MachineStatus.poweredOff ||
        powerBusy ||
        sessionCleanup.pending ||
        display.stale ||
        display.busy ||
        display.error != null ||
        display.checkedAt == null ||
        (count != 1 && count != 2) ||
        monitors.length != count) {
      return null;
    }
    final ids = monitors.map((monitor) => monitor.id).toSet();
    return ids.length == count &&
            ids.contains('0') &&
            (count == 1 || ids.contains('1'))
        ? count
        : null;
  }

  bool get canConfigureDisplays =>
      !sessionCleanup.pending &&
      virtualMonitorControl &&
      display.supported &&
      hostOnline == true &&
      !display.stale &&
      !display.busy &&
      !powerBusy &&
      status != MachineStatus.poweredOff;

  bool get canStart => kind == MachineKind.pc
      ? _pcCan('start')
      : kind == MachineKind.vm &&
            !powerStale &&
            !powerBusy &&
            !display.busy &&
            powerState == 'stopped';
  bool get canShutdown => kind == MachineKind.pc
      ? _pcCan('shutdown')
      : kind == MachineKind.vm &&
            !powerStale &&
            !powerBusy &&
            !display.busy &&
            powerState == 'running';
  bool get canReboot => kind == MachineKind.pc ? _pcCan('reboot') : canShutdown;

  /// Why this client refuses a PC power [action] before sending it (null:
  /// send it); [pending] is this client's own request still running.
  String? pcPowerRefusal(String action, {bool pending = false}) =>
      pending || powerBusy
      ? 'power_in_progress'
      : !pcPower.supported ||
            !const {'start', 'shutdown', 'reboot'}.contains(action)
      ? 'invalid_power_action'
      : action == 'start' && hostOnline != true && !pcPower.wake
      ? 'pc_wake_unavailable'
      : !_pcCan(action)
      ? 'invalid_power_state'
      : null;

  /// A PC action the broker offers now: 켜기 only while the Host is offline,
  /// 종료 and 다시 시작 only while it is online.
  bool _pcCan(String action) =>
      pcPower.actions.contains(action) &&
      !powerBusy &&
      !display.busy &&
      (action == 'start' ? hostOnline != true : hostOnline == true);

  /// 정지 (Proxmox stop, immediate power-off): while the broker says this
  /// account may stop the VM now ([powerCanForceStop]): a running VM that is
  /// idle, or still in this account's own 종료 (a shutdown Windows does not
  /// finish). A broker before client stop (2026-10-26) never reports it, so
  /// 정지 stays unavailable there.
  bool get canStop =>
      kind == MachineKind.vm &&
      powerCanForceStop &&
      !powerStale &&
      !display.busy &&
      powerState == 'running';
  DesktopMachine withPower({
    required String state,
    required DateTime? checkedAt,
    required bool stale,
    required bool busy,
    bool canForceStop = false,
    String? error,
    String? action,
    String? taskStatus,
    String? exitStatus,
  }) => DesktopMachine(
    id: id,
    name: name,
    description: description,
    kind: kind,
    status: state == 'stopped' && !stale
        ? MachineStatus.poweredOff
        : state == 'running' && !stale && status == MachineStatus.poweredOff
        ? MachineStatus.awaitingHost
        : status,
    specification: specification,
    accent: accent,
    consoleRelayProtocol: consoleRelayProtocol,
    signinAvailability: signinAvailability,
    consoleInUse: consoleInUse,
    qemuAgentOff: qemuAgentOff,
    consoleBrowserProtocol: consoleBrowserProtocol,
    vmAudio: vmAudio,
    audioState: audioState,
    localPcApproved: localPcApproved,
    monitors: monitors,
    hostOnline: hostOnline,
    thumbnailPrivacy: thumbnailPrivacy,
    interactiveSession: interactiveSession,
    virtualDisplayReady: virtualDisplayReady,
    streamPreparation: streamPreparation,
    consolePreparation: consolePreparation,
    sessionCleanup: sessionCleanup,
    powerState: state,
    powerCheckedAt: checkedAt,
    powerStale: stale,
    powerBusy: busy,
    powerCanForceStop: canForceStop,
    powerError: error,
    powerAction: action,
    powerTaskStatus: taskStatus,
    powerExitStatus: exitStatus,
    vmState: vmState,
    vmStateReason: vmStateReason,
    vmStateRevision: vmStateRevision,
    display: display,
    virtualMonitorControl: virtualMonitorControl,
    independentDisplayStreaming: independentDisplayStreaming,
    initialViewport: initialViewport,
    viewportEncoderLimits: viewportEncoderLimits,
    streamSize: streamSize,
    streamSizeV2: streamSizeV2,
    driveUnavailable: driveUnavailable,
    printUnavailable: printUnavailable,
    profileTracks: profileTracks,
    resources: resources,
    maxBitrate: maxBitrate,
    hostUpdate: hostUpdate,
    hostMissingAfterUpdate: hostMissingAfterUpdate,
    use: use,
    useAccount: useAccount,
    organize: organize,
    pcPower: pcPower,
  );

  DesktopMachine withDisplay(VirtualDisplayState state) => DesktopMachine(
    id: id,
    name: name,
    description: description,
    kind: kind,
    status: status,
    specification: specification,
    accent: accent,
    consoleRelayProtocol: consoleRelayProtocol,
    signinAvailability: signinAvailability,
    consoleInUse: consoleInUse,
    qemuAgentOff: qemuAgentOff,
    consoleBrowserProtocol: consoleBrowserProtocol,
    vmAudio: vmAudio,
    audioState: audioState,
    localPcApproved: localPcApproved,
    monitors: monitors,
    hostOnline: hostOnline,
    thumbnailPrivacy: thumbnailPrivacy,
    interactiveSession: interactiveSession,
    virtualDisplayReady: virtualDisplayReady,
    streamPreparation: streamPreparation,
    consolePreparation: consolePreparation,
    sessionCleanup: sessionCleanup,
    powerState: powerState,
    powerCheckedAt: powerCheckedAt,
    powerStale: powerStale,
    powerBusy: powerBusy,
    powerCanForceStop: powerCanForceStop,
    powerError: powerError,
    powerAction: powerAction,
    powerTaskStatus: powerTaskStatus,
    powerExitStatus: powerExitStatus,
    vmState: vmState,
    vmStateReason: vmStateReason,
    vmStateRevision: vmStateRevision,
    display: state,
    virtualMonitorControl: virtualMonitorControl,
    independentDisplayStreaming: independentDisplayStreaming,
    initialViewport: initialViewport,
    viewportEncoderLimits: viewportEncoderLimits,
    streamSize: streamSize,
    streamSizeV2: streamSizeV2,
    driveUnavailable: driveUnavailable,
    printUnavailable: printUnavailable,
    profileTracks: profileTracks,
    resources: resources,
    maxBitrate: maxBitrate,
    hostUpdate: hostUpdate,
    hostMissingAfterUpdate: hostMissingAfterUpdate,
    use: use,
    useAccount: useAccount,
    organize: organize,
    pcPower: pcPower,
  );

  DesktopMachine withResources(VmResources sample) => DesktopMachine(
    id: id,
    name: name,
    description: description,
    kind: kind,
    status: status,
    specification: specification,
    accent: accent,
    consoleRelayProtocol: consoleRelayProtocol,
    signinAvailability: signinAvailability,
    consoleInUse: consoleInUse,
    qemuAgentOff: qemuAgentOff,
    consoleBrowserProtocol: consoleBrowserProtocol,
    vmAudio: vmAudio,
    audioState: audioState,
    localPcApproved: localPcApproved,
    monitors: monitors,
    hostOnline: hostOnline,
    thumbnailPrivacy: thumbnailPrivacy,
    interactiveSession: interactiveSession,
    virtualDisplayReady: virtualDisplayReady,
    streamPreparation: streamPreparation,
    consolePreparation: consolePreparation,
    sessionCleanup: sessionCleanup,
    powerState: powerState,
    powerCheckedAt: powerCheckedAt,
    powerStale: powerStale,
    powerBusy: powerBusy,
    powerCanForceStop: powerCanForceStop,
    powerError: powerError,
    powerAction: powerAction,
    powerTaskStatus: powerTaskStatus,
    powerExitStatus: powerExitStatus,
    vmState: vmState,
    vmStateReason: vmStateReason,
    vmStateRevision: vmStateRevision,
    display: display,
    virtualMonitorControl: virtualMonitorControl,
    independentDisplayStreaming: independentDisplayStreaming,
    initialViewport: initialViewport,
    viewportEncoderLimits: viewportEncoderLimits,
    streamSize: streamSize,
    streamSizeV2: streamSizeV2,
    driveUnavailable: driveUnavailable,
    printUnavailable: printUnavailable,
    profileTracks: profileTracks,
    resources: sample,
    maxBitrate: maxBitrate,
    hostUpdate: hostUpdate,
    hostMissingAfterUpdate: hostMissingAfterUpdate,
    use: use,
    useAccount: useAccount,
    organize: organize,
    pcPower: pcPower,
  );

  DesktopMachine withOrganize(DesktopOrganize value) => DesktopMachine(
    id: id,
    name: name,
    description: description,
    kind: kind,
    status: status,
    specification: specification,
    accent: accent,
    consoleRelayProtocol: consoleRelayProtocol,
    signinAvailability: signinAvailability,
    consoleInUse: consoleInUse,
    qemuAgentOff: qemuAgentOff,
    consoleBrowserProtocol: consoleBrowserProtocol,
    vmAudio: vmAudio,
    audioState: audioState,
    localPcApproved: localPcApproved,
    monitors: monitors,
    hostOnline: hostOnline,
    thumbnailPrivacy: thumbnailPrivacy,
    interactiveSession: interactiveSession,
    virtualDisplayReady: virtualDisplayReady,
    streamPreparation: streamPreparation,
    consolePreparation: consolePreparation,
    sessionCleanup: sessionCleanup,
    powerState: powerState,
    powerCheckedAt: powerCheckedAt,
    powerStale: powerStale,
    powerBusy: powerBusy,
    powerCanForceStop: powerCanForceStop,
    powerError: powerError,
    powerAction: powerAction,
    powerTaskStatus: powerTaskStatus,
    powerExitStatus: powerExitStatus,
    vmState: vmState,
    vmStateReason: vmStateReason,
    vmStateRevision: vmStateRevision,
    display: display,
    virtualMonitorControl: virtualMonitorControl,
    independentDisplayStreaming: independentDisplayStreaming,
    initialViewport: initialViewport,
    viewportEncoderLimits: viewportEncoderLimits,
    streamSize: streamSize,
    streamSizeV2: streamSizeV2,
    driveUnavailable: driveUnavailable,
    printUnavailable: printUnavailable,
    profileTracks: profileTracks,
    resources: resources,
    maxBitrate: maxBitrate,
    hostUpdate: hostUpdate,
    hostMissingAfterUpdate: hostMissingAfterUpdate,
    use: use,
    useAccount: useAccount,
    organize: value,
    pcPower: pcPower,
  );

  DesktopMachine withHostUpdate(HostUpdateState state) => DesktopMachine(
    id: id,
    name: name,
    description: description,
    kind: kind,
    status: status,
    specification: specification,
    accent: accent,
    consoleRelayProtocol: consoleRelayProtocol,
    signinAvailability: signinAvailability,
    consoleInUse: consoleInUse,
    qemuAgentOff: qemuAgentOff,
    consoleBrowserProtocol: consoleBrowserProtocol,
    vmAudio: vmAudio,
    audioState: audioState,
    localPcApproved: localPcApproved,
    monitors: monitors,
    hostOnline: hostOnline,
    thumbnailPrivacy: thumbnailPrivacy,
    interactiveSession: interactiveSession,
    virtualDisplayReady: virtualDisplayReady,
    streamPreparation: streamPreparation,
    powerState: powerState,
    powerCheckedAt: powerCheckedAt,
    powerStale: powerStale,
    powerBusy: powerBusy,
    powerCanForceStop: powerCanForceStop,
    powerError: powerError,
    powerAction: powerAction,
    powerTaskStatus: powerTaskStatus,
    powerExitStatus: powerExitStatus,
    vmState: vmState,
    vmStateReason: vmStateReason,
    vmStateRevision: vmStateRevision,
    display: display,
    virtualMonitorControl: virtualMonitorControl,
    independentDisplayStreaming: independentDisplayStreaming,
    initialViewport: initialViewport,
    viewportEncoderLimits: viewportEncoderLimits,
    streamSize: streamSize,
    streamSizeV2: streamSizeV2,
    driveUnavailable: driveUnavailable,
    printUnavailable: printUnavailable,
    profileTracks: profileTracks,
    resources: resources,
    consolePreparation: consolePreparation,
    sessionCleanup: sessionCleanup,
    maxBitrate: maxBitrate,
    hostUpdate: state,
    hostMissingAfterUpdate: hostMissingAfterUpdate,
    use: use,
    useAccount: useAccount,
    organize: organize,
    pcPower: pcPower,
  );

  final SigninAvailability? signinAvailability;

  /// A console connection holds this VM: the tile offers 'Console 끊고 연결'.
  final bool consoleInUse;

  /// The VM's Proxmox option for the QEMU guest agent is off (broker
  /// qemuAgentOff); the tile shows a badge on its picture (2026-11-09).
  final bool qemuAgentOff;
  bool signinReadyAt(DateTime now) =>
      signinAvailability?.fresh(now) == true &&
      hostOnline == true &&
      (kind != MachineKind.pc || localPcApproved) &&
      status != MachineStatus.poweredOff &&
      powerState != 'paused' &&
      !powerBusy &&
      !display.busy &&
      !sessionCleanup.pending;
  bool get signinReady => signinReadyAt(DateTime.now());
  bool get isReady =>
      (status == MachineStatus.ready || signinReady) &&
      !powerBusy &&
      !display.busy &&
      !sessionCleanup.pending;
  String get kindLabel => kind == MachineKind.pc ? 'Windows PC' : 'Windows VM';

  /// Cleanup outlived its normal wait (see [SessionCleanupState.stalledAt]).
  /// Streaming stays blocked either way; only the broker clears the barrier.
  bool get cleanupStalled => sessionCleanup.stalled;

  /// Plain cause of a stalled cleanup. Never states that restoration
  /// succeeded: the barrier is still present.
  String get cleanupCause => switch (sessionCleanup.phase) {
    'recovery_required' => l10n.hostCouldntConfirmScreenWas,
    'awaiting_capabilities' => l10n.hostReportedScreenWasRestored,
    _ => l10n.hostCouldntConfirmTimePrevious,
  };

  /// Fixed broker phase label for comparison with administrator diagnostics.
  String get cleanupEvidence => switch (sessionCleanup.phase) {
    'recovery_required' => l10n.hostReportRestoreUnconfirmed,
    'awaiting_capabilities' => l10n.restoreReportedWaitingScreenCheck,
    'unknown' => l10n.hostCheckTimedOut,
    _ => l10n.endCheckDelayed,
  };

  /// Next step offered for a stalled cleanup.
  String get cleanupNextStep => !sessionCleanup.workerQuiescent
      ? l10n.endPreviousStreamingIsntConfirmed
      : l10n.refreshCheckAgainIfKeeps;

  String get streamReadiness {
    if (sessionCleanup.pending) {
      if (!cleanupStalled) return l10n.checkingPreviousConnectionEndedScreen;
      return l10n.connectingBlockedBecauseScreenRestore;
    }
    if (display.busy) return l10n.settingUpVirtualDisplayYou;
    if (isReady) return l10n.cantStartConnectionRightNow;
    if (status == MachineStatus.poweredOff) return l10n.desktopOff;
    if (hostOnline == false) return l10n.desktopIsntResponding;
    if (hostOnline == true) {
      if (streamPreparation != null) return streamPreparation!.detail;
      if (virtualDisplayReady == false) {
        return l10n.virtualDisplayIsntReadyTell;
      }
      if (interactiveSession == false) return StreamPreparation.session.detail;
      return l10n.checkingWhetherStreamingReady;
    }
    return l10n.waitingHostConnect;
  }

  String? get streamReadinessBadge {
    if (powerBusy) return l10n.switchingPower;
    if (sessionCleanup.pending) return cleanupStalled ? l10n.restoreUnconfirmed : l10n.restoringScreen;
    if (display.busy) return l10n.settingUpScreen;
    if (isReady) return null;
    if (status == MachineStatus.poweredOff) return l10n.poweredOff;
    if (hostOnline == false) return l10n.offline;
    if (hostOnline == true && streamPreparation != null) {
      return streamPreparation!.badge;
    }
    if (virtualDisplayReady == false) return l10n.screenSetup;
    if (interactiveSession == false) return StreamPreparation.session.badge;
    return l10n.waiting;
  }
}
