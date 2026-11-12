import 'dart:convert';
import 'dart:io';
import 'dart:ui' as ui;

import 'package:flutter/material.dart';
import 'package:flutter/rendering.dart';
import 'package:flutter/services.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:integration_test/integration_test.dart';
import 'package:multitap_client/app.dart';
import 'package:multitap_client/data/broker_api.dart';
import 'package:multitap_client/data/client_settings.dart';
import 'package:multitap_client/ui/remote_view.dart';
import 'package:multitap_client/ui/session_preview.dart';
import 'package:multitap_client/models/stream_options.dart';

// Only a temporary, ordinary QA identity is read from an ACL-protected file.
// Sign-in widgets receive fixed QA placeholders, never a user's real password.
class WorkspaceQaApi implements BrokerApi {
  @override
  Future<VirtualDisplayState> configureDisplays(
    String token,
    String desktopId,
    int count,
  ) => throw StateError('Display operations are outside read-only QA.');
  @override
  Future<VirtualDisplayState> display(
    String token,
    String desktopId, {
    String? operationId,
  }) => throw StateError('Display operations are outside read-only QA.');
  @override
  Future<BrokerPowerResult> requestPower(
    String token,
    String desktopId,
    String action,
  ) => throw StateError(
    'Power operations are outside this read-only session QA.',
  );
  @override
  Future<BrokerPowerResult> power(
    String token,
    String desktopId, {
    String? taskId,
  }) => throw StateError(
    'Power operations are outside this read-only session QA.',
  );
  WorkspaceQaApi(this.delegate, this.credentials);
  final HttpBrokerApi delegate;
  final Map<String, dynamic> credentials;
  String token = '';
  int connects = 0;
  String? connectedDesktop;
  String connectStatus = 'not_called';
  @override
  Future<BrokerSession> login(String username, String password) async {
    expect(username, 'isolated-workspace-qa');
    expect(password, 'qa-credentials-from-private-file');
    final session = await delegate.login(
      credentials['username'] as String,
      credentials['password'] as String,
    );
    credentials['username'] = '';
    credentials['password'] = '';
    token = session.token;
    return session;
  }

  @override
  Future<List<BrokerDesktop>> desktops(String token) =>
      delegate.desktops(token);
  @override
  Future<BrokerConnection> connect(
    String token,
    String desktopId, {
    String mode = 'stream',
    StreamOptions? options,
  }) async {
    connects++;
    connectedDesktop = desktopId;
    try {
      final result = await delegate.connect(
        token,
        desktopId,
        mode: mode,
        options: options,
      );
      connectStatus = 'success';
      return result;
    } on BrokerException catch (failure) {
      connectStatus = '${failure.code}/${failure.status}';
      rethrow;
    }
  }

  @override
  Future<void> logout(String token) => delegate.logout(token);
  @override
  Future<void> changePassword(
    String token,
    String current,
    String password,
    String confirmation,
  ) => throw StateError('Password changes are outside this QA.');
  @override
  void close() => delegate.close();
}

