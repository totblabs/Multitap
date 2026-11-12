import 'dart:async';
import 'dart:convert';
import 'dart:io';

import 'package:crypto/crypto.dart';
import 'package:flutter/foundation.dart';
import 'package:flutter/painting.dart';

import 'user_data_path.dart';
import '../models/thumbnail_privacy.dart';
import '../l10n/l10n.dart';

export '../models/thumbnail_privacy.dart';

// Launcher pictures of each desktop's last streamed screen.
//
// A streaming session window writes them natively (JPEG, at most 480x270)
// to %LOCALAPPDATA%\Multitap\thumbnails\<key>\<desktopId>.jpg when the stream
// ends and about once a minute while it runs. They are screen content: they
// stay in the current Windows user's LocalAppData, are never uploaded and
// never enter diagnostics or product logs. This file only reads and deletes.

final _keyPattern = RegExp(r'^[0-9a-f]{16}$');
final _desktopPattern = RegExp(
  r'^(?:pve-[0-9]{1,12}|pc-[a-f0-9]{8}-[a-f0-9]{4}-[a-f0-9]{4}-[a-f0-9]{4}-[a-f0-9]{12})$',
);

/// Whether [value] is an account key as [thumbnailAccountKey] produces it.
bool isThumbnailAccountKey(Object? value) =>
    value is String && _keyPattern.hasMatch(value);

/// Whether [value] names a desktop picture file (a VM or a local PC).
bool isThumbnailDesktopId(Object? value) =>
    value is String && _desktopPattern.hasMatch(value);

/// The account folder name: the first 16 hex digits of
/// SHA-256(broker origin + "\n" + immutable account id).
String thumbnailAccountKey(Uri origin, String accountId) => sha256
    .convert(utf8.encode('${origin.origin}\n$accountId'))
    .toString()
    .substring(0, 16);

/// From this age, or whenever the desktop is not running, a tile shows its
/// picture as a snapshot (dimmed, "저장된 마지막 화면") so an old frame does
/// not read as a live view of a frozen app.
const thumbnailSnapshotAge = Duration(minutes: 10);

/// Whether a picture taken at [taken] reads as a snapshot by [now].
bool thumbnailIsOld(DateTime taken, DateTime now) =>
    now.difference(taken) >= thumbnailSnapshotAge;

/// "마지막 화면 · 3분 전": how long ago the picture was taken; a [snapshot]
/// says plainly that it is the stored last screen ("저장된 마지막 화면 ·
/// 2시간 전").
String lastScreenCaption(
  DateTime taken,
  DateTime now, {
  bool snapshot = false,
}) => '${snapshot ? l10n.savedLastScreen : l10n.lastScreen} · ${screenAge(taken, now)}';

/// "방금", "3분 전", "2시간 전", "1일 전".
String screenAge(DateTime taken, DateTime now) {
  final age = now.difference(taken);
  return age.inMinutes < 1
      ? l10n.justNow
      : age.inHours < 1
      ? l10n.minAgo(age.inMinutes)
      : age.inDays < 1
      ? l10n.text7(age.inHours)
      : l10n.text8(age.inDays);
}

/// One desktop's picture and when it was written.
@immutable
class DesktopThumbnail {
  const DesktopThumbnail({
    required this.image,
    required this.modified,
    this.remote = false,
  });
  final ImageProvider image;
  final DateTime modified;

  /// Taken by the desktop's Host on the broker's request (desktop-preview-v1),
  /// not this PC's last streamed frame; held in memory only.
  final bool remote;
  int get encodedBytes => switch (image) {
    MemoryImage value => value.bytes.length,
    ThumbnailFileImage value => value.length,
    _ => 0,
  };

  @override
  bool operator ==(Object other) =>
      other is DesktopThumbnail &&
      other.image == image &&
      other.modified == modified &&
      other.remote == remote;

  @override
  int get hashCode => Object.hash(image, modified, remote);
}

/// A [FileImage] whose cache identity includes the file's modification time,
/// length and a revision bumped by each replacement notice, so a replaced
/// picture is decoded again instead of the image cache (or a coarse file
/// timestamp) returning the previous one for the same path.
@immutable
class ThumbnailFileImage extends FileImage {
  const ThumbnailFileImage(
    super.file, {
    required this.modified,
    required this.length,
    this.revision = 0,
  });
  final DateTime modified;
  final int length, revision;

  @override
  bool operator ==(Object other) =>
      other is ThumbnailFileImage &&
      other.file.path == file.path &&
      other.modified == modified &&
      other.length == length &&
      other.revision == revision &&
      other.scale == scale;

