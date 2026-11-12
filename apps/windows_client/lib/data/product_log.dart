import 'dart:async';
import 'dart:convert';
import 'dart:io';

import 'package:flutter/foundation.dart';

import 'client_diagnostic_codes.dart';
import 'connect_timing.dart';

enum LogEvent {
  appStarted,
  brokerTest,
  signIn,
  inventory,
  streamStart,
  streamReady,
  streamEnd,
  viewportRequest,
  viewportResult,
  updateCheck,
  updateResult,
  updateDownload,
  installerStart,
  installerStage,
  installerResult,
  displayObserved,
  driverPreserved,
  driverInstall,
  displayConfigure,
  modeCatalog,
  serviceState,
  brokerState,
  hostAction,

  /// The update handed to the installer (local log only; never sent).
  updateInstall,

  /// Clipboard channel and transfer outcomes (fixed codes, never content).
  clipboard,

  /// drive-v1 sharing outcomes (fixed codes and counts, never names or paths).
  drive,

  /// usb-v1 sharing outcomes (fixed codes and counts, never a device name,
  /// vendor/product ID, serial number or data).
  usb,

  /// Local Android print stages only; no document metadata or content.
  print,

  /// A tested server address that could not be saved (local log only; never
  /// sent): where it stopped and the kind of failure, see
  /// server_save_log.dart. Never the address or the exception's text.
  serverSave,
}

/// A product version as one log number (1.2.3 -> 1002003): log values are
/// numbers and booleans only. Null when it does not fit.
int? logVersion(String version) {
  final match = RegExp(r'^(\d{1,3})\.(\d{1,3})\.(\d{1,3})$')
      .firstMatch(version);
  if (match == null) return null;
  return int.parse(match[1]!) * 1000000 +
      int.parse(match[2]!) * 1000 +
      int.parse(match[3]!);
}

String logVersionText(int value) =>
    '${value ~/ 1000000}.${value ~/ 1000 % 1000}.${value % 1000}';

const logFields = <String>{
  'ok',
  'action',
  'stage',
  'installed',
  'started',
  'active',
  'owned',
  'pending',
  'recovery',
  'reboot',
  'remote',
  'monitors',
  'running',
  'registered',
  'online',
  'width',
  'height',
  'error',
  'changed',
  'restartAttempted',
  'expected',
  'missing',
  'missingOwned',
  'fallback',
  'consoleExpected',
  'consoleActive',
  'stale',
  'remapped',
  // Host 240 (send queue skipped media): the packets skipped.
  'dropped',
  'available',
  'elapsedMs',
  // Update records: running and catalog versions (logVersion) and whether
  // the graphical installer was opened instead of an in-place update.
  'current',
  'latest',
  'manual',
  // Update downloads: requests made (an interrupted transfer continues) and
  // KiB on disk when it ended.
  'attempts',
  'receivedKib',
  // Clipboard records: an automatic (Ctrl+C/Ctrl+V) transfer.
  'auto',
  // Drive records: folders offered to the VM, drive letters it mapped, file
  // requests served and refused.
  'shares',
  'mapped',
  'requests',
  'failedRequests',
  // Drive stage 10: remembered folders found / opened again on this PC.
  'remembered',
  'restored',
  // usb-v1 records (counts only): devices shared, requests served, KiB moved.
  'usbDevices',
  // usb-v1 stage 15: what one listing saw (counts only): devices
  // UsbDk listed, left out as this PC's own, refused by the policy, and whose
  // descriptors could not be read.
  'usbFound',
  'usbHidden',
  'usbDenied',
  'usbUnreadable',
  'usbUrbs',
  'usbKib',
  'usbClass',
  // usb-v1: requests the VM's device stalled / that failed / timed
  // out / ran longer than 5 s, the class of the first non-OK request
  // (bmRequestType<<8 | bRequest, or 0x10000 | type<<8 | endpoint), and whether
  // Windows took the device back after it was given back (0 unknown, 1 yes, 2 no).
  'usbStalls',
  'usbFailed',
  'usbTimeouts',
  'usbSlow',
  'usbReq',
  'usbBack',
  // usb-v1: the raw Windows USBD_STATUS of that first request.
  'usbUsbd',
  // viewportResult, milliseconds: this
  // client's decoder check before the request, request -> the relay's
  // acknowledgement, request -> the new size's stream.info, its decoder start
  // and decoder start -> the first new picture; then the Host's stages from
  // that stream.info (resize_stages.hpp). elapsedMs is request -> picture.
  ...viewportStageFields,
  // streamReady: the stream's audio format (1 PCM, 2 Opus) and,
  // on Windows, whether its decoder started before stream.info.
  'audioFormat',
  'decoderPrestarted',
  // serverSave: the operating system's error number of a settings file that
  // could not be written (5 is access denied on Windows).
  'osError',
};

