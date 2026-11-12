// Negotiated cursor separation (cursor-v1): the exact HTTP request/response
// gating, which binary kind-3 records a session accepts and hands to the native
// presenter (epoch, key-frame and legacy rules), native failure handling, the
// picture geometry the view reports, and the statistics row. Injected HTTP,
// socket and media; no network, VM, native presenter or real cursor.
import 'dart:async';
import 'dart:convert';

import 'package:flutter/gestures.dart';
import 'package:flutter/material.dart';
import 'package:flutter/services.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:multitap_client/data/broker_api.dart';
import 'package:multitap_client/data/remote_cursor.dart';
import 'package:multitap_client/data/remote_session.dart';
import 'package:multitap_client/data/video_geometry.dart';
import 'package:multitap_client/models/stream_options.dart';
import 'package:multitap_client/ui/remote_view.dart';
import 'package:multitap_client/ui/stream_session_stats.dart';

import 'display_remote_test.dart' show machine;
import 'power_api_test.dart' show ApiClient;
import 'remote_session_test.dart' show SessionSocket, SessionMedia, flush;
import 'viewport_session_test.dart' show startInfo;

const _origin = 'https://fixture.invalid';

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
Map<String, dynamic> _connected({bool granted = false}) => {
  'sessionId': 'cursor-session',
  'webSocketUrl': 'wss://fixture.invalid/api/v1/sessions/cursor-session',
  'mode': 'stream',
  'options': const StreamOptions().toJson(),
  if (granted) 'cursorProtocol': 'cursor-v1',
};

Uint8List _video({bool key = false, int epoch = 0}) => Uint8List.fromList([
  1,
  key ? 1 : 0,
  epoch & 255,
  epoch >> 8,
  ...List.filled(8, 0),
  0, 0, 0, 1, key ? 0x65 : 0x41, //
]);
void _put16(List<int> b, int at, int v) => b
  ..[at] = v & 255
  ..[at + 1] = v >> 8;
void _put32(List<int> b, int at, int v) {
  for (var i = 0; i < 4; i++) {
    b[at + i] = (v >> (8 * i)) & 255;
  }
}

Uint8List shapeRecord(
  int id, {
  int format = 1,
  int width = 32,
  int height = 32,
}) {
  final stride = ((width + 31) ~/ 32) * 4;
  final mask = stride * height * (format == 2 ? 2 : 1);
  final color = format == 1 ? width * height * 4 : 0;
  final b = List<int>.filled(16 + mask + color, 0);
  b[0] = 1;
  b[1] = format;
  _put32(b, 4, id);
  _put16(b, 8, width);
  _put16(b, 10, height);
  return Uint8List.fromList(b);
}

Uint8List stateRecord(
  bool visible,
  int id, {
  bool position = false,
  int x = 0,
  int y = 0,
}) {
  final b = List<int>.filled(16, 0);
  b[0] = 2;
  b[1] = (visible ? 1 : 0) | (position ? 2 : 0);
  _put32(b, 4, id);
  _put32(b, 8, x);
  _put32(b, 12, y);
  return Uint8List.fromList(b);
}

Uint8List cursorPacket(Uint8List record, {int epoch = 0, int flags = 0}) =>
    Uint8List.fromList([
      cursorMediaKind,
      flags,
      epoch & 255,
      epoch >> 8,
      ...List.filled(8, 0),
      ...record,
    ]);

class CursorMedia extends SessionMedia implements RemoteCursorMedia {
  final records = <(int, Uint8List)>[];
  final views = <CursorView>[];
  Object? cursorFailure, viewFailure;
  // Any decoder size (resizes); a new texture per start, like the runner.
  @override
  Future<int> start(int width, int height) async => 40 + ++starts;
  @override
  Future<void> cursor(int epoch, Uint8List record) async {
    if (cursorFailure != null) throw cursorFailure!;
    records.add((epoch, Uint8List.fromList(record)));
  }

  @override
  Future<void> cursorView(CursorView view) async {
    if (viewFailure != null) throw viewFailure!;
    views.add(view);
  }
}

class CursorRetryMedia extends CursorMedia {
  final attempts = <(int, Uint8List)>[];
  Future<void> Function(int, Uint8List)? stateReply;

  @override
  Future<void> cursor(int epoch, Uint8List record) async {
    attempts.add((epoch, Uint8List.fromList(record)));
    if (record[0] == cursorStateRecord) await stateReply?.call(epoch, record);
    await super.cursor(epoch, record);
  }
}

