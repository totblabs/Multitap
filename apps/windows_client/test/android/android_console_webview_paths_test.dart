import 'dart:io';

import 'package:flutter_test/flutter_test.dart';

// The Android console view loads only listed paths (ConsoleWebViewPolicy.kt).
// A path the Console page uses and the list lacks never reaches the server:
// without the VM's 한/영 read (GET .../sessions/<id>/ime) the phone Console
// types only Latin letters. The same regular expression is checked here
// against the paths the Console page uses.
void main() {
  final source = File(
    'android/app/src/main/kotlin/me/totb/multitap_client/ConsoleWebViewPolicy.kt',
  ).readAsStringSync();
  final pattern = RegExp(r'Regex\("(\^/console/api/desktops/\$\{Regex\.escape\(desktopId\)\}/sessions/[^"]+)"\)');

  test('the Console page\'s session and 한/영 reads are listed; nothing else under sessions is', () {
    final match = pattern.firstMatch(source);
    expect(match, isNotNull, reason: 'session path rule not found');
    final rule = RegExp(match!.group(1)!.replaceFirst(r'${Regex.escape(desktopId)}', 'pve-173'));
    const session = '/console/api/desktops/pve-173/sessions/8ded6a23-f25e-4312-943a-3a984a22ca5d';
    expect(rule.hasMatch(session), isTrue);
    expect(rule.hasMatch('$session/ime'), isTrue);
    expect(rule.hasMatch('$session/other'), isFalse);
    expect(rule.hasMatch('$session/ime/x'), isFalse);
    expect(rule.hasMatch('/console/api/desktops/pve-174/sessions/8ded6a23-f25e-4312-943a-3a984a22ca5d/ime'), isFalse);
  });
}