  @override
  int get hashCode => Object.hash(file.path, modified, length, revision, scale);
}

/// A session window replaced [desktopId]'s picture in [key]'s folder.
@immutable
class ThumbnailUpdate {
  const ThumbnailUpdate(
    this.key,
    this.desktopId, {
    this.generation = 0,
    this.invalidated = false,
    this.epoch,
  });
  final String key, desktopId;
  final String? epoch;
  final int generation;
  final bool invalidated;
}

/// Existing Flutter cache policy also bounds our retained encoded providers.
int get thumbnailCacheByteBudget {
  try {
    return PaintingBinding.instance.imageCache.maximumSizeBytes;
  } catch (_) {
    return 0;
  }
}

/// Native JPEG COM metadata, not a filename or a claimed Windows identity.
String? thumbnailJpegEpoch(List<int> bytes) {
  final prefix = ascii.encode('Multitap:$thumbnailPrivacyProtocol:');
  final payload = prefix.length + 32;
  if (bytes.length < 6 + payload + 4 ||
      bytes[0] != 0xff ||
      bytes[1] != 0xd8 ||
      bytes[2] != 0xff ||
      bytes[3] != 0xfe ||
      ((bytes[4] << 8) | bytes[5]) != payload + 2 ||
      bytes[6 + payload] != 0xff ||
      bytes[bytes.length - 2] != 0xff ||
      bytes.last != 0xd9) {
    return null;
  }
  for (var i = 0; i < prefix.length; i++) {
    if (bytes[6 + i] != prefix[i]) return null;
  }
  final epoch = String.fromCharCodes(
    bytes.sublist(6 + prefix.length, 6 + payload),
  );
  return isThumbnailPrivacyEpoch(epoch) ? epoch : null;
}

class _ThumbnailPolicy {
  String? epoch;
  bool allowed = false, revoked = false;
  void observe(ThumbnailPrivacy? next) {
    if (next == null || !isThumbnailPrivacyEpoch(next.epoch)) {
      allowed = false;
      return;
    }
    // The App admits a different service prefix only from its newest
    // authenticated list owner. The fixed-width counter orders this prefix;
    // native file notices never establish a policy or advance this watermark.
    if (epoch != null &&
        epoch!.substring(0, 16) == next.epoch.substring(0, 16)) {
      final order = next.epoch.substring(16).compareTo(epoch!.substring(16));
      if (order < 0) return;
      if (order > 0) revoked = false;
    } else {
      revoked = false;
    }
    allowed = false;
    epoch = next.epoch;
    if (!next.allowed) revoked = true;
    allowed = next.allowed && !revoked;
  }

  String? get allowedEpoch => allowed ? epoch : null;
}

class _ThumbnailRead {
  _ThumbnailRead(this.refresh);
  bool refresh;
  final done = Completer<DesktopThumbnail?>();
}

/// The pictures folder of the current Windows user.
class DesktopThumbnails {
  DesktopThumbnails(this.root, {this._cacheByteBudget});
  final int? _cacheByteBudget;

  /// `%LOCALAPPDATA%\Multitap\thumbnails`, the folder the native writer uses.
  final Directory root;

  /// Larger files are not something this client wrote.
  static const maximumBytes = 4 * 1024 * 1024;

  static DesktopThumbnails? windows() {
    if (!Platform.isWindows) return null;
    final local = Platform.environment['LOCALAPPDATA']?.replaceFirst(
      RegExp(r'[\\/]+$'),
      '',
    );
    if (local == null || local.isEmpty) return null;
    return DesktopThumbnails(Directory('$local\\Multitap\\thumbnails'));
  }

  final _known = <String, (FileStat, DesktopThumbnail)>{};
  final _revisions = <String, int>{};
  final _accountEpochs = <String, int>{};
  final _pathEpochs = <String, int>{};
  final _nativeGenerations = <String, int>{};
  final _blocked = <String>{};
  final _policies = <String, _ThumbnailPolicy>{};
  final _changes = StreamController<ThumbnailUpdate>.broadcast(sync: true);
  Stream<ThumbnailUpdate> get updates => _changes.stream;
  final _reads = <String, _ThumbnailRead>{};
  Future<void> _readTail = Future<void>.value();
  int _cacheBytes = 0;
  int get cacheByteBudget => _cacheByteBudget ?? thumbnailCacheByteBudget;

  String? allowedEpoch(String key, String desktopId) {
    if (!isThumbnailAccountKey(key) || !_desktopPattern.hasMatch(desktopId)) {
      return null;
    }
    final path = file(key, desktopId).path;
    return _blocked.contains(path) ? null : _policies[path]?.allowedEpoch;
  }

