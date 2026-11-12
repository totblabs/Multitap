import 'dart:async';
import 'dart:convert';
import 'dart:io';

import 'package:flutter/services.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:multitap_client/data/desktop_watch.dart';

import 'remote_session_test.dart' show SessionSocket;

final _origin = Uri.parse('https://cloud.example.com:18443/');
const _token = 'fixture-memory-only-bearer';
const _ready = '{"type":"desktops.ready"}';
String _changed(Object? revision) =>
    jsonEncode({'type': 'desktops.changed', 'revision': revision});

class _WatchSubscription extends Fake implements StreamSubscription<dynamic> {
  _WatchSubscription(
    this.inner, {
    this.cancelError = false,
    this.cancelHangs = false,
  });
  final StreamSubscription<dynamic> inner;
  final bool cancelError, cancelHangs;
  int cancellations = 0;
  @override
  Future<void> cancel() async {
    cancellations++;
    await inner.cancel();
    if (cancelError) throw StateError('fixture cancellation failure');
    if (cancelHangs) await Completer<void>().future;
  }
}

class _WatchSocket extends SessionSocket {
  _WatchSocket({
    this.cancelError = false,
    this.cancelHangs = false,
    this.closeError = false,
    this.closeHangs = false,
  });
  final bool cancelError, cancelHangs, closeError, closeHangs;
  int closes = 0;
  _WatchSubscription? subscription;
  @override
  StreamSubscription<dynamic> listen(
    void Function(dynamic)? onData, {
    Function? onError,
    void Function()? onDone,
    bool? cancelOnError,
  }) {
    final inner = super.listen(
      onData,
      onError: onError,
      onDone: onDone,
      cancelOnError: cancelOnError,
    );
    return subscription = _WatchSubscription(
      inner,
      cancelError: cancelError,
      cancelHangs: cancelHangs,
    );
  }

  @override
  Future<void> close([int? code, String? reason]) async {
    closes++;
    await super.close(code, reason);
    if (closeError) throw StateError('fixture socket close failure');
    if (closeHangs) await Completer<void>().future;
  }
}

BrokerDesktopWatch _watch(_WatchSocket socket, {DesktopWatchDialer? dialer}) =>
    BrokerDesktopWatch(
      origin: _origin,
      token: _token,
      dialer: dialer ?? (_, _, _) async => socket,
    );

Future<void> _connect(
  WidgetTester t,
  BrokerDesktopWatch watch,
  _WatchSocket socket, {
  bool ready = true,
}) async {
  watch.start();
  await t.pump();
  expect(watch.connected, isFalse);
  if (ready) {
    socket.incoming.add(_ready);
    await t.pump();
    expect(watch.connected, isTrue);
  }
}

Future<void> _closeWatch(WidgetTester t, BrokerDesktopWatch watch) async {
  var done = false;
  final closing = watch.close()..then((_) => done = true);
  await t.pump();
  await t.pump(const Duration(seconds: 5));
  expect(
    done,
    isTrue,
    reason: 'Watch cleanup must finish within bounded fake time.',
  );
  await closing;
}

