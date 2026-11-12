import 'package:flutter/material.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:multitap_client/data/drive_files.dart';
import 'package:multitap_client/data/drive_session.dart';
import 'package:multitap_client/data/product_log.dart';
import 'package:multitap_client/ui/app_theme.dart';
import 'package:multitap_client/ui/drive_panel.dart';

import 'drive_session_test.dart' as fixture;

Future<void> withDrive(
  WidgetTester t,
  Future<void> Function(DriveSession, fixture.DriveSocket, String) body, {
  bool supported = true,
  bool mapped = true,
}) async {
  final socket = fixture.DriveSocket(), files = fixture.FakeFiles();
  final drive = DriveSession(
    url: fixture.driveUrl,
    token: 'fixture-only',
    sessionId: fixture.sessionId,
    files: files,
    log: ProductLog(),
    dialer: (_, _) async => socket,
  );
  try {
    await t.runAsync(() async {
      await drive.start();
      socket.emit({
        ...fixture.ready,
        if (supported) 'openProtocol': 'drive.open-v1',
      });
      await fixture.flush();
      files.pick = const DrivePick(r'C:\Synthetic', 'Synthetic');
      expect(await drive.addFolder(), isNull);
      if (mapped) {
        socket.emit({
          'type': 'drive.mounted',
          'shares': [
            {'id': drive.shares.single.id, 'state': 'mapped', 'letter': 'Z'},
          ],
        });
        await fixture.flush();
      }
    });
    await body(drive, socket, drive.shares.single.id);
  } finally {
    await t.pumpWidget(const SizedBox.shrink());
    await t.runAsync(() async {
      await drive.close();
      await fixture.flush();
    });
    drive.dispose();
  }
}

Future<void> receive(
  WidgetTester t,
  fixture.DriveSocket socket,
  Object value,
) async {
  socket.emit(value);
  await t.runAsync(fixture.flush);
  await t.pump();
}

