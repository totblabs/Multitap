import 'dart:async';

import 'package:flutter/material.dart';

import '../data/product_updates.dart';
import '../models/desktop_machine.dart';
import 'app_dialog.dart';
import 'app_form.dart';
import 'app_theme.dart';
import 'release_notes.dart';
import 'korean_text.dart';
import '../l10n/l10n.dart';

/// The administrator's answer to the remote Host update question.
enum HostUpdateChoice {
  /// Update now, or when the active session ends (the broker waits).
  update,

  /// End the active session and update now.
  disconnect,
}

/// Asks an administrator to update [machine]'s Host to the broker's latest
/// release, with the version step and its changelog. A second explicit
/// choice, "지금 업데이트" (disconnects), exists only while someone is connected.
Future<HostUpdateChoice?> showHostUpdateConfirm(
  BuildContext context, {
  required DesktopMachine machine,
  required String server,
  Future<ProductRelease?> Function()? loadRelease,
}) => showDialog<HostUpdateChoice>(
  context: context,
  builder: (_) => HostUpdateConfirmDialog(
    machine: machine,
    server: server,
    loadRelease: loadRelease,
  ),
);

class HostUpdateConfirmDialog extends StatefulWidget {
  const HostUpdateConfirmDialog({
    super.key,
    required this.machine,
    required this.server,
    this.loadRelease,
    this.api,
  });
  final DesktopMachine machine;
  final String server;

  /// The catalog entry of the offered version; defaults to this server's
  /// public download catalog. Nothing is downloaded or installed here.
  final Future<ProductRelease?> Function()? loadRelease;

  /// This server's catalog connection (tests lend one); by default the
  /// dialog opens its own and closes it with itself.
  final ProductUpdates? api;

  @override
  State<HostUpdateConfirmDialog> createState() =>
      _HostUpdateConfirmDialogState();
}

class _HostUpdateConfirmDialogState extends State<HostUpdateConfirmDialog> {
  ProductUpdates? _owned;
  ProductUpdates? get _api => widget.api ?? _owned;
  ProductRelease? _release;
  bool _loading = true, _details = false;
  final _scroll = ScrollController();

  String get _current => widget.machine.hostUpdate.version ?? '';
  String get _latest => widget.machine.hostUpdate.latestVersion ?? '';

  @override
  void initState() {
    super.initState();
    unawaited(_load());
  }

  Future<void> _load() async {
    ProductRelease? release;
    try {
      if (widget.loadRelease != null) {
        release = await widget.loadRelease!();
      } else {
        final api = widget.api ?? (_owned = ProductUpdates(widget.server));
        release = await api.check('host', _current);
      }
    } catch (_) {
      release = null;
    }
    if (!mounted) return;
    setState(() {
      // Only the notes of exactly the offered version.
      _release = release?.version == _latest ? release : null;
      _loading = false;
    });
  }

  @override
  void dispose() {
    _owned?.close();
    _scroll.dispose();
    super.dispose();
  }

