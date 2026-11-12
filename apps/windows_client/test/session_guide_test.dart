import 'dart:io';

import 'package:flutter_test/flutter_test.dart';
import 'package:multitap_client/data/clipboard_policy.dart';
import 'package:multitap_client/data/launcher_memory.dart';
import 'package:multitap_client/data/session_guide_memory.dart';
import 'package:multitap_client/data/session_windows.dart';
import 'package:multitap_client/models/session_display_mode.dart';
import 'package:multitap_client/models/session_guide.dart';
import 'package:multitap_client/models/stream_options.dart';

void main() {
  test('guide preference scope isolates server, immutable account and VM without names or secrets', () {
    final a = Uri.parse('https://one.example');
    final b = Uri.parse('https://two.example');
    final key = sessionGuideAccountKey(a, 'immutable-id-A')!;
    expect(key, matches(RegExp(r'^[0-9a-f]{64}$')));
    expect(key, isNot(contains('immutable-id-A')));
    expect(key, isNot(contains('one.example')));
    expect(
      sessionGuideAccountKey(
        Uri.parse('https://one.example:443/'),
        'immutable-id-A',
      ),
      key,
    );
    expect(
      sessionGuideAccountKey(
        Uri.parse('https://one.example/path'),
        'immutable-id-A',
      ),
      isNull,
    );
    expect(sessionGuideAccountKey(b, 'immutable-id-A'), isNot(key));
    expect(sessionGuideAccountKey(a, 'immutable-id-B'), isNot(key));
    expect(sessionGuideAccountKey(a, null), isNull);
    expect(
      sessionGuideAccountKey(Uri.parse('http://one.example'), 'immutable-id-A'),
      isNull,
    );
    final first = SessionGuideIdentity(accountKey: key, desktopId: 'pve-149');
    final second = SessionGuideIdentity(accountKey: key, desktopId: 'pve-170');
    expect(first.storageKey, isNot(second.storageKey));
    expect(first.storageKey, matches(RegExp(r'^[0-9a-f]{64}$')));
  });

  test('optional launch hash round-trips and rejects malformed hashes without requiring a C++ field', () {
    final launch = SessionWindowLaunch(
      origin: Uri.parse('https://one.example'),
      token: 'synthetic-only',
      expiresAt: DateTime.utc(2030),
      desktopId: 'pve-149',
      name: '검사 VM',
      mode: SessionDisplayMode.windowed,
      options: const StreamOptions(),
      guideAccountKey: sessionGuideAccountKey(
        Uri.parse('https://one.example'),
        'stable-account',
      ),
    );
    expect(
      SessionWindowLaunch.fromMap(launch.toMap()).guideAccountKey,
      launch.guideAccountKey,
    );
    final legacy = {...launch.toMap()}..remove('guideAccountKey');
    expect(SessionWindowLaunch.fromMap(legacy).guideAccountKey, isNull);
    for (final invalid in [
      '',
      'account-name',
      List.filled(63, 'f').join(),
      1,
    ]) {
      expect(
        () => SessionWindowLaunch.fromMap({
          ...launch.toMap(),
          'guideAccountKey': invalid,
        }),
        throwsFormatException,
      );
    }
  });

  test('a preview provider cannot advertise persistent opt-out; the file adapter reuses the safe path API', () {
    expect(SessionLauncherMemory().sessionGuideStore.persistent, isFalse);
    // Constructing a File/Directory does not touch the filesystem.
    final preferences = FileLauncherMemory(
      File('synthetic-preferences/launcher.json'),
    );
    final store = preferences.sessionGuideStore as FileSessionGuideStore;
    expect(store.persistent, isTrue);
    expect(store.directory.path, endsWith('session-guides'));
  });

  test('one VM opt-out does not hide another VM, account or server', () async {
    final store = MemorySessionGuideStore();
    SessionGuideIdentity scope(String host, String id, String vm) =>
        SessionGuideIdentity(
          accountKey: sessionGuideAccountKey(Uri.parse(host), id)!,
          desktopId: vm,
        );
    final a = scope('https://one.example', 'id-A', 'pve-149');
    final others = [
      scope('https://one.example', 'id-A', 'pve-170'),
      scope('https://one.example', 'id-B', 'pve-149'),
      scope('https://two.example', 'id-A', 'pve-149'),
    ];
    await store.setHidden(a, true);
    expect(await store.hidden(a), isTrue);
    for (final other in others) {
      expect(await store.hidden(other), isFalse);
    }
    await store.setHidden(a, false);
    expect(await store.hidden(a), isFalse);
  });

  test(
    'file preference worker keeps bounded reads and independent VM files',
    () async {
      final tempRoot = Directory(
        await Directory.systemTemp.resolveSymbolicLinks(),
      );
      final directory = await tempRoot.createTemp('multitap-guide-worker-');
      addTearDown(() async {
        final resolved = await directory.resolveSymbolicLinks();
        expect(
          resolved.startsWith('${tempRoot.path}${Platform.pathSeparator}'),
          isTrue,
        );
        await Directory(resolved).delete(recursive: true);
      });
      final store = FileSessionGuideStore(directory);
      final account = sessionGuideAccountKey(
        Uri.parse('https://fixture.invalid'),
        'worker-test',
      )!;
      final first = SessionGuideIdentity(
        accountKey: account,
        desktopId: 'vm-one',
      );
      final second = SessionGuideIdentity(
        accountKey: account,
        desktopId: 'vm-two',
      );
      expect(await store.hidden(first), isFalse);
      await store.setHidden(first, true);
      expect(await store.hidden(first), isTrue);
      expect(await store.hidden(second), isFalse);
      await store.setHidden(first, false);
      expect(await store.hidden(first), isFalse);
      await File('${directory.path}/${first.storageKey}.json')
          .writeAsString(' ' * 1025);
      await expectLater(store.hidden(first), throwsFormatException);
    },
  );

  test('missing grants never become usable badges even if stale ready flags are true', () {
    final content = SessionGuideContent.fromFacts(
      printerReady: true,
      usbReady: true,
      foldersReady: true,
      clipboardReady: true,
      policy: const ClipboardPolicy.all(),
    );
    expect(content.tips, hasLength(4));
    expect(
      content.tips.every(
        (tip) => tip.availability == SessionGuideAvailability.unavailable,
      ),
      isTrue,
    );
    expect(
      content.tips.every((tip) => tip.message.startsWith('관리자에게')),
      isTrue,
    );
  });

  test('ready negotiated functions explain their actual workflow without installing or granting anything', () {
    final content = SessionGuideContent.fromFacts(
      printerGranted: true,
      printerReady: true,
      usbGranted: true,
      usbReady: true,
      foldersGranted: true,
      foldersReady: true,
      clipboardGranted: true,
      clipboardReady: true,
      policy: const ClipboardPolicy.all(),
    );
    expect(
      content.tips.every(
        (tip) => tip.availability == SessionGuideAvailability.ready,
      ),
      isTrue,
    );
    expect(content.tips[0].message, contains('미리보기'));
    expect(content.tips[1].message, contains('데스크톱에서만 쓸 수 있어요'));
    expect(content.tips[2].message, contains('연결을 끊으면 공유도 끝나요'));
    expect(content.tips[3].message, contains('붙여넣을 때 전송'));
  });

  test('clipboard instructions retain asymmetric text/file directions', () {
    final content = SessionGuideContent.fromFacts(
      clipboardGranted: true,
      clipboardReady: true,
      policy: const ClipboardPolicy(textToRemote: true),
    );
    final tip = content.tips.last;
    expect(tip.availability, SessionGuideAvailability.ready);
    expect(tip.message, contains('텍스트: PC → 데스크톱'));
    expect(tip.message, isNot(contains('데스크톱 → PC')));
    expect(tip.message, isNot(contains('파일:')));
    expect(tip.message, isNot(contains('붙여넣을 때 전송')));
  });

  test(
    'bidirectional labels stay compact without widening narrower permissions',
    () {
      final all = SessionGuideContent.fromFacts(
        clipboardGranted: true,
        clipboardReady: true,
        policy: const ClipboardPolicy.all(),
      ).tips.last.message;
      expect(all, contains('텍스트: PC ↔ 데스크톱\n파일: PC ↔ 데스크톱'));
      final mixed = SessionGuideContent.fromFacts(
        clipboardGranted: true,
        clipboardReady: true,
        policy: const ClipboardPolicy(textToLocal: true, filesToRemote: true),
      ).tips.last.message;
      expect(mixed, contains('텍스트: 데스크톱 → PC\n파일: PC → 데스크톱'));
      expect(mixed, isNot(contains('↔')));
    },
  );

  test('negotiation is distinguished from current feature readiness and channel failure', () {
    final pending = SessionGuideContent.fromFacts(
      printerGranted: true,
      usbGranted: true,
      foldersGranted: true,
      clipboardGranted: true,
      policy: const ClipboardPolicy.all(),
    );
    expect(
      pending.tips.every(
        (tip) => tip.availability == SessionGuideAvailability.preparing,
      ),
      isTrue,
    );
    expect(
      pending.tips.every((tip) => tip.message.startsWith('연결 준비가 끝나면')),
      isTrue,
    );
    final failed = SessionGuideContent.fromFacts(
      printerGranted: true,
      printerReady: true,
      printerFailed: true,
      usbGranted: true,
      usbReady: true,
      usbFailed: true,
      foldersGranted: true,
      foldersReady: true,
      foldersFailed: true,
      clipboardGranted: true,
      clipboardReady: true,
      clipboardFailed: true,
      policy: const ClipboardPolicy.all(),
    );
    expect(
      failed.tips.every(
        (tip) => tip.availability == SessionGuideAvailability.unavailable,
      ),
      isTrue,
    );
  });
}
