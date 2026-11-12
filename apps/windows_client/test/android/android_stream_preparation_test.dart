import 'dart:async';

import 'package:flutter/services.dart';
import 'package:flutter/material.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:multitap_client/android/android_app.dart';
import 'package:multitap_client/android/android_remote.dart';
import 'package:multitap_client/android/android_session_keepalive.dart';
import 'package:multitap_client/android/android_store.dart';
import 'package:multitap_client/android/android_stream_preparation.dart';
import 'package:multitap_client/data/broker_api.dart';
import 'package:multitap_client/data/initial_viewport.dart';
import 'package:multitap_client/data/track_stream.dart';
import 'package:multitap_client/models/session_cleanup.dart';
import 'package:multitap_client/models/stream_options.dart';

import 'android_takeover_context_test.dart' show androidOffer;

const _optionalPenChannel = MethodChannel('multitap/pen-input');

const id = '12345678-1234-1234-1234-123456789012';
const otherId = '92345678-1234-1234-1234-123456789012';
final checked = DateTime.utc(2026, 10, 24);

VirtualDisplayState state({
  int count = 1,
  String? operation = id,
  String status = 'succeeded',
  bool stale = false,
  bool busy = false,
  int requestedCount = 1,
}) => VirtualDisplayState(
  supported: true,
  activeCount: count,
  checkedAt: checked,
  stale: stale,
  busy: busy,
  operation: operation == null
      ? null
      : VirtualDisplayOperation(
          id: operation,
          status: status,
          requestedCount: requestedCount,
        ),
);

BrokerDesktop desktop({
  int count = 1,
  VirtualDisplayState? display,
  bool control = true,
  List<RemoteMonitor>? monitors,
  bool cleanup = false,
  bool initialViewport = false,
  bool encoderLimits = false,
  bool scaled = false,
  bool profileTracks = false,
}) => BrokerDesktop(
  id: 'pve-149',
  name: 'TESTER',
  kind: 'vm',
  description: '',
  os: 'Windows',
  status: 'online',
  canConnect: true,
  canStream: true,
  hostConnected: true,
  virtualMonitorControl: control,
  initialViewport: initialViewport,
  viewportEncoderLimits: encoderLimits,
  displayScale: scaled,
  profileTracks: profileTracks,
  sessionCleanup: cleanup
      ? SessionCleanupState(
          pending: true,
          phase: 'pending',
          checkedAt: DateTime.now().toUtc(),
        )
      : const SessionCleanupState(),
  display: display ?? state(count: count, operation: null),
  monitors:
      monitors ??
      [
        for (var i = 0; i < count; i++)
          RemoteMonitor(id: '$i', width: 1920, height: 1080),
      ],
);

/// A current broker: initial viewport and takeover, as HttpBrokerApi has.
class FakeCurrentApi extends FakeApi
    implements
        InitialViewportBrokerApi,
        TakeoverBrokerApi,
        ConfirmedTakeoverBrokerApi {
  InitialViewport? viewport;
  bool released = true;

  @override
  Future<BrokerConnection> connectViewport(
    String token,
    String desktopId,
    InitialViewport viewport,
    StreamOptions options,
  ) async {
    calls.add('connectViewport');
    this.viewport = viewport;
    requested = options;
    return BrokerConnection(
      sessionId: 'fixture',
      webSocketUrl: Uri.parse('wss://fixture.example/relay'),
      options: options,
    );
  }

  @override
  Future<bool> takeover(String token, String desktopId) async {
    calls.add('takeover:$desktopId');
    return released;
  }

  @override
  Future<bool> confirmTakeover(
    String token,
    String desktopId,
    StreamTakeoverOffer offer,
  ) async {
    expect(offer.desktopId, desktopId);
    calls.add('takeover:$desktopId');
    return released;
  }
}

/// A broker with display-scale-v1 (as HttpBrokerApi has).
class FakeScaledApi extends FakeCurrentApi implements DisplayScaleBrokerApi {
  int? scale;

