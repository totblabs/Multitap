import 'dart:async';

import 'package:flutter/foundation.dart';
import 'package:flutter/gestures.dart';
import 'package:flutter/services.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:multitap_client/android/android_pen.dart';

const _ready = {'type': 'ready', 'protocol': 'pen-signals-v1'};
const _channel = 'multitap/pen-input';
const _codec = StandardMethodCodec();

PointerEvent _event(
  String phase, {
  PointerDeviceKind kind = PointerDeviceKind.stylus,
  int device = 7,
  int pointer = 10,
  int time = 10,
  Offset point = const Offset(20, 30),
  int buttons = 0,
}) {
  final stamp = Duration(microseconds: time);
  return switch (phase) {
    'down' => PointerDownEvent(
      kind: kind,
      device: device,
      pointer: pointer,
      timeStamp: stamp,
      position: point,
      buttons: buttons,
    ),
    'move' => PointerMoveEvent(
      kind: kind,
      device: device,
      pointer: pointer,
      timeStamp: stamp,
      position: point,
      buttons: buttons,
    ),
    'up' => PointerUpEvent(
      kind: kind,
      device: device,
      pointer: pointer,
      timeStamp: stamp,
      position: point,
      buttons: buttons,
    ),
    'cancel' => PointerCancelEvent(
      kind: kind,
      device: device,
      pointer: pointer,
      timeStamp: stamp,
      position: point,
      buttons: buttons,
    ),
    'hover' => PointerHoverEvent(
      kind: kind,
      device: device,
      pointer: pointer,
      timeStamp: stamp,
      position: point,
      buttons: buttons,
    ),
    'removed' => PointerRemovedEvent(
      kind: kind,
      device: device,
      pointer: pointer,
      timeStamp: stamp,
      position: point,
    ),
    _ => throw ArgumentError(phase),
  };
}

class _Rig {
  _Rig() {
    input = AndroidPenInput(
      project: (point) =>
          point.dx < 0 || point.dy < 0 || point.dx > 100 || point.dy > 100
          ? null
          : point / 100,
      clamp: (point) => Offset(
        (point.dx / 100).clamp(0.0, 1.0),
        (point.dy / 100).clamp(0.0, 1.0),
      ),
      send: (event) {
        events.add(Map<String, dynamic>.from(event));
        order.add(
          event['event'] == 'button'
              ? '${event['button']}:${event['down']}'
              : 'move',
        );
        onSend?.call(event);
      },
      takeOver: () {
        takeovers++;
        order.add('takeover');
      },
    );
  }
  late final AndroidPenInput input;
  final events = <Map<String, dynamic>>[];
  final order = <String>[];
  int takeovers = 0;
  void Function(Map<String, dynamic>)? onSend;
  List<(String, bool)> get buttons => [
    for (final event in events)
      if (event['event'] == 'button')
        (event['button'] as String, event['down'] as bool),
  ];
  void wire() {
    for (final event in events) {
      expect(event['type'], 'input');
      expect(event['event'], isIn(['move', 'button']));
      expect(
        event.keys.toSet(),
        event['event'] == 'move'
            ? {'type', 'event', 'x', 'y'}
            : {'type', 'event', 'button', 'down', 'x', 'y'},
      );
      for (final key in ['x', 'y']) {
        expect((event[key] as double).isFinite, isTrue);
        expect(event[key] as double, inInclusiveRange(0.0, 1.0));
      }
    }
  }
}

Map<String, Object> _exit({
  int device = 7,
  int time = 40,
  String kind = 'stylus',
}) => {'type': 'exit', 'device': device, 'kind': kind, 'timeMicros': time};

Future<void> _settleChannels() async {
  for (var n = 0; n < 6; n++) {
    await Future<void>.delayed(Duration.zero);
  }
}

Future<void> _cancelSubscription(
  StreamSubscription<Object?> subscription,
) async {
  await subscription.cancel().timeout(const Duration(seconds: 2));
  await _settleChannels();
}

