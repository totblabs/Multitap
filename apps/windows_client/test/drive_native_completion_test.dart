// Source-only regression preparation. Production DriveSession and
// NativeDriveFileSystem run against a private MethodChannel fake; no native
// runner, filesystem, socket, clipboard or device is used by this fixture.
import 'dart:async';

import 'package:flutter/services.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:multitap_client/data/drive_files.dart';
import 'package:multitap_client/data/drive_memory.dart';
import 'package:multitap_client/data/drive_session.dart';
import 'package:multitap_client/data/product_log.dart';

import 'drive_session_test.dart'
    show DriveSocket, driveUrl, ready, sessionId, writeFrame;

const _channel = MethodChannel('multitap/private-drive-completion-fixture');
const _deadline = Duration(milliseconds: 20);

class _PendingWrite {
  _PendingWrite(this.key, this.bytes);
  final String key;
  final Uint8List bytes;
  final done = Completer<void>();
  bool failAfterChangingMemory = false;
}

class _NativeMemory {
  final roots = <String, String>{};
  final bytes = <String, Uint8List>{};
  final pending = <_PendingWrite>[];
  final calls = <String>[];
  DrivePick? pick;
  bool holdNewWrites = true;
  int generation = 0;

  Future<Object?> call(MethodCall call) async {
    final args = (call.arguments! as Map).cast<String, Object?>();
    calls.add(call.method);
    if (call.method == 'pickFolder') {
      final selected = pick;
      pick = null;
      return selected == null
          ? null
          : {'path': selected.path, 'name': selected.name};
    }
    final share = args['share']! as String;
    switch (call.method) {
      case 'open':
        roots[share] = args['path']! as String;
        return {'name': 'memory-root', 'generation': ++generation};
      case 'close':
        roots.remove(share);
        return null;
      case 'status':
        return {
          'deleted': false,
          'offline': false,
          'path': roots[share] ?? '',
          'name': 'memory-root',
        };
      case 'stat':
        return {
          'name': 'memory.bin',
          'dir': false,
          'size': 1,
          'modified': 0,
          'created': 0,
          'readOnly': false,
        };
      case 'write':
        // Snapshot at admission: closing the share cannot undo already
        // running native work. Completion is separate from its observer.
        final root = roots[share];
        if (root == null) throw PlatformException(code: 'drive_share_unknown');
        final key = '$root/${args['path']}';
        final payload = Uint8List.fromList(args['data']! as Uint8List);
        if (holdNewWrites) {
          final write = _PendingWrite(key, payload);
          pending.add(write);
          await write.done.future;
          bytes[key] = write.bytes;
          if (write.failAfterChangingMemory) {
            throw PlatformException(code: 'drive_io_failed');
          }
        } else {
          bytes[key] = payload;
        }
        return null;
      default:
        throw StateError('Unexpected private fixture method');
    }
  }

  void completeAll({bool failAfterChangingMemory = false}) {
    for (final write in pending) {
      if (write.done.isCompleted) continue;
      write.failAfterChangingMemory = failAfterChangingMemory;
      write.done.complete();
    }
  }
}

class _SessionFixture {
  _SessionFixture(this.drive, this.socket, this.log);
  final DriveSession drive;
  final DriveSocket socket;
  final ProductLog log;
}

Future<void> _pump(WidgetTester tester) async {
  for (var i = 0; i < 5; ++i) {
    await tester.pump();
  }
}

Future<_SessionFixture> _start(
  WidgetTester tester,
  _NativeMemory native, {
  int roots = 1,
  String prefix = 'old',
}) async {
  final memory = SessionDriveMemory(),
      socket = DriveSocket(),
      log = ProductLog();
  await memory.save('https://fixture.invalid', 'fixture-vm', [
    for (var i = 0; i < roots; ++i)
      DriveFolder(path: 'R:\\memory-$prefix-$i', label: 'Memory$i'),
  ]);
  final drive = DriveSession(
    url: driveUrl,
    token: 'synthetic-not-persisted',
    sessionId: sessionId,
    files: NativeDriveFileSystem(channel: _channel),
    memory: memory,
    server: 'https://fixture.invalid',
    desktopId: 'fixture-vm',
    fs: true,
    requestDeadline: _deadline,
    folderCheckInterval: const Duration(hours: 1),
    dialer: (_, _) async => socket,
    log: log,
  );
  addTearDown(() {
    native.completeAll();
    // The body settles native ownership under its fake clock. Cleanup must
    // not await that zone's completed Future after testWidgets has left it.
    if (!drive.closed) unawaited(drive.close());
    log.dispose();
  });
  await drive.start();
  socket.emit(ready);
  await _pump(tester);
  expect(drive.ready, isTrue);
  expect(
    drive.shares,
    isEmpty,
    reason: 'Legacy folder memory never authorizes sharing in a new session',
  );
  for (var i = 0; i < roots; ++i) {
    native.pick = DrivePick('R:\\memory-$prefix-$i', 'Memory$i');
    // Setup's asynchronous MethodChannel mock must finish outside FakeAsync.
    expect(
      await tester.runAsync(() => drive.addFolder(confirm: (_) async => true)),
      isNull,
    );
    expect(drive.shares, hasLength(i + 1));
  }
  return _SessionFixture(drive, socket, log);
}

