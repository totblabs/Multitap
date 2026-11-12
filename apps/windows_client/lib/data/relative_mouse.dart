import 'dart:async';

import 'session_activity.dart';

import 'package:flutter/services.dart';

/// relative-mouse-v1 (docs/remote-session-protocol.md). With the user's
/// "마우스 잠금" option on, a click into the picture hands the mouse to the VM:
/// the client holds the pointer in the picture and sends movement instead of
/// positions (RelativeMouseControl). When anything is uncertain the client
/// stays in normal absolute mode: a user must never be left without a pointer.
const relativeMouseProtocol = 'relative-mouse-v1';

/// Released brokers reject this connect field and strip this capability, so a
/// client that does not see it here must not send the field.
bool relativeMouseAdvertised(Object? capabilities) =>
    capabilities is Map &&
    capabilities['relativeMouseProtocol'] == relativeMouseProtocol;

/// Where a capture's input goes: movement as moveRelative (injected on the
/// Host), or the 마우스 잠금 virtual USB mouse (usb_game_mouse.dart), which then
/// takes movement, buttons and wheel from the runner's raw input.
enum RelativeMousePath { injected, usb }

/// The runner's clip and raw input (windows/runner/relative_mouse.h).
abstract interface class RelativeMouseNative {
  /// The path of the capture that began now, or null: the runner refuses when
  /// it is unsure. [picture]: logical pixels of the window, where the pointer
  /// is held; without it the pointer is parked where it stands. [pin]
  /// (cursor-echo-v1): parked, then held on each VM cursor position the Host
  /// echoes, so the pointer shown is the VM's own. [conceal]
  /// (cursor-compose-v1): parked where it stands and shown as nothing at all;
  /// the Host draws the VM's cursor into the video instead.
  Future<RelativeMousePath?> engage(
    Rect? picture, {
    bool pin = false,
    bool conceal = false,
  });
  Future<void> release();

  /// Whether Ctrl+Alt+Shift+Z stays here instead of reaching the remote.
  Future<void> armKey(bool armed);

  /// The runner ended capture by itself (`focus`, `lost`, `device`), or the
  /// user pressed the release key (`toggle`).
  void listen(void Function(String event) onEvent);
  void dispose();
}

/// Android's side adds whether a mouse is connected (MainActivity,
/// AndroidGameMouse.kt): a finger never hands anything over.
abstract interface class MousePresenceNative implements RelativeMouseNative {
  /// A mouse (USB, Bluetooth, DeX) is connected now.
  Future<bool> mousePresent();

  /// A mouse came, or the last one left.
  set onMouse(void Function(bool present)? callback);
}

class ChannelRelativeMouse implements MousePresenceNative {
  static const _channel = MethodChannel('multitap/relative-mouse');
  static ChannelRelativeMouse? _listener;
  void Function(bool present)? _onMouse;

  @override
  set onMouse(void Function(bool present)? callback) => _onMouse = callback;

  @override
  Future<bool> mousePresent() async {
    try {
      return await _channel.invokeMethod<bool>('mouse') == true;
    } on MissingPluginException {
      return false;
    } on PlatformException {
      return false;
    }
  }

  @override
  Future<RelativeMousePath?> engage(
    Rect? picture, {
    bool pin = false,
    bool conceal = false,
  }) async {
    try {
      // 0 refused, 1 injected, 2 the virtual USB mouse.
      final path = await _channel.invokeMethod<Object>(
        'engage',
        picture == null && !pin && !conceal
            ? null
            : {
                if (picture != null) ...{
                  'left': picture.left,
                  'top': picture.top,
                  'width': picture.width,
                  'height': picture.height,
                },
                if (pin) 'pin': true,
                if (conceal) 'conceal': true,
              },
      );
      if (identical(_listener, this)) {
        SessionHumanSource.relative(this, path == 1 || path == 2);
      }
      return path == 2
          ? RelativeMousePath.usb
          : path == 1
          ? RelativeMousePath.injected
          : null;
    } on MissingPluginException {
      return null;
    } on PlatformException {
      return null;
    }
  }

  @override
  Future<void> release() async {
    try {
      await _call('release');
    } finally {
      SessionHumanSource.relative(this, false);
    }
  }

  @override
  Future<void> armKey(bool armed) => _call('armKey', armed);

  Future<void> _call(String method, [Object? argument]) async {
    try {
      await _channel.invokeMethod<void>(method, argument);
    } on MissingPluginException {
      // Widget tests and previews have no native window.
    } on PlatformException {
      // The runner releases by itself on every path that matters.
    }
  }

