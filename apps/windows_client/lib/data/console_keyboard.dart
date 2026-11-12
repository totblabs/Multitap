import 'dart:async';
import 'dart:math';

import 'package:flutter/foundation.dart';
import 'package:flutter/services.dart';

abstract class ConsoleKeyboardCapture extends ChangeNotifier {
  void Function(int keysym, bool down)? onKey;

  /// One producer: native adapters prefer this callback when installed.
  void Function(ConsoleKeyStroke event)? onPhysicalKey;
  VoidCallback? onReset;

  /// Actual current native reset/release, distinct from local claim setup.
  VoidCallback? onCaptureLost;
  bool get captured;
  bool get supported;
  Future<bool> acquire(int generation);
  Future<bool> capture(bool enabled);
  Future<void> release();
}

class ConsoleKeyStroke {
  const ConsoleKeyStroke(
    this.physical,
    this.keysym,
    this.down,
    this.qemuKeyCode,
  );
  final int physical, keysym;
  final bool down;
  final int? qemuKeyCode;
}

/// An explicit fake/unsupported adapter; never invokes an OS or channel API.
class NoConsoleKeyboardCapture extends ConsoleKeyboardCapture {
  @override
  bool get captured => false;
  @override
  bool get supported => false;
  @override
  Future<bool> acquire(int generation) async => false;
  @override
  Future<bool> capture(bool enabled) async => false;
  @override
  Future<void> release() async {
    onReset?.call();
  }
}

/// A separate keyboard-only native owner. It never starts the media engine.
class NativeConsoleKeyboard extends ConsoleKeyboardCapture {
  NativeConsoleKeyboard({MethodChannel? channel, String? owner})
    : _channel = channel ?? _defaultChannel,
      owner = owner ?? _randomOwner() {
    if (!RegExp(r'^[a-f0-9]{32}$').hasMatch(this.owner)) {
      throw ArgumentError('Console input owner');
    }
    _router(_channel)[this.owner] = this;
  }
  static const _defaultChannel = MethodChannel('multitap/console-input');
  static final _routers = Expando<Map<String, NativeConsoleKeyboard>>();
  static Map<String, NativeConsoleKeyboard> _router(MethodChannel channel) {
    var routes = _routers[channel];
    if (routes != null) return routes;
    routes = <String, NativeConsoleKeyboard>{};
    _routers[channel] = routes;
    channel.setMethodCallHandler((call) async {
      if (call.method != 'event' || call.arguments is! Map) return;
      final value = call.arguments as Map;
      final owner = value['owner'];
      if (owner is String) _routers[channel]?[owner]?._event(value);
    });
    return routes;
  }

  static String _randomOwner() {
    final random = Random.secure();
    return List.generate(
      16,
      (_) => random.nextInt(256).toRadixString(16).padLeft(2, '0'),
    ).join();
  }

  final MethodChannel _channel;
  final String owner;
  int _generation = 0, _lease = 0, _revision = 0;
  int _pendingPlatformCalls = 0;
  bool _captured = false, _supported = false, _disposed = false;
  bool _captureSubmitted = false;
  bool _capturePending = false;
  final _pendingEvents = <Map>[];
  final _held = <int, ConsoleKeyStroke>{};
  @override
  bool get captured => _captured;
  @override
  bool get supported => _supported;
  Map<String, Object> _identity({int? lease, int? generation}) => {
    'owner': owner,
    'generation': generation ?? _generation,
    'lease': lease ?? _lease,
  };
  bool _matches(Map? response, int generation, {int? lease}) =>
      response?['protocol'] == 'console-input-v1' &&
      response?['owner'] == owner &&
      response?['generation'] == generation &&
      response?['accepted'] == true &&
      response?['captured'] is bool &&
      response?['lease'] is int &&
      (lease == null
          ? (response!['lease'] as int) > 0
          : response!['lease'] == lease);

  Future<Map<Object?, Object?>?> _call(
    String method,
    Map<String, Object> arguments,
  ) {
    if (_pendingPlatformCalls >= 8) {
      return Future.error(PlatformException(code: 'input_busy'));
    }
    _pendingPlatformCalls++;
    // Only actual completion releases a slot; observer timeouts cannot create
    // an unbounded set of detached platform calls during repeated focus changes.
    return _channel
        .invokeMapMethod<Object?, Object?>(method, arguments)
        .whenComplete(() => _pendingPlatformCalls--);
  }

  void _reset() {
    _capturePending = false;
    _pendingEvents.clear();
    _held.clear();
    _captured = false;
    onReset?.call();
    if (!_disposed) notifyListeners();
  }

