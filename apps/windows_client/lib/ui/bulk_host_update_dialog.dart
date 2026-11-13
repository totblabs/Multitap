import 'dart:async';

import 'package:flutter/foundation.dart';
import 'package:flutter/material.dart';

import '../data/broker_api.dart';
import '../data/bulk_host_updates.dart';
import '../data/product_updates.dart';
import '../models/desktop_machine.dart';
import 'app_dialog.dart';
import 'app_form.dart';
import 'app_theme.dart';
import 'korean_text.dart';
import 'release_notes.dart';
import '../l10n/l10n.dart';

/// Only the signed-in account's assigned list belongs here. Either observation
/// source is required to execute; this dialog never discovers other desktops.
Future<void> showBulkHostUpdateDialog(
  BuildContext context, {
  required HostUpdateBrokerApi api,
  required String token,
  required String server,
  required List<DesktopMachine> assignedMachines,
  ValueListenable<List<DesktopMachine>>? liveMachines,
  Future<List<DesktopMachine>> Function()? refreshAssigned,
  Future<ProductRelease?> Function()? loadRelease,
  bool administrator = false,
}) => showDialog<void>(
  context: context,
  barrierDismissible: false,
  builder: (_) => BulkHostUpdateDialog(
    api: api,
    token: token,
    server: server,
    assignedMachines: assignedMachines,
    liveMachines: liveMachines,
    refreshAssigned: refreshAssigned,
    loadRelease: loadRelease,
    administrator: administrator,
  ),
);

class BulkHostUpdateDialog extends StatefulWidget {
  const BulkHostUpdateDialog({
    super.key,
    required this.api,
    required this.token,
    required this.server,
    required this.assignedMachines,
    this.liveMachines,
    this.refreshAssigned,
    this.loadRelease,
    this.administrator = false,
    this.pollInterval = const Duration(seconds: 3),
    this.controllerFactory,
  });
  final HostUpdateBrokerApi api;
  final String token, server;
  final List<DesktopMachine> assignedMachines;

  /// Only what the notice above the button says: an administrator's request
  /// ends every connection, an ordinary account's only its own (the broker
  /// decides either way).
  final bool administrator;
  final ValueListenable<List<DesktopMachine>>? liveMachines;
  final Future<List<DesktopMachine>> Function()? refreshAssigned;
  final Future<ProductRelease?> Function()? loadRelease;
  final Duration pollInterval;
  @visibleForTesting
  final BulkHostUpdateController Function(ProductRelease, List<DesktopMachine>)?
  controllerFactory;
  @override
  State<BulkHostUpdateDialog> createState() => _BulkHostUpdateDialogState();
}

class _BulkHostUpdateDialogState extends State<BulkHostUpdateDialog> {
  BulkHostUpdateController? _controller;
  ProductUpdates? _catalog;
  Timer? _poll;
  bool _loading = true, _refreshing = false, _observationFailed = false;
  int _observationRevision = 0;
  bool get _observed =>
      widget.liveMachines != null || widget.refreshAssigned != null;

  @override
  void initState() {
    super.initState();
    widget.liveMachines?.addListener(_liveChanged);
    unawaited(_load());
  }

  Future<void> _load() async {
    ProductRelease? release;
    try {
      release = widget.loadRelease != null
          ? await widget.loadRelease!()
          : await (_catalog = ProductUpdates(
              widget.server,
            )).check('host', '0.0.0');
    } catch (_) {}
    if (!mounted) return;
    final latest = release;
    if (latest != null && latest.product == 'host') {
      try {
        final assigned = widget.liveMachines?.value ?? widget.assignedMachines;
        _controller =
            (widget.controllerFactory?.call(latest, assigned) ??
                  BulkHostUpdateController(
                    api: widget.api,
                    token: widget.token,
                    release: latest,
                    assignedMachines: assigned,
                  ))
              ..addListener(_changed);
      } catch (_) {}
    }
    setState(() => _loading = false);
    if (widget.refreshAssigned != null) {
      unawaited(_refresh());
      _poll = Timer.periodic(widget.pollInterval, (_) {
        if (_controller?.hasServerWork == true ||
            _controller?.submitting == true) {
          unawaited(_refresh());
        }
      });
    }
  }

