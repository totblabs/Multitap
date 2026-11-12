import 'dart:async';
import 'dart:convert';
import 'dart:typed_data';

import 'package:flutter_test/flutter_test.dart';
import 'package:fake_async/fake_async.dart';

import 'package:multitap_client/data/broker_api.dart';
import 'package:multitap_client/data/emergency_console_session.dart';

import 'console_rfb_test.dart' as fixture;

const grantId = '01234567-89ab-cdef-8123-0123456789ab';
final consoleOrigin = Uri.parse('https://console.invalid');
BrokerConsoleGrant testGrant({Uri? relay}) => BrokerConsoleGrant(
  id: grantId,
  ticket: 'synthetic-one-use-ticket',
  relay: relay ?? Uri.parse('wss://console.invalid/relay/console/$grantId'),
  expiresAt: DateTime.utc(2035),
);

class FakeConsoleTransport implements ConsoleTransport {
  final incoming = StreamController<Object?>.broadcast(sync: true);
  final sent = <Object>[];
  bool wasClosed = false, throwSend = false;
  @override
  Stream<Object?> get messages => incoming.stream;
  @override
  void send(Object value) {
    if (throwSend) throw StateError('synthetic');
    sent.add(value);
  }

  @override
  Future<void> close() async {
    wasClosed = true;
    await incoming.close();
  }

  void ready({String? keyboardProtocol}) => incoming.add(
    jsonEncode({
      'type': 'console-ready',
      'protocol': 'rfb-none-v1',
      'keyboardProtocol': ?keyboardProtocol,
    }),
  );
}

