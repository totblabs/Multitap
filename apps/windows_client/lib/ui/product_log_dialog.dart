import 'dart:async';

import 'package:flutter/material.dart';
import 'package:flutter/services.dart';

import '../data/product_log.dart';
import 'app_dialog.dart';
import 'app_theme.dart';
import '../l10n/l10n.dart';

Future<void> showProductLog(
  BuildContext context, {
  Future<List<ProductLogEntry>> Function()? loadHost,
}) => showDialog<void>(
  context: context,
  builder: (_) => _LogDialog(loadHost: loadHost),
);

class _LogDialog extends StatefulWidget {
  const _LogDialog({this.loadHost});
  final Future<List<ProductLogEntry>> Function()? loadHost;
  @override
  State<_LogDialog> createState() => _LogDialogState();
}

class _LogDialogState extends State<_LogDialog> {
  bool _loading = false, _failed = false, _errors = false;
  bool? _copied;
  Timer? _copyReset;
  List<ProductLogEntry> _host = [];
  @override
  void initState() {
    super.initState();
    _refresh();
  }

  @override
  void dispose() {
    _copyReset?.cancel();
    super.dispose();
  }

  Future<void> _refresh() async {
    if (_loading || widget.loadHost == null) return;
    setState(() {
      _loading = true;
      _failed = false;
    });
    try {
      final entries = await widget.loadHost!();
      if (mounted) setState(() => _host = entries);
    } catch (_) {
      if (mounted) setState(() => _failed = true);
    } finally {
      if (mounted) setState(() => _loading = false);
    }
  }

  // Older records hold the check apart from its result; the result row alone
  // tells the story.
  List<ProductLogEntry> get _all =>
      [
          ...ProductLog.instance.entries,
          ..._host,
        ].where((entry) => entry.event != LogEvent.updateCheck).toList()
        ..sort((a, b) => b.time.compareTo(a.time));
  static bool _failure(ProductLogEntry entry) => entry.values['ok'] == false;
  static bool _warning(ProductLogEntry entry) =>
      !_failure(entry) &&
      (entry.values['reboot'] == true || entry.values['recovery'] == true);
  static bool _attention(ProductLogEntry entry) =>
      _failure(entry) || _warning(entry);
  String _details(ProductLogEntry entry) => [
    if (entry.code != null)
      entry.values['ok'] == true
          ? l10n.logCode('${entry.code}')
          : l10n.logErrorCode('${entry.code}'),
    if (!_versioned(entry.event)) ?_values(entry.values),
  ].join('\n');
  // The copied text keeps the full timestamp; the list shows a short one.
  String _line(ProductLogEntry entry) {
    final details = _details(entry);
    return '${entry.time.toLocal().toIso8601String().replaceFirst('T', ' ')} · ${_title(entry)}'
        '${details.isEmpty ? '' : '\n$details'}';
  }

  static String _clock(DateTime time) {
    final local = time.toLocal();
    String two(int value) => value.toString().padLeft(2, '0');
    return '${two(local.month)}-${two(local.day)} '
        '${two(local.hour)}:${two(local.minute)}:${two(local.second)}';
  }

  Future<void> _copy() async {
    var copied = true;
    try {
      await Clipboard.setData(
        ClipboardData(
          text: [
            l10n.multitapLog(widget.loadHost == null ? 'Client' : 'Host'),
            l10n.recentEntriesPasswordsTokensClipboard,
            ..._all.map(_line),
          ].join('\n\n'),
        ),
      );
    } catch (_) {
      copied = false;
    }
    if (!mounted) return;
    // Feedback on the button itself; a snack bar would sit under the dialog.
    setState(() => _copied = copied);
    _copyReset?.cancel();
    _copyReset = Timer(const Duration(seconds: 2), () {
      if (mounted) setState(() => _copied = null);
    });
  }

