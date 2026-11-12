import 'package:flutter/services.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:multitap_client/data/transfer_notice_window.dart';

void main() {
  TestWidgetsFlutterBinding.ensureInitialized();
  test(
    'late native notice actions cannot cancel or close a replacement transfer',
    () async {
      const channel = MethodChannel(
        'multitap/transfer-notice-generation-fixture',
      );
      final messenger =
          TestDefaultBinaryMessengerBinding.instance.defaultBinaryMessenger;
      messenger.setMockMethodCallHandler(channel, (_) async => null);
      final window = NativeTransferNoticeWindow(channel);
      var cancels = 0, closes = 0;
      window.onCancel = () => cancels++;
      window.onClose = () => closes++;
      Future<void> action(String method, Object? generation) async {
        await messenger.handlePlatformMessage(
          channel.name,
          const StandardMethodCodec().encodeMethodCall(
            MethodCall(method, generation),
          ),
          (_) {},
        );
      }

      try {
        await window.update(
          const TransferNoticeContent(kind: 0, generation: 11),
        );
        await window.update(
          const TransferNoticeContent(kind: 0, generation: 12),
        );
        await action('cancel', 11);
        await action('close', 11);
        await action('cancel', null);
        expect((cancels, closes), (0, 0));
        await action('cancel', 12);
        await action('close', 12);
        expect((cancels, closes), (1, 1));
        await window.hide();
        await action('cancel', 12);
        expect(cancels, 1);
      } finally {
        channel.setMethodCallHandler(null);
        messenger.setMockMethodCallHandler(channel, null);
      }
    },
  );
}
