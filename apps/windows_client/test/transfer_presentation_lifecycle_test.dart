import 'dart:async';

import 'package:flutter/services.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:multitap_client/data/transfer_notice_window.dart';

class _Protocol {
  _Protocol(this.channel, this.messenger);
  final MethodChannel channel;
  final TestDefaultBinaryMessenger messenger;
  final methods = <String>[];
  Map<String, Object?> identity = {};
  bool supported = true;
  bool hideFails = false;
  Future<Object?> call(MethodCall call) async {
    methods.add(call.method);
    final args = Map<String, Object?>.from(call.arguments as Map? ?? {});
    if (!supported) return null;
    if (call.method == 'acquire') {
      identity = {'owner': args['owner'], 'generation': args['generation'],
        'lease': 'aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa'};
    }
    return {'protocol': 'presentation-lease-v1', 'accepted': true,
      ...identity, 'revision': 1,
      'visible': call.method == 'hide' && hideFails,
      'phase': call.method == 'hide' && hideFails ? 'native' : 'inline'};
  }
  Future<Object?> send(String method, Map<String, Object?> fields) async {
    final completed = Completer<Object?>();
    await messenger.handlePlatformMessage(channel.name,
      const StandardMethodCodec().encodeMethodCall(MethodCall(method, {
        'protocol': 'presentation-lease-v1', 'accepted': true,
        ...identity, ...fields,
      })), (reply) {
        try { completed.complete(reply == null ? null
            : const StandardMethodCodec().decodeEnvelope(reply)); }
        catch (error, stack) { completed.completeError(error, stack); }
      });
    return completed.future;
  }
}

