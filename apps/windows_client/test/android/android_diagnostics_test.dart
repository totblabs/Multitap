import 'package:flutter_test/flutter_test.dart';
import 'package:multitap_client/android/android_diagnostics.dart';

void main() {
  test(
    'diagnostics identify the actual APK preview without a version constant',
    () {
      expect(androidDiagnosticVersion('0.1.0-preview.26'), '0.1.0+26');
      expect(androidDiagnosticVersion('1.2.3-preview.517'), '1.2.3+517');
      expect(androidDiagnosticVersion('1.2.3-preview'), '1.2.3+0');
      expect(androidDiagnosticVersion('1.2.3'), '1.2.3');
    },
  );

  test('unknown and invalid APK versions remain unknown', () {
    for (final value in <String?>[
      null,
      '',
      'unknown',
      '1.2.3-preview.0',
      '1.2.3-preview.01',
      '1.2.3-preview.26\n',
      '1.2.3+26',
      '1.2.3-preview.26-private',
    ]) {
      expect(androidDiagnosticVersion(value), isNull, reason: '$value');
    }
  });
}
