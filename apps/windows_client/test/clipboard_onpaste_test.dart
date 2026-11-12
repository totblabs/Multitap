import 'dart:async';
import 'dart:convert';

import 'package:flutter/services.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:multitap_client/data/clipboard_limits.dart';
import 'package:multitap_client/data/clipboard_policy.dart';
import 'package:multitap_client/data/clipboard_session.dart';
import 'package:multitap_client/data/native_clipboard.dart';
import 'package:multitap_client/data/product_log.dart';

import 'clipboard_pull_test.dart' as pull;
import 'clipboard_scale_test.dart' as scale;
import 'clipboard_session_test.dart' as base;

// The on-paste revision (X-Multitap-Clipboard-OnPaste: 1): a copy is silent. Files and folders are never pushed, text and images
// only while small; everything else is announced (clipboard.offer) and rendered when something pastes. A new copy withdraws the older offers,
// the Host reports a paste that waits (clipboard.paste.waiting), our card is the only progress (no Explorer dialog) and a cancel ends what
// is blocked in the platform. Every other pair keeps its behaviour.
Map<String, dynamic> onPasteLimits() => {
  ...scale.scaleLimits(),
  'onPasteProfile': 1,
  'pushTextBytes': 1024 * 1024,
  'pushImageBytes': 8 * 1024 * 1024,
};
Map<String, dynamic> onPasteReady({Map<String, dynamic>? policy}) => {
  ...base.ready,
  'limits': onPasteLimits(),
  'clipboardPolicy': ?policy,
};

class OnPasteClipboard extends scale.ScaleClipboard
    implements ClipboardOnPasteBridge, ClipboardClaimBridge {
  @override
  Future<int?> claim(String owner, String requestId) async {
    events.add('claim');
    return null;
  }

  @override
  bool get supportsOnPaste => true;
  bool onPasteOn = false;
  @override
  set onPaste(bool value) => onPasteOn = value;

  /// (offer id, format, entries) in the order the session published them.
  final publishedContent = <(String, String, List<ClipboardItem>)>[];
  Object? publishContentFailure;
  Completer<void>? publishGate;
  @override
  Future<void> publishOfferOnPaste(
    String owner,
    String offerId,
    String format,
    List<ClipboardItem> entries,
  ) async {
    events.add('publishOfferOnPaste');
    await publishGate?.future;
    if (publishContentFailure != null) throw publishContentFailure!;
    publishedContent.add((offerId, format, entries));
  }

  List<String> offersToDrop = const [];
  int drops = 0;
  @override
  Future<List<String>> dropOffers(String owner) async {
    events.add('dropOffers');
    drops++;
    final ids = offersToDrop;
    offersToDrop = const [];
    return ids;
  }

  int aborts = 0, supersedes = 0;
  final retired = <String>[], withdrawn = <String>[];
  @override
  Future<void> pullAbort(String owner) async {
    events.add('pullAbort');
    aborts++;
  }

  @override
  Future<void> pullSupersede(String owner) async {
    events.add('pullSupersede');
    supersedes++;
  }

  @override
  Future<void> pullRetire(String owner, String offerId) async {
    events.add('pullRetire');
    retired.add(offerId);
  }

  @override
  Future<void> withdrawOffer(String owner, String offerId) async {
    events.add('withdrawOffer');
    withdrawn.add(offerId);
  }
}

