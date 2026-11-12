import 'dart:typed_data';

import '../models/stream_options.dart';
import 'video_geometry.dart';
import 'stream_size.dart';

const _u32 = 0xffffffff, _maxPts = 9007199254740991;
bool _integer(Object? v, int low, int high) =>
    v is int && v >= low && v <= high;
bool _sameInteger(Object? value, int expected) =>
    value is int && value == expected;
bool _shape(Map value, List<String> keys) =>
    value.length == keys.length && keys.every(value.containsKey);
const profileTracksProtocol = 'group-v1';
bool independentTracksAdvertised(Object? value) =>
    value is Map &&
    value['videoTrackProtocol'] == 'tracks-v1' &&
    value['trackViewportProvisioning'] == true &&
    (value['streamProfileProtocol'] == null || profileTracksAdvertised(value));
// PC sessions use ordinary tracks, not the VM console-profile transaction.
// Keep the actual basic readiness and source-shape checks even when unrelated
// profile metadata is present or temporarily incomplete.
bool ordinaryPcTracksAdvertised(Object? value) {
  if (value is! Map ||
      value['videoTrackProtocol'] != 'tracks-v1' ||
      [
        'trackViewportProvisioning',
        'service',
        'interactiveSession',
        'streaming',
        'virtualDisplayReady',
      ].any((key) => value[key] != true)) {
    return false;
  }
  final monitors = value['monitors'];
  if (monitors is! List || monitors.isEmpty || monitors.length > 2) {
    return false;
  }
  final ids = <String>{};
  for (final monitor in monitors) {
    if (monitor is! Map ||
        !['0', '1'].contains(monitor['id']) ||
        !ids.add(monitor['id'] as String) ||
        !_integer(monitor['width'], 320, maximumGeometrySide) ||
        !_integer(monitor['height'], 180, maximumGeometrySide) ||
        !videoGeometry(
          monitor['width'] as int,
          monitor['height'] as int,
          minimumWidth: 320,
          minimumHeight: 180,
        )) {
      return false;
    }
  }
  return ids.contains('0');
}

bool profileTracksAdvertised(Object? value) {
  if (value is! Map ||
      value['streamProfileTracksProtocol'] != profileTracksProtocol ||
      value['streamProfileProtocol'] != 'console-profile-v1' ||
      value['videoTrackProtocol'] != 'tracks-v1' ||
      value['resizeProtocol'] != 'epoch-v1' ||
      [
        'trackViewportProvisioning',
        'viewportProvisioning',
        'dynamicResolution',
        'service',
        'interactiveSession',
        'streaming',
        'virtualDisplayReady',
      ].any((key) => value[key] != true)) {
    return false;
  }
  final monitors = value['monitors'];
  if (monitors is! List || monitors.isEmpty || monitors.length > 2) {
    return false;
  }
  final ids = <String>{};
  for (final monitor in monitors) {
    if (monitor is! Map ||
        !['0', '1'].contains(monitor['id']) ||
        !ids.add(monitor['id'] as String) ||
        !_integer(monitor['width'], 320, maximumGeometrySide) ||
        !_integer(monitor['height'], 180, maximumGeometrySide) ||
        (monitor['width'] as int).isOdd ||
        (monitor['height'] as int).isOdd) {
      return false;
    }
  }
  return ids.contains('0');
}

const _restoreReasons = {
  'unsupported_display_mode',
  'display_change_failed',
  'virtual_display_required',
  'monitor_not_available',
  'resize_failed',
};

class _TrackChange {
  _TrackChange(this.requestId, this.plan, this.fps, this.deadlineUs);
  final String requestId;
  TrackPlan plan;
  int fps;
  final int deadlineUs;
  String? resizeId, restoreReason;
  Map<String, int>? epochs;
  DecoderLimits? decoderLimits;
}

/// The actual output arrangement requested by an explicitly negotiated client.
/// This is separate from the resolution of a composite image.
class TrackViewport {
  const TrackViewport(this.id, this.width, this.height, this.x, this.y);
  final String id;
  final int width, height, x, y;
  Map<String, Object> toJson() => {
    'id': id,
    'width': width,
    'height': height,
    'x': x,
    'y': y,
  };
}

