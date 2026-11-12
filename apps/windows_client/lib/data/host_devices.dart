import '../l10n/l10n.dart';

/// devices-v1 (Host status `devices`, apps/windows_host/src/device_status.hpp):
/// whether this VM can provide the streaming device features. Observation
/// only. Who may use them is the administrator's per-user × VM permission on
/// the server, which the VM cannot see, so nothing here claims a user can.
enum HostDeviceLevel {
  ready,
  automatic,
  setup,
  attention,
  unsupported,
  unknown;

  const HostDeviceLevel();
  String get label => switch (this) {
    HostDeviceLevel.ready => l10n.ready,
    HostDeviceLevel.automatic => l10n.autoInstall,
    HostDeviceLevel.setup => l10n.setupNeeded,
    HostDeviceLevel.attention => l10n.needsCheck,
    HostDeviceLevel.unsupported => l10n.notSupported,
    HostDeviceLevel.unknown => l10n.unknown2,
  };

  /// Needs the user (or a check) before the feature can work on this VM.
  bool get needsAction => this == setup || this == attention || this == unknown;
}

/// One feature: what it is now and, when something is missing, the one next
/// action. Fixed text only; never a device name or Windows error.
class HostDeviceRow {
  const HostDeviceRow(
    this.id,
    this.title,
    this.level,
    this.status, [
    this.next,
    this.action,
  ]);
  final String id, title, status;
  final String? next;
  final HostDeviceLevel level;

  /// What Host settings itself can do about it, if anything.
  final HostDeviceAction? action;
}

/// The fixes Host settings performs (settings_rpc.hpp): installing the WinFsp
/// this Host carries, starting a stopped (never Disabled) spooler, and
/// replacing another product's USB controller after a confirmation.
enum HostDeviceAction {
  installWinfsp,
  startSpooler,
  replaceUsbController;

  const HostDeviceAction();
  String get label => switch (this) {
    HostDeviceAction.installWinfsp => l10n.installWinfsp,
    HostDeviceAction.startSpooler => l10n.startPrintSpooler,
    HostDeviceAction.replaceUsbController => l10n.switchMultitap,
  };

}

