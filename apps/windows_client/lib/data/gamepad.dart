/// gamepad-v1 (docs/remote-session-protocol.md "Gamepads"): the client reads
/// the gamepads of its PC (XInput) or phone (Android's game controllers) and
/// sends each pad's whole state; the Host plugs in a virtual Xbox 360
/// controller per pad and sends back the rumble the VM's game asks for.
/// Ordinary stream sessions only, up to four pads.
const gamepadProtocol = 'gamepad-v1';
const gamepadMaxPads = 4;

/// Released brokers reject this connect field and strip this capability.
bool gamepadAdvertised(Object? capabilities) =>
    capabilities is Map && capabilities['gamepadProtocol'] == gamepadProtocol;

/// XInput's button bits (XINPUT_GAMEPAD_*), as the report carries them.
abstract final class GamepadButtons {
  static const dpadUp = 0x0001, dpadDown = 0x0002, dpadLeft = 0x0004;
  static const dpadRight = 0x0008, start = 0x0010, back = 0x0020;
  static const leftThumb = 0x0040, rightThumb = 0x0080;
  static const leftShoulder = 0x0100, rightShoulder = 0x0200, guide = 0x0400;
  static const a = 0x1000, b = 0x2000, x = 0x4000, y = 0x8000;
  static const all = 0xF7FF;
}

/// One pad's whole state in XInput's ranges.
class GamepadState {
  const GamepadState({
    this.buttons = 0,
    this.lt = 0,
    this.rt = 0,
    this.lx = 0,
    this.ly = 0,
    this.rx = 0,
    this.ry = 0,
  });
  static const neutral = GamepadState();
  final int buttons, lt, rt, lx, ly, rx, ry;

  /// Bounded to the protocol's ranges (an undefined button bit dropped).
  factory GamepadState.clamped({
    required int buttons,
    required int lt,
    required int rt,
    required int lx,
    required int ly,
    required int rx,
    required int ry,
  }) {
    int axis(int value) => value.clamp(-32768, 32767);
    return GamepadState(
      buttons: buttons & GamepadButtons.all,
      lt: lt.clamp(0, 255),
      rt: rt.clamp(0, 255),
      lx: axis(lx),
      ly: axis(ly),
      rx: axis(rx),
      ry: axis(ry),
    );
  }

  bool get isNeutral => this == neutral;

  Map<String, Object?> message(int pad) => {
    'type': 'gamepad.state',
    'pad': pad,
    'buttons': buttons,
    'lt': lt,
    'rt': rt,
    'lx': lx,
    'ly': ly,
    'rx': rx,
    'ry': ry,
  };

  @override
  bool operator ==(Object other) =>
      other is GamepadState &&
      other.buttons == buttons &&
      other.lt == lt &&
      other.rt == rt &&
      other.lx == lx &&
      other.ly == ly &&
      other.rx == rx &&
      other.ry == ry;
  @override
  int get hashCode => Object.hash(buttons, lt, rt, lx, ly, rx, ry);
  @override
  String toString() => 'GamepadState($buttons, $lt, $rt, $lx, $ly, $rx, $ry)';
}

Map<String, Object?> gamepadRemoveMessage(int pad) => {
  'type': 'gamepad.remove',
  'pad': pad,
};

/// The Host's rumble for one pad (0..255 each motor).
class GamepadRumble {
  const GamepadRumble(this.pad, this.low, this.high);
  final int pad, low, high;
  static GamepadRumble? parse(Map<String, dynamic> event) {
    final pad = event['pad'], low = event['low'], high = event['high'];
    if (event.length != 4 ||
        pad is! int ||
        low is! int ||
        high is! int ||
        pad < 0 ||
        pad >= gamepadMaxPads ||
        low < 0 ||
        low > 255 ||
        high < 0 ||
        high > 255) {
      return null;
    }
    return GamepadRumble(pad, low, high);
  }

  @override
  bool operator ==(Object other) =>
      other is GamepadRumble &&
      other.pad == pad &&
      other.low == low &&
      other.high == high;
  @override
  int get hashCode => Object.hash(pad, low, high);
}

