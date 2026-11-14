import 'package:flutter_test/flutter_test.dart';
import 'package:multitap_client/data/host_diagnostics.dart';

void main() {
  test('Broker diagnostics require fresh server evidence and omit unknown raw errors', () {
    final state = <String, dynamic>{
      'registered': true,
      'service': {'running': true},
      'broker': <String, dynamic>{
        'state': 'online',
        'online': true,
        'heartbeatAgeSeconds': 3,
        'failureReason': 'canary-secret',
        'token': 'canary-token',
      },
    };
    expect(hostDiagnosticsText(state), contains('서버 연결: 서버 응답 확인'));
    expect(hostDiagnosticsText(state), isNot(contains('canary')));
    for (final age in [31, -1, double.nan, double.infinity, null]) {
      (state['broker'] as Map)['heartbeatAgeSeconds'] = age;
      expect(hostDiagnosticsText(state), contains('서버 연결: 확인 필요'));
    }
    (state['broker'] as Map)['heartbeatAgeSeconds'] = 0;
    state['service'] = {'running': false};
    expect(hostDiagnosticsText(state), contains('서버 연결: 확인 필요'));
    state['service'] = {'running': true};
    state['registered'] = false;
    expect(hostDiagnosticsText(state), contains('서버 연결: 확인 필요'));
    (state['broker'] as Map)['failureReason'] = 'tls_failed';
    expect(hostDiagnosticsText(state), contains('서버 연결 실패 사유: tls_failed'));
  });
  test('The newest acknowledgement decides, capabilities or heartbeat; a heartbeat age only from an older helper', () {
    // Right after the capabilities ACK: no heartbeat yet, a fresh acknowledgement.
    final state = <String, dynamic>{
      'registered': true,
      'service': {'running': true},
      'broker': <String, dynamic>{
        'state': 'online',
        'online': true,
        'heartbeatAgeSeconds': null,
        'ackAgeSeconds': 0,
      },
    };
    expect(hostDiagnosticsText(state), contains('서버 연결: 서버 응답 확인'));
    expect(hostDiagnosticsText(state), contains('0.0초'));
    // The helper says this connection has no acknowledgement: an older
    // heartbeat's age does not stand in for it. It is still the last
    // response, as 진단 shows it.
    (state['broker'] as Map)
      ..['ackAgeSeconds'] = null
      ..['heartbeatAgeSeconds'] = 3;
    expect(hostDiagnosticsText(state), contains('서버 연결: 확인 필요'));
    expect(hostDiagnosticsText(state), contains('마지막 서버 응답 경과: 3.0초'));
    // A helper from before ackAgeSeconds names only the heartbeat's age.
    (state['broker'] as Map).remove('ackAgeSeconds');
    expect(hostDiagnosticsText(state), contains('서버 연결: 서버 응답 확인'));
    expect(hostBrokerAckAge(state['broker'] as Map), 3);
  });
  test(
    '진단 names one response: its age and its time from the same acknowledgement',
    () {
      // The capabilities ACK of a new connection: that ACK, also while an older
      // connection's heartbeat is known.
      expect(
        hostBrokerLastResponse({
          'ackAgeSeconds': 0,
          'heartbeatAgeSeconds': null,
          'lastReadyAt': 2000,
          'lastHeartbeatAt': null,
        }),
        (age: 0, at: 2000),
      );
      expect(
        hostBrokerLastResponse({
          'ackAgeSeconds': 0,
          'heartbeatAgeSeconds': 70,
          'lastReadyAt': 2000,
          'lastHeartbeatAt': 1000,
        }),
        (age: 0, at: 2000),
      );
      // A heartbeat after it: that heartbeat.
      expect(
        hostBrokerLastResponse({
          'ackAgeSeconds': 2,
          'heartbeatAgeSeconds': 2,
          'lastReadyAt': 2000,
          'lastHeartbeatAt': 3000,
        }),
        (age: 2, at: 3000),
      );
      // Connecting again (no acknowledgement of this connection), and a helper
      // before ackAgeSeconds: the last heartbeat in both.
      for (final broker in <Map<String, dynamic>>[
        {
          'ackAgeSeconds': null,
          'heartbeatAgeSeconds': 45,
          'lastReadyAt': 4000,
          'lastHeartbeatAt': 3000,
        },
        {
          'heartbeatAgeSeconds': 45,
          'lastReadyAt': 4000,
          'lastHeartbeatAt': 3000,
        },
      ]) {
        expect(hostBrokerLastResponse(broker), (age: 45, at: 3000));
      }
      expect(hostBrokerLastResponse({}), (age: null, at: null));
    },
  );
  test('Report includes only public diagnostic fields and distinguishes unknown from false', () {
    final report = hostDiagnosticsText({
      'version': '0.2.7',
      'desktopId': 'pve-149',
      'serverOrigin': 'https://cloud.example.com',
      'registered': true,
      'secret': 'secret-canary',
      'code': 'code-canary',
      'password': 'password-canary',
      'service': {'installed': null, 'running': null, 'queryError': 5},
      'display': {
        'ready': false,
        'recoveryRequired': false,
        'rebootRequired': false,
        'monitors': [
          {'id': '0', 'width': 1920, 'height': 1080, 'token': 'monitor-secret'},
        ],
      },
      'windowsAccount': {
        'configured': true,
        'username': 'account-canary',
        'secret': 'account-secret',
      },
    });
    expect(report, contains('서비스 설치: 정보 없음'));
    expect(report, contains('서비스 조회 오류: 5'));
    expect(report, contains('화면 복구: 필요 없음'));
    expect(report, contains('Windows 계정: 저장됨'));
    expect(report, isNot(contains('account-canary')));
    expect(report, contains('가상 모니터 0: 1920 × 1080'));
    for (final canary in [
      'secret-canary',
      'code-canary',
      'password-canary',
      'monitor-secret',
      'account-secret',
    ]) {
      expect(report, isNot(contains(canary)));
    }
  });
  test('External strings cannot inject additional diagnostic lines', () {
    final report = hostDiagnosticsText({
      'desktopId': 'pve-149\r\nforged-line',
      'version': '0.2.7',
    });
    expect(report.split('\n'), contains('데스크톱: pve-149  forged-line'));
    expect(report.split('\n'), isNot(contains('forged-line')));
  });
}
