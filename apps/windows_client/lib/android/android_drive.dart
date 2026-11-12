import 'dart:async';

import 'package:flutter/services.dart';

import '../data/broker_api.dart';
import '../data/drive_files.dart';
import '../data/drive_rules.dart';
import '../data/drive_session.dart';
import '../ui/drive_panel.dart';
import '../l10n/l10n.dart';

/// drive-v1 from Android (docs/drive-redirection.md "Android"): a folder the
/// person picks with Android's own folder picker is a drive in the desktop
/// for this connection, through the same [DriveSession] as on Windows. The
/// engine (AndroidDrive.kt, channel `multitap/android/drive`) reaches only
/// the picked folder, through the Storage Access Framework; the app asks for
/// no storage permission.
///
/// A share's `path` is the tree URI the picker returned (never shown: the
/// panel shows [where]). What a documents provider cannot do reaches the
/// desktop as one fixed drive_* error (DriveFiles.kt).
class AndroidDriveFileSystem implements DriveFileSystem {
  AndroidDriveFileSystem({MethodChannel? channel})
    : _channel = channel ?? const MethodChannel('multitap/android/drive') {
    _files = NativeDriveFileSystem(channel: _channel);
  }
  final MethodChannel _channel;
  late final NativeDriveFileSystem _files;

  static const _codes = {...driveErrorCodes, 'drive_unavailable'};
  final _where = <String, String>{};
  final _whole = <String>{};

  /// The last share that ended by itself lost its access (the person or
  /// Android took it back), rather than its folder.
  bool lastEndRevoked = false;

  /// Why the folder picker last did not open (AndroidDrive.kt `pick`): 1 its
  /// app is turned off, 2 this device has none, 3 Android refused, 4 it could
  /// not be started, 5 it was started and never showed, 6 this app has no
  /// folder engine, 7 its app (the platform's own) is turned off, 8 its app
  /// is on and did not answer; 0 not said. Recorded with the failure and read
  /// by [androidPickerMessage].
  int lastPickFailure = 0;

  /// The picker's app is turned off: the person can turn it on.
  bool get pickerOff => lastPickFailure == 1 || lastPickFailure == 7;

  /// Opens Android's page of the picker app that is turned off (the person
  /// turns it on there); false when it could not.
  Future<bool> openPickerSettings() async {
    try {
      return await _channel.invokeMethod<bool>('pickerSettings') == true;
    } catch (_) {
      return false;
    }
  }

  /// Where the picked folder is, in words ("내부 저장공간/Documents").
  String? where(String path) => _where[path];

  /// The pick is a whole storage volume (an SD card's root).
  bool whole(String path) => _whole.contains(path);

  /// This app's own folder list with all-files access:
  /// the only way wherever that access exists (Android 11+).
  /// Android's own picker is an app that differs from phone to phone, can be
  /// turned off and is missing on a TV, so it is not used there at all. The
  /// screen that can ask sets this: it gets the access allowed where it is
  /// not yet, shows the list and answers the chosen folder's path (null: the
  /// person left).
  Future<String?> Function(AndroidDriveFileSystem files)? chooser;
  /// All-files access on this device: whether it exists (Android 11+) and
  /// whether the person allowed it.
  Future<({bool supported, bool granted})> storageAccess() async {
    try {
      final value = await _channel.invokeMapMethod<String, Object?>(
        'storageAccess',
      );
      return (
        supported: value?['supported'] == true,
        granted: value?['granted'] == true,
      );
    } catch (_) {
      return (supported: false, granted: false);
    }
  }

  /// Opens Android's page where the person allows all-files access.
  Future<bool> openStorageSettings() async {
    try {
      return await _channel.invokeMethod<bool>('storageSettings') == true;
    } catch (_) {
      return false;
    }
  }

