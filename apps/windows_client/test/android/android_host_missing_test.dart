// The Android card shows the broker's hostMissingAfterUpdate through its
// existing status lines (no Android-only text). No broker, Host or VM.
import 'package:flutter/services.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:flutter/material.dart';
import 'package:multitap_client/android/android_session_keepalive.dart';
import 'package:multitap_client/android/android_store.dart';
import 'package:multitap_client/data/broker_api.dart';

import '../host_missing_after_update_test.dart'
    show listed, failedAttempt, headline;
import 'android_launcher_test.dart' show signedIn;

const _pen = MethodChannel('multitap/pen-input');

void main() {
  TestWidgetsFlutterBinding.ensureInitialized();
  setUp(() {
    final messenger =
        TestDefaultBinaryMessengerBinding.instance.defaultBinaryMessenger;
    messenger.setMockMethodCallHandler(_pen, (_) async => null);
    messenger.setMockMethodCallHandler(
      AndroidSessionKeepalive.channel,
      (call) async => call.method == 'start' ? true : null,
    );
  });
  tearDown(() {
    final messenger =
        TestDefaultBinaryMessengerBinding.instance.defaultBinaryMessenger;
    for (final channel in [
      _pen,
      AndroidStore.channel,
      AndroidSessionKeepalive.channel,
    ]) {
      messenger.setMockMethodCallHandler(channel, null);
    }
  });

  testWidgets(
    'the Android card says the Host did not come back after its update',
    (tester) async {
      await signedIn(tester, [
        BrokerDesktop.fromJson(listed('pve-247', missing: failedAttempt)),
      ]);
      expect(
        tester.widget<Text>(find.byKey(const Key('status-pve-247'))).data,
        headline,
      );
      expect(
        find.text('Console로 데스크톱을 열고 Host 설정 → 진단을 확인하세요.'),
        findsOneWidget,
      );
      for (final unprovenAdvice in ['보호 기록', '복원', '다시 설치']) {
        expect(find.textContaining(unprovenAdvice), findsNothing);
      }
      expect(find.text('Host 연결이 끊겼어요'), findsNothing);
    },
  );
}
