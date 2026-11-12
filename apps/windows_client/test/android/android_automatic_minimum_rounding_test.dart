// Private SOURCE fixture: physical API units, no live Android device or stream.
import 'package:flutter/widgets.dart' show Size;
import 'package:flutter_test/flutter_test.dart';
import 'package:multitap_client/android/android_screen.dart';
import 'package:multitap_client/android/android_stream_preparation.dart';
import 'package:multitap_client/data/broker_api.dart';
import 'package:multitap_client/data/stream_size.dart';
import 'package:multitap_client/models/stream_options.dart';

const limits = DecoderLimits(maxWidth: 3840, maxHeight: 3840,
  maxPixels: 8294400, maxPixelRate: 0);
const phone = AndroidScreenPolicy();

void main() {
  test('binding180px minimum retains its exact algebraic boundary before even floor', () {
    // 186 / (186 / 180) is179.99999999999997 in the released operation order.
    // The mathematical minimum is180; width still uses normal even-down fit.
    expect(phone.recommendedSize(const Size(640, 186), 4), const Size(618, 180));
  });

  test('public viewport and broker options admit the physically feasible boundary', () {
    final plan = AndroidViewportPlan.of(const Size(640, 186), 4, phone,
      hostScale: true, portraitLimits: limits);
    expect(plan.size, const Size(618, 180));
    expect(plan.pixelScale, 0);
    final options = androidViewportOptions(plan.size, const StreamOptions(), portrait: true);
    expect((options.maxWidth, options.maxHeight), (618, 180));
    expect((plan.width - 640 * plan.height / 186).abs(), lessThan(2));
  });

  test('automatic menu uses the same valid boundary as viewport planning', () {
    final options = androidResolutionOptions(const Size(640, 186),
      policy: phone, devicePixelRatio: 4);
    expect(options.first.resolution, AndroidResolution.auto);
    expect(options.first.size, const Size(618, 180));
  });

  test('logical640x186 atDPR4 is a distinct larger physical viewport', () {
    // These API arguments are PHYSICAL pixels. No logical/physical conflation.
    const physical = Size(2560, 744);
    expect(phone.recommendedSize(physical, 4), const Size(1024, 296));
    expect(AndroidViewportPlan.of(physical, 4, phone,
      hostScale: true, portraitLimits: limits).size, const Size(1024, 296));
  });

  test('width binding and exact native minimum retain their existing values', () {
    expect(phone.recommendedSize(const Size(500, 300), 4), const Size(320, 192));
    expect(phone.recommendedSize(const Size(320, 180), 4), const Size(320, 180));
    expect(phone.recommendedSize(const Size(640, 360), 1), const Size(640, 360));
  });

  test('genuinely subminimum native inputs remain invalid without upsampling', () {
    for (final physical in [const Size(319.999, 186), const Size(640, 179.999)]) {
      expect(() => AndroidViewportPlan.of(physical, 4, phone,
        hostScale: true, portraitLimits: limits), throwsA(isA<BrokerException>()));
    }
  });

  test('long-edge safety still rejects an aspect with no feasible minimum', () {
    expect(phone.recommendedSize(const Size(4000, 180), 4), const Size(2560, 114));
    expect(() => AndroidViewportPlan.of(const Size(4000, 180), 4, phone,
      hostScale: true, portraitLimits: limits), throwsA(isA<BrokerException>()));
  });

  test('decoder safety is not raised to force180px after proportional fitting', () {
    const tight = DecoderLimits(maxWidth: 512, maxHeight: 180,
      maxPixels: 92160, maxPixelRate: 0);
    expect(() => AndroidViewportPlan.of(const Size(640, 186), 4, phone,
      hostScale: true, portraitLimits: tight), throwsA(isA<BrokerException>()));
  });

  test('manual high resolutions and saved manual DPI retain exact requested choices', () {
    for (final edge in AndroidResolution.longEdges) {
      final choice = AndroidScreenChoice(scale: 175, resolution: AndroidResolution.longEdge(edge));
      expect(AndroidScreenChoice.parse(choice.encode()).forDevice(AndroidDeviceClass.phone), choice);
      final plan = AndroidViewportPlan.of(const Size(3840, 2160), 4,
        AndroidScreenPolicy(choice: choice), hostScale: true, portraitLimits: limits);
      expect(plan.width, edge);
      expect(plan.height, edge * 9 ~/ 16 ~/ 2 * 2);
      expect(plan.displayScale, 175);
    }
  });

  test('legacy integer recommendation and TV native behavior stay unchanged', () {
    final legacy = AndroidViewportPlan.of(const Size(640, 186), 4, phone,
      hostScale: false, portraitLimits: limits);
    expect(legacy.size, const Size(640, 186)); expect(legacy.pixelScale, 1);
    expect(legacy.displayScale, isNull);
    const tv = AndroidScreenPolicy(deviceClass: AndroidDeviceClass.television);
    expect(tv.recommendedSize(const Size(640, 186), 4), const Size(640, 186));
  });

  test('ordinary phone and measured tablet adaptive recommendations are unchanged', () {
    expect(phone.recommendedSize(const Size(2340, 1080), 3), const Size(1248, 576));
    const tablet = AndroidScreenPolicy(deviceClass: AndroidDeviceClass.tablet,
      window: Size(1280, 800), density: AndroidScreenDensity(
        xdpi: 274.3, ydpi: 274.3, width: 2560, height: 1600));
    expect(tablet.recommendedSize(const Size(2560, 1600), 2), const Size(1462, 914));
  });
}