  // Only exceptions are marked: a failure in red, a warning (restart or
  // recovery needed) in amber; ordinary records carry no dot.
  Widget _row(
    AppPalette p,
    ProductLogEntry entry,
    int index, {
    bool gutter = true,
  }) {
    final failure = _failure(entry), warning = _warning(entry);
    final details = _details(entry);
    return Container(
      padding: const EdgeInsets.symmetric(horizontal: 14, vertical: 10),
      decoration: BoxDecoration(
        border: index == 0 ? null : Border(top: BorderSide(color: p.tileLine)),
      ),
      child: Row(
        crossAxisAlignment: CrossAxisAlignment.start,
        children: [
          // The mark column only while some record has a mark: otherwise the
          // times line up with the controls above.
          if (gutter) ...[
            SizedBox(
              width: 18,
              height: 20,
              child: Center(
                child: failure
                    ? Icon(
                        Icons.error_outline_rounded,
                        key: Key('log-failure-$index'),
                        size: 16,
                        color: p.red,
                      )
                    : warning
                    ? Icon(
                        Icons.warning_amber_rounded,
                        key: Key('log-warning-$index'),
                        size: 16,
                        color: p.warn,
                      )
                    : null,
              ),
            ),
            const SizedBox(width: 10),
          ],
          SizedBox(
            width: 112,
            child: Text(
              _clock(entry.time),
              style: uiText(
                size: 12.5,
                height: 1.6,
                color: p.muted,
              ).copyWith(fontFeatures: const [FontFeature.tabularFigures()]),
            ),
          ),
          Expanded(
            child: Text.rich(
              TextSpan(
                children: [
                  TextSpan(
                    text: _title(entry),
                    style: uiText(
                      size: 13,
                      weight: FontWeight.w600,
                      color: failure ? p.red : p.ink,
                    ),
                  ),
                  if (details.isNotEmpty)
                    TextSpan(
                      text: '\n$details',
                      style: uiText(size: 12.5, color: p.muted),
                    ),
                ],
              ),
              style: const TextStyle(height: 1.6),
            ),
          ),
        ],
      ),
    );
  }

  @override
  Widget build(BuildContext context) => AnimatedBuilder(
    animation: ProductLog.instance,
    builder: (context, _) {
      final p = AppPalette.of(context);
      final all = _all;
      final attention = all.where(_attention).length;
      final entries = all
          .where((entry) => !_errors || _attention(entry))
          .take(256)
          .toList();
      final marked = entries.any((e) => _failure(e) || _warning(e));
      final warning = _failed
          ? l10n.couldNotReadHostLog
          : !ProductLog.instance.storageAvailable
          ? l10n.logFileCannotWrittenSo
          : null;
      return AppDialog(
        icon: Icons.receipt_long_outlined,
        title: l10n.viewLogs,
        subtitle:
            l10n.multitap(widget.loadHost == null ? 'Client' : 'Host', entries.length),
        onClose: () => Navigator.pop(context),
        closeKey: const Key('close-product-log'),
        maxWidth: 760,
        scrollBody: false,
        body: [
          Wrap(
            spacing: 8,
            runSpacing: 8,
            crossAxisAlignment: WrapCrossAlignment.center,
            children: [
              // A filter chip: filled with a check while only the records that
              // need attention are listed; the count shows before choosing.
              // With nothing to check there is nothing to filter: no chip
              // (it stays while it is on, so it can be turned off).
              if (attention > 0 || _errors)
                FilterChip(
                  key: const Key('log-attention-filter'),
                  label: Text(l10n.needsAttention2(attention)),
                  selected: _errors,
                  onSelected: (value) => setState(() => _errors = value),
                ),
              if (widget.loadHost != null)
                OutlinedButton.icon(
                  onPressed: _loading ? null : _refresh,
                  style: AppButtons.outlined(p).copyWith(
                    minimumSize: const WidgetStatePropertyAll(Size(0, 36)),
                  ),
                  icon: const Icon(Icons.refresh_rounded, size: 17),
                  label: Text(_loading ? l10n.loading2 : l10n.refresh),
                ),
              OutlinedButton.icon(
                key: const Key('copy-product-log'),
                onPressed: _copy,
                style: AppButtons.outlined(p).copyWith(
                  minimumSize: const WidgetStatePropertyAll(Size(0, 36)),
                ),
                icon: Icon(
                  _copied == true ? Icons.check_rounded : Icons.copy_rounded,
                  size: 17,
                ),
                label: Text(switch (_copied) {
                  true => l10n.copied,
                  false => l10n.copyAgain,
                  null => l10n.copy,
                }),
              ),
            ],
          ),
          if (_loading) ...[
            const SizedBox(height: 10),
            const LinearProgressIndicator(),
          ],
          if (warning != null) ...[
            const SizedBox(height: 10),
            AppStatusLine(
              warning,
              tone: _failed ? AppTone.danger : AppTone.warn,
            ),
          ],
          const SizedBox(height: 12),
          Flexible(
            child: ConstrainedBox(
              constraints: const BoxConstraints(maxHeight: 420),
              child: DecoratedBox(
                decoration: BoxDecoration(
                  color: p.tileWell,
                  borderRadius: BorderRadius.circular(12),
                  border: Border.all(color: p.tileLine),
                ),
                // As high as what it lists, up to the most above: a few
                // records do not leave a large empty box.
                child: entries.isEmpty
                    ? Center(
                        heightFactor: 1,
                        child: Padding(
                          padding: const EdgeInsets.all(20),
                          child: Text(
                            l10n.noEntriesShow,
                            textAlign: TextAlign.center,
                            style: uiText(size: 13, color: p.muted),
                          ),
                        ),
                      )
                    : ClipRRect(
                        borderRadius: BorderRadius.circular(12),
                        child: SelectionArea(
                          child: ListView.builder(
                            shrinkWrap: true,
                            itemCount: entries.length,
                            itemBuilder: (context, index) =>
                                _row(p, entries[index], index, gutter: marked),
                          ),
                        ),
                      ),
              ),
            ),
          ),
        ],
      );
    },
  );
}

