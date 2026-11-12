import 'dart:convert';
import 'dart:io';

import 'package:flutter/material.dart';

import 'user_data_path.dart';
import '../l10n/l10n.dart';

/// The scenic wallpaper: each scene is one AI-generated image shown as the same place
/// by night (dark appearance) and by day (light appearance); the counterpart
/// of each original is graded by tools/backgrounds.
enum BackgroundScene {
  peaks(night: 'alpine-dusk.png', day: 'alpine-dusk-day.jpg'),
  meadow(night: 'alpine-daylight-night.jpg', day: 'alpine-daylight.png');

  const BackgroundScene({required this.night, required this.day});
  String get label => switch (this) {
    BackgroundScene.peaks => l10n.peaks,
    BackgroundScene.meadow => l10n.meadow,
  };

  final String night, day;

  String asset({required bool dark}) =>
      'assets/backgrounds/${dark ? night : day}';

  static BackgroundScene parse(Object? value) => BackgroundScene.values
      .firstWhere((scene) => scene.name == value, orElse: () => peaks);
}

class AppearanceSettings {
  const AppearanceSettings(this.file);
  final File file;

  factory AppearanceSettings.windows() {
    final root = Platform.environment['LOCALAPPDATA'];
    if (root == null || root.isEmpty) {
      throw StateError('Windows app data directory is unavailable.');
    }
    return AppearanceSettings(File('$root/Multitap/appearance.json'));
  }

  Future<Map<Object?, Object?>> _read() async {
    try {
      final value = jsonDecode(await file.readAsString());
      return value is Map ? value : const {};
    } on FileSystemException {
      return const {};
    } on FormatException {
      return const {};
    }
  }

  Future<ThemeMode> load() async =>
      (await _read())['theme'] == 'light' ? ThemeMode.light : ThemeMode.dark;

  Future<BackgroundScene> loadScene() async =>
      BackgroundScene.parse((await _read())['scene']);

  /// "언어 / Language": auto (the system's), ko or en.
  Future<AppLanguage> loadLanguage() async =>
      AppLanguage.parse((await _read())['language']);

  Future<void> _write(Map<String, Object> changes) async {
    final current = await _read();
    await writeUserDataFile(
      file,
      jsonEncode({
        'theme': current['theme'] == 'light' ? 'light' : 'dark',
        'scene': BackgroundScene.parse(current['scene']).name,
        // Written once chosen; a file without it means 자동 (auto).
        if (current['language'] != null)
          'language': AppLanguage.parse(current['language']).name,
        ...changes,
      }),
    );
  }

  Future<void> save(ThemeMode mode) async {
    if (mode == ThemeMode.system) throw ArgumentError.value(mode);
    await _write({'theme': mode.name});
  }

  Future<void> saveScene(BackgroundScene scene) =>
      _write({'scene': scene.name});

  Future<void> saveLanguage(AppLanguage language) =>
      _write({'language': language.name});
}

class AppAppearance extends ChangeNotifier {
  // Keep the public named arguments independent from the private mutable fields.
  AppAppearance({
    ThemeMode mode = ThemeMode.dark,
    BackgroundScene scene = BackgroundScene.peaks,
    this.onSave,
    this.onSaveScene,
  }) : _mode = mode, // ignore: prefer_initializing_formals
       _scene = scene; // ignore: prefer_initializing_formals
  ThemeMode _mode;
  BackgroundScene _scene;
  bool _saving = false;
  bool _disposed = false;
  final Future<void> Function(ThemeMode)? onSave;
  final Future<void> Function(BackgroundScene)? onSaveScene;
  ThemeMode get mode => _mode;
  BackgroundScene get scene => _scene;
  bool get saving => _saving;

  Future<void> select(ThemeMode mode) async {
    if (_disposed || _saving || mode == _mode || mode == ThemeMode.system) {
      return;
    }
    final previous = _mode;
    _mode = mode;
    _saving = true;
    notifyListeners();
    try {
      await onSave?.call(mode);
    } catch (_) {
      _mode = previous;
      rethrow;
    } finally {
      _saving = false;
      if (!_disposed) notifyListeners();
    }
  }

  Future<void> selectScene(BackgroundScene scene) async {
    if (_disposed || _saving || scene == _scene) return;
    final previous = _scene;
    _scene = scene;
    _saving = true;
    notifyListeners();
    try {
      await onSaveScene?.call(scene);
    } catch (_) {
      _scene = previous;
      rethrow;
    } finally {
      _saving = false;
      if (!_disposed) notifyListeners();
    }
  }

  @override
  void dispose() {
    _disposed = true;
    super.dispose();
  }
}

class AppearanceScope extends InheritedNotifier<AppAppearance> {
  const AppearanceScope({
    super.key,
    required AppAppearance controller,
    required super.child,
  }) : super(notifier: controller);

  static AppAppearance? maybeOf(BuildContext context) =>
      context.dependOnInheritedWidgetOfExactType<AppearanceScope>()?.notifier;
}
