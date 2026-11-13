// Shared folders from Android (drive-v1): the Android folder engine's channel
// contract (AndroidDrive.kt, here a recorded fake), the same DriveSession and
// wire fixtures as the Windows client's drive tests over it, and the 공유 폴더
// row and panel of the Android session menu.
//
// No phone, folder picker, documents provider, Host or VM: nothing here says a
// device shares a folder this way. What a provider cannot do is covered in
// DriveFilesTest.kt (JVM, a fake provider).
import 'dart:convert';
import 'dart:typed_data';

import 'package:flutter/material.dart';
import 'package:flutter/services.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:multitap_client/android/android_drive.dart';
import 'package:multitap_client/android/android_folder_chooser.dart';
import 'package:multitap_client/android/android_remote.dart';
import 'package:multitap_client/android/android_session_keepalive.dart';
import 'package:multitap_client/android/android_store.dart';
import 'package:multitap_client/android/android_toolbar.dart';
import 'package:multitap_client/data/broker_api.dart';
import 'package:multitap_client/data/drive_files.dart';
import 'package:multitap_client/data/drive_session.dart';
import 'package:multitap_client/data/remote_session.dart';
import 'package:multitap_client/models/session_guide.dart';
import 'package:multitap_client/models/stream_options.dart';
import 'package:multitap_client/ui/app_theme.dart';
import 'package:multitap_client/ui/drive_panel.dart';
import 'package:multitap_client/ui/session_panels.dart';

import '../drive_session_test.dart' show DriveSocket, ready, sessionId;
import '../remote_session_test.dart' show SessionMedia, SessionSocket;
import 'android_session_contract_test.dart' show AndroidNativeFake;
import '../helpers/pump_work.dart';

const _sessionUrl = 'wss://fixture.invalid/api/v1/sessions/$sessionId';
const _pen = MethodChannel('multitap/pen-input');
const _tree =
    'content://com.android.externalstorage.documents/tree/primary%3ADocuments';

/// AndroidDrive.kt as the Dart side sees it: one picked tree with files kept
/// in memory. [pick] is what the folder picker answers (null: cancelled).
class _Engine {
  _Engine(this.channel) {
    TestDefaultBinaryMessengerBinding.instance.defaultBinaryMessenger
        .setMockMethodCallHandler(channel, _handle);
  }
  final MethodChannel channel;
  final calls = <MethodCall>[];
  final files = <String, List<int>>{'report.txt': utf8.encode('hello world')};
  Map<String, Object?>? pick = {
    'path': _tree,
    'name': 'Documents',
    'where': '내부 저장공간/Documents',
    'whole': false,
  };
  String? pickError, failWith, storagePickError;
  Object? pickWhy;
  // Off unless a test turns it on: the device of the tests written for
  // Android's picker (Android 10 and older; newer ones use this app's list).
  bool storageSupported = false, storageGranted = false;
  bool revoked = false, gone = false;
  final open = <String, bool>{};

  void dispose() => TestDefaultBinaryMessengerBinding
      .instance
      .defaultBinaryMessenger
      .setMockMethodCallHandler(channel, null);

  int count(String method) => calls.where((c) => c.method == method).length;

  Future<Object?> _handle(MethodCall call) async {
    calls.add(call);
    final args = (call.arguments as Map?) ?? const {};
    final path = args['path'] as String?;
    if (call.method == 'pickFolder') {
      if (pickError != null) {
        throw PlatformException(code: pickError!, details: pickWhy);
      }
      return pick;
    }
    if (call.method == 'pickerSettings') return true;
    // All-files access and this app's own folder list (one volume, two
    // folders in it).
    const volume = '/storage/emulated/0';
    switch (call.method) {
      case 'storageAccess':
        return {'supported': storageSupported, 'granted': storageGranted};
      case 'storageSettings':
        return true;
      case 'storageFolders':
        if (!storageGranted) throw PlatformException(code: 'drive_denied');
        if (path == null) {
          return {
            'folders': [
              {'path': volume, 'name': '내부 저장공간'},
            ],
          };
        }
        return {
          'folders': path == volume
              ? [
                  {'path': '$volume/DCIM', 'name': 'DCIM'},
                  {'path': '$volume/Documents', 'name': 'Documents'},
                ]
              : const <Object>[],
          'path': path,
          'where': path == volume
              ? '내부 저장공간'
              : '내부 저장공간/${path.split('/').last}',
          'parent': path == volume ? null : volume,
        };
      case 'storagePick':
        if (storagePickError != null) {
          throw PlatformException(code: storagePickError!);
        }
        return {
          'path': path,
          'name': path!.split('/').last,
          'where': '내부 저장공간/${path.split('/').last}',
          'whole': false,
        };
    }
    if (failWith != null && call.method != 'close') {
      throw PlatformException(code: failWith!);
    }
    switch (call.method) {
      case 'open':
        if (path != _tree) throw PlatformException(code: 'drive_denied');
        open[args['share'] as String] = args['readOnly'] == true;
        return {'name': 'Documents'};
      case 'close':
        open.remove(args['share']);
        return null;
      case 'status':
        return {
          'deleted': revoked || gone,
          'offline': false,
          'revoked': revoked,
          'path': _tree,
          'name': 'Documents',
        };
      case 'stat':
        if (path == '') {
          return {'name': '', 'dir': true, 'free': 10, 'total': 100};
        }
        final data = files[path];
        if (data == null) throw PlatformException(code: 'drive_not_found');
        return {
          'name': path,
          'dir': false,
          'size': data.length,
          'modified': 1700000000000,
          'created': 1700000000000,
          'readOnly': false,
        };
      case 'list':
        return {
          'entries': [
            for (final entry in files.entries)
              {
                'name': entry.key,
                'dir': false,
                'size': entry.value.length,
                'modified': 1700000000000,
                'created': 1700000000000,
              },
          ],
        };
      case 'read':
        final data = files[path];
        if (data == null) throw PlatformException(code: 'drive_not_found');
        final offset = args['offset'] as int, length = args['length'] as int;
        return Uint8List.fromList(
          data.skip(offset).take(length).toList(growable: false),
        );
      case 'write':
        if (open[args['share']] == true) {
          throw PlatformException(code: 'drive_read_only');
        }
        final data = args['truncate'] == true
            ? <int>[]
            : List<int>.of(files[path] ?? const []);
        final offset = args['offset'] as int;
        final bytes = args['data'] as Uint8List;
        if (data.length < offset + bytes.length) {
          data.addAll(List.filled(offset + bytes.length - data.length, 0));
        }
        data.setRange(offset, offset + bytes.length, bytes);
        files[path!] = data;
        return null;
      case 'rename':
        files[args['to'] as String] = files.remove(path)!;
        return null;
      case 'delete':
        files.remove(path);
        return null;
      default:
        return null;
    }
  }
}

