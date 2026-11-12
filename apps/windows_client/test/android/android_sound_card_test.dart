// The Android card's 소리 없음 / 다시 시작 필요 badge is an indicator only
// (2026-11-10): Multitap adds no sound device, the administrator adds an
// audio device in Proxmox. A tap opens nothing, the tooltip is a plain
// statement, and the ⋯ menu lists no sound-card row. Fake broker and store.
import 'package:flutter/material.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:multitap_client/android/android_app.dart';
import 'package:multitap_client/android/android_store.dart';
import 'package:multitap_client/data/broker_api.dart';
import 'package:multitap_client/models/stream_options.dart';

import 'android_launcher_test.dart' show FakeBroker, MemoryStore;

BrokerDesktop _vm({required bool vmAudio, String? audio}) => BrokerDesktop(
  audioState: audio,
  id: 'pve-149',
  name: 'TESTER',
  kind: 'vm',
  description: '',
  os: 'Windows 11',
  status: 'online',
  canConnect: true,
  canStream: true,
  hostConnected: true,
  vmAudio: vmAudio,
  power: BrokerPowerSnapshot(
    state: 'running',
    stale: false,
    checkedAt: DateTime.utc(2026, 11, 2),
  ),
);

Future<void> _signIn(WidgetTester tester, FakeBroker broker) async {
  tester.view.physicalSize = const Size(1080, 2400);
  tester.view.devicePixelRatio = 3;
  addTearDown(tester.view.reset);
  MemoryStore(origin: 'https://vdi.example', username: 't')
    ..prefs['launcher.view'] = 'cards'
    ..install();
  await tester.pumpWidget(AndroidVdiApp(apiFactory: (_) => broker));
  await tester.pumpAndSettle();
  await tester.enterText(
    find.byKey(const Key('sign-in-password')),
    'secret-pass',
  );
  await tester.ensureVisible(find.byKey(const Key('login-submit')));
  await tester.tap(find.byKey(const Key('login-submit')));
  await tester.pumpAndSettle();
}

void main() {
  TestWidgetsFlutterBinding.ensureInitialized();
  tearDown(() {
    TestDefaultBinaryMessengerBinding.instance.defaultBinaryMessenger
        .setMockMethodCallHandler(AndroidStore.channel, null);
  });

  for (final (audio, label, statement) in [
    ('absent', '소리 없음', audioDeviceMissingText),
    ('pending', '다시 시작 필요', audioDevicePendingText),
  ]) {
    testWidgets(
      'The card shows $label from the list: a plain statement, and a tap opens nothing',
      (tester) async {
        await _signIn(tester, FakeBroker([_vm(vmAudio: true, audio: audio)]));
        final badge = find.byKey(const Key('sound-badge-pve-149'));
        expect(badge, findsOneWidget);
        expect(
          find.descendant(of: badge, matching: find.text(label)),
          findsOneWidget,
        );
        final tooltip = tester.widget<Tooltip>(
          find.descendant(of: badge, matching: find.byType(Tooltip)),
        );
        expect(tooltip.message, statement);
        expect(tooltip.message, isNot(contains('추가하세요')));
        expect(tooltip.message, isNot(contains('눌러')));
        // Not a button: nothing to press inside it.
        expect(
          find.descendant(of: badge, matching: find.byType(InkWell)),
          findsNothing,
        );
        await tester.ensureVisible(badge);
        await tester.pumpAndSettle();
        await tester.tap(badge, warnIfMissed: false);
        await tester.pumpAndSettle();
        expect(find.byType(Dialog), findsNothing);
        expect(find.text('사운드카드 추가'), findsNothing);
        expect(find.text('가상 사운드카드'), findsNothing);
        await tester.pumpWidget(const SizedBox.shrink());
      },
    );
  }

  testWidgets('The ⋯ menu has no sound-card row', (tester) async {
    await _signIn(tester, FakeBroker([_vm(vmAudio: true, audio: 'absent')]));
    final more = find.byKey(const Key('desktop-more-pve-149'));
    await tester.ensureVisible(more);
    await tester.pumpAndSettle();
    await tester.tap(more);
    await tester.pumpAndSettle();
    expect(find.byKey(const Key('details-pve-149')), findsOneWidget);
    expect(find.byKey(const Key('vm-sound-card-pve-149')), findsNothing);
    expect(find.text('가상 사운드카드…'), findsNothing);
    expect(find.textContaining('사운드카드'), findsNothing);
    await tester.pumpWidget(const SizedBox.shrink());
  });
}
