import 'dart:convert';
import 'dart:io';

import '../models/session_display_mode.dart';
import 'user_data_path.dart';
import 'session_guide_memory.dart';

/// Local presentation only; never part of broker authentication or VM policy.
enum LauncherView {
  cards,
  list,
  mini,
  explorer,
  monitorWall,
  dock;

  bool get usesPreviews => this != list && this != mini;

  bool get isScene => this == explorer || this == monitorWall || this == dock;

  static LauncherView fromName(Object? value) =>
      values.where((view) => view.name == value).firstOrNull ?? cards;
}

/// Per-user launcher conveniences: the last desktop opened on each server and
/// local view choices. Connection modes are never remembered for the next open.
/// Never keeps credentials, tokens or screen content.
/// The 분류 chip, the 상태 and 구분 filters and the sort one account last
/// chose (never the search text).
class OrganizePreference {
  const OrganizePreference({
    this.chip = 'all',
    this.status = 'all',
    this.sort = 'number',
    this.kind = 'all',
  });
  final String chip, status, sort, kind;

  static const _statuses = {'all', 'running', 'stopped'};
  static const _kinds = {'all', 'vm', 'pc'};

  /// Older versions kept 실행 중 / 꺼짐 as chips: they read as the 상태 filter
  /// now, and an unknown status or kind means 전체.
  OrganizePreference migrated() {
    var chip = this.chip, status = this.status;
    if ((chip == 'running' || chip == 'stopped') && status == 'all') {
      status = chip;
    }
    if (chip == 'running' || chip == 'stopped') chip = 'all';
    if (!_statuses.contains(status)) status = 'all';
    return OrganizePreference(
      chip: chip,
      status: status,
      sort: sort,
      kind: _kinds.contains(kind) ? kind : 'all',
    );
  }
}

class LauncherPreferences {
  const LauncherPreferences({
    this.lastDesktop,
    this.modes = const {},
    this.organize = const {},
    LauncherView view = LauncherView.cards,
    this.showPreviews = true,
  }) : _legacyView = view;
  final String? lastDesktop;
  // Legacy constructor input only; never used as a connection default or saved.
  final Map<String, SessionDisplayMode> modes;
  // Keep old serialized names/API inputs readable, while every launcher uses
  // the one responsive card layout selected by the user.
  final LauncherView _legacyView;
  LauncherView get view =>
      _legacyView == LauncherView.cards ? _legacyView : LauncherView.cards;
  final bool showPreviews;

  LauncherPreferences withView(LauncherView view, {bool? showPreviews}) =>
      LauncherPreferences(
        lastDesktop: lastDesktop,
        modes: modes,
        organize: organize,
        view: view,
        showPreviews: showPreviews ?? this.showPreviews,
      );

  /// By account name on this server.
  final Map<String, OrganizePreference> organize;

  OrganizePreference organizeFor(String account) =>
      organize[account] ?? const OrganizePreference();

  LauncherPreferences withOrganize(String account, OrganizePreference value) =>
      LauncherPreferences(
        lastDesktop: lastDesktop,
        modes: modes,
        organize: {...organize, account: value},
        view: view,
        showPreviews: showPreviews,
      );

  SessionDisplayMode modeFor(String desktopId) => SessionDisplayMode.windowed;

  LauncherPreferences opened(String desktopId, SessionDisplayMode mode) =>
      LauncherPreferences(
        lastDesktop: desktopId,
        organize: organize,
        view: view,
        showPreviews: showPreviews,
      );
}

abstract class LauncherMemory {
  Future<LauncherPreferences> load(String server);
  Future<void> save(String server, LauncherPreferences preferences);

  /// Drops what an earlier client saved for "마우스 가두기", removed on
  /// 2026-11-03 (마우스 잠금 is the one lock left); it is never read.
  Future<void> forgetPointerConfinement();

  // "마우스 잠금" (formerly 게임 마우스) is not remembered: every connection
  // starts with it off. A `gameMouse` key an earlier
  // client left in session.json is ignored, and dropped with the above.

  /// Whether the one-time "keeps running in the tray" notice was shown
  /// (2026-10-27). Kept once for this PC user across servers; never sent to a
  /// broker.
  Future<bool> loadTrayNoticeShown();
  Future<void> saveTrayNoticeShown();
}

/// Keeps preferences for this process only (tests and previews).
class SessionLauncherMemory
    implements LauncherMemory, SessionGuideStoreProvider {
  SessionLauncherMemory();
  @override
  final SessionGuideStore sessionGuideStore = MemorySessionGuideStore();
  final _values = <String, LauncherPreferences>{};
  bool _trayNoticeShown = false;
  @override
  Future<LauncherPreferences> load(String server) async =>
      _values[server] ?? const LauncherPreferences();
  @override
  Future<void> save(String server, LauncherPreferences preferences) async {
    _values[server] = preferences;
  }

  @override
  Future<void> forgetPointerConfinement() async {}

  @override
  Future<bool> loadTrayNoticeShown() async => _trayNoticeShown;
  @override
  Future<void> saveTrayNoticeShown() async {
    _trayNoticeShown = true;
  }
}

