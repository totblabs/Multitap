// The exported launcher activity ignores Flutter's launch extras and deep
// links. Source checks only: the overrides were
// compiled against the Flutter embedding; a device run is separate.
import 'dart:io';

import 'package:flutter_test/flutter_test.dart';

void main() {
  final manifest = File('android/app/src/main/AndroidManifest.xml').readAsStringSync();
  final activity = File(
    'android/app/src/main/kotlin/me/totb/multitap_client/MainActivity.kt',
  ).readAsStringSync();

  test('the launcher activity turns Flutter deep links off', () {
    final main = RegExp(
      r'<activity\b[^>]*android:name="\.MainActivity"[\s\S]*?</activity>',
    ).firstMatch(manifest)!.group(0)!;
    expect(main, contains('android:exported="true"'));
    expect(
      main,
      matches(RegExp(
        r'<meta-data\s+android:name="flutter_deeplinking_enabled"\s+android:value="false"\s*/>',
      )),
    );
  });

  test('nothing is backed up or moved to another device', () {
    final application = RegExp(r'<application\b[^>]*>').firstMatch(manifest)!.group(0)!;
    expect(application, contains('android:allowBackup="false"'));
    expect(application, contains('android:fullBackupContent="@xml/backup_rules"'));
    expect(application, contains('android:dataExtractionRules="@xml/data_extraction_rules"'));
    const domains = [
      'root', 'file', 'database', 'sharedpref', 'external',
      'device_root', 'device_file', 'device_database', 'device_sharedpref',
    ];
    final rules = File('android/app/src/main/res/xml/data_extraction_rules.xml').readAsStringSync();
    for (final section in ['cloud-backup', 'device-transfer']) {
      final body = RegExp('<$section>([\\s\\S]*?)</$section>').firstMatch(rules)!.group(1)!;
      for (final domain in domains) {
        expect(body, contains('<exclude domain="$domain" path="." />'), reason: '$section $domain');
      }
      expect(body, isNot(contains('<include')));
    }
    final backup = File('android/app/src/main/res/xml/backup_rules.xml').readAsStringSync();
    for (final domain in domains) {
      expect(backup, contains('<exclude domain="$domain" path="." />'));
    }
    expect(backup, isNot(contains('<include')));
  });

  test('MainActivity ignores every Flutter launch extra', () {
    for (final line in [
      'override fun getBackgroundMode() = io.flutter.embedding.android.FlutterActivityLaunchConfigs.BackgroundMode.opaque',
      'override fun getCachedEngineId(): String? = null',
      'override fun getCachedEngineGroupId(): String? = null',
      'override fun getInitialRoute(): String? = null',
      'override fun getDartEntrypointFunctionName(): String = "main"',
      'override fun getDartEntrypointArgs(): List<String>? = null',
      'override fun shouldHandleDeeplinking(): Boolean = false',
      'override fun shouldDestroyEngineWithHost(): Boolean = true',
      'override fun getFlutterShellArgs()',
    ]) {
      expect(activity, contains(line));
    }
  });
}