  void _liveChanged() {
    // A response requested before this push cannot replace its observation.
    ++_observationRevision;
    _controller?.synchronize(widget.liveMachines!.value);
    if (mounted) setState(() => _observationFailed = false);
  }

  void _changed() {
    if (mounted) setState(() {});
  }

  Future<void> _refresh() async {
    if (!mounted || _refreshing || widget.refreshAssigned == null) return;
    final revision = _observationRevision;
    _refreshing = true;
    try {
      final assigned = await widget.refreshAssigned!();
      if (!mounted || revision != _observationRevision) return;
      _controller?.synchronize(assigned);
      _observationFailed = false;
    } catch (_) {
      if (mounted && revision == _observationRevision) {
        _observationFailed = true;
      }
    } finally {
      _refreshing = false;
      if (mounted) setState(() {});
    }
  }

  /// The notice above the button says what it does (connections end), so it
  /// acts at once (no second question). The controller and each
  /// broker request recheck assignment and catalog.
  void _submit() {
    final controller = _controller;
    if (controller == null ||
        !_observed ||
        controller.submitting ||
        controller.selectedCount == 0) {
      return;
    }
    unawaited(controller.submitSelected());
  }

  void _close() {
    _controller?.stopPending();
    Navigator.pop(context);
  }

  @override
  void dispose() {
    _poll?.cancel();
    widget.liveMachines?.removeListener(_liveChanged);
    _controller?.removeListener(_changed);
    _controller?.dispose();
    _catalog?.close();
    super.dispose();
  }