/// Between a source of local pads (XInput slots, Android devices) and a
/// session: each local pad gets the first free protocol pad (0..3); a state
/// goes only when it changed; while the view is not active every pad is sent
/// neutral once (nothing stays pressed in the VM) and its state again when it
/// is; a pad that went away is removed. [resync] sends everything again when
/// the stream takes pads again. The Host's rumble goes back to the local pad
/// that holds that protocol pad, only while the view is active: the physical
/// pads are shared by every window of the PC, so a view drives only the
/// motors it set and stops only those.
class GamepadForwarder {
  GamepadForwarder({required this.send, required this.rumble});

  /// Sends one protocol message; false when the session could not take it.
  final bool Function(Map<String, Object?> message) send;

  /// Rumbles the local pad [local] (0..255 each motor).
  final void Function(Object local, int low, int high) rumble;

  final _pads = <Object, int>{};
  final _states = <Object, GamepadState>{};
  final _sent = <int, GamepadState>{};
  // Removals the session could not take, and removals it took since the last
  // [resync] (a socket already dead may have lost them): sent again by
  // [resync] unless the protocol pad was given to another local pad meanwhile.
  final _pendingRemoves = <int>{}, _sentRemoves = <int>{};
  // The Host's latest rumble per local pad, and the local pads whose motors
  // this view set and has not stopped.
  final _levels = <Object, (int, int)>{};
  final _driving = <Object>{};
  bool _active = true;
  bool get active => _active;

  /// The local pads in use, by protocol pad.
  Map<int, Object> get pads => {for (final e in _pads.entries) e.value: e.key};

  /// The newest state of [local].
  void state(Object local, GamepadState value) {
    _states[local] = value;
    final pad = _pads[local] ?? _assign(local);
    if (pad == null) return;
    _deliver(pad, _active ? value : GamepadState.neutral);
  }

  /// [local] went away.
  void removed(Object local) {
    _states.remove(local);
    _levels.remove(local);
    _driving.remove(local);
    final pad = _pads.remove(local);
    if (pad == null) return;
    _sent.remove(pad);
    (send(gamepadRemoveMessage(pad)) ? _sentRemoves : _pendingRemoves).add(pad);
  }

  /// The view is (in)active: inactive, every pad is neutral in the VM and the
  /// motors this view set stop; active again, the pads' states and the Host's
  /// latest rumble come back.
  void setActive(bool active) {
    if (active == _active) return;
    _active = active;
    for (final entry in _pads.entries) {
      _deliver(
        entry.value,
        active
            ? (_states[entry.key] ?? GamepadState.neutral)
            : GamepadState.neutral,
      );
    }
    if (!active) {
      _stopMotors();
    } else {
      for (final entry in _levels.entries) {
        _drive(entry.key, entry.value.$1, entry.value.$2);
      }
    }
  }

  /// The Host's rumble: to the local pad that holds that protocol pad, while
  /// the view is active (kept for when it is again).
  void hostRumble(GamepadRumble value) {
    for (final entry in _pads.entries) {
      if (entry.value != value.pad) continue;
      _levels[entry.key] = (value.low, value.high);
      if (_active) _drive(entry.key, value.low, value.high);
    }
  }

  /// The stream takes pads again (its first picture, a resume): every pad's
  /// state and every removal the session could not take are sent again.
  void resync() {
    _sent.clear();
    final removals = {..._pendingRemoves, ..._sentRemoves};
    _pendingRemoves.clear();
    _sentRemoves.clear();
    for (final pad in removals) {
      if (_pads.containsValue(pad)) continue; // its new state replaces it
      if (!send(gamepadRemoveMessage(pad))) _pendingRemoves.add(pad);
    }
    for (final entry in _pads.entries) {
      _deliver(
        entry.value,
        _active
            ? (_states[entry.key] ?? GamepadState.neutral)
            : GamepadState.neutral,
      );
    }
  }

