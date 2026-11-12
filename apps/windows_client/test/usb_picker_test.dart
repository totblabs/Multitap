import 'dart:async';
import 'dart:io';

import 'package:flutter/material.dart';
import 'package:flutter/services.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:multitap_client/data/usb_memory.dart';
import 'package:multitap_client/data/usb_picker.dart';
import 'package:multitap_client/data/usb_redirection.dart';
import 'package:multitap_client/ui/app_theme.dart';
import 'package:multitap_client/ui/usb_panel.dart';

import 'usb_redirection_test.dart'
    show FakeBackend, FakeSocket, ready, sessionId, settle, vector;

// The USB picker: what is listed first, what is never offered, the order, the
// badge, live updates, the check before a drive leaves this PC, and what the
// panel shows for each of those. The relay and the PC are the fakes of
// usb_redirection_test.dart.
final base = DateTime(2026, 10, 27, 12);

UsbLocalDevice dev(
  String id,
  String vector_, {
  String? name,
  String subtitle = '',
  UsbKind? kind,
  int? plugged,
  String key = '',
  List<String> chips = const [],
  bool neededByPc = false,
  bool busyElsewhere = false,
  String vidPid = '0781:5567',
}) => UsbLocalDevice(
  id: id,
  name: name ?? id,
  descriptors: vector(vector_),
  speed: 'high',
  subtitle: subtitle,
  kind: kind,
  arrivalMs: plugged ?? 0,
  memoryKey: key,
  chips: chips,
  neededByPc: neededByPc,
  busyElsewhere: busyElsewhere,
  vidPid: vidPid,
);

int ago(Duration age) => base.subtract(age).millisecondsSinceEpoch;

Future<(UsbSession, FakeSocket, FakeBackend)> begin(
  List<UsbLocalDevice> devices, {
  DateTime Function()? clock,
  UsbMemory? memory,
}) async {
  final socket = FakeSocket();
  final backend = FakeBackend()..devices = devices;
  final session = UsbSession(
    url: Uri.parse('wss://vdi.example/api/v1/sessions/$sessionId/usb'),
    token: 'token',
    sessionId: sessionId,
    backend: backend,
    dialer: (url, token) async => socket,
    memory: memory,
    clock: clock ?? () => base,
  );
  await session.start();
  socket.emit(ready);
  socket.emit({'type': 'usb.status', 'state': 'ready'});
  for (var i = 0; i < 200 && (!session.ready || session.listing); i++) {
    await Future<void>.delayed(const Duration(milliseconds: 1));
  }
  await settle();
  return (session, socket, backend);
}

List<String> names(List<UsbPickerRow> rows) => [
  for (final r in rows) r.entry.device.id,
];