  @override
  Widget build(BuildContext context) {
    final p = AppPalette.of(context), controller = _controller;
    final visibleRows =
        controller?.rows.where((row) => row.assigned).toList() ??
        const <BulkHostUpdateRow>[];
    return PopScope(
      canPop: true,
      onPopInvokedWithResult: (didPop, _) {
        if (didPop) _controller?.stopPending();
      },
      child: AppDialog(
        key: const Key('bulk-host-update-dialog'),
        title: l10n.hostBulkUpdate,
        subtitle: controller == null
            ? l10n.hostsAssignedDesktops
            : l10n.assignedDesktopsLatest(visibleRows.length, controller.release.version),
        icon: Icons.system_update_alt_rounded,
        maxWidth: 720,
        body: [
          if (_loading) ...[
            const LinearProgressIndicator(),
            const SizedBox(height: 12),
            AppDialog.message(context, l10n.checkingServersLatestHostRelease),
          ] else if (controller == null)
            AppDialog.message(
              context,
              l10n.couldntLoadLatestHostRelease,
            ),
          if (controller != null) ...[
            AppDisclosure(
              key: const Key('bulk-host-notes'),
              label: l10n.hostChanges(controller.release.version),
              initiallyOpen: true,
              children: [
                Padding(
                  padding: const EdgeInsets.only(bottom: 12),
                  child: controller.release.changelog.trim().isEmpty
                      ? AppDialog.message(context, l10n.noChangeLogRegisteredServer)
                      : SelectionArea(
                          child: ChangelogText(controller.release.changelog, keepWords: true),
                        ),
                ),
              ],
            ),
            if (!_observed)
              _notice(
                p,
                l10n.theresNoConnectionCheckInstall,
                'bulk-host-no-observer',
              ),
            if (_observationFailed)
              _notice(
                p,
                l10n.couldntGetLatestStatusFrom,
                'bulk-host-observation-failed',
              ),
            if (controller.authorizationStopped)
              _notice(
                p,
                l10n.remainingRequestsStoppedBecauseYou,
                'bulk-host-authorization',
              ),
            if (controller.catalogChanged)
              _notice(
                p,
                l10n.latestReleaseChangedSoRemaining,
                'bulk-host-catalog-changed',
              ),
            // One line for the whole run while desktops still wait their
            // turn: it does not come and go between two requests (the rows
            // under it would move each time).
            if (controller.pacing ||
                (controller.submitting && controller.pendingCount > 0))
              _notice(
                p,
                l10n.waitingNextRequestLeft(controller.pendingCount),
                'bulk-host-queue-wait',
              ),
            // The head of the list: one box that selects every desktop that
            // can be updated, or clears the selection.
            Row(
              children: [
                Expanded(child: _selectAll(p, controller)),
                if (widget.refreshAssigned != null)
                  IconButton(
                    key: const Key('bulk-host-refresh'),
                    tooltip: l10n.refreshInstallStatus,
                    onPressed: _refreshing ? null : () => unawaited(_refresh()),
                    icon: const Icon(Icons.refresh_rounded, size: 20),
                  ),
              ],
            ),
            if (visibleRows.isEmpty)
              AppDialog.message(context, l10n.noDesktopsAssigned2),
            for (final row in visibleRows) _row(p, controller, row),
            if (controller.summary case final summary?) ...[
              const SizedBox(height: 12),
              KeepAllText(
                controller.finished &&
                        (controller.hasServerWork ||
                            controller.checkingCount > 0)
                    ? summary.replaceFirst(l10n.allFinished, l10n.checkProgress)
                    : summary,
                key: const Key('bulk-host-summary'),
                style: uiText(
                  size: 13,
                  weight: FontWeight.w600,
                  color: controller.finished ? p.ink : p.muted,
                ),
              ),
            ],
            // Not beside "모두 끝났어요": every request has its result then.
            if (controller.requestedCount > 0 &&
                !(controller.finished &&
                    !controller.hasServerWork &&
                    controller.checkingCount == 0))
              AppDialog.message(
                context,
                l10n.requestServerAcceptedIsntFinished,
              ),
            if (controller.rebootCount > 0 || controller.checkingCount > 0)
              Padding(
                padding: const EdgeInsets.only(top: 6),
                child: AppStatusLine(
                  l10n.windowsRestartNeededCheckingResults(controller.rebootCount, controller.checkingCount),
                  tone: AppTone.warn,
                ),
              ),
          ],
        ],
        // Right above the button, and always in view: what it does to open
        // connections (a bulk update disconnects instead of waiting).
        footer: controller == null
            ? null
            : Container(
                key: const Key('bulk-host-disconnect-note'),
                padding: const EdgeInsets.all(12),
                decoration: BoxDecoration(
                  color: p.warnSoft,
                  borderRadius: BorderRadius.circular(12),
                  border: Border.all(color: p.warnLine),
                ),
                child: KeepAllText(
                  widget.administrator
                      ? l10n.connectedDesktopsDisconnectedUpdatedRight
                      : l10n.connectedDesktopsConnectionEndsUpdate,
                  style: uiText(size: 13, height: 1.5, color: p.ink),
                ),
              ),
        actions: [
          TextButton(
            key: const Key('bulk-host-close'),
            onPressed: _close,
            style: AppButtons.quiet(p),
            child: KeepAllText(
              controller?.submitting == true ? l10n.stopRemainingRequestsClose : l10n.close,
            ),
          ),
          if (controller?.submitting == true)
            OutlinedButton(
              key: const Key('bulk-host-stop'),
              onPressed: controller!.stopped ? null : controller.stopPending,
              style: AppButtons.outlined(p),
              child: KeepAllText(l10n.stopRemainingRequests),
            )
          else if (controller != null)
            FilledButton(
              key: const Key('bulk-host-submit'),
              onPressed:
                  _observed &&
                      controller.canSubmit &&
                      controller.selectedCount > 0 &&
                      !controller.authorizationStopped &&
                      !controller.catalogChanged
                  ? _submit
                  : null,
              style: AppButtons.filled(p),
              child: KeepAllText(l10n.updateSelected(controller.selectedCount)),
            ),
        ],
      ),
    );
  }

