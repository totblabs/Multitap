import 'dart:async';

import 'package:flutter/material.dart';
import 'package:flutter/services.dart';

/// Disables Windows IME composition only while this password field is focused.
/// It never filters, normalizes or rewrites the supplied password.
class PasswordInputScope extends StatefulWidget {
  const PasswordInputScope({super.key, required this.builder});
  final Widget Function(FocusNode) builder;

  @override
  State<PasswordInputScope> createState() => _PasswordInputScopeState();
}

class _PasswordInputScopeState extends State<PasswordInputScope> {
  static int _nextScope = 0;
  final int _scope = ++_nextScope;
  static const _channel = MethodChannel('multitap/input');
  final _focus = FocusNode();

  @override
  void initState() {
    super.initState();
    _focus.addListener(_changed);
  }

  Future<void> _setMode(bool enabled) async {
    try {
      await _channel.invokeMethod<void>('passwordMode', {
        'enabled': enabled,
        'scope': _scope,
      });
    } on MissingPluginException {
      // Design/widget tests have no native Windows window.
    } on PlatformException {
      // Input is left untouched if the OS cannot change IME mode.
    }
  }

  void _changed() => unawaited(_setMode(_focus.hasFocus));

  @override
  void dispose() {
    if (_focus.hasFocus) unawaited(_setMode(false));
    _focus.removeListener(_changed);
    _focus.dispose();
    super.dispose();
  }

  @override
  Widget build(BuildContext context) => widget.builder(_focus);
}