void main() {
  for (final (closeCode, reason, terminal) in <(int, String, String?)>[
    (1008, 'session_expired', 'session_reauthentication_required'),
    (1008, 'authentication_required', 'session_reauthentication_required'),
    (1008, 'authorization_revoked', 'session_reauthentication_required'),
    (1001, 'authorization_revoked', null),
    (1008, 'rate_limited', null),
    (1008, 'unexpected_message', null),
  ]) {
    testWidgets('account watch scoped auth close $closeCode/$reason', (
      t,
    ) async {
      final socket = SessionSocket();
      var dials = 0;
      final watch = BrokerDesktopWatch(
        origin: _origin,
        token: _token,
        dialer: (uri, _, _) async {
          expect(uri.path, '/api/v1/events');
          dials++;
          return socket;
        },
      );
      final notices = <String>[];
      final observer = watch.terminalAuthentication.listen(notices.add);
      watch.start();
      await t.pump();
      socket.incoming.add(_ready);
      await t.pump();
      expect(watch.connected, isTrue);
      socket.closeCode = closeCode;
      socket.closeReason = reason;
      socket.incoming.addError(StateError('synthetic closed socket'));
      await t.pump();
      await t.pump(const Duration(seconds: 2));
      await t.pump(const Duration(seconds: 2));
      expect(watch.connected, isFalse);
      expect(notices, terminal == null ? isEmpty : [terminal]);
      if (terminal != null) expect(dials, 1);
      unawaited(observer.cancel());
      await _closeWatch(t, watch);
    });
  }
  test(
    'Watch rejects insecure or credential-bearing origins before dialing',
    () {
      var dials = 0;
      for (final origin in [
        'http://cloud.example.com',
        'wss://cloud.example.com',
        'https://',
        'https://user:password@cloud.example.com',
        'https://cloud.example.com/path',
        'https://cloud.example.com/?token=private',
        'https://cloud.example.com/#private',
      ]) {
        expect(
          () => BrokerDesktopWatch(
            origin: Uri.parse(origin),
            token: _token,
            dialer: (_, _, _) {
              dials++;
              return Future.error(StateError('must not dial'));
            },
          ),
          throwsArgumentError,
        );
      }
      expect(dials, 0);
    },
  );

  testWidgets(
    'Start is idempotent and same-origin WSS authentication stays read-only',
    (t) async {
      final socket = _WatchSocket();
      final calls = <(Uri, String, Uri)>[];
      final persistenceCalls = <MethodCall>[];
      const credentials = MethodChannel('multitap/credentials');
      final messenger =
          TestDefaultBinaryMessengerBinding.instance.defaultBinaryMessenger;
      messenger.setMockMethodCallHandler(credentials, (call) async {
        persistenceCalls.add(call);
        return null;
      });
      addTearDown(() => messenger.setMockMethodCallHandler(credentials, null));
      final watch = _watch(
        socket,
        dialer: (url, token, origin) async {
          calls.add((url, token, origin));
          return socket;
        },
      );
      var changes = 0;
      final connectedStates = <bool>[];
      watch.changes.listen((_) => changes++);
      watch.addListener(() => connectedStates.add(watch.connected));
      watch.start();
      watch.start();
      watch.start();
      expect(calls, [
        (
          Uri.parse('wss://cloud.example.com:18443/api/v1/events'),
          _token,
          _origin,
        ),
      ]);
      expect(watch.connected, isFalse);
      await t.pump();
      expect(changes, 0);
      expect(socket.pingInterval, const Duration(seconds: 10));
      socket.incoming.add(_ready);
      await t.pump();
      expect(watch.connected, isTrue);
      expect(changes, 1);
      expect(connectedStates, [true]);
      watch.start();
      expect(calls, hasLength(1));
      expect(
        socket.sent,
        isEmpty,
        reason: 'This channel never sends keyboard, clipboard or VM commands.',
      );
      expect(
        persistenceCalls,
        isEmpty,
        reason: 'The watch token is supplied only to the in-memory dialer.',
      );
      await _closeWatch(t, watch);
      expect(socket.closes, 1);
      expect(watch.connected, isFalse);
      await t.pump(const Duration(seconds: 40));
      expect(calls, hasLength(1));
    },
  );

  testWidgets(
    'Ready refreshes once and only increasing positive revisions invalidate',
    (t) async {
      final socket = _WatchSocket();
      final watch = _watch(socket);
      var changes = 0;
      watch.changes.listen((_) => changes++);
      await _connect(t, watch, socket);
      for (final revision in [1, 1, 3, 2, 9007199254740991, 9007199254740991]) {
        socket.incoming.add(_changed(revision));
        await t.pump();
      }
      expect(changes, 4);
      expect(watch.connected, isTrue);
      expect(socket.sent, isEmpty);
      await _closeWatch(t, watch);
    },
  );

  testWidgets(
    'Malformed, unknown, binary and oversized messages lose readiness without dispatching data',
    (t) async {
      final invalid = <String, Object>{
        'unknown type': '{"type":"input.key","key":"PRIVATE_SENTINEL"}',
        'binary': <int>[1, 2, 3],
        'oversized': 'x' * 2049,
        'invalid JSON': '{not JSON',
        'nonobject JSON': '[]',
        'duplicate ready': _ready,
      };
      for (final entry in invalid.entries) {
        final socket = _WatchSocket();
        final watch = _watch(socket);
        final states = <bool>[];
        var changes = 0;
        watch.addListener(() => states.add(watch.connected));
        watch.changes.listen((_) => changes++);
        await _connect(t, watch, socket);
        socket.incoming.add(entry.value);
        await t.pump();
        expect(watch.connected, isFalse, reason: entry.key);
        expect(socket.wasClosed, isTrue, reason: entry.key);
        expect(changes, 1, reason: entry.key);
        expect(states, [true, false], reason: entry.key);
        expect(socket.sent, isEmpty);
        await _closeWatch(t, watch);
      }
    },
  );

  testWidgets(
    'Changed before ready and invalid revision values close the watch',
    (t) async {
      for (final revision in [null, true, '1', 0, -1, 1.5, 9007199254740992]) {
        final socket = _WatchSocket();
        final watch = _watch(socket);
        await _connect(t, watch, socket);
        socket.incoming.add(_changed(revision));
        await t.pump();
        expect(watch.connected, isFalse, reason: 'revision=$revision');
        expect(socket.wasClosed, isTrue);
        await _closeWatch(t, watch);
      }
      final socket = _WatchSocket();
      final watch = _watch(socket);
      var changes = 0;
      watch.changes.listen((_) => changes++);
      await _connect(t, watch, socket, ready: false);
      socket.incoming.add(_changed(1));
      await t.pump();
      expect(watch.connected, isFalse);
      expect(changes, 0);
      expect(socket.wasClosed, isTrue);
      await _closeWatch(t, watch);
    },
  );

  testWidgets(
    'Failures retry with capped backoff and close cancels the next attempt',
    (t) async {
      var dials = 0;
      final watch = BrokerDesktopWatch(
        origin: _origin,
        token: _token,
        dialer: (_, _, _) async {
          dials++;
          throw StateError('fixture offline');
        },
      );
      watch.start();
      await t.pump();
      expect(dials, 1);
      for (final seconds in [1, 2, 4, 8, 15, 15]) {
        final before = dials;
        await t.pump(Duration(milliseconds: seconds * 1000 - 1));
        expect(dials, before);
        await t.pump(const Duration(milliseconds: 1));
        expect(dials, before + 1);
      }
      expect(watch.connected, isFalse);
      await _closeWatch(t, watch);
      await t.pump(const Duration(minutes: 2));
      expect(dials, 7);
    },
  );

  testWidgets('A successful ready resets retry backoff and revision baseline', (
    t,
  ) async {
    final sockets = <_WatchSocket>[];
    var dials = 0, changes = 0;
    final watch = BrokerDesktopWatch(
      origin: _origin,
      token: _token,
      dialer: (_, _, _) async {
        if (++dials < 3) throw StateError('fixture offline');
        final socket = _WatchSocket();
        sockets.add(socket);
        return socket;
      },
    );
    watch.changes.listen((_) => changes++);
    watch.start();
    await t.pump();
    await t.pump(const Duration(seconds: 1));
    await t.pump(const Duration(seconds: 2));
    sockets.single.incoming.add(_ready);
    sockets.single.incoming.add(_changed(50));
    await t.pump();
    expect(changes, 2);
    unawaited(sockets.single.incoming.close());
    await t.pump();
    expect(watch.connected, isFalse);
    await t.pump(const Duration(milliseconds: 999));
    expect(dials, 3);
    await t.pump(const Duration(milliseconds: 1));
    expect(dials, 4);
    sockets.last.incoming.add(_ready);
    sockets.last.incoming.add(_changed(1));
    await t.pump();
    expect(
      changes,
      4,
      reason: 'A new authenticated connection has its own revision baseline.',
    );
    await _closeWatch(t, watch);
  });

  testWidgets(
    'Connect timeout is 12 seconds and a late handshake is closed once',
    (t) async {
      final first = Completer<WebSocket>();
      final late = _WatchSocket(), current = _WatchSocket();
      var dials = 0;
      final watch = _watch(
        current,
        dialer: (_, _, _) {
          dials++;
          return dials == 1 ? first.future : Future.value(current);
        },
      );
      watch.start();
      await t.pump(const Duration(milliseconds: 11999));
      expect(dials, 1);
      expect(watch.connected, isFalse);
      await t.pump(const Duration(milliseconds: 1));
      await t.pump(const Duration(milliseconds: 999));
      expect(dials, 1);
      await t.pump(const Duration(milliseconds: 1));
      expect(dials, 2);
      first.complete(late);
      await t.pump();
      expect(late.closes, 1);
      expect(watch.connected, isFalse);
      current.incoming.add(_ready);
      await t.pump();
      expect(watch.connected, isTrue);
      await _closeWatch(t, watch);
    },
  );

  testWidgets(
    'Missing ready closes after 10 seconds and schedules fallback retry',
    (t) async {
      final sockets = <_WatchSocket>[];
      final watch = BrokerDesktopWatch(
        origin: _origin,
        token: _token,
        dialer: (_, _, _) async {
          final socket = _WatchSocket();
          sockets.add(socket);
          return socket;
        },
      );
      watch.start();
      await t.pump();
      await t.pump(const Duration(milliseconds: 9999));
      expect(sockets.single.wasClosed, isFalse);
      await t.pump(const Duration(milliseconds: 1));
      expect(sockets.single.wasClosed, isTrue);
      expect(watch.connected, isFalse);
      await t.pump(const Duration(milliseconds: 999));
      expect(sockets, hasLength(1));
      await t.pump(const Duration(milliseconds: 1));
      expect(sockets, hasLength(2));
      await _closeWatch(t, watch);
    },
  );

  testWidgets(
    'Logout before handshake closes the late socket with no reconnection or changes',
    (t) async {
      final pending = Completer<WebSocket>();
      final late = _WatchSocket();
      var dials = 0, changes = 0;
      final watch = _watch(
        late,
        dialer: (_, _, _) {
          dials++;
          return pending.future;
        },
      );
      watch.changes.listen((_) => changes++);
      watch.start();
      await t.pump();
      final firstClose = watch.close();
      expect(identical(firstClose, watch.close()), isTrue);
      await _closeWatch(t, watch);
      await firstClose;
      pending.complete(late);
      await t.pump();
      expect(late.closes, 1);
      expect(watch.connected, isFalse);
      watch.start();
      await t.pump(const Duration(minutes: 1));
      expect(dials, 1);
      expect(changes, 0);
    },
  );

  testWidgets(
    'Socket close and subscription cancellation failures do not escape cleanup',
    (t) async {
      for (final loseFirst in [false, true]) {
        final socket = _WatchSocket(cancelError: true, closeError: true);
        final watch = _watch(socket);
        await _connect(t, watch, socket);
        if (loseFirst) {
          socket.incoming.addError(StateError('fixture read failure'));
          await t.pump();
        }
        await _closeWatch(t, watch);
        await t.pump();
        expect(socket.closes, 1);
        expect(socket.subscription!.cancellations, 1);
        expect(watch.connected, isFalse);
        expect(t.takeException(), isNull);
      }
    },
  );

  testWidgets(
    'Hanging cancellation and socket close are bounded concurrently at 2 seconds',
    (t) async {
      final socket = _WatchSocket(cancelHangs: true, closeHangs: true);
      final watch = _watch(socket);
      await _connect(t, watch, socket);
      var closed = false;
      final closing = watch.close()..then((_) => closed = true);
      expect(identical(closing, watch.close()), isTrue);
      await t.pump(const Duration(milliseconds: 1999));
      expect(closed, isFalse);
      await t.pump(const Duration(milliseconds: 2));
      expect(closed, isTrue);
      expect(socket.closes, 1);
      expect(watch.connected, isFalse);
      await t.pump(const Duration(minutes: 1));
      expect(t.takeException(), isNull);
    },
  );

  testWidgets(
    'Close before start is idempotent and never creates a network connection',
    (t) async {
      var dials = 0;
      final socket = _WatchSocket();
      final watch = _watch(
        socket,
        dialer: (_, _, _) async {
          dials++;
          return socket;
        },
      );
      final closing = watch.close();
      expect(identical(closing, watch.close()), isTrue);
      await _closeWatch(t, watch);
      await closing;
      watch.start();
      await t.pump(const Duration(minutes: 1));
      expect(dials, 0);
      expect(socket.closes, 0);
    },
  );
}
