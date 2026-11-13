import 'dart:async';

import 'package:flutter/material.dart';

import '../data/product_updates.dart';
import '../ui/app_dialog.dart';
import '../ui/app_progress.dart';
import '../ui/app_theme.dart';
import '../ui/korean_text.dart';
import '../ui/release_notes.dart';
import '../models/host_update.dart';
import 'android_tv.dart';
import '../l10n/l10n.dart';

/// The request response, not an assumption based on the confirmation snapshot.
String androidHostUpdateResult(HostUpdateState state) {
  final operation = state.operation;
  if (operation == null || !state.present) {
    return state.present && !state.connected
        ? l10n.lostConnectionHostCouldntConfirm
        : l10n.couldntConfirmResultUpdateRequest;
  }
  // A just-received response must remain readable even if its timestamp is old.
  return state.tileStatus(operation.checkedAt) ?? l10n.couldntConfirmResultUpdateRequest2;
}

bool androidHostUpdateSameObservation(
  HostUpdateOperation a,
  HostUpdateOperation b,
) =>
    a.status == b.status &&
    a.targetVersion == b.targetVersion &&
    a.code == b.code &&
    a.progress == b.progress &&
    a.rebootRequired == b.rebootRequired &&
    a.checkedAt == b.checkedAt;

/// No operation ID exists: equal/missing timestamps do not order different results.
({bool apply, String message}) androidHostUpdateReply(
  HostUpdateState observed,
  HostUpdateState reply,
) {
  final before = observed.operation, after = reply.operation;
  if (!reply.present || after == null) {
    return (apply: false, message: l10n.couldntConfirmResultUpdateRequest);
  }
  if (before == null) {
    return (apply: true, message: androidHostUpdateResult(reply));
  }
  if (androidHostUpdateSameObservation(before, after)) {
    return (apply: false, message: androidHostUpdateResult(observed));
  }
  final oldTime = before.checkedAt, newTime = after.checkedAt;
  if (oldTime != null && newTime != null) {
    if (newTime.isAfter(oldTime)) {
      return (apply: true, message: androidHostUpdateResult(reply));
    }
    if (newTime.isBefore(oldTime)) {
      return (
        apply: false,
        message: l10n.currentStatus(androidHostUpdateResult(observed)),
      );
    }
  }
  return (
    apply: false,
    message: l10n.lastCheckedStatus(androidHostUpdateResult(observed)),
  );
}

/// Reads metadata only. The broker/Host still authorize and verify installation.
class AndroidHostUpdateDialog extends StatefulWidget {
  const AndroidHostUpdateDialog({
    super.key,
    required this.origin,
    required this.machine,
    required this.currentVersion,
    required this.targetVersion,
    required this.sessionActive,
    required this.isCurrent,
    this.loadRelease,
    this.request,
    this.pc = false,
    this.confirmKey = const Key('android-host-update-confirm'),
  });

  /// Sends the update request for the confirmed release. With it the dialog
  /// stays open while the request is sent, says so, and closes (with the
  /// release) when it was answered; it must not throw. Null: the dialog
  /// closes with the release at once.
  final Future<void> Function(ProductRelease release)? request;
  final Key confirmKey;
  final Uri origin;
  final String machine, currentVersion, targetVersion;
  final bool sessionActive;

  /// A local PC's Host: the texts name it PC, not VM.
  final bool pc;
  final bool Function() isCurrent;
  final Future<ProductRelease?> Function()? loadRelease;
  @override
  State<AndroidHostUpdateDialog> createState() =>
      _AndroidHostUpdateDialogState();
}

class _AndroidHostUpdateDialogState extends State<AndroidHostUpdateDialog> {
  ProductUpdates? _reader;
  ProductRelease? _release;
  String? _failure;
  bool _loading = true;

  /// The request is on its way ([AndroidHostUpdateDialog.request]).
  bool _requesting = false;

  /// The person left the dialog; an answered request closes nothing then.
  bool _left = false;
  int _request = 0;
  String get _kind =>
      widget.pc ? l10n.localPcInSentence : l10n.desktopInSentence;
  String get _changed => l10n.serverSignStatusChangedCancel(_kind);
  @override
  void initState() {
    super.initState();
    unawaited(_load());
  }

  @override
  void dispose() {
    ++_request;
    _reader?.close();
    super.dispose();
  }