BrokerConnection _connection({
  bool cursor = true,
  bool epochMedia = false,
  bool cursorEcho = false,
}) => BrokerConnection(
  sessionId: 'cursor-session',
  webSocketUrl: Uri.parse(
    'wss://fixture.invalid/api/v1/sessions/cursor-session',
  ),
  cursor: cursor,
  epochMedia: epochMedia,
  viewportProvisioning: epochMedia,
  relativeMouse: cursorEcho,
  cursorEcho: cursorEcho,
);

Future<(RemoteSession, SessionSocket, CursorMedia)> _started({
  bool cursor = true,
  bool keyFrame = true,
  bool epochMedia = false,
  bool cursorEcho = false,
  CursorMedia? cursorMedia,
}) async {
  final socket = SessionSocket(), media = cursorMedia ?? CursorMedia();
  final session = RemoteSession(
    connection: _connection(
      cursor: cursor,
      epochMedia: epochMedia,
      cursorEcho: cursorEcho,
    ),
    token: 'fixture',
    media: media,
    dialer: (uri, token) async => socket,
  );
  unawaited(session.start());
  await flush();
  socket.incoming.add(
    jsonEncode(
      epochMedia
          ? startInfo
          : {
              'type': 'stream.info',
              'codec': 'h264',
              'width': 1280,
              'height': 720,
            },
    ),
  );
  await flush();
  if (keyFrame) {
    socket.incoming.add(_video(key: true));
    await flush();
  }
  return (session, socket, media);
}

// Thin baseline adapter: Client102 has no readiness getter and always hides
// the local pointer while captured. The same assertions run against the fix.
bool _localCursorFallback(RemoteSession session) {
  try {
    return (session as dynamic).localCursorFallback as bool;
  } on NoSuchMethodError {
    return false;
  }
}

