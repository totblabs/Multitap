import 'package:flutter_test/flutter_test.dart';
import 'package:multitap_client/data/clipboard_session.dart';

// The same verdicts as the native receiver's safeLeaf
// (native/windows_clipboard/clipboard.cpp); checked against that function
// when this list was written.
final cases = <String, bool>{
  'report.txt': true,
  '한글 파일.bin': true,
  'conx.txt': true,
  'LPT0.log': true,
  'COM10.txt': true,
  '.hidden': true,
  'a' * 240: true,
  '': false,
  '.': false,
  '..': false,
  'trailing.': false,
  'trailing ': false,
  'a' * 241: false,
  'dir/name.txt': false,
  r'dir\name.txt': false,
  'stream:zone': false,
  'what?.txt': false,
  'tab\tname': false,
  'nul\u0000.txt': false,
  'CON': false,
  'con.txt': false,
  'Con.tar.gz': false,
  // Windows strips trailing spaces and dots from the stem: still the device.
  'CON .txt': false,
  'NUL  .log': false,
  'COM1 . txt': false,
  'AUX': false,
  'prn.doc': false,
  r'CONIN$.txt': false,
  r'conout$': false,
  'COM1.txt': false,
  'lpt9': false,
  'COM¹.txt': false,
  'LPT²': false,
  'COM³.x': false,
  // Only ASCII letters fold, as natively (dotless i is not I).
  'CONıN\$.txt': true,
  'unpaired\ud800.txt': false,
  'low\udc00.txt': false,
  'pair💾.txt': true,
};

void main() {
  test('Clipboard file names follow the native receiver rule exactly', () {
    for (final entry in cases.entries) {
      expect(
        clipboardSafeLeaf(entry.key),
        entry.value,
        reason: entry.key.length > 40 ? '${entry.key.length} units' : entry.key,
      );
    }
  });
}
