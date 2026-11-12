// Negotiated input latency measurement (input-latency-v1): the client meter's
// sampling bounds and estimate, the exact HTTP request to released and new
// brokers, the sampled messages a granted session sends (order and content
// unchanged), acknowledgement/frame-tag handling, and the statistics rows.
// Injected HTTP/socket/media; no network, VM or native engine.
import 'dart:async';
import 'dart:convert';

import 'package:flutter/foundation.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:multitap_client/data/broker_api.dart';
import 'package:multitap_client/data/input_latency.dart';
import 'package:multitap_client/data/remote_session.dart';
import 'package:multitap_client/data/stream_telemetry.dart';
import 'package:multitap_client/models/stream_options.dart';
import 'package:multitap_client/ui/session_stats_dialog.dart';
import 'package:multitap_client/ui/stream_session_stats.dart';

import 'power_api_test.dart' show ApiClient;
import 'remote_session_test.dart' show SessionSocket, SessionMedia, flush;

const _origin = 'https://fixture.invalid';
const _button = {
  'type': 'input',
  'event': 'button',
  'button': 'left',
  'down': true,
  'x': 0.25,
  'y': 0.75,
};
const _move = {'type': 'input', 'event': 'move', 'x': 0.5, 'y': 0.5};
const _key = {
  'type': 'input',
  'event': 'key',
  'vk': 65,
  'scan': 30,
  'down': true,
  'extended': false,
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
Map<String, dynamic> _connected({bool granted = false}) => {
  'sessionId': 'latency-session',
  'webSocketUrl': 'wss://fixture.invalid/api/v1/sessions/latency-session',
  'mode': 'stream',
  'options': const StreamOptions().toJson(),
  if (granted) 'inputLatencyProtocol': 'input-latency-v1',
};
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

/// SessionMedia plus the native frame watch and its stats.
class _WatchMedia extends SessionMedia implements InputFrameWatchMedia {
  final watched = <Uint8List>[];
  final frames = <List<int>>[];
  @override
  Future<void> pushWatched(Uint8List packet) async {
    watched.add(packet);
    await push(packet);
  }

  @override
  Future<Map<String, dynamic>> stats() async => {
    ...await super.stats(),
    'inputFrames': frames,
  };
}

/// [_WatchMedia] whose engine reports each delivered frame only once, as the
/// Android engine does (`take()`); [reads] counts statistics readings.
class _ReportOnceMedia extends _WatchMedia {
  int reads = 0;
  // No expect() here: flutter_test refuses it inside a pumped frame.
  @override
  Future<int> start(int width, int height) async {
    starts++;
    return 42;
  }

  @override
  Future<Map<String, dynamic>> stats() async {
    reads++;
    final values = await super.stats();
    final reported = List<List<int>>.of(frames);
    frames.clear();
    return {...values, 'inputFrames': reported};
  }
}

class _Clock extends Stopwatch {
  int now = 0;
  @override
  int get elapsedMicroseconds => now;
}

class _StatsSession extends ChangeNotifier implements RemoteSession {
  _StatsSession(this.inputLatency);

  @override
  String get videoPathLabel => 'TCP 중계';

  @override
  final onUdp = false, independentOutputs = false;
  @override
  bool closed = false, resizing = false, hostTelemetryFresh = false;
  @override
  bool canResizeViewport = false;
  @override
  String? resizeError, captureBackend;
  @override
  String status = '중계 연결됨', audioStatus = '오디오 끔';
  @override
  int width = 1920, height = 1080;
  @override
  int? sourceWidth,
      sourceHeight,
      appliedFps,
      appliedBitrate,
      clientQueuePackets,
      skippedDecodedFrames,
      losslessTiles;
  @override
  double? capturedFps,
      encodedFps,
      decodedFps,
      textureFps,
      receivedMbps,
      rttMs,
      clientDecodeMs,
      clientDecodeMaxMs,
      clientQueueMs,
      losslessKbps;
  @override
  HostStreamTelemetry? hostTelemetry;
  @override
  InputLatencySummary inputLatency;
  @override
  bool cursorSeparated = false;
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

Future<(RemoteSession, SessionSocket, SessionMedia)> _started({
  required bool granted,
  SessionMedia? media,
}) async {
  final socket = SessionSocket(), engine = media ?? _WatchMedia();
  final session = RemoteSession(
    connection: BrokerConnection(
      sessionId: 'latency-session',
      webSocketUrl: Uri.parse(
        'wss://fixture.invalid/api/v1/sessions/latency-session',
      ),
      inputLatency: granted,
    ),
    token: 'fixture',
    media: engine,
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
    }),
  );
  await flush();
  socket.incoming.add(_video(16667, key: true));
  await flush();
  await session.capture(true);
  expect(session.frameReady && session.captured, isTrue);
  return (session, socket, engine);
}

