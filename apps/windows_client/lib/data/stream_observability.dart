import 'dart:collection';
import 'dart:convert';
import 'dart:io';

import 'user_data_path.dart';

const streamObservabilityProtocol = 'stream-observability-v1';

/// Explicit opt-in. Windows child engines load the same per-user preference.
abstract final class StreamObservability {
  static bool enabled = false;
  static File? _store;
  static Future<void> _writes = Future.value();
  static Future<void> init(File? store) async {
    await _writes;
    _store = store;
    enabled = false;
    if (store == null) return;
    try {
      if (await store.length() > 256) return;
      final raw = jsonDecode(await store.readAsString());
      enabled = raw is Map && raw['enabled'] == true;
    } catch (_) {
      // Missing/corrupt preferences cannot enable telemetry or block startup.
    }
  }

  static Future<void> initWindows() async {
    final root = Platform.environment['LOCALAPPDATA'];
    await init(
      root == null || root.isEmpty
          ? null
          : File('$root/Multitap/observability.json'),
    );
  }

  static Future<void> choose(bool value) async {
    enabled = value;
    final store = _store;
    if (store == null) return;
    _writes = _writes.then((_) async {
      try {
        await writeUserDataFile(store, jsonEncode({'enabled': value}));
      } catch (_) {
        /* The current window still honors the choice. */
      }
    });
    await _writes;
  }
}

const hostObservationBounds = <String, int>{
  'hostQueuePackets': 4096,
  'hostQueueBytes': 16777216,
  'hostQueueOldestUs': 60000000,
  'hostWriteUs': 60000000,
  'readbackUs': 60000000,
  'readbackMaxUs': 60000000,
  'serviceCpuPct10': 1000,
  'workerCpuPct10': 1000,
  'relayQueueBytes': 16777216,
  'relayForwardUs': 60000000,
  'relayVideoQueuePackets': 4096,
  'relayVideoQueueOldestUs': 60000000,
};

Map<String, int> parseHostObservation(Object? raw) {
  if (raw is! Map || raw['protocol'] != streamObservabilityProtocol) return {};
  final values = raw['metrics'];
  if (values is! Map) return {};
  return {
    for (final entry in hostObservationBounds.entries)
      if (values[entry.key] is int &&
          (values[entry.key] as int) >= 0 &&
          (values[entry.key] as int) <= entry.value)
        entry.key: values[entry.key] as int,
  };
}

Map<String, String> parseObservabilityPath(Object? raw) {
  if (raw is! Map) return {};
  const values = {'direct', 'proxy', 'plain', 'unknown'};
  return {
    for (final key in ['clientTls', 'hostTls'])
      if (values.contains(raw[key])) key: raw[key] as String,
  };
}

Map<String, String?> parseObservabilityVersions(Object? raw) {
  if (raw is! Map) return {};
  final pattern = RegExp(
    r'^[0-9]{1,6}\.[0-9]{1,6}\.[0-9]{1,6}(?:\+[0-9]{1,10})?$',
  );
  return {
    for (final key in ['broker', 'host'])
      key: raw[key] is String && pattern.hasMatch(raw[key] as String)
          ? raw[key] as String
          : null,
  };
}

const clientObservationBounds = <String, int>{
  ...hostObservationBounds,
  'clientInboxPackets': 4096,
  'clientInboxBytes': 16777216,
  'clientInboxOldestUs': 60000000,
  'inputGateWaitUs': 60000000,
  'inputGateWaitMaxUs': 60000000,
  'inputGateWaitCount': 0xffffffff,
  'inputGateQueuePackets': 128,
  'decodeQueuePackets': 4096,
  'decodeQueueBytes': 16777216,
  'decodeQueueOldestUs': 60000000,
  'presentQueuePackets': 4096,
  'presentQueueOldestUs': 60000000,
  'decodeUs': 60000000,
  'textureWaitUs': 60000000,
  'textureUploadUs': 60000000,
  'captureFps10': 12000,
  'encodeFps10': 12000,
  'decodeFps10': 12000,
  'textureFps10': 12000,
  'bitrateKbps': 1000000,
  'appRttUs': 60000000,
  'hostCaptureUs': 60000000,
  'hostConvertUs': 60000000,
  'hostEncodeUs': 60000000,
  'hostWorkUs': 60000000,
};

