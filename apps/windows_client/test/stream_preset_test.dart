// Quality modes (backlog #99): what each mode asks for and how a request
// reads back as a mode. Pure model; no widgets, sockets or VMs.
import 'package:flutter_test/flutter_test.dart';
import 'package:multitap_client/android/android_store.dart';
import 'package:multitap_client/models/stream_options.dart';

void main() {
  const adaptive = 100000000;

  test('작업 is today\'s default: 60 fps with the automatic rate', () {
    final work = StreamPreset.work.options;
    expect(work.fps, 60);
    expect(work.automaticBitrate, isTrue);
    expect(
      StreamPreset.of(StreamOptions.recommended, adaptive),
      StreamPreset.work,
    );
    expect(StreamPreset.of(StreamOptions.recommended, null), StreamPreset.work);
    // Android's default too.
    expect(
      StreamPreset.of(AndroidStreamQuality.recommended.options, adaptive),
      StreamPreset.work,
    );
  });

  test(
    '절약 asks for 30 fps at a fixed 8 Mbps, an Android choice, on every Host',
    () {
      final saver = StreamPreset.saver.options;
      expect(saver.fps, 30);
      expect(saver.bitrateMode, StreamBitrateMode.fixed);
      expect(saver.bitrate, 8000000);
      expect(AndroidStreamQuality.bitrates, contains(8));
      for (final ceiling in [null, 20000000, adaptive]) {
        final sent = saver.negotiated(ceiling);
        expect(sent.fps, 30);
        expect(sent.bitrate, 8000000);
        expect(sent.automaticBitrate, isFalse);
        expect(sent.toJson()['bitrateMode'], isNull, reason: 'released wire');
      }
    },
  );

  test('No mode asks for more than the wire allows or touches sound', () {
    for (final mode in StreamPreset.values) {
      expect([30, 60], contains(mode.options.fps));
      expect(mode.options.toJson, returnsNormally);
      // Sound is the user's own switch: a mode never reads it.
      for (final audio in [true, false]) {
        final choices = StreamOptions(
          fps: mode.options.fps,
          bitrate: mode.options.bitrate,
          bitrateMode: mode.options.bitrateMode,
          audio: audio,
        );
        expect(StreamPreset.of(choices, adaptive), mode);
      }
    }
  });

  test('A request reads as the mode it negotiates to, else 사용자 설정', () {
    StreamOptions fixed(int fps, int mbps, {String monitor = '0'}) =>
        StreamOptions(fps: fps, bitrate: mbps * 1000000, monitor: monitor);
    // A released Host turns 작업 into a fixed 20 Mbps: the same request.
    expect(StreamPreset.of(fixed(60, 20), null), StreamPreset.work);
    expect(StreamPreset.of(fixed(60, 20), adaptive), isNull);
    // The connection's echo of an automatic session (ceiling as bitrate).
    expect(
      StreamPreset.of(
        const StreamOptions(
          bitrate: adaptive,
          bitrateMode: StreamBitrateMode.auto,
        ),
        adaptive,
      ),
      StreamPreset.work,
    );
    // Independent outputs keep a fixed total rate: 작업 is 20 Mbps there.
    expect(
      StreamPreset.of(fixed(60, 20, monitor: 'all'), adaptive),
      StreamPreset.work,
    );
    expect(StreamPreset.of(fixed(30, 8), adaptive), StreamPreset.saver);
    expect(StreamPreset.of(fixed(30, 8), null), StreamPreset.saver);
    for (final custom in [
      fixed(60, 8),
      fixed(30, 12),
      const StreamOptions(fps: 30, bitrateMode: StreamBitrateMode.auto),
    ]) {
      expect(StreamPreset.of(custom, adaptive), isNull);
    }
  });
}
