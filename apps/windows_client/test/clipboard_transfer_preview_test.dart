// Every state of the in-session transfer card with the real fonts, dark and
// light, at text scales 1.0 / 1.5 / 2.0 (Windows 100 %, 150 %, 200 %): nothing
// overflows or leaves the window. With MULTITAP_TRANSFER_CAPTURE set to a
// folder, each one is also written there as PNG for review.
import 'dart:io';
import 'dart:ui' as ui;

import 'package:flutter/material.dart';
import 'package:flutter/rendering.dart';
import 'package:flutter/services.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:multitap_client/data/clipboard_session.dart';
import 'package:multitap_client/data/paste_target.dart';
import 'package:multitap_client/ui/app_theme.dart';
import 'package:multitap_client/ui/clipboard_transfer_card.dart';

class _Clipboard extends ClipboardSession {
  _Clipboard()
    : super(
        url: Uri.parse('wss://fixture.invalid/clipboard'),
        token: 'fixture-only',
        sessionId: 'fixture-only',
      );
  ClipboardProgress? current;
  bool running = false;
  @override
  bool get transferring => running;
  @override
  bool get userWaiting => true;
  @override
  ClipboardProgress? get progress => current;
  @override
  Future<void> cancelTransfer() async {}
  void set(ClipboardProgress? progress) {
    current = progress;
    running = true;
    notifyListeners();
  }

  void end({String? code, String message = ''}) {
    running = false;
    error = code;
    status = message;
    notifyListeners();
  }
}

class _Probe implements PasteTargetProbe {
  _Probe(this.start);
  final DateTime start;
  @override
  Future<PasteDestination?> foregroundFolder() async => const PasteDestination(
    path: r'C:\Users\<user>\Downloads',
    name: '다운로드',
  );
  @override
  Future<DateTime?> created(String path) async =>
      path.endsWith('사진') ? start : null;
  @override
  Future<bool> open(String folder, {String? select}) async => true;
}

