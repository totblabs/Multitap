// Adaptive streaming bitrate (bitrate-adaptive-v1, docs/adaptive-bitrate.md):
// option negotiation per desktop, the exact HTTP request sent to released and
// new brokers, the arrival feedback a negotiated session sends, telemetry and
// the settings dialog. Injected HTTP/socket/media; no network or VM.
import 'dart:async';
import 'dart:convert';
import 'dart:typed_data';

import 'package:flutter/material.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:multitap_client/data/broker_api.dart';
import 'package:multitap_client/data/desktop_mapping.dart';
import 'package:multitap_client/data/input_latency.dart';
import 'package:multitap_client/data/remote_session.dart';
import 'package:multitap_client/data/stream_telemetry.dart';
import 'package:multitap_client/models/desktop_machine.dart';
import 'package:multitap_client/models/stream_options.dart';
import 'package:multitap_client/ui/session_stats_dialog.dart';
import 'package:multitap_client/ui/stream_session_stats.dart';
import 'package:multitap_client/ui/stream_settings_dialog.dart';

import 'power_api_test.dart' show ApiClient;
import 'remote_session_test.dart' show SessionSocket, SessionMedia, flush;

const _origin = 'https://fixture.invalid';
const _adaptiveCaps = {
  'service': true,
  'streaming': true,
  'bitrateProtocol': 'bitrate-adaptive-v1',
  'maxBitrate': 100000000,
};
Map<String, dynamic> _desktop(Map<String, Object> capabilities) => {
  'id': 'pve-149',
  'name': 'TESTER',
  'kind': 'vm',
  'description': '',
  'os': 'Windows',
  'status': 'ready',
  'canConnect': true,
  'host': {'connected': true, 'capabilities': capabilities},
};
Map<String, dynamic> _connected(
  Map<String, Object> options, {
  bool granted = false,
}) => {
  'sessionId': 'adaptive-session',
  'webSocketUrl': 'wss://fixture.invalid/api/v1/sessions/adaptive-session',
  'mode': 'stream',
  'options': options,
  if (granted) 'bitrateProtocol': 'bitrate-adaptive-v1',
};
DesktopMachine _machine({int? maxBitrate}) => DesktopMachine(
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
  maxBitrate: maxBitrate,
);
Uint8List _video(int pts, {bool key = false}) {
  final bytes = Uint8List(17);
  bytes[0] = 1;
  bytes[1] = key ? 1 : 0;
  for (var i = 0; i < 8; i++) {
    bytes[4 + i] = (pts >> (8 * i)) & 255;
  }
  bytes.setAll(12, [0, 0, 0, 1, 0x65]);
  return bytes;
}

class _StatsSession extends ChangeNotifier implements RemoteSession {
  _StatsSession(this.hostTelemetry);

