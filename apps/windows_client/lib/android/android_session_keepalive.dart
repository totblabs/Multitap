import 'dart:math';
import 'dart:async';

import 'package:flutter/services.dart';

/// Process priority support only: this does not prove relay/session readiness.
enum AndroidSessionKeepaliveState {
  started,
  startedWithoutNotificationPermission,
  background,
  failed,
  superseded,
}

class AndroidSessionKeepalive {
  static const channel = MethodChannel('multitap/android/session-keepalive');
  static AndroidSessionLease? _latest;
  static bool _listening = false;

  static void _listen() {
    if (_listening) return;
    _listening = true;
    channel.setMethodCallHandler((call) async {
      if (call.method != 'disconnect' || call.arguments is! Map) return false;
      final lease = _latest;
      if (lease == null ||
          lease._released ||
          (call.arguments as Map)['lease'] != lease._id ||
          lease._onDisconnect == null) {
        return false;
      }
      try {
        await (lease._disconnecting ??= Future<void>.sync(lease._onDisconnect));
        return true;
      } catch (_) {
        return false;
      }
    });
  }

  /// Call from a foreground user connection; release even if connection fails.
  static Future<AndroidSessionLease> acquire({
    Future<void> Function()? onDisconnect,
  }) async {
    _listen();
    final random = Random.secure();
    final id = List.generate(
      16,
      (_) => random.nextInt(256).toRadixString(16).padLeft(2, '0'),
    ).join();
    final lease = AndroidSessionLease._(id, onDisconnect);
    _latest = lease;
    try {
      final state = await channel
          .invokeMethod<String>('start', {'lease': id})
          .timeout(const Duration(seconds: 12));
      lease._state = AndroidSessionKeepaliveState.values.firstWhere(
        (value) => value.name == state,
        orElse: () => AndroidSessionKeepaliveState.failed,
      );
    } catch (_) {
      lease._state = AndroidSessionKeepaliveState.failed;
      // A late native start must not outlive a timed-out or failed request.
      try {
        await channel
            .invokeMethod<bool>('stop', {'lease': id})
            .timeout(const Duration(seconds: 2));
      } catch (_) {}
    }
    if (!identical(_latest, lease) || lease._released) {
      lease._state = AndroidSessionKeepaliveState.superseded;
      await lease.release();
    }
    return lease;
  }

  /// User-triggered only; after granting, retry acquire while foreground.
  static Future<bool> requestNotificationPermission() async {
    try {
      return await channel.invokeMethod<bool>('notificationPermission') == true;
    } catch (_) {
      return false;
    }
  }

  static Future<bool> openNotificationSettings() async {
    try {
      return await channel.invokeMethod<bool>('notificationSettings') == true;
    } catch (_) {
      return false;
    }
  }
}

class AndroidSessionLease {
  AndroidSessionLease._(this._id, this._onDisconnect);
  final String _id;
  final Future<void> Function()? _onDisconnect;
  Future<void>? _disconnecting;
  bool _released = false;
  AndroidSessionKeepaliveState _state = AndroidSessionKeepaliveState.failed;
  AndroidSessionKeepaliveState get state => _state;
  bool get started =>
      !_released &&
      (_state == AndroidSessionKeepaliveState.started ||
          _state ==
              AndroidSessionKeepaliveState
                  .startedWithoutNotificationPermission);

  Future<void> release() async {
    if (_released) return;
    _released = true;
    if (identical(AndroidSessionKeepalive._latest, this)) {
      AndroidSessionKeepalive._latest = null;
    }
    // Native also compares the lease: delayed A stop can never stop B.
    try {
      await AndroidSessionKeepalive.channel
          .invokeMethod<bool>('stop', {'lease': _id})
          .timeout(const Duration(seconds: 3));
    } catch (_) {
      /* No retry or background start. */
    }
  }
}

/// A view owns at most one service lease, including an in-flight acquisition.
/// Closing during acquisition retires the eventual lease without touching a
/// later view's lease.
class AndroidSessionKeepaliveOwner {
  AndroidSessionKeepaliveOwner({this.onDisconnect});
  final Future<void> Function()? onDisconnect;
  AndroidSessionLease? _lease;
  Future<bool>? _starting;
  int _generation = 0;

  bool get started => _lease?.started == true;

  Future<bool> start() {
    if (started) return Future.value(true);
    final pending = _starting;
    if (pending != null) return pending;
    final generation = _generation;
    final next = AndroidSessionKeepalive.acquire(onDisconnect: onDisconnect)
        .then((lease) {
          if (generation != _generation) {
            unawaited(lease.release());
            return false;
          }
          _lease = lease;
          return lease.started;
        });
    _starting = next;
    unawaited(
      next.whenComplete(() {
        if (identical(_starting, next)) _starting = null;
      }),
    );
    return next;
  }

  void stop() {
    ++_generation;
    _starting = null;
    final lease = _lease;
    _lease = null;
    if (lease != null) unawaited(lease.release());
  }
}