class TrackPlan {
  TrackPlan(Iterable<TrackViewport> outputs)
    : outputs = List.unmodifiable(
        outputs.toList()..sort((a, b) => a.id.compareTo(b.id)),
      ) {
    var pixels = 0;
    final ids = <String>{};
    if (this.outputs.isEmpty || this.outputs.length > 2) {
      throw const FormatException('Invalid output count.');
    }
    for (final t in this.outputs) {
      if (!['0', '1'].contains(t.id) ||
          !ids.add(t.id) ||
          t.width < 320 ||
          t.width > maximumGeometrySide ||
          t.width.isOdd ||
          t.height < 180 ||
          t.height > maximumGeometrySide ||
          t.height.isOdd ||
          !videoPlacement(t.width, t.height, t.x, t.y)) {
        throw const FormatException('Invalid output geometry.');
      }
      if (!videoGeometry(
            t.width,
            t.height,
            minimumWidth: 320,
            minimumHeight: 180,
          ) ||
          t.width * t.height > maximumWireInteger - pixels) {
        throw const FormatException('Output geometry overflow.');
      }
      pixels += t.width * t.height;
    }
    if (this.outputs.length == 2) {
      final a = this.outputs[0], b = this.outputs[1];
      if (a.x < b.x + b.width &&
          b.x < a.x + a.width &&
          a.y < b.y + b.height &&
          b.y < a.y + a.height) {
        throw const FormatException('Overlapping outputs.');
      }
    }
  }
  final List<TrackViewport> outputs;
  bool get legacyCompatible =>
      outputs.every(
        (t) =>
            legacyOwnedOutputSize(t.width, t.height) &&
            t.x >= -32768 &&
            t.y >= -32768 &&
            t.x + t.width <= 32768 &&
            t.y + t.height <= 32768,
      ) &&
      outputs.fold<int>(0, (sum, t) => sum + t.width * t.height) <=
          streamMaximumPixels;
  List<Map<String, Object>> toJson() => outputs.map((t) => t.toJson()).toList();
  bool matches(Object? value) {
    if (value is! List || value.length != outputs.length) return false;
    final seen = <String>{};
    for (final entry in value) {
      if (entry is! Map ||
          !_shape(entry, ['id', 'width', 'height', 'x', 'y'])) {
        return false;
      }
      final t = outputs.where((t) => t.id == entry['id']).firstOrNull;
      if (t == null ||
          !seen.add(t.id) ||
          !_sameInteger(entry['width'], t.width) ||
          !_sameInteger(entry['height'], t.height) ||
          !_sameInteger(entry['x'], t.x) ||
          !_sameInteger(entry['y'], t.y)) {
        return false;
      }
    }
    return true;
  }

  void checkOptions(StreamOptions options) {
    options.toJson();
    if ((outputs.length == 2
            ? options.monitor != 'all'
            : options.monitor != outputs.single.id) ||
        outputs.any(
          (t) => t.width > options.maxWidth || t.height > options.maxHeight,
        )) {
      throw const FormatException('Output options mismatch.');
    }
  }
}

class StreamTrack {
  const StreamTrack({
    required this.id,
    required this.epoch,
    required this.width,
    required this.height,
    required this.left,
    required this.top,
    required this.primary,
    required this.bitrate,
    this.encodedWidth,
    this.encodedHeight,
  });
  final String id;
  final int epoch, width, height, left, top, bitrate;
  final bool primary;
  final int? encodedWidth, encodedHeight;
  Map<String, Object> toNative() => {
    'id': id,
    'epoch': epoch,
    'width': encodedWidth ?? width,
    'height': encodedHeight ?? height,
    'sourceWidth': width,
    'sourceHeight': height,
    'left': left,
    'top': top,
    'primary': primary,
  };
  bool same(StreamTrack other) =>
      id == other.id &&
      epoch == other.epoch &&
      width == other.width &&
      height == other.height &&
      encodedWidth == other.encodedWidth &&
      encodedHeight == other.encodedHeight &&
      left == other.left &&
      top == other.top &&
      primary == other.primary &&
      bitrate == other.bitrate;
}

