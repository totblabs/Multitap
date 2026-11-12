import 'dart:convert';
import 'dart:io';

import 'package:flutter/material.dart' show ThemeMode;
import 'package:flutter_test/flutter_test.dart';
import 'package:multitap_client/data/appearance_settings.dart';
import 'package:multitap_client/data/client_settings.dart';
import 'package:multitap_client/data/launcher_memory.dart';
import 'package:multitap_client/data/user_data_path.dart';

/// A directory junction (no privilege needed, unlike a symbolic link).
void junction(String link, String target) {
  final result = Process.runSync('cmd', [
    '/c',
    'mklink',
    '/J',
    link.replaceAll('/', r'\'),
    target.replaceAll('/', r'\'),
  ]);
  if (result.exitCode != 0) {
    throw StateError('junction fixture: ${result.stderr}');
  }
}

void main() {
  TestWidgetsFlutterBinding.ensureInitialized();
  late Directory temp;
  setUp(
    () async =>
        temp = await Directory.systemTemp.createTemp('multitap-protected-'),
  );
  tearDown(() async {
    // Links first, as links: never through them.
    for (final entry
        in temp.listSync(recursive: true, followLinks: false).reversed) {
      if (FileSystemEntity.isLinkSync(entry.path)) {
        Directory(entry.path).deleteSync();
      }
    }
    await temp.delete(recursive: true);
  });

  test('the server origin round-trips through settings.json', () async {
    final file = File('${temp.path}/Client/settings.json');
    await file.parent.create(recursive: true);
    final settings = ClientSettings(file);
    expect(await settings.load(), isNull);
    await settings.save(Uri.parse('https://other.example:18443'));
    expect(await settings.load(), Uri.parse('https://other.example:18443'));
  });

  test('settings.json is read only as this client writes it', () async {
    final file = File('${temp.path}/Client/settings.json');
    await file.parent.create(recursive: true);
    final settings = ClientSettings(file);
    for (final text in [
      jsonEncode({
        'serverOrigin': 'https://vdi.example.com',
        'update': 'https://evil.example',
      }),
      jsonEncode({'serverOrigin': 42}),
      jsonEncode(['https://vdi.example.com']),
      jsonEncode({'serverOrigin': 'http://vdi.example.com'}),
      '{"serverOrigin":"https://vdi.example.com"}${' ' * 5000}',
    ]) {
      await file.writeAsString(text);
      expect(
        await settings.load(),
        isNull,
        reason: text.length > 200 ? 'oversized' : text,
      );
    }
  });

  test('per-user data is never written through a junction', () async {
    final elsewhere = await Directory('${temp.path}/elsewhere').create();
    final local = '${temp.path}${Platform.pathSeparator}Local';
    await Directory('$local/Multitap').create(recursive: true);
    // %LOCALAPPDATA%\Multitap\Client is a junction to another folder.
    junction('$local\\Multitap\\Client', elsewhere.path);
    final file = File('$local/Multitap/Client/settings.json');
    expect(
      () => requirePlainUserDataPath(file.path, localAppData: local),
      throwsA(isA<FileSystemException>()),
    );
    // A plain folder below the anchor passes.
    expect(
      () => requirePlainUserDataPath(
        '$local/Multitap/appearance.json',
        localAppData: local,
      ),
      returnsNormally,
    );
    // Outside %LOCALAPPDATA% the file itself must not be a link.
    final target = File('${temp.path}/target.json')..writeAsStringSync('{}');
    junction('${temp.path}\\settings.json', elsewhere.path);
    await expectLater(
      writeUserDataFile(File('${temp.path}/settings.json'), '{"x":1}'),
      throwsA(isA<FileSystemException>()),
    );
    expect(target.readAsStringSync(), '{}');
    expect(elsewhere.listSync(), isEmpty);
    // The real writers use it.
    final linkedFolder = '${temp.path}${Platform.pathSeparator}linked';
    junction(linkedFolder, elsewhere.path);
    await expectLater(
      AppearanceSettings(File('$linkedFolder/appearance.json'))
          .save(ThemeMode.light),
      throwsA(isA<FileSystemException>()),
    );
    await expectLater(
      FileLauncherMemory(File('$linkedFolder/launcher.json'))
          .saveTrayNoticeShown(),
      throwsA(isA<FileSystemException>()),
    );
    expect(elsewhere.listSync(), isEmpty);
  });
}