  // One yellow icon and an ordinary sentence: a whole sentence in yellow
  // read as log output.
  Widget _notice(AppPalette p, String text, String key) => Padding(
    padding: const EdgeInsets.only(bottom: 10),
    child: AppStatusLine(text, textKey: Key(key), tone: AppTone.warn),
  );

  /// "모두 선택": checked when every desktop that can be updated is chosen,
  /// a dash when some are. One press chooses them all; a press on the full
  /// box clears the choice.
  Widget _selectAll(AppPalette p, BulkHostUpdateController controller) {
    final eligible = controller.rows.where((row) => row.eligible).length;
    final chosen = controller.selectedCount;
    final all = eligible > 0 && chosen >= eligible;
    final enabled =
        eligible > 0 &&
        !controller.submitting &&
        !controller.catalogChanged &&
        !controller.authorizationStopped;
    void toggle() => controller.selectAll(!all);
    return Align(
      alignment: Alignment.centerLeft,
      child: MergeSemantics(
        child: InkWell(
          key: const Key('bulk-host-select-all'),
          onTap: enabled ? toggle : null,
          borderRadius: BorderRadius.circular(8),
          child: Padding(
            padding: const EdgeInsets.only(right: 10),
            child: Row(
              mainAxisSize: MainAxisSize.min,
              children: [
                // The row is the control; the box only shows the state.
                ExcludeFocus(
                  child: IgnorePointer(
                    child: Checkbox(
                      tristate: true,
                      value: all
                          ? true
                          : chosen > 0
                          ? null
                          : false,
                      onChanged: enabled ? (_) => toggle() : null,
                    ),
                  ),
                ),
                Flexible(
                  child: KeepAllText(
                    l10n.selectAll2,
                    style: uiText(
                      size: 13,
                      weight: FontWeight.w600,
                      color: enabled ? p.ink : p.disabled,
                    ),
                  ),
                ),
              ],
            ),
          ),
        ),
      ),
    );
  }

  Widget _stageChip(AppPalette p, BulkHostUpdateRow row, Color color) =>
      Container(
        key: ValueKey('bulk-host-stage-${row.id}'),
        padding: const EdgeInsets.symmetric(horizontal: 8, vertical: 3),
        decoration: BoxDecoration(
          color: color.withValues(alpha: .14),
          borderRadius: BorderRadius.circular(7),
          border: Border.all(color: color.withValues(alpha: .38)),
        ),
        child: Text(
          row.stage,
          style: uiText(
            size: 12,
            weight: FontWeight.w600,
            height: 1.3,
            color: color,
          ),
        ),
      );

  /// "데스크톱 · 1.2.3 → 1.2.4"; a Host already on the target version
  /// (the update finished) reads that one version, never "1.2.4 → 1.2.4".
  static String _versionLine(BulkHostUpdateRow row) {
    final kind = row.machine.kind == MachineKind.pc
        ? l10n.localPc
        : l10n.desktop;
    final current = row.machine.hostUpdate.version;
    return current != null && current == row.targetVersion
        ? '$kind · $current'
        : '$kind · ${current ?? l10n.currentVersionUnknown} → ${row.targetVersion}';
  }

