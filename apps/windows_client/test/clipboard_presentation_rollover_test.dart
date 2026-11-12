import 'dart:async';

import 'package:flutter/material.dart';
import 'package:flutter/services.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:multitap_client/data/clipboard_session.dart';
import 'package:multitap_client/data/transfer_notice_window.dart';
import 'package:multitap_client/ui/app_theme.dart';
import 'package:multitap_client/ui/clipboard_transfer_card.dart';

class _Clipboard extends ClipboardSession {
  _Clipboard() : super(url: Uri.parse('wss://fixture.invalid/clipboard'),
      token: 'fixture-only', sessionId: 'fixture');
  bool running = false;
  int percent = 25;
  @override
  bool get transferring => running;
  @override
  bool get userWaiting => running;
  @override
  ClipboardProgress? get progress => running ? ClipboardProgress(
      sending: false, format: 'files', fileCount: 1, fileIndex: 1,
      transferred: percent, total: 100, fileName: 'synthetic.bin') : null;
  void begin(int value) { percent = value; running = true; notifyListeners(); }
  void end() { running = false; notifyListeners(); }
}

class _Native {
  _Native(this.channel, this.messenger);
  final MethodChannel channel;
  final TestDefaultBinaryMessenger messenger;
  Map<String, Object?> identity = {};
  bool visible = false, hideFails = false;
  int revision = 1, claims = 0, acquisitions = 0, surfaceRevision = 1;
  Future<Object?> call(MethodCall call) async {
    final args = Map<String, Object?>.from(call.arguments as Map? ?? {});
    if (call.method == 'acquire') {
      acquisitions++;
      if (visible && hideFails) {
        return {'protocol': 'presentation-lease-v1', 'accepted': false,
          'owner': args['owner'], 'generation': args['generation'],
          'lease': '00000000000000000000000000000000',
          'revision': 1, 'visible': false, 'phase': 'inline',
          'inlineAllowed': false, 'blockedByNative': true,
          'surfaceRevision': surfaceRevision};
      }
      claims++;
      identity = {'owner': args['owner'], 'generation': args['generation'],
        'lease': claims.toRadixString(16).padLeft(32, '0')};
      visible = false;
      revision = 1;
    }
    final matches = args['owner'] == identity['owner'] &&
        args['generation'] == identity['generation'] &&
        (call.method == 'acquire' || args['lease'] == identity['lease']);
    if (call.method == 'hide' && matches && !hideFails) {
      visible = false;
      revision++;
    }
    return {'protocol': 'presentation-lease-v1', 'accepted': matches,
      'owner': args['owner'], 'generation': args['generation'],
      'lease': call.method == 'acquire' ? identity['lease'] : args['lease'],
      'revision': revision, 'visible': matches && visible,
      'phase': matches && visible ? 'native' : 'inline',
      'inlineAllowed': !visible, 'blockedByNative': visible,
      'surfaceRevision': surfaceRevision};
  }
  Future<void> presentation(bool value) async {
    visible = value;
    surfaceRevision++;
    await send('presentation', {
      'protocol': 'presentation-lease-v1', 'accepted': true, ...identity,
      'revision': ++revision, 'visible': value,
      'phase': value ? 'native' : 'inline',
    });
  }
  Future<void> hiddenProof() async {
    // This is an explicit current physical-surface observation, never a timer
    // or a rejected request pretending to prove another owner's popup hidden.
    visible = false;
    await send('surface', {
      'protocol': 'presentation-lease-v1',
      'surfaceRevision': ++surfaceRevision,
      'inlineAllowed': true, 'blockedByNative': false,
    });
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
  testWidgets('old native hide failure blocks new-generation inline paint without reusing old bytes', (tester) async {
    const channel = MethodChannel('fixture/notice-generation-hide-failure');
    final messenger = TestDefaultBinaryMessengerBinding.instance.defaultBinaryMessenger;
    final native = _Native(channel, messenger);
    messenger.setMockMethodCallHandler(channel, native.call);
    final window = NativeTransferNoticeWindow(channel);
    final clipboard = _Clipboard();
    final controller = ClipboardTransferController(clipboard: clipboard, window: window);
    try {
      await tester.pumpWidget(MaterialApp(theme: buildAppTheme(), home: Scaffold(
        body: ClipboardTransferOverlay(controller: controller))));
      clipboard.begin(25);
      await tester.pump(const Duration(milliseconds: 701));
      for (var frame = 0; frame < 24 && native.identity.isEmpty; frame++) {
        await tester.pump(const Duration(milliseconds: 16));
      }
      expect(native.identity, isNotEmpty);
      await native.presentation(true); // Confirmed old presenter; no real HWND.
      await tester.pump();
      final card = find.byKey(const Key('clipboard-transfer')).hitTestable();
      expect(card, findsNothing);
      expect(controller.view.percent, 25);
      native.hideFails = true;
      clipboard.end();
      clipboard.begin(75);
      await tester.pump(const Duration(milliseconds: 701));
      await tester.pump();
      expect(native.visible, isTrue);
      expect(controller.view.percent, 75, reason: 'new operation keeps its own progress');
      expect(controller.blocksInteraction, isTrue);
      expect(find.byKey(const Key('clipboard-transfer-barrier')), findsOneWidget);
      final painters = card.evaluate().length + (native.visible ? 1 : 0);
      expect(painters, 1,
          reason: 'failed old Native Hide cannot permit a new inline card beside it');
      expect(native.acquisitions, 2);
      await native.hiddenProof();
      await tester.pump();
      expect(card, findsOneWidget);
      expect(controller.view.percent, 75);
      expect(native.acquisitions, 2, reason: 'hidden proof must not reacquire');
    } finally {
      native.hideFails = false;
      await tester.pumpWidget(const SizedBox.shrink());
      controller.dispose();
      await window.hide();
      window.dispose();
      channel.setMethodCallHandler(null);
      messenger.setMockMethodCallHandler(channel, null);
    }
  });

  testWidgets('a rejected new wrapper stays inline-blocked until current hidden proof', (tester) async {
    const channel = MethodChannel('fixture/notice-wrapper-hide-failure');
    final messenger = TestDefaultBinaryMessengerBinding.instance.defaultBinaryMessenger;
    final native = _Native(channel, messenger);
    messenger.setMockMethodCallHandler(channel, native.call);
    final oldWindow = NativeTransferNoticeWindow(channel);
    final newWindow = NativeTransferNoticeWindow(channel);
    final clipboard = _Clipboard();
    final controller = ClipboardTransferController(clipboard: clipboard, window: newWindow);
    try {
      await oldWindow.update(const TransferNoticeContent(kind: 0, generation: 1, percent: 25));
      await native.presentation(true);
      expect(oldWindow.isPresented(1), isTrue);
      native.hideFails = true;
      await tester.pumpWidget(MaterialApp(theme: buildAppTheme(), home: Scaffold(
        body: ClipboardTransferOverlay(controller: controller))));
      clipboard.begin(75);
      await tester.pump(const Duration(milliseconds: 701));
      for (var frame = 0; frame < 24 && native.acquisitions < 2; frame++) {
        await tester.pump(const Duration(milliseconds: 16));
      }
      expect(native.acquisitions, 2);
      expect(native.visible, isTrue);
      expect(controller.view.percent, 75);
      expect(controller.blocksInteraction, isTrue);
      expect(find.byKey(const Key('clipboard-transfer-barrier')), findsOneWidget);
      final card = find.byKey(const Key('clipboard-transfer')).hitTestable();
      expect(card.evaluate().length + (native.visible ? 1 : 0), 1,
          reason: 'rejected acquire is not proof the process popup is hidden');
      await native.hiddenProof();
      await tester.pump();
      expect(card, findsOneWidget);
      expect(controller.view.percent, 75);
      expect(native.acquisitions, 2, reason: 'unblocking is inline-only, never a new claim');
    } finally {
      native.hideFails = false;
      await tester.pumpWidget(const SizedBox.shrink());
      controller.dispose();
      await newWindow.hide();
      newWindow.dispose();
      await oldWindow.hide();
      oldWindow.dispose();
      channel.setMethodCallHandler(null);
      messenger.setMockMethodCallHandler(channel, null);
    }
  });
}