  /// The storage volumes ([path] null) or the folders inside [path].
  Future<AndroidFolderList> storageFolders(String? path) async {
    final Map<String, Object?>? value;
    try {
      value = await _channel.invokeMapMethod<String, Object?>(
        'storageFolders',
        {'path': path},
      );
    } on PlatformException catch (error) {
      throw DriveFileException(
        _codes.contains(error.code) ? error.code : 'drive_io_failed',
      );
    } on MissingPluginException {
      throw const DriveFileException('drive_unavailable');
    }
    final folders = <({String path, String name})>[];
    final listed = value?['folders'];
    if (listed is List) {
      for (final item in listed) {
        if (item is! Map) continue;
        final at = item['path'], name = item['name'];
        if (at is String && at.isNotEmpty && name is String) {
          folders.add((path: at, name: name));
        }
      }
    }
    final at = value?['path'], where = value?['where'];
    final parent = value?['parent'];
    return AndroidFolderList(
      folders: folders,
      path: at is String ? at : null,
      where: where is String ? where : null,
      parent: parent is String ? parent : null,
    );
  }

  /// The folder the person chose in this app's own list, as the picker
  /// answers one.
  Future<Map<Object?, Object?>?> _own(String path) async {
    try {
      return await _channel.invokeMethod<Map<Object?, Object?>>(
        'storagePick',
        {'path': path},
      );
    } on PlatformException catch (error) {
      throw DriveFileException(
        _codes.contains(error.code) ? error.code : 'drive_io_failed',
      );
    } on MissingPluginException {
      throw const DriveFileException('drive_unavailable', win32: 6);
    }
  }

  /// Android's own folder picker.
  Future<Map<Object?, Object?>?> _system() async {
    try {
      return await _channel.invokeMethod<Map<Object?, Object?>>('pickFolder');
    } on PlatformException catch (error) {
      final code = _codes.contains(error.code) ? error.code : 'drive_io_failed';
      final why = error.details;
      final reason = why is int && why >= 1 && why <= 8 && why != 6 ? why : 0;
      if (code == 'drive_unavailable') lastPickFailure = reason;
      throw DriveFileException(code, win32: reason);
    } on MissingPluginException {
      lastPickFailure = 6;
      throw const DriveFileException('drive_unavailable', win32: 6);
    }
  }

  @override
  Future<DrivePick?> pickFolder() async {
    final Map<Object?, Object?>? value;
    final choose = chooser;
    // This app's own list wherever all-files access exists (Android 11+) and
    // a screen can show it; Android's picker only on older devices, which
    // have no other way.
    if (choose != null && (await storageAccess()).supported) {
      final path = await choose(this);
      if (path == null) return null;
      value = await _own(path);
    } else {
      value = await _system();
    }    final path = value?['path'], name = value?['name'];
    if (path is! String || path.isEmpty) return null;
    final where = value?['where'];
    if (where is String && where.isNotEmpty && where.length <= 300) {
      _where[path] = where;
    }
    if (value?['whole'] == true) _whole.add(path);
    return DrivePick(path, name is String ? name : '');
  }

  @override
  Future<DriveRootStatus> status(String share) async {
    final Map<Object?, Object?>? value;
    try {
      value = await _channel.invokeMethod<Map<Object?, Object?>>('status', {
        'share': share,
      });
    } on PlatformException catch (error) {
      throw DriveFileException(
        _codes.contains(error.code) ? error.code : 'drive_io_failed',
      );
    } on MissingPluginException {
      throw const DriveFileException('drive_unavailable');
    }
    final deleted = value?['deleted'], path = value?['path'];
    final name = value?['name'];
    if (deleted is! bool || path is! String || name is! String) {
      throw const DriveFileException('drive_io_failed');
    }
    if (deleted) lastEndRevoked = value?['revoked'] == true;
    return DriveRootStatus(
      deleted: deleted,
      path: path,
      name: name,
      offline: value?['offline'] == true,
    );
  }

