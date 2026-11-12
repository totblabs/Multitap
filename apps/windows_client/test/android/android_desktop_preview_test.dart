// desktop-preview-v1 on the Android launcher: the broker's recent picture of
// each assigned desktop on its card, without connecting first. A fake broker
// and the in-memory store: no broker, Host, file or screen is involved.
import 'dart:async';
import 'dart:typed_data';

import 'package:flutter/material.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:multitap_client/android/android_app.dart';
import 'package:multitap_client/android/android_store.dart';
import 'package:multitap_client/data/broker_api.dart';
import 'package:multitap_client/data/desktop_previews.dart';
import 'package:multitap_client/data/desktop_thumbnails.dart';
import 'package:multitap_client/ui/desktop_art.dart';

import '../desktop_preview_test.dart' show captured, jpeg, relocked, unlocked;
import '../pc_power_test.dart' show pcId, pcJson;
import 'android_launcher_test.dart' show FakeBroker, MemoryStore, vm;
import 'android_updates_test.dart' show FakePlatform;

BrokerDesktop desk(String id, {String? epoch = unlocked, bool allowed = true}) {
  final base = vm(id: id, name: 'VM $id');
  return BrokerDesktop(
    id: base.id,
    name: base.name,
    kind: base.kind,
    description: base.description,
    os: base.os,
    status: base.status,
    canConnect: base.canConnect,
    canStream: base.canStream,
    hostConnected: base.hostConnected,
    power: base.power,
    powerCanForceStop: base.powerCanForceStop,
    thumbnailPrivacy: epoch == null
        ? null
        : ThumbnailPrivacy(epoch: epoch, allowed: allowed),
  );
}

/// Records whether the launcher let go of the picture's decoded pixels.
class SpyImage extends MemoryImage {
  SpyImage() : super(Uint8List.fromList(jpeg));
  final _evictions = <void>[];
  bool get evicted => _evictions.isNotEmpty;
  @override
  Future<bool> evict({
    ImageCache? cache,
    ImageConfiguration configuration = ImageConfiguration.empty,
  }) {
    _evictions.add(null);
    return super.evict(cache: cache, configuration: configuration);
  }
}

class PreviewBroker extends FakeBroker implements DesktopPreviewBrokerApi {
  PreviewBroker(super.inventory);
  final offered = <String>{};

  /// A broker answer (JSON), or a function making the preview itself.
  final answers = <String, Object?>{};
  final reads = <String>[];
  final spies = <SpyImage>[];
  Completer<void>? hold;

  @override
  bool previewOffered(String token, String desktopId) =>
      offered.contains(desktopId);

  @override
  Future<DesktopPreview> desktopPreview(String token, String desktopId) async {
    reads.add(desktopId);
    final gate = hold;
    if (gate != null) await gate.future;
    final value = answers[desktopId];
    if (value == #spy) {
      final spy = SpyImage();
      spies.add(spy);
      return DesktopPreview(
        DesktopPreviewState.captured,
        picture: DesktopThumbnail(
          image: spy,
          modified: DateTime.now().subtract(const Duration(minutes: 1)),
          remote: true,
        ),
      );
    }
    return DesktopPreview.parse(value);
  }
}

Map<String, Object?> state(String value) => {
  'protocol': desktopPreviewProtocol,
  'state': value,
};

