/// connect-timing-v1 (tuning backlog #2, 2026-11-02): where one connection's
/// time from 스트리밍 연결 to its first decoded picture goes. Fixed millisecond
/// stages only, added to the connection's first streamReady record; nothing
/// decides on them.
library;

/// Longest stage kept (10 min); a longer or negative one is left out.
const connectStageMaxMs = 600000;

/// The Host's stages in its first stream.info (`startStages`, Host
/// connect-timing-v1), by the name they get in this client's record:
/// session.start received -> its stream thread (the previous stream's stop,
/// a running display probe), -> worker process launched and bound, -> the
/// start handed to the worker (display preparation), -> the worker's
/// stream.info (capture, encoder and audio setup).
const hostStartStageFields = {
  'admitMs': 'hostAdmitMs',
  'workerMs': 'hostWorkerMs',
  'displayMs': 'hostDisplayMs',
  'mediaMs': 'hostMediaMs',
};

/// Every field this file can add to a streamReady record (product_log.dart
/// keeps them; the broker's client-diagnostics.mjs accepts them).
const connectTimingFields = {
  // Before the session existed: the click -> the session window's first
  // build, its desktop read, then the view's preparation steps and the
  // broker's connect request.
  'windowMs', 'loadMs', 'retireMs', 'refreshMs', 'presentMs', 'outputsMs',
  'viewportMs', 'connectMs',
  // The session: relay WebSocket opened, -> first stream info, -> decoder
  // started, -> first key frame received, -> its decoded picture seen (and
  // elapsedMs, streamStart -> ready, which the log already keeps).
  'dialMs', 'infoMs', 'decoderMs', 'keyFrameMs', 'firstFrameMs',
  // The values of hostStartStageFields.
  'hostAdmitMs', 'hostWorkerMs', 'hostDisplayMs', 'hostMediaMs',
};

int? connectStageMs(int microseconds) =>
    microseconds < 0 || microseconds > connectStageMaxMs * 1000
    ? null
    : microseconds ~/ 1000;

class ConnectTiming {
  /// Stages measured before the session existed (the window and the view
  /// fill them in ms before it starts).
  final before = <String, int>{};
  final _marksUs = <String, int>{};
  Map<String, int> _host = const {};
  bool _reported = false;

  /// Only the first time of each mark counts (a resize's stream info or key
  /// frame is not the connection's).
  void mark(String name, int nowUs) => _marksUs.putIfAbsent(name, () => nowUs);

  /// The Host's `startStages` of the first stream info; anything else is
  /// ignored (older Hosts send none).
  void host(Object? stages) {
    if (_marksUs.containsKey('hostSeen')) return;
    _marksUs['hostSeen'] = 0;
    if (stages is! Map) return;
    _host = {
      for (final entry in hostStartStageFields.entries)
        if (stages[entry.key] is int &&
            (stages[entry.key] as int) >= 0 &&
            (stages[entry.key] as int) <= connectStageMaxMs)
          entry.value: stages[entry.key] as int,
    };
  }

  /// The fields of the connection's first ready record; empty afterwards.
  Map<String, int> ready(int nowUs) {
    if (_reported) return const {};
    _reported = true;
    int? between(String from, String? to) {
      final a = _marksUs[from], b = to == null ? nowUs : _marksUs[to];
      return a == null || b == null ? null : connectStageMs(b - a);
    }

    return {
      for (final entry in before.entries)
        if (connectTimingFields.contains(entry.key) &&
            entry.value >= 0 &&
            entry.value <= connectStageMaxMs)
          entry.key: entry.value,
      'elapsedMs': ?between('start', null),
      'dialMs': ?between('start', 'socket'),
      'infoMs': ?between('socket', 'info'),
      'decoderMs': ?between('info', 'decoder'),
      'keyFrameMs': ?between('decoder', 'keyFrame'),
      'firstFrameMs': ?between('keyFrame', null),
      ..._host,
    };
  }
}
