/// Stall forensics (stream-stall-v1): one diagnostic event per displayed-video
/// gap of 250 ms or more, with fixed numbers that say where the time went. The
/// native engine records the gap (RemoteEngine kStall); this adds the arrival
/// gap, the UDP leg's counters across it and the Host's stall fields of the
/// statistics windows that cover it. Holds no pixels, input or text.
///
/// An idle screen is no stall: the Host sends nothing but a refresh every
/// second while the picture stands still, so every idle second is a gap (about
/// half of all windows in the field). A gap counts only when video was late on
/// its way here, lost, or late on the Host while the picture changed (see
/// [StallWatch.idle]); rows are capped per session and per minute, and the
/// rest are counted (`stallDropped`, also as one row at the session's end).
library;

/// The broker acknowledges this before a client may send `streamStall` rows;
/// released brokers reject a batch with an unknown event.
const streamStallProtocol = 'stream-stall-v1';

/// Integer fields of one `streamStall` row.
const streamStallFields = <String>{
  // The engine's record: 1 no frame was published (decode side), 2 a frame
  // waited for Flutter's take (present side); the gap; the frame that ended
  // it was a key frame (1); decode queue now and its deepest during the gap;
  // packets the queue took and refused as full; decode calls and the longest
  // one (us); frames waiting for Flutter; frames the pacing skipped; and, for
  // kind 2, how long the frame waited for its take.
  'stallKind', 'stallMs', 'stallKey', 'stallQueue', 'stallQueueMax',
  'stallOffers', 'stallFull', 'stallDecodes', 'stallDecodeMaxUs',
  'stallPending', 'stallSkipped', 'stallWaitMs',
  // Kind 1: how far apart the Host stamped the two frames around the gap (ms).
  'stallPtsGapMs',
  // The longest gap between two video arrivals that overlaps the stall.
  'stallArrivalMs',
  // UDP leg, during the stall: datagrams given up, KEYFRAME requests, video
  // that waited for a full decode queue, skips to a key frame, NACKs, returns
  // to WebSocket; the path now (0 WebSocket, 1 UDP).
  'stallUdpLost', 'stallUdpKeys', 'stallUdpWaits', 'stallUdpSkips',
  'stallUdpNacks', 'stallUdpFallbacks', 'udpActivePath',
  // Host statistics windows covering the stall: their count; the longest gap
  // between encoder outputs and between captured frames; the loop's wait,
  // capture, convert and encode of the tick that ended the encode gap (ms);
  // IDRs, key frame requests taken and bitrate changes (summed); the lowest
  // target applied (kbps); UDP sender: IDRs its pacer asked for, client
  // KEYFRAME requests it honoured and repair datagrams it refused; the
  // longest a changed picture waited on the Host before its encode (ms).
  'hostWindows', 'hostEncodeGapMs', 'hostCaptureGapMs', 'hostWaitMs',
  'hostCaptureMs', 'hostConvertMs', 'hostEncodeMs', 'hostKeyFrames',
  'hostKeyRequests', 'hostBitrateSteps', 'hostTargetMinKbps',
  'hostUdpBacklogKeys', 'hostUdpClientKeys', 'hostUdpRefused',
  'hostChangeLateMs',
  // Session state at the end: round trip (ms), decoder busy (per mille) and
  // stalls not sent since the last row (spacing, per-minute and per-session
  // caps). A row with only this field is the session's closing count.
  'rttMs', 'decoderBusy', 'stallDropped',
};

const _hostMax = [
  'encodeGapMs', 'captureGapMs', 'waitMs', 'captureMs', 'convertMs', 'encodeMs',
  'changeLateMs',
];

/// A display gap this long or longer is a stall (RemoteEngine kStall).
const _stallMs = 250;

/// The Host refreshes a still picture this often (FrameEncodePolicy).
const _hostRefreshMs = 1000;

