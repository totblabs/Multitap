import 'dart:math';
import 'dart:typed_data';

import 'package:flutter/services.dart';

import '../data/audio_stats.dart';
import '../data/remote_cursor.dart';
import '../data/remote_session.dart';
import 'android_cursor.dart';

// The 연결 통계 rows made from [AndroidRemoteMedia.diagnostics].
export 'android_engine_stats.dart';

/// media-wire-v1: the binary channel the Android engine answers on a
/// background queue (AndroidMedia.kt). In this Flutter, Dart runs on the
/// Android main thread; the shared `push` method encoded every packet into a
/// growing map buffer there, and Kotlin decoded and copied it there again.
const androidMediaChannel = BasicMessageChannel<ByteData>(
  'multitap/remote/media',
  BinaryCodec(),
);

/// The one-byte answers of media-wire-v1 other than 0 (taken), as the
/// PlatformException codes the shared session handles (AndroidMediaStatus).
const androidMediaStatusCodes = <int, String>{
  1: 'media_queue_full',
  2: 'session_replaced',
  3: 'decoder_failed',
  4: 'invalid_packet',
};

/// Encodes media-wire-v1 into one reused buffer: [0] version 1, [1] flags
/// (bit 0: input-latency-v1 watched video packet; bit 1: udp-media-v1 keys
/// are done, so the packet follows the path rule), [2..7] zero, [8..15] the
/// lease (little endian), then the media-v1 packet. The engine copies a
/// message while it is sent, and the session sends one packet at a time, so
/// the buffer is reused instead of allocating one per packet.
class AndroidMediaWire {
  static const header = 16;
  static const _maxPacket = 4 * 1024 * 1024;
  Uint8List _buffer = Uint8List(0);
  ByteData _view = ByteData(0);

  ByteData encode(
    int lease,
    Uint8List packet, {
    bool watch = false,
    bool path = false,
  }) {
    final size = header + packet.length;
    if (_buffer.length < size) {
      // Room for somewhat larger packets, never beyond the largest one.
      _buffer = Uint8List(max(size, min(size + size ~/ 2, header + _maxPacket)));
      _view = ByteData.sublistView(_buffer);
    }
    _buffer[0] = 1;
    _buffer[1] = (watch ? 1 : 0) | (path ? 2 : 0);
    _buffer.fillRange(2, 8, 0);
    _view.setInt64(8, lease, Endian.little);
    _buffer.setRange(header, size, packet);
    return ByteData.sublistView(_buffer, 0, size);
  }
}

/// Null when the engine took the packet, otherwise the error code. No answer
/// (an engine without the channel) is an error, never a silent drop.
String? androidMediaReplyCode(ByteData? reply) {
  if (reply == null || reply.lengthInBytes != 1) return 'invalid_media_reply';
  final status = reply.getUint8(0);
  return status == 0
      ? null
      : androidMediaStatusCodes[status] ?? 'invalid_media_reply';
}

/// Retains Android-specific codec/audio diagnostics without changing relay data.
///
/// The shared [NativeRemoteMedia] hands cursor-v1 records to a native
/// `cursor` method that only the Windows runner implements. On Android an
/// unimplemented channel method raises MissingPluginException, which the
/// session does not treat as a cursor failure, so a granted separated-cursor
/// session would end at its first record (and the host leaves the cursor out
/// of the video). Android presents the remote cursor in Flutter instead.
class AndroidRemoteMedia extends NativeRemoteMedia
    implements SharedAudioCounterMedia {
  AndroidRemoteMedia({super.audioEnabled});

  /// The jitter buffer's counters are the app's totals (one per AndroidMedia),
  /// so every session of this app reads them against the same baselines.
  static final _audioBaselines = <String, num>{};
  @override
  Map<String, num> get audioCounterBaselines => _audioBaselines;

  Map<String, dynamic> diagnostics = const {};
  final cursorState = AndroidCursorState();
  final _wire = AndroidMediaWire();

  /// False once the engine reported that no Opus decoder of this device
  /// plays (audio-path-v1 `audioOpusUnusable`, for this app run): connections
  /// from then on ask for PCM (HttpBrokerApi.audioOpusDecodable).
  static bool opusDecodable = true;

  /// One engine statistics reading: what it says of the device's Opus decoders.
  static void observeAudio(Map<String, dynamic> stats) {
    if (stats['audioOpusUnusable'] == true) opusDecodable = false;
  }

  @override
  Future<Map<String, dynamic>> stats() async {
    final result = await super.stats();
    diagnostics = result;
    observeAudio(result);
    return result;
  }

  /// media-wire-v1 instead of the `push` method (same lease rules, same
  /// answers: media_queue_full is backpressure the session waits out).
  @override
  Future<void> sendPacket(int lease, Uint8List packet, bool watch) async {
    final failed = _audioFailure;
    if (failed != null && failed.$1 == lease) {
      _audioFailure = null;
      throw PlatformException(code: failed.$2);
    }
    // Audio is not awaited: its replies came back through the same thread as
    // the UI, 100 times a second, and every video packet, acknowledgement and
    // control message of the session waited behind them. The packet is still
    // copied into the channel before this returns (order is kept); a full
    // queue drops that audio packet, and any other refusal is raised by the
    // next packet of this lease.
    if (!watch && packet.isNotEmpty && packet[0] == 2) {
      _sendAudio(lease, packet);
      return;
    }
    final code = androidMediaReplyCode(
      await androidMediaChannel.send(
        _wire.encode(lease, packet, watch: watch, path: udpPathRule),
      ),
    );
    if (code != null) throw PlatformException(code: code);
  }

  /// Audio packets sent and not yet answered (bounded; beyond it they drop).
  int audioInFlight = 0;
  static const maxAudioInFlight = 32;
  (int, String)? _audioFailure;

  void _sendAudio(int lease, Uint8List packet) {
    if (audioInFlight >= maxAudioInFlight) return;
    audioInFlight++;
    androidMediaChannel
        .send(_wire.encode(lease, packet, path: udpPathRule))
        .then(
          (reply) {
            audioInFlight--;
            final code = androidMediaReplyCode(reply);
            if (code != null && code != 'media_queue_full') {
              _audioFailure ??= (lease, code);
            }
          },
          onError: (Object _) {
            audioInFlight--;
            _audioFailure ??= (lease, 'invalid_media_reply');
          },
        );
  }

  @override
  Future<void> cursor(int epoch, Uint8List record) async =>
      cursorState.record(record);

  /// The Flutter overlay already knows the picture rectangle.
  @override
  Future<void> cursorView(CursorView view) async {}

  @override
  Future<int> start(int width, int height) {
    cursorState.reset();
    _audioFailure = null;
    return super.start(width, height);
  }

  @override
  Future<void> stop() {
    cursorState.reset();
    return super.stop();
  }

  void dispose() => cursorState.dispose();
}