  @override
  final onUdp = false, independentOutputs = false;
  @override
  String get videoPathLabel => 'TCP 중계';
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
  final inputLatency = InputLatencySummary.unsupported;
  @override
  final cursorSeparated = false;
  @override
  final streamSize = false;
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

Map<String, String> _rows(List<SessionMetricSection> sections) => {
  for (final section in sections)
    for (final metric in section.metrics) metric.label: metric.value,
};
void main() {
  test('Negotiation sends only what the desktop supports; fixed keeps the released shape', () {
    const auto = StreamOptions.recommended;
    expect(auto.automaticBitrate, isTrue);
    expect(const StreamOptions().automaticBitrate, isFalse);
    // Released broker/host (no ceiling): fixed, never above 20 Mbps.
    expect(auto.negotiated(null).toJson(), const StreamOptions().toJson());
    expect(auto.negotiated(null).toJson().containsKey('bitrateMode'), isFalse);
    expect(
      const StreamOptions(bitrate: 60000000).negotiated(null).bitrate,
      20000000,
    );
    expect(
      const StreamOptions(bitrate: 8000000).negotiated(null).toJson(),
      const StreamOptions(bitrate: 8000000).toJson(),
    );
    // Adaptive desktop: auto requests its ceiling, manual is bounded by it.
    final requested = auto.negotiated(100000000);
    expect(requested.toJson(), {
      ...const StreamOptions().toJson(),
      'bitrate': 100000000,
      'bitrateMode': 'auto',
    });
    expect(auto.negotiated(50000000).bitrate, 50000000);
    expect(
      const StreamOptions(bitrate: 60000000).negotiated(100000000).toJson(),
      {...const StreamOptions().toJson(), 'bitrate': 60000000},
    );
    expect(
      const StreamOptions(bitrate: 90000000).negotiated(50000000).bitrate,
      50000000,
    );
    // Independent outputs (tracks-v1) stay fixed-rate.
    final tracks = auto.negotiated(100000000, independentOutputs: true);
    expect(tracks.automaticBitrate, isFalse);
    expect(tracks.bitrate, 20000000);
    // Preferences survive the session-window IPC and monitor narrowing.
    expect(StreamOptions.fromJson(requested.toJson()).automaticBitrate, isTrue);
    expect(
      const StreamOptions(
        monitor: '1',
        bitrateMode: StreamBitrateMode.auto,
      ).forSingleDisplay().automaticBitrate,
      isTrue,
    );
    expect(
      () => StreamOptions.fromJson({
        ...const StreamOptions().toJson(),
        'bitrateMode': 'turbo',
      }),
      throwsArgumentError,
    );
    expect(
      () => const StreamOptions(bitrate: 100000001).toJson(),
      throwsArgumentError,
    );
  });

  test('Only an exact advertised capability yields a ceiling', () {
    int? ceiling(Map<String, Object> caps) =>
        machineFromBroker(BrokerDesktop.fromJson(_desktop(caps))).maxBitrate;
    expect(ceiling(_adaptiveCaps), 100000000);
    expect(ceiling({..._adaptiveCaps, 'maxBitrate': 50000000}), 50000000);
    for (final bad in [
      <String, Object>{'service': true},
      {..._adaptiveCaps, 'maxBitrate': 19999999},
      {..._adaptiveCaps, 'maxBitrate': 100000001},
      {..._adaptiveCaps, 'maxBitrate': '100000000'},
      {..._adaptiveCaps, 'bitrateProtocol': 'bitrate-adaptive-v2'},
    ]) {
      expect(ceiling(bad), isNull, reason: '$bad');
    }
    final machine = machineFromBroker(
      BrokerDesktop.fromJson(_desktop(_adaptiveCaps)),
    );
    expect(machine.withDisplay(machine.display).maxBitrate, 100000000);
    expect(machine.withResources(machine.resources).bitrateCeiling, 100000000);
    expect(_machine().bitrateCeiling, 20000000);
  });

  test('HTTP: auto only to an adaptive desktop, released shape otherwise, one fallback on a stale inventory', () async {
    Future<(Map, BrokerConnection)> run(
      Map<String, Object> caps,
      List<(int, Map<String, dynamic>)> connectReplies,
    ) async {
      final client = ApiClient()
        ..responses.add((
          200,
          {
            'desktops': [_desktop(caps)],
          },
        ))
        ..responses.addAll(connectReplies);
      final api = HttpBrokerApi(origin: Uri.parse(_origin), client: client);
      await api.desktops('fixture');
      final connection = await api.connect(
        'fixture',
        'pve-149',
        options: StreamOptions.recommended,
      );
      final bodies = [
        for (final request in client.requests.skip(1))
          jsonDecode(request.body.toString()) as Map,
      ];
      return ({'bodies': bodies, 'last': bodies.last['options']}, connection);
    }

    final adaptiveOptions = StreamOptions.recommended
        .negotiated(100000000)
        .toJson();
    final adaptive = await run(_adaptiveCaps, [
      (200, _connected(adaptiveOptions, granted: true)),
    ]);
    expect(adaptive.$1['last'], adaptiveOptions);
    expect(adaptive.$2.bitrateAdaptive, isTrue);
    expect(adaptive.$2.options.bitrate, 100000000);

    final legacyOptions = const StreamOptions().toJson();
    final legacy = await run(
      {'service': true, 'streaming': true},
      [(200, _connected(legacyOptions))],
    );
    expect(legacy.$1['last'], legacyOptions);
    expect((legacy.$1['bodies'] as List).length, 1);
    expect(legacy.$2.bitrateAdaptive, isFalse);

    // The host was replaced by a released one after the inventory was read.
    final stale = await run(_adaptiveCaps, [
      (
        409,
        {
          'error': {'code': 'stream_bitrate_unavailable'},
        },
      ),
      (200, _connected(legacyOptions)),
    ]);
    final bodies = stale.$1['bodies'] as List;
    expect(bodies.length, 2);
    expect(bodies.first['options'], adaptiveOptions);
    expect(bodies.last['options'], legacyOptions);
    expect(stale.$2.bitrateAdaptive, isFalse);

    // A grant is accepted only for an ordinary stream session.
    expect(
      () => BrokerConnection.fromJson({
        ..._connected(adaptiveOptions, granted: true),
        'bitrateProtocol': 'bitrate-adaptive-v2',
      }, Uri.parse(_origin)),
      throwsA(isA<BrokerException>()),
    );
    expect(
      BrokerConnection.fromJson(
        _connected(legacyOptions, granted: true),
        Uri.parse(_origin),
      ).bitrateAdaptive,
      isFalse,
      reason: 'A fixed session never reports arrivals.',
    );
  });

  test(
    'A negotiated session reports video arrivals every 100 ms; others never do',
    () async {
      for (final adaptive in [true, false]) {
        final socket = SessionSocket(), media = SessionMedia();
        final session = RemoteSession(
          connection: BrokerConnection(
            sessionId: 'adaptive-session',
            webSocketUrl: Uri.parse(
              'wss://fixture.invalid/api/v1/sessions/adaptive-session',
            ),
            options: adaptive
                ? StreamOptions.recommended.negotiated(100000000)
                : const StreamOptions(),
            bitrateAdaptive: adaptive,
          ),
          token: 'fixture',
          media: media,
          dialer: (uri, token) async => socket,
        );
        unawaited(session.start());
        await flush();
        socket.incoming.add(
          jsonEncode({
            'type': 'stream.info',
            'codec': 'h264',
            'width': 1280,
            'height': 720,
            'sourceWidth': 1280,
            'sourceHeight': 720,
            'fpsTarget': 60,
            'bitrate': 20000000,
            'audio': {
              'sampleRate': 48000,
              'channels': 2,
              'format': 's16le',
              'available': false,
            },
          }),
        );
        await flush();
        for (final pts in [16667, 33334, 50001]) {
          socket.incoming.add(_video(pts, key: pts == 16667));
        }
        await Future<void>.delayed(const Duration(milliseconds: 250));
        final reports = socket.sent
            .where((event) => event['type'] == 'stream.feedback')
            .toList();
        if (!adaptive) {
          expect(reports, isEmpty);
        } else {
          expect(reports, isNotEmpty);
          final frames = [
            for (final report in reports) ...(report['frames'] as List),
          ];
          expect(
            [for (final f in frames) (f as List).first],
            [16667, 33334, 50001],
          );
          expect(frames.every((f) => (f as List)[1] is int && f[1] >= 0), true);
          final report = reports.first;
          expect(report.keys.toSet(), {
            'type',
            'epoch',
            'frames',
            'bytes',
            'intervalUs',
          });
          expect(report['epoch'], 0);
          expect(report['bytes'], greaterThanOrEqualTo(17));
          expect(report['intervalUs'], greaterThan(0));
        }
        await session.close();
      }
    },
  );

  test(
    'Host statistics carry the current automatic target and queueing delay',
    () {
      final telemetry = HostStreamTelemetry.parse({
        'type': 'stream.stats',
        'capturedFps': 60,
        'encodedFps': 60,
        'bitrateKbps': 98000,
        'bitrateTarget': 85000000,
        'bitrateMode': 'auto',
        'bitrateMax': 100000000,
        'networkDelayMs': 14.5,
      })!;
      expect(telemetry.bitrateTarget, 85000000);
      expect(telemetry.bitrateMax, 100000000);
      expect(telemetry.automaticBitrate, isTrue);
      expect(telemetry.networkDelayMs, 14.5);
      final released = HostStreamTelemetry.parse({
        'capturedFps': 60,
        'encodedFps': 60,
        'bitrateKbps': 9000,
      })!;
      expect(released.bitrateTarget, isNull);
      expect(released.automaticBitrate, isFalse);
      expect(
        HostStreamTelemetry.parse({
          'capturedFps': 60,
          'encodedFps': 60,
          'bitrateKbps': 9000,
          'bitrateTarget': 200000000,
          'networkDelayMs': -1,
        })!.bitrateTarget,
        isNull,
      );
    },
  );

  test('Statistics show the current automatic rate and queueing delay; fixed rows are unchanged', () {
    const auto = HostStreamTelemetry(
      capturedFps: 60,
      encodedFps: 60,
      bitrateKbps: 78000,
      bitrateTarget: 82000000,
      bitrateMax: 100000000,
      automaticBitrate: true,
      networkDelayMs: 13.4,
    );
    final adaptive = _rows(
      streamMetricSections(
        _StatsSession(auto),
        options: StreamOptions.recommended,
        autoViewport: true,
      ),
    );
    expect(adaptive['목표 비트레이트'], '자동 · 현재 82.0 / 최대 100.0 Mbps');
    expect(adaptive['전송 대기'], '13 ms');
    const fixed = HostStreamTelemetry(
      capturedFps: 60,
      encodedFps: 60,
      bitrateKbps: 19000,
    );
    final released = _rows(
      streamMetricSections(
        _StatsSession(fixed),
        options: const StreamOptions(),
        autoViewport: true,
      ),
    );
    expect(released['목표 비트레이트'], '요청 20.0 / 적용 20.0 Mbps');
    expect(released.containsKey('전송 대기'), isFalse);
  });
  testWidgets(
    'Settings offer 자동 (권장) or a manual slider up to the supported ceiling',
    (tester) async {
      tester.view.physicalSize = const Size(1200, 1800);
      tester.view.devicePixelRatio = 1;
      addTearDown(tester.view.reset);
      // Opens the dialog, runs `interact`, applies, returns the dialog result.
      Future<StreamOptions?> choose(
        DesktopMachine machine,
        Future<void> Function() interact,
      ) async {
        StreamOptions? result;
        await tester.pumpWidget(
          MaterialApp(
            home: Builder(
              builder: (context) => TextButton(
                onPressed: () async => result = await showDialog<StreamOptions>(
                  context: context,
                  builder: (_) => StreamSettingsDialog(machine: machine),
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
        await interact();
        await tester.tap(find.byKey(const Key('apply-stream-settings')));
        await tester.pumpAndSettle();
        return result;
      }

      // Adaptive desktop: automatic is the recommended default, no slider.
      final automatic = await choose(_machine(maxBitrate: 100000000), () async {
        expect(find.byKey(const Key('stream-bitrate-mode')), findsOneWidget);
        expect(find.text('자동 (권장)'), findsOneWidget);
        expect(find.byKey(const Key('stream-bitrate')), findsNothing);
        expect(
          find.byKey(const Key('stream-bitrate-auto-note')),
          findsOneWidget,
        );
      });
      expect(automatic!.automaticBitrate, isTrue);
      expect(automatic.negotiated(100000000).bitrate, 100000000);

      // Manual: a slider from 4 up to the advertised 100 Mbps.
      final manual = await choose(_machine(maxBitrate: 100000000), () async {
        await tester.tap(find.text('직접 설정'));
        await tester.pumpAndSettle();
        final slider = tester.widget<Slider>(
          find.byKey(const Key('stream-bitrate')),
        );
        expect(slider.min, 4);
        expect(slider.max, 100);
        await tester.drag(
          find.byKey(const Key('stream-bitrate')),
          const Offset(600, 0),
        );
        await tester.pumpAndSettle();
      });
      expect(manual!.automaticBitrate, isFalse);
      expect(manual.bitrate, greaterThan(20000000));
      expect(manual.bitrate, lessThanOrEqualTo(100000000));

      // Released desktop: today's 4-20 Mbps slider and no mode control.
      final released = await choose(_machine(), () async {
        expect(find.byKey(const Key('stream-bitrate-mode')), findsNothing);
        final slider = tester.widget<Slider>(
          find.byKey(const Key('stream-bitrate')),
        );
        expect(slider.max, 20);
        expect(slider.value, 20);
      });
      final sent = released!.negotiated(null).toJson();
      expect(sent['bitrate'], 20000000);
      expect(sent.containsKey('bitrateMode'), isFalse);
    },
  );
}
