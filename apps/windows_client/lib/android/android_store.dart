import '../data/broker_transport.dart';

import 'dart:convert';

import 'package:flutter/material.dart';
import 'package:flutter/services.dart';

import '../data/appearance_settings.dart';
import '../data/launcher_memory.dart' show OrganizePreference;
import '../data/login_memory.dart' show passwordMemoryAllowed;
import '../models/stream_options.dart';
import 'android_screen.dart';
import 'android_touch.dart';
import '../l10n/l10n.dart';

export 'android_screen.dart';
export 'android_touch.dart' show AndroidTouchMode, AndroidPointerSpeed;

class AndroidLoginMemory {
  const AndroidLoginMemory(this.username, this.password);
  final String username;
  final String? password;
}

/// The streaming quality this device remembers between connections. The
/// resolution is never stored: every connection measures the actual screen.
/// Sound is no choice: every connection asks for it (PC and Android
/// alike).
class AndroidStreamQuality {
  const AndroidStreamQuality({this.fps = 60, this.bitrate = automatic});
  final int fps, bitrate;

  /// Used only without a valid saved choice; a saved choice is kept. 60 fps
  /// with the automatic rate, like Windows.
  static const recommended = AndroidStreamQuality();

  /// [bitrate] value for the automatic rate (bitrate-adaptive-v1 where the
  /// desktop supports it: the Host follows what the network carries; the
  /// released fixed 20 Mbps elsewhere). Mbps otherwise.
  static const automatic = 0;
  static const bitrates = [
    automatic,
    4,
    8,
    12,
    16,
    20,
    30,
    40,
    50,
    60,
    80,
    100,
  ];

  StreamOptions get options => StreamOptions(
    // Quality memory stores no geometry. Each route supplies its measured
    // workspace; negotiated decoder capacity belongs to the connection.
    fps: fps,
    bitrate: bitrate == automatic ? 20000000 : bitrate * 1000000,
    monitor: '0',
    bitrateMode: bitrate == automatic
        ? StreamBitrateMode.auto
        : StreamBitrateMode.fixed,
  );

  /// The choices [options] make, within this device's choices (an unknown
  /// rate reads as 자동, an unknown frame rate as 60 fps).
  static AndroidStreamQuality of(StreamOptions options) {
    final mbps = options.bitrate ~/ 1000000;
    return AndroidStreamQuality(
      fps: options.fps == 30 ? 30 : 60,
      bitrate: options.automaticBitrate || !bitrates.contains(mbps)
          ? automatic
          : mbps,
    );
  }

  static String bitrateLabel(int value) =>
      value == automatic ? l10n.auto : '$value Mbps';

  String get label => '$fps fps · ${bitrateLabel(bitrate)}';

  /// A saved choice; its `audio` (an earlier version's, possibly false) is
  /// read past and never applied.
  static AndroidStreamQuality parse(String? value) {
    try {
      final data = jsonDecode(value ?? '');
      if (data is Map &&
          [30, 60].contains(data['fps']) &&
          bitrates.contains(data['bitrate']) &&
          (data['audio'] == null || data['audio'] is bool)) {
        return AndroidStreamQuality(
          fps: data['fps'] as int,
          bitrate: data['bitrate'] as int,
        );
      }
    } on FormatException {
      // Fall through to the defaults.
    }
    return recommended;
  }

  /// `audio` stays in the saved form, always true: an earlier version of the
  /// app reads a choice only with it.
  String encode() => jsonEncode({'fps': fps, 'bitrate': bitrate, 'audio': true});
}

/// The screen edge the session's tool bubble rests on.
enum AndroidBubbleEdge {
  left,
  top,
  right,
  bottom;

  static AndroidBubbleEdge parse(String? value) => values.firstWhere(
    (edge) => edge.name == value,
    orElse: () => AndroidBubbleEdge.right,
  );
}

/// Remote-session choices this device remembers (not the server's).
class AndroidSessionSettings {
  const AndroidSessionSettings({
    this.touch = AndroidTouchMode.trackpad,
    this.speed = AndroidPointerSpeed.standard,
    this.toolbar = defaultToolbar,
    this.toolbarEdge = AndroidBubbleEdge.right,
    this.helpSeen = false,
    this.screens = const {},
  });
  final AndroidTouchMode touch;

  /// Pointer mode's cursor travel (정밀/표준/빠름).
  final AndroidPointerSpeed speed;