  @override
  void listen(void Function(String event) onEvent) {
    _listener = this;
    _channel.setMethodCallHandler((call) async {
      final arguments = call.arguments;
      if (call.method == 'toggle') onEvent('toggle');
      if (call.method == 'released' && arguments is String) {
        SessionHumanSource.relative(this, false);
        onEvent(arguments);
      }
      if (call.method == 'mouse' && arguments is bool) {
        _onMouse?.call(arguments);
      }
    });
  }

  @override
  void dispose() {
    SessionHumanSource.relative(this, false);
    if (!identical(_listener, this)) return;
    _listener = null;
    _channel.setMethodCallHandler(null);
  }
}

/// When capture starts and ends, for one session window (마우스 잠금,
/// like ToTB Link's 마우스 넘기기). With the option on, a click into the picture
/// hands the mouse to the VM whether or not the remote cursor is visible: the
/// local pointer is held inside the picture and only movement travels. The
/// release key, focus loss, minimize, a pen or touch, the option going off and
/// the session's end take it back; the next click (or the release key again)
/// hands it over again. Native code releases by itself on focus loss, minimize
/// and teardown; this decides the rest.
class RelativeMouseControl {
  RelativeMouseControl({
    required this._native,
    this.picture,
    this.pinned,
    this.composed,
    this.onChanged,
    this.onEngaged,
    this.onToggle,
  }) {
    _native.listen(_nativeEvent);
  }

  final RelativeMouseNative _native;

  /// The picture in logical pixels of the window: where the pointer is held.
  final Rect? Function()? picture;

  /// cursor-echo-v1 is granted: a capture holds the pointer on the VM's
  /// echoed cursor position (asked when the capture begins).
  final bool Function()? pinned;

  /// The current capture was engaged pinned.
  bool get pinnedEngaged => _engaged && _pinned;
  bool _pinned = false;

  /// The picture the current capture holds the pointer in (null: pinned or composed).
  Rect? _held;

  /// cursor-compose-v1 is granted: a capture parks the pointer, shows no
  /// cursor here and the Host draws the VM's cursor into the video (like
  /// ToTB Link). Takes precedence over [pinned].
  final bool Function()? composed;

  /// The current capture was engaged with the cursor in the video.
  bool get composedEngaged => _engaged && _composed;
  bool _composed = false;

  /// Capture or the pointer the view must show changed.
  final void Function()? onChanged;
  final void Function()? onEngaged;

  /// Windows toggles the user's whole option, including from off. Other
  /// platforms keep their existing capture-only release action.
  final void Function()? onToggle;

  bool _available = false, _hidden = false, _owned = false, _over = false;
  bool _engaging = false, _engaged = false;
  // A click into the picture (or the release key) asked for the mouse; every
  // release clears it, so only the next click hands it over again.
  bool _wanted = false;
  // The user confirmed the option (handOver): the pointer need not be over
  // the picture. Cleared with _wanted and by the attempt it starts.
  bool _anywhere = false;
  bool _keyArmed = false, _disposed = false, _resume = false;
  bool _shortcutAvailable = false;
  int _attempt = 0;

  /// Movement goes through native code; positions must not be sent.
  bool get capturing => _engaging || _engaged;
  bool get engaged => _engaged;

  /// The virtual USB mouse takes buttons and wheel too: only the releases of
  /// buttons pressed before it began may still be sent.
  bool get usbPath => _engaged && _path == RelativeMousePath.usb;
  RelativeMousePath? _path;

  /// The remote cursor is hidden and the mouse is not handed over: show a
  /// local pointer, or the user could not find the picture or the bar.
  bool get localPointerNeeded => _available && _hidden && !capturing;