void main() {
  TestWidgetsFlutterBinding.ensureInitialized();
  test('an unsupported runner keeps inline paint and never receives a legacy native update', () async {
    const channel = MethodChannel('fixture/notice-unsupported');
    final messenger = TestDefaultBinaryMessengerBinding.instance.defaultBinaryMessenger;
    final protocol = _Protocol(channel, messenger)..supported = false;
    messenger.setMockMethodCallHandler(channel, protocol.call);
    final window = NativeTransferNoticeWindow(channel);
    try {
      await window.update(const TransferNoticeContent(kind: 0, generation: 9));
      await window.update(const TransferNoticeContent(kind: 0, generation: 9, percent: 25));
      expect(window.isPresented(9), isFalse);
      expect(protocol.methods, ['acquire']);
    } finally {
      await window.hide();
      window.dispose();
      messenger.setMockMethodCallHandler(channel, null);
    }
  });
  testWidgets('prepare owns paint until presentation; content ACK cannot clear it', (tester) async {
    const channel = MethodChannel('fixture/notice-prepare-order');
    final messenger = TestDefaultBinaryMessengerBinding.instance.defaultBinaryMessenger;
    final protocol = _Protocol(channel, messenger);
    messenger.setMockMethodCallHandler(channel, protocol.call);
    final window = NativeTransferNoticeWindow(channel);
    try {
      await window.update(const TransferNoticeContent(kind: 0, generation: 8));
      final preparing = protocol.send('prepare', {'revision': 2, 'requiresFrame': true});
      await tester.idle();
      expect(window.isPresented(8), isTrue);
      await window.update(const TransferNoticeContent(kind: 0, generation: 8, percent: 25));
      expect(window.isPresented(8), isTrue);
      await tester.pump();
      final prepared = await preparing;
      expect((prepared as Map)['prepared'], isTrue);
      await protocol.send('presentation', {'revision': 2, 'visible': true, 'phase': 'native'});
      expect(window.isPresented(8), isTrue);
      await protocol.send('presentation', {'revision': 1, 'visible': false, 'phase': 'inline'});
      expect(window.isPresented(8), isTrue);
      await protocol.send('presentation', {'revision': 3, 'visible': false, 'phase': 'inline'});
      expect(window.isPresented(8), isFalse);
    } finally {
      await window.hide();
      window.dispose();
      messenger.setMockMethodCallHandler(channel, null);
    }
  });
  testWidgets('frame timeout restores inline and rejects a late same-revision show', (tester) async {
    const channel = MethodChannel('fixture/notice-prepare-timeout');
    final messenger = TestDefaultBinaryMessengerBinding.instance.defaultBinaryMessenger;
    final protocol = _Protocol(channel, messenger);
    messenger.setMockMethodCallHandler(channel, protocol.call);
    final window = NativeTransferNoticeWindow(channel);
    try {
      await window.update(const TransferNoticeContent(kind: 0, generation: 7));
      final preparing = protocol.send('prepare', {'revision': 2, 'requiresFrame': true});
      await tester.idle(); // Flush channel work without providing the requested paint frame.
      expect(window.isPresented(7), isTrue);
      await tester.pump(const Duration(milliseconds: 751));
      final prepared = await preparing;
      expect((prepared as Map)['prepared'], isFalse);
      expect(window.isPresented(7), isFalse);
      await protocol.send('presentation', {'revision': 2, 'visible': true, 'phase': 'native'});
      expect(window.isPresented(7), isFalse);
      final retry = await protocol.send('prepare', {'revision': 4, 'requiresFrame': false});
      expect((retry as Map)['prepared'], isTrue);
      await protocol.send('presentation', {'revision': 4, 'visible': true, 'phase': 'native'});
      expect(window.isPresented(7), isTrue);
    } finally {
      await window.hide();
      window.dispose();
      messenger.setMockMethodCallHandler(channel, null);
    }
  });
  test('revoked owner ignores old cancel and cannot reacquire on another update', () async {
    const channel = MethodChannel('fixture/notice-revoked');
    final messenger = TestDefaultBinaryMessengerBinding.instance.defaultBinaryMessenger;
    final protocol = _Protocol(channel, messenger);
    messenger.setMockMethodCallHandler(channel, protocol.call);
    final window = NativeTransferNoticeWindow(channel);
    var cancels = 0;
    window.onCancel = () => cancels++;
    try {
      await window.update(const TransferNoticeContent(kind: 0, generation: 6));
      await protocol.send('presentation', {'revision': 2,
        'accepted': false, 'visible': false, 'phase': 'inline'});
      await protocol.send('cancel', {'revision': 2});
      await window.update(const TransferNoticeContent(kind: 0, generation: 6, percent: 75));
      expect(cancels, 0);
      expect(window.isPresented(6), isFalse);
      expect(protocol.methods.where((method) => method == 'acquire').length, 1);
      expect(protocol.methods.where((method) => method == 'update').length, 1);
    } finally {
      await window.hide();
      window.dispose();
      messenger.setMockMethodCallHandler(channel, null);
    }
  });
  test('a failed same-lease native hide does not falsely restore inline paint', () async {
    const channel = MethodChannel('fixture/notice-hide-failure');
    final messenger = TestDefaultBinaryMessengerBinding.instance.defaultBinaryMessenger;
    final protocol = _Protocol(channel, messenger);
    messenger.setMockMethodCallHandler(channel, protocol.call);
    final window = NativeTransferNoticeWindow(channel);
    var cancels = 0;
    window.onCancel = () => cancels++;
    try {
      await window.update(const TransferNoticeContent(kind: 0, generation: 5));
      await protocol.send('presentation', {'revision': 2, 'visible': true, 'phase': 'native'});
      protocol.hideFails = true;
      await window.hide();
      expect(window.isPresented(5), isTrue);
      await protocol.send('cancel', {'revision': 2});
      expect(cancels, 0);
      protocol.hideFails = false;
      await window.hide();
      expect(window.isPresented(5), isFalse);
    } finally {
      protocol.hideFails = false;
      await window.hide();
      window.dispose();
      messenger.setMockMethodCallHandler(channel, null);
    }
  });
}
