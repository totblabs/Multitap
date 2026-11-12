import 'dart:io';
import 'dart:ui' as ui;

import 'package:flutter/material.dart';
import 'package:flutter/rendering.dart';
import 'package:flutter/services.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:multitap_client/data/clipboard_session.dart';
import 'package:multitap_client/ui/app_theme.dart';
import 'package:multitap_client/ui/clipboard_transfer_card.dart';

class _Clipboard extends ClipboardSession {
  _Clipboard()
    : super(
        url: Uri.parse('wss://fixture.invalid/clipboard'),
        token: 'fixture-only',
        sessionId: 'fixture-only',
      );
  bool running = false, waiting = true;
  int cancels = 0;
  ClipboardProgress? current;
  @override
  bool get transferring => running;
  @override
  bool get userWaiting => waiting;
  @override
  ClipboardProgress? get progress => current;
  @override
  Future<void> cancelTransfer() async {
    cancels++;
  }

  void begin({
    String format = 'tree',
    bool userWaiting = true,
    bool asyncConsumer = false,
  }) {
    running = true;
    waiting = userWaiting;
    current = ClipboardProgress(
      sending: false,
      format: format,
      fileCount: 20343,
      folderCount: 17,
      fileIndex: 152,
      transferred: 1500 * 1048576,
      total: 9000 * 1048576,
      fileName: '긴 이름의 업무 자료 파일 예시.bin',
      asyncConsumer: asyncConsumer,
    );
    notifyListeners();
  }

  void end([String? code]) {
    running = false;
    current = null;
    error = code;
    status = code == null ? '전송 종료' : '파일을 저장할 공간이 부족해요.';
    notifyListeners();
  }
}

bool _fontsLoaded = false;
Future<void> _fonts(WidgetTester tester) async {
  if (_fontsLoaded) return;
  await tester.runAsync(() async {
    Future<ByteData> bytes(String path) async =>
        ByteData.sublistView(await File(path).readAsBytes());
    final text = FontLoader('Pretendard');
    for (final weight in ['Regular', 'Medium', 'SemiBold', 'Bold']) {
      text.addFont(bytes('assets/fonts/Pretendard-$weight.otf'));
    }
    await text.load();
    final artifacts = File(Platform.resolvedExecutable).parent.parent.parent;
    await (FontLoader('MaterialIcons')..addFont(
          bytes('${artifacts.path}/material_fonts/MaterialIcons-Regular.otf'),
        ))
        .load();
  });
  _fontsLoaded = true;
}

