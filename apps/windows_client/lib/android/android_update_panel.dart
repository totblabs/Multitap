import 'dart:async';

import 'package:flutter/material.dart';

import '../data/product_updates.dart' show UpdateFailure;
import '../models/desktop_machine.dart';
import '../ui/app_dialog.dart';
import '../ui/app_theme.dart';
import '../ui/korean_text.dart';
import '../ui/release_notes.dart';
import 'android_update_dialog.dart';
import 'android_updates.dart';
import '../l10n/l10n.dart';

/// The Android update panel, the phone form of the Windows client's:
/// the running version against the newest on the configured server, that
/// version's notes, and 지난 변경 내역 (every version's notes, a version
/// sidebar on wide screens and one timeline on a phone). Opening it only
/// reads the public catalog; the download starts from 업데이트 and the
/// Android installer still asks once more.
class AndroidUpdatePanel extends StatefulWidget {
  const AndroidUpdatePanel({
    super.key,
    required this.server,
    required this.current,
    required this.updates,
    required this.platform,
    this.onChecked,
    this.readCurrent,
    this.isCurrent,
    this.hostUpdates,
    this.hostUpdatesChanged,
    this.onHostUpdate,
  });
  final String server;

  /// Assigned desktops whose Host can be updated now (the header badge leads
  /// here), read again whenever [hostUpdatesChanged] fires. Each
  /// row closes the panel and opens that desktop's own Host update
  /// ([onHostUpdate] with its id): Android has no bulk Host update.
  final List<DesktopMachine> Function()? hostUpdates;
  final Listenable? hostUpdatesChanged;
  final ValueChanged<String>? onHostUpdate;

  /// The running build, from the installed package.
  final String? current;
  final Future<String?> Function()? readCurrent;
  final bool Function()? isCurrent;

  /// A fresh catalog reader per use (each one is closed by its owner).
  final AndroidUpdates Function() updates;
  final AndroidUpdatePlatform platform;

  /// The newer release this check found (null: none), for the header badge.
  final void Function(AndroidRelease? newer)? onChecked;

  @override
  State<AndroidUpdatePanel> createState() => _AndroidUpdatePanelState();
}

class _AndroidUpdatePanelState extends State<AndroidUpdatePanel> {
  late final AndroidUpdates _reader = widget.updates();
  AndroidRelease? _latest;
  String? _abi, _failure;
  bool _checking = true;
  late String? _current = widget.current;

  /// The server's latest against this app: above 0 newer, 0 the same, below
  /// 0 older (this app is newer than the server's, e.g. a preview); null
  /// unknown.
  int? get _order {
    if (widget.isCurrent?.call() == false) return null;
    final latest = _latest, current = _current;
    if (latest == null || current == null) return null;
    try {
      return compareAndroidVersions(latest.version, current);
    } on UpdateFailure {
      return null;
    }
  }

  bool get _newer => (_order ?? 0) > 0;
  bool get _older => (_order ?? 0) < 0;

  @override
  void initState() {
    super.initState();
    unawaited(_check());
  }

  @override
  void dispose() {
    _reader.close();
    super.dispose();
  }

  Future<void> _check() async {
    setState(() {
      _checking = true;
      _failure = null;
    });
    try {
      if (widget.readCurrent != null) {
        String? current;
        try {
          current = await widget.readCurrent!();
        } catch (_) {}
        if (!mounted || widget.isCurrent?.call() == false) return;
        _current = current;
      }
      if (_current == null) throw const UpdateFailure('version_unknown');
      compareAndroidVersions(_current!, _current!);
      final abi = _abi ??= await widget.platform.abi();
      if (abi == null) throw const UpdateFailure('abi_unavailable');
      final latest = await _reader.latest(abi);
      if (!mounted || widget.isCurrent?.call() == false) return;
      setState(() => _latest = latest);
      widget.onChecked?.call(_newer ? latest : null);
    } on UpdateFailure catch (error) {
      if (mounted) setState(() => _failure = error.code);
    } catch (_) {
      if (mounted) setState(() => _failure = 'catalog_unavailable');
    } finally {
      if (mounted) setState(() => _checking = false);
    }
  }

  void _history() {
    if (widget.isCurrent?.call() == false) return;
    final abi = _abi, current = _current;
    if (abi == null) return;
    unawaited(
      showDialog<void>(
        context: context,
        builder: (_) => ReleaseHistoryPanel(
          server: widget.server,
          product: 'android-client',
          productLabel: 'Android',
          currentVersion: current ?? '',
          compare: compareAndroidVersions,
          loadPage: ({String? before, int limit = 40}) =>
              _reader.historyPage(abi, before: before, limit: limit),
        ),
      ),
    );
  }

  Future<void> _install() async {
    if (widget.isCurrent?.call() == false) return;
    final latest = _latest, current = _current;
    if (latest == null || current == null) return;
    await showDialog<void>(
      context: context,
      barrierDismissible: false,
      builder: (_) => AndroidUpdateDialog(
        release: latest,
        current: current,
        updates: widget.updates(),
        platform: widget.platform,
      ),
    );
  }

