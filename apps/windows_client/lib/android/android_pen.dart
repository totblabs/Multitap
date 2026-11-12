import 'dart:async';

import 'package:flutter/gestures.dart';
import 'package:flutter/services.dart';

/// A basic absolute mouse fallback, not pressure, Windows Ink or an eraser.
/// The outer view owns authorization, capture, ordered delivery and input.reset.
class AndroidPenInput {
  AndroidPenInput({
    required this.project,
    required this.clamp,
    required this.send,
    required this.takeOver,
  });

  final Offset? Function(Offset) project;
  final Offset Function(Offset) clamp;
  final void Function(Map<String, dynamic>) send;
  final VoidCallback takeOver;
  final _forwardedFingers = <int>{};
  final _ignoredFingers = <int>{};
  final _heldMice = <int>{};
  final _ignoredMice = <int, int>{};
  int? _device, _pointer;
  PointerDeviceKind? _kind;
  int _time = -1, _retiredAt = -1, _buttons = 0;
  int _revision = 0;
  int? _retiredDevice;
  PointerDeviceKind? _retiredKind;
  Offset? _anchor;
  bool _contacting = false, _nativeReady = false;

  bool get active => _device != null;
  bool get contacting => _contacting;
  int get buttons => _buttons;

  static bool _pen(PointerDeviceKind kind) =>
      kind == PointerDeviceKind.stylus ||
      kind == PointerDeviceKind.invertedStylus;
  static bool _valid(Offset? point) =>
      point != null &&
      point.dx.isFinite &&
      point.dy.isFinite &&
      point.dx >= 0 &&
      point.dx <= 1 &&
      point.dy >= 0 &&
      point.dy <= 1;

  bool get _suppressFingers => active && (_contacting || _nativeReady);

  /// Call for every pointer kind, before the existing finger/mouse route.
  /// Pen events are always consumed, including outside the picture.
  bool handle(PointerEvent event) {
    if (_pen(event.kind)) {
      _handlePen(event);
      return true;
    }
    if (event.kind == PointerDeviceKind.mouse ||
        event.kind == PointerDeviceKind.trackpad) {
      final ignoredSince = _ignoredMice[event.device];
      if (ignoredSince != null && event is! PointerScrollEvent) {
        final released =
            event is PointerUpEvent ||
            event is PointerCancelEvent ||
            ((event is PointerDownEvent ||
                    event is PointerMoveEvent ||
                    event is PointerHoverEvent) &&
                event.buttons == 0);
        if (released && event.timeStamp.inMicroseconds >= ignoredSince) {
          _ignoredMice.remove(event.device);
          _heldMice.remove(event.device);
        }
        // Consume even the ending event: an old device's up must not release
        // the existing view's global button state for a different new mouse.
        return true;
      }
      if (active && event.timeStamp.inMicroseconds < _time) return true;
      final explicit =
          event is PointerDownEvent ||
          event is PointerScrollEvent ||
          (event is PointerMoveEvent && event.buttons != 0);
      if (active && explicit) {
        // Fence queued pen work first. These ups belong to the new pointer
        // epoch and must not be discarded along with the old queued work.
        takeOver();
        release();
      } else if (active) {
        _observeMouse(event);
        return true;
      }
      _observeMouse(event);
      return false;
    }
    if (event.kind != PointerDeviceKind.touch) return false;
    final ending = event is PointerUpEvent || event is PointerCancelEvent;
    if (_ignoredFingers.contains(event.pointer)) {
      if (ending) _ignoredFingers.remove(event.pointer);
      return true;
    }
    if (_suppressFingers) {
      _forwardedFingers.remove(event.pointer);
      if (!ending) _ignoredFingers.add(event.pointer);
      return true;
    }
    if (event is PointerDownEvent) {
      if (active) {
        takeOver();
        release();
      }
      _forwardedFingers.add(event.pointer);
      return false;
    }
    if (_forwardedFingers.contains(event.pointer)) {
      if (ending) _forwardedFingers.remove(event.pointer);
      return false;
    }
    // A held/retired contact does not become a new gesture after pen exit.
    return true;
  }

  void _observeMouse(PointerEvent event) {
    if (event is PointerCancelEvent || event is PointerRemovedEvent) {
      _heldMice.remove(event.device);
    } else if (event is PointerDownEvent ||
        event is PointerMoveEvent ||
        event is PointerUpEvent ||
        event is PointerHoverEvent) {
      if (event.buttons == 0) {
        _heldMice.remove(event.device);
      } else {
        _heldMice.add(event.device);
      }
    }
  }

