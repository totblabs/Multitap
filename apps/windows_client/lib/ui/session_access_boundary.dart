import 'dart:async';

import 'package:flutter/gestures.dart';
import 'package:flutter/material.dart';
import 'package:flutter/services.dart';

import '../data/session_activity.dart';
import '../data/broker_api.dart';
import 'app_dialog.dart';
import 'app_progress.dart';
import 'app_theme.dart';
import 'korean_text.dart';
import '../l10n/l10n.dart';

/// Exact retained route objects only; no pop loop and no late future-owner pop.
class SessionAccessRoutes extends NavigatorObserver {
  SessionAccessRoutes(this.controller) {
    controller?.bindRoutes(this, close);
  }
  final SessionActivity? controller;
  final _routes = <Route<dynamic>>[];
  @override
  void didPush(Route<dynamic> route, Route<dynamic>? previousRoute) {
    _routes.add(route);
  }

  @override
  void didPop(Route<dynamic> route, Route<dynamic>? previousRoute) {
    _routes.remove(route);
  }

  @override
  void didRemove(Route<dynamic> route, Route<dynamic>? previousRoute) {
    _routes.remove(route);
  }

  @override
  void didReplace({Route<dynamic>? newRoute, Route<dynamic>? oldRoute}) {
    final index = oldRoute == null ? -1 : _routes.indexOf(oldRoute);
    if (index >= 0) {
      _routes.removeAt(index);
      if (newRoute != null) _routes.insert(index, newRoute);
    }
  }

  Future<void> close() {
    final routes = _routes
        .where((route) => !route.isFirst)
        .toList()
        .reversed
        .toList();
    final finished = Completer<void>();
    WidgetsBinding.instance.addPostFrameCallback((_) {
      for (final route in routes) {
        if (route.isActive && route.navigator == navigator) {
          navigator?.removeRoute(route);
        }
      }
      finished.complete();
    });
    WidgetsBinding.instance.ensureVisualUpdate();
    return finished.future;
  }

  void dispose() => controller?.unbindRoutes(this);
}

String _loginConfirmationMessage(LoginConfirmation challenge, {
  bool recoveryCodeConsumed = false,
}) {
  final existing = challenge.existingLogin;
  String stamp(DateTime at) {
    final local = at.toLocal();
    String two(int value) => value.toString().padLeft(2, '0');
    return '${local.year}-${two(local.month)}-${two(local.day)} ${two(local.hour)}:${two(local.minute)}';
  }

  return [
    if (existing != null) ...[
      l10n.account3(existing.username),
      l10n.text32(existing.sessionCount),
      for (final session in existing.sessions) ...[
        l10n.signed2(stamp(session.createdAt)),
        if (session.desktops.isNotEmpty)
          l10n.connectedDesktops(session.desktops.map((desktop) => desktop.name).join(', ')),
      ],
      '',
    ],
    l10n.signingHereEndsStreamingConsole,
    l10n.desktopsRunningWorkStayAs,
    if (recoveryCodeConsumed)
      l10n.recoveryCodeHasBeenUsed,
  ].join('\n');
}

/// A disposed Navigator need not complete its route future. Retire the
/// challenge from the prompt's own lifetime, while preserving an explicit yes
/// until the caller's fresh owner check and single-use API consumption.
class _LoginConfirmationPrompt extends StatefulWidget {
  const _LoginConfirmationPrompt(this.challenge, {
    this.recoveryCodeConsumed = false,
  });
  final LoginConfirmation challenge;
  final bool recoveryCodeConsumed;

  @override
  State<_LoginConfirmationPrompt> createState() =>
      _LoginConfirmationPromptState();
}

class _LoginConfirmationPromptState extends State<_LoginConfirmationPrompt> {
  bool _accepted = false;

  @override
  void dispose() {
    if (!_accepted) widget.challenge.forget();
    super.dispose();
  }