  /// [available]: the option is on, granted, no RDP around this client.
  /// [hidden]: an accepted explicit remote state said so. [owned]: this window
  /// has focus and the picture owns input. [over]: the pointer is on the picture.
  void update({
    required bool available,
    required bool hidden,
    required bool owned,
    required bool over,
    bool? shortcutAvailable,
    bool paused = false,
  }) {
    if (_disposed) return;
    // [paused] (the stream is resizing): ownership goes for a moment. A capture
    // it ends comes back by itself when the resize is over.
    if (paused && capturing) _resume = true;
    if (!available) _resume = false;
    final before = localPointerNeeded;
    final lost = (_available && !available) || (_owned && !owned);
    _available = available;
    _hidden = hidden && available;
    _owned = owned;
    _over = over;
    _shortcutAvailable = shortcutAvailable ?? available;
    if (_shortcutAvailable != _keyArmed) {
      _keyArmed = _shortcutAvailable;
      unawaited(_native.armKey(_keyArmed));
    }
    if (lost || !available) {
      final resume = _resume;
      _wanted = _anywhere = false;
      if (capturing) _end();
      _resume = resume && available;
    }
    if (_resume && !paused && owned) {
      _resume = false;
      _wanted = _anywhere = true;
    } else if (!paused && !owned) {
      _resume = false;
    }
    _evaluate();
    if (localPointerNeeded != before) onChanged?.call();
  }

  /// A press on the picture: the mouse goes to the VM as soon as the window
  /// owns it (the press itself still reaches the remote as a click).
  void pressed() {
    if (_disposed || !_available || capturing) return;
    _wanted = true;
    _evaluate();
  }

  /// The user turned the option on and pressed 확인: the mouse goes to the VM
  /// as soon as the window owns it, wherever the pointer stands, without a
  /// click into the picture. The capture holds it in the picture, pinned on
  /// the VM's cursor where cursor-echo-v1 is granted.
  void handOver() {
    if (_disposed || !_available || capturing) return;
    _wanted = _anywhere = true;
    _evaluate();
  }

  /// The clip is the picture as it was when the capture began. When the picture
  /// moves or changes size in the same window (a new remote resolution, the
  /// letterbox), the capture is taken again on the new picture. Pinned and
  /// composed captures do not use the picture.
  void pictureMoved() {
    if (_disposed || !_engaged || _pinned || _composed) return;
    final now = picture?.call();
    if (now == null || _held == null || now == _held) return;
    _attempt++;
    _engaged = false;
    unawaited(_native.release());
    _wanted = _anywhere = true;
    _evaluate();
  }

  /// Takes the mouse back now, as the release key does (Android's Back, a
  /// finger on the picture); the next click hands it over again.
  void release() {
    if (!_disposed && capturing) _end();
  }

  void _evaluate() {
    final ready = _wanted && _available && _owned && (_over || _anywhere);
    if (!_disposed && !capturing && ready) unawaited(_engage());
  }

  Future<void> _engage() async {
    final attempt = ++_attempt;
    _engaging = true;
    _anywhere = false;
    onChanged?.call();
    RelativeMousePath? path;
    final compose = composed?.call() == true;
    final pin = !compose && pinned?.call() == true;
    try {
      final area = compose || pin ? null : picture?.call();
      _held = area;
      path = compose
          ? await _native.engage(null, conceal: true)
          : await _native.engage(area, pin: pin);
    } catch (_) {
      // Stays absolute.
    }
    // A release or native notice already answered this attempt, in order.
    if (attempt != _attempt) return;
    final accepted = path != null;
    _path = path;
    _pinned = accepted && pin;
    _composed = accepted && compose;
    _engaging = false;
    _engaged = accepted;
    // Refused: absolute until the next click, never a retry loop.
    if (!accepted) _wanted = false;
    onChanged?.call();
    if (accepted) onEngaged?.call();
  }

  void _end() {
    _attempt++;
    _engaged = _engaging = false;
    _wanted = _anywhere = false;
    unawaited(_native.release());
    onChanged?.call();
  }

  void _nativeEvent(String event) {
    if (_disposed) return;
    if (event == 'toggle') {
      final toggle = onToggle;
      if (toggle == null) {
        _toggle();
      } else if (_shortcutAvailable) {
        // Clear the Dart capture immediately too; the Windows runner already
        // released its native clip before delivering this method call.
        if (capturing) _end();
        toggle();
      }
    } else if (capturing) {
      // `focus`, `lost`, `device` (a pen or touch): the runner already let go.
      _attempt++;
      _engaged = _engaging = false;
      _wanted = _anywhere = false;
      onChanged?.call();
    }
  }

  // The release key: lets go, or hands the mouse over again as a click would.
  void _toggle() {
    if (capturing) {
      _end();
    } else if (_available && _owned) {
      _wanted = true;
      _evaluate();
    }
  }

  void dispose() {
    if (_disposed) return;
    _disposed = true;
    _attempt++;
    if (capturing) unawaited(_native.release());
    _engaged = _engaging = false;
    if (_keyArmed) unawaited(_native.armKey(false));
    _native.dispose();
  }
}