void main() {
  final binding = IntegrationTestWidgetsFlutterBinding.ensureInitialized();
  binding.framePolicy = LiveTestWidgetsFlutterBindingFramePolicy.fullyLive;
  testWidgets(
    'Production workspace card opens the actual public relay and native texture',
    (tester) async {
      const credentials = String.fromEnvironment('CREDENTIAL_FILE');
      const output = String.fromEnvironment('OUTPUT_PATH');
      expect(credentials, isNotEmpty);
      final data = jsonDecode(
        await File(credentials).readAsString(),
      ) as Map<String, dynamic>;
      expect(data['username'], matches(RegExp(r'^qa-ui-[a-f0-9]{32}$')));
      final origin = parseServerOrigin(data['serverOrigin'] as String);
      final desktopId = data['desktopId'] as String;
      final api = WorkspaceQaApi(HttpBrokerApi(origin: origin), data);
      const native = MethodChannel('multitap/remote');
      final boundary = GlobalKey();
      try {
        await tester.pumpWidget(
          RepaintBoundary(
            key: boundary,
            child: MultitapApp(api: api, serverOrigin: origin),
          ),
        );
        await tester.pumpAndSettle();
        await tester.enterText(
          find.byKey(const Key('sign-in-id')),
          'isolated-workspace-qa',
        );
        await tester.enterText(
          find.byKey(const Key('sign-in-password')),
          'qa-credentials-from-private-file',
        );
        await tester.tap(find.byKey(const Key('login-submit')));
        final loginClock = Stopwatch()..start();
        final button = find.byKey(Key('open-$desktopId'));
        while (button.evaluate().isEmpty &&
            loginClock.elapsed < const Duration(seconds: 20)) {
          await tester.runAsync(
            () => Future<void>.delayed(const Duration(milliseconds: 50)),
          );
          await tester.pump();
        }
        expect(button, findsOneWidget);
        expect(
          tester.widget<Text>(find.byKey(Key('status-$desktopId'))).data,
          '연결 가능',
        );
        expect(find.text('호스트 등록 준비 중'), findsNothing);
        expect(tester.widget<FilledButton>(button).onPressed, isNotNull);
        expect(
          find.descendant(of: button, matching: find.text('연결하기')),
          findsOneWidget,
        );
        await tester.ensureVisible(button);
        await tester.tap(
          button,
        ); // The actual product card, not a direct RemoteView harness.
        final clock = Stopwatch()..start();
        Map<String, dynamic> stats = {};
        while (clock.elapsed < const Duration(seconds: 12)) {
          await tester.runAsync(
            () => Future<void>.delayed(const Duration(milliseconds: 50)),
          );
          await tester.pump();
          if (find.byType(Texture).evaluate().isNotEmpty) {
            stats = (await native.invokeMapMethod<String, dynamic>('stats'))!;
          }
        }
        expect(api.connects, 1);
        expect(api.connectedDesktop, desktopId);
        expect(find.byType(RemoteView), findsOneWidget);
        expect(find.byType(SessionPreview), findsNothing);
        if (find.byType(Texture).evaluate().isEmpty) {
          final nativeStatus = await native.invokeMapMethod<String, dynamic>(
            'stats',
          );
          final messages = tester
              .widgetList<Text>(
                find.descendant(
                  of: find.byType(RemoteView),
                  matching: find.byType(Text),
                ),
              )
              .map((text) => text.data ?? '')
              .join(' | ');
          debugPrint(
            'Workspace failure: connect=${api.connectStatus}; native=$nativeStatus; view=$messages',
          );
        }
        expect(find.byType(Texture), findsOneWidget);
        expect(stats['error'], 0);
        expect(stats['decodedFrames'], greaterThan(100));
        final image =
            await (boundary.currentContext!.findRenderObject()!
                    as RenderRepaintBoundary)
                .toImage();
        final rgba = await image.toByteData();
        var colored = 0;
        // Read actual Flutter-rendered pixels inside the texture, excluding toolbar.
        final textureRect = tester.getRect(find.byType(Texture));
        final ratio =
            image.width /
            (boundary.currentContext!.findRenderObject()!
                    as RenderRepaintBoundary)
                .size
                .width;
        for (
          var y = (textureRect.top * ratio).ceil();
          y < (textureRect.bottom * ratio).floor();
          y += 11
        ) {
          for (
            var x = (textureRect.left * ratio).ceil();
            x < (textureRect.right * ratio).floor();
            x += 11
          ) {
            final offset = (y * image.width + x) * 4;
            if (rgba!.getUint8(offset) +
                    rgba.getUint8(offset + 1) +
                    rgba.getUint8(offset + 2) >
                30) {
              colored++;
            }
          }
        }
        expect(colored, greaterThan(100));
        if (output.isNotEmpty) {
          final png = await image.toByteData(format: ui.ImageByteFormat.png);
          final file = File(output);
          await file.parent.create(recursive: true);
          await file.writeAsBytes(png!.buffer.asUint8List());
        }
        image.dispose();
        final metricText = tester
            .widgetList<Text>(find.byType(Text))
            .map((text) => text.data ?? '')
            .where((text) => text.contains('호스트 RTT'))
            .join();
        debugPrint(
          'Workspace public relay: connectCalls=${api.connects}, decoded=${stats['decodedFrames']}, nativeError=${stats['error']}, coloredTextureSamples=$colored, $metricText, audioReceivedFrames=${stats['audioReceivedFrames']}, audioPlayedFrames=${stats['audioPlayedFrames']}; no texture tap or key input sent.',
        );
        await tester.tap(find.byKey(const Key('stream-menu-연결')));
        await tester.pumpAndSettle();
        await tester.tap(find.byKey(const Key('close-remote-session')));
        await tester.pumpAndSettle();
        expect(find.byKey(Key('card-$desktopId')), findsOneWidget);
        await tester.tap(find.byKey(const Key('sign-out')));
        await tester.pumpAndSettle();
        expect(find.byKey(const Key('login-submit')), findsOneWidget);
      } finally {
        data['username'] = '';
        data['password'] = '';
        await native.invokeMethod<void>('capture', {'enabled': false});
        await tester.pumpWidget(const SizedBox.shrink());
        await tester.runAsync(
          () => Future<void>.delayed(const Duration(milliseconds: 300)),
        );
        if (api.token.isNotEmpty) {
          final cleanup = HttpBrokerApi(origin: origin);
          try {
            await cleanup.logout(api.token);
          } catch (_) {
          } finally {
            cleanup.close();
          }
        }
        api.token = '';
        api.close();
      }
    },
  );
}
