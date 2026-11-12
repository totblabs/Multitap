import 'dart:async';
import 'dart:convert';
import 'dart:math';

import 'client_build_version.dart';
import 'product_log.dart';
import 'client_diagnostic_codes.dart';
import 'stream_stall.dart';
import 'stream_stats_report.dart';

enum DiagnosticDelivery { accepted, retry, rejected, unavailable }

const diagnosticEvents = {
  'appStarted',
  'signIn',
  'inventory',
  'streamStart',
  'streamReady',
  'streamEnd',
  'viewportRequest',
  'viewportResult',
  'displayConfigure',
  'brokerRequest',
  'updateCheck',
  'updateResult',
  'updateDownload',
  'streamStats',
  // Not for an older broker, which rejects a batch with an unknown event.
  'clipboard',
  // drive-v1 brokers only: recorded only in a session such a broker granted.
  'drive',
  // usb-v1 brokers only (recorded only in a session such a broker granted USB).
  // Without this entry no USB record reaches the broker.
  'usb',
  // stream-stall-v1 brokers only: sent after the broker acknowledged it.
  'streamStall',
};

/// Memory-only, session-scoped diagnostics. No token, machine name, arbitrary
/// exception, clipboard or account text is accepted into the wire schema.
/// A failed acknowledgement retains exactly the same batch ID for retry.
class ClientDiagnostics {
  ClientDiagnostics({
    required this.send,
    this.desktopId,
    this.mode,
    ProductLog? log,
    this.interval = const Duration(seconds: 5),
    String? runId,
    String? Function()? version,
  }) : runId = runId ?? _newRun(),
       _version = version ?? _runningVersion {
    _subscription = (log ?? ProductLog.instance).localRecords.listen((entry) {
      record(
        entry.event.name,
        values: entry.values,
        time: entry.time,
        code: entry.code,
        inheritSession: entry.inheritSession,
      );
    });
    record('appStarted');
  }
  final Future<DiagnosticDelivery> Function(Map<String, Object?> report) send;
  final String? desktopId, mode;
  final String runId;
  final Duration interval;
  final String? Function() _version;
  final _queue = <Map<String, Object?>>[];
  // A definitive schema rejection is isolated to individual safe rows. A
  // future event or one oversized row must not disable all later evidence.
  final _isolatedRows = <Map<String, Object?>>[];
  static const maxBatchBytes = 16384;
  int rejectedBatches = 0, discardedRows = 0;
  StreamSubscription<ProductLogEntry>? _subscription;
  Timer? _timer;
  Map<String, Object?>? _batch;
  int _sequence = 0, _failures = 0;
  bool _closed = false, _sending = false;
  Completer<void>? _inFlight;
  bool get pending =>
      !_closed &&
      (_sending ||
          _batch != null ||
          _queue.isNotEmpty ||
          _isolatedRows.isNotEmpty);
  bool get retryPending => _failures != 0;
  String? _sessionId;
  String? _sessionDesktop;

  static String _newRun() {
    final random = Random.secure();
    return List.generate(
      16,
      (_) => random.nextInt(256).toRadixString(16).padLeft(2, '0'),
    ).join();
  }

  static String? _runningVersion() {
    final value = runningClientBuild;
    return value.version == null
        ? null
        : '${value.version}${value.build == null ? '' : '+${value.build}'}';
  }

  static bool validUuid(Object? value) =>
      value is String &&
      RegExp(
        r'^[a-fA-F0-9]{8}-[a-fA-F0-9]{4}-[a-fA-F0-9]{4}-[a-fA-F0-9]{4}-[a-fA-F0-9]{12}$',
      ).hasMatch(value);
  void bindSession(Object? value, {String? desktop}) {
    if (validUuid(value)) {
      _sessionId = value as String;
      _sessionDesktop = desktop ?? desktopId;
    }
  }