void _write(_SessionFixture fixture, int id, int index, int value) {
  fixture.socket.incoming.add(
    writeFrame(
      id,
      {
        'op': 'write',
        'share': fixture.drive.shares[index].id,
        'path': 'memory.bin',
        'offset': 0,
        'truncate': false,
      },
      [value],
    ),
  );
}

void main() {
  TestWidgetsFlutterBinding.ensureInitialized();
  late _NativeMemory native;
  setUp(() {
    native = _NativeMemory();
    TestDefaultBinaryMessengerBinding.instance.defaultBinaryMessenger
        .setMockMethodCallHandler(_channel, native.call);
  });
  tearDown(() {
    native.completeAll();
    TestDefaultBinaryMessengerBinding.instance.defaultBinaryMessenger
        .setMockMethodCallHandler(_channel, null);
  });

  for (final lateFailure in [false, true]) {
    testWidgets(
      'Timeout retains actual native write ownership until late '
      '${lateFailure ? 'partial failure' : 'success'}, without a second reply',
      (tester) async {
        final fixture = await _start(tester, native);
        _write(fixture, 1, 0, 65);
        await _pump(tester);
        expect(native.pending, hasLength(1));
        await tester.pump(_deadline + const Duration(milliseconds: 1));
        await _pump(tester);
        final ownedAfterTimeout = fixture.drive.active;
        final failureReply = fixture.socket.response(1);
        fixture.socket.emit({
          'type': 'drive.request',
          'id': 2,
          'op': 'stat',
          'share': fixture.drive.shares.single.id,
          'path': 'other.bin',
        });
        await _pump(tester);
        final independentReadWorked = fixture.socket.response(2)['ok'] == true;
        native.completeAll(failAfterChangingMemory: lateFailure);
        await _pump(tester);
        native.holdNewWrites = false;
        _write(fixture, 3, 0, 66);
        await _pump(tester);
        // Cancel the periodic folder timer before testWidgets checks its
        // invariants; addTearDown runs after that invariant check.
        await fixture.drive.close();
        expect(
          ownedAfterTimeout,
          1,
          reason: 'A timeout retires a waiter, not the actual native operation',
        );
        expect(failureReply['ok'], isFalse);
        expect(independentReadWorked, isTrue);
        expect(fixture.drive.active, 0);
        expect(
          fixture.socket.sent.where((row) => row['id'] == 1),
          hasLength(1),
        );
        expect(fixture.socket.response(3)['ok'], isTrue);
        expect(native.bytes.values.single, [66]);
      },
      timeout: const Timeout(Duration(seconds: 30)),
    );
  }

  testWidgets('Eight timed-out native writes keep the real admission bound '
      'until an operation actually finishes', (tester) async {
    final fixture = await _start(tester, native, roots: 8);
    for (var i = 0; i < 8; ++i) {
      _write(fixture, i + 1, i, 65);
    }
    await _pump(tester);
    expect(native.pending, hasLength(8));
    await tester.pump(_deadline + const Duration(milliseconds: 1));
    await _pump(tester);
    final ownedAfterTimeout = fixture.drive.active;
    _write(fixture, 99, 0, 66);
    await _pump(tester);
    final admittedAfterTimeout = native.pending.length;
    final refusal = fixture.socket.sent
        .where((row) => row['id'] == 99)
        .toList();
    native.completeAll();
    await _pump(tester);
    native.holdNewWrites = false;
    _write(fixture, 100, 0, 67);
    await _pump(tester);
    await fixture.drive.close();
    expect(ownedAfterTimeout, 8);
    expect(
      admittedAfterTimeout,
      8,
      reason: 'Timed-out observers must not let native jobs accumulate',
    );
    expect(refusal.single['error'], 'drive_busy');
    expect(fixture.drive.active, 0);
    expect(fixture.socket.response(100)['ok'], isTrue);
  }, timeout: const Timeout(Duration(seconds: 30)));

  testWidgets('Closing the old session keeps its native owner until completion '
      'while a different new-session root remains usable', (tester) async {
    final old = await _start(tester, native);
    _write(old, 1, 0, 65);
    await _pump(tester);
    await tester.pump(_deadline + const Duration(milliseconds: 1));
    await _pump(tester);
    await old.drive.close(reason: 'authorization_revoked');
    final ownedAfterClose = old.drive.active;
    final oldReplyCount = old.socket.sent.length;
    native.holdNewWrites = false;
    final fresh = await _start(tester, native, prefix: 'different');
    _write(fresh, 2, 0, 67);
    await _pump(tester);
    native.completeAll();
    await _pump(tester);
    await fresh.drive.close();
    expect(ownedAfterClose, 1);
    expect(old.drive.active, 0);
    expect(old.socket.sent, hasLength(oldReplyCount));
    expect(fresh.socket.response(2)['ok'], isTrue);
  }, timeout: const Timeout(Duration(seconds: 30)));
}
