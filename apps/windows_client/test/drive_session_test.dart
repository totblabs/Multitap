// drive-v1 on the client (docs/drive-redirection.md): the pair handshake,
// the share list, every file request incl. the binary read/write framing,
// list paging from a snapshot, refusals, size bounds, remembered folders and
// the end of sharing. Fake socket and file system; no relay, Host or disk.
import 'dart:async';
import 'dart:convert';
import 'dart:io';
import 'dart:typed_data';

import 'package:flutter_test/flutter_test.dart';
import 'package:multitap_client/data/drive_files.dart';
import 'package:multitap_client/data/drive_memory.dart';
import 'package:multitap_client/data/drive_rules.dart';
import 'package:multitap_client/data/drive_session.dart';
import 'package:multitap_client/data/product_log.dart';
import 'package:multitap_client/ui/drive_panel.dart';

const sessionId = '0b7d6f5c-3f9e-4f69-9d8c-2c1d6b8f0a11';
final driveUrl = Uri.parse(
  'wss://fixture.invalid/api/v1/sessions/$sessionId/drive',
);
const ready = {
  'type': 'drive.ready',
  'sessionId': sessionId,
  'protocol': 'drive-v1',
  'limits': {
    'maxShares': 8,
    'chunkBytes': 262144,
    'maxInFlight': 8,
    'listPage': 256,
    'maxPath': 1024,
    'maxName': 255,
    'maxDepth': 64,
  },
};

class DriveSocket extends Fake implements WebSocket {
  final incoming = StreamController<dynamic>();
  final sent = <Map<String, dynamic>>[];
  final binary = <Uint8List>[];
  final sizes = <int>[];
  bool wasClosed = false;
  @override
  StreamSubscription<dynamic> listen(
    void Function(dynamic)? onData, {
    Function? onError,
    void Function()? onDone,
    bool? cancelOnError,
  }) => incoming.stream.listen(
    onData,
    onError: onError,
    onDone: onDone,
    cancelOnError: cancelOnError,
  );
  @override
  void add(dynamic data) {
    if (data is String) {
      sizes.add(utf8.encode(data).length);
      sent.add(jsonDecode(data) as Map<String, dynamic>);
    } else {
      binary.add(Uint8List.fromList(data as List<int>));
    }
  }

  void emit(Object value) => incoming.add(jsonEncode(value));
  @override
  Future<void> close([int? code, String? reason]) async {
    wasClosed = true;
    unawaited(incoming.close());
  }

  Map<String, dynamic> lastOf(String type) =>
      sent.lastWhere((value) => value['type'] == type);
  Map<String, dynamic> response(int id) => sent.lastWhere(
    (value) => value['type'] == 'drive.response' && value['id'] == id,
  );
}

class FakeFiles implements DriveFileSystem {
  final opened = <String, ({String path, bool readOnly})>{};
  final closed = <String>[];
  final calls = <String>[];
  final failOpen = <String, String>{};
  DrivePick? pick;
  List<DriveEntry> listing = const [];
  DriveFileException? failure;
  Uint8List content = Uint8List.fromList(utf8.encode('hello world'));
  final writes = <({String path, int offset, List<int> data, bool truncate})>[];
  Completer<void>? gate;

  /// Win32 errors reported with a failed open, and what happens while the
  /// native picker is up (the real one takes the window's focus).
  final win32 = <String, int>{};
  void Function()? whilePicking;
  DriveFileException? pickFailure;
  @override
  Future<DrivePick?> pickFolder() async {
    whilePicking?.call();
    if (pickFailure != null) throw pickFailure!;
    return pick;
  }

  @override
  Future<String> open(
    String share,
    String path, {
    required bool readOnly,
  }) async {
    final code = failOpen[path];
    if (code != null) throw DriveFileException(code, win32: win32[path] ?? 0);
    opened[share] = (path: path, readOnly: readOnly);
    return 'label';
  }

  @override
  Future<void> close(String share) async {
    closed.add(share);
    opened.remove(share);
  }

