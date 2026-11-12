// Quality modes (backlog #99) in the Windows streaming settings and the
// Android 스트리밍 화질 dialog. Keys and text only, so the same file runs
// (and fails) against a client without the modes. Broker, store and
// windows are fixtures; no real VM, display or input is involved.
import 'package:flutter/material.dart';
import 'package:flutter/services.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:multitap_client/android/android_app.dart';
import 'package:multitap_client/android/android_session_keepalive.dart';
import 'package:multitap_client/android/android_store.dart';
import 'package:multitap_client/data/broker_api.dart';
import 'package:multitap_client/models/desktop_machine.dart';
import 'package:multitap_client/models/stream_options.dart';
import 'package:multitap_client/ui/app_theme.dart';
import 'package:multitap_client/ui/stream_settings_dialog.dart';

import 'android/android_launcher_test.dart'
    as fixture
    show FakeBroker, MemoryStore;
import 'android/android_stream_preparation_test.dart'
    as preparation
    show desktop;

import 'helpers/stream_connect_consent.dart';

DesktopMachine _machine({int? maxBitrate = 100000000}) => DesktopMachine(
  id: 'pve-149',
  name: 'Preset fixture',
  description: '',
  kind: MachineKind.vm,
  status: MachineStatus.ready,
  specification: 'Windows',
  accent: Colors.blue,
  hostOnline: true,
  maxBitrate: maxBitrate,
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

String _plain(WidgetTester tester, String key) {
  final text = tester.widget<Text>(find.byKey(Key(key)));
  return text.textSpan?.toPlainText() ?? text.data!;
}

/// The Windows mode row's selection, by name ('' when none is).
String _windowsMode(WidgetTester tester) {
  final selected = tester
      .widget<SegmentedButton<Object?>>(find.byKey(const Key('stream-preset')))
      .selected;
  return selected.isEmpty ? '' : (selected.single! as Enum).name;
}

Set<int> _windowsFps(WidgetTester tester) => tester
    .widget<SegmentedButton<int>>(find.byType(SegmentedButton<int>))
    .selected;

void main() {
  TestWidgetsFlutterBinding.ensureInitialized();
  final messenger =
      TestDefaultBinaryMessengerBinding.instance.defaultBinaryMessenger;

  group('Windows streaming settings', () {
    Future<List<StreamOptions?>> open(
      WidgetTester tester, {
      DesktopMachine? machine,
      StreamOptions initial = StreamOptions.recommended,
    }) async {
      tester.view.devicePixelRatio = 1;
      tester.view.physicalSize = const Size(1280, 1000);
      addTearDown(tester.view.reset);
      final results = <StreamOptions?>[];
      await tester.pumpWidget(
        MaterialApp(
          theme: buildAppTheme(),
          home: Builder(
            builder: (context) => TextButton(
              onPressed: () async => results.add(
                await showDialog<StreamOptions>(
                  context: context,
                  builder: (_) => StreamSettingsDialog(
                    machine: machine ?? _machine(),
                    initialOptions: initial,
                  ),
                ),
              ),
              child: const Text('open'),
            ),
          ),
        ),
      );
      await tester.tap(find.text('open'));
      await tester.pumpAndSettle();
      await tester.tap(find.byKey(const Key('stream-advanced')));
      await tester.pumpAndSettle();
      return results;
    }

    Future<void> apply(WidgetTester tester) async {
      await tester.tap(find.byKey(const Key('apply-stream-settings')));
      await tester.pumpAndSettle();
    }

    testWidgets('A new connection starts at 작업 (60 fps, automatic)', (
      tester,
    ) async {
      final results = await open(tester);
      expect(_plain(tester, 'stream-preset-label'), '화질 모드  작업');
      expect(_windowsMode(tester), 'work');
      expect(_windowsFps(tester), {60});
      expect(_plain(tester, 'stream-bitrate-label'), '영상 전송률  자동');
      await apply(tester);
      expect(results.single!.fps, 60);
      expect(results.single!.automaticBitrate, isTrue);
    });

    testWidgets(
      '절약 asks for 30 fps at a fixed 8 Mbps; 작업 brings back the default',
      (tester) async {
        final results = await open(tester);
        await tester.tap(find.byKey(const Key('stream-preset-saver')));
        await tester.pump();
        expect(_plain(tester, 'stream-preset-label'), '화질 모드  절약');
        expect(_windowsMode(tester), 'saver');
        expect(_windowsFps(tester), {30});
        expect(_plain(tester, 'stream-bitrate-label'), '영상 전송률  8 Mbps');
        await apply(tester);
        final saver = results.single!;
        expect(saver.fps, 30);
        expect(saver.bitrateMode, StreamBitrateMode.fixed);
        expect(saver.bitrate, 8000000);
        expect(saver.audio, isTrue, reason: 'a mode leaves sound as it was');

        final again = await open(tester, initial: saver);
        expect(_windowsMode(tester), 'saver', reason: 'kept for the session');
        await tester.tap(find.byKey(const Key('stream-preset-work')));
        await tester.pump();
        expect(_windowsMode(tester), 'work');
        await apply(tester);
        expect(again.single!.fps, 60);
        expect(again.single!.automaticBitrate, isTrue);
      },
    );

    testWidgets('Changing one setting reads 사용자 설정; sound is always on', (
      tester,
    ) async {
      final results = await open(tester);
      // No sound switch (2026-11-11): sound is no choice.
      expect(find.byKey(const Key('stream-audio')), findsNothing);
      expect(_windowsMode(tester), 'work');
      await tester.tap(find.text('30 fps'));
      await tester.pump();
      expect(_plain(tester, 'stream-preset-label'), '화질 모드  사용자 설정');
      expect(_windowsMode(tester), '');
      await apply(tester);
      expect(results.single!.fps, 30);
      expect(results.single!.automaticBitrate, isTrue);
      expect(results.single!.audio, isTrue);
    });

    testWidgets('A released Host (fixed 4-20 Mbps) shows the same modes', (
      tester,
    ) async {
      // What a released Host's connection echoes for 작업: fixed 20 Mbps.
      final results = await open(
        tester,
        machine: _machine(maxBitrate: null),
        initial: const StreamOptions(),
      );
      expect(_windowsMode(tester), 'work');
      await tester.tap(find.byKey(const Key('stream-preset-saver')));
      await tester.pump();
      expect(_plain(tester, 'stream-bitrate-label'), '영상 전송률  8 Mbps');
      await apply(tester);
      expect(results.single!.fps, 30);
      expect(results.single!.bitrate, 8000000);
    });
  });

  group('Android 스트리밍 화질', () {
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

    Future<(fixture.MemoryStore, _Broker)> signIn(
      WidgetTester tester, {
      String? savedQuality,
    }) async {
      tester.view.devicePixelRatio = 3;
      tester.view.physicalSize = const Size(1080, 2400);
      addTearDown(tester.view.reset);
      final store = fixture.MemoryStore(
        origin: 'https://fixture.invalid',
        username: 'tester',
        prefs: {'launcher.view': 'cards', 'stream.quality': ?savedQuality},
      )..install();
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
      return (store, broker);
    }

    Future<void> openDialog(WidgetTester tester) async {
      // ▾ beside the main button holds 화질 옵션, as on Windows (2026-11-03).
      await tester.ensureVisible(find.byKey(const Key('stream-modes-pve-149')));
      await tester.tap(find.byKey(const Key('stream-modes-pve-149')));
      await tester.pumpAndSettle();
      await tester.tap(find.byKey(const Key('stream-settings-pve-149')));
      await tester.pumpAndSettle();
      await tester.ensureVisible(find.text('고급 설정'));
      await tester.tap(find.text('고급 설정'));
      await tester.pumpAndSettle();
    }

    bool chip(WidgetTester tester, String key) =>
        tester.widget<ChoiceChip>(find.byKey(Key(key))).selected;

    Future<void> tapChip(WidgetTester tester, String key) async {
      await tester.ensureVisible(find.byKey(Key(key)));
      await tester.tap(find.byKey(Key(key)));
      await tester.pump();
    }

    testWidgets(
      'Fresh: 작업 is chosen; 절약 sends 30 fps at 8 Mbps and is remembered',
      (tester) async {
        final (store, broker) = await signIn(tester);
        await openDialog(tester);
        expect(find.text('화질 모드 · 작업'), findsOneWidget);
        expect(chip(tester, 'preset-work'), isTrue);
        expect(chip(tester, 'preset-saver'), isFalse);
        expect(chip(tester, 'fps-60'), isTrue);
        expect(chip(tester, 'bitrate-0'), isTrue, reason: '자동');
        await tapChip(tester, 'preset-saver');
        expect(find.text('화질 모드 · 절약'), findsOneWidget);
        expect(chip(tester, 'preset-saver'), isTrue);
        expect(chip(tester, 'fps-30'), isTrue);
        expect(chip(tester, 'bitrate-8'), isTrue);
        await tester.tap(
          find.descendant(
            of: find.byType(AndroidStreamDialog),
            matching: find.widgetWithText(FilledButton, '연결'),
          ),
        );
        await tester.pumpAndSettle();
        await acceptStreamConnect(tester, 'pve-149');
        final sent = broker.requests.single;
        expect(sent.fps, 30);
        expect(sent.bitrate, 8000000);
        expect(sent.bitrateMode, StreamBitrateMode.fixed);
        expect(sent.audio, isTrue, reason: 'a mode leaves sound as it was');
        expect(
          store.prefs['stream.quality'],
          '{"fps":30,"bitrate":8,"audio":true}',
        );
      },
    );

    testWidgets('A remembered 절약 opens as 절약', (tester) async {
      await signIn(
        tester,
        savedQuality: '{"fps":30,"bitrate":8,"audio":false}',
      );
      await openDialog(tester);
      expect(find.text('화질 모드 · 절약'), findsOneWidget);
      expect(chip(tester, 'preset-saver'), isTrue);
      expect(chip(tester, 'preset-work'), isFalse);
    });

    testWidgets('Changing one setting reads 사용자 설정 until it matches a mode', (
      tester,
    ) async {
      await signIn(tester);
      await openDialog(tester);
      await tapChip(tester, 'fps-30');
      expect(find.text('화질 모드 · 사용자 설정'), findsOneWidget);
      expect(chip(tester, 'preset-work'), isFalse);
      expect(chip(tester, 'preset-saver'), isFalse);
      await tapChip(tester, 'bitrate-8');
      expect(find.text('화질 모드 · 절약'), findsOneWidget);
      await tapChip(tester, 'preset-work');
      expect(chip(tester, 'fps-60'), isTrue);
      expect(chip(tester, 'bitrate-0'), isTrue);
    });
  });
}