String _label(LogEvent event) => switch (event) {
  LogEvent.appStarted => l10n.appStarted,
  LogEvent.brokerTest => l10n.testServerConnection,
  LogEvent.signIn => l10n.sign,
  LogEvent.inventory => l10n.desktopListLoaded,
  LogEvent.streamStart => l10n.streamingConnectionStarted,
  LogEvent.streamReady => l10n.streamingReadyReply,
  LogEvent.streamEnd => l10n.streamingEnded,
  LogEvent.viewportRequest => l10n.resolutionChangeRequested,
  LogEvent.viewportResult => l10n.resolutionChangeResult,
  LogEvent.updateCheck => l10n.checkUpdates,
  LogEvent.updateResult => l10n.updateCheckResult,
  LogEvent.updateDownload => l10n.updateDownload,
  LogEvent.installerStart => l10n.hostInstallStarted,
  LogEvent.installerStage => l10n.installStep,
  LogEvent.installerResult => l10n.installResult,
  LogEvent.displayObserved => l10n.virtualDisplayStatus,
  LogEvent.driverPreserved => l10n.existingDriverKept,
  LogEvent.driverInstall => l10n.virtualDisplayDriverInstall,
  LogEvent.displayConfigure => l10n.virtualDisplayTurned,
  LogEvent.modeCatalog => l10n.resolutionListChecked,
  LogEvent.serviceState => l10n.serviceStatus,
  LogEvent.brokerState => l10n.serverResponseStatus,
  LogEvent.hostAction => l10n.hostSettingsAction,
  LogEvent.updateInstall => l10n.updateInstall,
  LogEvent.clipboard => l10n.clipboard,
  LogEvent.drive => l10n.driveSharing,
  LogEvent.usb => l10n.usbDevices,
  LogEvent.print => l10n.androidPrinting,
  LogEvent.serverSave => l10n.serverAddressSave,
};

// Records whose title already says everything (no field list under them).
bool _versioned(LogEvent event) => const {
  LogEvent.updateResult,
  LogEvent.updateDownload,
  LogEvent.updateInstall,
  LogEvent.clipboard,
}.contains(event);

/// Update records read as one sentence with their versions ("업데이트 확인 ·
/// 새 버전 1.2.4 발견", "1.2.3 → 1.2.4 업데이트 후 재시작"); records from
/// before versions were logged keep a version-less sentence.
String _title(ProductLogEntry entry) {
  final values = entry.values;
  String? version(String key) =>
      values[key] is int ? logVersionText(values[key] as int) : null;
  final current = version('current'), latest = version('latest');
  final failed = values['ok'] == false;
  switch (entry.event) {
    case LogEvent.print:
      return switch (values['stage']) {
        0 => l10n.androidPrintingDocumentRequested,
        1 => l10n.androidPrintingRequestReceived,
        2 => l10n.androidPrintingDocumentChecked,
        3 => l10n.androidPrintingSystemPrintRequested,
        4 => l10n.androidPrintingPrintServiceConnected,
        5 => l10n.androidPrintingLayoutChecked,
        6 => l10n.androidPrintingDocumentTransferStarted,
        7 => l10n.androidPrintingDocumentTransferFinished,
        8 => l10n.androidPrintingPrintScreenClosed,
        9 => l10n.androidPrintingSystemErrorSeen,
        10 => l10n.androidPrintingJobAccepted,
        11 => l10n.androidPrintingCanceled,
        12 => l10n.androidPrintingRequestFailed,
        _ => l10n.androidPrinting,
      };
    case LogEvent.updateResult:
      if (failed) {
        if (values['stage'] == 1) {
          return switch (values['error']) {
            1 => l10n.updateCheckFailedCheckRunning,
            2 => l10n.updateCheckFailedUpdateList,
            3 => l10n.updateCheckFailedCouldNot,
            _ => l10n.updateCheckFailed,
          };
        }
        return l10n.updateCheckFailed;
      }
      if (values['available'] == true) {
        return latest == null
            ? l10n.updateCheckNewVersionAvailable
            : l10n.updateCheckNewVersionFound(latest);
      }
      if (current != null && latest == null) return l10n.updateCheckNoPublishedVersion;
      return current == null ? l10n.updateCheckUpDate : l10n.updateCheckUpDate2(current);
    case LogEvent.updateDownload:
      final name = latest ?? l10n.update;
      return failed ? l10n.downloadFailed(name) : l10n.downloadFinished(name);
    case LogEvent.updateInstall:
      final step = current != null && latest != null
          ? '$current → $latest'
          : l10n.update;
      if (failed) return l10n.installFailed(step);
      return values['manual'] == true
          ? l10n.installerStarted(step)
          : l10n.restartingAfterUpdate(step);
    case LogEvent.clipboard:
      final direction = values['remote'] == true ? l10n.pcDesktop2 : l10n.desktopPc;
      return switch (values['action']) {
        1 => values['auto'] == true ? l10n.clipboardConnectedCopyPasteShared : l10n.clipboardConnected,
        2 => failed ? l10n.clipboardTransferFailed(direction) : l10n.clipboardTransfer(direction),
        3 => failed ? l10n.clipboardDisconnected : l10n.clipboardConnectionEnded,
        4 => l10n.couldNotPrepareAutomaticClipboard,
        _ => l10n.clipboard,
      };
    case LogEvent.drive:
      return switch (values['stage']) {
        1 => l10n.sharedFolderConnected,
        2 => l10n.sharedFolderChanged,
        3 => failed ? l10n.desktopDriveConnectionFailed : l10n.desktopDriveConnected,
        4 => failed ? l10n.sharedFolderDisconnected : l10n.sharedFolderConnectionEnded,
        _ => l10n.sharedFolder,
      };
    case LogEvent.usb:
      return switch (values['stage']) {
        1 => l10n.usbConnectionReady,
        2 => failed ? l10n.usbDeviceConnectionFailed : l10n.usbDeviceConnected,
        3 => l10n.usbDeviceRemoved,
        4 => l10n.desktopDidNotAcceptUsb,
        5 => failed ? l10n.usbDisconnected : l10n.usbConnectionEnded,
        6 => l10n.usbCaptureDriverInstallFailed,
        _ => l10n.usbDevices,
      };
    default:
      return _label(entry.event);
  }
}

