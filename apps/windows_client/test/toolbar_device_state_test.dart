// Actual RemoteView/RemoteSession notification ownership; device state is a
// memory fake. No native device, file, clipboard, capture or window operation.
//
// 2026-11-10 (session bar v2): shared folders and USB devices are one bar
// button, 폴더·USB (key remote-share), lit with a green dot while anything
// is connected; each side is a tab of its panel (keys remote-drives and
// remote-usb) with its own count.
import 'dart:convert';

import 'package:flutter/material.dart';
import 'package:flutter/services.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:multitap_client/data/broker_api.dart';
import 'package:multitap_client/data/drive_session.dart';
import 'package:multitap_client/data/remote_session.dart';
import 'package:multitap_client/data/usb_redirection.dart';
import 'package:multitap_client/ui/app_theme.dart';
import 'package:multitap_client/ui/remote_view.dart';
import 'package:multitap_client/ui/session_menu.dart';

import 'drive_session_test.dart' show FakeFiles;
import 'remote_session_test.dart'
    show SessionSocket, SessionMedia, info, packet;
import 'stream_display_entry_test.dart' show entryMachine;
import 'usb_redirection_test.dart' show FakeBackend;

const _id = '0b7d6f5c-3f9e-4f69-9d8c-2c1d6b8f0a11';
final _url = Uri.parse('wss://fixture.invalid/api/v1/sessions/$_id');

/// The bar's one button for both sides, and the tabs inside its panel.
const _share = Key('remote-share');
const _drives = Key('remote-drives'), _usb = Key('remote-usb');
const _badge = Key('session-command-badge');
const _green = Color(0xFF62E3A9);
const _idle = '폴더·USB · PC의 폴더와 USB 장치를 데스크톱에서 쓰기';

class _Drive extends DriveSession {
  _Drive()
    : super(url: _url, token: 'fixture', sessionId: _id, files: FakeFiles());
  final rows = <DriveShare>[];
  @override
  List<DriveShare> get shares => List.unmodifiable(rows);
  @override
  Future<void> start() async {
    ready = true;
    notifyListeners();
  }

  void changed() => notifyListeners();
  @override
  Future<void> close({String reason = 'drive_session_closed'}) async {
    closed = true;
    ready = false;
    notifyListeners();
  }
}

class _Usb extends UsbSession {
  _Usb()
    : super(
        url: _url,
        token: 'fixture',
        sessionId: _id,
        backend: FakeBackend(),
      );
  bool up = true, ended = false;
  @override
  bool get ready => up && !ended;
  @override
  bool get closed => ended;
  @override
  Future<void> start() async => notifyListeners();

  /// The USB tab's panel lists this PC's devices as it opens; these entries
  /// stay the test's own.
  @override
  Future<void> refresh() async {}
  void changed() => notifyListeners();
  @override
  Future<void> close({String reason = 'usb_session_closed'}) async {
    ended = true;
    notifyListeners();
  }
}

class _View {
  _View({this.allowed = true, bool? usbAllowed, this.usbRequested = false})
    : usbAllowed = usbAllowed ?? allowed;

  /// The assignment grants shared folders (and, unless [usbAllowed] says
  /// otherwise, USB).
  final bool allowed, usbAllowed;

  /// The connect asked for usb-v1 (the Host offers it); without its socket
  /// the assignment does not allow it.
  final bool usbRequested;
  final drive = _Drive(), usb = _Usb();
  final socket = SessionSocket(), media = SessionMedia();
  late final session = RemoteSession(
    connection: BrokerConnection(
      sessionId: _id,
      webSocketUrl: _url,
      driveWebSocketUrl: allowed ? _url : null,
      usbWebSocketUrl: usbAllowed ? _url : null,
      usbRequested: usbRequested,
    ),
    token: 'fixture-only',
    media: media,
    dialer: (_, _) async => socket,
    driveFactory: (_, _) => drive,
    usbFactory: (_, _) => usb,
  );
  Future<void> mount(
    WidgetTester tester, {
    Brightness brightness = Brightness.dark,
  }) async {
    await tester.pumpWidget(
      MaterialApp(
        theme: buildAppTheme(brightness: brightness),
        home: RemoteView(
          key: ObjectKey(this),
          machine: entryMachine(1),
          load: () async => session,
          sessionGuideEnabled: false,
        ),
      ),
    );
    await tester.pump();
    await tester.pump();
    socket.incoming.add(jsonEncode(info));
    await tester.pump();
    await tester.pump();
    socket.incoming.add(packet());
    await tester.pump();
    await tester.pump();
    expect(session.frameReady, isTrue);
    await tester.tap(find.byKey(const Key('expand-session-toolbar')));
    await tester.pump();
  }

