// Real Flutter entry/layout/input dispatch with injected broker, monitor and
// media boundaries. This does not claim two physical display or VM acceptance.
import 'dart:async';
import 'dart:convert';

import 'package:flutter/gestures.dart';
import 'package:flutter/material.dart';
import 'package:flutter/services.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:multitap_client/data/broker_api.dart';
import 'package:multitap_client/data/remote_session.dart';
import 'package:multitap_client/data/track_stream.dart';
import 'package:multitap_client/models/desktop_machine.dart';
import 'package:multitap_client/models/session_display_mode.dart';
import 'package:multitap_client/models/stream_options.dart';
import 'package:multitap_client/ui/remote_view.dart';

import 'stream_display_entry_test.dart' show entryMachine;
import 'remote_session_test.dart' show SessionSocket;
import 'track_stream_test.dart' show TrackMedia, packet, trackStats;

void main() {
  TestWidgetsFlutterBinding.ensureInitialized();
  final messenger =
      TestDefaultBinaryMessengerBinding.instance.defaultBinaryMessenger;
  const channel = MethodChannel('multitap/window'),
      events = MethodChannel('multitap/window/events');
  tearDown(() {
    messenger.setMockMethodCallHandler(channel, null);
    messenger.setMockMethodCallHandler(events, null);
  });

  for (final mode in [
    SessionDisplayMode.windowed,
    SessionDisplayMode.fullscreen,
  ]) {
    testWidgets(
      '${mode.name} on a dual-monitor PC waits for exactly one verified owned output',
      (t) async {
        final pending = Completer<DesktopMachine>(), order = <String>[];
        var connects = 0;
        messenger.setMockMethodCallHandler(channel, (call) async {
          order.add(call.method);
          return mode == SessionDisplayMode.fullscreen;
        });
        messenger.setMockMethodCallHandler(events, (_) async => null);
        await t.pumpWidget(
          MaterialApp(
            home: RemoteView(
              machine: entryMachine(2),
              prepareDisplaysOnEntry: true,
              initialDisplayMode: mode,
              initialOptions: const StreamOptions(monitor: 'all'),
              load: () async {
                fail('Entry must use prepared options.');
              },
              onConfigureDisplays: (count) {
                order.add('count:$count');
                return pending.future;
              },
              onReconnect: (options) async {
                connects++;
                order.add('connect:${options.monitor}');
                throw const BrokerException('network_error');
              },
            ),
          ),
        );
        await t.pump();
        await t.pump(const Duration(milliseconds: 100));
        await t.pump();
        expect(order, ['fullscreen', 'count:1']);
        expect(connects, 0);
        pending.complete(entryMachine(1));
        await t.pumpAndSettle();
        expect(order, ['fullscreen', 'count:1', 'connect:0']);
        expect(t.takeException(), isNull);
        await t.pumpWidget(const SizedBox());
        await t.pump();
      },
    );
  }

  for (final profile in [false, true]) {
    testWidgets(
      'separate output textures use physical rectangles and seam input (profile=$profile)',
      (t) async {
        t.view.physicalSize = const Size(1920, 720);
        t.view.devicePixelRatio = 1.5;
        addTearDown(t.view.resetPhysicalSize);
        addTearDown(t.view.resetDevicePixelRatio);
        final pending = Completer<DesktopMachine>(), order = <String>[];
        final socket = SessionSocket(), media = TrackMedia();
        late RemoteSession session;
        TrackPlan? request;
        var primaryWidth = 640, primaryHeight = 360;
        messenger.setMockMethodCallHandler(channel, (call) async {
          order.add(call.method);
          if (call.method == 'monitorLayout') {
            // Like the native window: the covered read fails until the window
            // was spread over both monitors (reading it first made every
            // attempt fail before reaching the broker).
            final args = call.arguments;
            if ((args is! Map || args['requireCover'] != false) &&
                !order.contains('allMonitors')) {
              throw PlatformException(code: 'local_display_layout_unavailable');
            }
            return {
              'x': 0,
              'y': 0,
              'width': primaryWidth + 1280,
              'height': 720,
              'monitors': [
                {
                  'id': '0',
                  'x': 0,
                  'y': 0,
                  'width': primaryWidth,
                  'height': primaryHeight,
                  'primary': true,
                  'dpi': 144,
                },
                {
                  'id': '1',
                  'x': primaryWidth,
                  'y': 0,
                  'width': 1280,
                  'height': 720,
                  'primary': false,
                  'dpi': 96,
                },
              ],
            };
          }
          return true;
        });
        messenger.setMockMethodCallHandler(events, (_) async => null);
        await t.pumpWidget(
          MaterialApp(
            home: RemoteView(
              machine: entryMachine(1),
              prepareDisplaysOnEntry: true,
              initialDisplayMode: SessionDisplayMode.allMonitors,
              initialOptions: const StreamOptions(fps: 60),
              load: () async {
                fail(
                  'A dual connection must never fall back to composite streaming.',
                );
              },
              onConfigureDisplays: (count) {
                order.add('count:$count');
                return pending.future;
              },
              onConnectTracks: (plan, options) async {
                order.add('tracks');
                request = plan;
                final connection = BrokerConnection.fromJson(
                  {
                    'sessionId': 'track-session',
                    'webSocketUrl':
                        'wss://fixture.invalid/api/v1/sessions/track-session',
                    'mode': 'stream',
                    'transport': 'media-tracks-v1',
                    'mediaProtocol': 'tracks-v1',
                    'trackViewportProvisioning': true,
                    if (profile)
                      'streamProfileTracksProtocol': profileTracksProtocol,
                    'tracks': plan.toJson(),
                    'options': options.toJson(),
                  },
                  Uri.parse('https://fixture.invalid'),
                  requestedOptions: options,
                  requestedTracks: plan,
                  requestedProfileTracks: profile,
                );
                return session = RemoteSession(
                  connection: connection,
                  token: 'fixture-token',
                  media: media,
                  dialer: (_, _) async => socket,
                );
              },
            ),
          ),
        );
        await t.pump();
        await t.pump(const Duration(milliseconds: 100));
        await t.pump();
        expect(order, ['monitorLayout', 'allMonitors', 'count:2']);
        pending.complete(entryMachine(2));
        await t.pump();
        await t.pump();
        expect(order.last, 'tracks');
        expect(order.where((step) => step == 'monitorLayout'), hasLength(2));
        expect(request!.toJson(), [
          {'id': '0', 'width': 640, 'height': 360, 'x': 0, 'y': 0},
          {'id': '1', 'width': 1280, 'height': 720, 'x': 640, 'y': 0},
        ]);
        socket.incoming.add(
          jsonEncode({
            'type': 'stream.tracks.info',
            'sessionId': 'track-session',
            'codec': 'h264',
            'topologyEpoch': 1,
            'fpsTarget': 60,
            'bitrateTarget': 12000000,
            'tracks': [
              for (final output in request!.outputs)
                {
                  'id': output.id,
                  'epoch': 1,
                  'width': output.width,
                  'height': output.height,
                  'sourceWidth': output.width,
                  'sourceHeight': output.height,
                  'left': output.x,
                  'top': output.y,
                  'primary': output.id == '0',
                  'bitrateTarget': 6000000,
                },
            ],
            'audio': {
              'sampleRate': 48000,
              'channels': 2,
              'format': 's16le',
              'available': false,
            },
          }),
        );
        await t.pump();
        await t.pump();
        socket.incoming.add(packet(0));
        socket.incoming.add(packet(1));
        await t.pump();
        await t.pump();
        media.currentStats = trackStats();
        await t.pump(const Duration(seconds: 1));
        await t.pump();
        expect(session.closed, isFalse, reason: session.error);
        expect(
          socket.sent.any((event) => event['type'] == 'stream.tracks.ready'),
          isTrue,
        );
        final first = find.byKey(const Key('local-output-0')),
            second = find.byKey(const Key('local-output-1'));
        expect(
          t.getRect(first),
          const Rect.fromLTWH(0, 0, 640 / 1.5, 360 / 1.5),
        );
        expect(
          t.getRect(second),
          const Rect.fromLTWH(640 / 1.5, 0, 1280 / 1.5, 720 / 1.5),
        );
        expect(
          t
              .widgetList<Texture>(find.byType(Texture))
              .map((texture) => texture.textureId),
          [90, 91],
        );
        final mouse = await t.createGesture(kind: PointerDeviceKind.mouse);
        final firstPoint = t.getRect(first).center,
            secondPoint = t.getRect(second).center;
        await mouse.addPointer(location: firstPoint);
        await mouse.down(firstPoint);
        await t.pump();
        await t.pump();
        await mouse.up();
        await t.pump();
        expect(session.captured, isTrue);
        socket.sent.clear();
        await mouse.moveTo(firstPoint + const Offset(8, 8));
        await mouse.moveTo(secondPoint);
        await t.pump(const Duration(milliseconds: 20));
        final moves = socket.sent
            .where((event) => event['event'] == 'move')
            .toList();
        expect(
          moves.map((event) => event['track']),
          containsAllInOrder(['0', '1']),
        );
        expect(moves.last['x'], closeTo(.5, 1e-8));
        expect(moves.last['y'], closeTo(.5, 1e-8));

        // Flutter routes a held pointer to its original Listener even after it
        // crosses a sibling surface. The remote drag must follow the actual
        // whole-window position, preserving one continuous held-button state.
        socket.sent.clear();
        await mouse.down(firstPoint);
        await t.pump();
        await t.pump();
        await mouse.moveTo(secondPoint);
        await t.pump(const Duration(milliseconds: 20));
        await mouse.up();
        await t.pump();
        final drag = socket.sent
            .where((event) => event['type'] == 'input')
            .toList();
        expect(
          drag
              .where((event) => event['event'] == 'button')
              .map((event) => [event['track'], event['down']]),
          [
            ['0', true],
            ['1', false],
          ],
        );
        final crossed = drag.lastWhere((event) => event['event'] == 'move');
        expect(crossed['track'], '1');
        expect(crossed['x'], closeTo(.5, 1e-8));
        expect(crossed['y'], closeTo(.5, 1e-8));
        expect(
          socket.sent.where((event) => event['type'] == 'input.reset'),
          isEmpty,
        );

        // The primary is shorter than the secondary. The uncovered part of the
        // union has no remote output. Ignore moves there, but release a held
        // button at its last valid output/position when released in that gap.
        await mouse.moveTo(firstPoint);
        await mouse.down(firstPoint);
        await t.pump();
        await t.pump();
        await t.pump(const Duration(milliseconds: 20));
        socket.sent.clear();
        await mouse.moveTo(Offset(firstPoint.dx, 320));
        await t.pump(const Duration(milliseconds: 20));
        expect(socket.sent.where((event) => event['type'] == 'input'), isEmpty);
        await mouse.up();
        await t.pump();
        final gapRelease = socket.sent.lastWhere(
          (event) => event['event'] == 'button',
        );
        expect(gapRelease['track'], '0');
        expect(gapRelease['down'], isFalse);
        expect(gapRelease['x'], closeTo(.5, 1e-8));
        expect(gapRelease['y'], closeTo(.5, 1e-8));

        // Focus loss cancels pending movement and resets held host input. Later
        // events on the original drag route cannot resurrect capture or a drag.
        await mouse.moveTo(firstPoint);
        await mouse.down(firstPoint);
        await t.pump();
        await t.pump();
        await mouse.moveTo(secondPoint);
        socket.sent.clear();
        FocusManager.instance.primaryFocus?.unfocus();
        await t.pump();
        await t.pump();
        expect(session.captured, isFalse);
        expect(
          socket.sent.where((event) => event['type'] == 'input.reset'),
          isNotEmpty,
        );
        socket.sent.clear();
        await mouse.moveTo(secondPoint + const Offset(8, 8));
        await mouse.up();
        await t.pump(const Duration(milliseconds: 40));
        expect(socket.sent.where((event) => event['type'] == 'input'), isEmpty);
        if (profile) {
          primaryWidth = 800;
          primaryHeight = 600;
          t.view.physicalSize = const Size(2080, 720);
          await t.pump(const Duration(milliseconds: 700));
          await t.pump();
          final resize = socket.sent.lastWhere(
            (e) => e['type'] == 'stream.tracks.resize',
          );
          expect(resize['tracks'], [
            {'id': '0', 'width': 800, 'height': 600, 'x': 0, 'y': 0},
            {'id': '1', 'width': 1280, 'height': 720, 'x': 800, 'y': 0},
          ]);
          expect(session.captured, false);
          expect(session.frameReady, false);
          socket.incoming.add(
            jsonEncode({
              'type': 'stream.tracks.resizing',
              'requestId': resize['requestId'],
              'resizeId': 'widget-group-resize',
              'topologyEpoch': 2,
              'fps': 60,
              'timeoutMs': 90000,
              'tracks': [
                for (final t in resize['tracks'] as List)
                  {...(t as Map), 'epoch': t['id'] == '0' ? 2 : 1},
              ],
            }),
          );
          socket.incoming.add(
            jsonEncode({
              'type': 'stream.tracks.info',
              'sessionId': 'track-session',
              'codec': 'h264',
              'requestId': resize['requestId'],
              'resizeId': 'widget-group-resize',
              'topologyEpoch': 2,
              'fpsTarget': 60,
              'bitrateTarget': 12000000,
              'tracks': [
                for (final t in resize['tracks'] as List)
                  {
                    'id': t['id'],
                    'epoch': t['id'] == '0' ? 2 : 1,
                    'width': t['width'],
                    'height': t['height'],
                    'sourceWidth': t['width'],
                    'sourceHeight': t['height'],
                    'left': t['x'],
                    'top': t['y'],
                    'primary': t['id'] == '0',
                    'bitrateTarget': 6000000,
                  },
              ],
              'audio': {
                'sampleRate': 48000,
                'channels': 2,
                'format': 's16le',
                'available': false,
              },
            }),
          );
          await t.pump();
          await t.pump();
          media.currentStats = {
            ...trackStats(),
            'topologyEpoch': 2,
            'tracks': [
              for (final t in trackStats()['tracks'] as List)
                {
                  ...(t as Map),
                  if (t['id'] == '0') ...{
                    'width': 800,
                    'height': 600,
                    'epoch': 2,
                  },
                },
            ],
          };
          socket.incoming.add(packet(0, topology: 2, epoch: 2, pts: 0));
          socket.incoming.add(packet(1, topology: 2, pts: 0));
          await t.pump();
          await t.pump(const Duration(seconds: 1));
          await t.pump();
          expect(session.frameReady, true, reason: session.error);
          expect(
            t.getRect(first),
            const Rect.fromLTWH(0, 0, 800 / 1.5, 600 / 1.5),
          );
          expect(
            t.getRect(second),
            const Rect.fromLTWH(800 / 1.5, 0, 1280 / 1.5, 720 / 1.5),
          );
          // DPI-only changes update presentation without issuing another physical
          // output resize, and stats notifications cannot start a request loop.
          t.view.devicePixelRatio = 2;
          await t.pump(const Duration(milliseconds: 700));
          await t.pump();
          await t.pump(const Duration(seconds: 2));
          await t.pump();
          expect(
            socket.sent.where((e) => e['type'] == 'stream.tracks.resize'),
            hasLength(1),
          );
          expect(t.getRect(first), const Rect.fromLTWH(0, 0, 400, 300));
        }
        await t.pumpWidget(const SizedBox());
        unawaited(session.close());
        await t.pump(const Duration(seconds: 3));
        await t.pump();
        expect(media.stopped, isTrue);
      },
    );
  }
}