  @override
  Future<BrokerConnection> connectScaled(
    String token,
    String desktopId, {
    required StreamOptions options,
    InitialViewport? initialViewport,
    required int displayScale,
  }) async {
    calls.add('connectScaled');
    viewport = initialViewport;
    scale = displayScale;
    requested = options;
    return BrokerConnection(
      sessionId: 'fixture',
      webSocketUrl: Uri.parse('wss://fixture.example/relay'),
      options: options,
      displayScale: displayScale,
    );
  }
}

class FakeApi implements BrokerApi {
  final calls = <String>[];
  late Future<List<BrokerDesktop>> Function() inventory;
  Future<VirtualDisplayState> Function()? apply;
  Future<VirtualDisplayState> Function(String?)? observe;
  StreamOptions? requested;

  @override
  Future<List<BrokerDesktop>> desktops(String token) {
    calls.add('inventory');
    return inventory();
  }

  @override
  Future<VirtualDisplayState> configureDisplays(
    String token,
    String desktopId,
    int count,
  ) {
    calls.add('count:$count');
    return apply!();
  }

  @override
  Future<VirtualDisplayState> display(
    String token,
    String desktopId, {
    String? operationId,
  }) {
    calls.add('observe:$operationId');
    return observe!(operationId);
  }

  @override
  Future<BrokerConnection> connect(
    String token,
    String desktopId, {
    String mode = 'stream',
    StreamOptions? options,
  }) async {
    calls.add('connect');
    requested = options;
    return BrokerConnection(
      sessionId: 'fixture',
      webSocketUrl: Uri.parse('wss://fixture.example/relay'),
      options: options!,
    );
  }

  @override
  dynamic noSuchMethod(Invocation invocation) =>
      throw UnimplementedError(invocation.memberName.toString());
}

class FakeTrackApi extends FakeApi implements ProfileTrackBrokerApi {
  TrackPlan? plan;
  @override
  Future<BrokerConnection> connectProfileTracks(
    String token,
    String desktopId,
    TrackPlan tracks,
    StreamOptions options, {
    int? displayScale,
  }) async {
    calls.add('tracks:${tracks.outputs.length}');
    plan = tracks;
    requested = options;
    return BrokerConnection(
      sessionId: 'dual-fixture',
      webSocketUrl: Uri.parse('wss://fixture.example/relay'),
      options: options,
      trackPlan: tracks,
      profileTracks: true,
    );
  }
}

