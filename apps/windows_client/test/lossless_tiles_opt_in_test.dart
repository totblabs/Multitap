import 'dart:async';
import 'dart:convert';
import 'dart:typed_data';

import 'package:flutter_test/flutter_test.dart';
import 'package:multitap_client/android/android_media.dart';
import 'package:multitap_client/data/broker_api.dart';
import 'package:multitap_client/data/remote_session.dart';
import 'package:multitap_client/models/stream_options.dart';

import 'remote_session_test.dart' show SessionSocket, SessionMedia, flush;
import 'track_stream_test.dart' show configured, packet;

/// A media fake whose native engine composites ToTBSeamless tiles-v1.
class TileMedia extends SessionMedia implements LosslessTileMedia {
  @override
  bool get losslessTiles => true;
}

RemoteSession session(SessionSocket socket, SessionMedia media) =>
    RemoteSession(
      connection: BrokerConnection(
        sessionId: 'test-session',
        webSocketUrl: Uri.parse(
          'wss://cloud.example.com/api/v1/sessions/test-session',
        ),
        options: const StreamOptions(),
      ),
      token: 'test-session-token',
      media: media,
      dialer: (uri, token) async => socket,
    );

const info = {
  'type': 'stream.info',
  'codec': 'h264',
  'width': 1280,
  'height': 720,
};
Uint8List idr() =>
    Uint8List.fromList([1, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0x65]);
// A tiles-v1 payload inside an ordinary non-key video envelope.
Uint8List tiles() => Uint8List.fromList([
  1, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, //
  84, 83, 76, 49, 0, 5, 208, 2, 1, 0, 0, 0,
]);

void main() {
  test('Only a tile-compositing engine opts in, through the RTT ping nonce', () async {
    for (final lossless in [true, false]) {
      final socket = SessionSocket();
      final media = lossless ? TileMedia() : SessionMedia();
      final remote = session(socket, media);
      unawaited(remote.start());
      await flush();
      socket.incoming.add(jsonEncode(info));
      await flush();
      socket.incoming.add(idr());
      await Future<void>.delayed(const Duration(milliseconds: 1050));
      final ping = socket.sent.firstWhere((event) => event['type'] == 'ping');
      final id = ping['id'] as String;
      expect(id.startsWith(losslessTilesPingPrefix), lossless);
      expect(id.length, lessThanOrEqualTo(64)); // relay bound for ping ids
      // Released hosts just echo it: RTT still measures.
      socket.incoming.add(jsonEncode({'type': 'pong', 'id': id}));
      await flush();
      expect(remote.rttMs, greaterThanOrEqualTo(0));
      await remote.close();
      await media.events.close();
    }
  });

  test('The opt-in is offered with the first media, ordinary pings keep their cadence', () async {
    for (final lossless in [true, false]) {
      final socket = SessionSocket();
      final media = lossless ? TileMedia() : SessionMedia();
      final remote = session(socket, media);
      unawaited(remote.start());
      await flush();
      socket.incoming.add(jsonEncode(info));
      await flush();
      socket.incoming.add(idr());
      await flush();
      final pings = socket.sent.where((event) => event['type'] == 'ping');
      expect(pings.length, lossless ? 1 : 0);
      if (lossless) {
        expect(
          (pings.single['id'] as String).startsWith(losslessTilesPingPrefix),
          isTrue,
        );
      }
      await remote.close();
      await media.events.close();
    }
  });

  test('마우스 잠금 pauses the tiles at once (lt0-) and every ping says so until the release', () async {
    for (final lossless in [true, false]) {
      final socket = SessionSocket();
      final media = lossless ? TileMedia() : SessionMedia();
      final remote = session(socket, media);
      unawaited(remote.start());
      await flush();
      socket.incoming.add(jsonEncode(info));
      await flush();
      socket.incoming.add(idr());
      await flush();
      List<String> pings() => [
        for (final event in socket.sent)
          if (event['type'] == 'ping') event['id'] as String,
      ];
      expect(pings(), hasLength(lossless ? 1 : 0)); // the opt-in offer
      remote.pauseLosslessTiles(true);
      expect(remote.losslessTilesPaused, isTrue);
      await flush();
      if (lossless) {
        expect(pings(), hasLength(2), reason: 'sent at once');
        expect(pings().last.startsWith(losslessTilesPausePrefix), isTrue);
        expect(pings().last.length, lessThanOrEqualTo(64));
        // Its pong measures RTT like any other.
        socket.incoming.add(jsonEncode({'type': 'pong', 'id': pings().last}));
        await flush();
        expect(remote.rttMs, greaterThanOrEqualTo(0));
      } else {
        expect(pings(), isEmpty, reason: 'nothing to pause');
      }
      // The periodic ping keeps the pause (a plain one without tiles).
      await Future<void>.delayed(const Duration(milliseconds: 1050));
      expect(pings(), isNotEmpty);
      expect(pings().last.startsWith(losslessTilesPausePrefix), lossless);
      expect(pings().last.startsWith(losslessTilesPingPrefix), isFalse);
      final before = pings().length;
      remote.pauseLosslessTiles(true); // a repeated value sends nothing
      await flush();
      expect(pings(), hasLength(before));
      remote.pauseLosslessTiles(false);
      await flush();
      expect(pings(), hasLength(lossless ? before + 1 : before));
      if (lossless) {
        expect(pings().last.startsWith(losslessTilesPingPrefix), isTrue);
      }
      await remote.close();
      await media.events.close();
    }
  });

  test('Engines that cannot composite tiles never opt in', () {
    expect(NativeRemoteMedia().losslessTiles, isFalse);
    expect(AndroidRemoteMedia().losslessTiles, isFalse);
    expect(NativeRemoteMedia(losslessTiles: true).losslessTiles, isTrue);
  });

  test('A tile packet reaches the native engine unchanged as ordinary non-key video', () async {
    final socket = SessionSocket(), media = TileMedia();
    final remote = session(socket, media);
    unawaited(remote.start());
    await flush();
    socket.incoming.add(jsonEncode(info));
    await flush();
    socket.incoming.add(idr());
    socket.incoming.add(tiles());
    await flush();
    expect(remote.closed, isFalse);
    expect(media.packets, hasLength(2));
    expect(media.packets.last, tiles());
    await remote.close();
    await media.events.close();
  });

  test('Track streams accept tile packets between strictly increasing video timestamps', () {
    final stream = configured();
    expect(stream.accept(packet(0, pts: 100), 0), isTrue);
    // The host sends a tile packet at pts T-1 before its video frame T.
    final payload = [84, 83, 76, 49, 128, 2, 104, 1, 1, 0, 0, 0];
    final tile = Uint8List(28 + payload.length)
      ..setRange(0, 28, packet(0, pts: 199, key: false))
      ..setRange(28, 28 + payload.length, payload);
    ByteData.sublistView(tile).setUint32(24, payload.length, Endian.little);
    expect(stream.accept(tile, 1), isTrue);
    expect(stream.accept(packet(0, pts: 200, key: false), 2), isTrue);
    expect(
      () => stream.accept(packet(0, pts: 200, key: false), 3),
      throwsFormatException,
    );
  });
}