void main() {
  test('DesktopSize has a bounded picture wait; repeated resize/LED cannot renew it', () {
    fakeAsync((clock) {
      final transport = FakeConsoleTransport(), released = <String>[];
      var claims = 0;
      final session = EmergencyConsoleSession(
        origin: consoleOrigin,
        token: 'synthetic-token',
        deadline: const Duration(seconds: 2),
        acquire: () async {
          claims++;
          return testGrant();
        },
        release: (id) async => released.add(id),
        transportFactory: (_, _) async => transport,
      )..requestNativeKeyboardProtocol();
      unawaited(session.start());
      clock.flushMicrotasks();
      transport.ready(keyboardProtocol: 'qemu-keyboard-v1');
      transport.incoming.add(fixture.initRfb());
      transport.incoming.add(fixture.rfbRect(0, 0, 2, 2, -258, []));
      transport.incoming.add(fixture.rfbRect(0, 0, 1, 1, -261, [0]));
      // Actual black pixels are a valid image, not an inactivity failure.
      transport.incoming.add(
        fixture.rfbRect(0, 0, 2, 2, 0, List.filled(16, 0)),
      );
      clock.elapse(const Duration(seconds: 8));
      expect(session.connected, isTrue);
      transport.incoming.add(fixture.rfbRect(0, 0, 4, 3, -223, []));
      expect(session.waiting, isTrue);
      expect(session.connected, isFalse);
      clock.elapse(const Duration(milliseconds: 1500));
      // A second size notification without pixels must keep the original
      // deadline, rather than starting another two-second wait.
      transport.incoming.add(fixture.rfbRect(0, 0, 5, 4, -223, []));
      expect(session.rfb.frameValid, isFalse);
      transport.incoming.add(fixture.rfbRect(0, 0, 1, 1, -261, [4]));
      clock.elapse(const Duration(milliseconds: 501));
      clock.flushMicrotasks();
      expect(session.closed, isTrue);
      expect(session.failureCode, 'console_frame_timeout');
      expect(
        consoleFailureMessage(session.failureCode),
        'Console 화면을 받지 못했어요.\n다시 연결하세요.',
      );
      expect(transport.wasClosed, isTrue);
      expect(released, [grantId]);
      expect(claims, 1);
      session.dispose();
      clock.flushMicrotasks();
    });
  });

  test('new pixels cancel resize deadline on the same grant and the next resize rearms it', () {
    fakeAsync((clock) {
      final transport = FakeConsoleTransport();
      var claims = 0;
      final session = EmergencyConsoleSession(
        origin: consoleOrigin,
        token: 'synthetic-token',
        deadline: const Duration(seconds: 2),
        acquire: () async {
          claims++;
          return testGrant();
        },
        release: (_) async {},
        transportFactory: (_, _) async => transport,
      );
      unawaited(session.start());
      clock.flushMicrotasks();
      transport.ready();
      transport.incoming.add(fixture.initRfb());
      transport.incoming.add(
        fixture.rfbRect(0, 0, 2, 2, 0, List.filled(16, 0)),
      );
      transport.incoming.add(fixture.rfbRect(0, 0, 4, 3, -223, []));
      final request = transport.sent.whereType<Uint8List>().last;
      expect(request[0], 3);
      expect(request[1], 0); // A real full-frame request, without reconnecting.
      clock.elapse(const Duration(seconds: 1));
      transport.incoming.add(
        fixture.rfbRect(0, 0, 4, 3, 0, List.filled(48, 0)),
      );
      clock.elapse(const Duration(seconds: 4));
      expect(session.connected, isTrue);
      expect(session.failureCode, isNull);
      expect(claims, 1);
      transport.incoming.add(fixture.rfbRect(0, 0, 2, 2, -223, []));
      clock.elapse(const Duration(seconds: 3));
      clock.flushMicrotasks();
      expect(session.failureCode, 'console_frame_timeout');
      expect(claims, 1);
      session.dispose();
      clock.flushMicrotasks();
    });
  });

  test('Windows opt-in AND exact new broker marker are required; old auth DTO stays unchanged', () async {
    for (final optedIn in [false, true]) {
      for (final marker in [null, 'unknown-keyboard-v2', 'qemu-keyboard-v1']) {
        final transport = FakeConsoleTransport();
        final session = EmergencyConsoleSession(
          origin: consoleOrigin,
          token: 'synthetic-token',
          acquire: () async => testGrant(),
          release: (_) async {},
          transportFactory: (_, _) async => transport,
        );
        addTearDown(session.dispose);
        if (optedIn) session.requestNativeKeyboardProtocol();
        await session.start();
        // No extension field is imposed on existing broker request/auth DTOs.
        expect(jsonDecode(transport.sent.single as String), {
          'type': 'console-auth',
          'ticket': 'synthetic-one-use-ticket',
        });
        session
            .requestNativeKeyboardProtocol(); // Too late: cannot change intent.
        transport.incoming.add(
          jsonEncode({
            'type': 'console-ready',
            'protocol': 'rfb-none-v1',
            'keyboardProtocol': ?marker,
          }),
        );
        transport.incoming.add(fixture.initRfb());
        final request = transport.sent.whereType<Uint8List>().firstWhere(
          (b) => b[0] == 2,
        );
        expect(
          ByteData.sublistView(request).getUint16(2),
          optedIn && marker == 'qemu-keyboard-v1' ? 6 : 4,
        );
        expect(session.connected, isFalse);
        await session.close();
      }
    }
  });

  test('broker grant constrains relay path to configured origin and canonical exact session', () {
    final value = {
      'id': grantId,
      'ticket': 'synthetic',
      'relayPath': '/relay/console/$grantId',
      'expiresAt': '2035-01-01T00:00:00.000Z',
    };
    expect(
      BrokerConsoleGrant.fromJson(value, consoleOrigin).relay.toString(),
      'wss://console.invalid/relay/console/$grantId',
    );
    for (final bad in [
      'https://pve.invalid/secret',
      '//evil.invalid/a',
      '/relay/console/other?ticket=x',
    ]) {
      expect(
        () => BrokerConsoleGrant.fromJson({
          ...value,
          'relayPath': bad,
        }, consoleOrigin),
        throwsA(isA<BrokerException>()),
      );
    }
  });

  test('ready control precedes None binary protocol; no automatic re-acquisition on end', () async {
    final transport = FakeConsoleTransport();
    var claims = 0;
    final released = <String>[];
    final session = EmergencyConsoleSession(
      origin: consoleOrigin,
      token: 'synthetic-token',
      acquire: () async {
        claims++;
        return testGrant();
      },
      release: (id) async {
        released.add(id);
      },
      transportFactory: (_, _) async => transport,
    );
    addTearDown(session.dispose);
    await session.start();
    expect(jsonDecode(transport.sent.single as String), {
      'type': 'console-auth',
      'ticket': 'synthetic-one-use-ticket',
    });
    transport.ready();
    transport.incoming.add(fixture.initRfb());
    expect(session.rfb.initialized, isTrue);
    expect(session.rfb.frameRevision, 0);
    transport.incoming.add(fixture.rfbRect(0, 0, 1, 1, 0, [0, 0, 255, 0]));
    expect(session.rfb.frameRevision, 1);
    transport.incoming.add(
      jsonEncode({'type': 'console-ended', 'code': 'authorization_revoked'}),
    );
    await session.close();
    expect(session.closed, isTrue);
    expect(claims, 1);
    expect(released, [grantId]);
    expect(transport.wasClosed, isTrue);
  });

  test(
    'binary before broker ready is rejected and exact grant is released',
    () async {
      final transport = FakeConsoleTransport();
      final released = <String>[];
      final session = EmergencyConsoleSession(
        origin: consoleOrigin,
        token: 'synthetic-token',
        acquire: () async => testGrant(),
        release: (id) async {
          released.add(id);
        },
        transportFactory: (_, _) async => transport,
      );
      addTearDown(session.dispose);
      await session.start();
      transport.incoming.add(Uint8List.fromList([1]));
      await session.close();
      expect(session.failureCode, 'console_invalid_frame');
      expect(released, [grantId]);
    },
  );

  test('late granted reservation after closed intent is released without opening socket', () async {
    final grant = Completer<BrokerConsoleGrant>();
    final released = <String>[];
    var sockets = 0;
    final session = EmergencyConsoleSession(
      origin: consoleOrigin,
      token: 'synthetic-token',
      acquire: () => grant.future,
      release: (id) async {
        released.add(id);
      },
      transportFactory: (_, _) async {
        sockets++;
        return FakeConsoleTransport();
      },
    );
    addTearDown(session.dispose);
    final pending = session.start();
    await session.close();
    grant.complete(testGrant());
    await pending;
    expect(sockets, 0);
    expect(released, [grantId]);
    expect(session.connected, isFalse);
  });

  test('late transport after closed intent is closed without authentication or paint', () async {
    final gate = Completer<ConsoleTransport>(), entered = Completer<void>();
    final released = <String>[];
    final session = EmergencyConsoleSession(
      origin: consoleOrigin,
      token: 'synthetic-token',
      acquire: () async => testGrant(),
      release: (id) async {
        released.add(id);
      },
      transportFactory: (_, _) {
        entered.complete();
        return gate.future;
      },
    );
    addTearDown(session.dispose);
    final pending = session.start();
    await entered.future;
    await session.close();
    final late = FakeConsoleTransport();
    gate.complete(late);
    await pending;
    expect(late.wasClosed, isTrue);
    expect(late.sent, isEmpty);
    expect(released, [grantId]);
  });

  test('forged foreign relay never receives bearer and only exact grant is retired', () async {
    var sockets = 0;
    final released = <String>[];
    final session = EmergencyConsoleSession(
      origin: consoleOrigin,
      token: 'synthetic-token',
      acquire: () async => testGrant(
        relay: Uri.parse('wss://elsewhere.invalid/relay/console/$grantId'),
      ),
      release: (id) async {
        released.add(id);
      },
      transportFactory: (_, _) async {
        sockets++;
        return FakeConsoleTransport();
      },
    );
    addTearDown(session.dispose);
    await session.start();
    await session.close();
    expect(sockets, 0);
    expect(session.failureCode, 'console_auth_failed');
    expect(released, [grantId]);
  });

  test('send failure during input release does not reenter retirement or repeat DELETE', () async {
    final transport = FakeConsoleTransport();
    final released = <String>[];
    final session = EmergencyConsoleSession(
      origin: consoleOrigin,
      token: 'synthetic-token',
      acquire: () async => testGrant(),
      release: (id) async {
        released.add(id);
      },
      transportFactory: (_, _) async => transport,
    );
    addTearDown(session.dispose);
    await session.start();
    transport.ready();
    transport.incoming.add(fixture.initRfb());
    session.rfb.key(0xffe3, true);
    transport.throwSend = true;
    await Future.wait([session.close(), session.close()]);
    expect(released, [grantId]);
    expect(transport.wasClosed, isTrue);
  });

  test(
    'held initial grant reaches bounded timeout without automatic POST retry',
    () {
      fakeAsync((clock) {
        final gate = Completer<BrokerConsoleGrant>();
        var claims = 0;
        final session = EmergencyConsoleSession(
          origin: consoleOrigin,
          token: 'synthetic-token',
          acquire: () {
            claims++;
            return gate.future;
          },
          release: (_) async {},
        );
        unawaited(session.start());
        clock.elapse(const Duration(seconds: 21));
        clock.flushMicrotasks();
        expect(session.closed, isTrue);
        expect(session.failureCode, 'console_timeout');
        expect(claims, 1);
        session.dispose();
        clock.flushMicrotasks();
      });
    },
  );
}
