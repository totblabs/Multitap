// stream-size-v1 compatibility and v2 ABI geometry: decoder limits, the connect
// request only toward an advertising inventory (and the released fallback),
// the exact broker grant, portrait viewports (Windows and the Android plan),
// mid-epoch picture changes and decoder load reports. Injected HTTP, socket
// and media only: no device, Host, decoder or display is involved.
import 'dart:async';
import 'dart:convert';

import 'package:flutter/widgets.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:multitap_client/android/android_screen.dart'
    show AndroidScreenPolicy;
import 'package:multitap_client/android/android_stream_preparation.dart';
import 'package:multitap_client/android/android_touch.dart'
    show AndroidPictureView;
import 'package:multitap_client/data/broker_api.dart';
import 'package:multitap_client/data/desktop_mapping.dart';
import 'package:multitap_client/data/initial_viewport.dart';
import 'package:multitap_client/data/remote_session.dart';
import 'package:multitap_client/data/stream_size.dart';
import 'package:multitap_client/data/stream_stats_report.dart';
import 'package:multitap_client/data/video_geometry.dart';
import 'package:multitap_client/models/stream_options.dart';
import 'package:multitap_client/ui/remote_viewport.dart';

import 'power_api_test.dart' show ApiClient;
import 'remote_session_test.dart' show SessionSocket, flush, packet;
import 'viewport_session_test.dart' show ResizeMedia;

const _origin = 'https://fixture.invalid';
const _session = 'aaaaaaaa-aaaa-aaaa-aaaa-aaaaaaaaaaaa';
const _phone = DecoderLimits(
  maxWidth: 3840,
  maxHeight: 3840,
  maxPixels: 8294400,
  maxPixelRate: 248832000,
);
const _windows = DecoderLimits(
  maxWidth: 3840,
  maxHeight: 3840,
  maxPixels: 8294400,
  maxPixelRate: 0,
);
const _quality = StreamOptions(audio: false);
final _box = _quality.withBox(3840, 3840);