  /// Memory changes before any native/disk operation. Returned old epochs may
  /// be passed only to the native exact-handle, epoch-scoped purge API.
  List<String> observePrivacy(
    String key,
    String desktopId,
    ThumbnailPrivacy? next,
  ) {
    if (!isThumbnailAccountKey(key) || !_desktopPattern.hasMatch(desktopId)) {
      return const [];
    }
    final path = file(key, desktopId).path;
    final policy = _policies.putIfAbsent(path, _ThumbnailPolicy.new);
    final previous = policy.epoch,
        before = policy.allowedEpoch,
        wasRevoked = policy.revoked;
    policy.observe(next);
    if (before == policy.allowedEpoch &&
        previous == policy.epoch &&
        wasRevoked == policy.revoked) {
      return const [];
    }
    if (previous != policy.epoch) {
      _nativeGenerations.remove(path);
      _blocked.remove(path);
    }
    _pathEpochs[path] = (_pathEpochs[path] ?? 0) + 1;
    _revisions[path] = (_revisions[path] ?? 0) + 1;
    _evict(path);
    _changes.add(
      ThumbnailUpdate(key, desktopId, invalidated: true, epoch: policy.epoch),
    );
    if (next == null) {
      return const []; // Suspended observation is not revocation.
    }
    return <String>{
      if (previous != null && (previous != next.epoch || !next.allowed))
        previous,
      if (!next.allowed && isThumbnailPrivacyEpoch(next.epoch)) next.epoch,
    }.toList(growable: false);
  }

  /// Observation loss keeps no screen content authorized in memory. It is
  /// suspension, so a fresh same-epoch Host proof may resume a normal snapshot.
  void suspendAccount(String key) {
    if (!isThumbnailAccountKey(key)) return;
    final prefix = '${folder(key).path}$_separator';
    for (final path
        in _policies.keys.where((p) => p.startsWith(prefix)).toList()) {
      final id = path.substring(prefix.length, path.length - 4);
      observePrivacy(key, id, null);
    }
  }

  void retainDesktops(String key, Set<String> ids) {
    if (!isThumbnailAccountKey(key)) return;
    final prefix = '${folder(key).path}$_separator';
    for (final path
        in _policies.keys.where((p) => p.startsWith(prefix)).toList()) {
      final id = path.substring(prefix.length, path.length - 4);
      if (!ids.contains(id)) observePrivacy(key, id, null);
    }
  }

  void _remember(String path, FileStat stat, DesktopThumbnail thumbnail) {
    _evict(path);
    final budget = cacheByteBudget;
    while (_known.isNotEmpty && _cacheBytes + thumbnail.encodedBytes > budget) {
      _evict(_known.keys.first);
    }
    if (thumbnail.encodedBytes > budget || budget <= 0) return;
    _known[path] = (stat, thumbnail);
    _cacheBytes += thumbnail.encodedBytes;
  }

  void _evict(String path) {
    final previous = _known.remove(path);
    if (previous != null) {
      _cacheBytes -= previous.$2.encodedBytes;
      unawaited(previous.$2.image.evict().catchError((Object _) => false));
    }
  }

  /// Identifiers/fixed generation only; accepted clear means immediate cache
  /// invalidation, not acknowledgement that native disk deletion completed.
  bool acceptNotice(ThumbnailUpdate notice) {
    if (!_keyPattern.hasMatch(notice.key) ||
        !_desktopPattern.hasMatch(notice.desktopId) ||
        notice.generation < 0 ||
        !isThumbnailPrivacyEpoch(notice.epoch)) {
      return false;
    }
    final path = file(notice.key, notice.desktopId).path;
    final policy = _policies[path];
    if (policy?.epoch != notice.epoch ||
        (!notice.invalidated && policy?.allowedEpoch != notice.epoch)) {
      return false;
    }
    final previous = _nativeGenerations[path] ?? 0;
    if (notice.generation < previous ||
        (notice.generation == 0 && _blocked.contains(path))) {
      return false;
    }
    if (notice.generation > 0 && notice.generation == previous) return true;
    _nativeGenerations[path] = notice.generation;
    _pathEpochs[path] = (_pathEpochs[path] ?? 0) + 1;
    _revisions[path] = (_revisions[path] ?? 0) + 1;
    if (notice.invalidated) {
      _blocked.add(path);
    } else {
      _blocked.remove(path);
    }
    _evict(path);
    _changes.add(notice);
    return true;
  }