  /// Where the session's tool bubble rests: on [toolbarEdge], [toolbar] of
  /// the way along it (0 top or left .. 1 bottom or right).
  final double toolbar;
  final AndroidBubbleEdge toolbarEdge;
  static const defaultToolbar = .62;

  /// The gesture overview was shown once already.
  final bool helpSeen;

  /// 화면 배율, 해상도 and 화면 방향 per kind of screen (a Fold's cover and
  /// inner screen differ); a kind not chosen yet uses 자동.
  final Map<AndroidDeviceClass, AndroidScreenChoice> screens;

  AndroidScreenChoice screen(AndroidDeviceClass kind) =>
      screens[kind] ??
      (kind == AndroidDeviceClass.television
          ? const AndroidScreenChoice(orientation: AndroidOrientation.landscape)
          : const AndroidScreenChoice());

  AndroidSessionSettings withScreen(
    AndroidDeviceClass kind,
    AndroidScreenChoice choice,
  ) => copyWith(screens: {...screens, kind: choice});

  AndroidSessionSettings copyWith({
    AndroidTouchMode? touch,
    AndroidPointerSpeed? speed,
    double? toolbar,
    AndroidBubbleEdge? toolbarEdge,
    bool? helpSeen,
    Map<AndroidDeviceClass, AndroidScreenChoice>? screens,
  }) => AndroidSessionSettings(
    touch: touch ?? this.touch,
    speed: speed ?? this.speed,
    toolbar: toolbar ?? this.toolbar,
    toolbarEdge: toolbarEdge ?? this.toolbarEdge,
    helpSeen: helpSeen ?? this.helpSeen,
    screens: screens ?? this.screens,
  );
}

/// Saved appearance; [mode] is null until the user picks one (the device's
/// own dark/light setting applies until then).
class AndroidAppearanceMemory {
  const AndroidAppearanceMemory({
    this.mode,
    this.scene = BackgroundScene.peaks,
    this.language = AppLanguage.auto,
  });
  final ThemeMode? mode;
  final BackgroundScene scene;

  /// "언어 / Language" (auto: the device's language).
  final AppLanguage language;
}

class AndroidStore {
  static const channel = MethodChannel('multitap/android/store');
  Future<String?> origin() async {
    Object? value;
    try {
      value = await channel.invokeMethod<Object?>('transportSettings');
    } on MissingPluginException {
      /* Released HTTPS-only store. */
    }
    if (value != null) {
      final origin = decodeBrokerTransportSettings(value);
      BrokerTransport.restore(origin, (value as Map)['httpConsentOrigin']);
      return origin.origin;
    }
    final legacy = await channel.invokeMethod<String>('origin');
    if (legacy == null) {
      BrokerTransport.clear();
      return null;
    }
    final origin = normalizeBrokerOrigin(legacy);
    if (origin.scheme != 'https') {
      throw FormatException(l10n.checkHttpConsentAgain);
    }
    BrokerTransport.restore(origin, null);
    return origin.origin;
  }

  Future<void> saveOrigin(Uri origin) async {
    final value = encodeBrokerTransportSettings(origin);
    await channel.invokeMethod<void>('saveOrigin', {
      'origin': value['serverOrigin'],
      if (value.containsKey('httpConsentOrigin'))
        'httpConsentOrigin': value['httpConsentOrigin'],
    });
    BrokerTransport.stored(origin);
  }

  Future<AndroidLoginMemory> login(Uri origin) async {
    final value = await channel.invokeMapMethod<String, dynamic>('login', {
      'origin': origin.origin,
    });
    return AndroidLoginMemory(
      value?['username'] as String? ?? '',
      // Never a password for an HTTP server (the engine holds it back too).
      passwordMemoryAllowed(origin) ? (value?['password'] as String?) : null,
    );
  }

  Future<void> forgetPassword(Uri origin) =>
      channel.invokeMethod<void>('forgetPassword', {'origin': origin.origin});
  Future<void> saveLogin(
    Uri origin,
    String username,
    String password,
    bool remember,
  ) {
    // Over HTTP nothing is remembered; saving the ID also drops an old password.
    final keep = remember && passwordMemoryAllowed(origin);
    return channel.invokeMethod<void>('saveLogin', {
      'origin': origin.origin,
      'username': username,
      'password': keep ? password : null,
      'remember': keep,
    });
  }

