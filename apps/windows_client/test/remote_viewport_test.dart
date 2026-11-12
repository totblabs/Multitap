import 'package:flutter/gestures.dart';
import 'package:flutter/material.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:multitap_client/ui/remote_viewport.dart';

void main() {
  for (final ratio in [1.0, 1.25, 1.5, 1.75, 2.0]) {
    testWidgets(
      'A 1:1 picture sits on whole physical pixels at ${ratio}x (no half-pixel blur)',
      (tester) async {
        tester.view.devicePixelRatio = ratio;
        tester.view.physicalSize = const Size(1600, 1000);
        addTearDown(tester.view.resetDevicePixelRatio);
        addTearDown(tester.view.resetPhysicalSize);
        // The window's physical area is 3 x 3 pixels larger than the 4x2
        // rounded request: plain centring would offset by 1.5 pixels.
        await tester.pumpWidget(
          MaterialApp(
            home: Align(
              alignment: Alignment.topLeft,
              child: SizedBox(
                width: 1267 / ratio,
                height: 763 / ratio,
                child: RemoteViewport(
                  frameSize: const Size(1264, 760),
                  mode: RemoteViewMode.fitNoUpscale,
                  captured: false,
                  builder: (_) => const SizedBox.expand(),
                ),
              ),
            ),
          ),
        );
        final bounds = tester.getRect(
          find.byKey(const Key('remote-image-bounds')),
        );
        double physical(double value) => value * ratio;
        expect(
          physical(bounds.left),
          closeTo(physical(bounds.left).roundToDouble(), 1e-6),
        );
        expect(
          physical(bounds.top),
          closeTo(physical(bounds.top).roundToDouble(), 1e-6),
        );
        expect(physical(bounds.width), closeTo(1264, 1e-6));
        expect(physical(bounds.height), closeTo(760, 1e-6));
        expect(physical(bounds.left), 1);
        expect(physical(bounds.top), 1);
      },
    );
  }
  test('Every physical size at every Windows scale requests its whole physical viewport', () {
    // Flutter's logical size is the physical size divided by the scale; at
    // 175 % multiplying back gave e.g. 2559.9999999999995 and a 2556-pixel
    // request: a 4-pixel black edge and no 1:1 mode for that window.
    final wrong = <String>[];
    for (final ratio in [1.0, 1.25, 1.5, 1.75, 2.0, 2.25, 2.5, 3.0]) {
      for (var width = 320; width <= 7680; width++) {
        final request = viewportResolution(
          Size(width / ratio, 1080 / ratio),
          ratio,
        )!;
        if (request.width != (width ~/ 4) * 4 || request.height != 1080) {
          wrong.add('$ratio: ${width}x1080 -> $request');
        }
      }
      for (var height = 180; height <= 4320; height++) {
        final request = viewportResolution(
          Size(1920 / ratio, height / ratio),
          ratio,
        )!;
        if (request.width != 1920 || request.height != (height ~/ 2) * 2) {
          wrong.add('$ratio: 1920x$height -> $request');
        }
      }
    }
    expect(wrong, isEmpty);
  });
  test('Monitors at 100-200 %: the request is the monitor itself, shown 1:1 on device pixels with nearest sampling', () {
    for (final (width, height) in [
      (1366, 768),
      (1600, 900),
      (1920, 1080),
      (1920, 1200),
      (2560, 1440),
      (2880, 1800),
      (3440, 1440),
      (3840, 2160),
      (5120, 1440),
      (5120, 2160),
      // Window client areas: odd sizes, as Windows reports them.
      (2557, 1381),
      (5117, 1403),
      (1265, 761),
    ]) {
      for (final ratio in [1.0, 1.25, 1.5, 1.75, 2.0]) {
        final where = '${width}x$height @ $ratio';
        final logical = Size(width / ratio, height / ratio);
        final request = viewportResolution(logical, ratio);
        expect(
          request,
          Size(((width ~/ 4) * 4).toDouble(), ((height ~/ 2) * 2).toDouble()),
          reason: where,
        );
        final shown = remoteDisplaySize(
          request!,
          logical,
          ratio,
          RemoteViewMode.fitNoUpscale,
        );
        expect(
          shown.width * ratio,
          closeTo(request.width, 1e-6),
          reason: where,
        );
        expect(
          shown.height * ratio,
          closeTo(request.height, 1e-6),
          reason: where,
        );
        expect(
          remoteTextureFilter(
            texture: request,
            shown: shown,
            pixelRatio: ratio,
          ),
          FilterQuality.none,
          reason: where,
        );
      }
    }
  });
  test('A texture smaller or larger than its device pixels is filtered, never nearest-scaled', () {
    for (final ratio in [1.0, 1.25, 1.5, 1.75, 2.0]) {
      const source = Size(1920, 1080);
      final shown = Size(1920 / ratio, 1080 / ratio);
      // The Host encodes a downscaled video under the 1920x1080 source.
      expect(
        remoteTextureFilter(
          texture: const Size(1440, 810),
          shown: shown,
          pixelRatio: ratio,
        ),
        FilterQuality.low,
      );
      expect(
        remoteTextureFilter(texture: source, shown: shown, pixelRatio: ratio),
        FilterQuality.none,
      );
      // A window smaller than the picture (fit) and a 1-pixel mismatch.
      expect(
        remoteTextureFilter(
          texture: source,
          shown: Size(shown.width * 0.75, shown.height * 0.75),
          pixelRatio: ratio,
        ),
        FilterQuality.low,
      );
      expect(
        remoteTextureFilter(
          texture: const Size(1924, 1080),
          shown: shown,
          pixelRatio: ratio,
        ),
        FilterQuality.low,
      );
    }
  });
  test('An arbitrary window width requests a tested Windows source mode at actual DPI', () {
    expect(viewportResolution(const Size(1582, 952), 1), const Size(1580, 952));
    expect(
      viewportResolution(const Size(1265.6, 761.6), 1.25),
      const Size(1580, 952),
    );
    expect(viewportResolution(const Size(1584, 952), 1), const Size(1584, 952));
    expect(
      viewportResolution(const Size(1920, 1080), 1),
      const Size(1920, 1080),
    );
    for (var width = 320; width <= 3840; width++) {
      final requested = viewportResolution(Size(width.toDouble(), 952), 1)!;
      expect(requested.width.toInt() % 4, 0);
      expect(width - requested.width, inInclusiveRange(0, 3));
      expect(requested.height, 952);
    }
    // Render the real source with its own aspect and geometry. A two-pixel
    // edge does not become a fictitious1582-pixel display or stretched image.
    expect(
      remoteDisplaySize(
        const Size(1580, 952),
        const Size(1582, 952),
        1,
        RemoteViewMode.fit,
      ),
      const Size(1580, 952),
    );
  });
  test('Automatic resolution requests complete physical viewport geometry and rejects invalid representation without stretching', () {
    expect(
      viewportResolution(const Size(1536, 864), 1.25),
      const Size(1920, 1080),
    );
    expect(
      viewportResolution(const Size(3188, 1176), 1),
      const Size(3188, 1176),
    );
    expect(viewportResolution(const Size(1281, 721), 1), const Size(1280, 720));
    // Geometry is preserved; device/type admission is a separate boundary.
    // A tiny window keeps the current resolution.
    expect(
      viewportResolution(const Size(4000, 2160), 1),
      const Size(4000, 2160),
    );
    expect(
      viewportResolution(const Size(5120, 1440), 1),
      const Size(5120, 1440),
    );
    expect(viewportResolution(const Size(300, 170), 1), isNull);
    expect(viewportResolution(const Size(1280, 720), 0), isNull);
    expect(viewportResolution(const Size(double.infinity, 720), 1), isNull);
  });
  test('5K, portrait, ultrawide and 8K physical requests keep DPI/source alignment before actual device admission', () {
    for (final (logical, dpr, physical) in [
      (const Size(2560, 1440), 2.0, const Size(5120, 2880)),
      (const Size(1440, 2560), 2.0, const Size(2880, 5120)),
      (const Size(5120, 1440), 1.0, const Size(5120, 1440)),
      (const Size(3840, 2160), 2.0, const Size(7680, 4320)),
    ]) {
      expect(viewportResolution(logical, dpr), physical);
    }
    expect(
      viewportResolution(const Size(4097, 2305), 1),
      const Size(4096, 2304),
    );
    expect(viewportResolution(const Size(0x80000000, 1080), 1), isNull);
    expect(viewportResolution(const Size(1e308, 720), 1e308), isNull);
    expect(viewportResolution(const Size(double.nan, 720), 1), isNull);
    expect(viewportResolution(const Size(1920, 1080), double.infinity), isNull);
  });
  test('Fit keeps source ratio, never enlarges physical pixels by default, and 1:1 respects Windows DPI', () {
    expect(
      remoteDisplaySize(
        const Size(1280, 720),
        const Size(1800, 1000),
        1.5,
        RemoteViewMode.fitNoUpscale,
      ),
      const Size(1280 / 1.5, 480),
    );
    final fit = remoteDisplaySize(
      const Size(3840, 1080),
      const Size(1000, 700),
      1,
      RemoteViewMode.fitNoUpscale,
    );
    expect(fit.width, closeTo(1000, 0.0001));
    expect(fit.height, closeTo(281.25, 0.0001));
    expect(
      remoteDisplaySize(
        const Size(3840, 1080),
        const Size(1000, 700),
        2,
        RemoteViewMode.pixels,
      ),
      const Size(1920, 540),
    );
  });

  testWidgets(
    'Original-pixel dual viewport scrolls and pointer coordinates remain relative to the complete composite frame',
    (tester) async {
      tester.view.physicalSize = const Size(1200, 800);
      tester.view.devicePixelRatio = 1;
      addTearDown(tester.view.resetPhysicalSize);
      addTearDown(tester.view.resetDevicePixelRatio);
      Offset? sourcePoint;
      await tester.pumpWidget(
        MaterialApp(
          home: Scaffold(
            body: Center(
              child: SizedBox(
                width: 800,
                height: 600,
                child: MediaQuery(
                  data: const MediaQueryData(devicePixelRatio: 2),
                  child: RemoteViewport(
                    frameSize: const Size(3840, 1080),
                    mode: RemoteViewMode.pixels,
                    captured: false,
                    builder: (display) => Listener(
                      onPointerDown: (event) => sourcePoint = Offset(
                        event.localPosition.dx / display.width,
                        event.localPosition.dy / display.height,
                      ),
                      child: const ColoredBox(color: Colors.blue),
                    ),
                  ),
                ),
              ),
            ),
          ),
        ),
      );
      final frame = find.byKey(const Key('remote-image-bounds'));
      expect(tester.getSize(frame), const Size(1920, 540));
      final horizontal = find.byWidgetPredicate(
        (widget) =>
            widget is SingleChildScrollView &&
            widget.scrollDirection == Axis.horizontal,
      );
      final scrollView = tester.widget<SingleChildScrollView>(horizontal);
      scrollView.controller!.jumpTo(500);
      await tester.pump();
      final visiblePoint = tester.getRect(horizontal).center;
      final fullFrame = tester.getRect(frame);
      final gesture = await tester.createGesture(kind: PointerDeviceKind.mouse);
      await gesture.addPointer(location: visiblePoint);
      await gesture.down(visiblePoint);
      await gesture.up();
      expect(
        sourcePoint!.dx,
        closeTo((visiblePoint.dx - fullFrame.left) / 1920, 0.0001),
      );
      expect(
        sourcePoint!.dy,
        closeTo((visiblePoint.dy - fullFrame.top) / 540, 0.0001),
      );
      expect(sourcePoint!.dx, greaterThan(0.4));
      expect(tester.takeException(), isNull);
    },
  );
}