  String get _separator => Platform.pathSeparator;

  Directory folder(String key) {
    if (!_keyPattern.hasMatch(key)) throw ArgumentError.value(key, 'key');
    return Directory('${root.path}$_separator$key');
  }

  File file(String key, String desktopId) {
    if (!_desktopPattern.hasMatch(desktopId)) {
      throw ArgumentError.value(desktopId, 'desktopId');
    }
    return File('${folder(key).path}$_separator$desktopId.jpg');
  }

  /// The current picture, or null when it is missing, empty, oversized or not
  /// a complete JPEG; the tile then keeps its placeholder. An unchanged file
  /// returns the same value without being opened again.
  Future<DesktopThumbnail?> read(
    String key,
    String desktopId, {
    bool refresh = false,
  }) {
    if (!_keyPattern.hasMatch(key) || !_desktopPattern.hasMatch(desktopId)) {
      return Future.value();
    }
    final file = this.file(key, desktopId);
    if (allowedEpoch(key, desktopId) == null || cacheByteBudget <= 0) {
      return Future.value();
    }
    final pending = _reads[file.path];
    if (pending != null) {
      if (refresh) {
        _revisions[file.path] = (_revisions[file.path] ?? 0) + 1;
        pending.refresh = true;
      }
      return pending.done.future;
    }
    final read = _ThumbnailRead(refresh);
    _reads[file.path] = read;
    final before = _readTail, completed = Completer<void>();
    final ownerAccountEpoch = _accountEpochs.putIfAbsent(key, () => 0);
    _readTail = completed.future;
    Future<void> task() async {
      try {
        await before;
        while (true) {
          if (ownerAccountEpoch != (_accountEpochs[key] ?? 0)) {
            read.done.complete(null);
            break;
          }
          final fresh = read.refresh;
          read.refresh = false;
          final epoch = allowedEpoch(key, desktopId);
          if (epoch == null) {
            read.done.complete(null);
            break;
          }
          final accountEpoch = _accountEpochs.putIfAbsent(key, () => 0);
          final pathEpoch = _pathEpochs[file.path] ?? 0;
          final revision = fresh
              ? _revisions[file.path] = (_revisions[file.path] ?? 0) + 1
              : _revisions[file.path] ?? 0;
          bool current() =>
              accountEpoch == (_accountEpochs[key] ?? 0) &&
              pathEpoch == (_pathEpochs[file.path] ?? 0) &&
              epoch == allowedEpoch(key, desktopId) &&
              revision == (_revisions[file.path] ?? 0);
          final result = await _read(file, epoch, fresh, current);
          // A replacement during stat/read invalidates that result and owns
          // one subsequent read, rather than returning a permanently null task.
          if (read.refresh) continue;
          read.done.complete(result);
          break;
        }
      } catch (error, stack) {
        read.done.completeError(error, stack);
      } finally {
        if (identical(_reads[file.path], read)) _reads.remove(file.path);
        completed.complete();
      }
    }

    unawaited(task());
    return read.done.future;
  }

  Future<DesktopThumbnail?> _read(
    File file,
    String epoch,
    bool refresh,
    bool Function() current,
  ) async {
    try {
      requirePlainUserDataPath(file.path);
      if (await _revokedOnDisk(file, epoch) || !current()) {
        if (current()) _evict(file.path);
        return null;
      }
      final stat = await file.stat();
      if (!current()) return null;
      if (stat.type != FileSystemEntityType.file ||
          stat.size < 4 ||
          stat.size > maximumBytes) {
        _evict(file.path);
        return null;
      }
      final known = _known[file.path];
      if (!refresh &&
          known != null &&
          known.$1.modified == stat.modified &&
          known.$1.size == stat.size) {
        return known.$2;
      }
      final reader = await file.open();
      final Uint8List bytes;
      try {
        bytes = await reader.read(stat.size);
      } finally {
        await reader.close();
      }
      if (!current()) return null;
      if (bytes.length != stat.size ||
          thumbnailJpegEpoch(bytes) != epoch ||
          bytes.length > cacheByteBudget) {
        _evict(file.path);
        return null;
      }
      if (await _revokedOnDisk(file, epoch) || !current()) {
        if (current()) _evict(file.path);
        return null;
      }
      final thumbnail = DesktopThumbnail(
        // The decoder receives the checked bytes, never a path it can reopen
        // after a native atomic replacement with a different privacy epoch.
        image: MemoryImage(Uint8List.fromList(bytes)),
        modified: stat.modified,
      );
      if (!current()) return null;
      _remember(file.path, stat, thumbnail);
      return thumbnail;
    } on FileSystemException {
      if (current()) _evict(file.path);
      return null;
    }
  }

