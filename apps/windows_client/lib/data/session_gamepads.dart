import 'dart:async';

import 'package:flutter/services.dart';

import 'gamepad.dart';
import 'remote_session.dart';

/// The Windows runner's channel (gamepad_input.h): XInput slots 0..3.
const windowsGamepadChannel = MethodChannel('multitap/gamepad');

/// The Android activity's channel (GamepadInput.kt): input device ids.
const androidGamepadChannel = MethodChannel('multitap/android/gamepad');

/// gamepad-v1 for one session: the platform reads its gamepads only while a
/// granted session runs ("start"/"stop"), each local pad's state goes through
/// a [GamepadForwarder] to the session, and the Host's rumble comes back to
/// the local pad ("rumble" {slot, low, high}). The platform sends "state"
/// {slot, buttons, lt, rt, lx, ly, rx, ry} and "removed" slot.
class SessionGamepads {
  SessionGamepads(this.session, {required this.channel});
  final RemoteSession session;
  final MethodChannel channel;
  GamepadForwarder? _forwarder;
  StreamSubscription<GamepadRumble>? _rumble;
  StreamSubscription<void>? _resync;
  bool _active = true, _resuming = false;

  /// Whether the platform reads pads for this session now.
  bool get running => _forwarder != null;

  Future<void> start() async {
    if (_forwarder != null || !session.gamepad) return;
    final forwarder = GamepadForwarder(
      send: session.sendGamepad,
      rumble: (local, low, high) => unawaited(
        channel
            .invokeMethod<void>('rumble', {
              'slot': local,
              'low': low,
              'high': high,
            })
            .catchError((Object _) {}),
      ),
    )..setActive(_active);
    _forwarder = forwarder;
    channel.setMethodCallHandler(_handle);
    _rumble = session.gamepadRumble.listen(forwarder.hostRumble);
    // The stream takes pads again (first picture, resume): all sent again.
    _resync = session.gamepadResync.listen((_) => forwarder.resync());
    _resuming = session.resuming;
    // A session that ended lets the pads go at once (on Android the
    // controller's B is Back again); a frozen one stops the motors.
    session.addListener(_sessionChanged);
    try {
      await channel.invokeMethod<Object?>('start');
    } catch (_) {
      // No gamepad support on this platform: nothing to read.
    }
  }

  /// The view is (in)active (focused, in front); inactive, nothing stays
  /// pressed in the VM.
  void setActive(bool active) {
    _active = active;
    _forwarder?.setActive(active);
  }

  Future<void> stop() async {
    final forwarder = _forwarder;
    if (forwarder == null) return;
    _forwarder = null;
    session.removeListener(_sessionChanged);
    channel.setMethodCallHandler(null);
    await _rumble?.cancel();
    _rumble = null;
    await _resync?.cancel();
    _resync = null;
    forwarder.close();
    try {
      await channel.invokeMethod<void>('stop');
    } catch (_) {}
  }

  void _sessionChanged() {
    if (session.closed) {
      unawaited(stop());
      return;
    }
    // A resume began: nothing keeps vibrating while the stream is frozen; the
    // broker or the Host sends the level again once it is back.
    final resuming = session.resuming;
    if (resuming && !_resuming) _forwarder?.holdRumble();
    _resuming = resuming;
  }

  Future<Object?> _handle(MethodCall call) async {
    final forwarder = _forwarder;
    if (forwarder == null) return null;
    final args = call.arguments;
    if (call.method == 'state' && args is Map) {
      final slot = args['slot'], state = gamepadStateFrom(args);
      if (slot is int && state != null) forwarder.state(slot, state);
    } else if (call.method == 'removed' && args is int) {
      forwarder.removed(args);
    }
    return null;
  }
}
