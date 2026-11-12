import 'dart:async';

import 'package:flutter/services.dart';
import '../l10n/l10n.dart';

/// The words of the client's notification-area notices (balloons).
class TrayNotices {
  const TrayNotices._();

  /// Shown once, the first time the window rests in the tray.
  static String get runsInTray =>
      l10n.multitapKeepsRunningTrayClose;

  /// Shown only while the window rests in the tray and the sign-in lapsed and
  /// needs the password; a click on the balloon brings the window back.
  static String get signInLapsed => l10n.signExpiredClickSignAgain;
}

/// The client's hand-off to the Windows notification area (2026-10-27).
///
/// Closing the main window (X, Alt+F4) rests the client in the tray instead of
/// ending it; the native side hides the window and tells the app here. The tray
/// icon, its menu and the 완전 종료 command live natively; the app only learns
/// that the window went to the tray (for the one-time notice), can raise a
/// balloon, and asks whether remote windows are open.
abstract class ClientTray {
  factory ClientTray.windows() = NativeClientTray;

  /// [onHidden] runs each time the window has just gone to the tray.
  void attach(Future<void> Function() onHidden);

  /// A balloon from the tray icon. Shown only while the window rests in the
  /// tray; returns whether it was shown.
  Future<bool> balloon(String text);

  /// Whether remote session windows are open (exiting would end them).
  Future<bool> hasRemoteSessions();

  void dispose();
}

class NativeClientTray implements ClientTray {
  static const _channel = MethodChannel('multitap/tray');
  static const _window = MethodChannel('multitap/window');
  static int _next = 0, _active = 0;
  final int _lease = ++_next;
  bool _disposed = false;

  @override
  void attach(Future<void> Function() onHidden) {
    _active = _lease;
    _channel.setMethodCallHandler((call) async {
      if (!_disposed && _active == _lease && call.method == 'hidden') {
        await onHidden();
      }
    });
  }

  @override
  Future<bool> balloon(String text) async {
    if (_disposed || _active != _lease) return false;
    try {
      return await _channel.invokeMethod<bool>('balloon', {
            'title': 'Multitap',
            'text': text,
          }) ??
          false;
    } on PlatformException {
      return false;
    } on MissingPluginException {
      return false;
    }
  }

  @override
  Future<bool> hasRemoteSessions() async {
    try {
      return await _window.invokeMethod<bool>('hasSessionWindows') ?? false;
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
    }
    _disposed = true;
  }
}