/// Client side of tracks-v1. Metadata, received IDRs and actual decoder frames
/// are independent evidence. Only the last one permits the ready acknowledgement.
class TrackStream {
  TrackStream(
    this.sessionId,
    TrackPlan plan,
    this.options, {
    this.profile = false,
    this.streamSizeV2 = false,
    this.decoderLimits,
    this.audioOpus = false,
  }) : _plan = plan,
       fps = options.fps {
    plan.checkOptions(options);
  }
  final String sessionId;
  final StreamOptions options;
  final bool profile;
  final bool streamSizeV2;

  /// audio-opus-tracks-v1 was granted: the metadata may state "opus", and the
  /// shared audio is then a u16 sequence + an Opus packet of any length.
  final bool audioOpus;

  /// The current outputs' audio is Opus (as their metadata stated).
  bool opusAudio = false;
  DecoderLimits? decoderLimits;
  TrackPlan _plan;
  TrackPlan get plan => _plan;
  int fps;
  _TrackChange? _change;
  Map<String, Object>? _lastCorrelation;
  bool get preparing => _change != null;
  int? get preparationDeadlineUs => _change?.deadlineUs;
  String? get restoreReason => _change?.restoreReason;
  List<StreamTrack> tracks = const [];
  Map<String, int> textures = const {};
  int topology = 0;
  bool audio = false, ready = false;
  final _keys = <String>{};
  final _pts = <String, int>{}, _videoAt = <String, int>{};
  final _decodedAt = <String, int>{}, _decodedCount = <String, int>{};
  int? _audioPts;
  bool get configured => topology != 0 && textures.length == tracks.length;
  StreamTrack get primary =>
      tracks.where((t) => t.primary).firstOrNull ?? tracks.first;

  /// This is a request, never evidence that Windows changed its outputs. The
  /// relay must bind it to a fresh topology and resize ID before metadata.
  Map<String, Object> requestResize(
    TrackPlan requested,
    int targetFps,
    String requestId,
    int nowUs, {
    DecoderLimits? nextDecoderLimits,
  }) {
    if (!profile ||
        !ready ||
        preparing ||
        topology == _u32 ||
        ![30, 60].contains(targetFps) ||
        !RegExp(r'^[A-Za-z0-9_-]{1,64}$').hasMatch(requestId) ||
        requested.outputs.length != tracks.length ||
        requested.outputs.indexed.any((p) => p.$2.id != tracks[p.$1].id)) {
      throw const FormatException('Output resize unavailable.');
    }
    // Track viewports are physical output sizes, independent of quality ceilings.
    _change = _TrackChange(requestId, requested, targetFps, nowUs + 90000000);
    _change!.decoderLimits = nextDecoderLimits;
    ready = false;
    return {
      'type': 'stream.tracks.resize',
      'requestId': requestId,
      'topologyEpoch': topology,
      'tracks': requested.toJson(),
      'fps': targetFps,
      if (nextDecoderLimits != null)
        'decoderLimits': nextDecoderLimits.toJson(),
    };
  }

  bool acceptResizing(Map<String, dynamic> value, int nowUs) {
    final change = _change;
    if (change == null || value['requestId'] != change.requestId) return false;
    if (!_shape(value, [
          'type',
          'requestId',
          'resizeId',
          'topologyEpoch',
          'tracks',
          'fps',
          'timeoutMs',
        ]) ||
        value['type'] != 'stream.tracks.resizing' ||
        !_sameInteger(value['topologyEpoch'], topology + 1) ||
        !_sameInteger(value['fps'], change.fps) ||
        !_sameInteger(value['timeoutMs'], 90000) ||
        value['resizeId'] is! String ||
        !RegExp(r'^[A-Za-z0-9-]{1,80}$')
            .hasMatch(value['resizeId'] as String) ||
        nowUs > change.deadlineUs) {
      throw const FormatException('Invalid output resize acknowledgement.');
    }
    final epochs = <String, int>{};
    for (final t in change.plan.outputs) {
      final old = tracks.singleWhere((old) => old.id == t.id);
      final changed =
          t.width != old.width || t.height != old.height || change.fps != fps;
      if (changed && old.epoch == _u32) {
        throw const FormatException('Output epoch exhausted.');
      }
      epochs[t.id] = old.epoch + (changed ? 1 : 0);
    }
    if (!_requestMatches(value['tracks'], change.plan, epochs) ||
        (change.resizeId != null && change.resizeId != value['resizeId'])) {
      throw const FormatException('Altered output resize acknowledgement.');
    }
    change.resizeId = value['resizeId'] as String;
    change.epochs = epochs;
    return true;
  }

