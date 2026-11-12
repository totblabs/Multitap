// Last-frame picture store: account key, file validation, cache identity and
// deletion. Real files under a temporary folder; no native writer, broker or
// session is involved.
import 'dart:async';
import 'dart:convert';
import 'dart:io';

import 'package:flutter/painting.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:multitap_client/data/broker_api.dart';
import 'package:multitap_client/data/desktop_thumbnails.dart';
import 'package:multitap_client/data/session_windows.dart';
import 'package:multitap_client/models/session_display_mode.dart';
import 'package:multitap_client/models/stream_options.dart';

/// SOI, a few marker bytes and EOI: what the reader checks for.
const epoch = '0123456789abcdef0000000000000001';
final rawJpeg = [0xFF, 0xD8, 0xFF, 0xE0, 0x00, 0x10, 0x4A, 0x46, 0xFF, 0xD9];
List<int> markedJpeg(List<int> bytes) {
  final marker = ascii.encode('Multitap:thumbnail-privacy-v1:$epoch');
  return [
    0xff,
    0xd8,
    0xff,
    0xfe,
    (marker.length + 2) >> 8,
    (marker.length + 2) & 255,
    ...marker,
    ...bytes.skip(2),
  ];
}

final jpeg = markedJpeg(rawJpeg);
const account = '3f2a9c4e-1b7d-4e0a-9c55-7d1e2b3a4f60';