  @override
  Widget build(BuildContext context) => PopScope<bool>(
    onPopInvokedWithResult: (didPop, result) {
      if (!didPop) return;
      _accepted = result == true;
      if (!_accepted) widget.challenge.forget();
    },
    child: AppConfirmDialog(
      key: const Key('login-confirmation'),
      title: l10n.signOutElsewhereSignHere,
      message: _loginConfirmationMessage(
        widget.challenge,
        recoveryCodeConsumed: widget.recoveryCodeConsumed,
      ),
      confirmKey: const Key('confirm-account-login'),
      cancelKey: const Key('cancel-account-login'),
      confirmLabel: l10n.signOutSign,
    ),
  );
}

/// Only manual submission calls this; automatic recovery has no consent path.
Future<BrokerSession?> confirmManualLogin(
  BuildContext context, {
  required BrokerApi api,
  required bool Function() current,
  required Future<BrokerSession> Function() attempt,
  bool recoveryCodeConsumed = false,
}) async {
  try {
    return await attempt();
  } on LoginConfirmationRequired catch (required) {
    final challenge = required.challenge;
    try {
      if (!context.mounted ||
          !current() ||
          api is! LoginConfirmationBrokerApi) {
        return null;
      }
      final confirmed =
          await showDialog<bool>(
            context: context,
            builder: (_) => _LoginConfirmationPrompt(
              challenge,
              recoveryCodeConsumed: recoveryCodeConsumed,
            ),
          ) ==
          true;
      if (!confirmed || !context.mounted || !current()) return null;
      return await (api as LoginConfirmationBrokerApi).confirmLogin(challenge);
    } finally {
      challenge.forget();
    }
  }
}

/// Covers the whole Navigator, including an already opened remote route.
/// Source observers report no text, coordinates or key identities.
class SessionAccessBoundary extends StatefulWidget {
  const SessionAccessBoundary({
    super.key,
    required this.controller,
    required this.child,
  });
  final SessionActivity controller;
  final Widget child;
  static VoidCallback? _human;
  static void human() => _human?.call();
  @override
  State<SessionAccessBoundary> createState() => _SessionAccessBoundaryState();
}

class _SessionAccessBoundaryState extends State<SessionAccessBoundary> {
  void _human() => widget.controller.human();
  bool _key(KeyEvent event) {
    if (!event.synthesized) _human();
    return false;
  }

  void _pointer(PointerEvent event) {
    if (event.synthesized ||
        (SessionHumanSource.nativeRelativePointer &&
            (event is PointerMoveEvent || event is PointerHoverEvent))) {
      return;
    }
    if (event is PointerDownEvent ||
        event is PointerMoveEvent ||
        event is PointerHoverEvent ||
        event is PointerSignalEvent ||
        event is PointerUpEvent) {
      _human();
    }
  }

  @override
  void initState() {
    super.initState();
    SessionAccessBoundary._human = _human;
    SessionHumanSource.bindInput(this, () => widget.controller.check());
    HardwareKeyboard.instance.addHandler(_key);
    GestureBinding.instance.pointerRouter.addGlobalRoute(_pointer);
  }

  @override
  void dispose() {
    SessionHumanSource.unbindInput(this);
    if (SessionAccessBoundary._human == _human) {
      SessionAccessBoundary._human = null;
    }
    HardwareKeyboard.instance.removeHandler(_key);
    GestureBinding.instance.pointerRouter.removeGlobalRoute(_pointer);
    super.dispose();
  }