Future<void> _native(ByteData? packet) async {
  final reply = Completer<void>();
  TestWidgetsFlutterBinding.ensureInitialized().channelBuffers.push(
    _channel,
    packet,
    (_) {
      reply.complete();
    },
  );
  await reply.future.timeout(const Duration(seconds: 2));
  await _settleChannels();
}

List<FlutterErrorDetails> _captureFlutterErrors() {
  final errors = <FlutterErrorDetails>[];
  final previous = FlutterError.onError;
  FlutterError.onError = errors.add;
  addTearDown(() {
    FlutterError.onError = previous;
    expect(errors, isEmpty);
  });
  return errors;
}

void main() {
  TestWidgetsFlutterBinding.ensureInitialized();
  test('contact is absolute Down/Up even without a contact button bit', () {
    final rig = _Rig();
    expect(rig.input.handle(_event('down')), isTrue);
    expect(rig.input.contacting, isTrue);
    expect(rig.events.first['x'], .2);
    expect(rig.events.first['y'], .3);
    rig.input.handle(_event('move', time: 20, point: const Offset(40, 50)));
    rig.input.handle(_event('up', time: 30, point: const Offset(40, 50)));
    expect(rig.buttons, [('left', true), ('left', false)]);
    expect(
      rig.input.active,
      isFalse,
      reason: 'Missing plugin fallback retires on up',
    );
    expect(rig.input.buttons, 0);
    expect(rig.takeovers, 1);
    rig.wire();
  });

  test('fallback hover never clicks or permanently suppresses a finger', () {
    final rig = _Rig();
    rig.input.signal({..._ready, 'extra': true});
    expect(
      rig.input.handle(_event('hover', buttons: kPrimaryStylusButton)),
      isTrue,
    );
    expect(rig.buttons, isEmpty);
    expect(rig.input.active, isFalse);
    expect(
      rig.input.handle(
        _event(
          'down',
          kind: PointerDeviceKind.touch,
          device: 1,
          pointer: 1,
          time: 20,
        ),
      ),
      isFalse,
    );
    expect(
      rig.input.handle(
        _event(
          'up',
          kind: PointerDeviceKind.touch,
          device: 1,
          pointer: 1,
          time: 30,
        ),
      ),
      isFalse,
    );
    rig.wire();
  });

  test(
    'native proximity suppresses palms until their actual up after exit',
    () {
      final rig = _Rig();
      rig.input.signal(_ready);
      rig.input.handle(_event('hover'));
      expect(rig.input.active, isTrue);
      expect(rig.input.contacting, isFalse);
      expect(
        rig.input.handle(
          _event('down', kind: PointerDeviceKind.touch, pointer: 1),
        ),
        isTrue,
      );
      rig.input.signal(_exit());
      expect(rig.input.active, isFalse);
      expect(
        rig.input.handle(
          _event('move', kind: PointerDeviceKind.touch, pointer: 1, time: 50),
        ),
        isTrue,
      );
      expect(
        rig.input.handle(
          _event('up', kind: PointerDeviceKind.touch, pointer: 1, time: 60),
        ),
        isTrue,
      );
      expect(
        rig.input.handle(
          _event('down', kind: PointerDeviceKind.touch, pointer: 2, time: 70),
        ),
        isFalse,
      );
      expect(rig.buttons, isEmpty);
    },
  );

  test('barrel buttons change during move and hover; right overrides left', () {
    final rig = _Rig();
    rig.input.signal(_ready);
    rig.input.handle(_event('down', buttons: kStylusContact));
    rig.input.signal(_ready);
    expect(
      rig.input.contacting,
      isTrue,
      reason: 'Duplicate ready is idempotent',
    );
    rig.input.handle(
      _event('move', time: 20, buttons: kStylusContact | kPrimaryStylusButton),
    );
    expect(rig.input.buttons, kSecondaryMouseButton);
    rig.input.handle(
      _event(
        'move',
        time: 30,
        buttons: kStylusContact | kSecondaryStylusButton,
      ),
    );
    expect(rig.input.buttons, kPrimaryMouseButton | kMiddleMouseButton);
    rig.input.handle(_event('up', time: 40));
    rig.input.handle(
      _event('hover', pointer: 0, time: 50, buttons: kPrimaryStylusButton),
    );
    rig.input.handle(_event('hover', pointer: 0, time: 60));
    expect(rig.buttons, [
      ('left', true),
      ('left', false),
      ('right', true),
      ('right', false),
      ('left', true),
      ('middle', true),
      ('left', false),
      ('middle', false),
      ('right', true),
      ('right', false),
    ]);
    for (var n = 0; n < rig.order.length; n++) {
      if (rig.order[n].contains(':')) {
        expect(rig.order.take(n), contains('move'));
      }
    }
    rig.input.signal(_exit(time: 70));
    rig.wire();
  });

  test(
    'inverted stylus remains basic mouse fallback without an eraser claim',
    () {
      final rig = _Rig();
      rig.input.handle(_event('down', kind: PointerDeviceKind.invertedStylus));
      rig.input.handle(
        _event('up', kind: PointerDeviceKind.invertedStylus, time: 20),
      );
      expect(rig.buttons, [('left', true), ('left', false)]);
      rig.wire();
    },
  );

  test('cancel without a current position retains the cached pen anchor', () {
    for (final terminal in ['cancel', 'removed']) {
      final rig = _Rig();
      rig.input.handle(_event('down', point: const Offset(60, 70)));
      rig.input.handle(_event(terminal, time: 20, point: Offset.zero));
      final lastMove = rig.events.lastWhere((e) => e['event'] == 'move');
      expect(
        (lastMove['x'], lastMove['y']),
        (.6, .7),
        reason: 'cancellation must release at the cached pen anchor',
      );
      expect(rig.input.active, isFalse);
      rig.wire();
    }
  });

  test('pen cancel and removal fence pending work before owned release', () {
    for (final terminal in ['cancel', 'removed']) {
      final rig = _Rig();
      rig.input.handle(_event('down'));
      final start = rig.order.length;
      rig.input.handle(_event(terminal, time: 20));
      expect(rig.order.skip(start), ['takeover', 'move', 'left:false']);
      expect(rig.input.active, isFalse);
      expect(rig.input.buttons, 0);
      final count = rig.events.length;
      rig.input.handle(_event('up', time: 30));
      expect(rig.events.length, count);
      rig.wire();
    }
  });

  test(
    'stale time, foreign device and second active pen cannot mutate owner',
    () {
      final rig = _Rig();
      rig.input.handle(_event('down', time: 100));
      final count = rig.events.length;
      rig.input.handle(_event('move', time: 99));
      rig.input.handle(_event('down', device: 8, pointer: 11, time: 200));
      rig.input.handle(_event('up', pointer: 11, time: 110));
      expect(rig.events.length, count);
      expect(rig.input.contacting, isTrue);
      rig.input.handle(_event('up', time: 120));
      expect(rig.input.active, isFalse);
      final retired = rig.events.length;
      rig.input.handle(_event('move', time: 130));
      rig.input.handle(_event('hover', time: 119));
      expect(rig.events.length, retired);
      expect(
        rig.input.handle(_event('down', device: 8, pointer: 11, time: 140)),
        isTrue,
      );
      expect(rig.input.contacting, isTrue);
      rig.input.release();
    },
  );

  test(
    'outside pen never falls into fingers; owned outside up uses edge anchor',
    () {
      final rig = _Rig();
      expect(
        rig.input.handle(_event('down', point: const Offset(-10, 20))),
        isTrue,
      );
      expect(rig.events, isEmpty);
      expect(rig.input.active, isFalse);
      rig.input.handle(_event('down', time: 20));
      rig.input.handle(_event('up', time: 30, point: const Offset(120, -10)));
      expect(rig.buttons, [('left', true), ('left', false)]);
      expect(rig.events.last['x'], 1.0);
      expect(rig.events.last['y'], 0.0);
      rig.wire();
    },
  );

  test(
    'mouse drag keeps hover; explicit pen and mouse takeovers fence before ups',
    () {
      final rig = _Rig();
      expect(
        rig.input.handle(
          _event(
            'down',
            kind: PointerDeviceKind.mouse,
            device: 1,
            pointer: 1,
            buttons: kPrimaryMouseButton,
          ),
        ),
        isFalse,
      );
      expect(
        rig.input.handle(
          _event('hover', kind: PointerDeviceKind.mouse, device: 2, pointer: 0),
        ),
        isFalse,
        reason: 'Another mouse cannot clear the held device',
      );
      rig.input.signal(_ready);
      rig.input.handle(_event('hover', time: 20));
      expect(rig.events, isEmpty);
      rig.input.handle(_event('down', time: 30));
      expect(rig.takeovers, 1);
      final count = rig.events.length;
      expect(
        rig.input.handle(
          _event(
            'down',
            kind: PointerDeviceKind.mouse,
            device: 1,
            pointer: 2,
            time: 25,
            buttons: kPrimaryMouseButton,
          ),
        ),
        isTrue,
      );
      expect(rig.takeovers, 1, reason: 'Stale mouse cannot steal a new pen');
      expect(
        rig.input.handle(
          _event(
            'move',
            kind: PointerDeviceKind.mouse,
            device: 1,
            pointer: 1,
            time: 35,
            buttons: kPrimaryMouseButton,
          ),
        ),
        isTrue,
      );
      expect(
        rig.input.contacting,
        isTrue,
        reason: 'Old held move is not a fresh mouse press',
      );
      expect(
        rig.input.handle(
          _event(
            'up',
            kind: PointerDeviceKind.mouse,
            device: 1,
            pointer: 1,
            time: 40,
          ),
        ),
        isTrue,
      );
      expect(
        rig.input.handle(
          _event(
            'hover',
            kind: PointerDeviceKind.mouse,
            device: 1,
            pointer: 0,
            time: 50,
          ),
        ),
        isTrue,
      );
      expect(rig.events.length, count);
      final orderStart = rig.order.length;
      expect(
        rig.input.handle(
          _event(
            'down',
            kind: PointerDeviceKind.mouse,
            device: 1,
            pointer: 2,
            time: 60,
            buttons: kPrimaryMouseButton,
          ),
        ),
        isFalse,
      );
      expect(rig.order.skip(orderStart), ['takeover', 'move', 'left:false']);
      expect(rig.input.active, isFalse);
      rig.wire();
    },
  );

  test('real wheel is an explicit mouse takeover, not passive hover', () {
    final rig = _Rig();
    rig.input.handle(_event('down'));
    final start = rig.order.length;
    expect(
      rig.input.handle(
        const PointerScrollEvent(
          kind: PointerDeviceKind.mouse,
          device: 1,
          timeStamp: Duration(microseconds: 20),
          position: Offset(20, 30),
          scrollDelta: Offset(0, 20),
        ),
      ),
      isFalse,
    );
    expect(rig.order.skip(start), ['takeover', 'move', 'left:false']);
  });

  test(
    'retired held mouse cannot revive, while wheel and a fresh device hand off',
    () {
      final rig = _Rig();
      expect(
        rig.input.handle(
          _event(
            'down',
            kind: PointerDeviceKind.mouse,
            device: 1,
            pointer: 1,
            time: 10,
            buttons: kPrimaryMouseButton,
          ),
        ),
        isFalse,
      );
      rig.input.handle(_event('down', time: 20));
      final count = rig.events.length;
      expect(
        rig.input.handle(
          _event(
            'move',
            kind: PointerDeviceKind.mouse,
            device: 1,
            pointer: 1,
            time: 30,
            buttons: kPrimaryMouseButton,
          ),
        ),
        isTrue,
      );
      expect(
        rig.input.handle(
          _event(
            'down',
            kind: PointerDeviceKind.mouse,
            device: 1,
            pointer: 2,
            time: 31,
            buttons: kPrimaryMouseButton,
          ),
        ),
        isTrue,
      );
      expect(rig.events.length, count);
      expect(rig.input.contacting, isTrue);
      expect(
        rig.input.handle(
          const PointerScrollEvent(
            kind: PointerDeviceKind.mouse,
            device: 1,
            timeStamp: Duration(microseconds: 32),
            position: Offset(20, 30),
            scrollDelta: Offset(0, 20),
          ),
        ),
        isFalse,
      );
      expect(rig.buttons, [('left', true), ('left', false)]);
      expect(
        rig.input.handle(
          _event(
            'move',
            kind: PointerDeviceKind.mouse,
            device: 1,
            pointer: 1,
            time: 33,
            buttons: kPrimaryMouseButton,
          ),
        ),
        isTrue,
      );
      expect(
        rig.input.handle(
          _event(
            'down',
            kind: PointerDeviceKind.mouse,
            device: 2,
            pointer: 3,
            time: 34,
            buttons: kPrimaryMouseButton,
          ),
        ),
        isFalse,
      );
      expect(
        rig.input.handle(
          _event(
            'up',
            kind: PointerDeviceKind.mouse,
            device: 1,
            pointer: 1,
            time: 35,
          ),
        ),
        isTrue,
        reason: 'Old up must not reach the other mouse global button state',
      );
      rig.input.handle(_event('hover', time: 36));
      expect(rig.input.active, isFalse, reason: 'The fresh mouse remains held');
      expect(
        rig.input.handle(
          _event(
            'up',
            kind: PointerDeviceKind.mouse,
            device: 2,
            pointer: 3,
            time: 37,
          ),
        ),
        isFalse,
      );
      rig.input.handle(_event('down', pointer: 11, time: 40));
      expect(
        rig.input.handle(
          _event(
            'down',
            kind: PointerDeviceKind.mouse,
            device: 1,
            pointer: 4,
            time: 50,
            buttons: kPrimaryMouseButton,
          ),
        ),
        isFalse,
        reason: 'Observed release permits a fresh down on the same device',
      );
      rig.wire();
    },
  );

  test('global reset does not turn an old held mouse into a fresh press', () {
    final rig = _Rig();
    rig.input.handle(
      _event(
        'down',
        kind: PointerDeviceKind.mouse,
        device: 1,
        pointer: 1,
        time: 10,
        buttons: kPrimaryMouseButton,
      ),
    );
    rig.input.handle(_event('down', time: 20));
    rig.input.reset();
    expect(
      rig.input.handle(
        _event(
          'move',
          kind: PointerDeviceKind.mouse,
          device: 1,
          pointer: 1,
          time: 30,
          buttons: kPrimaryMouseButton,
        ),
      ),
      isTrue,
    );
    expect(
      rig.input.handle(
        _event(
          'up',
          kind: PointerDeviceKind.mouse,
          device: 1,
          pointer: 1,
          time: 40,
        ),
      ),
      isTrue,
    );
    expect(
      rig.input.handle(
        _event(
          'down',
          kind: PointerDeviceKind.mouse,
          device: 1,
          pointer: 2,
          time: 50,
          buttons: kPrimaryMouseButton,
        ),
      ),
      isFalse,
    );
  });

  test(
    'only a nonstale zero-button or cancel observation frees a retired mouse',
    () {
      for (final ending in ['hover', 'move', 'cancel']) {
        final rig = _Rig();
        rig.input.handle(
          _event(
            'down',
            kind: PointerDeviceKind.mouse,
            device: 1,
            pointer: 1,
            time: 10,
            buttons: kPrimaryMouseButton,
          ),
        );
        rig.input.handle(_event('down', time: 30));
        expect(
          rig.input.handle(
            _event(
              'up',
              kind: PointerDeviceKind.mouse,
              device: 1,
              pointer: 1,
              time: 20,
            ),
          ),
          isTrue,
        );
        expect(
          rig.input.handle(
            _event(
              'move',
              kind: PointerDeviceKind.mouse,
              device: 1,
              pointer: 1,
              time: 40,
              buttons: kPrimaryMouseButton,
            ),
          ),
          isTrue,
        );
        expect(rig.input.contacting, isTrue);
        expect(
          rig.input.handle(
            _event(
              ending,
              kind: PointerDeviceKind.mouse,
              device: 1,
              pointer: 1,
              time: 45,
            ),
          ),
          isTrue,
        );
        expect(
          rig.input.handle(
            _event(
              'down',
              kind: PointerDeviceKind.mouse,
              device: 1,
              pointer: 2,
              time: 50,
              buttons: kPrimaryMouseButton,
            ),
          ),
          isFalse,
        );
        expect(rig.input.active, isFalse);
        rig.wire();
      }
    },
  );

  test(
    'pen takeover cancels forwarded fingers but never reuses held palm ids',
    () {
      final rig = _Rig();
      expect(
        rig.input.handle(
          _event('down', kind: PointerDeviceKind.touch, pointer: 1),
        ),
        isFalse,
      );
      rig.input.handle(_event('down', time: 20));
      rig.input.handle(_event('up', time: 30));
      expect(
        rig.input.handle(
          _event('move', kind: PointerDeviceKind.touch, pointer: 1, time: 40),
        ),
        isTrue,
      );
      expect(
        rig.input.handle(
          _event('cancel', kind: PointerDeviceKind.touch, pointer: 1, time: 50),
        ),
        isTrue,
      );
      expect(
        rig.input.handle(
          _event('down', kind: PointerDeviceKind.touch, pointer: 2, time: 60),
        ),
        isFalse,
      );
    },
  );

  test(
    'exit accepts only exact ready owner kind/device and nonstale timestamp',
    () {
      final rig = _Rig();
      rig.input.signal(_ready);
      rig.input.handle(_event('down', time: 100));
      rig.input.signal(_exit(time: 99));
      rig.input.signal(_exit(device: 8, time: 110));
      rig.input.signal(_exit(kind: 'invertedStylus', time: 110));
      rig.input.signal({..._exit(time: 110), 'extra': true});
      expect(rig.input.contacting, isTrue);
      rig.input.signal(_exit(time: 120));
      expect(rig.input.active, isFalse);
      expect(rig.buttons, [('left', true), ('left', false)]);
      final count = rig.events.length;
      rig.input.handle(_event('hover', time: 119));
      rig.input.handle(_event('hover', time: 120));
      expect(rig.events.length, count);
    },
  );

  test(
    'reset is clear-only, retains readiness, and blocks stale prior events',
    () {
      final rig = _Rig();
      rig.input.signal(_ready);
      rig.input.handle(_event('down', time: 100));
      final count = rig.events.length;
      rig.input.reset();
      expect(
        rig.events.length,
        count,
        reason: 'Outer input.reset owns release',
      );
      expect(rig.input.buttons, 0);
      rig.input.handle(_event('move', time: 101));
      rig.input.handle(_event('hover', time: 99));
      expect(rig.events.length, count);
      rig.input.handle(_event('hover', time: 110));
      expect(
        rig.input.active,
        isTrue,
        reason: 'Readiness is not an input owner',
      );
      expect(
        rig.input.handle(
          _event('down', kind: PointerDeviceKind.touch, pointer: 1),
        ),
        isTrue,
      );
      rig.input.reset();
    },
  );

  test(
    'signal failure releases owned buttons and falls back without palm lock',
    () {
      final rig = _Rig();
      rig.input.signal(_ready);
      rig.input.handle(_event('down'));
      rig.input.signal(const {'type': 'unavailable'});
      expect(rig.input.active, isFalse);
      expect(rig.buttons, [('left', true), ('left', false)]);
      rig.input.handle(_event('hover', time: 20));
      expect(rig.input.active, isFalse);
      expect(
        rig.input.handle(
          _event('down', kind: PointerDeviceKind.touch, pointer: 1, time: 30),
        ),
        isFalse,
      );
    },
  );

  test('reentrant reset after a move prevents a late pen button down', () {
    final rig = _Rig();
    rig.onSend = (_) => rig.input.reset();
    rig.input.handle(_event('down'));
    expect(rig.events.length, 1);
    expect(rig.buttons, isEmpty);
    expect(rig.input.active, isFalse);
    expect(rig.input.buttons, 0);
  });

  test(
    'optional channel replays only ready and clears it on failure/end',
    () async {
      final errors = _captureFlutterErrors();
      final messenger =
          TestDefaultBinaryMessengerBinding.instance.defaultBinaryMessenger;
      final calls = <String>[];
      messenger.setMockMethodCallHandler(const MethodChannel(_channel), (
        call,
      ) async {
        calls.add(call.method);
        return null;
      });
      final first = <Object?>[], second = <Object?>[], lateEvents = <Object?>[];
      final subscriptions = <StreamSubscription<Object?>>[];
      try {
        subscriptions.add(AndroidPenSignals.events.listen(first.add));
        await _settleChannels();
        await _native(_codec.encodeSuccessEnvelope(_ready));
        subscriptions.add(AndroidPenSignals.events.listen(second.add));
        await _settleChannels();
        expect(first, [_ready]);
        expect(second, [_ready]);
        await _native(_codec.encodeSuccessEnvelope(_exit()));
        subscriptions.add(AndroidPenSignals.events.listen(lateEvents.add));
        await _settleChannels();
        expect(lateEvents, [_ready], reason: 'No owner or exit replay');
        expect(calls.where((call) => call == 'listen').length, 1);
        await _native(_codec.encodeErrorEnvelope(code: 'lost'));
        expect(first.last, {'type': 'unavailable'});
        final afterFailure = <Object?>[];
        final d = AndroidPenSignals.events.listen(afterFailure.add);
        try {
          await _settleChannels();
          expect(
            afterFailure,
            isEmpty,
            reason: 'Old readiness must not replay',
          );
          await _native(_codec.encodeSuccessEnvelope(_ready));
          expect(afterFailure, [_ready]);
          await _native(null);
          expect(afterFailure.last, {'type': 'unavailable'});
        } finally {
          await _cancelSubscription(d);
        }
      } finally {
        for (final subscription in subscriptions) {
          await _cancelSubscription(subscription);
        }
        await _settleChannels();
        messenger.setMockMethodCallHandler(const MethodChannel(_channel), null);
      }
      expect(calls.where((call) => call == 'cancel').length, 1);
      expect(errors, isEmpty);
    },
    timeout: const Timeout(Duration(seconds: 15)),
  );

  test(
    'missing optional plugin yields unavailable without a Flutter error',
    () async {
      final errors = _captureFlutterErrors();
      final messenger =
          TestDefaultBinaryMessengerBinding.instance.defaultBinaryMessenger;
      messenger.setMockMethodCallHandler(const MethodChannel(_channel), (
        call,
      ) async {
        if (call.method == 'listen') throw MissingPluginException();
        return null;
      });
      final events = <Object?>[];
      final subscription = AndroidPenSignals.events.listen(events.add);
      try {
        await _settleChannels();
        expect(events, [
          {'type': 'unavailable'},
        ]);
        expect(errors, isEmpty);
      } finally {
        await _cancelSubscription(subscription);
        await _settleChannels();
        messenger.setMockMethodCallHandler(const MethodChannel(_channel), null);
      }
    },
    timeout: const Timeout(Duration(seconds: 15)),
  );
}