void main() {
  late Directory root;
  late DesktopThumbnails store;
  setUp(() async {
    root = await Directory.systemTemp.createTemp('multitap-thumbnails-');
    store = DesktopThumbnails(root, cacheByteBudget: 1024 * 1024);
  });
  tearDown(() async {
    if (await root.exists()) await root.delete(recursive: true);
  });

  Future<File> put(String key, String id, List<int> bytes) async {
    store.observePrivacy(
      key,
      id,
      const ThumbnailPrivacy(epoch: epoch, allowed: true),
    );
    final file = store.file(key, id);
    await file.parent.create(recursive: true);
    await file.writeAsBytes(bytes, flush: true);
    return file;
  }

  group('account key', () {
    test('is the first 16 hex digits of SHA-256(origin + newline + id)', () {
      // Expected values computed independently with .NET SHA256.
      expect(
        thumbnailAccountKey(Uri.parse('https://vdi.example.com'), account),
        '5f13825dbc51da81',
      );
      expect(
        thumbnailAccountKey(Uri.parse('https://vdi.example.com:8443'), account),
        'f16c66eea1612389',
      );
      // The origin is canonical: a trailing slash names the same server.
      expect(
        thumbnailAccountKey(Uri.parse('https://vdi.example.com/'), account),
        '5f13825dbc51da81',
      );
      final other = thumbnailAccountKey(
        Uri.parse('https://vdi.example.com'),
        'another-account',
      );
      expect(other, isNot('5f13825dbc51da81'));
      expect(isThumbnailAccountKey(other), isTrue);
      for (final bad in [
        '5F13825DBC51DA81',
        '5f13825dbc51da8',
        '../13825dbc51da8',
        null,
        42,
      ]) {
        expect(isThumbnailAccountKey(bad), isFalse, reason: '$bad');
      }
    });

    test('the login response supplies the immutable account id', () {
      final session = BrokerSession.fromJson({
        'sessionToken': 'test-token',
        'expiresAt': '2026-10-25T09:00:00Z',
        'user': {'id': account, 'username': '한글아이디'},
      });
      expect(session.accountId, account);
      expect(
        BrokerSession.fromJson({
          'sessionToken': 'test-token',
          'expiresAt': '2026-10-25T09:00:00Z',
          'user': {'username': 'legacy'},
        }).accountId,
        isNull,
      );
    });

    test('the session window launch carries only a valid key', () {
      final launch = SessionWindowLaunch(
        origin: Uri.parse('https://vdi.example.com'),
        token: 'fixture-ephemeral-token',
        expiresAt: DateTime.now().add(const Duration(hours: 1)),
        desktopId: 'pve-149',
        name: 'TESTER',
        mode: SessionDisplayMode.windowed,
        options: const StreamOptions(),
        thumbnailKey: '5f13825dbc51da81',
        thumbnailEpoch: epoch,
      );
      final map = launch.toMap();
      expect(map['thumbnailKey'], '5f13825dbc51da81');
      expect(map['thumbnailEpoch'], epoch);
      expect(map.containsKey('accountId'), isFalse);
      expect(SessionWindowLaunch.fromMap(map).thumbnailKey, '5f13825dbc51da81');
      final without = {...map}
        ..remove('thumbnailKey')
        ..remove('thumbnailEpoch');
      expect(SessionWindowLaunch.fromMap(without).thumbnailKey, isNull);
      expect(
        () => SessionWindowLaunch.fromMap({...map, 'thumbnailKey': '../x'}),
        throwsFormatException,
      );
      // An approved local PC's window writes its last frame like a VM's.
      const pc = 'pc-0123abcd-4567-49ef-8123-456789abcdef';
      final pcLaunch = SessionWindowLaunch.fromMap({
        ...map,
        'desktopId': pc,
        'localPcApproved': true,
      });
      expect(pcLaunch.thumbnailKey, '5f13825dbc51da81');
      expect(pcLaunch.thumbnailEpoch, epoch);
    });
  });

  group('read', () {
    const key = '5f13825dbc51da81';

    test('a missing, empty, foreign or torn file is no picture', () async {
      expect(await store.read(key, 'pve-149'), isNull);
      await put(key, 'pve-149', []);
      expect(await store.read(key, 'pve-149'), isNull);
      await put(key, 'pve-149', List.filled(64, 0x41));
      expect(await store.read(key, 'pve-149'), isNull);
      // A write cut short has its start but no end marker.
      await put(key, 'pve-149', jpeg.sublist(0, jpeg.length - 2));
      expect(await store.read(key, 'pve-149'), isNull);
      await put(key, 'pve-149', [...jpeg, 0x00, 0x00]);
      expect(await store.read(key, 'pve-149'), isNull);
    });

    test('identifiers that could leave the folder are refused', () async {
      expect(await store.read('../4825dbc51da81', 'pve-149'), isNull);
      expect(await store.read(key, '..\\pve-149'), isNull);
      expect(await store.read(key, 'pve-149.jpg'), isNull);
      expect(() => store.file(key, 'pve-1/../x'), throwsArgumentError);
    });

    test('a complete picture is read with its time; the cache key follows '
        'the file, and a replacement notice always yields a new key', () async {
      final file = await put(key, 'pve-149', jpeg);
      final first = await store.read(key, 'pve-149');
      expect(first, isNotNull);
      final image = first!.image as MemoryImage;
      expect(image.bytes, jpeg);
      expect(first.modified, (await file.stat()).modified);
      // Unchanged: the same value, so the tile does not decode again.
      expect(await store.read(key, 'pve-149'), same(first));
      // Rewritten with other content: a different image identity.
      await put(
        key,
        'pve-149',
        markedJpeg([...rawJpeg.sublist(0, 8), 0x01, 0x02, 0xFF, 0xD9]),
      );
      final second = await store.read(key, 'pve-149');
      expect(second, isNotNull);
      expect(second!.image, isNot(first.image));
      // A notice re-reads even if time and size look unchanged.
      final third = await store.read(key, 'pve-149', refresh: true);
      expect(third!.image, isNot(second.image));
      expect(await store.read(key, 'pve-149'), same(third));
      await file.delete();
      expect(await store.read(key, 'pve-149'), isNull);
    });

    test('image identity includes path, time, length and revision', () {
      final file = File('a.jpg'), time = DateTime(2026, 10, 25, 9);
      ThumbnailFileImage of({
        DateTime? at,
        int length = 10,
        int revision = 0,
      }) => ThumbnailFileImage(
        file,
        modified: at ?? time,
        length: length,
        revision: revision,
      );
      expect(of(), of());
      expect(of().hashCode, of().hashCode);
      expect(of(at: time.add(const Duration(seconds: 1))), isNot(of()));
      expect(of(length: 11), isNot(of()));
      expect(of(revision: 1), isNot(of()));
      expect(
        ThumbnailFileImage(File('b.jpg'), modified: time, length: 10),
        isNot(of()),
      );
    });
  });

  group('deletion', () {
    const mine = '5f13825dbc51da81', previous = 'f16c66eea1612389';

    test('sign-out deletes the whole account folder only', () async {
      await put(mine, 'pve-149', jpeg);
      await put(mine, 'pve-150', jpeg);
      File('${store.folder(mine).path}${Platform.pathSeparator}pve-150.jpg.tmp')
          .writeAsBytesSync(jpeg);
      await put(previous, 'pve-149', jpeg);
      expect(await store.read(mine, 'pve-149'), isNotNull);
      await store.forget(mine);
      expect(await store.folder(mine).exists(), isFalse);
      expect(await store.read(mine, 'pve-149'), isNull);
      expect(await store.read(previous, 'pve-149'), isNotNull);
      // Repeating it, or an invalid key, is harmless.
      await store.forget(mine);
      await store.forget('..');
      expect(await root.exists(), isTrue);
    });

    test('another account signing in removes every other account folder and '
        'nothing else', () async {
      await put(mine, 'pve-149', jpeg);
      await put(previous, 'pve-149', jpeg);
      await put('0123456789abcdef', 'pve-7', jpeg);
      final unrelated = Directory('${root.path}${Platform.pathSeparator}notes');
      await unrelated.create();
      final loose = File('${root.path}${Platform.pathSeparator}readme.txt');
      await loose.writeAsString('x');
      await store.retainOnly(mine);
      expect(await store.folder(previous).exists(), isFalse);
      expect(await store.folder('0123456789abcdef').exists(), isFalse);
      expect(await store.read(mine, 'pve-149'), isNotNull);
      expect(await unrelated.exists(), isTrue);
      expect(await loose.exists(), isTrue);
    });

    test('a missing pictures folder is not an error', () async {
      final empty = DesktopThumbnails(
        Directory('${root.path}${Platform.pathSeparator}absent'),
      );
      await empty.retainOnly(mine);
      await empty.forget(mine);
      expect(await empty.read(mine, 'pve-149'), isNull);
    });
  });

  test(
    'an account view reads its own folder and hears only its updates',
    () async {
      const mine = '5f13825dbc51da81';
      await put(mine, 'pve-149', jpeg);
      final updates = StreamController<ThumbnailUpdate>.broadcast();
      addTearDown(updates.close);
      final view = AccountThumbnails(store, mine, updates: updates.stream);
      expect(await view.read('pve-149'), isNotNull);
      expect(await view.read('pve-150'), isNull);
      final heard = <String>[];
      final subscription = view.updates.listen(heard.add);
      updates
        ..add(const ThumbnailUpdate('f16c66eea1612389', 'pve-149'))
        ..add(const ThumbnailUpdate(mine, 'pve-150'));
      await Future<void>.delayed(Duration.zero);
      await subscription.cancel();
      expect(heard, ['pve-150']);
    },
  );

  test('the caption says how long ago the picture was taken', () {
    final now = DateTime(2026, 10, 25, 12);
    String at(Duration age) => lastScreenCaption(now.subtract(age), now);
    expect(at(Duration.zero), '마지막 화면 · 방금');
    expect(at(const Duration(seconds: 59)), '마지막 화면 · 방금');
    expect(at(const Duration(minutes: 3)), '마지막 화면 · 3분 전');
    expect(at(const Duration(minutes: 59, seconds: 59)), '마지막 화면 · 59분 전');
    expect(at(const Duration(hours: 2, minutes: 30)), '마지막 화면 · 2시간 전');
    expect(at(const Duration(days: 1)), '마지막 화면 · 1일 전');
    expect(at(const Duration(days: 12)), '마지막 화면 · 12일 전');
    // A clock that went backwards is still "just now", never negative.
    expect(at(const Duration(minutes: -5)), '마지막 화면 · 방금');
    // A snapshot says it is the stored last screen.
    expect(
      lastScreenCaption(
        now.subtract(const Duration(hours: 2)),
        now,
        snapshot: true,
      ),
      '저장된 마지막 화면 · 2시간 전',
    );
  });

  test('a picture reads as a snapshot from ten minutes on', () {
    final now = DateTime(2026, 10, 25, 12);
    bool old(Duration age) => thumbnailIsOld(now.subtract(age), now);
    expect(old(Duration.zero), isFalse);
    expect(old(const Duration(minutes: 9, seconds: 59)), isFalse);
    expect(old(const Duration(minutes: 10)), isTrue);
    expect(old(const Duration(days: 3)), isTrue);
    expect(old(const Duration(minutes: -5)), isFalse);
  });
}
