import 'dart:async';
import 'dart:convert';
import 'dart:io';

import 'package:flutter/material.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:multitap_client/data/appearance_settings.dart';
import 'package:multitap_client/data/client_settings.dart';

void main() {
  late Directory directory;
  late File file;
  late AppearanceSettings settings;

  setUp(() async {
    directory = await Directory.systemTemp.createTemp('multitap-appearance-');
    file = File('${directory.path}/appearance.json');
    settings = AppearanceSettings(file);
  });
  tearDown(() async {
    await directory.delete(recursive: true);
  });

  test('Missing appearance defaults to dark without creating a file', () async {
    expect(await settings.load(), ThemeMode.dark);
    expect(await file.exists(), isFalse);
  });

  test(
    'Corrupt and unsupported appearance values safely default to dark',
    () async {
      for (final value in [
        '{broken',
        'null',
        '[]',
        '123',
        '{"theme":"system"}',
        '{"theme":"LIGHT"}',
        '{"theme":false}',
        '{}',
      ]) {
        await file.writeAsString(value);
        expect(await settings.load(), ThemeMode.dark, reason: value);
        expect(await file.readAsString(), value);
      }
    },
  );

  test('Light and dark round trip with no authentication data', () async {
    for (final mode in [ThemeMode.light, ThemeMode.dark]) {
      await settings.save(mode);
      expect(await settings.load(), mode);
      expect(jsonDecode(await file.readAsString()), {
        'theme': mode.name,
        'scene': 'peaks',
      });
    }
  });

  test('The background scene is saved apart from the theme and survives a theme change', () async {
    expect(await settings.loadScene(), BackgroundScene.peaks);
    await settings.save(ThemeMode.light);
    await settings.saveScene(BackgroundScene.meadow);
    expect(await settings.loadScene(), BackgroundScene.meadow);
    expect(await settings.load(), ThemeMode.light, reason: 'The scene kept the theme.');
    await settings.save(ThemeMode.dark);
    expect(await settings.loadScene(), BackgroundScene.meadow, reason: 'The theme kept the scene.');
    // An unknown or damaged value falls back to the default scene.
    await file.writeAsString('{"theme":"dark","scene":"../x"}');
    expect(await settings.loadScene(), BackgroundScene.peaks);
    // The released dark image stays the default night.
    expect(BackgroundScene.peaks.asset(dark: true), 'assets/backgrounds/alpine-dusk.png');
    expect(BackgroundScene.peaks.asset(dark: false), 'assets/backgrounds/alpine-dusk-day.jpg');
    // Every scene's day and night are packaged.
    for (final scene in BackgroundScene.values) {
      for (final dark in [true, false]) {
        expect(File('${Directory.current.path}/${scene.asset(dark: dark)}').existsSync(), isTrue, reason: scene.asset(dark: dark));
      }
    }
  });

  test(
    'Appearance storage creates its parent and leaves server settings intact',
    () async {
      final serverFile = File('${directory.path}/Client/settings.json');
      final serverSettings = ClientSettings(serverFile);
      final origin = Uri.parse('https://configured.example.com:9443');
      await serverSettings.save(origin);
      final before = await serverFile.readAsBytes();
      final nested = AppearanceSettings(
        File('${directory.path}/appearance/appearance.json'),
      );
      await nested.save(ThemeMode.light);
      expect(await nested.load(), ThemeMode.light);
      expect(await serverFile.readAsBytes(), before);
      expect(await serverSettings.load(), origin);
    },
  );

  test(
    'System mode is rejected without overwriting the saved choice',
    () async {
      await settings.save(ThemeMode.light);
      final before = await file.readAsBytes();
      await expectLater(settings.save(ThemeMode.system), throwsArgumentError);
      expect(await file.readAsBytes(), before);
    },
  );

  test(
    'A real write failure is reported instead of pretending it saved',
    () async {
      final occupied = Directory('${directory.path}/occupied');
      await occupied.create();
      await expectLater(
        AppearanceSettings(File(occupied.path)).save(ThemeMode.light),
        throwsA(isA<FileSystemException>()),
      );
    },
  );

  test(
    'Controller defaults to dark and ignores unchanged or system choices',
    () async {
      final saves = <ThemeMode>[];
      final appearance = AppAppearance(onSave: (mode) async => saves.add(mode));
      addTearDown(appearance.dispose);
      var notifications = 0;
      appearance.addListener(() => notifications++);
      expect(appearance.mode, ThemeMode.dark);
      expect(appearance.saving, isFalse);
      await appearance.select(ThemeMode.dark);
      await appearance.select(ThemeMode.system);
      expect(saves, isEmpty);
      expect(notifications, 0);
    },
  );

  test(
    'Save failure restores the previous appearance and releases the busy gate',
    () async {
      final snapshots = <(ThemeMode, bool)>[];
      final appearance = AppAppearance(
        onSave: (_) async => throw const FileSystemException('fixture failure'),
      );
      addTearDown(appearance.dispose);
      appearance.addListener(
        () => snapshots.add((appearance.mode, appearance.saving)),
      );
      await expectLater(
        appearance.select(ThemeMode.light),
        throwsA(isA<FileSystemException>()),
      );
      expect(appearance.mode, ThemeMode.dark);
      expect(appearance.saving, isFalse);
      expect(snapshots, [(ThemeMode.light, true), (ThemeMode.dark, false)]);
    },
  );

  test(
    'A busy selection ignores concurrent changes and can save again afterwards',
    () async {
      final pending = Completer<void>();
      final saves = <ThemeMode>[];
      final appearance = AppAppearance(
        onSave: (mode) async {
          saves.add(mode);
          if (saves.length == 1) await pending.future;
        },
      );
      addTearDown(appearance.dispose);
      final first = appearance.select(ThemeMode.light);
      expect(appearance.mode, ThemeMode.light);
      expect(appearance.saving, isTrue);
      await appearance.select(ThemeMode.dark);
      await appearance.select(ThemeMode.light);
      expect(saves, [ThemeMode.light]);
      pending.complete();
      await first;
      expect(appearance.mode, ThemeMode.light);
      expect(appearance.saving, isFalse);
      await appearance.select(ThemeMode.dark);
      expect(saves, [ThemeMode.light, ThemeMode.dark]);
      expect(appearance.mode, ThemeMode.dark);
    },
  );
}
