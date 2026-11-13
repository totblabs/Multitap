import 'package:flutter/material.dart';

import '../models/desktop_machine.dart';
import '../models/session_display_mode.dart';
import 'app_dialog.dart';
import 'app_menu.dart';
import 'app_theme.dart';
import 'korean_text.dart';
import '../l10n/l10n.dart';

/// The words for a client power action, as the ⏻ menu shows them
/// (Proxmox's 종료 / 정지; "정상 종료" reads as jargon).
String powerActionLabel(String action) => switch (action) {
  'start' => l10n.start,
  'reboot' => l10n.restart,
  'stop' => l10n.stop,
  _ => l10n.shutDown,
};

/// Why 정지 alone is unavailable while 종료 is offered: the broker does not
/// report the stop permission (a broker without client stop).
/// Null when the shared power reason or the VM state explains it.
String? stopUnavailableReason(DesktopMachine m) => !m.canStop && m.canShutdown
    ? l10n.stopIsntAvailableServerYet
    : null;

/// The one question before 켜기, 종료, 다시 시작 or 정지.
/// 정지 cuts the power, so it asks in the danger tone and Enter stays on
/// 취소. Resolves true only for the main button. [pc]: an approved PC
/// (pc-power-v1), the same question about the PC.
Future<bool> confirmPowerAction(
  BuildContext context, {
  required String action,
  required String desktopId,
  required String name,
  bool pc = false,
}) {
  final label = powerActionLabel(action);
  final noun = pc ? l10n.localPc2 : l10n.desktop3;
  return showAppConfirm(
    context,
    icon: switch (action) {
      'reboot' => Icons.restart_alt_rounded,
      'stop' => Icons.power_off_rounded,
      _ => Icons.power_settings_new_rounded,
    },
    tone: action == 'stop' ? AppTone.danger : AppTone.accent,
    title: action == 'start' ? l10n.start2(noun) : l10n.the(noun, label),
    subtitle: name,
    message: switch (action) {
      'start' when pc => l10n.startsLocalPcOverNetwork,
      'start' => l10n.youCanConnectOnceIts,
      'stop' => l10n.cutsPowerRightAwayUnsaved,
      'reboot' => l10n.saveAnyUnsavedWorkFirst,
      _ => l10n.asksWindowsShutDownNormally,
    },
    confirmLabel: label,
    confirmKey: Key('confirm-power-$action-$desktopId'),
    cancelKey: Key('cancel-power-$action-$desktopId'),
  );
}

/// Everything a launcher tile or list row can do for one desktop. A null
/// callback is an unavailable action; the reason strings explain why.
class DesktopActions {
  const DesktopActions({
    this.primary,
    this.stream,
    this.streamUnavailable,
    this.allMonitors = false,
    this.allMonitorsReason,
    this.localMonitors = 0,
    this.quality,
    this.showStart = false,
    this.start,
    this.reboot,
    this.shutdown,
    this.stop,
    this.stopReason,
    this.showPower = false,
    this.pcPower = false,
    this.powerReason,
    this.details,
    this.attention,
    this.cancel,
    this.menu = true,
    this.hostUpdate,
    this.console,
    this.consoleReason,
    this.favorite,
    this.toggleFavorite,
    this.working,
    this.powerSince,
  });

  /// What this client is doing for the desktop right now, as one sentence
  /// (opening its Console or its remote window, asking for a Host update):
  /// the tile's status shows it beside a moving mark until the work ends.
  /// Null: nothing of the kind.
  final String? working;

  /// When this client asked for the power task that is running, by its own
  /// clock: the main button counts the wait from it. Null for a task it did
  /// not start (no time is shown then).
  final DateTime? powerSince;

  /// The tile's default action: stream windowed, or power on
  /// and connect when the VM is off.
  final VoidCallback? primary;

  /// Streams (or powers on, then streams) in a chosen mode.
  final ValueChanged<SessionDisplayMode>? stream;
  final String? streamUnavailable;
  final bool allMonitors;
  final String? allMonitorsReason;
  final int localMonitors;
  final VoidCallback? quality;
  final bool showStart, showPower;

  /// An approved PC (pc-power-v1): 켜기 while it is offline, 종료 and 다시
  /// 시작 while its Host is online, and never 정지.
  final bool pcPower;

  /// 켜기, 다시 시작, 종료 (graceful shutdown) and 정지 (immediate
  /// power-off, Proxmox stop).
  final VoidCallback? start, reboot, shutdown, stop;

