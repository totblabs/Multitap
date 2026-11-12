import 'package:flutter/services.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:multitap_client/data/transfer_notice_window.dart';

void main() {
  TestWidgetsFlutterBinding.ensureInitialized();
  const invalid = <String, Map<String, Object?>>{
    'partial': {'inlineAllowed': true},
    'revision-type': {'inlineAllowed': true, 'blockedByNative': false,
      'surfaceRevision': '1'},
    'contradiction': {'inlineAllowed': false, 'blockedByNative': false,
      'surfaceRevision': 1},
    'revision-zero': {'inlineAllowed': true, 'blockedByNative': false,
      'surfaceRevision': 0},
  };
  for (final entry in invalid.entries) {
    test('present malformed physical fields reject an otherwise valid ACK: ${entry.key}', () async {
      final channel = MethodChannel('fixture/notice-malformed-surface/${entry.key}');
      final messenger = TestDefaultBinaryMessengerBinding.instance.defaultBinaryMessenger;
      final methods = <String>[];
      messenger.setMockMethodCallHandler(channel, (call) async {
        methods.add(call.method);
        if (call.method == 'surfaceStatus') return null;
        final args = Map<String, Object?>.from(call.arguments as Map? ?? {});
        return {'protocol': 'presentation-lease-v1', 'accepted': true,
          'owner': args['owner'], 'generation': args['generation'],
          'lease': 'aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa',
          'revision': 1, 'visible': false, 'phase': 'inline', ...entry.value};
      });
      final window = NativeTransferNoticeWindow(channel);
      try {
        await window.update(const TransferNoticeContent(kind: 0, generation: 1));
        expect(methods.where((method) => method == 'update'), isEmpty,
            reason: 'malformed-present is not the explicit absent-field compatibility case');
        expect(window.isPresented(1), isTrue,
            reason: 'malformed acquisition cannot prove physical absence');
      } finally {
        await window.hide();
        window.dispose();
        messenger.setMockMethodCallHandler(channel, null);
      }
    });
  }
}
