import 'dart:async';

import 'package:flutter/material.dart';

import '../data/pc_restart.dart';
import '../data/usb_picker.dart';
import '../data/usb_redirection.dart';
import 'app_dialog.dart';
import 'app_menu.dart';
import 'app_theme.dart';
import 'korean_text.dart';
import 'session_menu.dart' show SessionPopups;
import '../l10n/l10n.dart';

/// Opens the USB panel: dropped down from the session bar's button at
/// [anchor], or as a dialog without one (the narrow bar's overflow) or when it
/// cannot fit. Connecting and disconnecting a device apply at once; installing
/// the capture driver is the only step that asks first.
Future<void> showUsbPanel(
  BuildContext context, {
  required UsbSession usb,
  Rect? anchor,
  SessionPopups? popups,
}) {
  final navigator = Navigator.of(context);
  final media = MediaQuery.of(navigator.overlay!.context);
  if (anchor != null) {
    final area = AnchoredPanelRoute.areaFor(media, anchor);
    const width = 400.0;
    if (area.width >= width + 2 * AnchoredPanelRoute.margin &&
        area.bottom - anchor.bottom - AnchoredPanelRoute.margin >= 260) {
      final track = popups?.tracker();
      return navigator.push(
        AnchoredPanelRoute<void>(
          anchor: anchor,
          reduceMotion: MediaQuery.disableAnimationsOf(context),
          width: width,
          label: l10n.usbDevices,
          keepOpenForDialog: true,
          theme: buildAppTheme(brightness: Brightness.dark),
          builder: (context) {
            final panel = UsbPanel(usb: usb);
            return track == null ? panel : track(panel);
          },
        ),
      );
    }
  }
  final track = popups?.tracker();
  return showDialog<void>(
    context: context,
    builder: (context) {
      final panel = Theme(
        data: buildAppTheme(brightness: Brightness.dark),
        child: Dialog(
          backgroundColor: const AppPalette(true).surface,
          child: ConstrainedBox(
            constraints: const BoxConstraints(maxWidth: 440, maxHeight: 600),
            child: UsbPanel(usb: usb),
          ),
        ),
      );
      return track == null ? panel : track(panel);
    },
  );
}

/// The USB devices of this PC: each with what it is doing and one button.
/// Follows [usb] live.
class UsbPanel extends StatefulWidget {
  const UsbPanel({
    super.key,
    required this.usb,
    this.restartPc = requestPcRestart,
  });
  final UsbSession usb;

  /// Asks Windows to restart this PC (replaced in tests); true when the
  /// request was accepted.
  final Future<bool> Function() restartPc;

  @override
  State<UsbPanel> createState() => _UsbPanelState();
}

class _UsbPanelState extends State<UsbPanel> {
  /// What the last action could not do, and the list's state when it was
  /// said: the line goes once that state changes (its cause is over), not
  /// only with the next action.
  String? _notice, _noticeAt;

  /// 기타 USB 장치 as the person left it; null: open by itself while it holds
  /// the only devices there are or one the desktop has.
  bool? _otherOpen;
  bool _helpOpen = false;

  static bool _active(UsbEntry entry) =>
      entry.state == UsbEntryState.shared ||
      entry.state == UsbEntryState.returning ||
      entry.state == UsbEntryState.connecting;

  String _listState(UsbSession usb) => [
    usb.closed,
    usb.ready,
    usb.driver.index,
    usb.restartRequired,
    for (final entry in usb.entries)
      '${entry.device.id}:${entry.state.index}:${entry.moving}:'
          '${entry.device.otherWindow}:${entry.device.busy}:'
          '${entry.device.busyElsewhere}:${entry.returnUncertain}',
  ].join('|');

  void _say(String? notice) {
    _notice = notice;
    _noticeAt = notice == null ? null : _listState(widget.usb);
  }
  Timer? _badgeTimer;

  /// 나중에 was chosen, or the restart was asked: the two buttons go.
  bool _restartLater = false, _restartAsked = false;

