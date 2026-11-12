import 'dart:async';
import 'dart:convert';

import 'package:flutter_test/flutter_test.dart';
import 'package:multitap_client/data/broker_api.dart';
import 'package:multitap_client/data/remote_session.dart';

import 'remote_session_test.dart'
    show SessionSocket, SessionMedia, info, flush, packet;

void main() {
  test('Only explicit allowlisted shortcuts send ordered session-scoped downs/ups and reset; closed sessions send nothing', () async {
    final socket = SessionSocket(), media = SessionMedia();
    final session = RemoteSession(
      connection: BrokerConnection(
        sessionId: 'fixture-session',
        webSocketUrl: Uri.parse(
          'wss://cloud.example.com/api/v1/sessions/fixture-session',
        ),
      ),
      token: 'fixture-only-token',
      media: media,
      dialer: (_, _) async => socket,
    );
    expect(await session.shortcut('altTab'), isFalse);
    unawaited(session.start());
    await flush();
    socket.incoming.add(jsonEncode(info));
    await flush();
    socket.incoming.add(packet());
    await flush();
    for (final command in {
      'windows': [0x5B],
      'altTab': [0xA4, 9],
      'taskManager': [0xA2, 0xA0, 0x1B],
    }.entries) {
      socket.sent.clear();
      expect(await session.shortcut(command.key), isTrue);
      final keys = socket.sent.where((e) => e['type'] == 'input').toList();
      expect(keys.map((e) => e['vk']).toList(), [
        ...command.value,
        ...command.value.reversed,
      ]);
      expect(keys.map((e) => e['down']).toList(), [
        ...List.filled(command.value.length, true),
        ...List.filled(command.value.length, false),
      ]);
      expect(keys.every((e) => e['event'] == 'key' && e['scan'] == 0), isTrue);
      expect(socket.sent.first['type'], 'input.reset');
      expect(socket.sent.last['type'], 'input.reset');
      expect(session.captured, isFalse);
    }
    socket.sent.clear();
    expect(await session.shortcut('ctrlAltDel'), isFalse);
    expect(socket.sent, isEmpty);
    await session.close();
    socket.sent.clear();
    expect(await session.shortcut('windows'), isFalse);
    expect(socket.sent, isEmpty);
  });
}
