import 'dart:async';
import 'dart:io';

/// channel-resume-v1 (Broker server/src/channel-resume.mjs): a stream's side
/// channels (clipboard, printing, shared folders, USB devices) come back by
/// themselves after the stream resumes. This client says on its session socket
/// that it dials them again ([channelResumeHeader]); a broker and Host that do
/// the same answer with the marker in `session.resume.token`. Every other mix
/// keeps the released behaviour: a channel that ended stays ended.
const channelResumeProtocol = 'channel-resume-v1';

/// The request header of the session socket (a released broker ignores it).
const channelResumeHeader = 'X-Multitap-Channel-Resume';

/// The broker's grant in a `session.resume.token` or `session.resumed` message.
bool channelResumeGranted(Map<String, dynamic> event) =>
    event['channelResumeProtocol'] == channelResumeProtocol;

/// A side channel of [kind] (`print`, `drive`, `usb`) that ended with [code]
/// may be dialled again in the same session: its transport or the stream
/// itself went away. Anything else (a violation, a revoked permission) stays
/// ended, as before.
bool channelEndTransient(String kind, String? code) =>
    code == '${kind}_transport_closed' ||
    code == '${kind}_pair_timeout' ||
    code == '${kind}_stream_away' ||
    code == '${kind}_timeout';

/// The broker answered a dial with a refusal (401, 409...), not with "not
/// now" (503 while the stream is away, a gateway's 502 or 504): never again.
bool channelDialRefused(Object error) {
  if (error is! WebSocketException) return false;
  final status = error.httpStatusCode;
  return status != null && !const {408, 502, 503, 504}.contains(status);
}

/// The bounded attempts of one side channel, the clipboard's released rule
/// for all of them: three fresh pairs after 1, 2 and 4 seconds, none spent
/// while the stream itself is away (a dial then says nothing about the
/// channel), one fresh set when the stream continues, and a new count once a
/// pair really worked (the broker bounds new pairs). Where the protocol was
/// not negotiated nothing is dialled: [recovering] then only covers the
/// moment in which the stream may turn out to be away, so no notice is shown
/// for a channel that fell with the stream.
class ChannelRedial {
  ChannelRedial({
    required this.enabled,
    required this.away,
    required this.dial,
    required this.changed,
    this.maximum = 3,
    this.unit = const Duration(seconds: 1),
    this.settle = const Duration(seconds: 2),
  });

  /// channel-resume-v1 was negotiated for this session.
  final bool Function() enabled;

  /// The stream itself is away (its socket or the Host is coming back).
  final bool Function() away;

  /// Replaces the ended channel with a fresh one and starts it. False: the
  /// old one has not let go yet, so nothing was dialled (counts as an attempt).
  final Future<bool> Function() dial;

  /// [recovering] changed.
  final void Function() changed;
  final int maximum;
  final Duration unit, settle;

  int _attempts = 0;
  Timer? _timer;
  bool _dialling = false, _waiting = false, _ended = false, _final = false;

  /// The channel is down and is being brought back (or waits for the stream).
  bool get recovering => _timer != null || _dialling || _waiting;

  /// The channel is down and nothing more is tried by itself.
  bool get gaveUp => _ended && !recovering;

  /// The channel ended for a transient reason.
  void ended() {
    if (_final) return;
    _ended = true;
    if (_timer != null || _dialling) return;
    if (away()) {
      _waiting = true;
      changed();
      return;
    }
    _waiting = false;
    if (!enabled()) {
      // The stream may be about to say it is away; then the wait is for it.
      _timer = Timer(settle, () {
        _timer = null;
        _waiting = away();
        changed();
      });
      changed();
      return;
    }
    if (_attempts >= maximum) {
      changed();
      return;
    }
    _timer = Timer(unit * (1 << _attempts++), () {
      _timer = null;
      if (away()) {
        // The stream went away during the delay: the attempt is not spent.
        _attempts--;
        _waiting = true;
        changed();
        return;
      }
      _dialling = true;
      changed();
      unawaited(
        dial().catchError((Object _) => false).then((started) {
          _dialling = false;
          if (started) {
            // A fresh channel exists; its own end is told to [ended] again.
            _ended = false;
            changed();
          } else if (_final || !_ended) {
            changed();
          } else {
            ended();
          }
        }),
      );
    });
    changed();
  }

  /// The fresh channel is paired: it works, and a later loss starts over.
  void paired() {
    _attempts = 0;
    _ended = false;
  }

  /// The channel ended for a reason of its own, or its dial was refused.
  void refused() {
    _timer?.cancel();
    _timer = null;
    _waiting = false;
    _ended = _final = true;
    changed();
  }

  /// The stream continues: a channel that waits for it (or ran out of
  /// attempts during the outage) gets one fresh set.
  void resumed() {
    if (_final || !_ended || _timer != null || _dialling) return;
    if (!enabled()) {
      if (_waiting) {
        _waiting = false;
        changed();
      }
      return;
    }
    if (!_waiting && _attempts < maximum) return;
    _waiting = false;
    _attempts = 0;
    ended();
  }

  /// The session (or its sign-in phase) let the channel go: nothing is
  /// pending, and a channel started later counts from the beginning.
  void reset() {
    _timer?.cancel();
    _timer = null;
    _waiting = _ended = _final = false;
    _attempts = 0;
  }
}
