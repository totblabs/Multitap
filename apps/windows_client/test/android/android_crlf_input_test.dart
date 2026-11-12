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

String _remoteText(List<Map<String, dynamic>> input) {
  final runes = <int>[];
  for (final packet in input) {
    if (packet['event'] == 'text') {
      runes.addAll((packet['text'] as String).runes);
    }
    if (packet['event'] == 'key' && packet['down'] == true) {
      if (packet['vk'] == 13) runes.add(10);
      if (packet['vk'] == 8 && runes.isNotEmpty) runes.removeLast();
    }
  }
  return String.fromCharCodes(runes);
}

void _newlineCases() {
  for (final mode in [
    'LF',
    'CRLF',
    'split-CRLF',
    'compose-finish',
    'normalized-finish',
    'emoji-CRLF',
    'selected-finish',
    'two-LF',
    'two-CR',
    'repeat-word',
    'two-native-Enter',
    'bare-CR',
    'delete-LF',
  ]) {
    testWidgets('actual production CRLF consistency: $mode', (tester) async {
      final socket = await _openKeyboard(tester);
      try {
        String expected = 'first\nsecond';
        var raw = mode == 'LF' || mode == 'delete-LF'
            ? 'first\nsecond'
            : 'first\r\nsecond';
        if (mode == 'bare-CR') raw = 'first\rsecond';
        if (mode == 'emoji-CRLF') {
          raw = '\u{1f600}\r\n\u{1f642}';
          expected = '\u{1f600}\n\u{1f642}';
        }
        if (mode == 'two-LF') {
          raw = 'first\n\nsecond';
          expected = raw;
        }
        if (mode == 'two-CR') {
          raw = 'first\r\rsecond';
          expected = 'first\n\nsecond';
        }
        if (mode == 'repeat-word') {
          raw = 'word\nword';
          expected = raw;
          tester.testTextInput.updateEditingValue(_edit('word\n'));
          await _pump(tester);
        }
        if (mode == 'split-CRLF') {
          tester.testTextInput.updateEditingValue(_edit('first\r'));
          await _pump(tester);
        }
        if (mode == 'two-native-Enter') {
          var buffer = 'first';
          tester.testTextInput.updateEditingValue(_edit(buffer));
          await _pump(tester);
          for (var n = 0; n < 2; n++) {
            final handled = await tester.sendKeyDownEvent(
              LogicalKeyboardKey.enter,
              platform: 'android',
            );
            expect(handled, false);
            buffer += '\n';
            tester.testTextInput.updateEditingValue(_edit(buffer));
            await tester.sendKeyUpEvent(
              LogicalKeyboardKey.enter,
              platform: 'android',
            );
            await _pump(tester);
          }
          raw = buffer;
          expected = buffer;
        } else {
          tester.testTextInput.updateEditingValue(
            _edit(
              raw,
              composing:
                  mode == 'compose-finish' ||
                  mode == 'normalized-finish' ||
                  mode == 'selected-finish',
            ),
          );
          await _pump(tester);
        }
        final beforeFinish = socket.sent
            .where((p) => p['type'] == 'input')
            .length;
        if (mode == 'compose-finish' ||
            mode == 'normalized-finish' ||
            mode == 'selected-finish') {
          var finish = _edit(
            mode == 'normalized-finish' ? 'first\nsecond' : raw,
          );
          if (mode == 'selected-finish') {
            finish = finish.copyWith(
              selection: TextSelection(
                baseOffset: AndroidTypingBridge.sentinel.length,
                extentOffset: finish.text.length,
              ),
            );
          }
          tester.testTextInput.updateEditingValue(finish);
          await _pump(tester);
        }
        if (mode == 'delete-LF') {
          expected = 'first\nsecon';
          tester.testTextInput.updateEditingValue(_edit(expected));
          await _pump(tester);
        }
        final input = socket.sent.where((p) => p['type'] == 'input').toList();
        final enters = input
            .where(
              (p) => p['event'] == 'key' && p['vk'] == 13 && p['down'] == true,
            )
            .length;
        debugPrint(
          'CRLF_REPRO mode=$mode enters=$enters packets=${input.length} beforeFinish=$beforeFinish',
        );
        expect(
          _remoteText(input),
          expected,
          reason: 'One logical CRLF is one newline; distinct LF/Enter intents remain distinct',
        );
        expect(
          enters,
          mode == 'two-LF' || mode == 'two-CR' || mode == 'two-native-Enter'
              ? 2
              : 1,
        );
        if (mode == 'normalized-finish' ||
            mode == 'compose-finish' ||
            mode == 'selected-finish') {
          expect(
            input.length,
            beforeFinish,
            reason: 'Same logical text finishing composition must not replay text or keys',
          );
        }
        expect(tester.takeException(), isNull);
      } finally {
        await tester.pumpWidget(const SizedBox.shrink());
        await _pump(tester);
      }
    });
  }
}