  /// 지금 다시 시작, after the one save-your-work question. Windows gets an
  /// ordinary restart request; whether it restarts is up to Windows.
  Future<void> _restartNow() async {
    final confirmed = await showDialog<bool>(
      context: context,
      routeSettings: AnchoredPanelRoute.ownedDialogSettings(context),
      builder: (_) => AppConfirmDialog(
        key: Key('usb-restart-confirm'),
        icon: Icons.restart_alt_rounded,
        tone: AppTone.warn,
        title: l10n.restartPcNow,
        message: l10n.saveOpenWorkFirstUnsaved,
        confirmLabel: l10n.restart,
        confirmKey: Key('usb-restart-confirm-yes'),
        cancelKey: Key('usb-restart-confirm-no'),
      ),
    );
    if (confirmed != true || !mounted) return;
    setState(() => _restartAsked = true);
    var accepted = false;
    try {
      accepted = await widget.restartPc();
    } catch (_) {}
    if (!mounted) return;
    setState(() {
      _restartAsked = accepted;
      _say(accepted ? null : l10n.couldntRequestRestartRestartFrom);
    });
  }

  @override
  void initState() {
    super.initState();
    unawaited(widget.usb.refresh());
  }

  @override
  void dispose() {
    _badgeTimer?.cancel();
    super.dispose();
  }

  /// The order changes when the first 방금 연결됨 badge runs out.
  void _watchBadges(Duration? ends) {
    _badgeTimer?.cancel();
    _badgeTimer = ends == null
        ? null
        : Timer(ends + const Duration(milliseconds: 300), () {
            if (mounted) setState(() {});
          });
  }

  Future<void> _install() async {
    final agreed = await showDialog<bool>(
      context: context,
      routeSettings: AnchoredPanelRoute.ownedDialogSettings(context),
      builder: (context) => Theme(
        data: buildAppTheme(brightness: Brightness.dark),
        child: Builder(
          builder: (context) => AppDialog(
            title: l10n.installUsbDriver,
            subtitle: 'UsbDk',
            icon: Icons.usb_rounded,
            tone: AppTone.warn,
            body: [
              // What happens next on screen: Windows asks for approval.
              AppDialog.message(
                context,
                l10n.installsDriverPcNeedsUsb,
              ),
              AppDialog.message(
                context,
                l10n.onlyInstallersVerifiedSignatureUsed,
              ),
            ],
            actions: [
              TextButton(
                key: const Key('usb-install-cancel'),
                onPressed: () => Navigator.pop(context, false),
                style: AppButtons.quiet(const AppPalette(true)),
                child: KeepAllText(l10n.cancel),
              ),
              FilledButton(
                key: const Key('usb-install-confirm'),
                onPressed: () => Navigator.pop(context, true),
                style: AppButtons.filled(
                  const AppPalette(true),
                  tone: AppTone.warn,
                ),
                child: KeepAllText(l10n.install),
              ),
            ],
          ),
        ),
      ),
    );
    if (agreed != true || !mounted) return;
    setState(() => _say(null));
    final failure = await widget.usb.installDriver();
    if (!mounted) return;
    // A needed restart is a state (the status line), not a failure.
    setState(
      () => _say(
        failure == null || widget.usb.restartRequired
            ? null
            : usbMessage(failure),
      ),
    );
  }

  /// Taking a device away from a VM asks once first: that VM may
  /// be writing to it. Used by 연결 끊기 and, in its own words ([move]), by
  /// 이 창으로 옮기기.
  Future<bool> _confirmDisconnect(UsbEntry entry, {bool move = false}) async =>
      await showDialog<bool>(
        context: context,
        routeSettings: AnchoredPanelRoute.ownedDialogSettings(context),
        builder: (_) => AppConfirmDialog(
          key: const Key('usb-stop-confirm'),
          icon: move ? Icons.usb_rounded : Icons.usb_off_rounded,
          tone: AppTone.warn,
          title: move ? l10n.moveUsbHereTitle : l10n.disconnectUsb,
          subtitle: entry.device.name,
          message: move
              ? l10n.moveUsbHereMessage
              : l10n.ifDesktopUsingDeviceUnsaved,
          confirmLabel: move ? l10n.moveUsbHereConfirm : l10n.disconnect,
          confirmKey: const Key('usb-stop-confirm-yes'),
          cancelKey: const Key('usb-stop-confirm-no'),
        ),
      ) ==
      true;