  /// Why the power actions are unavailable right now (tooltip on the greyed
  /// items); null when the VM state alone explains it.
  final String? powerReason;

  /// Why 정지 alone is unavailable, when [powerReason] does not say it.
  final String? stopReason;
  final VoidCallback? details;

  /// The fix behind the main button on a tile that needs attention (a
  /// refresh, or a restart for a restart-required state).
  final VoidCallback? attention;

  /// Stops an automatic power-on-then-connect.
  final VoidCallback? cancel;

  /// Whether the connection menu exists (not in the design demo).
  final bool menu;

  /// Any account the VM is assigned to, while the broker reports a newer
  /// Host that can be installed remotely now ("Host 업데이트 · 1.2.3 →
  /// 1.2.4"). Null lists no row at all: the Host version and update state
  /// are information in the readiness check instead.
  final VoidCallback? hostUpdate;
  final VoidCallback? console;

  /// Why 콘솔 cannot open now: its row stays, greyed, with this
  /// (Android); null with no [console] leaves the row out (Windows).
  final String? consoleReason;

  /// This desktop is a favourite; null where the broker has no favourites
  /// (the menu then lists no row). [toggleFavorite] flips it (optimistically).
  final bool? favorite;
  final VoidCallback? toggleFavorite;
}

/// Which part of the tile menu a trigger opens. ▾ chooses how to connect, ⋯
/// manages the VM, ⏻ holds the power actions (on its own button, not in
/// ⋯), and a right click shows all three (one menu behind both buttons
/// would read as a duplicate).
enum DesktopMenuKind { connect, manage, power, all }

/// The tile menu in the shared menu geometry ([AppMenu]). Connect: streaming
/// modes, stream quality and the broker-supported emergency console. Manage:
/// an installable Host update, favourite and the readiness checklist. Power:
/// 켜기, 종료, 다시 시작 and, apart in red, 정지 (Proxmox's start / shutdown / reboot / stop; never
/// reset).
///
/// No virtual-monitor count here: connecting already prepares
/// one output for a window or full screen and two for 모든 모니터, and the
/// session toolbar keeps its own count control.
///
/// Android shows the same menu ([touch]), less the window modes
/// it does not have ([mode] null): 48 dp rows, and a greyed row says why
/// under its label (no hover on a phone).
class DesktopMenu extends StatefulWidget {
  const DesktopMenu({
    super.key,
    required this.machine,
    required this.actions,
    required this.mode,
    required this.controller,
    required this.child,
    this.kind = DesktopMenuKind.all,
    this.anchorRect,
    this.touch = false,
  });

  final DesktopMachine machine;
  final DesktopActions actions;

  /// The default connection mode; null: no window modes (Android).
  final SessionDisplayMode? mode;
  final MenuController controller;
  final Widget child;
  final DesktopMenuKind kind;
  final bool touch;

  /// The actual trigger in [child]'s local coordinates. The viewport flip must
  /// use this button, not the whole (potentially very tall) desktop card.
  final Rect? anchorRect;

  @override
  State<DesktopMenu> createState() => _DesktopMenuState();
}

class _DesktopMenuState extends State<DesktopMenu> {
  final _anchorFocus = FocusNode(
    debugLabel: 'Desktop menu anchor',
    skipTraversal: true,
  );
  FocusNode? _returnFocus;

  @override
  void dispose() {
    AppMenu.closed(widget.controller);
    _anchorFocus.dispose();
    super.dispose();
  }

  void _opened() {
    // MenuAnchor requests child focus before onOpen; Flutter applies that
    // request after this synchronous callback, so retain the prior focus.
    _returnFocus ??= FocusManager.instance.primaryFocus;
    AppMenu.opened(widget.controller);
  }

  void _closed() {
    final kind = widget.kind;
    AppMenu.closed(widget.controller);
    WidgetsBinding.instance.addPostFrameCallback((_) {
      // Switching between two controls of this card reopens in this frame.
      // Do not restore stale focus over the new menu or a dialog's focus.
      if (!mounted || widget.controller.isOpen || widget.kind != kind) {
        return;
      }
      final previous = _returnFocus;
      _returnFocus = null;
      if (_anchorFocus.hasFocus &&
          previous != null &&
          previous.context?.mounted == true &&
          previous.canRequestFocus) {
        previous.requestFocus();
      }
    });
  }

