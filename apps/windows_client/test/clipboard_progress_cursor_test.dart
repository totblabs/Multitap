import 'dart:ui' as ui;

import 'package:flutter/material.dart';
import 'package:flutter/rendering.dart';
import 'package:flutter/services.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:multitap_client/data/clipboard_session.dart';
import 'package:multitap_client/data/transfer_notice_window.dart';
import 'package:multitap_client/ui/app_theme.dart';
import 'package:multitap_client/ui/clipboard_transfer_card.dart';
import 'package:multitap_client/ui/remote_view.dart' show remoteViewCursor;

class _Clipboard extends ClipboardSession {
  _Clipboard() : super(url: Uri.parse('wss://fixture.invalid/clipboard'),
      token: 'fixture-only', sessionId: 'fixture');
  bool running = false;
  @override
  bool get transferring => running;
  @override
  bool get userWaiting => running;
  @override
  ClipboardProgress? get progress => running ? const ClipboardProgress(
      sending: false, format: 'files', fileCount: 1, fileIndex: 1,
      transferred: 1000, total: 4000, fileName: 'synthetic.bin') : null;
  void change(bool value) { running = value; notifyListeners(); }
}
class _Window extends ChangeNotifier
    implements TransferNoticeWindow, TransferNoticePresentation {
  bool external = false;
  int generation = -1;
  @override
  VoidCallback? onCancel, onClose;
  @override
  bool isPresented(int value) => external && value == generation;
  void handoff() { external = true; notifyListeners(); }
  @override
  Future<void> update(TransferNoticeContent content) async { generation = content.generation; }
  @override
  Future<void> hide() async { external = false; }
  @override
  Future<void> taskbar(TaskbarProgressMode mode, int percent) async {}
}

void main() {
  test('ordinary remote cursor preserves baked/ready/explicit-hidden and uses fallback only when needed', () {
    expect(remoteViewCursor(captured: false, localCursorFallback: false), SystemMouseCursors.click);
    expect(remoteViewCursor(captured: false, localCursorFallback: true), SystemMouseCursors.click);
    // false covers legacy baked, a ready native cursor and explicitly hidden remote cursor.
    expect(remoteViewCursor(captured: true, localCursorFallback: false), SystemMouseCursors.none);
    expect(remoteViewCursor(captured: true, localCursorFallback: true), SystemMouseCursors.basic);
  });
  testWidgets('blocked blank modal keeps local arrow while external card owns paint; nonblocking defers', (tester) async {
    const cursorChannel = MethodChannel('flutter/mousecursor');
    final messenger = TestDefaultBinaryMessengerBinding.instance.defaultBinaryMessenger;
    messenger.setMockMethodCallHandler(cursorChannel, (_) async => null);
    final clipboard = _Clipboard(), window = _Window();
    final controller = ClipboardTransferController(clipboard: clipboard, window: window);
    // This SDK's mouse TestPointer defaults device=1 and has no gesture getter.
    const mouseDevice = 1;
    final mouse = await tester.createGesture(pointer: mouseDevice,
        kind: ui.PointerDeviceKind.mouse);
    try {
      await tester.pumpWidget(MaterialApp(theme: buildAppTheme(), home: Scaffold(
        body: MouseRegion(cursor: SystemMouseCursors.none,
          child: Stack(fit: StackFit.expand, children: [
            ClipboardTransferOverlay(controller: controller),
          ]),
        ),
      )));
      await mouse.addPointer(location: const Offset(8, 8));
      await tester.pump();
      expect(RendererBinding.instance.mouseTracker.debugDeviceActiveCursor(mouseDevice),
          SystemMouseCursors.none);
      clipboard.change(true);
      await tester.pump(const Duration(milliseconds: 701));
      await mouse.moveTo(const Offset(9, 9));
      await tester.pump();
      expect(RendererBinding.instance.mouseTracker.debugDeviceActiveCursor(mouseDevice),
          SystemMouseCursors.basic);
      window.handoff();
      await tester.pump();
      expect(find.byKey(const Key('clipboard-transfer')).hitTestable(), findsNothing);
      expect(find.byKey(const Key('clipboard-transfer-barrier')), findsOneWidget);
      await mouse.moveTo(const Offset(10, 10));
      await tester.pump();
      expect(RendererBinding.instance.mouseTracker.debugDeviceActiveCursor(mouseDevice),
          SystemMouseCursors.basic);
      clipboard.change(false);
      await tester.pump();
      await mouse.moveTo(const Offset(11, 11));
      await tester.pump();
      expect(find.byKey(const Key('clipboard-transfer-barrier')), findsNothing);
      expect(RendererBinding.instance.mouseTracker.debugDeviceActiveCursor(mouseDevice),
          SystemMouseCursors.none);
    } finally {
      await mouse.removePointer();
      await tester.pumpWidget(const SizedBox.shrink());
      controller.dispose();
      window.dispose();
      messenger.setMockMethodCallHandler(cursorChannel, null);
    }
  });
}
