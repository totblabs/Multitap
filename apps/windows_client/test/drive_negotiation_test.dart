// drive-v1 negotiation on the client: the inventory flag, an exact grant
// only after a request (stream sessions only, exactly this session's drive
// socket), the connect field only toward an advertising inventory from a
// client that shares folders, the released-broker retry without it, and the
// stream session starting/closing its drive session. No relay or Host.
import 'dart:async';
import 'dart:convert';
import 'dart:io';

import 'package:flutter/services.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:multitap_client/data/broker_api.dart';
import 'package:multitap_client/data/drive_files.dart';
import 'package:multitap_client/data/drive_session.dart';
import 'package:multitap_client/data/remote_session.dart';
import 'package:multitap_client/models/stream_options.dart';

import 'drive_session_test.dart' show FakeFiles;
import 'power_api_test.dart' show ApiClient;
import 'remote_session_test.dart'
    show SessionSocket, SessionMedia, flush, info, packet;

const _origin = 'https://fixture.invalid';
const _session = 'drive-session';
Map<String, dynamic> _desktop(Map<String, Object> capabilities) => {
  'id': 'pve-149',
  'name': 'TESTER',
  'kind': 'vm',
  'description': '',
  'os': 'Windows',
  'status': 'ready',
  'canConnect': true,
  'host': {'connected': true, 'capabilities': capabilities},
};
Map<String, dynamic> _connected({
  bool granted = false,
  String mode = 'stream',
}) => {
  'sessionId': _session,
  'webSocketUrl': 'wss://fixture.invalid/api/v1/sessions/$_session',
  'mode': mode,
  if (mode == 'stream') 'options': const StreamOptions().toJson(),
  if (granted) ...{
    'driveProtocol': 'drive-v1',
    'driveWebSocketUrl':
        'wss://fixture.invalid/api/v1/sessions/$_session/drive',
  },
};

class _QuietSocket extends Fake implements WebSocket {
  final incoming = StreamController<dynamic>();
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
  void add(dynamic data) {}
  @override
  Future<void> close([int? code, String? reason]) async {
    wasClosed = true;
    unawaited(incoming.close());
  }
}

