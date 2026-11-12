import 'dart:async';
import 'dart:convert';

import 'package:flutter/material.dart';
import 'package:flutter/services.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:multitap_client/data/native_print.dart';
import 'package:multitap_client/data/print_preview.dart';
import 'package:multitap_client/ui/app_theme.dart';
import 'package:multitap_client/ui/print_preview.dart';

final _png = base64Decode(
  'iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAQAAAC1HAwCAAAAC0lEQVR42mP8/x8AAusB9Wl2nAAAAABJRU5ErkJggg==',
);

LocalPrintPreview page(int index, {int pages = 3}) => LocalPrintPreview(
  png: _png,
  width: index.isOdd ? 1123 : 794,
  height: index.isOdd ? 794 : 1123,
  page: index,
  pages: pages,
);

class PreviewBridge implements LocalPrintPreviewBridge {
  final calls = <int>[];
  final pending = <Completer<LocalPrintPreview?>>[];
  @override
  Future<LocalPrintPreview?> preview(String path, {required int page}) {
    expect(path, 'synthetic.pdf');
    calls.add(page);
    final gate = Completer<LocalPrintPreview?>();
    pending.add(gate);
    return gate.future;
  }
}

Future<void> turn() => Future<void>.delayed(Duration.zero);

void main() {
  TestWidgetsFlutterBinding.ensureInitialized();

  test(
    'rapid navigation coalesces without overlapping native work or stale image',
    () async {
      final bridge = PreviewBridge();
      final controller = PrintPreviewController(
        bridge: bridge,
        path: 'synthetic.pdf',
        pages: 3,
      );
      addTearDown(controller.dispose);
      controller.load();
      controller.select(1);
      controller.select(2);
      expect(bridge.calls, [0]);
      bridge.pending[0].complete(page(0));
      await turn();
      expect(bridge.calls, [0, 2]);
      expect(controller.preview, isNull);
      expect(controller.loading, isTrue);
      bridge.pending[1].complete(page(2));
      await turn();
      expect(controller.preview?.page, 2);
      expect(controller.loading, isFalse);
    },
  );

  test(
    'failed preview can retry and preserves the original page aspect',
    () async {
      final bridge = PreviewBridge();
      final controller = PrintPreviewController(
        bridge: bridge,
        path: 'synthetic.pdf',
        pages: 3,
      );
      addTearDown(controller.dispose);
      controller.select(1);
      bridge.pending[0].complete(null);
      await turn();
      expect(controller.failed, isTrue);
      controller.load();
      bridge.pending[1].complete(page(1));
      await turn();
      expect(controller.failed, isFalse);
      expect(
        controller.preview!.width,
        greaterThan(controller.preview!.height),
      );
      expect(bridge.calls, [1, 1]);
    },
  );

  test(
    'wrong document page/count cannot replace the selected preview',
    () async {
      final bridge = PreviewBridge();
      final controller = PrintPreviewController(
        bridge: bridge,
        path: 'synthetic.pdf',
        pages: 3,
      );
      addTearDown(controller.dispose);
      controller.load();
      bridge.pending[0].complete(page(0, pages: 4));
      await turn();
      expect(controller.preview, isNull);
      expect(controller.failed, isTrue);
      controller.select(-1);
      controller.select(3);
      expect(bridge.calls, [0]);
    },
  );

  test(
    'closing with pending render ignores late completion and queued selection',
    () async {
      final bridge = PreviewBridge();
      final controller = PrintPreviewController(
        bridge: bridge,
        path: 'synthetic.pdf',
        pages: 3,
      );
      var changes = 0;
      controller.addListener(() => changes++);
      controller.load();
      controller.select(2);
      controller.dispose();
      final closedChanges = changes;
      bridge.pending[0].complete(page(0));
      await turn();
      expect(changes, closedChanges);
      expect(bridge.calls, [0]);
      expect(controller.preview, isNull);
    },
  );

  test(
    'native preview uses only the preview method and rejects malformed results',
    () async {
      const channel = MethodChannel('multitap/print');
      final calls = <MethodCall>[];
      Object? result = {
        'png': _png,
        'width': 1123.0,
        'height': 794.0,
        'page': 1,
        'pages': 3,
      };
      TestDefaultBinaryMessengerBinding.instance.defaultBinaryMessenger
          .setMockMethodCallHandler(channel, (call) async {
            calls.add(call);
            return result;
          });
      addTearDown(
        () => TestDefaultBinaryMessengerBinding.instance.defaultBinaryMessenger
            .setMockMethodCallHandler(channel, null),
      );
      const bridge = NativeLocalPrint();
      expect((await bridge.preview('synthetic.pdf', page: 1))!.width, 1123);
      for (final bad in [
        null,
        {
          'png': _png,
          'width': double.nan,
          'height': 794,
          'page': 1,
          'pages': 3,
        },
        {
          'png': Uint8List(0),
          'width': 1123,
          'height': 794,
          'page': 1,
          'pages': 3,
        },
        {'png': _png, 'width': 1123, 'height': 794, 'page': 0, 'pages': 3},
        {'png': _png, 'width': 1123, 'height': 794, 'page': 1, 'pages': 10001},
      ]) {
        result = bad;
        expect(await bridge.preview('synthetic.pdf', page: 1), isNull);
      }
      expect(calls.every((call) => call.method == 'preview'), isTrue);
      final count = calls.length;
      expect(await bridge.preview('synthetic.pdf', page: 10000), isNull);
      expect(calls.length, count);
    },
  );

  for (final dark in [true, false]) {
    for (final size in [
      const Size(960, 640),
      const Size(1366, 768),
      const Size(1920, 1080),
    ]) {
      testWidgets(
        'preview controls fit ${dark ? 'dark' : 'light'} $size at 150%',
        (tester) async {
          tester.view.devicePixelRatio = 1;
          tester.view.physicalSize = size;
          addTearDown(tester.view.resetPhysicalSize);
          addTearDown(tester.view.resetDevicePixelRatio);
          final bridge = PreviewBridge();
          await tester.pumpWidget(
            MaterialApp(
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
                    child: SizedBox(
                      width: 500,
                      child: PrintPreview(
                        path: 'synthetic.pdf',
                        pages: 3,
                        bridge: bridge,
                        enabled: true,
                      ),
                    ),
                  ),
                ),
              ),
            ),
          );
          bridge.pending[0].complete(null);
          await tester.pump();
          expect(find.text('1 / 3'), findsOneWidget);
          expect(find.byKey(const Key('print-preview-retry')), findsOneWidget);
          await tester.tap(find.byKey(const Key('print-preview-next')));
          await tester.pump();
          expect(bridge.calls, [0, 1]);
          expect(find.text('2 / 3'), findsOneWidget);
          // Closing while this render is pending does not prevent disposal.
          await tester.pumpWidget(const SizedBox());
          bridge.pending[1].complete(page(1));
          await tester.pump();
          expect(tester.takeException(), isNull);
        },
      );
    }
  }
}