  Future<Map<String, String>> _preferences() async {
    final value = await channel.invokeMapMethod<String, dynamic>('preferences');
    return {
      for (final entry in (value ?? const {}).entries)
        if (entry.value is String) entry.key: entry.value as String,
    };
  }

  Future<void> _save(String key, String value) => channel.invokeMethod<void>(
    'savePreference',
    {'key': key, 'value': value},
  );

  Future<AndroidAppearanceMemory> appearance() async {
    final values = await _preferences();
    return AndroidAppearanceMemory(
      mode: switch (values['appearance.theme']) {
        'dark' => ThemeMode.dark,
        'light' => ThemeMode.light,
        _ => null,
      },
      scene: BackgroundScene.parse(values['appearance.scene']),
      language: AppLanguage.parse(values['appearance.language']),
    );
  }

  /// Also read by the native side (notifications, folder access screens).
  Future<void> saveLanguage(AppLanguage language) =>
      _save('appearance.language', language.name);

  Future<void> saveTheme(ThemeMode mode) {
    if (mode == ThemeMode.system) throw ArgumentError.value(mode);
    return _save('appearance.theme', mode.name);
  }

  Future<void> saveScene(BackgroundScene scene) =>
      _save('appearance.scene', scene.name);

  // One card grid, the Windows launcher's (2026-11-03): a 'launcher.view'
  // saved by an earlier version is not read.
  Future<bool> launcherPreviews() async =>
      (await _preferences())['launcher.previews'] != 'off';

  Future<void> saveLauncherPreviews(bool enabled) =>
      _save('launcher.previews', enabled ? 'on' : 'off');

  Future<AndroidStreamQuality> quality() async =>
      AndroidStreamQuality.parse((await _preferences())['stream.quality']);

  Future<void> saveQuality(AndroidStreamQuality quality) =>
      _save('stream.quality', quality.encode());

  Future<AndroidSessionSettings> sessionSettings() async {
    final values = await _preferences();
    final toolbar = double.tryParse(values['session.toolbar'] ?? '');
    return AndroidSessionSettings(
      // 포인터 became the default (2026-10-26): the earlier key held the
      // former 직접 터치 default for everyone, so it is not read.
      touch: AndroidTouchMode.parse(values['session.input']),
      speed: AndroidPointerSpeed.parse(values['session.pointer']),
      toolbar: toolbar != null && toolbar >= 0 && toolbar <= 1
          ? toolbar
          : AndroidSessionSettings.defaultToolbar,
      toolbarEdge: AndroidBubbleEdge.parse(values['session.toolbar.edge']),
      helpSeen: values['session.help'] == 'seen',
      screens: {
        for (final kind in AndroidDeviceClass.values)
          if (values['session.screen.${kind.name}'] case final saved?)
            kind: AndroidScreenChoice.parse(saved).forDevice(kind),
      },
    );
  }

  Future<void> saveSessionSettings(AndroidSessionSettings settings) async {
    await _save('session.input', settings.touch.name);
    await _save('session.pointer', settings.speed.name);
    await _save('session.toolbar', settings.toolbar.toStringAsFixed(3));
    await _save('session.toolbar.edge', settings.toolbarEdge.name);
    if (settings.helpSeen) await _save('session.help', 'seen');
    for (final entry in settings.screens.entries) {
      await _save('session.screen.${entry.key.name}', entry.value.encode());
    }
  }

  /// The 분류 chip, the 상태 and 구분 filters and the sort this account last
  /// chose on this server (never the search text).
  Future<OrganizePreference> organize(Uri origin, String account) async {
    try {
      final data = jsonDecode(
        (await _preferences())['organize.${origin.origin}|$account'] ?? '',
      );
      if (data is Map && data['chip'] is String && data['sort'] is String) {
        final status = data['status'], kind = data['kind'];
        return OrganizePreference(
          chip: data['chip'] as String,
          status: status is String ? status : 'all',
          sort: data['sort'] as String,
          kind: kind is String ? kind : 'all',
        ).migrated();
      }
    } on FormatException {
      // The defaults.
    }
    return const OrganizePreference();
  }

  Future<void> saveOrganize(
    Uri origin,
    String account,
    OrganizePreference value,
  ) => _save(
    'organize.${origin.origin}|$account',
    jsonEncode({
      'chip': value.chip,
      'status': value.status,
      'sort': value.sort,
      'kind': value.kind,
    }),
  );

  /// versionName of the running APK, or null when Android cannot report it.
  Future<String?> appVersion() => channel.invokeMethod<String>('appVersion');
}
