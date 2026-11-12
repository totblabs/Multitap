import 'dart:async';
import 'dart:convert';
import 'dart:io';

import 'package:flutter/painting.dart' show MemoryImage;
import 'package:flutter/services.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:multitap_client/data/desktop_thumbnails.dart';
import 'package:multitap_client/data/remote_session.dart';
import 'package:multitap_client/data/broker_api.dart';
import 'package:multitap_client/data/desktop_mapping.dart';
import 'package:multitap_client/data/session_windows.dart';

import 'remote_session_test.dart' show SessionMedia, SessionSocket, info, flush;

const _key = '1234567890abcdef';
const _epoch = '0123456789abcdef0000000000000001';
String _epochAt(int value) =>
    '0123456789abcdef${value.toRadixString(16).padLeft(16, '0')}';
List<int> _jpegAt(String epoch) {
  final marker = ascii.encode('Multitap:thumbnail-privacy-v1:$epoch');
  return [
    0xff,
    0xd8,
    0xff,
    0xfe,
    (marker.length + 2) >> 8,
    (marker.length + 2) & 255,
    ...marker,
    0xff,
    0xe0,
    0x10,
    0xff,
    0xd9,
  ];
}

final _jpeg = _jpegAt(_epoch);
void _allow(
  DesktopThumbnails store, {
  String id = 'pve-149',
  String epoch = _epoch,
  bool allowed = true,
}) => store.observePrivacy(
  _key,
  id,
  ThumbnailPrivacy(epoch: epoch, allowed: allowed),
);

class _Stat extends Fake implements FileStat {
  @override
  final FileSystemEntityType type;
  _Stat({this.type = FileSystemEntityType.file, this.length});
  final int? length;
  @override
  int get size => length ?? _jpeg.length;
  @override
  DateTime get modified => DateTime.utc(2026, 10, 28);
}

class _Entry {
  bool exists = true;
  int opens = 0;
  List<int> bytes = _jpeg;
  Completer<FileStat>? stat;
  Completer<void>? read;
  bool failStat = false;
}

class _Fs {
  final entries = <String, _Entry>{};
  _Entry entry(String path) => entries.putIfAbsent(path, () {
    final entry = _Entry();
    if (path.endsWith('.revoked')) {
      entry.exists = false;
      entry.bytes = const [];
    }
    return entry;
  });
  Future<void> run(
    Future<void> Function(DesktopThumbnails store) body, {
    bool authorize = true,
    int? budget,
  }) => IOOverrides.runZoned(
    () {
      final store = DesktopThumbnails(
        _Directory(this, '${Platform.pathSeparator}memory'),
        cacheByteBudget: budget,
      );
      if (authorize) {
        _allow(store);
        _allow(store, id: 'pve-150');
      }
      return body(store);
    },
    createFile: (path) => _File(this, path),
    createDirectory: (path) => _Directory(this, path),
    fseGetTypeSync: (_, _) => FileSystemEntityType.directory,
  );
}

class _Directory extends Fake implements Directory {
  _Directory(this.fs, this.path);
  final _Fs fs;
  @override
  final String path;
  @override
  Future<Directory> delete({bool recursive = false}) async {
    for (final item in fs.entries.entries) {
      if (item.key.startsWith('$path${Platform.pathSeparator}')) {
        item.value.exists = false;
      }
    }
    return this;
  }

  @override
  Stream<FileSystemEntity> list({
    bool recursive = false,
    bool followLinks = true,
  }) => const Stream.empty();
}

class _File extends Fake implements File {
  _File(this.fs, this.path);
  final _Fs fs;
  @override
  final String path;
  @override
  File get absolute => this;
  @override
  Directory get parent => _Directory(
    fs,
    path.substring(0, path.lastIndexOf(Platform.pathSeparator)),
  );
  @override
  Future<FileStat> stat() async {
    final value = fs.entry(path);
    if (value.failStat) throw FileSystemException('fixture unreadable marker');
    return await value.stat?.future ??
        _Stat(
          type: value.exists
              ? FileSystemEntityType.file
              : FileSystemEntityType.notFound,
          length: value.bytes.length,
        );
  }

  @override
  Future<RandomAccessFile> open({FileMode mode = FileMode.read}) async {
    final entry = fs.entry(path);
    entry.opens++;
    return _Reader(entry);
  }
}