Map<String, Object> parseOutputObservation(Object? raw) {
  if (raw is! Map || !const {'0', '1'}.contains(raw['id'])) return {};
  final epoch = raw['epoch'];
  if (epoch is! int || epoch < 0 || epoch > 0xffffffff) return {};
  final result = <String, Object>{'id': raw['id'] as String, 'epoch': epoch};
  const numeric = <String, num>{
    'width': 1048576,
    'height': 1048576,
    'encodedWidth': 1048576,
    'encodedHeight': 1048576,
    'capturedFps': 240,
    'encodedFps': 240,
    'bitrateKbps': 1000000,
    'decodedFrames': 9007199254740991,
    'textureFrames': 9007199254740991,
    'nativeFrameReads': 9007199254740991,
    'decodeCalls': 9007199254740991,
    'decodeTotalMs': 1e12,
    'decodeMaxMs': 60000,
    'decoderFallbacks': 0xffffffff,
    'decodeTier': 2,
    // The output's own decode lane (Windows engine): packets waiting, the
    // oldest one's age and how often a packet found the lane full.
    'queuedPackets': 64,
    'queueOldestMs': 60000,
    'queueFullRefusals': 0xffffffff,
  };
  for (final entry in numeric.entries) {
    final value = raw[entry.key];
    if (value is num && value.isFinite && value >= 0 && value <= entry.value) {
      result[entry.key] = value;
    }
  }
  if (raw['hardwareDecode'] is bool) {
    result['hardwareDecode'] = raw['hardwareDecode'] as bool;
  }
  return result;
}

/// Snapshot percentiles, never frame percentiles or physical display latency.
/// Stores numeric allowlisted values only; no URLs, names or input content.
class StreamObservationReport {
  StreamObservationReport({this.maximumSamples = 600})
    : assert(maximumSamples > 0 && maximumSamples <= 3600);

  final int maximumSamples;
  final _rows = Queue<Map<String, Object>>();
  int _total = 0, _lastUs = -1;
  bool _finished = false;

  int get retained => _rows.length;

  void add(
    int nowUs,
    String phase,
    Map<String, Object?> values, {
    int? epoch,
    Map<String, Object?> outputs = const {},
  }) {
    if (_finished || nowUs < 0 || nowUs < _lastUs) return;
    if (!const {
      'connecting',
      'streaming',
      'resizing',
      'closed',
    }.contains(phase)) {
      return;
    }
    final safe = <String, Object>{};
    for (final entry in clientObservationBounds.entries) {
      final n = values[entry.key];
      if (n is int && n >= 0 && n <= entry.value) safe[entry.key] = n;
    }
    final safeOutputs = <String, Object>{};
    for (final entry in outputs.entries) {
      if (!const {'0', '1'}.contains(entry.key) || entry.value is! Map) {
        continue;
      }
      final sources = entry.value as Map;
      final safeSources = <String, Object>{};
      for (final key in ['host', 'decoder']) {
        final source = parseOutputObservation(sources[key]);
        if (source['id'] == entry.key) safeSources[key] = source;
      }
      if (safeSources.isNotEmpty) safeOutputs[entry.key] = safeSources;
    }
    _rows.add({
      'atMs': nowUs ~/ 1000,
      'phase': phase,
      'metrics': safe,
      if (safeOutputs.isNotEmpty) 'outputs': safeOutputs,
      if (epoch != null && epoch >= 0 && epoch <= 0xffffffff) 'epoch': epoch,
    });
    _lastUs = nowUs;
    _total++;
    while (_rows.length > maximumSamples) {
      _rows.removeFirst();
    }
    if (phase == 'closed') _finished = true;
  }