void main() {
  TestWidgetsFlutterBinding.ensureInitialized();
  late base.ClipboardSocket socket;
  late OnPasteClipboard bridge;
  late ClipboardSession session;

  ClipboardSession make({
    ClipboardBridge? with_,
    Duration linger = Duration.zero,
  }) => ClipboardSession(
    policy: const ClipboardPolicy.all(),
    url: Uri.parse(
      'wss://cloud.example.com/api/v1/sessions/${base.sessionId}/clipboard',
    ),
    token: 'test-token',
    sessionId: base.sessionId,
    bridge: with_ ?? bridge,
    dialer: (url, token) async => socket,
    pullLinger: linger,
  );
  Future<void> connect([Map<String, dynamic>? ready]) async {
    final starting = session.start();
    await base.settle();
    socket.emit(ready ?? onPasteReady());
    await starting;
  }

  setUp(() {
    socket = base.ClipboardSocket();
    bridge = OnPasteClipboard();
    session = make();
  });
  tearDown(() async => session.close());

  List<Map<String, dynamic>> sentOf(String type) => [
    for (final value in socket.sent)
      if (value['type'] == type) value,
  ];
  ClipboardCapture offerOf(
    String format,
    List<ClipboardItem> items, {
    int tops = 0,
  }) => ClipboardCapture(format: format, items: items, tops: tops, offer: true);

  group('negotiation', () {
    test('the native compatibility adapter can omit the on-paste header', () {
      final native = NativeClipboardBridge(
        allowedText: true,
        allowedFiles: true,
        announceOnPaste: false,
      );
      session = make(with_: native);
      expect(native.supportsOnPaste, isFalse);
      expect(session.dialHeaders, isNot(contains(clipboardOnPasteHeader)));
    });
    test(
      'the numbers are accepted exactly, on top of the scale revision only',
      () {
        final parsed = ClipboardLimits.parse(
          onPasteLimits(),
          chunkBytes: 32768,
        );
        expect(parsed, same(ClipboardLimits.onPasteRevision));
        expect(
          parsed!.onPaste && parsed.scale && parsed.pull && parsed.formats,
          isTrue,
        );
        expect(parsed.pushTextBytes, 1024 * 1024);
        expect(parsed.pushImageBytes, 8 * 1024 * 1024);
        expect(ClipboardLimits.scaleRevision.onPaste, isFalse);
        for (final change in <String, Object>{
          'onPasteProfile': 2,
          'pushTextBytes': 2 * 1024 * 1024,
          'pushImageBytes': 4 * 1024 * 1024,
          'scaleProfile': 2,
          'extra': 1,
        }.entries) {
          expect(
            ClipboardLimits.parse({
              ...onPasteLimits(),
              change.key: change.value,
            }, chunkBytes: 32768),
            isNull,
            reason: change.key,
          );
        }
        for (final missing in ['pushTextBytes', 'pushImageBytes']) {
          expect(
            ClipboardLimits.parse(
              {...onPasteLimits()}..remove(missing),
              chunkBytes: 32768,
            ),
            isNull,
          );
        }
        expect(
          ClipboardLimits.parse(scale.scaleLimits(), chunkBytes: 32768),
          same(ClipboardLimits.scaleRevision),
        );
      },
    );

    test('only a bridge that says so announces the header; an unannounced revision closes the channel', () async {
      expect(session.dialHeaders, {
        clipboardLimitsHeader: clipboardLimitsHeaderValue,
        clipboardFormatsHeader: clipboardFormatsHeaderValue,
        clipboardPullHeader: clipboardPullHeaderValue,
        clipboardScaleHeader: clipboardScaleHeaderValue,
        clipboardOnPasteHeader: clipboardOnPasteHeaderValue,
      });
      final scaleOnly = make(with_: scale.ScaleClipboard());
      expect(
        scaleOnly.dialHeaders.containsKey(clipboardOnPasteHeader),
        isFalse,
      );
      addTearDown(scaleOnly.close);
      final refused = make(with_: scale.ScaleClipboard());
      final starting = refused.start();
      starting.ignore();
      await base.settle();
      socket.emit(onPasteReady());
      await expectLater(starting, throwsA(isA<ClipboardTransferException>()));
      expect(refused.ready, isFalse);
      await refused.close();
    });

    test(
      'a pair with the revision tells the bridge; a scale pair does not',
      () async {
        await connect();
        expect(bridge.onPasteOn, isTrue);
        expect(session.limits.onPaste, isTrue);
        await session.close();
        socket = base.ClipboardSocket();
        bridge = OnPasteClipboard();
        session = make();
        await connect(scale.scaleReady());
        expect(bridge.onPasteOn, isFalse);
        expect(session.limits.onPaste, isFalse);
      },
    );
  });

  group('a copy on this PC is silent', () {
    test('files are offered, nothing is pushed, nothing is shown', () async {
      await connect();
      bridge.offerCapture = offerOf('tree', [pull.tree('a.txt', 3)], tops: 1);
      await session.setSharingActive(true);
      final sending = session.sendLocal(automatic: true);
      final offer = await socket.nextMessage('clipboard.offer');
      expect(offer['format'], 'tree');
      await socket.nextMessage('clipboard.entries');
      await base.settle();
      expect(sentOf('clipboard.begin'), isEmpty);
      expect(sentOf('clipboard.chunk'), isEmpty);
      expect(
        session.userWaiting,
        isFalse,
        reason: 'a background copy has nobody waiting',
      );
      socket.emit({
        'type': 'clipboard.complete',
        'requestId': offer['requestId'],
      });
      await sending;
      expect(session.error, isNull);
    });

    test('big text, rich text and images are offered by their manifest; the items are pulled by index', () async {
      await connect();
      for (final (format, items, expected)
          in <(String, List<ClipboardItem>, Map<String, Object?>)>[
            (
              'text',
              [
                const ClipboardItem(
                  size: 5000000,
                  kind: 'text',
                  units: 3000000,
                ),
              ],
              {'size': 5000000},
            ),
            (
              'richtext',
              [
                const ClipboardItem(size: 100, kind: 'text'),
                const ClipboardItem(size: 3000000, kind: 'html'),
              ],
              {
                'items': [
                  {'kind': 'text', 'size': 100},
                  {'kind': 'html', 'size': 3000000},
                ],
              },
            ),
            (
              'image',
              [
                const ClipboardItem(
                  size: 20000000,
                  kind: 'png',
                  width: 4000,
                  height: 3000,
                ),
              ],
              {
                'items': [
                  {
                    'kind': 'png',
                    'size': 20000000,
                    'width': 4000,
                    'height': 3000,
                  },
                ],
              },
            ),
          ]) {
        socket.sent.clear();
        bridge.offerCapture = offerOf(format, items);
        final sending = session.sendLocal();
        final offer = await socket.nextMessage('clipboard.offer');
        expect(offer, {
          'type': 'clipboard.offer',
          'requestId': offer['requestId'],
          'format': format,
          ...expected,
        });
        await base.settle();
        expect(
          sentOf('clipboard.entries'),
          isEmpty,
          reason: 'a content offer has no entries pages',
        );
        expect(sentOf('clipboard.chunk'), isEmpty);
        socket.emit({
          'type': 'clipboard.complete',
          'requestId': offer['requestId'],
        });
        await sending;
        expect(session.status, contains('붙여넣'));
        // The peer pastes: it pulls item 0 (and 1 for the HTML).
        bridge.files[offer['requestId'] as String] = [
          for (final item in items)
            Uint8List(item.size > 100000 ? 0 : item.size),
        ];
      }
    });

    test('a new copy withdraws the offers made for the older one, before it is captured, even when it cannot be sent', () async {
      await connect();
      bridge.offersToDrop = ['old-offer-1', 'old-offer-2'];
      bridge.offerCapture = offerOf('tree', [pull.tree('b.txt', 2)], tops: 1);
      final sending = session.sendLocal();
      final offer = await socket.nextMessage('clipboard.offer');
      final withdraws = sentOf('clipboard.offer.withdraw');
      expect(
        [for (final w in withdraws) w['requestId']],
        ['old-offer-1', 'old-offer-2'],
      );
      expect(withdraws.first.keys.toSet(), {'type', 'requestId'});
      // The drop happened before the capture, so a copy that is then refused still left no stale offer behind.
      expect(
        bridge.events.indexOf('dropOffers'),
        lessThan(bridge.events.indexOf('captureWithOffer')),
      );
      await socket.nextMessage('clipboard.entries');
      socket.emit({
        'type': 'clipboard.complete',
        'requestId': offer['requestId'],
      });
      await sending;
      socket.sent.clear();
      bridge.offersToDrop = ['old-offer-3'];
      bridge.offerCapture = null;
      bridge.captureFailure = PlatformException(
        code: 'unsupported_clipboard_format',
      );
      await expectLater(session.sendLocal(), throwsA(anything));
      expect(
        [for (final w in sentOf('clipboard.offer.withdraw')) w['requestId']],
        ['old-offer-3'],
      );
    });

    test(
      'a pair without the revision never drops or withdraws anything',
      () async {
        await connect(scale.scaleReady());
        bridge.offersToDrop = ['x'];
        bridge.offerCapture = offerOf('tree', [pull.tree('b.txt', 2)], tops: 1);
        final sending = session.sendLocal();
        final offer = await socket.nextMessage('clipboard.offer');
        await socket.nextMessage('clipboard.entries');
        socket.emit({
          'type': 'clipboard.complete',
          'requestId': offer['requestId'],
        });
        await sending;
        expect(bridge.drops, 0);
        expect(sentOf('clipboard.offer.withdraw'), isEmpty);
      },
    );
  });

  group('a copy in the VM arrives as an announcement', () {
    Future<String> receiveOffer(
      Map<String, dynamic> header, {
      List<Map<String, dynamic>>? entries,
    }) async {
      final receiving = session.receiveRemote();
      final read = await socket.nextMessage('clipboard.read');
      final id = read['requestId'] as String;
      socket.emit({...header, 'requestId': id});
      await base.settle();
      if (entries != null) {
        socket.emit({
          'type': 'clipboard.entries',
          'requestId': id,
          'start': 0,
          'entries': entries,
        });
      }
      await socket.nextMessage('clipboard.applied');
      socket.emit({'type': 'clipboard.complete', 'requestId': id});
      await receiving;
      return id;
    }

    test('text, rich text and images are published at once as data objects that render on demand', () async {
      await connect();
      final textId = await receiveOffer({
        'type': 'clipboard.offer',
        'format': 'text',
        'size': 5000000,
      });
      expect(bridge.publishedContent.last.$1, textId);
      expect(bridge.publishedContent.last.$2, 'text');
      expect(bridge.publishedContent.last.$3.single.size, 5000000);
      final richId = await receiveOffer({
        'type': 'clipboard.offer',
        'format': 'richtext',
        'items': [
          {'kind': 'text', 'size': 10},
          {'kind': 'html', 'size': 3000000},
        ],
      });
      expect(
        [for (final item in bridge.publishedContent.last.$3) item.kind],
        ['text', 'html'],
      );
      expect(bridge.publishedContent.last.$1, richId);
      await receiveOffer({
        'type': 'clipboard.offer',
        'format': 'image',
        'items': [
          {'kind': 'png', 'size': 9000000, 'width': 640, 'height': 480},
        ],
      });
      final image = bridge.publishedContent.last.$3.single;
      expect((image.kind, image.width, image.height), ('png', 640, 480));
      // Nothing was received and nothing is waiting.
      expect(bridge.events.contains('begin'), isFalse);
      expect(bridge.events.contains('commit'), isFalse);
      expect(session.progress, isNull);
      expect(session.transferring, isFalse);
      expect(session.error, isNull);
    });

    test('a file offer is published with the on-paste options (no Explorer dialog, CF_HDROP for a small set)', () async {
      await connect();
      await receiveOffer(
        {
          'type': 'clipboard.offer',
          'format': 'tree',
          'entries': 1,
          'files': 1,
          'bytes': 5,
          'tops': 1,
          'skipped': 0,
        },
        entries: [
          {
            'path': 'a.txt',
            'kind': 'file',
            'size': 5,
            'attrs': 32,
            'mtime': 1761500000123,
          },
        ],
      );
      expect(bridge.publishedContent.single.$2, 'tree');
      expect(bridge.publishedContent.single.$3.single.name, 'a.txt');
      expect(
        bridge.publishedOffers,
        isEmpty,
        reason: 'the pre-revision publisher was not used',
      );
    });

    test(
      'a newer copy first ends what is blocked waiting for the older offer',
      () async {
        await connect();
        await receiveOffer({
          'type': 'clipboard.offer',
          'format': 'text',
          'size': 2000000,
        });
        expect(bridge.supersedes, 1);
        expect(
          bridge.events.indexOf('pullSupersede'),
          lessThan(bridge.events.indexOf('publishOfferOnPaste')),
        );
        // A pushed copy (small text) does the same before it replaces the clipboard.
        final receiving = session.receiveRemote();
        final read = await socket.nextMessage('clipboard.read');
        socket.emit({
          'type': 'clipboard.begin',
          'requestId': read['requestId'],
          'format': 'text',
          'size': 3,
        });
        await base.settle();
        expect(bridge.supersedes, 2);
        socket.emit({
          'type': 'clipboard.error',
          'requestId': read['requestId'],
          'reason': 'clipboard_cancelled',
        });
        await receiving.catchError((_) {});
      },
    );

    test('text, rich text and image offers are refused without their permission and on pairs without the revision', () async {
      session = make();
      await connect(
        onPasteReady(
          policy: {
            'textToRemote': true,
            'textToLocal': false,
            'filesToRemote': true,
            'filesToLocal': true,
          },
        ),
      );
      final receiving = session.receiveRemote().catchError((_) {});
      final read = await socket.nextMessage('clipboard.read');
      socket.emit({
        'type': 'clipboard.offer',
        'requestId': read['requestId'],
        'format': 'text',
        'size': 2000000,
      });
      await base.settle();
      expect(session.ready, isFalse);
      await receiving;
      socket = base.ClipboardSocket();
      bridge = OnPasteClipboard();
      session = make();
      await connect(scale.scaleReady());
      final other = session.receiveRemote().catchError((_) {});
      final again = await socket.nextMessage('clipboard.read');
      socket.emit({
        'type': 'clipboard.offer',
        'requestId': again['requestId'],
        'format': 'text',
        'size': 2000000,
      });
      await base.settle();
      expect(session.ready, isFalse);
      await other;
    });

    test('malformed content offers close the channel', () async {
      for (final bad in <Map<String, dynamic>>[
        {'type': 'clipboard.offer', 'format': 'text', 'size': 'x'},
        {'type': 'clipboard.offer', 'format': 'text', 'size': 5, 'extra': 1},
        {'type': 'clipboard.offer', 'format': 'richtext', 'items': []},
        {
          'type': 'clipboard.offer',
          'format': 'richtext',
          'items': [
            {'kind': 'html', 'size': 5},
          ],
        },
        {
          'type': 'clipboard.offer',
          'format': 'image',
          'items': [
            {'kind': 'png', 'size': 5},
          ],
        },
        {'type': 'clipboard.offer', 'format': 'files'},
      ]) {
        socket = base.ClipboardSocket();
        bridge = OnPasteClipboard();
        session = make();
        await connect();
        final receiving = session.receiveRemote().catchError((_) {});
        final read = await socket.nextMessage('clipboard.read');
        socket.emit({...bad, 'requestId': read['requestId']});
        await base.settle();
        expect(session.ready, isFalse, reason: '$bad');
        await receiving;
        expect(bridge.publishedContent, isEmpty);
      }
    });

    test(
      'a platform that cannot publish fails the offer without applying it',
      () async {
        await connect();
        bridge.publishContentFailure = PlatformException(
          code: 'clipboard_write_failed',
        );
        final receiving = session.receiveRemote().catchError((_) {});
        final read = await socket.nextMessage('clipboard.read');
        socket.emit({
          'type': 'clipboard.offer',
          'requestId': read['requestId'],
          'format': 'text',
          'size': 2000000,
        });
        final error = await socket.nextMessage('clipboard.error');
        expect(error['requestId'], read['requestId']);
        expect(sentOf('clipboard.applied'), isEmpty);
        await receiving;
      },
    );

    test('this PC\'s clipboard is not claimed for a copy that arrives as an announcement', () async {
      await connect();
      await receiveOffer({
        'type': 'clipboard.offer',
        'format': 'image',
        'items': [
          {'kind': 'png', 'size': 9000000, 'width': 640, 'height': 480},
        ],
      });
      expect(bridge.events.contains('claim'), isFalse);
    });
  });

  group('withdrawing', () {
    test('the peer withdrew an offer: what reads it fails now and the clipboard is cleared if it still holds it', () async {
      await connect();
      final receiving = session.receiveRemote();
      final read = await socket.nextMessage('clipboard.read');
      final id = read['requestId'] as String;
      socket.emit({
        'type': 'clipboard.offer',
        'requestId': id,
        'format': 'text',
        'size': 2000000,
      });
      await socket.nextMessage('clipboard.applied');
      socket.emit({'type': 'clipboard.complete', 'requestId': id});
      await receiving;
      socket.emit({'type': 'clipboard.offer.withdraw', 'requestId': id});
      await base.settle();
      expect(bridge.retired, [id]);
      expect(bridge.withdrawn, [id]);
      expect(
        bridge.events.indexOf('pullRetire'),
        lessThan(bridge.events.indexOf('withdrawOffer')),
      );
      expect(session.ready, isTrue);
      // An offer nobody knows is harmless; so is a repeat.
      socket.emit({'type': 'clipboard.offer.withdraw', 'requestId': 'unknown'});
      socket.emit({'type': 'clipboard.offer.withdraw', 'requestId': id});
      await base.settle();
      expect(session.ready, isTrue);
    });

    test('a withdraw that overtakes the publication retires the offer as soon as it is published', () async {
      await connect();
      bridge.publishGate = Completer<void>();
      final receiving = session.receiveRemote().catchError((_) {});
      final read = await socket.nextMessage('clipboard.read');
      final id = read['requestId'] as String;
      socket.emit({
        'type': 'clipboard.offer',
        'requestId': id,
        'format': 'text',
        'size': 2000000,
      });
      await base.settle();
      socket.emit({'type': 'clipboard.offer.withdraw', 'requestId': id});
      await base.settle();
      expect(
        bridge.withdrawn,
        isEmpty,
        reason: 'not published yet: nothing to clear',
      );
      bridge.publishGate!.complete();
      await socket.nextMessage('clipboard.applied');
      await base.settle();
      expect(bridge.retired, contains(id));
      expect(bridge.withdrawn, [id]);
      socket.emit({'type': 'clipboard.complete', 'requestId': id});
      await receiving;
    });

    test('malformed or unexpected withdraws end the channel; a pair without the revision has none', () async {
      await connect();
      socket.emit({
        'type': 'clipboard.offer.withdraw',
        'requestId': 'x',
        'extra': 1,
      });
      await base.settle();
      expect(session.ready, isFalse);
      socket = base.ClipboardSocket();
      bridge = OnPasteClipboard();
      session = make();
      await connect(scale.scaleReady());
      socket.emit({'type': 'clipboard.offer.withdraw', 'requestId': 'x'});
      await base.settle();
      expect(session.ready, isFalse);
    });
  });

  group('who is waiting, and the card being the only progress', () {
    Future<String> offeredFiles(Uint8List data) async {
      bridge.offerCapture = offerOf('tree', [
        pull.tree('f.bin', data.length),
      ], tops: 1);
      await session.setSharingActive(true);
      final sending = session.sendLocal(automatic: true);
      final offer = await socket.nextMessage('clipboard.offer');
      await socket.nextMessage('clipboard.entries');
      socket.emit({
        'type': 'clipboard.complete',
        'requestId': offer['requestId'],
      });
      await sending;
      bridge.files[offer['requestId'] as String] = [data];
      return offer['requestId'] as String;
    }

    Map<String, dynamic> pullRequest(String id, String offer) => {
      'type': 'clipboard.pull',
      'requestId': id,
      'offerId': offer,
      'index': 0,
      'offset': 0,
    };

    test(
      'a failed source open reports the precise reason and PC-to-VM direction',
      () async {
        final records = <ProductLogEntry>[];
        final recording = ProductLog.instance.localRecords.listen(records.add);
        addTearDown(recording.cancel);
        await connect();
        final id = await offeredFiles(Uint8List.fromList([1, 2, 3]));
        bridge.openFailure = PlatformException(
          code: 'clipboard_selected_file_changed',
        );
        socket.emit({
          'type': 'clipboard.paste.waiting',
          'requestId': 'wait-1',
          'active': true,
        });
        socket.emit(pullRequest('pullh-1', id));
        expect(
          (await socket.nextMessage('clipboard.error'))['reason'],
          'clipboard_selected_file_changed',
        );
        expect(session.error, 'clipboard_selected_file_changed');
        expect(session.ready, isTrue);
        final detail = records.singleWhere(
          (entry) =>
              entry.event == LogEvent.clipboard && entry.values['action'] == 8,
        );
        expect(detail.values['remote'], isTrue);
        expect(detail.values['ok'], isFalse);
      },
    );

    test('cancel between VM file pulls withdraws the source offer and leaves a new copy usable', () async {
      session = make(linger: const Duration(seconds: 1));
      await connect();
      bridge.offerCapture = offerOf('tree', [
        pull.tree('a.bin', 3),
        pull.tree('b.bin', 3),
      ], tops: 2);
      await session.setSharingActive(true);
      final sending = session.sendLocal(automatic: true);
      final offer = await socket.nextMessage('clipboard.offer');
      await socket.nextMessage('clipboard.entries');
      final id = offer['requestId'] as String;
      socket.emit({'type': 'clipboard.complete', 'requestId': id});
      await sending;
      bridge.files[id] = [
        Uint8List.fromList([1, 2, 3]),
        Uint8List.fromList([4, 5, 6]),
      ];
      socket.emit({
        'type': 'clipboard.paste.waiting',
        'requestId': 'wait-1',
        'active': true,
      });
      socket.emit({
        'type': 'clipboard.pull',
        'requestId': 'pullh-1',
        'offerId': id,
        'index': 0,
        'offset': 0,
      });
      await socket.nextMessage('clipboard.item.end');
      await base.settle();
      expect(
        session.transferring,
        isTrue,
        reason: 'the card lingers between files',
      );
      expect(session.userWaiting, isTrue);
      bridge.offersToDrop = [id];
      socket.sent.clear();
      await session.cancelTransfer();
      expect(sentOf('clipboard.offer.withdraw'), [
        {'type': 'clipboard.offer.withdraw', 'requestId': id},
      ]);
      socket.sent.clear();
      socket.emit({
        'type': 'clipboard.pull',
        'requestId': 'pullh-2',
        'offerId': id,
        'index': 1,
        'offset': 0,
      });
      final rejected = await socket.nextMessage('clipboard.error');
      expect(rejected['reason'], 'clipboard_offer_expired');
      expect(sentOf('clipboard.chunk'), isEmpty);
      expect(session.ready, isTrue);
      socket.emit({
        'type': 'clipboard.paste.waiting',
        'requestId': 'wait-1',
        'active': false,
      });
      final next = await offeredFiles(Uint8List.fromList([9, 8, 7]));
      socket.emit(pullRequest('pullh-3', next));
      expect(
        (await socket.nextMessage('clipboard.chunk'))['data'],
        base64Encode([9, 8, 7]),
      );
    });

    test('a VM paste that pulls our offer shows a card only once the Host says it waits', () async {
      await connect();
      final id = await offeredFiles(Uint8List(3 * 1024 * 1024));
      socket.emit(pullRequest('pullh-1', id));
      await Future<void>.delayed(const Duration(milliseconds: 30));
      expect(session.pulling, isTrue);
      expect(session.transferring, isTrue);
      expect(
        session.userWaiting,
        isFalse,
        reason: 'serving alone is not a waiting paste in this revision',
      );
      socket.emit({
        'type': 'clipboard.paste.waiting',
        'requestId': 'wait-1',
        'active': true,
      });
      await base.settle();
      expect(session.userWaiting, isTrue);
      socket.emit({
        'type': 'clipboard.paste.waiting',
        'requestId': 'wait-1',
        'active': false,
      });
      await base.settle();
      expect(session.userWaiting, isFalse);
      expect(session.ready, isTrue);
    });

    test('a pair without the revision shows a served pull as before (the Host reports nothing)', () async {
      await connect(scale.scaleReady());
      final id = await offeredFiles(Uint8List(3 * 1024 * 1024));
      socket.emit(pullRequest('pullh-1', id));
      await Future<void>.delayed(const Duration(milliseconds: 30));
      expect(session.userWaiting, isTrue);
    });

    test('the Host\'s waiting event exists only in this revision', () async {
      await connect(scale.scaleReady());
      socket.emit({
        'type': 'clipboard.paste.waiting',
        'requestId': 'wait-1',
        'active': true,
      });
      await base.settle();
      expect(session.ready, isFalse);
      socket = base.ClipboardSocket();
      bridge = OnPasteClipboard();
      session = make();
      await connect();
      socket.emit({
        'type': 'clipboard.paste.waiting',
        'requestId': 'wait-1',
        'active': 'yes',
      });
      await base.settle();
      expect(session.ready, isFalse);
    });

    test('a paste on this PC that pulls is somebody waiting, and Explorer\'s operation does not hide our card', () async {
      await connect();
      final receiving = session.receiveRemote();
      final read = await socket.nextMessage('clipboard.read');
      final id = read['requestId'] as String;
      socket.emit({
        'type': 'clipboard.offer',
        'requestId': id,
        'format': 'tree',
        'entries': 1,
        'files': 1,
        'bytes': 100000,
        'tops': 1,
        'skipped': 0,
      });
      await base.settle();
      socket.emit({
        'type': 'clipboard.entries',
        'requestId': id,
        'start': 0,
        'entries': [
          {
            'path': 'a.bin',
            'kind': 'file',
            'size': 100000,
            'attrs': 32,
            'mtime': 1761500000123,
          },
        ],
      });
      await socket.nextMessage('clipboard.applied');
      socket.emit({'type': 'clipboard.complete', 'requestId': id});
      await receiving;
      expect(session.userWaiting, isFalse);
      // Explorer-style consumer: StartOperation, then reads. The data object carries no FD_PROGRESSUI, so no dialog shows and our card is the progress.
      bridge.platform(ClipboardPullOperation(true, offerId: id));
      bridge.platform(ClipboardPullRequest(31, id, 0, 0));
      await socket.nextMessage('clipboard.pull');
      expect(session.userWaiting, isTrue);
      expect(session.progress!.asyncConsumer, isFalse);
      expect(session.progress!.fileName, 'a.bin');
    });

    test('the same Explorer-style consumer on a pair without the revision keeps Windows\' own dialog as the progress', () async {
      await connect(scale.scaleReady());
      final receiving = session.receiveRemote();
      final read = await socket.nextMessage('clipboard.read');
      final id = read['requestId'] as String;
      socket.emit({
        'type': 'clipboard.offer',
        'requestId': id,
        'format': 'tree',
        'entries': 1,
        'files': 1,
        'bytes': 100000,
        'tops': 1,
        'skipped': 0,
      });
      await base.settle();
      socket.emit({
        'type': 'clipboard.entries',
        'requestId': id,
        'start': 0,
        'entries': [
          {
            'path': 'a.bin',
            'kind': 'file',
            'size': 100000,
            'attrs': 32,
            'mtime': 1761500000123,
          },
        ],
      });
      await socket.nextMessage('clipboard.applied');
      socket.emit({'type': 'clipboard.complete', 'requestId': id});
      await receiving;
      bridge.platform(ClipboardPullOperation(true, offerId: id));
      bridge.platform(ClipboardPullRequest(31, id, 0, 0));
      await socket.nextMessage('clipboard.pull');
      expect(session.progress!.asyncConsumer, isTrue);
    });

    test('a text paste on this PC is a blocking read: the progress names the format and the card waits', () async {
      await connect();
      final receiving = session.receiveRemote();
      final read = await socket.nextMessage('clipboard.read');
      final id = read['requestId'] as String;
      socket.emit({
        'type': 'clipboard.offer',
        'requestId': id,
        'format': 'text',
        'size': 2000000,
      });
      await socket.nextMessage('clipboard.applied');
      socket.emit({'type': 'clipboard.complete', 'requestId': id});
      await receiving;
      bridge.platform(ClipboardPullRequest(41, id, 0, 0));
      final request = await socket.nextMessage('clipboard.pull');
      expect(request['index'], 0);
      expect(session.userWaiting, isTrue);
      expect(session.progress!.format, 'text');
      expect(session.progress!.total, 2000000);
    });

    test('cancelling the card ends what is blocked in the platform and the serve; the offer stays', () async {
      await connect();
      final id = await offeredFiles(Uint8List(3 * 1024 * 1024));
      socket.emit(pullRequest('pullh-1', id));
      await Future<void>.delayed(const Duration(milliseconds: 30));
      socket.emit({
        'type': 'clipboard.paste.waiting',
        'requestId': 'wait-1',
        'active': true,
      });
      await base.settle();
      socket.sent.clear();
      await session.cancelTransfer();
      expect(bridge.aborts, 1);
      expect(sentOf('clipboard.error').single, {
        'type': 'clipboard.error',
        'requestId': 'pullh-1',
        'reason': 'clipboard_cancelled',
      });
      expect(session.ready, isTrue);
      // The offer is still served: the user can paste again.
      socket.emit(pullRequest('pullh-2', id));
      await base.settle();
      expect(sentOf('clipboard.chunk'), isNotEmpty);
    });
  });
}
