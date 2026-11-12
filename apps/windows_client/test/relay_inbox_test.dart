import 'dart:async';
import 'dart:typed_data';

import 'package:flutter_test/flutter_test.dart';
import 'package:multitap_client/data/relay_inbox.dart';

void main() {
  test('answered text is handled at arrival, ahead of a busy consumer', () async {
    final socket = StreamController<dynamic>();
    final answered = <String>[];
    final inbox = relayInbox(
      socket.stream,
      answer: (message) {
        if (!message.startsWith('{"type":"input.ack"')) return false;
        answered.add(message);
        return true;
      },
    );
    final seen = <Object>[];
    final gate = Completer<void>();
    final done = Completer<void>();
    unawaited(() async {
      await for (final message in inbox) {
        seen.add(message is String ? message : (message as List<int>).length);
        if (seen.length == 1) await gate.future; // the reader is busy pushing media
      }
      done.complete();
    }());
    socket
      ..add(Uint8List(10))
      ..add('{"type":"input.ack","sample":1}')
      ..add(Uint8List(20))
      ..add('{"type":"stream.info"}')
      ..add('{"type":"input.ack","sample":2}');
    await Future<void>.delayed(const Duration(milliseconds: 5));
    expect(answered, hasLength(2), reason: 'both acknowledgements, while the reader is busy');
    expect(seen, [10]);
    gate.complete();
    await socket.close();
    await done.future;
    expect(seen, [10, 20, '{"type":"stream.info"}'], reason: 'the rest keeps its order');
  });

  test('audio is handed on at arrival while the reader waits on video (#98)', () async {
    final socket = StreamController<dynamic>();
    final heard = <int>[];
    final inbox = relayInbox(
      socket.stream,
      answer: (_) => false,
      media: (message) {
        if (message[0] != 2) return false;
        if (message[4] == 99) return false; // the session declines this one
        heard.add(message[4]);
        return true;
      },
    );
    List<int> packet(int kind, int id) => Uint8List(16)
      ..[0] = kind
      ..[4] = id;
    final seen = <String>[];
    final gate = Completer<void>();
    final done = Completer<void>();
    unawaited(() async {
      await for (final message in inbox) {
        seen.add(message is String ? message : 'k${message[0]}#${message[4]}');
        if (seen.length == 1) await gate.future; // a video push waits for decoder room
      }
      done.complete();
    }());
    socket
      ..add(packet(1, 1))
      ..add(packet(1, 2))
      ..add(packet(2, 3))
      ..add(packet(2, 4))
      ..add(packet(1, 5))
      ..add(packet(2, 6));
    await Future<void>.delayed(const Duration(milliseconds: 5));
    expect(heard, [3, 4, 6], reason: 'audio behind waiting video is not held');
    expect(seen, ['k1#1']);
    // A declined audio packet and text after it keep their order, and so does
    // every message behind them until they were delivered.
    socket
      ..add(packet(2, 99))
      ..add('{"type":"stream.info"}')
      ..add(packet(2, 7));
    await Future<void>.delayed(const Duration(milliseconds: 5));
    expect(heard, [3, 4, 6], reason: 'nothing overtakes what waits for order');
    gate.complete();
    await Future<void>.delayed(const Duration(milliseconds: 5));
    socket.add(packet(2, 8));
    await socket.close();
    await done.future;
    expect(seen, ['k1#1', 'k1#2', 'k1#5', 'k2#99', '{"type":"stream.info"}', 'k2#7']);
    expect(heard, [3, 4, 6, 8], reason: 'at arrival again once the ordered ones were delivered');
  });

  test('without a media callback every binary message keeps its order', () async {
    final socket = StreamController<dynamic>();
    final got = relayInbox(socket.stream, answer: (_) => false).toList();
    socket
      ..add(Uint8List(13)..[0] = 1)
      ..add(Uint8List(14)..[0] = 2);
    await socket.close();
    expect((await got).map((m) => (m as List<int>).length), [13, 14]);
  });

  test('a throwing or declining answer leaves the message in the stream', () async {
    final socket = StreamController<dynamic>();
    final inbox = relayInbox(
      socket.stream,
      answer: (message) => message == 'boom' ? throw StateError('x') : false,
    );
    final got = inbox.toList();
    socket
      ..add('boom')
      ..add('plain');
    await socket.close();
    expect(await got, ['boom', 'plain']);
  });

  test('the socket pauses past the bound and resumes when the reader catches up', () async {
    var paused = false;
    final socket = StreamController<dynamic>(
      onPause: () => paused = true,
      onResume: () => paused = false,
    );
    final inbox = relayInbox(socket.stream, answer: (_) => false, maxPackets: 4);
    final iterator = StreamIterator(inbox);
    for (var n = 0; n < 6; n++) {
      socket.add(Uint8List(n + 1));
    }
    await Future<void>.delayed(Duration.zero);
    expect(paused, isTrue);
    var taken = 0;
    while (paused && taken < 6) {
      expect(await iterator.moveNext(), isTrue);
      taken++;
      await Future<void>.delayed(Duration.zero);
    }
    expect(paused, isFalse);
    expect(taken, inInclusiveRange(3, 5), reason: 'resumes near half the bound, not at once');
    await iterator.cancel();
    expect(socket.hasListener, isFalse, reason: 'cancel reaches the socket');
  });

  test('errors and the end of the socket reach the reader', () async {
    final socket = StreamController<dynamic>();
    final inbox = relayInbox(socket.stream, answer: (_) => false);
    final events = <Object>[];
    final done = Completer<void>();
    inbox.listen(
      (Object? event) => events.add(event!),
      onError: events.add,
      onDone: done.complete,
    );
    socket
      ..add('a')
      ..addError(StateError('closed'));
    await socket.close();
    await done.future;
    expect(events.first, 'a');
    expect(events.last, isA<StateError>());
  });
}
