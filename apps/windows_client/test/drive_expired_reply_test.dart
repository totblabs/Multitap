// Private MethodChannel/memory-only trace. No native runner, filesystem,
// network, clipboard or device is used. Keep operation #1 held while later
// timed-out actions fail and actually complete, then finish #1 successfully.
import 'dart:async';

import 'package:flutter/foundation.dart' show debugPrintSynchronously;
import 'package:flutter/services.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:multitap_client/data/drive_files.dart';
import 'package:multitap_client/data/drive_memory.dart';
import 'package:multitap_client/data/drive_session.dart';
import 'package:multitap_client/data/product_log.dart';

import 'drive_session_test.dart'
    show DriveSocket, driveUrl, ready, sessionId, writeFrame;

const _channel = MethodChannel('multitap/private-drive-expired-reply-fixture');
const _deadline = Duration(milliseconds: 20);
const _cycles = 65;

class _HeldWrite {
  _HeldWrite(this.key, this.bytes);
  final String key;
  final Uint8List bytes;
  final done = Completer<void>();
  bool failAfterChangingMemory = false;
}

class _SelectiveNativeMemory {
  final roots = <String, String>{};
  final bytes = <String, Uint8List>{};
  final writes = <int, _HeldWrite>{};
  int writeCount = 0;
  int completedErrors = 0;
  DrivePick? pick;

  Future<Object?> call(MethodCall call) async {
    if (call.method == 'pickFolder') {
      final selected = pick;
      pick = null;
      return selected == null
          ? null
          : {'path': selected.path, 'name': selected.name};
    }
    final args = (call.arguments! as Map).cast<String, Object?>();
    final share = args['share']! as String;
    switch (call.method) {
      case 'open':
        roots[share] = args['path']! as String;
        return {'name': 'memory-root'};
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
        final root = roots[share];
        if (root == null) throw PlatformException(code: 'drive_share_unknown');
        final write = _HeldWrite(
          '$root/${args['path']}',
          Uint8List.fromList(args['data']! as Uint8List),
        );
        writes[++writeCount] = write;
        await write.done.future;
        bytes[write.key] = write.bytes;
        if (write.failAfterChangingMemory) {
          completedErrors++;
          throw PlatformException(code: 'drive_io_failed');
        }
        return null;
      default:
        throw StateError('Unexpected private fixture method');
    }
  }

  void complete(int ordinal, {required bool fail}) {
    final write = writes[ordinal];
    if (write == null || write.done.isCompleted) {
      throw StateError('Private fixture completion must be selective and new');
    }
    write.failAfterChangingMemory = fail;
    write.done.complete();
  }

  void cleanupIfPending(int ordinal) {
    final write = writes[ordinal];
    if (write != null && !write.done.isCompleted) {
      complete(ordinal, fail: true);
    }
  }
}

Future<void> _pump(WidgetTester tester) async {
  for (var i = 0; i < 5; ++i) {
    await tester.pump();
  }
}

void _write(
  DriveSocket socket,
  DriveSession drive,
  int id,
  int root,
  int value,
) {
  socket.incoming.add(
    writeFrame(
      id,
      {
        'op': 'write',
        'share': drive.shares[root].id,
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

  testWidgets(
    'A long timed-out native write replies once after more than 64 later '
    'timeouts finish with errors',
    (tester) async {
      final native = _SelectiveNativeMemory();
      final messenger =
          TestDefaultBinaryMessengerBinding.instance.defaultBinaryMessenger;
      messenger.setMockMethodCallHandler(_channel, native.call);
      final memory = SessionDriveMemory();
      final socket = DriveSocket();
      final log = ProductLog();
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
      var currentOrdinal = 0;
      var longRepliesBeforeCompletion = -1;
      var longRepliesAfterCompletion = -1;
      var activeAfterCompletion = -1;
      var firstReplyIsTimeout = false;
      var lateReplyIsSuccess = false;
      try {
        await drive.start();
        socket.emit(ready);
        await _pump(tester);
        // Every connection starts empty; only this session's actual picker admits roots.
        expect(drive.shares, isEmpty);
        for (final pick in const [
          DrivePick(r'R:\memory-long', 'Long'),
          DrivePick(r'R:\memory-cycles', 'Cycles'),
        ]) {
          native.pick = pick;
          expect(await drive.addFolder(), isNull);
        }
        expect(drive.shares, hasLength(2));
        _write(socket, drive, 1, 0, 65);
        await _pump(tester);
        expect(native.writeCount, 1);
        await tester.pump(_deadline + const Duration(milliseconds: 1));
        await _pump(tester);
        expect(socket.response(1)['error'], 'drive_io_failed');
        expect(drive.active, 1);

        for (var cycle = 0; cycle < _cycles; ++cycle) {
          final id = 10 + cycle * 2;
          _write(socket, drive, id, 1, cycle + 1);
          currentOrdinal = cycle + 2;
          await _pump(tester);
          expect(native.writeCount, currentOrdinal);
          await tester.pump(_deadline + const Duration(milliseconds: 1));
          await _pump(tester);
          expect(socket.response(id)['error'], 'drive_io_failed');
          expect(drive.active, 2);

          // Finish only this later native operation, never long operation #1.
          native.complete(currentOrdinal, fail: true);
          await _pump(tester);
          expect(drive.active, 1);
          socket.emit({
            'type': 'drive.request',
            'id': id + 1,
            'op': 'stat',
            'share': drive.shares[1].id,
            'path': 'memory.bin',
          });
          await _pump(tester);
          expect(socket.response(id + 1)['ok'], isTrue);
          expect(drive.shares[1].offline, isFalse);
        }

        longRepliesBeforeCompletion = socket.sent
            .where((row) => row['id'] == 1)
            .length;
        native.complete(1, fail: false);
        await _pump(tester);
        final replies = socket.sent.where((row) => row['id'] == 1).toList();
        longRepliesAfterCompletion = replies.length;
        firstReplyIsTimeout = replies.first['error'] == 'drive_io_failed';
        lateReplyIsSuccess = replies.last['ok'] == true;
        activeAfterCompletion = drive.active;
      } finally {
        // Only known in-flight ordinals are completed during failure cleanup.
        // Close timers before testWidgets performs its invariant checks.
        native.cleanupIfPending(currentOrdinal);
        native.cleanupIfPending(1);
        await _pump(tester);
        await drive.close();
        log.dispose();
        messenger.setMockMethodCallHandler(_channel, null);
      }

      debugPrintSynchronously(
        '{"audit":"expired-id-eviction","laterCompletedErrors":'
        '${native.completedErrors},"longRepliesBeforeCompletion":'
        '$longRepliesBeforeCompletion,"longRepliesAfterCompletion":'
        '$longRepliesAfterCompletion,"firstReplyIsTimeout":'
        '$firstReplyIsTimeout,"lastReplyIsSuccess":$lateReplyIsSuccess,'
        '"activeAfterCompletion":$activeAfterCompletion,'
        '"realNative":false,"realFile":false,"network":false}',
      );
      expect(native.completedErrors, _cycles);
      expect(longRepliesBeforeCompletion, 1);
      expect(firstReplyIsTimeout, isTrue);
      expect(activeAfterCompletion, 0);
      expect(
        longRepliesAfterCompletion,
        1,
        reason: 'A later native success cannot reply again after its timeout',
      );
    },
  );
}
