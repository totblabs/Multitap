import 'dart:io';

import 'package:flutter_test/flutter_test.dart';

void main() {
  test('one APK has both phone and TV home launchers without requiring TV or touch', () {
    final manifest = File('android/app/src/main/AndroidManifest.xml')
        .readAsStringSync();
    final main = RegExp(
      r'<activity\b[^>]*android:name="\.MainActivity"[\s\S]*?</activity>',
    ).firstMatch(manifest)!.group(0)!;
    expect(main, contains('android.intent.action.MAIN'));
    expect(main, contains('android.intent.category.LAUNCHER'));
    expect(main, contains('android.intent.category.LEANBACK_LAUNCHER'));
    for (final feature in [
      'android.software.leanback',
      'android.hardware.touchscreen',
    ]) {
      final declaration = RegExp(
        '<uses-feature\\b[^>]*android:name="$feature"[^>]*/>',
      ).firstMatch(manifest)!.group(0)!;
      expect(declaration, contains('android:required="false"'));
    }
    expect(manifest, contains('android:banner="@drawable/multitap_tv_banner"'));
  });

  test('TV home banner resolves to a bundled landscape vector with the power-strip mark', () {
    final banner = File(
      'android/app/src/main/res/drawable/multitap_tv_banner.xml',
    ).readAsStringSync();
    expect(banner, contains('<vector'));
    expect(banner, contains('android:viewportWidth="320"'));
    expect(banner, contains('android:viewportHeight="180"'));
    // The app icon colours: the dark mark on amber.
    expect(banner, contains('M47 38H89a23 23 0 0 1 0 46H47a23 23 0 0 1 0-46Z'));
    expect(banner, contains('#FFF5A623'));
    expect(banner, contains('#FF1E1503'));
    final content = banner.replaceAll(RegExp(r'xmlns(?::\w+)?="[^"]*"'), '');
    expect(content, isNot(contains('http://')));
    expect(content, isNot(contains('https://')));
  });
}
