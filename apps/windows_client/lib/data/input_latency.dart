/// Negotiated ToTBSeamless input latency measurement (input-latency-v1; see
/// docs/remote-session-protocol.md "Input latency measurement").
///
/// Only a connection whose broker response granted the marker samples input.
/// A sample adds one `sample` number to an ordinary input message; ordering,
/// pointer coalescing and focus/input ownership are unchanged. Reports carry
/// sequence numbers, durations and a video PTS only, never input content.
library;

import 'dart:math' show max;

const inputLatencyProtocol = 'input-latency-v1';

/// p50/p95 of a bounded recent window, in milliseconds.
class LatencyPercentiles {
  const LatencyPercentiles({
    required this.p50Ms,
    required this.p95Ms,
    required this.samples,
  });
  final double p50Ms, p95Ms;
  final int samples;
}

/// What a connection measured recently. `supported` is false when the broker
/// did not grant the protocol (released broker, host or independent outputs).
class InputLatencySummary {
  const InputLatencySummary({
    required this.supported,
    this.frameSupported = false,
    this.toHost,
    this.toFrame,
    this.hostApply,
    this.roundTrip,
    this.framePush,
    this.frameDecode,
    this.transfer,
    this.frameBytes,
    this.downlink,
    this.failedInjections = 0,
  });
  static const unsupported = InputLatencySummary(supported: false);
  final bool supported;

  /// The local media engine reports when a tagged frame reached the texture.
  final bool frameSupported;

  /// ESTIMATE: client send -> host SendInput return, computed per sample as
  /// (round trip - host hold) / 2 + host receive -> injection.
  final LatencyPercentiles? toHost;

  /// Client send -> the first changed frame the host captured after injecting
  /// that input was decoded and handed to the texture. A proxy: it excludes
  /// compositor/monitor scan-out and includes up to one platform-channel reply.
  /// Only inputs that can change the picture by themselves are measured (key
  /// presses, buttons, wheel steps, drag moves); newer hosts also drop
  /// a sample when an unsampled click or committed text follows it before the
  /// change, which could otherwise be that later input's.
  final LatencyPercentiles? toFrame;

  /// MEASURED on the host clock: host receive -> injection call returned.
  final LatencyPercentiles? hostApply;

  /// MEASURED on this PC's clock: input sent -> its acknowledgement received.
  final LatencyPercentiles? roundTrip;

  /// MEASURED on this PC's clock, the client's share of [toFrame]: the tagged
  /// packet's arrival -> the native engine accepted it (platform channel
  /// push), and accepted -> its frame was handed to the texture (decode).
  final LatencyPercentiles? framePush, frameDecode;

  /// MEASURED on this PC's clock: the tag's arrival -> its video packet's
  /// arrival in the receive loop. The host writes the two back to back, so
  /// this is the time the packet's bytes after the first took (relay store
  /// and forward, the slower link, TCP send windows). [frameBytes] holds that
  /// packet's size in bytes (the fields named "Ms" carry bytes there).
  final LatencyPercentiles? transfer, frameBytes;

  /// Per input with a frame-timing-v1 tag (absent from older brokers and
  /// Hosts): the host's write of the tag -> its video packet's arrival here,
  /// = (arrival - send) - the uplink ESTIMATE of [toHost] - the host's own
  /// durations (injection -> tag write). Network and relay both ways of the
  /// tag and packet bytes plus this PC's receive loop; computed per input, so
  /// unlike a difference of percentiles it adds up.
  final LatencyPercentiles? downlink;

  /// Acknowledged samples whose injection the host reported as not applied.
  final int failedInjections;
}

/// Nearest-rank percentile of non-empty [values] (sorted in place).
double latencyPercentile(List<double> values, double fraction) {
  values.sort();
  final rank = (fraction * values.length).ceil() - 1;
  return values[rank.clamp(0, values.length - 1)];
}

class _Window {
  _Window(this.capacity);
  final int capacity;
  final _values = <(double, int)>[];
  void add(double ms, int atUs) {
    _values.add((ms, atUs));
    if (_values.length > capacity) _values.removeAt(0);
  }

  LatencyPercentiles? summary(int nowUs, int maxAgeUs) {
    final recent = [
      for (final (ms, at) in _values)
        if (nowUs - at <= maxAgeUs) ms,
    ];
    if (recent.isEmpty) return null;
    return LatencyPercentiles(
      p50Ms: latencyPercentile(List.of(recent), .5),
      p95Ms: latencyPercentile(List.of(recent), .95),
      samples: recent.length,
    );
  }

  void clear() => _values.clear();
}

class _Sample {
  _Sample(this.sentUs);
  final int sentUs;
  bool acknowledged = false;

  /// This sample's send -> host injection ESTIMATE (us), once acknowledged.
  int? toHostUs;
}

class _Watched {
  _Watched(this.sentUs, this.atUs);
  final int sentUs, atUs;
  int? pushedUs;
}