  Future<void> _load() async {
    final request = ++_request;
    setState(() {
      _loading = true;
      _release = null;
      _failure = null;
    });
    ProductUpdates? reader;
    try {
      if (!widget.isCurrent()) throw const UpdateFailure('context_changed');
      final ProductRelease? release;
      if (widget.loadRelease != null) {
        release = await widget.loadRelease!();
      } else {
        final active = ProductUpdates(widget.origin.origin);
        _reader = reader = active;
        release = await active.check('host', '0.0.0');
      }
      if (!mounted || request != _request) return;
      if (!widget.isCurrent()) throw const UpdateFailure('context_changed');
      if (release == null) throw const UpdateFailure('release_missing');
      if (release.product != 'host' ||
          release.version != widget.targetVersion) {
        throw const UpdateFailure('version_changed');
      }
      setState(() => _release = release);
    } catch (error) {
      if (mounted && request == _request) {
        setState(() {
          _failure = switch (error is UpdateFailure ? error.code : '') {
            'context_changed' => _changed,
            'version_changed' =>
              l10n.targetListDiffersFromLatest,
            'release_missing' => l10n.couldntFindLatestHostRelease,
            _ => l10n.couldntCheckHostReleaseInformation,
          };
        });
      }
    } finally {
      reader?.close();
      if (identical(_reader, reader)) _reader = null;
      if (mounted && request == _request) setState(() => _loading = false);
    }
  }

  void _confirm() {
    final release = _release;
    if (release == null || _requesting) return;
    if (!widget.isCurrent()) {
      setState(() {
        _release = null;
        _failure = _changed;
      });
      return;
    }
    final request = widget.request;
    if (request == null) {
      Navigator.pop(context, release);
      return;
    }
    setState(() => _requesting = true);
    unawaited(
      request(release).whenComplete(() {
        if (!mounted || _left) return;
        final route = ModalRoute.of(context);
        if (route == null || !route.isActive) return;
        // Its own route only, also when something else is above it.
        if (route.isCurrent) {
          Navigator.pop(context, release);
        } else {
          Navigator.removeRoute(context, route);
        }
      }),
    );
  }

  @override
  Widget build(BuildContext context) {
    final p = AppPalette.of(context);
    return PopScope(
      onPopInvokedWithResult: (didPop, _) {
        if (didPop) _left = true;
      },
      child: _dialog(context, p),
    );
  }

  Widget _dialog(BuildContext context, AppPalette p) {
    return AppDialog(
      key: const Key('android-host-update-dialog'),
      icon: Icons.system_update_alt_rounded,
      busy: _loading || _requesting,
      title: l10n.hostUpdate,
      subtitle: widget.machine,
      body: [
        AppDialog.message(
          context,
          l10n.currentNewVersion(widget.currentVersion, widget.targetVersion),
        ),
        const SizedBox(height: 12),
        AppDialog.message(
          context,
          widget.sessionActive
              ? l10n.updateStartsWhenConnectionUse
              : l10n.updatesOnlyHostProgramYou(_kind),
        ),
        const SizedBox(height: 12),
        if (_requesting)
          AppBusyLine(
            l10n.androidHostUpdateRequesting,
            textKey: const Key('android-host-update-requesting'),
            trailing: const AppElapsedText(after: Duration(seconds: 3)),
          )
        else if (_loading)
          AppBusyLine(
            l10n.androidHostUpdateReading,
            textKey: const Key('android-host-update-loading'),
          )
        else if (_failure != null)
          KeepAllText(
            _failure!,
            style: uiText(size: 13.5, height: 1.6, color: p.red),
          )
        else if (_release != null) ...[
          // As the client's update panel: a small caption over a well.
          KeepAllText(
            l10n.changeLog,
            style: uiText(size: 12, weight: FontWeight.w600, color: p.muted),
          ),
          const SizedBox(height: 6),
          Container(
            padding: const EdgeInsets.fromLTRB(14, 12, 14, 12),
            decoration: BoxDecoration(
              color: p.tileWell,
              borderRadius: BorderRadius.circular(12),
              border: Border.all(color: p.tileLine),
            ),
            child: _release!.changelog.isEmpty
                ? KeepAllText(
                    l10n.noChangeLogRegisteredServer,
                    style: uiText(size: 13, height: 1.5, color: p.ink),
                  )
                : DefaultTextStyle.merge(
                    style: uiText(size: 13, height: 1.5, color: p.ink),
                    child: ChangelogText(_release!.changelog, keepWords: true),
                  ),
          ),
        ],
      ],
      // The product's three button looks (AppButtons), as every dialog.
      actions: [
        TextButton(
          key: const Key('android-host-update-cancel'),
          onPressed: () => Navigator.pop(context),
          style: AndroidTvFocus.style(context, AppButtons.quiet(p)),
          // A request already sent is not taken back by leaving: 닫기.
          child: Text(_requesting ? l10n.close : l10n.cancel),
        ),
        if (!_loading && _failure != null)
          OutlinedButton(
            key: const Key('android-host-update-retry'),
            onPressed: _load,
            style: AndroidTvFocus.style(context, AppButtons.outlined(p)),
            child: Text(l10n.checkAgain),
          ),
        FilledButton(
          key: widget.confirmKey,
          style: AndroidTvFocus.style(context, AppButtons.filled(p)),
          onPressed:
              !_loading &&
                  !_requesting &&
                  _release != null &&
                  widget.isCurrent()
              ? _confirm
              : null,
          child: Text(l10n.update),
        ),
      ],
    );
  }
}
