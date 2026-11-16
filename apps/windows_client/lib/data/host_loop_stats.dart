/// Where the Host's capture loop spent time outside its measured stages, from
/// each Host `stream.stats` (about one a second): the `stall` member's skipped
/// ticks by reason, its pacing lateness, the unmeasured tail of a tick and the
/// worker's longest service pipe write, plus the capture backend and whether
/// the WGC update interval took effect. Hosts before these fields send none;
/// nothing is then reported. Fixed numbers only, never pixels, input or text.
library;

/// Host `stall` members summed over the readings of a window (counts and ms).
const _summed = [
  'backgroundTicks', 'backgroundMs', 'secureHoldTicks', 'secureHoldMs',
  'secureNoticeTicks', 'secureNoticeMs', 'secureHelperTicks', 'secureHelperMs',
  'follows', 'followMs',
];

/// Host `stall` members whose largest reading a window keeps (us).
const _largest = ['overshootMaxUs', 'tailMaxUs', 'pipeWriteMaxUs'];

/// The row field of a Host `stall` member: `follows` -> `hostFollows`.
String hostLoopField(String key) => 'host${key[0].toUpperCase()}${key.substring(1)}';

/// `stream.stats` captureBackend as a fixed code.
const hostCaptureBackends = {'gdi': 1, 'wgc-d3d11': 2, 'wgc-warp': 3};

/// The fields one `streamStats` row may carry from [HostLoopTally]:
///   hostBackgroundTicks/Ms     ticks that only waited while this PC's view was hidden
///   hostSecureHoldTicks/Ms     UAC prompt: the helper was asked for, nothing encoded yet
///   hostSecureNoticeTicks/Ms   UAC prompt: the notice picture instead of the capture
///   hostSecureHelperTicks/Ms   UAC prompt: the SYSTEM helper's frames instead of the capture
///   hostFollows/hostFollowMs   capture rebuilds after a display mode change
///   hostOvershootMaxUs         the longest a pacing wait returned after its target
///   hostTailMaxUs              the longest unmeasured time from encode end to the pacing wait
///   hostPipeWriteMaxUs         the longest worker -> service pipe write of one packet
///   hostCaptureBackend         1 GDI, 2 WGC on a GPU, 3 WGC on WARP (the latest reading)
///   hostCaptureMinInterval     1: every WGC session took the 1 ms MinUpdateInterval, 0: not (WGC only)
/// The `stall` members are also the Host fields of a `streamStall` row (summed or
/// the largest over the windows covering the stall).
const hostLoopFields = <String>{
  ...hostLoopStallFields,
  'hostCaptureBackend',
  'hostCaptureMinInterval',
};

/// The row fields of the Host `stall` members ([hostLoopField] of each).
const hostLoopStallFields = <String>{
  'hostBackgroundTicks', 'hostBackgroundMs', 'hostSecureHoldTicks',
  'hostSecureHoldMs', 'hostSecureNoticeTicks', 'hostSecureNoticeMs',
  'hostSecureHelperTicks', 'hostSecureHelperMs', 'hostFollows', 'hostFollowMs',
  'hostOvershootMaxUs', 'hostTailMaxUs', 'hostPipeWriteMaxUs',
};

/// Host `stall` keys a stall row sums / takes the largest of (stream_stall.dart).
const hostLoopSummed = _summed;
const hostLoopLargest = _largest;

int? _integer(Object? value) {
  if (value is int && value >= 0 && value <= 0xffffffff) return value;
  return null;
}

/// Accumulates readings (one per Host `stream.stats`, or the per-second
/// takes of a session) until [take]: sums, maxima and the latest backend.
class HostLoopTally {
  final _values = <String, int>{};

  /// One Host `stream.stats` as row fields; empty from Hosts without them.
  static Map<String, int> reading(Map<String, dynamic> event) {
    final values = <String, int>{};
    final stall = event['stall'];
    if (stall is Map) {
      for (final key in [..._summed, ..._largest]) {
        final value = _integer(stall[key]);
        if (value != null) values[hostLoopField(key)] = value;
      }
    }
    final backend = hostCaptureBackends[event['captureBackend']];
    if (backend != null) values['hostCaptureBackend'] = backend;
    final interval = event['captureMinInterval'];
    if (backend != null && backend != 1 && interval is bool) {
      values['hostCaptureMinInterval'] = interval ? 1 : 0;
    }
    return values;
  }

  void add(Map<String, int>? reading) {
    if (reading == null) return;
    for (final key in _summed) {
      final field = hostLoopField(key), value = reading[field];
      if (value != null) {
        _values[field] = ((_values[field] ?? 0) + value).clamp(0, 0xffffffff);
      }
    }
    for (final key in _largest) {
      final field = hostLoopField(key), value = reading[field];
      if (value != null && value > (_values[field] ?? -1)) _values[field] = value;
    }
    for (final field in ['hostCaptureBackend', 'hostCaptureMinInterval']) {
      final value = reading[field];
      if (value != null) _values[field] = value;
    }
    // A GDI reading after a WGC one: the interval no longer applies.
    if (reading['hostCaptureBackend'] == 1) _values.remove('hostCaptureMinInterval');
  }

  /// The values since the last take (null when there were none); resets.
  Map<String, int>? take() {
    if (_values.isEmpty) return null;
    final values = Map.of(_values);
    _values.clear();
    return values;
  }
}