  @override
  Widget build(BuildContext context) => ListenableBuilder(
    listenable: widget.controller,
    builder: (context, _) {
      final state = widget.controller;
      final reason = state.reason;
      return Stack(
        fit: StackFit.expand,
        children: [
          // Removed from paint immediately, even if native/session teardown waits.
          Offstage(
            offstage: reason != null,
            child: ExcludeFocus(excluding: reason != null, child: widget.child),
          ),
          if (reason != null)
            Positioned.fill(
              child: Material(
                color: Theme.of(context).scaffoldBackgroundColor,
                child: Center(
                  child: AppDialog(
                    title: l10n.signedOutMultitap,
                    body: [
                      Text(pcLoginTerminalMessage(reason)),
                      // 확인 waits until the open connections are closed (up
                      // to half a minute): said, with a moving mark. What
                      // could not be confirmed takes its place.
                      if (state.retirementRecovering ||
                          (!state.retirementFinished &&
                              !state.retirementFailed))
                        Padding(
                          padding: const EdgeInsets.only(top: 10),
                          child: AppBusyLine(
                            l10n.sessionEndClosingConnections,
                            textKey: const Key('session-end-busy'),
                          ),
                        )
                      else if (state.retirementFailed)
                        Text(state.retirementRecoveryAvailable
                            ? l10n.notYetConfirmedAllConnections
                            : l10n.couldntConfirmAllConnectionsClosed),
                    ],
                    actions: [
                      if (state.retirementRecoveryAvailable)
                        TextButton(
                          key: const ValueKey('session-end-retry'),
                          onPressed: state.retirementRecovering
                              ? null
                              : () => unawaited(state.retryRetirement()),
                          child: Text(state.retirementRecovering
                              ? l10n.checking5
                              : l10n.checkAgain4),
                        ),
                      FilledButton(
                        key: const ValueKey('session-end-ack'),
                        onPressed: state.retirementFinished &&
                            !state.retirementRecovering
                            ? state.acknowledge
                            : null,
                        child: Text(l10n.ok),
                      ),
                    ],
                  ),
                ),
              ),
            ),
          if (state.warning)
            // Under the header (64 px), never over it.
            Positioned(
              left: 16,
              right: 16,
              top: 76,
              child: SafeArea(
                child: Align(
                  alignment: Alignment.topCenter,
                  child: _IdleWarning(
                    remaining: state.remaining,
                    onKeep: state.human,
                  ),
                ),
              ),
            ),
        ],
      );
    },
  );
}

/// One minute before the automatic sign-out: a compact notice in the app's
/// toast style (a dark card in both appearances) with the seconds left and
/// 계속 사용.
class _IdleWarning extends StatelessWidget {
  const _IdleWarning({required this.remaining, required this.onKeep});
  final Duration? remaining;
  final VoidCallback onKeep;

  @override
  Widget build(BuildContext context) {
    // The card is dark in both themes, so the dark theme's colours on it.
    const onCard = AppPalette(true);
    final left = remaining;
    // Rounded up: the last second reads 1, never 0 while time is left.
    final seconds = left == null ? null : (left.inMilliseconds / 1000).ceil();
    return ConstrainedBox(
      constraints: const BoxConstraints(maxWidth: 540),
      child: Material(
        key: const Key('idle-warning'),
        color: AppPalette.of(context).toast,
        elevation: 6,
        shadowColor: const Color(0x80000000),
        shape: RoundedRectangleBorder(
          borderRadius: BorderRadius.circular(12),
          side: const BorderSide(color: AppPalette.toastEdge),
        ),
        child: Padding(
          padding: const EdgeInsets.fromLTRB(14, 8, 8, 8),
          child: Row(
            mainAxisSize: MainAxisSize.min,
            children: [
              Icon(Icons.timer_outlined, size: 18, color: onCard.amber),
              const SizedBox(width: 10),
              Flexible(
                child: KeepAllText(
                  seconds == null || seconds <= 0
                      ? l10n.youllSignedOutSoonIf
                      : l10n.youllSignedOutInSeconds(seconds),
                  key: const Key('idle-warning-text'),
                  style: uiText(
                    size: 13,
                    height: 1.45,
                    weight: FontWeight.w500,
                    color: AppPalette.toastInk,
                  ).copyWith(
                    fontFeatures: const [FontFeature.tabularFigures()],
                  ),
                ),
              ),
              const SizedBox(width: 12),
              FilledButton(
                key: const Key('idle-warning-keep'),
                onPressed: onKeep,
                style: AppButtons.filled(onCard),
                child: KeepAllText(l10n.keepUsing),
              ),
            ],
          ),
        ),
      ),
    );
  }
}
