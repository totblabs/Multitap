import 'package:flutter_test/flutter_test.dart';
import 'package:multitap_client/ui/clipboard_transfer_notice.dart';

const _gb = 1073741824, _mb = 1048576;

void main() {
  test('Sizes and remaining time read in plain units', () {
    expect(clipboardBytes(512), '512 B');
    expect(clipboardBytes(2048), '2 KB');
    expect(clipboardBytes((1.2 * _gb).round()), '1.2 GB');
    expect(clipboardBytes(30 * _mb), '30.0 MB');
    expect(clipboardRemaining(const Duration(seconds: 40)), '약 40초 남음');
    expect(clipboardRemaining(const Duration(milliseconds: 100)), '약 1초 남음');
    expect(clipboardRemaining(const Duration(seconds: 150)), '약 3분 남음');
    expect(clipboardRemaining(const Duration(minutes: 80)), '약 1시간 20분 남음');
    expect(clipboardRemaining(const Duration(days: 2)), isNull);
    expect(clipboardRemaining(null), isNull);
  });

  test('The estimate waits for warm-up and a steady rate', () {
    final steady = TransferEstimator();
    const total = 100 * _mb;
    for (var t = 0; t <= 6000; t += 250) {
      final bytes = t * 1000; // 1 MB/s (decimal)
      steady.add(Duration(milliseconds: t), bytes);
      final left = steady.remaining(Duration(milliseconds: t), bytes, total);
      if (t < 3000) expect(left, isNull, reason: 'warm-up at $t ms');
      if (t == 6000) expect(left!.inSeconds, inInclusiveRange(90, 100));
    }
    expect(
      steady.remaining(const Duration(seconds: 6), total, total),
      isNull,
      reason: 'nothing left',
    );
    final swinging = TransferEstimator();
    var bytes = 0;
    for (var t = 0; t <= 8000; t += 250) {
      // Bursts and stalls of 2 s each.
      if ((t ~/ 2000).isEven) bytes += 2 * _mb;
      swinging.add(Duration(milliseconds: t), bytes);
    }
    expect(
      swinging.remaining(const Duration(seconds: 8), bytes, 500 * _mb),
      isNull,
    );
  });

  test('a file name is shortened in the middle and keeps its extension', () {
    expect(middleEllipsize('short.txt', 20), 'short.txt');
    final long = middleEllipsize('${'a' * 80}.mp4', 20);
    expect(long.length, 20);
    expect(long, contains('…'));
    expect(long, endsWith('.mp4'));
    expect(long, startsWith('aaaa'));
    expect(middleEllipsize('abcdefghij', 4), 'abcdefghij', reason: 'too small to shorten');
  });

  test('the smoothed rate is exposed once measured', () {
    final estimate = TransferEstimator();
    expect(estimate.rate, isNull);
    for (var t = 0; t <= 2000; t += 250) {
      estimate.add(Duration(milliseconds: t), t * 1000);
    }
    expect(estimate.rate, closeTo(1000000, 100000));
  });
}
