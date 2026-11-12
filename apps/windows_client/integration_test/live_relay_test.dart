import 'dart:convert';
import 'dart:io';
import 'dart:ui' as ui;

import 'package:flutter/material.dart';
import 'package:flutter/rendering.dart';
import 'package:flutter/services.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:integration_test/integration_test.dart';
import 'package:multitap_client/data/broker_api.dart';
import 'package:multitap_client/data/client_settings.dart';
import 'package:multitap_client/data/remote_session.dart';
import 'package:multitap_client/models/desktop_machine.dart';
import 'package:multitap_client/ui/remote_view.dart';

void main() {
  final binding = IntegrationTestWidgetsFlutterBinding.ensureInitialized();
  binding.framePolicy = LiveTestWidgetsFlutterBindingFramePolicy.fullyLive;
  testWidgets(
    'Live public relay feeds the production Flutter remote view without input capture',
    (tester) async {
      const credentials = String.fromEnvironment('CREDENTIAL_FILE');
      const output = String.fromEnvironment('OUTPUT_PATH');
      expect(credentials, isNotEmpty);
      final data = jsonDecode(
        await File(credentials).readAsString(),
      ) as Map<String, dynamic>;
      final origin = parseServerOrigin(data['serverOrigin'] as String);
      final api = HttpBrokerApi(origin: origin);
      RemoteSession? remote;
      String token = '';
      final boundary = GlobalKey();
      try {
        final login = await api.login(
          data['username'] as String,
          data['password'] as String,
        );
        data['password'] = '';
        data['username'] = '';
        token = login.token;
        final connection = await api.connect(
          token,
          data['desktopId'] as String,
        );
        remote = RemoteSession(connection: connection, token: token);
        await tester.pumpWidget(
          MaterialApp(
            home: RepaintBoundary(
              key: boundary,
              child: RemoteView(
                machine: DesktopMachine(
                  id: data['desktopId'] as String,
                  name: '실제 중계 연결 검증',
                  description: '',
                  kind: MachineKind.vm,
                  status: MachineStatus.ready,
                  specification: 'Windows',
                  accent: Colors.blue,
                ),
                load: () async => remote!,
              ),
            ),
          ),
        );
        final clock = Stopwatch()..start();
        const native = MethodChannel('multitap/remote');
        Map<String, dynamic> stats = {};
        while (clock.elapsed < const Duration(seconds: 12)) {
          await tester.runAsync(
            () => Future<void>.delayed(const Duration(milliseconds: 50)),
          );
          await tester.pump();
          expect(
            remote.closed,
            isFalse,
            reason:
                '${remote.error ?? remote.status} [${remote.diagnosticCode}]',
          );
          if (remote.textureId != null) {
            stats = (await native.invokeMapMethod<String, dynamic>('stats'))!;
          }
        }
        expect(remote.textureId, isNotNull);
        expect(stats['decodedFrames'], greaterThan(100));
        expect(stats['error'], 0);
        expect(remote.rttMs, isNotNull);
        expect(
          remote.captured,
          isFalse,
          reason: 'This QA must never install the keyboard hook or send remote input.',
        );
        final image =
            await (boundary.currentContext!.findRenderObject()!
                    as RenderRepaintBoundary)
                .toImage();
        if (output.isNotEmpty) {
          final png = await image.toByteData(format: ui.ImageByteFormat.png);
          final file = File(output);
          await file.parent.create(recursive: true);
          await file.writeAsBytes(png!.buffer.asUint8List());
        }
        image.dispose();
        debugPrint(
          'Live public relay texture: decoded=${stats['decodedFrames']}, decodeFps=${remote.decodedFps?.toStringAsFixed(2)}, capturedFps=${remote.capturedFps}, encodedFps=${remote.encodedFps}, hostRttMs=${remote.rttMs?.toStringAsFixed(2)}, dimensions=${remote.width}x${remote.height}, audioReceivedFrames=${stats['audioReceivedFrames']}, audioPlayedFrames=${stats['audioPlayedFrames']}, audioError=${stats['audioError']}, inputCaptured=${remote.captured}',
        );
      } finally {
        data['password'] = '';
        data['username'] = '';
        await tester.pumpWidget(const SizedBox.shrink());
        await remote?.close();
        if (token.isNotEmpty) {
          try {
            await api.logout(token);
          } catch (_) {}
        }
        token = '';
        api.close();
      }
    },
  );
}