void main() {
  testWidgets('old peer has no open action and performs no open request', (
    t,
  ) async {
    await withDrive(t, (drive, socket, id) async {
      expect(drive.canOpen(id), false);
      expect(await drive.openShare(id), 'drive_open_unavailable');
      expect(socket.sent.where((x) => x['type'] == 'drive.open'), isEmpty);
    }, supported: false);
  });
  testWidgets('pending share is never openable before actual mapped status', (
    t,
  ) async {
    await withDrive(t, (drive, socket, id) async {
      expect(drive.canOpen(id), false);
      expect(await drive.openShare(id), 'drive_open_unavailable');
    }, mapped: false);
  });
  testWidgets('share-only control ID is independent from file reply IDs', (
    t,
  ) async {
    await withDrive(t, (drive, socket, id) async {
      await receive(t, socket, {
        'type': 'drive.request',
        'id': 1,
        'op': 'stat',
        'share': id,
        'path': '',
      });
      expect(socket.response(1)['ok'], true);
      final result = drive.openShare(id);
      expect(socket.lastOf('drive.open'), {
        'type': 'drive.open',
        'id': 1,
        'share': id,
      });
      await receive(t, socket, {
        'type': 'drive.opened',
        'id': 1,
        'share': id,
        'ok': true,
      });
      expect(await result, isNull);
      expect(drive.canOpen(id), true);
    });
  });
  testWidgets(
    'observation deadline retains slot; late actual ACK permits retry',
    (t) async {
      await withDrive(t, (drive, socket, id) async {
        final result = drive.openShare(id);
        await t.pump(const Duration(seconds: 5));
        expect(await result, 'drive_open_unknown');
        expect(drive.openingShare, id);
        expect(await drive.openShare(id), 'drive_busy');
        expect(
          socket.sent.where((x) => x['type'] == 'drive.open'),
          hasLength(1),
        );
        await receive(t, socket, {
          'type': 'drive.opened',
          'id': 1,
          'share': id,
          'ok': true,
        });
        expect(drive.openError, isNull);
        expect(drive.canOpen(id), true);
      });
    },
  );
  // A failed 열기 must not stay in the panel after a later action has
  // succeeded.
  testWidgets('what a failed open left goes with the next action in the panel', (
    t,
  ) async {
    await withDrive(t, (drive, socket, id) async {
      final result = drive.openShare(id);
      await receive(t, socket, {
        'type': 'drive.opened',
        'id': 1,
        'share': id,
        'ok': false,
        'error': 'drive_open_failed',
      });
      expect(await result, 'drive_open_failed');
      expect(drive.openError, 'drive_open_failed');
      await t.runAsync(() async {
        (drive.files as fixture.FakeFiles).pick = const DrivePick(
          r'C:\Other',
          'Other',
        );
        expect(await drive.addFolder(), isNull);
      });
      expect(drive.openError, isNull);
    });
  });

  testWidgets(
    'share removal and session end never turn a late reply into success',
    (t) async {
      await withDrive(t, (drive, socket, id) async {
        final result = drive.openShare(id);
        await drive.remove(id);
        await receive(t, socket, {
          'type': 'drive.opened',
          'id': 1,
          'share': id,
          'ok': true,
        });
        expect(await result, 'drive_share_unknown');
      });
      await withDrive(t, (drive, socket, id) async {
        final result = drive.openShare(id);
        await t.runAsync(drive.close);
        expect(await result, 'drive_open_unknown');
        expect(drive.openingShare, isNull);
        expect(drive.canOpen(id), false);
      });
    },
  );
  testWidgets(
    'actual terminal unknown does not auto repeat but permits explicit retry',
    (t) async {
      await withDrive(t, (drive, socket, id) async {
        final first = drive.openShare(id);
        await receive(t, socket, {
          'type': 'drive.opened',
          'id': 1,
          'share': id,
          'ok': false,
          'error': 'drive_open_unknown',
        });
        expect(await first, 'drive_open_unknown');
        expect(drive.canOpen(id), true);
        expect(
          socket.sent.where((x) => x['type'] == 'drive.open'),
          hasLength(1),
        );
        final second = drive.openShare(id);
        await receive(t, socket, {
          'type': 'drive.opened',
          'id': 2,
          'share': id,
          'ok': true,
        });
        expect(await second, isNull);
      });
    },
  );
  testWidgets(
    'narrow enlarged panel shows simple open and folds passive detail',
    (t) async {
      await withDrive(t, (drive, socket, id) async {
        drive.oldFileLimit = true;
        await t.pumpWidget(
          MaterialApp(
            theme: buildAppTheme(brightness: Brightness.dark),
            home: MediaQuery(
              data: const MediaQueryData(textScaler: TextScaler.linear(2)),
              child: Scaffold(
                body: Center(
                  child: SizedBox(
                    width: 360,
                    height: 620,
                    child: DrivePanel(drive: drive),
                  ),
                ),
              ),
            ),
          ),
        );
        await t.pump();
        expect(find.text('열기'), findsOneWidget);
        expect(
          find.byKey(ValueKey('drive-remove-$id')).hitTestable(),
          findsOneWidget,
        );
        expect(find.byKey(const Key('drive-old-limit')), findsNothing);
        await t.tap(find.byKey(const Key('drive-details')));
        await t.pump();
        expect(find.byKey(const Key('drive-old-limit')), findsOneWidget);
        expect(t.takeException(), isNull);
        // Narrow and 2x text: the row's buttons stand under its text, below the fold.
        await t.ensureVisible(find.byKey(ValueKey('drive-open-$id')));
        await t.pump();
        await t.tap(find.byKey(ValueKey('drive-open-$id')));
        await t.pump();
        expect(socket.lastOf('drive.open')['share'], id);
        await receive(t, socket, {
          'type': 'drive.opened',
          'id': 1,
          'share': id,
          'ok': true,
        });
      });
    },
  );
}
