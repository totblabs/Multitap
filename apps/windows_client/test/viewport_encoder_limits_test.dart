// Actual Dart HTTP/session state machines with injected socket/media. No real
// Windows output, physical rendering, driver or VM performance claim.
import 'dart:convert';

import 'package:flutter_test/flutter_test.dart';
import 'package:multitap_client/data/broker_api.dart';
import 'package:multitap_client/data/initial_viewport.dart';
import 'package:multitap_client/data/desktop_mapping.dart';
import 'package:multitap_client/data/remote_session.dart';
import 'package:multitap_client/models/stream_options.dart';

import 'initial_viewport_test.dart' as initial;
import 'power_api_test.dart' show ApiClient;
import 'remote_session_test.dart' show SessionSocket, flush, packet;
import 'viewport_session_test.dart' show ResizeMedia;

InitialViewport target({bool limited = true}) => InitialViewport(
  width: 2560,
  height: 1440,
  fps: 60,
  preserveEncoderLimits: limited,
);
BrokerConnection connection({bool limited = true, StreamOptions? options}) =>
    BrokerConnection.fromJson(
      {
        ...initial.response(),
        if (limited) 'viewportEncoderLimitsProtocol': 'separate-v1',
        if (options != null) 'options': options.toJson(),
      },
      Uri.parse('https://fixture.invalid'),
      requestedOptions: options ?? initial.requestedOptions,
      requestedInitialViewport: target(limited: limited),
    );

