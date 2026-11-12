// media-wire-v1 as the Dart session writes it and reads the engine's answer
// (AndroidMediaPacing.kt androidMediaWire is the Kotlin reader; its JVM check
// covers the same layout). No device or MediaCodec is involved.
import 'dart:async';
import 'dart:typed_data';

import 'package:flutter/services.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:multitap_client/android/android_media.dart';

Uint8List _packet(int kind, int length, {int fill = 7}) {
  final bytes = Uint8List(length)..fillRange(12, length, fill);
  bytes[0] = kind;
  bytes[4] = 0x34;
  bytes[5] = 0x12;
  return bytes;
}

Uint8List _bytes(ByteData data) =>
    data.buffer.asUint8List(data.offsetInBytes, data.lengthInBytes);

void main() {
  test('header: version, watch flag, zero bytes, little-endian lease', () {
    final wire = AndroidMediaWire();
    final packet = _packet(1, 40);
    final data = wire.encode(0x0102030405060708, packet, watch: true);
    final bytes = _bytes(data);
    expect(bytes.length, AndroidMediaWire.header + 40);
    expect(bytes.sublist(0, 16), [
      1, 1, 0, 0, 0, 0, 0, 0, //
      8, 7, 6, 5, 4, 3, 2, 1,
    ]);
    expect(bytes.sublist(16), packet);
    expect(_bytes(wire.encode(9, packet)).sublist(0, 2), [1, 0]);
    // udp-media-v1: bit 1 sends the packet through the path rule.
    expect(_bytes(wire.encode(9, packet, path: true)).sublist(0, 2), [1, 2]);
    expect(
      _bytes(wire.encode(9, packet, watch: true, path: true)).sublist(0, 2),
      [1, 3],
    );
  });

  test('one reused buffer: a smaller packet after a larger one is exact', () {
    final wire = AndroidMediaWire();
    final large = _bytes(wire.encode(3, _packet(1, 5000, fill: 9)));
    final small = _bytes(wire.encode(4, _packet(2, 1932, fill: 5)));
    // The same memory: the first view now shows the second header.
    expect(large[8], 4, reason: 'no new buffer');
    expect(small.length, 16 + 1932);
    expect(small[16], 2);
    expect(small.sublist(28).every((b) => b == 5), isTrue);
    expect(ByteData.sublistView(small).getInt64(8, Endian.little), 4);
    // A larger packet grows it once, with room to spare.
    final grown = _bytes(wire.encode(5, _packet(1, 9000)));
    expect(grown.length, 16 + 9000);
    expect(grown.buffer.lengthInBytes, greaterThan(16 + 9000));
    expect(large[8], 4, reason: 'the grown buffer is new memory');
    wire.encode(6, _packet(1, 9500));
    expect(grown[8], 6, reason: 'and is reused after that');
  });

  test('the largest relay packet fits exactly', () {
    final wire = AndroidMediaWire();
    final data = wire.encode(1, _packet(1, 4 * 1024 * 1024));
    expect(data.lengthInBytes, 16 + 4 * 1024 * 1024);
    expect(data.buffer.lengthInBytes, 16 + 4 * 1024 * 1024);
  });

  test('answers: one status byte; anything else is an error', () {
    ByteData answer(List<int> bytes) =>
        ByteData.sublistView(Uint8List.fromList(bytes));
    expect(androidMediaReplyCode(answer([0])), isNull);
    expect(androidMediaReplyCode(answer([1])), 'media_queue_full');
    expect(androidMediaReplyCode(answer([2])), 'session_replaced');
    expect(androidMediaReplyCode(answer([3])), 'decoder_failed');
    expect(androidMediaReplyCode(answer([4])), 'invalid_packet');
    expect(androidMediaReplyCode(answer([5])), 'invalid_media_reply');
    // No handler answers null: never taken as a delivered packet.
    expect(androidMediaReplyCode(null), 'invalid_media_reply');
    expect(androidMediaReplyCode(answer([])), 'invalid_media_reply');
    expect(androidMediaReplyCode(answer([0, 0])), 'invalid_media_reply');
  });

  group('audio is not awaited', () {
    TestWidgetsFlutterBinding.ensureInitialized();
    final messenger =
        TestDefaultBinaryMessengerBinding.instance.defaultBinaryMessenger;
    late List<Completer<ByteData?>> answers;
    late List<int> kinds, flags;

    setUp(() {
      answers = [];
      kinds = [];
      flags = [];
      messenger.setMockMessageHandler(androidMediaChannel.name, (message) {
        kinds.add(_bytes(message!)[16]);
        flags.add(_bytes(message)[1]);
        final answer = Completer<ByteData?>();
        answers.add(answer);
        return answer.future;
      });
    });
    tearDown(() => messenger.setMockMessageHandler(androidMediaChannel.name, null));

    ByteData status(int code) =>
        ByteData.sublistView(Uint8List.fromList([code]));

    test('an audio packet returns at once, a video packet waits for its answer', () async {
      final media = _Probe();
      await media.send(1, _packet(2, 60), false);
      expect(kinds, [2]);
      expect(media.audioInFlight, 1);
      var videoDone = false;
      final video = media.send(1, _packet(1, 80), false).then((_) => videoDone = true);
      await Future<void>.delayed(Duration.zero);
      expect(kinds, [2, 1], reason: 'sent in order');
      expect(videoDone, isFalse, reason: 'video keeps its backpressure');
      answers[1].complete(status(0));
      await video;
      expect(videoDone, isTrue);
      answers[0].complete(status(0));
      await Future<void>.delayed(Duration.zero);
      expect(media.audioInFlight, 0);
    });

    test('once the UDP keys are done every packet carries the path flag', () async {
      final media = _Probe();
      await media.send(1, _packet(2, 60), false);
      final before = media.send(1, _packet(1, 80), true);
      await Future<void>.delayed(Duration.zero);
      answers[1].complete(status(0));
      await before;
      media.udpPathRule = true;
      await media.send(1, _packet(2, 60), false);
      final after = media.send(1, _packet(1, 80), true);
      await Future<void>.delayed(Duration.zero);
      answers[3].complete(status(0));
      await after;
      expect(kinds, [2, 1, 2, 1]);
      expect(flags, [0, 1, 2, 3]);
      answers[0].complete(status(0));
      answers[2].complete(status(0));
      await Future<void>.delayed(Duration.zero);
    });

    test('audio in flight is bounded; extra packets drop', () async {
      final media = _Probe();
      for (var n = 0; n < AndroidRemoteMedia.maxAudioInFlight + 10; n++) {
        await media.send(1, _packet(2, 60), false);
      }
      expect(kinds.length, AndroidRemoteMedia.maxAudioInFlight);
      for (final answer in answers) {
        answer.complete(status(0));
      }
      await Future<void>.delayed(Duration.zero);
      expect(media.audioInFlight, 0);
      await media.send(1, _packet(2, 60), false);
      expect(kinds.length, AndroidRemoteMedia.maxAudioInFlight + 1);
    });

    test('a full queue drops audio quietly; a refusal reaches the next packet of its lease', () async {
      final media = _Probe();
      await media.send(1, _packet(2, 60), false);
      answers[0].complete(status(1));
      await Future<void>.delayed(Duration.zero);
      // media_queue_full for audio is not an error.
      final ok = media.send(1, _packet(1, 80), false);
      await Future<void>.delayed(Duration.zero);
      answers[1].complete(status(0));
      await ok;
      await media.send(1, _packet(2, 60), false);
      answers[2].complete(status(3));
      await Future<void>.delayed(Duration.zero);
      await expectLater(
        media.send(1, _packet(1, 80), false),
        throwsA(isA<PlatformException>().having((e) => e.code, 'code', 'decoder_failed')),
      );
      // Reported once; and never into another lease.
      await media.send(1, _packet(2, 60), false);
      answers[3].complete(status(2));
      await Future<void>.delayed(Duration.zero);
      final other = media.send(2, _packet(1, 80), false);
      await Future<void>.delayed(Duration.zero);
      answers[4].complete(status(0));
      await other;
    });
  });
}

class _Probe extends AndroidRemoteMedia {
  Future<void> send(int lease, Uint8List packet, bool watch) =>
      sendPacket(lease, packet, watch);
}