  void record(
    String event, {
    Map<String, Object?> values = const {},
    DateTime? time,
    String? desktop,
    String? connectionMode,
    Object? operationId,
    Object? requestId,
    String? sessionId,
    // Request-scoped observations can omit a session without discarding the
    // binding still owned by earlier session/feature events.
    bool inheritSession = true,
    String? code,
  }) {
    if (_closed || !diagnosticEvents.contains(event)) return;
    final target = desktop ?? desktopId;
    final selectedMode = connectionMode ?? mode;
    final row = <String, Object?>{
      'event': event,
      'time': (time ?? DateTime.now()).toUtc().toIso8601String(),
      'values': event == 'streamStats' || event == 'streamStall'
          ? {
              for (final key in event == 'streamStats'
                  ? streamStatsFields
                  : streamStallFields)
                if (values[key] is bool ||
                    (values[key] is int &&
                        (values[key] as int) >= 0 &&
                        (values[key] as int) <= 0xffffffff))
                  key: values[key],
            }
          : ProductLogEntry.safeValues(values),
      if (target != null && RegExp(r'^[A-Za-z0-9_-]{1,80}$').hasMatch(target))
        'desktopId': target,
      if (selectedMode == 'stream') 'mode': selectedMode,
      if (validUuid(operationId)) 'operationId': operationId,
      if (validUuid(requestId)) 'requestId': requestId,
      if (validUuid(sessionId) && target != null)
        'sessionId': sessionId
      else if (inheritSession &&
          _sessionId != null &&
          target != null &&
          target == _sessionDesktop)
        'sessionId': _sessionId,
      if (code != null)
        'code': diagnosticCodes.contains(code) ? code : 'unknown_failure',
    };
    _queue.add(row);
    if (_queue.length > 128) _queue.removeAt(0);
    _schedule();
  }

  void _schedule() {
    if (_closed || _sending || _timer != null) return;
    _timer = Timer(interval * (1 << _failures.clamp(0, 4)), () {
      _timer = null;
      unawaited(flush());
    });
  }

  Future<void> flush() async {
    if (_closed) return;
    if (_sending) return _inFlight!.future;
    if (_batch == null && _queue.isEmpty && _isolatedRows.isEmpty) return;
    _timer?.cancel();
    _timer = null;
    if (_batch == null) {
      final version = _version();
      final rows = <Map<String, Object?>>[];
      final report = <String, Object?>{
        'protocol': 'client-diagnostics-v1',
        'runId': runId,
        'sequence': _sequence + 1,
        'version':
            version != null &&
                RegExp(r'^\d{1,6}\.\d{1,6}\.\d{1,6}(?:\+\d{1,10})?$')
                    .hasMatch(version)
            ? version
            : null,
        'events': rows,
      };
      final source = _isolatedRows.isNotEmpty ? _isolatedRows : _queue;
      final maximum = _isolatedRows.isNotEmpty ? 1 : 16;
      while (source.isNotEmpty && rows.length < maximum) {
        rows.add(source.first);
        if (utf8.encode(jsonEncode(report)).length > maxBatchBytes) {
          rows.removeLast();
          if (rows.isNotEmpty) break;
          source.removeAt(0);
          discardedRows++;
          continue;
        }
        source.removeAt(0);
      }
      if (rows.isEmpty) {
        if (pending) _schedule();
        return;
      }
      _sequence++;
      _batch = report;
    }
    _sending = true;
    final completed = _inFlight = Completer<void>();
    try {
      final result = await send(_batch!);
      if (_closed) return;
      if (result == DiagnosticDelivery.accepted) {
        _batch = null;
        _failures = 0;
      } else if (result == DiagnosticDelivery.unavailable) {
        close();
      } else if (result == DiagnosticDelivery.rejected) {
        rejectedBatches++;
        final rows = (_batch!['events']! as List).cast<Map<String, Object?>>();
        if (rows.length > 1) {
          _isolatedRows.insertAll(0, rows);
        } else {
          discardedRows++;
        }
        _batch = null;
        _failures = 0;
      } else {
        _failures++;
      }
    } catch (_) {
      // Never enter ProductLog here: that would log/upload the upload failure.
      _failures++;
    } finally {
      _sending = false;
      _inFlight = null;
      completed.complete();
      if (pending) _schedule();
    }
  }

  void close() {
    _closed = true;
    _timer?.cancel();
    _timer = null;
    unawaited(_subscription?.cancel());
    _subscription = null;
    _queue.clear();
    _isolatedRows.clear();
    _batch = null;
  }
}