  bool acceptRestored(Map<String, dynamic> value, int nowUs) {
    final change = _change;
    if (!profile ||
        change?.resizeId == null ||
        value['sessionId'] != sessionId ||
        value['requestId'] != change!.requestId ||
        value['resizeId'] != change.resizeId ||
        !_sameInteger(value['topologyEpoch'], topology + 1)) {
      return false;
    }
    final oldEpochs = {for (final t in tracks) t.id: t.epoch};
    if (!_shape(value, [
          'type',
          'sessionId',
          'requestId',
          'resizeId',
          'topologyEpoch',
          'fps',
          'tracks',
          'reason',
        ]) ||
        value['type'] != 'stream.tracks.resize.restored' ||
        !_sameInteger(value['fps'], fps) ||
        !_restoreReasons.contains(value['reason']) ||
        !_requestMatches(value['tracks'], plan, oldEpochs) ||
        (change.restoreReason != null &&
            change.restoreReason != value['reason']) ||
        nowUs > change.deadlineUs) {
      throw const FormatException('Invalid restored output descriptor.');
    }
    // Only previous applied geometry is reusable. New correlation and topology
    // remain mandatory, followed by new IDRs and actual decoder output.
    change.plan = plan;
    change.decoderLimits = decoderLimits;
    change.fps = fps;
    change.epochs = oldEpochs;
    change.restoreReason = value['reason'] as String;
    return true;
  }

  bool rejectUnchanged(Map<String, dynamic> value) {
    final change = _change;
    if (change == null || value['requestId'] != change.requestId) return false;
    if (change.resizeId != null ||
        !_shape(value, ['type', 'requestId', 'reason', 'unchanged']) ||
        value['type'] != 'stream.tracks.resize.failed' ||
        value['unchanged'] != true ||
        !{
          'track_streaming_unavailable',
          'resize_rate_limited',
          'invalid_track_request',
          'stale_topology',
          'resize_in_progress',
          'epoch_exhausted',
        }.contains(value['reason'])) {
      throw const FormatException('Unproved unchanged output state.');
    }
    _change = null;
    ready = true; // The relay rejected before dispatch; no decoder was reset.
    return true;
  }