/// Any of these during the stall means video was lost, refused or replaced
/// by a key frame on its way: the gap is no idle one.
const _lossFields = [
  'stallFull', 'stallUdpLost', 'stallUdpKeys', 'stallUdpSkips',
  'stallUdpFallbacks', 'hostKeyRequests', 'hostUdpBacklogKeys',
  'hostUdpClientKeys',
];
const _hostSum = ['keyFrames', 'keyRequests', 'bitrateSteps'];
const _hostCounters = {
  'udpBacklogKeys': 'hostUdpBacklogKeys',
  'udpClientKeys': 'hostUdpClientKeys',
  'udpRefused': 'hostUdpRefused',
};
const _udpCounters = {
  'udpGaps': 'stallUdpLost',
  'udpKeyRequests': 'stallUdpKeys',
  'udpQueueWaits': 'stallUdpWaits',
  'udpQueueSkips': 'stallUdpSkips',
  'udpNacks': 'stallUdpNacks',
  'udpFallbacks': 'stallUdpFallbacks',
};

class _Pending {
  _Pending(this.values, this.startUs, this.endUs);
  final Map<String, int> values;
  final int startUs, endUs;
}

/// Pure and clock-injected: the session feeds it and takes ready events.
class StallWatch {
  StallWatch({
    this.hostCoverUs = 1200000,
    this.spacingUs = 2000000,
    this.arrivalGapUs = 100000,
    this.sessionRows = 30,
    this.minuteRows = 6,
  });

  /// A stall waits this long after it ended for the Host window covering it.
  final int hostCoverUs;

  /// At most one event per this long; the rest are counted in the next.
  final int spacingUs;

  /// Arrival gaps shorter than this are not kept.
  final int arrivalGapUs;

  /// At most this many events per session (this watch) and per 60 s; the
  /// rest are counted in the next event or the closing [summary].
  final int sessionRows, minuteRows;

  final _serials = <int, int>{};
  final _pending = <_Pending>[];
  final _host = <(int, Map<String, int>)>[];
  final _udp = <(int, Map<String, int>)>[];
  final _arrivalGaps = <(int, int)>[];
  final _sentUs = <int>[];
  int _lastArrivalUs = 0, _lastEmitUs = 0, _dropped = 0, _sent = 0;
  int _windowStalls = 0, _windowMaxMs = 0;

  /// A video message arrived (session clock).
  void arrival(int arrivedUs) {
    if (_lastArrivalUs != 0 && arrivedUs - _lastArrivalUs >= arrivalGapUs) {
      _arrivalGaps.add((arrivedUs, arrivedUs - _lastArrivalUs));
      if (_arrivalGaps.length > 32) _arrivalGaps.removeAt(0);
    }
    if (arrivedUs > _lastArrivalUs) _lastArrivalUs = arrivedUs;
  }

  /// A Host `stream.stats` arrived: its `stall` object and UDP counters.
  void host(Map<String, dynamic> event, int nowUs) {
    final values = <String, int>{};
    final stall = event['stall'];
    if (stall is Map) {
      for (final key in [..._hostMax, ..._hostSum, 'targetMinKbps']) {
        final value = _integer(stall[key]);
        if (value != null) values[key] = value;
      }
    }
    for (final key in _hostCounters.keys) {
      final value = _integer(event[key]);
      if (value != null) values[key] = value;
    }
    _host.add((nowUs, values));
    while (_host.length > 1 && nowUs - _host.first.$1 > 10000000) {
      _host.removeAt(0);
    }
  }

