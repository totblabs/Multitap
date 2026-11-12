import 'package:flutter/services.dart';

import 'drive_rules.dart';

/// A failed file operation; [code] is one fixed drive_* code (or
/// `drive_user_unavailable` when this PC's desktop user cannot be used).
class DriveFileException implements Exception {
  const DriveFileException(this.code, {this.win32 = 0});
  final String code;

  /// The Win32 error behind [code] when the native side reported one (0: none).
  final int win32;
  @override
  String toString() => code;
}

/// One file or folder as the PC reports it. Times are ms since the epoch.
class DriveEntry {
  const DriveEntry({
    required this.name,
    required this.dir,
    this.size = 0,
    this.modified = 0,
    this.created = 0,
    this.readOnly = false,
    this.free,
    this.total,
  });
  final String name;
  final bool dir;
  final int size, modified, created;
  final bool readOnly;

  /// Volume space, reported for a share root only.
  final int? free, total;
}

/// A folder the user picked: its path and a suggested label.
class DrivePick {
  const DrivePick(this.path, this.name);
  final String path, name;
}

/// The share root's current location (see [DriveFileSystem.status]).
class DriveRootStatus {
  const DriveRootStatus({
    required this.deleted,
    required this.path,
    required this.name,
    this.offline = false,
  });
  final bool deleted;

  /// The folder's device is gone (a pulled USB stick, a dropped network
  /// share): not deleted, but nothing can be read or written until it is
  /// opened again.
  final bool offline;
  final String path, name;
}

/// The PC's side of drive-v1: folders picked by the user and the file
/// operations the VM asks for, always relative to a share root. Every
/// implementation re-validates paths and refuses links itself; on Windows
/// the native worker runs them as the desktop user (drive_channel.h).
///
/// Android: lib/android/android_drive.dart (tree URIs from
/// ACTION_OPEN_DOCUMENT_TREE through the Storage Access Framework).
abstract interface class DriveFileSystem {
  /// The folder picker; null when cancelled.
  Future<DrivePick?> pickFolder();

  /// Registers (or replaces) [share] for [path]; returns a default label.
  Future<String> open(String share, String path, {required bool readOnly});
  Future<void> close(String share);

  /// Where the shared folder is now. A share follows the folder itself: a
  /// rename or move on the PC changes [DriveRootStatus.path]; a deleted
  /// folder is [DriveRootStatus.deleted] (and a recreated one of the same
  /// name is never picked up).
  Future<DriveRootStatus> status(String share);
  Future<DriveEntry> stat(String share, String path);

  /// The whole directory at once (at most 20,000 entries); links and unsafe
  /// names are already left out.
  Future<List<DriveEntry>> list(String share, String path);
  Future<Uint8List> read(String share, String path, int offset, int length);
  Future<void> write(
    String share,
    String path,
    int offset,
    Uint8List data, {
    required bool truncate,
  });

  /// Sets a file's length exactly (shrink truncates, grow zero-fills). fs only.
  Future<void> setSize(String share, String path, int size);

  /// Makes earlier writes durable; a directory, the root or a read-only share
  /// is a no-op. fs only.
  Future<void> flush(String share, String path);
  Future<void> create(
    String share,
    String path, {
    required bool dir,
    required bool truncate,
  });
  Future<void> rename(
    String share,
    String path,
    String to, {
    required bool replace,
  });
  Future<void> delete(String share, String path);
  Future<void> touch(String share, String path, int modified);
}

/// The Windows runner's worker over "multitap/drive".
class NativeDriveFileSystem implements DriveFileSystem {
  NativeDriveFileSystem({MethodChannel? channel})
    : _channel = channel ?? const MethodChannel('multitap/drive');
  final MethodChannel _channel;

  static const _codes = {
    ...driveErrorCodes,
    'drive_user_unavailable',
    'drive_unavailable',
  };

  Future<T?> _call<T>(String method, Map<String, Object?> arguments) async {
    try {
      return await _channel.invokeMethod<T>(method, arguments);
    } on PlatformException catch (error) {
      final details = error.details;
      throw DriveFileException(
        _codes.contains(error.code) ? error.code : 'drive_io_failed',
        win32: details is int && details > 0 ? details : 0,
      );
    } on MissingPluginException {
      throw const DriveFileException('drive_unavailable');
    }
  }

