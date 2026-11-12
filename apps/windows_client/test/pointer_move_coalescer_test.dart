import 'package:flutter_test/flutter_test.dart';
import 'package:multitap_client/ui/pointer_move_coalescer.dart';

void main() {
  testWidgets(
    'A move burst delivers its final position without another event',
    (tester) async {
      var now = Duration.zero;
      final sent = <(double, double)>[];
      final moves = PointerMoveCoalescer(
        send: (x, y) => sent.add((x, y)),
        elapsed: () => now,
      );
      addTearDown(moves.dispose);
      moves.move(0, 0);
      now = const Duration(milliseconds: 3);
      await tester.pump(now);
      moves.move(.2, .3);
      now = const Duration(milliseconds: 10);
      await tester.pump(const Duration(milliseconds: 7));
      moves.move(.8, .9);
      expect(sent, [(0.0, 0.0)]);
      now = const Duration(milliseconds: 16);
      await tester.pump(const Duration(milliseconds: 6));
      expect(sent, [(0.0, 0.0), (.8, .9)]);
      await tester.pump(const Duration(seconds: 1));
      expect(sent, hasLength(2));
    },
  );

  testWidgets('Flushing before a transition leaves no delayed duplicate', (
    tester,
  ) async {
    var now = Duration.zero;
    final events = <String>[];
    final moves = PointerMoveCoalescer(
      send: (x, y) => events.add('$x,$y'),
      elapsed: () => now,
    );
    addTearDown(moves.dispose);
    moves.move(0, 0);
    now = const Duration(milliseconds: 1);
    moves.move(.2, .3);
    moves.flush();
    events.add('key-down');
    moves.move(.4, .5, immediate: true);
    events.add('button-up');
    await tester.pump(const Duration(milliseconds: 30));
    expect(events, ['0.0,0.0', '0.2,0.3', 'key-down', '0.4,0.5', 'button-up']);
  });

  testWidgets('Discard and reuse never send a retired pending position', (
    tester,
  ) async {
    var now = Duration.zero;
    final sent = <(double, double)>[];
    final moves = PointerMoveCoalescer(
      send: (x, y) => sent.add((x, y)),
      elapsed: () => now,
    );
    addTearDown(moves.dispose);
    moves.move(0, 0);
    now = const Duration(milliseconds: 1);
    moves.move(.2, .3);
    moves.discard();
    moves.move(.8, .9);
    await tester.pump(const Duration(milliseconds: 30));
    expect(sent, [(0.0, 0.0), (.8, .9)]);
    moves.move(.6, .7);
    moves.dispose();
    moves.move(1, 1, immediate: true);
    moves.flush();
    await tester.pump(const Duration(milliseconds: 30));
    expect(sent, hasLength(2));
  });

  test('Nonpositive intervals are rejected', () {
    expect(
      () => PointerMoveCoalescer(send: (_, _) {}, interval: Duration.zero),
      throwsArgumentError,
    );
  });
}