void main() {
  group('what is listed, in which section', () {
    test('drives and security tokens are the main list; everything else is under 기타', () async {
      final (session, _, _) = await begin([
        dev('stick', 'mass storage stick', kind: UsbKind.storage),
        dev('token', 'smart card reader', kind: UsbKind.token),
        dev('printer', 'printer', kind: UsbKind.printer),
        dev('phone', 'vendor specific serial chip', kind: UsbKind.phone),
        dev('fido', 'fido key (report-protocol hid)', kind: UsbKind.other),
        dev('serial', 'cdc serial adapter'),
      ]);
      final picker = session.picker();
      expect(names(picker.main), unorderedEquals(['stick', 'token']));
      expect(
        names(picker.other),
        unorderedEquals(['printer', 'phone', 'fido', 'serial']),
      );
      await session.close();
    });

    test('without the runner classification the permissions decide (drive, smart card, printer)', () async {
      final (session, _, _) = await begin([
        dev('stick', 'mass storage stick'),
        dev('token', 'smart card reader'),
        dev('printer', 'printer'),
      ]);
      final picker = session.picker();
      expect(names(picker.main), unorderedEquals(['stick', 'token']));
      expect(names(picker.other), ['printer']);
      expect(
        session.entries.firstWhere((e) => e.device.id == 'stick').kind,
        UsbKind.storage,
      );
      await session.close();
    });

    test('keyboards, mice, hubs, network adapters are never listed and are counted; Bluetooth and audio are dropped silently', () async {
      final (session, _, _) = await begin([
        dev('stick', 'mass storage stick', kind: UsbKind.storage),
        dev('kb', 'boot keyboard'),
        dev('mouse', 'boot mouse'),
        dev('hub', 'hub'),
        dev(
          'nic',
          'cdc serial adapter',
          kind: UsbKind.network,
          neededByPc: true,
        ),
        dev(
          'gaming',
          'fido key (report-protocol hid)',
          kind: UsbKind.input,
          neededByPc: true,
        ),
        dev('bt', 'bluetooth adapter'),
        dev('headset', 'usb headset (audio, isochronous)'),
      ]);
      expect(names(session.picker().all), ['stick']);
      expect(
        session.hiddenNeeded,
        5,
        reason: 'keyboard, mouse, hub, adapter, gaming keyboard',
      );
      expect(session.picker().hiddenNeeded, 5);
      await session.close();
    });
  });

  group('order, badge and memory', () {
    test('newest first; ties by name; storage and tokens sort inside their own list', () async {
      final (session, _, _) = await begin([
        dev(
          'b',
          'mass storage stick',
          name: 'Bravo',
          plugged: ago(const Duration(hours: 3)),
        ),
        dev(
          'a',
          'mass storage stick',
          name: 'Alpha',
          plugged: ago(const Duration(hours: 3)),
        ),
        dev(
          'c',
          'mass storage stick',
          name: 'Charlie',
          plugged: ago(const Duration(hours: 1)),
        ),
        dev(
          't',
          'smart card reader',
          name: 'Token',
          plugged: ago(const Duration(hours: 2)),
        ),
      ]);
      expect(names(session.picker().main), ['c', 't', 'a', 'b']);
      await session.close();
    });

    test('a device plugged in within 2 minutes ranks first and carries the badge; it runs out', () async {
      var now = base;
      final (session, _, _) = await begin(
        [
          dev(
            'old',
            'mass storage stick',
            name: 'Old',
            plugged: ago(const Duration(days: 1)),
            key: 'old',
          ),
          dev(
            'fresh',
            'smart card reader',
            name: 'Fresh',
            plugged: ago(const Duration(seconds: 30)),
            key: 'fresh',
          ),
        ],
        clock: () => now,
        memory: SessionUsbMemory(['old']),
      );
      var picker = session.picker();
      expect(names(picker.main), ['fresh', 'old']);
      expect(picker.main.first.justPlugged, isTrue);
      expect(picker.main.last.justPlugged, isFalse);
      expect(picker.badgeEnds, const Duration(seconds: 90));
      now = base.add(const Duration(seconds: 60));
      picker = session.picker();
      expect(
        picker.main.first.justPlugged,
        isTrue,
        reason: 'still within 2 minutes',
      );
      expect(picker.badgeEnds, const Duration(seconds: 30));
      now = base.add(const Duration(seconds: 91));
      picker = session.picker();
      expect(picker.main.any((r) => r.justPlugged), isFalse);
      expect(picker.badgeEnds, isNull);
      await session.close();
    });

    test('devices given to a VM before sort right after the new ones and before the rest', () async {
      final (session, _, _) = await begin([
        dev(
          'newer',
          'mass storage stick',
          plugged: ago(const Duration(hours: 1)),
          key: 'newer',
        ),
        dev(
          'cert',
          'mass storage stick',
          plugged: ago(const Duration(days: 30)),
          key: 'cert',
        ),
        dev(
          'fresh',
          'mass storage stick',
          plugged: ago(const Duration(seconds: 10)),
          key: 'fresh',
        ),
        dev(
          'nokey',
          'mass storage stick',
          plugged: ago(const Duration(hours: 2)),
        ),
      ], memory: SessionUsbMemory(['cert']));
      expect(names(session.picker().main), ['fresh', 'cert', 'newer', 'nokey']);
      await session.close();
    });

    test('a device given to the VM is remembered (as a hash) and the memory survives the session', () async {
      final memory = SessionUsbMemory();
      final (session, socket, backend) = await begin([
        dev(
          'cert',
          'smart card reader',
          key: '1050:0407:abc',
          plugged: ago(const Duration(days: 2)),
        ),
      ], memory: memory);
      expect(session.share(session.entries.single), isNull);
      final attach = await socket.text('usb.attach');
      socket.emit({'type': 'usb.attached', 'device': attach['device']});
      for (var i = 0; i < 200 && backend.captures.isEmpty; i++) {
        await Future<void>.delayed(const Duration(milliseconds: 1));
      }
      await settle();
      final saved = await memory.load();
      expect(saved, {usbMemoryHash('1050:0407:abc')});
      expect(
        saved.single,
        isNot(contains('1050')),
        reason: 'only a hash is kept',
      );
      expect(session.remembered, saved);
      await session.close();
    });

    test('a device that arrives again because this session gave it back is not "just plugged in"', () async {
      final (session, socket, backend) = await begin([
        dev(
          'cert',
          'smart card reader',
          key: 'k',
          plugged: ago(const Duration(seconds: 20)),
        ),
      ]);
      expect(session.picker().main.single.justPlugged, isTrue);
      session.share(session.entries.single);
      final attach = await socket.text('usb.attach');
      socket.emit({'type': 'usb.attached', 'device': attach['device']});
      for (var i = 0; i < 200 && backend.captures.isEmpty; i++) {
        await Future<void>.delayed(const Duration(milliseconds: 1));
      }
      await settle();
      await session.stop(session.entries.single);
      expect(session.picker().main.single.justPlugged, isFalse);
      await session.close();
    });

    test(
      'the memory file keeps hashes only, bounded, and ignores garbage',
      () async {
        final directory = Directory.systemTemp.createTempSync('usb-memory');
        addTearDown(() => directory.deleteSync(recursive: true));
        final file = File('${directory.path}/usb-memory.json');
        final memory = FileUsbMemory(file);
        expect(await memory.load(), isEmpty);
        await memory.remember('0781:5567:abc');
        expect(await memory.load(), {usbMemoryHash('0781:5567:abc')});
        expect(file.readAsStringSync(), isNot(contains('0781')));
        for (var i = 0; i < FileUsbMemory.maxKeys + 10; i++) {
          await memory.remember('k$i');
        }
        final all = await memory.load();
        expect(all.length, FileUsbMemory.maxKeys);
        expect(all, contains(usbMemoryHash('k${FileUsbMemory.maxKeys + 9}')));
        expect(
          all,
          isNot(contains(usbMemoryHash('0781:5567:abc'))),
          reason: 'the oldest go first',
        );
        file.writeAsStringSync(
          '{"devices": ["not a hash", 7, "0123456789abcdef01234567"]}',
        );
        expect(await memory.load(), {'0123456789abcdef01234567'});
        file.writeAsStringSync('garbage');
        expect(await memory.load(), isEmpty);
      },
    );
  });

  group('live updates', () {
    test('plugging in and unplugging updates the list without a refresh, and a burst is one listing', () async {
      final (session, _, backend) = await begin([
        dev('a', 'mass storage stick', name: 'A'),
      ]);
      expect(backend.watching, 1, reason: 'watching starts with the channel');
      var lists = 0;
      session.addListener(() => lists++);
      backend.devices = [
        dev('a', 'mass storage stick', name: 'A'),
        dev('b', 'smart card reader', name: 'B'),
      ];
      for (var i = 0; i < 5; i++) {
        backend.changeController.add(null);
      }
      await Future<void>.delayed(const Duration(milliseconds: 700));
      expect(names(session.picker().main), unorderedEquals(['a', 'b']));
      expect(
        lists,
        lessThan(6),
        reason: 'five notifications are not five listings',
      );
      backend.devices = [dev('b', 'smart card reader', name: 'B')];
      backend.changeController.add(null);
      await Future<void>.delayed(const Duration(milliseconds: 700));
      expect(names(session.picker().main), ['b']);
      await session.close();
      expect(backend.watching, 0, reason: 'watching stops with the channel');
    });

    test('the entry (and so the focus and selection) of a device that stays is the same object', () async {
      final (session, _, backend) = await begin([
        dev('a', 'mass storage stick'),
      ]);
      final before = session.entries.single;
      backend.devices = [
        dev('a', 'mass storage stick', name: 'Renamed'),
        dev('b', 'printer'),
      ];
      backend.changeController.add(null);
      await Future<void>.delayed(const Duration(milliseconds: 700));
      expect(
        session.entries.firstWhere((e) => e.device.id == 'a'),
        same(before),
      );
      expect(before.device.name, 'Renamed');
      await session.close();
    });

    test('a change during a listing is listed once more afterwards', () async {
      final (session, _, backend) = await begin([
        dev('a', 'mass storage stick'),
      ]);
      backend.devices = [dev('a', 'mass storage stick'), dev('b', 'printer')];
      final first = session.refresh();
      final second = session.refresh(); // while the first is running
      await Future.wait([first, second]);
      expect(session.entries.length, 2);
      await session.close();
    });
  });

  group('a drive is checked before it leaves this PC', () {
    test('open files stop the hand-over: no attach, the reason in words, the program named', () async {
      final (session, socket, backend) = await begin([
        dev('stick', 'mass storage stick'),
      ]);
      backend.prepareResult = const UsbPrepareResult(
        ready: false,
        blocker: 'Notepad.exe',
      );
      expect(session.share(session.entries.single), isNull);
      await settle();
      final entry = session.entries.single;
      expect(backend.prepared, ['stick']);
      expect(socket.texts.where((m) => m['type'] == 'usb.attach'), isEmpty);
      expect(entry.state, UsbEntryState.failed);
      expect(entry.code, usbPcInUse);
      expect(entry.blocker, 'Notepad.exe');
      expect(
        usbMessage(usbPcInUse),
        'PC에서 사용 중이라 연결할 수 없어요. 열린 파일이나 창을 닫고 다시 시도하세요.',
      );
      // After closing the files the same button works.
      backend.prepareResult = const UsbPrepareResult();
      expect(session.share(entry), isNull);
      await settle();
      expect(
        socket.texts.where((m) => m['type'] == 'usb.attach'),
        hasLength(1),
      );
      await session.close();
    });

    test(
      'the device is not offered while the check runs, then it is',
      () async {
        final (session, socket, backend) = await begin([
          dev('stick', 'mass storage stick'),
        ]);
        backend.prepareGate = Completer();
        session.share(session.entries.single);
        await settle();
        expect(session.entries.single.state, UsbEntryState.connecting);
        expect(socket.texts.where((m) => m['type'] == 'usb.attach'), isEmpty);
        expect(
          session.share(session.entries.single),
          isNull,
          reason: 'a second press does nothing',
        );
        expect(backend.prepared, hasLength(1));
        backend.prepareGate!.complete();
        await settle();
        expect(
          socket.texts.where((m) => m['type'] == 'usb.attach'),
          hasLength(1),
        );
        await session.close();
      },
    );

    test('a drive that cannot be checked is still offered; tokens and others are not checked at all', () async {
      final (session, socket, backend) = await begin([
        dev('stick', 'mass storage stick'),
        dev('token', 'smart card reader'),
        dev('printer', 'printer'),
      ]);
      backend.prepareResult = const UsbPrepareResult(checked: false);
      for (final entry in session.entries) {
        session.share(entry);
      }
      await settle();
      expect(
        socket.texts.where((m) => m['type'] == 'usb.attach'),
        hasLength(3),
      );
      expect(backend.prepared, [
        'stick',
      ], reason: 'only a drive is asked about open files');
      await session.close();
    });

    test('the session ending during the check offers nothing', () async {
      final (session, socket, backend) = await begin([
        dev('stick', 'mass storage stick'),
      ]);
      backend.prepareGate = Completer();
      session.share(session.entries.single);
      await session.close();
      backend.prepareGate!.complete();
      await settle();
      expect(socket.texts.where((m) => m['type'] == 'usb.attach'), isEmpty);
      expect(session.entries.single.state, isNot(UsbEntryState.connecting));
    });

    test('drives being checked count against the limit of four', () async {
      final (session, _, backend) = await begin([
        for (var i = 0; i < 5; i++) dev('d$i', 'mass storage stick'),
      ]);
      backend.prepareGate = Completer();
      for (var i = 0; i < 4; i++) {
        expect(session.share(session.entries[i]), isNull);
      }
      expect(session.share(session.entries[4]), 'usb_limit_reached');
      backend.prepareGate!.complete();
      await session.close();
    });

    test(
      'a device another session holds is refused with its own words',
      () async {
        final (session, socket, _) = await begin([
          dev('held', 'smart card reader', busyElsewhere: true),
        ]);
        expect(session.share(session.entries.single), usbBusyElsewhere);
        expect(usbMessage(usbBusyElsewhere), contains('다른 곳'));
        expect(socket.texts.where((m) => m['type'] == 'usb.attach'), isEmpty);
        await session.close();
      },
    );
  });

  group('the panel', () {
    Widget host(
      UsbSession usb, {
      Brightness brightness = Brightness.dark,
      double width = 420,
      double scale = 1,
    }) => MaterialApp(
      theme: buildAppTheme(brightness: brightness),
      home: MediaQuery(
        data: MediaQueryData(textScaler: TextScaler.linear(scale)),
        child: Scaffold(
          body: Align(
            alignment: Alignment.topCenter,
            child: SizedBox(
              width: width,
              height: 1500,
              child: UsbPanel(usb: usb),
            ),
          ),
        ),
      ),
    );

    final devices = [
      dev(
        'stick',
        'mass storage stick',
        name: '내 USB (E:)',
        subtitle: 'SanDisk Cruzer Blade · 32GB · exFAT',
        kind: UsbKind.storage,
        chips: ['공동인증서'],
        plugged: ago(const Duration(hours: 5)),
        key: 'stick',
        vidPid: '0781:5567',
      ),
      dev(
        'token',
        'smart card reader',
        name: 'YubiKey FIDO+CCID',
        subtitle: 'Yubico',
        kind: UsbKind.token,
        plugged: ago(const Duration(hours: 6)),
        key: 'token',
        vidPid: '1050:0407',
      ),
      dev(
        'printer',
        'printer',
        name: 'LaserJet 1020',
        subtitle: 'HP',
        kind: UsbKind.printer,
        plugged: ago(const Duration(hours: 7)),
        vidPid: '03F0:0001',
      ),
    ];

    Future<UsbSession> open(
      WidgetTester tester,
      Widget Function(UsbSession) build, {
      List<UsbLocalDevice>? list,
      UsbMemory? memory,
    }) async {
      tester.view.physicalSize = const Size(1000, 1600);
      tester.view.devicePixelRatio = 1;
      addTearDown(tester.view.reset);
      late UsbSession usb;
      await tester.runAsync(
        () async => usb = (await begin(list ?? devices, memory: memory)).$1,
      );
      await tester.pumpWidget(build(usb));
      await tester.pump();
      return usb;
    }

    testWidgets(
      'drives and tokens are listed with real names; the rest is folded under 기타 USB 장치 (N)',
      (tester) async {
        final usb = await open(tester, host);
        expect(find.text('내 USB (E:)'), findsOneWidget);
        expect(
          find.text('SanDisk Cruzer Blade · 32GB · exFAT'),
          findsOneWidget,
        );
        expect(find.text('공동인증서'), findsNothing);
        expect(find.text('YubiKey FIDO+CCID'), findsOneWidget);
        expect(find.text('보안 토큰·스마트 카드'), findsOneWidget);
        expect(
          find.text('0781:5567'),
          findsOneWidget,
          reason: 'VID:PID is small secondary text',
        );
        expect(find.text('기타 USB 장치 (1)'), findsOneWidget);
        expect(find.text('LaserJet 1020'), findsNothing, reason: 'folded');
        expect(find.byKey(const ValueKey('usb-share-printer')), findsNothing);
        await tester.tap(find.byKey(const Key('usb-other-toggle')));
        await tester.pump();
        expect(find.text('LaserJet 1020'), findsOneWidget);
        expect(find.byKey(const ValueKey('usb-share-printer')), findsOneWidget);
        await tester.tap(find.byKey(const Key('usb-other-toggle')));
        await tester.pump();
        expect(find.text('LaserJet 1020'), findsNothing);
        await tester.runAsync(usb.close);
      },
    );

    testWidgets(
      'the one-line note about devices this PC needs shows in 기타, only when some were left out',
      (tester) async {
        final usb = await open(
          tester,
          host,
          list: [...devices, dev('kb', 'boot keyboard'), dev('hub', 'hub')],
        );
        expect(
          find.byKey(const Key('usb-hidden-note')),
          findsNothing,
          reason: 'folded',
        );
        await tester.tap(find.byKey(const Key('usb-other-toggle')));
        await tester.pump();
        expect(find.text('PC를 쓰는 데 필요한 장치는 연결할 수 없어요 · 2개 숨김'), findsOneWidget);
        await tester.runAsync(usb.close);

        final plain = await open(tester, host);
        await tester.tap(find.byKey(const Key('usb-other-toggle')));
        await tester.pump();
        expect(find.byKey(const Key('usb-hidden-note')), findsNothing);
        await tester.runAsync(plain.close);
      },
    );

    testWidgets(
      'only keyboards and hubs: 기타 still opens to say why they are missing',
      (tester) async {
        final usb = await open(
          tester,
          host,
          list: [dev('kb', 'boot keyboard')],
        );
        expect(find.byKey(const Key('usb-empty-main')), findsOneWidget);
        expect(find.text('기타 USB 장치 (0)'), findsOneWidget);
        await tester.tap(find.byKey(const Key('usb-other-toggle')));
        await tester.pump();
        expect(find.byKey(const Key('usb-hidden-note')), findsOneWidget);
        await tester.runAsync(usb.close);
      },
    );

    // "연결할 USB 장치가 없어요." must not stand right above "기타 USB 장치
    // (1)", nor a connected printer hide in the closed fold.
    testWidgets(
      '기타 opens by itself while it holds the only devices or a connected one, and can still be folded',
      (tester) async {
        final printer = devices.last;
        final only = await open(tester, host, list: [printer]);
        expect(find.byKey(const Key('usb-empty-main')), findsNothing);
        expect(find.text('연결할 USB 장치가 없어요.'), findsNothing);
        expect(find.text('기타 USB 장치 (1)'), findsOneWidget);
        expect(find.text('LaserJet 1020'), findsOneWidget, reason: 'open');
        await tester.tap(find.byKey(const Key('usb-other-toggle')));
        await tester.pump();
        expect(find.text('LaserJet 1020'), findsNothing, reason: 'folded');
        await tester.runAsync(only.close);

        // A panel opened anew (the fold above was this person's choice).
        await tester.pumpWidget(const SizedBox.shrink());
        final usb = await open(tester, host);
        expect(find.text('LaserJet 1020'), findsNothing, reason: 'folded');
        // The desktop has the printer now.
        usb.entries.firstWhere((e) => e.device.id == 'printer').state =
            UsbEntryState.shared;
        await tester.runAsync(usb.refresh);
        await tester.pump();
        expect(find.text('LaserJet 1020'), findsOneWidget);
        expect(find.byKey(const ValueKey('usb-stop-printer')), findsOneWidget);
        await tester.runAsync(usb.close);
      },
    );

    testWidgets('방금 연결됨 badge, and it goes away on its own', (tester) async {
      final fresh = [
        dev(
          'stick',
          'mass storage stick',
          name: '새 USB (F:)',
          kind: UsbKind.storage,
          plugged: base.millisecondsSinceEpoch - 20000,
          key: 'x',
        ),
        ...devices.skip(1),
      ];
      late UsbSession usb;
      var now = base;
      await tester.runAsync(() async {
        final result = await begin(fresh, clock: () => now);
        usb = result.$1;
      });
      await tester.pumpWidget(host(usb));
      await tester.pump();
      expect(find.byKey(const ValueKey('usb-new-stick')), findsOneWidget);
      expect(find.text('방금 꽂음'), findsOneWidget);
      // First in the list.
      expect(
        tester.getTopLeft(find.text('새 USB (F:)')).dy,
        lessThan(tester.getTopLeft(find.text('YubiKey FIDO+CCID')).dy),
      );
      now = base.add(const Duration(minutes: 3));
      await tester.pump(const Duration(minutes: 2));
      await tester.pump();
      expect(find.text('방금 꽂음'), findsNothing);
      await tester.runAsync(usb.close);
    });

    testWidgets('a new device in 기타 shows the badge on the folded header', (
      tester,
    ) async {
      final list = [
        ...devices,
        dev(
          'phone',
          'vendor specific serial chip',
          name: 'Galaxy',
          kind: UsbKind.phone,
          plugged: base.millisecondsSinceEpoch - 5000,
        ),
      ];
      final usb = await open(tester, host, list: list);
      expect(find.text('방금 꽂음'), findsOneWidget);
      expect(find.byKey(const Key('usb-other-toggle')), findsOneWidget);
      await tester.tap(find.byKey(const Key('usb-other-toggle')));
      await tester.pump();
      expect(
        find.text('방금 꽂음'),
        findsOneWidget,
        reason: 'now on the device, not on the header',
      );
      expect(find.byKey(const ValueKey('usb-new-phone')), findsOneWidget);
      await tester.runAsync(usb.close);
    });

    testWidgets('each state says what it is', (tester) async {
      late UsbSession usb;
      late FakeSocket socket;
      await tester.runAsync(() async {
        final result = await begin([
          ...devices,
          dev(
            'held',
            'smart card reader',
            name: 'Other',
            kind: UsbKind.token,
            busyElsewhere: true,
            plugged: ago(const Duration(days: 1)),
          ),
        ]);
        usb = result.$1;
        socket = result.$2;
      });
      await tester.pumpWidget(host(usb));
      await tester.pump();
      expect(find.text('사용 가능'), findsNWidgets(2));
      expect(find.text('다른 곳에서 쓰는 중이에요.'), findsOneWidget);
      expect(
        tester
            .widget<FilledButton>(find.byKey(const ValueKey('usb-share-held')))
            .onPressed,
        isNull,
      );
      await tester.tap(find.byKey(const ValueKey('usb-share-token')));
      await tester.pump();
      await tester.runAsync(
        () => Future<void>.delayed(const Duration(milliseconds: 30)),
      );
      await tester.pump();
      expect(find.text('데스크톱에 연결하는 중…'), findsOneWidget);
      await tester.runAsync(() async {
        socket.emit({'type': 'usb.attached', 'device': 1});
        await Future<void>.delayed(const Duration(milliseconds: 60));
      });
      await tester.pump();
      expect(find.textContaining('데스크톱에 연결됨'), findsOneWidget);
      await tester.runAsync(usb.close);
    });

    testWidgets(
      'a missing driver says installation is required and offers it',
      (tester) async {
        late UsbSession usb;
        await tester.runAsync(() async {
          final result = await begin(devices);
          usb = result.$1;
          (await Future.value(result.$3)).driver = UsbDriverState.missing;
          await usb.refresh();
        });
        await tester.pumpWidget(host(usb));
        await tester.pump();
        expect(find.textContaining('USB 드라이버 설치가 필요해요.'), findsOneWidget);
        expect(find.byKey(const Key('usb-install')), findsOneWidget);
        await tester.runAsync(usb.close);
      },
    );

    testWidgets(
      'open files: the panel says so in words and names the program',
      (tester) async {
        late UsbSession usb;
        late FakeBackend backend;
        await tester.runAsync(() async {
          final result = await begin(devices);
          usb = result.$1;
          backend = result.$3;
        });
        backend.prepareResult = const UsbPrepareResult(
          ready: false,
          blocker: 'Excel',
        );
        await tester.pumpWidget(host(usb));
        await tester.pump();
        await tester.tap(find.byKey(const ValueKey('usb-share-stick')));
        await tester.pump();
        await tester.runAsync(
          () => Future<void>.delayed(const Duration(milliseconds: 30)),
        );
        await tester.pump();
        expect(
          find.textContaining('PC에서 사용 중이라 연결할 수 없어요. 열린 파일이나 창을 닫고 다시 시도하세요.'),
          findsOneWidget,
        );
        expect(find.textContaining('사용 중인 프로그램: Excel'), findsOneWidget);
        expect(
          tester
              .widget<FilledButton>(
                find.byKey(const ValueKey('usb-share-stick')),
              )
              .onPressed,
          isNotNull,
          reason: 'press again after closing the files',
        );
        await tester.runAsync(usb.close);
      },
    );

    testWidgets('a live update keeps the focus and the opened 기타', (
      tester,
    ) async {
      tester.view.physicalSize = const Size(1000, 1600);
      tester.view.devicePixelRatio = 1;
      addTearDown(tester.view.reset);
      late UsbSession usb;
      late FakeBackend backend;
      await tester.runAsync(() async {
        final result = await begin(devices);
        usb = result.$1;
        backend = result.$3;
      });
      await tester.pumpWidget(host(usb));
      await tester.pump();
      await tester.tap(find.byKey(const Key('usb-other-toggle')));
      await tester.pump();
      bool inRow(FocusNode? node, String id) {
        var found = false;
        node?.context?.visitAncestorElements((element) {
          if (element.widget.key == ValueKey('usb-device-$id')) found = true;
          return !found;
        });
        return found;
      }

      // Tab to a device's button (the second row's) and remember who has the focus.
      const owner = 'token';
      for (
        var i = 0;
        i < 12 && !inRow(FocusManager.instance.primaryFocus, owner);
        i++
      ) {
        await tester.sendKeyEvent(LogicalKeyboardKey.tab);
        await tester.pump();
      }
      final focused = FocusManager.instance.primaryFocus!;
      expect(
        inRow(focused, owner),
        isTrue,
        reason: 'the focus reached the token row',
      );
      // A new device is plugged in and the list is refreshed.
      backend.devices = [
        ...devices,
        dev(
          'new',
          'smart card reader',
          name: '새 토큰',
          kind: UsbKind.token,
          plugged: base.millisecondsSinceEpoch - 1000,
        ),
      ];
      backend.changeController.add(null);
      await tester.runAsync(
        () => Future<void>.delayed(const Duration(milliseconds: 700)),
      );
      await tester.pump();
      expect(
        find.text('새 토큰'),
        findsOneWidget,
        reason: 'the list followed the plug-in',
      );
      expect(FocusManager.instance.primaryFocus, same(focused));
      expect(inRow(FocusManager.instance.primaryFocus!, owner), isTrue);
      expect(
        find.text('LaserJet 1020'),
        findsOneWidget,
        reason: '기타 stayed open',
      );
      await tester.runAsync(usb.close);
    });

    for (final brightness in Brightness.values) {
      for (final (width, scale) in [
        (420.0, 1.0),
        (300.0, 1.0),
        (300.0, 2.0),
        (420.0, 2.0),
      ]) {
        testWidgets(
          '${brightness.name}, width ${width.toInt()}, text ${scale}x: nothing overflows, every action is reachable',
          (tester) async {
            tester.view.physicalSize = Size(width * 2, 1600);
            tester.view.devicePixelRatio = 2;
            addTearDown(tester.view.reset);
            final usb = await open(
              tester,
              (u) =>
                  host(u, brightness: brightness, width: width, scale: scale),
            );
            await tester.tap(find.byKey(const Key('usb-other-toggle')));
            await tester.pump();
            for (final id in ['stick', 'token', 'printer']) {
              final share = find.byKey(ValueKey('usb-share-$id'));
              await tester.ensureVisible(share);
              expect(share, findsOneWidget);
              expect(tester.getSize(share).width, greaterThan(40));
              final panel = tester.getRect(find.byType(UsbPanel));
              expect(panel.width, width);
              expect(
                tester.getRect(share).right,
                lessThanOrEqualTo(panel.right + 0.5),
              );
              expect(
                tester.getRect(share).left,
                greaterThanOrEqualTo(panel.left - 0.5),
              );
            }
            expect(tester.takeException(), isNull);
            await tester.runAsync(usb.close);
          },
        );
      }
    }
  });
}
