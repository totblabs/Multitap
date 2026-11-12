// The shared-folder panel: live rows with the VM drive letter or the reason,
// 공유 해제 applying at once (no 읽기 전용 switch since 2026-11-12), 폴더 추가… with its limits, and
// the ended state. Fake socket and file system.
import 'dart:async';

import 'package:flutter/material.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:multitap_client/data/drive_files.dart';
import 'package:multitap_client/data/drive_memory.dart';
import 'package:multitap_client/data/drive_rules.dart';
import 'package:multitap_client/data/drive_session.dart';
import 'package:multitap_client/data/product_log.dart';
import 'package:multitap_client/ui/app_theme.dart';
import 'package:multitap_client/ui/drive_panel.dart';
import 'package:multitap_client/ui/session_menu.dart' show SessionPopups;

import 'drive_session_test.dart'
    show DriveSocket, FakeFiles, driveUrl, flush, ready, sessionId;

void main() {
  late DriveSocket socket;
  late FakeFiles files;
  late SessionDriveMemory memory;

  Future<DriveSession> open(
    WidgetTester tester,
    List<DriveFolder> remembered,
  ) async {
    late DriveSession drive;
    await tester.runAsync(() async {
      await memory.save('https://fixture.invalid', 'pve-149', remembered);
      drive = DriveSession(
        url: driveUrl,
        token: 'not-persisted',
        sessionId: sessionId,
        files: files,
        memory: memory,
        server: 'https://fixture.invalid',
        desktopId: 'pve-149',
        dialer: (_, _) async => socket,
        log: ProductLog(),
      );
      await drive.start();
      socket.emit(ready);
      await flush();
      for (final folder in remembered) {
        final failure = files.failOpen.remove(folder.path);
        files.pick = DrivePick(folder.path, folder.label);
        expect(await drive.addFolder(), isNull);
        final id = drive.shares.last.id;
        if (folder.readOnly) await drive.setReadOnly(id, true);
        if (failure != null) {
          files.failOpen[folder.path] = failure;
          // The folder disappears after the explicit pick, during reopen.
          await drive.setReadOnly(id, !folder.readOnly);
          await drive.setReadOnly(id, folder.readOnly);
        }
      }
      files.pick = null;
    });
    return drive;
  }

  Future<void> show(WidgetTester tester, DriveSession drive) async {
    await tester.pumpWidget(
      MaterialApp(
        theme: buildAppTheme(brightness: Brightness.dark),
        home: Scaffold(
          body: Center(
            child: SizedBox(width: 384, child: DrivePanel(drive: drive)),
          ),
        ),
      ),
    );
    await tester.pump();
  }

  setUp(() {
    socket = DriveSocket();
    files = FakeFiles();
    memory = SessionDriveMemory();
  });

  testWidgets('empty: says so and offers 폴더 추가…', (tester) async {
    final drive = await open(tester, const []);
    await show(tester, drive);
    expect(find.text('아직 공유한 폴더가 없어요.'), findsOneWidget);
    final add = tester.widget<OutlinedButton>(
      find.byKey(const Key('drive-add')),
    );

    expect(add.onPressed, isNotNull);
  });

  testWidgets('rows follow the session; no read-only switch; remove applies at once', (
    tester,
  ) async {
    files.failOpen[r'C:\Gone'] = 'drive_not_found';
    final drive = await open(tester, const [
      DriveFolder(path: r'C:\Data', label: 'Data'),
      DriveFolder(path: r'C:\Gone', label: 'Gone'),
    ]);
    await show(tester, drive);
    final data = drive.shares[0], gone = drive.shares[1];
    expect(find.text('Data'), findsOneWidget);
    expect(find.text(r'C:\Data'), findsOneWidget);
    expect(find.text('연결 중…'), findsOneWidget);
    expect(find.text(driveMessage('drive_not_found')), findsOneWidget);
    expect(find.byKey(ValueKey('drive-retry-${gone.id}')), findsOneWidget);

    await tester.runAsync(() async {
      socket.emit({
        'type': 'drive.mounted',
        'shares': [
          {'id': data.id, 'state': 'mapped', 'letter': 'Z'},
        ],
      });
      await flush();
    });
    await tester.pump();
    expect(find.text('Z: 연결됨'), findsOneWidget);

    // No 읽기 전용 switch; a shared folder can be changed.
    expect(find.byKey(ValueKey('drive-readonly-${data.id}')), findsNothing);
    expect(find.text('읽기 전용'), findsNothing);
    expect(files.opened[data.id]!.readOnly, isFalse);

    await tester.tap(find.byKey(ValueKey('drive-remove-${gone.id}')));
    await tester.pumpAndSettle();
    await tester.tap(find.byKey(const Key('drive-remove-confirm-yes')));
    await tester.runAsync(flush);
    await tester.pumpAndSettle();
    expect(find.text('Gone'), findsNothing);
    expect(drive.shares, hasLength(1));
  });

  testWidgets('폴더 추가… shows why a folder cannot be shared, in place', (
    tester,
  ) async {
    final drive = await open(tester, const []);
    await show(tester, drive);
    files.pick = const DrivePick(r'C:\Locked', 'Locked');
    files.failOpen[r'C:\Locked'] = 'drive_denied';
    await tester.tap(find.byKey(const Key('drive-add')));
    await tester.runAsync(flush);
    await tester.pumpAndSettle();
    expect(find.byKey(const Key('drive-notice')), findsOneWidget);
    expect(find.text(driveMessage('drive_denied')), findsOneWidget);
    files.failOpen.clear();
    files.pick = const DrivePick(r'C:\Music', 'Music');
    await tester.tap(find.byKey(const Key('drive-add')));
    await tester.runAsync(flush);
    await tester.pumpAndSettle();
    expect(find.byKey(const Key('drive-notice')), findsNothing);
    expect(find.text('Music'), findsOneWidget);
  });

  testWidgets('a pulled USB stick: the row says so and 다시 시도 reopens it', (
    tester,
  ) async {
    final drive = await open(tester, const [
      DriveFolder(path: r'E:\Photos', label: 'Photos'),
    ]);
    await show(tester, drive);
    final share = drive.shares.single;
    await tester.runAsync(() async {
      socket.emit({
        'type': 'drive.mounted',
        'shares': [
          {'id': share.id, 'state': 'mapped', 'letter': 'Y'},
        ],
      });
      await flush();
      files.offline.add(share.id);
      await drive.checkFolders();
    });
    await tester.pump();
    expect(find.text(driveOfflineText), findsOneWidget);
    expect(find.text('Y: 연결됨'), findsNothing);
    expect(find.byKey(ValueKey('drive-retry-${share.id}')), findsOneWidget);
    // Not the green of a folder the desktop can use (review 2026-11-12).
    const p = AppPalette(true);
    Color folderIcon() =>
        tester.widget<Icon>(find.byIcon(Icons.folder_rounded)).color!;
    expect(folderIcon(), p.amber);
    await tester.runAsync(() async {
      files.offline.clear();
      await drive.retry(share.id);
    });
    await tester.pump();
    expect(find.text(driveOfflineText), findsNothing);
    expect(find.text('Y: 연결됨'), findsOneWidget);
    expect(folderIcon(), p.green);
  });

  testWidgets('a cut listing is said once in the panel', (tester) async {
    final drive = await open(tester, const [
      DriveFolder(path: r'C:\Data', label: 'Data'),
    ]);
    await show(tester, drive);
    expect(find.byKey(const Key('drive-truncated')), findsNothing);
    await tester.runAsync(() async {
      files.listing = [
        for (var i = 0; i < 20000; i++) DriveEntry(name: 'f$i', dir: false),
      ];
      socket.emit({
        'type': 'drive.request',
        'id': 1,
        'op': 'list',
        'share': drive.shares.single.id,
        'path': '',
        'cursor': 0,
      });
      await flush();
    });
    await tester.pump();
    expect(find.text(driveTruncatedText), findsNothing);
    await tester.tap(find.byKey(const Key('drive-details')));
    await tester.pump();
    expect(find.text(driveTruncatedText), findsOneWidget);
  });

  testWidgets('the Hosts hints are said once in the panel', (tester) async {
    final drive = await open(tester, const []);
    await show(tester, drive);
    expect(find.byKey(const Key('drive-old-limit')), findsNothing);
    expect(find.byKey(const Key('drive-too-large')), findsNothing);
    await tester.runAsync(() async {
      socket.emit({
        'type': 'drive.notice',
        'code': 'drive_webclient_old_limit',
      });
      socket.emit({
        'type': 'drive.notice',
        'code': 'drive_webclient_old_limit',
      });
      await flush();
    });
    await tester.pump();
    expect(find.text(driveOldLimitText), findsNothing);
    await tester.tap(find.byKey(const Key('drive-details')));
    await tester.pump();
    expect(find.text(driveOldLimitText), findsOneWidget);
    expect(driveOldLimitText, '데스크톱을 다시 시작하기 전까지는 50 MB가 넘는 파일을 옮길 수 없어요.');
    expect(find.byKey(const Key('drive-too-large')), findsNothing);
    await tester.runAsync(() async {
      socket.emit({'type': 'drive.notice', 'code': 'drive_file_too_large'});
      await flush();
    });
    await tester.pump();
    expect(find.text(driveTooLargeText), findsOneWidget);
    expect(
      driveTooLargeText,
      '파일 하나가 4 GB를 넘으면 공유 폴더로 옮길 수 없어요. 클립보드 복사·붙여넣기로 옮기세요.',
    );
  });
  testWidgets('a drive root or system folder is named in the confirmation', (
    tester,
  ) async {
    final drive = await open(tester, const []);
    await tester.pumpWidget(
      MaterialApp(
        theme: buildAppTheme(brightness: Brightness.dark),
        home: Scaffold(
          body: Center(
            child: SizedBox(
              width: 384,
              child: DrivePanel(
                drive: drive,
                riskOf: (path) => driveRiskOf(
                  path,
                  environment: const {'SystemRoot': r'C:\Windows'},
                ),
              ),
            ),
          ),
        ),
      ),
    );
    await tester.pump();
    for (final (path, warning) in [
      (r'D:\', driveRiskMessage(DriveRisk.driveRoot)),
      (r'C:\Windows\Temp', driveRiskMessage(DriveRisk.system)),
      (r'C:\Work', null),
    ]) {
      files.pick = DrivePick(path, 'x');
      await tester.tap(find.byKey(const Key('drive-add')));
      await tester.runAsync(flush);
      await tester.pumpAndSettle();
      if (warning == null) {
        // An ordinary folder asks nothing more.
        expect(
          find.byKey(const Key('drive-add-confirm')),
          findsNothing,
          reason: path,
        );
        continue;
      }
      expect(
        find.textContaining('주의: $warning'),
        findsOneWidget,
        reason: path,
      );
      await tester.tap(find.byKey(const Key('drive-add-confirm-no')));
      await tester.runAsync(flush);
      await tester.pumpAndSettle();
    }
    expect(drive.shares.single.path, r'C:\Work');
  });

  testWidgets(
    'the read-write consequence is said beside 폴더 추가… first; an ordinary folder is shared as picked, with no second question',
    (tester) async {
      final drive = await open(tester, const []);
      await show(tester, drive);
      expect(
        tester
            .widget<Text>(find.byKey(const Key('drive-add-consequence')))
            .data,
        '데스크톱에서 공유한 폴더의 파일을 읽고 바꿀 수 있어요.',
      );
      files.pick = const DrivePick(r'C:\Music', 'Music');
      await tester.tap(find.byKey(const Key('drive-add')));
      await tester.runAsync(flush);
      await tester.pumpAndSettle();
      expect(find.byKey(const Key('drive-add-confirm')), findsNothing);
      expect(drive.shares.single.path, r'C:\Music');
      expect(drive.shares.single.readOnly, isFalse);
      expect(files.opened, hasLength(1));
    },
  );

  testWidgets(
    'the real flow: the native picker takes the focus and the session closes '
    'its popups; the choice is still confirmed, shared and offered',
    (tester) async {
      final drive = await open(tester, const []);
      final popups = SessionPopups();
      late BuildContext host;
      await tester.pumpWidget(
        MaterialApp(
          theme: buildAppTheme(brightness: Brightness.dark),
          home: Scaffold(
            body: Builder(
              builder: (context) {
                host = context;
                return const SizedBox.expand();
              },
            ),
          ),
        ),
      );
      unawaited(
        showDrivePanel(
          host,
          drive: drive,
          anchor: const Rect.fromLTWH(300, 0, 40, 30),
          popups: popups,
        ),
      );
      await tester.pumpAndSettle();
      expect(find.byKey(const Key('drive-panel')), findsOneWidget);
      // A drive root still asks, so the question must survive the panel.
      files.pick = const DrivePick(r'D:\', 'Data');
      // What the window's deactivation does while the picker is open.
      files.whilePicking = popups.closeAll;
      await tester.tap(find.byKey(const Key('drive-add')));
      await tester.runAsync(flush);
      await tester.pumpAndSettle();
      expect(tester.takeException(), isNull);
      expect(
        find.byKey(const Key('drive-panel')),
        findsNothing,
        reason: 'the deactivation removed the panel while the picker was up',
      );
      expect(find.byKey(const Key('drive-add-confirm')), findsOneWidget);
      await tester.tap(find.byKey(const Key('drive-add-confirm-yes')));
      await tester.runAsync(flush);
      await tester.pumpAndSettle();
      expect(drive.shares.single.path, r'D:\');
      expect(files.opened, hasLength(1));
      expect(
        (socket.lastOf('drive.shares')['shares'] as List).single['name'],
        'Data',
      );
      expect(await memory.load('https://fixture.invalid', 'pve-149'), isEmpty);
    },
  );

  testWidgets('removing asks first; 취소 keeps the share', (tester) async {
    final drive = await open(tester, const [
      DriveFolder(path: r'C:\Data', label: 'Data'),
    ]);
    await show(tester, drive);
    final id = drive.shares.single.id;
    await tester.tap(find.byKey(ValueKey('drive-remove-$id')));
    await tester.pumpAndSettle();
    expect(find.text('공유를 끝낼까요?'), findsOneWidget);
    expect(find.textContaining('저장하지 못할 수 있어요'), findsOneWidget);
    // The row's own button and the question's confirm button both say it.
    expect(find.text('공유 끝내기'), findsNWidgets(2));
    expect(
      find.descendant(of: find.byType(Dialog), matching: find.text('공유 끝내기')),
      findsOneWidget,
    );
    await tester.tap(find.byKey(const Key('drive-remove-confirm-no')));
    await tester.runAsync(flush);
    await tester.pumpAndSettle();
    expect(drive.shares, hasLength(1));
    expect(find.text('Data'), findsOneWidget);

    await tester.tap(find.byKey(ValueKey('drive-remove-$id')));
    await tester.pumpAndSettle();
    await tester.tap(find.byKey(const Key('drive-remove-confirm-yes')));
    await tester.runAsync(flush);
    await tester.pumpAndSettle();
    expect(drive.shares, isEmpty);
  });

  testWidgets('a shared folder deleted on the PC ends its share and says so', (
    tester,
  ) async {
    final drive = await open(tester, const [
      DriveFolder(path: r'C:\Data', label: 'Data'),
    ]);
    await show(tester, drive);
    final id = drive.shares.single.id;
    files.deleted.add(id);
    await tester.runAsync(() async {
      await drive.checkFolders();
      await flush();
    });
    await tester.pump();
    expect(drive.shares, isEmpty);
    expect(find.text('Data'), findsNothing);
    expect(find.byKey(const Key('drive-gone')), findsOneWidget);
    expect(find.text('‘Data’ 폴더가 삭제돼서 공유를 끝냈어요'), findsOneWidget);
    expect(socket.lastOf('drive.shares')['shares'], isEmpty);
    expect(files.closed, contains(id));
    // Review 2026-11-12: the line stayed for the whole connection. It goes
    // with the person's next action in the panel.
    await tester.runAsync(() async {
      files.pick = const DrivePick(r'C:\Other', 'Other');
      expect(await drive.addFolder(), isNull);
      await flush();
    });
    await tester.pump();
    expect(drive.lastGone, isNull);
    expect(drive.goneCount, 1, reason: 'the count for the toast stays');
    expect(find.byKey(const Key('drive-gone')), findsNothing);
  });

  // Review 2026-11-12: an open 폴더·USB panel told the person to reconnect
  // and pick the folders again while the channel was coming back by itself.
  testWidgets('a channel that is coming back by itself says so', (tester) async {
    final drive = await open(tester, const [
      DriveFolder(path: r'C:\Data', label: 'Data'),
    ]);
    await show(tester, drive);
    await tester.runAsync(() async {
      socket.emit({'type': 'drive.error', 'reason': 'drive_transport_closed'});
      await flush();
    });
    expect(drive.closed, isTrue);
    drive.recovering = true;
    await tester.pump();
    expect(find.text('공유 폴더를 다시 연결하고 있어요. 잠시만 기다려 주세요.'), findsOneWidget);
    expect(find.textContaining('폴더를 다시 고르세요'), findsNothing);
    drive.recovering = false;
    await tester.pump();
    expect(find.textContaining('폴더를 다시 고르세요'), findsOneWidget);
  });

  testWidgets('eight folders: 폴더 추가… is disabled and says why', (tester) async {
    final drive = await open(tester, [
      for (var i = 0; i < 8; i++) DriveFolder(path: 'C:\\F$i', label: 'F$i'),
    ]);
    await show(tester, drive);
    final add = tester.widget<OutlinedButton>(
      find.byKey(const Key('drive-add')),
    );

    expect(add.onPressed, isNull);
    final tooltip = tester.widget<Tooltip>(
      find.ancestor(
        of: find.byKey(const Key('drive-add')),
        matching: find.byType(Tooltip),
      ),
    );
    expect(tooltip.message, '폴더는 8개까지 공유할 수 있어요');
  });

  testWidgets('after revocation shares are withdrawn and nothing can change', (
    tester,
  ) async {
    final drive = await open(tester, const [
      DriveFolder(path: r'C:\Data', label: 'Data'),
    ]);
    await show(tester, drive);
    await tester.runAsync(() async {
      socket.emit({'type': 'drive.error', 'reason': 'drive_policy_changed'});
      await flush();
    });
    await tester.pump();
    expect(drive.shares, isEmpty);
    expect(files.opened, isEmpty);
    expect(find.textContaining('관리자가 공유 폴더 권한을 해제했어요'), findsOneWidget);
    expect(find.byType(Switch), findsNothing);
    expect(
      tester
          .widget<OutlinedButton>(find.byKey(const Key('drive-add')))
          .onPressed,
      isNull,
    );
  });
}