  /// 연결 끊기 on a device the VM has asks first. 취소 while it is still
  /// connecting does not.
  Future<void> _stop(UsbEntry entry) async {
    if (entry.state == UsbEntryState.shared) {
      final confirmed = await _confirmDisconnect(entry);
      // The device may have changed state while the question was up.
      if (!confirmed || !mounted || entry.state != UsbEntryState.shared) {
        return;
      }
    }
    await widget.usb.stop(entry);
  }

  /// 이 창으로 옮기기: the other window's VM loses the device, so the same
  /// question first; 취소 changes nothing.
  Future<void> _move(UsbEntry entry) async {
    final confirmed = await _confirmDisconnect(entry, move: true);
    // Let go meanwhile: moveHere then just connects it here.
    if (!confirmed || !mounted) return;
    setState(() => _say(null));
    final failure = await widget.usb.moveHere(entry);
    if (mounted) {
      setState(() => _say(failure == null ? null : usbMessage(failure)));
    }
  }

  void _share(UsbEntry entry) {
    final failure = widget.usb.share(entry);
    setState(() => _say(failure == null ? null : usbMessage(failure)));
  }

  @override
  Widget build(BuildContext context) {
    final p = AppPalette.of(context);
    return ListenableBuilder(
      listenable: widget.usb,
      builder: (context, _) {
        final usb = widget.usb;
        final picker = usb.picker();
        _watchBadges(picker.badgeEnds);
        final status = _status(usb);
        // The list moved on since the notice was said: its cause is over.
        if (_notice != null && _noticeAt != _listState(usb)) _say(null);
        final otherOpen =
            _otherOpen ??
            (picker.other.isNotEmpty &&
                (picker.main.isEmpty ||
                    picker.other.any((row) => _active(row.entry))));
        return Column(
          key: const Key('usb-panel'),
          mainAxisSize: MainAxisSize.min,
          crossAxisAlignment: CrossAxisAlignment.stretch,
          children: [
            Padding(
              padding: const EdgeInsets.fromLTRB(16, 14, 8, 0),
              child: Row(
                children: [
                  Icon(Icons.usb_rounded, size: 18, color: p.amber),
                  const SizedBox(width: 10),
                  Expanded(
                    child: KeepAllText(
                      'PC USB',
                      maxLines: 1,
                      overflow: TextOverflow.ellipsis,
                      style: uiText(
                        size: 14,
                        weight: FontWeight.w600,
                        color: p.ink,
                      ),
                    ),
                  ),
                  IconButton(
                    key: const Key('usb-help-toggle'),
                    tooltip: _helpOpen ? l10n.hideSupportedDevices : l10n.supportedDevices,
                    isSelected: _helpOpen,
                    onPressed: () => setState(() => _helpOpen = !_helpOpen),
                    icon: const Icon(Icons.help_outline_rounded, size: 18),
                    selectedIcon: const Icon(Icons.help_rounded, size: 18),
                  ),
                  IconButton(
                    key: const Key('usb-refresh'),
                    tooltip: l10n.refreshList,
                    onPressed: usb.listing || usb.closed
                        ? null
                        : () => unawaited(usb.refresh()),
                    icon: Icon(Icons.refresh_rounded, size: 18, color: p.muted),
                  ),
                ],
              ),
            ),
            Padding(
              padding: const EdgeInsets.fromLTRB(16, 4, 16, 8),
              child: KeepAllText(
                l10n.connectedDeviceCanOnlyUsed,
                style: uiText(size: 12.5, height: 1.45, color: p.muted),
              ),
            ),
            if (_helpOpen)
              Padding(
                padding: const EdgeInsets.fromLTRB(16, 0, 16, 8),
                child: KeepAllText(
                  l10n.keyboardsMiceHubsBluetoothAudio,
                  key: const Key('usb-help-details'),
                  style: uiText(size: 12.5, height: 1.45, color: p.muted),
                ),
              ),
            // The colour is the icon's; the sentence is ordinary text.
            if (status != null)
              Padding(
                padding: const EdgeInsets.fromLTRB(16, 0, 16, 8),
                child: AppStatusLine(
                  status.$1,
                  textKey: const Key('usb-status'),
                  tone: status.$2,
                ),
              ),
            // The installed driver needs this PC restarted: offer it now or
            // later; nothing restarts without the save-your-work question.
            if (usb.driver == UsbDriverState.missing &&
                usb.restartRequired &&
                !_restartLater &&
                !_restartAsked &&
                !usb.closed)
              Padding(
                padding: const EdgeInsets.fromLTRB(16, 0, 16, 10),
                child: Wrap(
                  spacing: 8,
                  runSpacing: 6,
                  children: [
                    FilledButton.icon(
                      key: const Key('usb-restart-now'),
                      onPressed: () => unawaited(_restartNow()),
                      style: AppButtons.filled(p, tone: AppTone.warn),
                      icon: const Icon(Icons.restart_alt_rounded, size: 18),
                      label: KeepAllText(l10n.restartNow),
                    ),
                    TextButton(
                      key: const Key('usb-restart-later'),
                      onPressed: () => setState(() => _restartLater = true),
                      style: AppButtons.quiet(p),
                      child: KeepAllText(l10n.later),
                    ),
                  ],
                ),
              ),
            if (usb.driver == UsbDriverState.missing &&
                !usb.restartRequired &&
                !usb.closed)
              Padding(
                padding: const EdgeInsets.fromLTRB(16, 0, 16, 10),
                child: Tooltip(
                  message: l10n.installsAfterVerifyingInstallersSignature,
                  child: OutlinedButton.icon(
                    key: const Key('usb-install'),
                    onPressed: usb.installing
                        ? null
                        : () => unawaited(_install()),
                    style: AppButtons.outlined(p),
                    icon: usb.installing
                        ? const SizedBox.square(
                            dimension: 16,
                            child: CircularProgressIndicator(strokeWidth: 2),
                          )
                        : const Icon(Icons.download_rounded, size: 18),
                    label: KeepAllText(
                      usb.installing ? l10n.installing2 : l10n.installUsbDriver2,
                    ),
                  ),
                ),
              ),
            if (usb.unreadable > 0)
              Padding(
                padding: const EdgeInsets.fromLTRB(16, 0, 16, 8),
                child: KeepAllText(
                  key: const Key('usb-unreadable'),
                  l10n.usbUnreadableDevices(usb.unreadable),
                  style: uiText(size: 13, color: p.muted),
                ),
              ),
            // Readable, but refused at SuperSpeed: replugging the same way
            // would not help, a USB 2.0 port does.
            if (usb.superSpeed > 0)
              Padding(
                padding: const EdgeInsets.fromLTRB(16, 0, 16, 8),
                child: KeepAllText(
                  key: const Key('usb-superspeed'),
                  l10n.usbSuperSpeedDevices(usb.superSpeed),
                  style: uiText(size: 13, color: p.muted),
                ),
              ),
            Flexible(
              child: picker.isEmpty && picker.hiddenNeeded == 0
                  ? Padding(
                      padding: const EdgeInsets.fromLTRB(16, 4, 16, 14),
                      child: KeepAllText(
                        key: const Key('usb-empty'),
                        usb.listing
                            ? l10n.lookingDevices
                            : usb.driver == UsbDriverState.ready
                            ? l10n.noUsbDevicesConnect
                            : '',
                        style: uiText(size: 13, color: p.muted),
                      ),
                    )
                  : ListView(
                      shrinkWrap: true,
                      padding: const EdgeInsets.symmetric(horizontal: 8),
                      children: [
                        // Only when 기타 has none either: never "no devices"
                        // right above a count of them.
                        if (picker.main.isEmpty &&
                            picker.other.isEmpty &&
                            usb.driver == UsbDriverState.ready)
                          Padding(
                            padding: const EdgeInsets.fromLTRB(8, 2, 8, 8),
                            child: KeepAllText(
                              key: const Key('usb-empty-main'),
                              l10n.noUsbDevicesConnect,
                              style: uiText(size: 13, color: p.muted),
                            ),
                          ),
                        for (final row in picker.main) _row(row, usb),
                        if (picker.other.isNotEmpty || picker.hiddenNeeded > 0)
                          _OtherHeader(
                            count: picker.other.length,
                            open: otherOpen,
                            fresh: picker.other.any((row) => row.justPlugged),
                            onToggle: () =>
                                setState(() => _otherOpen = !otherOpen),
                          ),
                        if (otherOpen) ...[
                          for (final row in picker.other) _row(row, usb),
                          if (picker.hiddenNeeded > 0)
                            Padding(
                              padding: const EdgeInsets.fromLTRB(8, 4, 8, 8),
                              child: KeepAllText(
                                l10n.devicesPcNeedsCantConnected(picker.hiddenNeeded),
                                key: const Key('usb-hidden-note'),
                                style: uiText(
                                  size: 12,
                                  height: 1.4,
                                  color: p.muted,
                                ),
                              ),
                            ),
                        ],
                      ],
                    ),
            ),
            if (_notice != null)
              Padding(
                padding: const EdgeInsets.fromLTRB(16, 4, 16, 0),
                child: AppStatusLine(
                  _notice!,
                  textKey: const Key('usb-notice'),
                ),
              ),
            const SizedBox(height: 12),
          ],
        );
      },
    );
  }