  @override
  Future<String> open(String share, String path, {required bool readOnly}) =>
      _files.open(share, path, readOnly: readOnly);
  @override
  Future<void> close(String share) => _files.close(share);
  @override
  Future<DriveEntry> stat(String share, String path) =>
      _files.stat(share, path);
  @override
  Future<List<DriveEntry>> list(String share, String path) =>
      _files.list(share, path);
  @override
  Future<Uint8List> read(String share, String path, int offset, int length) =>
      _files.read(share, path, offset, length);
  @override
  Future<void> write(
    String share,
    String path,
    int offset,
    Uint8List data, {
    required bool truncate,
  }) => _files.write(share, path, offset, data, truncate: truncate);
  @override
  Future<void> setSize(String share, String path, int size) =>
      _files.setSize(share, path, size);
  @override
  Future<void> flush(String share, String path) => _files.flush(share, path);
  @override
  Future<void> create(
    String share,
    String path, {
    required bool dir,
    required bool truncate,
  }) => _files.create(share, path, dir: dir, truncate: truncate);
  @override
  Future<void> rename(
    String share,
    String path,
    String to, {
    required bool replace,
  }) => _files.rename(share, path, to, replace: replace);
  @override
  Future<void> delete(String share, String path) => _files.delete(share, path);
  @override
  Future<void> touch(String share, String path, int modified) =>
      _files.touch(share, path, modified);
}

/// One level of this app's own folder list ([AndroidDriveFileSystem.storageFolders]).
class AndroidFolderList {
  const AndroidFolderList({
    required this.folders,
    this.path,
    this.where,
    this.parent,
  });
  final List<({String path, String name})> folders;

  /// The folder listed (null: the volumes), where it is in words, and the
  /// folder above it (null at a volume's top).
  final String? path, where, parent;
}

/// The app's one folder engine (one method channel per Flutter engine).
final androidDriveFiles = AndroidDriveFileSystem();

/// drive-v1 where the broker granted it: the same session code as Windows,
/// over this device's picked folders. Nothing is remembered between
/// connections: every share starts with the picker.
DriveSession androidDriveSession(
  BrokerConnection connection,
  String token, {
  AndroidDriveFileSystem? files,
}) => DriveSession(
  url: connection.driveWebSocketUrl!,
  token: token,
  sessionId: connection.sessionId,
  files: files ?? androidDriveFiles,
  fs: connection.driveFs,
);

/// What the user reads for a fixed drive code on Android: this device's own
/// reasons where they differ from a Windows PC's.
String androidDriveMessage(String? code) => switch (code) {
  'drive_denied' => l10n.noPermissionOpenFolderChoose,
  'drive_not_found' => l10n.cantFindFolderDeviceMay,
  'drive_unavailable' => androidPickerMessage(2),
  _ => driveMessage(code),
};

/// Why Android's folder picker did not open, by
/// [AndroidDriveFileSystem.lastPickFailure]: what the person can do about it
/// where there is something, never "this device cannot" for a passing failure.
String androidPickerMessage(int reason) => switch (reason) {
  1 || 7 => l10n.filesAppTurnedOffSo,
  2 || 6 => l10n.deviceHasNoFolderPicker,
  _ => l10n.couldntOpenFolderPickerTry,
};

/// The 공유 폴더 panel's words on Android (see [DriveWording]).
DriveWording androidDriveWording(AndroidDriveFileSystem files) => DriveWording(
  device: l10n.device,
  message: (code) => code == 'drive_unavailable'
      ? androidPickerMessage(files.lastPickFailure)
      : androidDriveMessage(code),
  fix: (code) => code == 'drive_unavailable' && files.pickerOff
      ? (
          label: l10n.openFilesAppSettings,
          run: () => unawaited(files.openPickerSettings()),
        )
      : null,
  risk: (path) => files.whole(path)
      ? l10n.youreAboutShareAllStorage
      : null,
  path: (path) => files.where(path) ?? l10n.chosenFolder,
  gone: (name) => files.lastEndRevoked
      ? l10n.stoppedSharingBecausePermissionOpen(name)
      : l10n.stoppedSharingBecauseFolderCant(name),
  offline: l10n.cantOpenFoldersDeviceCheck,
);
