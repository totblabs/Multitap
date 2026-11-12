// Protocol and real Flutter entry ordering with injected media/broker/native
// monitor boundaries. No installed Windows driver or VM is changed here.
import 'dart:async';
import 'dart:convert';

import 'package:flutter/material.dart';
import 'package:flutter/services.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:multitap_client/data/broker_api.dart';
import 'package:multitap_client/data/initial_viewport.dart';
import 'package:multitap_client/data/remote_session.dart';
import 'package:multitap_client/models/session_display_mode.dart';
import 'package:multitap_client/models/stream_options.dart';
import 'package:multitap_client/ui/remote_view.dart';

import 'power_api_test.dart' show ApiClient;
import 'remote_session_test.dart' show SessionSocket, SessionMedia, packet;
import 'stream_display_entry_test.dart' show entryMachine;

const sessionId = 'aaaaaaaa-aaaa-aaaa-aaaa-aaaaaaaaaaaa';
const requestedOptions = StreamOptions(
  maxWidth: 1280,
  maxHeight: 720,
  audio: false,
);
InitialViewport target() => InitialViewport(width: 2560, height: 1440, fps: 60);
Map<String, dynamic> preparation() => {
  'protocol': 'epoch-v1',
  'sessionId': sessionId,
  'requestId': 'bbbbbbbb-bbbb-bbbb-bbbb-bbbbbbbbbbbb',
  'resizeId': 'cccccccc-cccc-cccc-cccc-cccccccccccc',
  'monitor': '0',
  'width': 2560,
  'height': 1440,
  'fps': 60,
  'epoch': 1,
};
Map<String, dynamic> response() => {
  'sessionId': sessionId,
  'webSocketUrl': 'wss://fixture.invalid/api/v1/sessions/$sessionId',
  'mode': 'stream',
  'transport': 'media-epoch-v1',
  'resizeProtocol': 'epoch-v1',
  'viewportProvisioning': true,
  'initialViewportProtocol': 'epoch-v1',
  'initialViewport': preparation(),
  'options': requestedOptions.toJson(),
};
BrokerConnection connection([Map<String, dynamic>? data]) =>
    BrokerConnection.fromJson(
      data ?? response(),
      Uri.parse('https://fixture.invalid'),
      requestedOptions: requestedOptions,
      requestedInitialViewport: target(),
    );
Map<String, dynamic> info() => {
  'type': 'stream.info',
  ...preparation(),
  'codec': 'h264',
  'width': 1280,
  'height': 720,
  'sourceWidth': 2560,
  'sourceHeight': 1440,
  'fpsTarget': 60,
};

class _Clock extends Stopwatch {
  int now = 0;
  @override
  int get elapsedMicroseconds => now;
}

class _Media extends SessionMedia {
  final sizes = <(int, int)>[];
  @override
  Future<int> start(int width, int height) async {
    starts++;
    sizes.add((width, height));
    return 42;
  }
}

