import 'dart:async';

import 'package:flutter/material.dart';
import 'package:flutter/services.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:multitap_client/data/input_latency.dart';
import 'package:multitap_client/data/remote_session.dart';
import 'package:multitap_client/data/stream_telemetry.dart';
import 'package:multitap_client/data/track_stream.dart';
import 'package:multitap_client/models/stream_options.dart';
import 'package:multitap_client/ui/app_theme.dart';
import 'package:multitap_client/ui/session_stats_dialog.dart';
import 'package:multitap_client/ui/stream_session_stats.dart';

const _copy = Key('copy-session-diagnostics');
const _close = Key('close-session-stats');

class _MetricSession extends ChangeNotifier implements RemoteSession {
  @override
  String get videoPathLabel => 'TCP 중계';
  @override
  final onUdp = false;
  @override
  bool independentOutputs = false;
  @override
  List<StreamTrack> tracks = const [];
  @override
  bool closed = false, resizing = false, hostTelemetryFresh = true;
  @override
  bool canResizeViewport = true;
  @override
  String? resizeError;
  @override
  String status = '중계 연결됨', audioStatus = '오디오 끔';
  @override
  int width = 1600, height = 900;
  @override
  int? sourceWidth = 1920,
      sourceHeight = 1080,
      appliedFps = 30,
      appliedBitrate = 8000000;
  @override
  double? capturedFps = 29.8,
      encodedFps = 29.7,
      decodedFps = 29.6,
      textureFps = 29.5,
      receivedMbps = 6.4,
      rttMs = 24,
      clientDecodeMs = 4.18,
      clientDecodeMaxMs = 8.26,
      clientQueueMs = 0;
  @override
  int? clientQueuePackets = 0, skippedDecodedFrames = 3, losslessTiles = 412;
  @override
  double? losslessKbps = 2400;
  @override
  String? captureBackend = 'wgc-warp';
  @override
  InputLatencySummary inputLatency = InputLatencySummary.unsupported;
  @override
  bool cursorSeparated = false;
  @override
  final streamSize = false;
  @override
  HostStreamTelemetry? hostTelemetry = const HostStreamTelemetry(
    capturedFps: 29.8,
    encodedFps: 29.7,
    bitrateKbps: 6400,
    measuredMs: 1012,
    pipeline: HostPipelineMetrics(
      samples: 30,
      topologyMs: .04,
      captureMs: 2.45,
      convertMs: 1.27,
      encodeMs: 3.52,
      workMs: 7.28,
      p95Ms: 11.42,
      maxMs: 14.29,
    ),
  );

  @override
  bool? gpuHardware;
  @override
  int? texturePath;
  @override
  int? decodeTier, decodeVendor;
  @override
  int? appCpuPct10, systemCpuPct10, gpuDecodePct10, gpu3dPct10;
  @override
  dynamic noSuchMethod(Invocation invocation) => super.noSuchMethod(invocation);
}

Map<String, String> _values(List<SessionMetricSection> sections) => {
  for (final section in sections)
    for (final metric in section.metrics) metric.label: metric.value,
};

