// Actual production bridge and remote TextField transaction regression.
// Synthetic IME/media/relay only: no phone, Host, input injection or network.
import 'dart:convert';

import 'package:flutter/material.dart';
import 'package:flutter/services.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:multitap_client/android/android_keyboard.dart';
import 'package:multitap_client/android/android_remote.dart';
import 'package:multitap_client/android/android_session_keepalive.dart';
import 'package:multitap_client/data/broker_api.dart';
import 'package:multitap_client/data/remote_session.dart';

import 'package:multitap_client/models/stream_options.dart';

import '../remote_session_test.dart' show SessionSocket;
import 'android_session_contract_test.dart' show AndroidNativeFake;

TextEditingValue _edit(String text, {bool composing = false}) =>
    TextEditingValue(
      text: '${AndroidTypingBridge.sentinel}$text',
      selection: TextSelection.collapsed(
        offset: AndroidTypingBridge.sentinel.length + text.length,
      ),
      composing: composing
          ? TextRange(
              start: AndroidTypingBridge.sentinel.length,
              end: AndroidTypingBridge.sentinel.length + text.length,
            )
          : TextRange.empty,
    );

Future<void> _pump(WidgetTester tester) async {
  for (var i = 0; i < 8; i++) {
    await tester.pump(const Duration(milliseconds: 20));
  }
}

Future<SessionSocket> _openKeyboard(WidgetTester tester) async {
  tester.view.physicalSize = const Size(1080, 2400);
  tester.view.devicePixelRatio = 3;
  addTearDown(tester.view.reset);
  AndroidNativeFake().install();
  addTearDown(AndroidNativeFake.uninstall);
  final messenger =
      TestDefaultBinaryMessengerBinding.instance.defaultBinaryMessenger;
  messenger.setMockMethodCallHandler(
    AndroidSessionKeepalive.channel,
    (call) async => call.method == 'start' ? 'started' : true,
  );
  messenger.setMockMethodCallHandler(
    const MethodChannel('multitap/pen-input'),
    (_) async => null,
  );
  late SessionSocket socket;
  await tester.pumpWidget(
    MaterialApp(
      home: AndroidRemoteView(
        name: 'Fixture',
        token: 'not-real',
        options: const StreamOptions(),
        density: () async => null,
        load: (options, authorized, scale) async => BrokerConnection(
          reservation: BrokerSessionReservation(() async {}),
          sessionId: 'android-session',
          webSocketUrl: Uri.parse(
            'wss://fixture.invalid/sessions/android-session',
          ),
          textInput: true,
        ),
        sessionFactory: (connection, media, stats) => RemoteSession(
          connection: connection,
          token: 'not-real',
          media: media,
          statsSink: stats,
          dialer: (_, _) async => socket = SessionSocket(),
        ),
        pictureBuilder: (_) => const ColoredBox(color: Colors.black),
      ),
    ),
  );
  await _pump(tester);
  socket.incoming.add(
    jsonEncode({
      'type': 'stream.info',
      'codec': 'h264',
      'width': 1280,
      'height': 720,
      'sourceWidth': 1280,
      'sourceHeight': 720,
    }),
  );
  await _pump(tester);
  socket.incoming.add(
    Uint8List(17)
      ..[0] = 1
      ..[1] = 1
      ..setAll(12, [0, 0, 0, 1, 0x65]),
  );
  await _pump(tester);
  expect(find.byKey(const Key('android-remote-surface')), findsOneWidget);
  await tester.tap(find.byKey(const Key('android-remote-keyboard-button')));
  await _pump(tester);
  final field = find.byKey(const Key('android-remote-keyboard-field'));
  expect(tester.widget<TextField>(field).focusNode!.hasFocus, true);
  socket.sent.clear();
  return socket;
}

void main() {
  for (final mode in [
    'late-finish-before-fallback',
    'numpad-enter',
    'two-enter',
    'same-sentence-twice',
    'newline-then-distinct-enter',
  ]) {
    testWidgets('production native editing owner: $mode', (tester) async {
      final socket = await _openKeyboard(tester);
      const sentence = '가상 시험 문장';
      var buffer = sentence;
      tester.testTextInput.updateEditingValue(_edit(buffer, composing: true));
      await _pump(tester);
      if (mode == 'newline-then-distinct-enter') {
        buffer += '\n';
        tester.testTextInput.updateEditingValue(_edit(buffer));
        await _pump(tester);
      }
      final key = mode == 'numpad-enter'
          ? LogicalKeyboardKey.numpadEnter
          : LogicalKeyboardKey.enter;
      final lateFinish =
          mode == 'late-finish-before-fallback' || mode == 'numpad-enter';
      for (
        var index = 0;
        index <
            ((mode == 'two-enter' || mode == 'same-sentence-twice') ? 2 : 1);
        index++
      ) {
        if (index > 0 && mode == 'same-sentence-twice') {
          buffer += sentence;
          tester.testTextInput.updateEditingValue(
            _edit(buffer, composing: true),
          );
          await _pump(tester);
        }
        final handled = await tester.sendKeyDownEvent(key, platform: 'android');

        expect(handled, false);
        if (lateFinish) {
          tester.testTextInput.updateEditingValue(_edit(buffer));
          await _pump(tester);
        }
        // Pinned InputConnectionAdaptor.handleKeyEvent inserts the Enter
        // character into its multiline editable after framework returns false.
        if (!handled) {
          buffer += '\n';
          tester.testTextInput.updateEditingValue(_edit(buffer));
        }
        await tester.sendKeyUpEvent(key, platform: 'android');
        await _pump(tester);
        tester.testTextInput.updateEditingValue(_edit(buffer));
        await _pump(tester);
      }
      final input = socket.sent.where((v) => v['type'] == 'input').toList();
      final text = input
          .where((v) => v['event'] == 'text')
          .map((v) => v['text'])
          .join();
      final enters = input
          .where(
            (v) => v['event'] == 'key' && v['vk'] == 13 && v['down'] == true,
          )
          .length;
      debugPrint(
        'PRODUCTION_BEFORE_ASSERT mode=$mode textPackets=${input.where((v) => v["event"] == "text").length} enters=$enters',
      );
      expect(text, mode == 'same-sentence-twice' ? sentence * 2 : sentence);
      expect(enters, lateFinish ? 1 : 2);
      await tester.pumpWidget(const SizedBox.shrink());
      await _pump(tester);
    });
  }
}