/// The values as short words: a true flag is its name, a false one its
/// opposite where that means something (else left out), "성공" is implied by
/// the absence of a mark, and width/height read as one resolution.
String? _values(Map<String, Object> values) {
  final negative = {
    'installed': l10n.notInstalled2,
    'started': l10n.deviceNotStarted,
    'running': l10n.notRunning,
    'registered': l10n.notRegistered2,
    'online': l10n.noResponse,
    'changed': l10n.noChange,
    'consoleActive': l10n.consoleSessionOff,
  };
  final width = values['width'], height = values['height'];
  final parts = <String>[
    if (width is int && height is int && width > 0 && height > 0)
      l10n.resolution2(width, height),
    for (final MapEntry(:key, :value) in values.entries)
      if (key == 'ok' || key == 'width' || key == 'height')
        ...const <String>[]
      else if (value == true)
        _field(key)
      else if (value == false)
        ?negative[key]
      else if (key == 'elapsedMs')
        l10n.tookMs(value)
      else
        '${_field(key)} $value',
  ];
  return parts.isEmpty ? null : parts.join(' · ');
}

String _field(String value) =>
    {
      'ok': l10n.succeeded,
      'action': l10n.work,
      'stage': l10n.step,
      'installed': l10n.installed,
      'started': l10n.deviceStarted,
      'active': l10n.activeDisplay,
      'owned': l10n.ownDevices,
      'pending': l10n.waitingSetup,
      'recovery': l10n.repairNeeded,
      'reboot': l10n.restartNeeded,
      'remote': l10n.rdpSession,
      'monitors': l10n.monitors,
      'running': l10n.running,
      'registered': l10n.registered,
      'online': l10n.responded,
      'width': l10n.landscape,
      'height': l10n.portrait,
      'error': l10n.errorCode2,
      'changed': l10n.changed,
      'restartAttempted': l10n.deviceRestartTried,
      'expected': l10n.restore2,
      'missing': l10n.notRestored,
      'missingOwned': l10n.notRestoredVirtualMonitor,
      'fallback': l10n.backConsoleSession,
      'consoleExpected': l10n.consoleSessionNeedsRestoring,
      'consoleActive': l10n.consoleSessionActive,
      'stale': l10n.adapterBeforeRestart,
      'remapped': l10n.adapterReconnected,
      'dropped': l10n.skippedPackets,
      'available': l10n.newVersionAvailable2,
      'shares': l10n.sharedFolder,
      'mapped': l10n.desktopDrive,
      'requests': l10n.fileRequests,
      'failedRequests': l10n.failedRequests,
      'remembered': l10n.rememberedFolders,
      'restored': l10n.reopenedFolders,
      'usbDevices': l10n.connectedDevices,
      'usbUrbs': l10n.handledRequests,
      'usbKib': l10n.kibTransferred,
      'usbClass': l10n.deviceType,
      'osError': l10n.systemErrorCode,
    }[value] ??
    value;
