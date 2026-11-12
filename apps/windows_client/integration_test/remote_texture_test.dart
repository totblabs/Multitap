import 'dart:io';
import 'dart:typed_data';
import 'dart:ui' as ui;

import 'package:flutter/material.dart';
import 'package:flutter/rendering.dart';
import 'package:flutter/services.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:integration_test/integration_test.dart';

void main() {
  IntegrationTestWidgetsFlutterBinding.ensureInitialized();
  testWidgets('Actual Windows MF decode is rendered by the Flutter texture', (
    tester,
  ) async {
    const fixture = String.fromEnvironment('FIXTURE_PATH');
    const output = String.fromEnvironment('OUTPUT_PATH');
    const width = int.fromEnvironment('FRAME_WIDTH', defaultValue: 1280);
    const height = int.fromEnvironment('FRAME_HEIGHT', defaultValue: 720);
    const interval = int.fromEnvironment('FRAME_INTERVAL_MS', defaultValue: 17);
    expect(fixture, isNotEmpty);
    const native = MethodChannel('multitap/remote');
    final bytes = await File(fixture).readAsBytes();
    final texture = await native.invokeMethod<int>('start', {
      'width': width,
      'height': height,
    });
    expect(texture, isNotNull);
    final boundary = GlobalKey();
    await tester.pumpWidget(
      MaterialApp(
        home: Scaffold(
          backgroundColor: Colors.black,
          body: Center(
            child: ConstrainedBox(
              constraints: BoxConstraints(maxWidth: width.toDouble()),
              child: AspectRatio(
                aspectRatio: width / height,
                child: RepaintBoundary(
                  key: boundary,
                  child: Texture(textureId: texture!),
                ),
              ),
            ),
          ),
        ),
      ),
    );
    var offset = 0, supplied = 0;
    try {
      while (offset + 4 <= bytes.length && supplied < 60) {
        final size = ByteData.sublistView(
          bytes,
          offset,
          offset + 4,
        ).getUint32(0, Endian.little);
        offset += 4;
        expect(size, inExclusiveRange(0, 4 * 1024 * 1024));
        expect(offset + size, lessThanOrEqualTo(bytes.length));
        final packet = Uint8List(size + 12)..[0] = 1;
        ByteData.sublistView(packet)
            .setUint64(4, supplied * 1000000 ~/ 60, Endian.little);
        packet.setRange(12, packet.length, bytes, offset);
        await native.invokeMethod<void>('push', packet);
        offset += size;
        supplied++;
        await tester.runAsync(
          () => Future<void>.delayed(const Duration(milliseconds: interval)),
        );
        await tester.pump();
      }
      Map<String, dynamic> stats = {};
      for (var attempt = 0; attempt < 100; attempt++) {
        stats = (await native.invokeMapMethod<String, dynamic>('stats'))!;
        expect(stats['error'], 0);
        if ((stats['decodedFrames'] as int) >= supplied - 2) break;
        await tester.runAsync(
          () => Future<void>.delayed(const Duration(milliseconds: 20)),
        );
        await tester.pump();
      }
      expect(stats['decodedFrames'], greaterThanOrEqualTo(supplied - 2));
      expect(
        stats['audioReceivedFrames'],
        0,
        reason: 'This fixture contains no remote PCM packets.',
      );
      expect(
        stats['audioPlayedFrames'],
        0,
        reason: 'An empty audio queue must never report remote audio playback.',
      );
      await tester.pump(const Duration(milliseconds: 40));
      final image =
          await (boundary.currentContext!.findRenderObject()!
                  as RenderRepaintBoundary)
              .toImage();
      final rgba = (await image.toByteData(format: ui.ImageByteFormat.rawRgba))!
          .buffer
          .asUint8List();
      var color = 0;
      for (var index = 0; index < rgba.length; index += 4) {
        color += rgba[index] + rgba[index + 1] + rgba[index + 2];
      }
      expect(
        color,
        greaterThan(1000000),
        reason: 'Texture readback must contain actual decoded pixels, not a black placeholder.',
      );
      if (output.isNotEmpty) {
        final png = await image.toByteData(format: ui.ImageByteFormat.png);
        final file = File(output);
        await file.parent.create(recursive: true);
        await file.writeAsBytes(png!.buffer.asUint8List());
      }
      image.dispose();
      debugPrint(
        'Actual native texture: supplied=$supplied, decoded=${stats['decodedFrames']}, nonblack RGB checksum=$color, audioError=${stats['audioError']}',
      );
    } finally {
      await tester.pumpWidget(const SizedBox.shrink());
      await native.invokeMethod<void>('stop');
    }
  });
}