  void _handlePen(PointerEvent event) {
    final at = event.timeStamp.inMicroseconds;
    if (at < 0 || at < _time) return;
    final down = event is PointerDownEvent;
    final hover = event is PointerHoverEvent;
    final end =
        event is PointerUpEvent ||
        event is PointerCancelEvent ||
        event is PointerRemovedEvent ||
        event is PointerExitEvent;
    if (active) {
      if (event.device != _device || event.kind != _kind) return;
      if (_contacting &&
          event.pointer != _pointer &&
          event is! PointerRemovedEvent &&
          event is! PointerExitEvent) {
        return;
      }
      if (down && _contacting) return;
    } else {
      if (!down && !hover) return;
      if (event.device == _retiredDevice &&
          event.kind == _retiredKind &&
          at <= _retiredAt &&
          !down) {
        return;
      }
      // Proximity alone never steals an existing hardware mouse drag or an
      // already forwarded finger gesture. Explicit pen contact may take over.
      if (!down && (_heldMice.isNotEmpty || _forwardedFingers.isNotEmpty)) {
        return;
      }
      final point = project(event.localPosition);
      if (!_valid(point)) return;
      final before = _revision;
      takeOver();
      if (_revision != before) return;
      _ignoredFingers.addAll(_forwardedFingers);
      _forwardedFingers.clear();
      for (final device in _heldMice) {
        _ignoredMice[device] = at;
      }
      _heldMice.clear();
      _device = event.device;
      _kind = event.kind;
      _anchor = point;
      _revision++;
    }
    _time = at;
    if (event is PointerCancelEvent ||
        event is PointerRemovedEvent ||
        event is PointerExitEvent) {
      // Terminal metadata can omit position; never move to an invented zero.
      _cancelOwner();
      return;
    }
    if (down) {
      final point = project(event.localPosition);
      if (!_valid(point)) {
        release();
        return;
      }
      _anchor = point;
      _pointer = event.pointer;
      _contacting = true;
    } else if (event is PointerUpEvent) {
      _point(event.localPosition, anchored: true);
      _contacting = false;
      if (!_nativeReady) {
        release();
        return;
      }
    } else if (event is PointerMoveEvent || hover) {
      if (!_point(event.localPosition, anchored: _contacting)) {
        if (!_contacting) release();
        return;
      }
    } else if (!end) {
      return;
    }
    var desired = 0;
    final primary = (event.buttons & kPrimaryStylusButton) != 0;
    final secondary = (event.buttons & kSecondaryStylusButton) != 0;
    if (_contacting && !primary) desired |= kPrimaryMouseButton;
    if (primary) desired |= kSecondaryMouseButton;
    if (secondary) desired |= kMiddleMouseButton;
    if (!_nativeReady && !_contacting) desired = 0;
    _update(desired);
    // Missing native exit support uses contact-only ownership/suppression.
    if (!_nativeReady && !_contacting) _retire();
  }

  bool _point(Offset position, {required bool anchored}) {
    final point = project(position);
    if (_valid(point)) {
      _anchor = point;
      return true;
    }
    if (!anchored) return false;
    final edge = clamp(position);
    if (_valid(edge)) _anchor = edge;
    return _anchor != null;
  }

  void _update(int desired) {
    final point = _anchor;
    if (point == null) return;
    final owner = _revision;
    // The integrator flushes its pending move before each button transition.
    send({'type': 'input', 'event': 'move', 'x': point.dx, 'y': point.dy});
    if (_revision != owner || !active) return;
    for (final down in [false, true]) {
      for (final entry in const [
        (kPrimaryMouseButton, 'left'),
        (kSecondaryMouseButton, 'right'),
        (kMiddleMouseButton, 'middle'),
      ]) {
        final was = (_buttons & entry.$1) != 0;
        final next = (desired & entry.$1) != 0;
        if (was != next && next == down) {
          send({
            'type': 'input',
            'event': 'button',
            'button': entry.$2,
            'down': down,
            'x': point.dx,
            'y': point.dy,
          });
          if (_revision != owner || !active) return;
        }
      }
    }
    _buttons = desired;
  }

  void _retire() {
    _revision++;
    if (active) {
      _retiredDevice = _device;
      _retiredKind = _kind;
      _retiredAt = _time;
    }
    _device = _pointer = null;
    _kind = null;
    _contacting = false;
    _buttons = 0;
    _anchor = null;
  }

