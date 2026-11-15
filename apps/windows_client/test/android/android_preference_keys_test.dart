// Every preference the Android store saves is a key the native store accepts
// (AndroidPreferencePolicy.kt). A key missing there fails its save with
// '저장하지 못했어요' (카드 크기, 2026-11-15), and in a call that saves
// several, every save after it is lost too (the session settings stopped at
// the tool bubble's edge: 도움말 and the screen choices were never kept).
import 'dart:io';

import 'package:flutter_test/flutter_test.dart';
import 'package:multitap_client/android/android_screen.dart';

void main() {
  test('every key the Android store saves is on the native allow-list', () {
    final policy = File(
      'android/app/src/main/kotlin/me/totb/multitap_client/'
      'AndroidPreferencePolicy.kt',
    ).readAsStringSync();
    final fixed = RegExp(r'private val fixed = setOf\(([^)]*)\)')
        .firstMatch(policy)!
        .group(1)!;
    final allowed = {
      for (final match in RegExp(r'"([^"]+)"').allMatches(fixed))
        match.group(1)!,
    };
    final store = File('lib/android/android_store.dart').readAsStringSync();
    // Every fixed key ('organize.…' is its own namespace, checked natively).
    final saved = {
      for (final match in RegExp(r"_save\(\s*'([^'$]+)'").allMatches(store))
        match.group(1)!,
    };
    expect(saved, containsAll(['launcher.cards', 'session.toolbar.edge']));
    expect(saved.difference(allowed), isEmpty);
    // 'session.screen.${kind.name}'.
    expect(store, contains(r"_save('session.screen.${entry.key.name}'"));
    for (final kind in AndroidDeviceClass.values) {
      expect(allowed, contains('session.screen.${kind.name}'));
    }
    // Only these two are written with a computed key.
    expect(RegExp(r"_save\(\s*'[^']*\$").allMatches(store), hasLength(2));
  });
}
