import 'package:flutter/material.dart';
import 'package:flutter/services.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:multitap_client/android/android_tv_remote.dart';
import 'package:multitap_client/android/android_touch.dart';

void main() {
  late Offset point;
  late List<Offset> moves;
  late List<bool> clicks;
  late int menus;
  late bool enabled, permitted;
  late Object session;
  late StateSetter rebuild;
  late GlobalKey<AndroidTvRemoteState> remote;
  late FocusNode local;
  late AndroidTvPointerMode mode;
  late AndroidPointerSpeed speed;
  late List<(Object, bool?)> dragEvents;
  late List<int> wheels;
  setUp(() {
    point = const Offset(.5, .5);
    moves = [];
    clicks = [];
    menus = 0;
    enabled = true;
    permitted = true;
    session = Object();
    remote = GlobalKey<AndroidTvRemoteState>();
    local = FocusNode();
    mode = AndroidTvPointerMode.pointer;
    speed = AndroidPointerSpeed.standard;
    dragEvents = [];
    wheels = [];
  });
  tearDown(() => local.dispose());
  Future<void> open(WidgetTester t) async {
    await t.pumpWidget(
      MaterialApp(
        home: StatefulBuilder(
          builder: (context, setState) {
            rebuild = setState;
            return Scaffold(
              body: AndroidTvRemote(
                key: remote,
                mode: mode,
                speed: speed,
                enabled: enabled,
                session: session,
                position: () => point,
                source: () => const Size(1000, 800),
                canControl: () => permitted,
                onMove: (at) {
                  point = at;
                  moves.add(at);
                },
                onClick: (_, right) => clicks.add(right),
                onMenu: () => menus++,
                onScroll: (_, delta) => wheels.add(delta),
                onDragStart: (_) {
                  final owner = session;
                  final events = dragEvents;
                  events.add((owner, null));
                  return (_, {required cancelled}) =>
                      events.add((owner, cancelled));
                },
                child: Center(
                  child: TextButton(
                    focusNode: local,
                    onPressed: () {},
                    child: const Text('local'),
                  ),
                ),
              ),
            );
          },
        ),
      ),
    );
    await t.pump();
  }

  testWidgets(
    'TV directions move a bounded pointer, accelerate and stop on up',
    (t) async {
      await open(t);
      await t.sendKeyDownEvent(LogicalKeyboardKey.arrowRight);
      expect(point.dx, closeTo(.508, .00001));
      await t.pump(const Duration(milliseconds: 330));
      final initial = moves[1].dx - moves[0].dx;
      await t.pump(const Duration(milliseconds: 400));
      expect(moves.last.dx - moves[moves.length - 2].dx, greaterThan(initial));
      await t.pump(const Duration(seconds: 3));
      expect(point.dx, 1);
      expect(point.dy, .5);
      await t.sendKeyUpEvent(LogicalKeyboardKey.arrowRight);
      final count = moves.length;
      await t.pump(const Duration(seconds: 1));
      expect(moves.length, count);
    },
  );

  testWidgets(
    'short OK clicks left once; held OK clicks right once without a left click',
    (t) async {
      await open(t);
      await t.sendKeyDownEvent(LogicalKeyboardKey.select);
      await t.pump(const Duration(milliseconds: 100));
      expect(clicks, isEmpty);
      await t.sendKeyUpEvent(LogicalKeyboardKey.select);
      expect(clicks, [false]);
      await t.sendKeyDownEvent(LogicalKeyboardKey.select);
      await t.pump(const Duration(milliseconds: 600));
      await t.sendKeyRepeatEvent(LogicalKeyboardKey.select);
      await t.pump(const Duration(seconds: 1));
      await t.sendKeyUpEvent(LogicalKeyboardKey.select);
      expect(clicks, [false, true]);
    },
  );

  testWidgets('Menu is local and physical repeat cannot open it twice', (
    t,
  ) async {
    await open(t);
    await t.sendKeyDownEvent(LogicalKeyboardKey.contextMenu);
    await t.sendKeyRepeatEvent(LogicalKeyboardKey.contextMenu);
    await t.sendKeyUpEvent(LogicalKeyboardKey.contextMenu);
    expect(menus, 1);
    expect(clicks, isEmpty);
    expect(moves, isEmpty);
  });

  testWidgets(
    'phone/IME ownership disables pointer handling and cancels a held OK',
    (t) async {
      await open(t);
      await t.sendKeyDownEvent(LogicalKeyboardKey.select);
      rebuild(() => enabled = false);
      await t.pump();
      await t.pump(const Duration(seconds: 1));
      await t.sendKeyUpEvent(LogicalKeyboardKey.select);
      await t.sendKeyEvent(LogicalKeyboardKey.arrowRight);
      expect(clicks, isEmpty);
      expect(moves, isEmpty);
    },
  );

  testWidgets(
    'local control focus and dialogs keep their navigation and stop repetition',
    (t) async {
      await open(t);
      await t.sendKeyDownEvent(LogicalKeyboardKey.arrowDown);
      local.requestFocus();
      await t.pump();
      final count = moves.length;
      await t.pump(const Duration(milliseconds: 500));
      await t.sendKeyUpEvent(LogicalKeyboardKey.arrowDown);
      expect(moves.length, count);
      expect(local.hasFocus, true);
      final future = showDialog<void>(
        context: local.context!,
        builder: (context) => AlertDialog(
          actions: [
            TextButton(
              autofocus: true,
              onPressed: () => Navigator.pop(context),
              child: const Text('close'),
            ),
          ],
        ),
      );
      await t.pumpAndSettle();
      await t.sendKeyEvent(LogicalKeyboardKey.enter);
      await t.pumpAndSettle();
      await future;
      expect(clicks, isEmpty);
    },
  );

  testWidgets(
    'lease replacement and native capture release cancel pending actions',
    (t) async {
      await open(t);
      await t.sendKeyDownEvent(LogicalKeyboardKey.select);
      rebuild(() => session = Object());
      await t.pump();
      await t.pump();
      await t.pump(const Duration(seconds: 1));
      await t.sendKeyUpEvent(LogicalKeyboardKey.select);
      expect(clicks, isEmpty);
      await t.sendKeyDownEvent(LogicalKeyboardKey.arrowLeft);
      remote.currentState!.cancel();
      final count = moves.length;
      await t.pump(const Duration(seconds: 1));
      await t.sendKeyUpEvent(LogicalKeyboardKey.arrowLeft);
      expect(moves.length, count);
    },
  );

  testWidgets('foreground loss and a runtime readiness loss stop movement', (
    t,
  ) async {
    await open(t);
    await t.sendKeyDownEvent(LogicalKeyboardKey.arrowRight);
    t.binding.handleAppLifecycleStateChanged(AppLifecycleState.inactive);
    final count = moves.length;
    await t.pump(const Duration(milliseconds: 500));
    await t.sendKeyUpEvent(LogicalKeyboardKey.arrowRight);
    expect(moves.length, count);
    t.binding.handleAppLifecycleStateChanged(AppLifecycleState.resumed);
    await t.pump();
    await t.sendKeyDownEvent(LogicalKeyboardKey.arrowDown);
    permitted = false;
    await t.pump(const Duration(milliseconds: 40));
    final stopped = moves.length;
    await t.pump(const Duration(seconds: 1));
    await t.sendKeyUpEvent(LogicalKeyboardKey.arrowDown);
    expect(moves.length, stopped);
  });

  testWidgets(
    'explicit drag mode locks left across movement and the next OK releases it',
    (t) async {
      mode = AndroidTvPointerMode.drag;
      await open(t);
      await t.sendKeyEvent(LogicalKeyboardKey.select);
      await t.pump();
      expect(dragEvents, [(session, null)]);
      await t.pump();
      expect(find.text('끄는 중 · OK로 놓기'), findsOneWidget);
      await t.sendKeyDownEvent(LogicalKeyboardKey.arrowRight);
      await t.pump(const Duration(milliseconds: 200));
      await t.sendKeyUpEvent(LogicalKeyboardKey.arrowRight);
      expect(moves, isNotEmpty);
      expect(dragEvents, hasLength(1));
      await t.sendKeyDownEvent(LogicalKeyboardKey.select);
      await t.pump(const Duration(milliseconds: 600));
      await t.sendKeyUpEvent(LogicalKeyboardKey.select);
      expect(dragEvents, [(session, null), (session, false)]);
      expect(clicks, isEmpty);
    },
  );

  for (final cause in [
    'session',
    'mode',
    'disabled',
    'focus',
    'background',
    'native',
    'dispose',
  ]) {
    testWidgets('drag cancellation releases its original owner on $cause', (
      t,
    ) async {
      mode = AndroidTvPointerMode.drag;
      await open(t);
      final original = session;
      await t.sendKeyEvent(LogicalKeyboardKey.select);
      switch (cause) {
        case 'session':
          rebuild(() => session = Object());
          await t.pump();
        case 'mode':
          rebuild(() => mode = AndroidTvPointerMode.scroll);
          await t.pump();
        case 'disabled':
          rebuild(() => enabled = false);
          await t.pump();
        case 'focus':
          local.requestFocus();
          await t.pump();
        case 'background':
          t.binding.handleAppLifecycleStateChanged(AppLifecycleState.inactive);
          await t.pump();
          t.binding.handleAppLifecycleStateChanged(AppLifecycleState.resumed);
        case 'native':
          remote.currentState!.cancel();
          await t.pump();
        case 'dispose':
          await t.pumpWidget(const SizedBox());
      }
      expect(dragEvents, [(original, null), (original, true)]);
      await t.pump(const Duration(seconds: 1));
      expect(dragEvents, hasLength(2));
    });
  }

  testWidgets(
    'scroll mode sends vertical wheel only and Menu stops repetition',
    (t) async {
      mode = AndroidTvPointerMode.scroll;
      await open(t);
      await t.sendKeyEvent(LogicalKeyboardKey.arrowLeft);
      await t.sendKeyEvent(LogicalKeyboardKey.arrowRight);
      expect(moves, isEmpty);
      expect(wheels, isEmpty);
      await t.sendKeyDownEvent(LogicalKeyboardKey.arrowUp);
      await t.pump(const Duration(milliseconds: 220));
      expect(wheels, everyElement(120));
      expect(wheels.length, greaterThan(1));
      await t.sendKeyEvent(LogicalKeyboardKey.contextMenu);
      final count = wheels.length;
      await t.pump(const Duration(milliseconds: 500));
      await t.sendKeyUpEvent(LogicalKeyboardKey.arrowUp);
      expect(wheels.length, count);
      expect(menus, 1);
      await t.sendKeyEvent(LogicalKeyboardKey.arrowDown);
      expect(wheels.last, -120);
    },
  );

  testWidgets(
    'precise short taps move one remote pixel and saved fast speed travels farther',
    (t) async {
      speed = AndroidPointerSpeed.precise;
      await open(t);
      await t.sendKeyEvent(LogicalKeyboardKey.arrowRight);
      expect(point.dx, closeTo(.501, .000001));
      final travel = <double>[];
      for (final value in [
        AndroidPointerSpeed.precise,
        AndroidPointerSpeed.fast,
      ]) {
        point = const Offset(.5, .5);
        rebuild(() => speed = value);
        await t.pump();
        await t.sendKeyDownEvent(LogicalKeyboardKey.arrowRight);
        await t.pump(const Duration(milliseconds: 200));
        await t.sendKeyUpEvent(LogicalKeyboardKey.arrowRight);
        travel.add(point.dx - .5);
      }
      expect(travel.last, greaterThan(travel.first));
    },
  );

  testWidgets(
    'disposing while a key is held leaves no repeats or click behind',
    (t) async {
      await open(t);
      await t.sendKeyDownEvent(LogicalKeyboardKey.arrowDown);
      await t.sendKeyDownEvent(LogicalKeyboardKey.select);
      await t.pumpWidget(const SizedBox());
      final count = moves.length;
      await t.pump(const Duration(seconds: 1));
      await t.sendKeyUpEvent(LogicalKeyboardKey.arrowDown);
      await t.sendKeyUpEvent(LogicalKeyboardKey.select);
      expect(moves.length, count);
      expect(clicks, isEmpty);
      expect(t.takeException(), isNull);
    },
  );
}