void main() {
  test('Meter: bounded sampling of buttons/keys/wheel and fewer moves', () {
    final meter = InputLatencyMeter();
    expect(meter.sample({'type': 'input.reset'}, 0), isNull);
    expect(meter.sample({'type': 'ping', 'id': '1'}, 0), isNull);
    expect(meter.sample({'type': 'input', 'event': 'other'}, 0), isNull);
    expect(meter.sample(_button, 1000000), 1);
    expect(meter.sample(_key, 1050000), isNull, reason: 'within 100 ms');
    expect(meter.sample(_key, 1100000), 2);
    expect(meter.sample(_move, 1200000), 3);
    expect(meter.sample(_move, 1300000), isNull, reason: 'moves at most 4/s');
    expect(meter.sample(_button, 1300000), 4);
    expect(meter.sample(_move, 1450000), 5);
    // Ten seconds of continuous 250 Hz events, each sample acknowledged at
    // once: at most 10 samples per second, and moves at most 4 per second.
    int run(Map<String, dynamic> event) {
      final busy = InputLatencyMeter();
      var sampled = 0;
      for (var us = 0; us < 10000000; us += 4000) {
        final sample = busy.sample(event, us);
        if (sample == null) continue;
        sampled++;
        busy.acknowledged({
          'sample': sample,
          'ok': true,
          'injectUs': 0,
          'holdUs': 0,
        }, us + 1000);
      }
      return sampled;
    }

    expect(run(_button), 100);
    expect(run(_key), 100);
    expect(run(_move), 40);
    // Unacknowledged samples are bounded until they expire.
    final bounded = InputLatencyMeter();
    var count = 0;
    for (var n = 0; n < 40; n++) {
      if (bounded.sample(_button, n * 100000) != null) count++;
    }
    expect(count, InputLatencyMeter.outstandingLimit);
    expect(bounded.sample(_button, 40 * 100000 + 5000001), isNotNull);
  });

  test('Meter: key releases and modifier presses are not sampled', () {
    final meter = InputLatencyMeter();
    expect(meter.sample({..._key, 'down': false}, 1000000), isNull);
    for (final vk in InputLatencyMeter.modifierKeys) {
      expect(meter.sample({..._key, 'vk': vk}, 2000000), isNull, reason: '$vk');
    }
    // They did not use the spacing: the next key press is sampled at once.
    expect(meter.sample(_key, 2000001), 1);
    expect(meter.sample({..._key, 'vk': 0x0d}, 2100001), 2, reason: 'Enter');
    expect(meter.sample({..._key, 'vk': 0x15}, 2200001), 3, reason: 'Hangul');
  });

  test('Meter: the release of a sampled press is sampled within the spacing', () {
    final meter = InputLatencyMeter();
    final release = {..._button, 'down': false};
    expect(meter.sample(_button, 1000000), 1);
    expect(meter.sample(release, 1050000), 2, reason: 'the click result');
    expect(meter.sample(_button, 1100000), isNull, reason: '50 ms after');
    expect(meter.sample(release, 1130000), isNull, reason: 'press unsampled');
    expect(meter.sample(_button, 1200000), 3);
    expect(meter.sample(_key, 1220000), isNull, reason: 'spacing');
    expect(meter.sample(release, 1240000), 4);
    expect(meter.sample(release, 1260000), isNull, reason: 'one per press');
    // Per button: a right press during the spacing does not make its release
    // eligible; the left release still is.
    final right = {..._button, 'button': 'right'};
    expect(meter.sample(_button, 2000000), 5);
    expect(meter.sample(right, 2010000), isNull);
    expect(meter.sample({...right, 'down': false}, 2020000), isNull);
    expect(meter.sample(release, 2030000), 6);
    // Rapid clicking (a press or release every 4 ms for 10 s, each sample
    // acknowledged at once) stays within the relay's 20 samples per second.
    final busy = InputLatencyMeter();
    final perSecond = List.filled(10, 0);
    var presses = 0, releases = 0;
    for (var n = 0; n < 2500; n++) {
      final us = n * 4000;
      final sample = busy.sample(n.isEven ? _button : release, us);
      if (sample == null) continue;
      perSecond[us ~/ 1000000]++;
      n.isEven ? presses++ : releases++;
      busy.acknowledged({
        'sample': sample,
        'ok': true,
        'injectUs': 0,
        'holdUs': 0,
      }, us + 1000);
    }
    expect(perSecond.every((count) => count <= 20), isTrue, reason: '$perSecond');
    expect(presses, greaterThanOrEqualTo(90));
    expect(releases, presses, reason: 'every sampled press, its release');
  });

  test('Meter: send->inject estimate = (round trip - host hold) / 2 + host receive->inject', () {
    final meter = InputLatencyMeter();
    final sample = meter.sample(_button, 1000000)!;
    // Round trip 10 ms measured here; host held 2 ms of it; injection 0.5 ms.
    expect(
      meter.acknowledged({
        'sample': sample,
        'ok': true,
        'injectUs': 500,
        'holdUs': 2000,
      }, 1010000),
      isTrue,
    );
    final summary = meter.summary(1010000, frameSupported: true);
    expect(summary.supported, isTrue);
    expect(summary.roundTrip!.p50Ms, 10);
    expect(summary.hostApply!.p50Ms, .5);
    expect(summary.toHost!.p50Ms, 4.5);
    expect(summary.toHost!.samples, 1);
    // Duplicates, unknown samples and malformed reports measure nothing and
    // do not consume a pending sample.
    final failed = meter.sample(_key, 2000000)!;
    for (final bad in <Map<String, dynamic>>[
      {'sample': sample, 'ok': true, 'injectUs': 500, 'holdUs': 2000},
      {'sample': 99, 'ok': true, 'injectUs': 1, 'holdUs': 2},
      {'sample': failed, 'ok': 'yes', 'injectUs': 1, 'holdUs': 2},
      {'sample': failed, 'ok': true, 'injectUs': 3, 'holdUs': 2},
      {'sample': failed, 'ok': true, 'injectUs': -1, 'holdUs': 2},
      {'sample': failed, 'ok': true, 'injectUs': 1, 'holdUs': 10000001},
      {'sample': '$failed', 'ok': true, 'injectUs': 1, 'holdUs': 2},
    ]) {
      expect(meter.acknowledged(bad, 2001000), isFalse, reason: '$bad');
    }
    // A failed injection is counted, never averaged as a latency.
    expect(
      meter.acknowledged({
        'sample': failed,
        'ok': false,
        'injectUs': 300,
        'holdUs': 900,
      }, 2004000),
      isTrue,
    );
    final after = meter.summary(2004000, frameSupported: true);
    expect(after.failedInjections, 1);
    expect(after.toHost!.samples, 1);
    // A hold larger than the round trip (clock rounding) cannot go negative.
    final odd = meter.sample(_key, 3000000)!;
    meter.acknowledged({
      'sample': odd,
      'ok': true,
      'injectUs': 200,
      'holdUs': 5000,
    }, 3001000);
    expect(meter.summary(3001000, frameSupported: true).toHost!.samples, 2);
  });

  test('Meter: nearest-rank p50/p95 over a bounded, time-limited window', () {
    expect(
      latencyPercentile([for (var n = 20; n >= 1; n--) n.toDouble()], .5),
      10,
    );
    expect(
      latencyPercentile([for (var n = 1; n <= 20; n++) n.toDouble()], .95),
      19,
    );
    expect(latencyPercentile([7], .95), 7);
    final meter = InputLatencyMeter(window: 8);
    for (var n = 0; n < 20; n++) {
      final at = 1000000 + n * 200000;
      final sample = meter.sample(_button, at)!;
      meter.acknowledged({
        'sample': sample,
        'ok': true,
        'injectUs': 0,
        'holdUs': 0,
      }, at + (n + 1) * 1000);
    }
    final now = 1000000 + 19 * 200000 + 20000;
    final window = meter.summary(now, frameSupported: true).roundTrip!;
    expect(window.samples, 8, reason: 'fixed-size window');
    expect(window.p50Ms, 16);
    expect(window.p95Ms, 20);
    expect(
      meter.summary(now + 31000000, frameSupported: true).roundTrip,
      isNull,
    );
  });

  test(
    'Meter: the tagged packet is watched and its texture delivery measured',
    () {
      final meter = InputLatencyMeter();
      final sample = meter.sample(_button, 1000000)!;
      meter.tagged({'sample': sample, 'pts': 50000}, 1020000);
      expect(meter.watch(33333, 1021000), isFalse, reason: 'earlier packet');
      expect(meter.watch(50000, 1022000), isTrue);
      expect(meter.watch(50000, 1022000), isFalse, reason: 'one watch per tag');
      meter.pushed(50000, 1023000);
      meter.delivered([
        [16667, 900],
        [50000, 4000],
        ['bad', 1],
      ], 1030000);
      // (pushed - sent) 23 ms + native queued->texture 4 ms.
      expect(meter.summary(1030000, frameSupported: true).toFrame!.p50Ms, 27);
      expect(meter.summary(1030000, frameSupported: false).toFrame, isNull);
      // The client's share: watched at arrival -> pushed 1 ms, native 4 ms.
      expect(meter.summary(1030000, frameSupported: true).framePush!.p50Ms, 1);
      expect(
        meter.summary(1030000, frameSupported: true).frameDecode!.p50Ms,
        4,
      );
      expect(meter.summary(1030000, frameSupported: false).framePush, isNull);
      // No size given: no transfer measurement.
      expect(meter.summary(1030000, frameSupported: true).transfer, isNull);
      // The tag's arrival -> its packet's arrival, and the packet's size.
      final sized = meter.sample(_key, 1500000)!;
      meter.tagged({'sample': sized, 'pts': 60000}, 1520000);
      expect(meter.watch(60000, 1526500, bytes: 48211), isTrue);
      final transfer = meter.summary(1530000, frameSupported: true);
      expect(transfer.transfer!.p50Ms, 6.5);
      expect(transfer.frameBytes!.p50Ms, 48211);
      expect(transfer.downlink, isNull, reason: 'no host durations, no ack');
      // frame-timing-v1: sent 1600000; ack -> uplink estimate 4 ms + 0.5 ms
      // injection; host 18 + 5.5 + 0.6 ms; packet here at 1640000:
      // 40 - 4.5 - 24.1 = 11.4 ms from the host's tag write to arrival.
      final timedSample = meter.sample(_key, 1600000)!;
      meter.acknowledged({
        'sample': timedSample,
        'ok': true,
        'injectUs': 500,
        'holdUs': 2000,
      }, 1610000);
      meter.tagged({
        'sample': timedSample,
        'pts': 70000,
        'captureUs': 18000,
        'encodeUs': 5500,
        'queueUs': 600,
      }, 1636000);
      expect(meter.watch(70000, 1640000, bytes: 9000), isTrue);
      expect(meter.summary(1650000, frameSupported: true).downlink!.p50Ms, 11.4);
      // A tag with a malformed duration measures no downlink.
      final partial = meter.sample(_key, 1700000)!;
      meter.acknowledged({
        'sample': partial,
        'ok': true,
        'injectUs': 0,
        'holdUs': 0,
      }, 1701000);
      meter.tagged({
        'sample': partial,
        'pts': 80000,
        'captureUs': 1,
        'encodeUs': -1,
        'queueUs': 1,
      }, 1702000);
      expect(meter.watch(80000, 1703000, bytes: 100), isTrue);
      expect(meter.summary(1704000, frameSupported: true).downlink!.samples, 1);
      // A tag whose packet never arrives is dropped at the next later packet.
      final lost = meter.sample(_key, 2000000)!;
      meter.tagged({'sample': lost, 'pts': 70000}, 2010000);
      expect(meter.watch(90000, 2020000), isFalse);
      expect(meter.watch(70000, 2021000), isFalse);
      meter.tagged({'sample': 999, 'pts': 1}, 2030000);
      expect(meter.watch(1, 2031000), isFalse, reason: 'unknown sample');
      final reset = meter.sample(_key, 3000000)!;
      meter.tagged({'sample': reset, 'pts': 80000}, 3001000);
      meter.resetFrames();
      expect(meter.watch(80000, 3002000), isFalse, reason: 'new decoder epoch');
    },
  );

  test('HTTP: requested only from an advertising inventory; released request shape otherwise', () async {
    Future<(Map, BrokerConnection)> run(
      Map<String, Object> caps,
      Map<String, dynamic> reply,
    ) async {
      final client = ApiClient()
        ..responses.add((
          200,
          {
            'desktops': [_desktop(caps)],
          },
        ))
        ..responses.add((200, reply));
      final api = HttpBrokerApi(origin: Uri.parse(_origin), client: client);
      await api.desktops('fixture');
      final connection = await api.connect('fixture', 'pve-149');
      return (
        jsonDecode(client.requests.last.body.toString()) as Map,
        connection,
      );
    }

    const released = {'service': true, 'streaming': true};
    final legacy = await run(released, _connected());
    expect(legacy.$1.containsKey('inputLatencyProtocol'), isFalse);
    expect(legacy.$1.keys.toSet(), {
      'mode',
      'resizeProtocol',
      'clipboardProtocol',
      'options',
    });
    expect(legacy.$2.inputLatency, isFalse);
    final measuring = await run({
      ...released,
      'inputLatencyProtocol': 'input-latency-v1',
    }, _connected(granted: true));
    expect(measuring.$1['inputLatencyProtocol'], 'input-latency-v1');
    expect(measuring.$2.inputLatency, isTrue);
    // Requested but not granted (host replaced): an ordinary session.
    final ungranted = await run({
      ...released,
      'inputLatencyProtocol': 'input-latency-v1',
    }, _connected());
    expect(ungranted.$2.inputLatency, isFalse);
    // A malformed capability is not a reason to request it.
    final malformed = await run({
      ...released,
      'inputLatencyProtocol': 'input-latency-v2',
    }, _connected());
    expect(malformed.$1.containsKey('inputLatencyProtocol'), isFalse);
  });

  test('Response: a grant is accepted only when requested, exact, and for a stream session', () {
    final origin = Uri.parse(_origin);
    expect(
      () => BrokerConnection.fromJson(_connected(granted: true), origin),
      throwsA(isA<BrokerException>()),
      reason: 'never requested',
    );
    expect(
      () => BrokerConnection.fromJson(
        {
          ..._connected(granted: true),
          'inputLatencyProtocol': 'input-latency-v2',
        },
        origin,
        requestedInputLatency: true,
      ),
      throwsA(isA<BrokerException>()),
    );
    expect(
      BrokerConnection.fromJson(
        _connected(granted: true),
        origin,
        requestedInputLatency: true,
      ).inputLatency,
      isTrue,
    );
    expect(
      BrokerConnection.fromJson(
        _connected(),
        origin,
        requestedInputLatency: true,
      ).inputLatency,
      isFalse,
    );
  });

  test('Granted session: same messages in the same order, one sample field on sampled input only', () async {
    final (session, socket, media) = await _started(granted: true);
    final before = socket.sent.length;
    final events = <Map<String, dynamic>>[
      Map.of(_move),
      Map.of(_button),
      {...Map.of(_move), 'x': 0.75},
      {..._button, 'down': false},
      Map.of(_key),
      {..._key, 'down': false},
    ];
    for (final event in events) {
      media.events.add(event);
    }
    await flush();
    final sent = socket.sent
        .sublist(before)
        .where((value) => value['type'] == 'input')
        .toList();
    expect(sent, hasLength(events.length));
    for (var n = 0; n < events.length; n++) {
      final copy = Map<String, dynamic>.of(sent[n])..remove('sample');
      expect(copy, events[n], reason: 'content and order are unchanged');
    }
    final samples = [for (final value in sent) value['sample']];
    expect(samples.whereType<int>().toList(), [1], reason: '100 ms bound');
    expect(samples.first, 1, reason: 'the first event is sampled');
    expect(sent.skip(1).every((value) => !value.containsKey('sample')), isTrue);
    // Acknowledgement and frame tag of that sample.
    socket.incoming.add(
      jsonEncode({
        'type': 'input.ack',
        'sessionId': 'latency-session',
        'sample': 1,
        'ok': true,
        'injectUs': 400,
        'holdUs': 900,
      }),
    );
    socket.incoming.add(
      jsonEncode({
        'type': 'input.frame',
        'sessionId': 'latency-session',
        'epoch': 0,
        'sample': 1,
        'pts': 33334,
      }),
    );
    socket.incoming.add(_video(33334));
    socket.incoming.add(_video(50001));
    await flush();
    final watch = media as _WatchMedia;
    expect(watch.watched, hasLength(1));
    expect(watch.packets, hasLength(3), reason: 'every packet pushed once');
    expect(session.inputLatency.supported, isTrue);
    expect(session.inputToHostP50Ms, isNotNull);
    expect(session.inputToHostP50Ms, greaterThanOrEqualTo(.4));
    expect(session.inputToFrameP50Ms, isNull, reason: 'not yet delivered');
    watch.frames.add([33334, 2500]);
    await Future<void>.delayed(const Duration(milliseconds: 1150));
    expect(session.inputToFrameP50Ms, greaterThanOrEqualTo(2.5));
    expect(session.inputToFrameP95Ms, isNotNull);
    final rows = _rows(
      streamMetricSections(
        session,
        options: const StreamOptions(),
        autoViewport: false,
      ),
    );
    expect(rows['입력→Host 적용'], startsWith('p50 '));
    expect(rows['입력→화면 표시'], contains(' / p95 '));
    await session.close();
  });

  testWidgets(
    'A readiness read still pending from the first frame forwards a frame report the engine gives only once',
    (t) async {
      final socket = SessionSocket(),
          media = _ReportOnceMedia(),
          clock = _Clock();
      final session = RemoteSession(
        connection: BrokerConnection(
          sessionId: 'latency-session',
          webSocketUrl: Uri.parse(
            'wss://fixture.invalid/api/v1/sessions/latency-session',
          ),
          inputLatency: true,
        ),
        token: 'fixture',
        media: media,
        clock: clock,
        dialer: (_, _) async => socket,
      );
      // The session clock and the widget clock move together.
      Future<void> step([int milliseconds = 0]) async {
        clock.now += milliseconds * 1000;
        await t.pump(Duration(milliseconds: milliseconds));
        await t.pump();
      }

      try {
        unawaited(session.start());
        await step();
        socket.incoming.add(
          jsonEncode({
            'type': 'stream.info',
            'codec': 'h264',
            'width': 1280,
            'height': 720,
          }),
        );
        await step();
        // The first frame decodes at once and also arms the 50 ms check.
        socket.incoming.add(_video(16667, key: true));
        await step();
        expect(session.frameReady, isTrue);
        await session.capture(true);
        expect(session.captured, isTrue);
        // Before that check runs: a sampled click, its frame tag, the tagged
        // packet 4 ms later, and the engine handing that frame to its texture.
        media.events.add(Map.of(_button));
        await step();
        expect(
          socket.sent.lastWhere((value) => value['type'] == 'input')['sample'],
          1,
        );
        socket.incoming.add(
          jsonEncode({
            'type': 'input.frame',
            'sessionId': 'latency-session',
            'epoch': 0,
            'sample': 1,
            'pts': 33334,
          }),
        );
        socket.incoming.add(_video(33334));
        await step(4);
        expect(media.watched, hasLength(1));
        media.frames.add([33334, 2500]);
        final reads = media.reads;
        // Only the readiness check runs here; the 1 s poll comes later.
        await step(50);
        expect(media.reads, reads + 1);
        expect(media.frames, isEmpty, reason: 'reported to that read only');
        // (pushed - sent) 4 ms + queued -> texture 2.5 ms, kept.
        expect(session.inputToFrameP50Ms, 6.5);
        await step(1000);
        expect(media.reads, greaterThan(reads + 1), reason: 'regular poll');
        expect(session.inputToFrameP50Ms, 6.5);
        expect(session.inputLatency.toFrame!.samples, 1);
      } finally {
        unawaited(session.close());
        await step();
        unawaited(media.events.close());
        await step();
      }
    },
  );

  test('Old peer (no grant): no sample field ever, reports ignored, rows say 측정 안 함', () async {
    final (session, socket, media) = await _started(granted: false);
    final before = socket.sent.length;
    for (var n = 0; n < 5; n++) {
      media.events.add(Map.of(_button));
      await Future<void>.delayed(const Duration(milliseconds: 120));
    }
    final sent = socket.sent.sublist(before);
    expect(sent.where((value) => value['type'] == 'input'), hasLength(5));
    expect(sent.any((value) => value.containsKey('sample')), isFalse);
    socket.incoming.add(
      jsonEncode({
        'type': 'input.frame',
        'sessionId': 'latency-session',
        'epoch': 0,
        'sample': 1,
        'pts': 33334,
      }),
    );
    socket.incoming.add(_video(33334));
    await flush();
    expect((media as _WatchMedia).watched, isEmpty);
    expect(session.closed, isFalse);
    expect(session.inputLatency.supported, isFalse);
    expect(session.inputToHostP50Ms, isNull);
    final rows = _rows(
      streamMetricSections(
        session,
        options: const StreamOptions(),
        autoViewport: false,
      ),
    );
    expect(rows['입력→Host 적용'], '측정 안 함');
    expect(rows['입력→화면 표시'], '측정 안 함');
    await session.close();
  });

  test('Granted session on a media engine without a frame watch measures injection only', () async {
    final (session, socket, media) = await _started(
      granted: true,
      media: SessionMedia(),
    );
    media.events.add(Map.of(_button));
    await flush();
    final sample = socket.sent.lastWhere((v) => v['type'] == 'input')['sample'];
    socket.incoming.add(
      jsonEncode({
        'type': 'input.frame',
        'sessionId': 'latency-session',
        'epoch': 0,
        'sample': sample,
        'pts': 33334,
      }),
    );
    socket.incoming.add(_video(33334));
    await flush();
    expect(media.packets, hasLength(2));
    expect(session.inputLatency.frameSupported, isFalse);
    final rows = _rows(
      streamMetricSections(
        session,
        options: const StreamOptions(),
        autoViewport: false,
      ),
    );
    expect(rows['입력→화면 표시'], '측정 안 함');
    expect(rows['입력→Host 적용'], '—', reason: 'no acknowledgement yet');
    await session.close();
  });

  test('Statistics rows: p50/p95 with sample counts, unsupported and unknown states', () {
    const values = InputLatencySummary(
      supported: true,
      frameSupported: true,
      toHost: LatencyPercentiles(p50Ms: 6.24, p95Ms: 11.5, samples: 24),
      toFrame: LatencyPercentiles(p50Ms: 31.0, p95Ms: 48.26, samples: 18),
      hostApply: LatencyPercentiles(p50Ms: .3, p95Ms: .9, samples: 24),
      roundTrip: LatencyPercentiles(p50Ms: 12.1, p95Ms: 21.7, samples: 24),
    );
    final rows = _rows(
      streamMetricSections(
        _StatsSession(values),
        options: const StreamOptions(),
        autoViewport: false,
      ),
    );
    expect(rows['입력→Host 적용'], 'p50 6.2 / p95 11.5 ms · 24회');
    expect(rows['입력→화면 표시'], 'p50 31.0 / p95 48.3 ms · 18회');
    expect(rows['Host 수신→적용'], 'p50 0.3 / p95 0.9 ms · 24회');
    expect(rows['입력 확인 왕복'], 'p50 12.1 / p95 21.7 ms · 24회');
    final section = streamMetricSections(
      _StatsSession(values),
      options: const StreamOptions(),
      autoViewport: false,
    ).last;
    expect(section.title, '입력 지연');
    expect(section.note, contains('추정'));
    expect(section.note, contains('모니터 표시 시간은 포함하지 않아요'));
    final unsupported = _rows(
      streamMetricSections(
        _StatsSession(InputLatencySummary.unsupported),
        options: const StreamOptions(),
        autoViewport: false,
      ),
    );
    expect(unsupported['입력→Host 적용'], '측정 안 함');
    expect(unsupported['입력→화면 표시'], '측정 안 함');
    final waiting = _rows(
      streamMetricSections(
        _StatsSession(const InputLatencySummary(supported: true)),
        options: const StreamOptions(),
        autoViewport: false,
      ),
    );
    expect(waiting['입력→Host 적용'], '—');
    expect(waiting['입력→화면 표시'], '측정 안 함', reason: 'no frame watch');
    final none = _rows(
      streamMetricSections(
        null,
        options: const StreamOptions(),
        autoViewport: false,
      ),
    );
    expect(none['입력→Host 적용'], '—');
  });
}
