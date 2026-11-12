import 'dart:convert';
import 'dart:io';

import 'package:flutter_test/flutter_test.dart';
import 'package:multitap_client/data/launcher_memory.dart';
import 'package:multitap_client/models/session_display_mode.dart';

void main() {
  test(
    'Old fullscreen and all-monitor defaults never select the next mode',
    () {
      for (final mode in [
        SessionDisplayMode.fullscreen,
        SessionDisplayMode.allMonitors,
      ]) {
        final legacy = LauncherPreferences(modes: {'pve-149': mode});
        expect(legacy.modeFor('pve-149'), SessionDisplayMode.windowed);
        final next = legacy.opened('pve-149', mode);
        expect(next.modeFor('pve-149'), SessionDisplayMode.windowed);
        expect(next.modes, isEmpty);
      }
    },
  );

  test(
    'Legacy view inputs become cards without changing other conveniences',
    () {
      final original =
          const LauncherPreferences(
            lastDesktop: 'pve-149',
            modes: {'pve-149': SessionDisplayMode.fullscreen},
            view: LauncherView.mini,
            showPreviews: false,
          ).withOrganize(
            'tester',
            const OrganizePreference(status: 'running', sort: 'name'),
          );
      final changed = original
          .opened('pve-170', SessionDisplayMode.windowed)
          .withView(LauncherView.list);
      expect(changed.view, LauncherView.cards);
      expect(changed.showPreviews, isFalse);
      expect(changed.lastDesktop, 'pve-170');
      expect(changed.modeFor('pve-149'), SessionDisplayMode.windowed);
      expect(changed.modes, isEmpty);
      expect(changed.organizeFor('tester').sort, 'name');
      expect(
        changed.withOrganize('other', const OrganizePreference()).view,
        LauncherView.cards,
      );
      expect(
        changed
            .withView(LauncherView.cards, showPreviews: true)
            .organizeFor('tester')
            .status,
        'running',
      );
    },
  );

  group('local launcher presentation file', () {
    late Directory directory;
    late File file;
    setUp(() {
      directory = Directory.systemTemp.createTempSync('totb-launcher-view-');
      file = File('${directory.path}/launcher.json');
    });
    tearDown(() {
      expect(
        directory.absolute.path,
        startsWith(
          '${Directory.systemTemp.absolute.path}${Platform.pathSeparator}',
        ),
      );
      directory.deleteSync(recursive: true);
    });

    test('All legacy inputs save cards and preserve preview and unrelated preferences', () async {
      final memory = FileLauncherMemory(file);
      await memory.saveTrayNoticeShown();
      for (final view in LauncherView.values) {
        await memory.save(
          'https://one.example',
          LauncherPreferences(view: view, showPreviews: false),
        );
        await memory.save(
          'https://two.example',
          const LauncherPreferences(view: LauncherView.list),
        );
        final stored = await FileLauncherMemory(file)
            .load('https://one.example');
        expect(stored.view, LauncherView.cards);
        expect(stored.showPreviews, isFalse);
        expect(
          (await memory.load('https://two.example')).view,
          LauncherView.cards,
        );
        expect(await memory.loadTrayNoticeShown(), isTrue);
      }
      final entry =
          (jsonDecode(await file.readAsString()) as Map)['https://one.example']
              as Map;
      expect(entry['view'], 'cards');
      expect(entry['showPreviews'], isFalse);
      expect(entry.keys, isNot(contains('token')));
      expect(entry.keys, isNot(contains('password')));
    });

    test(
      'Every prior raw layout reads as cards and retains preview settings',
      () async {
        for (final view in LauncherView.values) {
          await file.writeAsString(
            jsonEncode({
              'https://one.example': {
                'view': view.name,
                'showPreviews': false,
                'lastDesktop': 'pve-149',
                'modes': {'pve-149': 'fullscreen'},
              },
            }),
          );
          final restored = await FileLauncherMemory(file)
              .load('https://one.example');
          expect(restored.view, LauncherView.cards, reason: view.name);
          expect(restored.showPreviews, isFalse);
          expect(restored.lastDesktop, 'pve-149');
          expect(restored.modeFor('pve-149'), SessionDisplayMode.windowed);
          expect(restored.modes, isEmpty);
        }
      },
    );

    test('Older or invalid values retain the default card view and visible previews', () async {
      for (final raw in <Map<String, Object?>>[
        {},
        {'view': 'unknown', 'showPreviews': 'false'},
        {'view': 7, 'showPreviews': null},
      ]) {
        await file.writeAsString(jsonEncode({'https://one.example': raw}));
        final restored = await FileLauncherMemory(file)
            .load('https://one.example');
        expect(restored.view, LauncherView.cards);
        expect(restored.showPreviews, isTrue);
      }
    });

    test(
      'Old mode entries are ignored and removed on the next preference save',
      () async {
        for (final mode in ['fullscreen', 'allMonitors']) {
          await file.writeAsString(
            jsonEncode({
              'https://one.example': {
                'lastDesktop': 'pve-149',
                'modes': {'pve-149': mode},
                'showPreviews': false,
                'organize': {
                  'tester': {
                    'chip': 'all',
                    'status': 'running',
                    'sort': 'name',
                  },
                },
              },
            }),
          );
          final memory = FileLauncherMemory(file);
          final restored = await memory.load('https://one.example');
          expect(restored.modeFor('pve-149'), SessionDisplayMode.windowed);
          expect(restored.modes, isEmpty);
          await memory.save(
            'https://one.example',
            restored.opened('pve-149', SessionDisplayMode.fullscreen),
          );
          final entry =
              (jsonDecode(await file.readAsString())
                      as Map)['https://one.example']
                  as Map;
          expect(entry.containsKey('modes'), isFalse);
          expect(entry['lastDesktop'], 'pve-149');
          expect(entry['showPreviews'], isFalse);
          expect(
            (await memory.load('https://one.example'))
                .organizeFor('tester')
                .sort,
            'name',
          );
        }
      },
    );
  });
}
