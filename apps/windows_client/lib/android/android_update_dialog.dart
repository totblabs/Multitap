import 'dart:async';
import 'dart:io';

import 'package:flutter/material.dart';

import '../data/product_updates.dart' show UpdateFailure;
import '../ui/app_dialog.dart';
import '../ui/app_progress.dart';
import '../ui/app_theme.dart';
import '../ui/korean_text.dart';
import 'android_updates.dart';
import '../l10n/l10n.dart';

enum _Stage {
  ready,
  preparing,
  permission,
  downloading,
  verifying,

  /// The native hand-off: the package is checked, copied into Android's
  /// install session and committed.
  installing,

  /// Handed to Android: its confirmation screen opens and waits.
  handedOff,

  /// Back from Android's confirmation with no failure reported: Android is
  /// installing, and ends this app when it is done.
  finishing,
  installed,
  failed,
}

String _megabytes(int bytes) => (bytes / (1024 * 1024)).toStringAsFixed(1);

/// The version and changelog of a newer APK, then (only on the user's
/// confirmation) download, size/SHA-256 check and the hand-off to Android's
/// installer, which asks once more. Owns and closes [updates].
///
/// Every step shows that it is running and what it is. A bar is filled only
/// from measured bytes (the download, and the copy into Android's install
/// session where the platform reports it); the other steps run it back and
/// forth. After the hand-off the dialog follows the installer's own reports:
/// its confirmation opened, the person is back from it, or it failed (the
/// reason then replaces the instruction).
class AndroidUpdateDialog extends StatefulWidget {
  const AndroidUpdateDialog({
    super.key,
    required this.release,
    required this.current,
    required this.updates,
    required this.platform,
  });
  final AndroidRelease release;
  final String current;
  final AndroidUpdates updates;
  final AndroidUpdatePlatform platform;
  @override
  State<AndroidUpdateDialog> createState() => _AndroidUpdateDialogState();
}