  /// Duplicate metadata must be byte-for-byte equivalent in meaning. A fresh
  /// topology requires a separately correlated resize; unsolicited changes fail.
  bool configure(Map<String, dynamic> info) {
    final change = _change;
    if (info['type'] != 'stream.tracks.info' ||
        info['sessionId'] != sessionId ||
        !_integer(info['topologyEpoch'], 1, _u32)) {
      throw const FormatException('Invalid output identity.');
    }
    if ((info['topologyEpoch'] as int) < topology ||
        (change != null && info['topologyEpoch'] == topology)) {
      return false;
    }
    final expectedTopology = change == null
        ? (topology == 0 ? 1 : topology)
        : topology + 1;
    final expectedPlan = change?.plan ?? plan;
    final expectedFps = change?.fps ?? fps;
    final expectedEpochs =
        change?.epochs ??
        {
          for (final t in expectedPlan.outputs)
            t.id: topology == 0
                ? 1
                : tracks.singleWhere((old) => old.id == t.id).epoch,
        };
    final correlation = change == null
        ? _lastCorrelation
        : {'requestId': change.requestId, 'resizeId': change.resizeId ?? ''};
    if (!_shape(info, [
          'type',
          'sessionId',
          'codec',
          'topologyEpoch',
          'fpsTarget',
          'bitrateTarget',
          'tracks',
          'audio',
          if (correlation != null) ...['requestId', 'resizeId'],
        ]) ||
        info['type'] != 'stream.tracks.info' ||
        info['sessionId'] != sessionId ||
        info['codec'] != 'h264' ||
        !_integer(info['topologyEpoch'], 1, _u32) ||
        !_sameInteger(info['topologyEpoch'], expectedTopology) ||
        (change != null && change.resizeId == null) ||
        (correlation != null &&
            correlation.entries.any((e) => info[e.key] != e.value)) ||
        !_sameInteger(info['fpsTarget'], expectedFps) ||
        !_integer(info['bitrateTarget'], 500000, options.bitrate) ||
        info['tracks'] is! List ||
        (info['tracks'] as List).length != expectedPlan.outputs.length) {
      throw const FormatException('Invalid output metadata.');
    }
    final sound = info['audio'];
    if (sound is! Map ||
        !_shape(sound, ['sampleRate', 'channels', 'format', 'available']) ||
        !_sameInteger(sound['sampleRate'], 48000) ||
        !_sameInteger(sound['channels'], 2) ||
        !(sound['format'] == 's16le' ||
            (audioOpus && sound['format'] == 'opus')) ||
        sound['available'] is! bool ||
        (sound['available'] == true && !options.audio)) {
      throw const FormatException('Invalid shared audio.');
    }
    final parsed = <StreamTrack>[];
    final seen = <String>{};
    int? offsetX, offsetY;
    var bits = 0, primaries = 0;
    for (final value in info['tracks'] as List) {
      if (value is! Map ||
          !_shape(value, [
            'id',
            'epoch',
            'width',
            'height',
            'sourceWidth',
            'sourceHeight',
            'left',
            'top',
            'primary',
            'bitrateTarget',
          ])) {
        throw const FormatException('Invalid output metadata shape.');
      }
      final requested = expectedPlan.outputs
          .where((t) => t.id == value['id'])
          .firstOrNull;
      if (requested == null ||
          !seen.add(requested.id) ||
          !_sameInteger(value['epoch'], expectedEpochs[requested.id]!) ||
          !(streamSizeV2
              ? value['width'] is int &&
                    value['height'] is int &&
                    videoGeometry(
                      value['width'] as int,
                      value['height'] as int,
                    ) &&
                    (value['width'] as int) <= requested.width &&
                    (value['height'] as int) <= requested.height &&
                    (change?.decoderLimits ?? decoderLimits)?.fits(
                          value['width'] as int,
                          value['height'] as int,
                          expectedFps,
                        ) ==
                        true
              : _sameInteger(value['width'], requested.width) &&
                    _sameInteger(value['height'], requested.height)) ||
          !_sameInteger(value['sourceWidth'], requested.width) ||
          !_sameInteger(value['sourceHeight'], requested.height) ||
          !(streamSizeV2
              ? value['left'] is int &&
                    value['top'] is int &&
                    videoPlacement(
                      requested.width,
                      requested.height,
                      value['left'] as int,
                      value['top'] as int,
                    )
              : _integer(value['left'], -32768, 32768 - requested.width) &&
                    _integer(value['top'], -32768, 32768 - requested.height)) ||
          value['primary'] is! bool ||
          !_integer(value['bitrateTarget'], 500000, options.bitrate)) {
        throw const FormatException('Output did not match requested viewport.');
      }
      final left = value['left'] as int, top = value['top'] as int;
      offsetX ??= left - requested.x;
      offsetY ??= top - requested.y;
      if (left - requested.x != offsetX || top - requested.y != offsetY) {
        throw const FormatException('Output arrangement mismatch.');
      }
      final bitrate = value['bitrateTarget'] as int;
      bits += bitrate;
      primaries += value['primary'] == true ? 1 : 0;
      parsed.add(
        StreamTrack(
          id: requested.id,
          epoch: expectedEpochs[requested.id]!,
          width: requested.width,
          height: requested.height,
          encodedWidth: streamSizeV2 ? value['width'] as int : null,
          encodedHeight: streamSizeV2 ? value['height'] as int : null,
          left: left,
          top: top,
          primary: value['primary'] as bool,
          bitrate: bitrate,
        ),
      );
    }
    if (bits != info['bitrateTarget'] || primaries > 1) {
      throw const FormatException('Invalid output allocation.');
    }
    parsed.sort((a, b) => a.id.compareTo(b.id));
    if (profile &&
        (parsed.first.id != '0' ||
            !parsed.first.primary ||
            parsed.first.left != 0 ||
            parsed.first.top != 0)) {
      throw const FormatException('Owned primary output required.');
    }
    if (topology != 0 && change == null) {
      if (audio != sound['available'] ||
          opusAudio != (sound['format'] == 'opus') ||
          parsed.length != tracks.length ||
          parsed.indexed.any((pair) => !pair.$2.same(tracks[pair.$1]))) {
        throw const FormatException('Unannounced topology change.');
      }
      return false;
    }
    tracks = List.unmodifiable(parsed);
    topology = expectedTopology;
    _plan = expectedPlan;
    fps = expectedFps;
    if (change?.decoderLimits != null) decoderLimits = change!.decoderLimits;
    _lastCorrelation = correlation?.cast<String, Object>();
    _change = null;
    ready = false;
    textures = const {};
    _keys.clear();
    _pts.clear();
    _videoAt.clear();
    _decodedAt.clear();
    _decodedCount.clear();
    _audioPts = null;
    audio = sound['available'] as bool;
    opusAudio = sound['format'] == 'opus';
    return true;
  }

