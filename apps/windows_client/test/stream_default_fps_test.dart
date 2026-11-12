// Launch/default-choice coverage; transport and native windows are fixtures.
import 'package:flutter/material.dart';
import 'package:flutter/services.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:multitap_client/android/android_app.dart';
import 'package:multitap_client/android/android_remote.dart';
import 'package:multitap_client/android/android_session_keepalive.dart';
import 'package:multitap_client/android/android_store.dart';
import 'package:multitap_client/data/broker_api.dart';
import 'package:multitap_client/models/desktop_machine.dart';
import 'package:multitap_client/models/session_display_mode.dart';
import 'package:multitap_client/models/stream_options.dart';
import 'package:multitap_client/ui/app_theme.dart';
import 'package:multitap_client/ui/workspace_shell.dart';

import 'android/android_launcher_test.dart'
    as fixture
    show FakeBroker, MemoryStore;
import 'android/android_stream_preparation_test.dart'
    as preparation
    show desktop;
import 'helpers/stream_connect_consent.dart';

const _machine = DesktopMachine(
  id: 'pve-149',
  name: 'FPS fixture',
  description: '',
  kind: MachineKind.vm,
  status: MachineStatus.ready,
  specification: 'Windows',
  accent: Colors.blue,
  hostOnline: true,
  independentDisplayStreaming: true,
);

class _Broker extends fixture.FakeBroker {
  _Broker() : super([preparation.desktop()]);
  final requests = <StreamOptions>[];

  @override
  Future<BrokerConnection> connect(
    String token,
    String desktopId, {
    String mode = 'stream',
    StreamOptions? options,
  }) {
    requests.add(options!);
    return super.connect(token, desktopId, mode: mode, options: options);
  }
}