  /// A durable local revoke survives app/worker death even when the Host's
  /// authorization epoch did not change. Malformed/unreadable markers deny.
  Future<bool> _revokedOnDisk(File image, String epoch) async {
    final marker = File('${image.path}.revoked');
    try {
      requirePlainUserDataPath(marker.path);
      final stat = await marker.stat();
      if (stat.type == FileSystemEntityType.notFound) return false;
      if (stat.type != FileSystemEntityType.file || stat.size != 32) {
        return true;
      }
      final reader = await marker.open();
      final Uint8List bytes;
      try {
        bytes = await reader.read(33);
      } finally {
        await reader.close();
      }
      if (bytes.length != 32) return true;
      final revoked = String.fromCharCodes(bytes);
      return !isThumbnailPrivacyEpoch(revoked) || revoked == epoch;
    } on FileSystemException {
      return true;
    }
  }

  /// Deletes one account's pictures (sign-out).
  Future<void> forget(String key) async {
    if (!_keyPattern.hasMatch(key)) return;
    final target = folder(key);
    // Fence first, before the first asynchronous file operation. A late stat,
    // read or decode-owner acquisition cannot repopulate the forgotten cache.
    _accountEpochs[key] = (_accountEpochs[key] ?? 0) + 1;
    suspendAccount(key);
    final prefix = '${target.path}$_separator';
    _reads.removeWhere((path, _) => path.startsWith(prefix));
    for (final path
        in _known.keys.where((path) => path.startsWith(prefix)).toList()) {
      _evict(path);
    }
    _revisions.removeWhere((path, _) => path.startsWith(prefix));
    try {
      // Account sign-out deletes only through plain folders (no link).
      requirePlainUserDataPath(target.path);
      await target.delete(recursive: true);
    } on FileSystemException {
      // Already gone, or briefly in use: the next sign-in removes it.
    }
  }

  /// Keeps only [key]'s pictures: another account or server signed in.
  Future<void> retainOnly(String key) async {
    if (!_keyPattern.hasMatch(key)) return;
    final prefix = '${root.path}$_separator$key$_separator';
    final oldKeys = <String>{..._accountEpochs.keys};
    for (final path in _policies.keys) {
      final relative = path.substring('${root.path}$_separator'.length);
      oldKeys.add(relative.split(_separator).first);
    }
    for (final old in oldKeys.where((old) => old != key)) {
      _accountEpochs[old] = (_accountEpochs[old] ?? 0) + 1;
      suspendAccount(old);
    }
    _reads.removeWhere((path, _) => !path.startsWith(prefix));
    for (final path
        in _known.keys.where((path) => !path.startsWith(prefix)).toList()) {
      _evict(path);
    }
    try {
      requirePlainUserDataPath(root.path);
      await for (final entry in root.list(followLinks: false)) {
        final name = entry.path.split(RegExp(r'[\\/]')).last;
        if (entry is! Directory || name == key || !_keyPattern.hasMatch(name)) {
          continue;
        }
        try {
          await entry.delete(recursive: true);
        } on FileSystemException {
          // Retried at the next sign-in.
        }
      }
    } on FileSystemException {
      // No pictures folder yet.
    }
  }
}

/// What the launcher shows for one signed-in account.
abstract interface class ThumbnailSource {
  /// [refresh] re-reads the file even when its time and size look unchanged.
  Future<DesktopThumbnail?> read(String desktopId, {bool refresh = false});

  /// Desktop ids whose picture a session window just replaced.
  Stream<String> get updates;
}

/// Optional authority for the launcher; fixture/legacy presentation sources
/// need not invent a Host policy. Production AccountThumbnails always has it.
abstract interface class ThumbnailPolicySource {
  bool isAllowed(String desktopId);
}

class AccountThumbnails implements ThumbnailSource, ThumbnailPolicySource {
  AccountThumbnails(this.store, this.key, {Stream<ThumbnailUpdate>? updates})
    : updates = (updates ?? store.updates)
          .where((update) => update.key == key)
          .map((update) => update.desktopId);

  final DesktopThumbnails store;
  final String key;

  @override
  final Stream<String> updates;

  @override
  bool isAllowed(String desktopId) =>
      store.allowedEpoch(key, desktopId) != null;

  @override
  Future<DesktopThumbnail?> read(String desktopId, {bool refresh = false}) =>
      store.read(key, desktopId, refresh: refresh);
}
