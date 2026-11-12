// display-scale-v1 on the client and the Android crispness plan: Windows
// scale steps, the viewport plan for both paths (device pixels + Windows
// scale; exact integer fraction for older Hosts), the broker grant, the
// connect request only toward an advertising inventory, and the session's
// request/result handling. No device, Host or display change.
import 'dart:async';
import 'dart:convert';

import 'package:flutter/widgets.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:multitap_client/android/android_screen.dart';
import 'package:multitap_client/android/android_stream_preparation.dart';
import 'package:multitap_client/data/broker_api.dart';
import 'package:multitap_client/data/display_scale.dart';
import 'package:multitap_client/data/remote_session.dart';
import 'package:multitap_client/models/stream_options.dart';

import '../power_api_test.dart' show ApiClient;
import '../remote_session_test.dart' show SessionSocket, SessionMedia, flush;

const _origin = 'https://fixture.invalid';
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
Map<String, dynamic> _connected([int? granted]) => {
  'sessionId': 'scale-session',
  'webSocketUrl': 'wss://fixture.invalid/api/v1/sessions/scale-session',
  'mode': 'stream',
  'options': const StreamOptions().toJson(),
  if (granted != null) ...{
    'displayScaleProtocol': 'display-scale-v1',
    'displayScale': granted,
  },
};
Map<String, Object> _applied({
  bool ok = true,
  int scale = 175,
  int maximum = 175,
  int code = 0,
  String session = 'scale-session',
}) => {
  'type': 'display.scale.applied',
  'sessionId': session,
  'protocol': 'display-scale-v1',
  'ok': ok,
  'scale': scale,
  'maximum': maximum,
  'code': code,
};

