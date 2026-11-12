import 'package:fake_async/fake_async.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:multitap_client/data/console_pointer.dart';

void main() {
  test(
    '1000 Hz motion is latest-only while button/wheel edges remain ordered',
    () {
      fakeAsync((clock) {
        final sent = <(int, int, int)>[];
        final pacer = ConsolePointerPacer(
          (x, y, mask) => sent.add((x, y, mask)),
        );
        for (var i = 0; i < 1000; i++) {
          pacer.motion(i, i, 0);
          clock.elapse(const Duration(milliseconds: 1));
        }
        expect(sent.length, lessThanOrEqualTo(120));
        pacer.edge(1000, 1000, 1);
        expect(sent.sublist(sent.length - 2), [(999, 999, 0), (1000, 1000, 1)]);
        pacer.motion(1001, 1002, 1);
        pacer.edge(1003, 1004, 9);
        pacer.edge(1003, 1004, 1);
        pacer.edge(1005, 1006, 0);
        expect(sent.sublist(sent.length - 4), [
          (1001, 1002, 1),
          (1003, 1004, 9),
          (1003, 1004, 1),
          (1005, 1006, 0),
        ]);
        final count = sent.length;
        clock.elapse(const Duration(seconds: 1));
        expect(sent.length, count);
        pacer.dispose();
      });
    },
  );

  test(
    'focus/session/geometry reset discards late motion; fresh owner works',
    () {
      fakeAsync((clock) {
        final sent = <(int, int, int)>[];
        final pacer = ConsolePointerPacer(
          (x, y, mask) => sent.add((x, y, mask)),
        );
        pacer.motion(1, 2, 1);
        pacer.reset();
        clock.elapse(const Duration(seconds: 1));
        expect(sent, isEmpty);
        pacer.motion(3, 4, 0);
        clock.elapse(ConsolePointerPacer.interval);
        expect(sent, [(3, 4, 0)]);
        pacer.motion(5, 6, 0);
        pacer.dispose();
        clock.elapse(const Duration(seconds: 1));
        expect(sent, [(3, 4, 0)]);
      });
    },
  );

  test('a synchronous end during flush cannot emit a later button edge', () {
    fakeAsync((clock) {
      final sent = <int>[];
      late ConsolePointerPacer pacer;
      pacer = ConsolePointerPacer((x, y, mask) {
        sent.add(mask);
        pacer.reset();
      });
      pacer.motion(1, 2, 0);
      pacer.edge(3, 4, 1);
      expect(sent, [0]);
      clock.elapse(const Duration(seconds: 1));
      expect(sent, [0]);
      pacer.dispose();
    });
  });
}