  Future<void> close(WidgetTester tester) async {
    await tester.pumpWidget(const SizedBox.shrink());
    await tester.runAsync(() async {
      await session.close();
      await media.events.close();
    });
    await tester.pump(const Duration(seconds: 3));
  }

  DriveShare folder(String id, DriveShareState state) {
    final share = DriveShare(id: id, path: r'C:\Fixture', label: 'Fixture')
      ..state = state;
    drive.rows.add(share);
    return share;
  }

  UsbEntry device(String id, UsbEntryState state) {
    final entry = UsbEntry(
      UsbLocalDevice(
        id: id,
        name: 'Fixture',
        descriptors: Uint8List(0),
        speed: 'high',
      ),
      const UsbDecision.allowed({}, {}),
    )..state = state;
    usb.entries = [...usb.entries, entry];
    return entry;
  }
}

SessionToolbarMenu command(WidgetTester tester, Key key) =>
    tester.widget<SessionToolbarMenu>(find.byKey(key));

/// A popup route opened or closed: its short transition (the panel follows
/// live sessions, so fixed frames rather than settling).
Future<void> _popup(WidgetTester tester) async {
  await tester.pump();
  await tester.pump(const Duration(milliseconds: 300));
}

/// The 폴더·USB panel under its bar button, and closed again (Escape).
Future<void> _openShare(WidgetTester tester) async {
  await tester.tap(find.byKey(_share));
  await _popup(tester);
  expect(find.byKey(const Key('session-share-panel')), findsOneWidget);
}

Future<void> _closeShare(WidgetTester tester) async {
  await tester.sendKeyEvent(LogicalKeyboardKey.escape);
  await _popup(tester);
  expect(find.byKey(const Key('session-share-panel')), findsNothing);
}

/// The number in a tab's count pill; null when the tab has none.
String? _count(WidgetTester tester, String side) {
  final pill = find.byKey(Key('remote-$side-count'));
  if (pill.evaluate().isEmpty) return null;
  return tester
      .widget<Text>(find.descendant(of: pill, matching: find.byType(Text)))
      .data;
}

