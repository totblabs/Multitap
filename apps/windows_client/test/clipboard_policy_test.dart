import 'package:flutter_test/flutter_test.dart';
import 'package:multitap_client/data/clipboard_policy.dart';

Map<String, bool> policyJson(int mask) => {
  'textToRemote': mask & 1 != 0,
  'textToLocal': mask & 2 != 0,
  'filesToRemote': mask & 4 != 0,
  'filesToLocal': mask & 8 != 0,
};

void main() {
  test('missing policy denies all text/file directions', () {
    for (final policy in [
      const ClipboardPolicy(),
      ClipboardPolicy.fromJson(null),
    ]) {
      expect(policy.any, isFalse);
      expect(policy.toRemote, isFalse);
      expect(policy.toLocal, isFalse);
      for (final format in ['text', 'files', 'unknown']) {
        expect(policy.allows(format, toRemote: true), isFalse);
        expect(policy.allows(format, toRemote: false), isFalse);
      }
    }
  });

  test('four permissions remain independent across all sixteen policies', () {
    for (var mask = 0; mask < 16; mask++) {
      final json = policyJson(mask), policy = ClipboardPolicy.fromJson(json);
      expect(policy.allows('text', toRemote: true), json['textToRemote']);
      expect(policy.allows('text', toRemote: false), json['textToLocal']);
      expect(policy.allows('files', toRemote: true), json['filesToRemote']);
      expect(policy.allows('files', toRemote: false), json['filesToLocal']);
      expect(policy.toRemote, mask & 5 != 0);
      expect(policy.toLocal, mask & 10 != 0);
      expect(policy.any, mask != 0);
      expect(policy.allows('text/html', toRemote: true), isFalse);
    }
  });

  test('malformed or extra policy fields never coerce into permission', () {
    for (final value in [
      true,
      15,
      'all',
      <Object?>[],
      <String, bool>{},
      {'textToRemote': true},
      {...policyJson(0), 'textToLocal': 'true'},
      {...policyJson(0), 'filesToRemote': 1},
      {...policyJson(0), 'filesToLocal': null},
      {...policyJson(15), 'administrator': true},
      {
        for (final entry in policyJson(15).entries)
          entry.key.toUpperCase(): entry.value,
      },
    ]) {
      expect(() => ClipboardPolicy.fromJson(value), throwsFormatException);
    }
  });

  test(
    'intersection never broadens the broker policy or the session snapshot',
    () {
      for (var session = 0; session < 16; session++) {
        for (var current = 0; current < 16; current++) {
          final result = ClipboardPolicy.fromJson(policyJson(session))
              .intersect(ClipboardPolicy.fromJson(policyJson(current)));
          final expected = policyJson(session & current);
          expect(result.textToRemote, expected['textToRemote']);
          expect(result.textToLocal, expected['textToLocal']);
          expect(result.filesToRemote, expected['filesToRemote']);
          expect(result.filesToLocal, expected['filesToLocal']);
        }
      }
      expect(
        const ClipboardPolicy.all().intersect(const ClipboardPolicy()).any,
        isFalse,
      );
    },
  );
}
