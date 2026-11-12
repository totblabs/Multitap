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
  Completer<Object?>? acquireGate, hideGate;

  Map<String, Object?> packet({int revision = 1, bool visible = false}) => {
    'protocol': 'presentation-lease-v1', 'accepted': true, ...identity,
    'revision': revision, 'visible': visible,
    'phase': visible ? 'native' : 'inline',
  };
  Future<Object?> call(MethodCall call) async {
    methods.add(call.method);
    final args = Map<String, Object?>.from(call.arguments as Map? ?? {});
    if (call.method == 'surfaceStatus') return null;
    if (call.method == 'acquire') {
      identity = {'owner': args['owner'], 'generation': args['generation'],
        'lease': 'aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa'};
      if (acquireGate != null) return acquireGate!.future;
    }
    if (call.method == 'hide' && hideGate != null) return hideGate!.future;
    return packet();
  }
  Future<void> send(String method, Map<String, Object?> fields) async {
    final completed = Completer<void>();
    await messenger.handlePlatformMessage(channel.name,
      const StandardMethodCodec().encodeMethodCall(MethodCall(method, fields)),
      (_) => completed.complete());
    await completed.future;
  }
}

void main() {
  TestWidgetsFlutterBinding.ensureInitialized();
  testWidgets('newer hidden surface proof wins over a late blocked acquire ACK', (tester) async {
    const channel = MethodChannel('fixture/notice-surface-ack-order');
    final messenger = TestDefaultBinaryMessengerBinding.instance.defaultBinaryMessenger;
    final protocol = _Protocol(channel, messenger)..acquireGate = Completer<Object?>();
    messenger.setMockMethodCallHandler(channel, protocol.call);
    final window = NativeTransferNoticeWindow(channel);
    try {
      final updating = window.update(const TransferNoticeContent(kind: 0, generation: 4));
      await tester.idle();
      expect(window.isPresented(4), isTrue, reason: 'acquire pending excludes inline paint');
      await protocol.send('surface', {
        'protocol': 'presentation-lease-v1', 'surfaceRevision': 9,
        'inlineAllowed': true, 'blockedByNative': false,
      });
      protocol.acquireGate!.complete({
        ...protocol.packet(), 'accepted': false,
        'surfaceRevision': 8, 'inlineAllowed': false, 'blockedByNative': true,
      });
      await updating;
      await tester.idle();
      expect(window.isPresented(4), isFalse,
          reason: 'an older blocked reply cannot overwrite newer physical absence');
      await protocol.send('surface', {
        'protocol': 'presentation-lease-v1', 'surfaceRevision': 8,
        'inlineAllowed': false, 'blockedByNative': true,
      });
      expect(window.isPresented(4), isFalse);
      expect(protocol.methods.where((value) => value == 'acquire').length, 1);
    } finally {
      if (!protocol.acquireGate!.isCompleted) protocol.acquireGate!.complete(null);
      await window.hide();
      window.dispose();
      messenger.setMockMethodCallHandler(channel, null);
    }
  });

  testWidgets('old hide finally cannot clear a resumed same-generation presentation', (tester) async {
    const channel = MethodChannel('fixture/notice-hide-finally-order');
    final messenger = TestDefaultBinaryMessengerBinding.instance.defaultBinaryMessenger;
    final protocol = _Protocol(channel, messenger);
    messenger.setMockMethodCallHandler(channel, protocol.call);
    final window = NativeTransferNoticeWindow(channel);
    try {
      await window.update(const TransferNoticeContent(kind: 0, generation: 6, percent: 25));
      await protocol.send('presentation', protocol.packet(revision: 2, visible: true));
      expect(window.isPresented(6), isTrue);
      protocol.hideGate = Completer<Object?>();
      final hiding = window.hide();
      await tester.idle();
      await protocol.send('presentation', protocol.packet(revision: 3));
      expect(window.isPresented(6), isFalse);
      await window.update(const TransferNoticeContent(kind: 0, generation: 6, percent: 75));
      await protocol.send('presentation', protocol.packet(revision: 5, visible: true));
      expect(window.isPresented(6), isTrue);
      protocol.hideGate!.complete(protocol.packet(revision: 3));
      await hiding;
      expect(window.isPresented(6), isTrue,
          reason: 'the retired attempt cannot overwrite resumed paint ownership');
      await protocol.send('presentation', protocol.packet(revision: 6));
      expect(window.isPresented(6), isFalse,
          reason: 'the resumed callback router must still be attached');
      expect(protocol.methods.where((value) => value == 'acquire').length, 1);
    } finally {
      if (protocol.hideGate != null && !protocol.hideGate!.isCompleted) {
        protocol.hideGate!.complete(protocol.packet());
      }
      protocol.hideGate = null;
      await window.hide();
      window.dispose();
      messenger.setMockMethodCallHandler(channel, null);
    }
  });

  testWidgets('known runner null reply cannot reuse hidden proof cached before acquisition', (tester) async {
    const channel = MethodChannel('fixture/notice-cached-hidden-proof');
    final messenger = TestDefaultBinaryMessengerBinding.instance.defaultBinaryMessenger;
    final protocol = _Protocol(channel, messenger)..acquireGate = Completer<Object?>();
    messenger.setMockMethodCallHandler(channel, protocol.call);
    final window = NativeTransferNoticeWindow(channel);
    try {
      final first = window.update(const TransferNoticeContent(kind: 0, generation: 1));
      await tester.idle();
      protocol.acquireGate!.complete({
        ...protocol.packet(), 'surfaceRevision': 1,
        'inlineAllowed': true, 'blockedByNative': false,
      });
      await first;
      await protocol.send('presentation', protocol.packet(revision: 2, visible: true));
      protocol.hideGate = Completer<Object?>()
        ..complete(protocol.packet(revision: 3, visible: true));
      protocol.acquireGate = Completer<Object?>()..complete(null);
      await window.update(const TransferNoticeContent(kind: 0, generation: 2));
      await tester.idle();
      expect(window.isPresented(2), isTrue,
          reason: 'old cached absence is not current proof after a failed Hide and unknown acquire');
      expect(protocol.methods.where((value) => value == 'acquire').length, 2);
    } finally {
      if (!protocol.acquireGate!.isCompleted) protocol.acquireGate!.complete(null);
      protocol.hideGate = null;
      await window.hide();
      window.dispose();
      messenger.setMockMethodCallHandler(channel, null);
    }
  });
}
