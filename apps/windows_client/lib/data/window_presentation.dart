import 'dart:async';

import 'package:flutter/foundation.dart';
import 'package:flutter/services.dart';

class WindowPresentation extends ChangeNotifier {
  static const _channel = MethodChannel('multitap/window');
  static final _events = const EventChannel('multitap/window/events')
      .receiveBroadcastStream();
  static int _next = 0, _active = 0;
  final int _lease = ++_next;
  StreamSubscription<dynamic>? _subscription;
  bool fullscreen = false;
  int displayRevision = 0;
  bool _disposed = false;

  /// The user maximized the window (button, caption double-click, Win+Up)
  /// while [maximizeToFullscreen] is on; the view decides to go fullscreen.
  VoidCallback? onMaximizeRequested;

  /// The user drags the window's border or caption (Windows' size/move
  /// loop); the remote view asks for the final size once it ends.
  bool sizeMoving = false;
  ValueChanged<bool>? onSizeMove;

  bool _maximizeOptIn = false;

  /// Remote views treat maximize as fullscreen, like remote desktop clients.
  Future<void> maximizeToFullscreen(bool enabled) async {
    if (_disposed || _active != _lease) return;
    _maximizeOptIn = enabled;
    try {
      await _channel.invokeMethod<void>('maximizeToFullscreen', enabled);
    } on MissingPluginException {
      // Widget tests/non-Windows previews have no native window.
    }
  }

  Future<void> enter({bool fullscreen = true, bool allMonitors = false}) async {
    _active = _lease;
    _subscription ??= _events.listen((event) {
      if (!_disposed && _active == _lease && event is bool) {
        this.fullscreen = event;
        notifyListeners();
      } else if (!_disposed &&
          _active == _lease &&
          event is Map &&
          event['type'] == 'displayLayoutChanged') {
        displayRevision++;
        notifyListeners();
      } else if (!_disposed &&
          _active == _lease &&
          event is Map &&
          event['type'] == 'maximizeRequested') {
        onMaximizeRequested?.call();
      } else if (!_disposed &&
          _active == _lease &&
          event is Map &&
          event['type'] == 'sizeMove' &&
          event['active'] is bool) {
        sizeMoving = event['active'] as bool;
        onSizeMove?.call(sizeMoving);
      }
    }, onError: (_) {});
    await set(fullscreen, allMonitors: allMonitors);
  }

  Future<void> set(bool value, {bool allMonitors = false}) async {
    if (_disposed || _active != _lease) return;
    try {
      final applied = allMonitors && value
          ? await _channel.invokeMethod<bool>('allMonitors', true)
          : await _channel.invokeMethod<bool>('fullscreen', value);
      if (!_disposed && _active == _lease) {
        fullscreen = applied ?? false;
        notifyListeners();
      }
    } on MissingPluginException {
      // Widget tests/non-Windows previews have no native window.
    }
  }

  /// Flashes the taskbar button until the window is in front (a document the
  /// VM printed waits for the person). No-op when it already is in front.
  Future<void> flash() async {
    if (_disposed || _active != _lease) return;
    try {
      await _channel.invokeMethod<void>('flash');
    } on MissingPluginException {
      // A preview has no native window.
    } on PlatformException {
      // Not worth more than a missing flash.
    }
  }

  Future<void> minimize() async {
    if (_disposed || _active != _lease) return;
    try {
      await _channel.invokeMethod<void>('minimize');
    } on MissingPluginException {
      // A preview has no native window.
    }
  }

  @override
  void dispose() {
    _disposed = true;
    onMaximizeRequested = null;
    onSizeMove = null;
    unawaited(_subscription?.cancel());
    if (_active == _lease) {
      _active = 0;
      unawaited(
        _channel.invokeMethod<void>('restore').catchError((Object _) {}),
      );
      if (_maximizeOptIn) {
        unawaited(
          _channel
              .invokeMethod<void>('maximizeToFullscreen', false)
              .catchError((Object _) {}),
        );
      }
    }
    super.dispose();
  }
}
