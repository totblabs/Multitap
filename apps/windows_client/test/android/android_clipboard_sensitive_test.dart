// A clip marked sensitive (ClipDescription.EXTRA_IS_SENSITIVE) is never
// shared automatically. Source check: the
// engine is compiled separately and the session side is played by a fake in
// android_clipboard_sync_test.dart, where "own" already means "nothing to
// share".
import 'dart:io';

import 'package:flutter_test/flutter_test.dart';

void main() {
  test('automatic capture answers a sensitive clip as nothing to share', () {
    final source = File(
      'android/app/src/main/kotlin/me/totb/multitap_client/AndroidClipboard.kt',
    ).readAsStringSync();
    expect(source, contains('getBoolean(ClipDescription.EXTRA_IS_SENSITIVE, false)'));
    expect(
      source,
      contains('if (automatic && description().let { own(it) || sensitive(it) })'),
    );
    expect(
      source,
      contains('if (automatic && clip != null && sensitive(clip.description))'),
    );
  });
}
