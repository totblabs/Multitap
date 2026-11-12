import 'package:flutter_test/flutter_test.dart';
import 'package:multitap_client/data/emergency_console_session.dart';

void main() {
  test('invalid upstream explains the connection information failure without exposing details', () {
    expect(
      consoleFailureMessage('console_invalid_upstream'),
      'Proxmox에서 Console 연결 정보를 받지 못했어요. 관리자에게 알려 주세요.',
    );
  });
}
