import 'dart:async';

import 'package:flutter/services.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:multitap_client/data/transfer_notice_window.dart';

void main() {
  TestWidgetsFlutterBinding.ensureInitialized();
  test('a second wrapper null reply cannot downgrade a channel already known to own native paint', () async {
    const channel = MethodChannel('fixture/notice-shared-capability');
    final messenger = TestDefaultBinaryMessengerBinding.instance.defaultBinaryMessenger;
    Map<String, Object?> identity = {};
    var acquisitions = 0;
    var nativeVisible = false;
    var surfaceRevision = 1;
    Map<String, Object?> proof() => {
      'protocol': 'presentation-lease-v1', 'surfaceRevision': surfaceRevision,
      'inlineAllowed': !nativeVisible, 'blockedByNative': nativeVisible,
    };
    Future<void> send(String method, Map<String, Object?> fields) async {
      final completed = Completer<void>();
      await messenger.handlePlatformMessage(channel.name,
        const StandardMethodCodec().encodeMethodCall(MethodCall(method, fields)),
        (_) => completed.complete());
      await completed.future;
    }
    messenger.setMockMethodCallHandler(channel, (call) async {
      final args = Map<String, Object?>.from(call.arguments as Map? ?? {});
      if (call.method == 'surfaceStatus') return proof();
      if (call.method == 'acquire') {
        acquisitions++;
        if (acquisitions > 1) return null; // Unknown outcome, not old capability.
        identity = {'owner': args['owner'], 'generation': args['generation'],
          'lease': 'aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa'};
      }
      if (call.method == 'hide') nativeVisible = false;
      return {'protocol': 'presentation-lease-v1', 'accepted': true,
        ...identity, 'revision': 1, 'visible': nativeVisible,
        'phase': nativeVisible ? 'native' : 'inline',
        if (call.method == 'acquire') ...proof(),
      };
    });
    final oldWindow = NativeTransferNoticeWindow(channel);
    final newWindow = NativeTransferNoticeWindow(channel);
    try {
      await oldWindow.update(const TransferNoticeContent(kind: 0, generation: 1, percent: 25));
      nativeVisible = true;
      surfaceRevision++;
      await send('presentation', {'protocol': 'presentation-lease-v1',
        'accepted': true, ...identity, 'revision': 2, 'visible': true,
        'phase': 'native'});
      await send('surface', proof());
      expect(oldWindow.isPresented(1), isTrue);
      await newWindow.update(const TransferNoticeContent(kind: 0, generation: 2, percent: 75));
      expect(nativeVisible, isTrue);
      expect(newWindow.isPresented(2), isTrue,
          reason: 'shared v1 proof forbids treating another wrapper null as unsupported');
      nativeVisible = false;
      surfaceRevision++;
      await send('surface', proof());
      expect(newWindow.isPresented(2), isFalse);
      expect(acquisitions, 2, reason: 'physical recovery must not claim again');
    } finally {
      await newWindow.hide();
      newWindow.dispose();
      await oldWindow.hide();
      oldWindow.dispose();
      messenger.setMockMethodCallHandler(channel, null);
    }
  });
}
