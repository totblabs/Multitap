import 'package:flutter_test/flutter_test.dart';
import 'package:multitap_client/data/session_failure.dart';

const reasons = <String, String>{
  'frame_layout_unsupported': 'Host가 이 화면 크기를 처리할 수 없어요.',
  'frame_memory_unavailable': 'Host의 사용 가능한 메모리를 확인하지 못했어요.',
  'frame_memory_insufficient': 'Host의 사용 가능한 메모리가 화면 처리에 부족해요.',
  'encoder_format_unsupported': 'Host가 이 화면 크기나 형식을 지원하지 않아요.',
  'unsupported_display_mode': '가상 화면이 이 해상도·주사율을 지원하지 않아요.',
};

void main() {
  for (final entry in reasons.entries) {
    test('${entry.key} stays precise and does not automatically reconnect', () {
      final failure = SessionFailure.ended(entry.key);
      expect(failure.code, entry.key);
      expect(failure.message, startsWith(entry.value));
      expect(
        failure.canRetry,
        isTrue,
        reason: 'explicit retry remains available',
      );
      expect(failure.takeover, isFalse);
      expect(SessionFailure.transientTransportEnd(entry.key), isFalse);
      expect(SessionFailure.blocksAutomaticReconnect(entry.key), isTrue);
      expect(SessionFailure.transportCloseReason(entry.key), entry.key);
      expect(failure.message, isNot(contains('이전 크기를 유지')));
      expect(
        resolutionFailureMessage(entry.key, unchanged: true),
        '${entry.value} 이전 크기를 유지해요.',
      );
    });
  }

  test('unknown memory capacity is not described as insufficient capacity', () {
    final unknown = SessionFailure.ended('frame_memory_unavailable');
    final insufficient = SessionFailure.ended('frame_memory_insufficient');
    expect(unknown.message, contains('확인하지 못했어요'));
    expect(unknown.message, isNot(contains('부족해요')));
    expect(insufficient.message, contains('부족해요'));
    expect(insufficient.message, isNot(contains('확인하지 못했어요')));
  });

  test(
    'unknown or malformed reasons never become raw UI or close messages',
    () {
      for (final reason in <Object?>[
        null,
        true,
        <Object?>[],
        <String, Object?>{},
        'driver detail / synthetic secret',
        'frame_memory_insufficient\n',
        'FRAME_MEMORY_INSUFFICIENT',
      ]) {
        expect(resolutionFailureMessage(reason), isNull);
        expect(resolutionFailureMessage(reason, unchanged: true), isNull);
        expect(SessionFailure.transportCloseReason(reason), isNull);
        final failure = SessionFailure.ended(reason);
        expect(failure.code, 'session_ended');
        expect(failure.message, isNot(contains('synthetic secret')));
      }
    },
  );

  test('legacy worker and encoder errors remain generic', () {
    expect(SessionFailure.ended('worker_failed').code, 'capture_failed');
    expect(SessionFailure.ended('encoder_failed').code, 'encoder_failed');
    expect(resolutionFailureMessage('worker_failed'), isNull);
    expect(resolutionFailureMessage('resize_failed', unchanged: true), isNull);
    expect(SessionFailure.transientTransportEnd('host_disconnected'), isTrue);
    expect(
      SessionFailure.blocksAutomaticReconnect('host_disconnected'),
      isFalse,
    );
  });
}