Uint8List _write(
  int id,
  String share,
  String path,
  int offset,
  List<int> data,
) {
  final header = utf8.encode(
    jsonEncode({
      'op': 'write',
      'share': share,
      'path': path,
      'offset': offset,
      'truncate': offset == 0,
    }),
  );
  final out = BytesBuilder()
    ..addByte(2)
    ..add((ByteData(4)..setUint32(0, id, Endian.little)).buffer.asUint8List())
    ..add(
      (ByteData(
        2,
      )..setUint16(0, header.length, Endian.little)).buffer.asUint8List(),
    )
    ..add(header)
    ..add(data);
  return out.toBytes();
}

Future<void> _settle() async {
  for (var i = 0; i < 12; i++) {
    await Future<void>.delayed(Duration.zero);
  }
}

class _Harness {
  late RemoteSession session;
  final drives = <DriveSession>[];
  final driveSockets = <DriveSocket>[];
  DriveSession get drive => drives.last;
  DriveSocket get driveSocket => driveSockets.last;
}

Uint8List _video(int pts, {bool key = false}) {
  final bytes = Uint8List(17);
  bytes[0] = 1;
  bytes[1] = key ? 1 : 0;
  for (var i = 0; i < 8; i++) {
    bytes[4 + i] = (pts >> (8 * i)) & 255;
  }
  bytes.setAll(12, [0, 0, 0, 1, 0x65]);
  return bytes;
}

/// A connected remote view on a phone held upright. [granted]: the broker
/// granted drive-v1 (the assignment allows shared folders).
Future<_Harness> _open(
  WidgetTester tester,
  AndroidDriveFileSystem files, {
  bool granted = true,
  String? unavailable,
}) async {
  tester.view.physicalSize = const Size(412, 915);
  tester.view.devicePixelRatio = 1;
  addTearDown(tester.view.reset);
  AndroidNativeFake().install();
  addTearDown(AndroidNativeFake.uninstall);
  final h = _Harness();
  final sockets = <SessionSocket>[];
  await tester.pumpWidget(
    MaterialApp(
      theme: buildAppTheme(brightness: Brightness.dark),
      home: AndroidRemoteView(
        name: 'VM173',
        token: 'not-persisted',
        options: const StreamOptions(),
        settings: const AndroidSessionSettings(helpSeen: true),
        decoderLimits: () async => null,
        density: () async => null,
        driveUnavailable: unavailable,
        driveFiles: files,
        load: (options, _, scale) async => BrokerConnection(
          sessionId: sessionId,
          webSocketUrl: Uri.parse(_sessionUrl),
          options: options,
          driveWebSocketUrl: granted ? Uri.parse('$_sessionUrl/drive') : null,
          driveFs: granted,
        ),
        sessionFactory: (connection, media, statsSink) =>
            h.session = RemoteSession(
              connection: connection,
              token: 'not-persisted',
              media: media,
              statsSink: statsSink,
              dialer: (_, _) async {
                final socket = SessionSocket();
                sockets.add(socket);
                return socket;
              },
              driveFactory: (connection, token) {
                final drive = DriveSession(
                  url: connection.driveWebSocketUrl!,
                  token: token,
                  sessionId: connection.sessionId,
                  files: files,
                  fs: connection.driveFs,
                  dialer: (_, _) async {
                    final socket = DriveSocket();
                    h.driveSockets.add(socket);
                    return socket;
                  },
                  folderCheckInterval: const Duration(milliseconds: 200),
                );
                h.drives.add(drive);
                return drive;
              },
            ),
        pictureBuilder: (_) => const ColoredBox(color: Color(0xFF2B4C6F)),
      ),
    ),
  );
  for (var i = 0; i < 5; i++) {
    await tester.pump(const Duration(milliseconds: 20));
  }
  sockets.last.incoming.add(
    jsonEncode({
      'type': 'stream.info',
      'codec': 'h264',
      'width': 1280,
      'height': 720,
      'sourceWidth': 1280,
      'sourceHeight': 720,
    }),
  );
  await tester.pump(const Duration(milliseconds: 20));
  sockets.last.incoming.add(_video(16667, key: true));
  for (var i = 0; i < 6; i++) {
    await tester.pump(const Duration(milliseconds: 40));
  }
  expect(find.byKey(const Key('android-remote-surface')), findsOneWidget);
  if (granted) {
    expect(h.drives, hasLength(1));
    h.driveSocket.emit(ready);
    await tester.pump(const Duration(milliseconds: 40));
    expect(h.drive.ready, isTrue);
  }
  return h;
}