void main() {
  TestWidgetsFlutterBinding.ensureInitialized();
  final messenger =
      TestDefaultBinaryMessengerBinding.instance.defaultBinaryMessenger;

  setUp(() {
    messenger.setMockMethodCallHandler(
      const MethodChannel('multitap/pen-input'),
      (_) async => null,
    );
    messenger.setMockMethodCallHandler(
      AndroidSessionKeepalive.channel,
      (call) async => call.method == 'start' ? 'started' : true,
    );
  });
  tearDown(() {
    for (final channel in [
      const MethodChannel('multitap/pen-input'),
      AndroidSessionKeepalive.channel,
      AndroidStore.channel,
    ]) {
      messenger.setMockMethodCallHandler(channel, null);
    }
  });

  test('Windows recommendation is 60 with automatic bitrate; legacy constructor/wire and explicit 60 stay 60', () {
    expect(StreamOptions.recommended.fps, 60);
    expect(StreamOptions.recommended.automaticBitrate, isTrue);
    expect(const StreamOptions().toJson()['fps'], 60);
    final selected = StreamOptions.fromJson(
      const StreamOptions(fps: 60).toJson(),
    );
    expect(
      selected.negotiated(null).withCodecCeiling().forSingleDisplay().fps,
      60,
    );
    expect(const AndroidStreamQuality().options.fps, 60);
    expect(AndroidStreamQuality.recommended.options.fps, 60);
    expect(
      AndroidStreamQuality.recommended.options.bitrateMode,
      StreamBitrateMode.auto,
    );
  });

  test('Android missing/invalid memory recommends 60 automatic; valid saved FPS is kept', () {
    for (final missing in [null, '', '{}', 'invalid', '{"fps":60}', '{"fps":30,"bitrate":12,"audio":1}']) {
      final quality = AndroidStreamQuality.parse(missing);
      expect(quality.fps, 60);
      expect(quality.bitrate, AndroidStreamQuality.automatic);
    }
    for (final fps in [30, 60]) {
      final saved = '{"fps":$fps,"bitrate":12,"audio":false}';
      final quality = AndroidStreamQuality.parse(saved);
      expect(quality.fps, fps);
      expect(quality.options.bitrate, 12000000);
      // An earlier version's 소리 끔 is read past: sound is always on.
      expect(quality.options.audio, isTrue);
      expect(quality.encode(), contains('"audio":true'));
      expect(AndroidStreamQuality.parse(quality.encode()).fps, fps);
    }
  });

  Future<void> windows(
    WidgetTester tester,
    void Function(SessionDisplayMode, StreamOptions) opened,
  ) async {
    tester.view.devicePixelRatio = 1;
    tester.view.physicalSize = const Size(1440, 1000);
    addTearDown(tester.view.reset);
    await tester.pumpWidget(
      MaterialApp(
        theme: buildAppTheme(),
        home: WorkspaceShell(
          displayName: 'fixture',
          serverOrigin: 'https://fixture.invalid',
          onSignOut: () {},
          onRefresh: () {},
          machines: const [_machine],
          localMonitors: 2,
          onConnect: (_, {options = const StreamOptions()}) async =>
              throw StateError('detached path expected'),
          onOpenWindow: (_, mode, options) async {
            opened(mode, options);
            return null;
          },
        ),
      ),
    );
    await tester.pumpAndSettle();
    addTearDown(() async {
      await tester.pumpWidget(const SizedBox());
      await tester.pumpAndSettle();
    });
  }

  for (final mode in SessionDisplayMode.values) {
    testWidgets('Windows new ${mode.name} launch requests recommended 60', (
      tester,
    ) async {
      final requests = <StreamOptions>[];
      await windows(tester, (actual, options) {
        expect(actual, mode);
        requests.add(options);
      });
      await tester.tap(find.byKey(const Key('stream-modes-pve-149')));
      await tester.pumpAndSettle();
      await tester.tap(find.byKey(Key('stream-mode-${mode.name}')));
      await acceptStreamConnect(tester, 'pve-149');
      await tester.pumpAndSettle();
      expect(requests.single.fps, 60);
      expect(requests.single.automaticBitrate, isTrue);
      expect(tester.takeException(), isNull);
    });
  }

  testWidgets(
    'Windows pre-connect settings start at 60 and transmit explicit 30',
    (tester) async {
      final requests = <StreamOptions>[];
      await windows(tester, (_, options) => requests.add(options));
      await tester.tap(find.byKey(const Key('stream-modes-pve-149')));
      await tester.pumpAndSettle();
      await tester.tap(find.byKey(const Key('stream-settings-pve-149')));
      await tester.pumpAndSettle();
      await tester.tap(find.byKey(const Key('stream-advanced')));
      await tester.pumpAndSettle();
      expect(
        tester
            .widget<SegmentedButton<int>>(find.byType(SegmentedButton<int>))
            .selected,
        {60},
      );
      await tester.tap(find.text('30 fps'));
      await tester.tap(find.byKey(const Key('apply-stream-settings')));
      await acceptStreamConnect(tester, 'pve-149');
      await tester.pumpAndSettle();
      expect(requests.single.fps, 30);
      expect(tester.takeException(), isNull);
    },
  );

  Future<_Broker> android(WidgetTester tester, {String? savedQuality}) async {
    tester.view.devicePixelRatio = 3;
    tester.view.physicalSize = const Size(1080, 2400);
    addTearDown(tester.view.reset);
    fixture.MemoryStore(
      origin: 'https://fixture.invalid',
      username: 'tester',
      prefs: {'launcher.view': 'cards', 'stream.quality': ?savedQuality},
    ).install();
    final broker = _Broker();
    await tester.pumpWidget(AndroidVdiApp(apiFactory: (_) => broker));
    await tester.pumpAndSettle();
    await tester.enterText(
      find.byKey(const Key('sign-in-password')),
      'fixture-password',
    );
    await tester.ensureVisible(find.byKey(const Key('login-submit')));
    await tester.tap(find.byKey(const Key('login-submit')));
    await tester.pumpAndSettle();
    addTearDown(() async {
      await tester.pumpWidget(const SizedBox());
      await tester.pumpAndSettle();
    });
    return broker;
  }

  for (final saved in [false, true]) {
    testWidgets(
      'Android ${saved ? 'saved 30' : 'fresh default 60'} connects after consent and preserves request through retry',
      (tester) async {
        final fps = saved ? 30 : 60;
        final broker = await android(
          tester,
          savedQuality: saved ? '{"fps":30,"bitrate":12,"audio":false}' : null,
        );
        await tester.ensureVisible(find.byKey(const Key('play-pve-149')));
        await tester.tap(find.byKey(const Key('play-pve-149')));
      await acceptStreamConnect(tester, 'pve-149');
        await tester.pumpAndSettle();
        expect(find.byKey(const Key('confirm-connect-pve-149')), findsNothing);
        expect(broker.requests, hasLength(1));
        expect(
          tester
              .widget<AndroidRemoteView>(find.byType(AndroidRemoteView))
              .options
              .fps,
          fps,
        );
        expect(broker.requests.single.fps, fps);
        await tester.tap(find.byKey(const Key('android-retry')));
        await tester.pumpAndSettle();
        expect(broker.requests, hasLength(2));
        expect(broker.requests.last.fps, fps);
        expect(tester.takeException(), isNull);
      },
    );
  }

  testWidgets(
    'Android fresh pre-connect settings start at 60 and send explicit 30',
    (tester) async {
      final broker = await android(tester);
      // ▾ beside the main button holds 화질 옵션, as on Windows.
      await tester.ensureVisible(find.byKey(const Key('stream-modes-pve-149')));
      await tester.tap(find.byKey(const Key('stream-modes-pve-149')));
      await tester.pumpAndSettle();
      await tester.tap(find.byKey(const Key('stream-settings-pve-149')));
      await tester.pumpAndSettle();
      await tester.ensureVisible(find.text('고급 설정'));
      await tester.tap(find.text('고급 설정'));
      await tester.pumpAndSettle();
      expect(
        tester.widget<ChoiceChip>(find.byKey(const Key('fps-60'))).selected,
        isTrue,
      );
      await tester.ensureVisible(find.byKey(const Key('fps-30')));
      await tester.tap(find.byKey(const Key('fps-30')));
      await tester.tap(
        find.descendant(
          of: find.byType(AndroidStreamDialog),
          matching: find.widgetWithText(FilledButton, '연결'),
        ),
      );
      await acceptStreamConnect(tester, 'pve-149');
      await tester.pumpAndSettle();
      expect(broker.requests.single.fps, 30);
      expect(tester.takeException(), isNull);
    },
  );
}