void main() {
  late _Clipboard clipboard;
  late ClipboardTransferController controller;
  int taps = 0;
  final boundary = GlobalKey();
  Future<void> mount(
    WidgetTester tester, {
    Size size = const Size(960, 640),
    bool dark = true,
    double scale = 1,
    Rect? primary,
  }) async {
    clipboard = _Clipboard();
    taps = 0;
    controller = ClipboardTransferController(clipboard: clipboard, dark: dark);
    tester.view.devicePixelRatio = 1;
    tester.view.physicalSize = size;
    addTearDown(tester.view.reset);
    await _fonts(tester);
    await tester.pumpWidget(
      MaterialApp(
        theme: buildAppTheme(
          brightness: dark ? Brightness.dark : Brightness.light,
        ),
        builder: (context, child) => MediaQuery(
          data: MediaQuery.of(context)
              .copyWith(textScaler: TextScaler.linear(scale)),
          child: child!,
        ),
        home: RepaintBoundary(
          key: boundary,
          child: Scaffold(
            body: Stack(
              fit: StackFit.expand,
              children: [
                Align(
                  alignment: Alignment.topLeft,
                  child: TextButton(
                    key: const Key('underlying-action'),
                    onPressed: () => taps++,
                    child: const Text('다른 작업'),
                  ),
                ),
                ClipboardTransferOverlay(
                  controller: controller,
                  dark: dark,
                  primaryRegion: primary,
                ),
              ],
            ),
          ),
        ),
      ),
    );
  }

  void check(String name, Future<void> Function(WidgetTester) body) {
    testWidgets(name, (tester) async {
      try {
        await body(tester);
      } finally {
        await tester.pumpWidget(const SizedBox());
        controller.dispose();
      }
    });
  }

  check(
    'a file paste blocks only this view, stays centered and always offers cancel',
    (tester) async {
      await mount(tester);
      clipboard.begin();
      await tester.pump();
      expect(controller.blocksInteraction, isTrue);
      await tester.tap(
        find.byKey(const Key('underlying-action')),
        warnIfMissed: false,
      );
      expect(taps, 0);
      await tester.pump(const Duration(milliseconds: 701));
      final rect = tester.getRect(find.byKey(const Key('clipboard-transfer')));
      expect(rect.center.dx, closeTo(480, 1));
      expect(rect.center.dy, closeTo(320, 1));
      expect(find.byKey(const Key('clipboard-collapse')), findsNothing);
      await tester.tap(find.text('취소'));
      await tester.pump();
      expect(clipboard.cancels, 1);
      expect(find.text('취소 중…'), findsOneWidget);
      expect(controller.blocksInteraction, isTrue);
      clipboard.end('clipboard_cancelled');
      await tester.pump();
      expect(controller.blocksInteraction, isFalse);
      expect(find.byKey(const Key('clipboard-transfer-barrier')), findsNothing);
      await tester.tap(find.byKey(const Key('underlying-action')));
      expect(taps, 1);
    },
  );

  check('an announced copy and silent text do not lock the view', (
    tester,
  ) async {
    await mount(tester);
    clipboard.begin(userWaiting: false);
    await tester.pump(const Duration(seconds: 1));
    expect(controller.blocksInteraction, isFalse);
    expect(find.byKey(const Key('clipboard-transfer')), findsNothing);
    await tester.tap(find.byKey(const Key('underlying-action')));
    expect(taps, 1);
    clipboard.end();
    await tester.pump();
    clipboard.begin(format: 'text');
    await tester.pump(const Duration(seconds: 2));
    expect(controller.blocksInteraction, isFalse);
  });

  check('cancel is available during the short progress delay', (tester) async {
    await mount(tester);
    clipboard.begin();
    await tester.pump();
    expect(controller.phase, TransferPhase.hidden);
    expect(controller.blocksInteraction, isTrue);
    await tester.sendKeyEvent(LogicalKeyboardKey.escape);
    await tester.pump();
    expect(clipboard.cancels, 1);
    expect(find.text('취소 중…'), findsOneWidget);
    clipboard.end('clipboard_cancelled');
    await tester.pump();
    expect(controller.blocksInteraction, isFalse);
  });

  check('mouse cancellation is also available before progress appears', (
    tester,
  ) async {
    await mount(tester);
    clipboard.begin();
    await tester.pump();
    expect(controller.phase, TransferPhase.hidden);
    expect(find.byKey(const Key('clipboard-transfer-pending')), findsOneWidget);
    await tester.tap(find.byKey(const Key('clipboard-cancel')));
    await tester.pump();
    expect(clipboard.cancels, 1);
    clipboard.end('clipboard_cancelled');
    await tester.pump();
    expect(controller.blocksInteraction, isFalse);
  });

  check('legacy Explorer progress retains a visible modal cancel surface', (
    tester,
  ) async {
    await mount(tester);
    clipboard.begin(asyncConsumer: true);
    await tester.pump(const Duration(milliseconds: 800));
    expect(controller.view.suppressed, isTrue);
    expect(find.byKey(const Key('clipboard-transfer-pending')), findsOneWidget);
    await tester.tap(find.byKey(const Key('clipboard-cancel')));
    await tester.pump();
    expect(clipboard.cancels, 1);
    clipboard.end('clipboard_cancelled');
    await tester.pump();
  });

  for (final failure in [false, true]) {
    check('${failure ? 'failure' : 'completion'} releases the input barrier', (
      tester,
    ) async {
      await mount(tester);
      clipboard.begin();
      await tester.pump(const Duration(milliseconds: 701));
      clipboard.end(failure ? 'clipboard_disk_full' : null);
      await tester.pump();
      expect(controller.blocksInteraction, isFalse);
      expect(find.byKey(const Key('clipboard-transfer-barrier')), findsNothing);
      if (failure) {
        expect(find.text('옮기지 못했어요'), findsOneWidget);
      } else {
        expect(find.text('옮겼어요'), findsNothing);
      }
      await tester.tap(find.byKey(const Key('underlying-action')));
      expect(taps, 1);
    });
  }

  check(
    'a spanning view centers its card on the primary while blocking both monitors',
    (tester) async {
      await mount(
        tester,
        size: const Size(2560, 800),
        primary: const Rect.fromLTWH(0, 0, 1280, 800),
      );
      clipboard.begin();
      await tester.pump(const Duration(milliseconds: 701));
      final rect = tester.getRect(find.byKey(const Key('clipboard-transfer')));
      expect(rect.center.dx, closeTo(640, 1));
      expect(rect.center.dy, closeTo(400, 1));
      final barrier = tester.getRect(
        find.byKey(const Key('clipboard-transfer-barrier')),
      );
      expect(barrier.size, const Size(2560, 800));
    },
  );

  for (final dark in [true, false]) {
    for (final sample in [
      (1920.0, 1080.0, 1.0),
      (1366.0, 768.0, 1.5),
      (960.0, 640.0, 2.0),
    ]) {
      check(
        'render centered ${dark ? 'dark' : 'light'} ${sample.$1}x${sample.$2} scale${sample.$3}',
        (tester) async {
          await mount(
            tester,
            size: Size(sample.$1, sample.$2),
            dark: dark,
            scale: sample.$3,
          );
          clipboard.begin();
          await tester.pump(const Duration(milliseconds: 701));
          expect(tester.takeException(), isNull);
          final rect = tester.getRect(
            find.byKey(const Key('clipboard-transfer')),
          );
          expect(rect.center.dx, closeTo(sample.$1 / 2, 1));
          expect(rect.center.dy, closeTo(sample.$2 / 2, 1));
          final output = Platform.environment['MULTITAP_TRANSFER_CAPTURE'];
          if (output != null) {
            await tester.runAsync(() async {
              final render =
                  boundary.currentContext!.findRenderObject()!
                      as RenderRepaintBoundary;
              final image = await render.toImage(pixelRatio: 1);
              final bytes = await image.toByteData(
                format: ui.ImageByteFormat.png,
              );
              final directory = Directory(output);
              await directory.create(recursive: true);
              await File(
                '${directory.path}/${dark ? 'dark' : 'light'}-${sample.$1.toInt()}-${sample.$2.toInt()}-${sample.$3}.png',
              ).writeAsBytes(bytes!.buffer.asUint8List());
              image.dispose();
            });
          }
        },
      );
    }
  }
}
