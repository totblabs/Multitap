// In-memory DriveFileSystem and socket only. No native plugin, real file,
// network, clipboard or device is used by these lifetime boundaries.
import 'dart:async';
import 'dart:typed_data';

import 'package:flutter_test/flutter_test.dart';
import 'package:multitap_client/data/drive_files.dart';
import 'package:multitap_client/data/drive_memory.dart';
import 'package:multitap_client/data/drive_session.dart';
import 'package:multitap_client/data/product_log.dart';

import 'drive_session_test.dart'
    show DriveSocket, FakeFiles, driveUrl, ready, sessionId, writeFrame;

const _deadline = Duration(milliseconds: 20);

class _BoundaryFiles extends FakeFiles {
  Completer<Uint8List>? heldRead;
  Completer<void>? heldWrite;
  bool failReleasedWrite = false;
  bool throwActionTimeout = false;
  int admittedWrites = 0;

  @override
  Future<Uint8List> read(String share, String path, int offset, int length) =>
      heldRead?.future ?? super.read(share, path, offset, length);

  @override
  Future<void> write(
    String share,
    String path,
    int offset,
    Uint8List data, {
    required bool truncate,
  }) {
    admittedWrites++;
    if (throwActionTimeout) throw TimeoutException('synthetic action failure');
    final held = heldWrite;
    if (held == null) return Future<void>.value();
    return held.future.then<void>((_) {
      if (failReleasedWrite) {
        throw const DriveFileException('drive_io_failed');
      }
    });
  }
}

class _Fixture {
  _Fixture(this.files, this.drive, this.socket, this.log);
  final _BoundaryFiles files;
  final DriveSession drive;
  final DriveSocket socket;
  final ProductLog log;

  Future<void> close(WidgetTester tester) async {
    final write = files.heldWrite;
    if (write != null && !write.isCompleted) write.complete();
    final read = files.heldRead;
    if (read != null && !read.isCompleted) read.complete(Uint8List(0));
    await _pump(tester);
    await drive.close();
    log.dispose();
  }
}

Future<void> _pump(WidgetTester tester) async {
  for (var i = 0; i < 5; ++i) {
    await tester.pump();
  }
}

Future<_Fixture> _start(WidgetTester tester) async {
  final files = _BoundaryFiles();
  final memory = SessionDriveMemory();
  final socket = DriveSocket();
  final log = ProductLog();
  final drive = DriveSession(
    url: driveUrl,
    token: 'synthetic-not-persisted',
    sessionId: sessionId,
    files: files,
    memory: memory,
    server: 'https://fixture.invalid',
    desktopId: 'fixture-vm',
    fs: true,
    requestDeadline: _deadline,
    folderCheckInterval: const Duration(hours: 1),
    dialer: (_, _) async => socket,
    log: log,
  );
  await drive.start();
  socket.emit(ready);
  await _pump(tester);
  // Share roots through the same explicit picker/add path as the current UI.
  expect(drive.shares, isEmpty);
  for (final pick in const [
    DrivePick(r'R:\memory-long', 'Long'),
    DrivePick(r'R:\memory-cycles', 'Cycles'),
  ]) {
    files.pick = pick;
    expect(await drive.addFolder(), isNull);
  }
  files.pick = null;
  expect(drive.shares, hasLength(2));
  return _Fixture(files, drive, socket, log);
}

void _write(_Fixture fixture, int id, int root) {
  fixture.socket.incoming.add(
    writeFrame(
      id,
      {
        'op': 'write',
        'share': fixture.drive.shares[root].id,
        'path': 'memory.bin',
        'offset': 0,
        'truncate': false,
      },
      [65],
    ),
  );
}

void _stat(_Fixture fixture, int id, int root) {
  fixture.socket.emit({
    'type': 'drive.request',
    'id': id,
    'op': 'stat',
    'share': fixture.drive.shares[root].id,
    'path': 'memory.bin',
  });
}

int _terminalReplies(DriveSocket socket, int id) =>
    socket.sent.where((row) => row['id'] == id).length +
    socket.binary
        .where(
          (frame) =>
              frame.length >= 5 &&
              ByteData.sublistView(frame).getUint32(1, Endian.little) == id,
        )
        .length;

