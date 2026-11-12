// The session bar's panels on their own (2026-11-10, session bar v2): 화면,
// 폴더·USB, 설정 and the quality chip's 연결. Widgets with memory fakes only:
// no socket, file system, USB backend, Windows DPI or window is touched.
// How RemoteView opens them is in session_toolbar_items_test.
import 'dart:typed_data';

import 'package:flutter/material.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:multitap_client/data/display_scale.dart';
import 'package:multitap_client/data/drive_session.dart';
import 'package:multitap_client/data/remote_session.dart';
import 'package:multitap_client/data/usb_redirection.dart';
import 'package:multitap_client/models/session_display_mode.dart';
import 'package:multitap_client/ui/app_theme.dart';
import 'package:multitap_client/ui/session_connection_panel.dart';
import 'package:multitap_client/ui/session_menu.dart' show SessionPopups;
import 'package:multitap_client/ui/session_panels.dart';
import 'package:multitap_client/ui/session_tools.dart';

import 'package:flutter/rendering.dart';
import 'package:multitap_client/l10n/l10n.dart';

import 'drive_session_test.dart' show FakeFiles;
import 'usb_redirection_test.dart' show FakeBackend;

final _url = Uri.parse('wss://fixture.invalid/api/v1/sessions/panels');

/// The session whose 배율 the 화면 panel shows (as display_scale_panel_test).
class _Session extends Fake implements RemoteSession {
  DisplayScaleApplied? applied;
  int? wanted, sourceW = 960, sourceH = 640;
  bool ended = false;
  @override
  DisplayScaleApplied? get displayScaleApplied => applied;
  @override
  int? get displayScaleWanted => wanted;
  @override
  bool get closed => ended;
  @override
  int? get sourceWidth => sourceW;
  @override
  int? get sourceHeight => sourceH;
  @override
  void addListener(VoidCallback listener) {}
  @override
  void removeListener(VoidCallback listener) {}
}

/// Granted shared folders whose rows the test sets (as
/// toolbar_device_state_test's): no socket, no file system.
class _Drive extends DriveSession {
  _Drive()
    : super(
        url: _url,
        token: 'fixture',
        sessionId: 'panels',
        files: FakeFiles(),
      ) {
    ready = true;
  }
  final rows = <DriveShare>[];
  @override
  List<DriveShare> get shares => List.unmodifiable(rows);
  void changed() => notifyListeners();

  DriveShare folder(String id, DriveShareState state) {
    final share = DriveShare(id: id, path: 'C:\\Fixture\\$id', label: id)
      ..state = state;
    rows.add(share);
    return share;
  }
}

/// Granted USB whose entries the test sets: no socket, no capture, and no
/// listing of this PC's devices when its panel opens (the entries stay the
/// test's; the real listing has its own test below).
class _Usb extends UsbSession {
  _Usb()
    : super(
        url: _url,
        token: 'fixture',
        sessionId: 'panels',
        backend: FakeBackend(),
      );
  bool up = true, ended = false;
  @override
  bool get ready => up && !ended;
  @override
  bool get closed => ended;
  @override
  Future<void> refresh() async {}
  void changed() => notifyListeners();

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
    entries = [...entries, entry];
    return entry;
  }
}

/// The lines [text] is laid out on inside [inside], and whether it was cut.
({int lines, bool cut}) _laidOut(WidgetTester tester, Finder inside, String text) {
  final paragraph = tester.renderObject<RenderParagraph>(
    find.descendant(
      of: inside,
      matching: find.byWidgetPredicate(
        (widget) => widget is RichText && widget.text.toPlainText() == text,
      ),
    ),
  );
  final tops = {
    for (final box in paragraph.getBoxesForSelection(
      TextSelection(baseOffset: 0, extentOffset: text.length),
    ))
      box.top.round(),
  };
  return (lines: tops.length, cut: paragraph.didExceedMaxLines);
}

/// [panel] at the top of a dark window, [width] wide, as under the bar.
Future<void> _show(
  WidgetTester tester,
  Widget panel, {
  required double width,
}) async {
  tester.view.devicePixelRatio = 1;
  tester.view.physicalSize = const Size(900, 900);
  addTearDown(tester.view.resetPhysicalSize);
  addTearDown(tester.view.resetDevicePixelRatio);
  await tester.pumpWidget(
    MaterialApp(
      theme: buildAppTheme(brightness: Brightness.dark),
      home: Scaffold(
        body: Align(
          alignment: Alignment.topCenter,
          child: SizedBox(width: width, child: panel),
        ),
      ),
    ),
  );
  await tester.pump();
  await tester.pump();
}

Finder _key(String key) => find.byKey(Key(key));

/// The tap target of a keyed row, tile or tab: its one InkWell. A
/// SessionPanelRow hands its own key on to its InkWell, so for a row the
/// key alone finds both widgets; a tile's or a tab's key is on its InkWell
/// only.
Finder _row(String key) {
  final inside = find.descendant(of: _key(key), matching: find.byType(InkWell));
  return inside.evaluate().isNotEmpty ? inside : _key(key);
}

/// The Semantics a tile, tab or row is described by (selected / toggled /
/// enabled): the nearest one around its InkWell that says [says].
Semantics _semantics(
  WidgetTester tester,
  String key,
  bool Function(Semantics) says,
) => tester.widget<Semantics>(
  find
      .ancestor(
        of: _row(key),
        matching: find.byWidgetPredicate(
          (widget) => widget is Semantics && says(widget),
        ),
      )
      .first,
);

bool? _selected(WidgetTester tester, String key) => _semantics(
  tester,
  key,
  (semantics) => semantics.properties.selected != null,
).properties.selected;

bool? _toggled(WidgetTester tester, String key) => _semantics(
  tester,
  key,
  (semantics) => semantics.properties.toggled != null,
).properties.toggled;

String? _text(WidgetTester tester, String key) =>
    tester.widget<Text>(_key(key)).data;

bool _has(String inside, String text) => find
    .descendant(of: _key(inside), matching: find.text(text))
    .evaluate()
    .isNotEmpty;

Switch _switch(WidgetTester tester, String key) => tester.widget<Switch>(
  find.descendant(of: _key(key), matching: find.byType(Switch)),
);

/// The number and its unit drawn as one rich text inside [inside].
TextSpan _rich(WidgetTester tester, String inside) =>
    tester
            .widget<Text>(
              find.descendant(
                of: _key(inside),
                matching: find.byWidgetPredicate(
                  (widget) => widget is Text && widget.textSpan != null,
                ),
                matchRoot: true,
              ),
            )
            .textSpan!
        as TextSpan;

/// Measured values as sessionQualityValues words them.
List<SessionQualityValue> _values() => [
  (id: 'resolution', label: '해상도 · 원격 화면', value: '1920 × 1080'),
  (id: 'fps', label: '화면 · PC 디코딩', value: '60.0 fps'),
  (id: 'rate', label: '수신 · 받은 영상', value: '12.3 Mbps'),
  (id: 'path', label: '영상 경로', value: 'UDP 직접'),
  (id: 'encode', label: '인코딩 · Host 처리', value: '1.25 ms'),
  (id: 'decode', label: '디코딩 · PC 처리', value: '2.50 ms'),
];