  Widget _versionCard(AppPalette p, AndroidRelease latest) {
    final digits = uiText(
      size: 17,
      weight: FontWeight.w600,
      color: p.ink,
      height: 1.3,
    ).copyWith(fontFeatures: const [FontFeature.tabularFigures()]);
    Widget label(String text) => KeepAllText(
      text,
      style: uiText(size: 12, weight: FontWeight.w600, color: p.muted),
    );
    Widget version(String caption, String value, {Key? key, Color? color}) =>
        Column(
          mainAxisSize: MainAxisSize.min,
          crossAxisAlignment: CrossAxisAlignment.start,
          children: [
            label(caption),
            const SizedBox(height: 3),
            KeepAllText(
              value,
              key: key,
              style: digits.copyWith(color: color),
            ),
          ],
        );
    final meta = [
      if (latest.releasedAt != null) l10n.released(releaseDate(latest.releasedAt!)),
      '${(latest.size / 1048576).toStringAsFixed(1)} MB',
    ].join('  ·  ');
    return Container(
      key: const Key('android-update-version-card'),
      padding: const EdgeInsets.fromLTRB(16, 14, 16, 14),
      decoration: BoxDecoration(
        color: p.tileWell,
        borderRadius: BorderRadius.circular(14),
        border: Border.all(color: p.tileLine),
      ),
      child: Column(
        crossAxisAlignment: CrossAxisAlignment.start,
        children: [
          if (_newer)
            LayoutBuilder(
              builder: (context, constraints) {
                double width(String value) {
                  final painter = TextPainter(
                    text: TextSpan(text: value, style: digits),
                    textScaler: MediaQuery.textScalerOf(context),
                    textDirection: Directionality.of(context),
                  )..layout();
                  final result = painter.width;
                  painter.dispose();
                  return result;
                }

                final current = version(l10n.current3, _current!, color: p.muted);
                final next = version(
                  l10n.newVersion4,
                  latest.version,
                  key: const Key('android-update-new-version'),
                  color: p.amberInk,
                );
                // Keep each version readable as a unit before using two columns.
                // A phone or large text stacks the labels and keeps its full width.
                if (width(_current!) + width(latest.version) + 40 >
                    constraints.maxWidth) {
                  return Column(
                    crossAxisAlignment: CrossAxisAlignment.start,
                    children: [current, const SizedBox(height: 12), next],
                  );
                }
                return Row(
                  children: [
                    current,
                    Padding(
                      padding: const EdgeInsets.fromLTRB(10, 16, 10, 0),
                      child: Icon(
                        Icons.arrow_forward_rounded,
                        size: 20,
                        color: p.faintText,
                      ),
                    ),
                    Expanded(child: next),
                  ],
                );
              },
            )
          else
            Row(
              children: [
                Expanded(
                  child: Column(
                    crossAxisAlignment: CrossAxisAlignment.start,
                    children: [
                      label(l10n.installedVersion),
                      const SizedBox(height: 3),
                      KeepAllText(_current ?? '', style: digits),
                    ],
                  ),
                ),
                _older
                    ? VersionBadge(l10n.newerThanServer)
                    : VersionBadge(l10n.upDate, tone: AppTone.success),
              ],
            ),
          const SizedBox(height: 10),
          KeepAllText(meta, style: uiText(size: 12.5, color: p.muted)),
        ],
      ),
    );
  }

  /// The assigned Hosts with an installable update, each with its own
  /// 업데이트 that leads to that desktop's Host update question.
  Widget _hostOffer(
    AppPalette p,
    List<DesktopMachine> hosts,
    ValueChanged<String> open,
  ) => Container(
    key: const Key('host-update-offer'),
    padding: const EdgeInsets.fromLTRB(14, 12, 12, 12),
    // Host program updates: blue with a server glyph, never the app's amber,
    // so the two can be told apart.
    decoration: BoxDecoration(
      color: p.blueSoft,
      borderRadius: BorderRadius.circular(12),
      border: Border.all(color: p.blueLine),
    ),
    child: Column(
      crossAxisAlignment: CrossAxisAlignment.stretch,
      children: [
        Row(
          children: [
            Icon(Icons.dns_outlined, size: 18, color: p.blue),
            const SizedBox(width: 8),
            Expanded(
              child: KeepAllText(
                l10n.text4(hosts.length),
                style: uiText(
                  size: 13.5,
                  weight: FontWeight.w600,
                  color: p.ink,
                ),
              ),
            ),
          ],
        ),
        for (final machine in hosts) ...[
          const SizedBox(height: 10),
          Row(
            key: ValueKey('host-update-row-${machine.id}'),
            children: [
              Expanded(
                child: Column(
                  crossAxisAlignment: CrossAxisAlignment.start,
                  children: [
                    KeepAllText(
                      machine.name,
                      style: uiText(
                        size: 13.5,
                        weight: FontWeight.w600,
                        color: p.ink,
                      ),
                    ),
                    const SizedBox(height: 2),
                    KeepAllText(
                      '${machine.kind == MachineKind.pc ? l10n.localPc : 'VM'} · '
                      '${machine.hostUpdate.version} → '
                      '${machine.hostUpdate.latestVersion}',
                      style: uiText(size: 12, color: p.muted),
                    ),
                  ],
                ),
              ),
              const SizedBox(width: 10),
              FilledButton(
                key: ValueKey('open-host-update-${machine.id}'),
                onPressed: () {
                  Navigator.pop(context);
                  open(machine.id);
                },
                style: AppButtons.filled(p),
                child: Text(l10n.update),
              ),
            ],
          ),
        ],
      ],
    ),
  );