  Widget _row(
    AppPalette p,
    BulkHostUpdateController controller,
    BulkHostUpdateRow row,
  ) {
    final color = switch (row.phase) {
      BulkHostPhase.completed => p.green,
      BulkHostPhase.failed => p.red,
      BulkHostPhase.uncertain ||
      BulkHostPhase.rebootRequired ||
      BulkHostPhase.needsCheck => p.warn,
      BulkHostPhase.available ||
      BulkHostPhase.unavailable ||
      BulkHostPhase.stopped => p.muted,
      _ => p.blue,
    };
    final scale = MediaQuery.textScalerOf(context).scale(1);
    return Padding(
      padding: const EdgeInsets.symmetric(vertical: 4),
      child: DecoratedBox(
        decoration: BoxDecoration(
          color: p.tileWell,
          borderRadius: BorderRadius.circular(12),
          border: Border.all(color: p.tileLine),
        ),
        child: Padding(
          padding: const EdgeInsets.fromLTRB(4, 8, 12, 8),
          child: Row(
            crossAxisAlignment: CrossAxisAlignment.start,
            children: [
              Checkbox(
                key: ValueKey('bulk-host-select-${row.id}'),
                value: row.selected,
                activeColor: p.amber,
                checkColor: p.onAmber,
                onChanged:
                    row.eligible &&
                        !controller.submitting &&
                        !controller.authorizationStopped &&
                        !controller.catalogChanged
                    ? (value) => controller.select(row.id, value == true)
                    : null,
              ),
              Expanded(
                child: Column(
                  crossAxisAlignment: CrossAxisAlignment.start,
                  children: [
                    // Two lines: which desktop and which version, then its
                    // state. The badge is only for a state that is not
                    // simply "can be updated" (the second line says that).
                    Row(
                      children: [
                        Expanded(
                          child: Wrap(
                            spacing: 8,
                            runSpacing: 2,
                            crossAxisAlignment: WrapCrossAlignment.center,
                            children: [
                              Text(
                                row.machine.name,
                                style: uiText(
                                  size: 14,
                                  weight: FontWeight.w600,
                                  color: p.ink,
                                ),
                              ),
                              KeepAllText(
                                _versionLine(row),
                                key: ValueKey('bulk-host-version-${row.id}'),
                                style: uiText(size: 12, color: p.muted),
                              ),
                            ],
                          ),
                        ),
                        if (row.phase != BulkHostPhase.available) ...[
                          const SizedBox(width: 8),
                          _stageChip(p, row, color),
                        ],
                      ],
                    ),
                    const SizedBox(height: 4),
                    // The colour is the dot's; the sentence is ordinary text.
                    Row(
                      crossAxisAlignment: CrossAxisAlignment.start,
                      children: [
                        Padding(
                          padding: EdgeInsets.only(
                            top: (12 * 1.5 * scale - 6) / 2,
                          ),
                          child: Container(
                            width: 6,
                            height: 6,
                            decoration: BoxDecoration(
                              color: color,
                              shape: BoxShape.circle,
                            ),
                          ),
                        ),
                        const SizedBox(width: 8),
                        Expanded(
                          child: KeepAllText(
                            row.message,
                            key: ValueKey('bulk-host-status-${row.id}'),
                            style: uiText(
                              size: 12,
                              height: 1.5,
                              color: p.muted,
                            ),
                          ),
                        ),
                      ],
                    ),
                    // Every state in which the request or the Host is at work
                    // moves (the server took it and the Host has it too);
                    // only the download reports how far it is.
                    if (switch (row.phase) {
                      BulkHostPhase.requesting ||
                      BulkHostPhase.acknowledged ||
                      BulkHostPhase.downloading ||
                      BulkHostPhase.verifying ||
                      BulkHostPhase.installing ||
                      BulkHostPhase.reconnecting => true,
                      _ => false,
                    }) ...[
                      const SizedBox(height: 8),
                      LinearProgressIndicator(
                        key: ValueKey('bulk-host-progress-${row.id}'),
                        value:
                            row.phase == BulkHostPhase.downloading &&
                                row.operation?.progress != null
                            ? row.operation!.progress! / 100
                            : null,
                      ),
                    ],
                    if (row.phase == BulkHostPhase.needsCheck &&
                        row.operation?.rebootRequired == true)
                      Padding(
                        padding: EdgeInsets.only(top: 4),
                        child: AppStatusLine(
                          l10n.windowsRestartNeeded,
                          tone: AppTone.warn,
                          size: 12,
                        ),
                      ),
                  ],
                ),
              ),
            ],
          ),
        ),
      ),
    );
  }
}