  /// Share ids whose folder was deleted on the PC, and the new path of ones
  /// renamed or moved there.
  final deleted = <String>{};
  final movedTo = <String, String>{};
  @override
  Future<DriveRootStatus> status(String share) async {
    final path = movedTo[share] ?? opened[share]?.path;
    if (path == null) throw const DriveFileException('drive_share_unknown');
    return DriveRootStatus(
      deleted: deleted.contains(share),
      offline: offline.contains(share),
      path: path,
      name: path.split(r'\').last,
    );
  }

  /// Share ids whose device is gone (a pulled USB stick).
  final offline = <String>{};

  Future<void> _check(String call) async {
    calls.add(call);
    await gate?.future;
    if (failure != null) throw failure!;
  }

  @override
  Future<DriveEntry> stat(String share, String path) async {
    await _check('stat $share $path');
    return path.isEmpty
        ? const DriveEntry(
            name: '',
            dir: true,
            modified: 5,
            created: 4,
            free: 10,
            total: 20,
          )
        : DriveEntry(
            name: path.split('/').last,
            dir: false,
            size: 11,
            modified: 1700000000000,
            created: 1600000000000,
          );
  }

  @override
  Future<List<DriveEntry>> list(String share, String path) async {
    await _check('list $share $path');
    return listing;
  }

  @override
  Future<Uint8List> read(
    String share,
    String path,
    int offset,
    int length,
  ) async {
    await _check('read $share $path $offset $length');
    final end = (offset + length).clamp(0, content.length);
    return Uint8List.fromList(
      offset >= content.length ? const [] : content.sublist(offset, end),
    );
  }

  @override
  Future<void> write(
    String share,
    String path,
    int offset,
    Uint8List data, {
    required bool truncate,
  }) async {
    await _check('write $share $path');
    writes.add((
      path: path,
      offset: offset,
      data: data.toList(),
      truncate: truncate,
    ));
  }

  @override
  Future<void> setSize(String share, String path, int size) =>
      _check('setsize $share $path $size');
  @override
  Future<void> flush(String share, String path) => _check('flush $share $path');
  @override
  Future<void> create(
    String share,
    String path, {
    required bool dir,
    required bool truncate,
  }) => _check('create $share $path $dir $truncate');
  @override
  Future<void> rename(
    String share,
    String path,
    String to, {
    required bool replace,
  }) => _check('rename $share $path $to $replace');
  @override
  Future<void> delete(String share, String path) =>
      _check('delete $share $path');
  @override
  Future<void> touch(String share, String path, int modified) =>
      _check('touch $share $path $modified');
}

Future<void> flush() async {
  for (var i = 0; i < 20; i++) {
    await Future<void>.delayed(Duration.zero);
  }
}

Uint8List writeFrame(int id, Map<String, Object> header, List<int> data) {
  final json = utf8.encode(jsonEncode(header));
  final frame = Uint8List(7 + json.length + data.length);
  frame[0] = 2;
  ByteData.sublistView(frame)
    ..setUint32(1, id, Endian.little)
    ..setUint16(5, json.length, Endian.little);
  frame.setRange(7, 7 + json.length, json);
  frame.setRange(7 + json.length, frame.length, data);
  return frame;
}

void main() {
  late DriveSocket socket;
  late FakeFiles files;
  late SessionDriveMemory memory;
  late ProductLog log;
  var dials = 0;

  var requestDeadline = const Duration(seconds: 40);
  var fs = false;
  DriveSession session({Uri? url, Duration? readyTimeout}) => DriveSession(
    requestDeadline: requestDeadline,
    fs: fs,
    url: url ?? driveUrl,
    token: 'not-persisted',
    sessionId: sessionId,
    files: files,
    memory: memory,
    server: 'https://fixture.invalid',
    desktopId: 'pve-149',
    dialer: (_, _) async {
      dials++;
      return socket;
    },
    readyTimeout: readyTimeout ?? const Duration(seconds: 20),
    log: log,
  );

  Future<DriveSession> started({
    List<DriveFolder> remembered = const [],
    List<DriveFolder> legacyFolders = const [],
  }) async {
    await memory.save('https://fixture.invalid', 'pve-149', legacyFolders);
    final drive = session();
    await drive.start();
    socket.emit(ready);
    await flush();
    // Current-session fixtures use the same explicit picker as the UI.
    for (final folder in remembered) {
      files.pick = DrivePick(folder.path, folder.label);
      expect(await drive.addFolder(), isNull);
      if (folder.readOnly) await drive.setReadOnly(drive.shares.last.id, true);
    }
    files.pick = null;
    return drive;
  }

  Future<DriveSession> sharing({bool readOnly = false}) async {
    final drive = await started(
      remembered: [
        DriveFolder(path: r'C:\Data', label: 'Data', readOnly: readOnly),
      ],
    );
    return drive;
  }

  String idOf(DriveSession drive) => drive.shares.single.id;

  void request(
    int id,
    String op,
    String share,
    String path, [
    Map<String, Object> extra = const {},
  ]) => socket.emit({
    'type': 'drive.request',
    'id': id,
    'op': op,
    'share': share,
    'path': path,
    ...extra,
  });

  setUp(() {
    socket = DriveSocket();
    files = FakeFiles();
    memory = SessionDriveMemory();
    log = ProductLog();
    dials = 0;
    requestDeadline = const Duration(seconds: 40);
    fs = false;
  });

  test('fs revision: setsize and flush are served with exact fields', () async {
    fs = true;
    final drive = await sharing();
    final share = idOf(drive);
    request(1, 'setsize', share, 'a.bin', {'size': 0});
    request(2, 'setsize', share, 'a.bin', {'size': 9007199254740991});
    request(3, 'flush', share, 'a.bin');
    request(4, 'flush', share, '');
    await flush();
    for (final id in [1, 2, 3, 4]) {
      expect(socket.response(id), {
        'type': 'drive.response',
        'id': id,
        'ok': true,
      });
    }
    expect(files.calls, [
      'setsize $share a.bin 0',
      'setsize $share a.bin 9007199254740991',
      'flush $share a.bin',
      'flush $share ',
    ]);
    // A size out of range, the root as a setsize target and a missing size
    // are refused before any file access; the pair stays up.
    files.calls.clear();
    request(5, 'setsize', share, 'a.bin', {'size': 9007199254740992});
    request(6, 'setsize', share, 'a.bin', {'size': -1});
    request(7, 'setsize', share, '', {'size': 1});
    request(8, 'setsize', share, 'a.bin', {'size': 1.5});
    request(9, 'setsize', share, 'a.bin');
    request(10, 'flush', share, 'a.bin', {'size': 1});
    request(11, 'flush', share, '../x');
    await flush();
    for (final id in [5, 6, 7, 8, 9, 10, 11]) {
      expect(socket.response(id)['error'], 'drive_invalid_path', reason: '$id');
    }
    expect(files.calls, isEmpty);
    expect(drive.closed, isFalse);
    // Errors keep their fixed codes.
    files.failure = const DriveFileException('drive_no_space');
    request(12, 'setsize', share, 'a.bin', {'size': 5});
    await flush();
    expect(socket.response(12)['error'], 'drive_no_space');
  });

  test(
    'fs revision: a read-only share refuses setsize, flush is not a change',
    () async {
      fs = true;
      final drive = await sharing(readOnly: true);
      final share = idOf(drive);
      request(1, 'setsize', share, 'a', {'size': 3});
      request(2, 'flush', share, 'a');
      await flush();
      expect(socket.response(1)['error'], 'drive_read_only');
      expect(socket.response(2)['ok'], isTrue);
      expect(files.calls, ['flush $share a']);
    },
  );

  test('without the fs revision setsize and flush are unknown ops', () async {
    final drive = await sharing();
    final share = idOf(drive);
    request(1, 'flush', share, 'a');
    await flush();
    expect(drive.closed, isTrue);
    expect(drive.error, 'drive_invalid_message');
    expect(files.calls, isEmpty);
  });

  test(
    'a write beyond the end and above 4 GiB reaches the files unchanged',
    () async {
      final drive = await sharing();
      final share = idOf(drive);
      socket.incoming.add(
        writeFrame(
          3,
          {
            'op': 'write',
            'share': share,
            'path': 'big.bin',
            'offset': 5 * 1024 * 1024 * 1024,
            'truncate': false,
          },
          [7],
        ),
      );
      await flush();
      expect(files.writes.single.offset, 5 * 1024 * 1024 * 1024);
      expect(socket.response(3)['ok'], isTrue);
    },
  );

  test('Host notices set the hints once; unknown codes and a notice before ready are handled', () async {
    final drive = await started();
    expect(drive.oldFileLimit || drive.fileTooLarge, isFalse);
    socket.emit({'type': 'drive.notice', 'code': 'drive_webclient_old_limit'});
    socket.emit({'type': 'drive.notice', 'code': 'drive_webclient_old_limit'});
    socket.emit({'type': 'drive.notice', 'code': 'something_newer'});
    await flush();
    expect(drive.oldFileLimit, isTrue);
    expect(drive.fileTooLarge, isFalse);
    expect(drive.closed, isFalse, reason: 'an unknown code never ends sharing');
    socket.emit({'type': 'drive.notice', 'code': 'drive_file_too_large'});
    await flush();
    expect(drive.fileTooLarge, isTrue);
    expect(drive.closed, isFalse);
    // A malformed notice ends sharing like any other malformed message.
    socket.emit({
      'type': 'drive.notice',
      'code': 'drive_file_too_large',
      'x': 1,
    });
    await flush();
    expect(drive.error, 'drive_invalid_message');
    // Before ready the same message is a violation.
    socket = DriveSocket();
    final early = session();
    await early.start();
    socket.emit({'type': 'drive.notice', 'code': 'drive_file_too_large'});
    await flush();
    expect(early.error, 'drive_invalid_message');
    expect(early.fileTooLarge, isFalse);
  });
  test('a hung folder is answered at the deadline, once; two in a row make it offline', () async {
    requestDeadline = const Duration(milliseconds: 60);
    final drive = await sharing();
    final share = idOf(drive);
    final gate = files.gate = Completer<void>();
    request(1, 'read', share, 'big.bin', {'offset': 0, 'length': 100});
    await Future<void>.delayed(const Duration(milliseconds: 160));
    await flush();
    // Answered as a plain failure well before the relay's 60 s limit.
    expect(socket.response(1), {
      'type': 'drive.response',
      'id': 1,
      'ok': false,
      'error': 'drive_io_failed',
    });
    expect(
      drive.closed,
      isFalse,
      reason: 'one hung folder must not end the pair',
    );
    expect(
      drive.shares.single.offline,
      isFalse,
      reason: 'a slow read (a OneDrive download) is not an unplugged disk',
    );
    expect(
      drive.active,
      1,
      reason: 'the unanswered native action still owns its slot',
    );
    request(2, 'stat', share, 'again');
    await Future<void>.delayed(const Duration(milliseconds: 160));
    await flush();
    expect(socket.response(2)['error'], 'drive_io_failed');
    expect(drive.shares.single.offline, isTrue);
    expect(drive.closed, isFalse);
    expect(drive.active, 2);
    // Later requests for that folder are refused at once, not queued behind it.
    files.gate = null;
    files.calls.clear();
    request(3, 'stat', share, 'f');
    await flush();
    expect(socket.response(3)['error'], 'drive_not_found');
    expect(files.calls.where((call) => call.startsWith('stat')), isEmpty);
    gate.complete();
    await flush();
    expect(drive.active, 0, reason: 'real completion returns both slots');
    for (final id in [1, 2]) {
      expect(socket.sent.where((value) => value['id'] == id), hasLength(1));
    }
  });

  test(
    'one slow request followed by a fast one does not count as a stall',
    () async {
      requestDeadline = const Duration(milliseconds: 60);
      final drive = await sharing();
      final share = idOf(drive);
      final gate = files.gate = Completer<void>();
      request(1, 'stat', share, 'slow');
      await Future<void>.delayed(const Duration(milliseconds: 160));
      gate.complete();
      files.gate = null;
      await flush();
      request(2, 'stat', share, 'fast');
      await flush();
      expect(socket.response(2)['ok'], isTrue);
      request(3, 'stat', share, 'fast-again');
      await flush();
      expect(socket.response(3)['ok'], isTrue);
      files.gate = Completer<void>();
      request(4, 'stat', share, 'slow-again');
      await Future<void>.delayed(const Duration(milliseconds: 160));
      expect(
        drive.shares.single.offline,
        isFalse,
        reason: 'the success in between reset the count',
      );
    },
  );

  test('a folder with more entries than the protocol carries is flagged, and only then', () async {
    final drive = await sharing();
    final share = idOf(drive);
    files.listing = [
      for (var i = 0; i < 100; i++) DriveEntry(name: 'f$i', dir: false),
    ];
    request(1, 'list', share, '', {'cursor': 0});
    await flush();
    expect(drive.listingTruncated, isFalse);
    files.listing = [
      for (var i = 0; i < driveMaxListEntries; i++)
        DriveEntry(name: 'f$i', dir: false),
    ];
    request(2, 'list', share, '', {'cursor': 0});
    await flush();
    expect(drive.listingTruncated, isTrue);
    expect(socket.response(2)['ok'], isTrue);
  });

  test('the late result of a request already answered is dropped', () async {
    requestDeadline = const Duration(milliseconds: 60);
    final drive = await sharing();
    final share = idOf(drive);
    final gate = files.gate = Completer<void>();
    request(7, 'read', share, 'big.bin', {'offset': 0, 'length': 5});
    await Future<void>.delayed(const Duration(milliseconds: 160));
    final before = socket.sent.length + socket.binary.length;
    gate.complete();
    await flush();
    expect(
      socket.sent.length + socket.binary.length,
      before,
      reason: 'the id is answered exactly once',
    );
    expect(socket.binary, isEmpty, reason: 'no late read data frame');
    expect(drive.closed, isFalse);
  });

  test('a folder whose device is gone is offline, answers not found and returns on retry', () async {
    final drive = await sharing();
    final share = idOf(drive);
    files.offline.add(share);
    await drive.checkFolders();
    expect(drive.shares.single.offline, isTrue);
    request(1, 'stat', share, '');
    await flush();
    expect(socket.response(1)['error'], 'drive_not_found');
    // Still unplugged: the retry keeps the drive in the VM and says why.
    files.failOpen[r'C:\Data'] = 'drive_not_found';
    expect(await drive.retry(share), 'drive_not_found');
    expect(drive.shares.single.offline, isTrue);
    expect(drive.shares.single.offered, isTrue);
    // Plugged in again: opened anew, the VM's requests work.
    files.failOpen.remove(r'C:\Data');
    files.offline.remove(share);
    expect(await drive.retry(share), isNull);
    expect(drive.shares.single.offline, isFalse);
    request(2, 'stat', share, 'a.txt');
    await flush();
    expect(socket.response(2)['ok'], isTrue);
  });

  test(
    'a folder that answers again is online without a retry when it only hung',
    () async {
      final drive = await sharing();
      final share = idOf(drive);
      files.offline.add(share);
      await drive.checkFolders();
      expect(drive.shares.single.offline, isTrue);
      files.offline.remove(share);
      await drive.checkFolders();
      expect(drive.shares.single.offline, isFalse);
    },
  );

  test('risky folders are named before they are shared', () {
    const env = {
      'SystemRoot': r'C:\Windows',
      'ProgramFiles': r'C:\Program Files',
      'ProgramData': r'C:\ProgramData',
      'SystemDrive': 'C:',
      'USERPROFILE': r'C:\Users\me',
    };
    DriveRisk risk(String path) => driveRiskOf(path, environment: env);
    expect(risk(r'D:\'), DriveRisk.driveRoot);
    expect(risk('d:'), DriveRisk.driveRoot);
    expect(risk(r'C:\'), DriveRisk.driveRoot);
    expect(risk(r'C:\Windows'), DriveRisk.system);
    expect(risk(r'c:\windows\system32\'), DriveRisk.system);
    expect(risk(r'C:\Program Files\App'), DriveRisk.system);
    expect(risk(r'C:\ProgramData'), DriveRisk.system);
    expect(risk(r'C:\Users'), DriveRisk.system);
    expect(risk(r'C:\Users\me'), DriveRisk.profile);
    expect(risk(r'C:\Users\me\AppData\Roaming'), DriveRisk.profile);
    expect(risk(r'\\server\share\team'), DriveRisk.network);
    expect(risk(r'\\?\UNC\server\share'), DriveRisk.network);
    expect(risk(r'C:\Users\me\Documents'), DriveRisk.none);
    expect(risk(r'D:\Work\한글 폴더'), DriveRisk.none);
    expect(
      risk(r'C:\WindowsApps'),
      DriveRisk.none,
      reason: 'prefix is not a parent',
    );
    for (final value in DriveRisk.values) {
      expect(driveRiskMessage(value) == null, value == DriveRisk.none);
    }
  });

  test('every code the user can meet says what to do next', () {
    for (final code in [
      'drive_not_found',
      'drive_denied',
      'drive_user_unavailable',
      'drive_webclient_unavailable',
      'drive_letter_unavailable',
      'no_interactive_session',
      'drive_unavailable',
    ]) {
      expect(driveMessage(code), isNot(driveMessage('unknown')), reason: code);
      expect(driveMessage(code).length, greaterThan(20), reason: code);
    }
    expect(driveMessage('drive_not_found'), contains('USB'));
    expect(driveMessage('drive_webclient_unavailable'), contains('WebClient'));
  });

  test('rules mirror the broker: names, paths and labels', () {
    for (final name in [
      'a',
      'Documents',
      '한글 폴더',
      'x' * 255,
      '😀',
      'COM0',
      'CONSOLE',
      '.hidden',
    ]) {
      expect(driveSafeName(name), isTrue, reason: name);
    }
    for (final name in [
      '',
      '.',
      '..',
      'a.',
      'a ',
      'a/b',
      r'a\b',
      'C:',
      'a*',
      'x' * 256,
      'CON',
      'con.txt',
      'NUL.tar.gz',
      'COM1',
      'lpt9.log',
      'COM¹',
      r'CONIN$',
      'AUX .txt',
      '\ud800',
      'a\u0001',
    ]) {
      expect(driveSafeName(name), isFalse, reason: name);
    }
    for (final path in ['', 'a', 'a/b/c', List.filled(64, 'd').join('/')]) {
      expect(driveSafePath(path), isTrue, reason: path);
    }
    for (final path in [
      '/',
      '/a',
      'a/',
      'a//b',
      '..',
      'a/../b',
      r'a\b',
      'C:/x',
      List.filled(65, 'd').join('/'),
      List.filled(5, 'y' * 250).join('/'),
      null,
      3,
    ]) {
      expect(driveSafePath(path), isFalse, reason: '$path');
    }
    expect(driveLabelFor(r'C:\Users\me\Documents'), 'Documents');
    expect(driveLabelFor(r'D:\'), 'D');
    expect(driveLabelFor(r'C:\x', 'Pictures'), 'Pictures');
    expect(driveLabelFor(r'\\server\share\team'), 'team');
    expect(driveLabelFor(r'C:\' + 'n' * 100).length, driveMaxLabel);
    expect(driveLabelFor(r'C:\CON'), '폴더');
  });

  test(
    'ready is exact; nothing is sent before it; the empty list follows',
    () async {
      final drive = session();
      await drive.start();
      expect(socket.sent, isEmpty, reason: 'nothing before drive.ready');
      socket.emit(ready);
      await flush();
      expect(drive.ready, isTrue);
      expect(socket.lastOf('drive.shares'), {
        'type': 'drive.shares',
        'shares': [],
      });
      expect(
        log.entries.any(
          (e) => e.event == LogEvent.drive && e.values['stage'] == 1,
        ),
        isTrue,
      );
    },
  );

  for (final (name, value) in [
    ('another session', {...ready, 'sessionId': 'other'}),
    ('another protocol', {...ready, 'protocol': 'drive-v2'}),
    (
      'other limits',
      {
        ...ready,
        'limits': {...ready['limits'] as Map, 'chunkBytes': 1 << 20},
      },
    ),
    ('an extra field', {...ready, 'extra': true}),
  ]) {
    test('a ready with $name ends sharing', () async {
      final drive = session();
      await drive.start();
      socket.emit(value);
      await flush();
      expect(drive.closed, isTrue);
      expect(drive.error, 'drive_invalid_message');
      expect(socket.wasClosed, isTrue);
    });
  }

  test(
    'a URL other than this session\'s drive socket is never dialed',
    () async {
      for (final url in [
        Uri.parse('wss://fixture.invalid/api/v1/sessions/other/drive'),
        Uri.parse('ws://fixture.invalid/api/v1/sessions/$sessionId/drive'),
        Uri.parse('wss://fixture.invalid/api/v1/sessions/$sessionId/drive?x=1'),
      ]) {
        final drive = session(url: url);
        await drive.start();
        expect(drive.error, 'drive_invalid_url');
      }
      expect(dials, 0);
    },
  );

  test('no ready in time ends sharing', () async {
    final drive = session(readyTimeout: const Duration(milliseconds: 20));
    await drive.start();
    await Future<void>.delayed(const Duration(milliseconds: 60));
    expect(drive.error, 'drive_pair_timeout');
  });

  test(
    'legacy remembered folders never open or reappear on a new connection',
    () async {
      files.failOpen[r'C:\Gone'] = 'drive_not_found';
      final drive = await started(
        legacyFolders: const [
          DriveFolder(path: r'C:\Data', label: 'Data'),
          DriveFolder(path: r'C:\Gone', label: 'Gone', readOnly: true),
        ],
      );
      expect(drive.shares, isEmpty);
      expect(files.opened, isEmpty);
      expect(socket.lastOf('drive.shares')['shares'], isEmpty);
      expect(await memory.load('https://fixture.invalid', 'pve-149'), isEmpty);
      files.pick = const DrivePick(r'C:\Data', 'Data');
      expect(await drive.addFolder(), isNull);
      final id = drive.shares.single.id;
      expect(files.opened.keys, [id]);
      expect(await memory.load('https://fixture.invalid', 'pve-149'), isEmpty);
      await drive.close();
      expect(drive.shares, isEmpty);
      expect(files.closed, contains(id));
      expect(files.opened, isEmpty);
    },
  );

  test('a malformed drive.mounted ends sharing', () async {
    final drive = await sharing();
    socket.emit({
      'type': 'drive.mounted',
      'shares': [
        {'id': idOf(drive), 'state': 'mapped', 'letter': 'C'},
      ],
    });
    await flush();
    expect(drive.error, 'drive_invalid_message');
  });

  test(
    'stat, read (binary), list, create, rename, delete and touch are served',
    () async {
      final drive = await sharing();
      final share = idOf(drive);
      request(1, 'stat', share, '');
      await flush();
      expect(socket.response(1), {
        'type': 'drive.response',
        'id': 1,
        'ok': true,
        'entry': {
          'name': '',
          'dir': true,
          'size': 0,
          'modified': 5,
          'created': 4,
          'readOnly': false,
          'space': {'free': 10, 'total': 20},
        },
      });
      request(2, 'stat', share, 'docs/a.txt');
      await flush();
      expect((socket.response(2)['entry'] as Map)['name'], 'a.txt');
      expect(
        (socket.response(2)['entry'] as Map).containsKey('space'),
        isFalse,
      );

      request(3, 'read', share, 'docs/a.txt', {'offset': 6, 'length': 100});
      await flush();
      final frame = socket.binary.single;
      expect(frame[0], 1);
      expect(ByteData.sublistView(frame).getUint32(1, Endian.little), 3);
      expect(utf8.decode(frame.sublist(5)), 'world');

      request(4, 'create', share, 'new', {'dir': true, 'truncate': false});
      request(5, 'rename', share, 'new', {'to': 'old', 'replace': false});
      request(6, 'touch', share, 'docs/a.txt', {'modified': 1700000000000});
      request(7, 'delete', share, 'old');
      await flush();
      for (final id in [4, 5, 6, 7]) {
        expect(socket.response(id), {
          'type': 'drive.response',
          'id': id,
          'ok': true,
        });
      }
      expect(
        files.calls,
        containsAll([
          'create $share new true false',
          'rename $share new old false',
          'touch $share docs/a.txt 1700000000000',
          'delete $share old',
        ]),
      );
      expect(drive.requests, 7);
      expect(drive.failedRequests, 0);
    },
  );

  test('writes arrive as one binary message: header + data', () async {
    final drive = await sharing();
    final share = idOf(drive);
    socket.incoming.add(
      writeFrame(
        9,
        {
          'op': 'write',
          'share': share,
          'path': 'f.bin',
          'offset': 0,
          'truncate': true,
        },
        [1, 2, 3],
      ),
    );
    await flush();
    expect(files.writes.single.path, 'f.bin');
    expect(files.writes.single.data, [1, 2, 3]);
    expect(files.writes.single.truncate, isTrue);
    expect(socket.response(9)['ok'], isTrue);
    socket.incoming.add(
      writeFrame(
        10,
        {
          'op': 'write',
          'share': share,
          'path': 'f.bin',
          'offset': 3,
          'truncate': false,
        },
        [4],
      ),
    );
    await flush();
    expect(files.writes.last.offset, 3);
    // Refused before any file access.
    socket.incoming.add(
      writeFrame(
        11,
        {
          'op': 'write',
          'share': share,
          'path': '../x',
          'offset': 0,
          'truncate': true,
        },
        [1],
      ),
    );
    socket.incoming.add(
      writeFrame(
        12,
        {
          'op': 'write',
          'share': 'nope',
          'path': 'x',
          'offset': 0,
          'truncate': true,
        },
        [1],
      ),
    );
    await flush();
    expect(socket.response(11)['error'], 'drive_invalid_path');
    expect(socket.response(12)['error'], 'drive_share_unknown');
    expect(files.writes, hasLength(2));
    // A frame that is not a write ends sharing.
    socket.incoming.add(Uint8List.fromList([1, 0, 0, 0, 1]));
    await flush();
    expect(drive.error, 'drive_invalid_message');
  });

  test(
    'a read-only share refuses every change, here as on the relay',
    () async {
      final drive = await sharing(readOnly: true);
      final share = idOf(drive);
      expect(files.opened[share]!.readOnly, isTrue);
      request(1, 'create', share, 'x', {'dir': false, 'truncate': true});
      request(2, 'rename', share, 'a', {'to': 'b', 'replace': true});
      request(3, 'delete', share, 'a');
      request(4, 'touch', share, 'a', {'modified': 0});
      socket.incoming.add(
        writeFrame(
          5,
          {
            'op': 'write',
            'share': share,
            'path': 'a',
            'offset': 0,
            'truncate': true,
          },
          [1],
        ),
      );
      request(6, 'read', share, 'a', {'offset': 0, 'length': 5});
      await flush();
      for (final id in [1, 2, 3, 4, 5]) {
        expect(socket.response(id)['error'], 'drive_read_only', reason: '$id');
      }
      expect(socket.binary, hasLength(1), reason: 'reads still work');
      expect(files.calls, ['read $share a 0 5']);
    },
  );

  test(
    'refusals: unknown share, invalid path, file errors with fixed codes',
    () async {
      final drive = await sharing();
      final share = idOf(drive);
      request(1, 'stat', 'other', '');
      request(2, 'stat', share, '../secret');
      request(3, 'stat', share, r'a\b');
      request(4, 'rename', share, 'a', {'to': 'a', 'replace': false});
      request(5, 'read', share, 'a', {
        'offset': 0,
        'length': driveChunkBytes + 1,
      });
      await flush();
      expect(socket.response(1)['error'], 'drive_share_unknown');
      for (final id in [2, 3, 4, 5]) {
        expect(
          socket.response(id)['error'],
          'drive_invalid_path',
          reason: '$id',
        );
      }
      expect(files.calls, isEmpty);
      files.failure = const DriveFileException('drive_not_found');
      request(6, 'stat', share, 'gone.txt');
      await flush();
      expect(socket.response(6), {
        'type': 'drive.response',
        'id': 6,
        'ok': false,
        'error': 'drive_not_found',
      });
      files.failure = const DriveFileException('drive_user_unavailable');
      request(7, 'stat', share, 'a');
      await flush();
      expect(
        socket.response(7)['error'],
        'drive_denied',
        reason: 'a local-only code never reaches the VM',
      );
      files.failure = const DriveFileException('C:\\private\\path');
      request(8, 'stat', share, 'a');
      await flush();
      expect(socket.response(8)['error'], 'drive_io_failed');
      expect(drive.failedRequests, 8);
      // A request without a valid id or op ends sharing.
      socket.emit({
        'type': 'drive.request',
        'op': 'stat',
        'share': share,
        'path': '',
      });
      await flush();
      expect(drive.error, 'drive_invalid_message');
    },
  );

  test('listing: snapshot at cursor 0, pages of <= 256 entries and <= 60,000 bytes, unsafe names left out', () async {
    final drive = await sharing();
    final share = idOf(drive);
    files.listing = [
      for (var i = 0; i < 600; i++)
        DriveEntry(name: 'f$i.txt', dir: false, size: i),
      const DriveEntry(name: 'CON', dir: false),
      const DriveEntry(name: 'bad.', dir: false),
      const DriveEntry(name: 'sub', dir: true, size: 99),
    ];
    request(1, 'list', share, '', {'cursor': 0});
    await flush();
    final first = socket.response(1);
    expect((first['entries'] as List).length, 256);
    expect(first['next'], 256);
    files.listing = const []; // later pages come from the snapshot
    request(2, 'list', share, '', {'cursor': 256});
    request(3, 'list', share, '', {'cursor': 512});
    await flush();
    expect(socket.response(2)['next'], 512);
    final last = socket.response(3);
    expect((last['entries'] as List).length, 89);
    expect(last.containsKey('next'), isFalse);
    final names = [
      for (final page in [first, socket.response(2), last])
        for (final entry in page['entries'] as List) (entry as Map)['name'],
    ];
    expect(names, isNot(contains('CON')));
    expect(names, isNot(contains('bad.')));
    expect(names.last, 'sub');
    expect(
      ((last['entries'] as List).last as Map)['size'],
      0,
      reason: 'folders have no size',
    );
    expect(files.calls.where((c) => c.startsWith('list')).length, 1);

    // Long names: a page stops before 60,000 bytes.
    files.listing = [
      for (var i = 0; i < 256; i++)
        DriveEntry(name: '${'가' * 240}$i', dir: false, size: 1 << 40),
    ];
    request(4, 'list', share, 'docs', {'cursor': 0});
    await flush();
    final page = socket.response(4);
    expect((page['entries'] as List).length, lessThan(256));
    expect(page['next'], (page['entries'] as List).length);
    expect(socket.sizes.every((size) => size <= driveJsonLimit), isTrue);
  });

  test(
    'adding, read-only and removing update only the current session',
    () async {
      final drive = await started();
      files.pick = const DrivePick(r'C:\Users\me\Documents', 'Documents');
      expect(await drive.addFolder(), isNull);
      final share = drive.shares.single;
      expect(share.label, 'Documents');
      expect(socket.lastOf('drive.shares')['shares'], [
        {'id': share.id, 'name': 'Documents', 'readOnly': false},
      ]);
      expect(await memory.load('https://fixture.invalid', 'pve-149'), isEmpty);
      // The same folder again, a cancelled picker, a folder that cannot be shared.
      files.pick = const DrivePick(r'c:\users\me\documents\', 'Documents');
      expect(await drive.addFolder(), 'drive_exists');
      files.pick = null;
      expect(await drive.addFolder(), isNull);
      files.pick = const DrivePick(r'C:\Locked', 'Locked');
      files.failOpen[r'C:\Locked'] = 'drive_denied';
      expect(await drive.addFolder(), 'drive_denied');
      expect(drive.shares, hasLength(1), reason: 'not added, not remembered');

      await drive.setReadOnly(share.id, true);
      expect(files.opened[share.id]!.readOnly, isTrue);
      expect(
        (socket.lastOf('drive.shares')['shares'] as List).single['readOnly'],
        isTrue,
      );
      expect(await memory.load('https://fixture.invalid', 'pve-149'), isEmpty);

      await drive.remove(share.id);
      expect(socket.lastOf('drive.shares')['shares'], isEmpty);
      expect(files.closed, contains(share.id));
      expect(await memory.load('https://fixture.invalid', 'pve-149'), isEmpty);
    },
  );

  test(
    'manual sharing leaves fixed diagnostics without restoring old choices',
    () async {
      files.failOpen[r'C:\Gone'] = 'drive_not_found';
      final drive = await started(
        remembered: const [DriveFolder(path: r'C:\Data', label: 'Data')],
      );
      List<ProductLogEntry> drives() =>
          log.entries.where((e) => e.event == LogEvent.drive).toList();
      ProductLogEntry stage(int number) =>
          drives().lastWhere((e) => e.values['stage'] == number);
      expect(drives().any((e) => e.values['stage'] == 10), isFalse);
      files.pick = const DrivePick(r'C:\Gone', 'Gone');
      expect(await drive.addFolder(), 'drive_not_found');
      expect(stage(8).code, 'drive_not_found');

      files.pick = null;
      await drive.addFolder();
      expect(stage(5).values['ok'], isTrue);
      expect(stage(6).values['ok'], isTrue);

      files.pick = const DrivePick(r'C:\Pics', 'Pics');
      await drive.addFolder(confirm: (_) async => false);
      expect(stage(7).values['ok'], isTrue);

      files.failOpen[r'C:\Locked'] = 'drive_denied';
      files.win32[r'C:\Locked'] = 5;
      files.pick = const DrivePick(r'C:\Locked', 'Locked');
      await drive.addFolder(confirm: (_) async => true);
      expect(stage(8).code, 'drive_denied');
      expect(stage(8).values['error'], 5);

      files.pick = const DrivePick(r'C:\Pics', 'Pics');
      expect(await drive.addFolder(confirm: (_) async => true), isNull);
      expect(stage(9).values['shares'], 2);

      files.pick = const DrivePick(r'C:\Other', 'Other');
      expect(
        await drive.addFolder(
          confirm: (_) async => throw StateError('dialog gone'),
        ),
        'drive_io_failed',
        reason: 'a dialog that cannot open is reported, never silent',
      );
      expect(stage(11).code, 'drive_io_failed');
      expect(drive.adding, isFalse);

      files.pickFailure = const DriveFileException(
        'drive_io_failed',
        win32: 1400,
      );
      expect(await drive.addFolder(), 'drive_io_failed');
      expect(stage(12).values['error'], 1400);
      files.pickFailure = null;

      files.deleted.add(drive.shares.first.id);
      await drive.checkFolders();
      expect(stage(13).values['ok'], isTrue);
      expect(
        drives().every((e) => e.values.keys.every(logFields.contains)),
        isTrue,
      );
    },
  );

  test('declining the confirmation opens and offers nothing', () async {
    final drive = await started();
    files.pick = const DrivePick(r'C:\Users\me\Pics', 'Pics');
    DrivePick? asked;
    expect(
      await drive.addFolder(
        confirm: (pick) async {
          asked = pick;
          return false;
        },
      ),
      isNull,
    );
    expect(asked?.path, r'C:\Users\me\Pics');
    expect(drive.shares, isEmpty);
    expect(files.opened, isEmpty);
    expect(drive.adding, isFalse);
    expect(await drive.addFolder(confirm: (_) async => true), isNull);
    expect(drive.shares.single.path, r'C:\Users\me\Pics');
  });

  test(
    'a renamed folder is followed; a deleted one ends its share for good',
    () async {
      final drive = await started(
        remembered: const [DriveFolder(path: r'C:\Data\Old', label: 'Old')],
      );
      final share = drive.shares.single;
      // Renamed on the PC: same share, new path and name, still offered.
      files.movedTo[share.id] = r'C:\Data\New';
      await drive.checkFolders();
      expect(drive.shares.single, same(share));
      expect(share.path, r'C:\Data\New');
      expect(share.shownName, 'New');
      expect(share.label, 'Old', reason: 'the VM keeps the name it was given');
      expect(await memory.load('https://fixture.invalid', 'pve-149'), isEmpty);

      // Deleted: unmapped at the Host, released here, forgotten, said once.
      files.deleted.add(share.id);
      await drive.checkFolders();
      expect(drive.shares, isEmpty);
      expect(socket.lastOf('drive.shares')['shares'], isEmpty);
      expect(files.closed, contains(share.id));
      expect(drive.lastGone, 'New');
      expect(drive.goneCount, 1);
      expect(await memory.load('https://fixture.invalid', 'pve-149'), isEmpty);
      await drive.checkFolders();
      expect(drive.goneCount, 1);
      expect(drive.closed, isFalse, reason: 'the other shares go on');
    },
  );

  test('at most eight folders', () async {
    final drive = await started(
      remembered: [
        for (var i = 0; i < 8; i++) DriveFolder(path: 'C:\\F$i', label: 'F$i'),
      ],
    );
    expect(drive.canAdd, isFalse);
    files.pick = const DrivePick(r'C:\Ninth', 'Ninth');
    expect(await drive.addFolder(), 'drive_limit');
    expect((socket.lastOf('drive.shares')['shares'] as List).length, 8);
  });

  test(
    'revocation ends sharing once, closes the local shares and says why',
    () async {
      final drive = await sharing();
      final share = idOf(drive);
      socket.emit({'type': 'drive.error', 'reason': 'drive_policy_changed'});
      await flush();
      expect(drive.closed, isTrue);
      expect(drive.error, 'drive_policy_changed');
      expect(drive.endedUnexpectedly, isTrue);
      expect(driveEndedMessage(drive), '관리자가 공유 폴더 권한을 해제했어요.');
      expect(files.closed, [share]);
      expect(socket.wasClosed, isTrue);
      expect(drive.canAdd, isFalse);
      files.pick = const DrivePick(r'C:\Later', 'Later');
      expect(await drive.addFolder(), 'drive_session_closed');
    },
  );

  test('a lost socket ends sharing; a normal end is quiet', () async {
    var drive = await sharing();
    await socket.incoming.close();
    await flush();
    expect(drive.error, 'drive_transport_closed');
    expect(
      driveEndedMessage(drive),
      '공유 폴더 연결이 끊겼어요. 데스크톱에 다시 연결한 뒤 폴더를 다시 고르세요.',
    );

    socket = DriveSocket();
    drive = await sharing();
    socket.emit({'type': 'drive.error', 'reason': 'session_ended'});
    await flush();
    expect(driveEndedMessage(drive), isNull);
    socket = DriveSocket();
    drive = await sharing();
    await drive.close();
    expect(drive.error, 'drive_session_closed');
    expect(driveEndedMessage(drive), isNull);
    final records = log.entries
        .where((e) => e.event == LogEvent.drive)
        .toList();
    expect(
      records.every((e) => e.values.keys.every(logFields.contains)),
      isTrue,
    );
    expect(records.last.code, 'drive_session_closed');
  });

  // Tuning #7: a sharing end that is not the channel's own fault is ok, with
  // its code kept as information; a lost channel stays a failure.
  test('the end record is ok only for normal ends', () async {
    final cases = {
      'session_ended': true,
      'drive_policy_changed': true,
      'authorization_revoked': true,
      'server_stopping': true,
      'drive_pair_timeout': false,
      'drive_invalid_message': false,
    };
    for (final MapEntry(key: reason, value: ok) in cases.entries) {
      socket = DriveSocket();
      await sharing();
      socket.emit({'type': 'drive.error', 'reason': reason});
      await flush();
      final end = log.entries.lastWhere(
        (e) => e.event == LogEvent.drive && e.values['stage'] == 4,
      );
      expect(end.code, reason);
      expect(end.values['ok'], ok, reason: reason);
    }
    socket = DriveSocket();
    await sharing();
    await socket.incoming.close();
    await flush();
    final lost = log.entries.lastWhere(
      (e) => e.event == LogEvent.drive && e.values['stage'] == 4,
    );
    expect(lost.code, 'drive_transport_closed');
    expect(lost.values['ok'], isFalse);
  });

  test('concurrent requests are served independently', () async {
    final drive = await sharing();
    final share = idOf(drive);
    files.gate = Completer<void>();
    for (var id = 1; id <= 8; id++) {
      request(id, 'stat', share, 'f$id');
    }
    await flush();
    expect(drive.active, 8);
    files.gate!.complete();
    await flush();
    expect(drive.active, 0);
    for (var id = 1; id <= 8; id++) {
      expect(socket.response(id)['ok'], isTrue);
    }
  });

  test('a ninth request in flight is refused as busy, never queued', () async {
    final drive = await sharing();
    final share = idOf(drive);
    files.gate = Completer<void>();
    for (var id = 1; id <= 8; id++) {
      request(id, 'stat', share, 'f$id');
    }
    await flush();
    request(9, 'stat', share, 'f9');
    socket.incoming.add(
      writeFrame(
        10,
        {
          'op': 'write',
          'share': share,
          'path': 'w',
          'offset': 0,
          'truncate': true,
        },
        [1],
      ),
    );
    await flush();
    expect(socket.response(9)['error'], 'drive_busy');
    expect(socket.response(10)['error'], 'drive_busy');
    files.gate!.complete();
    await flush();
    expect(drive.active, 0);
  });

  test(
    'a folder that cannot be opened again when its mode changes is withdrawn',
    () async {
      final drive = await sharing();
      final share = idOf(drive);
      files.failOpen[r'C:\Data'] = 'drive_not_found';
      await drive.setReadOnly(share, true);
      expect(drive.shares.single.state, DriveShareState.unavailable);
      expect(socket.lastOf('drive.shares')['shares'], isEmpty);
      expect(files.closed, contains(share));
      expect(files.opened.containsKey(share), isFalse);
    },
  );

  test(
    'FileDriveMemory keeps one file per server and VM, through plain folders',
    () async {
      final root = await Directory.systemTemp.createTemp(
        'multitap-drive-memory',
      );
      addTearDown(() => root.delete(recursive: true));
      final store = FileDriveMemory(Directory('${root.path}/drives'));
      expect(await store.load('https://a.example', 'pve-1'), isEmpty);
      await store.save('https://a.example', 'pve-1', const [
        DriveFolder(path: r'C:\A', label: 'A', readOnly: true),
      ]);
      await store.save('https://a.example', 'pve-2', const [
        DriveFolder(path: r'C:\B', label: 'B'),
      ]);
      expect(await store.load('https://a.example', 'pve-1'), [
        const DriveFolder(path: r'C:\A', label: 'A', readOnly: true),
      ]);
      expect(await store.load('https://b.example', 'pve-1'), isEmpty);
      final files = Directory('${root.path}/drives').listSync();
      expect(files, hasLength(2));
      // A damaged or foreign file is ignored, never trusted.
      File(files.first.path)
          .writeAsStringSync('{"server":"x","desktop":"y","folders":[]}');
      File(files.last.path).writeAsStringSync('not json');
      expect(await store.load('https://a.example', 'pve-1'), isEmpty);
      expect(await store.load('https://a.example', 'pve-2'), isEmpty);
      await store.save('https://a.example', 'pve-2', const []);
      expect(await store.load('https://a.example', 'pve-2'), isEmpty);
    },
  );
}