void main() {
  group('화면 (SessionScreenPanel)', () {
    const window = SessionDisplayMode.windowed,
        full = SessionDisplayMode.fullscreen,
        all = SessionDisplayMode.allMonitors;

    testWidgets('a tile per offered mode; the current one is marked and only '
        'closes, another is chosen', (tester) async {
      final choices = <SessionScreenChoice?>[];
      await _show(
        tester,
        SessionScreenPanel(
          modes: const [window, full],
          shown: window,
          onSelected: choices.add,
        ),
        width: SessionScreenPanel.widthFor(2),
      );
      expect(_key('session-screen-panel'), findsOneWidget);
      expect(_key('stream-windowed'), findsOneWidget);
      expect(_key('stream-fullscreen'), findsOneWidget);
      expect(_key('stream-all-monitors'), findsNothing);
      expect(_key('remote-virtual-displays'), findsNothing);
      // The header names the panel and the mode in use.
      expect(find.text(SessionTool.screen.label), findsOneWidget);
      expect(find.text('화면'), findsOneWidget);
      expect(find.text('창 모드'), findsNWidgets(2));
      expect(_has('stream-windowed', '창 모드'), isTrue);
      expect(_has('stream-fullscreen', '전체 화면'), isTrue);
      // Side by side, in the offered order.
      final first = tester.getRect(_key('stream-windowed'));
      final second = tester.getRect(_key('stream-fullscreen'));
      expect(first.top, second.top);
      expect(first.height, second.height);
      expect(first.right, lessThanOrEqualTo(second.left));
      // Only the current one is selected, with the one check mark.
      expect(_selected(tester, 'stream-windowed'), isTrue);
      expect(_selected(tester, 'stream-fullscreen'), isFalse);
      expect(
        find.descendant(
          of: _key('stream-windowed'),
          matching: find.byIcon(Icons.check_rounded),
        ),
        findsOneWidget,
      );
      expect(find.byIcon(Icons.check_rounded), findsOneWidget);
      // Choosing the current mode closes without a change (null).
      await tester.tap(_key('stream-windowed'));
      expect(choices, [null]);
      await tester.tap(_key('stream-fullscreen'));
      expect(choices, hasLength(2));
      expect(choices.last!.mode, full);
      expect(choices.last!.scale, isNull);
      expect(choices.last!.displays, isFalse);
    });

    testWidgets('모든 모니터 사용 is a third tile where it is offered, in a '
        'wider panel', (tester) async {
      expect(SessionScreenPanel.widthFor(1), 316);
      expect(SessionScreenPanel.widthFor(2), 316);
      expect(SessionScreenPanel.widthFor(3), 372);
      final choices = <SessionScreenChoice?>[];
      await _show(
        tester,
        SessionScreenPanel(
          modes: const [window, full, all],
          shown: all,
          onSelected: choices.add,
        ),
        width: SessionScreenPanel.widthFor(3),
      );
      final lefts = [
        for (final key in [
          'stream-windowed',
          'stream-fullscreen',
          'stream-all-monitors',
        ])
          tester.getRect(_key(key)).left,
      ];
      expect(lefts[0], lessThan(lefts[1]));
      expect(lefts[1], lessThan(lefts[2]));
      expect(_has('stream-all-monitors', '모든 모니터'), isTrue);
      expect(_selected(tester, 'stream-all-monitors'), isTrue);
      expect(_selected(tester, 'stream-windowed'), isFalse);
      expect(_selected(tester, 'stream-fullscreen'), isFalse);
      await tester.tap(_key('stream-all-monitors'));
      expect(choices, [null]);
      await tester.tap(_key('stream-windowed'));
      expect(choices.last!.mode, window);
      expect(tester.takeException(), isNull);
    });

    testWidgets('while a reconnect or a screen change is under way the modes '
        'wait', (tester) async {
      final choices = <SessionScreenChoice?>[];
      await _show(
        tester,
        SessionScreenPanel(
          modes: const [window, full],
          shown: full,
          changing: true,
          onSelected: choices.add,
        ),
        width: SessionScreenPanel.widthFor(2),
      );
      for (final key in ['stream-windowed', 'stream-fullscreen']) {
        expect(tester.widget<InkWell>(_key(key)).onTap, isNull, reason: key);
        expect(
          _semantics(
            tester,
            key,
            (semantics) => semantics.properties.selected != null,
          ).properties.enabled,
          isFalse,
          reason: key,
        );
        await tester.tap(_key(key), warnIfMissed: false);
      }
      expect(choices, isEmpty);
      // Still readable: the mode in use stays marked.
      expect(_selected(tester, 'stream-fullscreen'), isTrue);
    });

    testWidgets('가상 모니터 1개·2개 구성… only where the count is offered, and '
        'usable only with a picture', (tester) async {
      final choices = <SessionScreenChoice?>[];
      SessionScreenPanel panel({required bool offered, bool enabled = false}) =>
          SessionScreenPanel(
            modes: const [window, full],
            shown: window,
            displaysOffered: offered,
            displaysEnabled: enabled,
            onSelected: choices.add,
          );
      await _show(tester, panel(offered: false), width: 316);
      expect(_key('remote-virtual-displays'), findsNothing);
      await _show(tester, panel(offered: true), width: 316);
      expect(_has('remote-virtual-displays', '모니터 수…'), isTrue);
      expect(
        tester.widget<InkWell>(_row('remote-virtual-displays')).onTap,
        isNull,
      );
      await _show(tester, panel(offered: true, enabled: true), width: 316);
      // Under the modes, above 배율.
      final row = tester.getRect(_row('remote-virtual-displays'));
      expect(
        tester.getRect(_key('stream-windowed')).bottom,
        lessThanOrEqualTo(row.top),
      );
      expect(
        row.bottom,
        lessThanOrEqualTo(tester.getRect(_key('remote-display-scale')).top),
      );
      await tester.tap(_row('remote-virtual-displays'));
      expect(choices, hasLength(1));
      expect(choices.single!.displays, isTrue);
      expect(choices.single!.mode, isNull);
      expect(choices.single!.scale, isNull);
    });

    testWidgets('배율 before a picture is up is one line that says so', (
      tester,
    ) async {
      await _show(
        tester,
        SessionScreenPanel(
          modes: const [window, full],
          shown: window,
          onSelected: (_) => fail('nothing to choose'),
        ),
        width: 316,
      );
      // The caption with its note, then the reason instead of the steps.
      expect(_has('remote-display-scale', SessionTool.scale.label), isTrue);
      expect(_has('remote-display-scale', '배율'), isTrue);
      expect(_has('remote-display-scale', 'Windows 글자·아이콘 크기'), isTrue);
      expect(_text(tester, 'display-scale-unavailable'), '연결된 뒤 바꿀 수 있어요');
      expect(_key('display-scale-steps'), findsNothing);
      expect(_key('display-scale-panel'), findsNothing);
      expect(_key('display-scale-status'), findsNothing);
      expect(
        tester.getRect(_key('remote-display-scale')).bottom,
        lessThanOrEqualTo(
          tester.getRect(_key('display-scale-unavailable')).top,
        ),
      );
      // The caller's own reason.
      await _show(
        tester,
        SessionScreenPanel(
          modes: const [window, full],
          shown: window,
          scaleReason: '화면 전환이 끝나면 바꿀 수 있어요',
          onSelected: (_) {},
        ),
        width: 316,
      );
      expect(_text(tester, 'display-scale-unavailable'), '화면 전환이 끝나면 바꿀 수 있어요');
    });

    testWidgets('배율: five steps across in one row; the applied one is '
        'selected, one above the Host\'s maximum is greyed, a step is '
        'chosen', (tester) async {
      final session = _Session()
        ..applied = const DisplayScaleApplied(
          ok: true,
          scale: 125,
          maximum: 150,
          code: 0,
        );
      final choices = <SessionScreenChoice?>[];
      await _show(
        tester,
        SessionScreenPanel(
          modes: const [window, full],
          shown: window,
          session: session,
          scaleAvailable: true,
          viewportPixels: const Size(960, 640),
          resolutionAvailable: true,
          onSelected: choices.add,
        ),
        width: 316,
      );
      expect(_key('display-scale-unavailable'), findsNothing);
      expect(_key('display-scale-panel'), findsOneWidget);
      // One caption (the panel has no title of its own inside 화면).
      expect(find.text('배율'), findsOneWidget);
      const steps = [100, 125, 150, 175, 200];
      final rows = [
        for (final step in steps)
          tester.getRect(_key('display-scale-$step-row')),
      ];
      final strip = tester.getRect(_key('display-scale-steps'));
      for (var n = 0; n < rows.length; n++) {
        expect(rows[n].top, rows.first.top, reason: '${steps[n]}');
        expect(rows[n].height, rows.first.height, reason: '${steps[n]}');
        expect(rows[n].left, greaterThanOrEqualTo(strip.left));
        expect(rows[n].right, lessThanOrEqualTo(strip.right));
        if (n > 0) {
          expect(
            rows[n].left,
            greaterThanOrEqualTo(rows[n - 1].right),
            reason: '${steps[n]}',
          );
        }
        expect(
          find.descendant(
            of: _key('display-scale-${steps[n]}'),
            matching: find.text('${steps[n]}%'),
          ),
          findsOneWidget,
        );
      }
      expect(_key('display-scale-250'), findsNothing);
      // Under the caption.
      expect(
        tester.getRect(_key('remote-display-scale')).bottom,
        lessThanOrEqualTo(strip.top),
      );
      bool selected(int step) =>
          tester
              .widget<Semantics>(_key('display-scale-$step-selected'))
              .properties
              .selected ==
          true;
      for (final step in steps) {
        expect(selected(step), step == 125, reason: '$step');
      }
      // The selected step has no check mark any more (the mode tile has).
      expect(
        find.descendant(
          of: _key('display-scale-steps'),
          matching: find.byIcon(Icons.check_rounded),
        ),
        findsNothing,
      );
      VoidCallback? pressed(int step) =>
          tester.widget<TextButton>(_key('display-scale-$step')).onPressed;
      for (final step in [100, 125, 150]) {
        expect(pressed(step), isNotNull, reason: '$step');
      }
      for (final step in [175, 200]) {
        expect(pressed(step), isNull, reason: '$step');
      }
      // The same status lines as before.
      expect(_text(tester, 'display-scale-status'), '자동 · 현재 125%');
      expect(_text(tester, 'display-scale-maximum'), '이 화면 크기에서 최대 150%');
      expect(
        _text(tester, 'display-resolution-status'),
        '해상도 자동 · 현재 960 × 640',
      );
      await tester.tap(_key('display-scale-200'), warnIfMissed: false);
      expect(choices, isEmpty);
      await tester.tap(_key('display-scale-150'));
      expect(choices, hasLength(1));
      expect(choices.single!.scale, 150);
      expect(choices.single!.mode, isNull);
      expect(choices.single!.displays, isFalse);
      expect(tester.takeException(), isNull);
    });

    testWidgets('배율 says what was asked and what applies, and greys every '
        'step on a connection that cannot change it', (tester) async {
      final session = _Session()
        ..sourceW = 1200
        ..sourceH = 800
        ..wanted = 175
        ..applied = const DisplayScaleApplied(
          ok: true,
          scale: 125,
          maximum: 200,
          code: 0,
        );
      SessionScreenPanel panel({required bool available}) => SessionScreenPanel(
        modes: const [window, full],
        shown: full,
        session: session,
        scaleAvailable: available,
        onSelected: (choice) => fail('nothing was chosen: $choice'),
      );
      await _show(tester, panel(available: true), width: 316);
      expect(_text(tester, 'display-scale-status'), '적용 125% · 요청 175%');
      expect(_key('display-scale-maximum'), findsNothing);
      expect(
        _text(tester, 'display-resolution-status'),
        '해상도 자동 · 현재 1200 × 800',
      );
      await _show(tester, panel(available: false), width: 316);
      expect(_text(tester, 'display-scale-status'), '이 연결에서는 배율을 바꿀 수 없어요.');
      for (final step in [100, 125, 150, 175, 200]) {
        expect(
          tester.widget<TextButton>(_key('display-scale-$step')).onPressed,
          isNull,
          reason: '$step',
        );
      }
      // Nothing applied yet, on a session that follows the window.
      session
        ..wanted = null
        ..applied = null
        ..sourceW = null
        ..sourceH = null;
      await _show(tester, panel(available: true), width: 316);
      expect(_text(tester, 'display-scale-status'), '자동 · 현재 배율 확인 전');
      expect(_text(tester, 'display-resolution-status'), '해상도 자동 · 현재 크기 확인 전');
      expect(tester.takeException(), isNull);
    });
  });

  group('폴더·USB (SessionSharePanel)', () {
    testWidgets('both sides: a tab each; a tab swaps the body and is '
        'reported', (tester) async {
      final drive = _Drive(), usb = _Usb();
      final shown = <SessionShareTab>[];
      await _show(
        tester,
        SessionSharePanel(drive: drive, usb: usb, onTab: shown.add),
        width: SessionSharePanel.width,
      );
      expect(SessionSharePanel.width, 400);
      expect(_key('session-share-panel'), findsOneWidget);
      expect(_key('remote-drives'), findsOneWidget);
      expect(_key('remote-usb'), findsOneWidget);
      expect(_has('remote-drives', SessionTool.drives.label), isTrue);
      expect(_has('remote-drives', '공유 폴더'), isTrue);
      expect(_has('remote-usb', SessionTool.usb.label), isTrue);
      expect(_has('remote-usb', 'USB 장치'), isTrue);
      final folders = tester.getRect(_key('remote-drives'));
      final devices = tester.getRect(_key('remote-usb'));
      expect(folders.top, devices.top);
      expect(folders.right, lessThanOrEqualTo(devices.left));
      // Folders first.
      expect(_selected(tester, 'remote-drives'), isTrue);
      expect(_selected(tester, 'remote-usb'), isFalse);
      expect(_key('drive-panel'), findsOneWidget);
      expect(_key('usb-panel'), findsNothing);
      expect(
        folders.bottom,
        lessThanOrEqualTo(tester.getRect(_key('drive-panel')).top),
      );
      // Nothing connected: no count pills.
      expect(_key('remote-drives-count'), findsNothing);
      expect(_key('remote-usb-count'), findsNothing);
      expect(shown, isEmpty);
      await tester.tap(_key('remote-usb'));
      await tester.pump();
      await tester.pump();
      expect(_selected(tester, 'remote-drives'), isFalse);
      expect(_selected(tester, 'remote-usb'), isTrue);
      expect(_key('usb-panel'), findsOneWidget);
      expect(_key('drive-panel'), findsNothing);
      expect(shown, [SessionShareTab.usb]);
      // The tab already shown is not reported again.
      await tester.tap(_key('remote-usb'));
      await tester.pump();
      expect(shown, [SessionShareTab.usb]);
      await tester.tap(_key('remote-drives'));
      await tester.pump();
      expect(_key('drive-panel'), findsOneWidget);
      expect(_key('usb-panel'), findsNothing);
      expect(shown, [SessionShareTab.usb, SessionShareTab.folders]);
      expect(tester.takeException(), isNull);
      await tester.pumpWidget(const SizedBox.shrink());
    });

    testWidgets('the USB tab lists this PC\'s devices as it opens, beside '
        'the tab that already follows the same session', (tester) async {
      // The real UsbSession.refresh, as a RemoteView's session has it: the
      // USB panel starts a listing while it is first built, and the session
      // says so at once to whoever already listens (the tab's count here,
      // RemoteSession and RemoteView in a window). That must not be an
      // error of the frame that is building.
      final usb = UsbSession(
        url: _url,
        token: 'fixture',
        sessionId: 'panels',
        backend: FakeBackend(),
      );
      await _show(
        tester,
        SessionSharePanel(drive: _Drive(), usb: usb),
        width: SessionSharePanel.width,
      );
      expect(_key('drive-panel'), findsOneWidget);
      await tester.tap(_key('remote-usb'));
      await tester.pump();
      await tester.pump();
      expect(tester.takeException(), isNull);
      expect(_key('usb-panel'), findsOneWidget);
      expect(_key('drive-panel'), findsNothing);
      await tester.pumpWidget(const SizedBox.shrink());
      await tester.runAsync(usb.close);
    });

    testWidgets('each tab counts what is connected now, live', (tester) async {
      final drive = _Drive(), usb = _Usb();
      await _show(
        tester,
        SessionSharePanel(drive: drive, usb: usb),
        width: SessionSharePanel.width,
      );
      String? count(String side) {
        final pill = _key('remote-$side-count');
        if (pill.evaluate().isEmpty) return null;
        return tester
            .widget<Text>(
              find.descendant(of: pill, matching: find.byType(Text)),
            )
            .data;
      }

      expect(count('drives'), isNull);
      expect(count('usb'), isNull);
      // Mapped folders that can be reached; not pending, failed or offline.
      final first = drive.folder('one', DriveShareState.mapped);
      drive.folder('two', DriveShareState.mapped);
      drive.folder('gone', DriveShareState.mapped).offline = true;
      drive.folder('pending', DriveShareState.pending);
      drive.folder('failed', DriveShareState.failed);
      drive.changed();
      await tester.pump();
      expect(count('drives'), '2');
      expect(count('usb'), isNull);
      expect(SessionSharePanel.mappedFolders(drive), 2);
      // The pill is inside its tab.
      expect(
        find.descendant(
          of: _key('remote-drives'),
          matching: _key('remote-drives-count'),
        ),
        findsOneWidget,
      );
      // Shared devices; not one that is connecting, returning or only here.
      final shared = usb.device('shared', UsbEntryState.shared);
      usb.device('connecting', UsbEntryState.connecting);
      usb.device('local', UsbEntryState.available);
      usb.changed();
      await tester.pump();
      expect(count('drives'), '2');
      expect(count('usb'), '1');
      expect(SessionSharePanel.sharedUsb(usb), 1);
      expect(
        find.descendant(
          of: _key('remote-usb'),
          matching: _key('remote-usb-count'),
        ),
        findsOneWidget,
      );
      first.state = DriveShareState.pending;
      drive.changed();
      shared.state = UsbEntryState.returning;
      usb.changed();
      await tester.pump();
      expect(count('drives'), '1');
      expect(count('usb'), isNull);
      // A channel that is not ready, or ended, has nothing connected.
      shared.state = UsbEntryState.shared;
      usb.up = false;
      usb.changed();
      drive.ready = false;
      drive.changed();
      await tester.pump();
      expect(count('drives'), isNull);
      expect(count('usb'), isNull);
      expect(SessionSharePanel.mappedFolders(drive), 0);
      expect(SessionSharePanel.sharedUsb(usb), 0);
      usb.up = true;
      drive.ready = true;
      expect(SessionSharePanel.mappedFolders(drive), 1);
      expect(SessionSharePanel.sharedUsb(usb), 1);
      drive.closed = true;
      usb.ended = true;
      expect(SessionSharePanel.mappedFolders(drive), 0);
      expect(SessionSharePanel.sharedUsb(usb), 0);
      expect(SessionSharePanel.mappedFolders(null), 0);
      expect(SessionSharePanel.sharedUsb(null), 0);
      expect(tester.takeException(), isNull);
      await tester.pumpWidget(const SizedBox.shrink());
    });

    testWidgets('one side only: no tabs, the body directly', (tester) async {
      await _show(
        tester,
        SessionSharePanel(drive: _Drive()),
        width: SessionSharePanel.width,
      );
      expect(_key('session-share-panel'), findsOneWidget);
      expect(_key('remote-drives'), findsNothing);
      expect(_key('remote-usb'), findsNothing);
      expect(_key('drive-panel'), findsOneWidget);
      expect(_key('usb-panel'), findsNothing);
      // USB only, whatever tab was remembered.
      for (final initial in SessionShareTab.values) {
        await tester.pumpWidget(const SizedBox.shrink());
        await _show(
          tester,
          SessionSharePanel(usb: _Usb(), initial: initial),
          width: SessionSharePanel.width,
        );
        expect(_key('remote-drives'), findsNothing, reason: '$initial');
        expect(_key('remote-usb'), findsNothing, reason: '$initial');
        expect(_key('usb-panel'), findsOneWidget, reason: '$initial');
        expect(_key('drive-panel'), findsNothing, reason: '$initial');
      }
      // Folders only, with the USB tab remembered from another connection.
      await tester.pumpWidget(const SizedBox.shrink());
      await _show(
        tester,
        SessionSharePanel(drive: _Drive(), initial: SessionShareTab.usb),
        width: SessionSharePanel.width,
      );
      expect(_key('drive-panel'), findsOneWidget);
      expect(_key('usb-panel'), findsNothing);
      expect(tester.takeException(), isNull);
      await tester.pumpWidget(const SizedBox.shrink());
    });

    testWidgets('initial picks the tab first shown', (tester) async {
      final shown = <SessionShareTab>[];
      await _show(
        tester,
        SessionSharePanel(
          drive: _Drive(),
          usb: _Usb(),
          initial: SessionShareTab.usb,
          onTab: shown.add,
        ),
        width: SessionSharePanel.width,
      );
      expect(_selected(tester, 'remote-usb'), isTrue);
      expect(_selected(tester, 'remote-drives'), isFalse);
      expect(_key('usb-panel'), findsOneWidget);
      expect(_key('drive-panel'), findsNothing);
      // Opening on it is not a change of tab.
      expect(shown, isEmpty);
      await tester.tap(_key('remote-drives'));
      await tester.pump();
      expect(_key('drive-panel'), findsOneWidget);
      expect(shown, [SessionShareTab.folders]);
      await tester.pumpWidget(const SizedBox.shrink());
    });

    testWidgets('a side this connection cannot use stays as a tab that says '
        'why', (tester) async {
      // Shared folders allowed, the VM cannot map them; USB works.
      await _show(
        tester,
        SessionSharePanel(usb: _Usb(), driveUnavailable: 'VM에서 연결하지 못했어요'),
        width: SessionSharePanel.width,
      );
      expect(_key('remote-drives'), findsOneWidget);
      expect(_key('remote-usb'), findsOneWidget);
      // The first usable-or-not tab in order: folders, with the reason.
      expect(_selected(tester, 'remote-drives'), isTrue);
      expect(_key('drive-panel'), findsNothing);
      expect(_key('usb-panel'), findsNothing);
      expect(
        _text(tester, 'share-drives-reason'),
        '공유 폴더를 쓸 수 없어요. VM에서 연결하지 못했어요.',
      );
      expect(
        find.descendant(
          of: _key('remote-drives'),
          matching: find.byIcon(Icons.folder_off_rounded),
        ),
        findsOneWidget,
      );
      expect(_key('remote-drives-count'), findsNothing);
      await tester.tap(_key('remote-usb'));
      await tester.pump();
      await tester.pump();
      expect(_key('usb-panel'), findsOneWidget);
      expect(_key('share-drives-reason'), findsNothing);

      // USB offered by the Host, not allowed by the administrator.
      await tester.pumpWidget(const SizedBox.shrink());
      await _show(
        tester,
        SessionSharePanel(drive: _Drive(), usbUnavailable: '관리자 허용이 필요해요'),
        width: SessionSharePanel.width,
      );
      expect(_key('drive-panel'), findsOneWidget);
      expect(
        find.descendant(
          of: _key('remote-usb'),
          matching: find.byIcon(Icons.usb_off_rounded),
        ),
        findsOneWidget,
      );
      await tester.tap(_key('remote-usb'));
      await tester.pump();
      expect(_key('drive-panel'), findsNothing);
      expect(_key('usb-panel'), findsNothing);
      expect(
        _text(tester, 'share-usb-reason'),
        'USB 장치를 쓸 수 없어요. 관리자 허용이 필요해요.',
      );

      // Only a reason, nothing usable: the reason, without tabs.
      await tester.pumpWidget(const SizedBox.shrink());
      await _show(
        tester,
        const SessionSharePanel(usbUnavailable: '관리자 허용이 필요해요'),
        width: SessionSharePanel.width,
      );
      expect(_key('remote-drives'), findsNothing);
      expect(_key('remote-usb'), findsNothing);
      expect(
        _text(tester, 'share-usb-reason'),
        'USB 장치를 쓸 수 없어요. 관리자 허용이 필요해요.',
      );
      expect(tester.takeException(), isNull);
      await tester.pumpWidget(const SizedBox.shrink());
    });
  });

  group('설정 (SessionSettingsPanel)', () {
    testWidgets('rows only for what this connection has', (tester) async {
      final choices = <SessionSettingsChoice>[];
      await _show(
        tester,
        SessionSettingsPanel(onSelected: choices.add),
        width: SessionSettingsPanel.width,
      );
      expect(SessionSettingsPanel.width, 336);
      expect(_key('session-settings-panel'), findsOneWidget);
      expect(find.text(SessionTool.more.label), findsOneWidget);
      expect(find.text('설정'), findsOneWidget);
      // 바 열어 두기 is always there; without a bar to switch it is greyed.
      expect(_row('pin-session-toolbar'), findsOneWidget);
      expect(tester.widget<InkWell>(_row('pin-session-toolbar')).onTap, isNull);
      for (final key in [
        'remote-stream-settings',
        'session-game-mouse',
        'remote-session-guide',
      ]) {
        expect(_key(key), findsNothing, reason: key);
      }

      await _show(
        tester,
        SessionSettingsPanel(
          onSelected: choices.add,
          quality: true,
          qualitySummary: '작업 · 60 fps',
          mouseLock: false,
          onPinned: (_) {},
          guide: true,
        ),
        width: SessionSettingsPanel.width,
      );
      // Top to bottom, in the vocabulary's order, each with its one name.
      var last = double.negativeInfinity;
      for (final (key, tool) in const [
        ('remote-stream-settings', SessionTool.settings),
        ('session-game-mouse', SessionTool.gameMouse),
        ('pin-session-toolbar', SessionTool.pin),
        ('remote-session-guide', SessionTool.guide),
      ]) {
        expect(_row(key), findsOneWidget, reason: key);
        final top = tester.getRect(_row(key)).top;
        expect(top, greaterThan(last), reason: key);
        last = top;
        expect(_has(key, tool.label), isTrue, reason: key);
        expect(tester.widget<InkWell>(_row(key)).onTap, isNotNull, reason: key);
      }
      expect(_has('remote-stream-settings', '화질'), isTrue);
      expect(_has('remote-stream-settings', '작업 · 60 fps'), isTrue);
      expect(_has('session-game-mouse', '마우스 잠금'), isTrue);
      expect(_has('pin-session-toolbar', '툴바 열어 두기'), isTrue);
      expect(_has('pin-session-toolbar', '데스크톱 화면을 눌러도 접지 않아요'), isTrue);
      expect(_has('remote-session-guide', '사용안내'), isTrue);
      expect(_has('remote-session-guide', '프린터 · USB · 공유 폴더 · 복사'), isTrue);
      // Switches for what turns on and off, none for what opens.
      expect(_toggled(tester, 'session-game-mouse'), isFalse);
      expect(_toggled(tester, 'pin-session-toolbar'), isFalse);
      for (final key in ['remote-stream-settings', 'remote-session-guide']) {
        expect(
          find.descendant(of: _key(key), matching: find.byType(Switch)),
          findsNothing,
          reason: key,
        );
        expect(
          find.descendant(
            of: _key(key),
            matching: find.byIcon(Icons.chevron_right_rounded),
          ),
          findsOneWidget,
          reason: key,
        );
      }
      // 화질 and 사용안내 return their choices.
      await tester.tap(_row('remote-stream-settings'));
      expect(choices, [SessionSettingsChoice.quality]);
      await tester.tap(_row('remote-session-guide'));
      expect(choices, [
        SessionSettingsChoice.quality,
        SessionSettingsChoice.guide,
      ]);
      expect(tester.takeException(), isNull);
    });

    testWidgets('a row that cannot be used yet is greyed and says why', (
      tester,
    ) async {
      final choices = <SessionSettingsChoice>[];
      await _show(
        tester,
        SessionSettingsPanel(
          onSelected: choices.add,
          quality: true,
          qualitySummary: '작업 · 60 fps',
          qualityReason: '연결된 뒤 바꿀 수 있어요',
          onPinned: (_) {},
          guide: true,
          guideReason: '사용안내가 열려 있어요',
        ),
        width: SessionSettingsPanel.width,
      );
      expect(
        tester.widget<InkWell>(_row('remote-stream-settings')).onTap,
        isNull,
      );
      expect(
        tester.widget<InkWell>(_row('remote-session-guide')).onTap,
        isNull,
      );
      expect(find.byTooltip('화질 · 연결된 뒤 바꿀 수 있어요'), findsOneWidget);
      expect(find.byTooltip('사용안내 · 사용안내가 열려 있어요'), findsOneWidget);
      // 바 열어 두기 is not affected.
      expect(
        tester.widget<InkWell>(_row('pin-session-toolbar')).onTap,
        isNotNull,
      );
      await tester.tap(_row('remote-stream-settings'), warnIfMissed: false);
      await tester.tap(_row('remote-session-guide'), warnIfMissed: false);
      expect(choices, isEmpty);
      // (a tap on a greyed row shows its reason for a moment)
      await tester.pump(const Duration(seconds: 3));
      await tester.pumpAndSettle();
      // Usable again: no reason left.
      await _show(
        tester,
        SessionSettingsPanel(
          onSelected: choices.add,
          quality: true,
          guide: true,
        ),
        width: SessionSettingsPanel.width,
      );
      expect(
        find.byWidgetPredicate(
          (widget) => widget is Tooltip && (widget.message ?? '').isNotEmpty,
        ),
        findsNothing,
      );
      expect(
        tester.widget<InkWell>(_row('remote-stream-settings')).onTap,
        isNotNull,
      );
    });

    testWidgets('마우스 잠금 shows whether it is on, with its keys, and '
        'returns its choice', (tester) async {
      final choices = <SessionSettingsChoice>[];
      SessionSettingsPanel panel({required bool on, bool usb = false}) =>
          SessionSettingsPanel(
            onSelected: choices.add,
            mouseLock: on,
            mouseLockUsb: usb,
          );
      await _show(tester, panel(on: false), width: SessionSettingsPanel.width);
      const lock = 'session-game-mouse';
      expect(_toggled(tester, lock), isFalse);
      expect(_switch(tester, lock).value, isFalse);
      // Ctrl Shift Alt Z as key caps, then what they do.
      for (final key in SessionKeycaps.mouseLock) {
        expect(_has(lock, key), isTrue, reason: key);
      }
      expect(SessionKeycaps.mouseLock, ['Ctrl', 'Shift', 'Alt', 'Z']);
      expect(_has(lock, '로 켜고 꺼요'), isTrue);
      await tester.tap(_row(lock));
      expect(choices, [SessionSettingsChoice.mouseLock]);

      await _show(tester, panel(on: true), width: SessionSettingsPanel.width);
      expect(_toggled(tester, lock), isTrue);
      expect(_switch(tester, lock).value, isTrue);
      expect(_has(lock, '로 꺼요 · 일반 입력'), isTrue);
      expect(_has(lock, '로 켜고 꺼요'), isFalse);

      await _show(
        tester,
        panel(on: true, usb: true),
        width: SessionSettingsPanel.width,
      );
      expect(_toggled(tester, lock), isTrue);
      expect(_has(lock, '로 꺼요 · USB 마우스'), isTrue);
      expect(_has(lock, '로 꺼요 · 일반 입력'), isFalse);
      // The old hover texts are gone.
      expect(find.textContaining('마우스 잠금 끄기'), findsNothing);
      await tester.tap(_row(lock));
      expect(choices, [
        SessionSettingsChoice.mouseLock,
        SessionSettingsChoice.mouseLock,
      ]);
      expect(tester.takeException(), isNull);
    });

    testWidgets('바 열어 두기 switches in place and reports, without closing', (
      tester,
    ) async {
      final choices = <SessionSettingsChoice>[];
      final reported = <bool>[];
      await _show(
        tester,
        SessionSettingsPanel(onSelected: choices.add, onPinned: reported.add),
        width: SessionSettingsPanel.width,
      );
      const pin = 'pin-session-toolbar';
      Finder icon(IconData data) =>
          find.descendant(of: _key(pin), matching: find.byIcon(data));
      expect(_toggled(tester, pin), isFalse);
      expect(_switch(tester, pin).value, isFalse);
      expect(icon(Icons.push_pin_outlined), findsOneWidget);
      await tester.tap(_row(pin));
      await tester.pump();
      expect(reported, [true]);
      expect(_toggled(tester, pin), isTrue);
      expect(_switch(tester, pin).value, isTrue);
      expect(icon(Icons.push_pin), findsOneWidget);
      expect(icon(Icons.push_pin_outlined), findsNothing);
      await tester.tap(_row(pin));
      await tester.pump();
      expect(reported, [true, false]);
      expect(_toggled(tester, pin), isFalse);
      expect(_switch(tester, pin).value, isFalse);
      // It is not a choice that closes the panel.
      expect(choices, isEmpty);
      expect(_key('session-settings-panel'), findsOneWidget);

      // Opened on a bar that is already kept open.
      await tester.pumpWidget(const SizedBox.shrink());
      await _show(
        tester,
        SessionSettingsPanel(
          onSelected: choices.add,
          pinned: true,
          onPinned: reported.add,
        ),
        width: SessionSettingsPanel.width,
      );
      expect(_toggled(tester, pin), isTrue);
      expect(_switch(tester, pin).value, isTrue);
      await tester.tap(_row(pin));
      await tester.pump();
      expect(reported, [true, false, false]);
      expect(_switch(tester, pin).value, isFalse);
      expect(choices, isEmpty);
    });
  });

  group('연결 (SessionConnectionPanel)', () {
    SessionConnectionPanel panel({
      double? latency = 12,
      bool connected = true,
      List<SessionQualityValue>? values,
      VoidCallback? onStats,
      bool Function()? advice,
      VoidCallback? onSaver,
      Listenable? listenable,
      double? Function()? live,
    }) => SessionConnectionPanel(
      title: 'TESTER',
      mode: '스트리밍',
      listenable: listenable,
      latency: live ?? () => latency,
      connected: () => connected,
      values: () => values ?? _values(),
      onStats: onStats ?? () {},
      advice: advice,
      onSaver: onSaver,
    );

    testWidgets('the round trip as a number, its unit and a plain word', (
      tester,
    ) async {
      expect(SessionConnectionPanel.width, 312);
      for (final (ms, number, word) in const [
        (0.4, '<1', '좋음'),
        (12.4, '12', '좋음'),
        (39.4, '39', '좋음'),
        (40.0, '40', '보통'),
        (79.4, '79', '보통'),
        (80.0, '80', '느림'),
        (142.0, '142', '느림'),
      ]) {
        await _show(
          tester,
          panel(latency: ms),
          width: SessionConnectionPanel.width,
        );
        final span = _rich(tester, 'quality-latency');
        expect(span.text, number, reason: '$ms');
        expect((span.children!.single as TextSpan).text, ' ms', reason: '$ms');
        expect(span.toPlainText(), '$number ms', reason: '$ms');
        expect(_has('quality-word', word), isTrue, reason: '$ms');
        for (final other in ['좋음', '보통', '느림']) {
          if (other != word) {
            expect(find.text(other), findsNothing, reason: '$ms $other');
          }
        }
      }
      expect(_key('session-connection-panel'), findsOneWidget);
      // Which desktop, what kind of connection, and what the number is.
      expect(find.text('TESTER'), findsOneWidget);
      expect(find.text('스트리밍'), findsOneWidget);
      expect(find.text('왕복 지연 · Host 응답'), findsOneWidget);
      expect(tester.takeException(), isNull);
    });

    testWidgets('nothing measured is said, never invented', (tester) async {
      Future<String> shown({double? latency, bool connected = true}) async {
        await _show(
          tester,
          panel(latency: latency, connected: connected),
          width: SessionConnectionPanel.width,
        );
        return _rich(tester, 'quality-latency').toPlainText();
      }

      expect(await shown(latency: null), '측정 전');
      expect(_key('quality-word'), findsNothing);
      expect(await shown(latency: 0), '측정 전');
      expect(await shown(latency: double.nan), '측정 전');
      expect(await shown(latency: double.infinity), '측정 전');
      expect(_key('quality-word'), findsNothing);
      // Not connected: waiting, whatever the last sample was.
      expect(await shown(latency: null, connected: false), '연결 대기');
      expect(await shown(latency: 12, connected: false), '연결 대기');
      expect(_key('quality-word'), findsNothing);
      expect(find.textContaining(' ms'), findsNWidgets(2));
      expect(tester.takeException(), isNull);
    });

    testWidgets('three tiles and three lines from the measured values', (
      tester,
    ) async {
      await _show(tester, panel(), width: SessionConnectionPanel.width);
      // 화면 · 수신 · 해상도, side by side.
      final tiles = [
        for (final id in ['fps', 'rate', 'resolution'])
          tester.getRect(_key('quality-value-$id')),
      ];
      expect(tiles[0].top, tiles[1].top);
      expect(tiles[1].top, tiles[2].top);
      expect(tiles[0].right, lessThanOrEqualTo(tiles[1].left));
      expect(tiles[1].right, lessThanOrEqualTo(tiles[2].left));
      expect(_has('quality-value-fps', '화면'), isTrue);
      expect(_has('quality-value-rate', '수신'), isTrue);
      expect(_has('quality-value-resolution', '해상도'), isTrue);
      // A number and its unit; the resolution whole, without spaces.
      var span = _rich(tester, 'quality-value-fps');
      expect(span.text, '60.0');
      expect((span.children!.single as TextSpan).text, ' fps');
      span = _rich(tester, 'quality-value-rate');
      expect(span.text, '12.3');
      expect((span.children!.single as TextSpan).text, ' Mbps');
      span = _rich(tester, 'quality-value-resolution');
      expect(span.toPlainText(), '1920×1080');
      expect(span.children, isEmpty);
      // The path and the two processing times, one under the other, with
      // the labels both platforms use.
      var last = tiles.first.bottom;
      for (final (id, label, value) in const [
        ('path', '영상 경로', 'UDP 직접'),
        ('encode', '인코딩 · Host 처리', '1.25 ms'),
        ('decode', '디코딩 · PC 처리', '2.50 ms'),
      ]) {
        final line = tester.getRect(_key('quality-value-$id'));
        expect(line.top, greaterThanOrEqualTo(last), reason: id);
        last = line.bottom;
        expect(_has('quality-value-$id', label), isTrue, reason: id);
        expect(_has('quality-value-$id', value), isTrue, reason: id);
      }
      // 연결 통계 보기 is the last row.
      expect(
        tester.getRect(_row('quality-stats')).top,
        greaterThanOrEqualTo(last),
      );
      expect(_has('quality-stats', '연결 통계 보기'), isTrue);

      // Unmeasured values stay a dash; a value that was not given at all
      // leaves a dash in its tile and no line.
      await _show(
        tester,
        panel(
          values: [
            (id: 'resolution', label: '해상도 · 원격 화면', value: '—'),
            (id: 'fps', label: '화면 · PC 디코딩', value: '—'),
            (id: 'path', label: '영상 경로', value: '확인 중'),
          ],
        ),
        width: SessionConnectionPanel.width,
      );
      for (final id in ['fps', 'rate', 'resolution']) {
        final dash = _rich(tester, 'quality-value-$id');
        expect(dash.toPlainText(), '—', reason: id);
        expect(dash.children, isEmpty, reason: id);
      }
      expect(_has('quality-value-path', '확인 중'), isTrue);
      expect(_key('quality-value-encode'), findsNothing);
      expect(_key('quality-value-decode'), findsNothing);
      expect(tester.takeException(), isNull);
    });

    testWidgets('the advice shows only while the latency has stayed poor, '
        'and its button only where a reconnect in 절약 is possible', (
      tester,
    ) async {
      var saved = 0;
      const text = '연결이 느려요. 화질을 ‘절약’으로 낮추면 덜 끊겨요.';
      // Not told about the latency at all, or not poor for long.
      for (final advice in <bool Function()?>[null, () => false]) {
        await _show(
          tester,
          panel(latency: 142, advice: advice, onSaver: () => saved++),
          width: SessionConnectionPanel.width,
        );
        expect(_key('quality-advice'), findsNothing);
        expect(_key('quality-saver'), findsNothing);
        expect(find.text(text), findsNothing);
      }
      // Poor for long, but nothing to offer (already 절약, or no reconnect).
      await _show(
        tester,
        panel(latency: 142, advice: () => true),
        width: SessionConnectionPanel.width,
      );
      expect(_key('quality-advice'), findsOneWidget);
      expect(_has('quality-advice', text), isTrue);
      expect(_key('quality-saver'), findsNothing);
      // Poor for long and a reconnect is possible.
      await _show(
        tester,
        panel(latency: 142, advice: () => true, onSaver: () => saved++),
        width: SessionConnectionPanel.width,
      );
      expect(_has('quality-advice', text), isTrue);
      expect(
        find.descendant(
          of: _key('quality-advice'),
          matching: _key('quality-saver'),
        ),
        findsOneWidget,
      );
      expect(_has('quality-saver', '절약으로 다시 연결'), isTrue);
      // Between the values and 연결 통계 보기.
      final box = tester.getRect(_key('quality-advice'));
      expect(
        tester.getRect(_key('quality-value-decode')).bottom,
        lessThanOrEqualTo(box.top),
      );
      expect(
        box.bottom,
        lessThanOrEqualTo(tester.getRect(_row('quality-stats')).top),
      );
      expect(saved, 0);
      await tester.tap(_key('quality-saver'));
      expect(saved, 1);
      expect(tester.takeException(), isNull);
    });

    testWidgets('연결 통계 보기 calls onStats', (tester) async {
      var stats = 0;
      await _show(
        tester,
        panel(onStats: () => stats++),
        width: SessionConnectionPanel.width,
      );
      expect(_row('quality-stats'), findsOneWidget);
      await tester.tap(_row('quality-stats'));
      expect(stats, 1);
    });

    testWidgets('it follows its session live', (tester) async {
      final latency = ValueNotifier<double?>(null);
      addTearDown(latency.dispose);
      var poor = false;
      await _show(
        tester,
        panel(
          listenable: latency,
          live: () => latency.value,
          advice: () => poor,
        ),
        width: SessionConnectionPanel.width,
      );
      expect(_rich(tester, 'quality-latency').toPlainText(), '측정 전');
      latency.value = 12;
      await tester.pump();
      expect(_rich(tester, 'quality-latency').toPlainText(), '12 ms');
      expect(_has('quality-word', '좋음'), isTrue);
      expect(_key('quality-advice'), findsNothing);
      poor = true;
      latency.value = 142;
      await tester.pump();
      expect(_rich(tester, 'quality-latency').toPlainText(), '142 ms');
      expect(_has('quality-word', '느림'), isTrue);
      expect(_key('quality-advice'), findsOneWidget);
    });
  });

  group('showSessionPanel', () {
    Future<BuildContext> host(WidgetTester tester, Size size) async {
      tester.view.devicePixelRatio = 1;
      tester.view.physicalSize = size;
      addTearDown(tester.view.resetPhysicalSize);
      addTearDown(tester.view.resetDevicePixelRatio);
      late BuildContext context;
      await tester.pumpWidget(
        MaterialApp(
          theme: buildAppTheme(brightness: Brightness.light),
          home: Scaffold(
            body: Builder(
              builder: (value) {
                context = value;
                return const SizedBox.expand();
              },
            ),
          ),
        ),
      );
      return context;
    }

    Future<int?> open(
      BuildContext context, {
      required Rect? anchor,
      SessionPopups? popups,
    }) => showSessionPanel<int>(
      context,
      anchor: anchor,
      width: 316,
      minHeight: 250,
      label: '화면',
      popups: popups,
      builder: (context) => TextButton(
        key: const Key('probe'),
        onPressed: () => Navigator.pop(context, 7),
        child: const Text('probe'),
      ),
    );

    testWidgets('drops from its anchor where it fits, always dark', (
      tester,
    ) async {
      final context = await host(tester, const Size(800, 600));
      const anchor = Rect.fromLTWH(380, 0, 60, 44);
      final result = open(context, anchor: anchor);
      await tester.pumpAndSettle();
      expect(_key('session-panel-dialog'), findsNothing);
      final panel = tester.getRect(_key('anchored-panel'));
      expect(panel.width, 316);
      expect(panel.top, greaterThanOrEqualTo(anchor.bottom));
      expect(panel.top - anchor.bottom, lessThan(24));
      expect((panel.center.dx - anchor.center.dx).abs(), lessThan(1));
      // Dark like the bar, in a light app.
      expect(
        Theme.of(tester.element(_key('probe'))).brightness,
        Brightness.dark,
      );
      await tester.tap(_key('probe'));
      await tester.pumpAndSettle();
      expect(await result, 7);
      expect(_key('anchored-panel'), findsNothing);
    });

    for (final (name, size, anchor) in const <(String, Size, Rect?)>[
      ('a window too narrow', Size(320, 600), Rect.fromLTWH(130, 0, 60, 44)),
      (
        'too little room below',
        Size(800, 600),
        Rect.fromLTWH(380, 320, 60, 44),
      ),
      ('no anchor', Size(800, 600), null),
    ]) {
      testWidgets('$name: the same content as a dark dialog', (tester) async {
        final context = await host(tester, size);
        final result = open(context, anchor: anchor);
        await tester.pumpAndSettle();
        expect(_key('anchored-panel'), findsNothing);
        expect(_key('session-panel-dialog'), findsOneWidget);
        expect(
          find.descendant(
            of: _key('session-panel-dialog'),
            matching: _key('probe'),
          ),
          findsOneWidget,
        );
        expect(
          Theme.of(tester.element(_key('probe'))).brightness,
          Brightness.dark,
        );
        await tester.tap(_key('probe'));
        await tester.pumpAndSettle();
        expect(await result, 7);
        expect(_key('session-panel-dialog'), findsNothing);
        expect(tester.takeException(), isNull);
      });
    }

    testWidgets('the bar\'s popups close it, as a panel or as a dialog, and '
        'it then applies nothing', (tester) async {
      for (final anchor in const <Rect?>[Rect.fromLTWH(380, 0, 60, 44), null]) {
        final context = await host(tester, const Size(800, 600));
        final popups = SessionPopups();
        final result = open(context, anchor: anchor, popups: popups);
        await tester.pumpAndSettle();
        expect(_key('probe'), findsOneWidget, reason: '$anchor');
        expect(popups.isEmpty, isFalse, reason: '$anchor');
        popups.closeAll();
        await tester.pumpAndSettle();
        expect(await result, isNull, reason: '$anchor');
        expect(popups.isEmpty, isTrue, reason: '$anchor');
        expect(_key('probe'), findsNothing, reason: '$anchor');
        await tester.pumpWidget(const SizedBox.shrink());
      }
    });
  });
  // Review 2026-11-12 (en-dark-800x2 sheets 08-10): English labels at 200 %
  // text were split or cut: "Window/ed", "All monitor", "Scre", "Rece",
  // "Resolut", "Shared… 2", "USB de… 1".
  group('English at large text', () {
    Future<void> show(WidgetTester tester, Widget panel, double width) async {
      AppLanguageController.instance.debugReset(AppLanguage.en);
      tester.view.devicePixelRatio = 1;
      tester.view.physicalSize = const Size(900, 1400);
      addTearDown(tester.view.resetPhysicalSize);
      addTearDown(tester.view.resetDevicePixelRatio);
      await tester.pumpWidget(
        MaterialApp(
          theme: buildAppTheme(brightness: Brightness.dark),
          home: MediaQuery(
            data: const MediaQueryData(
              size: Size(900, 1400),
              textScaler: TextScaler.linear(2),
            ),
            child: Scaffold(
              body: Align(
                alignment: Alignment.topCenter,
                child: SizedBox(width: width, child: panel),
              ),
            ),
          ),
        ),
      );
      await tester.pump();
      await tester.pump();
    }

    testWidgets('the screen mode tiles keep whole words', (tester) async {
      await show(
        tester,
        SessionScreenPanel(
          modes: SessionDisplayMode.values,
          shown: SessionDisplayMode.windowed,
          onSelected: (_) {},
        ),
        SessionScreenPanel.widthFor(3),
      );
      for (final (key, label, words) in const [
        ('stream-windowed', 'Windowed', 1),
        ('stream-fullscreen', 'Full screen', 2),
        ('stream-all-monitors', 'All monitors', 2),
      ]) {
        final text = _laidOut(tester, _key(key), label);
        expect(text.cut, isFalse, reason: label);
        expect(text.lines, lessThanOrEqualTo(words), reason: label);
      }
      // The note beside 배율 wraps instead of ending in "…".
      expect(
        _laidOut(
          tester,
          _key('remote-display-scale'),
          'Windows text and icon size',
        ).cut,
        isFalse,
      );
      expect(tester.takeException(), isNull);
    });

    testWidgets('the 폴더·USB tabs show their whole names', (tester) async {
      final drive = _Drive(), usb = _Usb();
      drive.folder('a', DriveShareState.mapped).letter = 'Z';
      usb.device('stick', UsbEntryState.shared);
      await show(
        tester,
        SessionSharePanel(drive: drive, usb: usb),
        SessionSharePanel.width,
      );
      expect(_key('remote-drives-count'), findsOneWidget);
      expect(_key('remote-usb-count'), findsOneWidget);
      for (final (key, label) in const [
        ('remote-drives', 'Shared folder'),
        ('remote-usb', 'USB devices'),
      ]) {
        final text = _laidOut(tester, _key(key), label);
        expect(text.cut, isFalse, reason: label);
        expect(text.lines, 1, reason: label);
      }
      expect(tester.takeException(), isNull);
      await tester.pumpWidget(const SizedBox.shrink());
    });

    testWidgets('the connection tiles show their whole labels', (tester) async {
      await show(
        tester,
        SessionConnectionPanel(
          title: 'TESTER',
          mode: 'Streaming',
          latency: () => 12,
          connected: () => true,
          values: () => const [
            (id: 'resolution', label: 'Resolution', value: '1280×720'),
            (id: 'fps', label: 'Screen', value: '60.0 fps'),
            (id: 'rate', label: 'Received', value: '12.3 Mbps'),
          ],
          onStats: () {},
        ),
        SessionConnectionPanel.width,
      );
      for (final (id, label) in const [
        ('fps', 'Screen'),
        ('rate', 'Received'),
        ('resolution', 'Resolution'),
      ]) {
        final text = _laidOut(tester, _key('quality-value-$id'), label);
        expect(text.cut, isFalse, reason: label);
        expect(text.lines, 1, reason: label);
      }
      expect(tester.takeException(), isNull);
    });
  });

  // Review 2026-11-12: the open panel kept the ended channel's object and
  // stayed "disconnected" after the session had swapped in a new one.
  group('폴더·USB follows the session (SessionShareFollower)', () {
    testWidgets('a channel the session replaced is the one shown', (
      tester,
    ) async {
      final session = ValueNotifier(0);
      final ended = _Drive()
        ..closed = true
        ..error = 'drive_transport_closed'
        ..recovering = true;
      final fresh = _Drive();
      DriveSession current = ended;
      final built = <DriveSession?>[];
      await _show(
        tester,
        SessionShareFollower(
          session: session,
          drive: () => current,
          usb: () => null,
          builder: (drive, usb) {
            built.add(drive);
            return SessionSharePanel(drive: drive, usb: usb);
          },
        ),
        width: SessionSharePanel.width,
      );
      expect(find.text('공유 폴더를 다시 연결하고 있어요. 잠시만 기다려 주세요.'), findsOneWidget);
      expect(
        tester.widget<OutlinedButton>(_key('drive-add')).onPressed,
        isNull,
      );
      // The session notifies for many things: the same objects build nothing.
      final builds = built.length;
      session.value++;
      await tester.pump();
      expect(built.length, builds);
      // The channel came back as a new object.
      current = fresh;
      session.value++;
      await tester.pump();
      await tester.pump();
      expect(built.last, same(fresh));
      expect(find.textContaining('다시 연결하고 있어요'), findsNothing);
      expect(
        tester.widget<OutlinedButton>(_key('drive-add')).onPressed,
        isNotNull,
      );
      expect(tester.takeException(), isNull);
      await tester.pumpWidget(const SizedBox.shrink());
    });
  });
}
