import 'package:flutter_test/flutter_test.dart';
import 'package:multitap_client/data/stream_capacity.dart';
import 'package:multitap_client/data/stream_size.dart';
import 'package:multitap_client/data/video_geometry.dart';
import 'package:multitap_client/data/decode_settings.dart';

void main() {
  TestWidgetsFlutterBinding.ensureInitialized();
  Map<String, Object?> reply(
    Map<String, Object> request,
    bool accepted, {
    String? reason,
  }) => {
    'schema': 1,
    'validation': 'exact-candidate',
    'accepted': accepted,
    'reason': reason,
    'tracks': [
      for (final raw in request['tracks'] as List)
        {...raw as Map, 'accepted': accepted, 'reason': reason},
    ],
  };
  test('native accepted 5K, ultrawide and portrait requests retain exact workspace', () async {
    for (final source in [
      const StreamSourceSize(5120, 2880),
      const StreamSourceSize(5120, 1440),
      const StreamSourceSize(2880, 5120),
    ]) {
      var calls = 0;
      final admission = await nativeStreamAdmission(
        [source],
        60,
        hardware: false,
        probe: (request) async {
          calls++;
          expect(request['hardware'], false);
          final t = (request['tracks'] as List).single as Map;
          expect(t['sourceWidth'], source.width);
          expect(t['sourceHeight'], source.height);
          return reply(request, true);
        },
      );
      expect(calls, 1);
      expect(admission.pictures.single.width, source.width);
      expect(admission.pictures.single.height, source.height);
      expect(admission.limits.valid, true);
    }
  });
  test('smaller encoded pictures never lower workspace or assume a max-size rectangle', () async {
    var calls = 0;
    final admission = await nativeStreamAdmission(
      [const StreamSourceSize(5120, 2880)],
      60,
      probe: (request) async {
        calls++;
        final t = (request['tracks'] as List).single as Map;
        expect(t['sourceWidth'], 5120);
        expect(t['sourceHeight'], 2880);
        final accepted =
            (t['width'] as int) <= 3840 && (t['height'] as int) <= 2160;
        return reply(
          request,
          accepted,
          reason: accepted ? null : 'unsupported_size',
        );
      },
    );
    expect(calls, greaterThan(1));
    expect(admission.pictures.single.width, lessThanOrEqualTo(3840));
    expect(admission.pictures.single.height, lessThanOrEqualTo(2160));
  });
  test('two 4K candidates use one aggregate native preflight', () async {
    final admission = await nativeStreamAdmission(
      [const StreamSourceSize(3840, 2160), const StreamSourceSize(3840, 2160)],
      30,
      probe: (request) async {
        expect((request['tracks'] as List).length, 2);
        return reply(request, true);
      },
    );
    expect(admission.pictures.length, 2);
  });
  test('unknown devices and insufficient instances fail without a larger invented ceiling', () async {
    for (final reason in ['capability_unknown', 'insufficient_instances']) {
      var calls = 0;
      await expectLater(
        nativeStreamAdmission(
          [const StreamSourceSize(5120, 2880)],
          60,
          probe: (request) async {
            calls++;
            return reply(request, false, reason: reason);
          },
        ),
        throwsA(
          isA<StreamCapacityException>().having(
            (e) => e.reason,
            'reason',
            reason,
          ),
        ),
      );
      expect(calls, 1);
    }
  });
  test(
    'stale/mismatched exact replies cannot authorize another size',
    () async {
      await expectLater(
        nativeStreamAdmission(
          [const StreamSourceSize(5120, 2880)],
          60,
          probe: (request) async => {
            ...reply(request, true),
            'tracks': [
              {'width': 1920, 'height': 1080, 'fps': 60, 'accepted': true},
            ],
          },
        ),
        throwsA(isA<StreamCapacityException>()),
      );
    },
  );
  test('native limits preserve supported values beyond 4K, invalid arithmetic is rejected', () {
    final limits = DecoderLimits.fromNative({
      'maxWidth': 8192,
      'maxHeight': 8192,
      'maxPixels': 67108864,
      'maxPixelRate': 4026531840,
    });
    expect(limits?.maxWidth, 8192);
    expect(limits?.maxPixels, 67108864);
    expect(limits?.maxPixelRate, 4026531840);
    expect(videoGeometry(maximumGeometrySide, maximumGeometrySide), false);
    expect(videoGeometry(5121, 2880), false);
  });
  test(
    'one admission retains the decoding intent across asynchronous fit probes',
    () async {
      final previous = DecodeSettings.hardwareDecode;
      try {
        DecodeSettings.hardwareDecode = true;
        var calls = 0;
        final result = await nativeStreamAdmission(
          [const StreamSourceSize(5120, 2880)],
          60,
          probe: (request) async {
            expect(request['hardware'], true);
            calls++;
            DecodeSettings.hardwareDecode = false;
            final t = (request['tracks'] as List).single as Map;
            final accepted = (t['width'] as int) <= 3840;
            return reply(
              request,
              accepted,
              reason: accepted ? null : 'stream_decoder_type',
            );
          },
        );
        expect(calls, greaterThan(1));
        expect(result.hardwareDecode, true);
        expect(
          result.limits.maxPixelRate,
          0,
          reason: 'type admission is not a throughput measurement',
        );
      } finally {
        DecodeSettings.hardwareDecode = previous;
      }
    },
  );
}