void main() {
  TestWidgetsFlutterBinding.ensureInitialized();
  test('Android explicitly requests two independent outputs without reducing the Host to one', () async {
    final api = FakeTrackApi()
      ..inventory = () async => [desktop(profileTracks: true)];
    const options = StreamOptions(
      maxWidth: 1280,
      maxHeight: 720,
      monitor: 'all',
    );
    final result = await AndroidStreamPreparation(
      api: api,
      authorized: () => true,
    ).connect('qa', 'pve-149', options);
    expect(result.trackPlan!.outputs.length, 2);
    expect(api.calls, ['inventory', 'tracks:2']);
    expect(api.plan!.outputs.map((track) => (track.id, track.x, track.y)), [
      ('0', 0, 0),
      ('1', 1280, 0),
    ]);
    expect(api.requested!.monitor, 'all');
  });
  setUp(() {
    // ACK only: this fixture does not advertise native pen readiness.
    TestDefaultBinaryMessengerBinding.instance.defaultBinaryMessenger
        .setMockMethodCallHandler(_optionalPenChannel, (_) async => null);
    TestDefaultBinaryMessengerBinding.instance.defaultBinaryMessenger
        .setMockMethodCallHandler(
          AndroidSessionKeepalive.channel,
          (call) async => call.method == 'start' ? 'started' : true,
        );
  });
  tearDown(() {
    TestDefaultBinaryMessengerBinding.instance.defaultBinaryMessenger
        .setMockMethodCallHandler(_optionalPenChannel, null);
    TestDefaultBinaryMessengerBinding.instance.defaultBinaryMessenger
        .setMockMethodCallHandler(AndroidSessionKeepalive.channel, null);
  });
  const quality = StreamOptions(fps: 30, bitrate: 8000000, audio: false);
  final options = androidViewportOptions(const Size(2560, 1440), quality);

  test(
    'physical viewport uses actual pixels and retains quality, one output',
    () {
      expect(options.maxWidth, 2560);
      expect(options.maxHeight, 1440);
      expect(options.fps, 30);
      expect(options.bitrate, 8000000);
      expect(options.audio, false);
      expect(options.monitor, '0');
      final portrait = androidViewportOptions(const Size(1080, 2400), quality);
      expect((portrait.maxWidth, portrait.maxHeight), (972, 2160));
      final landscape = androidViewportOptions(const Size(3841, 2161), quality);
      expect(landscape.maxWidth.isEven && landscape.maxHeight.isEven, true);
      expect(landscape.maxWidth <= 3840 && landscape.maxHeight <= 2160, true);
      for (final invalid in [
        Size.zero,
        const Size(double.infinity, 500),
        const Size(50, 50),
      ]) {
        expect(
          () => androidViewportOptions(invalid, quality),
          throwsA(isA<BrokerException>()),
        );
      }
    },
  );

  test('the automatic bitrate (자동, the default) survives the viewport', () {
    final automatic = const AndroidStreamQuality().options;
    expect(automatic.automaticBitrate, true);
    final sized = androidViewportOptions(const Size(2400, 1080), automatic);
    expect(sized.automaticBitrate, true);
    expect(sized.toJson()['bitrateMode'], 'auto');
    // A fixed choice stays fixed.
    final fixed = androidViewportOptions(
      const Size(2400, 1080),
      const AndroidStreamQuality(bitrate: 8).options,
    );
    expect(fixed.automaticBitrate, false);
    expect(fixed.bitrate, 8000000);
  });

  test(
    'a Windows scale is asked for only where the inventory advertises it',
    () async {
      final scaled = FakeScaledApi()
        ..inventory = () async => [
          desktop(initialViewport: true, scaled: true),
        ];
      final connection = await AndroidStreamPreparation(
        api: scaled,
        authorized: () => true,
      ).connect('token', 'pve-149', options, displayScale: 175);
      expect(scaled.calls, ['inventory', 'connectScaled']);
      expect(scaled.scale, 175);
      expect((scaled.viewport?.width, scaled.viewport?.height), (2560, 1440));
      expect(connection.displayScale, 175);
      // Not advertised (released Host), or nothing asked: the usual connect.
      final released = FakeScaledApi()
        ..inventory = () async => [desktop(initialViewport: true)];
      await AndroidStreamPreparation(
        api: released,
        authorized: () => true,
      ).connect('token', 'pve-149', options, displayScale: 175);
      expect(released.calls, ['inventory', 'connectViewport']);
      final unasked = FakeScaledApi()
        ..inventory = () async => [desktop(scaled: true)];
      await AndroidStreamPreparation(
        api: unasked,
        authorized: () => true,
      ).connect('token', 'pve-149', options);
      expect(unasked.calls, ['inventory', 'connect']);
    },
  );

  test('one fresh owned screen connects without a display mutation', () async {
    final api = FakeApi()..inventory = () async => [desktop()];
    await AndroidStreamPreparation(
      api: api,
      authorized: () => true,
    ).connect('not-persisted', 'pve-149', options);
    expect(api.calls, ['inventory', 'connect']);
    expect(api.requested, same(options));
  });

  test(
    'two outputs become one before fresh inventory and relay ticket',
    () async {
      var reads = 0;
      final applied = state();
      final api = FakeApi();
      api.inventory = () async => [
        ++reads == 1 ? desktop(count: 2) : desktop(display: applied),
      ];
      api.apply = () async => applied;
      await AndroidStreamPreparation(
        api: api,
        authorized: () => true,
      ).connect('token', 'pve-149', options);
      expect(api.calls, ['inventory', 'count:1', 'inventory', 'connect']);
    },
  );

  test(
    'ACK alone cannot connect before exact completed count and fresh monitors',
    () async {
      var reads = 0;
      final api = FakeApi();
      api.inventory = () async => [
        ++reads == 1 ? desktop(count: 2) : desktop(display: state()),
      ];
      api.apply = () async => state(count: 2, status: 'pending', busy: true);
      api.observe = (operation) async {
        expect(operation, id);
        return state();
      };
      await AndroidStreamPreparation(
        api: api,
        authorized: () => true,
        wait: (_) async {},
      ).connect('token', 'pve-149', options);
      expect(api.calls, [
        'inventory',
        'count:1',
        'observe:$id',
        'inventory',
        'connect',
      ]);
    },
  );

  test('wrong operation response is rejected before connect', () async {
    final api = FakeApi();
    api.inventory = () async => [desktop(count: 2)];
    api.apply = () async => state(count: 2, status: 'pending', busy: true);
    api.observe = (_) async => state(operation: otherId);
    await expectLater(
      AndroidStreamPreparation(
        api: api,
        authorized: () => true,
        wait: (_) async {},
      ).connect('token', 'pve-149', options),
      throwsA(
        isA<BrokerException>().having(
          (e) => e.code,
          'code',
          'invalid_response',
        ),
      ),
    );
    expect(api.calls.contains('connect'), false);
  });

  test(
    'late count completion after route closes cannot request a controller',
    () async {
      final pending = Completer<VirtualDisplayState>();
      var authorized = true;
      final api = FakeApi();
      api.inventory = () async => [desktop(count: 2)];
      api.apply = () => pending.future;
      final future = AndroidStreamPreparation(
        api: api,
        authorized: () => authorized,
      ).connect('token', 'pve-149', options);
      await Future<void>.delayed(Duration.zero);
      expect(api.calls.last, 'count:1');
      authorized = false;
      pending.complete(state());
      await expectLater(
        future,
        throwsA(isA<BrokerException>().having((e) => e.status, 'status', 401)),
      );
      expect(api.calls.contains('connect'), false);
    },
  );

  test(
    'stale inventory after success cannot silently use two screens',
    () async {
      var now = DateTime.utc(2026, 10, 24);
      final api = FakeApi();
      api.inventory = () async => [desktop(count: 2)];
      api.apply = () async => state();
      api.observe = (_) async => state();
      await expectLater(
        AndroidStreamPreparation(
          api: api,
          authorized: () => true,
          now: () => now,
          wait: (duration) async => now = now.add(duration),
          timeout: const Duration(seconds: 1),
        ).connect('token', 'pve-149', options),
        throwsA(
          isA<BrokerException>().having(
            (e) => e.code,
            'code',
            'display_operation_timeout',
          ),
        ),
      );
      expect(api.calls.contains('connect'), false);
    },
  );

  test(
    'unsupported count control or unowned screen never falls back to console',
    () async {
      for (final target in [
        desktop(count: 2, control: false),
        desktop(
          control: false,
          monitors: const [
            RemoteMonitor(id: 'console', width: 1920, height: 1080),
          ],
        ),
      ]) {
        final api = FakeApi()..inventory = () async => [target];
        await expectLater(
          AndroidStreamPreparation(
            api: api,
            authorized: () => true,
          ).connect('token', 'pve-149', options),
          throwsA(
            isA<BrokerException>().having(
              (e) => e.code,
              'code',
              'display_not_supported',
            ),
          ),
        );
        expect(api.calls, ['inventory']);
      }
    },
  );

  test(
    'missing assignment and failed count stop before relay ticket',
    () async {
      final api = FakeApi()..inventory = () async => [];
      await expectLater(
        AndroidStreamPreparation(
          api: api,
          authorized: () => true,
        ).connect('token', 'pve-149', options),
        throwsA(isA<BrokerException>().having((e) => e.status, 'status', 404)),
      );
      api.inventory = () async => [desktop(count: 2)];
      api.apply = () async => state(status: 'failed');
      await expectLater(
        AndroidStreamPreparation(
          api: api,
          authorized: () => true,
        ).connect('token', 'pve-149', options),
        throwsA(
          isA<BrokerException>().having(
            (e) => e.code,
            'code',
            'display_operation_failed',
          ),
        ),
      );
      expect(api.calls.contains('connect'), false);
    },
  );

  test('a previous stream cleanup is observed before any mutation', () async {
    var reads = 0;
    var now = DateTime.utc(2026, 10, 26);
    final api = FakeApi()
      ..inventory = () async => [desktop(cleanup: ++reads < 3)];
    await AndroidStreamPreparation(
      api: api,
      authorized: () => true,
      now: () => now,
      wait: (duration) async => now = now.add(duration),
    ).connect('token', 'pve-149', options);
    expect(api.calls, ['inventory', 'inventory', 'inventory', 'connect']);
  });

  test('a cleanup that never finishes stops before a relay ticket', () async {
    var now = DateTime.utc(2026, 10, 26);
    final api = FakeApi()..inventory = () async => [desktop(cleanup: true)];
    await expectLater(
      AndroidStreamPreparation(
        api: api,
        authorized: () => true,
        now: () => now,
        wait: (duration) async => now = now.add(duration),
        cleanupTimeout: const Duration(seconds: 2),
      ).connect('token', 'pve-149', options),
      throwsA(
        isA<BrokerException>().having(
          (e) => e.code,
          'code',
          'session_cleanup_pending',
        ),
      ),
    );
    expect(api.calls.where((call) => call != 'inventory'), isEmpty);
  });

  test(
    'a busy display is observed until fresh without repeating its operation',
    () async {
      var reads = 0;
      var now = DateTime.utc(2026, 10, 28);
      final api = FakeApi()
        ..inventory = () async => [
          ++reads < 3
              ? desktop(count: 2, display: state(count: 2, busy: true))
              : desktop(),
        ];
      await AndroidStreamPreparation(
        api: api,
        authorized: () => true,
        now: () => now,
        wait: (duration) async => now = now.add(duration),
      ).connect('token', 'pve-149', options);
      expect(api.calls, ['inventory', 'inventory', 'inventory', 'connect']);
    },
  );

  test(
    'an ongoing busy display has a bounded observation with no new command',
    () async {
      var now = DateTime.utc(2026, 10, 28);
      final api = FakeApi()
        ..inventory = () async => [desktop(display: state(busy: true))];
      await expectLater(
        AndroidStreamPreparation(
          api: api,
          authorized: () => true,
          now: () => now,
          wait: (duration) async => now = now.add(duration),
          settleTimeout: const Duration(seconds: 2),
        ).connect('token', 'pve-149', options),
        throwsA(
          isA<BrokerException>().having(
            (e) => e.code,
            'code',
            'host_maintenance',
          ),
        ),
      );
      expect(api.calls.where((call) => call != 'inventory'), isEmpty);
    },
  );

  test(
    'closing during busy observation prevents the controller request',
    () async {
      var authorized = true;
      final api = FakeApi()
        ..inventory = () async => [desktop(display: state(busy: true))];
      await expectLater(
        AndroidStreamPreparation(
          api: api,
          authorized: () => authorized,
          wait: (_) async => authorized = false,
        ).connect('token', 'pve-149', options),
        throwsA(isA<BrokerException>().having((e) => e.status, 'status', 401)),
      );
      expect(api.calls, ['inventory']);
    },
  );

  test('an advertised initial viewport opens at the device size', () async {
    final api = FakeCurrentApi()
      ..inventory = () async => [
        desktop(initialViewport: true, encoderLimits: true),
      ];
    await AndroidStreamPreparation(
      api: api,
      authorized: () => true,
    ).connect('token', 'pve-149', options);
    expect(api.calls, ['inventory', 'connectViewport']);
    expect(api.viewport?.toJson(), {
      'monitor': '0',
      'width': 2560,
      'height': 1440,
      'fps': 30,
    });
    expect(api.viewport?.preserveEncoderLimits, true);
    // The encoder box is the codec ceiling, not this first size: a later
    // larger viewport (a Fold unfolded) is not encoded smaller.
    final requested = api.requested!;
    expect([requested.maxWidth, requested.maxHeight], [3840, 2160]);
    expect(
      [
        requested.fps,
        requested.bitrate,
        requested.audio,
        requested.monitor,
        requested.bitrateMode,
      ],
      [
        options.fps,
        options.bitrate,
        options.audio,
        options.monitor,
        options.bitrateMode,
      ],
    );
  });

  test(
    'without separate encoder limits the box stays the first size',
    () async {
      final api = FakeCurrentApi()
        ..inventory = () async => [desktop(initialViewport: true)];
      await AndroidStreamPreparation(
        api: api,
        authorized: () => true,
      ).connect('token', 'pve-149', options);
      expect(api.calls, ['inventory', 'connectViewport']);
      expect(api.requested, same(options));
    },
  );

  test('without the capability the ordinary connect is unchanged', () async {
    final api = FakeCurrentApi()..inventory = () async => [desktop()];
    await AndroidStreamPreparation(
      api: api,
      authorized: () => true,
    ).connect('token', 'pve-149', options);
    expect(api.calls, ['inventory', 'connect']);
  });

  test('takeover only releases the other connection', () async {
    final api = FakeCurrentApi();
    await AndroidStreamPreparation(
      api: api,
      authorized: () => true,
    ).takeover('token', 'pve-149', offer: androidOffer());
    expect(api.calls, ['takeover:pve-149']);
    await expectLater(
      AndroidStreamPreparation(
        api: FakeApi(),
        authorized: () => true,
      ).takeover('token', 'pve-149', offer: androidOffer()),
      throwsA(
        isA<BrokerException>().having((e) => e.code, 'code', 'desktop_busy'),
      ),
    );
    await expectLater(
      AndroidStreamPreparation(
        api: api,
        authorized: () => false,
      ).takeover('token', 'pve-149', offer: androidOffer()),
      throwsA(isA<BrokerException>().having((e) => e.status, 'status', 401)),
    );
    expect(api.calls, ['takeover:pve-149']);
  });

  testWidgets('busy desktop offers takeover, then connects through load', (
    tester,
  ) async {
    tester.view.physicalSize = const Size(1080, 2400);
    tester.view.devicePixelRatio = 3;
    addTearDown(tester.view.resetPhysicalSize);
    addTearDown(tester.view.resetDevicePixelRatio);
    var loads = 0, takeovers = 0;
    await tester.pumpWidget(
      MaterialApp(
        home: AndroidRemoteView(
          name: 'TESTER',
          desktopId: 'pve-149',
          token: 'not-persisted',
          options: const StreamOptions(maxWidth: 3840, maxHeight: 2160),
          load: (options, authorized, _) async {
            loads++;
            throw BrokerException(
              'desktop_busy',
              409,
              null,
              loads == 1,
              loads == 1 ? androidOffer() : null,
            );
          },
          takeover: (offer, current) async => takeovers++,
        ),
      ),
    );
    await tester.pumpAndSettle();
    expect(find.text('끊고 여기서 연결'), findsOneWidget);
    expect((takeovers, loads), (0, 1));
    // One press on the card, no dialog after it.
    await tester.tap(find.byKey(const Key('android-takeover')));
    await tester.pump();
    expect(find.byType(Dialog), findsNothing);
    await tester.pumpAndSettle();
    expect((takeovers, loads), (1, 2));
    // The second refusal no longer allows takeover: plain retry only.
    expect(find.text('끊고 여기서 연결'), findsNothing);
    expect(find.text('다시 연결'), findsOneWidget);
    expect(tester.takeException(), isNull);
  });

  testWidgets(
    'Android quality dialog has one screen and automatic resolution',
    (tester) async {
      StreamOptions? selected;
      await tester.pumpWidget(
        MaterialApp(
          home: Builder(
            builder: (context) => Scaffold(
              body: TextButton(
                onPressed: () async {
                  selected = await showDialog<StreamOptions>(
                    context: context,
                    builder: (_) =>
                        AndroidStreamDialog(desktop: desktop(count: 2)),
                  );
                },
                child: const Text('open'),
              ),
            ),
          ),
        ),
      );
      await tester.tap(find.text('open'));
      await tester.pumpAndSettle();
      expect(find.text('최대 해상도'), findsNothing);
      expect(find.text('가상 모니터'), findsNothing);
      expect(find.textContaining('가상 화면 1개'), findsOneWidget);
      await tester.tap(find.text('연결'));
      await tester.pumpAndSettle();
      expect(selected?.monitor, '0');
      expect((selected?.maxWidth, selected?.maxHeight), (3840, 2160));
      expect(tester.takeException(), isNull);
    },
  );

  testWidgets('immersive route measures physical viewport before broker load', (
    tester,
  ) async {
    tester.view.physicalSize = const Size(2560, 1440);
    tester.view.devicePixelRatio = 2;
    addTearDown(tester.view.resetPhysicalSize);
    addTearDown(tester.view.resetDevicePixelRatio);
    StreamOptions? requested;
    // 해상도 기기 화소: the measured physical pixels as they are (자동 on a
    // 720 dp screen without display-scale-v1 would take a readable half).
    const pixels = AndroidScreenChoice(resolution: AndroidResolution.device);
    await tester.pumpWidget(
      MaterialApp(
        home: AndroidRemoteView(
          name: 'TESTER',
          token: 'not-persisted',
          options: const StreamOptions(maxWidth: 3840, maxHeight: 2160),
          settings: const AndroidSessionSettings(
            helpSeen: true,
            screens: {
              AndroidDeviceClass.phone: pixels,
              AndroidDeviceClass.fold: pixels,
              AndroidDeviceClass.tablet: pixels,
            },
          ),
          load: (options, authorized, _) async {
            expect(authorized(), true);
            requested = options;
            throw const BrokerException('network_error');
          },
        ),
      ),
    );
    await tester.pumpAndSettle();
    expect((requested?.maxWidth, requested?.maxHeight), (2560, 1440));
    expect(requested?.monitor, '0');
    expect(find.text('다시 연결'), findsOneWidget);
    expect(tester.takeException(), isNull);
  });

  testWidgets('closing during preparation invalidates pending authorization', (
    tester,
  ) async {
    final pending = Completer<BrokerConnection>();
    bool Function()? authorized;
    await tester.pumpWidget(
      MaterialApp(
        home: Builder(
          builder: (context) => Scaffold(
            body: TextButton(
              onPressed: () => Navigator.push<void>(
                context,
                MaterialPageRoute(
                  builder: (_) => AndroidRemoteView(
                    name: 'TESTER',
                    token: 'not-persisted',
                    options: const StreamOptions(
                      maxWidth: 3840,
                      maxHeight: 2160,
                    ),
                    load: (_, current, _) {
                      authorized = current;
                      return pending.future;
                    },
                  ),
                ),
              ),
              child: const Text('open'),
            ),
          ),
        ),
      ),
    );
    await tester.tap(find.text('open'));
    await tester.pumpAndSettle();
    expect(authorized?.call(), true);
    // The preparing panel's "목록으로" leaves without a question (nothing
    // is connected yet).
    await tester.tap(find.byKey(const Key('android-leave')));
    await tester.pumpAndSettle();
    expect(authorized?.call(), false);
    pending.completeError(const BrokerException('network_error'));
    await tester.pumpAndSettle();
    expect(find.text('open'), findsOneWidget);
    expect(tester.takeException(), isNull);
  });
}