void main() {
  test('Windows scale steps: nearest step, ties to the larger one', () {
    expect(nearestDisplayScale(0.8), 100);
    expect(nearestDisplayScale(1.64), 175);
    expect(nearestDisplayScale(1.875), 200);
    expect(nearestDisplayScale(2.4), 250);
    expect(nearestDisplayScale(9), 500);
    expect(nearestDisplayScale(double.nan), 100);
    expect(validDisplayScale(225), isTrue);
    expect(validDisplayScale(160), isFalse);
    expect(validDisplayScale('200'), isFalse);
  });

  test('plans: bounded handheld source with Windows scale, or legacy exact fraction', () {
    const phone = AndroidScreenPolicy();
    AndroidViewportPlan plan(
      Size physical,
      double dpr,
      AndroidScreenPolicy mode, {
      bool host = true,
    }) => AndroidViewportPlan.of(physical, dpr, mode, hostScale: host);
    const landscape = Size(2400, 1080), portrait = Size(1080, 2400);
    // The readability ratio chooses a moderate source for this actual DPR.
    expect(
      plan(landscape, 3, phone),
      const AndroidViewportPlan(
        width: 1280,
        height: 576,
        pixelScale: 0,
        displayScale: 100,
      ),
    );
    // A fixed 화면 배율 is asked for as it is (the Host clamps).
    expect(
      plan(
        landscape,
        3,
        const AndroidScreenPolicy(choice: AndroidScreenChoice(scale: 150)),
      ).displayScale,
      150,
    );
    // The same adaptive source follows rotation inside the decoder limits.
    expect(
      plan(portrait, 3, phone),
      const AndroidViewportPlan(
        width: 576,
        height: 1280,
        pixelScale: 0,
        displayScale: 100,
      ),
    );
    // Older Hosts: the whole fraction nearest the wanted size, never
    // shorter than 480 rows, no Windows scale.
    expect(
      plan(landscape, 3, phone, host: false),
      const AndroidViewportPlan(width: 1200, height: 540, pixelScale: 2),
    );
    expect(
      plan(
        landscape,
        3,
        const AndroidScreenPolicy(choice: AndroidScreenChoice(scale: 300)),
        host: false,
      ).pixelScale,
      2,
      reason: '3 would leave 360 rows',
    );
    // Odd pixel counts stay even (the encoder needs even sizes).
    expect(plan(const Size(2401, 1081), 3, phone).width, 1280);
  });

  test('자동 on a tablet comes from the panel density, not the 2x bucket', () {
    // An 11-inch 2560×1600 tablet: 274 ppi, reported as density 2.
    const tab = AndroidScreenDensity(
      xdpi: 274.3,
      ydpi: 274.3,
      width: 2560,
      height: 1600,
    );
    final policy = AndroidScreenPolicy.of(
      const Size(1280, 800),
      2,
      tab,
      (_) => const AndroidScreenChoice(),
    );
    expect(policy.deviceClass, AndroidDeviceClass.tablet);
    // 274 ppi × 40 cm (12 × √11) / (96 × 65) gives the source reduction
    // ratio1.75, preserving that readability at Windows100%.
    expect(
      AndroidViewportPlan.of(
        const Size(2560, 1600),
        2,
        policy,
        hostScale: true,
      ),
      const AndroidViewportPlan(
        width: 1462,
        height: 914,
        pixelScale: 0,
        displayScale: 100,
      ),
    );
    // 절반: two device pixels per remote pixel, so 자동 halves (100 %).
    final half = AndroidScreenPolicy.of(
      const Size(1280, 800),
      2,
      tab,
      (_) => const AndroidScreenChoice(resolution: AndroidResolution.half),
    );
    expect(
      AndroidViewportPlan.of(const Size(2560, 1600), 2, half, hostScale: true),
      const AndroidViewportPlan(
        width: 1280,
        height: 800,
        pixelScale: 2,
        displayScale: 100,
      ),
    );
    // Older Host on the tablet: 자동 takes the nearest whole fraction (2).
    expect(
      AndroidViewportPlan.of(
        const Size(2560, 1600),
        2,
        policy,
        hostScale: false,
      ),
      const AndroidViewportPlan(width: 1280, height: 800, pixelScale: 2),
    );
    // A preset of the screen's shape is drawn scaled (no whole ratio), with
    // the scale for the device pixels it spans.
    final preset = AndroidScreenPolicy.of(
      const Size(1280, 800),
      2,
      tab,
      (_) =>
          const AndroidScreenChoice(resolution: AndroidResolution.preset(1200)),
    );
    final scaled = AndroidViewportPlan.of(
      const Size(2560, 1600),
      2,
      preset,
      hostScale: true,
    );
    expect((scaled.width, scaled.height, scaled.pixelScale), (1920, 1200, 0));
    // 1.75 / (2560 / 1920) = 1.31 → 125 %.
    expect(scaled.displayScale, 125);
  });

  test('device classes, densities and the 자동 formula across screens', () {
    AndroidDeviceClass kind(Size dp, [double? inches]) =>
        AndroidDeviceClass.of(dp, diagonalInches: inches);
    expect(kind(const Size(412, 915)), AndroidDeviceClass.phone);
    expect(kind(const Size(915, 412), 6.7), AndroidDeviceClass.phone);
    // A Fold's inner screen: 600 dp and more, under 9 inches.
    expect(kind(const Size(933, 704), 8), AndroidDeviceClass.fold);
    expect(kind(const Size(933, 704)), AndroidDeviceClass.fold);
    expect(kind(const Size(1280, 800), 11), AndroidDeviceClass.tablet);
    expect(kind(const Size(1280, 800)), AndroidDeviceClass.tablet);
    // An implausible xdpi (a wrong panel, the bucket's 160) is not believed.
    const odd = AndroidScreenDensity(
      xdpi: 96,
      ydpi: 96,
      width: 2560,
      height: 1600,
    );
    expect(odd.ppi(2), 320);
    expect(androidPpi(null, 2.625), 420);
    int auto(
      AndroidDeviceClass kind,
      double ppi,
      double w,
      double h,
      double dpr,
    ) => AndroidScreenPolicy(
      deviceClass: kind,
      density: AndroidScreenDensity(xdpi: ppi, ydpi: ppi, width: w, height: h),
    ).windowsScale(dpr, 1);
    // 10-inch 1920×1200 (224 ppi): 125 %; 14.6-inch 2960×1848 (239 ppi):
    // 175 %; an 8-inch 383-ppi inner screen: 200 %.
    expect(auto(AndroidDeviceClass.tablet, 224, 1920, 1200, 1.5), 125);
    expect(auto(AndroidDeviceClass.tablet, 239, 2960, 1848, 2), 175);
    expect(auto(AndroidDeviceClass.fold, 383, 2448, 1848, 2.625), 200);
    // Phones keep the tuned rule whatever the panel says.
    expect(auto(AndroidDeviceClass.phone, 500, 1080, 2400, 3), 200);
    expect(auto(AndroidDeviceClass.phone, 400, 1080, 2400, 2.625), 175);
  });

  test('해상도 options fit the screen shape; choices round-trip', () {
    final tablet = androidResolutionOptions(
      const Size(2560, 1600),
      deviceClass: AndroidDeviceClass.television,
    );
    expect(tablet.map((o) => o.resolution.encode()), [
      'auto',
      'w100',
      'half',
      'p900',
      'p1200',
      'p1440',
      'w125',
      'w150',
      'w200',
    ]);
    expect(tablet[2].size, const Size(1280, 800));
    expect(tablet[4].label, '1920×1200');
    final phone = androidResolutionOptions(
      const Size(1080, 2400),
      deviceClass: AndroidDeviceClass.television,
    );
    expect(phone.map((o) => o.resolution.encode()), [
      'auto',
      'w100',
      'half',
      'p720',
      'p900',
      'w125',
      'w150',
      'w200',
    ]);
    expect(phone[3].label, '720×1600');
    // A small screen keeps at least 360 rows: no 절반.
    expect(
      androidResolutionOptions(const Size(1280, 600))
          .any((o) => o.resolution == AndroidResolution.half),
      isFalse,
    );
    const choice = AndroidScreenChoice(
      scale: 225,
      resolution: AndroidResolution.preset(1080),
      orientation: AndroidOrientation.landscape,
    );
    expect(AndroidScreenChoice.parse(choice.encode()), choice);
    expect(choice.encode().length, lessThanOrEqualTo(128));
    expect(
      AndroidScreenChoice.parse('{"s":160,"r":"p999","o":"x"}'),
      const AndroidScreenChoice(),
    );
    expect(AndroidScreenChoice.parse('junk'), const AndroidScreenChoice());
  });

  test('the broker grant is exact and only follows a request', () {
    final origin = Uri.parse(_origin);
    final granted = BrokerConnection.fromJson(
      _connected(175),
      origin,
      requestedDisplayScale: 175,
    );
    expect(granted.displayScale, 175);
    expect(
      BrokerConnection.fromJson(_connected(), origin).displayScale,
      isNull,
    );
    expect(
      BrokerConnection.fromJson(
        _connected(),
        origin,
        requestedDisplayScale: 175,
      ).displayScale,
      isNull,
      reason: 'requested from a released Host: an ordinary session',
    );
    for (final (data, requested) in [
      (_connected(175), null),
      (_connected(150), 175),
      ({..._connected(175), 'displayScaleProtocol': 'display-scale-v2'}, 175),
      ({..._connected(), 'displayScale': 175}, 175),
    ]) {
      expect(
        () => BrokerConnection.fromJson(
          data,
          origin,
          requestedDisplayScale: requested,
        ),
        throwsA(isA<BrokerException>()),
      );
    }
    final desktop = BrokerDesktop.fromJson(
      _desktop({'displayScaleProtocol': 'display-scale-v1'}),
    );
    expect(desktop.displayScale, isTrue);
    expect(BrokerDesktop.fromJson(_desktop({})).displayScale, isFalse);
  });

  test('HTTP: requested only from an advertising inventory', () async {
    Future<(Map, BrokerConnection)> run(
      Map<String, Object> caps,
      Map<String, dynamic> reply, {
      int? scale = 175,
    }) async {
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
      final connection = scale == null
          ? await api.connect('fixture', 'pve-149')
          : await api.connectScaled(
              'fixture',
              'pve-149',
              options: const StreamOptions(),
              displayScale: scale,
            );
      return (
        jsonDecode(client.requests.last.body.toString()) as Map,
        connection,
      );
    }

    const released = {'service': true, 'streaming': true};
    final legacy = await run(released, _connected());
    expect(legacy.$1.containsKey('displayScaleProtocol'), isFalse);
    expect(legacy.$1.containsKey('displayScale'), isFalse);
    expect(legacy.$2.displayScale, isNull);
    const capable = {...released, 'displayScaleProtocol': 'display-scale-v1'};
    final scaled = await run(capable, _connected(175));
    expect(scaled.$1['displayScaleProtocol'], 'display-scale-v1');
    expect(scaled.$1['displayScale'], 175);
    expect(scaled.$2.displayScale, 175);
    // A plain connect never asks, even toward a capable Host.
    final plain = await run(capable, _connected(), scale: null);
    expect(plain.$1.containsKey('displayScaleProtocol'), isFalse);
    expect(plain.$2.displayScale, isNull);
    // A released broker's preflight refusal: retried without the fields.
    final client = ApiClient()
      ..responses.add((
        200,
        {
          'desktops': [_desktop(capable)],
        },
      ))
      ..responses.add((
        400,
        {
          'error': {'code': 'unexpected_field'},
        },
      ))
      ..responses.add((200, _connected()));
    final api = HttpBrokerApi(origin: Uri.parse(_origin), client: client);
    await api.desktops('fixture');
    final retried = await api.connectScaled(
      'fixture',
      'pve-149',
      options: const StreamOptions(),
      displayScale: 150,
    );
    final body = jsonDecode(client.requests.last.body.toString()) as Map;
    expect(body.containsKey('displayScale'), isFalse);
    expect(retried.displayScale, isNull);
    expect(
      () => api.connectScaled(
        'fixture',
        'pve-149',
        options: const StreamOptions(),
        displayScale: 160,
      ),
      throwsArgumentError,
    );
  });

  group('session', () {
    Future<(RemoteSession, SessionSocket)> open({int? granted = 175}) async {
      final socket = SessionSocket();
      final connection = BrokerConnection.fromJson(
        _connected(granted),
        Uri.parse(_origin),
        requestedDisplayScale: granted,
      );
      final session = RemoteSession(
        connection: connection,
        token: 'not-persisted',
        media: SessionMedia(),
        dialer: (_, _) async => socket,
      );
      unawaited(session.start());
      await flush();
      return (session, socket);
    }

    test('a request is sent once per change; the result is kept', () async {
      final (session, socket) = await open();
      expect(session.displayScaleSupported, isTrue);
      expect(session.displayScaleWanted, 175);
      expect(session.requestDisplayScale(175), isFalse, reason: 'already');
      expect(session.requestDisplayScale(160), isFalse, reason: 'no step');
      expect(session.requestDisplayScale(250), isTrue);
      expect(session.requestDisplayScale(250), isFalse);
      expect(socket.sent.where((m) => m['type'] == 'display.scale').toList(), [
        {'type': 'display.scale', 'scale': 250},
      ]);
      socket.incoming.add(jsonEncode(_applied(scale: 175, maximum: 175)));
      await flush();
      expect(session.displayScaleApplied?.scale, 175);
      expect(session.displayScaleApplied?.ok, isTrue);
      // Another session's result is ignored.
      socket.incoming.add(jsonEncode(_applied(scale: 100, session: 'old')));
      await flush();
      expect(session.displayScaleApplied?.scale, 175);
      expect(session.closed, isFalse);
      // A malformed result ends the stream like any invalid message.
      socket.incoming.add(jsonEncode(_applied(scale: 160)));
      await flush();
      expect(session.closed, isTrue);
      expect(session.diagnosticCode, 'invalid_stream_message');
    });

    test('an ungranted session never asks', () async {
      final (session, socket) = await open(granted: null);
      expect(session.displayScaleSupported, isFalse);
      expect(session.requestDisplayScale(200), isFalse);
      socket.incoming.add(jsonEncode(_applied()));
      await flush();
      expect(session.displayScaleApplied, isNull);
      expect(socket.sent.where((m) => m['type'] == 'display.scale'), isEmpty);
      await session.close();
    });
  });
}
