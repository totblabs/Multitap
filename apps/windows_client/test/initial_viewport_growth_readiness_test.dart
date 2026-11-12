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

import 'stream_display_entry_test.dart' show entryMachine;
import 'remote_session_test.dart' show SessionSocket, packet;
import 'viewport_session_test.dart' show ResizeMedia;

void main() {
  for (final scenario in [
    'growth',
    'downscaled',
    'unchanged',
    'reverted',
    'failed',
    'disposed',
    'deadline',
    'slow-initial',
  ]) {
    testWidgets(
      'initial source intent $scenario preserves truthful first-target readiness',
      (t) async {
        final downscaled = scenario == 'downscaled';
        t.view.devicePixelRatio = 1;
        t.view.physicalSize = const Size(1580, 952);
        addTearDown(t.view.resetPhysicalSize);
        addTearDown(t.view.resetDevicePixelRatio);
        final messenger =
            TestDefaultBinaryMessengerBinding.instance.defaultBinaryMessenger;
        const window = MethodChannel('multitap/window'),
            events = MethodChannel('multitap/window/events');
        messenger.setMockMethodCallHandler(window, (_) async => false);
        messenger.setMockMethodCallHandler(events, (_) async => null);
        addTearDown(() {
          messenger.setMockMethodCallHandler(window, null);
          messenger.setMockMethodCallHandler(events, null);
        });
        const id = 'aaaaaaaa-aaaa-aaaa-aaaa-aaaaaaaaaaaa',
            requestId = 'bbbbbbbb-bbbb-bbbb-bbbb-bbbbbbbbbbbb',
            resizeId = 'cccccccc-cccc-cccc-cccc-cccccccccccc';
        const options = StreamOptions(audio: false);
        final socket = SessionSocket(),
            media = ResizeMedia(),
            pending = Completer<RemoteSession>();
        InitialViewport? requested;
        final session = RemoteSession(
          connection: BrokerConnection.fromJson(
            {
              'sessionId': id,
              'webSocketUrl': 'wss://fixture.invalid/api/v1/sessions/$id',
              'mode': 'stream',
              'transport': 'media-epoch-v1',
              'resizeProtocol': 'epoch-v1',
              'viewportProvisioning': true,
              'options': options.toJson(),
              'initialViewportProtocol': 'epoch-v1',
              'initialViewport': {
                'protocol': 'epoch-v1',
                'sessionId': id,
                'requestId': requestId,
                'resizeId': resizeId,
                'monitor': '0',
                'width': 1580,
                'height': 952,
                'fps': 60,
                'epoch': 1,
              },
              if (downscaled) 'viewportEncoderLimitsProtocol': 'separate-v1',
            },
            Uri.parse('https://fixture.invalid'),
            requestedOptions: options,
            requestedInitialViewport: InitialViewport(
              width: 1580,
              height: 952,
              fps: 60,
              preserveEncoderLimits: downscaled,
            ),
          ),
          token: 'private-fixture-only',
          media: media,
          dialer: (_, _) async => socket,
        );
        await t.pumpWidget(
          MaterialApp(
            home: RemoteView(
              machine: entryMachine(
                1,
                initialViewport: true,
                viewportEncoderLimits: downscaled,
              ),
              prepareDisplaysOnEntry: true,
              initialDisplayMode: SessionDisplayMode.windowed,
              initialOptions: options,
              load: () async => throw StateError('no legacy route'),
              onConnectViewport: (viewport, _) {
                requested = viewport;
                return pending.future;
              },
            ),
          ),
        );
        try {
          await t.pump();
          await t.pump(const Duration(milliseconds: 100));
          await t.pump();
          await t.pump();
          expect([requested!.width, requested!.height], [1580, 952]);
          // Legitimate size change while authenticated HTTP/Host preparation is pending.
          if (scenario != 'unchanged') {
            t.view.physicalSize = const Size(2736, 1340);
            await t.pump();
            await t.pump();
          }
          if (scenario == 'reverted') {
            t.view.physicalSize = const Size(1580, 952);
            await t.pump();
            await t.pump();
          }
          if (scenario == 'disposed') {
            await t.pumpWidget(const SizedBox());
            pending.complete(session);
            await t.pump();
            await t.pump();
            expect(session.closed, true);
            expect(
              socket.sent.where((v) => v['type'] == 'stream.resize'),
              isEmpty,
            );
            return;
          }
          pending.complete(session);
          await t.pump();
          await t.pump();
          if (scenario == 'slow-initial') {
            // Initial Host hardware preparation has its own longer watchdog.
            // The corrective-entry budget must not shorten that contract.
            await t.pump(const Duration(seconds: 46));
            await t.pump();
            expect(session.closed, false);
            expect(find.textContaining('창 크기에 맞는 화면을 준비하지 못했어요'), findsNothing);
          }
          socket.incoming.add(
            jsonEncode({
              'type': 'stream.info',
              'codec': 'h264',
              'sessionId': id,
              'requestId': requestId,
              'resizeId': resizeId,
              'epoch': 1,
              'width': downscaled ? 790 : 1580,
              'height': downscaled ? 476 : 952,
              'sourceWidth': 1580,
              'sourceHeight': 952,
              'fpsTarget': 60,
            }),
          );
          await t.pump();
          await t.pump();
          socket.incoming.add(packet()..[2] = 1);
          await t.pump();
          await t.pump(const Duration(milliseconds: 70));
          await t.pump();
          expect(session.frameReady, true);
          expect([session.sourceWidth, session.sourceHeight], [1580, 952]);
          if (scenario == 'unchanged' || scenario == 'reverted') {
            expect(find.byType(Texture), findsOneWidget);
            expect(
              socket.sent.where((v) => v['type'] == 'stream.resize'),
              isEmpty,
            );
            await t.pump(const Duration(milliseconds: 300));
            await t.pump();
            expect(
              socket.sent.where((v) => v['type'] == 'stream.resize'),
              isEmpty,
            );
            return;
          }
          expect(
            find.byType(Texture),
            findsNothing,
            reason: 'A decoded startup size is not the latest entry viewport target.',
          );
          expect(session.captured, false);
          expect(
            socket.sent.where((v) => v['type'] == 'stream.resize'),
            isEmpty,
          );
          await t.pump(const Duration(milliseconds: 300));
          await t.pump();
          final corrective = socket.sent.singleWhere(
            (v) => v['type'] == 'stream.resize',
          );
          expect([corrective['width'], corrective['height']], [2736, 1340]);
          if (scenario == 'deadline') {
            await t.pump(const Duration(seconds: 46));
            await t.pump();
            expect(session.closed, true);
            expect(find.byType(Texture), findsNothing);
            expect(
              find.textContaining('창 크기에 맞는 화면을 준비하지 못했어요'),
              findsOneWidget,
            );
            return;
          }
          if (scenario == 'failed') {
            for (var i = 0; i < 2; i++) {
              final request = socket.sent
                  .where((v) => v['type'] == 'stream.resize')
                  .last;
              socket.incoming.add(
                jsonEncode({
                  'type': 'stream.resize.failed',
                  'requestId': request['requestId'],
                  'unchanged': true,
                  'reason': 'unsupported_display_mode',
                }),
              );
              await t.pump();
              await t.pump();
              await t.pump(const Duration(milliseconds: 300));
              await t.pump();
            }
            expect(session.closed, true);
            expect(find.byType(Texture), findsNothing);
            expect(find.text('다시 연결'), findsOneWidget);
            expect(
              socket.sent.where((v) => v['type'] == 'stream.resize').length,
              2,
            );
            return;
          }
          final correctionId = corrective['requestId'];
          socket.incoming.add(
            jsonEncode({
              'type': 'stream.resizing',
              'requestId': correctionId,
              'resizeId': 'entry-correction',
              'epoch': 2,
              'width': 2736,
              'height': 1340,
              'fps': 60,
              'monitor': '0',
              'timeoutMs': 90000,
            }),
          );
          await t.pump();
          await t.pump();
          socket.incoming.add(
            jsonEncode({
              'type': 'stream.info',
              'codec': 'h264',
              'sessionId': id,
              'requestId': correctionId,
              'resizeId': 'entry-correction',
              'epoch': 2,
              'width': downscaled ? 1368 : 2736,
              'height': downscaled ? 670 : 1340,
              'sourceWidth': 2736,
              'sourceHeight': 1340,
              'fpsTarget': 60,
            }),
          );
          await t.pump();
          await t.pump();
          expect(
            find.byType(Texture),
            findsNothing,
            reason: 'Metadata is not decoded target readiness.',
          );
          socket.incoming.add(packet()..[2] = 2);
          await t.pump();
          await t.pump(const Duration(milliseconds: 70));
          await t.pump();
          expect(find.byType(Texture), findsOneWidget);
          expect(
            t.getSize(find.byKey(const Key('remote-image-bounds'))),
            const Size(2736, 1340),
          );
          if (downscaled) {
            expect(
              t.widget<Texture>(find.byType(Texture)).filterQuality,
              FilterQuality.low,
              reason: 'Legitimate encoder downscale does not block source readiness.',
            );
          }
          await t.tap(find.byType(Texture));
          await t.pump();
          expect(session.captured, true);
        } finally {
          await t.pumpWidget(const SizedBox());
          unawaited(session.close());
          await t.pump(const Duration(seconds: 3));
          unawaited(media.events.close());
        }
      },
    );
  }
}