const _mb = 1048576;
ClipboardProgress _paste(int moved, {int index = 120}) => ClipboardProgress(
  sending: false,
  format: 'tree',
  fileCount: 1702,
  folderCount: 297,
  fileIndex: index,
  transferred: moved,
  total: 1843 * _mb,
  fileName: '김정훈_고급2.jpg',
  topLevel: const ['사진'],
);

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
  final boundary = GlobalKey();
  final output = Platform.environment['MULTITAP_TRANSFER_CAPTURE'];

  // Each state: how to reach it from a fresh controller.
  final states =
      <String, Future<void> Function(WidgetTester, _Clipboard, ClipboardTransferController, void Function(Duration))>{
        'preparing': (tester, clipboard, controller, tick) async {
          clipboard.set(null);
          tick(const Duration(milliseconds: 800));
          await tester.pump(const Duration(milliseconds: 800));
        },
        'running-folder': (tester, clipboard, controller, tick) async {
          var moved = 0;
          clipboard.set(_paste(moved));
          for (var i = 0; i < 40; i++) {
            moved += 2 * _mb;
            clipboard.current = _paste(moved);
            tick(const Duration(milliseconds: 250));
            await tester.pump(const Duration(milliseconds: 250));
          }
        },
        'running-details': (tester, clipboard, controller, tick) async {
          var moved = 0;
          clipboard.set(_paste(moved));
          for (var i = 0; i < 40; i++) {
            moved += 2 * _mb;
            clipboard.current = _paste(moved);
            tick(const Duration(milliseconds: 250));
            await tester.pump(const Duration(milliseconds: 250));
          }
          controller.toggleDetails();
          await tester.pump();
        },
        'sending-one-file': (tester, clipboard, controller, tick) async {
          clipboard.set(
            const ClipboardProgress(
              sending: true,
              format: 'files',
              fileCount: 1,
              fileIndex: 1,
              transferred: 25 * _mb,
              total: 60 * _mb,
              fileName: '분기별 실적 보고서_최종_수정본(2026년 3분기 전체 부서 취합).xlsx',
            ),
          );
          tick(const Duration(milliseconds: 800));
          await tester.pump(const Duration(milliseconds: 800));
        },
        'finishing': (tester, clipboard, controller, tick) async {
          clipboard.set(_paste(1843 * _mb, index: 1702));
          tick(const Duration(milliseconds: 2000));
          await tester.pump(const Duration(milliseconds: 2000));
        },
        'done-folder': (tester, clipboard, controller, tick) async {
          clipboard.set(_paste(_mb));
          tick(const Duration(milliseconds: 2000));
          await tester.pump(const Duration(milliseconds: 2000));
          clipboard.end();
          await tester.pump();
          await tester.pump();
        },
        'failed': (tester, clipboard, controller, tick) async {
          clipboard.set(_paste(_mb));
          tick(const Duration(milliseconds: 800));
          await tester.pump(const Duration(milliseconds: 800));
          clipboard.end(
            code: 'clipboard_disk_full',
            message: '받는 쪽 디스크 공간이 부족해요. 공간을 확보한 뒤 다시 시도하세요.',
          );
          await tester.pump();
        },
        'cancelled': (tester, clipboard, controller, tick) async {
          clipboard.set(_paste(_mb));
          tick(const Duration(milliseconds: 800));
          await tester.pump(const Duration(milliseconds: 800));
          controller.cancel();
          clipboard.end(code: 'clipboard_cancelled');
          await tester.pump();
        },
      };

  for (final MapEntry(key: name, value: reach) in states.entries) {
    for (final dark in [true, false]) {
      for (final scale in [1.0, 1.5, 2.0]) {
        testWidgets('$name ${dark ? 'dark' : 'light'} x$scale fits', (
          tester,
        ) async {
          // 자세히 open at 200 % is tall; the session's overlay scrolls, this frame just fits it.
          const size = Size(520, 640);
          tester.view.devicePixelRatio = 1;
          tester.view.physicalSize = size;
          addTearDown(tester.view.reset);
          await _fonts(tester);
          var now = Duration.zero;
          final start = DateTime(2026, 11, 2, 9);
          final clipboard = _Clipboard();
          final controller = ClipboardTransferController(
            clipboard: clipboard,
            remoteName: 'WorkPC',
            pasteTargets: _Probe(start),
            wallClock: () => start.add(now),
            clock: () => now,
            dark: dark,
          );
          try {
            await tester.pumpWidget(
              MaterialApp(
                debugShowCheckedModeBanner: false,
                theme: buildAppTheme(
                  brightness: dark ? Brightness.dark : Brightness.light,
                ),
                builder: (context, child) => MediaQuery(
                  data: MediaQuery.of(
                    context,
                  ).copyWith(textScaler: TextScaler.linear(scale)),
                  child: child!,
                ),
                home: RepaintBoundary(
                  key: boundary,
                  child: ColoredBox(
                    color: dark
                        ? const Color(0xFF202A33)
                        : const Color(0xFFDDE3EA),
                    child: Center(
                      child: Padding(
                        padding: const EdgeInsets.all(16),
                        child: ClipboardTransferCard(
                          controller: controller,
                          dark: dark,
                        ),
                      ),
                    ),
                  ),
                ),
              ),
            );
            await reach(tester, clipboard, controller, (step) => now += step);
            expect(tester.takeException(), isNull);
            final card = find.byKey(const Key('clipboard-transfer'));
            if (name == 'done-folder') {
              expect(card, findsNothing);
              expect(find.text('옮겼어요'), findsNothing);
              expect(find.byKey(const Key('clipboard-open-folder')), findsNothing);
              return;
            }
            expect(card, findsOneWidget);
            final rect = tester.getRect(card);
            expect(rect.left, greaterThanOrEqualTo(0));
            expect(rect.right, lessThanOrEqualTo(size.width));
            expect(rect.bottom, lessThanOrEqualTo(size.height));
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
                  '${directory.path}/flutter-$name-${dark ? 'dark' : 'light'}-${(scale * 100).round()}pct.png',
                ).writeAsBytes(bytes!.buffer.asUint8List());
                image.dispose();
              });
            }
          } finally {
            await tester.pumpWidget(const SizedBox());
            controller.dispose();
          }
        });
      }
    }
  }
}
