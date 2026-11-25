import 'dart:convert';
import 'dart:io';

import 'decode_path_choice.dart';
import 'user_data_path.dart';

/// The user's "하드웨어 디코딩" choice (default on) and what this PC's streams
/// taught about its two decode paths (decode-path-v1, decode_path_choice.dart).
///
/// On: a stream decodes H.264 on the PC's GPU and hands the picture to Flutter
/// as a GPU texture, or on the CPU, whichever [reason] says is sooner here.
/// Off: the CPU path only. Both are read when a connection is made, so a
/// change applies from the next connection. Kept apart from the server and
/// sign-in settings. The launcher and every session window are processes of
/// their own with their own copy: each reads the file when it starts and
/// before it decides, and adds what it measured to the file's records.
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

  /// The user's switch.
  static bool hardwareDecode = true;
  static DecodeSettings? _store;

  /// The graphics adapter Flutter renders on ("vendor:device"), the key of
  /// this PC's records; null when it is not known (then nothing is learned
  /// and the hardware path is asked for, as before decode-path-v1).
  static String? adapter;

  /// Records as last read from the file, what this process measured since, and
  /// the part of that which is being written to the file right now.
  static Map<String, DecodePathMemory> _paths = {};
  static final Map<String, DecodePathMemory> _measured = {};
  static Map<String, DecodePathMemory> _writing = const {};
  static double _unsaved = 0;
  static Future<void>? _saving;

  /// At most this many adapters are remembered (a PC has one or two).
  static const _adapters = 8;

  /// What was measured is written once this much of it waits (seconds), and
  /// when a session ends.
  static const _saveEvery = 30.0;

  /// Loads the saved choice and records; a missing or unreadable file means
  /// the default (on) and no records.
  static Future<void> init(DecodeSettings store, {String? adapter}) async {
    _store = store;
    DecodeSettings.adapter = adapter;
    _measured.clear();
    _unsaved = 0;
    hardwareDecode = await store.load();
    _paths = await store.loadPaths();
    // An adapter seen for the first time gets its (empty) records now, so the
    // file shows which adapter this PC decides for before any stream ran.
    if (adapter != null && !_paths.containsKey(adapter)) {
      try {
        final paths = {..._paths, adapter: DecodePathMemory()};
        while (paths.length > _adapters) {
          paths.remove(paths.keys.first);
        }
        await store._write(hardwareDecode, paths);
        _paths = paths;
      } catch (_) {
        // It is written with the first measurement instead.
      }
    }
  }

  /// Reads the records again: another window may have added to them.
  static Future<void> refresh() async {
    final store = _store;
    if (store == null) return;
    try {
      _paths = await store.loadPaths();
    } catch (_) {
      // The records of the last read stay.
    }
  }

  /// Why a new connection decodes the way it will.
  static DecodePathReason get reason {
    if (!hardwareDecode) return DecodePathReason.cpuOnly;
    final key = adapter;
    if (key == null) return DecodePathReason.hardwareNoAdapter;
    // What the file had, what is being written to it, what waits to be.
    var all = _paths[key] ?? DecodePathMemory();
    for (final more in [_writing[key], _measured[key]]) {
      if (more != null) all = _both(all, more);
    }
    return all.reason;
  }

  static DecodePathMemory _both(DecodePathMemory a, DecodePathMemory b) =>
      DecodePathMemory(
        hardware: _sum(a.hardware, b.hardware),
        cpu: _sum(a.cpu, b.cpu),
      );

  /// What a new connection asks the runner for.
  static bool get useHardware => reason.hardware;

  /// One statistics reading of a fast stream on one path.
  static void observe({
    required bool hardwarePath,
    required double seconds,
    required double delayMs,
    required double frameMs,
    required bool decoderBusy,
  }) {
    final key = adapter;
    if (key == null) return;
    final memory = _measured.putIfAbsent(key, DecodePathMemory.new);
    final record = hardwarePath ? memory.hardware : memory.cpu;
    final before = record.seconds;
    record.add(seconds, delayMs, frameMs, decoderBusy);
    if (record.seconds == before) return;
    _unsaved += seconds;
    if (_unsaved >= _saveEvery) remember();
  }

  /// Adds what this process measured to the file's records (best effort).
  static Future<void> remember() {
    final running = _saving;
    // What was measured while that write ran is written after it.
    if (running != null) {
      return running.then((_) => _measured.isEmpty ? null : remember());
    }
    final store = _store;
    if (store == null || _measured.isEmpty) return Future.value();
    _unsaved = 0;
    final taken = Map<String, DecodePathMemory>.of(_measured);
    _measured.clear();
    _writing = taken;
    return _saving = () async {
      try {
        final paths = await store.loadPaths();
        taken.forEach((key, mine) {
          final stored = paths.remove(key) ?? DecodePathMemory();
          paths[key] = _both(stored, mine);
        });
        while (paths.length > _adapters) {
          paths.remove(paths.keys.first);
        }
        await store._write(await store.load(), paths);
        _paths = paths;
      } catch (_) {
        // What was measured stays for the next save.
        taken.forEach((key, mine) {
          final later = _measured[key];
          _measured[key] = later == null ? mine : _both(mine, later);
        });
      } finally {
        _writing = const {};
        _saving = null;
      }
    }();
  }

  static DecodePathRecord _sum(DecodePathRecord a, DecodePathRecord b) {
    final sum = DecodePathRecord(
      seconds: a.seconds + b.seconds,
      delay: a.delay + b.delay,
      behind: a.behind + b.behind,
      busy: a.busy + b.busy,
    );
    while (sum.seconds > DecodePathRule.keepSeconds) {
      sum.seconds /= 2;
      sum.delay /= 2;
      sum.behind /= 2;
      sum.busy /= 2;
    }
    return sum;
  }

  /// Applies [value] to later connections and remembers it (best effort).
  static Future<void> choose(bool value) async {
    hardwareDecode = value;
    try {
      await _store?.save(value);
    } catch (_) {
      // The choice still applies to this run.
    }
  }

  Future<Object?> _read() async {
    try {
      return jsonDecode(await file.readAsString());
    } on FileSystemException {
      return null;
    } on FormatException {
      return null;
    }
  }

  Future<bool> load() async {
    final value = await _read();
    return value is Map && value['hardwareDecode'] == false ? false : true;
  }

  /// The records by adapter, oldest written first.
  Future<Map<String, DecodePathMemory>> loadPaths() async {
    final value = await _read();
    final stored = value is Map ? value['paths'] : null;
    final paths = <String, DecodePathMemory>{};
    if (stored is! Map) return paths;
    for (final entry in stored.entries) {
      final key = entry.key;
      if (key is! String || !_adapterKey.hasMatch(key)) continue;
      if (paths.length == _adapters) break;
      paths[key] = DecodePathMemory.parse(entry.value);
    }
    return paths;
  }

  static final _adapterKey = RegExp(r'^[0-9a-f]{4}:[0-9a-f]{4}$');

  /// A value the runner's `renderAdapter` may be used as, else null.
  static String? adapterKey(Object? value) =>
      value is String && _adapterKey.hasMatch(value) ? value : null;

  /// The switch; the records in the file stay.
  Future<void> save(bool value) async => _write(value, await loadPaths());

  Future<void> _write(bool value, Map<String, DecodePathMemory> paths) =>
      writeUserDataFile(
        file,
        jsonEncode({
          'hardwareDecode': value,
          if (paths.isNotEmpty)
            'paths': {
              for (final entry in paths.entries) entry.key: entry.value.toJson(),
            },
        }),
      );
}
