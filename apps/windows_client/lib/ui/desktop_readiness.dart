import '../models/desktop_machine.dart';
import '../models/host_update.dart';
import '../models/stream_preparation.dart';
import '../l10n/l10n.dart';

/// What a launcher tile shows first. Derived only from broker fields the
/// client already has; nothing here is a measurement or an estimate.
enum TilePhase { ready, off, power, waiting, attention }

/// One readiness row: confirmed, in progress, needs attention, or unconfirmed.
enum CheckState { ok, pending, warn, unknown }

/// A step the signed-in user can take from the tile or the checklist.
/// [console]: Windows is signed out or locked; its Proxmox console opens the
/// real sign-in screen (the stream never shows it).
/// [releaseConsole]: a console connection holds the VM; end it and stream.
enum ReadinessAction { start, reboot, refresh, console, releaseConsole }

class ReadinessCheck {
  const ReadinessCheck(this.label, this.state, this.detail, {this.action});
  final String label;
  final CheckState state;
  final String detail;
  final ReadinessAction? action;
}

class DesktopReadiness {
  const DesktopReadiness._({
    required this.phase,
    required this.chip,
    required this.headline,
    required this.detail,
    required this.checks,
    this.cleanup = false,
    this.cleanupStalled = false,
    this.settling = false,
    this.console = false,
    this.primary,
    this.progressLabel,
    this.hostUpdating = false,
    this.hostMissing = false,
  });

  final TilePhase phase;

  /// The Host went away with its last update and stayed away (broker
  /// hostMissingAfterUpdate): the [detail] is the next step, in place of the
  /// update's own tile line.
  final bool hostMissing;

  /// The Host is away because its update restarts it: the
  /// [detail] says how far it got, in place of the update's tile line.
  final bool hostUpdating;

  /// Short power/state label for the thumbnail chip.
  final String chip;

  /// The tile's status line and its second line; also the checklist verdict.
  final String headline, detail;

  /// 전원 / Host 서비스 / 가상 모니터 / 중계 경로, in that order.
  final List<ReadinessCheck> checks;

  /// The broker's stream cleanup barrier explains this state.
  final bool cleanup, cleanupStalled;

  /// The previous connection's cleanup is still running (normally a second
  /// or two). The tile looks ready and a press connects as soon as it clears
  /// (a blocking "이전 연결을 정리하고 있어요" card would be in the way).
  final bool settling;

  /// Streaming opens through the console hand-over dialog first.
  final bool console;

  /// The tile's main button while it needs attention: a reboot only for a
  /// restart-required state, otherwise a refresh.
  final ReadinessAction? primary;

  /// Set while the Host is up and Windows is still getting ready (broker
  /// vmState 'preparing'): the main button shows it, disabled, as progress.
  final String? progressLabel;

  /// A connection may open now (not while [settling]: it waits instead).
  bool get canStream => phase == TilePhase.ready && !settling;

  static String _monitorSizes(DesktopMachine m) {
    final sizes = [
      for (final monitor in m.monitors)
        if (monitor.width > 0 && monitor.height > 0)
          '${monitor.width}×${monitor.height}',
    ];
    return sizes.isEmpty ? '' : ' · ${sizes.join(', ')}';
  }