/// Client half of input-latency-v1 for one connection. All times are
/// microseconds of this PC's monotonic session clock; the host's durations are
/// its own clock and are only ever subtracted from durations, never from
/// timestamps of this PC.
class InputLatencyMeter {
  InputLatencyMeter({this.window = 64, this.maxAgeUs = 30000000});
  final int window, maxAgeUs;

  /// At most 10 sampled inputs per second, plus the release of each sampled
  /// press (at most 20 per second, the relay's bound); moves at most 4 per
  /// second.
  static const discreteIntervalUs = 100000, moveIntervalUs = 250000;
  static const outstandingLimit = 16,
      expiryUs = 5000000,
      maxSample = 2147483647;
  static const maxHostUs = 10000000;

  /// Shift, Ctrl, Alt and Windows (the paste gate's modifiers). Pressing one,
  /// like releasing any key, shows nothing by itself: such inputs are not
  /// sampled. A released host credited them with the next unrelated change or
  /// with a later unsampled key's change, reporting the gap as latency.
  static const modifierKeys = {
    0x10,
    0x11,
    0x12,
    0xa0,
    0xa1,
    0xa2,
    0xa3,
    0xa4,
    0xa5,
    0x5b,
    0x5c,
  };

  int _next = 0;
  int? _lastSampleUs, _lastMoveUs;
  final _samples = <int, _Sample>{};

  /// Buttons whose press was sampled and is still down.
  final _sampledPresses = <Object?>{};
  (int, int, int)? _tag; // sample, pts, sentUs
  int _tagAtUs = 0;
  final _watched = <int, _Watched>{}; // pts -> sample timing
  late final _toHost = _Window(window), _toFrame = _Window(window);
  late final _hostApply = _Window(window), _roundTrip = _Window(window);
  late final _framePush = _Window(window), _frameDecode = _Window(window);
  late final _transfer = _Window(window), _frameBytes = _Window(window);
  late final _downlink = _Window(window);
  int _failed = 0;

  void _expire(int nowUs) {
    _samples.removeWhere((_, value) => nowUs - value.sentUs > expiryUs);
    _watched.removeWhere((_, value) => nowUs - value.atUs > expiryUs);
    final tag = _tag;
    if (tag != null && nowUs - tag.$3 > expiryUs) _tag = null;
  }

  /// The sample number to attach to this outgoing message, or null to send it
  /// unchanged. Pointer buttons, wheel steps, presses of non-modifier keys and
  /// (less often) moves qualify. The release of a sampled press is sampled
  /// regardless of the 100 ms spacing: a click's result follows its release,
  /// and the host then credits that change to the release, not to the press.
  int? sample(Map<String, dynamic> message, int nowUs) {
    if (message['type'] != 'input' || _next >= maxSample) return null;
    final kind = message['event'];
    final move = kind == 'move' || kind == 'moveRelative';
    if (!move && !const {'button', 'key', 'wheel'}.contains(kind)) return null;
    if (kind == 'key' &&
        (message['down'] != true || modifierKeys.contains(message['vk']))) {
      return null;
    }
    final button = kind == 'button' ? message['button'] : null;
    final pressed = kind == 'button' && message['down'] == true;
    // A new press forgets an older one of the same button; only a release of
    // a press that was itself sampled skips the spacing.
    final release =
        kind == 'button' && _sampledPresses.remove(button) && !pressed;
    final last = _lastSampleUs;
    if (!release && last != null && nowUs - last < discreteIntervalUs) {
      return null;
    }
    final lastMove = _lastMoveUs;
    if (move && lastMove != null && nowUs - lastMove < moveIntervalUs) {
      return null;
    }
    _expire(nowUs);
    if (_samples.values.where((value) => !value.acknowledged).length >=
        outstandingLimit) {
      return null;
    }
    final sample = ++_next;
    _samples[sample] = _Sample(nowUs);
    while (_samples.length > outstandingLimit * 2) {
      _samples.remove(_samples.keys.first);
    }
    _lastSampleUs = nowUs;
    if (move) _lastMoveUs = nowUs;
    if (pressed) _sampledPresses.add(button);
    return sample;
  }

  static bool _integer(Object? value, int minimum, int maximum) =>
      value is int && value >= minimum && value <= maximum;