/// viewportResult stage fields (see logFields).
const viewportStageFields = <String>{
  'probeMs',
  'ackMs',
  'infoMs',
  'decoderMs',
  'frameMs',
  'hostSuspendMs',
  'hostDisplayMs',
  'hostSdkMs',
  'hostSettleMs',
  'hostConfirmMs',
  'hostRestartMs',
  'hostCaptureStartMs',
  'hostEncoderStartMs',
  'hostAudioStartMs',
};

/// [logFields] and the connect-timing-v1 stages of a streamReady record
/// (connect_timing.dart), joined at run time so that a name both lists carry
/// never breaks the constant set.
final _keptFields = {...logFields, ...connectTimingFields};

class ProductLogEntry {
  const ProductLogEntry(this.time, this.event, this.values, {
    this.code,
    this.inheritSession = true,
  });
  final DateTime time;
  final LogEvent event;
  final Map<String, Object> values;
  final String? code;
  /// Local reporter scope only; never serialized or trusted from forwarded logs.
  final bool inheritSession;
  static Map<String, Object> safeValues(Map<Object?, Object?> source) => {
    for (final key in _keptFields)
      if (source[key] is bool ||
          (source[key] is int && (source[key] as int).abs() <= 0xffffffff))
        key: source[key] as Object,
  };
  Map<String, dynamic> toJson() => {
    'time': time.toUtc().toIso8601String(),
    'event': event.name,
    'values': values,
    if (code != null)
      'code': diagnosticCodes.contains(code) ? code : 'unknown_failure',
  };
  static ProductLogEntry? parse(Object? raw) {
    if (raw is! Map ||
        raw['time'] is! String ||
        (raw['time'] as String).length > 40 ||
        raw['event'] is! String ||
        raw['values'] is! Map) {
      return null;
    }
    final time = DateTime.tryParse(raw['time'] as String);
    final event = LogEvent.values
        .where((value) => value.name == raw['event'])
        .firstOrNull;
    if (time == null || event == null) {
      return null;
    }
    return ProductLogEntry(
      time.toUtc(),
      event,
      Map.unmodifiable(safeValues(raw['values'] as Map)),
      code: raw['code'] == null
          ? null
          : diagnosticCodes.contains(raw['code'])
          ? raw['code'] as String
          : 'unknown_failure',
    );
  }
}

typedef ProductLogSink = FutureOr<void> Function(ProductLogEntry entry);

/// Fixed event names and boolean/numeric fields only. Never accepts messages,
/// URLs, account IDs, passwords, tokens, clipboard content or exception text.
class ProductLog extends ChangeNotifier {
  ProductLog({ProductLogSink? recordSink}) {
    if (recordSink != null) forwardTo(recordSink);
  }
  static final instance = ProductLog();
  static const maximumEntries = 256, maximumBytes = 65536, maximumFiles = 8;
  final List<ProductLogEntry> _entries = [];
  List<ProductLogEntry> get entries => List.unmodifiable(_entries);
  Future<void> _writes = Future.value();
  Directory? _directory;
  File? _file;
  int _size = 0, _segment = 0;
  String _run = '';
  ProductLogSink? _recordSink;
  final _localRecords = StreamController<ProductLogEntry>.broadcast(sync: true);

  /// Only this engine's new records; forwarded child records are already
  /// uploaded by their own authenticated, desktop-scoped reporter.
  Stream<ProductLogEntry> get localRecords => _localRecords.stream;
  bool storageAvailable = true;

  /// Child engines share the dashboard's writer instead of rotating its files.
  /// Configure before initialization; the sink receives sanitized records only.
  void forwardTo(ProductLogSink sink) {
    if (_directory != null) {
      throw StateError('Log storage already initialized.');
    }
    _recordSink = sink;
  }