  static ({String headline, String detail}) _preparation(StreamPreparation p) =>
      switch (p) {
        StreamPreparation.restart => (
          headline: l10n.windowsNeedsRestart,
          detail: l10n.getsVirtualDisplayReadyUnsaved,
        ),
        StreamPreparation.console => (
          headline: l10n.signedOverRdp,
          detail: l10n.virtualDisplayCantPreparedRdp2,
        ),
        StreamPreparation.login => (
          headline: l10n.notSignedWindows,
          detail: l10n.signWindowsConnect,
        ),
        StreamPreparation.identity => (
          headline: l10n.windowsAccountNeedsChecking,
          detail: StreamPreparation.identity.detail,
        ),
        StreamPreparation.session => (
          headline: l10n.couldntCheckWindowsScreen,
          detail: l10n.refreshCheckAgainIfKeeps2,
        ),
        StreamPreparation.resume => (
          headline: l10n.windowsScreenFromRdpOpen,
          detail: l10n.bringScreenIntoStreamingConnect,
        ),
        StreamPreparation.locked => (
          headline: l10n.windowsLocked2,
          detail: l10n.unlockConnect,
        ),
        StreamPreparation.administrator => (
          headline: l10n.signWindowsAdministratorAccount,
          detail: StreamPreparation.administrator.detail,
        ),
        StreamPreparation.recovery => (
          headline: l10n.virtualDisplayNeedsRepair,
          detail: l10n.youCanRepairHostSettings,
        ),
        StreamPreparation.display => (
          headline: l10n.virtualDisplayIsntReady,
          detail: l10n.refreshMomentIfKeepsHappening,
        ),
        StreamPreparation.verification => (
          headline: l10n.virtualDisplayDriverNeedsChecking,
          detail: l10n.youCanCheckHostSettings,
        ),
        StreamPreparation.configuration => (
          headline: l10n.virtualDisplaySettingsNeedChecking2,
          detail: l10n.youCanCheckHostSettings,
        ),
        StreamPreparation.maintenance => (
          headline: l10n.hostSettingUpScreen,
          detail: l10n.youCanConnectWhenSetup,
        ),
        StreamPreparation.checking => (
          headline: l10n.couldntCheckVirtualDisplayStatus2,
          detail: l10n.refreshCheckAgainIfKeeps2,
        ),
      };

  /// The neutral boot step the broker published (vmState 'preparing'), or
  /// null. A released client never sees it as a state it knows and keeps its
  /// own texts, so only this build reads it.
  static ({String label, String detail})? _prepareStep(DesktopMachine m) {
    if (m.vmState != 'preparing' || m.hostOnline != true) return null;
    return switch (m.vmStateReason) {
      'windows_login' => (
        label: l10n.checkingWindowsSign,
        detail: l10n.checkingWindowsSignStateUpdates,
      ),
      'display_preparing' => (
        label: l10n.preparingScreen,
        detail: l10n.preparingVirtualDisplayUpdatesAs,
      ),
      _ => (label: l10n.preparing3, detail: l10n.hostPreparingConnectionUpdatesAs),
    };
  }