void _caretCases() {
  for (final mode in [
    'CRLF-left',
    'alias-after-to-between',
    'alias-between-to-after',
    'LF-left',
    'CR-left',
    'CRCR-left',
    'emoji-CRLF-left',
  ]) {
    testWidgets('actual production logical newline caret: $mode', (
      tester,
    ) async {
      final socket = await _openKeyboard(tester);
      try {
        var text = 'first\r\n';
        var expectedLeft = 1;
        if (mode == 'LF-left') text = 'first\n';
        if (mode == 'CR-left') text = 'first\r';
        if (mode == 'CRCR-left') {
          text = 'first\r\r';
          expectedLeft = 2;
        }
        if (mode == 'emoji-CRLF-left') text = '\u{1f600}\r\n';
        final lead = AndroidTypingBridge.sentinel.length;
        final initialOffset = mode == 'alias-between-to-after'
            ? lead + text.length - 1
            : lead + text.length;
        tester.testTextInput.updateEditingValue(
          _edit(
            text,
          ).copyWith(selection: TextSelection.collapsed(offset: initialOffset)),
        );
        await _pump(tester);
        socket.sent.clear();
        var nextOffset = lead + text.indexOf(RegExp('[\r\n]'));
        if (mode == 'alias-after-to-between') {
          nextOffset = lead + text.length - 1;
          expectedLeft = 0;
        }
        if (mode == 'alias-between-to-after') {
          nextOffset = lead + text.length;
          expectedLeft = 0;
        }
        tester.testTextInput.updateEditingValue(
          _edit(text)
              .copyWith(selection: TextSelection.collapsed(offset: nextOffset)),
        );
        await _pump(tester);
        final input = socket.sent.where((p) => p['type'] == 'input').toList();
        final left = input
            .where(
              (p) =>
                  p['event'] == 'key' && p['down'] == true && p['vk'] == 0x25,
            )
            .length;
        final right = input
            .where(
              (p) =>
                  p['event'] == 'key' && p['down'] == true && p['vk'] == 0x27,
            )
            .length;
        debugPrint('CARET_REPRO mode=$mode left=$left right=$right');
        expect(left, expectedLeft);
        expect(right, 0);
        if (mode.startsWith('alias-')) {
          expect(
            tester
                .widget<TextField>(
                  find.byKey(const Key('android-remote-keyboard-field')),
                )
                .controller!
                .text,
            _edit(text).text,
          );
        }
        expect(input.where((p) => p['event'] == 'text'), isEmpty);
        expect(
          input.where(
            (p) => p['event'] == 'key' && p['down'] == true && p['vk'] == 13,
          ),
          isEmpty,
        );
      } finally {
        await tester.pumpWidget(const SizedBox.shrink());
        await _pump(tester);
      }
    });
  }
}

void main() {
  _newlineCases();
  _caretCases();
  for (final mode in ['past-end', 'negative', 'past-end-extent']) {
    test('text newline caret rejects invalid raw selection: $mode', () {
      final bridge = AndroidTypingBridge(text: true);
      final initial = _edit('first\r\n');
      bridge.update(initial);
      final selection = mode == 'past-end-extent'
          ? TextSelection(
              baseOffset: initial.text.length,
              extentOffset: initial.text.length + 20,
            )
          : TextSelection.collapsed(
              offset: mode == 'negative' ? -1 : initial.text.length + 20,
            );
      final ignored = bridge.update(initial.copyWith(selection: selection));
      expect(ignored.actions, isEmpty);
      expect(ignored.reset, isNull);
      final restored = bridge.update(initial);
      expect(restored.actions, isEmpty);
      expect(restored.reset, isNull);
    });
  }
}
