import 'package:flutter/material.dart';

import '../models/desktop_machine.dart';
import 'broker_api.dart';

/// [canForceStop] replaces the listing's stop permission while this client's
/// own power request runs (the listing was read before it).
DesktopMachine machineFromBroker(
  BrokerDesktop desktop, {
  bool powerPending = false,
  bool displayPending = false,
  bool? canForceStop,
}) {
  final stopped = desktop.power.state == 'stopped' && !desktop.power.stale;
  final task = desktop.powerTask;
  final approved = desktop.kind != 'pc' || desktop.localPcApproved;
  return DesktopMachine(
    id: desktop.id,
    name: desktop.name,
    signinAvailability: desktop.canStream ? null : desktop.signinAvailability,
    consoleInUse: desktop.consoleInUse,
    qemuAgentOff: desktop.kind == 'vm' && desktop.qemuAgentOff,
    description: desktop.description,
    kind: desktop.kind == 'pc' ? MachineKind.pc : MachineKind.vm,
    status: stopped
        ? MachineStatus.poweredOff
        : desktop.canStream && approved
        ? MachineStatus.ready
        : desktop.status == 'powered_off' && desktop.power.state != 'running'
        ? MachineStatus.poweredOff
        : MachineStatus.awaitingHost,
    // The broker gives a Proxmox-synced VM the placeholder '미확인' (unknown
    // OS) until an administrator sets one. Beside a running Windows VM that
    // reads as a fault; the tile shows its kind instead.
    specification: desktop.os.trim() == '미확인' ? '' : desktop.os,
    accent: const Color(0xFF5579B8),
    consoleRelayProtocol: desktop.emergencyConsoleSupported
        ? desktop.consoleRelayProtocol
        : null,
    consoleBrowserProtocol: desktop.browserConsoleSupported
        ? desktop.consoleBrowserProtocol
        : null,
    localPcApproved: desktop.localPcApproved,
    vmAudio: desktop.vmAudio,
    audioState: desktop.audioState,
    monitors: desktop.monitors,
    hostOnline: desktop.hostConnected,
    thumbnailPrivacy: desktop.thumbnailPrivacy,
    interactiveSession: desktop.interactiveSession,
    virtualDisplayReady: desktop.virtualDisplayReady,
    streamPreparation: desktop.streamPreparation,
    powerState: desktop.power.state,
    powerCheckedAt: desktop.power.checkedAt,
    powerStale: desktop.power.stale,
    powerCanForceStop: canForceStop ?? desktop.powerCanForceStop,
    powerBusy: powerPending || desktop.powerBusy || task?.isActive == true,
    powerError: desktop.power.error,
    powerAction: task?.action,
    powerTaskStatus: task?.status,
    powerExitStatus: task?.exitStatus,
    vmState: desktop.vmState,
    vmStateReason: desktop.vmStateReason,
    vmStateRevision: desktop.vmStateRevision,
    display: displayPending ? desktop.display.pending() : desktop.display,
    virtualMonitorControl: desktop.virtualMonitorControl,
    independentDisplayStreaming: desktop.independentDisplayStreaming,
    initialViewport: desktop.initialViewport,
    viewportEncoderLimits: desktop.viewportEncoderLimits,
    streamSize: desktop.streamSize,
    streamSizeV2: desktop.streamSizeV2,
    driveUnavailable: desktop.driveUnavailable,
    printUnavailable: desktop.printUnavailable,
    profileTracks: desktop.profileTracks,
    resources: desktop.resources,
    consolePreparation: desktop.consolePreparation,
    sessionCleanup: desktop.sessionCleanup,
    maxBitrate: desktop.maxBitrate,
    hostUpdate: desktop.hostUpdate,
    hostMissingAfterUpdate: desktop.hostMissingAfterUpdate,
    use: desktop.use,
    useAccount: desktop.useAccount,
    organize: desktop.organize,
    pcPower: desktop.pcPower,
  );
}