  Widget _row(UsbPickerRow row, UsbSession usb) => _DeviceRow(
    key: ValueKey('usb-device-${row.entry.device.id}'),
    row: row,
    usb: usb,
    onShare: () => _share(row.entry),
    onStop: () => unawaited(_stop(row.entry)),
    onMove: () => unawaited(_move(row.entry)),
    // Takes the device back from UsbDk once more so Windows starts it again.
    onCheckReturn: () => unawaited(usb.bringBack(row.entry)),
  );

  (String, AppTone)? _status(UsbSession usb) {
    if (usb.closed) {
      return (
        usb.error == 'usb_policy_changed'
            ? l10n.administratorTurnedOffUsbPermission
            : usb.recovering
            ? l10n.reconnectingUsb
            : l10n.usbDisconnectedReconnectDesktopUse,
        AppTone.neutral,
      );
    }
    if (usb.unavailableReason != null) {
      return (usbMessage(usb.unavailableReason), AppTone.danger);
    }
    if (!usb.ready) return (l10n.desktopGettingReadyReceiveUsb, AppTone.info);
    if (usb.driver == UsbDriverState.unsupported) {
      return (l10n.pcCantConnectUsbDevices, AppTone.warn);
    }
    if (usb.driver == UsbDriverState.missing) {
      return _restartAsked
          ? (l10n.restartRequestedWindowsAsksIf, AppTone.info)
          : usb.restartRequired
          ? (usbMessage('usbdk_restart_required'), AppTone.warn)
          : (l10n.usbDriverNeedsInstalled, AppTone.warn);
    }
    return null;
  }
}