  /// One engine reading: its `stalls` records and the UDP leg's cumulative
  /// report at the same moment. New records become pending events.
  void engine(Object? records, Map<String, int>? udp, int nowUs) {
    _udp.add((nowUs, Map.of(udp ?? const {})));
    while (_udp.length > 1 && nowUs - _udp.first.$1 > 10000000) {
      _udp.removeAt(0);
    }
    if (records is! List) return;
    final parsed = <List<int>>[];
    final newest = <int, int>{};
    for (final record in records) {
      // 15: with ptsGapMs (engines before it had 14).
      if (record is! List || record.length < 14 || record.length > 15) continue;
      final r = [for (final value in record) value is int ? value : -1];
      if (r.any((value) => value < 0) || (r[0] != 1 && r[0] != 2)) continue;
      parsed.add(r);
      if (r[1] > (newest[r[0]] ?? 0)) newest[r[0]] = r[1];
    }
    // An engine that restarted without [restart] counts from 1 again.
    newest.forEach((kind, serial) {
      if (serial < (_serials[kind] ?? 0)) _serials[kind] = 0;
    });
    final seen = Map.of(_serials);
    _serials.addAll(newest);
    for (final r in parsed) {
      final kind = r[0];
      if (r[1] <= (seen[kind] ?? 0)) continue;
      final endUs = nowUs - r[3] * 1000, startUs = endUs - r[2] * 1000;
      _pending.add(
        _Pending(
          {
            'stallKind': kind,
            'stallMs': r[2],
            'stallKey': r[4],
            'stallQueue': r[5],
            'stallQueueMax': r[6],
            'stallOffers': r[7],
            'stallFull': r[8],
            'stallDecodes': r[9],
            'stallDecodeMaxUs': r[10],
            'stallPending': r[11],
            'stallSkipped': r[12],
            'stallWaitMs': r[13],
            if (kind == 1 && r.length == 15) 'stallPtsGapMs': r[14],
          },
          startUs,
          endUs,
        ),
      );
    }
    if (_pending.length > 8) {
      _dropped += _pending.length - 8;
      _pending.removeRange(0, _pending.length - 8);
    }
  }

  /// A new engine (picture change): its records count from 1, and the gap of
  /// the change itself is no stall.
  void restart() {
    _serials.clear();
    _lastArrivalUs = 0;
  }

  /// Stalls (idle gaps excluded) since the last call, and the longest (ms):
  /// the window's fields. A stall counts once its Host coverage arrived.
  (int, int) takeWindow() {
    final value = (_windowStalls, _windowMaxMs);
    _windowStalls = _windowMaxMs = 0;
    return value;
  }

  /// An idle gap: the Host itself left the time between the two frames (at
  /// least a stall, at most 1.25 refresh intervals: a still picture's refresh),
  /// the way here added less than a stall to it, nothing was lost, refused or
  /// replaced by a key frame, and the Host's spacing is not a changed picture
  /// that waited on the Host (a covering window's changeLate of a stall or
  /// more that leaves less than a stall of the spacing unexplained). Kind 2
  /// (Flutter did not take a waiting frame) and a record without the Host's
  /// spacing always count.
  static bool idle(Map<String, int> values) {
    final spacing = values['stallPtsGapMs'], gap = values['stallMs'];
    if (values['stallKind'] != 1 || spacing == null || gap == null) return false;
    if (spacing < _stallMs || spacing * 4 > _hostRefreshMs * 5) return false;
    if (gap - spacing >= _stallMs) return false;
    if (_lossFields.any((key) => (values[key] ?? 0) > 0)) return false;
    // shortcut: a window reports only its longest wait, so a typing pause as
    // long as a Host stall in the same window counts too (rare, one row).
    final late = values['hostChangeLateMs'] ?? 0;
    return late < _stallMs || spacing - late >= _stallMs;
  }

