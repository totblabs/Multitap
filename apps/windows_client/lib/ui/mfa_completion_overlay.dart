import 'package:flutter/material.dart';
import 'package:flutter/services.dart';

import '../data/mfa_account_host.dart';
import 'mfa_account_dialog.dart';

/// Place outside SessionAccessBoundary, with its own unauthenticated Navigator.
/// This view cannot acknowledge cleanup or authorize a new account/session.
class MfaCompletionOverlay extends StatefulWidget {
  const MfaCompletionOverlay({super.key, required this.host, required this.child});
  final MfaAccountHost host;
  final Widget child;
  @override
  State<MfaCompletionOverlay> createState() => _MfaCompletionOverlayState();
}
class _MfaCompletionOverlayState extends State<MfaCompletionOverlay> {
  final _scope = FocusScopeNode(debugLabel: 'mfa-completed-unauthed');
  @override
  void initState() { super.initState(); HardwareKeyboard.instance.addHandler(_key); }
  bool _key(KeyEvent event) {
    // Observe real local key events even when selection/copy consumes them.
    // This is presentation idleness only, never SessionActivity authority.
    if (!event.synthesized && _scope.hasFocus && widget.host.inbox.completed != null && (event is KeyDownEvent || event is KeyRepeatEvent)) {
      widget.host.human();
    }
    return false;
  }
  @override
  void dispose() { HardwareKeyboard.instance.removeHandler(_key); _scope.dispose(); super.dispose(); }
  @override
  Widget build(BuildContext context) => ListenableBuilder(listenable: widget.host, builder: (context, _) {
    final host = widget.host, receipt = host.inbox.completed;
    return Stack(fit: StackFit.expand, children: [widget.child,
      if (receipt != null) Positioned.fill(child: BlockSemantics(child: Listener(
        onPointerDown: (event) { if (!event.synthesized) { host.human(); } },
        onPointerSignal: (event) { if (!event.synthesized) { host.human(); } },
        child: FocusScope(node: _scope, autofocus: true, child: HeroControllerScope.none(child: Navigator(key: ObjectKey(receipt), onGenerateRoute: (_) => MaterialPageRoute<void>(
          builder: (_) => Material(color: Theme.of(context).colorScheme.surface,
            child: MfaCompletedRecoveryDialog(inbox: host.inbox, onFinished: () {}, canCopy: host.canCopyCompleted)))))),
      )))
    ]);
  });
}
