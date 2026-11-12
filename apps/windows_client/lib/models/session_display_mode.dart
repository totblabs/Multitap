import '../l10n/l10n.dart';

enum SessionDisplayMode {
  windowed,
  fullscreen,
  allMonitors;

  const SessionDisplayMode();
  String get label => switch (this) {
    SessionDisplayMode.windowed => l10n.windowed,
    SessionDisplayMode.fullscreen => l10n.fullScreen,
    SessionDisplayMode.allMonitors => l10n.allMonitors,
  };

}
