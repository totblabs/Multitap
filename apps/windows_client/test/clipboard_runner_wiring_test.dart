import 'dart:io';

import 'package:flutter_test/flutter_test.dart';

// The Windows runner is the only place that joins the native clipboard adapter to Dart. A component test of either side cannot
// notice a message that nobody forwards: if the paste-pull events (an Explorer paste of files the VM offered) are produced by
// ClientClipboardChannel but never delivered to Dart, such a paste can only time out. These checks read
// the sources, like the other wiring tests; tools/clipboard-e2e-qa runs the real adapter against the real session.
void main() {
  final runner = '${Directory.current.path}/windows/runner';
  final window = File('$runner/flutter_window.cpp').readAsStringSync();
  final bridge = File('lib/data/native_clipboard.dart').readAsStringSync();

  test('every native-to-Dart clipboard event the bridge handles is invoked by the runner', () {
    final handled = RegExp(r"call\.method\s*(?:==|!=)\s*'(\w+)'").allMatches(bridge).map((m) => m.group(1)!).toSet();
    expect(handled, containsAll(<String>['pull', 'changed']));
    for (final method in handled) {
      expect(window.contains('InvokeMethod("$method"'), isTrue, reason: 'flutter_window.cpp never invokes "$method" on the clipboard channel');
    }
  });

  test('pull events are handed to Dart on the platform thread when the clipboard thread posts pull_message', () {
    expect(window.contains('clipboard_worker_->SetPullHandler('), isTrue);
    expect(window.contains('ClientClipboardChannel::pull_message'), isTrue);
    expect(window.contains('clipboard_worker_->DeliverPullEvents()'), isTrue);
  });

  test('the pull event carries every field the bridge reads', () {
    final start = window.indexOf('InvokeMethod("pull"');
    expect(start, greaterThan(0));
    final body = window.substring(start, start + 700);
    for (final key in ['kind', 'rangeId', 'offerId', 'reason', 'index', 'offset', 'end']) {
      expect(body.contains('V("$key")'), isTrue, reason: 'pull event field $key');
      expect(bridge.contains("'$key'"), isTrue, reason: 'bridge reads $key');
    }
  });
}