Map<String, Object> _caps({bool size = true}) => {
  'initialViewportProtocol': 'epoch-v1',
  'viewportEncoderLimitsProtocol': 'separate-v1',
  'resizeProtocol': 'epoch-v1',
  'dynamicResolution': true,
  'viewportProvisioning': true,
  'monitors': [
    {'id': '0', 'width': 1920, 'height': 1080, 'primary': true},
  ],
  'streamSizeProtocol': ?(size ? 'stream-size-v1' : null),
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
InitialViewport _viewport(int width, int height) => InitialViewport(
  width: width,
  height: height,
  fps: 60,
  preserveEncoderLimits: true,
);
Map<String, dynamic> _preparation(int width, int height) => {
  'protocol': 'epoch-v1',
  'sessionId': _session,
  'requestId': 'bbbbbbbb-bbbb-bbbb-bbbb-bbbbbbbbbbbb',
  'resizeId': 'cccccccc-cccc-cccc-cccc-cccccccccccc',
  'monitor': '0',
  'width': width,
  'height': height,
  'fps': 60,
  'epoch': 1,
};
Map<String, dynamic> _response(
  int width,
  int height, {
  bool granted = true,
  StreamOptions? options,
}) => {
  'sessionId': _session,
  'webSocketUrl': 'wss://fixture.invalid/api/v1/sessions/$_session',
  'mode': 'stream',
  'transport': 'media-epoch-v1',
  'resizeProtocol': 'epoch-v1',
  'viewportProvisioning': true,
  'initialViewportProtocol': 'epoch-v1',
  'initialViewport': _preparation(width, height),
  'viewportEncoderLimitsProtocol': 'separate-v1',
  'options': (options ?? _box).toJson(),
  if (granted) 'streamSizeProtocol': 'stream-size-v1',
};
BrokerConnection _connection(
  int width,
  int height, {
  bool granted = true,
  DecoderLimits limits = _windows,
}) => BrokerConnection.fromJson(
  _response(
    width,
    height,
    granted: granted,
    options: granted ? _box : _quality.withCodecCeiling(),
  ),
  Uri.parse(_origin),
  requestedOptions: granted ? _box : _quality.withCodecCeiling(),
  requestedInitialViewport: _viewport(width, height),
  requestedStreamSize: limits,
);
Map<String, dynamic> _info(int width, int height, int sourceW, int sourceH) => {
  'type': 'stream.info',
  ..._preparation(sourceW, sourceH),
  'codec': 'h264',
  'width': width,
  'height': height,
  'sourceWidth': sourceW,
  'sourceHeight': sourceH,
  'fpsTarget': 60,
};

class _PictureMedia extends ResizeMedia implements PictureChangeMedia {
  final restarts = <List<int>>[];
  Map<String, dynamic> extra = {};
  @override
  Future<Map<String, dynamic>> stats() async => {
    ...await super.stats(),
    ...extra,
  };
  @override
  Future<int> restart(int width, int height) async {
    restarts.add([width, height]);
    return start(width, height);
  }
}

// The decoder publishes readiness through RemoteSession's stats observation.
// Wait for that actual state transition, never a fixed timer scheduling guess.
Future<void> _pictureSettled(RemoteSession session) async {
  final settled = Completer<void>();
  void observe() {
    if (!settled.isCompleted && (session.closed || !session.pictureChanging)) {
      settled.complete();
    }
  }

  session.addListener(observe);
  try {
    observe();
    await settled.future.timeout(
      const Duration(seconds: 3),
      onTimeout: () => fail(
        'The current picture did not reach decoded readiness through a '
        'session notification.',
      ),
    );
    expect(session.closed, isFalse, reason: 'readiness cannot come from close');
  } finally {
    session.removeListener(observe);
  }
}

class _DelayedPictureMedia extends _PictureMedia {
  final restartStarted = Completer<void>();
  final restartResult = Completer<int>();
  final captureStarted = Completer<void>();
  Completer<bool>? nextCapture;

  @override
  Future<int> restart(int width, int height) {
    restarts.add([width, height]);
    restartStarted.complete();
    return restartResult.future;
  }

  @override
  Future<bool> capture(bool enabled) {
    final pending = enabled ? nextCapture : null;
    if (pending == null) return super.capture(enabled);
    nextCapture = null;
    captures.add(enabled);
    captureStarted.complete();
    return pending.future;
  }
}

void main() {
  TestWidgetsFlutterBinding.ensureInitialized();

  test('owned outputs, encoded pictures, decoder limits and reports', () {
    expect(legacyOwnedOutputSize(1080, 2400), isFalse, reason: 'released rule');
    expect(legacyOwnedOutputSize(1080, 2400, portrait: true), isTrue);
    expect(legacyOwnedOutputSize(2160, 3840, portrait: true), isTrue);
    for (final (w, h) in [(3840, 3840), (2162, 3840), (1081, 2400)]) {
      expect(
        legacyOwnedOutputSize(w, h, portrait: true),
        isFalse,
        reason: '$w×$h',
      );
    }
    expect(encodedPictureSize(864, 1920, portrait: true), isTrue);
    expect(
      encodedPictureSize(1080, 2400),
      isTrue,
      reason: 'ABI geometry is separate from a released v1 grant',
    );
    expect(
      DecoderLimits.fromNative({
        'maxWidth': 4096,
        'maxHeight': 4096,
        'maxPixels': 9437184,
        'maxPixelRate': 600000000,
      })!.legacy,
      const DecoderLimits(
        maxWidth: 3840,
        maxHeight: 3840,
        maxPixels: 8294400,
        maxPixelRate: 497664000,
      ),
      reason: 'v1 compatibility explicitly clamps the informational envelope',
    );
    expect(
      DecoderLimits.fromNative({
        'maxWidth': 4096,
        'maxHeight': 4096,
        'maxPixels': 9437184,
        'maxPixelRate': 600000000,
      }),
      const DecoderLimits(
        maxWidth: 4096,
        maxHeight: 4096,
        maxPixels: 9437184,
        maxPixelRate: 600000000,
      ),
    );
    expect(DecoderLimits.fromNative(_windows.toJson()), _windows);
    for (final bad in [
      null,
      'limits',
      {'maxWidth': 3840},
      {..._windows.toJson(), 'maxWidth': 15},
      {..._windows.toJson(), 'maxPixelRate': 5},
    ]) {
      expect(DecoderLimits.fromNative(bad), isNull, reason: '$bad');
    }
    expect(DecoderLimits.safeDefault.valid, isTrue);
    expect(_phone.fits(1080, 2400, 60), isTrue);
    expect(_phone.fits(3840, 2160, 60), isFalse, reason: '4K60 above 4K30');
    expect(_phone.fits(3840, 2160, 30), isTrue);
    expect(
      _windows.fits(2398, 3458, 60),
      isFalse,
      reason: 'whole macroblocks exceed one 4K frame',
    );
    expect(decoderReport(decodeMs: 450, elapsedMs: 1000, backlogMs: 12.4), {
      'type': 'stream.decoder',
      'busy': 450,
      'backlogMs': 12,
    });
    expect(decoderReport(decodeMs: null, elapsedMs: 1000, backlogMs: null), {
      'type': 'stream.decoder',
      'busy': -1,
      'backlogMs': 0,
    });
    expect(decoderReport(decodeMs: 5000, elapsedMs: 1000, backlogMs: 99999), {
      'type': 'stream.decoder',
      'busy': 1000,
      'backlogMs': 60000,
    });
    expect(EncodeLimit.parse('host_cpu'), EncodeLimit.hostCpu);
    expect(EncodeLimit.parse('other'), isNull);
    expect(_box.toJson()['maxHeight'], 3840);
    expect(const StreamOptions(maxHeight: 3842).toJson()['maxHeight'], 3842);
    expect(legacyOwnedOutputSize(3840, 3842, portrait: true), isFalse);
    expect(
      () => const StreamOptions(maxHeight: maximumGeometrySide + 2).toJson(),
      throwsArgumentError,
    );
  });

  test('v2 large geometry is arithmetic only, never a decoder or memory capability grant', () {
    for (final (width, height) in [
      (5120, 2880),
      (5120, 1440),
      (2880, 5120),
      (7680, 4320),
    ]) {
      expect(ownedOutputSize(width, height), isTrue);
      expect(encodedPictureSize(width, height), isTrue);
      expect(legacyOwnedOutputSize(width, height, portrait: true), isFalse);
      expect(
        _windows.fits(width, height, 60),
        isFalse,
        reason: 'valid geometry does not override actual decoder limits',
      );
    }
    for (final (width, height) in [
      (0, 1080),
      (-320, 180),
      (5121, 2880),
      (maximumGeometrySide + 2, 180),
      (maximumGeometrySide, maximumGeometrySide),
    ]) {
      expect(ownedOutputSize(width, height), isFalse);
      expect(encodedPictureSize(width, height), isFalse);
    }
  });

  test('inventory: only a coherent advertisement survives the mapping', () {
    final desktop = BrokerDesktop.fromJson(_desktop(_caps()));
    expect(desktop.streamSize, isTrue);
    expect(machineFromBroker(desktop).streamSize, isTrue);
    expect(
      BrokerDesktop.fromJson(_desktop(_caps(size: false))).streamSize,
      isFalse,
    );
    expect(
      BrokerDesktop.fromJson(
        _desktop({..._caps(), 'viewportEncoderLimitsProtocol': 'x'}),
      ).streamSize,
      isFalse,
    );
  });

  test('the broker grant is exact and follows only a request', () {
    expect(_connection(1080, 2400).streamSize, _windows);
    expect(_connection(2560, 1440, granted: false).streamSize, isNull);
    // A grant nobody asked for, or for the wrong protocol, is refused.
    expect(
      () => BrokerConnection.fromJson(
        _response(2560, 1440),
        Uri.parse(_origin),
        requestedOptions: _box,
        requestedInitialViewport: _viewport(2560, 1440),
      ),
      throwsA(isA<BrokerException>()),
    );
    expect(
      () => BrokerConnection.fromJson(
        {..._response(2560, 1440), 'streamSizeProtocol': 'stream-size-v2'},
        Uri.parse(_origin),
        requestedOptions: _box,
        requestedInitialViewport: _viewport(2560, 1440),
        requestedStreamSize: _windows,
      ),
      throwsA(isA<BrokerException>()),
    );
  });

  test(
    'HTTP: asked only toward an advertising inventory, with the decoder limits',
    () async {
      Future<(Map, BrokerConnection)> run(
        Map<String, Object> caps,
        InitialViewport viewport,
        Map<String, dynamic> reply, {
        Future<DecoderLimits?> Function()? limits,
      }) async {
        final client = ApiClient()
          ..responses.add((
            200,
            {
              'desktops': [_desktop(caps)],
            },
          ))
          ..responses.add((200, reply));
        final api = HttpBrokerApi(
          origin: Uri.parse(_origin),
          client: client,
          decoderLimits: limits ?? () async => _phone,
        );
        await api.desktops('fixture');
        final connection = await api.connectViewport(
          'fixture',
          'pve-149',
          viewport,
          _quality,
        );
        return (
          jsonDecode(client.requests.last.body.toString()) as Map,
          connection,
        );
      }

      final portrait = await run(
        _caps(),
        _viewport(1080, 2400),
        _response(1080, 2400),
      );
      expect(portrait.$1['streamSizeProtocol'], 'stream-size-v1');
      expect(portrait.$1['decoderLimits'], _phone.toJson());
      expect((portrait.$1['options'] as Map)['maxWidth'], 3840);
      expect((portrait.$1['options'] as Map)['maxHeight'], 3840);
      expect((portrait.$1['initialViewport'] as Map)['height'], 2400);
      expect(portrait.$2.streamSize, _phone);
      // An engine that cannot say declares today's safe default.
      final unknown = await run(
        _caps(),
        _viewport(2560, 1440),
        _response(2560, 1440),
        limits: () async => null,
      );
      expect(unknown.$1['decoderLimits'], DecoderLimits.safeDefault.toJson());
      // A released Host (or broker): today's request, no fields, codec box.
      final released = await run(
        _caps(size: false),
        _viewport(2560, 1440),
        _response(
          2560,
          1440,
          granted: false,
          options: _quality.withCodecCeiling(),
        ),
      );
      expect(released.$1.containsKey('streamSizeProtocol'), isFalse);
      expect(released.$1.containsKey('decoderLimits'), isFalse);
      expect((released.$1['options'] as Map)['maxHeight'], 2160);
      expect(released.$2.streamSize, isNull);
      // A portrait output never goes to a released Host.
      await expectLater(
        run(_caps(size: false), _viewport(1080, 2400), _response(1080, 2400)),
        throwsA(
          isA<BrokerException>().having(
            (e) => e.code,
            'code',
            'stream_size_unavailable',
          ),
        ),
      );
    },
  );

  test('Windows physical requests preserve portrait and square source pixels; legacy compatibility stays separate', () {
    // A 2160x3840 portrait 4K monitor at 200 %.
    expect(
      viewportResolution(const Size(1080, 1920), 2, portrait: true),
      const Size(2160, 3840),
    );
    expect(
      viewportResolution(const Size(1080, 1920), 2),
      const Size(2160, 3840),
    );
    expect(legacyOwnedOutputSize(2160, 3840), isFalse);
    // Requesting this workspace does not imply it was admitted by a device.
    final wide = viewportResolution(const Size(3840, 3840), 1, portrait: true)!;
    expect(wide, const Size(3840, 3840));
    expect(
      legacyOwnedOutputSize(
        wide.width.toInt(),
        wide.height.toInt(),
        portrait: true,
      ),
      isFalse,
    );
  });

  test('Android: bounded portrait sources keep aspect and honest fractional pixel scale within negotiated limits', () {
    AndroidViewportPlan plan(
      Size physical, {
      DecoderLimits? limits,
      int fps = 60,
    }) => AndroidViewportPlan.of(
      physical,
      3,
      const AndroidScreenPolicy(),
      hostScale: true,
      portraitLimits: limits,
      fps: fps,
    );
    const phone = Size(1080, 2400);
    expect(
      plan(phone, limits: _phone),
      const AndroidViewportPlan(
        width: 576,
        height: 1280,
        pixelScale: 0,
        displayScale: 100,
      ),
    );
    // The adaptive source already fits below the released2160-row cap.
    // Fractional fit must not be described as integer/native pixel mapping.
    expect(
      plan(phone),
      const AndroidViewportPlan(
        width: 576,
        height: 1280,
        pixelScale: 0,
        displayScale: 100,
      ),
    );
    // A decoder capped at2304 rows still admits the adaptive fractional source.
    expect(
      plan(
        phone,
        limits: const DecoderLimits(
          maxWidth: 3840,
          maxHeight: 2304,
          maxPixels: 8294400,
          maxPixelRate: 497664000,
        ),
      ).pixelScale,
      0,
    );
    // Automatic readability reduces this source before the2560long safety bound.
    final large = plan(const Size(1440, 3200), limits: _phone);
    expect(large.size, const Size(768, 1706));
    expect(large.pixelScale, 0);
    expect(_phone.fits(large.width, large.height, 60), isTrue);
    expect(
      plan(const Size(1440, 3200), limits: _phone, fps: 30).size,
      large.size,
    );
    // No DPI capability still preserves the existing exact-integer fallback.
    final oldHost = AndroidViewportPlan.of(
      phone,
      3,
      const AndroidScreenPolicy(),
      hostScale: false,
    );
    expect(oldHost.size, const Size(540, 1200));
    expect(oldHost.pixelScale, 2);
    expect(oldHost.displayScale, isNull);
    // The options the plan is requested with keep it whole only with the grant.
    final portrait = androidViewportOptions(phone, _quality, portrait: true);
    expect((portrait.maxWidth, portrait.maxHeight), (1080, 2400));
    final released = androidViewportOptions(phone, _quality);
    expect((released.maxWidth, released.maxHeight), (972, 2160));
  });

  test('Android Fold: connected folded, unfolded, the whole picture fills the screen', () async {
    // Field data 2026-10-26 (Fold 8): connected folded at 1248x1868, then
    // unfolded to a 2448x1848 output (2.625 device pixels per logical one).
    const fold = DecoderLimits(
      maxWidth: 3840,
      maxHeight: 3840,
      maxPixels: 8294400,
      maxPixelRate: 497664000,
    );
    final client = ApiClient()
      ..responses.add((
        200,
        {
          'desktops': [_desktop(_caps())],
        },
      ))
      ..responses.add((200, _response(1248, 1868)));
    final api = HttpBrokerApi(
      origin: Uri.parse(_origin),
      client: client,
      decoderLimits: () async => fold,
    );
    await api.desktops('fixture');
    // AndroidStreamPreparation asks with the default box where the Host keeps
    // encoder limits apart from the first viewport; stream-size-v1 widens it
    // to the per-side ceilings, never to the folded size.
    final connection = await api.connectViewport(
      'fixture',
      'pve-149',
      _viewport(1248, 1868),
      const StreamOptions(audio: false),
    );
    final request = jsonDecode(client.requests.last.body.toString()) as Map;
    expect(request['options'], containsPair('maxWidth', 3840));
    expect(request['options'], containsPair('maxHeight', 3840));
    expect(request['initialViewport'], containsPair('width', 1248));
    expect(connection.streamSize, fold);
    // Unfolded: this default policy adapts its source to the actual DPR.
    const unfolded = Size(2448, 1848), dpr = 2.625;
    final plan = AndroidViewportPlan.of(
      unfolded,
      dpr,
      const AndroidScreenPolicy(),
      hostScale: true,
      portraitLimits: connection.streamSize,
    );
    expect((plan.width, plan.height, plan.pixelScale), (1492, 1126, 0));
    final area = Rect.fromLTWH(
      0,
      0,
      unfolded.width / dpr,
      unfolded.height / dpr,
    );
    // The whole output fills the screen without cropping ...
    final whole = AndroidPictureView.fit(
      area,
      unfolded,
      devicePixelRatio: dpr,
      pixelScale: plan.pixelScale,
    );
    expect(whole.rect.width, closeTo(area.width, .5));
    expect(whole.rect.height, closeTo(area.height, .5));
    // ... and a picture the Host reduced under load is scaled up to fill.
    final reduced = AndroidPictureView.fit(
      area,
      const Size(1958, 1478),
      devicePixelRatio: dpr,
      pixelScale: plan.pixelScale,
    );
    expect(reduced.rect.width, closeTo(area.width, .5));
  });

  group('session', () {
    Future<(RemoteSession, SessionSocket, _PictureMedia, Future<void>)> open(
      BrokerConnection connection,
      Map<String, dynamic> first, {
      _PictureMedia? engine,
    }) async {
      final socket = SessionSocket(), media = engine ?? _PictureMedia();
      final session = RemoteSession(
        connection: connection,
        token: 'fixture',
        media: media,
        dialer: (_, _) async => socket,
      );
      final running = session.start();
      socket.incoming.add(jsonEncode(first));
      await flush();
      socket.incoming.add(packet()..[2] = 1);
      await Future<void>.delayed(const Duration(milliseconds: 120));
      return (session, socket, media, running);
    }

    Future<(RemoteSession, SessionSocket, _DelayedPictureMedia)>
    pausedPicture() async {
      final media = _DelayedPictureMedia();
      final (session, socket, _, running) = await open(
        _connection(1080, 2400),
        _info(1080, 2400, 1080, 2400),
        engine: media,
      );
      addTearDown(() async {
        await session.close();
        if (!media.restartResult.isCompleted) media.restartResult.complete(43);
        await running;
        await media.events.close();
      });
      await session.capture(true);
      socket.incoming.add(jsonEncode(_info(864, 1920, 1080, 2400)));
      await media.restartStarted.future.timeout(const Duration(seconds: 1));
      return (session, socket, media);
    }

    test('focus/menu release during a picture restart never reacquires native capture', () async {
      final (session, socket, media) = await pausedPicture();
      await session.capture(false);
      media.restartResult.complete(43);
      await flush();
      expect(media.captures, [true, false]);
      expect(session.captured, isFalse);
      final before = socket.sent
          .where((event) => event['type'] == 'input')
          .length;
      session.input({'type': 'input', 'event': 'move', 'x': .2, 'y': .3});
      expect(
        socket.sent.where((event) => event['type'] == 'input').length,
        before,
      );
      expect(session.closed, isFalse);
    });

    test('native capture release during a picture restart supersedes its old ownership', () async {
      final (session, _, media) = await pausedPicture();
      media.events.add({'type': 'capture.released'});
      await flush();
      media.restartResult.complete(43);
      await flush();
      expect(media.captures, [true]);
      expect(session.captured, isFalse);
    });

    test('fresh capture during a picture restart is not replaced by its old restoration', () async {
      final (session, _, media) = await pausedPicture();
      await session.capture(false);
      await session.capture(true);
      media.restartResult.complete(43);
      await flush();
      expect(media.captures, [true, false, true]);
      expect(session.captured, isTrue);
    });

    test(
      'late rejected restoration cannot clear a newer successful capture',
      () async {
        final (session, socket, media) = await pausedPicture();
        final oldCapture = Completer<bool>();
        media.nextCapture = oldCapture;
        media.restartResult.complete(43);
        await media.captureStarted.future.timeout(const Duration(seconds: 1));
        await session.capture(false);
        await session.capture(true);
        oldCapture.complete(false);
        await flush();
        expect(session.captured, isTrue);
        session.input({'type': 'input', 'event': 'move', 'x': .2, 'y': .3});
        expect(socket.sent.last['type'], 'input');
      },
    );

    test(
      'disconnect during decoder restart never restores input capture',
      () async {
        final (session, _, media) = await pausedPicture();
        await session.close();
        media.restartResult.complete(43);
        await flush();
        expect(media.captures, [true, false]);
        expect(session.closed, isTrue);
        expect(session.captured, isFalse);
      },
    );

    test(
      'late restoration result cannot notify or alter a closed session',
      () async {
        final (session, _, media) = await pausedPicture();
        final oldCapture = Completer<bool>();
        media.nextCapture = oldCapture;
        media.restartResult.complete(43);
        await media.captureStarted.future.timeout(const Duration(seconds: 1));
        var notifications = 0;
        session.addListener(() => notifications++);
        await session.close();
        final afterClose = notifications;
        oldCapture.complete(true);
        await flush();
        expect(notifications, afterClose);
        expect(session.closed, isTrue);
        expect(session.captured, isFalse);
        expect(media.captures.last, isFalse);
      },
    );

    test(
      'unchanged focused ownership resumes capture after the picture restart',
      () async {
        final (session, _, media) = await pausedPicture();
        media.restartResult.complete(43);
        await flush();
        expect(media.captures, [true, true]);
        expect(session.captured, isTrue);
      },
    );

    test('a portrait source streams 1:1 and the Host may change the picture within the epoch', () async {
      final (session, socket, media, running) = await open(
        _connection(1080, 2400),
        {
          ..._info(1080, 2400, 1080, 2400),
          'encodeScale': 1000,
          'encodeLimit': 'none',
        },
      );
      expect(session.closed, isFalse);
      expect((session.width, session.height), (1080, 2400));
      expect(session.encodeLimit, EncodeLimit.none);
      expect(session.frameReady, isTrue);
      // The Host's encoder overloaded: a smaller picture of the same source.
      socket.incoming.add(
        jsonEncode({
          ..._info(864, 1920, 1080, 2400),
          'encodeScale': 800,
          'encodeLimit': 'host_cpu',
        }),
      );
      await flush();
      expect(media.restarts, [
        [864, 1920],
      ]);
      expect((session.width, session.height), (864, 1920));
      expect((session.sourceWidth, session.sourceHeight), (1080, 2400));
      expect(session.encodeScale, 800);
      expect(session.encodeLimit, EncodeLimit.hostCpu);
      expect(session.pictureChanging, isTrue);
      expect(session.resizing, isFalse, reason: 'input continues');
      // Frames before the IDR belong to no picture; the IDR completes it.
      final pictureSettled = _pictureSettled(session);
      socket.incoming.add(
        packet()
          ..[2] = 1
          ..[1] = 0,
      );
      socket.incoming.add(packet()..[2] = 1);
      await pictureSettled;
      expect(session.pictureChanging, isFalse);
      // A load report goes out with the statistics tick.
      await Future<void>.delayed(const Duration(milliseconds: 1100));
      final reports = socket.sent
          .where((m) => m['type'] == 'stream.decoder')
          .toList();
      expect(reports, isNotEmpty);
      expect(
        reports.first.keys,
        unorderedEquals(['type', 'busy', 'backlogMs']),
      );
      // A picture larger than its source is invalid metadata.
      socket.incoming.add(jsonEncode(_info(1082, 2400, 1080, 2400)));
      await flush();
      expect(session.closed, isTrue);
      expect(session.diagnosticCode, 'invalid_stream_message');
      await running;
      await media.events.close();
    });

    test(
      'the decoder report uses utilisation, not overlapping decode latencies',
      () async {
        final (session, socket, media, running) = await open(
          _connection(1080, 2400),
          _info(1080, 2400, 1080, 2400),
        );
        // A pipelined decoder: summed latencies 5 s per second, busy 0.25 s.
        media.extra = {'decodeTotalMs': 5000.0, 'decodeBusyMs': 250.0};
        socket.sent.clear();
        await Future<void>.delayed(const Duration(milliseconds: 1300));
        final reports = socket.sent
            .where((m) => m['type'] == 'stream.decoder')
            .toList();
        expect(reports, isNotEmpty);
        final busy = reports.first['busy'] as int;
        expect(busy, inInclusiveRange(1, 600), reason: 'not the clamped sum');
        await session.close();
        await running;
        await media.events.close();
      },
    );

    test(
      'a picture above the declared decoder limits ends the stream',
      () async {
        final (session, socket, media, running) = await open(
          _connection(3840, 2160, limits: _phone),
          _info(2560, 1440, 3840, 2160),
        );
        expect(session.closed, isFalse);
        socket.incoming.add(jsonEncode(_info(3840, 2160, 3840, 2160)));
        await flush();
        expect(session.closed, isTrue);
        expect(media.restarts, isEmpty);
        await running;
        await media.events.close();
      },
    );

    test(
      'an ungranted session ignores a same-epoch picture and never reports',
      () async {
        final (session, socket, media, running) = await open(
          _connection(2560, 1440, granted: false),
          _info(1280, 720, 2560, 1440),
        );
        socket.incoming.add(jsonEncode(_info(1920, 1080, 2560, 1440)));
        await flush();
        expect(session.closed, isFalse);
        expect(session.width, 1280);
        expect(media.restarts, isEmpty);
        expect(session.streamSize, isFalse);
        await Future<void>.delayed(const Duration(milliseconds: 1100));
        expect(
          socket.sent.where((m) => m['type'] == 'stream.decoder'),
          isEmpty,
        );
        await session.close();
        await running;
        await media.events.close();
      },
    );
  });

  test('stream statistics carry the picture scale, its reason and the decoder load', () {
    final window = StreamStatsWindow();
    for (final busy in [400, 600]) {
      window.add(
        StreamStatsSample(
          width: 864,
          height: 1920,
          encodeScale: 800,
          encodeLimit: EncodeLimit.hostCpu.code,
          decoderBusy: busy,
        ),
        0,
      );
    }
    final values = window.take(1000000)!;
    expect(values['encodeScale'], 800);
    expect(values['encodeLimit'], 4);
    expect(values['decoderBusy'], 500);
    expect(
      streamStatsFields.containsAll([
        'encodeScale',
        'encodeLimit',
        'decoderBusy',
      ]),
      isTrue,
    );
  });
}