void main() {
  TestWidgetsFlutterBinding.ensureInitialized();
  tearDown(() {
    TestDefaultBinaryMessengerBinding.instance.defaultBinaryMessenger
        .setMockMethodCallHandler(AndroidStore.channel, null);
  });

  // Session renewal and the 15 s nudge read this; it moves only when a test
  // moves it.
  var now = DateTime.now();

  Future<void> signIn(WidgetTester t) async {
    await t.enterText(find.byKey(const Key('sign-in-password')), 'secret-pass');
    await t.tap(find.byKey(const Key('login-submit')));
    await t.pumpAndSettle();
  }

  Future<void> open(
    WidgetTester t,
    PreviewBroker broker, {
    Map<String, String>? prefs,
  }) async {
    now = DateTime.now();
    t.view.physicalSize = const Size(1080, 2400);
    t.view.devicePixelRatio = 3;
    addTearDown(t.view.resetPhysicalSize);
    addTearDown(t.view.resetDevicePixelRatio);
    MemoryStore(
      origin: 'https://vdi.example',
      username: 'tester',
      prefs: prefs,
    ).install();
    await t.pumpWidget(
      AndroidVdiApp(
        apiFactory: (_) => broker,
        updatePlatform: FakePlatform(),
        clock: () => now,
      ),
    );
    await t.pumpAndSettle();
    await signIn(t);
  }

  /// One fallback list read (the live channel is down in these tests).
  Future<void> listRead(WidgetTester t) async {
    await t.pump(androidFallbackPollInterval);
    await t.pump();
    await t.pump();
  }

  DesktopArt art(WidgetTester t, String id) => t.widget<DesktopArt>(
    find.descendant(
      of: find.byKey(Key('card-$id')),
      matching: find.byType(DesktopArt),
    ),
  );

  String? text(WidgetTester t, String key) => t
      .widget<Text>(
        find.descendant(
          of: find.byKey(Key(key)),
          matching: find.byWidgetPredicate((widget) => widget is Text),
        ),
      )
      .data;

  testWidgets(
    'A desktop never streamed here shows its Host picture with its age; locked, sign-in and asleep say so; the card keeps its size',
    (t) async {
      final broker =
          PreviewBroker([
              desk('pve-149'),
              desk('pve-150', epoch: relocked, allowed: false),
              desk('pve-151', epoch: relocked, allowed: false),
              desk('pve-152'),
              desk('pve-153'),
              BrokerDesktop.fromJson({
                ...pcJson(),
                'host': {
                  'connected': true,
                  'thumbnailPrivacy': {
                    'protocol': thumbnailPrivacyProtocol,
                    'epoch': unlocked,
                    'allowed': true,
                  },
                },
              }),
            ])
            ..offered.addAll({'pve-149', 'pve-150', 'pve-151', 'pve-152', pcId})
            ..answers['pve-149'] = captured(ageMs: 60000)
            ..answers['pve-150'] = state('locked')
            ..answers['pve-151'] = state('signin')
            ..answers['pve-152'] = state('asleep')
            ..answers[pcId] = captured(ageMs: 120000);
      await open(t, broker);
      expect(broker.reads.toSet(), {
        'pve-149',
        'pve-150',
        'pve-151',
        'pve-152',
        pcId,
      });
      expect(broker.reads.length, 5, reason: 'one ask each at sign-in');
      final image = art(t, 'pve-149').thumbnail;
      expect(image, isA<MemoryImage>());
      expect((image! as MemoryImage).bytes, jpeg);
      expect(text(t, 'thumbnail-caption-pve-149'), '1분 전');
      expect(art(t, 'pve-150').thumbnail, isNull);
      expect(text(t, 'thumbnail-empty-pve-150'), '잠겨 있어요');
      expect(text(t, 'thumbnail-empty-pve-151'), '로그인 화면');
      expect(text(t, 'thumbnail-empty-pve-152'), '화면이 꺼져 있어요');
      // A physical PC's Host answers the same way.
      expect((art(t, pcId).thumbnail! as MemoryImage).bytes, jpeg);
      // Not offered: never asked, and the card says nothing new.
      expect(art(t, 'pve-153').thumbnail, isNull);
      expect(find.byKey(const Key('thumbnail-caption-pve-153')), findsNothing);
      expect(find.byKey(const Key('thumbnail-empty-pve-153')), findsNothing);
      // One card design: a picture changes no card's size.
      final size = t.getSize(find.byKey(const Key('card-pve-153')));
      for (final id in ['pve-149', 'pve-150', 'pve-152']) {
        expect(t.getSize(find.byKey(Key('card-$id'))), size);
      }
      expect(t.takeException(), isNull);
      await t.pumpWidget(const SizedBox.shrink());
    },
  );

  testWidgets(
    'The picture shows only while the list still shows the unlocked session it was taken under',
    (t) async {
      final broker = PreviewBroker([desk('pve-149')])
        ..offered.add('pve-149')
        ..answers['pve-149'] = captured();
      await open(t, broker);
      expect(art(t, 'pve-149').thumbnail, isNotNull);
      // A lock or user switch moves the epoch.
      broker.inventory = [desk('pve-149', epoch: relocked, allowed: false)];
      await listRead(t);
      expect(art(t, 'pve-149').thumbnail, isNull);
      expect(find.byKey(const Key('thumbnail-caption-pve-149')), findsNothing);
      // The same epoch turned not allowed hides it too.
      broker.inventory = [desk('pve-149', allowed: false)];
      await listRead(t);
      expect(art(t, 'pve-149').thumbnail, isNull);
      // No privacy observation at all: no Host picture.
      broker.inventory = [desk('pve-149', epoch: null)];
      await listRead(t);
      expect(art(t, 'pve-149').thumbnail, isNull);
      expect(broker.reads, ['pve-149'], reason: 'within 15 s: no new ask');
      expect(t.takeException(), isNull);
      await t.pumpWidget(const SizedBox.shrink());
    },
  );

  testWidgets(
    'It asks at sign-in, about once a minute, never in the background, and at once on return',
    (t) async {
      final broker = PreviewBroker([desk('pve-149')])
        ..offered.add('pve-149')
        ..answers['pve-149'] = captured();
      await open(t, broker);
      expect(broker.reads, ['pve-149']);
      await t.pump(const Duration(seconds: 30));
      expect(broker.reads.length, 1);
      await t.pump(const Duration(seconds: 31));
      expect(broker.reads.length, 2);
      final binding = t.binding;
      binding.handleAppLifecycleStateChanged(AppLifecycleState.inactive);
      binding.handleAppLifecycleStateChanged(AppLifecycleState.hidden);
      binding.handleAppLifecycleStateChanged(AppLifecycleState.paused);
      await t.pump(const Duration(seconds: 125));
      expect(
        broker.reads.length,
        2,
        reason: 'nothing is captured while nobody looks',
      );
      binding.handleAppLifecycleStateChanged(AppLifecycleState.hidden);
      binding.handleAppLifecycleStateChanged(AppLifecycleState.inactive);
      binding.handleAppLifecycleStateChanged(AppLifecycleState.resumed);
      await t.pump();
      await t.pump();
      expect(broker.reads.length, 3, reason: 'coming back asks at once');
      await t.pumpWidget(const SizedBox.shrink());
    },
  );

  testWidgets(
    'A new desktop, or a changed Windows session, is asked at once, at most every 15 s each',
    (t) async {
      final broker = PreviewBroker([desk('pve-149')])
        ..offered.addAll({'pve-149', 'pve-150'})
        ..answers['pve-149'] = captured()
        ..answers['pve-150'] = captured();
      await open(t, broker);
      expect(broker.reads, ['pve-149']);
      // A new desktop: asked with the list read, not a minute later.
      broker.inventory = [desk('pve-149'), desk('pve-150')];
      await listRead(t);
      expect(broker.reads, ['pve-149', 'pve-150']);
      expect(art(t, 'pve-150').thumbnail, isNotNull);
      // The Windows session changed less than 15 s after the last ask.
      broker.inventory = [desk('pve-149', epoch: relocked), desk('pve-150')];
      await listRead(t);
      expect(broker.reads.length, 2);
      expect(art(t, 'pve-149').thumbnail, isNull);
      // 15 s later the next list read asks it.
      now = now.add(const Duration(seconds: 16));
      await listRead(t);
      expect(broker.reads, ['pve-149', 'pve-150', 'pve-149']);
      expect(art(t, 'pve-149').thumbnail, isNotNull);
      expect(t.takeException(), isNull);
      await t.pumpWidget(const SizedBox.shrink());
    },
  );

  testWidgets(
    'Sign-out lets go of every picture, a late answer included; the next sign-in starts empty',
    (t) async {
      final broker = PreviewBroker([desk('pve-149')])
        ..offered.add('pve-149')
        ..answers['pve-149'] = #spy;
      await open(t, broker);
      final first = broker.spies.single;
      expect(art(t, 'pve-149').thumbnail, same(first));
      expect(first.evicted, isFalse);
      // The next minute's answer is still on its way at sign-out.
      broker.hold = Completer<void>();
      await t.pump(const Duration(seconds: 61));
      expect(broker.reads.length, 2);
      await t.tap(find.byKey(const Key('account-menu')));
      await t.pumpAndSettle();
      await t.tap(find.byKey(const Key('sign-out')));
      await t.pumpAndSettle();
      await t.tap(find.byKey(const Key('confirm-sign-out')));
      await t.pumpAndSettle();
      expect(find.byKey(const Key('sign-in-password')), findsOneWidget);
      expect(first.evicted, isTrue);
      broker.hold!.complete();
      await t.pump();
      expect(broker.spies.length, 2);
      expect(broker.spies.last.evicted, isTrue, reason: 'arrived too late');
      // Signed in again: nothing from before while the new answer is pending.
      broker.hold = Completer<void>();
      await signIn(t);
      expect(broker.reads.length, 3);
      expect(art(t, 'pve-149').thumbnail, isNull);
      expect(find.byKey(const Key('thumbnail-caption-pve-149')), findsNothing);
      broker.hold!.complete();
      await t.pump();
      await t.pump();
      expect(art(t, 'pve-149').thumbnail, same(broker.spies.last));
      expect(t.takeException(), isNull);
      await t.pumpWidget(const SizedBox.shrink());
    },
  );

  testWidgets(
    'Nothing is asked where the list offers no picture or 화면 미리보기 is off',
    (t) async {
      final unoffered = PreviewBroker([desk('pve-149')])
        ..answers['pve-149'] = captured();
      await open(t, unoffered);
      await t.pump(const Duration(seconds: 61));
      expect(unoffered.reads, isEmpty);
      expect(art(t, 'pve-149').thumbnail, isNull);
      await t.pumpWidget(const SizedBox.shrink());

      final off = PreviewBroker([desk('pve-149')])
        ..offered.add('pve-149')
        ..answers['pve-149'] = captured();
      await open(t, off, prefs: {'launcher.previews': 'off'});
      await t.pump(const Duration(seconds: 61));
      expect(off.reads, isEmpty);
      expect(find.byType(DesktopArt), findsNothing);
      await t.pumpWidget(const SizedBox.shrink());
    },
  );
}