void main() {
  test('the grant is exact and only follows a request', () {
    final origin = Uri.parse(_origin);
    final granted = BrokerConnection.fromJson(
      _connected(granted: true),
      origin,
      requestedDrive: true,
    );
    expect(
      granted.driveWebSocketUrl,
      Uri.parse('wss://fixture.invalid/api/v1/sessions/$_session/drive'),
    );
    expect(
      BrokerConnection.fromJson(_connected(), origin).driveWebSocketUrl,
      isNull,
    );
    expect(
      BrokerConnection.fromJson(
        _connected(),
        origin,
        requestedDrive: true,
      ).driveWebSocketUrl,
      isNull,
      reason:
          'requested but not granted (released Host or not allowed): no drives',
    );
    for (final (data, requested) in [
      (_connected(granted: true), false),
      ({..._connected(granted: true), 'driveProtocol': 'drive-v2'}, true),
      ({..._connected(), 'driveProtocol': 'drive-v1'}, true),
      (
        {
          ..._connected(),
          'driveWebSocketUrl':
              'wss://fixture.invalid/api/v1/sessions/$_session/drive',
        },
        true,
      ),
      (
        {
          ..._connected(granted: true),
          'driveWebSocketUrl':
              'wss://other.invalid/api/v1/sessions/$_session/drive',
        },
        true,
      ),
      (
        {
          ..._connected(granted: true),
          'driveWebSocketUrl':
              'wss://fixture.invalid:8443/api/v1/sessions/$_session/drive',
        },
        true,
      ),
      (
        {
          ..._connected(granted: true),
          'driveWebSocketUrl':
              'wss://fixture.invalid/api/v1/sessions/other/drive',
        },
        true,
      ),
      (
        {
          ..._connected(granted: true),
          'driveWebSocketUrl':
              'wss://fixture.invalid/api/v1/sessions/$_session/drive?x=1',
        },
        true,
      ),
      (
        {
          ..._connected(granted: true),
          'driveWebSocketUrl':
              'ws://fixture.invalid/api/v1/sessions/$_session/drive',
        },
        true,
      ),
      ({..._connected(granted: true), 'driveWebSocketUrl': 42}, true),
    ]) {
      expect(
        () =>
            BrokerConnection.fromJson(data, origin, requestedDrive: requested),
        throwsA(isA<BrokerException>()),
        reason: '$data',
      );
    }
    expect(
      BrokerDesktop.fromJson(_desktop({'driveProtocol': 'drive-v1'})).drive,
      isTrue,
    );
    expect(BrokerDesktop.fromJson(_desktop({})).drive, isFalse);
    expect(
      BrokerDesktop.fromJson(_desktop({'driveProtocol': 'drive-v2'})).drive,
      isFalse,
    );
  });

  test('HTTP: requested only by a folder-sharing client from an advertising inventory', () async {
    Future<(Map, BrokerConnection)> run(
      Map<String, Object> caps,
      Map<String, dynamic> reply, {
      bool requestDrives = true,
      String mode = 'stream',
    }) async {
      final client = ApiClient()
        ..responses.add((
          200,
          {
            'desktops': [_desktop(caps)],
          },
        ))
        ..responses.add((200, reply));
      final api = HttpBrokerApi(
        origin: Uri.parse(_origin),
        client: client,
        requestDrives: requestDrives,
      );
      await api.desktops('fixture');
      final connection = mode == 'stream'
          ? await api.connect('fixture', 'pve-149')
          : await api.connect('fixture', 'pve-149', mode: mode);
      return (
        jsonDecode(client.requests.last.body.toString()) as Map,
        connection,
      );
    }

    const released = {'service': true, 'streaming': true};
    final legacy = await run(released, _connected());
    expect(legacy.$1.containsKey('driveProtocol'), isFalse);
    expect(legacy.$2.driveWebSocketUrl, isNull);
    const capable = {...released, 'driveProtocol': 'drive-v1'};
    final drive = await run(capable, _connected(granted: true));
    expect(drive.$1['driveProtocol'], 'drive-v1');
    expect(drive.$2.driveWebSocketUrl, isNotNull);
    // A client that cannot share folders (the Android app) never asks.
    final android = await run(capable, _connected(), requestDrives: false);
    expect(android.$1.containsKey('driveProtocol'), isFalse);
    // A released broker's preflight refusal: retried without the field.
    final client = ApiClient()
      ..responses.add((
        200,
        {
          'desktops': [_desktop(capable)],
        },
      ))
      ..responses.add((
        400,
        {
          'error': {'code': 'unexpected_field'},
        },
      ))
      ..responses.add((200, _connected()));
    final api = HttpBrokerApi(
      origin: Uri.parse(_origin),
      client: client,
      requestDrives: true,
    );
    await api.desktops('fixture');
    final retried = await api.connect('fixture', 'pve-149');
    final body = jsonDecode(client.requests.last.body.toString()) as Map;
    expect(body.containsKey('driveProtocol'), isFalse);
    expect(retried.driveWebSocketUrl, isNull);
  });

  test('driveFs: inventory flag, exact grant, and the released-broker retries', () async {
    final origin = Uri.parse(_origin);
    // Inventory: the flag is read next to driveProtocol.
    expect(
      BrokerDesktop.fromJson(
        _desktop({'driveProtocol': 'drive-v1', 'driveFs': true}),
      ).driveFs,
      isTrue,
    );
    expect(
      BrokerDesktop.fromJson(_desktop({'driveProtocol': 'drive-v1'})).driveFs,
      isFalse,
    );
    expect(
      BrokerDesktop.fromJson(
        _desktop({'driveProtocol': 'drive-v1', 'driveFs': 'yes'}),
      ).driveFs,
      isFalse,
    );
    // Response: granted only with a request and a drive grant.
    Map<String, dynamic> withFs([Object? value = true]) => {
      ..._connected(granted: true),
      'driveFs': value,
    };
    final granted = BrokerConnection.fromJson(
      withFs(),
      origin,
      requestedDrive: true,
      requestedDriveFs: true,
    );
    expect(granted.driveFs, isTrue);
    expect(
      BrokerConnection.fromJson(
        _connected(granted: true),
        origin,
        requestedDrive: true,
        requestedDriveFs: true,
      ).driveFs,
      isFalse,
      reason: 'a Host without fs answers drive-v1 only',
    );
    expect(BrokerConnection.fromJson(_connected(), origin).driveFs, isFalse);
    for (final (data, drive, fsAsked) in [
      (withFs(), true, false), // never asked
      (withFs(), false, true), // no drive requested
      (withFs(false), true, true),
      (withFs('true'), true, true),
      ({..._connected(), 'driveFs': true}, true, true), // no drive grant
    ]) {
      expect(
        () => BrokerConnection.fromJson(
          data,
          origin,
          requestedDrive: drive,
          requestedDriveFs: fsAsked,
        ),
        throwsA(isA<BrokerException>()),
        reason: '$data',
      );
    }

    const fsCaps = {
      'service': true,
      'streaming': true,
      'driveProtocol': 'drive-v1',
      'driveFs': true,
    };
    Future<(HttpBrokerApi, ApiClient)> api(
      List<(int, Map<String, dynamic>)> replies, {
      Map<String, Object> caps = fsCaps,
    }) async {
      final client = ApiClient()
        ..responses.add((
          200,
          {
            'desktops': [_desktop(caps)],
          },
        ));
      replies.forEach(client.responses.add);
      final broker = HttpBrokerApi(
        origin: origin,
        client: client,
        requestDrives: true,
      );
      await broker.desktops('fixture');
      return (broker, client);
    }

    Map body(ApiClient client, int index) =>
        jsonDecode(client.requests[index].body.toString()) as Map;
    const refusal = (
      400,
      {
        'error': {'code': 'unexpected_field'},
      },
    );

    // Advertised and granted.
    var (broker, client) = await api([(200, withFs())]);
    final fs = await broker.connect('fixture', 'pve-149');
    expect(body(client, 1)['driveProtocol'], 'drive-v1');
    expect(body(client, 1)['driveFs'], isTrue);
    expect(fs.driveFs, isTrue);
    // Not advertised: never sent.
    (broker, client) = await api([
      (200, _connected(granted: true)),
    ], caps: const {'service': true, 'driveProtocol': 'drive-v1'});
    final plain = await broker.connect('fixture', 'pve-149');
    expect(body(client, 1).containsKey('driveFs'), isFalse);
    expect(plain.driveFs, isFalse);
    // A broker that knows drive-v1 but refuses driveFs: one retry without it
    // that keeps the drive grant.
    (broker, client) = await api([refusal, (200, _connected(granted: true))]);
    final kept = await broker.connect('fixture', 'pve-149');
    expect(client.requests, hasLength(3));
    expect(body(client, 2).containsKey('driveFs'), isFalse);
    expect(body(client, 2)['driveProtocol'], 'drive-v1');
    expect(kept.driveWebSocketUrl, isNotNull);
    expect(kept.driveFs, isFalse);
    // An older broker refuses driveProtocol too: the existing full fallback.
    (broker, client) = await api([refusal, refusal, (200, _connected())]);
    final bare = await broker.connect('fixture', 'pve-149');
    expect(client.requests, hasLength(4));
    expect(body(client, 3).containsKey('driveProtocol'), isFalse);
    expect(body(client, 3).containsKey('driveFs'), isFalse);
    expect(bare.driveWebSocketUrl, isNull);
    // Any other failure on the second try is that failure.
    (broker, client) = await api([
      refusal,
      (
        403,
        {
          'error': {'code': 'forbidden'},
        },
      ),
    ]);
    await expectLater(
      broker.connect('fixture', 'pve-149'),
      throwsA(isA<BrokerException>()),
    );
    expect(client.requests, hasLength(3));
  });

  test('the stream session starts its drive session with the first picture and closes it', () async {
    final socket = SessionSocket(),
        media = SessionMedia(),
        driveSocket = _QuietSocket();
    final created = <DriveSession>[];
    final session = RemoteSession(
      connection: BrokerConnection.fromJson(
        _connected(granted: true),
        Uri.parse(_origin),
        requestedDrive: true,
      ),
      token: 'not-persisted',
      media: media,
      dialer: (_, _) async => socket,
      driveFactory: (connection, token) {
        final drive = DriveSession(
          url: connection.driveWebSocketUrl!,
          token: token,
          sessionId: connection.sessionId,
          files: FakeFiles(),
          dialer: (_, _) async => driveSocket,
        );
        created.add(drive);
        return drive;
      },
    );
    unawaited(session.start());
    await flush();
    expect(created, isEmpty, reason: 'nothing before the first picture');
    socket.incoming.add(jsonEncode(info));
    await flush();
    socket.incoming.add(packet());
    await flush();
    await session.capture(true);
    await flush();
    expect(created, hasLength(1));
    expect(session.drive, same(created.single));
    await session.close();
    expect(created.single.closed, isTrue);
    expect(created.single.error, 'drive_session_closed');
    expect(driveSocket.wasClosed, isTrue);
  });

  test('an ungranted session never builds a drive session', () async {
    final socket = SessionSocket(), media = SessionMedia();
    var built = 0;
    final session = RemoteSession(
      connection: BrokerConnection.fromJson(
        _connected(),
        Uri.parse(_origin),
        requestedDrive: true,
      ),
      token: 'not-persisted',
      media: media,
      dialer: (_, _) async => socket,
      driveFactory: (connection, token) {
        built++;
        throw StateError('not granted');
      },
    );
    unawaited(session.start());
    await flush();
    socket.incoming.add(jsonEncode(info));
    await flush();
    socket.incoming.add(packet());
    await flush();
    await session.capture(true);
    await flush();
    expect(built, 0);
    expect(session.drive, isNull);
    await session.close();
  });

  test(
    'the native channel: exact arguments, fixed failure codes only',
    () async {
      TestWidgetsFlutterBinding.ensureInitialized();
      const channel = MethodChannel('multitap/drive');
      final calls = <MethodCall>[];
      TestDefaultBinaryMessengerBinding.instance.defaultBinaryMessenger
          .setMockMethodCallHandler(channel, (call) async {
            calls.add(call);
            final args = call.arguments as Map;
            switch (call.method) {
              case 'pickFolder':
                return {'path': r'C:\Data', 'name': 'Data'};
              case 'open':
                if (args['path'] == r'C:\Denied') {
                  throw PlatformException(code: 'drive_user_unavailable');
                }
                return {'name': 'Data'};
              case 'stat':
                if (args['path'] == 'odd') {
                  throw PlatformException(code: 'C:\\leak');
                }
                return {
                  'name': '',
                  'dir': true,
                  'size': 0,
                  'modified': 1,
                  'created': 2,
                  'readOnly': false,
                  'free': 3,
                  'total': 4,
                };
              case 'list':
                return {
                  'entries': [
                    {
                      'name': 'a',
                      'dir': false,
                      'size': 5,
                      'modified': 1,
                      'created': 1,
                      'readOnly': true,
                    },
                  ],
                };
              case 'read':
                return Uint8List.fromList([1, 2]);
              default:
                return null;
            }
          });
      addTearDown(
        () => TestDefaultBinaryMessengerBinding.instance.defaultBinaryMessenger
            .setMockMethodCallHandler(channel, null),
      );
      final files = NativeDriveFileSystem();
      expect((await files.pickFolder())!.path, r'C:\Data');
      expect(await files.open('d1', r'C:\Data', readOnly: true), 'Data');
      expect(calls.last.arguments, {
        'share': 'd1',
        'path': r'C:\Data',
        'readOnly': true,
      });
      await expectLater(
        files.open('d2', r'C:\Denied', readOnly: false),
        throwsA(
          isA<DriveFileException>().having(
            (e) => e.code,
            'code',
            'drive_user_unavailable',
          ),
        ),
      );
      final root = await files.stat('d1', '');
      expect((root.dir, root.free, root.total), (true, 3, 4));
      await expectLater(
        files.stat('d1', 'odd'),
        throwsA(
          isA<DriveFileException>().having(
            (e) => e.code,
            'code',
            'drive_io_failed',
          ),
        ),
      );
      expect((await files.list('d1', '')).single.readOnly, isTrue);
      expect(await files.read('d1', 'a', 0, 2), [1, 2]);
      await expectLater(
        files.read('d1', 'a', 0, 1),
        throwsA(isA<DriveFileException>()),
        reason: 'more than asked for',
      );
      await files.write('d1', 'a', 7, Uint8List.fromList([9]), truncate: false);
      expect(calls.last.arguments, {
        'share': 'd1',
        'path': 'a',
        'offset': 7,
        'data': Uint8List.fromList([9]),
        'truncate': false,
      });
      await files.rename('d1', 'a', 'b', replace: true);
      expect(calls.last.arguments, {
        'share': 'd1',
        'path': 'a',
        'to': 'b',
        'replace': true,
      });
    },
  );
}