void main() {
  TestWidgetsFlutterBinding.ensureInitialized();
  setUp(() {
    final messenger =
        TestDefaultBinaryMessengerBinding.instance.defaultBinaryMessenger;
    messenger.setMockMethodCallHandler(
      const MethodChannel('multitap/window'),
      (_) async => false,
    );
    messenger.setMockMethodCallHandler(
      const MethodChannel('multitap/window/events'),
      (_) async => null,
    );
  });
  tearDown(() {
    final messenger =
        TestDefaultBinaryMessengerBinding.instance.defaultBinaryMessenger;
    messenger.setMockMethodCallHandler(
      const MethodChannel('multitap/window'),
      null,
    );
    messenger.setMockMethodCallHandler(
      const MethodChannel('multitap/window/events'),
      null,
    );
  });
  for (final brightness in Brightness.values) {
    testWidgets('Only confirmed live shares light the toolbar in $brightness', (
      tester,
    ) async {
      tester.view.devicePixelRatio = 1;
      tester.view.physicalSize = const Size(1366, 768);
      addTearDown(tester.view.resetPhysicalSize);
      addTearDown(tester.view.resetDevicePixelRatio);
      final view = _View();
      try {
        await view.mount(tester, brightness: brightness);
        // Both sides granted: one button named for both, quiet until
        // something is connected. The tabs are inside its panel.
        expect(command(tester, _share).label, '폴더·USB');
        expect(command(tester, _share).enabled, isTrue);
        expect(command(tester, _share).active, isFalse);
        expect(command(tester, _share).badge, isNull);
        expect(command(tester, _share).hint, _idle);
        expect(find.byKey(_badge), findsNothing);
        expect(find.byKey(_drives), findsNothing);
        expect(find.byKey(_usb), findsNothing);
        view.folder('pending', DriveShareState.pending);
        view.folder('failed', DriveShareState.failed);
        view.device('local', UsbEntryState.available);
        view.device('pending', UsbEntryState.connecting);
        view.device('failed', UsbEntryState.failed);
        view.drive.changed();
        view.usb.changed();
        await tester.pump();
        expect(command(tester, _share).active, isFalse);
        expect(command(tester, _share).badge, isNull);
        expect(command(tester, _share).hint, _idle);
        // A mapped folder lights it; one whose device is gone does not count.
        final mapped = view.folder('mapped', DriveShareState.mapped);
        view.folder('offline', DriveShareState.mapped).offline = true;
        view.drive.changed();
        await tester.pump();
        expect(command(tester, _share).active, isTrue);
        expect(command(tester, _share).badge, _green);
        expect(command(tester, _share).hint, '공유 폴더 1개 연결됨');
        expect(find.byKey(_badge), findsOneWidget);
        // A shared device joins it: both are said, folders first.
        final shared = view.device('shared', UsbEntryState.shared);
        view.usb.changed();
        await tester.pump();
        expect(command(tester, _share).active, isTrue);
        expect(command(tester, _share).badge, _green);
        expect(command(tester, _share).hint, '공유 폴더 1개 연결됨 · USB 장치 1개 연결됨');
        // No new video frame or menu open is needed for this notification.
        mapped.state = DriveShareState.pending;
        view.drive.changed();
        await tester.pump();
        expect(command(tester, _share).active, isTrue);
        expect(command(tester, _share).hint, 'USB 장치 1개 연결됨');
        shared.state = UsbEntryState.returning;
        view.usb.changed();
        await tester.pump();
        expect(command(tester, _share).active, isFalse);
        expect(command(tester, _share).badge, isNull);
        expect(command(tester, _share).hint, _idle);
        expect(find.byKey(_badge), findsNothing);
        mapped.state = DriveShareState.mapped;
        shared.state = UsbEntryState.shared;
        view.drive.ready = false;
        view.usb.up = false;
        view.drive.changed();
        view.usb.changed();
        await tester.pump();
        expect(command(tester, _share).active, isFalse);
        expect(command(tester, _share).badge, isNull);
        view.drive.ready = true;
        view.usb.up = true;
        await view.drive.close();
        await view.usb.close();
        await tester.pump();
        expect(command(tester, _share).active, isFalse);
        expect(command(tester, _share).badge, isNull);
      } finally {
        await view.close(tester);
      }
    });
  }

  testWidgets('The panel has a tab for each side with its own count of what '
      'is connected', (tester) async {
    tester.view.devicePixelRatio = 1;
    tester.view.physicalSize = const Size(1366, 768);
    addTearDown(tester.view.resetPhysicalSize);
    addTearDown(tester.view.resetDevicePixelRatio);
    final view = _View();
    try {
      await view.mount(tester);
      await _openShare(tester);
      expect(find.byKey(_drives), findsOneWidget);
      expect(find.byKey(_usb), findsOneWidget);
      // Folders first; nothing connected: no pills.
      expect(find.byKey(const Key('drive-panel')), findsOneWidget);
      expect(find.byKey(const Key('usb-panel')), findsNothing);
      expect(_count(tester, 'drives'), isNull);
      expect(_count(tester, 'usb'), isNull);
      // The bar stays open under its panel.
      expect(find.byKey(const Key('session-command-bar')), findsOneWidget);
      // The pills follow the sessions while the panel is up. Only mapped
      // folders that can be reached and shared devices count.
      view.folder('one', DriveShareState.mapped);
      view.folder('two', DriveShareState.mapped);
      view.folder('offline', DriveShareState.mapped).offline = true;
      view.folder('pending', DriveShareState.pending);
      view.drive.changed();
      await tester.pump();
      expect(_count(tester, 'drives'), '2');
      expect(_count(tester, 'usb'), isNull);
      final shared = view.device('shared', UsbEntryState.shared);
      view.device('pending', UsbEntryState.connecting);
      view.usb.changed();
      await tester.pump();
      expect(_count(tester, 'drives'), '2');
      expect(_count(tester, 'usb'), '1');
      expect(command(tester, _share).hint, '공유 폴더 2개 연결됨 · USB 장치 1개 연결됨');
      shared.state = UsbEntryState.returning;
      view.usb.changed();
      await tester.pump();
      expect(_count(tester, 'usb'), isNull);
      expect(command(tester, _share).hint, '공유 폴더 2개 연결됨');
      await _closeShare(tester);
      expect(find.byKey(_drives), findsNothing);
      expect(tester.takeException(), isNull);
    } finally {
      await view.close(tester);
    }
  });

  testWidgets('The window remembers the tab 폴더·USB last showed', (
    tester,
  ) async {
    tester.view.devicePixelRatio = 1;
    tester.view.physicalSize = const Size(1366, 768);
    addTearDown(tester.view.resetPhysicalSize);
    addTearDown(tester.view.resetDevicePixelRatio);
    final view = _View();
    try {
      await view.mount(tester);
      await _openShare(tester);
      expect(find.byKey(const Key('drive-panel')), findsOneWidget);
      await tester.tap(find.byKey(_usb));
      await _popup(tester);
      expect(find.byKey(const Key('usb-panel')), findsOneWidget);
      expect(find.byKey(const Key('drive-panel')), findsNothing);
      await _closeShare(tester);
      await _openShare(tester);
      expect(find.byKey(const Key('usb-panel')), findsOneWidget);
      expect(find.byKey(const Key('drive-panel')), findsNothing);
      await _closeShare(tester);
      expect(tester.takeException(), isNull);
    } finally {
      await view.close(tester);
    }
  });

  testWidgets('Old session notifications cannot activate a replacement view', (
    tester,
  ) async {
    tester.view.devicePixelRatio = 1;
    tester.view.physicalSize = const Size(1366, 768);
    addTearDown(tester.view.resetPhysicalSize);
    addTearDown(tester.view.resetDevicePixelRatio);
    final old = _View(), current = _View();
    try {
      await old.mount(tester);
      old.folder('old', DriveShareState.mapped);
      old.device('old', UsbEntryState.shared);
      old.drive.changed();
      old.usb.changed();
      await tester.pump();
      expect(command(tester, _share).active, isTrue);
      expect(command(tester, _share).hint, '공유 폴더 1개 연결됨 · USB 장치 1개 연결됨');
      await old.close(tester);
      await current.mount(tester);
      old.drive.changed();
      old.usb.changed();
      await tester.pump();
      expect(command(tester, _share).active, isFalse);
      expect(command(tester, _share).badge, isNull);
      expect(command(tester, _share).hint, _idle);
      expect(tester.takeException(), isNull);
    } finally {
      await current.close(tester);
    }
  });

  testWidgets('Missing assignment channels do not reveal active actions', (
    tester,
  ) async {
    final view = _View(allowed: false);
    view.folder('not-granted', DriveShareState.mapped);
    view.device('not-granted', UsbEntryState.shared);
    try {
      await view.mount(tester);
      expect(find.byKey(_share), findsNothing);
      expect(find.byKey(_drives), findsNothing);
      expect(find.byKey(_usb), findsNothing);
      expect(find.byKey(_badge), findsNothing);
    } finally {
      await view.close(tester);
    }
  });

  testWidgets('One side only: the button is named for it and its panel has '
      'no tabs', (tester) async {
    tester.view.devicePixelRatio = 1;
    tester.view.physicalSize = const Size(1366, 768);
    addTearDown(tester.view.resetPhysicalSize);
    addTearDown(tester.view.resetDevicePixelRatio);
    final view = _View(usbAllowed: false);
    try {
      await view.mount(tester);
      expect(command(tester, _share).label, '공유 폴더');
      expect(command(tester, _share).hint, '공유 폴더 · PC 폴더를 데스크톱에서 열기');
      expect(command(tester, _share).active, isFalse);
      view.folder('mapped', DriveShareState.mapped);
      view.drive.changed();
      await tester.pump();
      expect(command(tester, _share).active, isTrue);
      expect(command(tester, _share).badge, _green);
      expect(command(tester, _share).hint, '공유 폴더 1개 연결됨');
      await _openShare(tester);
      expect(find.byKey(const Key('drive-panel')), findsOneWidget);
      expect(find.byKey(_drives), findsNothing);
      expect(find.byKey(_usb), findsNothing);
      await _closeShare(tester);
      expect(tester.takeException(), isNull);
    } finally {
      await view.close(tester);
    }
  });

  testWidgets('USB the Host offers but the administrator did not allow stays '
      'as a tab that says why, beside usable shared folders', (tester) async {
    tester.view.devicePixelRatio = 1;
    tester.view.physicalSize = const Size(1366, 768);
    addTearDown(tester.view.resetPhysicalSize);
    addTearDown(tester.view.resetDevicePixelRatio);
    final view = _View(usbAllowed: false, usbRequested: true);
    try {
      await view.mount(tester);
      // Usable (the folders): the button is the group's, and enabled.
      expect(command(tester, _share).label, '폴더·USB');
      expect(command(tester, _share).enabled, isTrue);
      expect(command(tester, _share).hint, _idle);
      await _openShare(tester);
      expect(find.byKey(_drives), findsOneWidget);
      expect(find.byKey(_usb), findsOneWidget);
      expect(find.byKey(const Key('drive-panel')), findsOneWidget);
      await tester.tap(find.byKey(_usb));
      await _popup(tester);
      expect(find.byKey(const Key('drive-panel')), findsNothing);
      expect(find.byKey(const Key('usb-panel')), findsNothing);
      expect(
        tester.widget<Text>(find.byKey(const Key('share-usb-reason'))).data,
        'USB 장치를 쓸 수 없어요. $adminPermissionNeeded.',
      );
      expect(adminPermissionNeeded, '관리자 허용이 필요해요');
      await _closeShare(tester);
      expect(tester.takeException(), isNull);
    } finally {
      await view.close(tester);
    }
  });

  testWidgets(
    'Additive active state survives lifecycle copies and overflow without changing defaults',
    (tester) async {
      final active = SessionToolbarMenu.command(
        label: 'USB 장치',
        icon: Icons.usb_rounded,
        onPressed: () {},
        onInteract: () {},
        active: true,
        badge: const Color(0xFF62E3A9),
        tooltip: 'USB 장치 2개 연결됨',
      );
      final copy = active.withLifecycle(
        (_) {},
        compact: true,
        commandBar: true,
      );
      expect(copy.active, isTrue);
      expect(copy.badge, active.badge);
      expect(copy.hint, active.hint);
      expect(copy.overflowItems.single.checked, isTrue);
      final normal = SessionToolbarMenu.command(
        label: '기본',
        icon: Icons.folder_shared_rounded,
        onPressed: () {},
        onInteract: () {},
      );
      expect(normal.active, isFalse);
      expect(normal.withLifecycle((_) {}).active, isFalse);
      expect(normal.overflowItems.single.checked, isFalse);
      await tester.pumpWidget(MaterialApp(home: Scaffold(body: copy)));
      final button = tester.widget<TextButton>(find.byType(TextButton));
      expect(
        button.style!.foregroundColor!.resolve({}),
        const Color(0xFFF5A623),
      );
      await tester.pumpWidget(const SizedBox.shrink());
    },
  );

  testWidgets('A group command lists its entries in the narrow 메뉴 under '
      'its caption, and its bar button keeps the dot and the lit name', (
    tester,
  ) async {
    final group = SessionToolbarMenu.command(
      key: const Key('remote-share'),
      label: '폴더·USB',
      icon: Icons.folder_shared_rounded,
      onPressed: () {},
      onPressedAt: (_) async => false,
      onInteract: () {},
      active: true,
      badge: _green,
      tooltip: '공유 폴더 1개 연결됨',
      overflow: [
        SessionMenuItem(
          '공유 폴더',
          key: const Key('remote-drives'),
          value: '1개 연결됨',
          onSelected: () {},
        ),
        const SessionMenuItem('USB 장치', key: Key('remote-usb')),
      ],
    );
    // As the bar builds it.
    final copy = group.withLifecycle(
      (_) {},
      denseLabel: true,
      commandBar: true,
    );
    expect(copy.overflow, group.overflow);
    final rows = copy.overflowItems;
    expect(rows.map((row) => row.label).toList(), [
      '폴더·USB',
      '공유 폴더',
      'USB 장치',
    ]);
    expect(rows.first.heading, isTrue);
    expect(rows[1].key, const Key('remote-drives'));
    expect(rows[1].value, '1개 연결됨');
    expect(rows[2].key, const Key('remote-usb'));
    expect(rows[2].onSelected, isNull);
    await tester.pumpWidget(MaterialApp(home: Scaffold(body: copy)));
    expect(find.byKey(_badge), findsOneWidget);
    expect(
      tester.widget<Container>(find.byKey(_badge)).decoration,
      isA<BoxDecoration>().having((d) => d.color, 'color', _green),
    );
    final button = tester.widget<TextButton>(find.byType(TextButton));
    expect(button.style!.foregroundColor!.resolve({}), const Color(0xFFF5A623));
    expect(find.byTooltip('공유 폴더 1개 연결됨'), findsOneWidget);
    // The bar's commands carry no chevron any more.
    expect(find.byIcon(Icons.expand_more_rounded), findsNothing);
    await tester.pumpWidget(const SizedBox.shrink());
  });
}
