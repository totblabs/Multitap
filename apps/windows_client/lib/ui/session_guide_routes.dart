import 'package:flutter/widgets.dart';

/// A popped modal can still paint its reverse transition after isCurrent
/// becomes true underneath it. Passive observation keeps the guide from
/// overlapping that surface; no route is popped or changed by this observer.
class SessionGuideRoutes extends NavigatorObserver {
  static final _owners = Expando<SessionGuideRoutes>();
  final _pending = <TransitionRoute<dynamic>>{};
  final _waiting = <VoidCallback>{};
  NavigatorState? _registered;
  bool _disposed = false;

  static bool blocking(BuildContext context) {
    final navigator = Navigator.maybeOf(context, rootNavigator: true);
    return navigator != null && _owners[navigator]?._pending.isNotEmpty == true;
  }

  /// One scoped wake after the last painted popup has actually retired.
  /// No frame polling and no subscription when this navigator is unblocked.
  static VoidCallback? whenUnblocked(BuildContext context, VoidCallback wake) {
    final navigator = Navigator.maybeOf(context, rootNavigator: true);
    final owner = navigator == null ? null : _owners[navigator];
    if (owner == null || owner._disposed || owner._pending.isEmpty) return null;
    owner._waiting.add(wake);
    return () => owner._waiting.remove(wake);
  }

  void _completed(TransitionRoute<dynamic> route) {
    if (_disposed) return;
    _pending.remove(route);
    if (_pending.isNotEmpty) return;
    final waiting = _waiting.toList();
    _waiting.clear();
    for (final wake in waiting) {
      wake();
    }
  }

  @override
  void didPush(Route<dynamic> route, Route<dynamic>? previousRoute) {
    final current = navigator;
    if (current != null && !_disposed) {
      _registered = current;
      _owners[current] = this;
    }
    _observe(route);
  }

  @override
  void didReplace({Route<dynamic>? newRoute, Route<dynamic>? oldRoute}) {
    if (newRoute != null) _observe(newRoute);
  }

  void _observe(Route<dynamic> route) {
    if (_disposed || route is! PopupRoute<dynamic> || !_pending.add(route)) {
      return;
    }
    route.completed.then(
      (_) => _completed(route),
      onError: (_) => _completed(route),
    );
  }

  void dispose() {
    _disposed = true;
    final registered = _registered;
    if (registered != null && identical(_owners[registered], this)) {
      _owners[registered] = null;
    }
    _registered = null;
    _pending.clear();
    _waiting.clear();
  }
}
