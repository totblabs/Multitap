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
