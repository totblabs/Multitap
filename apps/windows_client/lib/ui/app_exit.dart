import 'package:flutter/widgets.dart';

/// "완전 종료": the one way the client really ends (the tray menu and the
/// account menu). Provided by the client's main [ThemedApp]; absent in session
/// windows, Host settings and previews, where the item is not offered.
class AppExitScope extends InheritedWidget {
  const AppExitScope({
    super.key,
    required this.requestExit,
    required super.child,
  });

  /// Confirms first only when remote connections would end, then closes.
  final Future<void> Function() requestExit;

  static AppExitScope? maybeOf(BuildContext context) =>
      context.dependOnInheritedWidgetOfExactType<AppExitScope>();

  @override
  bool updateShouldNotify(AppExitScope oldWidget) => false;
}