void main() {
  TestWidgetsFlutterBinding.ensureInitialized();

  testWidgets(
    'Late binary read stays suppressed after 65 later deadline errors',
    (tester) async {
      final fixture = await _start(tester);
      final files = fixture.files;
      var repliesBeforeCompletion = -1;
      var repliesAfterCompletion = -1;
      try {
        files.heldRead = Completer<Uint8List>();
        fixture.socket.emit({
          'type': 'drive.request',
          'id': 1,
          'op': 'read',
          'share': fixture.drive.shares[0].id,
          'path': 'long.bin',
          'offset': 0,
          'length': 1,
        });
        await _pump(tester);
        await tester.pump(_deadline + const Duration(milliseconds: 1));
        await _pump(tester);
        expect(fixture.socket.response(1)['error'], 'drive_io_failed');
        expect(fixture.drive.active, 1);

        for (var cycle = 0; cycle < 65; ++cycle) {
          final id = 10 + cycle * 2;
          final gate = files.heldWrite = Completer<void>();
          files.failReleasedWrite = true;
          _write(fixture, id, 1);
          await _pump(tester);
          await tester.pump(_deadline + const Duration(milliseconds: 1));
          await _pump(tester);
          expect(fixture.socket.response(id)['error'], 'drive_io_failed');
          gate.complete();
          await _pump(tester);
          expect(fixture.drive.active, 1);
          _stat(fixture, id + 1, 1);
          await _pump(tester);
          expect(fixture.socket.response(id + 1)['ok'], isTrue);
        }

        repliesBeforeCompletion = _terminalReplies(fixture.socket, 1);
        files.heldRead!.complete(Uint8List.fromList([65]));
        await _pump(tester);
        repliesAfterCompletion = _terminalReplies(fixture.socket, 1);
        expect(fixture.drive.active, 0);
      } finally {
        await fixture.close(tester);
      }
      expect(repliesBeforeCompletion, 1);
      expect(
        repliesAfterCompletion,
        1,
        reason: 'A read has one terminal reply across JSON and binary forms',
      );
    },
  );

  testWidgets('Action-thrown TimeoutException releases both lifetimes and '
      'does not consume future admission capacity', (tester) async {
    final fixture = await _start(tester);
    final files = fixture.files;
    try {
      files.throwActionTimeout = true;
      for (var cycle = 0; cycle < 65; ++cycle) {
        final id = 10 + cycle * 2;
        _write(fixture, id, 0);
        await _pump(tester); // Actual error, without advancing the deadline.
        expect(fixture.socket.response(id)['error'], 'drive_io_failed');
        expect(fixture.drive.active, 0);
        _stat(fixture, id + 1, 0);
        await _pump(tester);
        expect(fixture.socket.response(id + 1)['ok'], isTrue);
      }
      files.throwActionTimeout = false;
      files.failReleasedWrite = false;
      final gate = files.heldWrite = Completer<void>();
      final before = files.admittedWrites;
      for (var id = 500; id < 508; ++id) {
        _write(fixture, id, 0);
      }
      await _pump(tester);
      expect(files.admittedWrites - before, 8);
      expect(fixture.drive.active, 8);
      _write(fixture, 599, 0);
      await _pump(tester);
      expect(fixture.socket.response(599)['error'], 'drive_busy');
      gate.complete();
      await _pump(tester);
      expect(fixture.drive.active, 0);
      for (var id = 500; id < 508; ++id) {
        expect(fixture.socket.response(id)['ok'], isTrue);
        expect(_terminalReplies(fixture.socket, id), 1);
      }
    } finally {
      await fixture.close(tester);
    }
  });

  for (final binaryDuplicate in [false, true]) {
    testWidgets('A repeated live id in a '
        '${binaryDuplicate ? 'binary' : 'JSON'} request closes the protocol '
        'without replying for the older pending action', (tester) async {
      final fixture = await _start(tester);
      final files = fixture.files;
      try {
        final gate = files.heldWrite = Completer<void>();
        _write(fixture, 1, 0);
        await _pump(tester);
        expect(files.admittedWrites, 1);
        expect(fixture.drive.active, 1);
        if (binaryDuplicate) {
          _write(fixture, 1, 0);
        } else {
          _stat(fixture, 1, 0);
        }
        await _pump(tester);
        expect(fixture.drive.closed, isTrue);
        expect(fixture.drive.error, 'drive_invalid_message');
        expect(fixture.socket.wasClosed, isTrue);
        expect(files.admittedWrites, 1);
        expect(
          fixture.drive.active,
          1,
          reason: 'Closing the protocol does not complete native work',
        );
        expect(
          _terminalReplies(fixture.socket, 1),
          0,
          reason: 'No busy/error reply may claim the older pending id',
        );
        gate.complete();
        await _pump(tester);
        expect(fixture.drive.active, 0);
        expect(_terminalReplies(fixture.socket, 1), 0);
      } finally {
        await fixture.close(tester);
      }
    });
  }
}
