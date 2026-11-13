import 'dart:async';

import 'package:flutter/material.dart';

import '../data/product_updates.dart';
import '../data/product_log.dart';
import 'app_dialog.dart';
import 'app_progress.dart';
import 'app_theme.dart';
import 'app_toast.dart';
import 'release_notes.dart';
import 'update_messages.dart';
import 'korean_text.dart';
import '../l10n/l10n.dart';

/// Opens the update panel. [manual] ends with the graphical installer
/// instead of the in-place restart (the "설치 프로그램으로 직접 설치" path).
Future<void> showProductUpdates(
  BuildContext context, {
  required String server,
  required String product,
  required String currentVersion,
  bool manual = false,
  UpdatePlatform? platform,
  Future<void> Function(BuildContext)? onHostBatchUpdate,
  int hostUpdates = 0,
}) => showDialog<void>(
  context: context,
  barrierDismissible: false,
  builder: (_) => ProductUpdateDialog(
    server: server,
    product: product,
    currentVersion: currentVersion,
    manual: manual,
    platform: platform,
    onHostBatchUpdate: onHostBatchUpdate,
    hostUpdates: hostUpdates,
  ),
);

/// The panel's states, in order. Every step happens in this window; the
/// installer only shows its small splash once [applying] was accepted.
enum UpdatePhase {
  checking,
  upToDate,
  unavailable,
  available,
  downloading,
  verifying,
  ready,
  applying,
  installerOpened,
  failed,
}

enum _FailedStep { check, download, apply }

/// Check → download (real bytes) → verify (exact size, SHA-256 and the
/// installer EXE version) → one "다시 시작해서 업데이트" → the installer takes
/// over and reopens this app at the same place (update-handoff-v1).
class ProductUpdateDialog extends StatefulWidget {
  const ProductUpdateDialog({
    super.key,
    required this.server,
    required this.product,
    required this.currentVersion,
    this.api,
    this.platform,
    this.manual = false,
    this.onHostBatchUpdate,
    this.hostUpdates = 0,
  });
  final String server, product, currentVersion;
  final ProductUpdates? api;
  final UpdatePlatform? platform;
  final bool manual;
  final Future<void> Function(BuildContext)? onHostBatchUpdate;

  /// Assigned desktops whose Host can be updated now: the Host row becomes
  /// a highlighted "Host 업데이트 N대" entry instead of the quiet link.
  final int hostUpdates;
  @override
  State<ProductUpdateDialog> createState() => _ProductUpdateDialogState();
}

class _ProductUpdateDialogState extends State<ProductUpdateDialog> {
  ProductUpdates? _api;
  late final UpdatePlatform _platform =
      widget.platform ?? const NativeUpdatePlatform();
  ProductRelease? _release;
  DownloadedUpdate? _downloaded;
  UpdatePhase _phase = UpdatePhase.checking;
  _FailedStep _failedStep = _FailedStep.check;
  String _code = '';
  int _received = 0, _total = 0;
  // This download: its clock and bytes for the remaining time, and its
  // request number once an interrupted transfer continues.
  final _downloadClock = Stopwatch();
  int _attempt = 1;
  bool _resuming = false;
  // No bytes for a few seconds: the time left is not known, so the last
  // estimate is not left standing.
  Timer? _stallTick;
  int _quietSeconds = 0;
  bool get _stalled => _quietSeconds >= 3;
  // What the last install attempt was (in place, or the installer window):
  // 다시 시도 repeats that one.
  late bool _lastInstallManual = widget.manual;
  // Set by 취소/닫기: what the closing transport ends is not a failure.
  bool _closing = false;
  bool _acting = false, _disposed = false;
  int _installRevision = 0;
  bool _hostBatchOpen = false;
  // The running install goes to the graphical installer (manual panel, or
  // the fallback after a failed restart).
  bool _manualRun = false;
  // The open remote windows are being closed before the installer takes
  // over: the step is still "ready", so the dialog says what it waits for.
  bool _closingSessions = false;
  final _notesScroll = ScrollController();

  bool get _host => widget.product == 'host';
  bool get _newer =>
      _release != null &&
      compareProductVersions(_release!.version, widget.currentVersion) > 0;

  @override
  void initState() {
    super.initState();
    unawaited(_check());
  }

  void _set(VoidCallback change) {
    if (mounted) setState(change);
  }

  void _fail(_FailedStep step, String code) => _set(() {
    _phase = UpdatePhase.failed;
    _failedStep = step;
    _code = code;
  });

