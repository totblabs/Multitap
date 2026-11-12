// Actual DriveSession with the existing memory-only socket/file adapter.
// No disk, native folder picker, credentials or network is accessed.
import 'dart:async';

import 'package:flutter_test/flutter_test.dart';
import 'package:multitap_client/data/drive_files.dart';
import 'package:multitap_client/data/drive_memory.dart';
import 'package:multitap_client/data/drive_session.dart';
import 'package:multitap_client/data/product_log.dart';

import 'drive_session_test.dart'
    show DriveSocket, FakeFiles, driveUrl, ready, sessionId;

const _origin = 'https://fixture.invalid';
const _vm = 'pve-149', _otherVm = 'pve-247';
const _nextId = '0b7d6f5c-3f9e-4f69-9d8c-2c1d6b8f0a22';
const _legacy = DriveFolder(path: r'C:\SYNTHETIC\legacy', label: 'legacy');
const _other = DriveFolder(path: r'C:\SYNTHETIC\other-vm', label: 'other');

class _LegacyStore implements DriveMemory {
  final values = <(String, String), List<DriveFolder>>{};
  final saves =
      <({String origin, String desktop, List<DriveFolder> folders})>[];
  int loads = 0;
  bool failClear = false;

  void seed(String desktop, List<DriveFolder> folders) =>
      values[(_origin, desktop)] = List.of(folders);
  List<DriveFolder> peek(String desktop) =>
      List.of(values[(_origin, desktop)] ?? const []);

  @override
  Future<List<DriveFolder>> load(String server, String desktopId) async {
    loads++;
    throw StateError('Legacy folder reads are forbidden in this fixture.');
  }

  @override
  Future<void> save(
    String server,
    String desktopId,
    List<DriveFolder> folders,
  ) async {
    saves.add((origin: server, desktop: desktopId, folders: List.of(folders)));
    if (failClear) throw StateError('Synthetic legacy cleanup refusal.');
    values[(server, desktopId)] = List.of(folders);
  }
}

class _HeldPicker extends FakeFiles {
  final picked = Completer<DrivePick?>();
  @override
  Future<DrivePick?> pickFolder() => picked.future;
}

// DriveSocket and the fake file methods complete via microtasks. No elapsed
// clock or sleep is used to authorize readiness or a late picker result.
Future<void> _drain() async {
  for (var n = 0; n < 20; n++) {
    await Future<void>.value();
  }
}

Future<DriveSession> _start(
  _LegacyStore store,
  FakeFiles files,
  DriveSocket socket, {
  String id = sessionId,
}) async {
  final drive = DriveSession(
    url: id == sessionId
        ? driveUrl
        : driveUrl.replace(path: '/api/v1/sessions/$id/drive'),
    token: 'synthetic-not-persisted',
    sessionId: id,
    files: files,
    memory: store,
    server: _origin,
    desktopId: _vm,
    log: ProductLog(),
    dialer: (_, _) async => socket,
  );
  try {
    await drive.start();
    socket.emit({...ready, 'sessionId': id});
    await _drain();
    expect(drive.ready, isTrue, reason: drive.error);
    return drive;
  } catch (_) {
    await drive.close();
    rethrow;
  }
}

void _emptyWire(DriveSocket socket) =>
    expect(socket.lastOf('drive.shares')['shares'], isEmpty);