  @override
  Future<bool> acquire(int generation) async {
    if (_disposed ||
        generation < 1 ||
        generation > 2147483647 ||
        generation <= _generation) {
      return false;
    }
    final oldLease = _lease, oldGeneration = _generation;
    final revision = ++_revision;
    _lease = 0;
    _generation = generation;
    _captureSubmitted = false;
    _reset();
    if (oldLease > 0) await _releaseExact(oldLease, oldGeneration);
    if (_disposed || revision != _revision) return false;
    Future<Map<Object?, Object?>?>? pending;
    try {
      pending = _call('acquire', {'owner': owner, 'generation': generation});
      final result = await pending.timeout(const Duration(seconds: 2));
      if (!_matches(result, generation)) return false;
      final lease = result!['lease'] as int;
      if (_disposed || revision != _revision || generation != _generation) {
        await _releaseExact(lease, generation);
        return false;
      }
      _lease = lease;
      _supported = true;
      if (!_disposed) notifyListeners();
      return true;
    } catch (_) {
      // The observer deadline does not cancel a platform call. Its actual late
      // grant is retired by exact stamp, never adopted or applied to a successor.
      if (pending != null) {
        unawaited(
          pending
              .then((result) async {
                if (_matches(result, generation)) {
                  await _releaseExact(result!['lease'] as int, generation);
                }
              })
              .catchError((Object _) {}),
        );
      }
      return false;
    }
  }

  @override
  Future<bool> capture(bool enabled) async {
    if (!enabled) {
      await release();
      return true;
    }
    if (_captureSubmitted) return _captured;
    final generation = _generation, lease = _lease, revision = ++_revision;
    if (_disposed || lease == 0) return false;
    _captureSubmitted = true;
    _capturePending = true;
    Future<Map<Object?, Object?>?>? pending;
    try {
      pending = _call('capture', {
        ..._identity(lease: lease, generation: generation),
        'enabled': enabled,
      });
      final value = await pending.timeout(const Duration(seconds: 2));
      if (_disposed ||
          revision != _revision ||
          generation != _generation ||
          lease != _lease) {
        return false;
      }
      final accepted = _matches(value, generation, lease: lease);
      _captured = accepted && value!['captured'] == true && enabled;
      if (!_captured) {
        _reset();
      } else {
        _capturePending = false;
        final queued = _pendingEvents.toList();
        _pendingEvents.clear();
        for (final event in queued) {
          _event(event);
        }
        notifyListeners();
      }
      return enabled ? _captured : accepted;
    } catch (_) {
      if (!_disposed && revision == _revision) _reset();
      unawaited(_releaseExact(lease, generation));
      if (pending != null) {
        unawaited(
          pending
              .then((_) => _releaseExact(lease, generation))
              .catchError((Object _) {}),
        );
      }
      return false;
    }
  }

  void _event(Map value) {
    if (_disposed ||
        _lease == 0 ||
        value['owner'] != owner ||
        value['generation'] != _generation ||
        value['lease'] != _lease) {
      return;
    }
    if (value['type'] == 'input.reset' || value['type'] == 'capture.released') {
      final generation = _generation, lease = _lease, revision = ++_revision;
      _reset();
      if (!_disposed &&
          revision == _revision &&
          generation == _generation &&
          lease == _lease) {
        onCaptureLost?.call();
      }
      return;
    }
    if (_capturePending &&
        value['type'] == 'input' &&
        value['event'] == 'key') {
      if (_pendingEvents.length >= 128) {
        unawaited(release());
        return;
      }
      _pendingEvents.add(Map.of(value));
      return;
    }
    if (!_captured || value['type'] != 'input' || value['event'] != 'key') {
      return;
    }
    final vk = value['vk'],
        scan = value['scan'],
        down = value['down'],
        extended = value['extended'];
    if (vk is! int ||
        vk < 1 ||
        vk > 255 ||
        scan is! int ||
        scan < 0 ||
        scan > 511 ||
        down is! bool ||
        extended is! bool) {
      return;
    }
    // Scan/ext owns a physical press even if Windows reports another VK on
    // key-up (layout/IME or generic versus sided modifier). Zero-scan keys
    // retain a disjoint VK identity. Repeat preserves its original keysym.
    final physical = scan > 0
        ? scan * 2 + (extended ? 1 : 0)
        : 1024 + vk * 2 + (extended ? 1 : 0);
    final held = down ? _held[physical] : _held.remove(physical);
    final symbol =
        held?.keysym ??
        (down ? consoleVirtualKeysym(vk, scan, extended) : null);
    if (symbol == null) return;
    final event = ConsoleKeyStroke(
      physical,
      symbol,
      down,
      held != null ? held.qemuKeyCode : consoleQemuKeyCode(vk, scan, extended),
    );
    if (down) {
      if (_held.length >= 128 && !_held.containsKey(physical)) return;
      _held[physical] = event;
    }
    final physicalCallback = onPhysicalKey;
    if (physicalCallback != null) {
      physicalCallback(event);
    } else {
      onKey?.call(symbol, down);
    }
  }