  Map<String, dynamic> get nativeConfiguration => {
    'topologyEpoch': topology,
    'audioEnabled': audio,
    'tracks': tracks.map((t) => t.toNative()).toList(),
    if (opusAudio) 'audioFormat': 'opus',
  };

  /// session-resume-v1 for independent outputs (tracks-resume-v1): the relay
  /// restarts every output at an IDR. Each output waits for its own again, and
  /// the time away is no stall of any output.
  void resumed(int nowUs) {
    if (!configured) return;
    _keys.clear();
    for (final t in tracks) {
      _videoAt[t.id] = _decodedAt[t.id] = nowUs;
    }
  }

  void bindTextures(Map<String, int> values, int nowUs) {
    if (topology == 0 ||
        values.length != tracks.length ||
        tracks.any((t) => !values.containsKey(t.id)) ||
        values.values.any((v) => v < 0) ||
        values.values.toSet().length != values.length) {
      throw const FormatException('Output texture mismatch.');
    }
    textures = Map.unmodifiable(values);
    for (final t in tracks) {
      _videoAt[t.id] = _decodedAt[t.id] = nowUs;
      _decodedCount[t.id] = 0;
    }
  }

  /// False means a structurally valid old or not-yet-ready packet was dropped.
  bool accept(Uint8List bytes, int nowUs) {
    if (bytes.length <= 28 ||
        bytes.length > 4 * 1024 * 1024 ||
        bytes[0] != 84 ||
        bytes[1] != 86 ||
        bytes[2] != 84 ||
        bytes[3] != 49 ||
        bytes[7] != 0) {
      throw const FormatException('Invalid output packet.');
    }
    final data = ByteData.sublistView(bytes), kind = bytes[4], flags = bytes[5];
    final packetTopology = data.getUint32(8, Endian.little),
        epoch = data.getUint32(12, Endian.little);
    // Native Dart integers are signed 64-bit. Read the words separately so an
    // attacker cannot turn a high-bit unsigned timestamp into a negative int.
    final ptsHigh = data.getUint32(20, Endian.little);
    final pts = ptsHigh * 4294967296 + data.getUint32(16, Endian.little);
    final id = bytes[6].toString();
    if (packetTopology == 0 ||
        ptsHigh > 0x1fffff ||
        pts > _maxPts ||
        data.getUint32(24, Endian.little) != bytes.length - 28 ||
        (kind == 1
            ? bytes[6] > 1 || epoch == 0 || flags & ~1 != 0
            : kind == 2
            ? bytes[6] != 255 ||
                  epoch != 0 ||
                  flags != 0 ||
                  (!opusAudio && (bytes.length - 28) % 4 != 0)
            : true)) {
      throw const FormatException('Invalid output envelope.');
    }
    if (preparing && packetTopology <= topology) return false;
    if (!configured) {
      throw const FormatException('Media before output configuration.');
    }
    if (packetTopology < topology) return false;
    if (packetTopology != topology) {
      throw const FormatException('Unannounced media topology.');
    }
    if (kind == 2) {
      if (!audio) throw const FormatException('Unrequested audio.');
      if (!ready) return false;
      if (_audioPts != null && pts <= _audioPts!) {
        throw const FormatException('Audio timestamp regression.');
      }
      _audioPts = pts;
      return true;
    }
    final t = tracks.where((t) => t.id == id).firstOrNull;
    if (t == null) throw const FormatException('Unannounced video output.');
    if (epoch < t.epoch) return false;
    if (epoch != t.epoch || (_pts[id] != null && pts <= _pts[id]!)) {
      throw const FormatException('Video epoch or timestamp regression.');
    }
    if (!_keys.contains(id)) {
      if (flags & 1 == 0) return false;
      if (!_initialIdr(bytes)) {
        throw const FormatException('Invalid initial video frame.');
      }
      _keys.add(id);
    }
    _pts[id] = pts;
    _videoAt[id] = nowUs;
    return true;
  }

