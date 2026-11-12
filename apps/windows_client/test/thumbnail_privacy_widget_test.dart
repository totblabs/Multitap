import 'dart:async';
import 'dart:io';

import 'package:flutter/material.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:multitap_client/data/desktop_thumbnails.dart';

import 'desktop_thumbnail_tile_test.dart' show shell, picture, art;
import 'desktop_tile_test.dart' show vm;

class _HeldPictures implements ThumbnailSource {
  final pending = <Completer<DesktopThumbnail?>>[];
  final changes = StreamController<String>.broadcast();
  @override
  Stream<String> get updates => changes.stream;
  @override
  Future<DesktopThumbnail?> read(String id, {bool refresh = false}) {
    final held = Completer<DesktopThumbnail?>();
    pending.add(held);
    return held.future;
  }
}

class _PolicyStore extends DesktopThumbnails {
  _PolicyStore() : super(Directory('unused-memory-thumbnail-fixture'));
  @override
  Future<DesktopThumbnail?> read(
    String key,
    String id, {
    bool refresh = false,
  }) async => allowedEpoch(key, id) == null
      ? null
      : picture(const Duration(minutes: 1));
}

void main() {
  testWidgets(
    'new privacy-clear read wins over an older late decode/provider result',
    (tester) async {
      tester.view.devicePixelRatio = 1;
      tester.view.physicalSize = const Size(1440, 900);
      addTearDown(tester.view.resetPhysicalSize);
      addTearDown(tester.view.resetDevicePixelRatio);
      final source = _HeldPictures();
      await tester.pumpWidget(shell([vm('pve-149')], source));
      await tester.pump();
      await tester.pump();
      expect(source.pending.length, 1);
      source.changes.add('pve-149');
      await tester.pump();
      expect(source.pending.length, 2);
      source.pending[1].complete(null);
      await tester.pump();
      expect(art(tester, 'pve-149').thumbnail, isNull);
      source.pending[0].complete(picture(const Duration(minutes: 1)));
      await tester.pump();
      expect(
        art(tester, 'pve-149').thumbnail,
        isNull,
        reason: 'late old provider must not repopulate the neutral tile',
      );
      await tester.pumpWidget(const SizedBox.shrink());
      await source.changes.close();
      expect(tester.takeException(), isNull);
    },
  );
  for (final scale in [1.0, 1.5, 2.0]) {
    testWidgets(
      'Host unknown/locked policy immediately neutralizes retained picture at text scale $scale',
      (tester) async {
        tester.view.devicePixelRatio = 1;
        tester.view.physicalSize = const Size(960, 640);
        tester.platformDispatcher.textScaleFactorTestValue = scale;
        addTearDown(tester.view.resetPhysicalSize);
        addTearDown(tester.view.resetDevicePixelRatio);
        addTearDown(tester.platformDispatcher.clearTextScaleFactorTestValue);
        const key = '0123456789abcdef',
            epoch = '0123456789abcdef0000000000000001';
        final store = _PolicyStore();
        // Use the actual account source and synchronous accepted-store updates.
        final current = AccountThumbnails(store, key);
        store.observePrivacy(
          key,
          'pve-149',
          const ThumbnailPrivacy(epoch: epoch, allowed: true),
        );
        await tester.pumpWidget(shell([vm('pve-149')], current));
        await tester.pump();
        await tester.pump();
        expect(art(tester, 'pve-149').thumbnail, isNotNull);
        store.suspendAccount(key);
        await tester.pump();
        expect(art(tester, 'pve-149').thumbnail, isNull);
        store.observePrivacy(
          key,
          'pve-149',
          const ThumbnailPrivacy(epoch: epoch, allowed: true),
        );
        await tester.pump();
        await tester.pump();
        expect(art(tester, 'pve-149').thumbnail, isNotNull);
        store.observePrivacy(
          key,
          'pve-149',
          const ThumbnailPrivacy(epoch: epoch, allowed: false),
        );
        await tester.pump();
        expect(art(tester, 'pve-149').thumbnail, isNull);
        store.observePrivacy(
          key,
          'pve-149',
          const ThumbnailPrivacy(epoch: epoch, allowed: true),
        );
        await tester.pump();
        await tester.pump();
        expect(
          art(tester, 'pve-149').thumbnail,
          isNull,
          reason: 'a real false burned this epoch',
        );
        await tester.pumpWidget(const SizedBox.shrink());
        expect(tester.takeException(), isNull);
      },
    );
  }
  testWidgets(
    'workspace retained providers obey the existing runtime image cache byte budget',
    (tester) async {
      tester.view.devicePixelRatio = 1;
      tester.view.physicalSize = const Size(1920, 1080);
      addTearDown(tester.view.resetPhysicalSize);
      addTearDown(tester.view.resetDevicePixelRatio);
      final cache = PaintingBinding.instance.imageCache,
          oldBudget = PaintingBinding.instance.imageCache.maximumSizeBytes;
      final first = picture(const Duration(minutes: 1));
      cache.maximumSizeBytes = first.encodedBytes * 2;
      addTearDown(() {
        cache.maximumSizeBytes = oldBudget;
      });
      final source = _HeldPictures();
      await tester.pumpWidget(
        shell([vm('pve-149'), vm('pve-150'), vm('pve-151')], source),
      );
      await tester.pump();
      await tester.pump();
      expect(source.pending, hasLength(3));
      for (var i = 0; i < source.pending.length; i++) {
        source.pending[i].complete(picture(const Duration(minutes: 1)));
      }
      await tester.pump();
      await tester.pump();
      final retained = [
        'pve-149',
        'pve-150',
        'pve-151',
      ].where((id) => art(tester, id).thumbnail != null).length;
      expect(retained, lessThanOrEqualTo(2));
      await tester.pumpWidget(const SizedBox.shrink());
      await source.changes.close();
    },
  );
}
