import 'dart:convert';
import 'dart:io';

import 'user_data_path.dart';

/// The user's "하드웨어 디코딩" choice (default on): whether a stream decodes
/// H.264 on the PC's GPU and hands the picture to Flutter as a GPU texture, or
/// uses the CPU path only (troubleshooting). It is read when a stream starts, so
/// a change applies from the next connection. Kept apart from the server and
/// sign-in settings.
class DecodeSettings {
  const DecodeSettings(this.file);
  final File file;

  factory DecodeSettings.windows() {
    final root = Platform.environment['LOCALAPPDATA'];
    if (root == null || root.isEmpty) {
      throw StateError('Windows app data directory is unavailable.');
    }
    return DecodeSettings(File('$root/Multitap/decoding.json'));
  }

  /// The choice every new stream reads.
  static bool hardwareDecode = true;
  static DecodeSettings? _store;

  /// Loads the saved choice; a missing or unreadable file means the default (on).
  static Future<void> init(DecodeSettings store) async {
    _store = store;
    hardwareDecode = await store.load();
  }

  /// Applies [value] to later streams and remembers it (best effort).
  static Future<void> choose(bool value) async {
    hardwareDecode = value;
    try {
      await _store?.save(value);
    } catch (_) {
      // The choice still applies to this run.
    }
  }

  Future<bool> load() async {
    try {
      final value = jsonDecode(await file.readAsString());
      return value is Map && value['hardwareDecode'] == false ? false : true;
    } on FileSystemException {
      return true;
    } on FormatException {
      return true;
    }
  }

  Future<void> save(bool value) =>
      writeUserDataFile(file, jsonEncode({'hardwareDecode': value}));
}