  @override
  Widget build(BuildContext context) {
    final p = AppPalette.of(context);
    final session = widget.machine.hostUpdate.sessionActive;
    final notes = _release?.changelog.trim() ?? '';
    // Local PCs get Host updates too; the texts name what it is.
    final kind = widget.machine.kind == MachineKind.pc
        ? l10n.localPcInSentence
        : l10n.desktopInSentence;
    return AppDialog(
      key: const Key('host-update-dialog'),
      icon: Icons.system_update_alt_rounded,
      title: l10n.updateHost,
      subtitle: widget.machine.name,
      // Three short one-line buttons need a little more room than two.
      maxWidth: session ? 520 : 480,
      body: [
        Wrap(
          spacing: 8,
          runSpacing: 6,
          crossAxisAlignment: WrapCrossAlignment.center,
          children: [
            VersionBadge(l10n.current4(_current)),
            Icon(Icons.arrow_forward_rounded, size: 16, color: p.muted),
            VersionBadge(
              l10n.newVersion5(_latest),
              key: const Key('host-update-new-version'),
              tone: AppTone.accent,
            ),
          ],
        ),
        const SizedBox(height: 10),
        Row(
          children: [
            Expanded(
              // The client update dialog's heading for the same list.
              child: KeepAllText(
                l10n.whatsNewVersion,
                style: uiText(
                  size: 12,
                  weight: FontWeight.w600,
                  color: p.muted,
                ),
              ),
            ),
            // Every Host version's notes, from this server's history.
            TextButton.icon(
              key: const Key('open-host-release-history'),
              onPressed: () => unawaited(
                showReleaseHistory(
                  context,
                  server: widget.server,
                  product: 'host',
                  currentVersion: _current,
                  api: _api,
                ),
              ),
              style: TextButton.styleFrom(
                padding: const EdgeInsets.symmetric(horizontal: 8),
                minimumSize: const Size(0, 32),
                tapTargetSize: MaterialTapTargetSize.shrinkWrap,
              ),
              icon: const Icon(Icons.history_rounded, size: 16),
              label: KeepAllText(l10n.earlierChanges),
            ),
          ],
        ),
        const SizedBox(height: 2),
        ConstrainedBox(
          constraints: const BoxConstraints(maxHeight: 200),
          child: DecoratedBox(
            decoration: BoxDecoration(
              color: p.tileWell,
              borderRadius: BorderRadius.circular(12),
              border: Border.all(color: p.tileLine),
            ),
            child: Scrollbar(
              controller: _scroll,
              thumbVisibility: true,
              child: SingleChildScrollView(
                key: const Key('host-update-notes'),
                controller: _scroll,
                padding: const EdgeInsets.fromLTRB(14, 12, 18, 12),
                child: _loading
                    ? const LinearProgressIndicator()
                    : notes.isEmpty
                    ? KeepAllText(
                        _release == null
                            ? l10n.couldNotLoadChangeList
                            : l10n.noChangeLogRegisteredServer,
                        style: uiText(size: 13, color: p.muted),
                      )
                    : SelectionArea(child: ChangelogText(notes, keepWords: true)),
              ),
            ),
          ),
        ),
        const SizedBox(height: 12),
        // Two short lines, for concise reading; how it is checked stays
        // behind 자세히.
        AppDialog.message(
          context,
          l10n.hostDownloadsFromServerInstalls(kind),
        ),
        AppDisclosure(
          headerKey: const Key('host-update-details'),
          label: _details ? l10n.hideDetails : l10n.details,
          open: _details,
          onChanged: (open) => setState(() => _details = open),
        ),
        if (_details)
          KeepAllText(
            l10n.fileSizeSha256Checked(kind),
            key: const Key('host-update-details-text'),
            style: uiText(size: 12.5, height: 1.55, color: p.muted),
          ),
        if (session) ...[
          const SizedBox(height: 12),
          Container(
            key: const Key('host-update-session-note'),
            padding: const EdgeInsets.all(12),
            decoration: BoxDecoration(
              color: p.warnSoft,
              borderRadius: BorderRadius.circular(12),
              border: Border.all(color: p.warnLine),
            ),
            child: KeepAllText(
              l10n.someoneConnectedNowUpdateWhen(kind),
              style: uiText(size: 13, height: 1.5, color: p.ink),
            ),
          ),
        ],
      ],
      actions: [
        TextButton(
          key: const Key('host-update-cancel'),
          onPressed: () => Navigator.pop(context),
          style: AppButtons.quiet(p),
          child: KeepAllText(l10n.cancel),
        ),
        if (session) ...[
          OutlinedButton(
            key: const Key('host-update-when-idle'),
            onPressed: () => Navigator.pop(context, HostUpdateChoice.update),
            style: AppButtons.outlined(p),
            child: KeepAllText(l10n.updateWhenDone, maxLines: 1),
          ),
          FilledButton(
            key: const Key('host-update-now'),
            onPressed: () =>
                Navigator.pop(context, HostUpdateChoice.disconnect),
            style: AppButtons.filled(p, tone: AppTone.danger),
            child: KeepAllText(l10n.updateNow, maxLines: 1),
          ),
        ] else
          FilledButton(
            key: const Key('host-update-confirm'),
            autofocus: true,
            onPressed: () => Navigator.pop(context, HostUpdateChoice.update),
            style: AppButtons.filled(p),
            child: KeepAllText(l10n.update),
          ),
      ],
    );
  }
}
