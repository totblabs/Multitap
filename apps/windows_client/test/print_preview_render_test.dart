// Synthetic Flutter rendering only. No VM files, printer, spool job or native
// Windows window is opened. Output is optional and bounded to six fixtures.
import 'dart:io';
import 'dart:ui' as ui;

import 'package:flutter/material.dart';
import 'package:flutter/rendering.dart';
import 'package:flutter/services.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:multitap_client/data/native_print.dart';
import 'package:multitap_client/data/print_session.dart';
import 'package:multitap_client/ui/app_theme.dart';
import 'package:multitap_client/ui/print_dialog.dart';

Future<LocalPrintPreview> syntheticPage(int page) async {
  final landscape = page.isOdd;
  final width = landscape ? 420 : 300, height = landscape ? 300 : 420;
  final recorder = ui.PictureRecorder();
  final canvas = Canvas(recorder);
  canvas.drawColor(Colors.white, BlendMode.src);
  canvas.drawRect(
    Rect.fromLTWH(25, 28, width - 50, 8),
    Paint()..color = const Color(0xfff4a71b),
  );
  final paragraph =
      (ui.ParagraphBuilder(
              ui.ParagraphStyle(fontSize: 22, fontFamily: 'Pretendard'),
            )
            ..pushStyle(ui.TextStyle(color: const Color(0xff20252a)))
            ..addText(
              'Multitap\n${landscape ? 'Landscape' : 'Portrait'} · ${page + 1}',
            ))
          .build()
        ..layout(ui.ParagraphConstraints(width: width - 50));
  canvas.drawParagraph(paragraph, const Offset(25, 55));
  for (var y = 145.0; y < height - 30; y += 24) {
    canvas.drawRect(
      Rect.fromLTWH(25, y, width - 65, 3),
      Paint()..color = const Color(0xffdce0e4),
    );
  }
  final picture = recorder.endRecording();
  final image = await picture.toImage(width, height);
  final bytes = await image.toByteData(format: ui.ImageByteFormat.png);
  final png = bytes!.buffer.asUint8List();
  image.dispose();
  picture.dispose();
  return LocalPrintPreview(
    png: png,
    width: width.toDouble(),
    height: height.toDouble(),
    page: page,
    pages: 3,
  );
}

class RenderPrintBridge implements LocalPrintBridge, LocalPrintPreviewBridge {
  RenderPrintBridge(this.images);
  final List<LocalPrintPreview> images;
  int printCalls = 0, saveCalls = 0;
  @override
  Future<List<LocalPrinter>> printers() async => const [
    LocalPrinter('사무실 프린터', isDefault: true),
  ];
  @override
  Future<int?> pages(String path) async => 3;
  @override
  Future<LocalPrintPreview?> preview(String path, {required int page}) async =>
      images[page];
  @override
  Future<String> status(String printer) async => '';
  @override
  Future<LocalPrintProgress?> progress() async => null;
  @override
  Future<LocalPrintResult> print(
    String path, {
    required String printer,
    required int copies,
    required String title,
  }) async {
    printCalls++;
    return const LocalPrintResult(true);
  }

  @override
  Future<LocalPrintResult> save(
    String path, {
    required String suggestedName,
  }) async {
    saveCalls++;
    return const LocalPrintResult(true);
  }
}

void main() {
  TestWidgetsFlutterBinding.ensureInitialized();
  for (final dark in [true, false]) {
    for (final size in [
      const Size(960, 640),
      const Size(1366, 768),
      const Size(1920, 1080),
    ]) {
      testWidgets(
        'print dialog original mixed pages ${dark ? 'dark' : 'light'} $size',
        (tester) async {
          tester.view.devicePixelRatio = 1;
          tester.view.physicalSize = size;
          addTearDown(tester.view.resetPhysicalSize);
          addTearDown(tester.view.resetDevicePixelRatio);
          late List<LocalPrintPreview> images;
          await tester.runAsync(() async {
            final fonts = FontLoader('Pretendard');
            for (final name in ['Regular', 'Medium', 'SemiBold', 'Bold']) {
              fonts.addFont(
                rootBundle.load('assets/fonts/Pretendard-$name.otf'),
              );
            }
            await fonts.load();
            final artifacts = File(Platform.resolvedExecutable)
                .parent
                .parent
                .parent;
            final icons = FontLoader('MaterialIcons')
              ..addFont(
                File(
                  '${artifacts.path}/material_fonts/MaterialIcons-Regular.otf',
                ).readAsBytes().then((bytes) => ByteData.sublistView(bytes)),
              );
            await icons.load();
            images = await Future.wait([
              syntheticPage(0),
              syntheticPage(1),
              syntheticPage(2),
            ]);
          });
          final bridge = RenderPrintBridge(images);
          final boundaryKey = GlobalKey();
          await tester.pumpWidget(
            RepaintBoundary(
              key: boundaryKey,
              child: MaterialApp(
                theme: buildAppTheme(
                  brightness: dark ? Brightness.dark : Brightness.light,
                ),
                home: MediaQuery(
                  data: MediaQueryData(
                    size: size,
                    textScaler: const TextScaler.linear(1.5),
                  ),
                  child: Scaffold(
                    body: Center(
                      child: PrintDialog(
                        document: PrintDocument(
                          jobId: 'synthetic',
                          title: '월간 보고서 · 가로·세로 혼합',
                          size: 2048,
                          file: File('synthetic.pdf'),
                        ),
                        machine: 'TESTER',
                        bridge: bridge,
                      ),
                    ),
                  ),
                ),
              ),
            ),
          );
          await tester.runAsync(() async {
            final context = tester.element(find.byType(PrintDialog));
            for (final image in images) {
              await precacheImage(MemoryImage(image.png), context);
            }
          });
          await tester.pumpAndSettle();
          expect(
            find.byKey(const ValueKey('print-preview-image-0')),
            findsOneWidget,
          );
          await tester.ensureVisible(
            find.byKey(const Key('print-preview-next')),
          );
          await tester.tap(find.byKey(const Key('print-preview-next')));
          await tester.pumpAndSettle();
          expect(
            find.byKey(const ValueKey('print-preview-image-1')),
            findsOneWidget,
          );
          expect(find.text('전체 세로'), findsNothing);
          expect(find.text('전체 가로'), findsNothing);
          expect(bridge.printCalls, 0);
          expect(bridge.saveCalls, 0);
          expect(tester.takeException(), isNull);
          final output = Platform.environment['PRINT_PREVIEW_QA_DIR'];
          if (output != null) {
            await tester.runAsync(() async {
              final boundary =
                  boundaryKey.currentContext!.findRenderObject()!
                      as RenderRepaintBoundary;
              final image = await boundary.toImage(pixelRatio: 1);
              final bytes = await image.toByteData(
                format: ui.ImageByteFormat.png,
              );
              final dir = Directory(output);
              await dir.create(recursive: true);
              await File(
                '${dir.path}/${dark ? 'dark' : 'light'}-${size.width.toInt()}x${size.height.toInt()}.png',
              ).writeAsBytes(bytes!.buffer.asUint8List(), flush: true);
              image.dispose();
            });
          }
          await tester.pumpWidget(const SizedBox());
        },
        timeout: const Timeout(Duration(seconds: 40)),
      );
    }
  }
}
