// Texts that point users somewhere: the Windows account lives under Host
// settings → Windows 로그인, and closing a session says that shared USB
// devices go back too.
import 'package:flutter/material.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:multitap_client/data/client_diagnostic_codes.dart';
import 'package:multitap_client/data/session_failure.dart';
import 'package:multitap_client/data/usb_redirection.dart' show usbMessage;
import 'package:multitap_client/models/console_preparation.dart';
import 'package:multitap_client/ui/app_dialog.dart';
import 'package:multitap_client/ui/app_theme.dart';

void main() {
  // The broker's VM checks before a stream have their own codes, not the
  // general virtual_display_required ("가상 화면이 준비되지 않았어요").
  test('A stream refused by the VM checks says why, with its own code', () {
    final generic = SessionFailure.ended('unspecified_reason').message;
    final virtualDisplay =
        SessionFailure.ended('virtual_display_required').message;
    final expected = {
      'host_machine_mismatch': ('이 데스크톱의 Host가 등록된 VM과 맞지 않아요. 관리자에게 알려 주세요.', false),
      'desktop_changed': ('이 데스크톱의 등록이 바뀌었어요. 데스크톱 목록을 새로고침한 뒤 다시 연결하세요.', true),
      'desktop_unavailable': ('Proxmox에서 이 데스크톱의 VM을 찾지 못했어요. 관리자에게 알려 주세요.', false),
      'power_in_progress': ('전원 작업이 진행 중이에요. 끝난 뒤 다시 시도하세요.', true),
    };
    for (final MapEntry(key: code, value: (message, retry))
        in expected.entries) {
      final failure = SessionFailure.ended(code);
      expect(failure.code, code);
      expect(failure.message, message, reason: code);
      expect(failure.message, isNot(generic), reason: code);
      expect(failure.message, isNot(virtualDisplay), reason: code);
      expect(failure.canRetry, retry, reason: code);
      expect(relayEndCode(code, failure.code), code);
    }
  });

  test('USB messages never assume a VM: the remote computer may be a PC', () {
    expect(
      usbMessage('usb_vhci_unavailable'),
      '데스크톱에 다른 USB 가상 장치 프로그램이 있어요.\n'
      '데스크톱의 Host 설정 → 장치 → USB에서 해결할 수 있어요.',
    );
    for (final code in [
      'usb_hid_boot_denied',
      'usb_attach_failed',
      'usb_setup_failed',
      'usb_vhci_unavailable',
      'usb_driver_missing',
      'usb_driver_install_failed',
      'usb_restart_required',
      'usb_vhci_unprotected',
      'usb_vm_detached',
    ]) {
      expect(usbMessage(code), isNot(contains('VM')), reason: code);
    }
  });

  test('The Windows account is under Host settings → Windows 로그인', () {
    expect(
      consolePreparationMessage('windows_identity_required'),
      'Host 설정 → Windows 로그인 → Windows 계정을 확인하세요.',
    );
  });

  // A takeover end says who took the VM.
  test('A takeover end says who, and falls back on an older server', () {
    expect(
      takenOverMessage('same_account', null),
      startsWith('같은 계정으로 다른 창이나 기기에서 이 데스크톱에 연결해'),
    );
    expect(
      takenOverMessage('other_account', 'bob'),
      startsWith('bob 계정이 다른 창이나 기기에서'),
    );
    expect(
      takenOverMessage('console_other_account', 'bob'),
      startsWith('bob 계정이 Console에서'),
    );
    expect(
      takenOverMessage('console_same_account', 'bob'),
      startsWith('같은 계정으로 Console에서'),
    );
    // A name that cannot be shown is not: the account is still "another".
    expect(
      takenOverMessage('other_account', 'x' * 65),
      startsWith('다른 계정이 다른 창이나 기기에서'),
    );
    expect(
      takenOverMessage('other_account', 'a\nb'),
      startsWith('다른 계정이'),
    );
    expect(
      takenOverMessage(null, null),
      SessionFailure.ended('taken_over').message,
    );
  });

  testWidgets('Closing with USB devices shared says they go too', (t) async {
    late BuildContext host;
    await t.pumpWidget(
      MaterialApp(
        theme: buildAppTheme(brightness: Brightness.dark),
        home: Builder(
          builder: (context) {
            host = context;
            return const SizedBox.expand();
          },
        ),
      ),
    );
    for (final shared in [0, 2]) {
      final answer = confirmDisconnect(host, machine: 'VM', usbShared: shared);
      await t.pumpAndSettle();
      expect(
        find.textContaining('USB 장치 2개도 끊겨요'),
        shared == 0 ? findsNothing : findsOneWidget,
      );
      await t.tap(find.text('취소'));
      await t.pumpAndSettle();
      expect(await answer, isFalse);
    }
  });

  // Closing also ends mapped folders and drops
  // documents not printed or saved yet; the question says so.
  testWidgets('Closing with shared folders or waiting prints says so', (t) async {
    late BuildContext host;
    await t.pumpWidget(
      MaterialApp(
        theme: buildAppTheme(brightness: Brightness.dark),
        home: Builder(
          builder: (context) {
            host = context;
            return const SizedBox.expand();
          },
        ),
      ),
    );
    final answer = confirmDisconnect(
      host,
      machine: 'VM',
      foldersShared: 1,
      printsWaiting: 3,
    );
    await t.pumpAndSettle();
    expect(find.textContaining('데스크톱에 공유한 폴더 1개도 끊겨요'), findsOneWidget);
    expect(find.textContaining('저장하지 않은 문서 3개는 사라져요'), findsOneWidget);
    await t.tap(find.text('취소'));
    await t.pumpAndSettle();
    expect(await answer, isFalse);
    final quiet = confirmDisconnect(host, machine: 'VM');
    await t.pumpAndSettle();
    expect(find.textContaining('공유한 폴더'), findsNothing);
    expect(find.textContaining('문서'), findsNothing);
    await t.tap(find.text('취소'));
    await t.pumpAndSettle();
    expect(await quiet, isFalse);
  });
}
