// The "하드웨어 디코딩 (기본 켜짐)" streaming setting: stored apart from the
// server/sign-in settings, shown in the settings dialog and session panel,
// applied from the next connection, and passed to the native runner when a
// stream (or an independent-output stream) starts. No native engine, network or VM.
import 'dart:io';

import 'package:flutter/material.dart';
import 'package:flutter/services.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:multitap_client/data/decode_settings.dart';
import 'package:multitap_client/data/input_latency.dart';
import 'package:multitap_client/data/remote_session.dart';
import 'package:multitap_client/data/stream_telemetry.dart';
import 'package:multitap_client/models/desktop_machine.dart';
import 'package:multitap_client/models/stream_options.dart';
import 'package:multitap_client/ui/stream_session_stats.dart';
import 'package:multitap_client/ui/stream_settings_dialog.dart';

DesktopMachine _machine() => DesktopMachine(
  id: 'pve-149',
  name: 'Fixture',
  description: '',
  kind: MachineKind.vm,
  status: MachineStatus.ready,
  specification: '',
  accent: Colors.blue,
  monitors: const [
    RemoteMonitor(id: '0', width: 1920, height: 1080, primary: true),
  ],
);

void main() {
  TestWidgetsFlutterBinding.ensureInitialized();
  const channel = MethodChannel('multitap/remote');
  final messenger =
      TestDefaultBinaryMessengerBinding.instance.defaultBinaryMessenger;

  late Directory dir;
  setUp(() {
    dir = Directory.systemTemp.createTempSync('decode-settings');
    DecodeSettings.hardwareDecode = true;
  });
  tearDown(() {
    DecodeSettings.hardwareDecode = true;
    dir.deleteSync(recursive: true);
  });

  test(
    'The choice defaults to on and survives a restart; a damaged file means on',
    () async {
      final store = DecodeSettings(File('${dir.path}/decoding.json'));
      expect(await store.load(), isTrue);
      await DecodeSettings.init(store);
      expect(DecodeSettings.hardwareDecode, isTrue);
      await DecodeSettings.choose(false);
      expect(DecodeSettings.hardwareDecode, isFalse);
      expect(await store.load(), isFalse);
      DecodeSettings.hardwareDecode = true;
      await DecodeSettings.init(store);
      expect(DecodeSettings.hardwareDecode, isFalse);
      await store.file.writeAsString('{not json');
      expect(await store.load(), isTrue);
      await store.file.writeAsString('{"hardwareDecode": "no"}');
      expect(await store.load(), isTrue);
    },
  );

  test('A stream start tells the runner which choice was made', () async {
    final calls = <MethodCall>[];
    messenger.setMockMethodCallHandler(channel, (call) async {
      calls.add(call);
      return switch (call.method) {
        'start' => 70,
        'startTracks' => {'0': 80},
        _ => null,
      };
    });
    addTearDown(() => messenger.setMockMethodCallHandler(channel, null));
    var media = NativeRemoteMedia(audioEnabled: false);
    await media.start(640, 360);
    expect((calls.last.arguments as Map)['hardwareDecode'], isTrue);
    await media.stop();
    media = NativeRemoteMedia(audioEnabled: false, hardwareDecode: false);
    await media.start(640, 360);
    expect((calls.last.arguments as Map)['hardwareDecode'], isFalse);
    await media.restart(800, 450);
    expect((calls.last.arguments as Map)['hardwareDecode'], isFalse);
    await media.stop();
    final tracks = NativeRemoteMedia(
      audioEnabled: false,
      hardwareDecode: false,
    );
    await tracks.startTracks({
      'topologyEpoch': 1,
      'audioEnabled': false,
      'tracks': [
        {
          'id': '0',
          'epoch': 1,
          'width': 640,
          'height': 360,
          'sourceWidth': 640,
          'sourceHeight': 360,
          'left': 0,
          'top': 0,
          'primary': true,
        },
      ],
    });
    final tracksCall = calls.lastWhere((c) => c.method == 'startTracks');
    expect((tracksCall.arguments as Map)['hardwareDecode'], isFalse);
    await tracks.stop();
  });

  testWidgets(
    'The connect dialog shows the switch on, and 연결 keeps a CPU-only choice for the next connection',
    (tester) async {
      tester.view.physicalSize = const Size(1200, 1800);
      tester.view.devicePixelRatio = 1;
      addTearDown(tester.view.reset);
      await tester.pumpWidget(
        MaterialApp(
          home: Builder(
            builder: (context) => TextButton(
              onPressed: () => showDialog<StreamOptions>(
                context: context,
                builder: (_) => StreamSettingsDialog(machine: _machine()),
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
      expect(find.text('하드웨어 디코딩'), findsOneWidget);
      final toggle = find.byKey(const Key('stream-hardware-decode'));
      expect(tester.widget<Switch>(toggle).value, isTrue);
      await tester.tap(toggle);
      await tester.pumpAndSettle();
      expect(tester.widget<Switch>(toggle).value, isFalse);
      expect(find.textContaining('CPU로만 디코딩'), findsOneWidget);
      // Nothing is stored until the dialog is applied.
      expect(DecodeSettings.hardwareDecode, isTrue);
      await tester.tap(find.byKey(const Key('apply-stream-settings')));
      await tester.pumpAndSettle();
      expect(DecodeSettings.hardwareDecode, isFalse);
    },
  );

  testWidgets('Cancelling the dialog leaves the choice alone', (tester) async {
    tester.view.physicalSize = const Size(1200, 1800);
    tester.view.devicePixelRatio = 1;
    addTearDown(tester.view.reset);
    await tester.pumpWidget(
      MaterialApp(
        home: Builder(
          builder: (context) => TextButton(
            onPressed: () => showDialog<StreamOptions>(
              context: context,
              builder: (_) => StreamSettingsDialog(machine: _machine()),
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
    await tester.tap(find.byKey(const Key('stream-hardware-decode')));
    await tester.pumpAndSettle();
    await tester.tap(find.text('취소'));
    await tester.pumpAndSettle();
    expect(DecodeSettings.hardwareDecode, isTrue);
  });

  testWidgets('In a session the switch alone enables 재연결 and applies on it', (
    tester,
  ) async {
    tester.view.physicalSize = const Size(1200, 1800);
    tester.view.devicePixelRatio = 1;
    addTearDown(tester.view.reset);
    StreamOptions? result;
    await tester.pumpWidget(
      MaterialApp(
        home: Builder(
          builder: (context) => TextButton(
            onPressed: () async => result = await showStreamSettingsPanel(
              context,
              anchor: const Rect.fromLTWH(400, 10, 40, 20),
              machine: _machine(),
              initialOptions: StreamOptions.recommended,
            ),
            child: const Text('open'),
          ),
        ),
      ),
    );
    await tester.tap(find.text('open'));
    await tester.pumpAndSettle();
    final apply = find.byKey(const Key('apply-stream-settings'));
    expect(tester.widget<FilledButton>(apply).onPressed, isNull);
    await tester.tap(find.byKey(const Key('stream-advanced')));
    await tester.pumpAndSettle();
    await tester.tap(find.byKey(const Key('stream-hardware-decode')));
    await tester.pumpAndSettle();
    expect(tester.widget<FilledButton>(apply).onPressed, isNotNull);
    await tester.tap(apply);
    await tester.pumpAndSettle();
    expect(result, isNotNull);
    expect(DecodeSettings.hardwareDecode, isFalse);
  });

  test('The statistics say whether this PC decodes on the GPU and how the picture reaches Flutter', () {
    Map<String, String> rows(RemoteSession? session) => {
      for (final section in streamMetricSections(
        session,
        options: StreamOptions.recommended,
        autoViewport: true,
      ))
        for (final metric in section.metrics) metric.label: metric.value,
    };
    expect(rows(null)['PC 디코더'], '—');
    expect(rows(null)['화면 전달 방식'], '—');
    final cpu = _Session(gpuHardware: false, texturePath: 0);
    expect(rows(cpu)['PC 디코더'], '소프트웨어 (CPU)');
    expect(rows(cpu)['화면 전달 방식'], 'CPU 픽셀 버퍼');
    final gpu = _Session(gpuHardware: true, texturePath: 1);
    expect(rows(gpu)['PC 디코더'], '하드웨어 (GPU)');
    // The app's own cost: CPU of the app and of the PC, GPU decode engine.
    expect(rows(null)['앱 CPU'], '—');
    expect(rows(gpu)['앱 CPU'], '—');
    expect(rows(gpu)['GPU 영상 디코딩'], '알 수 없음');
    final loaded = _Session(gpuHardware: true, texturePath: 1)
      ..decodeTier = 1
      ..appCpuPct10 = 120
      ..systemCpuPct10 = 340
      ..gpuDecodePct10 = 80;
    expect(rows(loaded)['앱 CPU'], '12% (PC 전체 34%)');
    expect(rows(loaded)['GPU 영상 디코딩'], '8% · GPU 디코더');
    final software = _Session(gpuHardware: false, texturePath: 0)
      ..decodeTier = 0
      ..appCpuPct10 = 450;
    expect(rows(software)['앱 CPU'], '45%');
    expect(rows(software)['GPU 영상 디코딩'], 'CPU 디코더');
    expect(rows(gpu)['화면 전달 방식'], 'GPU 표면');
  });
}

class _Session extends ChangeNotifier implements RemoteSession {
  @override
  String get videoPathLabel => 'TCP 중계';
  _Session({this.gpuHardware, this.texturePath});

  @override
  final onUdp = false, independentOutputs = false;
  @override
  bool closed = false, resizing = false, hostTelemetryFresh = true;
  @override
  bool canResizeViewport = true;
  @override
  String? resizeError, captureBackend = 'wgc-warp';
  @override
  String status = '중계 연결됨', audioStatus = '오디오 끔';
  @override
  int width = 1920, height = 1080;
  @override
  int? sourceWidth = 1920,
      sourceHeight = 1080,
      appliedFps = 60,
      appliedBitrate = 20000000,
      clientQueuePackets = 0,
      skippedDecodedFrames = 0,
      losslessTiles;
  @override
  double? capturedFps = 60,
      encodedFps = 60,
      decodedFps = 60,
      textureFps = 60,
      receivedMbps = 80,
      rttMs = 20,
      clientDecodeMs = 3,
      clientDecodeMaxMs = 6,
      clientQueueMs = 0,
      losslessKbps;
  @override
  HostStreamTelemetry? hostTelemetry;
  @override
  bool? gpuHardware;
  @override
  int? texturePath;
  @override
  int? decodeTier, decodeVendor;
  @override
  int? appCpuPct10, systemCpuPct10, gpuDecodePct10, gpu3dPct10;
  @override
  final inputLatency = InputLatencySummary.unsupported;
  @override
  final cursorSeparated = false;
  @override
  final streamSize = false;
  @override
  dynamic noSuchMethod(Invocation invocation) => super.noSuchMethod(invocation);
}