/// The four rows, or none when the Host did not report devices (an older
/// HostTool or a malformed reply): absence is never shown as ready.
///
/// A row is its name, its state word and one short fact (2026-11-11: "공유
/// 폴더 · 준비됨 · WinFsp 사용"); a state word that says it all ("확인 불가")
/// has no fact. [HostDeviceRow.next] is a sentence only when there is
/// something to do.
List<HostDeviceRow> hostDeviceRows(Object? devices) {
  if (devices is! Map || devices['schema'] != 1) return const [];
  Map<Object?, Object?> part(String key) =>
      devices[key] is Map ? devices[key] as Map : const {};
  final drives = part('drives'), usb = part('usb');
  final printers = part('printers'), audio = part('audio');
  // The MSI this Host carries makes WinFsp installable from here.
  final carried = drives['package'] == true;
  return [
    switch ((drives['winfsp'], drives['webClient'])) {
      (true, _) => HostDeviceRow(
        'drives',
        l10n.sharedFolder,
        HostDeviceLevel.ready,
        l10n.deviceFactWinfsp,
      ),
      (false, 'usable') => HostDeviceRow(
        'drives',
        l10n.sharedFolder,
        HostDeviceLevel.setup,
        l10n.deviceFactNoWinfsp,
        carried ? l10n.installWinfsp2 : l10n.runHostInstallerAgainInstall,
        carried ? HostDeviceAction.installWinfsp : null,
      ),
      (false, 'disabled' || 'missing') => HostDeviceRow(
        'drives',
        l10n.sharedFolder,
        HostDeviceLevel.attention,
        l10n.deviceFactNoWinfspWebClient,
        carried ? l10n.installWinfsp2 : l10n.runHostInstallerAgainInstall,
        carried ? HostDeviceAction.installWinfsp : null,
      ),
      _ => HostDeviceRow(
        'drives',
        l10n.sharedFolder,
        HostDeviceLevel.unknown,
        '',
        l10n.refreshStatus,
      ),
    },
    switch ((usb['supported'], usb['package'], usb['controller'])) {
      (false, _, _) => HostDeviceRow(
        'usb',
        'USB',
        HostDeviceLevel.unsupported,
        l10n.deviceFactUsbWindows,
      ),
      (true, false, _) => HostDeviceRow(
        'usb',
        'USB',
        HostDeviceLevel.attention,
        l10n.deviceFactUsbFilesMissing,
        l10n.runHostInstallerAgain,
      ),
      (true, true, 'none') => HostDeviceRow(
        'usb',
        'USB',
        HostDeviceLevel.automatic,
        l10n.deviceFactUsbFirstUse,
      ),
      (true, true, 'started') => HostDeviceRow(
        'usb',
        'USB',
        HostDeviceLevel.ready,
        l10n.deviceFactUsbRunning,
      ),
      (true, true, 'stopped') => HostDeviceRow(
        'usb',
        'USB',
        HostDeviceLevel.attention,
        l10n.deviceFactUsbStopped,
        l10n.saveWorkRestartWindowsWont,
      ),
      // Our own earlier install (marker lost, or registered twice): the Host takes
      // it back by itself at the next USB connection, so nothing to explain.
      (true, true, 'unmarked' || 'duplicates') => HostDeviceRow(
        'usb',
        'USB',
        HostDeviceLevel.ready,
        l10n.deviceFactUsbInstalled,
      ),
      (true, true, 'foreign' || 'multiple') => HostDeviceRow(
        'usb',
        'USB',
        HostDeviceLevel.attention,
        l10n.deviceFactUsbForeign,
        l10n.deviceSwitchUsbMultitap,
        HostDeviceAction.replaceUsbController,
      ),
      _ => HostDeviceRow(
        'usb',
        'USB',
        HostDeviceLevel.unknown,
        '',
        l10n.refreshStatus,
      ),
    },
    switch ((printers['reason'], printers['spooler'])) {
      ('', _) => HostDeviceRow(
        'printers',
        l10n.printer,
        HostDeviceLevel.ready,
        l10n.deviceFactPrinterReady,
      ),
      ('print_spooler_unavailable', 'stopped') => HostDeviceRow(
        'printers',
        l10n.printer,
        HostDeviceLevel.attention,
        l10n.deviceFactSpoolerOff,
        l10n.deviceStartSpooler,
        HostDeviceAction.startSpooler,
      ),
      // Disabled is a deliberate setting (often hardening): explained, never changed.
      ('print_spooler_unavailable', 'disabled') => HostDeviceRow(
        'printers',
        l10n.printer,
        HostDeviceLevel.attention,
        l10n.deviceFactSpoolerDisabled,
        l10n.maySecuritySettingSoIsnt,
      ),
      ('print_spooler_unavailable', 'starting') => HostDeviceRow(
        'printers',
        l10n.printer,
        HostDeviceLevel.attention,
        l10n.deviceFactSpoolerStarting,
        l10n.refreshStatusMoment,
      ),
      ('print_spooler_unavailable', _) => HostDeviceRow(
        'printers',
        l10n.printer,
        HostDeviceLevel.attention,
        l10n.deviceFactSpoolerOff,
        l10n.startPrintSpoolerWindowsServices,
      ),
      ('print_driver_unavailable', _) => HostDeviceRow(
        'printers',
        l10n.printer,
        HostDeviceLevel.attention,
        l10n.deviceFactPdfMissing,
        l10n.turnMicrosoftPrintPdfWindows,
      ),
      _ => HostDeviceRow(
        'printers',
        l10n.printer,
        HostDeviceLevel.unknown,
        '',
        l10n.refreshStatus,
      ),
    },
    switch (audio['output']) {
      'present' => HostDeviceRow(
        'audio',
        l10n.soundCard,
        HostDeviceLevel.ready,
        l10n.deviceFactAudioPresent,
      ),
      'absent' => HostDeviceRow(
        'audio',
        l10n.soundCard,
        HostDeviceLevel.setup,
        l10n.deviceFactAudioAbsent,
        l10n.administratorNeedsAddAudioDevice,
      ),
      'remote' => HostDeviceRow(
        'audio',
        l10n.soundCard,
        HostDeviceLevel.unknown,
        l10n.deviceFactAudioRemote,
        l10n.checkAgainFromStreamingProxmox,
      ),
      _ => HostDeviceRow(
        'audio',
        l10n.soundCard,
        HostDeviceLevel.unknown,
        '',
        l10n.refreshStatus,
      ),
    },
  ];
}

