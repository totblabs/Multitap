import 'dart:async';

import 'package:flutter/services.dart';

abstract class WindowCloseGuard {
  factory WindowCloseGuard.windows() = NativeWindowCloseGuard;
  void attach(Future<void> Function() onRequest);
  Future<bool> update({required bool busy, required bool dirty});
  Future<bool> close();
  void dispose();
}

class NativeWindowCloseGuard implements WindowCloseGuard {
  static const _channel = MethodChannel('multitap/window/close');
  static int _next = 0, _active = 0;
  final int _lease = ++_next;
  bool _disposed = false;

  /// Whether the last close request would also close remote windows.
  bool requestedWithSessions = false;

  @override
  void attach(Future<void> Function() onRequest) {
    _active = _lease;
    _channel.setMethodCallHandler((call) async {
      if (!_disposed && _active == _lease && call.method == 'requested') {
        final value = call.arguments;
        requestedWithSessions = value is Map && value['sessions'] == true;
        await onRequest();
      }
    });
  }

  @override
  Future<bool> update({required bool busy, required bool dirty}) async {
    if (_disposed || _active != _lease) return false;
    try {
      return await _channel.invokeMethod<bool>('update', {
            'busy': busy,
            'dirty': dirty,
          }) ??
          false;
    } on PlatformException {
      return false;
    } on MissingPluginException {
      return false;
    }
  }

  @override
  Future<bool> close() async {
    if (_disposed || _active != _lease) return false;
    try {
      return await _channel.invokeMethod<bool>('close') ?? false;
    } on PlatformException {
      return false;
    } on MissingPluginException {
      return false;
    }
  }

  @override
  void dispose() {
    if (_disposed) return;
    if (_active == _lease) {
      _active = 0;
      _channel.setMethodCallHandler(null);
      unawaited(
        _channel
            .invokeMethod<bool>('update', {'busy': false, 'dirty': false})
            .catchError((Object _) => false),
      );
    }
    _disposed = true;
  }
}
