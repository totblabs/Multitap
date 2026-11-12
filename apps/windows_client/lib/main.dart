import 'dart:async';
import 'dart:io';

import 'package:flutter/material.dart';

import 'app.dart';
import 'data/client_settings.dart';
import 'data/login_memory.dart';
import 'data/launcher_memory.dart';
import 'data/desktop_thumbnails.dart';
import 'data/appearance_settings.dart';
import 'data/decode_settings.dart';
import 'data/stream_observability.dart';
import 'ui/themed_app.dart';
import 'ui/host_settings_screen.dart';
import 'data/product_log.dart';
import 'data/product_updates.dart' show newerProductVersion;
import 'data/client_build_version.dart';
import 'data/session_windows.dart';
import 'data/update_handoff.dart';
import 'data/window_close_guard.dart';
import 'ui/session_status_panel.dart';
import 'ui/session_window.dart';
import 'l10n/l10n.dart';

Future<void> main(List<String> arguments) async {
  WidgetsFlutterBinding.ensureInitialized();
  final executableName = File(Platform.resolvedExecutable).uri.pathSegments.last
      .toLowerCase();
  final hostMode =
      arguments.contains('--host-settings') ||
      executableName == 'multitaphostsettings.exe';
  if (Platform.isWindows) await StreamObservability.initWindows();
  SessionWindowLaunch? remoteWindow;
  final childWindow = arguments.contains('--remote-session-window');
  var bootstrapFailed = false;
  if (Platform.isWindows) {
    SessionWindows.listen();
    try {
      remoteWindow = await SessionWindows.bootstrap();
      bootstrapFailed = childWindow && remoteWindow == null;
    } catch (_) {
      // Never open a sign-in/dashboard engine for a malformed child payload.
      // Show a fixed failure instead of leaving its protected HWND blank.
      bootstrapFailed = true;
    }
  }
  runningClientBuild = await ClientBuildVersion.load();
  if (Platform.isWindows) {
    try {
      await DecodeSettings.init(DecodeSettings.windows());
    } catch (_) {
      // Hardware decoding stays on (the default) without a readable choice.
    }
  }
  final appearanceSettings = AppearanceSettings.windows();
  final appearance = AppAppearance(
    mode: await appearanceSettings.load(),
    scene: await appearanceSettings.loadScene(),
    onSave: appearanceSettings.save,
    onSaveScene: appearanceSettings.saveScene,
  );
  // "언어 / Language", shared by the client, its session windows and Host
  // settings (each process reads it when it starts).
  AppLanguageController.instance.configure(
    await appearanceSettings.loadLanguage(),
    save: appearanceSettings.saveLanguage,
  );
  if (remoteWindow != null || childWindow) {
    ProductLog.instance.forwardTo(SessionWindows.forwardLog);
  } else if (Platform.isWindows) {
    final local = Platform.environment['LOCALAPPDATA'];
    if (local != null && local.isNotEmpty) {
      await ProductLog.instance.initialize(
        Directory(
          '$local${Platform.pathSeparator}Multitap${Platform.pathSeparator}Logs${Platform.pathSeparator}${hostMode ? 'HostSettings' : 'Client'}',
        ),
      );
    }
  }
  ProductLog.instance.record(LogEvent.appStarted);
  // After an update the installer reopened this app: one non-blocking notice
  // (update-handoff-v1). Session windows never consume it.
  if (Platform.isWindows && remoteWindow == null && !childWindow) {
    final handoff = await UpdateHandoff.take();
    if (handoff != null && handoff.product == (hostMode ? 'host' : 'client')) {
      UpdateNotices.instance.show(handoff);
    }
  }
  if (bootstrapFailed) {
    runApp(
      ThemedApp(
        appearance: appearance,
        title: 'Multitap',
        home: SessionBootstrapFailure(sessionWindow: childWindow),
      ),
    );
    return;
  }
  if (remoteWindow != null) {
    runApp(
      ThemedApp(
        appearance: appearance,
        title: '${remoteWindow.name} · Multitap',
        home: SessionWindow(
          launch: remoteWindow,
          preferences: FileLauncherMemory.windows(),
        ),
      ),
    );
    return;
  }
  if (arguments.contains('--host-settings') ||
      executableName == 'multitaphostsettings.exe') {
    runApp(
      ThemedApp(
        appearance: appearance,
        title: l10n.multitapHostSettings,
        // The header's update button reads only the configured server's
        // public catalog; installing stays the user's choice in the panel.
        home: HostSettingsScreen(
          updateCheck: (server, version) =>
              newerProductVersion(server, 'host', version),
        ),
      ),
    );
    return;
  }
  // The server origin is a per-user setting (this app runs with the user's own
  // token); update installers are accepted by their release signature.
  final settings = ClientSettings.windows();
  final origin = await settings.load();
  final loginMemory = WindowsLoginMemory.windows();
  unawaited(loginMemory.deleteRetiredLogoutOption());
  UpdateNotices.instance.configure(
    server: origin?.toString(),
    currentVersion: clientProductVersion,
  );
  runApp(
    MultitapApp(
      appearance: appearance,
      serverOrigin: origin,
      onServerChanged: settings.save,
      loginMemory: loginMemory,
      launcherMemory: FileLauncherMemory.windows(),
      thumbnails: DesktopThumbnails.windows(),
      updateCheck: (server) =>
          newerProductVersion(server, 'client', clientProductVersion),
      confirmExit: true,
    ),
  );
}

class SessionBootstrapFailure extends StatefulWidget {
  const SessionBootstrapFailure({
    super.key,
    required this.sessionWindow,
    this.guard,
  });
  final bool sessionWindow;
  final WindowCloseGuard? guard;
  @override
  State<SessionBootstrapFailure> createState() =>
      _SessionBootstrapFailureState();
}

class _SessionBootstrapFailureState extends State<SessionBootstrapFailure> {
  late final _guard = widget.guard ?? WindowCloseGuard.windows();
  bool _closing = false;
  @override
  void initState() {
    super.initState();
    _guard.attach(_close);
    unawaited(_guard.update(busy: false, dirty: false));
    SessionWindows.bindRetirement(_close);
    if (widget.sessionWindow) {
      unawaited(
        SessionWindows.channel
            .invokeMethod<void>('ready')
            .catchError((Object _) {}),
      );
    }
  }

  Future<void> _close() async {
    if (_closing) return;
    _closing = true;
    if (!await _guard.close()) _closing = false;
  }

  @override
  void dispose() {
    SessionWindows.bindRetirement(null);
    _guard.dispose();
    super.dispose();
  }

  // The same card as a failed connection, on the session window's dark stage.
  @override
  Widget build(BuildContext context) => Scaffold(
    backgroundColor: const Color(0xFF080D16),
    body: SessionStatusPanel(
      title: l10n.couldntPrepareRemoteWindow,
      machine: l10n.multitapRemoteWindow,
      message: l10n.closeWindowConnectAgainFrom,
      backLabel: l10n.close,
      onBack: () => unawaited(_close()),
    ),
  );
}