  /// The running and offered versions on every update record, so the log
  /// reads "1.2.3 → 1.2.4" instead of bare yes/no fields.
  Map<String, Object> _versions([Map<String, Object> values = const {}]) => {
    ...values,
    'current': ?logVersion(widget.currentVersion),
    'latest': ?logVersion(_release?.version ?? ''),
  };

  Future<void> _check() async {
    _set(() => _phase = UpdatePhase.checking);
    // One record per check: its result (the check itself is not logged apart).
    try {
      // Never offer repeated upgrades using a guessed or stale current version.
      compareProductVersions(widget.currentVersion, widget.currentVersion);
    } on UpdateFailure {
      // Fixed check diagnostics:1 running version unknown,2 invalid catalog,
      // 3 catalog unavailable. Keep exception text and server addresses out.
      ProductLog.instance.record(
        LogEvent.updateResult,
        _versions({'ok': false, 'stage': 1, 'error': 1}),
      );
      _fail(_FailedStep.check, 'version_unknown');
      return;
    }
    try {
      _api ??= widget.api ?? ProductUpdates(widget.server);
      final release = await _api!.check(widget.product, widget.currentVersion);
      final available =
          release != null &&
          compareProductVersions(release.version, widget.currentVersion) > 0;
      _release = release;
      ProductLog.instance.record(
        LogEvent.updateResult,
        _versions({'ok': true, 'available': available}),
      );
      _set(() {
        _phase = release == null
            ? UpdatePhase.unavailable
            : available
            ? UpdatePhase.available
            : UpdatePhase.upToDate;
      });
    } catch (error) {
      final invalidCatalog =
          error is UpdateFailure && error.code == 'invalid_catalog';
      ProductLog.instance.record(
        LogEvent.updateResult,
        _versions({'ok': false, 'stage': 1, 'error': invalidCatalog ? 2 : 3}),
      );
      _fail(
        _FailedStep.check,
        invalidCatalog ? 'invalid_catalog' : 'catalog_unavailable',
      );
    }
  }

  Future<void> _download() async {
    if (_acting || _hostBatchOpen || !_newer || _api == null) return;
    _set(() => _acting = true);
    await _downloaded?.discard();
    _downloaded = null;
    _set(() {
      _phase = UpdatePhase.downloading;
      _received = 0;
      _total = _release!.size;
      _attempt = 1;
      _resuming = false;
      _quietSeconds = 0;
    });
    _stallTick?.cancel();
    _stallTick = Timer.periodic(const Duration(seconds: 1), (_) {
      if (_phase != UpdatePhase.downloading) return;
      final was = _stalled;
      _quietSeconds++;
      if (_stalled != was) _set(() {});
    });
    final clock = _downloadClock
      ..reset()
      ..start();
    var last = -100;
    // Where it ended, for diagnostics: time, requests and bytes on disk.
    Map<String, Object> outcome(bool ok) => _versions({
      'ok': ok,
      'elapsedMs': clock.elapsedMilliseconds,
      'attempts': _attempt,
      'receivedKib': _received ~/ 1024,
    });
    try {
      final downloaded = await _api!.download(
        _release!,
        (received, total) {
          final stalled = _stalled;
          _quietSeconds = 0;
          if (received == total ||
              stalled ||
              clock.elapsedMilliseconds - last >= 50) {
            last = clock.elapsedMilliseconds;
            _set(() {
              _received = received;
              _total = total;
              _resuming = false;
            });
          }
        },
        verifying: () => _set(() => _phase = UpdatePhase.verifying),
        retrying: (attempt) => _set(() {
          _attempt = attempt;
          _resuming = true;
        }),
      );
      clock.stop();
      if (_disposed) {
        await downloaded.discard();
        return;
      }
      _downloaded = downloaded;
      _set(() => _phase = UpdatePhase.verifying);
      await _platform.verifyInstaller(downloaded);
      ProductLog.instance.record(LogEvent.updateDownload, outcome(true));
      _set(() => _phase = UpdatePhase.ready);
    } catch (error) {
      clock.stop();
      if (_disposed || _closing) return;
      final code = error is UpdateFailure ? error.code : 'download_failed';
      // Every failure carries its fixed code (network, server, disk or
      // verification) to diagnostics; a cancelled download is no failure.
      if (code != 'cancelled') {
        ProductLog.instance.recordFailure(
          LogEvent.updateDownload,
          code,
          outcome(false),
        );
      }
      await _downloaded?.discard();
      _downloaded = null;
      _fail(
        _FailedStep.download,
        code == 'cancelled' ||
                code == 'catalog_unavailable' ||
                code == 'invalid_catalog'
            ? 'download_failed'
            : updateFailureCodes.contains(code)
            ? code
            : 'download_failed',
      );
    } finally {
      _stallTick?.cancel();
      _stallTick = null;
      _set(() => _acting = false);
    }
  }

