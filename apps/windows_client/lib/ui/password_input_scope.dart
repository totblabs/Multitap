import 'dart:async';

import 'package:flutter/material.dart';
import 'package:flutter/services.dart';

/// Disables Windows IME composition only while this password field is focused.
/// It never filters, normalizes or rewrites the supplied password.
class PasswordInputScope extends StatefulWidget {
  const PasswordInputScope({super.key, required this.builder});
  final Widget Function(FocusNode) builder;

  /// Forgets the mode last sent, so the next focus change sends it again.
  @visibleForTesting
  static void debugReset() => _PasswordFocus._sent = null;

  @override
  State<PasswordInputScope> createState() => _PasswordInputScopeState();
}

/// The one sender of password mode in this engine. It follows the real
/// primary focus: a focused field that is removed detaches its node without a
/// notification of its own, but FocusManager still reports the change.
class _PasswordFocus {
  static const _channel = MethodChannel('multitap/input');
  static final _nodes = <FocusNode>{};
  static FocusManager? _manager;
  static bool? _sent;

  static void add(FocusNode node) {
    _nodes.add(node);
    final manager = FocusManager.instance;
    if (identical(manager, _manager)) return;
    _manager?.removeListener(_sync);
    _manager = manager..addListener(_sync);
  }

  static void remove(FocusNode node) {
    _nodes.remove(node);
    _sync();
    if (_nodes.isNotEmpty) return;
    _manager?.removeListener(_sync);
    _manager = null;
  }

  static void _sync() {
    final on = _nodes.contains(FocusManager.instance.primaryFocus);
    if (on == _sent) return;
    _sent = on;
    unawaited(_send(on));
  }

  static Future<void> _send(bool enabled) async {
    try {
      await _channel.invokeMethod<void>('passwordMode', {'enabled': enabled});
    } on MissingPluginException {
      // Android and design/widget tests have no native Windows window.
    } on PlatformException {
      // Not applied: forget it, so the next focus change sends again.
      _sent = null;
    }
  }
}

class _PasswordInputScopeState extends State<PasswordInputScope> {
  final _focus = FocusNode();

  @override
  void initState() {
    super.initState();
    _PasswordFocus.add(_focus);
  }

  @override
  void dispose() {
    // The node no longer counts. If its field held the focus, the field's
    // Focus has already cleared it on detach, so password mode goes off now.
    _PasswordFocus.remove(_focus);
    _focus.dispose();
    super.dispose();
  }

  @override
  Widget build(BuildContext context) => widget.builder(_focus);
}
