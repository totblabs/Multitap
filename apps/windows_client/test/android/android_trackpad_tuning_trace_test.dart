// Synthetic traces through the actual gesture controller. These measurements
// describe the baseline; they do not claim device feel or end-to-end latency.
import 'dart:convert';

import 'package:flutter/widgets.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:multitap_client/android/android_touch.dart';

const _partitions = [2, 4, 8, 16];
const _pointerArea = Rect.fromLTWH(0, 0, 2000, 1000);
const _pointerSource = Size(2000, 1000);
const _pointerStart = Offset(.25, .5);

class _TraceRig {
  _TraceRig({
    required this.clock,
    required Rect area,
    required Size encoded,
    required this.sourceHint,
    AndroidPointerSpeed speed = AndroidPointerSpeed.standard,
    Offset cursor = _pointerStart,
  }) {
    started = clock();
    view = AndroidPictureView.fit(area, encoded);
    input = AndroidTouchInput(
      view: () => view,
      visible: () => area,
      onView: (next) => view = next,
      send: (event) {
        events.add(Map<String, dynamic>.from(event));
        emittedAtUs.add(elapsed.inMicroseconds);
      },
      follow: () => false,
      source: () => sourceHint,
      mode: AndroidTouchMode.trackpad,
      speed: speed,
    )..cursor = cursor;
  }

  final DateTime Function() clock;
  late final DateTime started;
  final Size sourceHint;
  late AndroidPictureView view;
  late final AndroidTouchInput input;
  final events = <Map<String, dynamic>>[];
  final emittedAtUs = <int>[];

  Duration get elapsed => clock().difference(started);
  List<Map<String, dynamic>> get moves => [
    for (final event in events)
      if (event['event'] == 'move') event,
  ];
  List<Map<String, dynamic>> get buttons => [
    for (final event in events)
      if (event['event'] == 'button') event,
  ];
  List<Map<String, dynamic>> get wheels => [
    for (final event in events)
      if (event['event'] == 'wheel') event,
  ];
  int get wheelSum =>
      wheels.fold<int>(0, (total, event) => total + (event['delta'] as int));

  Future<void> at(WidgetTester tester, int milliseconds) async {
    final delay = Duration(milliseconds: milliseconds) - elapsed;
    expect(delay.isNegative, isFalse, reason: 'Trace clock must be monotonic');
    await tester.pump(delay);
  }

  void down(int pointer, Offset point) => input.down(pointer, point, elapsed);
  void move(int pointer, Offset point) => input.move(pointer, point, elapsed);
  void up(int pointer, Offset point) => input.up(pointer, point, elapsed);

  Map<String, Object> summary() => {
    'cursorX': input.cursor.dx,
    'cursorY': input.cursor.dy,
    'moveMessages': moves.length,
    'buttonMessages': buttons.length,
    'wheelMessages': wheels.length,
    'wheelSum': wheelSum,
    'firstEmissionUs': emittedAtUs.isEmpty ? -1 : emittedAtUs.first,
    'lastEmissionUs': emittedAtUs.isEmpty ? -1 : emittedAtUs.last,
    'active': input.active,
    'dragging': input.dragging,
    'clickPending': input.clickPending,
    'flinging': input.flinging,
  };

  Future<void> finish(WidgetTester tester) async {
    input.dispose();
    // Explicit cleanup before testWidgets checks for pending timers. This
    // drains only fake time after all this rig's timers have been cancelled.
    await tester.pump(const Duration(seconds: 2));
  }
}

_TraceRig _pointerRig(WidgetTester tester, AndroidPointerSpeed speed) =>
    _TraceRig(
      clock: tester.binding.clock.now,
      area: _pointerArea,
      encoded: _pointerSource,
      sourceHint: _pointerSource,
      speed: speed,
    );

void _emit(String kind, Map<String, Object> values) {
  // One complete line, readable from Flutter JSON reporter print messages.
  // Only synthetic scalar measurements are printed, never actual user input.
  // ignore: avoid_print
  print(
    'TRACKPAD_TUNING_BASELINE ${jsonEncode({'schema': 1, 'kind': kind, 'productionController': true, 'productionWidgetWiring': false, 'actualDevice': false, ...values})}',
  );
}

void _expectWire(_TraceRig rig) {
  for (final event in rig.events) {
    expect(event['type'], 'input');
    expect(event['event'], isIn(['move', 'button', 'wheel']));
    for (final axis in ['x', 'y']) {
      final value = event[axis] as double;
      expect(value.isFinite, isTrue);
      expect(value, inInclusiveRange(0.0, 1.0));
    }
    if (event['event'] == 'wheel') {
      expect(event['delta'], isA<int>());
      expect(event['delta'] as int, inInclusiveRange(-1200, 1200));
    }
  }
  expect(rig.input.cursor.dx.isFinite && rig.input.cursor.dy.isFinite, isTrue);
}

