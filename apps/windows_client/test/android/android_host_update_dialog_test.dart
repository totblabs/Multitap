import 'dart:async';

import 'package:flutter/material.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:multitap_client/android/android_host_update_dialog.dart';
import 'package:multitap_client/data/product_updates.dart';
import 'package:multitap_client/models/host_update.dart';

ProductRelease release(String version) => ProductRelease(
  product: 'host',
  version: version,
  filename: 'Multitap-Host-$version-windows-x64.exe',
  size: 10,
  sha256: 'a' * 64,
  url: Uri.parse('https://vdi.example/downloads/host.exe'),
  changelog: '이 버전의 Host 변경 내역',
);

void main() {
  test('missing, old or equally-timed different replies never overwrite '
      'an observed operation or announce themselves as newest', () {
    HostUpdateState state(String status, DateTime? at) => HostUpdateState(
      present: true,
      connected: true,
      operation: HostUpdateOperation(
        status: status,
        targetVersion: '0.2.81',
        checkedAt: at,
        code: status == 'unknown' ? 'host_disconnected' : '',
      ),
    );
    final now = DateTime.utc(2026, 1, 1);
    final observed = state('installing', now);
    for (final reply in [
      HostUpdateState.absent,
      const HostUpdateState(present: true),
      state('unknown', null),
    ]) {
      expect(androidHostUpdateReply(observed, reply).apply, isFalse);
      expect(
        androidHostUpdateReply(observed, reply).message,
        anyOf(contains('확인하지 못했어요'), contains('마지막으로 확인한 상태')),
      );
    }
    final old = androidHostUpdateReply(
      observed,
      state('unknown', now.subtract(const Duration(seconds: 1))),
    );
    expect(old.apply, isFalse);
    expect(old.message, contains('설치'));
    expect(old.message, isNot(contains('Host 연결이 끊겼어요')));
    final equalTime = androidHostUpdateReply(observed, state('unknown', now));
    expect(equalTime.apply, isFalse);
    expect(equalTime.message, contains('마지막으로 확인한 상태'));
    expect(
      androidHostUpdateReply(observed, state('installing', now)).apply,
      isFalse,
    );
    expect(
      androidHostUpdateReply(
        observed,
        state('unknown', now.add(const Duration(seconds: 1))),
      ).apply,
      isTrue,
    );
  });
  test('request-result text follows the returned operation instead of the '
      'old confirmation session snapshot', () {
    HostUpdateState state(
      String status, {
      String code = '',
      bool session = false,
    }) => HostUpdateState(
      present: true,
      connected: code != 'host_disconnected',
      sessionActive: session,
      operation: HostUpdateOperation(
        status: status,
        targetVersion: '0.2.81',
        code: code,
        checkedAt: DateTime.utc(2020),
      ),
    );
    expect(
      androidHostUpdateResult(state('waiting', session: false)),
      contains('연결이 끝나면 시작'),
    );
    expect(
      androidHostUpdateResult(state('dispatched', session: true)),
      'Host 업데이트 중',
    );
    expect(
      androidHostUpdateResult(state('unknown', code: 'host_disconnected')),
      'Host 업데이트 결과 확인 필요 · Host 연결이 끊겼어요',
    );
    expect(
      androidHostUpdateResult(state('rejected', code: 'signature_invalid')),
      contains('배포 서명을 확인하지 못했어요'),
    );
    expect(androidHostUpdateResult(state('completed')), contains('업데이트 완료'));
    expect(
      androidHostUpdateResult(const HostUpdateState(present: true)),
      contains('업데이트 요청 결과는 확인하지 못했어요'),
    );
    expect(
      androidHostUpdateResult(HostUpdateState.absent),
      contains('결과를 확인하지 못했어요'),
    );
  });
  Future<void> open(
    WidgetTester tester,
    Future<ProductRelease?> Function() load, {
    bool Function()? current,
  }) => tester.pumpWidget(
    MaterialApp(
      home: Scaffold(
        body: AndroidHostUpdateDialog(
          origin: Uri.parse('https://vdi.example'),
          machine: 'VM',
          currentVersion: '0.2.80',
          targetVersion: '0.2.81',
          sessionActive: false,
          isCurrent: current ?? () => true,
          loadRelease: load,
        ),
      ),
    ),
  );
  Finder getConfirm() => find.byKey(const Key('android-host-update-confirm'));
  testWidgets('failure and a different latest version block confirmation; '
      'retry shows notes only for the matching target', (tester) async {
    var reads = 0;
    await open(tester, () async {
      reads++;
      if (reads == 1) throw const UpdateFailure('catalog_unavailable');
      return release(reads == 2 ? '0.2.82' : '0.2.81');
    });
    await tester.pumpAndSettle();
    expect(tester.widget<FilledButton>(getConfirm()).onPressed, isNull);
    expect(find.textContaining('확인하지 못했어요'), findsOneWidget);
    await tester.tap(find.byKey(const Key('android-host-update-retry')));
    await tester.pumpAndSettle();
    expect(tester.widget<FilledButton>(getConfirm()).onPressed, isNull);
    expect(find.textContaining('배포 버전이 달라요'), findsOneWidget);
    await tester.tap(find.byKey(const Key('android-host-update-retry')));
    await tester.pumpAndSettle();
    expect(find.text('이 버전의 Host 변경 내역'), findsOneWidget);
    expect(tester.widget<FilledButton>(getConfirm()).onPressed, isNotNull);
  });
  testWidgets('late metadata cannot authorize a changed VM or session', (
    tester,
  ) async {
    final pending = Completer<ProductRelease?>();
    var current = true;
    await open(tester, () => pending.future, current: () => current);
    await tester.pump();
    current = false;
    pending.complete(release('0.2.81'));
    await tester.pumpAndSettle();
    expect(tester.widget<FilledButton>(getConfirm()).onPressed, isNull);
    expect(find.textContaining('상태가 바뀌었어요'), findsOneWidget);
  });
  testWidgets('metadata completion after disposal does not update the view', (
    tester,
  ) async {
    final pending = Completer<ProductRelease?>();
    await open(tester, () => pending.future);
    await tester.pump();
    await tester.pumpWidget(const SizedBox());
    pending.complete(release('0.2.81'));
    await tester.pump();
    expect(tester.takeException(), isNull);
  });
}