/// One line for the overview: the rows that need something, else what is
/// ready (USB counts once it installs itself) and what this Windows lacks.
String hostDevicesSummary(List<HostDeviceRow> rows) {
  if (rows.isEmpty) return l10n.checkDeviceStatus;
  String titles(bool Function(HostDeviceLevel) pick) => [
    for (final row in rows)
      if (pick(row.level)) row.title,
  ].join('·');
  final open = [
    for (final row in rows)
      if (row.level.needsAction) '${row.title} ${row.level.label}',
  ];
  if (open.isNotEmpty) return open.join(' · ');
  final unsupported = titles((l) => l == HostDeviceLevel.unsupported);
  return [
    l10n.ready2(titles((l) => l == HostDeviceLevel.ready || l == HostDeviceLevel.automatic)),
    if (unsupported.isNotEmpty) l10n.notSupported2(unsupported),
  ].join(' · ');
}

/// True when nothing on this VM needs the user for these features.
bool hostDevicesSettled(List<HostDeviceRow> rows) =>
    rows.isNotEmpty && rows.every((row) => !row.level.needsAction);

/// The result of `installWinfsp`: done (with a plain restart notice when
/// msiexec asked for one), or the cause and one next action. Never restarts.
({bool ok, String message}) hostWinfspInstallResult(
  Map<String, dynamic> reply,
) {
  final result = reply['winfsp'];
  if (result is! Map || result['error'] is! String) {
    return (ok: false, message: l10n.couldntConfirmInstallResultRefresh);
  }
  final code = result['error'] as String, restart = result['restart'] == true;
  final restartNotice =
      l10n.restartWindowsApplySaveWork;
  if (code.isEmpty) {
    return (
      ok: true,
      message: restart
          ? l10n.winfspInstalled(restartNotice)
          : l10n.winfspInstalledFromNextConnection,
    );
  }
  if (code == 'winfsp_already_installed') {
    return (ok: true, message: l10n.winfspAlreadyInstalled);
  }
  final cause = switch (code) {
    'winfsp_busy' => l10n.couldntInstallBecauseAnotherProgram,
    'winfsp_blocked' => l10n.windowsPolicyBlockedInstallCheck,
    'winfsp_package_mismatch' ||
    'winfsp_path_invalid' => l10n.couldntVerifyInstallerFileDownload,
    'winfsp_install_failed' ||
    'winfsp_unavailable' => l10n.couldntInstallRestartWindowsThen,
    'winfsp_dll_missing' ||
    'winfsp_load_failed' ||
    'winfsp_driver_missing' => l10n.couldntVerifyAfterInstallingRestart,
    _ => l10n.couldntConfirmInstallResultRefresh2,
  };
  final safe = RegExp(r'^[a-z_]{1,64}$').hasMatch(code)
      ? l10n.errorCode(code)
      : '';
  return (
    ok: false,
    message:
        l10n.meanwhileSharedFoldersUseBasic(cause, restart ? '\n$restartNotice' : '', safe),
  );
}

/// The result of `replaceUsbController`: replaced, a restart Windows asked for
/// (never performed), or the cause and one next action.
({bool ok, String message}) hostUsbReplaceResult(Map<String, dynamic> reply) {
  final result = reply['usb'];
  final code = result is Map ? result['error'] : null;
  final restart = result is Map && result['restart'] == true;
  final restartNotice = l10n.saveWorkThenRestartWindows;
  return switch (code) {
    '' => (
      ok: true,
      message: l10n.usbConnectionFeatureSwitchedMultitap,
    ),
    'usb_restart_required' when restart => (
      ok: true,
      message: l10n.restartWindowsSwitchUsbConnection(restartNotice),
    ),
    'usb_controller_not_foreign' => (ok: true, message: l10n.alreadySetUpMultitap),
    'usb_replace_remove_failed' => (
      ok: false,
      message: l10n.couldntRemoveOtherProgramsUsb,
    ),
    'usb_driver_missing' || 'usb_platform_unsupported' => (
      ok: false,
      message: l10n.multitapUsbDriverCantInstalled,
    ),
    _ => (
      ok: false,
      message: l10n.couldntInstallMultitapUsbDriver,
    ),
  };
}

/// The result of `startPrintSpooler` (a Disabled spooler is never changed).
({bool ok, String message}) hostSpoolerStartResult(Map<String, dynamic> reply) {
  final result = reply['spooler'];
  final code = result is Map ? result['error'] : null;
  return switch (code) {
    '' when result is Map && result['state'] == 'running' => (
      ok: true,
      message: l10n.printSpoolerHasStartedYou,
    ),
    'print_spooler_disabled' => (
      ok: false,
      message: l10n.printSpoolerSetDisabledIsnt,
    ),
    'print_spooler_start_failed' => (
      ok: false,
      message: l10n.couldntStartPrintSpoolerCheck,
    ),
    _ => (ok: false, message: l10n.couldntCheckPrintSpoolerStatus),
  };
}