  /// Events whose Host coverage arrived or timed out.
  List<Map<String, int>> take(int nowUs, {int? rttMs, int? decoderBusy, int? udpActivePath}) {
    final ready = <Map<String, int>>[];
    while (_pending.isNotEmpty && nowUs - _pending.first.endUs >= hostCoverUs) {
      final stall = _pending.removeAt(0);
      final values = {...stall.values};
      var arrival = 0;
      for (final (endUs, gapUs) in _arrivalGaps) {
        if (endUs > stall.startUs && endUs - gapUs < stall.endUs) {
          arrival = arrival > gapUs ~/ 1000 ? arrival : gapUs ~/ 1000;
        }
      }
      values['stallArrivalMs'] = arrival;
      _addUdp(values, stall);
      _addHost(values, stall);
      if (idle(values)) continue;
      _windowStalls++;
      if (values['stallMs']! > _windowMaxMs) _windowMaxMs = values['stallMs']!;
      _sentUs.removeWhere((atUs) => nowUs - atUs >= 60000000);
      if (_sent >= sessionRows ||
          _sentUs.length >= minuteRows ||
          (_lastEmitUs != 0 && nowUs - _lastEmitUs < spacingUs)) {
        _dropped++;
        continue;
      }
      _lastEmitUs = nowUs;
      _sentUs.add(nowUs);
      _sent++;
      if (rttMs != null && rttMs >= 0) values['rttMs'] = rttMs;
      if (decoderBusy != null && decoderBusy >= 0) {
        values['decoderBusy'] = decoderBusy;
      }
      if (udpActivePath != null) values['udpActivePath'] = udpActivePath;
      values['stallDropped'] = _dropped;
      _dropped = 0;
      ready.add({
        for (final entry in values.entries)
          if (entry.value >= 0 && entry.value <= 0xffffffff) entry.key: entry.value,
      });
    }
    return ready;
  }

  /// At the session's end: the stalls no row carried (caps and spacing since
  /// the last row) as one row of `stallDropped`; null when there are none.
  Map<String, int>? summary() {
    if (_dropped == 0) return null;
    final value = {'stallDropped': _dropped};
    _dropped = 0;
    return value;
  }

  void _addUdp(Map<String, int> values, _Pending stall) {
    if (_udp.isEmpty) return;
    // Counters before the stall began, and the first reading after it ended
    // (not the latest: loss a second later does not belong to this gap).
    var base = _udp.first.$2;
    for (final (atUs, report) in _udp) {
      if (atUs <= stall.startUs) base = report;
    }
    final now = _udp.firstWhere((e) => e.$1 >= stall.endUs, orElse: () => _udp.last).$2;
    for (final MapEntry(:key, :value) in _udpCounters.entries) {
      final a = base[key], b = now[key];
      if (b != null) values[value] = a != null && b >= a ? b - a : b;
    }
  }

  void _addHost(Map<String, int> values, _Pending stall) {
    // A Host window covers the stall when it ended after the stall began and
    // began before it ended (each window is about 1 s, reported on its end).
    final covering = [
      for (final (atUs, host) in _host)
        if (atUs > stall.startUs && atUs - 1100000 < stall.endUs) host,
    ];
    values['hostWindows'] = covering.length;
    if (covering.isEmpty) return;
    for (final key in _hostMax) {
      final found = [for (final host in covering) if (host[key] != null) host[key]!];
      if (found.isNotEmpty) {
        values['host${key[0].toUpperCase()}${key.substring(1)}'] =
            found.reduce((a, b) => a > b ? a : b);
      }
    }
    for (final key in _hostSum) {
      final found = [for (final host in covering) if (host[key] != null) host[key]!];
      if (found.isNotEmpty) {
        values['host${key[0].toUpperCase()}${key.substring(1)}'] =
            found.reduce((a, b) => a + b);
      }
    }
    final targets = [for (final host in covering) if (host['targetMinKbps'] != null) host['targetMinKbps']!];
    if (targets.isNotEmpty) {
      values['hostTargetMinKbps'] = targets.reduce((a, b) => a < b ? a : b);
    }
    // Cumulative Host counters: the last covering reading minus the last one
    // before the stall began.
    Map<String, int>? before;
    for (final (atUs, host) in _host) {
      if (atUs <= stall.startUs) before = host;
    }
    for (final MapEntry(:key, :value) in _hostCounters.entries) {
      final b = covering.last[key];
      if (b == null) continue;
      final a = before?[key];
      values[value] = a != null && b >= a ? b - a : 0;
    }
  }

  static int? _integer(Object? value) {
    if (value is int && value >= 0 && value <= 0xffffffff) return value;
    if (value is double && value.isFinite && value >= 0 && value <= 0xffffffff) {
      return value.round();
    }
    return null;
  }
}
