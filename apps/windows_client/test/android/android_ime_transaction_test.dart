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
import 'package:multitap_client/data/text_input.dart';
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

class _Delivered {
  String shown = '';
  final textPackets = <Map<String, dynamic>>[];
  int enters = 0;
  void take(AndroidTypingUpdate update) {
    var runes = shown.runes.toList();
    for (final action in update.actions) {
      if (action is String) {
        runes.addAll(action.runes);
        textPackets.addAll(textInputMessages(action));
      } else if (action is AndroidKeyStroke) {
        if (action.vk == 8 && runes.isNotEmpty) runes.removeLast();
        if (action.vk == 13) {
          enters++;
          runes.add(10);
        }
        if (action.vk == 9) runes.add(9);
      }
    }
    shown = String.fromCharCodes(runes);
  }
}

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
  test('Gboard commit newline and finish keep one sentence and one Enter', () {
    final bridge = AndroidTypingBridge(text: true), delivered = _Delivered();
    for (final edit in [
      _edit('문장을 보내요', composing: true),
      _edit('문장을 보내요\n'),
      _edit('문장을 보내요\n'),
    ]) {
      final update = bridge.update(edit);
      expect(update.reset, isNull);
      delivered.take(update);
    }
    expect(delivered.shown, '문장을 보내요\n');
    expect(delivered.enters, 1);
  });

  for (final units in [512, 4096]) {
    test(
      'committed $units UTF16 units and finish retain exact chunk count',
      () {
        final bridge = AndroidTypingBridge(text: true),
            delivered = _Delivered();
        final text = '한' * units;
        final commit = bridge.update(_edit(text));
        expect(commit.reset, isNull);
        delivered.take(commit);
        delivered.take(bridge.update(_edit(text)));
        delivered.take(bridge.update(_edit(text, composing: true)));
        delivered.take(bridge.update(_edit(text)));
        expect(delivered.shown, text);
        expect(delivered.textPackets, hasLength(units ~/ textInputMaxUnits));
        expect(
          delivered.textPackets.every(
            (packet) => (packet['text'] as String).length == textInputMaxUnits,
          ),
          true,
        );
      },
    );
  }

  test(
    'Samsung long word finish and suggestion commit do not replay prefix',
    () {
      final bridge = AndroidTypingBridge(text: true), delivered = _Delivered();
      final prefix = '이미 보낸 문장 ' * 12;
      for (final text in ['$prefixㅎ', '$prefix하', '$prefix한', '$prefix한글']) {
        delivered.take(bridge.update(_edit(text, composing: true)));
      }
    delivered.take(bridge.update(_edit('$prefix한글 ')));
    delivered.take(bridge.update(_edit('$prefix한글 ')));
    expect(delivered.shown, '$prefix한글 ');
    },
  );

  test('32 intentional identical messages preserve every text and Enter', () {
    final bridge = AndroidTypingBridge(text: true), delivered = _Delivered();
    final message = '가' * 128;
    var editor = '';
    for (var i = 0; i < 32; i++) {
      editor += '$message\n';
      delivered.take(bridge.update(_edit(editor)));
      delivered.take(bridge.update(_edit(editor)));
    }
    expect(delivered.shown, '$message\n' * 32);
    expect(delivered.enters, 32);
    expect(delivered.textPackets, hasLength(32));
    expect(
      editor.length + AndroidTypingBridge.sentinel.length,
      4130,
      reason: 'active editor history remains until a real owner boundary; this is not a memory cap',
    );
  });

  test('emoji chunk boundaries and later backspace still edit once', () {
    final bridge = AndroidTypingBridge(text: true), delivered = _Delivered();
    final text = '${'가' * 255}😀끝';
    delivered.take(bridge.update(_edit(text)));
    delivered.take(bridge.update(_edit(text)));
    expect(
      delivered.textPackets.map((packet) => (packet['text'] as String).length),
      [255, 3],
    );
    delivered.take(bridge.update(_edit('${'가' * 255}😀')));
    delivered.take(bridge.update(_edit('가' * 255)));
    expect(delivered.shown, '가' * 255);
  });

  test('legacy key mode newline and long finish do not replay key strokes', () {
    final bridge = AndroidTypingBridge();
    final first = bridge.update(_edit('${'a' * 512}\n'));
    expect(first.reset, isNull);
    expect(first.strokes.where((key) => key.vk == 0x41), hasLength(512));
    expect(first.strokes.where((key) => key.vk == 13), hasLength(1));
    expect(bridge.update(_edit('${'a' * 512}\n')).actions, isEmpty);
    expect(bridge.update(_edit('${'a' * 512}\n\n')).strokes, [
      AndroidKeyStroke.enter,
    ]);
  });

  for (final long in [false, true]) {
    testWidgets(
      'real remote TextField finish emits once${long ? ' with4096units' : ' and keeps intended repeats'}',
      (tester) async {
        final socket = await _openKeyboard(tester);
        final sentence = long ? '한' * 4096 : '문장을 보내요';
        for (final value in [
          _edit(sentence, composing: true),
          _edit('$sentence\n'),
          _edit('$sentence\n'),
        ]) {
          tester.testTextInput.updateEditingValue(value);
          await _pump(tester);
        }
        await tester.testTextInput.receiveAction(TextInputAction.newline);
        await _pump(tester);
        if (!long) {
          tester.testTextInput.updateEditingValue(
            _edit('$sentence\n$sentence\n'),
          );
          await _pump(tester);
          tester.testTextInput.updateEditingValue(
            _edit('$sentence\n$sentence\n'),
          );
          await _pump(tester);
        }
        final inputs = socket.sent
            .where((event) => event['type'] == 'input')
            .toList();
        final packets = inputs
            .where((event) => event['event'] == 'text')
            .toList();
        expect(
          packets.map((event) => event['text']).join(),
          long ? sentence : sentence * 2,
        );
        expect(
          inputs.where(
            (event) =>
                event['event'] == 'key' &&
                event['vk'] == 13 &&
                event['down'] == true,
          ),
          hasLength(long ? 1 : 2),
        );
        expect(packets, hasLength(long ? 16 : 2));
        await tester.pumpWidget(const SizedBox.shrink());
        await _pump(tester);
      },
    );
  }
}