void main() {
  test('Cursor view sources above16K retain the shared signed source ABI', () {
    for (final (width, height) in <(int, int)>[
      (32768, 2160),
      (2160, 32768),
      (maximumGeometrySide, 180),
      (320, maximumGeometrySide),
    ]) {
      final view = CursorView(
        left: 0,
        top: 0,
        width: 1280,
        height: 720,
        sourceWidth: width,
        sourceHeight: height,
      );
      expect(view.valid, isTrue);
      expect(view.toJson()['sourceWidth'], width);
      expect(view.toJson()['sourceHeight'], height);
    }
  });

  test(
    'Cursor view source ABI rejects zero, negative and overflow dimensions',
    () {
      for (final (width, height) in <(int, int)>[
        (0, 180),
        (-1, 180),
        (320, 0),
        (320, -1),
        (maximumGeometrySide + 1, 180),
        (320, maximumGeometrySide + 1),
        (0x100000000, 180),
      ]) {
        expect(
          CursorView(
            left: 0,
            top: 0,
            width: 1280,
            height: 720,
            sourceWidth: width,
            sourceHeight: height,
          ).valid,
          isFalse,
        );
      }
    },
  );

  test(
    'cursor ownership: no state and shape-only keep a local fallback',
    () async {
      final (session, socket, _) = await _started();
      final before = _localCursorFallback(session);
      socket.incoming.add(cursorPacket(shapeRecord(1)));
      await flush();
      final shapeOnly = _localCursorFallback(session);
      await session.close();
      expect(before, isTrue);
      expect(shapeOnly, isTrue);
    },
  );

  test(
    'cursor ownership: successful visible and explicit hidden are ready',
    () async {
      final (session, socket, _) = await _started();
      socket.incoming.add(cursorPacket(shapeRecord(1)));
      socket.incoming.add(cursorPacket(stateRecord(true, 1)));
      await flush();
      final visible = _localCursorFallback(session);
      socket.incoming.add(cursorPacket(stateRecord(false, 0)));
      await flush();
      final hidden = _localCursorFallback(session);
      await session.close();
      expect(visible, isFalse);
      expect(hidden, isFalse);
    },
  );

  test(
    'cursor ownership: presenter failure falls back and valid state recovers',
    () async {
      final (session, socket, media) = await _started();
      socket.incoming.add(cursorPacket(shapeRecord(1)));
      socket.incoming.add(cursorPacket(stateRecord(true, 1)));
      await flush();
      media.cursorFailure = PlatformException(code: 'invalid_cursor');
      socket.incoming.add(cursorPacket(stateRecord(true, 1)));
      await flush();
      final failed = _localCursorFallback(session);
      final stayedOpen = !session.closed;
      media.cursorFailure = null;
      socket.incoming.add(cursorPacket(stateRecord(true, 1)));
      await flush();
      final recovered = _localCursorFallback(session);
      await session.close();
      expect(failed, isTrue);
      expect(stayedOpen, isTrue);
      expect(recovered, isFalse);
    },
  );

  test('cursor retry: one failed native state recovers without a new Host record and waits for ACK', () async {
    final media = CursorRetryMedia();
    final secondAck = Completer<void>();
    var stateAttempts = 0;
    media.stateReply = (_, _) async {
      if (++stateAttempts == 1) {
        throw PlatformException(code: 'invalid_cursor');
      }
      await secondAck.future;
    };
    final (session, socket, _) = await _started(cursorMedia: media);
    try {
      socket.incoming.add(cursorPacket(shapeRecord(1)));
      socket.incoming.add(cursorPacket(stateRecord(true, 1)));
      await flush();
      expect(session.localCursorFallback, isTrue);
      await Future<void>.delayed(const Duration(milliseconds: 160));
      expect(stateAttempts, 2, reason: 'Host still uses the same shape');
      expect(session.localCursorFallback, isTrue, reason: 'native ACK pending');
      secondAck.complete();
      await flush();
      expect(session.localCursorFallback, isFalse);
      expect(
        media.attempts.where((e) => e.$2[0] == cursorShapeRecord),
        hasLength(1),
      );
      expect(
        media.attempts.where((e) => e.$2[0] == cursorStateRecord),
        hasLength(2),
      );
    } finally {
      if (!secondAck.isCompleted) secondAck.complete();
      await session.close();
    }
  });

  test('cursor retry: explicit hidden state recovers without inventing a visible cursor', () async {
    final media = CursorRetryMedia();
    var attempts = 0;
    media.stateReply = (_, _) async {
      if (++attempts == 1) throw PlatformException(code: 'invalid_cursor');
    };
    final (session, socket, _) = await _started(cursorMedia: media);
    try {
      socket.incoming.add(cursorPacket(stateRecord(false, 0)));
      await flush();
      expect(session.localCursorFallback, isTrue);
      expect(session.remoteCursorHidden, isFalse);
      await Future<void>.delayed(const Duration(milliseconds: 160));
      expect(attempts, 2);
      expect(session.localCursorFallback, isFalse);
      expect(session.remoteCursorHidden, isTrue);
      expect(media.records.single.$2, stateRecord(false, 0));
    } finally {
      await session.close();
    }
  });

  test('cursor retry: bounded failures keep the fallback without replaying shape IDs', () async {
    final media = CursorRetryMedia();
    media.stateReply = (_, _) async =>
        throw PlatformException(code: 'invalid_cursor');
    final (session, socket, _) = await _started(cursorMedia: media);
    try {
      socket.incoming.add(cursorPacket(shapeRecord(1)));
      socket.incoming.add(cursorPacket(stateRecord(true, 1)));
      await Future<void>.delayed(const Duration(milliseconds: 1100));
      final attempts = media.attempts.length;
      expect(
        attempts,
        5,
        reason: 'one shape, one state, at most three retries',
      );
      await Future<void>.delayed(const Duration(milliseconds: 600));
      expect(media.attempts, hasLength(attempts));
      expect(session.localCursorFallback, isTrue);
      expect(session.closed, isFalse);
    } finally {
      await session.close();
    }
  });

  test(
    'cursor retry: state replay removes old cursor warp and echo coordinates',
    () async {
      final media = CursorRetryMedia();
      var attempts = 0;
      media.stateReply = (_, _) async {
        if (++attempts == 1) throw PlatformException(code: 'invalid_cursor');
      };
      final (session, socket, _) = await _started(cursorMedia: media);
      final original = stateRecord(true, 1, position: true, x: 50, y: 60);
      try {
        socket.incoming.add(cursorPacket(shapeRecord(1)));
        socket.incoming.add(cursorPacket(original));
        await Future<void>.delayed(const Duration(milliseconds: 160));
        final states = media.attempts
            .where((e) => e.$2[0] == cursorStateRecord)
            .toList();
        expect(states, hasLength(2));
        expect(states.first.$2, original);
        expect(states.last.$2, stateRecord(true, 1));
        expect(original[1], 3, reason: 'wire message is not mutated');
      } finally {
        await session.close();
      }
    },
  );

  test(
    'cursor retry: a newer hidden state cancels the old visible retry',
    () async {
      final media = CursorRetryMedia();
      media.stateReply = (_, record) async {
        if (record[1] & 1 != 0) throw PlatformException(code: 'invalid_cursor');
      };
      final (session, socket, _) = await _started(cursorMedia: media);
      try {
        socket.incoming.add(cursorPacket(shapeRecord(1)));
        socket.incoming.add(cursorPacket(stateRecord(true, 1)));
        await flush();
        socket.incoming.add(cursorPacket(stateRecord(false, 0)));
        await Future<void>.delayed(const Duration(milliseconds: 180));
        expect(media.attempts, hasLength(3));
        expect(session.remoteCursorHidden, isTrue);
        expect(session.localCursorFallback, isFalse);
      } finally {
        await session.close();
      }
    },
  );

  test('cursor retry: close cancels a pending native retry', () async {
    final media = CursorRetryMedia();
    media.stateReply = (_, _) async =>
        throw PlatformException(code: 'invalid_cursor');
    final (session, socket, _) = await _started(cursorMedia: media);
    socket.incoming.add(cursorPacket(stateRecord(false, 0)));
    await flush();
    await session.close();
    await Future<void>.delayed(const Duration(milliseconds: 180));
    expect(media.attempts, hasLength(1));
  });

  test(
    'cursor retry: a replaced native owner still ends the old session',
    () async {
      final media = CursorRetryMedia();
      var attempts = 0;
      media.stateReply = (_, _) async => throw PlatformException(
        code: ++attempts == 1 ? 'invalid_cursor' : 'session_replaced',
      );
      final (session, socket, _) = await _started(cursorMedia: media);
      socket.incoming.add(cursorPacket(stateRecord(false, 0)));
      await Future<void>.delayed(const Duration(milliseconds: 180));
      expect(attempts, 2);
      expect(session.closed, isTrue);
      expect(session.diagnosticCode, 'session_replaced');
      await session.close();
    },
  );

  test(
    'cursor retry: a pending resize cancels retry before the Host responds',
    () async {
      final media = CursorRetryMedia();
      media.stateReply = (_, _) async =>
          throw PlatformException(code: 'invalid_cursor');
      final (session, socket, _) = await _started(
        cursorMedia: media,
        epochMedia: true,
      );
      Future<bool>? resizing;
      try {
        socket.incoming.add(cursorPacket(stateRecord(false, 0)));
        await flush();
        resizing = session.resizeViewport(1346, 812);
        await flush();
        expect(session.resizing, isTrue);
        await Future<void>.delayed(const Duration(milliseconds: 180));
        expect(media.attempts, hasLength(1));
      } finally {
        await session.close();
        await resizing;
      }
    },
  );

  for (final oldFails in [false, true]) {
    test(
      'cursor retry: pending retry ${oldFails ? 'failure' : 'ACK'} cannot poison the new decoder texture',
      () async {
        final media = CursorRetryMedia(), oldReply = Completer<void>();
        var oldAttempts = 0;
        media.stateReply = (epoch, record) async {
          if (epoch == 0 && record[1] & 1 != 0) {
            if (++oldAttempts == 1) {
              throw PlatformException(code: 'invalid_cursor');
            }
            await oldReply.future;
          }
        };
        final (session, socket, _) = await _started(
          cursorMedia: media,
          epochMedia: true,
        );
        Future<bool>? resizing;
        try {
          socket.incoming.add(cursorPacket(shapeRecord(1)));
          socket.incoming.add(cursorPacket(stateRecord(true, 1)));
          await Future<void>.delayed(const Duration(milliseconds: 160));
          final oldTexture = session.textureId;
          expect(oldAttempts, 2);
          expect(session.localCursorFallback, isTrue);
          resizing = session.resizeViewport(1346, 812);
          await flush();
          final id =
              socket.sent.lastWhere(
                    (v) => v['type'] == 'stream.resize',
                  )['requestId']
                  as String;
          socket.incoming.add(
            jsonEncode({
              'type': 'stream.resizing',
              'requestId': id,
              'resizeId': 'cursor-retry-resize',
              'epoch': 1,
              'width': 1346,
              'height': 812,
              'fps': 60,
              'monitor': '0',
              'timeoutMs': 90000,
            }),
          );
          await flush();
          socket.incoming.add(
            jsonEncode({
              'type': 'stream.info',
              'codec': 'h264',
              'sessionId': 'cursor-session',
              'requestId': id,
              'resizeId': 'cursor-retry-resize',
              'epoch': 1,
              'width': 1346,
              'height': 812,
              'sourceWidth': 1346,
              'sourceHeight': 812,
              'fpsTarget': 60,
            }),
          );
          await flush();
          socket.incoming.add(_video(key: true, epoch: 1));
          socket.incoming.add(cursorPacket(stateRecord(false, 0), epoch: 1));
          await flush();
          expect(session.textureId, isNot(oldTexture));
          expect(session.localCursorFallback, isFalse);
          expect(session.remoteCursorHidden, isTrue);
          if (oldFails) {
            oldReply.completeError(PlatformException(code: 'invalid_cursor'));
          } else {
            oldReply.complete();
          }
          await Future<void>.delayed(const Duration(milliseconds: 180));
          expect(session.localCursorFallback, isFalse);
          expect(session.remoteCursorHidden, isTrue);
          expect(
            media.attempts.where(
              (e) => e.$1 == 0 && e.$2[0] == cursorStateRecord,
            ),
            hasLength(2),
          );
        } finally {
          if (!oldReply.isCompleted) oldReply.complete();
          await session.close();
          await resizing;
        }
      },
    );
  }

  for (final oldFails in [false, true]) {
    test(
      'cursor retry: late ${oldFails ? 'failure' : 'ACK'} cannot replace a newer hidden state',
      () async {
        final media = CursorRetryMedia(), oldReply = Completer<void>();
        media.stateReply = (_, record) async {
          if (record[1] & 2 != 0) await oldReply.future;
        };
        final (session, socket, _) = await _started(
          cursorMedia: media,
          cursorEcho: true,
        );
        try {
          socket.incoming.add(cursorPacket(shapeRecord(1)));
          socket.incoming.add(cursorPacket(stateRecord(true, 1)));
          await flush();
          expect(session.echoCursor(true), isTrue);
          socket.incoming.add(
            cursorPacket(stateRecord(true, 1, position: true, x: 10, y: 20)),
          );
          await flush();
          socket.incoming.add(cursorPacket(stateRecord(false, 0)));
          await flush();
          expect(session.remoteCursorHidden, isTrue);
          if (oldFails) {
            oldReply.completeError(PlatformException(code: 'invalid_cursor'));
          } else {
            oldReply.complete();
          }
          await Future<void>.delayed(const Duration(milliseconds: 180));
          expect(session.remoteCursorHidden, isTrue);
          expect(session.localCursorFallback, isFalse);
          expect(
            media.attempts,
            hasLength(4),
            reason: 'stale result schedules no retry',
          );
        } finally {
          if (!oldReply.isCompleted) oldReply.complete();
          await session.close();
        }
      },
    );
  }

  test(
    'cursor ownership: legacy baked cursor does not gain a second pointer',
    () async {
      final (session, _, _) = await _started(cursor: false);
      final fallback = _localCursorFallback(session);
      await session.close();
      expect(fallback, isFalse);
    },
  );

  test(
    'Record bounds: exact shape and state sizes, types and reserved bytes',
    () {
      expect(validCursorRecord(shapeRecord(1)), isTrue);
      expect(
        validCursorRecord(shapeRecord(2, format: 2, width: 33, height: 7)),
        isTrue,
      );
      expect(
        validCursorRecord(shapeRecord(3, width: 256, height: 256)),
        isTrue,
      );
      expect(
        validCursorRecord(stateRecord(true, 1, position: true, x: 5, y: 6)),
        isTrue,
      );
      expect(validCursorRecord(stateRecord(false, 0)), isTrue);
      final bad = <Uint8List>[
        Uint8List(15),
        Uint8List.fromList([...stateRecord(true, 1), 0]),
        Uint8List.fromList([...shapeRecord(1)]..removeLast()),
        Uint8List.fromList([...shapeRecord(1), 0]),
        Uint8List.fromList(stateRecord(true, 1)..[1] = 4),
        Uint8List.fromList(stateRecord(true, 1)..[2] = 1),
        Uint8List.fromList(shapeRecord(1)..[0] = 3),
        Uint8List.fromList(shapeRecord(1)..[1] = 3),
        Uint8List.fromList(shapeRecord(1)..[12] = 32),
        shapeRecord(1, width: 257, height: 1),
        Uint8List.fromList(shapeRecord(1)..[1] = 2),
      ];
      for (final record in bad) {
        expect(validCursorRecord(record), isFalse, reason: '$record');
      }
    },
  );

  test('HTTP: requested only from an advertising inventory, dropped on the released fallback', () async {
    Future<(List<Map>, BrokerConnection)> run(
      Map<String, Object> caps,
      List<(int, Map<String, dynamic>)> replies,
    ) async {
      final client = ApiClient()
        ..responses.add((
          200,
          {
            'desktops': [_desktop(caps)],
          },
        ))
        ..responses.addAll(replies);
      final api = HttpBrokerApi(origin: Uri.parse(_origin), client: client);
      await api.desktops('fixture');
      final connection = await api.connect('fixture', 'pve-149');
      return (
        [
          for (final request in client.requests.skip(1))
            jsonDecode(request.body.toString()) as Map,
        ],
        connection,
      );
    }

    const released = {'service': true, 'streaming': true};
    final legacy = await run(released, [(200, _connected())]);
    expect(legacy.$1.single.containsKey('cursorProtocol'), isFalse);
    expect(legacy.$2.cursor, isFalse);
    final separated = await run(
      {...released, 'cursorProtocol': 'cursor-v1'},
      [(200, _connected(granted: true))],
    );
    expect(separated.$1.single['cursorProtocol'], 'cursor-v1');
    expect(separated.$2.cursor, isTrue);
    // Requested but not granted (host replaced, tracks): composed cursor.
    final ungranted = await run(
      {...released, 'cursorProtocol': 'cursor-v1'},
      [(200, _connected())],
    );
    expect(ungranted.$2.cursor, isFalse);
    final malformed = await run(
      {...released, 'cursorProtocol': 'cursor-v2'},
      [(200, _connected())],
    );
    expect(malformed.$1.single.containsKey('cursorProtocol'), isFalse);
    // A released broker rejecting the unknown field: one retry without it.
    final fallback = await run(
      {...released, 'cursorProtocol': 'cursor-v1'},
      [
        (
          400,
          {
            'error': {'code': 'unexpected_field'},
          },
        ),
        (200, _connected()),
      ],
    );
    expect(fallback.$1, hasLength(2));
    expect(fallback.$1.first['cursorProtocol'], 'cursor-v1');
    expect(fallback.$1.last.containsKey('cursorProtocol'), isFalse);
    expect(fallback.$2.cursor, isFalse);
  });

  test('Response: a grant is accepted only when requested and exact', () {
    final origin = Uri.parse(_origin);
    expect(
      () => BrokerConnection.fromJson(_connected(granted: true), origin),
      throwsA(isA<BrokerException>()),
      reason: 'never requested',
    );
    expect(
      () => BrokerConnection.fromJson(
        {..._connected(), 'cursorProtocol': 'cursor-v2'},
        origin,
        requestedCursor: true,
      ),
      throwsA(isA<BrokerException>()),
    );
    expect(
      BrokerConnection.fromJson(
        _connected(granted: true),
        origin,
        requestedCursor: true,
      ).cursor,
      isTrue,
    );
    expect(
      BrokerConnection.fromJson(
        _connected(),
        origin,
        requestedCursor: true,
      ).cursor,
      isFalse,
    );
  });

  test('Granted session: kind-3 records reach the presenter after the key frame, never the decoder', () async {
    final (session, socket, media) = await _started(keyFrame: false);
    expect(session.cursorSeparated, isTrue);
    socket.incoming.add(cursorPacket(shapeRecord(1)));
    await flush();
    expect(
      media.records,
      isEmpty,
      reason: 'no picture before the first key frame',
    );
    socket.incoming.add(_video(key: true));
    socket.incoming.add(cursorPacket(shapeRecord(2)));
    socket.incoming.add(cursorPacket(stateRecord(true, 2)));
    socket.incoming.add(_video());
    await flush();
    expect(media.records.map((r) => r.$1), [0, 0]);
    expect(media.records.first.$2, shapeRecord(2));
    expect(media.records.last.$2, stateRecord(true, 2));
    expect(media.packets.every((p) => p[0] == 1), isTrue);
    expect(media.packets, hasLength(2), reason: 'video only, each once');
    expect(session.closed, isFalse);
    await session.close();
  });

  test('Old peers: a kind-3 packet without the grant stays an invalid stream message', () async {
    final (session, socket, media) = await _started(cursor: false);
    expect(session.cursorSeparated, isFalse);
    socket.incoming.add(cursorPacket(stateRecord(true, 1)));
    await flush();
    expect(session.closed, isTrue);
    expect(session.diagnosticCode, 'invalid_stream_message');
    expect(media.records, isEmpty);
  });

  test('Malformed records and envelopes end a granted session', () async {
    for (final packet in [
      cursorPacket(Uint8List.fromList([...stateRecord(true, 1), 0])),
      cursorPacket(stateRecord(true, 1), flags: 1),
      cursorPacket(Uint8List.fromList(shapeRecord(1)..[1] = 9)),
      cursorPacket(stateRecord(true, 1), epoch: 1),
    ]) {
      final (session, socket, media) = await _started();
      socket.incoming.add(packet);
      await flush();
      expect(session.closed, isTrue, reason: '$packet');
      expect(session.diagnosticCode, 'invalid_stream_message');
      expect(media.records, isEmpty);
    }
  });

  test('A native presenter failure does not end the session; a replaced owner does', () async {
    final (session, socket, media) = await _started();
    media.cursorFailure = PlatformException(code: 'invalid_cursor');
    socket.incoming.add(cursorPacket(shapeRecord(1)));
    socket.incoming.add(_video());
    await flush();
    expect(session.closed, isFalse);
    expect(media.packets, hasLength(2));
    media.cursorFailure = null;
    socket.incoming.add(cursorPacket(shapeRecord(2)));
    await flush();
    expect(media.records, hasLength(1));
    media.cursorFailure = PlatformException(code: 'session_replaced');
    socket.incoming.add(cursorPacket(shapeRecord(3)));
    await flush();
    expect(session.closed, isTrue);
    expect(session.diagnosticCode, startsWith('session_replaced'));
  });

  test('Epochs: old-epoch records are dropped, the new epoch waits for its key frame', () async {
    final socket = SessionSocket(), media = CursorMedia();
    final session = RemoteSession(
      connection: _connection(epochMedia: true),
      token: 'fixture',
      media: media,
      dialer: (_, _) async => socket,
    );
    final running = session.start();
    socket.incoming.add(jsonEncode(startInfo));
    await flush();
    socket.incoming.add(_video(key: true));
    socket.incoming.add(cursorPacket(shapeRecord(1)));
    await flush();
    expect(media.records, hasLength(1));
    await session.capture(true);
    final result = session.resizeViewport(1346, 812);
    await flush();
    final id =
        socket.sent.lastWhere((v) => v['type'] == 'stream.resize')['requestId']
            as String;
    socket.incoming.add(
      jsonEncode({
        'type': 'stream.resizing',
        'requestId': id,
        'resizeId': 'matching-resize-id',
        'epoch': 1,
        'width': 1346,
        'height': 812,
        'fps': 60,
        'monitor': '0',
        'timeoutMs': 90000,
      }),
    );
    await flush();
    socket.incoming.add(cursorPacket(stateRecord(true, 1)));
    socket.incoming.add(
      jsonEncode({
        'type': 'stream.info',
        'codec': 'h264',
        'sessionId': 'cursor-session',
        'requestId': id,
        'resizeId': 'matching-resize-id',
        'epoch': 1,
        'width': 1346,
        'height': 812,
        'sourceWidth': 1346,
        'sourceHeight': 812,
        'fpsTarget': 60,
      }),
    );
    await flush();
    socket.incoming.add(cursorPacket(stateRecord(true, 1))); // epoch 0: stale
    socket.incoming.add(cursorPacket(shapeRecord(2), epoch: 1)); // before key
    socket.incoming.add(_video(key: true, epoch: 1));
    socket.incoming.add(cursorPacket(shapeRecord(3), epoch: 1));
    await flush();
    expect(media.records.map((r) => r.$1), [0, 1]);
    expect(media.records.last.$2, shapeRecord(3));
    expect(session.closed, isFalse);
    await session.close();
    await running;
    await result; // a pending resize settles on close
    await media.events.close();
  });

  test('Picture geometry: granted sessions only, sent once per decoder and retried after a failure', () async {
    final (session, _, media) = await _started();
    const view = CursorView(
      left: 10,
      top: 20,
      width: 960,
      height: 540,
      sourceWidth: 1280,
      sourceHeight: 720,
    );
    await session.cursorView(view);
    await session.cursorView(view);
    expect(media.views, [view]);
    await session.cursorView(
      const CursorView(
        left: 0,
        top: 0,
        width: 0,
        height: 540,
        sourceWidth: 1,
        sourceHeight: 1,
      ),
    );
    expect(media.views, hasLength(1), reason: 'invalid geometry is never sent');
    media.viewFailure = PlatformException(code: 'session_replaced');
    const moved = CursorView(
      left: 12,
      top: 20,
      width: 960,
      height: 540,
      sourceWidth: 1280,
      sourceHeight: 720,
    );
    await session.cursorView(moved);
    expect(session.closed, isFalse);
    media.viewFailure = null;
    await session.cursorView(moved);
    expect(media.views, [view, moved]);
    await session.close();
    final (legacy, _, legacyMedia) = await _started(cursor: false);
    await legacy.cursorView(view);
    expect(legacyMedia.views, isEmpty);
    await legacy.close();
  });

  test(
    'Native channel: cursor and cursorView carry the issuing lease',
    () async {
      TestWidgetsFlutterBinding.ensureInitialized();
      const channel = MethodChannel('multitap/remote');
      final messenger =
          TestDefaultBinaryMessengerBinding.instance.defaultBinaryMessenger;
      final calls = <MethodCall>[];
      messenger.setMockMethodCallHandler(channel, (call) async {
        calls.add(call);
        return call.method == 'start' ? 70 : null;
      });
      addTearDown(() => messenger.setMockMethodCallHandler(channel, null));
      final media = NativeRemoteMedia(audioEnabled: false);
      await expectLater(
        media.cursor(0, stateRecord(false, 0)),
        throwsA(isA<PlatformException>()),
        reason: 'no owner before start',
      );
      await media.start(640, 360);
      final lease = (calls.first.arguments as Map)['lease'];
      await media.cursor(3, stateRecord(true, 7));
      await media.cursorView(
        const CursorView(
          left: 1.5,
          top: 2,
          width: 640,
          height: 360,
          sourceWidth: 1280,
          sourceHeight: 720,
        ),
      );
      await media.stop();
      expect(calls.map((c) => c.method), [
        'start',
        'cursor',
        'cursorView',
        'stop',
      ]);
      expect(calls[1].arguments, {
        'lease': lease,
        'epoch': 3,
        'bytes': stateRecord(true, 7),
      });
      expect(calls[2].arguments, {
        'lease': lease,
        'left': 1.5,
        'top': 2.0,
        'width': 640.0,
        'height': 360.0,
        'sourceWidth': 1280,
        'sourceHeight': 720,
      });
      await expectLater(
        media.cursor(0, stateRecord(false, 0)),
        throwsA(isA<PlatformException>()),
        reason: 'retired after stop',
      );
    },
  );

  test(
    'Statistics row: local cursor for a granted session, composed otherwise',
    () async {
      final (session, _, _) = await _started();
      String row(RemoteSession? value) => {
        for (final section in streamMetricSections(
          value,
          options: const StreamOptions(),
          autoViewport: false,
        ))
          for (final metric in section.metrics) metric.label: metric.value,
      }['마우스 커서']!;
      expect(row(session), 'PC에서 표시 (cursor-v1)');
      expect(row(null), '—');
      await session.close();
      final (legacy, _, _) = await _started(cursor: false);
      expect(row(legacy), '영상에 포함');
      await legacy.close();
    },
  );

  testWidgets(
    'The view reports geometry and keeps a local fallback until a cursor state is ready',
    (tester) async {
      tester.view.devicePixelRatio = 1;
      tester.view.physicalSize = const Size(1280, 720);
      addTearDown(tester.view.resetPhysicalSize);
      addTearDown(tester.view.resetDevicePixelRatio);
      final messenger =
          TestDefaultBinaryMessengerBinding.instance.defaultBinaryMessenger;
      const window = MethodChannel('multitap/window');
      const events = MethodChannel('multitap/window/events');
      messenger.setMockMethodCallHandler(window, (_) async => false);
      messenger.setMockMethodCallHandler(events, (_) async => null);
      addTearDown(() {
        messenger.setMockMethodCallHandler(window, null);
        messenger.setMockMethodCallHandler(events, null);
      });
      final socket = SessionSocket(), media = CursorMedia();
      final session = RemoteSession(
        connection: _connection(epochMedia: true),
        token: 'fixture-only-token',
        media: media,
        dialer: (_, _) async => socket,
      );
      await tester.pumpWidget(
        MaterialApp(
          home: RemoteView(machine: machine(1), load: () async => session),
        ),
      );
      await tester.pump();
      await tester.pump();
      socket.incoming.add(jsonEncode(startInfo));
      await tester.pump();
      await tester.pump();
      socket.incoming.add(_video(key: true));
      await tester.pump();
      await tester.pump();
      final picture = tester.getRect(
        find.byKey(const Key('remote-image-bounds')),
      );
      expect(media.views, isNotEmpty);
      final view = media.views.last;
      expect(
        Rect.fromLTWH(view.left, view.top, view.width, view.height),
        picture,
      );
      expect((view.sourceWidth, view.sourceHeight), (1280, 720));
      final mouse = await tester.createGesture(kind: PointerDeviceKind.mouse);
      await mouse.addPointer(location: picture.center);
      await mouse.down(picture.center);
      await tester.pump();
      await tester.pump();
      await mouse.up();
      await tester.pump();
      expect(session.captured, isTrue);
      MouseCursor pictureCursor() => tester
          .widget<MouseRegion>(
            find
                .ancestor(
                  of: find.byType(Texture),
                  matching: find.byType(MouseRegion),
                )
                .first,
          )
          .cursor;
      expect(pictureCursor(), SystemMouseCursors.basic);
      socket.incoming.add(cursorPacket(shapeRecord(1)));
      socket.incoming.add(cursorPacket(stateRecord(true, 1)));
      await tester.pump();
      await tester.pump();
      expect(pictureCursor(), SystemMouseCursors.none);
      socket.incoming.add(cursorPacket(stateRecord(false, 0)));
      await tester.pump();
      await tester.pump();
      expect(pictureCursor(), SystemMouseCursors.none);
      await session.capture(false);
      await tester.pump();
      expect(session.captured, isFalse);
      expect(pictureCursor(), SystemMouseCursors.click);
      final count = media.views.length;
      await tester.pump();
      expect(
        media.views.length,
        count,
        reason: 'unchanged geometry is not resent',
      );
      await tester.pumpWidget(const SizedBox.shrink());
      unawaited(session.close());
      await tester.pump(const Duration(seconds: 3));
      await tester.pump();
      unawaited(media.events.close());
    },
  );
}