  Future<void> _install({required bool manual}) async {
    final downloaded = _downloaded;
    if (_closing ||
        _disposed ||
        _acting ||
        _hostBatchOpen ||
        downloaded == null) {
      return;
    }
    _lastInstallManual = manual;
    final revision = ++_installRevision;
    bool ownsInstall() =>
        mounted &&
        !_closing &&
        !_disposed &&
        revision == _installRevision &&
        identical(_downloaded, downloaded);
    _set(() => _acting = true);
    try {
      // The one confirmation, and only when remote windows would close.
      if (!_host) {
        final hasSessions = await _platform.hasRemoteSessions();
        // A false result is asynchronous too. Closing retires this operation
        // immediately, before the route's exit animation disposes its state.
        if (!mounted || !ownsInstall()) return;
        if (hasSessions) {
          final confirmed = await showAppConfirm(
            context,
            icon: Icons.link_off_rounded,
            title: l10n.disconnectUpdate,
            subtitle:
                '${updateProductLabel(widget.product)} ${_release!.version}',
            message: l10n.allOpenConnectionsClosedDesktops,
            confirmLabel: l10n.disconnectUpdate2,
            dialogKey: const Key('update-sessions-confirm'),
            confirmKey: const Key('confirm-update-sessions'),
          );
          if (!confirmed || !ownsInstall()) return;
          _set(() => _closingSessions = true);
          try {
            await _platform.closeRemoteSessions();
          } finally {
            // Before the next step or the failure is shown: never both.
            _set(() => _closingSessions = false);
          }
          if (!ownsInstall()) return;
        }
      }
      if (!ownsInstall()) return;
      _set(() {
        _manualRun = manual;
        _phase = UpdatePhase.applying;
      });
      // Written before the hand-off: an accepted in-place update closes this
      // app right away.
      ProductLog.instance.record(
        LogEvent.updateInstall,
        _versions({'ok': true, 'manual': manual}),
      );
      await _platform.install(
        downloaded,
        manual ? UpdateInstallMode.manual : UpdateInstallMode.apply,
        currentVersion: widget.currentVersion,
      );
      // apply: the installer accepted it and this app is closing now.
      if (manual && ownsInstall()) {
        _set(() => _phase = UpdatePhase.installerOpened);
      }
    } catch (error) {
      if (!ownsInstall()) return;
      if (error is UpdateFailure && error.code == 'signature_invalid') {
        ProductLog.instance.recordFailure(
          LogEvent.updateInstall,
          error.code,
          _versions({'ok': false, 'manual': manual}),
        );
      } else {
        ProductLog.instance.record(
          LogEvent.updateInstall,
          _versions({'ok': false, 'manual': manual}),
        );
      }
      _fail(
        _FailedStep.apply,
        error is UpdateFailure && updateFailureCodes.contains(error.code)
            ? error.code
            : unknownUpdateCode,
      );
    } finally {
      if (ownsInstall()) _set(() => _acting = false);
    }
  }

  void _retry() => unawaited(switch (_failedStep) {
    _FailedStep.check => _check(),
    _FailedStep.download => _download(),
    // What failed is tried again: the installer window stays the installer
    // window, never the in-place restart.
    _FailedStep.apply =>
      _downloaded == null ? _download() : _install(manual: _lastInstallManual),
  });

  void _retireRoute() {
    if (_closing) return;
    // Every successful pop retires pending work before the closing animation
    // disposes this state, including pops initiated by its parent Navigator.
    _closing = true;
    _installRevision++;
    _api?.close();
  }

  void _close() {
    if (_phase == UpdatePhase.applying || _hostBatchOpen) return;
    _retireRoute();
    Navigator.pop(context);
  }

  @override
  void dispose() {
    _disposed = true;
    _installRevision++;
    _stallTick?.cancel();
    _api?.close();
    unawaited(_downloaded?.discard());
    _notesScroll.dispose();
    super.dispose();
  }

  void _history() => unawaited(
    showReleaseHistory(
      context,
      server: widget.server,
      product: widget.product,
      currentVersion: widget.currentVersion,
      api: _api,
    ),
  );

  bool get _clientInstalling =>
      _acting ||
      switch (_phase) {
        UpdatePhase.downloading ||
        UpdatePhase.verifying ||
        UpdatePhase.applying => true,
        _ => false,
      };