class _AndroidUpdateDialogState extends State<AndroidUpdateDialog>
    with WidgetsBindingObserver {
  _Stage _stage = _Stage.ready;
  int _received = 0;

  /// Bytes copied into Android's install session of [_copyTotal], as the
  /// platform reported them; 0 before the copy starts.
  int _copied = 0, _copyTotal = 0;

  /// Android's confirmation screen was opened; then, that this app left the
  /// front for it (coming back means the screen was closed).
  bool _confirmOpen = false, _awayForConfirm = false;

  /// An installer result that arrived before the hand-off call returned.
  String? _earlyResult;
  String? _failure, _note;
  File? _file;
  int _request = 0;
  bool _closing = false;
  final _clock = Stopwatch();
  StreamSubscription<String>? _statusFeed;
  StreamSubscription<({int copied, int total})>? _copyFeed;
  bool _current(int request) => mounted && !_closing && request == _request;

  void _cancelRequest() {
    if (_closing) return;
    _closing = true;
    ++_request;
    widget.updates.close();
    unawaited(_discard());
  }

  void _close() {
    if (_stage == _Stage.installing) return;
    _cancelRequest();
    Navigator.pop(context);
  }

  bool get _busy =>
      _stage == _Stage.preparing ||
      _stage == _Stage.downloading ||
      _stage == _Stage.verifying ||
      _stage == _Stage.installing ||
      _stage == _Stage.handedOff ||
      _stage == _Stage.finishing;

  @override
  void initState() {
    super.initState();
    WidgetsBinding.instance.addObserver(this);
    final platform = widget.platform;
    _statusFeed = platform.statuses.listen(_installerStatus);
    if (platform is AndroidUpdateInstallProgress) {
      _copyFeed = (platform as AndroidUpdateInstallProgress).installProgress
          .listen((progress) {
            if (_closing || !mounted || _stage != _Stage.installing) return;
            setState(() {
              _copied = progress.copied;
              _copyTotal = progress.total;
            });
          });
    }
  }

  /// The installer's reports after (or just before) the hand-off returned.
  void _installerStatus(String code) {
    if (_closing || !mounted) return;
    final handed = _stage == _Stage.handedOff || _stage == _Stage.finishing;
    if (!handed && _stage != _Stage.installing) return;
    if (code == androidUpdateConfirmOpened) {
      final lifecycle = WidgetsBinding.instance.lifecycleState;
      setState(() {
        _confirmOpen = true;
        _awayForConfirm =
            lifecycle != null && lifecycle != AppLifecycleState.resumed;
      });
      return;
    }
    if (!handed) {
      _earlyResult = code;
      return;
    }
    _installerResult(code);
  }

  /// A result: the reason replaces the instruction and the mark that turns.
  void _installerResult(String code) => setState(() {
    if (code == 'installed') {
      _stage = _Stage.installed;
    } else {
      _stage = _Stage.failed;
      _failure = code;
    }
  });

  @override
  void didChangeAppLifecycleState(AppLifecycleState state) {
    // Back from Android settings: say whether the permission is now on.
    if (state == AppLifecycleState.resumed && _stage == _Stage.permission) {
      unawaited(_recheck());
    }
    // Android's confirmation covers the app while it is open. Back in front
    // with no failure reported, Android is installing; a cancel reported a
    // moment later still replaces that line.
    if (_stage == _Stage.handedOff && _confirmOpen) {
      if (state != AppLifecycleState.resumed) {
        _awayForConfirm = true;
      } else if (_awayForConfirm && !_closing) {
        setState(() => _stage = _Stage.finishing);
      }
    }
  }

  Future<void> _recheck() async {
    if (_closing) return;
    final request = _request;
    bool allowed;
    try {
      allowed = await widget.platform.canInstall();
    } catch (_) {
      allowed = false;
    }
    if (!_current(request) || _stage != _Stage.permission || !allowed) return;
    setState(() {
      _stage = _Stage.ready;
      _note = l10n.installPermissionConfirmedTapDownload;
    });
  }

  Future<void> _install() async {
    if (_closing || (_stage != _Stage.ready && _stage != _Stage.failed)) return;
    final request = ++_request;
    _clock
      ..reset()
      ..start();
    setState(() {
      _stage = _Stage.preparing;
      _failure = _note = null;
      _received = 0;
      _copied = _copyTotal = 0;
      _confirmOpen = _awayForConfirm = false;
      _earlyResult = null;
    });
    try {
      final allowed = await widget.platform.canInstall();
      if (!_current(request)) return;
      if (!allowed) {
        if (mounted) setState(() => _stage = _Stage.permission);
        return;
      }
      final directory = await widget.platform.directory();
      if (!_current(request)) return;
      setState(() => _stage = _Stage.downloading);
      final file = _file = await widget.updates.download(
        widget.release,
        directory,
        (received, _) {
          if (_current(request)) setState(() => _received = received);
        },
        verifying: () {
          if (_current(request)) setState(() => _stage = _Stage.verifying);
        },
      );
      if (!_current(request)) {
        _file = null;
        await widget.platform.discard(file);
        return;
      }
      setState(() => _stage = _Stage.installing);
      // The native side deletes the file after the hand-off or a failure.
      await widget.platform.install(file, widget.release);
      _file = null;
      if (_current(request)) {
        setState(() => _stage = _Stage.handedOff);
        final early = _earlyResult;
        if (early != null) _installerResult(early);
      }
    } on UpdateFailure catch (error) {
      await _discard();
      if (!_current(request)) return;
      setState(() {
        if (error.code == 'install_permission_required') {
          _stage = _Stage.permission;
        } else {
          _stage = _Stage.failed;
          _failure = error.code;
        }
      });
    } catch (_) {
      final preparing = _stage == _Stage.preparing;
      await _discard();
      if (_current(request)) {
        setState(() {
          _stage = _Stage.failed;
          _failure = preparing ? null : 'download_failed';
          if (preparing) _note = l10n.couldntCheckInstallPermissionDownload;
        });
      }
    }
  }

  Future<void> _discard() async {
    final file = _file;
    _file = null;
    if (file == null) return;
    try {
      await widget.platform.discard(file);
    } catch (_) {
      /* The download directory is emptied before the next download. */
    }
  }

  Future<void> _settings() async {
    if (_closing) return;
    final request = _request;
    try {
      await widget.platform.openInstallSettings();
    } catch (_) {
      if (_current(request)) {
        setState(
          () => _note = l10n.couldntOpenAndroidSettingsAllow,
        );
      }
    }
  }

  @override
  void dispose() {
    unawaited(_statusFeed?.cancel());
    unawaited(_copyFeed?.cancel());
    _clock.stop();
    WidgetsBinding.instance.removeObserver(this);
    // Closing cancels a running download; its partial file is removed.
    _cancelRequest();
    super.dispose();
  }

  @override
  Widget build(BuildContext context) {
    final p = AppPalette.of(context);
    final release = widget.release;
    final total = release.size;
    Widget text(String value, {Key? key, Color? color}) => KeepAllText(
      value,
      key: key,
      style: uiText(size: 13.5, height: 1.6, color: color ?? p.muted),
    );
    // One step of the update: the bar ([value] only when measured), the
    // sentence, and the time since 다운로드 및 설치 was pressed.
    Widget step(String sentence, Key key, {double? value, String? detail}) =>
        AppProgressBlock(
          step: sentence,
          stepKey: key,
          value: value,
          detail: detail,
          elapsed: () => _clock.elapsed,
        );
    final reportsCopy = widget.platform is AndroidUpdateInstallProgress;
    final notes = release.changelog.trim();
    final body = <Widget>[
      if (_stage == _Stage.ready || _stage == _Stage.failed) ...[
        Container(
          key: const Key('android-update-changelog'),
          constraints: const BoxConstraints(maxHeight: 220),
          padding: const EdgeInsets.all(12),
          decoration: BoxDecoration(
            color: p.tileWell,
            borderRadius: BorderRadius.circular(12),
            border: Border.all(color: p.tileLine),
          ),
          child: SingleChildScrollView(
            child: text(
              notes.isEmpty ? l10n.noChangeLogRegisteredServer : notes,
              color: p.ink,
            ),
          ),
        ),
        const SizedBox(height: 12),
        text(
          l10n.appDataSignSettingsKept,
        ),
      ],
      if (_stage == _Stage.permission)
        text(
          l10n.androidSettingsAllowInstallUnknown,
          key: const Key('android-update-permission'),
          color: p.ink,
        ),
      if (_stage == _Stage.preparing)
        step(
          l10n.checkingInstallPermissionDownloadLocation,
          const Key('android-update-preparing'),
        ),
      if (_stage == _Stage.downloading)
        step(
          l10n.downloadingMb(_megabytes(_received), _megabytes(total), total > 0 ? (_received * 100 ~/ total) : 0),
          const Key('android-update-progress'),
          // Measured: the bytes received of the catalog's size.
          value: total > 0 ? (_received / total).clamp(0.0, 1.0) : null,
        ),
      if (_stage == _Stage.verifying)
        step(
          l10n.checkingDownloadedFilesSizeSha,
          const Key('android-update-verifying'),
        ),
      if (_stage == _Stage.installing)
        // The hand-off checks the package, copies it into Android's install
        // session and commits. Only the copy has a measure.
        if (!reportsCopy)
          step(
            l10n.androidUpdatePreparingInstall,
            const Key('android-update-installing'),
          )
        else if (_copied <= 0)
          step(
            l10n.androidUpdateCheckingPackage,
            const Key('android-update-installing'),
          )
        else if (_copied < _copyTotal)
          step(
            l10n.androidUpdatePreparingInstall,
            const Key('android-update-installing'),
            value: (_copied / _copyTotal).clamp(0.0, 1.0),
            detail: '${_megabytes(_copied)} / ${_megabytes(_copyTotal)} MB',
          )
        else
          step(
            l10n.openingAndroidInstaller,
            const Key('android-update-installing'),
          ),
      if (_stage == _Stage.handedOff)
        if (!_confirmOpen)
          step(
            l10n.openingAndroidInstaller,
            const Key('android-update-opening-installer'),
          )
        else ...[
          AppBusyLine(
            l10n.androidUpdateWaitingConfirm,
            textKey: const Key('android-update-waiting-confirm'),
          ),
          const SizedBox(height: 10),
          text(
            l10n.tapUpdateAndroidInstallerOpen,
            key: const Key('android-update-handed-off'),
            color: p.ink,
          ),
        ],
      if (_stage == _Stage.finishing)
        step(
          l10n.androidUpdateInstallingNow,
          const Key('android-update-finishing'),
        ),
      if (_stage == _Stage.installed)
        text(
          l10n.updateInstalled,
          key: const Key('android-update-installed'),
          color: p.ink,
        ),
      if (_failure != null) ...[
        const SizedBox(height: 12),
        text(
          androidUpdateMessage(_failure!),
          key: const Key('android-update-failure'),
          color: p.red,
        ),
      ],
      if (_note != null) ...[
        const SizedBox(height: 12),
        text(_note!, key: const Key('android-update-note'), color: p.amberInk),
      ],
    ];
    final actions = switch (_stage) {
      _Stage.ready || _Stage.failed => [
        TextButton(
          key: const Key('android-update-later'),
          onPressed: _close,
          style: AppButtons.quiet(p),
          child: Text(l10n.later),
        ),
        FilledButton(
          key: const Key('android-update-install'),
          onPressed: _install,
          style: AppButtons.filled(p),
          child: KeepAllText(
            _stage == _Stage.failed ? l10n.tryAgain : l10n.downloadInstall,
            textAlign: TextAlign.center,
          ),
        ),
      ],
      _Stage.permission => [
        TextButton(
          onPressed: _close,
          style: AppButtons.quiet(p),
          child: Text(l10n.cancel),
        ),
        FilledButton(
          key: const Key('android-update-settings'),
          onPressed: _settings,
          style: AppButtons.filled(p),
          child: Text(l10n.openSettings),
        ),
      ],
      _Stage.preparing ||
      _Stage.downloading ||
      _Stage.verifying ||
      _Stage.installing => [
        TextButton(
          key: const Key('android-update-cancel'),
          // Preparation/download may be cancelled; the native hand-off (the
          // copy into Android's install session) runs to its end.
          onPressed: _stage == _Stage.installing ? null : _close,
          style: AppButtons.quiet(p),
          child: Text(l10n.cancel),
        ),
      ],
      _Stage.handedOff || _Stage.finishing || _Stage.installed => [
        FilledButton(
          key: const Key('android-update-close'),
          onPressed: _close,
          style: AppButtons.filled(p),
          child: Text(l10n.close),
        ),
      ],
    };
    return PopScope(
      canPop: _stage != _Stage.installing,
      onPopInvokedWithResult: (didPop, _) {
        if (didPop) _cancelRequest();
      },
      child: AppDialog(
        key: const Key('android-update-dialog'),
        icon: Icons.system_update_alt_rounded,
        busy: _busy,
        title: l10n.newVersion3(release.version),
        subtitle: l10n.currentMb(widget.current, _megabytes(total)),
        body: body,
        actions: actions,
      ),
    );
  }
}