class _OtherHeader extends StatelessWidget {
  const _OtherHeader({
    required this.count,
    required this.open,
    required this.fresh,
    required this.onToggle,
  });
  final int count;
  final bool open, fresh;
  final VoidCallback onToggle;

  @override
  Widget build(BuildContext context) {
    final p = AppPalette.of(context);
    return Padding(
      padding: const EdgeInsets.symmetric(vertical: 4),
      child: Semantics(
        button: true,
        expanded: open,
        label: l10n.text36(count),
        child: InkWell(
          key: const Key('usb-other-toggle'),
          borderRadius: BorderRadius.circular(8),
          mouseCursor: SystemMouseCursors.click,
          onTap: onToggle,
          child: Padding(
            padding: const EdgeInsets.symmetric(horizontal: 8, vertical: 8),
            child: Row(
              children: [
                // The product's fold: a chevron on the left that turns down.
                AnimatedRotation(
                  turns: open ? .25 : 0,
                  duration:
                      MediaQuery.maybeDisableAnimationsOf(context) ?? false
                      ? Duration.zero
                      : const Duration(milliseconds: 180),
                  curve: Curves.easeOutCubic,
                  child: Icon(
                    Icons.chevron_right_rounded,
                    size: 16,
                    color: p.muted,
                  ),
                ),
                const SizedBox(width: 6),
                Flexible(
                  child: KeepAllText(
                    l10n.otherUsbDevices(count),
                    style: uiText(
                      size: 13,
                      weight: FontWeight.w600,
                      color: p.ink2,
                    ),
                  ),
                ),
                if (fresh && !open) ...[
                  const SizedBox(width: 8),
                  _Chip(
                    text: l10n.justPlugged,
                    color: p.amberInk,
                    background: p.amberSoft,
                  ),
                ],
              ],
            ),
          ),
        ),
      ),
    );
  }
}