void _expectReleased(_TraceRig rig) {
  expect(rig.input.active, isFalse);
  expect(rig.input.dragging, isFalse);
  expect(rig.input.clickPending, isFalse);
  expect(rig.input.flinging, isFalse);
}

Future<void> _prefix(WidgetTester tester, _TraceRig rig) async {
  rig.down(1, const Offset(100, 500));
  await rig.at(tester, 16);
  // Same slop crossing at the same time in every pointer trace.
  rig.move(1, const Offset(116, 500));
}

Future<void> _beginDrag(WidgetTester tester, _TraceRig rig) async {
  rig.down(1, const Offset(100, 500));
  await rig.at(tester, 40);
  rig.up(1, const Offset(100, 500));
  await rig.at(tester, 100);
  rig.down(2, const Offset(100, 500));
  await rig.at(tester, 116);
  rig.move(2, const Offset(120, 500));
  expect(rig.input.dragging, isTrue);
  expect(rig.buttons.length, 1);
  expect(rig.buttons.single['down'], isTrue);
}

void main() {
  for (final speed in AndroidPointerSpeed.values) {
    testWidgets('baseline equal pointer paths: ${speed.name} at 2/4/8/16ms', (
      tester,
    ) async {
      final travels = <double>[];
      for (final dt in _partitions) {
        final rig = _pointerRig(tester, speed);
        try {
          await _prefix(tester, rig);
          for (var t = 16 + dt; t <= 112; t += dt) {
            await rig.at(tester, t);
            rig.move(1, Offset(100.0 + t, 500));
          }
          rig.up(1, const Offset(212, 500));
          _expectWire(rig);
          _expectReleased(rig);
          expect(rig.buttons, isEmpty);
          expect(rig.wheels, isEmpty);
          var previous = _pointerStart.dx;
          for (final event in rig.moves) {
            final next = event['x'] as double;
            expect(next, greaterThanOrEqualTo(previous));
            previous = next;
          }
          expect(rig.input.cursor.dx, greaterThan(_pointerStart.dx));
          expect(rig.input.cursor.dx, lessThan(1), reason: 'No edge clamp');
          expect(rig.input.cursor.dy, _pointerStart.dy);
          final travel =
              (rig.input.cursor.dx - _pointerStart.dx) * _pointerSource.width;
          travels.add(travel);
          _emit('pointer', {
            'preset': speed.name,
            'partitionMs': dt,
            'pathLogicalPixels': 112,
            'recognitionSlopLogicalPixels': rig.input.slop,
            'sourcePixelTravel': travel,
            ...rig.summary(),
          });
        } finally {
          await rig.finish(tester);
        }
      }
      final sorted = [...travels]..sort();
      _emit('partition-summary', {
        'preset': speed.name,
        'partitionsMs': _partitions,
        'sourcePixelTravels': travels,
        'sourcePixelSpread': sorted.last - sorted.first,
      });
      // This is an arithmetic equality, not a device-feel tolerance. The
      // cursor adds normalized binary64 values once per event. Bound only
      // rounding in those additions and the final source-pixel conversion;
      // do not permit a source-pixel-sized sampling difference.
      const unitRoundoff = 1.1102230246251565e-16;
      const maximumMovesPerTrace = 1 + 96 ~/ 2;
      const additionsAndConversion = maximumMovesPerTrace + 2;
      const gamma =
          additionsAndConversion *
          unitRoundoff /
          (1 - additionsAndConversion * unitRoundoff);
      final pairRoundoffBound = 2 * gamma * _pointerSource.width;
      for (final travel in travels.skip(1)) {
        expect(travel, closeTo(travels.first, pairRoundoffBound));
      }
    });
  }

  testWidgets('baseline wheel hint: fixed workspace, two encoded sizes', (
    tester,
  ) async {
    const workspace = Size(2160, 1080);
    const encodedSizes = [workspace, Size(1080, 540)];
    final workspaceSums = <int>[];
    for (final useWorkspaceHint in [false, true]) {
      for (final encoded in encodedSizes) {
        final rig = _TraceRig(
          clock: tester.binding.clock.now,
          area: const Rect.fromLTWH(0, 0, 720, 360),
          encoded: encoded,
          sourceHint: useWorkspaceHint ? workspace : encoded,
          cursor: const Offset(.5, .5),
        );
        try {
          expect(rig.view.rect, const Rect.fromLTWH(0, 0, 720, 360));
          rig.down(1, const Offset(240, 160));
          rig.down(2, const Offset(480, 160));
          for (var n = 1; n <= 6; n++) {
            await rig.at(tester, n * 16);
            final y = 160.0 - n * 20;
            rig.move(1, Offset(240, y));
            rig.move(2, Offset(480, y));
          }
          // Suppress inertia using the production 80ms freshness boundary.
          await rig.at(tester, 177);
          rig.up(1, const Offset(240, 40));
          rig.up(2, const Offset(480, 40));
          _expectWire(rig);
          _expectReleased(rig);
          expect(rig.buttons, isEmpty);
          expect(rig.moves, isEmpty);
          expect(rig.wheels, isNotEmpty);
          expect(rig.wheels.every((e) => (e['delta'] as int) < 0), isTrue);
          expect(rig.view.scale, 1, reason: 'Scroll must not become pinch');
          if (useWorkspaceHint) workspaceSums.add(rig.wheelSum);
          _emit('wheel', {
            'sourceHintKind': useWorkspaceHint ? 'workspace' : 'encoded',
            'workspaceWidth': workspace.width,
            'workspaceHeight': workspace.height,
            'encodedWidth': encoded.width,
            'encodedHeight': encoded.height,
            'centroidTravelLogicalPixels': -120,
            ...rig.summary(),
          });
        } finally {
          await rig.finish(tester);
        }
      }
    }
    // Identical controller input and identical source hint must behave alike.
    // The encoded-hint contrast is recorded without freezing a bad wire value.
    expect(workspaceSums[0], workspaceSums[1]);
  });

  testWidgets('baseline slow pointer trace across presets', (tester) async {
    for (final speed in AndroidPointerSpeed.values) {
      final rig = _pointerRig(tester, speed);
      try {
        await _prefix(tester, rig);
        for (var n = 1; n <= 12; n++) {
          await rig.at(tester, 16 + n * 32);
          rig.move(1, Offset(116.0 + n * 2, 500));
        }
        rig.up(1, const Offset(140, 500));
        _expectWire(rig);
        _expectReleased(rig);
        expect(rig.buttons, isEmpty);
        expect(rig.wheels, isEmpty);
        expect(rig.input.cursor.dx, greaterThan(_pointerStart.dx));
        _emit('slow', {
          'preset': speed.name,
          'movingSpeedLogicalPixelsPerMs': 2 / 32,
          'sourcePixelTravel':
              (rig.input.cursor.dx - _pointerStart.dx) * _pointerSource.width,
          ...rig.summary(),
        });
      } finally {
        await rig.finish(tester);
      }
    }
  });

  testWidgets('baseline jitter stays a single tap, not move or drag', (
    tester,
  ) async {
    for (final speed in AndroidPointerSpeed.values) {
      final rig = _pointerRig(tester, speed);
      try {
        rig.down(1, const Offset(100, 500));
        const jitter = [
          Offset(102, 501),
          Offset(99, 499),
          Offset(103, 502),
          Offset(100, 500),
        ];
        for (var n = 0; n < jitter.length; n++) {
          await rig.at(tester, (n + 1) * 16);
          rig.move(1, jitter[n]);
        }
        await rig.at(tester, 80);
        rig.up(1, const Offset(100, 500));
        expect(rig.moves, isEmpty);
        expect(rig.buttons, isEmpty);
        expect(rig.input.cursor, _pointerStart);
        expect(rig.input.clickPending, isTrue);
        await tester.pump(rig.input.doubleTap);
        _expectWire(rig);
        _expectReleased(rig);
        expect(rig.buttons.map((e) => (e['button'], e['down'])), [
          ('left', true),
          ('left', false),
        ]);
        expect(rig.wheels, isEmpty);
        _emit('jitter-tap', {'preset': speed.name, ...rig.summary()});
      } finally {
        await rig.finish(tester);
      }
    }
  });

  testWidgets('baseline reversal moves back without clicking', (tester) async {
    for (final speed in AndroidPointerSpeed.values) {
      final rig = _pointerRig(tester, speed);
      try {
        await _prefix(tester, rig);
        for (var n = 1; n <= 3; n++) {
          await rig.at(tester, 16 + n * 16);
          rig.move(1, Offset(116.0 + n * 16, 500));
        }
        final turningX = rig.input.cursor.dx;
        var previous = turningX;
        for (var n = 1; n <= 3; n++) {
          await rig.at(tester, 64 + n * 16);
          rig.move(1, Offset(164.0 - n * 16, 500));
          expect(rig.input.cursor.dx, lessThan(previous));
          previous = rig.input.cursor.dx;
        }
        rig.up(1, const Offset(116, 500));
        _expectWire(rig);
        _expectReleased(rig);
        expect(rig.buttons, isEmpty);
        expect(rig.wheels, isEmpty);
        _emit('reversal', {
          'preset': speed.name,
          'turningCursorX': turningX,
          'reverseSourcePixelTravel':
              (turningX - rig.input.cursor.dx) * _pointerSource.width,
          ...rig.summary(),
        });
      } finally {
        await rig.finish(tester);
      }
    }
  });

  testWidgets('baseline duplicate timestamps remain finite across presets', (
    tester,
  ) async {
    for (final speed in AndroidPointerSpeed.values) {
      final rig = _pointerRig(tester, speed);
      try {
        await _prefix(tester, rig);
        rig.move(1, const Offset(120, 500));
        rig.move(1, const Offset(124, 500));
        await rig.at(tester, 20);
        rig.move(1, const Offset(128, 500));
        rig.up(1, const Offset(128, 500));
        _expectWire(rig);
        _expectReleased(rig);
        expect(rig.buttons, isEmpty);
        expect(rig.wheels, isEmpty);
        _emit('duplicate-timestamps', {
          'preset': speed.name,
          'sourcePixelTravel':
              (rig.input.cursor.dx - _pointerStart.dx) * _pointerSource.width,
          ...rig.summary(),
        });
      } finally {
        await rig.finish(tester);
      }
    }
  });

  testWidgets(
    'reset cancels pending tap and rejects old pointer continuation',
    (tester) async {
      final rig = _pointerRig(tester, AndroidPointerSpeed.standard);
      try {
        rig.down(1, const Offset(100, 500));
        await rig.at(tester, 40);
        rig.up(1, const Offset(100, 500));
        expect(rig.input.clickPending, isTrue);
        rig.input.reset();
        await tester.pump(const Duration(seconds: 2));
        rig.move(1, const Offset(300, 500));
        rig.up(1, const Offset(300, 500));
        rig.input.cancel(1);
        expect(rig.events, isEmpty);
        _expectReleased(rig);
        rig.down(2, const Offset(100, 500));
        rig.up(2, const Offset(100, 500));
        await tester.pump(rig.input.doubleTap);
        _expectWire(rig);
        _expectReleased(rig);
        expect(rig.buttons.map((e) => e['down']), [true, false]);
        _emit('reset-pending-tap', {
          'oldPointerEventsAfterReset': 0,
          ...rig.summary(),
        });
      } finally {
        await rig.finish(tester);
      }
    },
  );

  testWidgets(
    'drag up and cancel each release once without the withheld click',
    (tester) async {
      for (final cancelled in [false, true]) {
        final rig = _pointerRig(tester, AndroidPointerSpeed.standard);
        try {
          await _beginDrag(tester, rig);
          await rig.at(tester, 132);
          if (cancelled) {
            rig.input.cancel(2);
          } else {
            rig.up(2, const Offset(120, 500));
          }
          await tester.pump(rig.input.doubleTap);
          _expectWire(rig);
          _expectReleased(rig);
          expect(rig.buttons.map((e) => (e['button'], e['down'])), [
            ('left', true),
            ('left', false),
          ]);
          _emit('drag-release', {'cancelled': cancelled, ...rig.summary()});
        } finally {
          await rig.finish(tester);
        }
      }
    },
  );

  testWidgets('held drag reset drops local state without replaying old input', (
    tester,
  ) async {
    final rig = _pointerRig(tester, AndroidPointerSpeed.standard);
    try {
      await _beginDrag(tester, rig);
      final countBeforeReset = rig.events.length;
      rig.input.reset();
      _expectReleased(rig);
      rig.move(2, const Offset(300, 500));
      rig.up(2, const Offset(300, 500));
      rig.input.cancel(2);
      await tester.pump(const Duration(seconds: 2));
      expect(rig.events.length, countBeforeReset);
      // reset intentionally delegates the remote input.reset to the session.
      // This controller-only fixture must not fabricate that production call.
      expect(rig.buttons.length, 1);
      _expectWire(rig);
      _emit('reset-held-drag', {
        'externalSessionResetRequired': true,
        'remoteSessionResetExercised': false,
        'oldPointerEventsAfterReset': rig.events.length - countBeforeReset,
        ...rig.summary(),
      });
    } finally {
      await rig.finish(tester);
    }
  });
}