  /// cursor-tracks-v1: the record carried by one TVT1 cursor message (kind 3,
  /// connector 255, track epoch 0) of the current topology; null for an older
  /// topology or one replaced by a resize in progress. Throws for a malformed
  /// envelope, a record before the outputs exist or an unannounced topology.
  Uint8List? cursorRecord(Uint8List bytes) {
    if (bytes.length <= 28 ||
        bytes.length > 4 * 1024 * 1024 ||
        bytes[0] != 84 ||
        bytes[1] != 86 ||
        bytes[2] != 84 ||
        bytes[3] != 49 ||
        bytes[4] != 3 ||
        bytes[5] != 0 ||
        bytes[6] != 255 ||
        bytes[7] != 0) {
      throw const FormatException('Invalid cursor envelope.');
    }
    final data = ByteData.sublistView(bytes);
    final packetTopology = data.getUint32(8, Endian.little);
    final ptsHigh = data.getUint32(20, Endian.little);
    if (packetTopology == 0 ||
        data.getUint32(12, Endian.little) != 0 ||
        ptsHigh > 0x1fffff ||
        data.getUint32(24, Endian.little) != bytes.length - 28) {
      throw const FormatException('Invalid cursor envelope.');
    }
    if (preparing && packetTopology <= topology) return null;
    if (!configured) {
      throw const FormatException('Cursor before output configuration.');
    }
    if (packetTopology < topology) return null;
    if (packetTopology != topology) {
      throw const FormatException('Unannounced cursor topology.');
    }
    return Uint8List.sublistView(bytes, 28);
  }

  Map<String, Object>? acknowledge(Map<String, dynamic> stats, int nowUs) {
    if (preparing) return null;
    if (!configured ||
        stats['mediaProtocol'] != 'tracks-v1' ||
        !_sameInteger(stats['topologyEpoch'], topology) ||
        stats['tracks'] is! List ||
        (stats['tracks'] as List).length != tracks.length ||
        !_integer(stats['error'], -0x80000000, 0) ||
        stats['error'] != 0 ||
        stats['tracksReady'] is! bool) {
      throw const FormatException('Invalid output decoder state.');
    }
    final seen = <String>{};
    var decoded = true;
    for (final value in stats['tracks'] as List) {
      if (value is! Map) {
        throw const FormatException('Invalid output decoder state.');
      }
      final t = tracks.where((t) => t.id == value['id']).firstOrNull;
      if (t == null ||
          !seen.add(t.id) ||
          !_sameInteger(value['epoch'], t.epoch) ||
          !_sameInteger(value['width'], t.width) ||
          !_sameInteger(value['height'], t.height) ||
          !_sameInteger(value['textureId'], textures[t.id]!) ||
          !_integer(value['decodedFrames'], 0, _maxPts)) {
        throw const FormatException('Stale output decoder state.');
      }
      final count = value['decodedFrames'] as int;
      if (count < _decodedCount[t.id]!) {
        throw const FormatException('Decoder count regressed.');
      }
      if (count > _decodedCount[t.id]!) _decodedAt[t.id] = nowUs;
      _decodedCount[t.id] = count;
      decoded &= count > 0 && _keys.contains(t.id);
    }
    if (ready || !decoded || stats['tracksReady'] != true) return null;
    ready = true;
    return {
      'type': 'stream.tracks.ready',
      'topologyEpoch': topology,
      'tracks': tracks.map((t) => {'id': t.id, 'epoch': t.epoch}).toList(),
    };
  }