List<SessionMetricSection> _sections(String capture) => [
  const SessionMetricSection('스트리밍', [
    SessionMetric('상태', '중계 연결됨'),
    SessionMetric('수신 화면', '1920 × 1080'),
    SessionMetric('원격 원본', '1920 × 1080'),
    SessionMetric('요청 화면', '자동 · 1920 × 1080'),
    SessionMetric('목표 FPS', '요청 60 / 적용 60'),
    SessionMetric('목표 비트레이트', '요청 12.0 / 적용 12.0 Mbps'),
    SessionMetric('캡처 방식', 'WGC · WARP'),
  ]),
  SessionMetricSection('프레임과 네트워크', [
    SessionMetric('소스 캡처', capture),
    const SessionMetric('Host 인코딩', '59.8 fps'),
    const SessionMetric('PC 디코딩', '59.6 fps'),
    const SessionMetric('새 텍스처 전달', '59.2 fps'),
    const SessionMetric('수신 속도', '8.4 Mbps'),
    const SessionMetric('Host RTT', '24 ms'),
    const SessionMetric('오디오', '재생 중 · 48 kHz · 스테레오'),
  ], note: '텍스처 전달은 Flutter 렌더 요청 기준이에요. 화면 표시 완료나 입력 지연을 뜻하지 않아요.'),
  const SessionMetricSection('Host 처리', [
    SessionMetric('화면 구성 확인', '0.04 ms'),
    SessionMetric('캡처·읽기', '2.45 ms'),
    SessionMetric('색 변환', '1.27 ms'),
    SessionMetric('인코딩', '3.52 ms'),
    SessionMetric('합계 평균 / p95', '7.28 / 11.42 ms'),
    SessionMetric('처리 최댓값', '14.29 ms'),
  ], note: '최근 1.0초 · 60프레임 표본. 네트워크 대기 시간은 포함하지 않아요.'),
  const SessionMetricSection('PC 처리', [
    SessionMetric('디코딩 평균', '4.18 ms'),
    SessionMetric('디코딩 최댓값', '8.26 ms'),
    SessionMetric('영상 대기', '0개 · 0.0 ms'),
    SessionMetric('전달 전 교체', '3프레임'),
  ], note: '평균은 최근 조회 구간, 최댓값과 교체 수는 현재 해상도 구간이에요.'),
];

Future<void> _open(
  WidgetTester t, {
  required List<SessionMetricSection> Function() sections,
  Size size = const Size(1440, 900),
  double textScale = 1,
}) async {
  t.view.devicePixelRatio = 1;
  t.view.physicalSize = size;
  addTearDown(t.view.resetDevicePixelRatio);
  addTearDown(t.view.resetPhysicalSize);
  await t.pumpWidget(
    MaterialApp(
      debugShowCheckedModeBanner: false,
      theme: buildAppTheme(),
      builder: (_, child) => MediaQuery(
        data: MediaQueryData(
          size: size,
          textScaler: TextScaler.linear(textScale),
        ),
        child: child!,
      ),
      home: Builder(
        builder: (context) => Scaffold(
          backgroundColor: const Color(0xFF101A2B),
          body: Center(
            child: TextButton(
              onPressed: () => showDialog<void>(
                context: context,
                builder: (_) => SessionStatsDialog(
                  title: '연결 정보',
                  machine: 'TESTER · Windows',
                  sections: sections,
                ),
              ),
              child: const Text('연결 정보 열기'),
            ),
          ),
        ),
      ),
    ),
  );
  await t.tap(find.text('연결 정보 열기'));
  await t.pumpAndSettle();
}