  static int _int(Object? value) => value is int ? value : 0;

  static DriveEntry _entry(Object? value) {
    if (value is! Map) throw const DriveFileException('drive_io_failed');
    final name = value['name'];
    final dir = value['dir'];
    if (name is! String || dir is! bool) {
      throw const DriveFileException('drive_io_failed');
    }
    return DriveEntry(
      name: name,
      dir: dir,
      size: _int(value['size']).clamp(0, driveMaxSize),
      modified: _int(value['modified']).clamp(0, driveMaxTime),
      created: _int(value['created']).clamp(0, driveMaxTime),
      readOnly: value['readOnly'] == true,
      free: value['free'] is int ? value['free'] as int : null,
      total: value['total'] is int ? value['total'] as int : null,
    );
  }

  @override
  Future<DrivePick?> pickFolder() async {
    final value = await _call<Map<Object?, Object?>>('pickFolder', const {});
    if (value == null) return null;
    final path = value['path'], name = value['name'];
    if (path is! String || path.isEmpty) return null;
    return DrivePick(path, name is String ? name : '');
  }

  @override
  Future<String> open(
    String share,
    String path, {
    required bool readOnly,
  }) async {
    final value = await _call<Map<Object?, Object?>>('open', {
      'share': share,
      'path': path,
      'readOnly': readOnly,
    });
    final name = value?['name'];
    return name is String ? name : '';
  }

  @override
  Future<void> close(String share) => _call<void>('close', {'share': share});

  @override
  Future<DriveRootStatus> status(String share) async {
    final value = await _call<Map<Object?, Object?>>('status', {
      'share': share,
    });
    final deleted = value?['deleted'], path = value?['path'];
    final name = value?['name'];
    if (deleted is! bool || path is! String || name is! String) {
      throw const DriveFileException('drive_io_failed');
    }
    return DriveRootStatus(
      deleted: deleted,
      path: path,
      name: name,
      offline: value?['offline'] == true,
    );
  }

  @override
  Future<DriveEntry> stat(String share, String path) async =>
      _entry(await _call<Object?>('stat', {'share': share, 'path': path}));

  @override
  Future<List<DriveEntry>> list(String share, String path) async {
    final value = await _call<Map<Object?, Object?>>('list', {
      'share': share,
      'path': path,
    });
    final entries = value?['entries'];
    if (entries is! List) throw const DriveFileException('drive_io_failed');
    return [for (final item in entries.take(driveMaxListEntries)) _entry(item)];
  }

  @override
  Future<Uint8List> read(
    String share,
    String path,
    int offset,
    int length,
  ) async {
    final value = await _call<Uint8List>('read', {
      'share': share,
      'path': path,
      'offset': offset,
      'length': length,
    });
    if (value == null || value.length > length) {
      throw const DriveFileException('drive_io_failed');
    }
    return value;
  }

  @override
  Future<void> write(
    String share,
    String path,
    int offset,
    Uint8List data, {
    required bool truncate,
  }) => _call<void>('write', {
    'share': share,
    'path': path,
    'offset': offset,
    'data': data,
    'truncate': truncate,
  });

  @override
  Future<void> setSize(String share, String path, int size) =>
      _call<void>('setSize', {'share': share, 'path': path, 'size': size});

  @override
  Future<void> flush(String share, String path) =>
      _call<void>('flush', {'share': share, 'path': path});

  @override
  Future<void> create(
    String share,
    String path, {
    required bool dir,
    required bool truncate,
  }) => _call<void>('create', {
    'share': share,
    'path': path,
    'dir': dir,
    'truncate': truncate,
  });

  @override
  Future<void> rename(
    String share,
    String path,
    String to, {
    required bool replace,
  }) => _call<void>('rename', {
    'share': share,
    'path': path,
    'to': to,
    'replace': replace,
  });

  @override
  Future<void> delete(String share, String path) =>
      _call<void>('delete', {'share': share, 'path': path});

  @override
  Future<void> touch(String share, String path, int modified) => _call<void>(
    'touch',
    {'share': share, 'path': path, 'modified': modified},
  );
}
