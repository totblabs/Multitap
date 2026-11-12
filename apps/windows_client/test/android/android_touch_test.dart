// The Android finger gesture model (ToTBDesk's tuned model) against the
// existing pointer messages and a substitute for real touch contacts.
// Simulated pointer sequences with a fake clock: no device, IME or host.
import 'dart:math' as math;

import 'package:flutter/widgets.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:multitap_client/android/android_picture.dart';
import 'package:multitap_client/android/android_touch.dart';
import 'package:multitap_client/android/android_touch_feedback.dart';

const _feedback = Key('feedback');

/// What the feedback overlay draws now: circle centres and arc sweeps.
({List<Offset> circles, List<double> arcs}) _drawn(WidgetTester tester) {
  final canvas = TestRecordingCanvas();
  tester
      .renderObject(find.byKey(_feedback))
      .paint(TestRecordingPaintingContext(canvas), Offset.zero);
  final circles = <Offset>{}, arcs = <double>[];
  for (final call in canvas.invocations) {
    final args = call.invocation.positionalArguments;
    if (call.invocation.memberName == #drawCircle) {
      circles.add(args[0] as Offset);
    } else if (call.invocation.memberName == #drawArc) {
      arcs.add(args[2] as double);
    }
  }
  return (circles: circles.toList(), arcs: arcs);
}

class _Rig {
  _Rig({
    AndroidTouchMode mode = AndroidTouchMode.direct,
    Rect? visible,
    AndroidTouchContacts? contacts,
    bool Function()? follow,
  }) {
    view = AndroidPictureView.fit(
      const Rect.fromLTWH(0, 0, 400, 800),
      const Size(400, 800),
    );
    this.visible = visible ?? view.area;
    input = AndroidTouchInput(
      view: () => view,
      visible: () => this.visible,
      onView: (next) => view = next,
      send: sent.add,
      onCue: (cue, at) {
        cues.add(cue);
        points.add((cue, at));
        counters.cue(cue);
      },
      mode: mode,
      source: () => source,
      contacts: contacts == null ? null : () => contacts,
      follow: follow,
    );
  }
  late AndroidPictureView view;
  Size? source;
  late Rect visible;
  late final AndroidTouchInput input;
  final sent = <Map<String, dynamic>>[];
  final cues = <AndroidTouchCue>[];
  final points = <(AndroidTouchCue, Offset?)>[];
  final counters = AndroidInputCounters();
  Duration now = Duration.zero;

  void down(int id, Offset at, {int ms = 0}) {
    now += Duration(milliseconds: ms);
    input.down(id, at, now);
  }

  void move(int id, Offset at, {int ms = 16}) {
    now += Duration(milliseconds: ms);
    input.move(id, at, now);
  }

  void up(int id, Offset at, {int ms = 16}) {
    now += Duration(milliseconds: ms);
    input.up(id, at, now);
  }

  /// One finger: down, a path of moves, up.
  void stroke(int id, List<Offset> path, {int ms = 16}) {
    down(id, path.first);
    for (final point in path.skip(1)) {
      move(id, point, ms: ms);
    }
    up(id, path.last, ms: ms);
  }

  List<String> get kinds => [
    for (final e in sent)
      e['event'] == 'button'
          ? '${e['button']}:${e['down'] ? 'down' : 'up'}'
          : e['event'] as String,
  ];
  List<Map<String, dynamic>> get buttons => [
    for (final e in sent)
      if (e['event'] == 'button') e,
  ];
  int get held {
    var down = 0;
    for (final e in buttons) {
      down += e['down'] == true ? 1 : -1;
    }
    return down;
  }
}