  /// Host `input.ack`. Returns whether it measured a sample of this meter.
  bool acknowledged(Map<String, dynamic> ack, int nowUs) {
    final sample = ack['sample'], ok = ack['ok'];
    final inject = ack['injectUs'], hold = ack['holdUs'];
    if (!_integer(sample, 1, maxSample) ||
        ok is! bool ||
        !_integer(inject, 0, maxHostUs) ||
        !_integer(hold, inject as int, maxHostUs)) {
      return false;
    }
    final entry = _samples[sample];
    if (entry == null || entry.acknowledged) return false;
    entry.acknowledged = true;
    final roundTrip = nowUs - entry.sentUs;
    if (roundTrip < 0) return false;
    _roundTrip.add(roundTrip / 1000, nowUs);
    if (!ok) {
      _failed++;
      return true;
    }
    final oneWay = (roundTrip - (hold as int)).clamp(0, roundTrip) / 2;
    _hostApply.add(inject / 1000, nowUs);
    _toHost.add((oneWay + inject) / 1000, nowUs);
    entry.toHostUs = (oneWay + inject).round();
    return true;
  }

  /// Host `input.frame`: the next video packet with this PTS shows the input.
  /// frame-timing-v1 tags also carry the host's durations for this input
  /// (captureUs + encodeUs + queueUs: injection -> this tag's write).
  void tagged(Map<String, dynamic> tag, int nowUs) {
    final sample = tag['sample'], pts = tag['pts'];
    if (!_integer(sample, 1, maxSample) || pts is! int || pts < 0) return;
    final entry = _samples[sample];
    if (entry == null) return;
    _tag = (sample as int, pts, entry.sentUs);
    _tagAtUs = nowUs;
    final durations = [tag['captureUs'], tag['encodeUs'], tag['queueUs']];
    final toHost = entry.toHostUs;
    _tagUpstreamUs =
        toHost != null && durations.every((v) => _integer(v, 0, maxHostUs))
        ? toHost + durations.cast<int>().reduce((a, b) => a + b)
        : null;
  }

  /// Send -> the tag's write on the host (uplink estimate + host durations).
  int? _tagUpstreamUs;

  /// Called for each video packet in order before it is decoded. True when
  /// this packet is the tagged one and should be pushed with a frame watch.
  /// [bytes] is the packet's size (0 = unknown: no transfer measurement).
  bool watch(int pts, int nowUs, {int bytes = 0}) {
    final tag = _tag;
    if (tag == null || pts < tag.$2) return false;
    _tag = null;
    if (pts > tag.$2) return false; // the tagged packet was not delivered
    if (bytes > 0 && nowUs >= _tagAtUs) {
      _transfer.add((nowUs - _tagAtUs) / 1000, nowUs);
      _frameBytes.add(bytes.toDouble(), nowUs);
    }
    // Per input: the host's tag write -> this packet's arrival here (the
    // ack-based uplink estimate and the host's own durations removed).
    final upstream = _tagUpstreamUs;
    if (upstream != null) {
      _downlink.add(max(0, nowUs - tag.$3 - upstream) / 1000, nowUs);
    }
    _watched[pts] = _Watched(tag.$3, nowUs);
    while (_watched.length > 16) {
      _watched.remove(_watched.keys.first);
    }
    return true;
  }

  /// The watched packet was accepted by the native engine at [nowUs].
  void pushed(int pts, int nowUs) => _watched[pts]?.pushedUs = nowUs;

  /// Native `inputFrames`: [pts, queued -> handed-to-texture microseconds].
  void delivered(Object? frames, int nowUs) {
    if (frames is! List) return;
    for (final item in frames.take(64)) {
      if (item is! List || item.length != 2) continue;
      final pts = item[0], us = item[1];
      if (pts is! int || us is! int || us < 0 || us > maxHostUs) continue;
      final watched = _watched[pts];
      final pushed = watched?.pushedUs;
      if (watched == null || pushed == null) continue;
      _watched.remove(pts);
      final total = pushed - watched.sentUs + us;
      if (total >= 0) {
        _toFrame.add(total / 1000, nowUs);
        if (pushed >= watched.atUs) {
          _framePush.add((pushed - watched.atUs) / 1000, nowUs);
        }
        _frameDecode.add(us / 1000, nowUs);
      }
    }
    _expire(nowUs);
  }

  /// A new decoder (stream.info/resize) cannot report older packets.
  void resetFrames() {
    _tag = null;
    _watched.clear();
  }

  InputLatencySummary summary(int nowUs, {required bool frameSupported}) =>
      InputLatencySummary(
        supported: true,
        frameSupported: frameSupported,
        toHost: _toHost.summary(nowUs, maxAgeUs),
        toFrame: frameSupported ? _toFrame.summary(nowUs, maxAgeUs) : null,
        hostApply: _hostApply.summary(nowUs, maxAgeUs),
        roundTrip: _roundTrip.summary(nowUs, maxAgeUs),
        framePush: frameSupported ? _framePush.summary(nowUs, maxAgeUs) : null,
        frameDecode: frameSupported
            ? _frameDecode.summary(nowUs, maxAgeUs)
            : null,
        transfer: _transfer.summary(nowUs, maxAgeUs),
        downlink: _downlink.summary(nowUs, maxAgeUs),
        frameBytes: _frameBytes.summary(nowUs, maxAgeUs),
        failedInjections: _failed,
      );
}
