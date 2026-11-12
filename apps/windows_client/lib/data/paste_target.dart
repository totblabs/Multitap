import 'dart:io';

import 'package:flutter/foundation.dart';
import 'package:flutter/services.dart';

/// A folder on this PC that a paste is known to be filling.
@immutable
class PasteDestination {
  const PasteDestination({required this.path, required this.name});

  /// The folder's full path.
  final String path;

  /// The name File Explorer shows for it ("다운로드", "바탕 화면").
  final String name;

  @override
  bool operator ==(Object other) =>
      other is PasteDestination && other.path == path && other.name == name;

  @override
  int get hashCode => Object.hash(path, name);
}

/// What this PC can say about where a paste goes. Explorer writes the pasted
/// files itself and never tells the source the folder, so the session asks two
/// separate questions and claims a folder only when both agree: which folder
/// the File Explorer window (or the desktop) in front shows as the paste
/// starts, and whether a pasted item was then just created in that folder.
abstract interface class PasteTargetProbe {
  /// The folder of the File Explorer window or the desktop in front now; null
  /// for any other window, a view that is no folder on disk, or no answer.
  Future<PasteDestination?> foregroundFolder();

  /// When [path] was created; null when nothing is there.
  Future<DateTime?> created(String path);

  /// Opens [folder] in File Explorer with [select] (an item directly in it)
  /// selected. False when it could not.
  Future<bool> open(String folder, {String? select});
}

/// The runner's `multitap/paste-target` channel and this PC's file system.
class NativePasteTargetProbe implements PasteTargetProbe {
  const NativePasteTargetProbe([
    this._channel = const MethodChannel('multitap/paste-target'),
  ]);
  final MethodChannel _channel;

  @override
  Future<PasteDestination?> foregroundFolder() async {
    try {
      final raw = await _channel
          .invokeMethod<Object?>('capture')
          .timeout(const Duration(seconds: 3));
      if (raw is! Map) return null;
      final path = raw['path'], name = raw['name'];
      if (path is! String || name is! String || path.isEmpty) return null;
      return PasteDestination(path: path, name: name.isEmpty ? path : name);
    } catch (_) {
      return null;
    }
  }

  @override
  Future<DateTime?> created(String path) async {
    try {
      final stat = await FileStat.stat(path);
      // On Windows FileStat.changed is the creation time.
      return stat.type == FileSystemEntityType.notFound ? null : stat.changed;
    } catch (_) {
      return null;
    }
  }

  @override
  Future<bool> open(String folder, {String? select}) async {
    try {
      return await _channel.invokeMethod<bool>('open', {
            'folder': folder,
            'select': ?select,
          }) ??
          false;
    } catch (_) {
      return false;
    }
  }
}

/// Finds the destination of one paste on this PC, or nothing. [since] is when
/// the paste began asking for data; an item counts only when it was created no
/// earlier than [slack] before that (an older item of the same name, a merge
/// into an existing folder or a replaced file says nothing about this paste).
class PasteDestinationCheck {
  PasteDestinationCheck(
    this.probe, {
    required this.since,
    this.slack = const Duration(seconds: 2),
  });
  final PasteTargetProbe probe;
  final DateTime since;
  final Duration slack;

  Future<PasteDestination?>? _candidate;
  PasteDestination? _confirmed;
  String? _item;
  Future<bool>? _checking;

  /// The folder, once a pasted item was seen created there.
  PasteDestination? get confirmed => _confirmed;

  /// The pasted item that was seen in it (for 폴더 열기 to select).
  String? get item => _item;

  /// Ask once, right as the paste starts, which folder is in front.
  void start() => _candidate ??= probe.foregroundFolder();

  /// Looks for one of [names] in the folder in front. True when it is now confirmed.
  Future<bool> check(List<String> names) async {
    if (_confirmed != null) return true;
    final pending = _candidate;
    if (pending == null || names.isEmpty) return false;
    // A look still running is waited for, then this one looks again.
    final running = _checking;
    if (running != null) return running.then((_) => check(names));
    final look = _checking = _look(pending, names);
    try {
      return await look;
    } finally {
      if (identical(_checking, look)) _checking = null;
    }
  }

  Future<bool> _look(
    Future<PasteDestination?> pending,
    List<String> names,
  ) async {
    final folder = await pending;
    if (folder == null) return false;
    final floor = since.subtract(slack);
    for (final name in names.take(4)) {
      if (!_plainName(name)) continue;
      final created = await probe.created(_join(folder.path, name));
      if (created != null && !created.isBefore(floor)) {
        _confirmed = folder;
        _item = name;
        return true;
      }
    }
    return false;
  }

  static bool _plainName(String name) =>
      name.isNotEmpty &&
      name != '.' &&
      name != '..' &&
      !name.contains(RegExp(r'[\\/:]'));

  static String _join(String folder, String name) =>
      folder.endsWith(r'\') || folder.endsWith('/')
      ? '$folder$name'
      : '$folder\\$name';
}