  Future<void> initialize(Directory directory) async {
    if (_recordSink != null) return;
    try {
      if (await FileSystemEntity.type(directory.path, followLinks: false) !=
              FileSystemEntityType.notFound &&
          await FileSystemEntity.type(directory.path, followLinks: false) !=
              FileSystemEntityType.directory) {
        throw const FileSystemException('Invalid log directory');
      }
      await directory.create(recursive: true);
      _directory = directory;
      final files = await _files();
      for (final file in files.take(maximumFiles).toList().reversed) {
        if (await file.length() > maximumBytes) continue;
        for (final line in await file.readAsLines()) {
          try {
            final entry = ProductLogEntry.parse(jsonDecode(line));
            if (entry != null) _append(entry);
          } catch (_) {
            /* A partial crash record is ignored. */
          }
        }
      }
      _run = '${DateTime.now().toUtc().microsecondsSinceEpoch}.$pid';
      await _rotate();
    } catch (_) {
      storageAvailable = false;
      _directory = null;
      _file = null;
    }
  }

  Future<List<File>> _files() async {
    final directory = _directory;
    if (directory == null) return [];
    final result = <File>[];
    await for (final entity in directory.list(followLinks: false)) {
      if (entity is File &&
          RegExp(r'^events\.\d+\.\d+\.\d+\.jsonl$')
              .hasMatch(entity.uri.pathSegments.last) &&
          await FileSystemEntity.type(entity.path, followLinks: false) ==
              FileSystemEntityType.file) {
        result.add(entity);
      }
    }
    result.sort((a, b) => b.path.compareTo(a.path));
    return result;
  }

  Future<void> _rotate() async {
    final directory = _directory;
    if (directory == null) return;
    _file = File(
      '${directory.path}${Platform.pathSeparator}events.$_run.${_segment++}.jsonl',
    );
    await _file!.create(exclusive: true);
    _size = 0;
    final files = await _files();
    for (final old in files.skip(maximumFiles)) {
      await old.delete();
    }
  }

  void _append(ProductLogEntry entry) {
    _entries.add(entry);
    if (_entries.length > maximumEntries) _entries.removeAt(0);
  }

  void record(LogEvent event, [Map<String, dynamic> values = const {}]) {
    final entry = ProductLogEntry(
      DateTime.now().toUtc(),
      event,
      Map.unmodifiable(ProductLogEntry.safeValues(values)),
    );
    _record(entry, forward: true);
  }

  /// Accepts only fixed failure codes, never arbitrary exception text. `ok` in
  /// [values] is the verdict: beside ok:true the code is information only.
  void recordFailure(
    LogEvent event,
    String? code, [
    Map<String, dynamic> values = const {},
  ]) {
    _recordFailure(event, code, values, inheritSession: true);
  }

  /// A failed new preparation has no established session to inherit. Keep the
  /// reporter's desktop context without borrowing a previous session ID.
  void recordPreparationFailure(
    LogEvent event,
    String? code, [
    Map<String, dynamic> values = const {},
  ]) {
    _recordFailure(event, code, values, inheritSession: false);
  }

  void _recordFailure(
    LogEvent event,
    String? code,
    Map<String, dynamic> values, {
    required bool inheritSession,
  }) {
    _record(
      ProductLogEntry(
        DateTime.now().toUtc(),
        event,
        Map.unmodifiable(ProductLogEntry.safeValues(values)),
        code: diagnosticCodes.contains(code) ? code : 'unknown_failure',
        inheritSession: inheritSession,
      ),
      forward: true,
    );
  }

  /// Received records never re-enter the sink, even if a forwarding logger is
  /// accidentally used as the receiver. Arbitrary payload fields are discarded.
  bool acceptForwarded(Object? raw) {
    final entry = ProductLogEntry.parse(raw);
    if (entry == null) return false;
    _record(entry, forward: false);
    return true;
  }

  void _record(ProductLogEntry entry, {required bool forward}) {
    _append(entry);
    if (forward && !_localRecords.isClosed) _localRecords.add(entry);
    notifyListeners();
    final sink = _recordSink;
    if (forward && sink != null) {
      _writes = _writes.then((_) async => await sink(entry)).catchError((
        Object _,
      ) {
        storageAvailable = false;
        notifyListeners();
      });
      return;
    }
    if (_directory == null) return;
    final line = '${jsonEncode(entry.toJson())}\n';
    _writes = _writes
        .then((_) async {
          if (_size + utf8.encode(line).length > maximumBytes) await _rotate();
          await _file!.writeAsString(line, mode: FileMode.append, flush: true);
          _size += utf8.encode(line).length;
        })
        .catchError((Object _) {
          storageAvailable = false;
          notifyListeners();
        });
  }

  Future<void> flush() => _writes;

  @override
  void dispose() {
    unawaited(_localRecords.close());
    super.dispose();
  }
}