  @override
  Widget build(BuildContext context) {
    final p = AppPalette.of(context);
    final latest = _latest;
    final body = <Widget>[];
    if (_checking) {
      body.addAll([
        const LinearProgressIndicator(),
        const SizedBox(height: 10),
        KeepAllText(
          l10n.checkingServersUpdateList,
          style: uiText(size: 13.5, color: p.muted),
        ),
      ]);
    } else if (_failure != null) {
      body.add(
        KeepAllText(
          androidUpdateMessage(_failure!),
          key: const Key('android-update-panel-failure'),
          style: uiText(size: 13.5, height: 1.6, color: p.red),
        ),
      );
    } else if (latest == null) {
      body.add(
        KeepAllText(
          l10n.serverHasNoAndroidInstaller,
          key: const Key('android-update-none'),
          style: uiText(size: 13.5, height: 1.6, color: p.muted),
        ),
      );
    } else {
      final notes = latest.changelog.trim();
      body.addAll([
        _versionCard(p, latest),
        const SizedBox(height: 14),
        KeepAllText(
          // Never another version's notes under "지금 버전" (review #11).
          _newer
              ? l10n.whatsNewVersion
              : _older
              ? l10n.whatsNewServerVersion(latest.version)
              : l10n.whatsNewCurrentVersion,
          style: uiText(size: 12, weight: FontWeight.w600, color: p.muted),
        ),
        const SizedBox(height: 6),
        Container(
          key: const Key('android-update-changelog'),
          constraints: const BoxConstraints(maxHeight: 220),
          decoration: BoxDecoration(
            color: p.tileWell,
            borderRadius: BorderRadius.circular(12),
            border: Border.all(color: p.tileLine),
          ),
          child: SingleChildScrollView(
            padding: const EdgeInsets.fromLTRB(14, 12, 14, 12),
            child: notes.isEmpty
                ? KeepAllText(
                    l10n.noChangeLogRegisteredServer,
                    style: uiText(size: 13, color: p.muted),
                  )
                : SelectionArea(
                    child: ChangelogText(latest.changelog, keepWords: true),
                  ),
          ),
        ),
      ]);
    }
    final hostUpdates = widget.hostUpdates, open = widget.onHostUpdate;
    if (hostUpdates != null && open != null) {
      body.add(
        ListenableBuilder(
          listenable: Listenable.merge([?widget.hostUpdatesChanged]),
          builder: (context, _) {
            final hosts = hostUpdates();
            return hosts.isEmpty
                ? const SizedBox.shrink()
                : Padding(
                    padding: const EdgeInsets.only(top: 16),
                    child: _hostOffer(p, hosts, open),
                  );
          },
        ),
      );
    }
    // A phone on its side (or very large text) has no room for the versions
    // between a fixed title and three stacked buttons: about 40 px are left,
    // and at 200 % text 닫기 is cut. There the whole
    // dialog scrolls, so the versions, the notes and every button are reached.
    final media = MediaQuery.of(context);
    final short = media.size.height / media.textScaler.scale(1) < 540;
    return AppDialog(
      key: const Key('android-update-panel'),
      icon: Icons.system_update_alt_rounded,
      busy: _checking,
      title: l10n.appUpdate,
      subtitle: widget.server,
      scrollAll: short,
      body: body,
      actions: [
        TextButton(
          key: const Key('android-update-panel-close'),
          onPressed: () => Navigator.pop(context),
          style: AppButtons.quiet(p),
          child: Text(l10n.close),
        ),
        if (!_checking && _abi != null)
          TextButton.icon(
            key: const Key('android-open-release-history'),
            onPressed: _history,
            style: AppButtons.quiet(p),
            icon: const Icon(Icons.history_rounded, size: 18),
            label: Text(l10n.changeLog),
          ),
        if (!_checking && _failure != null)
          FilledButton(
            key: const Key('android-update-panel-retry'),
            onPressed: _check,
            style: AppButtons.filled(p),
            child: Text(l10n.checkAgain),
          ),
        if (!_checking && _newer)
          FilledButton(
            key: const Key('android-update-panel-install'),
            onPressed: _install,
            style: AppButtons.filled(p),
            child: Text(l10n.update),
          ),
      ],
    );
  }
}