  Future<void> _releaseExact(int lease, int generation) async {
    try {
      await _channel
          .invokeMethod<void>(
            'release',
            _identity(lease: lease, generation: generation),
          )
          .timeout(const Duration(seconds: 2));
    } catch (_) {}
  }

  @override
  Future<void> release() async {
    final lease = _lease, generation = _generation;
    ++_revision;
    _lease = 0;
    _reset();
    if (lease > 0) await _releaseExact(lease, generation);
  }

  @override
  void dispose() {
    if (_disposed) return;
    _disposed = true;
    final generation = _generation;
    unawaited(() async {
      await release();
      if (generation > 0) {
        try {
          await _channel
              .invokeMethod<void>('retireView', {
                'owner': owner,
                'generation': generation,
              })
              .timeout(const Duration(seconds: 2));
        } catch (_) {}
      }
    }());
    _routers[_channel]?.remove(owner);
    super.dispose();
  }
}

/// QEMU's RFB keycode is XT set-1 with E0 represented by bit 7.
/// Korean language keys use QEMU qnum 0x72/0x71, not the Windows 0xf2/0xf1
/// make-only scan values. Pause/E1 and unknown scans retain standard RFB.
/// Contract: QEMU v10.0.0 ui/vnc.c; noVNC v1.6.0 xtscancodes.js.
int? consoleQemuKeyCode(int vk, int scan, bool extended) {
  if (vk == 0x15) return 0x72;
  if (vk == 0x19) return 0x71;
  // Windows low-level hooks may mark NumLock extended. Its QEMU qnum is
  // still 0x45, unlike the generic E0 conversion (0xc5).
  if (vk == 0x90 && scan == 0x45) return 0x45;
  if (vk == 0x13 || scan < 1 || scan > 0x7e) return null;
  return scan | (extended ? 0x80 : 0);
}

/// VK/scan/ext emitted by RemoteInput, not an invented local key injector.
int? consoleVirtualKeysym(int vk, int scan, bool extended) {
  if (vk >= 0x41 && vk <= 0x5a) return vk + 32;
  if (vk >= 0x30 && vk <= 0x39) return vk;
  if (vk >= 0x70 && vk <= 0x87) return 0xffbe + vk - 0x70;
  if (vk >= 0x60 && vk <= 0x69) return 0xffb0 + vk - 0x60;
  return switch (vk) {
    0x08 => 0xff08,
    0x09 => 0xff09,
    0x0d => extended ? 0xff8d : 0xff0d,
    0x10 => scan == 0x36 ? 0xffe2 : 0xffe1,
    0x11 => extended ? 0xffe4 : 0xffe3,
    0x12 => extended ? 0xffea : 0xffe9,
    0xa0 => 0xffe1,
    0xa1 => 0xffe2,
    0xa2 => 0xffe3,
    0xa3 => 0xffe4,
    0xa4 => 0xffe9,
    0xa5 => 0xffea,
    0x13 => 0xff13,
    0x14 => 0xffe5,
    0x15 => 0xff31, // XK_Hangul: the remote IME owns composition.
    0x19 => 0xff34, // XK_Hangul_Hanja, not local committed text.
    0x1b => 0xff1b,
    0x20 => 0x20,
    0x21 => 0xff55,
    0x22 => 0xff56,
    0x23 => 0xff57,
    0x24 => 0xff50,
    0x25 => 0xff51,
    0x26 => 0xff52,
    0x27 => 0xff53,
    0x28 => 0xff54,
    0x2c => 0xff61,
    0x2d => 0xff63,
    0x2e => 0xffff,
    0x5b => 0xffeb,
    0x5c => 0xffec,
    0x5d => 0xff67,
    0x6a => 0xffaa,
    0x6b => 0xffab,
    0x6d => 0xffad,
    0x6e => 0xffae,
    0x6f => 0xffaf,
    0x90 => 0xff7f,
    0x91 => 0xff14,
    0xba => 0x3b,
    0xbb => 0x3d,
    0xbc => 0x2c,
    0xbd => 0x2d,
    0xbe => 0x2e,
    0xbf => 0x2f,
    0xc0 => 0x60,
    0xdb => 0x5b,
    0xdc => 0x5c,
    0xdd => 0x5d,
    0xde => 0x27,
    _ => null,
  };
}