/// display-adapters-v1 (Host status `displayAdapters`): how many Display-class
/// nodes are Multitap's current adapter, provable leftovers (legacy, duplicate),
/// unproven nodes with the same driver, and anything else; plus the exact extra
/// nodes 정리 may remove once an administrator confirms the list.
class HostDisplayAdapterExtra {
  const HostDisplayAdapterExtra(this.id, this.kind);
  final String id, kind;

  String get description => switch (kind) {
    'legacy' => l10n.oldMultitapVirtualDisplayAdapter,
    'duplicate' => l10n.leftoverAdapterMadeByMultitap,
    _ => l10n.sameDriverNoMultitapRecord,
  };
}

class HostDisplayAdapters {
  const HostDisplayAdapters({
    this.read = false,
    this.current = 0,
    this.legacy = 0,
    this.duplicate = 0,
    this.unproven = 0,
    this.foreign = 0,
    this.extras = const [],
  });
  final bool read;
  final int current, legacy, duplicate, unproven, foreign;
  final List<HostDisplayAdapterExtra> extras;

  static final _instance = RegExp(
    r'^ROOT\\(DISPLAY\\[0-9A-Fa-f]{4,8}|MULTITAPVIRTUALDISPLAY\\[0-9]{4})$',
    caseSensitive: false,
  );

  /// Anything malformed is "not read": counts are never guessed.
  factory HostDisplayAdapters.parse(Object? value) {
    if (value is! Map || value['read'] != true) {
      return const HostDisplayAdapters();
    }
    int? count(String key) {
      final n = value[key];
      return n is int && n >= 0 && n <= 64 ? n : null;
    }

    final counts = [
      for (final key in [
        'current',
        'legacy',
        'duplicate',
        'unproven',
        'foreign',
      ])
        count(key),
    ];
    final raw = value['extras'];
    if (counts.contains(null) || raw is! List || raw.length > 64) {
      return const HostDisplayAdapters();
    }
    final extras = <HostDisplayAdapterExtra>[];
    for (final item in raw) {
      if (item is! Map ||
          item['id'] is! String ||
          !_instance.hasMatch(item['id'] as String) ||
          !const {'legacy', 'duplicate', 'unproven'}.contains(item['kind'])) {
        return const HostDisplayAdapters();
      }
      extras.add(
        HostDisplayAdapterExtra(item['id'] as String, item['kind'] as String),
      );
    }
    return HostDisplayAdapters(
      read: true,
      current: counts[0]!,
      legacy: counts[1]!,
      duplicate: counts[2]!,
      unproven: counts[3]!,
      foreign: counts[4]!,
      extras: extras,
    );
  }

  int get leftover => legacy + duplicate + unproven;

  String get summary => !read
      ? l10n.couldntCheckVirtualDisplayAdapters
      : leftover == 0
      ? l10n.virtualDisplayAdaptersUseNone(current)
      : l10n.virtualDisplayAdaptersUseLeft(current, leftover);
}

/// The result of `cleanDisplayAdapters`.
({bool ok, String message}) hostAdapterCleanupResult(
  Map<String, dynamic> reply,
) {
  final result = reply['adapters'];
  if (result is! Map || result['error'] is! String) {
    return (ok: false, message: l10n.couldntConfirmCleanupResultRefresh);
  }
  if (result['error'] == 'display_adapters_changed') {
    return (ok: false, message: l10n.listChangedMeantimeRefreshStatus);
  }
  if (result['error'] != '') {
    return (ok: false, message: l10n.couldntReadVirtualDisplayDevices);
  }
  int n(String key) => result[key] is int ? result[key] as int : 0;
  final removed = n('removed'), pending = n('pending');
  final failed = n('failed') + n('refused');
  return (
    ok: failed == 0,
    message: [
      if (removed > 0) l10n.text9(removed),
      if (pending > 0) l10n.afterYouRestartWindowsWont(pending),
      if (failed > 0) l10n.refreshStatusThenTryAgain(failed),
      if (removed + pending + failed == 0) l10n.thereWereNoDevicesRemove,
    ].join('\n'),
  );
}