  Future<void> _openHostBatch() async {
    final open = widget.onHostBatchUpdate;
    if (open == null || _host || _clientInstalling || _hostBatchOpen) return;
    setState(() => _hostBatchOpen = true);
    try {
      // This context belongs to the update route, below ThemedApp/Navigator.
      await open(context);
    } catch (_) {
      if (mounted) {
        showAppToast(
          context,
          l10n.couldNotOpenHostUpdate,
          kind: AppToastKind.failed,
        );
      }
    } finally {
      if (mounted) setState(() => _hostBatchOpen = false);
    }
  }

  // --- pieces ---------------------------------------------------------------

  static String _megabytes(int bytes) =>
      '${(bytes / 1048576).toStringAsFixed(1)} MB';
  static String _grouped(int value) => value.toString().replaceAllMapped(
    RegExp(r'\B(?=(\d{3})+(?!\d))'),
    (_) => ',',
  );

  Widget _label(AppPalette p, String text) => KeepAllText(
    text,
    style: uiText(size: 12, weight: FontWeight.w600, color: p.muted),
  );

  /// The version step: current → new, or the installed version when current.
  Widget _versionCard(AppPalette p, ProductRelease release) {
    final digits =
        uiText(
          size: 21,
          weight: FontWeight.w600,
          color: p.ink,
          height: 1.2,
        ).copyWith(
          fontFeatures: const [FontFeature.tabularFigures()],
          letterSpacing: -.3,
        );
    final meta = [
      if (release.releasedAt != null) l10n.released(releaseDate(release.releasedAt!)),
      _megabytes(release.size),
    ].join('  ·  ');
    return Container(
      key: const Key('update-version-card'),
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
            Row(
              children: [
                Column(
                  crossAxisAlignment: CrossAxisAlignment.start,
                  children: [
                    _label(p, l10n.current3),
                    const SizedBox(height: 3),
                    KeepAllText(
                      widget.currentVersion,
                      style: digits.copyWith(color: p.muted),
                    ),
                  ],
                ),
                Padding(
                  padding: const EdgeInsets.fromLTRB(16, 16, 16, 0),
                  child: Icon(
                    Icons.arrow_forward_rounded,
                    size: 20,
                    color: p.faintText,
                  ),
                ),
                Expanded(
                  child: Column(
                    crossAxisAlignment: CrossAxisAlignment.start,
                    children: [
                      _label(p, l10n.newVersion4),
                      const SizedBox(height: 3),
                      KeepAllText(
                        release.version,
                        key: const Key('update-new-version'),
                        style: digits.copyWith(color: p.amberInk),
                      ),
                    ],
                  ),
                ),
              ],
            )
          else
            Row(
              children: [
                Expanded(
                  child: Column(
                    crossAxisAlignment: CrossAxisAlignment.start,
                    children: [
                      _label(p, l10n.installedVersion),
                      const SizedBox(height: 3),
                      KeepAllText(widget.currentVersion, style: digits),
                    ],
                  ),
                ),
                VersionBadge(l10n.upDate, tone: AppTone.success),
              ],
            ),
          const SizedBox(height: 10),
          KeepAllText(meta, style: uiText(size: 12.5, color: p.muted)),
        ],
      ),
    );
  }

  Widget _notes(
    AppPalette p,
    ProductRelease release, {
    required bool compact,
  }) => Column(
    crossAxisAlignment: CrossAxisAlignment.stretch,
    mainAxisSize: MainAxisSize.min,
    children: [
      Row(
        children: [
          Expanded(child: _label(p, l10n.whatsNewVersion)),
          TextButton.icon(
            key: const Key('open-release-history'),
            onPressed: _api == null ? null : _history,
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
      const SizedBox(height: 4),
      ConstrainedBox(
        constraints: BoxConstraints(maxHeight: compact ? 150 : 230),
        child: DecoratedBox(
          decoration: BoxDecoration(
            color: p.tileWell,
            borderRadius: BorderRadius.circular(12),
            border: Border.all(color: p.tileLine),
          ),
          child: Scrollbar(
            controller: _notesScroll,
            thumbVisibility: true,
            child: SingleChildScrollView(
              key: const Key('update-notes-scroll'),
              controller: _notesScroll,
              padding: const EdgeInsets.fromLTRB(14, 12, 18, 12),
              child: SelectionArea(
                child: release.changelog.trim().isEmpty
                    ? KeepAllText(
                        l10n.noChangeLogRegisteredServer,
                        style: uiText(size: 13, color: p.muted),
                      )
                    : ChangelogText(release.changelog, keepWords: true),
              ),
            ),
          ),
        ),
      ),
    ],
  );

  /// 받기 · 확인 · 적용, each done, active or waiting.
  Widget _steps(AppPalette p) {
    // (steps completed, step in progress or failed)
    final (int done, int active) = switch (_phase) {
      UpdatePhase.downloading => (0, 0),
      UpdatePhase.verifying => (1, 1),
      UpdatePhase.ready || UpdatePhase.applying => (2, 2),
      UpdatePhase.installerOpened => (3, -1),
      UpdatePhase.failed => switch (_failedStep) {
        _FailedStep.apply => (2, 2),
        _
            when _code == 'hash_mismatch' ||
                _code == 'package_version_mismatch' ||
                _code == 'signature_invalid' =>
          (1, 1),
        _ => (0, 0),
      },
      _ => (0, -1),
    };
    final failed = _phase == UpdatePhase.failed;
    final labels = [l10n.download2, l10n.updateStepVerify, widget.manual ? l10n.installer : l10n.restart];
    Widget step(int index) {
      final complete = index < done;
      final current = index == active && !complete;
      final color = complete
          ? p.green
          : current
          ? (failed ? p.red : p.amberInk)
          : p.muted;
      return Row(
        mainAxisSize: MainAxisSize.min,
        children: [
          Container(
            width: 20,
            height: 20,
            alignment: Alignment.center,
            decoration: BoxDecoration(
              shape: BoxShape.circle,
              color: complete
                  ? p.okSoft
                  : current
                  ? (failed ? p.redSoft : p.amberSoft)
                  : p.quiet,
              border: Border.all(
                color: complete
                    ? p.okLine
                    : current
                    ? (failed ? p.redLine : p.amberLine)
                    : p.tileLine,
              ),
            ),
            child: complete
                ? Icon(Icons.check_rounded, size: 13, color: p.green)
                : current && failed
                ? Icon(Icons.close_rounded, size: 12, color: p.red)
                : KeepAllText(
                    '${index + 1}',
                    style: uiText(
                      size: 12,
                      weight: FontWeight.w600,
                      color: color,
                    ),
                  ),
          ),
          const SizedBox(width: 6),
          KeepAllText(
            labels[index],
            style: uiText(
              size: 12.5,
              weight: current || complete ? FontWeight.w600 : FontWeight.w500,
              color: complete || current ? p.ink : p.muted,
            ),
          ),
        ],
      );
    }

    Widget joint() => Expanded(
      child: Container(
        height: 1,
        margin: const EdgeInsets.symmetric(horizontal: 10),
        color: p.tileLine,
      ),
    );
    return Row(
      key: const Key('update-steps'),
      children: [step(0), joint(), step(1), joint(), step(2)],
    );
  }

  Widget _status(AppPalette p, String text, {Key? key, Color? color}) =>
      KeepAllText(
        text,
        key: key,
        style: uiText(size: 13, height: 1.55, color: color ?? p.muted),
      );

  Widget _progress(AppPalette p) {
    switch (_phase) {
      case UpdatePhase.downloading:
        final total = _total > 0 ? _total : _release!.size;
        final share = total > 0 ? (_received / total).clamp(0.0, 1.0) : null;
        return Column(
          crossAxisAlignment: CrossAxisAlignment.stretch,
          children: [
            LinearProgressIndicator(value: share, minHeight: 6),
            const SizedBox(height: 8),
            Row(
              children: [
                Expanded(
                  child: _status(
                    p,
                    '${(_received / 1048576).toStringAsFixed(1)} / ${(total / 1048576).toStringAsFixed(1)} MB',
                    key: const Key('update-download-bytes'),
                  ),
                ),
                if (share != null)
                  KeepAllText(
                    '${(share * 100).floor()}%',
                    style:
                        uiText(
                          size: 13,
                          weight: FontWeight.w600,
                          color: p.ink,
                        ).copyWith(
                          fontFeatures: const [FontFeature.tabularFigures()],
                        ),
                  ),
              ],
            ),
            _status(
              p,
              _resuming
                  ? l10n.networkDroppedSoDownloadResumes
                  : _stalled
                  ? l10n.calculatingTimeLeft
                  : remainingDownloadText(
                      _received,
                      total,
                      _downloadClock.elapsed,
                    ),
              key: const Key('update-download-remaining'),
            ),
          ],
        );
      case UpdatePhase.verifying:
        return Column(
          crossAxisAlignment: CrossAxisAlignment.stretch,
          children: [
            const LinearProgressIndicator(minHeight: 6),
            const SizedBox(height: 8),
            _status(p, l10n.checkingDownloadedFilesSizeSha2),
          ],
        );
      case UpdatePhase.applying:
        return Column(
          crossAxisAlignment: CrossAxisAlignment.stretch,
          children: [
            const LinearProgressIndicator(minHeight: 6),
            const SizedBox(height: 8),
            _status(
              p,
              _manualRun
                  ? l10n.whenInstallerOpensFollowIts
                  : _host
                  ? l10n.onceInstallerFinishesItsChecks
                  : l10n.onceInstallerFinishesItsChecks2,
              key: const Key('update-applying'),
            ),
          ],
        );
      default:
        return const SizedBox.shrink();
    }
  }

  /// What was checked, shown once the update is ready to apply.
  Widget _verified(AppPalette p, ProductRelease release) {
    final digest =
        '${release.sha256.substring(0, 8)}…${release.sha256.substring(56)}';
    Widget row(String text) => Padding(
      padding: const EdgeInsets.only(top: 6),
      child: Row(
        crossAxisAlignment: CrossAxisAlignment.start,
        children: [
          Padding(
            padding: const EdgeInsets.only(top: 1),
            child: Icon(Icons.check_circle_rounded, size: 15, color: p.green),
          ),
          const SizedBox(width: 8),
          Expanded(
            child: KeepAllText(
              text,
              style: uiText(size: 12.5, height: 1.45, color: p.ink),
            ),
          ),
        ],
      ),
    );
    return Container(
      key: const Key('update-verified'),
      padding: const EdgeInsets.fromLTRB(14, 6, 14, 12),
      decoration: BoxDecoration(
        color: p.okSoft,
        borderRadius: BorderRadius.circular(12),
        border: Border.all(color: p.okLine),
      ),
      child: Column(
        crossAxisAlignment: CrossAxisAlignment.stretch,
        children: [
          row(l10n.sizeMatchesBytes(_grouped(release.size))),
          row(l10n.sha256Matches(digest)),
          row(l10n.installerVersionConfirmed(release.version)),
          row(l10n.multitapReleaseSignatureConfirmed),
        ],
      ),
    );
  }

  Widget _failure(AppPalette p) {
    final needsInstaller = updateNeedsInstaller(_code);
    final tone = needsInstaller ? p.warn : p.red;
    return Container(
      key: const Key('update-failure'),
      padding: const EdgeInsets.fromLTRB(14, 12, 14, 12),
      decoration: BoxDecoration(
        color: needsInstaller ? p.warnSoft : p.redSoft,
        borderRadius: BorderRadius.circular(12),
        border: Border.all(
          color: needsInstaller ? p.warnLine : p.redLine,
        ),
      ),
      child: Column(
        crossAxisAlignment: CrossAxisAlignment.start,
        children: [
          KeepAllText(
            updateFailureMessage(
              _code,
              product: widget.product,
              version: _release?.version,
            ),
            style: uiText(size: 13.5, height: 1.55, color: p.ink),
          ),
          const SizedBox(height: 8),
          KeepAllText(
            l10n.code3(_code),
            key: const Key('update-failure-code'),
            style: uiText(size: 12, weight: FontWeight.w600, color: tone),
          ),
        ],
      ),
    );
  }

  /// Assigned Hosts with an installable update: one line and its button,
  /// which opens the Host 일괄 업데이트 (the header badge leads here).
  Widget _hostOffer(AppPalette p) => Container(
    key: const Key('host-update-offer'),
    padding: const EdgeInsets.fromLTRB(14, 10, 10, 10),
    decoration: BoxDecoration(
      color: p.amberSoft,
      borderRadius: BorderRadius.circular(12),
      border: Border.all(color: p.amberLine),
    ),
    child: Wrap(
      alignment: WrapAlignment.spaceBetween,
      crossAxisAlignment: WrapCrossAlignment.center,
      spacing: 12,
      runSpacing: 8,
      children: [
        Row(
          mainAxisSize: MainAxisSize.min,
          children: [
            Icon(Icons.system_update_alt_rounded, size: 18, color: p.amberInk),
            const SizedBox(width: 8),
            Flexible(
              child: KeepAllText(
                l10n.hostUpdatesAvailable(widget.hostUpdates),
                style: uiText(size: 13, weight: FontWeight.w600, color: p.ink),
              ),
            ),
          ],
        ),
        FilledButton(
          key: const Key('open-bulk-host-update'),
          onPressed: _clientInstalling || _hostBatchOpen
              ? null
              : () => unawaited(_openHostBatch()),
          style: AppButtons.filled(p),
          child: KeepAllText(l10n.hostUpdate, maxLines: 1),
        ),
      ],
    ),
  );

  String get _caution => widget.manual
      ? l10n.whenInstallerOpensInstallContinues
      : _host
      ? l10n.hostSettingsClosesConnectionsEnd
      : l10n.appClosesReopensSamePlace;

  @override
  Widget build(BuildContext context) {
    final p = AppPalette.of(context);
    final release = _release;
    final failed = _phase == UpdatePhase.failed;
    final needsInstaller = failed && updateNeedsInstaller(_code);
    final started = switch (_phase) {
      UpdatePhase.downloading ||
      UpdatePhase.verifying ||
      UpdatePhase.ready ||
      UpdatePhase.applying ||
      UpdatePhase.installerOpened => true,
      UpdatePhase.failed => _failedStep != _FailedStep.check,
      _ => false,
    };
    final (IconData icon, AppTone tone, String title) = switch (_phase) {
      UpdatePhase.checking => (
        Icons.system_update_alt_rounded,
        AppTone.accent,
        l10n.checkingUpdates,
      ),
      UpdatePhase.upToDate => (
        Icons.verified_rounded,
        AppTone.success,
        l10n.youreUpDate,
      ),
      UpdatePhase.unavailable => (
        Icons.cloud_off_rounded,
        AppTone.info,
        l10n.noInstallerPublished,
      ),
      UpdatePhase.available => (
        Icons.new_releases_rounded,
        AppTone.accent,
        l10n.newVersionAvailable3,
      ),
      UpdatePhase.downloading => (
        Icons.download_rounded,
        AppTone.accent,
        l10n.downloadingUpdate,
      ),
      UpdatePhase.verifying => (
        Icons.fact_check_rounded,
        AppTone.accent,
        l10n.checkingFile2,
      ),
      UpdatePhase.ready => (
        Icons.task_alt_rounded,
        AppTone.success,
        l10n.updateReady,
      ),
      UpdatePhase.applying => (
        Icons.restart_alt_rounded,
        AppTone.accent,
        _manualRun ? l10n.openingInstaller : l10n.applyingUpdate,
      ),
      UpdatePhase.installerOpened => (
        Icons.open_in_new_rounded,
        AppTone.success,
        l10n.installerOpened,
      ),
      UpdatePhase.failed => (
        needsInstaller ? Icons.front_hand_rounded : Icons.error_outline_rounded,
        needsInstaller ? AppTone.warn : AppTone.danger,
        updateFailureTitle(_code),
      ),
    };
    final busy = switch (_phase) {
      UpdatePhase.checking ||
      UpdatePhase.downloading ||
      UpdatePhase.verifying ||
      UpdatePhase.applying => true,
      _ => false,
    };
    final current = widget.currentVersion.isEmpty
        ? l10n.currentVersionUnknown
        : l10n.current5(widget.currentVersion);
    final body = <Widget>[
      if (_phase == UpdatePhase.checking) ...[
        const LinearProgressIndicator(minHeight: 6),
        const SizedBox(height: 10),
        _status(p, l10n.checkingReleaseInfoServer),
      ] else if (_phase == UpdatePhase.unavailable)
        _status(p, l10n.youCanUpdateOnceAdministrator)
      else if (release != null) ...[
        _versionCard(p, release),
        if (started) ...[
          const SizedBox(height: 16),
          _steps(p),
          const SizedBox(height: 12),
          _progress(p),
          if (_phase == UpdatePhase.ready) ...[
            _verified(p, release),
            const SizedBox(height: 12),
            _status(p, _caution, key: const Key('update-caution')),
          ],
          if (_phase == UpdatePhase.installerOpened)
            _status(p, l10n.followStepsInstallerWindowYou),
        ],
        if (failed) ...[const SizedBox(height: 12), _failure(p)],
        if (!started || _phase == UpdatePhase.ready) ...[
          const SizedBox(height: 16),
          _notes(p, release, compact: _phase == UpdatePhase.ready),
        ],
      ] else if (failed)
        _failure(p),
    ];
    final quiet = TextButton(
      key: const Key('close-product-update'),
      onPressed: _phase == UpdatePhase.applying ? null : _close,
      style: AppButtons.quiet(p),
      child: KeepAllText(switch (_phase) {
        UpdatePhase.downloading || UpdatePhase.verifying => l10n.cancel,
        UpdatePhase.available || UpdatePhase.ready => l10n.later,
        _ => l10n.close,
      }),
    );
    final closing = SizedBox.square(
      key: const Key('update-closing-sessions-mark'),
      dimension: 16,
      child: CircularProgressIndicator(strokeWidth: 2, color: p.amber),
    );
    // While bytes arrive only "취소" remains; while the installer takes over
    // nothing can be pressed twice, so no button is shown at all.
    final Widget? primary = switch (_phase) {
      UpdatePhase.available => FilledButton.icon(
        key: const Key('install-product-update'),
        autofocus: true,
        onPressed: _download,
        style: AppButtons.filled(p),
        icon: const Icon(Icons.download_rounded, size: 18),
        label: KeepAllText(l10n.downloadUpdate),
      ),
      // Pressed once: it stays where it is, greyed, while the remote
      // windows are looked for and closed (the mark turns while they close).
      UpdatePhase.ready => FilledButton.icon(
        key: const Key('apply-product-update'),
        autofocus: true,
        onPressed: _acting
            ? null
            : () => unawaited(_install(manual: widget.manual)),
        style: AppButtons.filled(p),
        icon: _closingSessions
            ? closing
            : Icon(
                widget.manual
                    ? Icons.open_in_new_rounded
                    : Icons.restart_alt_rounded,
                size: 18,
              ),
        label: KeepAllText(widget.manual ? l10n.openInstaller : l10n.restartUpdate),
      ),
      UpdatePhase.failed when needsInstaller && _downloaded != null =>
        FilledButton.icon(
          key: const Key('manual-product-update'),
          autofocus: true,
          onPressed: _acting ? null : () => unawaited(_install(manual: true)),
          style: AppButtons.filled(p, tone: AppTone.warn),
          icon: _closingSessions
              ? closing
              : const Icon(Icons.open_in_new_rounded, size: 18),
          label: KeepAllText(l10n.continueInstaller),
        ),
      UpdatePhase.failed => FilledButton.icon(
        key: const Key('retry-product-update'),
        autofocus: true,
        onPressed: _retry,
        style: AppButtons.filled(p),
        icon: const Icon(Icons.refresh_rounded, size: 18),
        label: KeepAllText(l10n.tryAgain),
      ),
      _ => null,
    };
    // After a failed restart the verified file can still go to the ordinary
    // installer window, with its own Install click and confirmations.
    // An unverified release signature refuses the installer either way.
    final manualLink =
        failed &&
        !needsInstaller &&
        !widget.manual &&
        _failedStep == _FailedStep.apply &&
        _code != 'signature_invalid' &&
        _downloaded != null;
    return PopScope(
      canPop: !busy,
      onPopInvokedWithResult: (didPop, _) {
        if (didPop) _retireRoute();
      },
      child: AppDialog(
        key: const Key('product-update-dialog'),
        icon: icon,
        tone: tone,
        busy: busy || _closingSessions,
        title: title,
        titleKey: const Key('update-title'),
        // Right above the buttons, in view whatever the body scrolled to.
        footer: _closingSessions
            ? AppBusyLine(
                l10n.updateClosingRemoteWindows,
                textKey: const Key('update-closing-sessions'),
                color: p.ink,
              )
            : null,
        subtitle: '${updateProductLabel(widget.product)} · $current',
        maxWidth: 520,
        body: [
          ...body,
          if (!_host && widget.onHostBatchUpdate != null) ...[
            const SizedBox(height: 16),
            const Divider(height: 1),
            const SizedBox(height: 8),
            if (widget.hostUpdates > 0)
              _hostOffer(p)
            else
              Align(
                alignment: Alignment.centerLeft,
                child: TextButton.icon(
                  key: const Key('open-bulk-host-update'),
                  onPressed: _clientInstalling || _hostBatchOpen
                      ? null
                      : () => unawaited(_openHostBatch()),
                  icon: const Icon(Icons.system_update_alt_rounded, size: 18),
                  label: KeepAllText(l10n.hostBulkUpdate),
                ),
              ),
          ],
          if (manualLink) ...[
            const SizedBox(height: 6),
            Align(
              alignment: Alignment.centerLeft,
              child: TextButton.icon(
                key: const Key('manual-product-update'),
                onPressed: _acting
                    ? null
                    : () => unawaited(_install(manual: true)),
                icon: const Icon(Icons.open_in_new_rounded, size: 16),
                label: KeepAllText(l10n.installInstaller),
              ),
            ),
          ],
        ],
        actions: _phase == UpdatePhase.applying ? const [] : [quiet, ?primary],
      ),
    );
  }
}