  @override
  Widget build(BuildContext context) {
    final machine = widget.machine,
        actions = widget.actions,
        mode = widget.mode;
    final controller = widget.controller,
        child = widget.child,
        kind = widget.kind,
        anchorRect = widget.anchorRect;
    final p = AppPalette.of(context);
    final id = machine.id;
    final touch = widget.touch;
    Widget item({
      required String key,
      required IconData icon,
      required Color color,
      required String label,
      VoidCallback? onPressed,
      String? note,
      String? disabledReason,
      bool danger = false,
    }) {
      final reason = onPressed == null ? disabledReason : null;
      // A destructive action reads red (the label too) while enabled.
      final red = !danger
          ? null
          : ButtonStyle(
              foregroundColor: WidgetStateProperty.resolveWith(
                (states) => states.contains(WidgetState.disabled)
                    ? p.muted.withValues(alpha: .55)
                    : p.red,
              ),
            );
      final button = MenuItemButton(
        key: Key(key),
        onPressed: onPressed,
        style: !touch
            ? (red ?? const ButtonStyle()).merge(
                MenuItemButton.styleFrom(
                  minimumSize: const Size(0, 40),
                  maximumSize: const Size(double.infinity, double.infinity),
                  padding: const EdgeInsets.symmetric(horizontal: 12),
                  textStyle: uiText(size: 13, weight: FontWeight.w500),
                ),
              )
            : (red ?? const ButtonStyle()).merge(
                MenuItemButton.styleFrom(
                  minimumSize: const Size(0, 48),
                  maximumSize: const Size(double.infinity, double.infinity),
                  padding: const EdgeInsets.symmetric(horizontal: 16),
                  textStyle: uiText(size: 14),
                ),
              ),
        leadingIcon: AppMenu.icon(
          p,
          icon,
          color: !touch && color == p.amber && note == null ? p.muted : color,
          enabled: onPressed != null,
        ),
        // Name the launcher's default in words. An explicit choice connects
        // only this attempt and never becomes the next launcher's default.
        trailingIcon: note == null
            ? null
            : KeepAllText(
                note,
                key: Key('$key-note'),
                style: uiText(size: 12, color: p.muted),
              ),
        // By finger the reason is read under the label, not hovered.
        child: touch && reason != null
            ? ConstrainedBox(
                constraints: const BoxConstraints(maxWidth: 240),
                child: Padding(
                  padding: const EdgeInsets.symmetric(vertical: 6),
                  child: Column(
                    crossAxisAlignment: CrossAxisAlignment.start,
                    mainAxisSize: MainAxisSize.min,
                    children: [
                      KeepAllText(label),
                      const SizedBox(height: 2),
                      KeepAllText(
                        reason,
                        key: Key('$key-reason'),
                        style: uiText(size: 12, height: 1.35, color: p.muted),
                      ),
                    ],
                  ),
                ),
              )
            : KeepAllText(label),
      );
      return !touch && reason != null
          ? Tooltip(message: reason, child: button)
          : button;
    }

    Widget divider() => Padding(
      padding: const EdgeInsets.symmetric(vertical: 4),
      child: Divider(height: 1, color: p.line),
    );

    final stream = actions.stream;
    final allReason = actions.localMonitors < 2
        ? l10n.pcNeeds2Monitors
        : actions.allMonitorsReason ?? l10n.desktopsHostDoesntSupportAll;

    final connect = <Widget>[
      // Android has no window modes: 스트리밍 설정 and 콘솔 only.
      if (mode != null) ...[
        item(
          key: 'stream-mode-windowed',
          icon: Icons.web_asset_rounded,
          color: p.amber,
          label: l10n.windowed,
          onPressed: stream == null
              ? null
              : () => stream(SessionDisplayMode.windowed),
          note: mode == SessionDisplayMode.windowed ? l10n.defaultLabel : null,
          disabledReason: actions.streamUnavailable,
        ),
        item(
          key: 'stream-mode-fullscreen',
          icon: Icons.fullscreen_rounded,
          color: p.amber,
          label: l10n.fullScreen,
          onPressed: stream == null
              ? null
              : () => stream(SessionDisplayMode.fullscreen),
          note: mode == SessionDisplayMode.fullscreen ? l10n.defaultLabel : null,
          disabledReason: actions.streamUnavailable,
        ),
        item(
          key: 'stream-mode-allMonitors',
          icon: Icons.view_column_rounded,
          color: p.amber,
          label: l10n.allMonitors,
          onPressed: stream == null || !actions.allMonitors
              ? null
              : () => stream(SessionDisplayMode.allMonitors),
          note: mode == SessionDisplayMode.allMonitors ? l10n.defaultLabel : null,
          disabledReason: stream == null
              ? actions.streamUnavailable
              : allReason,
        ),
      ],
      item(
        key: 'stream-settings-$id',
        icon: Icons.tune_rounded,
        color: p.amber,
        label: l10n.streamingSettings2,
        onPressed: actions.quality,
        disabledReason: actions.streamUnavailable,
      ),
      if (actions.console != null || actions.consoleReason != null) ...[
        divider(),
        item(
          key: 'emergency-console-$id',
          icon: Icons.settings_remote_outlined,
          color: p.amber,
          label: 'Console',
          onPressed: actions.console,
          disabledReason: actions.consoleReason,
        ),
      ],
    ];

    final manage = <Widget>[
      // No sound-card row: the card is added in Proxmox; the
      // tile's sound badge still opens the dialog.
      // Listed only while a newer Host can be installed now; a greyed
      // "최신 버전" row would read as a broken button.
      if (actions.hostUpdate != null) ...[
        item(
          key: 'host-update-$id',
          icon: Icons.system_update_alt_rounded,
          color: p.amber,
          label: machine.hostUpdate.menuLabel,
          onPressed: actions.hostUpdate,
        ),
        divider(),
      ],
      if (actions.favorite != null)
        item(
          key: 'favorite-$id',
          icon: actions.favorite!
              ? Icons.star_rounded
              : Icons.star_outline_rounded,
          color: p.amber,
          label: actions.favorite! ? l10n.removeFromFavorites : l10n.addFavorites,
          onPressed: actions.toggleFavorite,
        ),
      item(
        key: 'details-$id',
        icon: Icons.fact_check_outlined,
        color: p.green,
        label: l10n.checkReadiness,
        onPressed: actions.details,
      ),
    ];

    final power = <Widget>[
      if (actions.showPower && actions.showStart)
        item(
          key: 'power-start-$id',
          icon: Icons.power_settings_new_rounded,
          color: p.green,
          label: l10n.start,
          onPressed: actions.start,
          disabledReason: actions.powerReason,
        ),
      // Proxmox's words: 종료 asks Windows to shut down, 정지
      // cuts the power at once and stands apart in red. An offline PC has
      // only 켜기; a PC never has 정지.
      if (actions.showPower && !(actions.pcPower && actions.showStart)) ...[
        item(
          key: 'power-shutdown-$id',
          icon: Icons.power_settings_new_rounded,
          color: p.amber,
          label: l10n.shutDown,
          onPressed: actions.shutdown,
          disabledReason: actions.powerReason,
        ),
        item(
          key: 'power-reboot-$id',
          icon: Icons.restart_alt_rounded,
          color: p.blue,
          label: l10n.restart,
          onPressed: actions.reboot,
          disabledReason: actions.powerReason,
        ),
        if (!actions.pcPower) ...[
          divider(),
          item(
            key: 'power-stop-$id',
            icon: Icons.power_off_rounded,
            color: p.red,
            label: l10n.stop,
            onPressed: actions.stop,
            disabledReason: actions.stopReason ?? actions.powerReason,
            danger: true,
          ),
        ],
      ],
    ];

    final popup = MenuAnchor(
      controller: controller,
      childFocusNode: anchorRect == null ? null : _anchorFocus,
      consumeOutsideTap: true,
      onOpen: _opened,
      onClose: _closed,
      style: AppMenu.menuStyle(
        p,
        minWidth: kind == DesktopMenuKind.power ? 168 : 216,
      ).copyWith(alignment: AlignmentDirectional.bottomStart),
      alignmentOffset: const Offset(0, 6),
      menuChildren: switch (kind) {
        DesktopMenuKind.connect => connect,
        DesktopMenuKind.manage => manage,
        DesktopMenuKind.power => power,
        // A right click shows everything, power included.
        DesktopMenuKind.all => [
          ...connect,
          divider(),
          ...manage,
          if (power.isNotEmpty) ...[divider(), ...power],
        ],
      },
      child: anchorRect == null
          ? child
          : Focus(
              focusNode: _anchorFocus,
              skipTraversal: true,
              includeSemantics: false,
              child: const SizedBox.expand(),
            ),
    );
    if (anchorRect == null) {
      return popup;
    }
    return Stack(
      fit: StackFit.passthrough,
      clipBehavior: Clip.none,
      children: [
        // Keep every original card control in the same menu tap region. A
        // switch from its footer to ⋯ is still one click; outside taps remain
        // consumed by MenuAnchor, and no invisible layer captures the button.
        TapRegion(groupId: controller, child: child),
        Positioned.fromRect(rect: anchorRect, child: popup),
      ],
    );
  }
}
