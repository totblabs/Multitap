import 'package:flutter_test/flutter_test.dart';
import 'package:multitap_client/data/connection_feedback.dart';

void main() {
  test('only known retryable media and display failures offer settings', () {
    for (final code in [
      'connection_overloaded',
      'client_backpressure',
      'host_backpressure',
      'media_backpressure',
    ]) {
      expect(
        connectionRecovery(code, canRetry: true),
        ConnectionRecovery.quality,
      );
    }
    for (final code in [
      'frame_layout_unsupported',
      'frame_memory_insufficient',
      'encoder_format_unsupported',
      'unsupported_display_mode',
      'initial_viewport_mismatch',
    ]) {
      expect(
        connectionRecovery(code, canRetry: true),
        ConnectionRecovery.display,
      );
      expect(
        connectionRecovery(code, canRetry: false),
        ConnectionRecovery.none,
      );
      expect(
        connectionRecovery(code, canRetry: true, takeover: true),
        ConnectionRecovery.none,
      );
    }
    for (final code in [
      null,
      'authorization_revoked',
      'session_expired',
      'session_idle_timeout',
      'pc_login_replaced',
      'host_revoked',
      'invalid_control',
      'invalid_media',
      'desktop_busy',
      'unknown',
    ]) {
      expect(connectionRecovery(code, canRetry: true), ConnectionRecovery.none);
    }
  });

  test(
    'elapsed display handles clock boundaries without becoming a deadline',
    () {
      expect(connectionElapsed(const Duration(seconds: -1)), '00:00');
      expect(connectionElapsed(const Duration(seconds: 59)), '00:59');
      expect(connectionElapsed(const Duration(seconds: 60)), '01:00');
      expect(connectionElapsed(const Duration(minutes: 100)), '100:00');
    },
  );
}