  /// Emit only this pen owner's ups; keep palm tombstones until Up/Cancel.
  void release() {
    if (_buttons != 0) _update(0);
    _retire();
  }

  void _cancelOwner() {
    if (active) takeOver();
    release();
  }

  /// Clear-only: the outer release owns the actual Host input.reset.
  /// Capability stays valid while the same native subscription is alive.
  /// A retired held mouse still needs its own observed release before reuse.
  void reset() {
    _retire();
    _forwardedFingers.clear();
    _ignoredFingers.clear();
    _heldMice.clear();
  }

  void signal(dynamic raw) {
    if (raw is! Map) return;
    if (raw.length == 2 &&
        raw['type'] == 'ready' &&
        raw['protocol'] == 'pen-signals-v1') {
      _nativeReady = true;
      return;
    }
    if (raw.length == 1 && raw['type'] == 'unavailable') {
      _nativeReady = false;
      _cancelOwner();
      return;
    }
    if (raw.length != 4 ||
        raw['type'] != 'exit' ||
        raw['device'] is! int ||
        raw['timeMicros'] is! int ||
        (raw['kind'] != 'stylus' && raw['kind'] != 'invertedStylus')) {
      return;
    }
    final time = raw['timeMicros'] as int;
    if (!_nativeReady ||
        !active ||
        raw['device'] != _device ||
        raw['kind'] != _kind!.name ||
        time < 0 ||
        time < _time) {
      return;
    }
    _time = time;
    _cancelOwner();
  }
}

/// Optional EventChannel metadata. No input, device identity or exit is replayed;
/// a second view receives only the current subscription's ready capability.
class AndroidPenSignals {
  static const _name = 'multitap/pen-input';
  static const _codec = StandardMethodCodec();
  static const _method = MethodChannel(_name, _codec);
  static const _unavailable = <String, Object>{'type': 'unavailable'};
  static const _ready = <String, Object>{
    'type': 'ready',
    'protocol': 'pen-signals-v1',
  };
  static bool _wanted = false, _needsCancel = false, _isReady = false;
  static int _generation = 0;
  static Future<void> _serial = Future<void>.value();
  static final _hub = StreamController<Object?>.broadcast(
    onListen: _listen,
    onCancel: _cancel,
  );
  static final Stream<Object?> events = Stream<Object?>.multi((sink) {
    final subscription = _hub.stream.listen(sink.add);
    if (_wanted && _isReady) sink.add(_ready);
    sink.onCancel = subscription.cancel;
  }, isBroadcast: true);

  static void _failed() {
    _isReady = false;
    if (_wanted) _hub.add(_unavailable);
  }

  static void _listen() {
    _wanted = true;
    _isReady = false;
    final owner = ++_generation;
    // Use the ordinary EventChannel listen/cancel wire, catching an absent
    // optional plugin locally instead of reporting it as a Flutter error.
    ServicesBinding.instance.defaultBinaryMessenger.setMessageHandler(_name, (
      message,
    ) async {
      if (!_wanted || owner != _generation) return null;
      if (message == null) {
        _failed();
        return null;
      }
      try {
        final raw = _codec.decodeEnvelope(message);
        if (raw is Map &&
            raw.length == 2 &&
            raw['type'] == 'ready' &&
            raw['protocol'] == 'pen-signals-v1') {
          _isReady = true;
        }
        _hub.add(raw);
      } catch (_) {
        _failed();
      }
      return null;
    });
    _serial = _serial.then((_) async {
      if (!_wanted || owner != _generation) return;
      _needsCancel = true;
      try {
        await _method
            .invokeMethod<void>('listen')
            .timeout(const Duration(seconds: 2));
      } catch (_) {
        if (_wanted && owner == _generation) _failed();
      }
    });
  }

  static void _cancel() {
    _wanted = _isReady = false;
    ++_generation;
    ServicesBinding.instance.defaultBinaryMessenger.setMessageHandler(
      _name,
      null,
    );
    _serial = _serial.then((_) async {
      if (!_needsCancel) return;
      _needsCancel = false;
      try {
        await _method
            .invokeMethod<void>('cancel')
            .timeout(const Duration(seconds: 2));
      } catch (_) {
        // Optional plugin missing, teardown failure or timeout: no retry.
      }
    });
  }
}
