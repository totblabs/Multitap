import 'package:flutter/services.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:multitap_client/android/android_screen.dart';
import 'package:multitap_client/android/android_stream_preparation.dart';
import 'package:multitap_client/android/android_session_keepalive.dart';
import 'package:multitap_client/data/decode_settings.dart';
import 'package:multitap_client/data/stream_capacity.dart';
import 'package:multitap_client/data/stream_size.dart';
import 'package:multitap_client/models/stream_options.dart';

import 'android_large_desktop_test.dart'
    show openLargeDesktopStream, closeLargeDesktopStreamView;

const _optionalPenChannel = MethodChannel('multitap/pen-input');

const _smallDecoder = DecoderLimits(
  maxWidth: 1920,
  maxHeight: 1080,
  maxPixels: 2097152,
  maxPixelRate: 2097152 * 60,
);

void main() {
  TestWidgetsFlutterBinding.ensureInitialized();
  // Importing openLargeDesktopStream does not run the other file's main/setup.
  // This view awaits its own keepalive acquisition before calling the fake broker.
  setUp(() {
    // ACK only: this fixture does not advertise native pen readiness.
    TestDefaultBinaryMessengerBinding.instance.defaultBinaryMessenger
        .setMockMethodCallHandler(_optionalPenChannel, (_) async => null);
    TestDefaultBinaryMessengerBinding.instance.defaultBinaryMessenger
        .setMockMethodCallHandler(AndroidSessionKeepalive.channel, (
          call,
        ) async {
          return call.method == 'start' ? 'started' : true;
        });
  });
  tearDown(() {
    TestDefaultBinaryMessengerBinding.instance.defaultBinaryMessenger
        .setMockMethodCallHandler(_optionalPenChannel, null);
    TestDefaultBinaryMessengerBinding.instance.defaultBinaryMessenger
        .setMockMethodCallHandler(AndroidSessionKeepalive.channel, null);
  });
  for (final size in [
    const Size(3840, 2160),
    const Size(5120, 1440),
    const Size(2880, 5120),
  ]) {
    test(
      'v2 keeps actual workspace $size independently of smaller encoded decoder envelope',
      () {
        final policy = AndroidScreenPolicy(
          choice: const AndroidScreenChoice(
            resolution: AndroidResolution.device,
          ),
        );
        final plan = AndroidViewportPlan.of(
          size,
          1,
          policy,
          hostScale: true,
          portraitLimits: _smallDecoder,
          fullWorkspace: true,
        );
        expect(plan.size, size);
        final requested = androidViewportOptions(
          plan.size,
          const StreamOptions(),
          fullWorkspace: true,
        );
        expect(requested.maxWidth, size.width);
        expect(requested.maxHeight, size.height);
      },
    );
  }

  test('v1 retains released fallback bounds rather than sending an unnegotiated full workspace', () {
    final plan = AndroidViewportPlan.of(
      const Size(5120, 1440),
      1,
      const AndroidScreenPolicy(
        choice: AndroidScreenChoice(resolution: AndroidResolution.device),
      ),
      hostScale: true,
      portraitLimits: _smallDecoder,
    );
    expect(plan.width, lessThanOrEqualTo(1920));
    expect(plan.height, lessThanOrEqualTo(1080));
  });

  test('v2 rejects nonfinite and integer-overflow geometry without using decoder caps as a workspace limit', () {
    for (final size in [
      const Size(double.infinity, 1080),
      const Size(-1, 1080),
      const Size(1e20, 1e20),
    ]) {
      expect(
        () => androidViewportOptions(
          size,
          const StreamOptions(),
          fullWorkspace: true,
        ),
        throwsA(isA<Exception>()),
      );
    }
  });

  testWidgets(
    'actual Android route requests a 5120-wide source without using decoder envelope as workspace ceiling',
    (tester) async {
      const quality = StreamOptions();
      final hardware = DecodeSettings.hardwareDecode;
      final queries = <Map<String, Object>>[];
      final acceptedShapes = <(int, int)>[];
      // Exercise the production exact-candidate search, with a synthetic decoder
      // refusing pictures outside this fixture's actual supported envelope.
      // This supplies the fake broker's grant; no platform/decoder/HTTP is run.
      final admission = await nativeStreamAdmission(
        const [StreamSourceSize(5120, 1440)],
        quality.fps,
        hardware: hardware,
        probe: (request) async {
          queries.add(request);
          final track = (request['tracks'] as List).single as Map;
          final width = track['width'] as int, height = track['height'] as int;
          final fps = track['fps'] as int;
          final accepted = _smallDecoder.fits(width, height, fps);
          if (accepted) acceptedShapes.add((width, height));
          return {
            'schema': 1,
            'validation': 'exact-candidate',
            'accepted': accepted,
            'reason': accepted ? null : 'unsupported_size',
            'decoder': 'fixture-avc-decoder',
            ..._smallDecoder.toJson(),
            'tracks': [
              {
                'width': width,
                'height': height,
                'fps': fps,
                'accepted': accepted,
                'reason': accepted ? null : 'unsupported_size',
              },
            ],
          };
        },
      );
      expect(
        queries.length,
        greaterThan(1),
        reason: 'the full source cannot fit the fixture decoder',
      );
      for (final query in queries) {
        expect(query['schema'], 1);
        expect(query['hardware'], hardware);
        final track = (query['tracks'] as List).single as Map;
        expect((track['sourceWidth'], track['sourceHeight']), (5120, 1440));
        expect(track['fps'], quality.fps);
      }
      final fullCandidate = (queries.first['tracks'] as List).single as Map;
      expect((fullCandidate['width'], fullCandidate['height']), (5120, 1440));
      final picture = admission.pictures.single;
      expect(
        acceptedShapes,
        contains((picture.width, picture.height)),
        reason: 'the encoded picture must have an actual positive exact-query result',
      );
      expect(
        _smallDecoder.fits(picture.width, picture.height, quality.fps),
        isTrue,
      );
      expect(admission.hardwareDecode, hardware);
      final h = await openLargeDesktopStream(
        tester,
        physical: const Size(5120, 1440),
        limits: admission.limits,
        quality: quality,
        streamSizeV2: true,
        firstFrame: false,
      );
      expect(h.starts, hasLength(1));
      expect(h.starts.single.maxWidth, 5120);
      expect(h.starts.single.maxHeight, 1440);
      await closeLargeDesktopStreamView(tester);
    },
  );
}