void main() {
  test(
    'Streaming diagnostics distinguish requested, applied and received values',
    () {
      final session = _MetricSession();
      addTearDown(session.dispose);
      final sections = streamMetricSections(
        session,
        options: const StreamOptions(),
        autoViewport: true,
        desired: (1600, 900),
      );
      final values = _values(sections);
      expect(values['수신 화면'], '1600 × 900');
      expect(values['원격 원본'], '1920 × 1080');
      expect(values['요청 화면'], '자동 · 1600 × 900');
      expect(values['목표 FPS'], '요청 60 / 적용 30');
      expect(values['목표 비트레이트'], '요청 20.0 / 적용 8.0 Mbps');
      expect(values['무손실 선명화'], '적용 중 · 타일 412개 · 2.4 Mbps');
      expect(values['소스 캡처'], '29.8 fps');
      expect(values['새 텍스처 전달'], '29.5 fps');
      expect(values['합계 평균 / p95'], '7.28 / 11.42 ms');
      expect(values['처리 최댓값'], '14.29 ms');
      expect(values['영상 대기'], '0개 · 0.0 ms');
      expect(values['전달 전 교체'], '3프레임');
      expect(sections[1].note, contains('화면 표시 완료나 입력 지연을 뜻하지 않아요'));
    },
  );

  test('모든 모니터 사용 shows each monitor\'s share of the summed rates', () {
    StreamTrack output(String id, bool primary) => StreamTrack(
      id: id,
      epoch: 1,
      width: 1920,
      height: 1080,
      left: primary ? 0 : 1920,
      top: 0,
      primary: primary,
      bitrate: 10000000,
    );
    final session = _MetricSession()
      ..independentOutputs = true
      ..tracks = [output('0', true), output('1', false)]
      ..capturedFps = 119.6
      ..encodedFps = 119.4
      ..decodedFps = 119.2;
    addTearDown(session.dispose);
    final values = _values(
      streamMetricSections(
        session,
        options: const StreamOptions(),
        autoViewport: true,
      ),
    );
    expect(values['소스 캡처'], '59.8 fps (화면당)');
    expect(values['Host 인코딩'], '59.7 fps (화면당)');
    expect(values['PC 디코딩'], '59.6 fps (화면당)');
  });

  test(
    'Unknown and stale diagnostics remain unknown rather than zero or old FPS',
    () {
      final missing = _values(
        streamMetricSections(
          null,
          options: const StreamOptions(),
          autoViewport: true,
        ),
      );
      for (final key in [
        '수신 화면',
        '원격 원본',
        '소스 캡처',
        'Host 인코딩',
        'PC 디코딩',
        '새 텍스처 전달',
        '수신 속도',
        'Host RTT',
        '색 변환',
        '합계 평균 / p95',
        '영상 대기',
        '전달 전 교체',
      ]) {
        expect(missing[key], '—', reason: key);
      }
      final session = _MetricSession()..hostTelemetryFresh = false;
      addTearDown(session.dispose);
      var sections = streamMetricSections(
        session,
        options: const StreamOptions(),
        autoViewport: false,
      );
      var values = _values(sections);
      expect(values['소스 캡처'], '—');
      expect(values['Host 인코딩'], '—');
      expect(values['인코딩'], '—');
      expect(sections[2].note, contains('최근 Host 측정값'));
      expect(values['PC 디코딩'], '29.6 fps');
      session.resizing = true;
      values = _values(
        streamMetricSections(
          session,
          options: const StreamOptions(),
          autoViewport: false,
        ),
      );
      expect(values['PC 디코딩'], '—');
      expect(values['디코딩 평균'], '—');
      session.resizing = false;
      session.closed = true;
      values = _values(
        streamMetricSections(
          session,
          options: const StreamOptions(),
          autoViewport: false,
        ),
      );
      expect(values['상태'], '연결 끊김');
      expect(values['새 텍스처 전달'], '—');
      expect(values['영상 대기'], '—');
    },
  );

  testWidgets('Diagnostics stay readable and actions reachable at all sizes', (
    t,
  ) async {
    for (final spec in [
      (1440.0, 900.0, 1.0),
      (640.0, 480.0, 1.25),
      (320.0, 480.0, 2.0),
    ]) {
      await _open(
        t,
        sections: () => _sections('60.0 fps'),
        size: Size(spec.$1, spec.$2),
        textScale: spec.$3,
      );
      expect(t.takeException(), isNull);
      expect(find.text('1920 × 1080'), findsNWidgets(2));
      expect(find.byKey(_copy).hitTestable(), findsOneWidget);
      expect(find.byKey(_close).hitTestable(), findsOneWidget);
      await t.scrollUntilVisible(
        find.text('전달 전 교체'),
        200,
        scrollable: find.byType(Scrollable).first,
        maxScrolls: 40,
      );
      await t.pump();
      expect(find.text('3프레임').hitTestable(), findsOneWidget);
      expect(find.byKey(_copy).hitTestable(), findsOneWidget);
      expect(find.byKey(_close).hitTestable(), findsOneWidget);
      expect(t.takeException(), isNull);
      await t.tap(find.byKey(_close));
      await t.pumpAndSettle();
      expect(find.byType(SessionStatsDialog), findsNothing);
    }
  });

  testWidgets(
    'Diagnostics refresh while open and cancel their refresh on close',
    (t) async {
      var builds = 0, capture = '30.0 fps';
      await _open(
        t,
        sections: () {
          builds++;
          return _sections(capture);
        },
      );
      expect(find.text('30.0 fps'), findsOneWidget);
      capture = '60.0 fps';
      await t.pump(const Duration(seconds: 1));
      expect(find.text('60.0 fps'), findsOneWidget);
      await t.tap(find.byKey(_close));
      await t.pumpAndSettle();
      final closedBuilds = builds;
      await t.pump(const Duration(seconds: 3));
      expect(builds, closedBuilds);
      expect(t.takeException(), isNull);
    },
  );

  testWidgets(
    'Copy is explicit, writes only diagnostic rows and blocks repeats',
    (t) async {
      final calls = <MethodCall>[];
      final completion = Completer<void>();
      final messenger =
          TestDefaultBinaryMessengerBinding.instance.defaultBinaryMessenger;
      messenger.setMockMethodCallHandler(SystemChannels.platform, (call) async {
        calls.add(call);
        if (call.method == 'Clipboard.setData') await completion.future;
        return null;
      });
      addTearDown(
        () => messenger.setMockMethodCallHandler(SystemChannels.platform, null),
      );
      await _open(
        t,
        sections: () => const [
          SessionMetricSection('스트리밍', [
            SessionMetric('수신 화면', '1920 × 1080'),
            SessionMetric('목표 FPS', '요청 60 / 적용 60'),
          ], note: 'Rendered explanation, not raw transport state'),
        ],
      );
      expect(calls.where((c) => c.method.startsWith('Clipboard.')), isEmpty);
      await t.pump(const Duration(seconds: 2));
      expect(calls.where((c) => c.method.startsWith('Clipboard.')), isEmpty);
      await t.tap(find.byKey(_copy));
      await t.pump();
      expect(t.widget<TextButton>(find.byKey(_copy)).onPressed, isNull);
      expect(calls.where((c) => c.method == 'Clipboard.getData'), isEmpty);
      final writes = calls.where((c) => c.method == 'Clipboard.setData');
      expect(writes, hasLength(1));
      expect(
        (writes.single.arguments as Map)['text'],
        '연결 정보\nTESTER · Windows\n\n스트리밍\n수신 화면: 1920 × 1080\n목표 FPS: 요청 60 / 적용 60\n',
      );
      completion.complete();
      await t.pump();
      expect(find.text('복사됨'), findsOneWidget);
      await t.tap(find.byKey(_close));
      await t.pumpAndSettle();
    },
  );

  testWidgets(
    'Clipboard failure remains retryable without dismissing diagnostics',
    (t) async {
      var writes = 0;
      final messenger =
          TestDefaultBinaryMessengerBinding.instance.defaultBinaryMessenger;
      messenger.setMockMethodCallHandler(SystemChannels.platform, (call) async {
        if (call.method == 'Clipboard.setData' && ++writes == 1) {
          throw PlatformException(code: 'clipboard-busy');
        }
        return null;
      });
      addTearDown(
        () => messenger.setMockMethodCallHandler(SystemChannels.platform, null),
      );
      await _open(t, sections: () => _sections('60.0 fps'));
      await t.tap(find.byKey(_copy));
      await t.pump();
      expect(find.text('복사 다시 시도'), findsOneWidget);
      expect(find.byKey(_close).hitTestable(), findsOneWidget);
      await t.tap(find.byKey(_copy));
      await t.pump();
      expect(find.text('복사됨'), findsOneWidget);
      expect(writes, 2);
      await t.tap(find.byKey(_close));
      await t.pumpAndSettle();
    },
  );
}