class _Chip extends StatelessWidget {
  const _Chip({
    required this.text,
    required this.color,
    required this.background,
    super.key,
  });
  final String text;
  final Color color, background;

  @override
  Widget build(BuildContext context) => DecoratedBox(
    decoration: BoxDecoration(
      color: background,
      borderRadius: BorderRadius.circular(999),
    ),
    child: Padding(
      padding: const EdgeInsets.symmetric(horizontal: 8, vertical: 2),
      child: KeepAllText(
        text,
        style: uiText(size: 12, weight: FontWeight.w600, color: color),
      ),
    ),
  );
}

IconData _kindIcon(UsbKind kind) => switch (kind) {
  UsbKind.storage => Icons.sd_storage_rounded,
  UsbKind.token => Icons.vpn_key_rounded,
  UsbKind.printer => Icons.print_rounded,
  UsbKind.phone => Icons.smartphone_rounded,
  UsbKind.camera => Icons.photo_camera_rounded,
  UsbKind.audio => Icons.headphones_rounded,
  UsbKind.bluetooth => Icons.bluetooth_rounded,
  _ => Icons.usb_rounded,
};

class _DeviceRow extends StatelessWidget {
  const _DeviceRow({
    super.key,
    required this.row,
    required this.usb,
    required this.onShare,
    required this.onStop,
    required this.onMove,
    required this.onCheckReturn,
  });
  final UsbPickerRow row;
  final UsbSession usb;
  final VoidCallback onShare, onStop, onMove, onCheckReturn;