  bool videoStalled(int nowUs) =>
      !preparing &&
      configured &&
      tracks.any((t) => nowUs - _videoAt[t.id]! > 15000000);

  /// Native admission already checked the encrypted frame and output binding.
  /// UDP arrivals refresh traffic/watchdogs, never key or decoder readiness.
  bool observeUdp(String id, int epoch, int nowUs) {
    if (!configured ||
        !ready ||
        preparing ||
        !tracks.any((t) => t.id == id && t.epoch == epoch)) {
      return false;
    }
    _videoAt[id] = nowUs;
    return true;
  }

  bool decoderStalled(int nowUs) =>
      !preparing &&
      configured &&
      tracks.any((t) => nowUs - _decodedAt[t.id]! > 15000000);
  Map<String, dynamic>? input(Map<String, dynamic> event) {
    if (event['type'] == 'input.reset') return event;
    if (!ready || event['type'] != 'input') return null;
    if (event['topologyEpoch'] != null &&
        !_sameInteger(event['topologyEpoch'], topology)) {
      return null;
    }
    if (event['event'] == 'key') return {...event, 'topologyEpoch': topology};
    // 게임 마우스 (relative-mouse-tracks-v1): movement, and buttons and wheel
    // at the remote cursor, belong to no output and carry no position.
    if (event['event'] == 'moveRelative' ||
        (['button', 'wheel'].contains(event['event']) &&
            !event.containsKey('x') &&
            !event.containsKey('y'))) {
      return event.containsKey('track') || event.containsKey('epoch')
          ? null
          : {...event, 'topologyEpoch': topology};
    }
    if (!['move', 'button', 'wheel'].contains(event['event'])) return null;
    final id = event['track'] ?? primary.id;
    final t = tracks.where((t) => t.id == id).firstOrNull;
    if (t == null ||
        (event['epoch'] != null && !_sameInteger(event['epoch'], t.epoch))) {
      return null;
    }
    return {
      ...event,
      'topologyEpoch': topology,
      'track': t.id,
      'epoch': t.epoch,
    };
  }
}

bool _requestMatches(Object? value, TrackPlan plan, Map<String, int> epochs) {
  if (value is! List || value.length != plan.outputs.length) return false;
  final seen = <String>{};
  for (final item in value) {
    if (item is! Map ||
        !_shape(item, ['id', 'width', 'height', 'x', 'y', 'epoch'])) {
      return false;
    }
    final t = plan.outputs.where((t) => t.id == item['id']).firstOrNull;
    if (t == null ||
        !seen.add(t.id) ||
        !_sameInteger(item['width'], t.width) ||
        !_sameInteger(item['height'], t.height) ||
        !_sameInteger(item['x'], t.x) ||
        !_sameInteger(item['y'], t.y) ||
        !_sameInteger(item['epoch'], epochs[t.id]!)) {
      return false;
    }
  }
  return true;
}

bool _initialIdr(Uint8List bytes) {
  var mask = 0, first = true;
  for (var n = 28; n + 3 < bytes.length;) {
    var prefix = 0;
    if (bytes[n] == 0 && bytes[n + 1] == 0 && bytes[n + 2] == 1) {
      prefix = 3;
    } else if (n + 4 < bytes.length &&
        bytes[n] == 0 &&
        bytes[n + 1] == 0 &&
        bytes[n + 2] == 0 &&
        bytes[n + 3] == 1) {
      prefix = 4;
    }
    if (prefix == 0) {
      if (first && bytes[n] != 0) return false;
      n++;
      continue;
    }
    first = false;
    final header = bytes[n + prefix];
    if (header & 128 != 0) return false;
    switch (header & 31) {
      case 5:
        mask |= 1;
      case 7:
        mask |= 2;
      case 8:
        mask |= 4;
    }
    n += prefix + 1;
  }
  return mask == 7;
}