void main() {
  test('old saved folders are never opened and only this VM legacy entry is cleared', () async {
    final store = _LegacyStore()
      ..seed(_vm, [_legacy])
      ..seed(_otherVm, [_other]);
    final files = FakeFiles(), socket = DriveSocket();
    final drive = await _start(store, files, socket);
    try {
      expect(store.loads, 0);
      expect(files.opened, isEmpty);
      expect(drive.shares, isEmpty);
      _emptyWire(socket);
      expect(store.peek(_vm), isEmpty);
      expect(store.peek(_otherVm), [_other]);
      expect(store.saves, isNotEmpty);
      expect(
        store.saves.every(
          (row) =>
              row.origin == _origin &&
              row.desktop == _vm &&
              row.folders.isEmpty,
        ),
        isTrue,
      );
    } finally {
      await drive.close();
      await _drain();
    }
    expect(store.peek(_otherVm), [
      _other,
    ], reason: 'Closing one VM cannot alter another VM entry.');
  });

  test(
    'an explicit choice belongs to one session and reconnect starts empty',
    () async {
      final store = _LegacyStore(),
          files = FakeFiles(),
          firstSocket = DriveSocket();
      final first = await _start(store, files, firstSocket);
      String? owned;
      try {
        files.pick = const DrivePick(r'C:\SYNTHETIC\chosen', 'chosen');
        expect(await first.addFolder(), isNull);
        owned = first.shares.single.id;
        expect(files.opened.keys, [owned]);
        expect(firstSocket.lastOf('drive.shares')['shares'], hasLength(1));
        expect(
          store.saves.every((row) => row.folders.isEmpty),
          isTrue,
          reason: 'The current selection must not be persisted for another connection.',
        );
      } finally {
        await first.close();
        await _drain();
      }
      expect(first.shares, isEmpty);
      expect(files.opened, isEmpty);
      expect(files.closed, contains(owned));
      final nextSocket = DriveSocket();
      final next = await _start(store, files, nextSocket, id: _nextId);
      try {
        expect(next.shares, isEmpty);
        expect(files.opened, isEmpty);
        _emptyWire(nextSocket);
        expect(store.loads, 0);
      } finally {
        await next.close();
        await _drain();
      }
    },
  );

  test(
    'socket loss withdraws choices and closes every opened share handle',
    () async {
      final store = _LegacyStore(), files = FakeFiles(), socket = DriveSocket();
      final drive = await _start(store, files, socket);
      try {
        for (final name in ['one', 'two']) {
          files.pick = DrivePick('C:/SYNTHETIC/$name', name);
          expect(await drive.addFolder(), isNull);
        }
        final owned = drive.shares.map((share) => share.id).toSet();
        expect(owned, hasLength(2));
        socket.incoming.addError(StateError('Synthetic channel loss.'));
        await _drain();
        expect(drive.closed, isTrue);
        expect(drive.error, 'drive_transport_closed');
        expect(drive.shares, isEmpty);
        expect(files.opened, isEmpty);
        expect(files.closed.toSet(), containsAll(owned));
        expect(store.loads, 0);
        expect(store.saves.every((row) => row.folders.isEmpty), isTrue);
      } finally {
        await drive.close();
        await _drain();
      }
    },
  );

  test(
    'unreadable legacy memory and failed cleanup never authorize sharing',
    () async {
      final store = _LegacyStore()
        ..seed(_vm, [_legacy])
        ..failClear = true;
      final files = FakeFiles(), socket = DriveSocket();
      final drive = await _start(store, files, socket);
      try {
        expect(
          store.loads,
          0,
          reason: 'Even a throwing load must never be called.',
        );
        expect(store.peek(_vm), [
          _legacy,
        ], reason: 'The synthetic clear really failed.');
        expect(drive.shares, isEmpty);
        expect(files.opened, isEmpty);
        _emptyWire(socket);
        files.pick = const DrivePick(r'C:\SYNTHETIC\manual', 'manual');
        expect(await drive.addFolder(), isNull);
        expect(drive.shares, hasLength(1));
        expect(files.opened.values.single.path, r'C:\SYNTHETIC\manual');
        expect(store.loads, 0);
        expect(store.saves.every((row) => row.folders.isEmpty), isTrue);
      } finally {
        await drive.close();
        await _drain();
      }
      expect(files.opened, isEmpty);
      expect(drive.shares, isEmpty);
    },
  );

  test('closing while the manual picker waits cannot late-open or publish a folder', () async {
    final store = _LegacyStore(), files = _HeldPicker(), socket = DriveSocket();
    final drive = await _start(store, files, socket);
    final choosing = drive.addFolder();
    expect(drive.adding, isTrue);
    try {
      await drive.close();
      final sentAtClose = socket.sent.length;
      files.picked.complete(const DrivePick(r'C:\SYNTHETIC\late', 'late'));
      expect(await choosing, isNull);
      await _drain();
      expect(drive.closed, isTrue);
      expect(drive.adding, isFalse);
      expect(drive.shares, isEmpty);
      expect(files.opened, isEmpty);
      expect(files.closed, isEmpty, reason: 'No native root was ever opened.');
      expect(socket.sent.length, sentAtClose);
      expect(store.loads, 0);
      expect(store.saves.every((row) => row.folders.isEmpty), isTrue);
    } finally {
      if (!files.picked.isCompleted) files.picked.complete(null);
      await choosing;
      await drive.close();
      await _drain();
    }
  });
}