void main() {
  TestWidgetsFlutterBinding.ensureInitialized();
  test('Profile-only capability never implies initial viewport; combined contract must match explicitly', () {
    expect(
      initialViewportAdvertised({
        'viewportProvisioning': true,
        'resizeProtocol': 'epoch-v1',
        'streamProfileProtocol': 'console-profile-v1',
      }),
      isFalse,
    );
    expect(
      initialViewportAdvertised({'initialViewportProtocol': 'epoch-v1'}),
      isTrue,
    );
    final combined = {
      'initialViewportProtocol': 'epoch-v1',
      'streamProfileProtocol': 'console-profile-v1',
      'streamProfileViewportProtocol': 'epoch-v1',
    };
    expect(initialViewportAdvertised(combined), isTrue);
    for (final key in combined.keys) {
      final partial = {...combined}..remove(key);
      expect(initialViewportAdvertised(partial), isFalse, reason: key);
    }
    expect(
      initialViewportAdvertised({
        ...combined,
        'streamProfileViewportProtocol': 'epoch-v2',
      }),
      isFalse,
    );
  });
  test('HTTP sends actual initial output separately from encoder limits and rejects downgraded replies', () async {
    final client = ApiClient()..responses.add((200, response()));
    final api = HttpBrokerApi(
      origin: Uri.parse('https://fixture.invalid'),
      client: client,
    );
    final result = await api.connectViewport(
      'fixture-token',
      'pve-149',
      target(),
      requestedOptions,
    );
    final posted = jsonDecode(client.requests.single.body.toString()) as Map;
    expect(posted['initialViewportProtocol'], 'epoch-v1');
    expect(posted['initialViewport'], target().toJson());
    expect(posted['options'], requestedOptions.toJson());
    expect(result.options.maxWidth, 1280);
    expect(result.initialViewport!.viewport.width, 2560);
    final denied = ApiClient()
      ..responses.add((
        400,
        {
          'error': {'code': 'unexpected_field'},
        },
      ));
    final old = HttpBrokerApi(
      origin: Uri.parse('https://fixture.invalid'),
      client: denied,
    );
    await expectLater(
      old.connectViewport(
        'fixture-token',
        'pve-149',
        target(),
        requestedOptions,
      ),
      throwsA(isA<BrokerException>()),
    );
    expect(
      denied.requests,
      hasLength(1),
      reason: 'An explicitly requested display transaction cannot silently become legacy capture.',
    );
    api.close();
    old.close();
  });
  test('Response requires explicit negotiation and exact session/IDs/epoch/physical target, without changing options', () {
    final good = connection();
    expect(good.initialViewport, isNotNull);
    final bad = <Map<String, dynamic>>[
      {...response(), 'initialViewportProtocol': null},
      {...response(), 'viewportProvisioning': false},
      {...response(), 'transport': 'media-v1'},
      {
        ...response(),
        'options': {...requestedOptions.toJson(), 'maxWidth': 2560},
      },
      for (final patch in [
        {'sessionId': 'other-session'},
        {'epoch': 0},
        {'epoch': 1.0},
        {'width': 1920},
        {'height': 1441},
        {'monitor': '1'},
        {'requestId': 'bad-id'},
        {'resizeId': preparation()['requestId']},
        {'unrequested': 'field'},
      ])
        {
          ...response(),
          'initialViewport': {...preparation(), ...patch},
        },
    ];
    for (final value in bad) {
      expect(() => connection(value), throwsA(isA<BrokerException>()));
    }
    expect(
      () => BrokerConnection.fromJson(
        response(),
        Uri.parse('https://fixture.invalid'),
        requestedOptions: requestedOptions,
      ),
      throwsA(isA<BrokerException>()),
    );
  });
  testWidgets(
    'Initial preparation permits a bounded hardware phase, then requires exact source metadata, epoch1 IDR and decoded frame',
    (t) async {
      final media = _Media(), socket = SessionSocket(), clock = _Clock();
      final session = RemoteSession(
        connection: connection(),
        token: 'fixture-token',
        media: media,
        clock: clock,
        dialer: (_, _) async => socket,
      );
      unawaited(session.start());
      await t.pump();
      await t.pump();
      clock.now = 40000000;
      await t.pump(const Duration(seconds: 1));
      await t.pump();
      expect(session.closed, isFalse);
      expect(session.frameReady, isFalse);
      expect(media.starts, 0);
      expect(socket.sent.where((e) => e['type'] == 'ping'), isEmpty);
      await session.capture(true);
      expect(session.captured, isFalse);
      socket.incoming.add(jsonEncode(info()));
      await t.pump(const Duration(milliseconds: 10));
      await t.pump();
      expect(
        media.starts,
        1,
        reason:
            '${session.status}/${session.error}/${session.diagnosticCode}; match=${connection().initialViewport!.matchesInfo(info(), 1280, 720)}',
      );
      expect(session.sourceWidth, 2560);
      expect(session.width, 1280);
      expect(media.sizes, [(1280, 720)]);
      expect(session.frameReady, isFalse);
      socket.incoming.add(packet()..[2] = 0);
      await t.pump();
      expect(media.packets, isEmpty);
      socket.incoming.add(
        packet()
          ..[2] = 1
          ..[1] = 0,
      );
      await t.pump();
      expect(media.packets, isEmpty);
      socket.incoming.add(packet()..[2] = 1);
      await t.pump();
      await t.pump();
      await t.pump(const Duration(milliseconds: 60));
      await t.pump();
      expect(session.frameReady, isTrue);
      await session.capture(true);
      expect(session.captured, isTrue);
      expect(await session.resizeViewport(2560, 1440), isTrue);
      expect(socket.sent.where((e) => e['type'] == 'stream.resize'), isEmpty);
      unawaited(session.close());
      await t.pump(const Duration(seconds: 3));
      unawaited(media.events.close());
      await t.pump();
    },
  );
  for (final mismatch in [
    {'sourceWidth': 1920},
    {'epoch': 0},
    {'resizeId': 'retired'},
    {'width': 1920},
  ]) {
    testWidgets(
      'Unverified initial metadata fails before decoder creation: $mismatch',
      (t) async {
        final media = SessionMedia(), socket = SessionSocket();
        final session = RemoteSession(
          connection: connection(),
          token: 'fixture-token',
          media: media,
          dialer: (_, _) async => socket,
        );
        unawaited(session.start());
        await t.pump();
        await t.pump();
        socket.incoming.add(jsonEncode({...info(), ...mismatch}));
        await t.pump(const Duration(milliseconds: 10));
        await t.pump();
        expect(
          session.closed,
          isTrue,
          reason:
              '${session.status}/${session.error}/${session.diagnosticCode}; starts=${media.starts}',
        );
        expect(session.frameReady, isFalse);
        expect(media.starts, 0);
        await session.capture(true);
        expect(session.captured, isFalse);
        unawaited(session.close());
        await t.pump(const Duration(seconds: 3));
        unawaited(media.events.close());
        await t.pump();
      },
    );
  }
  testWidgets(
    'Initial preparation expires without metadata and cannot be replaced by an old epoch',
    (t) async {
      final media = SessionMedia(), socket = SessionSocket(), clock = _Clock();
      final session = RemoteSession(
        connection: connection(),
        token: 'fixture-token',
        media: media,
        clock: clock,
        dialer: (_, _) async => socket,
      );
      unawaited(session.start());
      await t.pump();
      await t.pump();
      clock.now = 91000000;
      await t.pump(const Duration(seconds: 1));
      await t.pump();
      expect(session.closed, isTrue);
      expect(session.diagnosticCode, 'initial_viewport_timeout');
      expect(media.starts, 0);
      expect(session.frameReady, isFalse);
      await session.capture(true);
      expect(session.captured, isFalse);
      unawaited(session.close());
      await t.pump(const Duration(seconds: 3));
      unawaited(media.events.close());
      await t.pump();
    },
  );

  testWidgets(
    'Late initial metadata cannot beat a not-yet-fired preparation watchdog',
    (t) async {
      final media = _Media(), socket = SessionSocket(), clock = _Clock();
      final session = RemoteSession(
        connection: connection(),
        token: 'fixture-token',
        media: media,
        clock: clock,
        dialer: (_, _) async => socket,
      );
      unawaited(session.start());
      await t.pump();
      await t.pump();
      clock.now = 91000000;
      socket.incoming.add(jsonEncode(info()));
      await t.pump();
      await t.pump();
      expect(session.closed, isTrue);
      expect(session.diagnosticCode, 'initial_viewport_timeout');
      expect(media.starts, 0);
      unawaited(session.close());
      await t.pump(const Duration(seconds: 3));
      unawaited(media.events.close());
      await t.pump();
    },
  );
  for (final mode in [
    SessionDisplayMode.windowed,
    SessionDisplayMode.fullscreen,
  ]) {
    testWidgets(
      '${mode.name} uses the final physical viewport after owned count preparation, preserving encoding preferences',
      (t) async {
        t.view.physicalSize = const Size(1920, 1080);
        t.view.devicePixelRatio = 1.5;
        addTearDown(t.view.resetPhysicalSize);
        addTearDown(t.view.resetDevicePixelRatio);
        final messenger =
            TestDefaultBinaryMessengerBinding.instance.defaultBinaryMessenger;
        const window = MethodChannel('multitap/window'),
            events = MethodChannel('multitap/window/events');
        messenger.setMockMethodCallHandler(
          window,
          (_) async => mode == SessionDisplayMode.fullscreen,
        );
        messenger.setMockMethodCallHandler(events, (_) async => null);
        addTearDown(() => messenger.setMockMethodCallHandler(window, null));
        addTearDown(() => messenger.setMockMethodCallHandler(events, null));
        final prepared = Completer<void>(), order = <String>[];
        InitialViewport? sent;
        await t.pumpWidget(
          MaterialApp(
            home: RemoteView(
              machine: entryMachine(2, initialViewport: true),
              prepareDisplaysOnEntry: true,
              initialDisplayMode: mode,
              initialOptions: requestedOptions,
              load: () async =>
                  throw StateError('Legacy capture must not run.'),
              onReconnect: (_) async =>
                  throw StateError('Legacy capture must not run.'),
              onConfigureDisplays: (count) async {
                order.add('count:$count');
                await prepared.future;
                return entryMachine(
                  1,
                  initialViewport: true,
                  viewportEncoderLimits: true,
                );
              },
              onConnectViewport: (viewport, options) async {
                order.add('initial');
                sent = viewport;
                expect(options.toJson(), requestedOptions.toJson());
                throw const BrokerException('network_error');
              },
            ),
          ),
        );
        await t.pump();
        await t.pump(const Duration(milliseconds: 100));
        await t.pump();
        expect(order, ['count:1']);
        t.view.physicalSize = const Size(2560, 1440);
        prepared.complete();
        await t.pump();
        await t.pump();
        await t.pumpAndSettle();
        expect(order, ['count:1', 'initial']);
        expect(sent!.toJson(), target().toJson());
        expect(sent!.preserveEncoderLimits, isTrue);
        expect(t.takeException(), isNull);
        await t.pumpWidget(const SizedBox());
        await t.pump();
      },
    );
  }

  testWidgets(
    'Verified physical source fills its viewport even when encoder caps are smaller',
    (t) async {
      t.view.physicalSize = const Size(2560, 1440);
      t.view.devicePixelRatio = 1.5;
      addTearDown(t.view.resetPhysicalSize);
      addTearDown(t.view.resetDevicePixelRatio);
      final messenger =
          TestDefaultBinaryMessengerBinding.instance.defaultBinaryMessenger;
      const window = MethodChannel('multitap/window'),
          events = MethodChannel('multitap/window/events');
      messenger.setMockMethodCallHandler(window, (_) async => true);
      messenger.setMockMethodCallHandler(events, (_) async => null);
      addTearDown(() => messenger.setMockMethodCallHandler(window, null));
      addTearDown(() => messenger.setMockMethodCallHandler(events, null));
      final socket = SessionSocket(), media = _Media();
      final session = RemoteSession(
        connection: connection(),
        token: 'fixture-token',
        media: media,
        dialer: (_, _) async => socket,
      );
      await t.pumpWidget(
        MaterialApp(
          home: RemoteView(
            machine: entryMachine(1, initialViewport: true),
            prepareDisplaysOnEntry: true,
            initialOptions: requestedOptions,
            load: () async => throw StateError('Legacy capture must not run.'),
            onConnectViewport: (viewport, options) async {
              expect(viewport.toJson(), target().toJson());
              return session;
            },
          ),
        ),
      );
      await t.pump();
      await t.pump(const Duration(milliseconds: 100));
      await t.pump();
      await t.pump();
      socket.incoming.add(jsonEncode(info()));
      await t.pump();
      await t.pump();
      socket.incoming.add(packet()..[2] = 1);
      await t.pump();
      await t.pump(const Duration(milliseconds: 60));
      await t.pump();
      expect(session.frameReady, isTrue);
      expect(
        t.getRect(find.byKey(const Key('remote-image-bounds'))),
        const Rect.fromLTWH(0, 0, 2560 / 1.5, 1440 / 1.5),
      );
      // The 1280x720 video texture fills the 2560x1440 source picture: scaled
      // up 2x, so it is filtered, not nearest-neighbour blocks.
      expect(
        t.widget<Texture>(find.byType(Texture)).filterQuality,
        FilterQuality.low,
      );
      expect(media.sizes, [(1280, 720)]);
      await t.pump(const Duration(milliseconds: 700));
      await t.pump();
      expect(socket.sent.where((e) => e['type'] == 'stream.resize'), isEmpty);
      await t.pumpWidget(const SizedBox());
      unawaited(session.close());
      await t.pump(const Duration(seconds: 3));
      unawaited(media.events.close());
      await t.pump();
    },
  );
}
