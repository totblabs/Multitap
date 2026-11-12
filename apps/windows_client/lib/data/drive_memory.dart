import 'dart:convert';
import 'dart:io';

import 'package:crypto/crypto.dart';

import 'drive_rules.dart';
import 'user_data_path.dart';

/// A folder the user chose to share with one VM.
class DriveFolder {
  const DriveFolder({
    required this.path,
    required this.label,
    this.readOnly = false,
  });
  final String path, label;
  final bool readOnly;

  Map<String, Object> toJson() => {
    'path': path,
    'label': label,
    'readOnly': readOnly,
  };

  static DriveFolder? fromJson(Object? value) {
    if (value is! Map) return null;
    final path = value['path'], label = value['label'];
    if (path is! String ||
        path.isEmpty ||
        path.length > 32000 ||
        label is! String ||
        !driveSafeName(label, driveMaxLabel)) {
      return null;
    }
    return DriveFolder(
      path: path,
      label: label,
      readOnly: value['readOnly'] == true,
    );
  }

  @override
  bool operator ==(Object other) =>
      other is DriveFolder &&
      other.path == path &&
      other.label == label &&
      other.readOnly == readOnly;
  @override
  int get hashCode => Object.hash(path, label, readOnly);
}

/// The folders shared with each VM, per server: nothing is shared until the
/// user picks a folder; the remembered ones are shared again on the next
/// streaming connection to that VM. Paths stay on this PC (never sent).
abstract interface class DriveMemory {
  Future<List<DriveFolder>> load(String server, String desktopId);
  Future<void> save(String server, String desktopId, List<DriveFolder> folders);
}

/// This process only (tests and previews).
class SessionDriveMemory implements DriveMemory {
  final _values = <String, List<DriveFolder>>{};
  @override
  Future<List<DriveFolder>> load(String server, String desktopId) async =>
      List.of(_values['$server\n$desktopId'] ?? const []);
  @override
  Future<void> save(
    String server,
    String desktopId,
    List<DriveFolder> folders,
  ) async {
    _values['$server\n$desktopId'] = List.of(folders);
  }
}

/// `%LOCALAPPDATA%\Multitap\Client\drives\<key>.json`, one file per server
/// and VM so two session windows never rewrite each other's choices.
class FileDriveMemory implements DriveMemory {
  FileDriveMemory(this.directory);
  final Directory directory;

  factory FileDriveMemory.windows() {
    final root = Platform.environment['LOCALAPPDATA'];
    if (root == null || root.isEmpty) {
      throw StateError('Windows app data directory is unavailable.');
    }
    return FileDriveMemory(Directory('$root/Multitap/Client/drives'));
  }

  File _file(String server, String desktopId) {
    final key = sha256
        .convert(utf8.encode('$server\n$desktopId'))
        .toString()
        .substring(0, 32);
    return File('${directory.path}/$key.json');
  }

  @override
  Future<List<DriveFolder>> load(String server, String desktopId) async {
    try {
      final value = jsonDecode(await _file(server, desktopId).readAsString());
      if (value is! Map ||
          value['server'] != server ||
          value['desktop'] != desktopId ||
          value['folders'] is! List) {
        return [];
      }
      final folders = <DriveFolder>[];
      for (final item in (value['folders'] as List).take(driveMaxShares)) {
        final folder = DriveFolder.fromJson(item);
        if (folder != null && !folders.any((f) => f.path == folder.path)) {
          folders.add(folder);
        }
      }
      return folders;
    } on FileSystemException {
      return [];
    } on FormatException {
      return [];
    }
  }

  @override
  Future<void> save(
    String server,
    String desktopId,
    List<DriveFolder> folders,
  ) async {
    final file = _file(server, desktopId);
    if (folders.isEmpty) {
      requirePlainUserDataPath(file.path);
      try {
        await file.delete();
      } on FileSystemException {
        /* Nothing remembered. */
      }
      return;
    }
    await writeUserDataFile(
      file,
      jsonEncode({
        'server': server,
        'desktop': desktopId,
        'folders': [
          for (final folder in folders.take(driveMaxShares)) folder.toJson(),
        ],
      }),
    );
  }
}