  factory DesktopReadiness.of(
    DesktopMachine m, {
    bool consoleRecovery = false,
    bool preview = false,
    String? server,
    DateTime? now,
  }) {
    server ??= l10n.server;
    final running = m.powerState == 'running' && !m.powerStale;
    final stopped =
        m.status == MachineStatus.poweredOff ||
        (m.powerState == 'stopped' && !m.powerStale);
    final paused = m.powerState == 'paused' && !m.powerStale;
    final hostUp = m.hostOnline == true;
    // A PC has no Proxmox power: the broker already found nothing saying it is off.
    final missing = hostUp || (m.kind == MachineKind.vm && !running)
        ? null
        : m.hostMissingAfterUpdate;
    final step = _prepareStep(m);
    final cleanup = m.sessionCleanup;
    final stalled = cleanup.pending && m.cleanupStalled;
    final console =
        !preview && !m.isReady && consoleRecovery && m.canRecoverConsole;
    final ready =
        (preview && m.status == MachineStatus.ready) || m.isReady || console;

    ReadinessCheck power() {
      if (m.powerBusy) {
        return ReadinessCheck(
          l10n.power,
          CheckState.unknown,
          l10n.checkedAfterPowerActionFinishes,
        );
      }
      if (running) {
        return ReadinessCheck(l10n.power, CheckState.ok, l10n.onLabel);
      }
      if (ready && !stopped) {
        // No fresh Proxmox snapshot, but a streaming host implies it is on.
        return ReadinessCheck(l10n.power, CheckState.ok, l10n.confirmedByHostResponse);
      }
      if (stopped) {
        return ReadinessCheck(
          l10n.power,
          CheckState.warn,
          l10n.off4,
          action: m.canStart ? ReadinessAction.start : null,
        );
      }
      if (paused) {
        return ReadinessCheck(
          l10n.power,
          CheckState.warn,
          l10n.pausedTellAdministrator,
        );
      }
      return ReadinessCheck(
        l10n.power,
        CheckState.unknown,
        l10n.powerStateNotReceivedYet,
      );
    }

    ReadinessCheck host() {
      if (stopped || paused) {
        return ReadinessCheck(
          l10n.hostService,
          CheckState.unknown,
          l10n.checkedWhenDesktopStarts,
        );
      }
      if (hostUp) {
        return ReadinessCheck(l10n.hostService, CheckState.ok, l10n.responding);
      }
      // The verdict reads "Host 업데이트 중" or "Windows 시작 중" in these
      // states: the Host is away for a known reason, so its row waits too
      // (no warning, no 다시 시작).
      if (running && m.hostUpdate.restartLine(now) != null) {
        return ReadinessCheck(
          l10n.hostService,
          CheckState.pending,
          l10n.hostUpdating,
        );
      }
      if (running && missing == null) {
        final waiting = switch (m.vmState) {
          'windows_starting' => l10n.windowsStarting2,
          'host_wait' => l10n.preparingConnection,
          _ => null,
        };
        if (waiting != null) {
          return ReadinessCheck(l10n.hostService, CheckState.pending, waiting);
        }
      }
      if (missing != null) {
        return ReadinessCheck(
          l10n.hostService,
          CheckState.warn,
          missing.stepsFor(localPc: m.kind == MachineKind.pc),
        );
      }
      if (m.hostOnline == false) {
        return ReadinessCheck(
          l10n.hostService,
          CheckState.warn,
          l10n.notRespondingIfWindowsStarting,
          action: m.canReboot ? ReadinessAction.reboot : null,
        );
      }
      return ReadinessCheck(
        l10n.hostService,
        CheckState.unknown,
        l10n.noResponseYet,
      );
    }

    ReadinessCheck display() {
      final label = l10n.virtualDisplay2;
      if (!hostUp && !ready) {
        return ReadinessCheck(
          label,
          CheckState.unknown,
          l10n.checkedWhenHostServiceResponds,
        );
      }
      if (cleanup.pending) {
        return stalled
            ? ReadinessCheck(
                label,
                CheckState.warn,
                l10n.screenRestoreFromLastConnection,
                action: ReadinessAction.refresh,
              )
            : ReadinessCheck(
                label,
                CheckState.unknown,
                l10n.checkingScreenRestoreFromLast,
              );
      }
      if (m.display.busy) {
        return ReadinessCheck(
          label,
          CheckState.unknown,
          l10n.checkingVirtualDisplaySetup,
        );
      }
      if (ready && !console) {
        final count = m.verifiedOwnedDisplayCount;
        if (count == null) {
          return ReadinessCheck(
            label,
            CheckState.unknown,
            l10n.numberScreensNotReceivedYet,
            action: ReadinessAction.refresh,
          );
        }
        return ReadinessCheck(
          label,
          CheckState.ok,
          l10n.ready3(count, _monitorSizes(m)),
        );
      }
      if (console) {
        return ReadinessCheck(
          label,
          CheckState.warn,
          l10n.readyOnceScreenUsedRdp,
        );
      }
      if (step != null) {
        return ReadinessCheck(label, CheckState.pending, step.detail);
      }
      final p = m.streamPreparation;
      if (p != null) {
        if (p == StreamPreparation.maintenance) {
          return ReadinessCheck(
            label,
            CheckState.unknown,
            _preparation(p).detail,
          );
        }
        return ReadinessCheck(
          label,
          CheckState.warn,
          '${p.badge} · ${_preparation(p).detail}',
          action: p == StreamPreparation.restart && m.canReboot
              ? ReadinessAction.reboot
              : null,
        );
      }
      if (m.virtualDisplayReady == false) {
        return ReadinessCheck(
          label,
          CheckState.warn,
          l10n.notReadyCheckHostSettings,
        );
      }
      if (m.interactiveSession == false) {
        return ReadinessCheck(
          label,
          CheckState.warn,
          l10n.couldNotCheckWindowsScreen,
        );
      }
      return ReadinessCheck(
        label,
        CheckState.unknown,
        l10n.checkingStreamingScreen,
      );
    }

    ReadinessCheck relay() => ready && !console
        ? ReadinessCheck(
            l10n.connectionPath,
            CheckState.ok,
            l10n.availableViaCheckedAgainWhen('$server'),
          )
        : ReadinessCheck(
            l10n.connectionPath,
            CheckState.unknown,
            l10n.notCheckedCheckedOnceEarlier,
          );

    final checks = [power(), host(), display(), relay()];
    DesktopReadiness make(
      TilePhase phase,
      String chip,
      String headline,
      String detail, {
      ReadinessAction? primary,
      bool settling = false,
      String? progressLabel,
      bool hostUpdating = false,
      bool hostMissing = false,
    }) => DesktopReadiness._(
      phase: phase,
      chip: chip,
      headline: headline,
      detail: detail,
      checks: checks,
      cleanup: cleanup.pending,
      cleanupStalled: stalled,
      settling: settling,
      console: console,
      primary: primary,
      progressLabel: progressLabel,
      hostUpdating: hostUpdating,
      hostMissing: hostMissing,
    );

    if (preview && ready) {
      return make(TilePhase.ready, l10n.sample, l10n.screenPreview, l10n.designSampleNotConnectedServer);
    }
    if (m.powerBusy) {
      final noun = m.kind == MachineKind.pc ? l10n.desktop3 : l10n.desktop3;
      final label = switch (m.powerAction) {
        'start' => l10n.starting(noun),
        'reboot' => l10n.restarting(noun),
        'shutdown' => l10n.shuttingDown(noun),
        'stop' => l10n.stopping(noun),
        _ => l10n.powerActionProgress,
      };
      return make(
        TilePhase.power,
        switch (m.powerAction) {
          'start' => l10n.starting2,
          'shutdown' => l10n.shuttingDown2,
          'stop' => l10n.stopping2,
          _ => l10n.powerAction,
        },
        label,
        // A 종료 Windows does not finish can be cut with 정지 (⏻ menu).
        m.powerAction == 'shutdown' && m.canStop
            ? l10n.ifWindowsDoesNotRespond
            : l10n.statusCheckedAgainWhenFinishes,
      );
    }
    if (ready) {
      return console
          ? make(
              TilePhase.ready,
              l10n.running,
              l10n.takeOverWindowsScreenConnect,
              l10n.bringsWindowsScreenUsedRdp,
            )
          : make(TilePhase.ready, l10n.running, l10n.readyConnect, l10n.serverSaysYouCanConnect);
    }
    if (stopped) {
      return make(
        TilePhase.off,
        l10n.off,
        l10n.off4,
        // One short sentence: the second was cut on narrow cards.
        l10n.pressStartStartDesktop,
      );
    }
    if (paused) {
      return make(
        TilePhase.attention,
        l10n.paused,
        l10n.desktopPaused,
        l10n.letAdministratorKnow,
        primary: ReadinessAction.refresh,
      );
    }
    if (cleanup.pending) {
      if (!stalled && running && hostUp) {
        return make(
          TilePhase.ready,
          l10n.running,
          l10n.readyConnect,
          l10n.cleaningUpConnectionJustEnded,
          settling: true,
        );
      }
      return stalled
          ? make(
              TilePhase.attention,
              l10n.running,
              l10n.screenRestoreNotConfirmed,
              m.cleanupNextStep,
              primary: ReadinessAction.refresh,
            )
          : make(
              TilePhase.waiting,
              l10n.running,
              l10n.cleaningUpLastConnection,
              l10n.youCanConnectWhenDone,
            );
    }
    // A console connection holds the VM and its display: say so and offer
    // to end it, instead of a display-preparation wait.
    if (m.consoleInUse && m.kind == MachineKind.vm && hostUp) {
      return make(
        TilePhase.attention,
        l10n.running,
        l10n.useConsole,
        l10n.youCanDisconnectConsoleConnect,
        primary: ReadinessAction.releaseConsole,
      );
    }
    if (m.display.busy) {
      return make(
        TilePhase.waiting,
        running ? l10n.running : l10n.checking,
        l10n.settingUpVirtualDisplay,
        l10n.youCanConnectWhenSetup,
      );
    }
    // A Host update restarts the Host service on purpose: progress, not a
    // warning, while it is under way; past its bound the texts below apply.
    final updating = m.hostUpdate.restartLine(now);
    if (running && !hostUp && updating != null) {
      return make(
        TilePhase.waiting,
        l10n.updating2,
        l10n.hostUpdating,
        updating,
        progressLabel: l10n.hostUpdating,
        hostUpdating: true,
      );
    }
    // The Host left with its last update and stayed away while the desktop
    // runs (the broker waited about 3 minutes): what to check, never why.
    if (missing != null) {
      return make(
        TilePhase.attention,
        l10n.checkHost,
        HostMissingAfterUpdate.headline,
        HostMissingAfterUpdate.guidanceFor(localPc: m.kind == MachineKind.pc),
        primary: ReadinessAction.refresh,
        hostMissing: true,
      );
    }
    // The broker's state machine says where a running VM without a Host is:
    // Windows starting, the agent up and the Host awaited, or a Host that
    // dropped from a healthy VM (never shown as 꺼짐).
    if (running && !hostUp) {
      switch (m.vmState) {
        case 'windows_starting':
          return make(
            TilePhase.waiting,
            l10n.windowsStarting,
            l10n.windowsStarting2,
            m.vmStateReason == 'rebooting'
                ? l10n.changesAutomaticallyWhenRestartFinishes
                : l10n.changesAutomaticallyWhenStartupFinishes,
          );
        case 'host_wait':
          return make(
            TilePhase.waiting,
            l10n.preparing3,
            l10n.preparingConnection,
            l10n.windowsHasStartedChangesAs,
          );
        case 'host_lost':
          return make(
            TilePhase.attention,
            l10n.hostDisconnected,
            l10n.hostConnectionLost,
            l10n.desktopRunningChangesAutomaticallyWhen,
            primary: ReadinessAction.refresh,
          );
      }
    }
    if (m.hostOnline == false) {
      return make(
        TilePhase.attention,
        running ? l10n.running : l10n.checking,
        l10n.hostNotResponding,
        l10n.ifYouJustStartedWait,
        primary: ReadinessAction.refresh,
      );
    }
    if (hostUp) {
      // What the main button does when streaming is blocked: a refresh. A
      // reboot is offered only for a state that really asks for one (restart
      // required), never as a generic fix.
      const fix = ReadinessAction.refresh;
      // The broker says these are the normal first minutes after the Host
      // connected (auto-logon, virtual display coming up): progress, never a
      // warning. Past its grace window or for a definite failure it does not
      // send 'preparing', and the attention texts below apply.
      // Windows has nobody signed in (or is locked): the Proxmox console shows
      // the real sign-in screen, and a VM restart signs in automatically
      // (the stream never shows the sign-in screen). Right after the Host
      // connects this may still be the automatic login, which switches the
      // tile to 스트리밍 연결 by itself.
      final signedOut =
          m.vmStateReason == 'windows_login' ||
          m.streamPreparation == StreamPreparation.login ||
          m.streamPreparation == StreamPreparation.locked;
      if (signedOut && m.kind == MachineKind.vm && hostUp) {
        final locked = m.streamPreparation == StreamPreparation.locked;
        return make(
          TilePhase.attention,
          l10n.running,
          locked ? l10n.windowsLocked2 : l10n.notSignedWindows,
          locked
              ? l10n.unlockConsole
              : step != null
              ? l10n.ifSigningAutomaticallyYouCan
              : l10n.signFromConsoleAfterRestart,
          primary: ReadinessAction.console,
        );
      }
      if (step != null) {
        return make(
          TilePhase.waiting,
          l10n.running,
          step.label,
          step.detail,
          progressLabel: step.label,
        );
      }
      final p = m.streamPreparation;
      if (p == StreamPreparation.maintenance) {
        final text = _preparation(p!);
        return make(TilePhase.waiting, l10n.running, text.headline, text.detail);
      }
      if (p != null) {
        final text = _preparation(p);
        return make(
          TilePhase.attention,
          l10n.running,
          text.headline,
          text.detail,
          primary: p == StreamPreparation.restart && m.canReboot
              ? ReadinessAction.reboot
              : fix,
        );
      }
      if (m.virtualDisplayReady == false) {
        return make(
          TilePhase.attention,
          l10n.running,
          l10n.virtualDisplayIsntReady,
          l10n.virtualDisplayNeedsCheckedHost,
          primary: fix,
        );
      }
      if (m.interactiveSession == false) {
        return make(
          TilePhase.attention,
          l10n.running,
          l10n.couldntCheckWindowsScreen,
          l10n.refreshCheckAgainIfKeeps2,
          primary: fix,
        );
      }
      return make(
        TilePhase.waiting,
        l10n.running,
        l10n.checkingStreamingReadiness,
        l10n.changesAsSoonAsHost,
      );
    }
    return make(
      TilePhase.waiting,
      running ? l10n.running : l10n.checking,
      running ? l10n.waitingHostConnect2 : l10n.checkingStatus,
      running ? l10n.hostConnectsOnceWindowsStarts : l10n.checkingAgainAutomaticallyMoment,
    );
  }
}