/// `%LOCALAPPDATA%\Multitap\Client\launcher.json`, keyed by server origin.
/// Earlier clients kept session-window choices beside it in [sessionFile]
/// (`session.json`); none is kept there any more.
class FileLauncherMemory implements LauncherMemory, SessionGuideStoreProvider {
  FileLauncherMemory(this.file, {File? sessionFile})
    : sessionFile = sessionFile ?? File('${file.parent.path}/session.json');
  final File file, sessionFile;
  @override
  late final SessionGuideStore sessionGuideStore = FileSessionGuideStore(
    Directory('${file.parent.path}/session-guides'),
  );

  factory FileLauncherMemory.windows() {
    final root = Platform.environment['LOCALAPPDATA'];
    if (root == null || root.isEmpty) {
      throw StateError('Windows app data directory is unavailable.');
    }
    return FileLauncherMemory(File('$root/Multitap/Client/launcher.json'));
  }

  Future<Map<String, Object?>> _readSession() async {
    try {
      final value = jsonDecode(await sessionFile.readAsString());
      return value is Map ? value.cast<String, Object?>() : {};
    } on FileSystemException {
      return {};
    } on FormatException {
      return {};
    }
  }

  /// The keys earlier clients saved: 마우스 가두기 and 게임 마우스.
  static const _retiredSessionKeys = ['confinePointer', 'gameMouse'];

  /// Removes them; the file goes too once nothing else is in it.
  @override
  Future<void> forgetPointerConfinement() async {
    final session = await _readSession();
    if (!_retiredSessionKeys.any(session.containsKey)) return;
    _retiredSessionKeys.forEach(session.remove);
    if (session.isEmpty) {
      await sessionFile.delete();
    } else {
      await writeUserDataFile(sessionFile, jsonEncode(session));
    }
  }

  /// Client-wide flags live beside the per-server entries in the dashboard's
  /// launcher file, under a key no server origin can equal.
  static const _clientKey = '_client';

  @override
  Future<bool> loadTrayNoticeShown() async {
    final entry = (await _read())[_clientKey];
    return entry is Map && entry['trayNoticeShown'] == true;
  }

  @override
  Future<void> saveTrayNoticeShown() async {
    final all = await _read();
    final entry = all[_clientKey];
    all[_clientKey] = {if (entry is Map) ...entry, 'trayNoticeShown': true};
    await writeUserDataFile(file, jsonEncode(all));
  }

  static final _id = RegExp(r'^[A-Za-z0-9._:-]{1,128}$');

  Future<Map<String, dynamic>> _read() async {
    try {
      final value = jsonDecode(await file.readAsString());
      return value is Map<String, dynamic> ? value : {};
    } on FileSystemException {
      return {};
    } on FormatException {
      return {};
    }
  }

  @override
  Future<LauncherPreferences> load(String server) async {
    final entry = (await _read())[server];
    if (entry is! Map) return const LauncherPreferences();
    final last = entry['lastDesktop'];
    final organize = <String, OrganizePreference>{};
    final rawOrganize = entry['organize'];
    if (rawOrganize is Map) {
      for (final item in rawOrganize.entries) {
        final value = item.value;
        if (item.key is! String ||
            (item.key as String).isEmpty ||
            (item.key as String).length > 128 ||
            value is! Map) {
          continue;
        }
        final chip = value['chip'], status = value['status'];
        final sort = value['sort'], kind = value['kind'];
        organize[item.key as String] = OrganizePreference(
          chip: chip is String && chip.isNotEmpty && chip.length <= 140
              ? chip
              : 'all',
          status: status is String && status.length <= 16 ? status : 'all',
          sort: sort is String && sort.length <= 16 ? sort : 'number',
          kind: kind is String && kind.length <= 16 ? kind : 'all',
        ).migrated();
      }
    }
    return LauncherPreferences(
      lastDesktop: last is String && _id.hasMatch(last) ? last : null,
      organize: organize,
      view: LauncherView.fromName(entry['view']),
      showPreviews: entry['showPreviews'] != false,
    );
  }

  @override
  Future<void> save(String server, LauncherPreferences preferences) async {
    final all = await _read();
    all[server] = {
      'view': preferences.view.name,
      'showPreviews': preferences.showPreviews,
      if (preferences.lastDesktop != null)
        'lastDesktop': preferences.lastDesktop,
      if (preferences.organize.isNotEmpty)
        'organize': {
          for (final entry in preferences.organize.entries)
            entry.key: {
              'chip': entry.value.chip,
              'status': entry.value.status,
              'sort': entry.value.sort,
              'kind': entry.value.kind,
            },
        },
    };
    await writeUserDataFile(file, jsonEncode(all));
  }
}