Future<void> _close(WidgetTester tester) async {
  await tester.pumpWidget(const SizedBox.shrink());
  await tester.pump(const Duration(seconds: 3));
}

final _card = find.byKey(const Key('session-command-bar'));

Future<void> _openMenu(WidgetTester tester) async {
  if (_card.evaluate().isNotEmpty) return;
  await tester.tap(find.byKey(const Key('expand-session-toolbar')));
  await tester.pumpAndSettle();
  expect(_card, findsOneWidget);
}

List<AndroidBarMenuRow> _rows(WidgetTester tester) => tester
    .widgetList<AndroidBarMenuRow>(
      find.descendant(of: _card, matching: find.byType(AndroidBarMenuRow)),
    )
    .toList();

void main() {
  TestWidgetsFlutterBinding.ensureInitialized();
  const channel = MethodChannel('multitap/android/drive-test');
  late _Engine engine;
  late AndroidDriveFileSystem files;

  setUpAll(() async {
    final font = FontLoader('Pretendard');
    for (final weight in ['Regular', 'Medium', 'SemiBold', 'Bold']) {
      font.addFont(rootBundle.load('assets/fonts/Pretendard-$weight.otf'));
    }
    await font.load();
    await (FontLoader(
      'MaterialIcons',
    )..addFont(rootBundle.load('fonts/MaterialIcons-Regular.otf'))).load();
  });
  setUp(() {
    engine = _Engine(channel);
    files = AndroidDriveFileSystem(channel: channel);
    final messenger =
        TestDefaultBinaryMessengerBinding.instance.defaultBinaryMessenger;
    messenger.setMockMethodCallHandler(_pen, (_) async => null);
    messenger.setMockMethodCallHandler(
      AndroidSessionKeepalive.channel,
      (call) async => call.method == 'start' ? 'started' : true,
    );
  });
  tearDown(() {
    engine.dispose();
    final messenger =
        TestDefaultBinaryMessengerBinding.instance.defaultBinaryMessenger;
    messenger.setMockMethodCallHandler(_pen, null);
    messenger.setMockMethodCallHandler(AndroidSessionKeepalive.channel, null);
  });

  // ------------------------------------------------------------- the engine

  test('The picker answers a tree URI with where it is in words; a cancelled '
      'pick is nothing; a device without a picker says so', () async {
    final pick = await files.pickFolder();
    expect(pick!.path, _tree);
    expect(pick.name, 'Documents');
    expect(files.where(_tree), '내부 저장공간/Documents');
    expect(files.whole(_tree), isFalse);

    engine.pick = null;
    expect(await files.pickFolder(), isNull);

    engine.pick = {
      'path': 'content://x/tree/sd%3A',
      'name': 'SD',
      'whole': true,
    };
    final card = await files.pickFolder();
    expect(files.whole(card!.path), isTrue);
    expect(files.where(card.path), isNull);

    engine.pickError = 'drive_unavailable';
    await expectLater(
      files.pickFolder(),
      throwsA(
        isA<DriveFileException>().having(
          (e) => e.code,
          'code',
          'drive_unavailable',
        ),
      ),
    );
    // An engine that does not say why (1.0.31-1.0.32): not "this device has
    // no picker", which it cannot know.
    expect(files.lastPickFailure, 0);
    expect(
      androidDriveWording(files).message('drive_unavailable'),
      startsWith('폴더를 고르는 화면을 열지 못했어요'),
    );
    // Why the picker did not open travels with the failure: into the record
    // (the drive record's `error`) and into the words.
    for (final (why, words) in [
      (1, '‘파일’ 앱이 꺼져 있어서'),
      (2, '이 기기에는 폴더를 고르는 화면이 없어서'),
      (3, '폴더를 고르는 화면을 열지 못했어요'),
      (4, '폴더를 고르는 화면을 열지 못했어요'),
      (5, '폴더를 고르는 화면을 열지 못했어요'),
      (7, '‘파일’ 앱이 꺼져 있어서'),
      (8, '폴더를 고르는 화면을 열지 못했어요'),
    ]) {
      engine.pickWhy = why;
      await expectLater(
        files.pickFolder(),
        throwsA(
          isA<DriveFileException>()
              .having((e) => e.code, 'code', 'drive_unavailable')
              .having((e) => e.win32, 'reason', why),
        ),
      );
      expect(files.lastPickFailure, why);
      expect(
        androidDriveWording(files).message('drive_unavailable'),
        startsWith(words),
      );
      // A picker app that is turned off is something the person can turn
      // on: one button to Android's page of that app, and only then.
      final fix = androidDriveWording(files).fix!('drive_unavailable');
      expect(fix != null, why == 1 || why == 7, reason: '$why');
      expect(androidDriveWording(files).fix!('drive_denied'), isNull);
      if (fix != null) {
        expect(fix.label, '‘파일’ 앱 설정 열기');
        final before = engine.count('pickerSettings');
        fix.run();
        await Future<void>.delayed(Duration.zero);
        expect(engine.count('pickerSettings'), before + 1);
      }
    }
    // Anything else an engine might put there is not a reason (6 is this
    // side's own: no engine).
    for (final Object? odd in ['1', 6, 9, -1, 2.5]) {
      engine.pickWhy = odd;
      await expectLater(
        files.pickFolder(),
        throwsA(isA<DriveFileException>().having((e) => e.win32, 'reason', 0)),
      );
      expect(files.lastPickFailure, 0);
    }
    engine.pickWhy = null;
    // Engine text never crosses: an unknown code is one fixed code.
    engine.pickError = 'java.lang.IllegalStateException';
    await expectLater(
      files.pickFolder(),
      throwsA(
        isA<DriveFileException>().having(
          (e) => e.code,
          'code',
          'drive_io_failed',
        ),
      ),
    );
    // No engine at all (an older APK): the same fixed code as on Windows.
    final none = AndroidDriveFileSystem(
      channel: const MethodChannel('multitap/android/drive-none'),
    );
    await expectLater(
      none.pickFolder(),
      throwsA(
        isA<DriveFileException>().having(
          (e) => e.code,
          'code',
          'drive_unavailable',
        ),
      ),
    );
    expect(none.lastPickFailure, 6);
    expect(
      androidDriveWording(none).message('drive_unavailable'),
      startsWith('이 기기에는 폴더를 고르는 화면이 없어서'),
    );
  });

  test('Wherever all-files access exists, this app\'s own folder list is the '
      'only way and Android\'s picker is never asked; older devices keep the '
      'picker', () async {
    const folder = '/storage/emulated/0/Documents';
    engine.storageSupported = true;
    var asked = 0;
    String? answer = folder;
    files.chooser = (given) async {
      expect(given, same(files));
      asked++;
      return answer;
    };
    final pick = await files.pickFolder();
    expect(pick!.path, folder);
    expect(pick.name, 'Documents');
    expect(files.where(folder), '내부 저장공간/Documents');
    expect(files.whole(folder), isFalse);
    expect(asked, 1);
    expect(engine.count('storagePick'), 1);
    expect(engine.count('pickFolder'), 0);
    // Left without choosing (or the access refused): a cancelled pick,
    // nothing said, and still no picker.
    answer = null;
    expect(await files.pickFolder(), isNull);
    expect(engine.count('storagePick'), 1);
    expect(engine.count('pickFolder'), 0);
    answer = folder;
    // A folder this app may not share after all: its own fixed code.
    engine.storagePickError = 'drive_not_found';
    await expectLater(
      files.pickFolder(),
      throwsA(
        isA<DriveFileException>().having(
          (e) => e.code,
          'code',
          'drive_not_found',
        ),
      ),
    );
    engine.storagePickError = null;
    // Android 10 and older have no such access: Android's picker, as before.
    engine.storageSupported = false;
    final before = asked;
    expect((await files.pickFolder())!.path, _tree);
    expect(asked, before);
    expect(engine.count('pickFolder'), 1);
    // No screen that could show the list (no connection on screen): the same.
    engine.storageSupported = true;
    files.chooser = null;
    expect((await files.pickFolder())!.path, _tree);
    expect(engine.count('pickFolder'), 2);
  });

  testWidgets('This app\'s folder list: the access is asked for first and '
      'the person is sent to Android\'s page; back with it allowed the list '
      'opens, and they share the folder they are in', (tester) async {
    Object? result = 'pending';
    engine.storageSupported = true;
    // To Android's settings page and back, as the engine reports it.
    Future<void> awayAndBack() async {
      for (final state in [
        AppLifecycleState.inactive,
        AppLifecycleState.hidden,
        AppLifecycleState.paused,
        AppLifecycleState.hidden,
        AppLifecycleState.inactive,
        AppLifecycleState.resumed,
      ]) {
        tester.binding.handleAppLifecycleStateChanged(state);
        await tester.pump();
      }
    }
    await tester.pumpWidget(
      MaterialApp(
        theme: buildAppTheme(brightness: Brightness.dark),
        home: Scaffold(
          body: Builder(
            builder: (context) => TextButton(
              key: const Key('go'),
              onPressed: () async {
                result = 'pending';
                result = await showAndroidFolderChooser(context, files);
              },
              child: const Text('go'),
            ),
          ),
        ),
      ),
    );
    // Not allowed yet, and the person says no: nothing happens.
    await tester.tap(find.byKey(const Key('go')));
    await tester.pumpAndSettle();
    expect(find.text('모든 파일 접근을 허용할까요?'), findsOneWidget);
    expect(find.textContaining('데스크톱에는 고른 폴더만 보여요'), findsOneWidget);
    expect(find.textContaining('파일’ 앱'), findsNothing);
    await tester.tap(find.byKey(const Key('android-storage-access-no')));
    await tester.pumpAndSettle();
    expect(result, isNull);
    expect(engine.count('storageSettings'), 0);
    // They agree: Android's page opens; back without allowing, nothing.
    await tester.tap(find.byKey(const Key('go')));
    await tester.pumpAndSettle();
    await tester.tap(find.byKey(const Key('android-storage-access-yes')));
    await tester.pumpAndSettle();
    expect(engine.count('storageSettings'), 1);
    expect(result, 'pending');
    await awayAndBack();
    await tester.pumpAndSettle();
    expect(result, isNull);
    expect(find.byKey(const Key('android-folder-chooser')), findsNothing);
    // They agree and allow it: back in the app the list opens by itself.
    await tester.tap(find.byKey(const Key('go')));
    await tester.pumpAndSettle();
    await tester.tap(find.byKey(const Key('android-storage-access-yes')));
    await tester.pumpAndSettle();
    engine.storageGranted = true;
    await awayAndBack();
    await tester.pumpAndSettle();
    // The one volume opens at once; nothing is above its top.
    expect(find.byKey(const Key('android-folder-chooser')), findsOneWidget);
    expect(find.text('내부 저장공간'), findsOneWidget);
    expect(find.text('DCIM'), findsOneWidget);
    expect(find.text('Documents'), findsOneWidget);
    expect(find.byKey(const Key('android-folder-up')), findsNothing);
    expect(find.textContaining('파일’ 앱'), findsNothing);
    // Into a folder: where it is, the way back up, nothing inside.
    await tester.tap(find.text('Documents'));
    await tester.pumpAndSettle();
    expect(find.text('내부 저장공간/Documents'), findsOneWidget);
    expect(find.byKey(const Key('android-folder-empty')), findsOneWidget);
    await tester.tap(find.byKey(const Key('android-folder-up')));
    await tester.pumpAndSettle();
    expect(find.text('DCIM'), findsOneWidget);
    await tester.tap(find.text('Documents'));
    await tester.pumpAndSettle();
    await tester.tap(find.byKey(const Key('android-folder-share')));
    await tester.pumpAndSettle();
    expect(result, '/storage/emulated/0/Documents');
    // Already allowed: the list at once. Left with 취소: nothing chosen.
    await tester.tap(find.byKey(const Key('go')));
    await tester.pumpAndSettle();
    expect(find.byKey(const Key('android-folder-chooser')), findsOneWidget);
    await tester.tap(find.byKey(const Key('android-folder-cancel')));
    await tester.pumpAndSettle();
    expect(result, isNull);
  });
  test('The words of the panel are this device\'s', () {
    final wording = androidDriveWording(files);
    expect(wording.device, '이 기기');
    expect(wording.path(_tree), '고른 폴더');
    expect(wording.message('drive_denied'), contains('권한'));
    expect(wording.message('drive_not_found'), startsWith('이 기기에서'));
    expect(wording.message('drive_limit'), driveMessage('drive_limit'));
    expect(wording.offline, startsWith('이 기기에서'));
    expect(wording.risk!(_tree), isNull);
    for (final text in [
      wording.offline,
      wording.gone('사진'),
      for (final code in [
        'drive_denied',
        'drive_not_found',
        'drive_unavailable',
      ])
        wording.message(code),
    ]) {
      expect(text.contains('PC'), isFalse, reason: text);
      expect(text.contains('VM'), isFalse, reason: text);
    }
  });

  // -------------------------------------------------------------- the wire

  test('The Windows client\'s drive-v1 exchange over the Android engine: '
      'shares, stat, list, ranged read, write, errors', () async {
    final socket = DriveSocket();
    final drive = DriveSession(
      url: Uri.parse('$_sessionUrl/drive'),
      token: 'not-persisted',
      sessionId: sessionId,
      files: files,
      fs: true,
      dialer: (_, _) async => socket,
      folderCheckInterval: const Duration(hours: 1),
    );
    await drive.start();
    socket.emit(ready);
    await _settle();
    expect(drive.ready, isTrue);
    expect(socket.lastOf('drive.shares')['shares'], isEmpty);

    expect(await drive.addFolder(), isNull);
    final share = drive.shares.single;
    expect(share.path, _tree);
    expect(share.label, 'Documents');
    expect(socket.lastOf('drive.shares')['shares'], [
      {'id': share.id, 'name': 'Documents', 'readOnly': false},
    ]);
    // The same folder twice is refused before anything is opened again.
    expect(await drive.addFolder(), 'drive_exists');
    expect(engine.count('open'), 1);

    socket.emit({
      'type': 'drive.mounted',
      'shares': [
        {'id': share.id, 'state': 'mapped', 'letter': 'Z'},
      ],
    });
    await _settle();
    expect(share.state, DriveShareState.mapped);

    Map<String, Object> request(
      int id,
      String op,
      String path, [
      Map<String, Object> more = const {},
    ]) => {
      'type': 'drive.request',
      'id': id,
      'op': op,
      'share': share.id,
      'path': path,
      ...more,
    };

    socket.emit(request(1, 'stat', ''));
    socket.emit(request(2, 'stat', 'report.txt'));
    socket.emit(request(3, 'stat', 'missing.txt'));
    socket.emit(request(4, 'list', '', {'cursor': 0}));
    await _settle();
    expect(socket.response(1)['entry'], {
      'name': '',
      'dir': true,
      'size': 0,
      'modified': 0,
      'created': 0,
      'readOnly': false,
      'space': {'free': 10, 'total': 100},
    });
    expect(socket.response(2)['entry'], {
      'name': 'report.txt',
      'dir': false,
      'size': 11,
      'modified': 1700000000000,
      'created': 1700000000000,
      'readOnly': false,
    });
    expect(socket.response(3), {
      'type': 'drive.response',
      'id': 3,
      'ok': false,
      'error': 'drive_not_found',
    });
    expect((socket.response(4)['entries'] as List).map((e) => e['name']), [
      'report.txt',
    ]);

    // A ranged read: kind 1, the request id, exactly the asked bytes.
    socket.emit(request(5, 'read', 'report.txt', {'offset': 6, 'length': 3}));
    await _settle();
    final frame = socket.binary.single;
    expect(frame[0], 1);
    expect(ByteData.sublistView(frame).getUint32(1, Endian.little), 5);
    expect(utf8.decode(frame.sublist(5)), 'wor');

    // Writes in pieces, as the Host sends them.
    socket.incoming.add(_write(6, share.id, 'new.bin', 0, [1, 2, 3, 4]));
    socket.incoming.add(_write(7, share.id, 'new.bin', 4, [5, 6]));
    await _settle();
    expect(socket.response(6)['ok'], isTrue);
    expect(socket.response(7)['ok'], isTrue);
    expect(engine.files['new.bin'], [1, 2, 3, 4, 5, 6]);

    socket.emit(request(8, 'setsize', 'new.bin', {'size': 2}));
    socket.emit(request(9, 'flush', 'new.bin'));
    socket.emit(
      request(10, 'rename', 'new.bin', {'to': 'old.bin', 'replace': false}),
    );
    await _settle();
    for (final id in [8, 9, 10]) {
      expect(socket.response(id)['ok'], isTrue, reason: '$id');
    }
    expect(engine.count('setSize'), 1);
    expect(engine.count('flush'), 1);
    expect(engine.files.containsKey('old.bin'), isTrue);

    // What the engine refuses reaches the desktop as its fixed code; a code
    // the wire does not know never does.
    engine.failWith = 'drive_no_space';
    socket.incoming.add(_write(11, share.id, 'full.bin', 0, [1]));
    await _settle();
    expect(socket.response(11)['error'], 'drive_no_space');
    engine.failWith = 'drive_unsupported';
    socket.emit(request(12, 'stat', 'report.txt'));
    await _settle();
    expect(socket.response(12)['error'], 'drive_io_failed');
    engine.failWith = null;

    // 읽기 전용 is enforced by the engine before the relay hears of it.
    await drive.setReadOnly(share.id, true);
    expect(engine.open[share.id], isTrue);
    expect(socket.lastOf('drive.shares')['shares'], [
      {'id': share.id, 'name': 'Documents', 'readOnly': true},
    ]);

    await drive.close();
    expect(engine.open, isEmpty);
    expect(socket.wasClosed, isTrue);
  });

  test('A folder whose access was taken back, or that is gone, ends its share '
      'and says which', () async {
    for (final revoked in [true, false]) {
      final socket = DriveSocket();
      final drive = DriveSession(
        url: Uri.parse('$_sessionUrl/drive'),
        token: 'not-persisted',
        sessionId: sessionId,
        files: files,
        dialer: (_, _) async => socket,
        folderCheckInterval: const Duration(hours: 1),
      );
      await drive.start();
      socket.emit(ready);
      await _settle();
      expect(await drive.addFolder(), isNull);
      final id = drive.shares.single.id;
      engine.revoked = revoked;
      engine.gone = !revoked;
      await drive.checkFolders();
      expect(drive.shares, isEmpty);
      expect(drive.lastGone, 'Documents');
      expect(drive.goneCount, 1);
      // The desktop's drive goes at once, and the engine lets the folder go.
      expect(socket.lastOf('drive.shares')['shares'], isEmpty);
      expect(engine.open.containsKey(id), isFalse);
      expect(
        androidDriveWording(files).gone(drive.lastGone!),
        revoked
            ? '‘Documents’ 폴더를 열 권한이 없어져서 공유를 끝냈어요'
            : '‘Documents’ 폴더를 찾을 수 없어서 공유를 끝냈어요',
      );
      engine.revoked = engine.gone = false;
      await drive.close();
    }
  });

  test('After a stream resume the folder is opened again by the engine under '
      'the same id and offered as before (channel-resume-v1)', () async {
    DriveSession session(DriveSocket socket) => DriveSession(
      url: Uri.parse('$_sessionUrl/drive'),
      token: 'not-persisted',
      sessionId: sessionId,
      files: files,
      dialer: (_, _) async => socket,
      folderCheckInterval: const Duration(hours: 1),
    );
    final first = DriveSocket();
    final before = session(first);
    await before.start();
    first.emit(ready);
    await _settle();
    expect(await before.addFolder(), isNull);
    final id = before.shares.single.id;
    // The channel is lost with the stream; the engine lets the folder go.
    await before.close(reason: 'drive_transport_closed');
    await _settle();
    expect(engine.open, isEmpty);
    expect(before.sharesAtEnd.single.id, id);

    final second = DriveSocket();
    final after = session(second)..inherit(before);
    await after.start();
    second.emit(ready);
    await _settle();
    // No picker: the same tree, the same id, the same name.
    expect(engine.count('pickFolder'), 1);
    expect(engine.count('open'), 2);
    expect(engine.open.keys, [id]);
    expect(after.shares.single.path, _tree);
    expect(second.lastOf('drive.shares')['shares'], [
      {'id': id, 'name': 'Documents', 'readOnly': false},
    ]);
    // A folder whose access is gone meanwhile is not offered again.
    await after.close(reason: 'drive_transport_closed');
    await _settle();
    engine.failWith = 'drive_denied';
    final third = DriveSocket();
    final denied = session(third)..inherit(after);
    await denied.start();
    third.emit(ready);
    await _settle();
    expect(third.lastOf('drive.shares')['shares'], isEmpty);
    engine.failWith = null;
    await denied.close();
  });

  // -------------------------------------------------------------- the menu

  testWidgets('공유 폴더 is a row of the menu between 화면 and 설정, named and '
      'drawn as on the PC, only where this connection has it', (tester) async {
    for (final granted in [true, false]) {
      await _open(tester, files, granted: granted);
      try {
        await _openMenu(tester);
        final rows = _rows(tester);
        expect(rows.map((row) => row.item.label).toList(), [
          '연결 품질',
          '화면',
          if (granted) '공유 폴더',
          '설정',
          '연결 끊기',
        ], reason: 'granted: $granted');
        // No caption over a row that is its group.
        expect(
          find.descendant(of: _card, matching: find.byType(AndroidMenuCaption)),
          findsNothing,
        );
        if (!granted) continue;
        final row = rows[2];
        expect(row.item.key, const Key('android-session-share'));
        expect(row.item.icon, Icons.folder_shared_rounded);
        expect(row.opens, isTrue);
        expect(row.description, '이 기기의 폴더를 데스크톱에서 쓰기');
        expect(
          tester.getSize(find.byKey(const Key('android-session-share'))).height,
          greaterThanOrEqualTo(48),
        );
      } finally {
        await _close(tester);
      }
    }
  });

  testWidgets(
    'A desktop that cannot take shared folders keeps the row, greyed, '
    'with the reason',
    (tester) async {
      await _open(
        tester,
        files,
        granted: false,
        unavailable: 'drive_webclient_unavailable',
      );
      try {
        await _openMenu(tester);
        final row = _rows(tester)[2];
        expect(row.item.label, '공유 폴더');
        expect(row.item.icon, Icons.folder_off_rounded);
        expect(row.enabled, isFalse);
        expect(row.description, driveMessage('drive_webclient_unavailable'));
        await tester.tap(find.byKey(const Key('android-session-share')));
        await tester.pumpAndSettle();
        expect(find.byKey(const Key('android-share-panel')), findsNothing);
      } finally {
        await _close(tester);
      }
    },
  );

  testWidgets('The row opens the PC\'s 공유 폴더 panel in the card with this '
      'device\'s words; 폴더 추가… picks with Android\'s picker and the folder '
      'is shared; every control is a 48 dp target', (tester) async {
    final h = await _open(tester, files);
    try {
      await _openMenu(tester);
      await tester.tap(find.byKey(const Key('android-session-share')));
      await tester.pumpAndSettle();
      final panel = find.byKey(const Key('android-share-panel'));
      expect(panel, findsOneWidget);
      expect(tester.widget<AndroidToolbarPanel>(panel).title, '공유 폴더');
      final shared = tester.widget<SessionSharePanel>(
        find.descendant(of: panel, matching: find.byType(SessionSharePanel)),
      );
      expect(shared.embedded, isTrue);
      expect(shared.driveWording.device, '이 기기');
      // The card has the title: the PC panel's own header is left out.
      expect(find.text('PC 폴더 공유'), findsNothing);
      expect(find.text('이 기기 폴더 공유'), findsNothing);
      expect(find.byKey(const Key('drive-empty')), findsOneWidget);
      expect(find.byKey(const Key('drive-add-consequence')), findsOneWidget);
      expect(find.textContaining('PC'), findsNothing);

      final add = find.byKey(const Key('drive-add'));
      expect(tester.getSize(add).height, greaterThanOrEqualTo(48));
      expect(engine.count('pickFolder'), 0);
      await tester.tap(add);
      await pumpWork(tester);
      expect(engine.count('pickFolder'), 1);
      expect(engine.count('open'), 1);
      // An ordinary folder is shared as picked, without another question.
      expect(find.byKey(const Key('drive-add-confirm')), findsNothing);
      final share = h.drive.shares.single;
      expect(h.driveSocket.lastOf('drive.shares')['shares'], [
        {'id': share.id, 'name': 'Documents', 'readOnly': false},
      ]);
      expect(find.byKey(ValueKey('drive-share-${share.id}')), findsOneWidget);
      // Where it is, in words; never the document URI.
      expect(find.text('내부 저장공간/Documents'), findsOneWidget);
      expect(find.textContaining('content://'), findsNothing);

      h.driveSocket.emit({
        'type': 'drive.mounted',
        'shares': [
          {'id': share.id, 'state': 'mapped', 'letter': 'Z'},
        ],
      });
      await tester.pumpAndSettle();
      expect(find.text('Z: 연결됨'), findsOneWidget);
      for (final key in [
        ValueKey('drive-remove-${share.id}'),
        const Key('drive-add'),
      ]) {
        final size = tester.getSize(find.byKey(key));
        expect(size.height, greaterThanOrEqualTo(48), reason: '$key');
        expect(size.width, greaterThanOrEqualTo(48), reason: '$key');
      }

      // The menu row now says what is shared.
      await tester.tap(find.byKey(const Key('session-menu-back')));
      await tester.pumpAndSettle();
      final row = _rows(tester)[2];
      expect(row.description, '공유 폴더 1개 연결됨');

      // Turning the device: the share is the session's, not the screen's.
      tester.view.physicalSize = const Size(915, 412);
      await tester.pumpAndSettle();
      expect(h.drive.shares.single.id, share.id);
      expect(h.drive.shares.single.state, DriveShareState.mapped);
      expect(engine.count('open'), 1);
      expect(engine.count('close'), 0);
      expect(tester.takeException(), isNull);
    } finally {
      await _close(tester);
    }
  });

  testWidgets('A whole storage volume asks first, in this device\'s words; '
      'ending a share says the files stay on this device', (tester) async {
    engine.pick = {
      'path': _tree,
      'name': 'SD',
      'where': 'SD 카드·USB 저장장치',
      'whole': true,
    };
    final h = await _open(tester, files);
    try {
      await _openMenu(tester);
      await tester.tap(find.byKey(const Key('android-session-share')));
      await tester.pumpAndSettle();
      await tester.tap(find.byKey(const Key('drive-add')));
      await tester.pumpAndSettle();
      final confirm = find.byKey(const Key('drive-add-confirm'));
      expect(confirm, findsOneWidget);
      expect(
        find.descendant(of: confirm, matching: find.textContaining('저장공간 전체')),
        findsOneWidget,
      );
      expect(
        find.descendant(
          of: confirm,
          matching: find.textContaining('폴더: SD 카드·USB 저장장치'),
        ),
        findsOneWidget,
      );
      // Nothing is opened or offered before the person agrees.
      expect(engine.count('open'), 0);
      await tester.tap(find.byKey(const Key('drive-add-confirm-no')));
      await tester.pumpAndSettle();
      expect(h.drive.shares, isEmpty);
      expect(engine.count('open'), 0);

      await tester.tap(find.byKey(const Key('drive-add')));
      await tester.pumpAndSettle();
      await tester.tap(find.byKey(const Key('drive-add-confirm-yes')));
      await pumpWork(tester);
      final share = h.drive.shares.single;

      await tester.tap(find.byKey(ValueKey('drive-remove-${share.id}')));
      await pumpWork(tester);
      final remove = find.byKey(const Key('drive-remove-confirm'));
      expect(
        find.descendant(
          of: remove,
          matching: find.textContaining('이 기기에 그대로 남아요'),
        ),
        findsOneWidget,
      );
      expect(
        find.descendant(of: remove, matching: find.textContaining('PC')),
        findsNothing,
      );
      await tester.tap(find.byKey(const Key('drive-remove-confirm-yes')));
      await pumpWork(tester);
      expect(h.drive.shares, isEmpty);
      expect(engine.open, isEmpty);
    } finally {
      await _close(tester);
    }
  });

  testWidgets('A share that ends by itself is said once on the picture, with '
      'the true reason; a lost channel too', (tester) async {
    final h = await _open(tester, files);
    try {
      expect(await h.drive.addFolder(), isNull);
      await tester.pump();
      engine.revoked = true;
      // The session's own periodic check of its folders.
      await tester.pump(const Duration(milliseconds: 250));
      await tester.pump(const Duration(milliseconds: 50));
      expect(h.drive.shares, isEmpty);
      expect(find.text('‘Documents’ 폴더를 열 권한이 없어져서 공유를 끝냈어요'), findsOneWidget);
      engine.revoked = false;

      // The channel ends while the stream goes on.
      h.driveSocket.emit({
        'type': 'drive.error',
        'reason': 'drive_policy_changed',
      });
      await tester.pump(const Duration(milliseconds: 50));
      expect(h.drive.closed, isTrue);
      expect(find.text('관리자가 공유 폴더 권한을 해제했어요.'), findsOneWidget);
      expect(tester.takeException(), isNull);
    } finally {
      await _close(tester);
    }
  });

  test('사용안내 lists 공유 폴더 only in a connection that has it', () {
    RemoteSession session(bool granted) => RemoteSession(
      connection: BrokerConnection(
        sessionId: sessionId,
        webSocketUrl: Uri.parse(_sessionUrl),
        driveWebSocketUrl: granted ? Uri.parse('$_sessionUrl/drive') : null,
      ),
      token: 'not-persisted',
      media: SessionMedia(),
      dialer: (_, _) async => SessionSocket(),
    );
    expect(
      SessionGuideContent.forAndroid(session(false)).tips
          .map((tip) => tip.feature),
      [SessionGuideFeature.printer, SessionGuideFeature.clipboard],
    );
    final tips = SessionGuideContent.forAndroid(session(true)).tips;
    expect(tips.map((tip) => tip.feature), [
      SessionGuideFeature.printer,
      SessionGuideFeature.folders,
      SessionGuideFeature.clipboard,
    ]);
    final folders = tips[1];
    expect(folders.title, '공유 폴더');
    expect(folders.message, contains('이 기기'));
    expect(folders.message.contains('PC'), isFalse);
    expect('\n'.allMatches(folders.message).length, lessThanOrEqualTo(2));
  });
}
