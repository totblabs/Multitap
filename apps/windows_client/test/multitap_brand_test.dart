import 'dart:io';
import 'dart:typed_data';
import 'dart:ui' as ui;

import 'package:crypto/crypto.dart';
import 'package:flutter/material.dart';
import 'package:flutter/rendering.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:multitap_client/ui/components.dart';

void main() {
  test(
    'the power-strip master and existing package identities stay exact',
    () {
      final master = File('assets/brand/multitap.svg').readAsBytesSync();
      expect(
        sha256.convert(master).toString(),
        'd66d6736051f3cbdc7026f224a11034159a0acbb4a6c442bc39449d3e21e2ed8',
      );
      final manifest = File('android/app/src/main/AndroidManifest.xml')
          .readAsStringSync();
      expect(manifest, contains('android:label="Multitap"'));
      expect(
        File('android/app/build.gradle.kts').readAsStringSync(),
        contains('applicationId = "me.totb.multitap_client"'),
      );
      final resource = File('windows/runner/Runner.rc').readAsStringSync();
      final identities = RegExp(
        r'#if defined\(FLUTTER_VERSION_BUILD\) && FLUTTER_VERSION_BUILD >= 20000\r?\n([\s\S]*?)#else\r?\n([\s\S]*?)#endif',
      ).firstMatch(resource);
      expect(identities, isNotNull);
      final expectedIdentities = [
        {
          'DISPLAY_NAME': 'Multitap Host',
          'FILE_DESCRIPTION': 'Multitap Host Settings',
          'INTERNAL_NAME': 'MultitapHostSettings',
          'ORIGINAL_FILENAME': 'MultitapHostSettings.exe',
        },
        {
          'DISPLAY_NAME': 'Multitap',
          'FILE_DESCRIPTION': 'Multitap Windows Client',
          'INTERNAL_NAME': 'Multitap',
          'ORIGINAL_FILENAME': 'Multitap.exe',
        },
      ];
      for (var branch = 0; branch < expectedIdentities.length; branch++) {
        for (final entry in expectedIdentities[branch].entries) {
          expect(
            identities!.group(branch + 1),
            contains('#define PRODUCT_${entry.key} "${entry.value}"'),
          );
        }
      }
      for (final entry in {
        'ProductName': 'DISPLAY_NAME',
        'FileDescription': 'FILE_DESCRIPTION',
        'InternalName': 'INTERNAL_NAME',
        'OriginalFilename': 'ORIGINAL_FILENAME',
      }.entries) {
        expect(resource, contains('"${entry.key}", PRODUCT_${entry.value}'));
      }
      final ico = File('windows/runner/resources/app_icon.ico')
          .readAsBytesSync();
      final table = ByteData.sublistView(ico);
      expect(table.getUint16(2, Endian.little), 1);
      expect(table.getUint16(4, Endian.little), 9);
      for (final (i, size) in [16, 20, 24, 32, 40, 48, 64, 128, 256].indexed) {
        final entry = 6 + i * 16;
        expect(table.getUint8(entry), size == 256 ? 0 : size);
        expect(table.getUint8(entry + 1), size == 256 ? 0 : size);
        final length = table.getUint32(entry + 8, Endian.little);
        final offset = table.getUint32(entry + 12, Endian.little);
        expect(length, greaterThan(0));
        expect(offset, greaterThanOrEqualTo(6 + 9 * 16));
        expect(offset + length, lessThanOrEqualTo(ico.length));
      }
      // The Android layers carry the same path as the master: the dark mark
      // over the amber background, and the bare ones for the themed icon and
      // the splash. The status bar icon is the small drawing in a 24 dp box.
      final masterPath = RegExp(' d="([^"]+)"')
          .firstMatch(String.fromCharCodes(master))!
          .group(1)!;
      String drawable(String name) => File(
        'android/app/src/main/res/drawable/multitap_$name.xml',
      ).readAsStringSync();
      for (final (name, colour) in [
        ('foreground', '#FF1E1503'),
        ('monochrome', '#FF000000'),
        ('symbol', '#FFF5A623'),
      ]) {
        expect(drawable(name), contains('android:pathData="$masterPath"'));
        expect(drawable(name), contains('android:fillColor="$colour"'));
      }
      expect(drawable('background'), contains('android:fillColor="#FFF5A623"'));
      expect(drawable('notification'), contains('android:viewportWidth="24"'));
    },
  );

  Future<ByteData> paint(WidgetTester t, Widget mark, int size) async {
    final boundary = GlobalKey();
    await t.pumpWidget(
      Directionality(
        textDirection: TextDirection.ltr,
        child: Center(
          child: RepaintBoundary(key: boundary, child: mark),
        ),
      ),
    );
    late ByteData rgba;
    await t.runAsync(() async {
      final image =
          await (boundary.currentContext!.findRenderObject()!
                  as RenderRepaintBoundary)
              .toImage(pixelRatio: 1);
      expect(image.width, size);
      rgba = (await image.toByteData(format: ui.ImageByteFormat.rawRgba))!;
      image.dispose();
    });
    return rgba;
  }

  testWidgets(
    'Flutter paints the bare power strip in one colour: bar, three holes, cord',
    (t) async {
      final rgba = await paint(
        t,
        const ProductMark(size: 120, color: ProductMark.amber),
        120,
      );
      int color(int x, int y) => rgba.getUint32((y * 120 + x) * 4);
      // The bar left of, between and right of the holes, and the cord.
      for (final point in [
        (32, 61),
        (57, 61),
        (79, 61),
        (104, 61),
        (60, 42),
        (60, 80),
        (16, 61),
        (8, 85),
      ]) {
        expect(color(point.$1, point.$2), 0xf5a623ff, reason: '$point');
      }
      // The three holes, and outside the mark: no tile behind it.
      for (final point in [
        (46, 61),
        (68, 61),
        (90, 61),
        (60, 30),
        (30, 95),
        (8, 110),
        (2, 2),
        (117, 117),
      ]) {
        expect(color(point.$1, point.$2) & 0xff, 0, reason: '$point');
      }
    },
  );

  testWidgets('the small drawing keeps three separate holes at 30 px', (
    t,
  ) async {
    final rgba = await paint(
      t,
      const ProductMark(size: 30, color: ProductMark.amber),
      30,
    );
    int color(int x, int y) => rgba.getUint32((y * 30 + x) * 4);
    // Hole centres at 43, 67 and 91 of 120, with solid bar between them.
    for (final x in [10, 16, 22]) {
      expect(color(x, 15) & 0xff, lessThan(0x40), reason: 'hole at $x');
    }
    for (final x in [6, 13, 19, 26]) {
      expect(color(x, 15), 0xf5a623ff, reason: 'bar at $x');
    }
  });

  testWidgets('the app icon tile is amber with the dark mark', (t) async {
    final rgba = await paint(
      t,
      const ProductMark(size: 160, tile: true),
      160,
    );
    int color(int x, int y) => rgba.getUint32((y * 160 + x) * 4);
    // Tile, then the mark moved by (20, 17): bar, a hole (tile shows), cord.
    expect(color(10, 80), 0xf5a623ff);
    expect(color(80, 150), 0xf5a623ff);
    expect(color(52, 78), 0x1e1503ff);
    expect(color(28, 102), 0x1e1503ff);
    expect(color(66, 78), 0xf5a623ff);
    expect(color(2, 2) & 0xff, 0);
  });

  testWidgets('the wordmark is one word in one weight and one colour', (
    t,
  ) async {
    for (final dark in [false, true]) {
      await t.pumpWidget(
        Directionality(
          textDirection: TextDirection.ltr,
          child: Center(child: Brand(onDark: dark)),
        ),
      );
      final word = t.widget<Text>(find.byKey(const Key('brand-vdi')));
      expect(word.data, 'Multitap');
      expect(word.style!.fontFamily, 'Pretendard');
      expect(word.style!.fontWeight, FontWeight.w700);
      expect(word.style!.letterSpacing, closeTo(-.04 * 22, 1e-9));
      expect(
        find.descendant(
          of: find.byType(Brand),
          matching: find.bySubtype<Text>(),
        ),
        findsNWidgets(2),
      );
      final line = t.widget<Text>(find.byKey(const Key('brand-for')));
      expect(line.textSpan!.toPlainText(), 'for Proxmox VE');
      // The mark stands beside the word, bare, in the amber of "Proxmox VE".
      final mark = t.widget<ProductMark>(find.byType(ProductMark));
      expect(mark.tile, isFalse);
      expect(
        mark.color,
        ((line.textSpan! as TextSpan).children!.single as TextSpan)
            .style!
            .color,
      );
      expect(
        t.getCenter(find.byType(ProductMark)).dx,
        lessThan(t.getTopLeft(find.byKey(const Key('brand-vdi'))).dx),
      );
    }
  });
}