  /// The stream froze (a resume began): the motors this view set stop, and the
  /// Host's level is unknown until it is sent again.
  void holdRumble() {
    _stopMotors();
    _levels.clear();
  }

  /// The session ended: nothing is sent any more; the motors this view set
  /// stop (another window's are left alone).
  void close() {
    _stopMotors();
    _pads.clear();
    _states.clear();
    _sent.clear();
    _pendingRemoves.clear();
    _sentRemoves.clear();
    _levels.clear();
  }

  void _drive(Object local, int low, int high) {
    if (low == 0 && high == 0) {
      // Nothing this view set to stop: leave another window's motors alone.
      if (!_driving.remove(local)) return;
    } else {
      _driving.add(local);
    }
    rumble(local, low, high);
  }

  void _stopMotors() {
    for (final local in _driving.toList()) {
      rumble(local, 0, 0);
    }
    _driving.clear();
  }

  int? _assign(Object local) {
    for (var pad = 0; pad < gamepadMaxPads; pad++) {
      if (!_pads.containsValue(pad)) {
        // A removal still pending for this pad is overtaken by its new state.
        _pendingRemoves.remove(pad);
        _sentRemoves.remove(pad);
        return _pads[local] = pad;
      }
    }
    return null;
  }

  void _deliver(int pad, GamepadState value) {
    // A pad is plugged in on the Host by its first state, also a neutral one.
    if (_sent[pad] == value) return;
    if (send(value.message(pad))) _sent[pad] = value;
  }
}

/// The `streamStats` fields of gamepad-v1 (counts only, never a button, an
/// axis or a controller's name): whether the session was granted, the pads
/// the Host was asked to hold at the window's end, states sent, states the
/// session could not take (not connected yet, resuming) and rumbles received
/// in the window. Brokers before these fields drop them.
const gamepadStatsFields = <String>[
  'gamepad',
  'gamepadPads',
  'gamepadStates',
  'gamepadRefused',
  'gamepadRumbles',
];

/// Counts the session's gamepad-v1 messages for [gamepadStatsFields]: a
/// granted session with no state sent means no pad was read on this device.
class GamepadTally {
  final _pads = <int>{};
  int _states = 0, _refused = 0, _rumbles = 0;

  /// One message given to the session; [taken] false when it was not sent.
  void message(Map<String, Object?> message, {required bool taken}) {
    final pad = message['pad'];
    if (pad is! int) return;
    if (!taken) {
      if (message['type'] == 'gamepad.state') _refused++;
      return;
    }
    if (message['type'] == 'gamepad.state') {
      _states++;
      _pads.add(pad);
    } else if (message['type'] == 'gamepad.remove') {
      _pads.remove(pad);
    }
  }

  void rumble() => _rumbles++;

  /// This window's values, then the counts start again (pads held stay).
  Map<String, Object> take({required bool granted}) {
    final values = <String, Object>{
      'gamepad': granted,
      if (granted) ...{
        'gamepadPads': _pads.length,
        'gamepadStates': _states,
        'gamepadRefused': _refused,
        'gamepadRumbles': _rumbles,
      },
    };
    _states = _refused = _rumbles = 0;
    return values;
  }
}

/// A native source of pads: events from the runner (Windows) or the activity
/// (Android) as {slot|device, buttons, lt, rt, lx, ly, rx, ry} maps.
GamepadState? gamepadStateFrom(Map<Object?, Object?> event) {
  int? value(String key) {
    final v = event[key];
    return v is int ? v : null;
  }

  final buttons = value('buttons'), lt = value('lt'), rt = value('rt');
  final lx = value('lx'), ly = value('ly'), rx = value('rx'), ry = value('ry');
  if (buttons == null ||
      lt == null ||
      rt == null ||
      lx == null ||
      ly == null ||
      rx == null ||
      ry == null) {
    return null;
  }
  return GamepadState.clamped(
    buttons: buttons,
    lt: lt,
    rt: rt,
    lx: lx,
    ly: ly,
    rx: rx,
    ry: ry,
  );
}
