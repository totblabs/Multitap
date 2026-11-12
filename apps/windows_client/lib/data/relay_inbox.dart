import 'dart:async';
import 'dart:collection';

/// Optional passive view; durations use the caller's monotonic clock.
class RelayInboxProbe {
  RelayInboxProbe(this.nowUs);
  final int Function() nowUs;
  final _arrivals = Queue<int>();
  bool _enabled = true;
  bool get enabled => _enabled;
  void disable() {
    _enabled = false;
    _clear();
  }

  int bytes = 0;
  int get packets => _arrivals.length;
  int? get oldestUs {
    if (!enabled) return null;
    if (_arrivals.isEmpty) return 0;
    if (_arrivals.first < 0) return null;
    try {
      final now = nowUs();
      return now >= _arrivals.first ? now - _arrivals.first : null;
    } catch (_) {
      return null;
    }
  }

  void _added(int size) {
    if (!enabled) return;
    var now = -1;
    try {
      now = nowUs();
    } catch (_) {}
    _arrivals.add(now);
    bytes += size;
  }

  void _removed(int size) {
    if (!enabled) return;
    if (_arrivals.isEmpty) return;
    _arrivals.removeFirst();
    bytes -= size;
  }

  void _clear() {
    _arrivals.clear();
    bytes = 0;
  }
}

/// The receive side of a relay session socket.
///
/// The session reads its socket in order and awaits the native engine for each
/// media packet, so a message that is not media (an input acknowledgement, a
/// ping answer) used to be handled only after every packet ahead of it had
/// been pushed. Its arrival time was then the time its turn came, and the
/// measured round trip and the input latency estimate included the client's
/// own backlog (about 14 ms on a phone, whose reader shares its thread with
/// the UI). This inbox reads the socket as soon as data arrives: [answer]
/// sees every text message at its arrival and consumes it (returns true) when
/// it needs no ordering with the media; everything else is delivered in
/// arrival order, unchanged.
///
/// Audio likewise: the reader also waits while the decoder's queue is full,
/// and every audio packet behind that video waited with it, so a slow decoder
/// became a gap in the sound (#98). [media] sees each binary message at its
/// arrival while only video (media kind 1) waits undelivered ahead of it, and
/// consumes it (returns true) when it needs no ordering with that video. Once
/// anything else waits (text, cursor, an audio packet [media] declined), every
/// message stays in arrival order until it has been delivered.
///
/// Delivery is bounded: past [maxPackets] undelivered messages or [maxBytes]
/// bytes the socket is paused (TCP flow control again) until the consumer has
/// caught up to half of that.
Stream<dynamic> relayInbox(
  Stream<dynamic> socket, {
  required bool Function(String message) answer,
  bool Function(List<int> message)? media,
  int maxPackets = 64,
  int maxBytes = 8 * 1024 * 1024,
  RelayInboxProbe? probe,
  Future<void>? until,
}) {
  final controller = StreamController<dynamic>();
  var buffered = 0, bufferedBytes = 0, ordered = 0;
  late final StreamSubscription<dynamic> source;
  int sizeOf(dynamic message) =>
      message is String ? message.length : (message as List<int>).length;
  bool video(dynamic message) =>
      message is List<int> && message.isNotEmpty && message[0] == 1;
  source = socket.listen(
    (message) {
      if (message is String) {
        bool handled;
        try {
          handled = answer(message);
        } catch (_) {
          handled = false;
        }
        if (handled) return;
      } else if (media != null && ordered == 0) {
        bool handled;
        try {
          handled = media(message as List<int>);
        } catch (_) {
          handled = false;
        }
        if (handled) return;
      }
      if (!video(message)) ordered++;
      buffered++;
      bufferedBytes += sizeOf(message);
      probe?._added(sizeOf(message));
      controller.add(message);
      if (!source.isPaused &&
          (buffered > maxPackets || bufferedBytes > maxBytes)) {
        source.pause();
      }
    },
    onError: controller.addError,
    onDone: controller.close,
  );
  // Not returned: a consumer that cancels does not wait for the socket's own
  // cancellation (which a closed socket may complete late).
  controller.onCancel = () {
    probe?._clear();
    unawaited(source.cancel());
  };
  // [until]: the reader leaves this socket (session-resume-v1 replaces a
  // silent one); what was already delivered still reaches it first.
  until?.then((_) {
    unawaited(source.cancel());
    if (!controller.isClosed) unawaited(controller.close());
  });
  return controller.stream.map((message) {
    buffered--;
    bufferedBytes -= sizeOf(message);
    if (!video(message)) ordered--;
    probe?._removed(sizeOf(message));
    if (source.isPaused &&
        buffered <= maxPackets ~/ 2 &&
        bufferedBytes <= maxBytes ~/ 2) {
      source.resume();
    }
    return message;
  });
}