  Map<String, Object?> export({
    required String sessionId,
    required bool enabled,
    required bool negotiated,
    required int outputs,
    Map<String, String> path = const {},
    Map<String, Object?> facts = const {},
    Map<String, Object?> distributions = const {},
    Map<String, String?> versions = const {},
    bool inboxObserved = false,
  }) {
    final series = <String, List<int>>{};
    for (final row in _rows) {
      if (row['phase'] != 'streaming') continue;
      for (final entry in (row['metrics'] as Map<String, Object>).entries) {
        (series[entry.key] ??= []).add(entry.value as int);
      }
    }
    final summary = <String, Object>{};
    for (final entry in series.entries) {
      final sorted = entry.value..sort();
      int percentile(double p) => sorted[(sorted.length * p).ceil() - 1];
      summary[entry.key] = {
        'samples': sorted.length,
        'min': sorted.first,
        'max': sorted.last,
        'mean': sorted.fold<double>(0, (a, b) => a + b) / sorted.length,
        'snapshotP50': percentile(.5),
        'snapshotP95': percentile(.95),
      };
    }
    // Facts are categorical/boolean and explicitly projected, never raw stats.
    final safeFacts = <String, Object>{};
    const enums = <String, Set<Object>>{
      'captureBackend': {'wgc-d3d11', 'wgc-warp', 'gdi'},
      'encoderBackend': {'nvenc', 'amf', 'qsv-mft', 'qsv', 'x264', 'mft'},
      'decoderMode': {'hardware', 'software', 'unknown'},
      'cursorNegotiatedMode': {'separate', 'composed'},
    };
    for (final entry in enums.entries) {
      final value = facts[entry.key];
      if (entry.value.contains(value)) safeFacts[entry.key] = value!;
    }
    const factBounds = {
      'targetFps': 120,
      'sourceWidth': 1048576,
      'sourceHeight': 1048576,
      'encodedWidth': 1048576,
      'encodedHeight': 1048576,
      'encoderGpuFallbackCode': 0xffffffff,
      'encoderSoftwareFallbackCode': 0xffffffff,
      'encoderObservedAtMs': 9007199254740991,
      'encoderObservedEpoch': 0xffffffff,
    };
    for (final entry in factBounds.entries) {
      final value = facts[entry.key];
      if (value is int && value >= 0 && value <= entry.value) {
        safeFacts[entry.key] = value;
      }
    }
    final safeDistributions = <String, Object>{};
    for (final key in const [
      'hostApply',
      'roundTrip',
      'toHostEstimate',
      'inputToTextureProxy',
      'framePush',
      'frameDecode',
      'transfer',
      'downlinkEstimate',
    ]) {
      final raw = distributions[key];
      if (raw is! Map) continue;
      final count = raw['samples'], p50 = raw['p50Ms'], p95 = raw['p95Ms'];
      if (count is int &&
          count > 0 &&
          count <= 4096 &&
          p50 is num &&
          p95 is num &&
          p50.isFinite &&
          p95.isFinite &&
          p50 >= 0 &&
          p95 >= p50 &&
          p95 <= 60000) {
        safeDistributions[key] = {'samples': count, 'p50Ms': p50, 'p95Ms': p95};
      }
    }
    String? version(Object? value) {
      if (value is! String) return null;
      final match = RegExp(
        r'^[0-9]{1,6}\.[0-9]{1,6}\.[0-9]{1,6}(?:\+[0-9]{1,10})?$',
      ).firstMatch(value);
      return match != null && match.end == value.length ? value : null;
    }

    return {
      'schema': streamObservabilityProtocol,
      'sessionId': RegExp(r'^[a-fA-F0-9-]{36}$').hasMatch(sessionId)
          ? sessionId
          : null,
      'enabled': enabled,
      'negotiated': negotiated,
      'transport': 'websocket-tcp',
      'outputs': outputs.clamp(1, 2),
      'rateScope': outputs > 1 ? 'sum_across_outputs' : 'single_output',
      'presentationScope': 'texture_consumer_proxy_not_physical_scanout',
      'pathEvidenceScope': {
        'clientTls': 'connect_http_request',
        'hostTls': 'host_control_websocket',
      },
      'versions': {
        for (final key in ['client', 'broker', 'host'])
          key: version(versions[key]),
      },
      'path': parseObservabilityPath(path),
      'facts': safeFacts,
      'factsScope': 'last_validated_observations_with_available_context',
      'totalSnapshots': _total,
      'retainedSnapshots': retained,
      'summary': summary,
      'inputDistributions': safeDistributions,
      'snapshots': _rows.toList(growable: false),
      'unavailable': {
        'tcpRtt': 'os_connection_measurement_required',
        'physicalDisplayLatency': 'not_measured',
        'inputCausality': 'changed_frame_correlation_only',
        if (!inboxObserved)
          'clientInbox': 'requires_connection_started_with_collection',
        if (!negotiated) 'hostObservability': 'not_negotiated',
      },
      'units': 'Us=microseconds; Bytes=bytes; Fps10=fps*10; Pct10=percent*10',
      'percentileScope': 'retained_snapshot_values_not_frame_distribution',
      'summaryScope': 'retained_streaming_snapshots',
    };
  }

  String toJson({
    required String sessionId,
    required bool enabled,
    required bool negotiated,
    required int outputs,
    Map<String, String> path = const {},
    Map<String, Object?> facts = const {},
    Map<String, Object?> distributions = const {},
    Map<String, String?> versions = const {},
    bool inboxObserved = false,
  }) => const JsonEncoder.withIndent('  ').convert(
    export(
      sessionId: sessionId,
      enabled: enabled,
      negotiated: negotiated,
      outputs: outputs,
      path: path,
      facts: facts,
      distributions: distributions,
      versions: versions,
      inboxObserved: inboxObserved,
    ),
  );
}