  @override
  Widget build(BuildContext context) {
    final p = AppPalette.of(context);
    final entry = row.entry;
    final device = entry.device;
    final kind = entry.kind;
    final storage = entry.decision.requires.contains('drives');
    final checking =
        entry.state == UsbEntryState.connecting && entry.id == null;
    final active =
        entry.state == UsbEntryState.shared ||
        entry.state == UsbEntryState.returning ||
        entry.state == UsbEntryState.connecting;
    // Another remote window of this client holds it: said by
    // name, and it can be moved to this window. Not for a device whose
    // return to the PC is in doubt: that row offers PC로 되돌리기, as its
    // text says.
    final otherWindow = active || (entry.returnUncertain && !entry.moving)
        ? null
        : device.otherWindow;
    // Going back with a device number: it is being given back. Without one
    // the PC side is only being looked at.
    final returningText = entry.code == 'usb_prepare_timeout'
        ? l10n.openFileCheckTimedOut
        : entry.code == 'usb_capture_timeout'
        ? l10n.deviceConnectionTimedOutChecking
        : entry.id == null
        ? l10n.checkingDevicePc
        : l10n.returningDevicePc;
    final elsewhere =
        device.busyElsewhere &&
        entry.state != UsbEntryState.shared &&
        otherWindow == null;
    // The state of the row: its colour is a dot, its sentence ordinary text.
    final (stateText, stateTone) = switch (entry.state) {
      UsbEntryState.available =>
        elsewhere
            ? (usbMessage(usbBusyElsewhere), AppTone.warn)
            : device.busy
            ? (usbMessage('usb_device_busy'), AppTone.warn)
            : (l10n.available, AppTone.neutral),
      UsbEntryState.connecting => (
        checking ? l10n.checkingOpenFiles : l10n.connectingDesktop2,
        AppTone.info,
      ),
      UsbEntryState.shared => (
        l10n.connectedDesktopNotAvailablePc,
        AppTone.success,
      ),
      UsbEntryState.returning => (returningText, AppTone.info),
      UsbEntryState.refused || UsbEntryState.failed => (
        entry.code == usbPcInUse && entry.blocker != null
            ? l10n.programUsing(usbMessage(entry.code), '${entry.blocker}')
            : entry.code == 'usb_class_not_granted' &&
                  entry.decision.requires.isNotEmpty
            ? usbClassNotGrantedMessage(entry.decision.requires)
            : usbMessage(entry.code),
        // The channel ended and the device is back: said, not a failure.
        entry.code == 'usb_session_closed' ? AppTone.neutral : AppTone.danger,
      ),
    };
    final from = device.otherWindow ?? '';
    final (text, tone) = entry.moving
        ? (
            from.isEmpty ? l10n.movingFromAnotherWindow : l10n.movingFromWindow(from),
            AppTone.info,
          )
        : otherWindow != null
        ? (usbOtherWindowMessage(otherWindow), AppTone.warn)
        : (stateText, stateTone);
    final String? notReady = usb.closed
        ? (usb.recovering ? l10n.reconnectingUsb2 : l10n.usbConnectionLost)
        : !usb.ready
        ? l10n.youCanConnectOnceDesktop
        : null;
    final String? blocked =
        notReady ??
        (elsewhere
            ? l10n.deviceUseElsewhere
            : device.busy
            ? l10n.anotherProgramUsingDevice2
            : null);
    final Widget button = entry.moving || otherWindow != null
        ? Tooltip(
            message: notReady ?? l10n.movesWindowsDesktop,
            child: FilledButton(
              key: ValueKey('usb-move-${device.id}'),
              onPressed: notReady == null && !entry.moving ? onMove : null,
              style: AppButtons.tonal(p),
              child: KeepAllText(
                entry.moving ? l10n.usbMovingHere : l10n.moveWindow,
              ),
            ),
          )
        : entry.returnUncertain && !active
        ? OutlinedButton(
            key: ValueKey('usb-checkreturn-${device.id}'),
            onPressed: usb.closed ? null : onCheckReturn,
            style: AppButtons.outlined(p),
            child: KeepAllText(l10n.returnPc),
          )
        : active
        ? OutlinedButton(
            key: ValueKey('usb-stop-${device.id}'),
            onPressed: entry.state == UsbEntryState.returning ? null : onStop,
            style: AppButtons.outlined(p),
            child: KeepAllText(
              entry.state == UsbEntryState.returning
                  // The same thing the line of the row says.
                  ? (returningText == l10n.returningDevicePc
                        ? l10n.returning
                        : l10n.checking2)
                  : entry.state == UsbEntryState.connecting
                  ? l10n.cancel
                  : l10n.disconnect,
            ),
          )
        : Tooltip(
            message: blocked ?? l10n.connectsUseDesktop,
            child: FilledButton(
              key: ValueKey('usb-share-${device.id}'),
              onPressed: blocked == null ? onShare : null,
              style: AppButtons.tonal(p),
              child: KeepAllText(
                entry.state == UsbEntryState.failed ? l10n.tryAgain : l10n.connect2,
              ),
            ),
          );
    final chips = <Widget>[
      if (row.justPlugged)
        _Chip(
          key: ValueKey('usb-new-${device.id}'),
          text: l10n.justPlugged,
          color: p.amberInk,
          background: p.amberSoft,
        ),
      if (kind == UsbKind.token)
        _Chip(
          text: l10n.securityTokenSmartCard,
          color: p.blue,
          background: p.blue.withValues(alpha: .14),
        ),
      for (final chip in device.chips)
        if (chip.replaceAll(' ', '') != '공동인증서')
          _Chip(text: chip, color: p.green, background: p.okSoft),
    ];
    return Padding(
      padding: const EdgeInsets.symmetric(vertical: 4),
      child: DecoratedBox(
        decoration: BoxDecoration(
          color: p.subtle,
          borderRadius: BorderRadius.circular(10),
          border: Border.all(color: p.line),
        ),
        child: Padding(
          padding: const EdgeInsets.fromLTRB(12, 10, 10, 10),
          child: LayoutBuilder(
            builder: (context, box) {
              // Narrow panel or large text: the button goes under the text.
              final stacked =
                  box.maxWidth < 330 ||
                  MediaQuery.textScalerOf(context).scale(14) > 19;
              final info = Column(
                crossAxisAlignment: CrossAxisAlignment.start,
                children: [
                  Wrap(
                    spacing: 6,
                    runSpacing: 4,
                    crossAxisAlignment: WrapCrossAlignment.center,
                    children: [
                      Text(
                        device.name,
                        key: ValueKey('usb-name-${device.id}'),
                        style: uiText(
                          size: 13.5,
                          weight: FontWeight.w600,
                          color: p.ink,
                        ),
                      ),
                      ...chips,
                    ],
                  ),
                  if (device.subtitle.isNotEmpty)
                    Padding(
                      padding: const EdgeInsets.only(top: 2),
                      child: Text(
                        device.subtitle,
                        key: ValueKey('usb-sub-${device.id}'),
                        style: uiText(size: 12, height: 1.35, color: p.muted),
                      ),
                    ),
                  if (storage && device.noReadableDrive)
                    Padding(
                      padding: const EdgeInsets.only(top: 2),
                      child: AppStatusLine(
                        entry.state == UsbEntryState.shared
                            ? l10n.ifDriveDoesntAppearDesktop
                            : l10n.pcHasNoReadableDrive,
                        textKey: ValueKey('usb-nodrive-${device.id}'),
                        tone: AppTone.warn,
                        size: 12,
                      ),
                    ),
                  if (device.vidPid.isNotEmpty)
                    Padding(
                      padding: const EdgeInsets.only(top: 2),
                      child: Text(
                        device.vidPid,
                        key: ValueKey('usb-id-${device.id}'),
                        style: uiText(size: 12, color: p.muted),
                      ),
                    ),
                  Padding(
                    padding: const EdgeInsets.only(top: 4),
                    child: AppStatusLine(
                      text,
                      textKey: ValueKey('usb-status-${device.id}'),
                      tone: tone,
                      size: 12,
                      dot: true,
                    ),
                  ),
                  // What needs attention: one line with a yellow icon.
                  if (entry.notReturned &&
                      !active &&
                      entry.code != usbNotReturned)
                    Padding(
                      padding: const EdgeInsets.only(top: 4),
                      child: AppStatusLine(
                        usbMessage(usbNotReturned),
                        textKey: ValueKey('usb-notreturned-${device.id}'),
                        tone: AppTone.warn,
                        size: 12,
                      ),
                    ),
                  if (storage && entry.state == UsbEntryState.shared)
                    Padding(
                      padding: EdgeInsets.only(top: 4),
                      child: AppStatusLine(
                        l10n.useSafelyRemoveHardwareDesktop,
                        tone: AppTone.warn,
                        size: 12,
                      ),
                    ),
                ],
              );
              final icon = Padding(
                padding: const EdgeInsets.only(top: 1),
                child: Icon(
                  _kindIcon(kind),
                  size: 18,
                  color: active ? p.green : p.amber,
                ),
              );
              if (stacked) {
                return Column(
                  crossAxisAlignment: CrossAxisAlignment.stretch,
                  children: [
                    Row(
                      crossAxisAlignment: CrossAxisAlignment.start,
                      children: [
                        icon,
                        const SizedBox(width: 8),
                        Expanded(child: info),
                      ],
                    ),
                    const SizedBox(height: 8),
                    Align(alignment: Alignment.centerRight, child: button),
                  ],
                );
              }
              return Row(
                crossAxisAlignment: CrossAxisAlignment.start,
                children: [
                  icon,
                  const SizedBox(width: 8),
                  Expanded(child: info),
                  const SizedBox(width: 8),
                  button,
                ],
              );
            },
          ),
        ),
      ),
    );
  }
}