class _Reader extends Fake implements RandomAccessFile {
  _Reader(this.entry);
  final _Entry entry;
  int at = 0;
  @override
  Future<Uint8List> read(int bytes) async {
    await entry.read?.future;
    final value = Uint8List.fromList(entry.bytes.skip(at).take(bytes).toList());
    at += value.length;
    return value;
  }

  @override
  Future<RandomAccessFile> setPosition(int position) async {
    at = position;
    return this;
  }

  @override
  Future<void> close() async {}
}

class _PrivacyMedia extends Fake
    implements RemoteMedia, RemoteThumbnailPrivacy {
  final calls = <String>[];
  final cleared = Completer<void>();
  @override
  Future<void> clearThumbnail() {
    calls.add('privacy');
    return cleared.future;
  }

  @override
  Future<void> stop() async {
    calls.add('stop');
  }

  @override
  Future<bool> capture(bool enabled) async => false;
  @override
  Stream<Map<String, dynamic>> get input => const Stream.empty();
}

class _LivePrivacyMedia extends SessionMedia implements RemoteThumbnailPrivacy {
  final order = <String>[];
  @override
  Future<void> clearThumbnail() async {
    order.add('privacy');
  }

  @override
  Future<void> stop() async {
    order.add('stop');
    await super.stop();
  }
}

BrokerConnection _connection() => BrokerConnection.fromJson({
  'sessionId': 'test-privacy',
  'webSocketUrl': 'wss://vdi.example.com/api/v1/sessions/test-privacy',
}, Uri.parse('https://vdi.example.com'));