void main() {
  test('picture view: fit, normalize and the keyboard-limited pan', () {
    // Positions snap to device pixels (2 per dp here: 287.5 is one).
    final view = AndroidPictureView.fit(
      const Rect.fromLTWH(0, 0, 400, 800),
      const Size(1600, 900),
      devicePixelRatio: 2,
    );
    expect(view.rect, const Rect.fromLTWH(0, 287.5, 400, 225));
    expect(view.normalize(const Offset(200, 400)), const Offset(.5, .5));
    expect(view.normalize(const Offset(200, 100)), isNull);
    const keyboard = Rect.fromLTWH(0, 0, 400, 450);
    expect(view.movable(view.area), false);
    // A picture that fits above the keyboard may go up as far as centred
    // there (ToTBDesk's keyboard camera), and back down to its rest.
    final up = view.panned(const Offset(0, -500), visible: keyboard);
    expect(up.rect.center.dy, 225);
    final down = up.panned(const Offset(0, 900), visible: keyboard);
    expect(down.rect.top, 287.5);
    final zoomed = view.zoomTo(2, const Offset(.5, .5), const Offset(200, 400));
    expect(zoomed.scale, 2);
    expect(zoomed.rect.left, -200);
    expect(zoomed.panned(const Offset(900, 0)).rect.left, 0);
    expect(zoomed.panned(const Offset(-900, 0)).rect.right, 400);
    expect(zoomed.reset().rect, view.rect);
    final tall = AndroidPictureView.fit(
      const Rect.fromLTWH(0, 0, 400, 800),
      const Size(400, 800),
    );
    final shown = tall.reveal(const Offset(.5, .9), keyboard, margin: 40);
    expect(shown.point(const Offset(.5, .9)).dy, lessThanOrEqualTo(410));
  });

  test('picture view: whole device pixels per remote pixel, pixel-snapped', () {
    const area = Rect.fromLTWH(0, 0, 360, 800);
    // 540×1200 remote pixels at 2 device pixels each on a 3x screen.
    final exact = AndroidPictureView.fit(
      area,
      const Size(540, 1200),
      devicePixelRatio: 3,
      pixelScale: 2,
    );
    expect(exact.rect, const Rect.fromLTWH(0, 0, 360, 800));
    expect(exact.wholePixelScale(const Size(540, 1200), 3), 2);
    // The device's own pixels, centred on a whole device pixel.
    final native = AndroidPictureView.fit(
      const Rect.fromLTWH(0, 0, 360, 801),
      const Size(1080, 2400),
      devicePixelRatio: 3,
      pixelScale: 1,
    );
    expect(native.wholePixelScale(const Size(1080, 2400), 3), 1);
    expect((native.rect.top * 3) % 1, 0);
    // Too large for the exact scale (an older picture kept during a
    // rotation): contained, filtered.
    final kept = AndroidPictureView.fit(
      const Rect.fromLTWH(0, 0, 800, 360),
      const Size(540, 1200),
      devicePixelRatio: 3,
      pixelScale: 2,
    );
    expect(kept.rect.height, 360);
    expect(kept.rect.width, closeTo(162, .01));
    expect(kept.wholePixelScale(const Size(540, 1200), 3), isNull);
    // A picture the encoder sent smaller than this screen (Fold 8 unfolded:
    // 1248×942 for a 2448×1848 output) fills the screen instead of being
    // shown 1:1 in its middle.
    final smaller = AndroidPictureView.fit(
      const Rect.fromLTWH(0, 0, 932.6, 704),
      const Size(1248, 942),
      devicePixelRatio: 2.625,
      pixelScale: 1,
    );
    expect(smaller.rect.width, closeTo(932.6, .5));
    // Zoomed: never drawn without smoothing.
    final zoomed = exact.zoomTo(2, const Offset(.5, .5), const Offset(180, 400));
    expect(zoomed.wholePixelScale(const Size(540, 1200), 3), isNull);
    expect(zoomed.reset().wholePixelScale(const Size(540, 1200), 3), 2);
  });

  test('a resize keeps the previous picture at its own size until settled', () {
    final pictures = AndroidPictureContinuity();
    const portrait = Size(540, 1200), landscape = Size(2400, 1080);
    expect(
      pictures.update(texture: 1, size: portrait, settled: true),
      const AndroidShownPicture(1, portrait),
    );
    // The session announces the new size before its texture exists...
    expect(
      pictures.update(texture: 1, size: landscape, settled: false),
      const AndroidShownPicture(1, portrait),
      reason: 'the old texture keeps its own size, never stretched',
    );
    // ...stops the old decoder (no texture)...
    expect(
      pictures.update(texture: null, size: landscape, settled: false),
      const AndroidShownPicture(1, portrait),
    );
    // ...and starts the new one, which has no frame until the resize ends.
    expect(
      pictures.update(texture: 2, size: landscape, settled: false),
      const AndroidShownPicture(1, portrait),
    );
    expect(
      pictures.update(texture: 2, size: landscape, settled: true),
      const AndroidShownPicture(2, landscape),
    );
    pictures.reset();
    expect(pictures.update(texture: null, size: landscape, settled: true), isNull);
    // A first picture is shown at once.
    expect(
      pictures.update(texture: 3, size: landscape, settled: false),
      const AndroidShownPicture(3, landscape),
    );
  });
  // --- 포인터 (trackpad), the default -------------------------------------

  testWidgets('pointer: a finger moves the cursor; a tap clicks there after '
      'the double-tap window, felt at once', (tester) async {
    final rig = _Rig(mode: AndroidTouchMode.trackpad);
    rig.stroke(1, const [
      Offset(300, 700),
      Offset(320, 700),
      Offset(340, 700),
    ], ms: 50);
    expect(rig.kinds, ['move', 'move']);
    // The cursor continues from the slop edge: 40 - 8 px of 400 at 1x.
    expect(rig.input.cursor.dx, closeTo(.58, .005));
    rig.down(2, const Offset(10, 10));
    rig.up(2, const Offset(10, 10));
    expect(rig.cues.last, AndroidTouchCue.tap, reason: 'felt at once');
    expect(rig.buttons, isEmpty, reason: 'waiting for a possible second tap');
    expect(rig.input.clickPending, isTrue);
    await tester.pump(const Duration(milliseconds: 320));
    expect(rig.kinds.skip(2), ['left:down', 'left:up']);
    expect(rig.buttons.first['x'], closeTo(.58, .005));
  });

  testWidgets('pointer: a jittering tap clicks where it began', (tester) async {
    final rig = _Rig(mode: AndroidTouchMode.trackpad);
    rig.down(1, const Offset(200, 400));
    rig.move(1, const Offset(204, 403));
    rig.up(1, const Offset(204, 403));
    await tester.pump(const Duration(milliseconds: 320));
    expect(rig.kinds, ['left:down', 'left:up']);
    expect((rig.buttons.first['x'], rig.buttons.first['y']), (.5, .5));
  });

  testWidgets('pointer: tap then press and hold or move drags; the first '
      'click is never sent', (tester) async {
    final rig = _Rig(mode: AndroidTouchMode.trackpad);
    rig.down(1, const Offset(200, 400));
    rig.up(1, const Offset(200, 400));
    rig.down(2, const Offset(210, 405), ms: 120);
    rig.move(2, const Offset(250, 405));
    rig.move(2, const Offset(300, 405));
    rig.up(2, const Offset(300, 405));
    expect(rig.kinds.first, 'left:down', reason: 'no click before the drag');
    expect(rig.kinds.where((k) => k == 'left:down'), hasLength(1));
    expect(rig.kinds.last, 'left:up');
    expect(rig.held, 0);
    // Continues from the slop edge, no jump.
    expect(rig.input.cursor.dx, closeTo(.5 + (90 - 8) / 400, .01));
    expect(rig.cues, contains(AndroidTouchCue.dragStart));
    expect(rig.cues, isNot(contains(AndroidTouchCue.hold)), reason: 'no buzz mid-swipe');
    await tester.pump(const Duration(seconds: 1));
    expect(rig.kinds.where((k) => k == 'left:down'), hasLength(1));

    // Held still past the window: the drag starts with a buzz.
    final hold = _Rig(mode: AndroidTouchMode.trackpad);
    hold.down(1, const Offset(200, 400));
    hold.up(1, const Offset(200, 400));
    hold.down(2, const Offset(200, 400), ms: 100);
    await tester.pump(const Duration(milliseconds: 320));
    expect(hold.kinds, ['left:down']);
    expect(hold.cues, contains(AndroidTouchCue.hold));
    hold.move(2, const Offset(260, 400), ms: 320);
    hold.up(2, const Offset(260, 400));
    expect(hold.kinds.last, 'left:up');
    expect(hold.held, 0);
  });

  testWidgets('pointer: a quick double tap is a double click', (tester) async {
    final rig = _Rig(mode: AndroidTouchMode.trackpad);
    rig.down(1, const Offset(200, 400));
    rig.up(1, const Offset(200, 400));
    rig.down(2, const Offset(205, 402), ms: 120);
    rig.up(2, const Offset(205, 402), ms: 60);
    expect(rig.kinds, ['left:down', 'left:up', 'left:down', 'left:up']);
    await tester.pump(const Duration(seconds: 1));
    expect(rig.kinds, hasLength(4), reason: 'no third click');
  });

  testWidgets('pointer: long press right-clicks at once; moving then picks a '
      'menu item without a click', (tester) async {
    final rig = _Rig(mode: AndroidTouchMode.trackpad);
    rig.down(1, const Offset(200, 400));
    await tester.pump(const Duration(milliseconds: 550));
    expect(rig.kinds, ['right:down', 'right:up']);
    expect(rig.cues, contains(AndroidTouchCue.rightClick));
    rig.move(1, const Offset(240, 440), ms: 550);
    rig.move(1, const Offset(260, 460));
    rig.up(1, const Offset(260, 460));
    expect(rig.kinds.skip(2).every((k) => k == 'move'), isTrue);
    await tester.pump(const Duration(seconds: 1));
    expect(rig.buttons, hasLength(2));
  });

  testWidgets('pointer speed: 정밀 is finer, 빠름 accelerates quick movement', (
    tester,
  ) async {
    double travel(AndroidPointerSpeed speed, int ms) {
      final rig = _Rig(mode: AndroidTouchMode.trackpad)
        ..input.speed = speed;
      rig.stroke(1, const [
        Offset(100, 700),
        Offset(140, 700),
        Offset(180, 700),
      ], ms: ms);
      return rig.input.cursor.dx - .5;
    }

    final standard = travel(AndroidPointerSpeed.standard, 200);
    expect(standard, closeTo((80 - 8) / 400, .005));
    expect(travel(AndroidPointerSpeed.standard, 8), closeTo(standard, .005));
    expect(
      travel(AndroidPointerSpeed.precise, 200),
      closeTo(standard * .75, .005),
    );
    expect(
      travel(AndroidPointerSpeed.fast, 200),
      closeTo(standard * 1.5, .005),
      reason: 'slow movement is not accelerated',
    );
    expect(travel(AndroidPointerSpeed.fast, 8), greaterThan(standard * 1.5));
  });

  // --- 터치 (direct) without real touch: mouse at the finger ----------------

  testWidgets('touch: a tap clicks where the finger is, after the window', (
    tester,
  ) async {
    final rig = _Rig();
    rig.down(1, const Offset(100, 200));
    rig.up(1, const Offset(103, 202));
    expect(rig.kinds, ['move']);
    await tester.pump(const Duration(milliseconds: 320));
    expect(rig.kinds, ['move', 'left:down', 'left:up']);
    expect(rig.buttons.first['x'], .25);
    expect(rig.buttons.first['y'], .25);
  });

  testWidgets('touch: a second tap lands on the first (double click)', (
    tester,
  ) async {
    final rig = _Rig();
    rig.down(1, const Offset(100, 100));
    rig.up(1, const Offset(100, 100));
    rig.down(2, const Offset(110, 106), ms: 100);
    rig.up(2, const Offset(110, 106), ms: 60);
    final clicks = rig.buttons;
    expect(clicks, hasLength(4));
    expect(clicks.every((e) => e['x'] == .25 && e['y'] == .125), isTrue);
  });

  testWidgets('touch: a one-finger swipe scrolls (never selects text)', (
    tester,
  ) async {
    final rig = _Rig()..source = const Size(1600, 3200);
    rig.down(1, const Offset(200, 600));
    for (var y = 580.0; y >= 400; y -= 20) {
      rig.move(1, Offset(200, y), ms: 60);
    }
    rig.up(1, const Offset(200, 400), ms: 60);
    final wheels = rig.sent.where((e) => e['event'] == 'wheel').toList();
    expect(wheels, isNotEmpty);
    expect(wheels.every((e) => (e['delta'] as int) < 0), isTrue);
    // 60 remote px are 15 px on screen here (one notch): about
    // (200 - 8) / 15 notches, the page following the finger.
    final total = wheels.fold<int>(0, (s, e) => s + (e['delta'] as int));
    expect(total.abs(), closeTo((192 / 15) * 120, 30));
    expect(rig.buttons, isEmpty);
    // A quick swipe glides on briefly.
    await tester.pump(const Duration(seconds: 2));
    expect(rig.input.flinging, isFalse);
  });

  testWidgets('touch: double tap and hold or move drags from the second press', (
    tester,
  ) async {
    final rig = _Rig();
    rig.down(1, const Offset(100, 200));
    rig.up(1, const Offset(100, 200));
    rig.down(2, const Offset(104, 204), ms: 100);
    rig.move(2, const Offset(150, 260));
    rig.move(2, const Offset(250, 360));
    rig.up(2, const Offset(250, 360));
    final buttons = rig.buttons;
    expect(buttons.map((e) => '${e['button']}:${e['down']}'), [
      'left:true',
      'left:false',
    ]);
    expect(buttons.first['x'], closeTo(.26, .001));
    expect(buttons.last['x'], closeTo(.625, .001));
    expect(rig.held, 0);
  });

  testWidgets('touch: long press right-clicks at the finger', (tester) async {
    final rig = _Rig();
    rig.down(1, const Offset(200, 400));
    await tester.pump(const Duration(milliseconds: 550));
    rig.up(1, const Offset(204, 402), ms: 550);
    expect(rig.kinds, ['move', 'right:down', 'right:up']);
    expect(rig.buttons.first['x'], .5);
  });

  testWidgets('a press outside the picture never presses a button', (
    tester,
  ) async {
    final rig = _Rig();
    rig.view = AndroidPictureView.fit(
      const Rect.fromLTWH(0, 0, 400, 800),
      const Size(1600, 900),
      devicePixelRatio: 2,
    );
    rig.down(1, const Offset(200, 50));
    rig.move(1, const Offset(200, 120));
    rig.up(1, const Offset(200, 120));
    await tester.pump(const Duration(seconds: 1));
    expect(rig.sent, isEmpty);
  });

  // --- both modes --------------------------------------------------------------

  testWidgets('two-finger tap right-clicks; two fingers moving scroll, also '
      'zoomed', (tester) async {
    for (final mode in AndroidTouchMode.values) {
      final rig = _Rig(mode: mode);
      rig.down(1, const Offset(100, 400));
      rig.down(2, const Offset(200, 400));
      rig.up(1, const Offset(100, 400), ms: 80);
      rig.up(2, const Offset(200, 400), ms: 10);
      expect(rig.buttons.map((e) => '${e['button']}:${e['down']}'), [
        'right:true',
        'right:false',
      ], reason: mode.name);
      expect(rig.cues, contains(AndroidTouchCue.rightClick));

      rig.sent.clear();
      rig.view = rig.view.zoomTo(2, const Offset(.5, .5), const Offset(200, 400));
      rig.down(3, const Offset(100, 400));
      rig.down(4, const Offset(200, 400));
      for (var y = 390.0; y >= 300; y -= 10) {
        rig.move(3, Offset(100, y), ms: 40);
        rig.move(4, Offset(200, y), ms: 1);
      }
      rig.up(3, const Offset(100, 300), ms: 200);
      rig.up(4, const Offset(200, 300));
      final wheels = rig.sent.where((e) => e['event'] == 'wheel');
      expect(wheels, isNotEmpty, reason: mode.name);
      expect(wheels.every((e) => (e['delta'] as int) < 0), isTrue);
      expect(rig.buttons, isEmpty);
    }
  });

  testWidgets('pinch zooms this picture only, never below the whole picture', (
    tester,
  ) async {
    final rig = _Rig();
    rig.down(1, const Offset(150, 400));
    rig.down(2, const Offset(250, 400));
    rig.move(1, const Offset(100, 400));
    rig.move(2, const Offset(300, 400));
    rig.up(1, const Offset(100, 400));
    rig.up(2, const Offset(300, 400));
    expect(rig.view.scale, closeTo(2, .01));
    expect(rig.view.normalize(const Offset(200, 400))!.dx, closeTo(.5, .01));
    expect(rig.buttons, isEmpty);
    expect(rig.cues, contains(AndroidTouchCue.pinch));
    rig.down(3, const Offset(100, 400));
    rig.down(4, const Offset(300, 400));
    rig.move(3, const Offset(190, 400));
    rig.move(4, const Offset(210, 400));
    rig.up(3, const Offset(190, 400));
    rig.up(4, const Offset(210, 400));
    expect(rig.view.scale, 1);
  });

  testWidgets('a two-finger flick keeps scrolling, slows down and stops at '
      'a touch', (tester) async {
    final rig = _Rig();
    rig.down(1, const Offset(100, 500));
    rig.down(2, const Offset(200, 500));
    for (var y = 480.0; y >= 380; y -= 20) {
      rig.move(1, Offset(100, y), ms: 8);
      rig.move(2, Offset(200, y), ms: 1);
    }
    rig.up(1, const Offset(100, 380), ms: 8);
    rig.up(2, const Offset(200, 380), ms: 1);
    final lifted = rig.sent.where((e) => e['event'] == 'wheel').length;
    expect(rig.input.flinging, isTrue);
    await tester.pump(const Duration(milliseconds: 200));
    final glided = rig.sent.where((e) => e['event'] == 'wheel').toList();
    expect(glided.length, greaterThan(lifted));
    expect(glided.every((e) => (e['delta'] as int) < 0), isTrue);
    rig.down(3, const Offset(200, 400));
    int wheelCount() => rig.sent.where((e) => e['event'] == 'wheel').length;
    final stopped = wheelCount();
    await tester.pump(const Duration(seconds: 1));
    expect(wheelCount(), stopped);
    expect(rig.input.flinging, isFalse);
    rig.up(3, const Offset(200, 400));
    await tester.pump(const Duration(seconds: 1));
  });

  testWidgets('three-finger tap asks for the keyboard; moving fingers do not', (
    tester,
  ) async {
    final rig = _Rig();
    rig.down(1, const Offset(100, 400));
    rig.down(2, const Offset(200, 400));
    rig.down(3, const Offset(300, 400));
    rig.up(1, const Offset(100, 400));
    rig.up(2, const Offset(200, 400));
    rig.up(3, const Offset(300, 400));
    expect(rig.cues, contains(AndroidTouchCue.keyboard));
    expect(rig.buttons, isEmpty);
    final moved = _Rig();
    moved.down(1, const Offset(100, 400));
    moved.down(2, const Offset(200, 400));
    moved.down(3, const Offset(300, 400));
    moved.move(1, const Offset(100, 460));
    moved.up(1, const Offset(100, 460));
    moved.up(2, const Offset(200, 400));
    moved.up(3, const Offset(300, 400));
    expect(moved.cues, isNot(contains(AndroidTouchCue.keyboard)));
  });

  testWidgets('four-finger tap switches the mode, not the keyboard', (
    tester,
  ) async {
    final rig = _Rig();
    for (var i = 1; i <= 4; i++) {
      rig.down(i, Offset(80.0 * i, 400));
    }
    for (var i = 1; i <= 4; i++) {
      rig.up(i, Offset(80.0 * i, 400));
    }
    expect(rig.cues, contains(AndroidTouchCue.touchMode));
    expect(rig.cues, isNot(contains(AndroidTouchCue.keyboard)));
    expect(rig.buttons, isEmpty);
    final moved = _Rig();
    for (var i = 1; i <= 4; i++) {
      moved.down(i, Offset(80.0 * i, 400));
    }
    moved.move(4, const Offset(320, 470));
    for (var i = 1; i <= 4; i++) {
      moved.up(i, Offset(80.0 * i, i == 4 ? 470 : 400));
    }
    expect(moved.cues, isNot(contains(AndroidTouchCue.touchMode)));
    expect(moved.cues, isNot(contains(AndroidTouchCue.keyboard)));
    final five = _Rig();
    for (var i = 1; i <= 5; i++) {
      five.down(i, Offset(60.0 * i, 400));
    }
    for (var i = 1; i <= 5; i++) {
      five.up(i, Offset(60.0 * i, 400));
    }
    expect(five.cues, isNot(contains(AndroidTouchCue.touchMode)));
    expect(five.cues, isNot(contains(AndroidTouchCue.keyboard)));
  });

  testWidgets('a waiting click goes first before other input; a reset drops it', (
    tester,
  ) async {
    final rig = _Rig(mode: AndroidTouchMode.trackpad);
    rig.down(1, const Offset(200, 400));
    rig.up(1, const Offset(200, 400));
    rig.input.flushPendingClick();
    expect(rig.kinds, ['left:down', 'left:up']);
    rig.down(2, const Offset(10, 10), ms: 500);
    rig.up(2, const Offset(10, 10));
    rig.input.reset();
    await tester.pump(const Duration(seconds: 1));
    expect(rig.kinds, hasLength(2));
  });

  testWidgets('real touch: contacts go to the session, no mouse at all', (
    tester,
  ) async {
    final contacts = _Contacts();
    final rig = _Rig(contacts: contacts);
    rig.down(1, const Offset(100, 200));
    rig.down(2, const Offset(300, 600));
    rig.move(1, const Offset(120, 220));
    rig.up(2, const Offset(300, 600));
    rig.up(1, const Offset(120, 220));
    await tester.pump(const Duration(seconds: 1));
    expect(rig.sent, isEmpty);
    expect(contacts.log, [
      '1:down:0.25,0.25',
      '2:down:0.75,0.75',
      '1:move:0.3,0.275',
      '2:up:0.75,0.75',
      '1:up:0.3,0.275',
    ]);
    // Unavailable (no grant): the mouse emulation instead.
    contacts.available = false;
    rig.down(3, const Offset(100, 200));
    rig.up(3, const Offset(100, 200));
    await tester.pump(const Duration(seconds: 1));
    expect(rig.kinds, ['move', 'left:down', 'left:up']);
  });

  testWidgets('input counters count gestures, never content, and reset', (
    tester,
  ) async {
    final rig = _Rig();
    rig.stroke(1, const [Offset(100, 100)]);
    await tester.pump(const Duration(milliseconds: 320));
    rig.down(2, const Offset(300, 300), ms: 400);
    rig.up(2, const Offset(300, 300));
    rig.down(3, const Offset(300, 300), ms: 100);
    rig.move(3, const Offset(340, 340));
    rig.up(3, const Offset(340, 340));
    final values = rig.counters.take(
      mode: AndroidTouchMode.direct,
      textInput: true,
    );
    expect(values['taps'], 1);
    expect(values['dragStarts'], 1);
    expect(values['touchMode'], 0);
    expect(values['textInput'], true);
    expect(
      values.values.every((v) => v is int || v is bool),
      true,
      reason: 'numbers and booleans only',
    );
    final next = rig.counters.take(
      mode: AndroidTouchMode.trackpad,
      textInput: false,
    );
    expect((next['taps'], next['dragStarts'], next['touchMode']), (0, 0, 1));
  });

  // --- this device's feedback: where and when each cue lands -------------

  testWidgets('feedback cues carry the picture point where the click lands, '
      'also zoomed and moved', (tester) async {
    final rig = _Rig();
    // Zoomed and moved: screen and picture coordinates differ.
    rig.view = rig.view
        .zoomTo(2, const Offset(.5, .5), const Offset(200, 400))
        .panned(const Offset(60, -40));
    const finger = Offset(150, 300);
    final landed = rig.view.normalize(finger)!;
    rig.down(1, finger);
    rig.up(1, finger);
    expect(rig.points.last, (AndroidTouchCue.tap, landed));
    expect(rig.view.point(landed), within(distance: .001, from: finger));
    await tester.pump(const Duration(milliseconds: 320));
    expect(
      (rig.buttons.first['x'], rig.buttons.first['y']),
      (landed.dx, landed.dy),
    );

    // 포인터: the tap clicks at the cursor, not under the finger.
    final pointer = _Rig(mode: AndroidTouchMode.trackpad);
    pointer.input.cursor = const Offset(.2, .7);
    pointer.down(1, const Offset(300, 100));
    pointer.up(1, const Offset(300, 100));
    expect(pointer.points.last, (AndroidTouchCue.tap, const Offset(.2, .7)));
    // A two-finger tap right-clicks there too.
    pointer.down(2, const Offset(300, 100), ms: 400);
    pointer.down(3, const Offset(340, 100));
    pointer.up(3, const Offset(340, 100), ms: 60);
    pointer.up(2, const Offset(300, 100));
    expect(pointer.points.last, (
      AndroidTouchCue.rightClick,
      const Offset(.2, .7),
    ));
    await tester.pump(const Duration(seconds: 1));
  });

  testWidgets('a still press announces its long press; holding right-clicks '
      'there, anything else ends it once', (tester) async {
    final rig = _Rig();
    rig.down(1, const Offset(100, 200));
    expect(rig.points.last, (AndroidTouchCue.press, const Offset(.25, .25)));
    await tester.pump(const Duration(milliseconds: 550));
    expect(rig.kinds, ['move', 'right:down', 'right:up']);
    expect(rig.points.last, (
      AndroidTouchCue.rightClick,
      const Offset(.25, .25),
    ));
    rig.up(1, const Offset(100, 200));
    expect(rig.cues, isNot(contains(AndroidTouchCue.pressEnd)));

    for (final (name, end) in <(String, void Function(_Rig))>[
      ('tap', (r) => r.up(1, const Offset(100, 200))),
      ('moved', (r) => r.move(1, const Offset(140, 200))),
      ('second finger', (r) => r.down(2, const Offset(300, 600))),
      ('cancelled', (r) => r.input.cancel(1)),
      ('reset', (r) => r.input.reset()),
    ]) {
      final r = _Rig();
      r.down(1, const Offset(100, 200));
      end(r);
      await tester.pump(const Duration(milliseconds: 600));
      expect(
        r.cues.where((c) => c == AndroidTouchCue.pressEnd),
        hasLength(1),
        reason: name,
      );
      expect(r.cues, isNot(contains(AndroidTouchCue.rightClick)), reason: name);
      r.input.reset();
    }

    // 포인터: the ring is where the right click would land, the cursor.
    final pointer = _Rig(mode: AndroidTouchMode.trackpad);
    pointer.input.cursor = const Offset(.6, .4);
    pointer.down(1, const Offset(20, 700));
    expect(pointer.points.last, (AndroidTouchCue.press, const Offset(.6, .4)));
    pointer.up(1, const Offset(20, 700));

    // The second press of a double tap drags; it never right-clicks.
    final drag = _Rig();
    drag.down(1, const Offset(100, 200));
    drag.up(1, const Offset(100, 200));
    drag.down(2, const Offset(102, 200), ms: 100);
    expect(drag.cues.where((c) => c == AndroidTouchCue.press), hasLength(1));
    drag.up(2, const Offset(102, 200), ms: 40);
    await tester.pump(const Duration(seconds: 1));
  });

  testWidgets('a drag is announced once when it starts and once when it '
      'ends, at the remote button', (tester) async {
    // Held still after a tap: [hold] only, counted as one drag start.
    final held = _Rig();
    held.down(1, const Offset(100, 200));
    held.up(1, const Offset(100, 200));
    held.down(2, const Offset(100, 200), ms: 100);
    await tester.pump(const Duration(milliseconds: 320));
    held.move(2, const Offset(200, 300), ms: 320);
    held.up(2, const Offset(200, 300));
    expect(held.cues.where((c) => c == AndroidTouchCue.hold), hasLength(1));
    expect(held.cues, isNot(contains(AndroidTouchCue.dragStart)));
    expect(held.points.last, (AndroidTouchCue.dragEnd, const Offset(.5, .375)));
    expect(
      held.counters.take(
        mode: AndroidTouchMode.direct,
        textInput: false,
      )['dragStarts'],
      1,
    );

    // Moved after a tap: [dragStart] where the button went down.
    final moved = _Rig();
    moved.down(1, const Offset(100, 200));
    moved.up(1, const Offset(100, 200));
    moved.down(2, const Offset(100, 200), ms: 100);
    moved.move(2, const Offset(160, 200));
    moved.up(2, const Offset(160, 200));
    expect(
      moved.points.where((p) => p.$1 == AndroidTouchCue.dragStart).toList(),
      [(AndroidTouchCue.dragStart, const Offset(.25, .25))],
    );
    expect(moved.cues.where((c) => c == AndroidTouchCue.dragEnd), hasLength(1));
    expect(moved.cues, isNot(contains(AndroidTouchCue.hold)));
    await tester.pump(const Duration(seconds: 1));
  });

  testWidgets('real touch: each contact is marked where it lands; Windows '
      'interprets holds', (tester) async {
    final contacts = _Contacts();
    final rig = _Rig(contacts: contacts);
    rig.down(1, const Offset(100, 200));
    rig.down(2, const Offset(300, 600));
    expect(rig.points, [
      (AndroidTouchCue.contact, const Offset(.25, .25)),
      (AndroidTouchCue.contact, const Offset(.75, .75)),
    ]);
    await tester.pump(const Duration(milliseconds: 600));
    rig.up(2, const Offset(300, 600));
    rig.up(1, const Offset(100, 200));
    expect(rig.cues, isNot(contains(AndroidTouchCue.press)));
    expect(rig.sent, isEmpty);
  });

  // --- the feedback overlay ------------------------------------------------

  testWidgets('feedback marks are drawn at the picture point on the current '
      'view, follow zoom and pan, end within 300 ms and take no touch', (
    tester,
  ) async {
    final cues = ValueNotifier<(AndroidTouchCue, Offset?, int)?>(null);
    addTearDown(cues.dispose);
    var view = AndroidPictureView.fit(
      const Rect.fromLTWH(0, 0, 400, 800),
      const Size(400, 800),
    ).zoomTo(2, const Offset(.5, .5), const Offset(200, 400));
    var below = 0;
    await tester.pumpWidget(
      Directionality(
        textDirection: TextDirection.ltr,
        child: Stack(
          children: [
            Positioned.fill(
              child: Listener(
                behavior: HitTestBehavior.opaque,
                onPointerDown: (_) => below++,
              ),
            ),
            Positioned.fill(
              child: AndroidTouchFeedback(
                key: _feedback,
                cues: cues,
                view: () => view,
              ),
            ),
          ],
        ),
      ),
    );
    expect(_drawn(tester).circles, isEmpty);
    // Zoomed 2x: picture point (.375, .4375) is drawn at (100, 300).
    cues.value = (AndroidTouchCue.tap, const Offset(.375, .4375), 1);
    await tester.pump();
    expect(_drawn(tester).circles, [const Offset(100, 300)]);
    // The picture moves under the mark: the mark moves with it.
    view = view.panned(const Offset(-50, 20));
    await tester.pump(const Duration(milliseconds: 50));
    expect(_drawn(tester).circles, [const Offset(50, 320)]);
    await tester.tapAt(const Offset(50, 320));
    expect(below, 1, reason: 'the touch reaches what is under the marks');
    await tester.pump(const Duration(milliseconds: 260));
    expect(_drawn(tester).circles, isEmpty);
    // Cues without a mark (wheel, click) draw nothing.
    cues.value = (AndroidTouchCue.wheel, null, 2);
    cues.value = (AndroidTouchCue.click, null, 3);
    await tester.pump();
    expect(_drawn(tester).circles, isEmpty);
  });

  testWidgets('feedback ring: a press fills a ring after a third of the long '
      'press; its right click turns it into a flash, a pressEnd removes it', (
    tester,
  ) async {
    final cues = ValueNotifier<(AndroidTouchCue, Offset?, int)?>(null);
    addTearDown(cues.dispose);
    final view = AndroidPictureView.fit(
      const Rect.fromLTWH(0, 0, 400, 800),
      const Size(400, 800),
    );
    await tester.pumpWidget(
      AndroidTouchFeedback(key: _feedback, cues: cues, view: () => view),
    );
    const centre = Offset(200, 400);
    cues.value = (AndroidTouchCue.press, const Offset(.5, .5), 1);
    await tester.pump();
    await tester.pump(const Duration(milliseconds: 150));
    expect(_drawn(tester).arcs, isEmpty, reason: 'quick taps show no ring');
    await tester.pump(const Duration(milliseconds: 100));
    expect(_drawn(tester).arcs.single, closeTo(math.pi, 1e-9));
    await tester.pump(const Duration(milliseconds: 200));
    expect(_drawn(tester).arcs.single, closeTo(1.8 * math.pi, 1e-9));
    expect(_drawn(tester).circles, [centre]);
    cues.value = (AndroidTouchCue.rightClick, const Offset(.5, .5), 2);
    await tester.pump();
    expect(_drawn(tester).arcs, isEmpty);
    expect(_drawn(tester).circles, [centre]);
    await tester.pump(const Duration(milliseconds: 400));
    expect(_drawn(tester).circles, isEmpty);

    cues.value = (AndroidTouchCue.press, const Offset(.5, .5), 3);
    await tester.pump();
    await tester.pump(const Duration(milliseconds: 300));
    expect(_drawn(tester).arcs, hasLength(1));
    cues.value = (AndroidTouchCue.pressEnd, null, 4);
    expect(_drawn(tester).arcs, isEmpty);
    expect(_drawn(tester).circles, isEmpty);
    await tester.pump(const Duration(seconds: 1));
  });

  // --- ToTBDesk parity: the picture above the phone keyboard -------------

  const screen = Rect.fromLTWH(0, 0, 400, 800);
  AndroidPictureView full() =>
      AndroidPictureView.fit(screen, const Size(400, 800));
  // The key row (48) arrives first, then the keyboard slides in (300).
  const keyRowOnly = Rect.fromLTWH(0, 0, 400, 752);
  const typingRoom = Rect.fromLTWH(0, 0, 400, 452);

  test('keyboard camera: the picture moves up by the keyboard and its key '
      'row, keeping its zoom, and returns exactly when they close', () {
    final rest = full();
    final camera = AndroidKeyboardCamera();
    const typing = Offset(.5, .9); // e.g. the taskbar's search box
    var view = camera.place(rest, keyRowOnly, typing);
    expect(camera.open, isTrue);
    expect(view.rect.top, -48);
    view = camera.place(view, typingRoom, typing);
    // Up by all it covers (348), never past the picture's bottom edge.
    expect(view.rect.top, -348);
    expect(view.rect.bottom, typingRoom.bottom);
    expect(view.scale, 1);
    expect(view.point(typing).dy, lessThan(typingRoom.bottom));
    view = camera.place(view, screen, typing);
    expect(view, rest);
    expect(camera.open, isFalse);
  });

  test('keyboard camera: a typing point near the top is never pushed out of '
      'sight', () {
    final camera = AndroidKeyboardCamera();
    // An address bar at 40 dp: lifting by 348 would hide it, so the picture
    // only moves as far as keeps it 56 dp inside the room above the keys.
    final view = camera.place(full(), typingRoom, const Offset(.5, .05));
    expect(view.point(const Offset(.5, .05)).dy, greaterThanOrEqualTo(40));
    expect(view.rect.top, 0);
    // A point in the middle: the whole lift, the point still visible.
    final middle = AndroidKeyboardCamera().place(
      full(),
      typingRoom,
      const Offset(.5, .6),
    );
    expect(middle.rect.top, -348);
    expect(middle.point(const Offset(.5, .6)).dy, closeTo(132, .01));
  });

  test('keyboard camera: a picture that fits above the keyboard is centred '
      'there; a zoom made meanwhile is kept when it closes', () {
    final letterbox = AndroidPictureView.fit(
      screen,
      const Size(1600, 900),
      devicePixelRatio: 2,
    );
    expect(letterbox.rect, const Rect.fromLTWH(0, 287.5, 400, 225));
    final camera = AndroidKeyboardCamera();
    var view = camera.place(letterbox, typingRoom, const Offset(.5, .5));
    expect(view.rect.center.dy, 226);
    expect(view.canPan(typingRoom), isFalse, reason: 'nothing hidden');
    // Zoomed by hand while typing: kept, inside the screen, on close.
    camera.moved(byHand: true);
    view = view.zoomTo(2, const Offset(.5, .5), typingRoom.center);
    expect(camera.manual, isTrue);
    view = camera.place(view, typingRoom, const Offset(.5, .5));
    expect(view.scale, 2);
    view = camera.place(view, screen, const Offset(.5, .5));
    expect(view.scale, 2);
    expect(screen.contains(view.rect.center), isTrue);
  });

  test('keyboard camera: a picture moved by hand stays while the keyboard '
      'changes height; 전체 보기 places it for typing again', () {
    final rest = full();
    final camera = AndroidKeyboardCamera();
    var view = camera.place(rest, typingRoom, const Offset(.5, .9));
    camera.moved(byHand: true);
    view = view.panned(const Offset(0, 200), visible: typingRoom);
    expect(view.rect.top, -148);
    // The keyboard grows (a suggestion strip): only kept on screen.
    const taller = Rect.fromLTWH(0, 0, 400, 412);
    view = camera.place(view, taller, const Offset(.5, .9));
    expect(view.rect.top, -148);
    view = camera.fit(view, taller, const Offset(.5, .9));
    expect(camera.manual, isFalse);
    expect(view.rect.bottom, taller.bottom);
    // Closing: back to the rest.
    expect(camera.place(view, screen, const Offset(.5, .9)), rest);
  });

  test('이동 is offered only while part of the picture is out of sight', () {
    final rest = full();
    expect(rest.canPan(screen), isFalse);
    expect(rest.canPan(typingRoom), isTrue, reason: 'under the keyboard');
    final zoomed = rest.zoomTo(1.5, const Offset(.5, .5), screen.center);
    expect(zoomed.canPan(screen), isTrue);
  });

  test('a picture moved by hand above the keyboard no longer follows the '
      'cursor', () {
    var follow = false;
    final rig = _Rig(mode: AndroidTouchMode.trackpad, follow: () => follow);
    rig.view = rig.view.zoomTo(3, const Offset(.5, .5), screen.center);
    final before = rig.view;
    rig.stroke(1, [
      const Offset(200, 400),
      for (var x = 210.0; x <= 390; x += 20) Offset(x, 400),
    ]);
    expect(rig.view, before);
    expect(rig.sent.where((e) => e['event'] == 'move'), isNotEmpty);
    follow = true;
    rig.stroke(2, [
      const Offset(200, 400),
      for (var x = 210.0; x <= 390; x += 20) Offset(x, 400),
    ], ms: 16);
    expect(rig.view.rect.left, lessThan(before.rect.left));
  });

  testWidgets('three-finger tap opens the keyboard over real touch too', (
    tester,
  ) async {
    final contacts = _Contacts();
    final rig = _Rig(contacts: contacts);
    rig.down(1, const Offset(100, 400));
    rig.down(2, const Offset(200, 400));
    rig.down(3, const Offset(300, 400));
    rig.up(3, const Offset(300, 400));
    rig.up(2, const Offset(200, 400));
    rig.up(1, const Offset(100, 400));
    expect(rig.cues, contains(AndroidTouchCue.keyboard));
    // The two contacts already down are cancelled on the remote.
    expect(contacts.log.where((l) => l.contains(':cancel:')), hasLength(2));
    expect(rig.buttons, isEmpty);
  });

  testWidgets('pinch: the whole picture is a detent and each limit ticks '
      'once per gesture', (tester) async {
    final rig = _Rig(mode: AndroidTouchMode.trackpad);
    rig.down(1, const Offset(150, 400));
    rig.down(2, const Offset(250, 400));
    rig.move(2, const Offset(270, 400)); // spread 120: 120 %
    expect(rig.view.scale, closeTo(1.2, .001));
    rig.move(2, const Offset(251, 400)); // 101 %: the detent
    expect(rig.view.scale, 1);
    rig.move(2, const Offset(230, 400)); // further in: stays whole
    rig.move(2, const Offset(210, 400));
    expect(rig.view.scale, 1);
    expect(rig.cues.where((c) => c == AndroidTouchCue.zoomLimit), hasLength(1));
    rig.move(2, const Offset(900, 400)); // far out: the largest zoom
    rig.move(2, const Offset(950, 400));
    expect(rig.view.scale, AndroidPictureView.maxScale);
    expect(rig.cues.where((c) => c == AndroidTouchCue.zoomLimit), hasLength(2));
    rig.up(2, const Offset(950, 400));
    rig.up(1, const Offset(150, 400));
    expect(rig.sent, isEmpty, reason: 'zoom is local');
  });
}

class _Contacts implements AndroidTouchContacts {
  @override
  bool available = true;
  final log = <String>[];
  @override
  void contact(int id, Offset point, AndroidContactPhase phase) {
    String n(double v) => v.toStringAsFixed(3).replaceFirst(RegExp(r'0+$'), '').replaceFirst(RegExp(r'\.$'), '');
    log.add('$id:${phase.name}:${n(point.dx)},${n(point.dy)}');
  }
}