void main() {
  TestWidgetsFlutterBinding.ensureInitialized();
  test('Only a coherent advertised capability survives desktop mapping and status refreshes', () {
    final caps = {
      'initialViewportProtocol': 'epoch-v1',
      'viewportEncoderLimitsProtocol': 'separate-v1',
      'resizeProtocol': 'epoch-v1',
      'dynamicResolution': true,
      'viewportProvisioning': true,
      'monitors': [
        {'id': '0', 'width': 1920, 'height': 1080, 'primary': true},
      ],
    };
    BrokerDesktop desktop(Map<String, Object> capabilities) =>
        BrokerDesktop.fromJson({
          'id': 'pve-149',
          'name': 'TESTER',
          'kind': 'vm',
          'description': '',
          'os': 'Windows',
          'status': 'ready',
          'canConnect': true,
          'host': {'connected': true, 'capabilities': capabilities},
        });
    final machine = machineFromBroker(desktop(caps));
    expect(machine.viewportEncoderLimits, isTrue);
    expect(
      machine.withResources(machine.resources).viewportEncoderLimits,
      isTrue,
    );
    expect(machine.withDisplay(machine.display).viewportEncoderLimits, isTrue);
    for (final key in caps.keys) {
      final partial = {...caps}..remove(key);
      expect(desktop(partial).viewportEncoderLimits, isFalse, reason: key);
    }
    expect(
      desktop({...caps, 'viewportEncoderLimitsProtocol': 'unknown'})
          .viewportEncoderLimits,
      isFalse,
    );
    expect(desktop({...caps, 'monitors': []}).viewportEncoderLimits, isFalse);
  });
  test(
    'HTTP opts in only explicitly and verifies the exact response',
    () async {
      final client = ApiClient()
        ..responses.add((
          200,
          {
            ...initial.response(),
            'viewportEncoderLimitsProtocol': 'separate-v1',
          },
        ));
      final api = HttpBrokerApi(
        origin: Uri.parse('https://fixture.invalid'),
        client: client,
      );
      final negotiated = await api.connectViewport(
        'fixture',
        'pve-149',
        target(),
        initial.requestedOptions,
      );
      final body = jsonDecode(client.requests.single.body.toString()) as Map;
      expect(body['viewportEncoderLimitsProtocol'], 'separate-v1');
      expect(
        (body['initialViewport'] as Map).keys,
        unorderedEquals(['monitor', 'width', 'height', 'fps']),
      );
      expect(negotiated.viewportEncoderLimits, isTrue);
      for (final value in [null, false, true, 'unknown', 'epoch-v1']) {
        expect(
          () => BrokerConnection.fromJson(
            {...initial.response(), 'viewportEncoderLimitsProtocol': value},
            Uri.parse('https://fixture.invalid'),
            requestedOptions: initial.requestedOptions,
            requestedInitialViewport: target(),
          ),
          throwsA(isA<BrokerException>()),
        );
      }
      expect(
        () => BrokerConnection.fromJson(
          {
            ...initial.response(),
            'viewportEncoderLimitsProtocol': 'separate-v1',
          },
          Uri.parse('https://fixture.invalid'),
          requestedOptions: initial.requestedOptions,
          requestedInitialViewport: target(limited: false),
        ),
        throwsA(isA<BrokerException>()),
      );
      expect(connection(limited: false).viewportEncoderLimits, isFalse);
      const lower = StreamOptions(maxWidth: 960, maxHeight: 540, audio: false);
      expect(connection(options: lower).options.maxWidth, 960);
      expect(connection(options: lower).viewportEncoderLimits, isTrue);
    },
  );

  test('Small to fullscreen to portrait retains quality limits and commits only a newly decoded epoch', () async {
    final socket = SessionSocket(), media = ResizeMedia();
    final session = RemoteSession(
      connection: connection(),
      token: 'fixture',
      media: media,
      dialer: (_, _) async => socket,
    );
    final running = session.start();
    socket.incoming.add(jsonEncode(initial.info()));
    await flush();
    socket.incoming.add(packet()..[2] = 1);
    await flush();
    for (final (epoch, w, h, ew, eh) in [
      (2, 960, 540, 960, 540),
      (3, 3840, 2160, 1280, 720),
      (4, 720, 1280, 404, 720),
      (5, 320, 2160, 106, 720),
    ]) {
      var settled = false;
      final completion = session.resizeViewport(w, h).then((v) {
        settled = true;
        return v;
      });
      await flush();
      final request = socket.sent.lastWhere(
        (v) => v['type'] == 'stream.resize',
      );
      expect(request['width'], w);
      expect(request['height'], h);
      final correlation = {
        'sessionId': initial.sessionId,
        'requestId': request['requestId'],
        'resizeId': 'resize-$epoch',
        'epoch': epoch,
      };
      socket.incoming.add(
        jsonEncode({
          'type': 'stream.resizing',
          ...correlation,
          'width': w,
          'height': h,
          'monitor': '0',
          'fps': 60,
          'timeoutMs': 90000,
        }),
      );
      await flush();
      socket.incoming.add(packet()..[2] = epoch - 1);
      socket.incoming.add(jsonEncode(initial.info()));
      await flush();
      expect(settled, isFalse);
      expect(session.resizing, isTrue);
      socket.incoming.add(
        jsonEncode({
          ...initial.info(),
          ...correlation,
          'width': ew,
          'height': eh,
          'sourceWidth': w,
          'sourceHeight': h,
        }),
      );
      await flush();
      expect(settled, isFalse);
      expect(session.width, ew);
      expect(session.sourceWidth, w);
      expect(media.dimensions.last, [ew, eh]);
      expect(session.connection.options.maxWidth, 1280);
      socket.incoming.add(
        packet()
          ..[2] = epoch
          ..[1] = 0,
      );
      await flush();
      expect(settled, isFalse);
      socket.incoming.add(packet()..[2] = epoch);
      expect(await completion.timeout(const Duration(seconds: 2)), isTrue);
      expect(session.resizing, isFalse);
      expect(session.closed, isFalse);
      final count = socket.sent
          .where((v) => v['type'] == 'stream.resize')
          .length;
      expect(await session.resizeViewport(w, h), isTrue);
      expect(
        socket.sent.where((v) => v['type'] == 'stream.resize').length,
        count,
      );
    }
    await session.close();
    await running;
    await media.events.close();
  });

  for (final limited in [false, true]) {
    for (final delta in [
      {'width': 1920, 'height': 1080},
      {'sourceWidth': 1920},
      {'sourceHeight': 1080},
      {'fpsTarget': 30},
    ]) {
      test(
        'Unverified ordinary metadata closes the session: limited=$limited $delta',
        () async {
          final socket = SessionSocket(), media = ResizeMedia();
          final session = RemoteSession(
            connection: connection(limited: limited),
            token: 'fixture',
            media: media,
            dialer: (_, _) async => socket,
          );
          final running = session.start();
          socket.incoming.add(jsonEncode(initial.info()));
          await flush();
          socket.incoming.add(packet()..[2] = 1);
          await flush();
          final completion = session.resizeViewport(3840, 2160);
          await flush();
          final request = socket.sent.lastWhere(
            (v) => v['type'] == 'stream.resize',
          );
          final correlation = {
            'sessionId': initial.sessionId,
            'requestId': request['requestId'],
            'resizeId': 'resize-invalid',
            'epoch': 2,
          };
          socket.incoming.add(
            jsonEncode({
              'type': 'stream.resizing',
              ...correlation,
              'width': 3840,
              'height': 2160,
              'monitor': '0',
              'fps': 60,
            }),
          );
          await flush();
          socket.incoming.add(
            jsonEncode({
              ...initial.info(),
              ...correlation,
              'sourceWidth': 3840,
              'sourceHeight': 2160,
              ...delta,
            }),
          );
          expect(await completion.timeout(const Duration(seconds: 2)), isFalse);
          expect(session.closed, isTrue);
          expect(media.dimensions.length, 1);
          await session.close();
          await running;
          await media.events.close();
        },
      );
    }
  }
}