void main() {
  TestWidgetsFlutterBinding.ensureInitialized();
  test('strict fresh Host privacy survives broker mapping and presentation-only machine copies', () {
    final privacy = <String, Object>{
      'protocol': thumbnailPrivacyProtocol,
      'epoch': _epoch,
      'allowed': true,
    };
    final data = <String, dynamic>{
      'id': 'pve-149',
      'name': 'Fixture',
      'kind': 'vm',
      'description': '',
      'os': 'Windows',
      'status': 'ready',
      'canConnect': true,
      'host': {
        'connected': true,
        'capabilitiesPending': false,
        'thumbnailPrivacy': privacy,
      },
    };
    final machine = machineFromBroker(BrokerDesktop.fromJson(data));
    expect(machine.thumbnailPrivacy?.epoch, _epoch);
    expect(
      machine.withResources(machine.resources).thumbnailPrivacy,
      machine.thumbnailPrivacy,
    );
    expect(
      machine.withDisplay(machine.display).thumbnailPrivacy,
      machine.thumbnailPrivacy,
    );
    expect(
      machine.withOrganize(machine.organize).thumbnailPrivacy,
      machine.thumbnailPrivacy,
    );
    expect(
      machine.withHostUpdate(machine.hostUpdate).thumbnailPrivacy,
      machine.thumbnailPrivacy,
    );
    expect(
      machine
          .withPower(
            state: 'running',
            checkedAt: null,
            stale: false,
            busy: false,
          )
          .thumbnailPrivacy,
      machine.thumbnailPrivacy,
    );
    for (final host in <Object?>[
      null,
      {'connected': false, 'thumbnailPrivacy': privacy},
      {
        'connected': true,
        'capabilitiesPending': true,
        'thumbnailPrivacy': privacy,
      },
      {
        'connected': true,
        'thumbnailPrivacy': {...privacy, 'allowed': 1},
      },
      {
        'connected': true,
        'thumbnailPrivacy': {
          ...privacy,
          'epoch': '0123456789abcdef0000000000000000',
        },
      },
    ]) {
      expect(
        BrokerDesktop.fromJson({...data, 'host': host}).thumbnailPrivacy,
        isNull,
      );
    }
  });
  test('old Broker raw Host capability cannot attest thumbnail privacy or override a fresh wrapper', () {
    final allowed = <String, Object>{
      'protocol': thumbnailPrivacyProtocol,
      'epoch': _epoch,
      'allowed': true,
    };
    final base = <String, dynamic>{
      'id': 'pve-149',
      'name': 'Fixture',
      'kind': 'vm',
      'description': '',
      'os': 'Windows',
      'status': 'ready',
      'canConnect': true,
    };
    final raw = <String, Object>{'thumbnailPrivacy': allowed};
    for (final host in <Map<String, Object?>>[
      {'connected': true, 'capabilities': raw},
      {'connected': true, 'capabilitiesPending': false, 'capabilities': raw},
      {'connected': true, 'capabilities': raw, 'thumbnailPrivacy': null},
      {
        'connected': true,
        'capabilities': raw,
        'thumbnailPrivacy': {...allowed, 'allowed': 'true'},
      },
      {
        'connected': true,
        'capabilitiesPending': true,
        'capabilities': raw,
        'thumbnailPrivacy': allowed,
      },
    ]) {
      expect(
        BrokerDesktop.fromJson({...base, 'host': host}).thumbnailPrivacy,
        isNull,
        reason: 'unknown/raw capabilities are not proof of Broker77 fresh projection',
      );
    }
    final projected = BrokerDesktop.fromJson({
      ...base,
      'host': {
        'connected': true,
        'capabilitiesPending': false,
        'capabilities': raw,
        'thumbnailPrivacy': {
          ...allowed,
          'epoch': _epochAt(2),
          'allowed': false,
        },
      },
    }).thumbnailPrivacy;
    expect(projected?.epoch, _epochAt(2));
    expect(
      projected?.allowed,
      isFalse,
      reason: 'raw allowed=true never overrides projected denial',
    );
  });
  test(
    'forget fences a held actual store stat before it can repopulate cache',
    () async {
      final fs = _Fs();
      await fs.run((store) async {
        final path = store.file(_key, 'pve-149').path,
            entry = fs.entry(store.file(_key, 'pve-149').path);
        final held = entry.stat = Completer<FileStat>();
        final old = store.read(_key, 'pve-149');
        await store.forget(_key);
        held.complete(_Stat());
        expect(await old, isNull);
        entry.stat = null;
        entry.exists = true;
        _allow(store);
        expect(await store.read(_key, 'pve-149'), isNotNull);
        expect(
          fs.entry(path).opens,
          1,
          reason: 'late metadata never installed an old cached provider',
        );
      });
    },
  );
  test(
    'forget fences a held open-reader completion without real filesystem IO',
    () async {
      final fs = _Fs();
      await fs.run((store) async {
        final entry = fs.entry(store.file(_key, 'pve-149').path),
            held = Completer<void>();
        entry.read = held;
        final old = store.read(_key, 'pve-149');
        for (var turn = 0; turn < 8 && entry.opens == 0; turn++) {
          await Future<void>.delayed(Duration.zero);
        }
        expect(entry.opens, 1);
        await store.forget(_key);
        held.complete();
        expect(await old, isNull);
        entry.read = null;
        entry.exists = true;
        _allow(store);
        expect(await store.read(_key, 'pve-149'), isNotNull);
        expect(entry.opens, 2);
      });
    },
  );
  test('privacy clear blocks stale file/old notice until a newer current native write', () async {
    final fs = _Fs();
    await fs.run((store) async {
      expect(await store.read(_key, 'pve-149'), isNotNull);
      expect(
        store.acceptNotice(
          const ThumbnailUpdate(
            _key,
            'pve-149',
            generation: 6,
            invalidated: true,
            epoch: _epoch,
          ),
        ),
        isTrue,
      );
      expect(
        await store.read(_key, 'pve-149'),
        isNull,
        reason: 'disk may still contain old bytes; cache must not display them',
      );
      expect(
        store.acceptNotice(
          const ThumbnailUpdate(_key, 'pve-149', generation: 5, epoch: _epoch),
        ),
        isFalse,
      );
      expect(
        store.acceptNotice(const ThumbnailUpdate(_key, 'pve-149')),
        isFalse,
        reason: 'legacy notice cannot revive a revoked target',
      );
      expect(await store.read(_key, 'pve-149'), isNull);
      expect(
        store.acceptNotice(
          const ThumbnailUpdate(_key, 'pve-149', generation: 9, epoch: _epoch),
        ),
        isTrue,
      );
      expect(await store.read(_key, 'pve-149'), isNotNull);
    });
  });
  test(
    'per-VM clear preserves another VM and rejects a late old-owner clear',
    () async {
      final fs = _Fs();
      await fs.run((store) async {
        final other = await store.read(_key, 'pve-150');
        store.acceptNotice(
          const ThumbnailUpdate(_key, 'pve-149', generation: 9, epoch: _epoch),
        );
        expect(
          store.acceptNotice(
            const ThumbnailUpdate(
              _key,
              'pve-149',
              generation: 6,
              invalidated: true,
              epoch: _epoch,
            ),
          ),
          isFalse,
        );
        expect(await store.read(_key, 'pve-149'), isNotNull);
        expect(await store.read(_key, 'pve-150'), same(other));
      });
    },
  );
  test('account switch fences an old pending read even if its provider was not cached yet', () async {
    final fs = _Fs();
    await fs.run((store) async {
      final entry = fs.entry(store.file(_key, 'pve-149').path),
          held = Completer<FileStat>();
      entry.stat = held;
      final old = store.read(_key, 'pve-149');
      await store.retainOnly('fedcba0987654321');
      held.complete(_Stat());
      expect(await old, isNull);
    });
  });
  test(
    'restart denies an old valid JPEG until a current same-epoch Host proof',
    () async {
      await _Fs().run((store) async {
        expect(await store.read(_key, 'pve-149'), isNull);
        _allow(store);
        final snapshot = await store.read(_key, 'pve-149');
        expect(snapshot, isNotNull);
        store.suspendAccount(_key);
        expect(await store.read(_key, 'pve-149'), isNull);
        _allow(store);
        expect(
          await store.read(_key, 'pve-149'),
          isNotNull,
          reason: 'normal disconnect may keep a same-epoch snapshot after fresh proof',
        );
      }, authorize: false);
    },
  );
  test('lock revokes its epoch, stale counter cannot contaminate a newer epoch, repeated locks do not saturate', () async {
    await _Fs().run((store) async {
      _allow(store, allowed: false);
      _allow(store);
      expect(store.allowedEpoch(_key, 'pve-149'), isNull);
      for (var counter = 2; counter <= 700; counter++) {
        _allow(store, epoch: _epochAt(counter), allowed: counter.isEven);
      }
      expect(store.allowedEpoch(_key, 'pve-149'), _epochAt(700));
      _allow(store, epoch: _epochAt(699), allowed: false);
      expect(store.allowedEpoch(_key, 'pve-149'), _epochAt(700));
      expect(
        store.acceptNotice(
          const ThumbnailUpdate(
            _key,
            'pve-149',
            generation: 999,
            epoch: _epoch,
          ),
        ),
        isFalse,
      );
    });
  });
  test(
    'immutable checked bytes cannot turn into a replacement epoch after read',
    () async {
      final fs = _Fs();
      await fs.run((store) async {
        final snapshot = await store.read(_key, 'pve-149');
        final bytes = (snapshot!.image as MemoryImage).bytes;
        final entry = fs.entry(store.file(_key, 'pve-149').path);
        entry.bytes = _jpegAt(_epochAt(2));
        expect(thumbnailJpegEpoch(bytes), _epoch);
        _allow(store, epoch: _epochAt(2));
        expect(await store.read(_key, 'pve-149'), isNotNull);
        entry.bytes = _jpeg;
        expect(
          await store.read(_key, 'pve-149', refresh: true),
          isNull,
          reason: 'a late old native replacement is unreadable under the new Host epoch',
        );
      });
    },
  );
  test('replacement refresh during a held read performs a fresh read instead of staying null', () async {
    final fs = _Fs();
    await fs.run((store) async {
      final entry = fs.entry(store.file(_key, 'pve-149').path);
      final held = entry.read = Completer<void>();
      final first = store.read(_key, 'pve-149');
      for (var n = 0; n < 8 && entry.opens == 0; n++) {
        await Future<void>.delayed(Duration.zero);
      }
      expect(entry.opens, 1);
      final refreshed = store.read(_key, 'pve-149', refresh: true);
      entry.read = null;
      held.complete();
      expect(await first, isNotNull);
      expect(await refreshed, isNotNull);
      expect(
        entry.opens,
        2,
        reason: 'the coalesced refresh owns a second actual read',
      );
    });
  });
  test('Host policy change during stat/read rejects old content even if disk deletion fails', () async {
    final fs = _Fs();
    await fs.run((store) async {
      final entry = fs.entry(store.file(_key, 'pve-149').path);
      final held = entry.read = Completer<void>();
      final first = store.read(_key, 'pve-149');
      for (var n = 0; n < 8 && entry.opens == 0; n++) {
        await Future<void>.delayed(Duration.zero);
      }
      _allow(store, allowed: false);
      held.complete();
      expect(await first, isNull);
      expect(
        entry.exists,
        isTrue,
        reason: 'this proof does not depend on deleting a file',
      );
      expect(await store.read(_key, 'pve-149'), isNull);
    });
  });
  test(
    'encoded cache budget bounds providers retained across many desktops',
    () async {
      final fs = _Fs();
      await fs.run((store) async {
        expect(await store.read(_key, 'pve-149'), isNotNull);
        expect(await store.read(_key, 'pve-150'), isNotNull);
        await store.read(_key, 'pve-149');
        expect(
          fs.entry(store.file(_key, 'pve-149').path).opens,
          2,
          reason: 'the first provider was evicted when the second exceeded the runtime-sized budget',
        );
      }, budget: _jpeg.length);
    },
  );
  test('durable same-epoch marker denies an already cached or restart JPEG; other-epoch marker does not', () async {
    final fs = _Fs();
    await fs.run((store) async {
      expect(await store.read(_key, 'pve-149'), isNotNull);
      final path = store.file(_key, 'pve-149').path;
      final marker = fs.entry('$path.revoked')
        ..exists = true
        ..bytes = ascii.encode(_epoch);
      expect(
        await store.read(_key, 'pve-149'),
        isNull,
        reason: 'cache hit still checks durable local revocation',
      );
      final restarted = DesktopThumbnails(
        _Directory(fs, '${Platform.pathSeparator}memory'),
        cacheByteBudget: 1024,
      );
      _allow(restarted);
      expect(await restarted.read(_key, 'pve-149'), isNull);
      marker.bytes = ascii.encode(_epochAt(2));
      expect(await store.read(_key, 'pve-149'), isNotNull);
      marker.bytes = ascii.encode('malformed');
      expect(await store.read(_key, 'pve-149'), isNull);
      marker.failStat = true;
      expect(await store.read(_key, 'pve-149'), isNull);
    });
  });
  test('a marker persisted during an open JPEG read wins even without a native notice', () async {
    final fs = _Fs();
    await fs.run((store) async {
      final path = store.file(_key, 'pve-149').path;
      final entry = fs.entry(path), held = Completer<void>();
      entry.read = held;
      final picture = store.read(_key, 'pve-149');
      for (var n = 0; n < 8 && entry.opens == 0; n++) {
        await Future<void>.delayed(Duration.zero);
      }
      fs.entry('$path.revoked')
        ..exists = true
        ..bytes = ascii.encode(_epoch);
      held.complete();
      expect(await picture, isNull);
    });
  });
  test('explicit ordinary disconnect preserves last-frame behavior', () async {
    final media = _PrivacyMedia();
    final normal = RemoteSession(
      connection: _connection(),
      token: 'memory-only',
      media: media,
    );
    await normal.close();
    expect(media.calls, ['stop']);
  });
  test('uncertain end revokes before actual media stop; receipt completion releases retirement', () async {
    final media = _PrivacyMedia();
    final uncertain = RemoteSession(
      connection: _connection(),
      token: 'memory-only',
      media: media,
    );
    final done = uncertain.close(notifyHost: false);
    await Future<void>.delayed(Duration.zero);
    expect(media.calls, ['privacy']);
    media.cleared.complete();
    await done;
    expect(media.calls, ['privacy', 'stop']);
  });
  test('actual no_interactive_session control path revokes old frame before retirement', () async {
    final socket = SessionSocket(), media = _LivePrivacyMedia();
    final session = RemoteSession(
      connection: _connection(),
      token: 'memory-only',
      media: media,
      dialer: (_, _) async => socket,
    );
    final started = session.start();
    try {
      await flush();
      socket.incoming.add(jsonEncode(info));
      await flush();
      socket.incoming.add(
        jsonEncode({
          'type': 'stream.unavailable',
          'sessionId': 'test-privacy',
          'reason': 'no_interactive_session',
        }),
      );
      await started.timeout(const Duration(seconds: 1));
      expect(media.order, ['privacy', 'stop']);
      expect(session.closed, isTrue);
    } finally {
      await session.close();
      await socket.incoming.close();
      await media.events.close();
      session.dispose();
    }
  });
  testWidgets(
    'unconfirmed native-start reply revokes its own lease before asynchronous stop',
    (tester) async {
      const channel = MethodChannel('multitap/remote');
      final methods = <String>[], leases = <int>[];
      final messenger =
          TestDefaultBinaryMessengerBinding.instance.defaultBinaryMessenger;
      messenger.setMockMethodCallHandler(channel, (call) async {
        methods.add(call.method);
        leases.add((call.arguments as Map)['lease'] as int);
        return call.method == 'start' ? 'invalid-texture-reply' : null;
      });
      addTearDown(() {
        messenger.setMockMethodCallHandler(channel, null);
      });
      final media = NativeRemoteMedia();
      await expectLater(
        media.start(1280, 720),
        throwsA(isA<PlatformException>()),
      );
      await tester.pump();
      await tester.pump();
      expect(methods, ['start', 'thumbnailPrivacy', 'stop']);
      expect(leases.toSet(), hasLength(1));
      await media.stop();
    },
    variant: TargetPlatformVariant.only(TargetPlatform.windows),
  );
  testWidgets(
    'a rejected replacement start retires the still-current previous native lease',
    (tester) async {
      const channel = MethodChannel('multitap/remote');
      final messenger =
          TestDefaultBinaryMessengerBinding.instance.defaultBinaryMessenger;
      final calls = <(String, int)>[];
      var current = 0, first = 0;
      messenger.setMockMethodCallHandler(channel, (call) async {
        final lease = (call.arguments as Map)['lease'] as int;
        calls.add((call.method, lease));
        if (call.method == 'start') {
          if (first == 0) {
            first = current = lease;
            return 1;
          }
          throw PlatformException(code: 'memory_budget');
        }
        if (call.method == 'thumbnailPrivacy') {
          if (lease == current) calls.add(('actual-revoke', lease));
          return lease == current ? 1 : 0;
        }
        if (call.method == 'thumbnailPrivacyStatus') return lease == current;
        if (call.method == 'stop' && lease == current) current = 0;
        return null;
      });
      addTearDown(() {
        messenger.setMockMethodCallHandler(channel, null);
      });
      final media = NativeRemoteMedia();
      await media.start(1280, 720);
      await expectLater(
        media.restart(1080, 1616),
        throwsA(isA<PlatformException>()),
      );
      for (var n = 0; n < 8; n++) {
        await tester.pump();
      }
      expect(current, 0);
      final revoke = calls.indexOf(('actual-revoke', first));
      expect(revoke, greaterThanOrEqualTo(0));
      expect(calls.indexOf(('stop', first)), greaterThan(revoke));
      await media.stop();
    },
    variant: TargetPlatformVariant.only(TargetPlatform.windows),
  );
  testWidgets(
    'late L2 rejection cleans only L1/L2 and cannot stop current L3',
    (tester) async {
      const channel = MethodChannel('multitap/remote');
      final messenger =
          TestDefaultBinaryMessengerBinding.instance.defaultBinaryMessenger;
      final held = Completer<Object?>(),
          leases = <int>[],
          actualStopped = <int>[];
      var current = 0, revokes = 0;
      messenger.setMockMethodCallHandler(channel, (call) async {
        final lease = (call.arguments as Map)['lease'] as int;
        if (call.method == 'start') {
          leases.add(lease);
          if (leases.length == 2) return held.future;
          current = lease;
          return 1;
        }
        if (call.method == 'thumbnailPrivacy') {
          if (lease == current) revokes++;
          return lease == current ? 1 : 0;
        }
        if (call.method == 'thumbnailPrivacyStatus') return lease == current;
        if (call.method == 'stop' && lease == current) {
          actualStopped.add(lease);
          current = 0;
        }
        return null;
      });
      addTearDown(() {
        messenger.setMockMethodCallHandler(channel, null);
      });
      final media = NativeRemoteMedia();
      await media.start(1280, 720);
      final rejected = media.restart(1080, 1616);
      final observed = expectLater(rejected, throwsA(isA<PlatformException>()));
      await tester.pump();
      await media.restart(1600, 900);
      final successor = leases.last;
      held.completeError(PlatformException(code: 'memory_budget'));
      await observed;
      for (var n = 0; n < 8; n++) {
        await tester.pump();
      }
      expect(current, successor);
      expect(actualStopped, isEmpty);
      expect(revokes, 0);
      await media.stop();
    },
    variant: TargetPlatformVariant.only(TargetPlatform.windows),
  );
  testWidgets(
    'clearThumbnail waits for the exact durable generation receipt before returning',
    (tester) async {
      const channel = MethodChannel('multitap/remote');
      final messenger =
          TestDefaultBinaryMessengerBinding.instance.defaultBinaryMessenger;
      var statusReads = 0, done = false;
      final observed = <Map>[];
      messenger.setMockMethodCallHandler(channel, (call) async {
        if (call.method == 'start') return 1;
        if (call.method == 'thumbnailPrivacy') return 7;
        if (call.method == 'thumbnailPrivacyStatus') {
          observed.add(Map.of(call.arguments as Map));
          return ++statusReads >= 2;
        }
        return null;
      });
      addTearDown(() {
        messenger.setMockMethodCallHandler(channel, null);
      });
      final media = NativeRemoteMedia();
      await media.start(1280, 720);
      final clearing = media.clearThumbnail().then((_) {
        done = true;
      });
      await tester.pump();
      expect(done, isFalse);
      await tester.pump(const Duration(milliseconds: 20));
      await clearing;
      expect(done, isTrue);
      expect(observed, hasLength(2));
      expect(observed.map((v) => v['lease']).toSet(), hasLength(1));
      expect(observed.every((v) => v['generation'] == 7), isTrue);
      await media.stop();
    },
    variant: TargetPlatformVariant.only(TargetPlatform.windows),
  );
  testWidgets(
    'session-window notice requires an exact epoch; invalidation takes only typed VM/PC ids and reports acceptance only',
    (tester) async {
      final messenger =
          TestDefaultBinaryMessengerBinding.instance.defaultBinaryMessenger;
      final heard = <ThumbnailUpdate>[], requests = <Map>[];
      final subscription = SessionWindows.thumbnailUpdates.listen(heard.add);
      SessionWindows.listen();
      messenger.setMockMethodCallHandler(SessionWindows.channel, (call) async {
        requests.add(Map.of(call.arguments as Map));
        return true;
      });
      addTearDown(() async {
        await subscription.cancel();
        SessionWindows.channel.setMethodCallHandler(null);
        messenger.setMockMethodCallHandler(SessionWindows.channel, null);
      });
      Future<void> notice(Map<String, Object?> args) =>
          messenger.handlePlatformMessage(
            SessionWindows.channel.name,
            const StandardMethodCodec().encodeMethodCall(
              MethodCall('thumbnailUpdated', args),
            ),
            (_) {},
          );
      final args = <String, Object?>{
        'key': _key,
        'desktopId': 'pve-149',
        'generation': 2,
        'epoch': _epoch,
      };
      await notice({...args}..remove('epoch'));
      await notice({...args, 'epoch': 'not-an-epoch'});
      await notice(args);
      await tester.pump();
      expect(heard, hasLength(1));
      expect(heard.single.epoch, _epoch);
      expect(
        await SessionWindows.invalidateThumbnail(_key, 'pve-149', _epoch),
        isTrue,
      );
      // A local PC's picture is purged the same way; a malformed id never.
      expect(
        await SessionWindows.invalidateThumbnail(
          _key,
          'pc-00000000-0000-0000-0000-000000000000',
          _epoch,
        ),
        isTrue,
      );
      expect(
        await SessionWindows.invalidateThumbnail(_key, 'pc-1', _epoch),
        isFalse,
      );
      expect(
        await SessionWindows.invalidateThumbnail(
          '../escape',
          'pve-149',
          _epoch,
        ),
        isFalse,
      );
      expect(requests, [
        {'key': _key, 'desktopId': 'pve-149', 'epoch': _epoch},
        {
          'key': _key,
          'desktopId': 'pc-00000000-0000-0000-0000-000000000000',
          'epoch': _epoch,
        },
      ]);
    },
  );
}
