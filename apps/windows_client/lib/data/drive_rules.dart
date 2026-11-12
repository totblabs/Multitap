/// drive-v1 (docs/drive-redirection.md): the rules this client shares with
/// the broker (server/src/drive.mjs) and the Host. A name or path this file
/// refuses is never sent; one the broker would refuse would end the pair.
library;

import 'dart:io' show Platform;
import '../l10n/l10n.dart';

const driveProtocol = 'drive-v1';
const driveMaxShares = 8;
const driveChunkBytes = 262144;
const driveMaxInFlight = 8;
const driveListPage = 256;
const driveMaxPath = 1024;
const driveMaxName = 255;
const driveMaxDepth = 64;
const driveMaxLabel = 64;

/// Every JSON message this client sends stays within the Host's control-JSON
/// bound (64 KiB) with room to spare.
const driveJsonLimit = 60000;

/// Snapshot entries one directory listing keeps (the Host lists at most this).
const driveMaxListEntries = 20000;

/// The latest valid time (9999-12-31T23:59:59.999Z) and size the wire takes.
const driveMaxTime = 253402300799999;
const driveMaxSize = 9007199254740991;

/// File request outcomes a client (and the relay) may report.
const driveErrorCodes = {
  'drive_not_found',
  'drive_exists',
  'drive_not_empty',
  'drive_read_only',
  'drive_denied',
  'drive_link_refused',
  'drive_invalid_path',
  'drive_share_unknown',
  'drive_busy',
  'drive_no_space',
  'drive_is_directory',
  'drive_not_directory',
  'drive_io_failed',
};

/// The Host's per-share mapping failures (`drive.mounted`).
const driveMountErrorCodes = {
  'drive_webclient_unavailable',
  'drive_letter_unavailable',
  'drive_map_failed',
  'no_interactive_session',
};

final driveShareIdPattern = RegExp(r'^[A-Za-z0-9_-]{1,32}$');

/// The operations that change something on this PC.
const driveModifyingOps = {
  'create',
  'rename',
  'delete',
  'touch',
  'write',
  'setsize',
};

bool _wellFormed(String text) {
  for (var i = 0; i < text.length; i++) {
    final unit = text.codeUnitAt(i);
    if (unit >= 0xd800 && unit <= 0xdbff) {
      if (++i == text.length) return false;
      final low = text.codeUnitAt(i);
      if (low < 0xdc00 || low > 0xdfff) return false;
    } else if (unit >= 0xdc00 && unit <= 0xdfff) {
      return false;
    }
  }
  return true;
}

final _forbidden = RegExp(r'[\x00-\x1f<>:"/\\|?*]');
final _device = RegExp(
  r'^(?:CON|PRN|AUX|NUL|CONIN\$|CONOUT\$|(?:COM|LPT)[1-9¹²³])$',
);
final _trailingDotsSpaces = RegExp(r'[ .]+$');

/// One plain Windows file name: the rule of server/src/drive.mjs
/// `safeDriveName` (1..[max] UTF-16 units, well formed, no control characters
/// or `<>:"/\|?*`, not ending in a dot or space, not `.`/`..`, not a DOS
/// device name with or without an extension).
bool driveSafeName(Object? name, [int max = driveMaxName]) {
  if (name is! String ||
      name.isEmpty ||
      name.length > max ||
      !_wellFormed(name) ||
      _forbidden.hasMatch(name) ||
      name.endsWith('.') ||
      name.endsWith(' ') ||
      name == '.' ||
      name == '..') {
    return false;
  }
  final stem = name
      .split('.')
      .first
      .replaceAll(_trailingDotsSpaces, '')
      .toUpperCase();
  return !_device.hasMatch(stem);
}

/// `''` (the share root) or 1..64 safe segments joined with `/`, at most
/// 1024 UTF-16 units (server/src/drive.mjs `safeDrivePath`).
bool driveSafePath(Object? path) {
  if (path == '') return true;
  if (path is! String || path.length > driveMaxPath) return false;
  final segments = path.split('/');
  return segments.length <= driveMaxDepth && segments.every(driveSafeName);
}

/// Folders that deserve a plain warning before they are shared (decided in
/// docs/drive-redirection.md "Case matrix"): sharing is never refused for
/// them (the VM gets only the rights of the person at this PC), but the
/// person is told what the VM will be able to touch.
enum DriveRisk { none, driveRoot, system, profile, network }

/// What [path] is, by its text alone (no disk access): a whole drive, the
/// Windows or program folders, the whole user profile or its AppData, or a
/// network folder. [environment] defaults to the process's.
DriveRisk driveRiskOf(String path, {Map<String, String>? environment}) {
  String normal(String value) {
    var text = value.replaceAll('/', r'\');
    if (text.startsWith(r'\\?\UNC\')) text = r'\\' + text.substring(8);
    if (text.startsWith(r'\\?\')) text = text.substring(4);
    while (text.length > 1 && text.endsWith(r'\')) {
      text = text.substring(0, text.length - 1);
    }
    return text.toLowerCase();
  }

  final env = <String, String>{
    for (final entry in (environment ?? Platform.environment).entries)
      entry.key.toUpperCase(): entry.value,
  };
  final shared = normal(path);
  if (RegExp(r'^[a-z]:$').hasMatch(shared)) return DriveRisk.driveRoot;
  bool within(String? parent, {bool strict = false}) {
    if (parent == null || parent.isEmpty) return false;
    final base = normal(parent);
    if (base.isEmpty || RegExp(r'^[a-z]:$').hasMatch(base)) return false;
    return shared == base ? !strict : shared.startsWith('$base\\');
  }

  final drive = normal(env['SYSTEMDRIVE'] ?? r'c:');
  if (within(env['SYSTEMROOT'] ?? env['WINDIR']) ||
      within(env['PROGRAMFILES']) ||
      within(env['PROGRAMFILES(X86)']) ||
      within(env['PROGRAMW6432']) ||
      within(env['PROGRAMDATA']) ||
      shared == '$drive\\users' ||
      shared == '$drive\\recovery' ||
      shared.startsWith('$drive\\system volume information')) {
    return DriveRisk.system;
  }
  final profile = env['USERPROFILE'];
  if (profile != null && profile.isNotEmpty) {
    final base = normal(profile);
    if (shared == base ||
        shared == '$base\\appdata' ||
        shared.startsWith('$base\\appdata\\')) {
      return DriveRisk.profile;
    }
  }
  if (shared.startsWith(r'\\')) return DriveRisk.network;
  return DriveRisk.none;
}

/// A label for a picked folder: its name, or the drive letter of a volume
/// root; trimmed to a safe name of at most [driveMaxLabel] units.
String driveLabelFor(String path, [String? suggested]) {
  String clean(String value) {
    var text = value.replaceAll(_forbidden, '_').trim();
    if (text.length > driveMaxLabel) {
      text = text.substring(0, driveMaxLabel);
      // Never end inside a surrogate pair.
      final last = text.codeUnitAt(text.length - 1);
      if (last >= 0xd800 && last <= 0xdbff) {
        text = text.substring(0, text.length - 1);
      }
    }
    return text.replaceAll(_trailingDotsSpaces, '');
  }

  for (final candidate in [
    ?suggested,
    path.replaceAll(RegExp(r'[\\/]+$'), '').split(RegExp(r'[\\/]')).last,
  ]) {
    if (RegExp(r'^[A-Za-z]:?$').hasMatch(candidate)) {
      return candidate[0].toUpperCase();
    }
    final text = clean(candidate);
    if (driveSafeName(text, driveMaxLabel)) return text;
  }
  return l10n.folder;
}
